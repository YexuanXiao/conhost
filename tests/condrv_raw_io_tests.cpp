#include "condrv/condrv_server.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstring>
#include <iterator>
#include <optional>
#include <string_view>
#include <vector>

namespace
{
    struct MemoryComm final
    {
        std::vector<std::byte> input;
        std::vector<std::byte> output;

        [[nodiscard]] std::expected<void, oc::condrv::DeviceCommError> read_input(oc::condrv::IoOperation& operation) noexcept
        {
            if (operation.buffer.data == nullptr)
            {
                return std::unexpected(oc::condrv::DeviceCommError{
                    .context = L"read_input received null buffer",
                    .win32_error = ERROR_INVALID_PARAMETER,
                });
            }

            const auto offset = static_cast<size_t>(operation.buffer.offset);
            const auto size = static_cast<size_t>(operation.buffer.size);
            if (offset > input.size())
            {
                return std::unexpected(oc::condrv::DeviceCommError{
                    .context = L"read_input offset exceeded input size",
                    .win32_error = ERROR_INVALID_DATA,
                });
            }

            const size_t remaining = input.size() - offset;
            const size_t to_copy = std::min(remaining, size);
            if (to_copy != 0)
            {
                std::memcpy(operation.buffer.data, input.data() + offset, to_copy);
            }
            if (to_copy < size)
            {
                std::memset(static_cast<std::byte*>(operation.buffer.data) + to_copy, 0, size - to_copy);
            }

            return {};
        }

        [[nodiscard]] std::expected<void, oc::condrv::DeviceCommError> write_output(oc::condrv::IoOperation& operation) noexcept
        {
            if (operation.buffer.data == nullptr)
            {
                return std::unexpected(oc::condrv::DeviceCommError{
                    .context = L"write_output received null buffer",
                    .win32_error = ERROR_INVALID_PARAMETER,
                });
            }

            const auto offset = static_cast<size_t>(operation.buffer.offset);
            const auto size = static_cast<size_t>(operation.buffer.size);
            if (offset > output.size())
            {
                output.resize(offset);
            }
            output.resize(offset + size);
            if (size != 0)
            {
                std::memcpy(output.data() + offset, operation.buffer.data, size);
            }
            return {};
        }

        [[nodiscard]] std::expected<void, oc::condrv::DeviceCommError> complete_io(const oc::condrv::IoComplete& /*completion*/) noexcept
        {
            return {};
        }
    };

    struct TestHostIo final
    {
        std::vector<std::byte> written;
        std::vector<std::byte> input;
        size_t input_offset{ 0 };
        bool answer_vt_queries{ true };
        std::vector<DWORD> end_task_pids;
        std::vector<DWORD> end_task_events;
        std::vector<DWORD> end_task_flags;

        [[nodiscard]] std::expected<size_t, oc::condrv::DeviceCommError> write_output_bytes(const std::span<const std::byte> bytes) noexcept
        {
            written.insert(written.end(), bytes.begin(), bytes.end());
            return bytes.size();
        }

        [[nodiscard]] std::expected<size_t, oc::condrv::DeviceCommError> read_input_bytes(const std::span<std::byte> dest) noexcept
        {
            const size_t remaining = input.size() - input_offset;
            const size_t to_copy = std::min(remaining, dest.size());
            if (to_copy != 0)
            {
                std::memcpy(dest.data(), input.data() + input_offset, to_copy);
                input_offset += to_copy;
            }
            return to_copy;
        }

        [[nodiscard]] std::expected<size_t, oc::condrv::DeviceCommError> peek_input_bytes(const std::span<std::byte> dest) noexcept
        {
            const size_t remaining = input.size() - input_offset;
            const size_t to_copy = std::min(remaining, dest.size());
            if (to_copy != 0)
            {
                std::memcpy(dest.data(), input.data() + input_offset, to_copy);
            }
            return to_copy;
        }

        [[nodiscard]] size_t input_bytes_available() const noexcept
        {
            return input.size() - input_offset;
        }

        [[nodiscard]] bool inject_input_bytes(const std::span<const std::byte> bytes) noexcept
        {
            if (bytes.empty())
            {
                return true;
            }

            try
            {
                input.insert(input.end(), bytes.begin(), bytes.end());
                return true;
            }
            catch (...)
            {
                return false;
            }
        }

        [[nodiscard]] bool vt_should_answer_queries() const noexcept
        {
            return answer_vt_queries;
        }

        [[nodiscard]] std::expected<void, oc::condrv::DeviceCommError> flush_input_buffer() noexcept
        {
            input.clear();
            input_offset = 0;
            return {};
        }

        [[nodiscard]] std::expected<bool, oc::condrv::DeviceCommError> wait_for_input(const DWORD /*timeout_ms*/) noexcept
        {
            return input_bytes_available() != 0;
        }

        [[nodiscard]] bool input_disconnected() const noexcept
        {
            return false;
        }

        [[nodiscard]] std::expected<void, oc::condrv::DeviceCommError> send_end_task(
            const DWORD process_id,
            const DWORD event_type,
            const DWORD ctrl_flags) noexcept
        {
            end_task_pids.push_back(process_id);
            end_task_events.push_back(event_type);
            end_task_flags.push_back(ctrl_flags);
            return {};
        }
    };

    [[nodiscard]] oc::condrv::IoPacket make_connect_packet(const DWORD pid, const DWORD tid) noexcept
    {
        oc::condrv::IoPacket packet{};
        packet.descriptor.identifier.LowPart = 1;
        packet.descriptor.function = oc::condrv::console_io_connect;
        packet.descriptor.process = pid;
        packet.descriptor.object = tid;
        return packet;
    }

    [[nodiscard]] oc::condrv::ConnectionInformation unpack_connection_information(const oc::condrv::IoComplete& completion) noexcept
    {
        oc::condrv::ConnectionInformation info{};
        std::memcpy(&info, completion.write.data, sizeof(info));
        return info;
    }

    [[nodiscard]] bool set_input_code_page(
        MemoryComm& comm,
        oc::condrv::ServerState& state,
        TestHostIo& host_io,
        const oc::condrv::ConnectionInformation info,
        const UINT code_page) noexcept
    {
        constexpr ULONG api_size = sizeof(CONSOLE_SETCP_MSG);
        constexpr ULONG header_size = sizeof(CONSOLE_MSG_HEADER);
        constexpr ULONG read_offset = api_size + header_size;

        oc::condrv::IoPacket packet{};
        packet.payload.user_defined = oc::condrv::UserDefinedPacket{};
        packet.descriptor.identifier.LowPart = 50;
        packet.descriptor.function = oc::condrv::console_io_user_defined;
        packet.descriptor.process = info.process;
        packet.descriptor.object = info.input;
        packet.descriptor.input_size = read_offset;
        packet.descriptor.output_size = api_size;
        packet.payload.user_defined.msg_header.ApiNumber = static_cast<ULONG>(ConsolepSetCP);
        packet.payload.user_defined.msg_header.ApiDescriptorSize = api_size;

        auto& body = packet.payload.user_defined.u.console_msg_l2.SetConsoleCP;
        body.CodePage = code_page;
        body.Output = FALSE;

        comm.input.assign(packet.descriptor.input_size, std::byte{});

        oc::condrv::BasicApiMessage<MemoryComm> message(comm, packet);
        auto outcome = oc::condrv::dispatch_message(state, message, host_io);
        return outcome && message.completion().io_status.Status == oc::core::status_success;
    }

    [[nodiscard]] bool write_console_user_defined_a(
        MemoryComm& comm,
        oc::condrv::ServerState& state,
        TestHostIo& host_io,
        const oc::condrv::ConnectionInformation info,
        const std::string_view text,
        const ULONG id) noexcept
    {
        constexpr ULONG api_size = sizeof(CONSOLE_WRITECONSOLE_MSG);
        constexpr ULONG header_size = sizeof(CONSOLE_MSG_HEADER);
        const ULONG read_offset = api_size + header_size;

        oc::condrv::IoPacket packet{};
        packet.payload.user_defined = oc::condrv::UserDefinedPacket{};
        packet.descriptor.identifier.LowPart = id;
        packet.descriptor.function = oc::condrv::console_io_user_defined;
        packet.descriptor.process = info.process;
        packet.descriptor.object = info.output;
        packet.descriptor.input_size = read_offset + static_cast<ULONG>(text.size());
        packet.descriptor.output_size = api_size;
        packet.payload.user_defined.msg_header.ApiNumber = static_cast<ULONG>(ConsolepWriteConsole);
        packet.payload.user_defined.msg_header.ApiDescriptorSize = api_size;
        packet.payload.user_defined.u.console_msg_l1.WriteConsole.Unicode = FALSE;

        comm.input.assign(packet.descriptor.input_size, std::byte{});
        std::memcpy(comm.input.data() + read_offset, text.data(), text.size());

        oc::condrv::BasicApiMessage<MemoryComm> message(comm, packet);
        auto outcome = oc::condrv::dispatch_message(state, message, host_io);
        return outcome && message.completion().io_status.Status == oc::core::status_success;
    }

    [[nodiscard]] bool write_console_user_defined_w(
        MemoryComm& comm,
        oc::condrv::ServerState& state,
        TestHostIo& host_io,
        const oc::condrv::ConnectionInformation info,
        const std::wstring_view text,
        const ULONG id) noexcept
    {
        constexpr ULONG api_size = sizeof(CONSOLE_WRITECONSOLE_MSG);
        constexpr ULONG header_size = sizeof(CONSOLE_MSG_HEADER);
        const ULONG read_offset = api_size + header_size;
        const ULONG utf16_bytes = static_cast<ULONG>(text.size() * sizeof(wchar_t));

        oc::condrv::IoPacket packet{};
        packet.payload.user_defined = oc::condrv::UserDefinedPacket{};
        packet.descriptor.identifier.LowPart = id;
        packet.descriptor.function = oc::condrv::console_io_user_defined;
        packet.descriptor.process = info.process;
        packet.descriptor.object = info.output;
        packet.descriptor.input_size = read_offset + utf16_bytes;
        packet.descriptor.output_size = api_size;
        packet.payload.user_defined.msg_header.ApiNumber = static_cast<ULONG>(ConsolepWriteConsole);
        packet.payload.user_defined.msg_header.ApiDescriptorSize = api_size;
        packet.payload.user_defined.u.console_msg_l1.WriteConsole.Unicode = TRUE;

        comm.input.assign(packet.descriptor.input_size, std::byte{});
        if (!text.empty())
        {
            std::memcpy(comm.input.data() + read_offset, text.data(), utf16_bytes);
        }

        oc::condrv::BasicApiMessage<MemoryComm> message(comm, packet);
        auto outcome = oc::condrv::dispatch_message(state, message, host_io);
        return outcome && message.completion().io_status.Status == oc::core::status_success;
    }

    [[nodiscard]] bool set_screen_buffer_size_user_defined(
        MemoryComm& comm,
        oc::condrv::ServerState& state,
        TestHostIo& host_io,
        const oc::condrv::ConnectionInformation info,
        const COORD size,
        const ULONG id) noexcept
    {
        constexpr ULONG api_size = sizeof(CONSOLE_SETSCREENBUFFERSIZE_MSG);
        constexpr ULONG header_size = sizeof(CONSOLE_MSG_HEADER);
        const ULONG read_offset = api_size + header_size;

        oc::condrv::IoPacket packet{};
        packet.payload.user_defined = oc::condrv::UserDefinedPacket{};
        packet.descriptor.identifier.LowPart = id;
        packet.descriptor.function = oc::condrv::console_io_user_defined;
        packet.descriptor.process = info.process;
        packet.descriptor.object = info.output;
        packet.descriptor.input_size = read_offset;
        packet.descriptor.output_size = api_size;
        packet.payload.user_defined.msg_header.ApiNumber = static_cast<ULONG>(ConsolepSetScreenBufferSize);
        packet.payload.user_defined.msg_header.ApiDescriptorSize = api_size;

        packet.payload.user_defined.u.console_msg_l2.SetConsoleScreenBufferSize.Size = size;

        comm.input.assign(packet.descriptor.input_size, std::byte{});

        oc::condrv::BasicApiMessage<MemoryComm> message(comm, packet);
        const auto outcome = oc::condrv::dispatch_message(state, message, host_io);
        return outcome && message.completion().io_status.Status == oc::core::status_success;
    }

    [[nodiscard]] std::optional<wchar_t> read_console_output_char(
        MemoryComm& comm,
        oc::condrv::ServerState& state,
        TestHostIo& host_io,
        const oc::condrv::ConnectionInformation info,
        const COORD coord,
        const ULONG id) noexcept
    {
        constexpr ULONG api_size = sizeof(CONSOLE_READCONSOLEOUTPUTSTRING_MSG);
        constexpr ULONG header_size = sizeof(CONSOLE_MSG_HEADER);
        constexpr ULONG read_offset = api_size + header_size;
        constexpr ULONG output_bytes = sizeof(wchar_t);

        oc::condrv::IoPacket packet{};
        packet.payload.user_defined = oc::condrv::UserDefinedPacket{};
        packet.descriptor.identifier.LowPart = id;
        packet.descriptor.function = oc::condrv::console_io_user_defined;
        packet.descriptor.process = info.process;
        packet.descriptor.object = info.output;
        packet.descriptor.input_size = read_offset;
        packet.descriptor.output_size = api_size + output_bytes;
        packet.payload.user_defined.msg_header.ApiNumber = static_cast<ULONG>(ConsolepReadConsoleOutputString);
        packet.payload.user_defined.msg_header.ApiDescriptorSize = api_size;

        auto& body = packet.payload.user_defined.u.console_msg_l2.ReadConsoleOutputString;
        body.ReadCoord = coord;
        body.StringType = CONSOLE_REAL_UNICODE;
        body.NumRecords = 0;

        comm.input.assign(packet.descriptor.input_size, std::byte{});
        comm.output.clear();

        oc::condrv::BasicApiMessage<MemoryComm> message(comm, packet);
        auto outcome = oc::condrv::dispatch_message(state, message, host_io);
        if (!outcome || message.completion().io_status.Status != oc::core::status_success)
        {
            return std::nullopt;
        }

        if (auto released = message.release_message_buffers(); !released)
        {
            return std::nullopt;
        }

        if (comm.output.size() != api_size + output_bytes)
        {
            return std::nullopt;
        }

        wchar_t value{};
        std::memcpy(&value, comm.output.data() + api_size, sizeof(value));
        return value;
    }

    bool test_raw_write_forwards_bytes_and_sets_information()
    {
        MemoryComm comm{};
        oc::condrv::ServerState state{};
        TestHostIo host_io{};

        auto connect_packet = make_connect_packet(1001, 2002);
        oc::condrv::BasicApiMessage<MemoryComm> connect_message(comm, connect_packet);
        auto connect_outcome = oc::condrv::dispatch_message(state, connect_message, host_io);
        if (!connect_outcome)
        {
            return false;
        }

        auto info = unpack_connection_information(connect_message.completion());

        comm.input = {
            static_cast<std::byte>('h'),
            static_cast<std::byte>('e'),
            static_cast<std::byte>('l'),
            static_cast<std::byte>('l'),
            static_cast<std::byte>('o'),
        };

        oc::condrv::IoPacket packet{};
        packet.descriptor.identifier.LowPart = 2;
        packet.descriptor.function = oc::condrv::console_io_raw_write;
        packet.descriptor.process = info.process;
        packet.descriptor.object = info.output;
        packet.descriptor.input_size = static_cast<ULONG>(comm.input.size());
        packet.descriptor.output_size = 0;

        oc::condrv::BasicApiMessage<MemoryComm> message(comm, packet);
        auto outcome = oc::condrv::dispatch_message(state, message, host_io);
        if (!outcome)
        {
            return false;
        }

        if (message.completion().io_status.Status != oc::core::status_success)
        {
            return false;
        }

        if (message.completion().io_status.Information != comm.input.size())
        {
            return false;
        }

        return host_io.written == comm.input;
    }

    bool test_raw_read_copies_bytes_to_output_buffer()
    {
        MemoryComm comm{};
        oc::condrv::ServerState state{};
        TestHostIo host_io{};

        auto connect_packet = make_connect_packet(1111, 2222);
        oc::condrv::BasicApiMessage<MemoryComm> connect_message(comm, connect_packet);
        auto connect_outcome = oc::condrv::dispatch_message(state, connect_message, host_io);
        if (!connect_outcome)
        {
            return false;
        }

        auto info = unpack_connection_information(connect_message.completion());
        host_io.input = { static_cast<std::byte>('o'), static_cast<std::byte>('k'), static_cast<std::byte>('!') };

        oc::condrv::IoPacket packet{};
        packet.descriptor.identifier.LowPart = 3;
        packet.descriptor.function = oc::condrv::console_io_raw_read;
        packet.descriptor.process = info.process;
        packet.descriptor.object = info.input;
        packet.descriptor.input_size = 0;
        packet.descriptor.output_size = 16;

        oc::condrv::BasicApiMessage<MemoryComm> message(comm, packet);
        auto outcome = oc::condrv::dispatch_message(state, message, host_io);
        if (!outcome)
        {
            return false;
        }

        if (message.completion().io_status.Status != oc::core::status_success)
        {
            return false;
        }

        if (message.completion().io_status.Information != host_io.input.size())
        {
            return false;
        }

        if (auto released = message.release_message_buffers(); !released)
        {
            return false;
        }

        return comm.output == host_io.input;
    }

    bool test_raw_read_processed_input_consumes_ctrl_c_and_sends_end_task()
    {
        MemoryComm comm{};
        oc::condrv::ServerState state{};
        TestHostIo host_io{};

        auto connect_packet = make_connect_packet(1113, 2224);
        oc::condrv::BasicApiMessage<MemoryComm> connect_message(comm, connect_packet);
        auto connect_outcome = oc::condrv::dispatch_message(state, connect_message, host_io);
        if (!connect_outcome)
        {
            return false;
        }

        auto info = unpack_connection_information(connect_message.completion());
        state.set_input_mode(ENABLE_PROCESSED_INPUT);
        host_io.input = { static_cast<std::byte>(0x03), static_cast<std::byte>('o'), static_cast<std::byte>('k') };

        oc::condrv::IoPacket packet{};
        packet.descriptor.identifier.LowPart = 4;
        packet.descriptor.function = oc::condrv::console_io_raw_read;
        packet.descriptor.process = info.process;
        packet.descriptor.object = info.input;
        packet.descriptor.input_size = 0;
        packet.descriptor.output_size = 16;

        oc::condrv::BasicApiMessage<MemoryComm> message(comm, packet);
        auto outcome = oc::condrv::dispatch_message(state, message, host_io);
        if (!outcome || message.completion().io_status.Status != oc::core::status_success)
        {
            return false;
        }

        if (message.completion().io_status.Information != 2)
        {
            return false;
        }

        if (host_io.end_task_pids.size() != 1 || host_io.end_task_pids[0] != 1113)
        {
            return false;
        }

        if (auto released = message.release_message_buffers(); !released)
        {
            return false;
        }

        const std::vector<std::byte> expected = { static_cast<std::byte>('o'), static_cast<std::byte>('k') };
        return comm.output == expected && host_io.input_bytes_available() == 0;
    }

    bool test_raw_read_processed_input_skips_ctrl_c_mid_buffer_and_still_fills_output()
    {
        MemoryComm comm{};
        oc::condrv::ServerState state{};
        TestHostIo host_io{};

        auto connect_packet = make_connect_packet(1114, 2225);
        oc::condrv::BasicApiMessage<MemoryComm> connect_message(comm, connect_packet);
        auto connect_outcome = oc::condrv::dispatch_message(state, connect_message, host_io);
        if (!connect_outcome)
        {
            return false;
        }

        auto info = unpack_connection_information(connect_message.completion());
        state.set_input_mode(ENABLE_PROCESSED_INPUT);
        host_io.input = { static_cast<std::byte>('X'), static_cast<std::byte>(0x03), static_cast<std::byte>('Y') };

        oc::condrv::IoPacket packet{};
        packet.descriptor.identifier.LowPart = 5;
        packet.descriptor.function = oc::condrv::console_io_raw_read;
        packet.descriptor.process = info.process;
        packet.descriptor.object = info.input;
        packet.descriptor.input_size = 0;
        packet.descriptor.output_size = 2;

        oc::condrv::BasicApiMessage<MemoryComm> message(comm, packet);
        auto outcome = oc::condrv::dispatch_message(state, message, host_io);
        if (!outcome || message.completion().io_status.Status != oc::core::status_success)
        {
            return false;
        }

        if (message.completion().io_status.Information != 2)
        {
            return false;
        }

        if (host_io.end_task_pids.size() != 1 || host_io.end_task_pids[0] != 1114)
        {
            return false;
        }

        if (auto released = message.release_message_buffers(); !released)
        {
            return false;
        }

        const std::vector<std::byte> expected = { static_cast<std::byte>('X'), static_cast<std::byte>('Y') };
        return comm.output == expected && host_io.input_bytes_available() == 0;
    }

    bool test_raw_read_processed_input_ctrl_break_returns_alerted_and_flushes_input()
    {
        MemoryComm comm{};
        oc::condrv::ServerState state{};
        TestHostIo host_io{};

        auto connect_packet = make_connect_packet(1119, 2230);
        oc::condrv::BasicApiMessage<MemoryComm> connect_message(comm, connect_packet);
        auto connect_outcome = oc::condrv::dispatch_message(state, connect_message, host_io);
        if (!connect_outcome)
        {
            return false;
        }

        const auto info = unpack_connection_information(connect_message.completion());
        state.set_input_mode(ENABLE_PROCESSED_INPUT);
        state.set_input_code_page(CP_UTF8);

        constexpr std::string_view ctrl_break = "\x1b[3;0;0;1;8;1_";
        constexpr std::string_view tail = "ok";

        host_io.input.clear();
        host_io.input_offset = 0;
        for (const unsigned char ch : ctrl_break)
        {
            host_io.input.push_back(static_cast<std::byte>(ch));
        }
        for (const unsigned char ch : tail)
        {
            host_io.input.push_back(static_cast<std::byte>(ch));
        }

        oc::condrv::IoPacket packet{};
        packet.descriptor.identifier.LowPart = 10;
        packet.descriptor.function = oc::condrv::console_io_raw_read;
        packet.descriptor.process = info.process;
        packet.descriptor.object = info.input;
        packet.descriptor.input_size = 0;
        packet.descriptor.output_size = 16;

        oc::condrv::BasicApiMessage<MemoryComm> message(comm, packet);
        auto outcome = oc::condrv::dispatch_message(state, message, host_io);
        if (!outcome || outcome->reply_pending || message.completion().io_status.Status != oc::core::status_alerted)
        {
            return false;
        }

        if (message.completion().io_status.Information != 0)
        {
            return false;
        }

        if (host_io.end_task_pids.size() != 1 || host_io.end_task_pids[0] != 1119)
        {
            return false;
        }

        if (host_io.end_task_events.size() != 1 || host_io.end_task_events[0] != CTRL_BREAK_EVENT)
        {
            return false;
        }

        if (host_io.end_task_flags.size() != 1 || host_io.end_task_flags[0] != oc::core::console_ctrl_break_flag)
        {
            return false;
        }

        if (auto released = message.release_message_buffers(); !released)
        {
            return false;
        }

        return comm.output.empty() && host_io.input_bytes_available() == 0;
    }

    bool test_raw_read_decodes_win32_input_mode_character_key()
    {
        MemoryComm comm{};
        oc::condrv::ServerState state{};
        TestHostIo host_io{};

        auto connect_packet = make_connect_packet(1115, 2226);
        oc::condrv::BasicApiMessage<MemoryComm> connect_message(comm, connect_packet);
        auto connect_outcome = oc::condrv::dispatch_message(state, connect_message, host_io);
        if (!connect_outcome)
        {
            return false;
        }

        auto info = unpack_connection_information(connect_message.completion());
        state.set_input_code_page(CP_UTF8);

        constexpr std::string_view seq = "\x1b[65;0;97;1;0;1_";
        host_io.input.clear();
        host_io.input_offset = 0;
        for (const unsigned char ch : seq)
        {
            host_io.input.push_back(static_cast<std::byte>(ch));
        }

        oc::condrv::IoPacket packet{};
        packet.descriptor.identifier.LowPart = 6;
        packet.descriptor.function = oc::condrv::console_io_raw_read;
        packet.descriptor.process = info.process;
        packet.descriptor.object = info.input;
        packet.descriptor.input_size = 0;
        packet.descriptor.output_size = 16;

        oc::condrv::BasicApiMessage<MemoryComm> message(comm, packet);
        auto outcome = oc::condrv::dispatch_message(state, message, host_io);
        if (!outcome || outcome->reply_pending || message.completion().io_status.Status != oc::core::status_success)
        {
            return false;
        }

        if (message.completion().io_status.Information != 1)
        {
            return false;
        }

        if (auto released = message.release_message_buffers(); !released)
        {
            return false;
        }

        const std::vector<std::byte> expected = { static_cast<std::byte>('a') };
        return comm.output == expected && host_io.input_bytes_available() == 0;
    }

    bool test_raw_read_processed_input_consumes_win32_ctrl_c_and_sends_end_task()
    {
        MemoryComm comm{};
        oc::condrv::ServerState state{};
        TestHostIo host_io{};

        auto connect_packet = make_connect_packet(1116, 2227);
        oc::condrv::BasicApiMessage<MemoryComm> connect_message(comm, connect_packet);
        auto connect_outcome = oc::condrv::dispatch_message(state, connect_message, host_io);
        if (!connect_outcome)
        {
            return false;
        }

        auto info = unpack_connection_information(connect_message.completion());
        state.set_input_mode(ENABLE_PROCESSED_INPUT);
        state.set_input_code_page(CP_UTF8);

        constexpr std::string_view ctrl_c = "\x1b[67;0;0;1;8;1_";
        constexpr std::string_view tail = "ok";

        host_io.input.clear();
        host_io.input_offset = 0;
        for (const unsigned char ch : ctrl_c)
        {
            host_io.input.push_back(static_cast<std::byte>(ch));
        }
        for (const unsigned char ch : tail)
        {
            host_io.input.push_back(static_cast<std::byte>(ch));
        }

        oc::condrv::IoPacket packet{};
        packet.descriptor.identifier.LowPart = 7;
        packet.descriptor.function = oc::condrv::console_io_raw_read;
        packet.descriptor.process = info.process;
        packet.descriptor.object = info.input;
        packet.descriptor.input_size = 0;
        packet.descriptor.output_size = 16;

        oc::condrv::BasicApiMessage<MemoryComm> message(comm, packet);
        auto outcome = oc::condrv::dispatch_message(state, message, host_io);
        if (!outcome || outcome->reply_pending || message.completion().io_status.Status != oc::core::status_success)
        {
            return false;
        }

        if (message.completion().io_status.Information != 2)
        {
            return false;
        }

        if (host_io.end_task_pids.size() != 1 || host_io.end_task_pids[0] != 1116)
        {
            return false;
        }

        if (auto released = message.release_message_buffers(); !released)
        {
            return false;
        }

        const std::vector<std::byte> expected = { static_cast<std::byte>('o'), static_cast<std::byte>('k') };
        return comm.output == expected && host_io.input_bytes_available() == 0;
    }

    bool test_raw_read_consumes_da1_and_focus_sequences_before_character_key()
    {
        MemoryComm comm{};
        oc::condrv::ServerState state{};
        TestHostIo host_io{};

        auto connect_packet = make_connect_packet(1117, 2228);
        oc::condrv::BasicApiMessage<MemoryComm> connect_message(comm, connect_packet);
        auto connect_outcome = oc::condrv::dispatch_message(state, connect_message, host_io);
        if (!connect_outcome)
        {
            return false;
        }

        auto info = unpack_connection_information(connect_message.completion());
        state.set_input_code_page(CP_UTF8);

        constexpr std::string_view payload =
            "\x1b[?62;c"
            "\x1b[I"
            "\x1b[O"
            "\x1b[65;0;97;1;0;1_";

        host_io.input.clear();
        host_io.input_offset = 0;
        for (const unsigned char ch : payload)
        {
            host_io.input.push_back(static_cast<std::byte>(ch));
        }

        oc::condrv::IoPacket packet{};
        packet.descriptor.identifier.LowPart = 8;
        packet.descriptor.function = oc::condrv::console_io_raw_read;
        packet.descriptor.process = info.process;
        packet.descriptor.object = info.input;
        packet.descriptor.input_size = 0;
        packet.descriptor.output_size = 16;

        oc::condrv::BasicApiMessage<MemoryComm> message(comm, packet);
        auto outcome = oc::condrv::dispatch_message(state, message, host_io);
        if (!outcome || outcome->reply_pending || message.completion().io_status.Status != oc::core::status_success)
        {
            return false;
        }

        if (message.completion().io_status.Information != 1)
        {
            return false;
        }

        if (auto released = message.release_message_buffers(); !released)
        {
            return false;
        }

        const std::vector<std::byte> expected = { static_cast<std::byte>('a') };
        return comm.output == expected && host_io.input_bytes_available() == 0;
    }

    bool test_raw_read_split_win32_sequence_reply_pends_and_drains_prefix()
    {
        MemoryComm comm{};
        oc::condrv::ServerState state{};
        TestHostIo host_io{};

        auto connect_packet = make_connect_packet(1118, 2229);
        oc::condrv::BasicApiMessage<MemoryComm> connect_message(comm, connect_packet);
        auto connect_outcome = oc::condrv::dispatch_message(state, connect_message, host_io);
        if (!connect_outcome)
        {
            return false;
        }

        auto info = unpack_connection_information(connect_message.completion());
        state.set_input_code_page(CP_UTF8);

        constexpr std::string_view fragment1 = "\x1b[65;0;";
        host_io.input.clear();
        host_io.input_offset = 0;
        for (const unsigned char ch : fragment1)
        {
            host_io.input.push_back(static_cast<std::byte>(ch));
        }

        oc::condrv::IoPacket packet{};
        packet.descriptor.identifier.LowPart = 9;
        packet.descriptor.function = oc::condrv::console_io_raw_read;
        packet.descriptor.process = info.process;
        packet.descriptor.object = info.input;
        packet.descriptor.input_size = 0;
        packet.descriptor.output_size = 16;

        oc::condrv::BasicApiMessage<MemoryComm> message(comm, packet);
        auto outcome = oc::condrv::dispatch_message(state, message, host_io);
        if (!outcome || !outcome->reply_pending)
        {
            return false;
        }

        auto* handle = state.find_object(info.input);
        if (handle == nullptr || handle->pending_input_bytes.size() != fragment1.size())
        {
            return false;
        }

        if (host_io.input_bytes_available() != 0)
        {
            return false;
        }

        constexpr std::string_view fragment2 = "97;1;0;1_";
        for (const unsigned char ch : fragment2)
        {
            host_io.input.push_back(static_cast<std::byte>(ch));
        }

        outcome = oc::condrv::dispatch_message(state, message, host_io);
        if (!outcome || outcome->reply_pending || message.completion().io_status.Status != oc::core::status_success)
        {
            return false;
        }

        if (message.completion().io_status.Information != 1)
        {
            return false;
        }

        if (auto released = message.release_message_buffers(); !released)
        {
            return false;
        }

        const std::vector<std::byte> expected = { static_cast<std::byte>('a') };
        return comm.output == expected && host_io.input_bytes_available() == 0 && handle->pending_input_bytes.size() == 0;
    }

    bool test_raw_write_updates_screen_buffer_model()
    {
        MemoryComm comm{};
        oc::condrv::ServerState state{};
        TestHostIo host_io{};

        auto connect_packet = make_connect_packet(1234, 5678);
        oc::condrv::BasicApiMessage<MemoryComm> connect_message(comm, connect_packet);
        auto connect_outcome = oc::condrv::dispatch_message(state, connect_message, host_io);
        if (!connect_outcome)
        {
            return false;
        }

        auto info = unpack_connection_information(connect_message.completion());

        comm.input = {
            static_cast<std::byte>('a'),
            static_cast<std::byte>('b'),
            static_cast<std::byte>('c'),
        };

        oc::condrv::IoPacket packet{};
        packet.descriptor.identifier.LowPart = 60;
        packet.descriptor.function = oc::condrv::console_io_raw_write;
        packet.descriptor.process = info.process;
        packet.descriptor.object = info.output;
        packet.descriptor.input_size = static_cast<ULONG>(comm.input.size());
        packet.descriptor.output_size = 0;

        oc::condrv::BasicApiMessage<MemoryComm> message(comm, packet);
        auto outcome = oc::condrv::dispatch_message(state, message, host_io);
        if (!outcome || message.completion().io_status.Status != oc::core::status_success)
        {
            return false;
        }

        // Read back from the screen buffer via ReadConsoleOutputString to ensure raw writes
        // update the in-memory buffer model (matching the inbox host's RAW_WRITE path).
        {
            constexpr ULONG read_api_size = sizeof(CONSOLE_READCONSOLEOUTPUTSTRING_MSG);
            constexpr ULONG read_header_size = sizeof(CONSOLE_MSG_HEADER);
            constexpr ULONG read_read_offset = read_api_size + read_header_size;
            constexpr ULONG output_bytes = 3 * sizeof(wchar_t);

            oc::condrv::IoPacket read_packet{};
            read_packet.payload.user_defined = oc::condrv::UserDefinedPacket{};
            read_packet.descriptor.identifier.LowPart = 61;
            read_packet.descriptor.function = oc::condrv::console_io_user_defined;
            read_packet.descriptor.process = info.process;
            read_packet.descriptor.object = info.output;
            read_packet.descriptor.input_size = read_read_offset;
            read_packet.descriptor.output_size = read_api_size + output_bytes;
            read_packet.payload.user_defined.msg_header.ApiNumber = static_cast<ULONG>(ConsolepReadConsoleOutputString);
            read_packet.payload.user_defined.msg_header.ApiDescriptorSize = read_api_size;

            auto& body = read_packet.payload.user_defined.u.console_msg_l2.ReadConsoleOutputString;
            body.ReadCoord = COORD{ 0, 0 };
            body.StringType = CONSOLE_REAL_UNICODE;
            body.NumRecords = 0;

            comm.input.assign(read_packet.descriptor.input_size, std::byte{});
            comm.output.clear();

            oc::condrv::BasicApiMessage<MemoryComm> read_message(comm, read_packet);
            auto read_outcome = oc::condrv::dispatch_message(state, read_message, host_io);
            if (!read_outcome || read_message.completion().io_status.Status != oc::core::status_success)
            {
                return false;
            }

            if (auto released = read_message.release_message_buffers(); !released)
            {
                return false;
            }

            if (comm.output.size() != read_api_size + output_bytes)
            {
                return false;
            }

            wchar_t chars[3]{};
            std::memcpy(chars, comm.output.data() + read_api_size, output_bytes);
            return chars[0] == L'a' && chars[1] == L'b' && chars[2] == L'c';
        }
    }

    bool test_raw_flush_clears_input_queue()
    {
        MemoryComm comm{};
        oc::condrv::ServerState state{};
        TestHostIo host_io{};

        auto connect_packet = make_connect_packet(2468, 1357);
        oc::condrv::BasicApiMessage<MemoryComm> connect_message(comm, connect_packet);
        auto connect_outcome = oc::condrv::dispatch_message(state, connect_message, host_io);
        if (!connect_outcome)
        {
            return false;
        }

        auto info = unpack_connection_information(connect_message.completion());
        host_io.input = { static_cast<std::byte>('x'), static_cast<std::byte>('y') };

        oc::condrv::IoPacket packet{};
        packet.descriptor.identifier.LowPart = 62;
        packet.descriptor.function = oc::condrv::console_io_raw_flush;
        packet.descriptor.process = info.process;
        packet.descriptor.object = info.input;
        packet.descriptor.input_size = 0;
        packet.descriptor.output_size = 0;

        oc::condrv::BasicApiMessage<MemoryComm> message(comm, packet);
        auto outcome = oc::condrv::dispatch_message(state, message, host_io);
        if (!outcome || message.completion().io_status.Status != oc::core::status_success)
        {
            return false;
        }

        return host_io.input_bytes_available() == 0;
    }

    bool test_raw_read_process_control_z_consumes_one_byte_and_returns_zero()
    {
        MemoryComm comm{};
        oc::condrv::ServerState state{};
        TestHostIo host_io{};

        auto connect_packet = make_connect_packet(4321, 8765);
        oc::condrv::BasicApiMessage<MemoryComm> connect_message(comm, connect_packet);
        auto connect_outcome = oc::condrv::dispatch_message(state, connect_message, host_io);
        if (!connect_outcome)
        {
            return false;
        }

        auto info = unpack_connection_information(connect_message.completion());
        host_io.input = { static_cast<std::byte>(0x1a), static_cast<std::byte>('A') };

        // First read: CTRL+Z returns 0 bytes but should consume only the CTRL+Z marker.
        {
            oc::condrv::IoPacket packet{};
            packet.descriptor.identifier.LowPart = 63;
            packet.descriptor.function = oc::condrv::console_io_raw_read;
            packet.descriptor.process = info.process;
            packet.descriptor.object = info.input;
            packet.descriptor.input_size = 0;
            packet.descriptor.output_size = 4;

            oc::condrv::BasicApiMessage<MemoryComm> message(comm, packet);
            auto outcome = oc::condrv::dispatch_message(state, message, host_io);
            if (!outcome || message.completion().io_status.Status != oc::core::status_success)
            {
                return false;
            }

            if (message.completion().io_status.Information != 0)
            {
                return false;
            }

            if (auto released = message.release_message_buffers(); !released)
            {
                return false;
            }
        }

        // Second read: the following byte should still be available.
        {
            oc::condrv::IoPacket packet{};
            packet.descriptor.identifier.LowPart = 64;
            packet.descriptor.function = oc::condrv::console_io_raw_read;
            packet.descriptor.process = info.process;
            packet.descriptor.object = info.input;
            packet.descriptor.input_size = 0;
            packet.descriptor.output_size = 4;

            comm.output.clear();

            oc::condrv::BasicApiMessage<MemoryComm> message(comm, packet);
            auto outcome = oc::condrv::dispatch_message(state, message, host_io);
            if (!outcome || message.completion().io_status.Status != oc::core::status_success)
            {
                return false;
            }

            if (message.completion().io_status.Information != 1)
            {
                return false;
            }

            if (auto released = message.release_message_buffers(); !released)
            {
                return false;
            }

            return comm.output.size() == 1 && comm.output[0] == static_cast<std::byte>('A');
        }
    }

    bool test_raw_write_rejects_input_handle()
    {
        MemoryComm comm{};
        oc::condrv::ServerState state{};
        TestHostIo host_io{};

        auto connect_packet = make_connect_packet(3333, 4444);
        oc::condrv::BasicApiMessage<MemoryComm> connect_message(comm, connect_packet);
        auto connect_outcome = oc::condrv::dispatch_message(state, connect_message, host_io);
        if (!connect_outcome)
        {
            return false;
        }

        auto info = unpack_connection_information(connect_message.completion());

        comm.input = { static_cast<std::byte>('x') };

        oc::condrv::IoPacket packet{};
        packet.descriptor.identifier.LowPart = 4;
        packet.descriptor.function = oc::condrv::console_io_raw_write;
        packet.descriptor.process = info.process;
        packet.descriptor.object = info.input; // wrong kind
        packet.descriptor.input_size = static_cast<ULONG>(comm.input.size());
        packet.descriptor.output_size = 0;

        oc::condrv::BasicApiMessage<MemoryComm> message(comm, packet);
        auto outcome = oc::condrv::dispatch_message(state, message, host_io);
        if (!outcome)
        {
            return false;
        }

        return message.completion().io_status.Status == oc::core::status_invalid_handle &&
               message.completion().io_status.Information == 0;
    }

    bool test_user_defined_write_console_a_forwards_bytes()
    {
        MemoryComm comm{};
        oc::condrv::ServerState state{};
        TestHostIo host_io{};

        auto connect_packet = make_connect_packet(5555, 6666);
        oc::condrv::BasicApiMessage<MemoryComm> connect_message(comm, connect_packet);
        auto connect_outcome = oc::condrv::dispatch_message(state, connect_message, host_io);
        if (!connect_outcome)
        {
            return false;
        }

        auto info = unpack_connection_information(connect_message.completion());

        constexpr std::string_view text = "abc";
        constexpr ULONG api_size = sizeof(CONSOLE_WRITECONSOLE_MSG);
        constexpr ULONG header_size = sizeof(CONSOLE_MSG_HEADER);
        const ULONG read_offset = api_size + header_size;

        oc::condrv::IoPacket packet{};
        packet.payload.user_defined = oc::condrv::UserDefinedPacket{};
        packet.descriptor.identifier.LowPart = 5;
        packet.descriptor.function = oc::condrv::console_io_user_defined;
        packet.descriptor.process = info.process;
        packet.descriptor.object = info.output;
        packet.descriptor.input_size = read_offset + static_cast<ULONG>(text.size());
        packet.descriptor.output_size = api_size;
        packet.payload.user_defined.msg_header.ApiNumber = static_cast<ULONG>(ConsolepWriteConsole);
        packet.payload.user_defined.msg_header.ApiDescriptorSize = api_size;
        packet.payload.user_defined.u.console_msg_l1.WriteConsole.Unicode = FALSE;

        comm.input.assign(packet.descriptor.input_size, std::byte{});
        std::memcpy(comm.input.data() + read_offset, text.data(), text.size());

        oc::condrv::BasicApiMessage<MemoryComm> message(comm, packet);
        auto outcome = oc::condrv::dispatch_message(state, message, host_io);
        if (!outcome)
        {
            return false;
        }

        if (message.completion().io_status.Status != oc::core::status_success)
        {
            return false;
        }

        const auto written = message.packet().payload.user_defined.u.console_msg_l1.WriteConsole.NumBytes;
        return written == text.size() &&
               message.completion().io_status.Information == written &&
               host_io.written.size() == text.size() &&
               std::memcmp(host_io.written.data(), text.data(), text.size()) == 0;
    }

    bool test_user_defined_write_console_w_utf8_encodes()
    {
        MemoryComm comm{};
        oc::condrv::ServerState state{};
        TestHostIo host_io{};

        auto connect_packet = make_connect_packet(7777, 8888);
        oc::condrv::BasicApiMessage<MemoryComm> connect_message(comm, connect_packet);
        auto connect_outcome = oc::condrv::dispatch_message(state, connect_message, host_io);
        if (!connect_outcome)
        {
            return false;
        }

        auto info = unpack_connection_information(connect_message.completion());

        constexpr std::wstring_view text = L"hi";
        constexpr ULONG api_size = sizeof(CONSOLE_WRITECONSOLE_MSG);
        constexpr ULONG header_size = sizeof(CONSOLE_MSG_HEADER);
        const ULONG read_offset = api_size + header_size;
        const ULONG utf16_bytes = static_cast<ULONG>(text.size() * sizeof(wchar_t));

        oc::condrv::IoPacket packet{};
        packet.payload.user_defined = oc::condrv::UserDefinedPacket{};
        packet.descriptor.identifier.LowPart = 6;
        packet.descriptor.function = oc::condrv::console_io_user_defined;
        packet.descriptor.process = info.process;
        packet.descriptor.object = info.output;
        packet.descriptor.input_size = read_offset + utf16_bytes;
        packet.descriptor.output_size = api_size;
        packet.payload.user_defined.msg_header.ApiNumber = static_cast<ULONG>(ConsolepWriteConsole);
        packet.payload.user_defined.msg_header.ApiDescriptorSize = api_size;
        packet.payload.user_defined.u.console_msg_l1.WriteConsole.Unicode = TRUE;

        comm.input.assign(packet.descriptor.input_size, std::byte{});
        std::memcpy(comm.input.data() + read_offset, text.data(), utf16_bytes);

        oc::condrv::BasicApiMessage<MemoryComm> message(comm, packet);
        auto outcome = oc::condrv::dispatch_message(state, message, host_io);
        if (!outcome)
        {
            return false;
        }

        if (message.completion().io_status.Status != oc::core::status_success)
        {
            return false;
        }

        const auto reported = message.packet().payload.user_defined.u.console_msg_l1.WriteConsole.NumBytes;
        if (reported != utf16_bytes || message.completion().io_status.Information != reported)
        {
            return false;
        }

        // ASCII subset: UTF-8 bytes match UTF-16 code points for this string.
        return host_io.written.size() == text.size() &&
               std::memcmp(host_io.written.data(), "hi", 2) == 0;
    }

    bool test_user_defined_write_console_a_updates_screen_buffer_model()
    {
        MemoryComm comm{};
        oc::condrv::ServerState state{};
        TestHostIo host_io{};

        auto connect_packet = make_connect_packet(10001, 10002);
        oc::condrv::BasicApiMessage<MemoryComm> connect_message(comm, connect_packet);
        auto connect_outcome = oc::condrv::dispatch_message(state, connect_message, host_io);
        if (!connect_outcome)
        {
            return false;
        }

        auto info = unpack_connection_information(connect_message.completion());

        constexpr std::string_view text = "abc";
        constexpr ULONG api_size = sizeof(CONSOLE_WRITECONSOLE_MSG);
        constexpr ULONG header_size = sizeof(CONSOLE_MSG_HEADER);
        const ULONG read_offset = api_size + header_size;

        oc::condrv::IoPacket packet{};
        packet.payload.user_defined = oc::condrv::UserDefinedPacket{};
        packet.descriptor.identifier.LowPart = 55;
        packet.descriptor.function = oc::condrv::console_io_user_defined;
        packet.descriptor.process = info.process;
        packet.descriptor.object = info.output;
        packet.descriptor.input_size = read_offset + static_cast<ULONG>(text.size());
        packet.descriptor.output_size = api_size;
        packet.payload.user_defined.msg_header.ApiNumber = static_cast<ULONG>(ConsolepWriteConsole);
        packet.payload.user_defined.msg_header.ApiDescriptorSize = api_size;
        packet.payload.user_defined.u.console_msg_l1.WriteConsole.Unicode = FALSE;

        comm.input.assign(packet.descriptor.input_size, std::byte{});
        std::memcpy(comm.input.data() + read_offset, text.data(), text.size());

        oc::condrv::BasicApiMessage<MemoryComm> message(comm, packet);
        auto outcome = oc::condrv::dispatch_message(state, message, host_io);
        if (!outcome || message.completion().io_status.Status != oc::core::status_success)
        {
            return false;
        }

        // Read back from the screen buffer via ReadConsoleOutputString.
        {
            constexpr ULONG read_api_size = sizeof(CONSOLE_READCONSOLEOUTPUTSTRING_MSG);
            constexpr ULONG read_header_size = sizeof(CONSOLE_MSG_HEADER);
            constexpr ULONG read_read_offset = read_api_size + read_header_size;
            constexpr ULONG output_bytes = 3 * sizeof(wchar_t);

            oc::condrv::IoPacket read_packet{};
            read_packet.payload.user_defined = oc::condrv::UserDefinedPacket{};
            read_packet.descriptor.identifier.LowPart = 56;
            read_packet.descriptor.function = oc::condrv::console_io_user_defined;
            read_packet.descriptor.process = info.process;
            read_packet.descriptor.object = info.output;
            read_packet.descriptor.input_size = read_read_offset;
            read_packet.descriptor.output_size = read_api_size + output_bytes;
            read_packet.payload.user_defined.msg_header.ApiNumber = static_cast<ULONG>(ConsolepReadConsoleOutputString);
            read_packet.payload.user_defined.msg_header.ApiDescriptorSize = read_api_size;

            auto& body = read_packet.payload.user_defined.u.console_msg_l2.ReadConsoleOutputString;
            body.ReadCoord = COORD{ 0, 0 };
            body.StringType = CONSOLE_REAL_UNICODE;
            body.NumRecords = 0;

            comm.input.assign(read_packet.descriptor.input_size, std::byte{});
            comm.output.clear();

            oc::condrv::BasicApiMessage<MemoryComm> read_message(comm, read_packet);
            auto read_outcome = oc::condrv::dispatch_message(state, read_message, host_io);
            if (!read_outcome || read_message.completion().io_status.Status != oc::core::status_success)
            {
                return false;
            }

            if (auto released = read_message.release_message_buffers(); !released)
            {
                return false;
            }

            if (comm.output.size() != read_api_size + output_bytes)
            {
                return false;
            }

            wchar_t chars[3]{};
            std::memcpy(chars, comm.output.data() + read_api_size, output_bytes);
            return chars[0] == L'a' && chars[1] == L'b' && chars[2] == L'c';
        }
    }

    bool test_user_defined_write_console_w_updates_screen_buffer_model()
    {
        MemoryComm comm{};
        oc::condrv::ServerState state{};
        TestHostIo host_io{};

        auto connect_packet = make_connect_packet(10003, 10004);
        oc::condrv::BasicApiMessage<MemoryComm> connect_message(comm, connect_packet);
        auto connect_outcome = oc::condrv::dispatch_message(state, connect_message, host_io);
        if (!connect_outcome)
        {
            return false;
        }

        auto info = unpack_connection_information(connect_message.completion());

        constexpr std::wstring_view text = L"hi";
        constexpr ULONG api_size = sizeof(CONSOLE_WRITECONSOLE_MSG);
        constexpr ULONG header_size = sizeof(CONSOLE_MSG_HEADER);
        const ULONG read_offset = api_size + header_size;
        const ULONG utf16_bytes = static_cast<ULONG>(text.size() * sizeof(wchar_t));

        oc::condrv::IoPacket packet{};
        packet.payload.user_defined = oc::condrv::UserDefinedPacket{};
        packet.descriptor.identifier.LowPart = 57;
        packet.descriptor.function = oc::condrv::console_io_user_defined;
        packet.descriptor.process = info.process;
        packet.descriptor.object = info.output;
        packet.descriptor.input_size = read_offset + utf16_bytes;
        packet.descriptor.output_size = api_size;
        packet.payload.user_defined.msg_header.ApiNumber = static_cast<ULONG>(ConsolepWriteConsole);
        packet.payload.user_defined.msg_header.ApiDescriptorSize = api_size;
        packet.payload.user_defined.u.console_msg_l1.WriteConsole.Unicode = TRUE;

        comm.input.assign(packet.descriptor.input_size, std::byte{});
        std::memcpy(comm.input.data() + read_offset, text.data(), utf16_bytes);

        oc::condrv::BasicApiMessage<MemoryComm> message(comm, packet);
        auto outcome = oc::condrv::dispatch_message(state, message, host_io);
        if (!outcome || message.completion().io_status.Status != oc::core::status_success)
        {
            return false;
        }

        // Read back from the screen buffer via ReadConsoleOutputString.
        {
            constexpr ULONG read_api_size = sizeof(CONSOLE_READCONSOLEOUTPUTSTRING_MSG);
            constexpr ULONG read_header_size = sizeof(CONSOLE_MSG_HEADER);
            constexpr ULONG read_read_offset = read_api_size + read_header_size;
            constexpr ULONG output_bytes = 2 * sizeof(wchar_t);

            oc::condrv::IoPacket read_packet{};
            read_packet.payload.user_defined = oc::condrv::UserDefinedPacket{};
            read_packet.descriptor.identifier.LowPart = 58;
            read_packet.descriptor.function = oc::condrv::console_io_user_defined;
            read_packet.descriptor.process = info.process;
            read_packet.descriptor.object = info.output;
            read_packet.descriptor.input_size = read_read_offset;
            read_packet.descriptor.output_size = read_api_size + output_bytes;
            read_packet.payload.user_defined.msg_header.ApiNumber = static_cast<ULONG>(ConsolepReadConsoleOutputString);
            read_packet.payload.user_defined.msg_header.ApiDescriptorSize = read_api_size;

            auto& body = read_packet.payload.user_defined.u.console_msg_l2.ReadConsoleOutputString;
            body.ReadCoord = COORD{ 0, 0 };
            body.StringType = CONSOLE_REAL_UNICODE;
            body.NumRecords = 0;

            comm.input.assign(read_packet.descriptor.input_size, std::byte{});
            comm.output.clear();

            oc::condrv::BasicApiMessage<MemoryComm> read_message(comm, read_packet);
            auto read_outcome = oc::condrv::dispatch_message(state, read_message, host_io);
            if (!read_outcome || read_message.completion().io_status.Status != oc::core::status_success)
            {
                return false;
            }

            if (auto released = read_message.release_message_buffers(); !released)
            {
                return false;
            }

            if (comm.output.size() != read_api_size + output_bytes)
            {
                return false;
            }

            wchar_t chars[2]{};
            std::memcpy(chars, comm.output.data() + read_api_size, output_bytes);
            return chars[0] == L'h' && chars[1] == L'i';
        }
    }

    bool test_write_console_newline_auto_return_resets_column()
    {
        MemoryComm comm{};
        oc::condrv::ServerState state{};
        TestHostIo host_io{};

        auto connect_packet = make_connect_packet(20003, 20004);
        oc::condrv::BasicApiMessage<MemoryComm> connect_message(comm, connect_packet);
        auto connect_outcome = oc::condrv::dispatch_message(state, connect_message, host_io);
        if (!connect_outcome)
        {
            return false;
        }

        state.set_output_mode(ENABLE_PROCESSED_OUTPUT | ENABLE_WRAP_AT_EOL_OUTPUT);

        auto info = unpack_connection_information(connect_message.completion());

        constexpr std::string_view text = "ab\nc";
        constexpr ULONG api_size = sizeof(CONSOLE_WRITECONSOLE_MSG);
        constexpr ULONG header_size = sizeof(CONSOLE_MSG_HEADER);
        const ULONG read_offset = api_size + header_size;

        oc::condrv::IoPacket packet{};
        packet.payload.user_defined = oc::condrv::UserDefinedPacket{};
        packet.descriptor.identifier.LowPart = 59;
        packet.descriptor.function = oc::condrv::console_io_user_defined;
        packet.descriptor.process = info.process;
        packet.descriptor.object = info.output;
        packet.descriptor.input_size = read_offset + static_cast<ULONG>(text.size());
        packet.descriptor.output_size = api_size;
        packet.payload.user_defined.msg_header.ApiNumber = static_cast<ULONG>(ConsolepWriteConsole);
        packet.payload.user_defined.msg_header.ApiDescriptorSize = api_size;
        packet.payload.user_defined.u.console_msg_l1.WriteConsole.Unicode = FALSE;

        comm.input.assign(packet.descriptor.input_size, std::byte{});
        std::memcpy(comm.input.data() + read_offset, text.data(), text.size());

        oc::condrv::BasicApiMessage<MemoryComm> message(comm, packet);
        auto outcome = oc::condrv::dispatch_message(state, message, host_io);
        if (!outcome || message.completion().io_status.Status != oc::core::status_success)
        {
            return false;
        }

        // Default behavior (DISABLE_NEWLINE_AUTO_RETURN not set): LF performs an implicit CRLF
        // translation in the buffer model, so the next character starts at column 0.
        {
            constexpr ULONG read_api_size = sizeof(CONSOLE_READCONSOLEOUTPUTSTRING_MSG);
            constexpr ULONG read_header_size = sizeof(CONSOLE_MSG_HEADER);
            constexpr ULONG read_read_offset = read_api_size + read_header_size;
            constexpr ULONG output_bytes = 3 * sizeof(wchar_t);

            oc::condrv::IoPacket read_packet{};
            read_packet.payload.user_defined = oc::condrv::UserDefinedPacket{};
            read_packet.descriptor.identifier.LowPart = 60;
            read_packet.descriptor.function = oc::condrv::console_io_user_defined;
            read_packet.descriptor.process = info.process;
            read_packet.descriptor.object = info.output;
            read_packet.descriptor.input_size = read_read_offset;
            read_packet.descriptor.output_size = read_api_size + output_bytes;
            read_packet.payload.user_defined.msg_header.ApiNumber = static_cast<ULONG>(ConsolepReadConsoleOutputString);
            read_packet.payload.user_defined.msg_header.ApiDescriptorSize = read_api_size;

            auto& body = read_packet.payload.user_defined.u.console_msg_l2.ReadConsoleOutputString;
            body.ReadCoord = COORD{ 0, 1 };
            body.StringType = CONSOLE_REAL_UNICODE;
            body.NumRecords = 0;

            comm.input.assign(read_packet.descriptor.input_size, std::byte{});
            comm.output.clear();

            oc::condrv::BasicApiMessage<MemoryComm> read_message(comm, read_packet);
            auto read_outcome = oc::condrv::dispatch_message(state, read_message, host_io);
            if (!read_outcome || read_message.completion().io_status.Status != oc::core::status_success)
            {
                return false;
            }

            if (auto released = read_message.release_message_buffers(); !released)
            {
                return false;
            }

            if (comm.output.size() != read_api_size + output_bytes)
            {
                return false;
            }

            wchar_t chars[3]{};
            std::memcpy(chars, comm.output.data() + read_api_size, output_bytes);
            return chars[0] == L'c' && chars[1] == L' ' && chars[2] == L' ';
        }
    }

    bool test_write_console_disable_newline_auto_return_preserves_column()
    {
        MemoryComm comm{};
        oc::condrv::ServerState state{};
        TestHostIo host_io{};

        auto connect_packet = make_connect_packet(20005, 20006);
        oc::condrv::BasicApiMessage<MemoryComm> connect_message(comm, connect_packet);
        auto connect_outcome = oc::condrv::dispatch_message(state, connect_message, host_io);
        if (!connect_outcome)
        {
            return false;
        }

        state.set_output_mode(ENABLE_PROCESSED_OUTPUT | ENABLE_WRAP_AT_EOL_OUTPUT | DISABLE_NEWLINE_AUTO_RETURN);

        auto info = unpack_connection_information(connect_message.completion());

        constexpr std::string_view text = "ab\nc";
        constexpr ULONG api_size = sizeof(CONSOLE_WRITECONSOLE_MSG);
        constexpr ULONG header_size = sizeof(CONSOLE_MSG_HEADER);
        const ULONG read_offset = api_size + header_size;

        oc::condrv::IoPacket packet{};
        packet.payload.user_defined = oc::condrv::UserDefinedPacket{};
        packet.descriptor.identifier.LowPart = 61;
        packet.descriptor.function = oc::condrv::console_io_user_defined;
        packet.descriptor.process = info.process;
        packet.descriptor.object = info.output;
        packet.descriptor.input_size = read_offset + static_cast<ULONG>(text.size());
        packet.descriptor.output_size = api_size;
        packet.payload.user_defined.msg_header.ApiNumber = static_cast<ULONG>(ConsolepWriteConsole);
        packet.payload.user_defined.msg_header.ApiDescriptorSize = api_size;
        packet.payload.user_defined.u.console_msg_l1.WriteConsole.Unicode = FALSE;

        comm.input.assign(packet.descriptor.input_size, std::byte{});
        std::memcpy(comm.input.data() + read_offset, text.data(), text.size());

        oc::condrv::BasicApiMessage<MemoryComm> message(comm, packet);
        auto outcome = oc::condrv::dispatch_message(state, message, host_io);
        if (!outcome || message.completion().io_status.Status != oc::core::status_success)
        {
            return false;
        }

        // When DISABLE_NEWLINE_AUTO_RETURN is set, LF performs a line feed only and the
        // following character starts at the previous column.
        {
            constexpr ULONG read_api_size = sizeof(CONSOLE_READCONSOLEOUTPUTSTRING_MSG);
            constexpr ULONG read_header_size = sizeof(CONSOLE_MSG_HEADER);
            constexpr ULONG read_read_offset = read_api_size + read_header_size;
            constexpr ULONG output_bytes = 3 * sizeof(wchar_t);

            oc::condrv::IoPacket read_packet{};
            read_packet.payload.user_defined = oc::condrv::UserDefinedPacket{};
            read_packet.descriptor.identifier.LowPart = 62;
            read_packet.descriptor.function = oc::condrv::console_io_user_defined;
            read_packet.descriptor.process = info.process;
            read_packet.descriptor.object = info.output;
            read_packet.descriptor.input_size = read_read_offset;
            read_packet.descriptor.output_size = read_api_size + output_bytes;
            read_packet.payload.user_defined.msg_header.ApiNumber = static_cast<ULONG>(ConsolepReadConsoleOutputString);
            read_packet.payload.user_defined.msg_header.ApiDescriptorSize = read_api_size;

            auto& body = read_packet.payload.user_defined.u.console_msg_l2.ReadConsoleOutputString;
            body.ReadCoord = COORD{ 0, 1 };
            body.StringType = CONSOLE_REAL_UNICODE;
            body.NumRecords = 0;

            comm.input.assign(read_packet.descriptor.input_size, std::byte{});
            comm.output.clear();

            oc::condrv::BasicApiMessage<MemoryComm> read_message(comm, read_packet);
            auto read_outcome = oc::condrv::dispatch_message(state, read_message, host_io);
            if (!read_outcome || read_message.completion().io_status.Status != oc::core::status_success)
            {
                return false;
            }

            if (auto released = read_message.release_message_buffers(); !released)
            {
                return false;
            }

            if (comm.output.size() != read_api_size + output_bytes)
            {
                return false;
            }

            wchar_t chars[3]{};
            std::memcpy(chars, comm.output.data() + read_api_size, output_bytes);
            return chars[0] == L' ' && chars[1] == L' ' && chars[2] == L'c';
        }
    }

    bool test_write_console_vt_sgr_updates_attributes_and_strips_sequences()
    {
        MemoryComm comm{};
        oc::condrv::ServerState state{};
        TestHostIo host_io{};

        auto connect_packet = make_connect_packet(20007, 20008);
        oc::condrv::BasicApiMessage<MemoryComm> connect_message(comm, connect_packet);
        auto connect_outcome = oc::condrv::dispatch_message(state, connect_message, host_io);
        if (!connect_outcome)
        {
            return false;
        }

        state.set_output_mode(
            ENABLE_PROCESSED_OUTPUT |
            ENABLE_WRAP_AT_EOL_OUTPUT |
            ENABLE_VIRTUAL_TERMINAL_PROCESSING);

        auto info = unpack_connection_information(connect_message.completion());

        constexpr std::string_view text = "A\x1b[31mB\x1b[0mC";
        constexpr ULONG api_size = sizeof(CONSOLE_WRITECONSOLE_MSG);
        constexpr ULONG header_size = sizeof(CONSOLE_MSG_HEADER);
        const ULONG read_offset = api_size + header_size;

        oc::condrv::IoPacket packet{};
        packet.payload.user_defined = oc::condrv::UserDefinedPacket{};
        packet.descriptor.identifier.LowPart = 65;
        packet.descriptor.function = oc::condrv::console_io_user_defined;
        packet.descriptor.process = info.process;
        packet.descriptor.object = info.output;
        packet.descriptor.input_size = read_offset + static_cast<ULONG>(text.size());
        packet.descriptor.output_size = api_size;
        packet.payload.user_defined.msg_header.ApiNumber = static_cast<ULONG>(ConsolepWriteConsole);
        packet.payload.user_defined.msg_header.ApiDescriptorSize = api_size;
        packet.payload.user_defined.u.console_msg_l1.WriteConsole.Unicode = FALSE;

        comm.input.assign(packet.descriptor.input_size, std::byte{});
        std::memcpy(comm.input.data() + read_offset, text.data(), text.size());

        oc::condrv::BasicApiMessage<MemoryComm> message(comm, packet);
        auto outcome = oc::condrv::dispatch_message(state, message, host_io);
        if (!outcome || message.completion().io_status.Status != oc::core::status_success)
        {
            return false;
        }

        // Read back characters: VT sequences should not be printed into the screen buffer.
        {
            constexpr ULONG read_api_size = sizeof(CONSOLE_READCONSOLEOUTPUTSTRING_MSG);
            constexpr ULONG read_header_size = sizeof(CONSOLE_MSG_HEADER);
            constexpr ULONG read_read_offset = read_api_size + read_header_size;
            constexpr ULONG output_bytes = 3 * sizeof(wchar_t);

            oc::condrv::IoPacket read_packet{};
            read_packet.payload.user_defined = oc::condrv::UserDefinedPacket{};
            read_packet.descriptor.identifier.LowPart = 66;
            read_packet.descriptor.function = oc::condrv::console_io_user_defined;
            read_packet.descriptor.process = info.process;
            read_packet.descriptor.object = info.output;
            read_packet.descriptor.input_size = read_read_offset;
            read_packet.descriptor.output_size = read_api_size + output_bytes;
            read_packet.payload.user_defined.msg_header.ApiNumber = static_cast<ULONG>(ConsolepReadConsoleOutputString);
            read_packet.payload.user_defined.msg_header.ApiDescriptorSize = read_api_size;

            auto& body = read_packet.payload.user_defined.u.console_msg_l2.ReadConsoleOutputString;
            body.ReadCoord = COORD{ 0, 0 };
            body.StringType = CONSOLE_REAL_UNICODE;
            body.NumRecords = 0;

            comm.input.assign(read_packet.descriptor.input_size, std::byte{});
            comm.output.clear();

            oc::condrv::BasicApiMessage<MemoryComm> read_message(comm, read_packet);
            auto read_outcome = oc::condrv::dispatch_message(state, read_message, host_io);
            if (!read_outcome || read_message.completion().io_status.Status != oc::core::status_success)
            {
                return false;
            }

            if (auto released = read_message.release_message_buffers(); !released)
            {
                return false;
            }

            if (comm.output.size() != read_api_size + output_bytes)
            {
                return false;
            }

            wchar_t chars[3]{};
            std::memcpy(chars, comm.output.data() + read_api_size, output_bytes);
            if (chars[0] != L'A' || chars[1] != L'B' || chars[2] != L'C')
            {
                return false;
            }
        }

        // Read back attributes: SGR should apply to the buffer model.
        {
            constexpr ULONG read_api_size = sizeof(CONSOLE_READCONSOLEOUTPUTSTRING_MSG);
            constexpr ULONG read_header_size = sizeof(CONSOLE_MSG_HEADER);
            constexpr ULONG read_read_offset = read_api_size + read_header_size;
            constexpr ULONG output_bytes = 3 * sizeof(USHORT);

            oc::condrv::IoPacket read_packet{};
            read_packet.payload.user_defined = oc::condrv::UserDefinedPacket{};
            read_packet.descriptor.identifier.LowPart = 67;
            read_packet.descriptor.function = oc::condrv::console_io_user_defined;
            read_packet.descriptor.process = info.process;
            read_packet.descriptor.object = info.output;
            read_packet.descriptor.input_size = read_read_offset;
            read_packet.descriptor.output_size = read_api_size + output_bytes;
            read_packet.payload.user_defined.msg_header.ApiNumber = static_cast<ULONG>(ConsolepReadConsoleOutputString);
            read_packet.payload.user_defined.msg_header.ApiDescriptorSize = read_api_size;

            auto& body = read_packet.payload.user_defined.u.console_msg_l2.ReadConsoleOutputString;
            body.ReadCoord = COORD{ 0, 0 };
            body.StringType = CONSOLE_ATTRIBUTE;
            body.NumRecords = 0;

            comm.input.assign(read_packet.descriptor.input_size, std::byte{});
            comm.output.clear();

            oc::condrv::BasicApiMessage<MemoryComm> read_message(comm, read_packet);
            auto read_outcome = oc::condrv::dispatch_message(state, read_message, host_io);
            if (!read_outcome || read_message.completion().io_status.Status != oc::core::status_success)
            {
                return false;
            }

            if (auto released = read_message.release_message_buffers(); !released)
            {
                return false;
            }

            if (comm.output.size() != read_api_size + output_bytes)
            {
                return false;
            }

            USHORT attrs[3]{};
            std::memcpy(attrs, comm.output.data() + read_api_size, output_bytes);
            return attrs[0] == 0x07 && attrs[1] == FOREGROUND_RED && attrs[2] == 0x07;
        }
    }

    bool test_write_console_vt_sgr_normal_color_clears_bright_foreground_intensity()
    {
        MemoryComm comm{};
        oc::condrv::ServerState state{};
        TestHostIo host_io{};

        auto connect_packet = make_connect_packet(20031, 20032);
        oc::condrv::BasicApiMessage<MemoryComm> connect_message(comm, connect_packet);
        auto connect_outcome = oc::condrv::dispatch_message(state, connect_message, host_io);
        if (!connect_outcome)
        {
            return false;
        }

        state.set_output_mode(
            ENABLE_PROCESSED_OUTPUT |
            ENABLE_WRAP_AT_EOL_OUTPUT |
            ENABLE_VIRTUAL_TERMINAL_PROCESSING);

        auto info = unpack_connection_information(connect_message.completion());

        constexpr std::string_view text = "A\x1b[91mB\x1b[31mC";
        if (!write_console_user_defined_a(comm, state, host_io, info, text, 675))
        {
            return false;
        }

        // Read back attributes: switching from bright (91) to normal (31) must clear intensity.
        {
            constexpr ULONG read_api_size = sizeof(CONSOLE_READCONSOLEOUTPUTSTRING_MSG);
            constexpr ULONG read_header_size = sizeof(CONSOLE_MSG_HEADER);
            constexpr ULONG read_read_offset = read_api_size + read_header_size;
            constexpr ULONG output_bytes = 3 * sizeof(USHORT);

            oc::condrv::IoPacket read_packet{};
            read_packet.payload.user_defined = oc::condrv::UserDefinedPacket{};
            read_packet.descriptor.identifier.LowPart = 676;
            read_packet.descriptor.function = oc::condrv::console_io_user_defined;
            read_packet.descriptor.process = info.process;
            read_packet.descriptor.object = info.output;
            read_packet.descriptor.input_size = read_read_offset;
            read_packet.descriptor.output_size = read_api_size + output_bytes;
            read_packet.payload.user_defined.msg_header.ApiNumber = static_cast<ULONG>(ConsolepReadConsoleOutputString);
            read_packet.payload.user_defined.msg_header.ApiDescriptorSize = read_api_size;

            auto& body = read_packet.payload.user_defined.u.console_msg_l2.ReadConsoleOutputString;
            body.ReadCoord = COORD{ 0, 0 };
            body.StringType = CONSOLE_ATTRIBUTE;
            body.NumRecords = 0;

            comm.input.assign(read_packet.descriptor.input_size, std::byte{});
            comm.output.clear();

            oc::condrv::BasicApiMessage<MemoryComm> read_message(comm, read_packet);
            auto read_outcome = oc::condrv::dispatch_message(state, read_message, host_io);
            if (!read_outcome || read_message.completion().io_status.Status != oc::core::status_success)
            {
                return false;
            }

            if (auto released = read_message.release_message_buffers(); !released)
            {
                return false;
            }

            if (comm.output.size() != read_api_size + output_bytes)
            {
                return false;
            }

            USHORT attrs[3]{};
            std::memcpy(attrs, comm.output.data() + read_api_size, output_bytes);
            return attrs[0] == 0x07 &&
                   attrs[1] == static_cast<USHORT>(FOREGROUND_RED | FOREGROUND_INTENSITY) &&
                   attrs[2] == FOREGROUND_RED;
        }
    }

    bool test_write_console_vt_sgr_normal_color_clears_bright_background_intensity()
    {
        MemoryComm comm{};
        oc::condrv::ServerState state{};
        TestHostIo host_io{};

        auto connect_packet = make_connect_packet(20033, 20034);
        oc::condrv::BasicApiMessage<MemoryComm> connect_message(comm, connect_packet);
        auto connect_outcome = oc::condrv::dispatch_message(state, connect_message, host_io);
        if (!connect_outcome)
        {
            return false;
        }

        state.set_output_mode(
            ENABLE_PROCESSED_OUTPUT |
            ENABLE_WRAP_AT_EOL_OUTPUT |
            ENABLE_VIRTUAL_TERMINAL_PROCESSING);

        auto info = unpack_connection_information(connect_message.completion());

        constexpr std::string_view text = "A\x1b[101mB\x1b[41mC";
        if (!write_console_user_defined_a(comm, state, host_io, info, text, 677))
        {
            return false;
        }

        // Read back attributes: switching from bright (101) to normal (41) must clear background intensity.
        {
            constexpr ULONG read_api_size = sizeof(CONSOLE_READCONSOLEOUTPUTSTRING_MSG);
            constexpr ULONG read_header_size = sizeof(CONSOLE_MSG_HEADER);
            constexpr ULONG read_read_offset = read_api_size + read_header_size;
            constexpr ULONG output_bytes = 3 * sizeof(USHORT);

            oc::condrv::IoPacket read_packet{};
            read_packet.payload.user_defined = oc::condrv::UserDefinedPacket{};
            read_packet.descriptor.identifier.LowPart = 678;
            read_packet.descriptor.function = oc::condrv::console_io_user_defined;
            read_packet.descriptor.process = info.process;
            read_packet.descriptor.object = info.output;
            read_packet.descriptor.input_size = read_read_offset;
            read_packet.descriptor.output_size = read_api_size + output_bytes;
            read_packet.payload.user_defined.msg_header.ApiNumber = static_cast<ULONG>(ConsolepReadConsoleOutputString);
            read_packet.payload.user_defined.msg_header.ApiDescriptorSize = read_api_size;

            auto& body = read_packet.payload.user_defined.u.console_msg_l2.ReadConsoleOutputString;
            body.ReadCoord = COORD{ 0, 0 };
            body.StringType = CONSOLE_ATTRIBUTE;
            body.NumRecords = 0;

            comm.input.assign(read_packet.descriptor.input_size, std::byte{});
            comm.output.clear();

            oc::condrv::BasicApiMessage<MemoryComm> read_message(comm, read_packet);
            auto read_outcome = oc::condrv::dispatch_message(state, read_message, host_io);
            if (!read_outcome || read_message.completion().io_status.Status != oc::core::status_success)
            {
                return false;
            }

            if (auto released = read_message.release_message_buffers(); !released)
            {
                return false;
            }

            if (comm.output.size() != read_api_size + output_bytes)
            {
                return false;
            }

            USHORT attrs[3]{};
            std::memcpy(attrs, comm.output.data() + read_api_size, output_bytes);
            return attrs[0] == 0x07 &&
                   attrs[1] == static_cast<USHORT>(0x07 | BACKGROUND_RED | BACKGROUND_INTENSITY) &&
                   attrs[2] == static_cast<USHORT>(0x07 | BACKGROUND_RED);
        }
    }

    bool test_write_console_vt_sgr_extended_palette_index_sets_bright_red_foreground()
    {
        MemoryComm comm{};
        oc::condrv::ServerState state{};
        TestHostIo host_io{};

        auto connect_packet = make_connect_packet(20035, 20036);
        oc::condrv::BasicApiMessage<MemoryComm> connect_message(comm, connect_packet);
        auto connect_outcome = oc::condrv::dispatch_message(state, connect_message, host_io);
        if (!connect_outcome)
        {
            return false;
        }

        state.set_output_mode(
            ENABLE_PROCESSED_OUTPUT |
            ENABLE_WRAP_AT_EOL_OUTPUT |
            ENABLE_VIRTUAL_TERMINAL_PROCESSING);

        auto info = unpack_connection_information(connect_message.completion());

        constexpr std::string_view text = "A\x1b[38;5;9mB\x1b[0mC";
        if (!write_console_user_defined_a(comm, state, host_io, info, text, 679))
        {
            return false;
        }

        // 38;5;9 is "bright red" in the xterm base palette.
        {
            constexpr ULONG read_api_size = sizeof(CONSOLE_READCONSOLEOUTPUTSTRING_MSG);
            constexpr ULONG read_header_size = sizeof(CONSOLE_MSG_HEADER);
            constexpr ULONG read_read_offset = read_api_size + read_header_size;
            constexpr ULONG output_bytes = 3 * sizeof(USHORT);

            oc::condrv::IoPacket read_packet{};
            read_packet.payload.user_defined = oc::condrv::UserDefinedPacket{};
            read_packet.descriptor.identifier.LowPart = 680;
            read_packet.descriptor.function = oc::condrv::console_io_user_defined;
            read_packet.descriptor.process = info.process;
            read_packet.descriptor.object = info.output;
            read_packet.descriptor.input_size = read_read_offset;
            read_packet.descriptor.output_size = read_api_size + output_bytes;
            read_packet.payload.user_defined.msg_header.ApiNumber = static_cast<ULONG>(ConsolepReadConsoleOutputString);
            read_packet.payload.user_defined.msg_header.ApiDescriptorSize = read_api_size;

            auto& body = read_packet.payload.user_defined.u.console_msg_l2.ReadConsoleOutputString;
            body.ReadCoord = COORD{ 0, 0 };
            body.StringType = CONSOLE_ATTRIBUTE;
            body.NumRecords = 0;

            comm.input.assign(read_packet.descriptor.input_size, std::byte{});
            comm.output.clear();

            oc::condrv::BasicApiMessage<MemoryComm> read_message(comm, read_packet);
            auto read_outcome = oc::condrv::dispatch_message(state, read_message, host_io);
            if (!read_outcome || read_message.completion().io_status.Status != oc::core::status_success)
            {
                return false;
            }

            if (auto released = read_message.release_message_buffers(); !released)
            {
                return false;
            }

            if (comm.output.size() != read_api_size + output_bytes)
            {
                return false;
            }

            USHORT attrs[3]{};
            std::memcpy(attrs, comm.output.data() + read_api_size, output_bytes);
            return attrs[0] == 0x07 &&
                   attrs[1] == static_cast<USHORT>(FOREGROUND_RED | FOREGROUND_INTENSITY) &&
                   attrs[2] == 0x07;
        }
    }

    bool test_write_console_vt_sgr_extended_truecolor_sets_bright_red_foreground()
    {
        MemoryComm comm{};
        oc::condrv::ServerState state{};
        TestHostIo host_io{};

        auto connect_packet = make_connect_packet(20037, 20038);
        oc::condrv::BasicApiMessage<MemoryComm> connect_message(comm, connect_packet);
        auto connect_outcome = oc::condrv::dispatch_message(state, connect_message, host_io);
        if (!connect_outcome)
        {
            return false;
        }

        state.set_output_mode(
            ENABLE_PROCESSED_OUTPUT |
            ENABLE_WRAP_AT_EOL_OUTPUT |
            ENABLE_VIRTUAL_TERMINAL_PROCESSING);

        auto info = unpack_connection_information(connect_message.completion());

        constexpr std::string_view text = "A\x1b[38;2;255;0;0mB\x1b[0mC";
        if (!write_console_user_defined_a(comm, state, host_io, info, text, 681))
        {
            return false;
        }

        // Truecolor 255,0,0 maps to the nearest palette entry (bright red in the default table).
        {
            constexpr ULONG read_api_size = sizeof(CONSOLE_READCONSOLEOUTPUTSTRING_MSG);
            constexpr ULONG read_header_size = sizeof(CONSOLE_MSG_HEADER);
            constexpr ULONG read_read_offset = read_api_size + read_header_size;
            constexpr ULONG output_bytes = 3 * sizeof(USHORT);

            oc::condrv::IoPacket read_packet{};
            read_packet.payload.user_defined = oc::condrv::UserDefinedPacket{};
            read_packet.descriptor.identifier.LowPart = 682;
            read_packet.descriptor.function = oc::condrv::console_io_user_defined;
            read_packet.descriptor.process = info.process;
            read_packet.descriptor.object = info.output;
            read_packet.descriptor.input_size = read_read_offset;
            read_packet.descriptor.output_size = read_api_size + output_bytes;
            read_packet.payload.user_defined.msg_header.ApiNumber = static_cast<ULONG>(ConsolepReadConsoleOutputString);
            read_packet.payload.user_defined.msg_header.ApiDescriptorSize = read_api_size;

            auto& body = read_packet.payload.user_defined.u.console_msg_l2.ReadConsoleOutputString;
            body.ReadCoord = COORD{ 0, 0 };
            body.StringType = CONSOLE_ATTRIBUTE;
            body.NumRecords = 0;

            comm.input.assign(read_packet.descriptor.input_size, std::byte{});
            comm.output.clear();

            oc::condrv::BasicApiMessage<MemoryComm> read_message(comm, read_packet);
            auto read_outcome = oc::condrv::dispatch_message(state, read_message, host_io);
            if (!read_outcome || read_message.completion().io_status.Status != oc::core::status_success)
            {
                return false;
            }

            if (auto released = read_message.release_message_buffers(); !released)
            {
                return false;
            }

            if (comm.output.size() != read_api_size + output_bytes)
            {
                return false;
            }

            USHORT attrs[3]{};
            std::memcpy(attrs, comm.output.data() + read_api_size, output_bytes);
            return attrs[0] == 0x07 &&
                   attrs[1] == static_cast<USHORT>(FOREGROUND_RED | FOREGROUND_INTENSITY) &&
                   attrs[2] == 0x07;
        }
    }

    bool test_write_console_vt_sgr_extended_palette_index_sets_blue_background()
    {
        MemoryComm comm{};
        oc::condrv::ServerState state{};
        TestHostIo host_io{};

        auto connect_packet = make_connect_packet(20039, 20040);
        oc::condrv::BasicApiMessage<MemoryComm> connect_message(comm, connect_packet);
        auto connect_outcome = oc::condrv::dispatch_message(state, connect_message, host_io);
        if (!connect_outcome)
        {
            return false;
        }

        state.set_output_mode(
            ENABLE_PROCESSED_OUTPUT |
            ENABLE_WRAP_AT_EOL_OUTPUT |
            ENABLE_VIRTUAL_TERMINAL_PROCESSING);

        auto info = unpack_connection_information(connect_message.completion());

        constexpr std::string_view text = "A\x1b[48;5;4mB\x1b[0mC";
        if (!write_console_user_defined_a(comm, state, host_io, info, text, 683))
        {
            return false;
        }

        // 48;5;4 is "blue" in the xterm base palette.
        {
            constexpr ULONG read_api_size = sizeof(CONSOLE_READCONSOLEOUTPUTSTRING_MSG);
            constexpr ULONG read_header_size = sizeof(CONSOLE_MSG_HEADER);
            constexpr ULONG read_read_offset = read_api_size + read_header_size;
            constexpr ULONG output_bytes = 3 * sizeof(USHORT);

            oc::condrv::IoPacket read_packet{};
            read_packet.payload.user_defined = oc::condrv::UserDefinedPacket{};
            read_packet.descriptor.identifier.LowPart = 684;
            read_packet.descriptor.function = oc::condrv::console_io_user_defined;
            read_packet.descriptor.process = info.process;
            read_packet.descriptor.object = info.output;
            read_packet.descriptor.input_size = read_read_offset;
            read_packet.descriptor.output_size = read_api_size + output_bytes;
            read_packet.payload.user_defined.msg_header.ApiNumber = static_cast<ULONG>(ConsolepReadConsoleOutputString);
            read_packet.payload.user_defined.msg_header.ApiDescriptorSize = read_api_size;

            auto& body = read_packet.payload.user_defined.u.console_msg_l2.ReadConsoleOutputString;
            body.ReadCoord = COORD{ 0, 0 };
            body.StringType = CONSOLE_ATTRIBUTE;
            body.NumRecords = 0;

            comm.input.assign(read_packet.descriptor.input_size, std::byte{});
            comm.output.clear();

            oc::condrv::BasicApiMessage<MemoryComm> read_message(comm, read_packet);
            auto read_outcome = oc::condrv::dispatch_message(state, read_message, host_io);
            if (!read_outcome || read_message.completion().io_status.Status != oc::core::status_success)
            {
                return false;
            }

            if (auto released = read_message.release_message_buffers(); !released)
            {
                return false;
            }

            if (comm.output.size() != read_api_size + output_bytes)
            {
                return false;
            }

            USHORT attrs[3]{};
            std::memcpy(attrs, comm.output.data() + read_api_size, output_bytes);
            return attrs[0] == 0x07 &&
                   attrs[1] == static_cast<USHORT>(0x07 | BACKGROUND_BLUE) &&
                   attrs[2] == 0x07;
        }
    }

    bool test_write_console_vt_sgr_reverse_video_sets_common_lvb_reverse_video()
    {
        MemoryComm comm{};
        oc::condrv::ServerState state{};
        TestHostIo host_io{};

        auto connect_packet = make_connect_packet(20007, 20008);
        oc::condrv::BasicApiMessage<MemoryComm> connect_message(comm, connect_packet);
        auto connect_outcome = oc::condrv::dispatch_message(state, connect_message, host_io);
        if (!connect_outcome)
        {
            return false;
        }

        state.set_output_mode(
            ENABLE_PROCESSED_OUTPUT |
            ENABLE_WRAP_AT_EOL_OUTPUT |
            ENABLE_VIRTUAL_TERMINAL_PROCESSING);

        auto info = unpack_connection_information(connect_message.completion());

        // 7: negative (reverse video), 27: positive (clear reverse).
        constexpr std::string_view text = "A\x1b[7mB\x1b[27mC";
        constexpr ULONG api_size = sizeof(CONSOLE_WRITECONSOLE_MSG);
        constexpr ULONG header_size = sizeof(CONSOLE_MSG_HEADER);
        const ULONG read_offset = api_size + header_size;

        oc::condrv::IoPacket packet{};
        packet.payload.user_defined = oc::condrv::UserDefinedPacket{};
        packet.descriptor.identifier.LowPart = 671;
        packet.descriptor.function = oc::condrv::console_io_user_defined;
        packet.descriptor.process = info.process;
        packet.descriptor.object = info.output;
        packet.descriptor.input_size = read_offset + static_cast<ULONG>(text.size());
        packet.descriptor.output_size = api_size;
        packet.payload.user_defined.msg_header.ApiNumber = static_cast<ULONG>(ConsolepWriteConsole);
        packet.payload.user_defined.msg_header.ApiDescriptorSize = api_size;
        packet.payload.user_defined.u.console_msg_l1.WriteConsole.Unicode = FALSE;

        comm.input.assign(packet.descriptor.input_size, std::byte{});
        std::memcpy(comm.input.data() + read_offset, text.data(), text.size());

        oc::condrv::BasicApiMessage<MemoryComm> message(comm, packet);
        auto outcome = oc::condrv::dispatch_message(state, message, host_io);
        if (!outcome || message.completion().io_status.Status != oc::core::status_success)
        {
            return false;
        }

        // Read back attributes: reverse video is represented by COMMON_LVB_REVERSE_VIDEO.
        {
            constexpr ULONG read_api_size = sizeof(CONSOLE_READCONSOLEOUTPUTSTRING_MSG);
            constexpr ULONG read_header_size = sizeof(CONSOLE_MSG_HEADER);
            constexpr ULONG read_read_offset = read_api_size + read_header_size;
            constexpr ULONG output_bytes = 3 * sizeof(USHORT);

            oc::condrv::IoPacket read_packet{};
            read_packet.payload.user_defined = oc::condrv::UserDefinedPacket{};
            read_packet.descriptor.identifier.LowPart = 672;
            read_packet.descriptor.function = oc::condrv::console_io_user_defined;
            read_packet.descriptor.process = info.process;
            read_packet.descriptor.object = info.output;
            read_packet.descriptor.input_size = read_read_offset;
            read_packet.descriptor.output_size = read_api_size + output_bytes;
            read_packet.payload.user_defined.msg_header.ApiNumber = static_cast<ULONG>(ConsolepReadConsoleOutputString);
            read_packet.payload.user_defined.msg_header.ApiDescriptorSize = read_api_size;

            auto& body = read_packet.payload.user_defined.u.console_msg_l2.ReadConsoleOutputString;
            body.ReadCoord = COORD{ 0, 0 };
            body.StringType = CONSOLE_ATTRIBUTE;
            body.NumRecords = 0;

            comm.input.assign(read_packet.descriptor.input_size, std::byte{});
            comm.output.clear();

            oc::condrv::BasicApiMessage<MemoryComm> read_message(comm, read_packet);
            auto read_outcome = oc::condrv::dispatch_message(state, read_message, host_io);
            if (!read_outcome || read_message.completion().io_status.Status != oc::core::status_success)
            {
                return false;
            }

            if (auto released = read_message.release_message_buffers(); !released)
            {
                return false;
            }

            if (comm.output.size() != read_api_size + output_bytes)
            {
                return false;
            }

            USHORT attrs[3]{};
            std::memcpy(attrs, comm.output.data() + read_api_size, output_bytes);
            return attrs[0] == 0x07 &&
                   attrs[1] == static_cast<USHORT>(0x07 | COMMON_LVB_REVERSE_VIDEO) &&
                   attrs[2] == 0x07;
        }
    }

    bool test_write_console_vt_sgr_underline_sets_common_lvb_underscore()
    {
        MemoryComm comm{};
        oc::condrv::ServerState state{};
        TestHostIo host_io{};

        auto connect_packet = make_connect_packet(20015, 20016);
        oc::condrv::BasicApiMessage<MemoryComm> connect_message(comm, connect_packet);
        auto connect_outcome = oc::condrv::dispatch_message(state, connect_message, host_io);
        if (!connect_outcome)
        {
            return false;
        }

        state.set_output_mode(
            ENABLE_PROCESSED_OUTPUT |
            ENABLE_WRAP_AT_EOL_OUTPUT |
            ENABLE_VIRTUAL_TERMINAL_PROCESSING);

        auto info = unpack_connection_information(connect_message.completion());

        // 4: underline, 24: clear underline.
        constexpr std::string_view text = "A\x1b[4mB\x1b[24mC";
        constexpr ULONG api_size = sizeof(CONSOLE_WRITECONSOLE_MSG);
        constexpr ULONG header_size = sizeof(CONSOLE_MSG_HEADER);
        const ULONG read_offset = api_size + header_size;

        oc::condrv::IoPacket packet{};
        packet.payload.user_defined = oc::condrv::UserDefinedPacket{};
        packet.descriptor.identifier.LowPart = 673;
        packet.descriptor.function = oc::condrv::console_io_user_defined;
        packet.descriptor.process = info.process;
        packet.descriptor.object = info.output;
        packet.descriptor.input_size = read_offset + static_cast<ULONG>(text.size());
        packet.descriptor.output_size = api_size;
        packet.payload.user_defined.msg_header.ApiNumber = static_cast<ULONG>(ConsolepWriteConsole);
        packet.payload.user_defined.msg_header.ApiDescriptorSize = api_size;
        packet.payload.user_defined.u.console_msg_l1.WriteConsole.Unicode = FALSE;

        comm.input.assign(packet.descriptor.input_size, std::byte{});
        std::memcpy(comm.input.data() + read_offset, text.data(), text.size());

        oc::condrv::BasicApiMessage<MemoryComm> message(comm, packet);
        auto outcome = oc::condrv::dispatch_message(state, message, host_io);
        if (!outcome || message.completion().io_status.Status != oc::core::status_success)
        {
            return false;
        }

        // Read back attributes: underline is represented by COMMON_LVB_UNDERSCORE.
        {
            constexpr ULONG read_api_size = sizeof(CONSOLE_READCONSOLEOUTPUTSTRING_MSG);
            constexpr ULONG read_header_size = sizeof(CONSOLE_MSG_HEADER);
            constexpr ULONG read_read_offset = read_api_size + read_header_size;
            constexpr ULONG output_bytes = 3 * sizeof(USHORT);

            oc::condrv::IoPacket read_packet{};
            read_packet.payload.user_defined = oc::condrv::UserDefinedPacket{};
            read_packet.descriptor.identifier.LowPart = 674;
            read_packet.descriptor.function = oc::condrv::console_io_user_defined;
            read_packet.descriptor.process = info.process;
            read_packet.descriptor.object = info.output;
            read_packet.descriptor.input_size = read_read_offset;
            read_packet.descriptor.output_size = read_api_size + output_bytes;
            read_packet.payload.user_defined.msg_header.ApiNumber = static_cast<ULONG>(ConsolepReadConsoleOutputString);
            read_packet.payload.user_defined.msg_header.ApiDescriptorSize = read_api_size;

            auto& body = read_packet.payload.user_defined.u.console_msg_l2.ReadConsoleOutputString;
            body.ReadCoord = COORD{ 0, 0 };
            body.StringType = CONSOLE_ATTRIBUTE;
            body.NumRecords = 0;

            comm.input.assign(read_packet.descriptor.input_size, std::byte{});
            comm.output.clear();

            oc::condrv::BasicApiMessage<MemoryComm> read_message(comm, read_packet);
            auto read_outcome = oc::condrv::dispatch_message(state, read_message, host_io);
            if (!read_outcome || read_message.completion().io_status.Status != oc::core::status_success)
            {
                return false;
            }

            if (auto released = read_message.release_message_buffers(); !released)
            {
                return false;
            }

            if (comm.output.size() != read_api_size + output_bytes)
            {
                return false;
            }

            USHORT attrs[3]{};
            std::memcpy(attrs, comm.output.data() + read_api_size, output_bytes);
            return attrs[0] == 0x07 &&
                   attrs[1] == static_cast<USHORT>(0x07 | COMMON_LVB_UNDERSCORE) &&
                   attrs[2] == 0x07;
        }
    }

    bool test_write_console_vt_cup_moves_cursor()
    {
        MemoryComm comm{};
        oc::condrv::ServerState state{};
        TestHostIo host_io{};

        auto connect_packet = make_connect_packet(20009, 20010);
        oc::condrv::BasicApiMessage<MemoryComm> connect_message(comm, connect_packet);
        auto connect_outcome = oc::condrv::dispatch_message(state, connect_message, host_io);
        if (!connect_outcome)
        {
            return false;
        }

        state.set_output_mode(
            ENABLE_PROCESSED_OUTPUT |
            ENABLE_WRAP_AT_EOL_OUTPUT |
            ENABLE_VIRTUAL_TERMINAL_PROCESSING);

        auto info = unpack_connection_information(connect_message.completion());

        constexpr std::string_view text = "A\x1b[2;3HZ";
        constexpr ULONG api_size = sizeof(CONSOLE_WRITECONSOLE_MSG);
        constexpr ULONG header_size = sizeof(CONSOLE_MSG_HEADER);
        const ULONG read_offset = api_size + header_size;

        oc::condrv::IoPacket packet{};
        packet.payload.user_defined = oc::condrv::UserDefinedPacket{};
        packet.descriptor.identifier.LowPart = 68;
        packet.descriptor.function = oc::condrv::console_io_user_defined;
        packet.descriptor.process = info.process;
        packet.descriptor.object = info.output;
        packet.descriptor.input_size = read_offset + static_cast<ULONG>(text.size());
        packet.descriptor.output_size = api_size;
        packet.payload.user_defined.msg_header.ApiNumber = static_cast<ULONG>(ConsolepWriteConsole);
        packet.payload.user_defined.msg_header.ApiDescriptorSize = api_size;
        packet.payload.user_defined.u.console_msg_l1.WriteConsole.Unicode = FALSE;

        comm.input.assign(packet.descriptor.input_size, std::byte{});
        std::memcpy(comm.input.data() + read_offset, text.data(), text.size());

        oc::condrv::BasicApiMessage<MemoryComm> message(comm, packet);
        auto outcome = oc::condrv::dispatch_message(state, message, host_io);
        if (!outcome || message.completion().io_status.Status != oc::core::status_success)
        {
            return false;
        }

        const auto read_one = [&](const COORD coord) noexcept -> std::optional<wchar_t> {
            constexpr ULONG read_api_size = sizeof(CONSOLE_READCONSOLEOUTPUTSTRING_MSG);
            constexpr ULONG read_header_size = sizeof(CONSOLE_MSG_HEADER);
            constexpr ULONG read_read_offset = read_api_size + read_header_size;
            constexpr ULONG output_bytes = sizeof(wchar_t);

            oc::condrv::IoPacket read_packet{};
            read_packet.payload.user_defined = oc::condrv::UserDefinedPacket{};
            read_packet.descriptor.identifier.LowPart = 69;
            read_packet.descriptor.function = oc::condrv::console_io_user_defined;
            read_packet.descriptor.process = info.process;
            read_packet.descriptor.object = info.output;
            read_packet.descriptor.input_size = read_read_offset;
            read_packet.descriptor.output_size = read_api_size + output_bytes;
            read_packet.payload.user_defined.msg_header.ApiNumber = static_cast<ULONG>(ConsolepReadConsoleOutputString);
            read_packet.payload.user_defined.msg_header.ApiDescriptorSize = read_api_size;

            auto& body = read_packet.payload.user_defined.u.console_msg_l2.ReadConsoleOutputString;
            body.ReadCoord = coord;
            body.StringType = CONSOLE_REAL_UNICODE;
            body.NumRecords = 0;

            comm.input.assign(read_packet.descriptor.input_size, std::byte{});
            comm.output.clear();

            oc::condrv::BasicApiMessage<MemoryComm> read_message(comm, read_packet);
            auto read_outcome = oc::condrv::dispatch_message(state, read_message, host_io);
            if (!read_outcome || read_message.completion().io_status.Status != oc::core::status_success)
            {
                return std::nullopt;
            }

            if (auto released = read_message.release_message_buffers(); !released)
            {
                return std::nullopt;
            }

            if (comm.output.size() != read_api_size + output_bytes)
            {
                return std::nullopt;
            }

            wchar_t value{};
            std::memcpy(&value, comm.output.data() + read_api_size, sizeof(value));
            return value;
        };

        const auto a = read_one(COORD{ 0, 0 });
        if (!a || a.value() != L'A')
        {
            return false;
        }

         // CUP is 1-based: ESC[2;3H -> row 2, col 3 -> coord (2, 1).
         const auto z = read_one(COORD{ 2, 1 });
         return z && z.value() == L'Z';
     }
 
     bool test_write_console_vt_c1_csi_cup_moves_cursor()
     {
         MemoryComm comm{};
         oc::condrv::ServerState state{};
         TestHostIo host_io{};
 
         auto connect_packet = make_connect_packet(20009, 20010);
         oc::condrv::BasicApiMessage<MemoryComm> connect_message(comm, connect_packet);
         auto connect_outcome = oc::condrv::dispatch_message(state, connect_message, host_io);
         if (!connect_outcome)
         {
             return false;
         }
 
         state.set_output_mode(
             ENABLE_PROCESSED_OUTPUT |
             ENABLE_WRAP_AT_EOL_OUTPUT |
             ENABLE_VIRTUAL_TERMINAL_PROCESSING);
 
         auto info = unpack_connection_information(connect_message.completion());
 
         // C1 CSI form: U+009B.
         constexpr wchar_t text[]{ L'A', static_cast<wchar_t>(0x009b), L'2', L';', L'3', L'H', L'Z' };
         if (!write_console_user_defined_w(comm, state, host_io, info, std::wstring_view(text, std::size(text)), 69))
         {
             return false;
         }
 
         const auto a = read_console_output_char(comm, state, host_io, info, COORD{ 0, 0 }, 691);
         if (!a || a.value() != L'A')
         {
             return false;
         }
 
         // CUP is 1-based: CSI 2;3H -> row 2, col 3 -> coord (2, 1).
         const auto z = read_console_output_char(comm, state, host_io, info, COORD{ 2, 1 }, 692);
         return z && z.value() == L'Z';
     }
 
     bool test_write_console_vt_ed_clears_screen()
     {
         MemoryComm comm{};
         oc::condrv::ServerState state{};
        TestHostIo host_io{};

        auto connect_packet = make_connect_packet(20011, 20012);
        oc::condrv::BasicApiMessage<MemoryComm> connect_message(comm, connect_packet);
        auto connect_outcome = oc::condrv::dispatch_message(state, connect_message, host_io);
        if (!connect_outcome)
        {
            return false;
        }

        state.set_output_mode(
            ENABLE_PROCESSED_OUTPUT |
            ENABLE_WRAP_AT_EOL_OUTPUT |
            ENABLE_VIRTUAL_TERMINAL_PROCESSING);

        auto info = unpack_connection_information(connect_message.completion());

        // ED(2) clears the screen but does not move the cursor. We therefore expect the
        // post-clear 'Z' to appear at the cursor position after 'A' was written.
        constexpr std::string_view text = "A\x1b[2JZ";
        constexpr ULONG api_size = sizeof(CONSOLE_WRITECONSOLE_MSG);
        constexpr ULONG header_size = sizeof(CONSOLE_MSG_HEADER);
        const ULONG read_offset = api_size + header_size;

        oc::condrv::IoPacket packet{};
        packet.payload.user_defined = oc::condrv::UserDefinedPacket{};
        packet.descriptor.identifier.LowPart = 70;
        packet.descriptor.function = oc::condrv::console_io_user_defined;
        packet.descriptor.process = info.process;
        packet.descriptor.object = info.output;
        packet.descriptor.input_size = read_offset + static_cast<ULONG>(text.size());
        packet.descriptor.output_size = api_size;
        packet.payload.user_defined.msg_header.ApiNumber = static_cast<ULONG>(ConsolepWriteConsole);
        packet.payload.user_defined.msg_header.ApiDescriptorSize = api_size;
        packet.payload.user_defined.u.console_msg_l1.WriteConsole.Unicode = FALSE;

        comm.input.assign(packet.descriptor.input_size, std::byte{});
        std::memcpy(comm.input.data() + read_offset, text.data(), text.size());

        oc::condrv::BasicApiMessage<MemoryComm> message(comm, packet);
        auto outcome = oc::condrv::dispatch_message(state, message, host_io);
        if (!outcome || message.completion().io_status.Status != oc::core::status_success)
        {
            return false;
        }

        // Read back the first two cells on the first row: [space, 'Z'].
        {
            constexpr ULONG read_api_size = sizeof(CONSOLE_READCONSOLEOUTPUTSTRING_MSG);
            constexpr ULONG read_header_size = sizeof(CONSOLE_MSG_HEADER);
            constexpr ULONG read_read_offset = read_api_size + read_header_size;
            constexpr ULONG output_bytes = 2 * sizeof(wchar_t);

            oc::condrv::IoPacket read_packet{};
            read_packet.payload.user_defined = oc::condrv::UserDefinedPacket{};
            read_packet.descriptor.identifier.LowPart = 71;
            read_packet.descriptor.function = oc::condrv::console_io_user_defined;
            read_packet.descriptor.process = info.process;
            read_packet.descriptor.object = info.output;
            read_packet.descriptor.input_size = read_read_offset;
            read_packet.descriptor.output_size = read_api_size + output_bytes;
            read_packet.payload.user_defined.msg_header.ApiNumber = static_cast<ULONG>(ConsolepReadConsoleOutputString);
            read_packet.payload.user_defined.msg_header.ApiDescriptorSize = read_api_size;

            auto& body = read_packet.payload.user_defined.u.console_msg_l2.ReadConsoleOutputString;
            body.ReadCoord = COORD{ 0, 0 };
            body.StringType = CONSOLE_REAL_UNICODE;
            body.NumRecords = 0;

            comm.input.assign(read_packet.descriptor.input_size, std::byte{});
            comm.output.clear();

            oc::condrv::BasicApiMessage<MemoryComm> read_message(comm, read_packet);
            auto read_outcome = oc::condrv::dispatch_message(state, read_message, host_io);
            if (!read_outcome || read_message.completion().io_status.Status != oc::core::status_success)
            {
                return false;
            }

            if (auto released = read_message.release_message_buffers(); !released)
            {
                return false;
            }

            if (comm.output.size() != read_api_size + output_bytes)
            {
                return false;
            }

             wchar_t chars[2]{};
             std::memcpy(chars, comm.output.data() + read_api_size, output_bytes);
             return chars[0] == L' ' && chars[1] == L'Z';
         }
     }
 
    bool test_write_console_vt_c1_csi_ed_clears_screen()
    {
        MemoryComm comm{};
        oc::condrv::ServerState state{};
        TestHostIo host_io{};
 
         auto connect_packet = make_connect_packet(20011, 20012);
         oc::condrv::BasicApiMessage<MemoryComm> connect_message(comm, connect_packet);
         auto connect_outcome = oc::condrv::dispatch_message(state, connect_message, host_io);
         if (!connect_outcome)
         {
             return false;
         }
 
         state.set_output_mode(
             ENABLE_PROCESSED_OUTPUT |
             ENABLE_WRAP_AT_EOL_OUTPUT |
             ENABLE_VIRTUAL_TERMINAL_PROCESSING);
 
         auto info = unpack_connection_information(connect_message.completion());
 
         // C1 CSI form: U+009B.
         // ED(2) clears the screen but does not move the cursor. We therefore expect the
         // post-clear 'Z' to appear at the cursor position after 'A' was written.
         constexpr wchar_t text[]{ L'A', static_cast<wchar_t>(0x009b), L'2', L'J', L'Z' };
         if (!write_console_user_defined_w(comm, state, host_io, info, std::wstring_view(text, std::size(text)), 71))
         {
             return false;
         }
 
         const auto c0 = read_console_output_char(comm, state, host_io, info, COORD{ 0, 0 }, 711);
         const auto c1 = read_console_output_char(comm, state, host_io, info, COORD{ 1, 0 }, 712);
         return c0 && c0.value() == L' ' &&
                c1 && c1.value() == L'Z';
     }

     bool test_write_console_vt_nel_moves_to_next_line_and_consumes_sequence()
     {
         MemoryComm comm{};
         oc::condrv::ServerState state{};
         TestHostIo host_io{};
 
         auto connect_packet = make_connect_packet(20017, 20018);
         oc::condrv::BasicApiMessage<MemoryComm> connect_message(comm, connect_packet);
         auto connect_outcome = oc::condrv::dispatch_message(state, connect_message, host_io);
         if (!connect_outcome)
         {
             return false;
         }
 
         state.set_output_mode(
             ENABLE_PROCESSED_OUTPUT |
             ENABLE_WRAP_AT_EOL_OUTPUT |
             ENABLE_VIRTUAL_TERMINAL_PROCESSING);
 
         auto info = unpack_connection_information(connect_message.completion());
 
         if (!write_console_user_defined_a(comm, state, host_io, info, "A\x1b" "EB", 720))
         {
             return false;
         }
 
         const auto a = read_console_output_char(comm, state, host_io, info, COORD{ 0, 0 }, 721);
         const auto gap = read_console_output_char(comm, state, host_io, info, COORD{ 1, 0 }, 722);
         const auto b = read_console_output_char(comm, state, host_io, info, COORD{ 0, 1 }, 723);
         return a && gap && b &&
                a.value() == L'A' &&
                gap.value() == L' ' &&
                b.value() == L'B';
     }
 
     bool test_write_console_vt_charset_designation_is_consumed()
     {
         MemoryComm comm{};
         oc::condrv::ServerState state{};
         TestHostIo host_io{};
 
         auto connect_packet = make_connect_packet(20021, 20022);
         oc::condrv::BasicApiMessage<MemoryComm> connect_message(comm, connect_packet);
         auto connect_outcome = oc::condrv::dispatch_message(state, connect_message, host_io);
         if (!connect_outcome)
         {
             return false;
         }
 
         state.set_output_mode(
             ENABLE_PROCESSED_OUTPUT |
             ENABLE_WRAP_AT_EOL_OUTPUT |
             ENABLE_VIRTUAL_TERMINAL_PROCESSING);
 
         auto info = unpack_connection_information(connect_message.completion());
 
         // Common line-drawing enable/disable sequences: ESC ( 0 and ESC ( B.
         if (!write_console_user_defined_a(comm, state, host_io, info, "A\x1b(0B\x1b(B C", 724))
         {
             return false;
         }
 
         const auto a = read_console_output_char(comm, state, host_io, info, COORD{ 0, 0 }, 725);
         const auto b = read_console_output_char(comm, state, host_io, info, COORD{ 1, 0 }, 726);
         const auto space = read_console_output_char(comm, state, host_io, info, COORD{ 2, 0 }, 727);
         const auto c = read_console_output_char(comm, state, host_io, info, COORD{ 3, 0 }, 728);
         return a && b && space && c &&
                a.value() == L'A' &&
                b.value() == L'B' &&
                space.value() == L' ' &&
                c.value() == L'C';
     }
 
     bool test_write_console_vt_decaln_screen_alignment_pattern_fills_and_homes_cursor()
     {
         MemoryComm comm{};
         oc::condrv::ServerState state{};
         TestHostIo host_io{};
 
         auto connect_packet = make_connect_packet(20023, 20024);
         oc::condrv::BasicApiMessage<MemoryComm> connect_message(comm, connect_packet);
         auto connect_outcome = oc::condrv::dispatch_message(state, connect_message, host_io);
         if (!connect_outcome)
         {
             return false;
         }
 
         state.set_output_mode(
             ENABLE_PROCESSED_OUTPUT |
             ENABLE_WRAP_AT_EOL_OUTPUT |
             ENABLE_VIRTUAL_TERMINAL_PROCESSING);
 
         auto info = unpack_connection_information(connect_message.completion());
 
         // Establish some non-default state, then run DECALN (ESC # 8).
         // - set scrolling margins + origin mode
         // - set red + reverse video
         // DECALN should:
         // - fill the screen with 'E' using the default attributes
         // - reset origin mode and scrolling margins
         // - clear reverse/underline bits in the current attributes
         // - home the cursor before printing 'Z'
         constexpr std::string_view text = "\x1b[2;4r\x1b[?6h\x1b[31;7m\x1b#8Z";
         if (!write_console_user_defined_a(comm, state, host_io, info, text, 729))
         {
             return false;
         }
 
         const auto z = read_console_output_char(comm, state, host_io, info, COORD{ 0, 0 }, 730);
         const auto e1 = read_console_output_char(comm, state, host_io, info, COORD{ 1, 0 }, 731);
         const auto e2 = read_console_output_char(comm, state, host_io, info, COORD{ 0, 1 }, 732);
         if (!z || !e1 || !e2 || z.value() != L'Z' || e1.value() != L'E' || e2.value() != L'E')
         {
             return false;
         }
 
         const auto read_one_attr = [&](const COORD coord, const ULONG id) noexcept -> std::optional<USHORT> {
             constexpr ULONG read_api_size = sizeof(CONSOLE_READCONSOLEOUTPUTSTRING_MSG);
             constexpr ULONG read_header_size = sizeof(CONSOLE_MSG_HEADER);
             constexpr ULONG read_read_offset = read_api_size + read_header_size;
             constexpr ULONG output_bytes = sizeof(USHORT);
 
             oc::condrv::IoPacket read_packet{};
             read_packet.payload.user_defined = oc::condrv::UserDefinedPacket{};
             read_packet.descriptor.identifier.LowPart = id;
             read_packet.descriptor.function = oc::condrv::console_io_user_defined;
             read_packet.descriptor.process = info.process;
             read_packet.descriptor.object = info.output;
             read_packet.descriptor.input_size = read_read_offset;
             read_packet.descriptor.output_size = read_api_size + output_bytes;
             read_packet.payload.user_defined.msg_header.ApiNumber = static_cast<ULONG>(ConsolepReadConsoleOutputString);
             read_packet.payload.user_defined.msg_header.ApiDescriptorSize = read_api_size;
 
             auto& body = read_packet.payload.user_defined.u.console_msg_l2.ReadConsoleOutputString;
             body.ReadCoord = coord;
             body.StringType = CONSOLE_ATTRIBUTE;
             body.NumRecords = 0;
 
             comm.input.assign(read_packet.descriptor.input_size, std::byte{});
             comm.output.clear();
 
             oc::condrv::BasicApiMessage<MemoryComm> read_message(comm, read_packet);
             auto read_outcome = oc::condrv::dispatch_message(state, read_message, host_io);
             if (!read_outcome || read_message.completion().io_status.Status != oc::core::status_success)
             {
                 return std::nullopt;
             }
 
             if (auto released = read_message.release_message_buffers(); !released)
             {
                 return std::nullopt;
             }
 
             if (comm.output.size() != read_api_size + output_bytes)
             {
                 return std::nullopt;
             }
 
             USHORT value{};
             std::memcpy(&value, comm.output.data() + read_api_size, sizeof(value));
             return value;
         };
 
         const auto z_attr = read_one_attr(COORD{ 0, 0 }, 733);
         const auto e_attr = read_one_attr(COORD{ 1, 0 }, 734);
         return z_attr && e_attr &&
                z_attr.value() == FOREGROUND_RED &&
                e_attr.value() == 0x07;
     }
 
     bool test_write_console_vt_el_clears_to_end_of_line()
     {
         MemoryComm comm{};
         oc::condrv::ServerState state{};
         TestHostIo host_io{};

        auto connect_packet = make_connect_packet(20013, 20014);
        oc::condrv::BasicApiMessage<MemoryComm> connect_message(comm, connect_packet);
        auto connect_outcome = oc::condrv::dispatch_message(state, connect_message, host_io);
        if (!connect_outcome)
        {
            return false;
        }

        state.set_output_mode(
            ENABLE_PROCESSED_OUTPUT |
            ENABLE_WRAP_AT_EOL_OUTPUT |
            ENABLE_VIRTUAL_TERMINAL_PROCESSING);

        auto info = unpack_connection_information(connect_message.completion());

        constexpr std::string_view text = "HELLO\x1b[1;3H\x1b[K";
        constexpr ULONG api_size = sizeof(CONSOLE_WRITECONSOLE_MSG);
        constexpr ULONG header_size = sizeof(CONSOLE_MSG_HEADER);
        const ULONG read_offset = api_size + header_size;

        oc::condrv::IoPacket packet{};
        packet.payload.user_defined = oc::condrv::UserDefinedPacket{};
        packet.descriptor.identifier.LowPart = 72;
        packet.descriptor.function = oc::condrv::console_io_user_defined;
        packet.descriptor.process = info.process;
        packet.descriptor.object = info.output;
        packet.descriptor.input_size = read_offset + static_cast<ULONG>(text.size());
        packet.descriptor.output_size = api_size;
        packet.payload.user_defined.msg_header.ApiNumber = static_cast<ULONG>(ConsolepWriteConsole);
        packet.payload.user_defined.msg_header.ApiDescriptorSize = api_size;
        packet.payload.user_defined.u.console_msg_l1.WriteConsole.Unicode = FALSE;

        comm.input.assign(packet.descriptor.input_size, std::byte{});
        std::memcpy(comm.input.data() + read_offset, text.data(), text.size());

        oc::condrv::BasicApiMessage<MemoryComm> message(comm, packet);
        auto outcome = oc::condrv::dispatch_message(state, message, host_io);
        if (!outcome || message.completion().io_status.Status != oc::core::status_success)
        {
            return false;
        }

        // After moving the cursor to column 3 and clearing to end-of-line, we should see "HE   ".
        {
            constexpr ULONG read_api_size = sizeof(CONSOLE_READCONSOLEOUTPUTSTRING_MSG);
            constexpr ULONG read_header_size = sizeof(CONSOLE_MSG_HEADER);
            constexpr ULONG read_read_offset = read_api_size + read_header_size;
            constexpr ULONG output_bytes = 5 * sizeof(wchar_t);

            oc::condrv::IoPacket read_packet{};
            read_packet.payload.user_defined = oc::condrv::UserDefinedPacket{};
            read_packet.descriptor.identifier.LowPart = 73;
            read_packet.descriptor.function = oc::condrv::console_io_user_defined;
            read_packet.descriptor.process = info.process;
            read_packet.descriptor.object = info.output;
            read_packet.descriptor.input_size = read_read_offset;
            read_packet.descriptor.output_size = read_api_size + output_bytes;
            read_packet.payload.user_defined.msg_header.ApiNumber = static_cast<ULONG>(ConsolepReadConsoleOutputString);
            read_packet.payload.user_defined.msg_header.ApiDescriptorSize = read_api_size;

            auto& body = read_packet.payload.user_defined.u.console_msg_l2.ReadConsoleOutputString;
            body.ReadCoord = COORD{ 0, 0 };
            body.StringType = CONSOLE_REAL_UNICODE;
            body.NumRecords = 0;

            comm.input.assign(read_packet.descriptor.input_size, std::byte{});
            comm.output.clear();

            oc::condrv::BasicApiMessage<MemoryComm> read_message(comm, read_packet);
            auto read_outcome = oc::condrv::dispatch_message(state, read_message, host_io);
            if (!read_outcome || read_message.completion().io_status.Status != oc::core::status_success)
            {
                return false;
            }

            if (auto released = read_message.release_message_buffers(); !released)
            {
                return false;
            }

            if (comm.output.size() != read_api_size + output_bytes)
            {
                return false;
            }

            wchar_t chars[5]{};
            std::memcpy(chars, comm.output.data() + read_api_size, output_bytes);
            return chars[0] == L'H' &&
                   chars[1] == L'E' &&
                   chars[2] == L' ' &&
                   chars[3] == L' ' &&
                   chars[4] == L' ';
        }
    }

    bool test_write_console_vt_osc_title_updates_server_title_and_is_not_rendered()
    {
        MemoryComm comm{};
        oc::condrv::ServerState state{};
        TestHostIo host_io{};

        auto connect_packet = make_connect_packet(20041, 20042);
        oc::condrv::BasicApiMessage<MemoryComm> connect_message(comm, connect_packet);
        auto connect_outcome = oc::condrv::dispatch_message(state, connect_message, host_io);
        if (!connect_outcome)
        {
            return false;
        }

        state.set_output_mode(
            ENABLE_PROCESSED_OUTPUT |
            ENABLE_WRAP_AT_EOL_OUTPUT |
            ENABLE_VIRTUAL_TERMINAL_PROCESSING);

        auto info = unpack_connection_information(connect_message.completion());

        auto get_title_w = [&](std::wstring_view expected, const ULONG id) -> bool {
            const ULONG api_size = sizeof(CONSOLE_GETTITLE_MSG);
            const ULONG header_size = sizeof(CONSOLE_MSG_HEADER);
            const ULONG read_offset = api_size + header_size;
            const ULONG output_bytes = static_cast<ULONG>((expected.size() + 1) * sizeof(wchar_t));

            oc::condrv::IoPacket packet{};
            packet.payload.user_defined = oc::condrv::UserDefinedPacket{};
            packet.descriptor.identifier.LowPart = id;
            packet.descriptor.function = oc::condrv::console_io_user_defined;
            packet.descriptor.process = info.process;
            packet.descriptor.object = info.output;
            packet.descriptor.input_size = read_offset;
            packet.descriptor.output_size = api_size + output_bytes;
            packet.payload.user_defined.msg_header.ApiNumber = static_cast<ULONG>(ConsolepGetTitle);
            packet.payload.user_defined.msg_header.ApiDescriptorSize = api_size;

            auto& body = packet.payload.user_defined.u.console_msg_l2.GetConsoleTitle;
            body.TitleLength = 0;
            body.Unicode = TRUE;
            body.Original = FALSE;

            comm.input.assign(packet.descriptor.input_size, std::byte{});
            comm.output.clear();

            oc::condrv::BasicApiMessage<MemoryComm> message(comm, packet);
            auto outcome = oc::condrv::dispatch_message(state, message, host_io);
            if (!outcome)
            {
                return false;
            }
            if (message.completion().io_status.Status != oc::core::status_success)
            {
                return false;
            }
            if (message.packet().payload.user_defined.u.console_msg_l2.GetConsoleTitle.TitleLength != expected.size())
            {
                return false;
            }

            if (auto released = message.release_message_buffers(); !released)
            {
                return false;
            }

            const size_t expected_bytes = expected.size() * sizeof(wchar_t);
            if (comm.output.size() != api_size + expected_bytes)
            {
                return false;
            }

            std::wstring actual(expected.size(), L'\0');
            std::memcpy(actual.data(), comm.output.data() + api_size, expected_bytes);
            return actual == expected;
        };

        if (!write_console_user_defined_a(comm, state, host_io, info, "A", 600))
        {
            return false;
        }
        if (!write_console_user_defined_a(comm, state, host_io, info, "\x1b]0;hello\x07", 601))
        {
            return false;
        }
        if (!write_console_user_defined_a(comm, state, host_io, info, "B", 602))
        {
            return false;
        }

        const auto a = read_console_output_char(comm, state, host_io, info, COORD{ 0, 0 }, 603);
        if (!a || *a != L'A')
        {
            return false;
        }

        const auto b = read_console_output_char(comm, state, host_io, info, COORD{ 1, 0 }, 604);
        if (!b || *b != L'B')
        {
            return false;
        }

        if (!get_title_w(L"hello", 605))
        {
            return false;
        }

        if (!write_console_user_defined_a(comm, state, host_io, info, "\x1b]2;world\x1b\\", 606))
        {
            return false;
        }
        if (!get_title_w(L"world", 607))
        {
            return false;
        }

        if (!write_console_user_defined_a(comm, state, host_io, info, "\x1b]21;third\x07", 608))
        {
            return false;
        }
        if (!get_title_w(L"third", 609))
        {
            return false;
        }

        // Verify the C1 OSC prefix (U+009D) is also consumed.
        constexpr wchar_t c1_osc[]{ static_cast<wchar_t>(0x009d), L'2', L';', L'c', L'1', L'\x07' };
        if (!write_console_user_defined_w(comm, state, host_io, info, std::wstring_view(c1_osc, std::size(c1_osc)), 610))
        {
            return false;
        }

        return get_title_w(L"c1", 611);
    }

    bool test_write_console_vt_split_osc_title_is_consumed_and_updates_state()
    {
        MemoryComm comm{};
        oc::condrv::ServerState state{};
        TestHostIo host_io{};

        auto connect_packet = make_connect_packet(20045, 20046);
        oc::condrv::BasicApiMessage<MemoryComm> connect_message(comm, connect_packet);
        auto connect_outcome = oc::condrv::dispatch_message(state, connect_message, host_io);
        if (!connect_outcome)
        {
            return false;
        }

        state.set_output_mode(
            ENABLE_PROCESSED_OUTPUT |
            ENABLE_WRAP_AT_EOL_OUTPUT |
            ENABLE_VIRTUAL_TERMINAL_PROCESSING);

        auto info = unpack_connection_information(connect_message.completion());

        if (!write_console_user_defined_a(comm, state, host_io, info, "A", 620))
        {
            return false;
        }

        // Split the OSC payload across separate writes to ensure the VT parser retains state.
        if (!write_console_user_defined_a(comm, state, host_io, info, "\x1b]2;hello", 621))
        {
            return false;
        }
        if (!write_console_user_defined_a(comm, state, host_io, info, "world\x07" "B", 622))
        {
            return false;
        }

        const auto a = read_console_output_char(comm, state, host_io, info, COORD{ 0, 0 }, 623);
        const auto b = read_console_output_char(comm, state, host_io, info, COORD{ 1, 0 }, 624);
        return a && b &&
               a.value() == L'A' &&
               b.value() == L'B' &&
               state.title(false) == L"helloworld";
    }

    bool test_write_console_vt_split_osc_st_terminator_is_consumed_and_updates_state()
    {
        MemoryComm comm{};
        oc::condrv::ServerState state{};
        TestHostIo host_io{};

        auto connect_packet = make_connect_packet(20047, 20048);
        oc::condrv::BasicApiMessage<MemoryComm> connect_message(comm, connect_packet);
        auto connect_outcome = oc::condrv::dispatch_message(state, connect_message, host_io);
        if (!connect_outcome)
        {
            return false;
        }

        state.set_output_mode(
            ENABLE_PROCESSED_OUTPUT |
            ENABLE_WRAP_AT_EOL_OUTPUT |
            ENABLE_VIRTUAL_TERMINAL_PROCESSING);

        auto info = unpack_connection_information(connect_message.completion());

        if (!write_console_user_defined_a(comm, state, host_io, info, "A", 630))
        {
            return false;
        }

        // Split the ST terminator (ESC \\) across writes.
        if (!write_console_user_defined_a(comm, state, host_io, info, "\x1b]2;split\x1b", 631))
        {
            return false;
        }
        if (!write_console_user_defined_a(comm, state, host_io, info, "\\B", 632))
        {
            return false;
        }

        const auto a = read_console_output_char(comm, state, host_io, info, COORD{ 0, 0 }, 633);
        const auto b = read_console_output_char(comm, state, host_io, info, COORD{ 1, 0 }, 634);
        return a && b &&
               a.value() == L'A' &&
               b.value() == L'B' &&
               state.title(false) == L"split";
    }

    bool test_write_console_vt_split_csi_sequence_is_consumed()
    {
        MemoryComm comm{};
        oc::condrv::ServerState state{};
        TestHostIo host_io{};

        auto connect_packet = make_connect_packet(20049, 20050);
        oc::condrv::BasicApiMessage<MemoryComm> connect_message(comm, connect_packet);
        auto connect_outcome = oc::condrv::dispatch_message(state, connect_message, host_io);
        if (!connect_outcome)
        {
            return false;
        }

        state.set_output_mode(
            ENABLE_PROCESSED_OUTPUT |
            ENABLE_WRAP_AT_EOL_OUTPUT |
            ENABLE_VIRTUAL_TERMINAL_PROCESSING);

        auto info = unpack_connection_information(connect_message.completion());

        if (!write_console_user_defined_a(comm, state, host_io, info, "A", 640))
        {
            return false;
        }

        // Split ED (ESC[2J) across writes; the escape bytes must not render.
        if (!write_console_user_defined_a(comm, state, host_io, info, "\x1b[2", 641))
        {
            return false;
        }
        if (!write_console_user_defined_a(comm, state, host_io, info, "JB", 642))
        {
            return false;
        }

        const auto c0 = read_console_output_char(comm, state, host_io, info, COORD{ 0, 0 }, 643);
        const auto c1 = read_console_output_char(comm, state, host_io, info, COORD{ 1, 0 }, 644);
        return c0 && c1 &&
               c0.value() == L' ' &&
               c1.value() == L'B';
    }

    bool test_write_console_vt_split_charset_designation_is_consumed()
    {
        MemoryComm comm{};
        oc::condrv::ServerState state{};
        TestHostIo host_io{};

        auto connect_packet = make_connect_packet(20051, 20052);
        oc::condrv::BasicApiMessage<MemoryComm> connect_message(comm, connect_packet);
        auto connect_outcome = oc::condrv::dispatch_message(state, connect_message, host_io);
        if (!connect_outcome)
        {
            return false;
        }

        state.set_output_mode(
            ENABLE_PROCESSED_OUTPUT |
            ENABLE_WRAP_AT_EOL_OUTPUT |
            ENABLE_VIRTUAL_TERMINAL_PROCESSING);

        auto info = unpack_connection_information(connect_message.completion());

        if (!write_console_user_defined_a(comm, state, host_io, info, "A\x1b(", 650))
        {
            return false;
        }
        if (!write_console_user_defined_a(comm, state, host_io, info, "0B", 651))
        {
            return false;
        }

        const auto a = read_console_output_char(comm, state, host_io, info, COORD{ 0, 0 }, 652);
        const auto b = read_console_output_char(comm, state, host_io, info, COORD{ 1, 0 }, 653);
        return a && b &&
               a.value() == L'A' &&
               b.value() == L'B';
    }

    bool test_write_console_vt_split_dcs_string_is_consumed()
    {
        MemoryComm comm{};
        oc::condrv::ServerState state{};
        TestHostIo host_io{};

        auto connect_packet = make_connect_packet(20053, 20054);
        oc::condrv::BasicApiMessage<MemoryComm> connect_message(comm, connect_packet);
        auto connect_outcome = oc::condrv::dispatch_message(state, connect_message, host_io);
        if (!connect_outcome)
        {
            return false;
        }

        state.set_output_mode(
            ENABLE_PROCESSED_OUTPUT |
            ENABLE_WRAP_AT_EOL_OUTPUT |
            ENABLE_VIRTUAL_TERMINAL_PROCESSING);

        auto info = unpack_connection_information(connect_message.completion());

        // DCS payload is ignored until ST; split the ESC \\ terminator across writes.
        if (!write_console_user_defined_a(comm, state, host_io, info, "A\x1bP1;2", 660))
        {
            return false;
        }
        if (!write_console_user_defined_a(comm, state, host_io, info, "\x1b", 661))
        {
            return false;
        }
        if (!write_console_user_defined_a(comm, state, host_io, info, "\\B", 662))
        {
            return false;
        }

        const auto a = read_console_output_char(comm, state, host_io, info, COORD{ 0, 0 }, 663);
        const auto b = read_console_output_char(comm, state, host_io, info, COORD{ 1, 0 }, 664);
        return a && b &&
               a.value() == L'A' &&
               b.value() == L'B';
    }

    bool test_write_console_vt_dsr_cpr_injects_response_into_input_queue()
    {
        MemoryComm comm{};
        oc::condrv::ServerState state{};
        TestHostIo host_io{};

        auto connect_packet = make_connect_packet(20043, 20044);
        oc::condrv::BasicApiMessage<MemoryComm> connect_message(comm, connect_packet);
        auto connect_outcome = oc::condrv::dispatch_message(state, connect_message, host_io);
        if (!connect_outcome)
        {
            return false;
        }

        state.set_output_mode(
            ENABLE_PROCESSED_OUTPUT |
            ENABLE_WRAP_AT_EOL_OUTPUT |
            ENABLE_VIRTUAL_TERMINAL_PROCESSING);

        auto info = unpack_connection_information(connect_message.completion());

        // After writing 'A', the cursor is at column 2 (1-based). DSR CPR should respond with ESC[1;2R.
        if (!write_console_user_defined_a(comm, state, host_io, info, "A\x1b[6nB", 610))
        {
            return false;
        }

        oc::condrv::IoPacket packet{};
        packet.descriptor.identifier.LowPart = 611;
        packet.descriptor.function = oc::condrv::console_io_raw_read;
        packet.descriptor.process = info.process;
        packet.descriptor.object = info.input;
        packet.descriptor.input_size = 0;
        packet.descriptor.output_size = 32;

        comm.input.clear();
        comm.output.clear();

        oc::condrv::BasicApiMessage<MemoryComm> message(comm, packet);
        auto outcome = oc::condrv::dispatch_message(state, message, host_io);
        if (!outcome)
        {
            return false;
        }

        if (message.completion().io_status.Status != oc::core::status_success)
        {
            return false;
        }

        if (auto released = message.release_message_buffers(); !released)
        {
            return false;
        }

        static constexpr std::byte expected[] = {
            static_cast<std::byte>(0x1b),
            static_cast<std::byte>('['),
            static_cast<std::byte>('1'),
            static_cast<std::byte>(';'),
            static_cast<std::byte>('2'),
            static_cast<std::byte>('R'),
        };

        if (comm.output.size() != sizeof(expected))
        {
            return false;
        }

        return std::memcmp(comm.output.data(), expected, sizeof(expected)) == 0;
    }

    bool test_write_console_vt_dsr_cpr_respects_host_query_policy()
    {
        MemoryComm comm{};
        oc::condrv::ServerState state{};
        TestHostIo host_io{};
        host_io.answer_vt_queries = false;

        auto connect_packet = make_connect_packet(20045, 20046);
        oc::condrv::BasicApiMessage<MemoryComm> connect_message(comm, connect_packet);
        auto connect_outcome = oc::condrv::dispatch_message(state, connect_message, host_io);
        if (!connect_outcome)
        {
            return false;
        }

        state.set_output_mode(
            ENABLE_PROCESSED_OUTPUT |
            ENABLE_WRAP_AT_EOL_OUTPUT |
            ENABLE_VIRTUAL_TERMINAL_PROCESSING);

        auto info = unpack_connection_information(connect_message.completion());
        if (!write_console_user_defined_a(comm, state, host_io, info, "\x1b[6n", 612))
        {
            return false;
        }

        oc::condrv::IoPacket packet{};
        packet.descriptor.identifier.LowPart = 613;
        packet.descriptor.function = oc::condrv::console_io_raw_read;
        packet.descriptor.process = info.process;
        packet.descriptor.object = info.input;
        packet.descriptor.input_size = 0;
        packet.descriptor.output_size = 32;

        comm.input.clear();
        comm.output.clear();

        oc::condrv::BasicApiMessage<MemoryComm> message(comm, packet);
        auto outcome = oc::condrv::dispatch_message(state, message, host_io);
        if (!outcome)
        {
            return false;
        }

        if (message.completion().io_status.Status != oc::core::status_success)
        {
            return false;
        }

        if (message.completion().io_status.Information != 0)
        {
            return false;
        }

        if (auto released = message.release_message_buffers(); !released)
        {
            return false;
        }

        return comm.output.empty();
    }

    bool test_write_console_vt_csi_save_restore_cursor_state()
    {
        MemoryComm comm{};
        oc::condrv::ServerState state{};
        TestHostIo host_io{};

        auto connect_packet = make_connect_packet(20015, 20016);
        oc::condrv::BasicApiMessage<MemoryComm> connect_message(comm, connect_packet);
        auto connect_outcome = oc::condrv::dispatch_message(state, connect_message, host_io);
        if (!connect_outcome)
        {
            return false;
        }

        state.set_output_mode(
            ENABLE_PROCESSED_OUTPUT |
            ENABLE_WRAP_AT_EOL_OUTPUT |
            ENABLE_VIRTUAL_TERMINAL_PROCESSING);

        auto info = unpack_connection_information(connect_message.completion());

        // Write:
        //  - A at (0,0)
        //  - Save cursor state at (1,0) with default attributes
        //  - Set red attributes and write R at (5,1)
        //  - Restore cursor state and write B at (1,0) with default attributes
        constexpr std::string_view text = "A\x1b[s\x1b[31m\x1b[2;6HR\x1b[uB";
        constexpr ULONG api_size = sizeof(CONSOLE_WRITECONSOLE_MSG);
        constexpr ULONG header_size = sizeof(CONSOLE_MSG_HEADER);
        const ULONG read_offset = api_size + header_size;

        oc::condrv::IoPacket packet{};
        packet.payload.user_defined = oc::condrv::UserDefinedPacket{};
        packet.descriptor.identifier.LowPart = 74;
        packet.descriptor.function = oc::condrv::console_io_user_defined;
        packet.descriptor.process = info.process;
        packet.descriptor.object = info.output;
        packet.descriptor.input_size = read_offset + static_cast<ULONG>(text.size());
        packet.descriptor.output_size = api_size;
        packet.payload.user_defined.msg_header.ApiNumber = static_cast<ULONG>(ConsolepWriteConsole);
        packet.payload.user_defined.msg_header.ApiDescriptorSize = api_size;
        packet.payload.user_defined.u.console_msg_l1.WriteConsole.Unicode = FALSE;

        comm.input.assign(packet.descriptor.input_size, std::byte{});
        std::memcpy(comm.input.data() + read_offset, text.data(), text.size());

        oc::condrv::BasicApiMessage<MemoryComm> message(comm, packet);
        auto outcome = oc::condrv::dispatch_message(state, message, host_io);
        if (!outcome || message.completion().io_status.Status != oc::core::status_success)
        {
            return false;
        }

        const auto read_one_char = [&](const COORD coord, const ULONG id) noexcept -> std::optional<wchar_t> {
            constexpr ULONG read_api_size = sizeof(CONSOLE_READCONSOLEOUTPUTSTRING_MSG);
            constexpr ULONG read_header_size = sizeof(CONSOLE_MSG_HEADER);
            constexpr ULONG read_read_offset = read_api_size + read_header_size;
            constexpr ULONG output_bytes = sizeof(wchar_t);

            oc::condrv::IoPacket read_packet{};
            read_packet.payload.user_defined = oc::condrv::UserDefinedPacket{};
            read_packet.descriptor.identifier.LowPart = id;
            read_packet.descriptor.function = oc::condrv::console_io_user_defined;
            read_packet.descriptor.process = info.process;
            read_packet.descriptor.object = info.output;
            read_packet.descriptor.input_size = read_read_offset;
            read_packet.descriptor.output_size = read_api_size + output_bytes;
            read_packet.payload.user_defined.msg_header.ApiNumber = static_cast<ULONG>(ConsolepReadConsoleOutputString);
            read_packet.payload.user_defined.msg_header.ApiDescriptorSize = read_api_size;

            auto& body = read_packet.payload.user_defined.u.console_msg_l2.ReadConsoleOutputString;
            body.ReadCoord = coord;
            body.StringType = CONSOLE_REAL_UNICODE;
            body.NumRecords = 0;

            comm.input.assign(read_packet.descriptor.input_size, std::byte{});
            comm.output.clear();

            oc::condrv::BasicApiMessage<MemoryComm> read_message(comm, read_packet);
            auto read_outcome = oc::condrv::dispatch_message(state, read_message, host_io);
            if (!read_outcome || read_message.completion().io_status.Status != oc::core::status_success)
            {
                return std::nullopt;
            }

            if (auto released = read_message.release_message_buffers(); !released)
            {
                return std::nullopt;
            }

            if (comm.output.size() != read_api_size + output_bytes)
            {
                return std::nullopt;
            }

            wchar_t value{};
            std::memcpy(&value, comm.output.data() + read_api_size, sizeof(value));
            return value;
        };

        const auto read_one_attr = [&](const COORD coord, const ULONG id) noexcept -> std::optional<USHORT> {
            constexpr ULONG read_api_size = sizeof(CONSOLE_READCONSOLEOUTPUTSTRING_MSG);
            constexpr ULONG read_header_size = sizeof(CONSOLE_MSG_HEADER);
            constexpr ULONG read_read_offset = read_api_size + read_header_size;
            constexpr ULONG output_bytes = sizeof(USHORT);

            oc::condrv::IoPacket read_packet{};
            read_packet.payload.user_defined = oc::condrv::UserDefinedPacket{};
            read_packet.descriptor.identifier.LowPart = id;
            read_packet.descriptor.function = oc::condrv::console_io_user_defined;
            read_packet.descriptor.process = info.process;
            read_packet.descriptor.object = info.output;
            read_packet.descriptor.input_size = read_read_offset;
            read_packet.descriptor.output_size = read_api_size + output_bytes;
            read_packet.payload.user_defined.msg_header.ApiNumber = static_cast<ULONG>(ConsolepReadConsoleOutputString);
            read_packet.payload.user_defined.msg_header.ApiDescriptorSize = read_api_size;

            auto& body = read_packet.payload.user_defined.u.console_msg_l2.ReadConsoleOutputString;
            body.ReadCoord = coord;
            body.StringType = CONSOLE_ATTRIBUTE;
            body.NumRecords = 0;

            comm.input.assign(read_packet.descriptor.input_size, std::byte{});
            comm.output.clear();

            oc::condrv::BasicApiMessage<MemoryComm> read_message(comm, read_packet);
            auto read_outcome = oc::condrv::dispatch_message(state, read_message, host_io);
            if (!read_outcome || read_message.completion().io_status.Status != oc::core::status_success)
            {
                return std::nullopt;
            }

            if (auto released = read_message.release_message_buffers(); !released)
            {
                return std::nullopt;
            }

            if (comm.output.size() != read_api_size + output_bytes)
            {
                return std::nullopt;
            }

            USHORT value{};
            std::memcpy(&value, comm.output.data() + read_api_size, sizeof(value));
            return value;
        };

        const auto a = read_one_char(COORD{ 0, 0 }, 75);
        const auto b = read_one_char(COORD{ 1, 0 }, 76);
        const auto r = read_one_char(COORD{ 5, 1 }, 77);
        if (!a || !b || !r || a.value() != L'A' || b.value() != L'B' || r.value() != L'R')
        {
            return false;
        }

        const auto b_attr = read_one_attr(COORD{ 1, 0 }, 78);
        const auto r_attr = read_one_attr(COORD{ 5, 1 }, 79);
        return b_attr && r_attr && b_attr.value() == 0x07 && r_attr.value() == FOREGROUND_RED;
    }

    bool test_write_console_vt_decsc_decrc_save_restore_cursor_state()
    {
        MemoryComm comm{};
        oc::condrv::ServerState state{};
        TestHostIo host_io{};

        auto connect_packet = make_connect_packet(20017, 20018);
        oc::condrv::BasicApiMessage<MemoryComm> connect_message(comm, connect_packet);
        auto connect_outcome = oc::condrv::dispatch_message(state, connect_message, host_io);
        if (!connect_outcome)
        {
            return false;
        }

        state.set_output_mode(
            ENABLE_PROCESSED_OUTPUT |
            ENABLE_WRAP_AT_EOL_OUTPUT |
            ENABLE_VIRTUAL_TERMINAL_PROCESSING);

        auto info = unpack_connection_information(connect_message.completion());

        // Equivalent to the CSI save/restore test, but using DECSC/DECRC (ESC7/ESC8).
        constexpr std::string_view text = "A\x1b"
                                          "7\x1b[31m\x1b[2;6HR\x1b"
                                          "8B";
        constexpr ULONG api_size = sizeof(CONSOLE_WRITECONSOLE_MSG);
        constexpr ULONG header_size = sizeof(CONSOLE_MSG_HEADER);
        const ULONG read_offset = api_size + header_size;

        oc::condrv::IoPacket packet{};
        packet.payload.user_defined = oc::condrv::UserDefinedPacket{};
        packet.descriptor.identifier.LowPart = 80;
        packet.descriptor.function = oc::condrv::console_io_user_defined;
        packet.descriptor.process = info.process;
        packet.descriptor.object = info.output;
        packet.descriptor.input_size = read_offset + static_cast<ULONG>(text.size());
        packet.descriptor.output_size = api_size;
        packet.payload.user_defined.msg_header.ApiNumber = static_cast<ULONG>(ConsolepWriteConsole);
        packet.payload.user_defined.msg_header.ApiDescriptorSize = api_size;
        packet.payload.user_defined.u.console_msg_l1.WriteConsole.Unicode = FALSE;

        comm.input.assign(packet.descriptor.input_size, std::byte{});
        std::memcpy(comm.input.data() + read_offset, text.data(), text.size());

        oc::condrv::BasicApiMessage<MemoryComm> message(comm, packet);
        auto outcome = oc::condrv::dispatch_message(state, message, host_io);
        if (!outcome || message.completion().io_status.Status != oc::core::status_success)
        {
            return false;
        }

        const auto read_one_char = [&](const COORD coord, const ULONG id) noexcept -> std::optional<wchar_t> {
            constexpr ULONG read_api_size = sizeof(CONSOLE_READCONSOLEOUTPUTSTRING_MSG);
            constexpr ULONG read_header_size = sizeof(CONSOLE_MSG_HEADER);
            constexpr ULONG read_read_offset = read_api_size + read_header_size;
            constexpr ULONG output_bytes = sizeof(wchar_t);

            oc::condrv::IoPacket read_packet{};
            read_packet.payload.user_defined = oc::condrv::UserDefinedPacket{};
            read_packet.descriptor.identifier.LowPart = id;
            read_packet.descriptor.function = oc::condrv::console_io_user_defined;
            read_packet.descriptor.process = info.process;
            read_packet.descriptor.object = info.output;
            read_packet.descriptor.input_size = read_read_offset;
            read_packet.descriptor.output_size = read_api_size + output_bytes;
            read_packet.payload.user_defined.msg_header.ApiNumber = static_cast<ULONG>(ConsolepReadConsoleOutputString);
            read_packet.payload.user_defined.msg_header.ApiDescriptorSize = read_api_size;

            auto& body = read_packet.payload.user_defined.u.console_msg_l2.ReadConsoleOutputString;
            body.ReadCoord = coord;
            body.StringType = CONSOLE_REAL_UNICODE;
            body.NumRecords = 0;

            comm.input.assign(read_packet.descriptor.input_size, std::byte{});
            comm.output.clear();

            oc::condrv::BasicApiMessage<MemoryComm> read_message(comm, read_packet);
            auto read_outcome = oc::condrv::dispatch_message(state, read_message, host_io);
            if (!read_outcome || read_message.completion().io_status.Status != oc::core::status_success)
            {
                return std::nullopt;
            }

            if (auto released = read_message.release_message_buffers(); !released)
            {
                return std::nullopt;
            }

            if (comm.output.size() != read_api_size + output_bytes)
            {
                return std::nullopt;
            }

            wchar_t value{};
            std::memcpy(&value, comm.output.data() + read_api_size, sizeof(value));
            return value;
        };

        const auto read_one_attr = [&](const COORD coord, const ULONG id) noexcept -> std::optional<USHORT> {
            constexpr ULONG read_api_size = sizeof(CONSOLE_READCONSOLEOUTPUTSTRING_MSG);
            constexpr ULONG read_header_size = sizeof(CONSOLE_MSG_HEADER);
            constexpr ULONG read_read_offset = read_api_size + read_header_size;
            constexpr ULONG output_bytes = sizeof(USHORT);

            oc::condrv::IoPacket read_packet{};
            read_packet.payload.user_defined = oc::condrv::UserDefinedPacket{};
            read_packet.descriptor.identifier.LowPart = id;
            read_packet.descriptor.function = oc::condrv::console_io_user_defined;
            read_packet.descriptor.process = info.process;
            read_packet.descriptor.object = info.output;
            read_packet.descriptor.input_size = read_read_offset;
            read_packet.descriptor.output_size = read_api_size + output_bytes;
            read_packet.payload.user_defined.msg_header.ApiNumber = static_cast<ULONG>(ConsolepReadConsoleOutputString);
            read_packet.payload.user_defined.msg_header.ApiDescriptorSize = read_api_size;

            auto& body = read_packet.payload.user_defined.u.console_msg_l2.ReadConsoleOutputString;
            body.ReadCoord = coord;
            body.StringType = CONSOLE_ATTRIBUTE;
            body.NumRecords = 0;

            comm.input.assign(read_packet.descriptor.input_size, std::byte{});
            comm.output.clear();

            oc::condrv::BasicApiMessage<MemoryComm> read_message(comm, read_packet);
            auto read_outcome = oc::condrv::dispatch_message(state, read_message, host_io);
            if (!read_outcome || read_message.completion().io_status.Status != oc::core::status_success)
            {
                return std::nullopt;
            }

            if (auto released = read_message.release_message_buffers(); !released)
            {
                return std::nullopt;
            }

            if (comm.output.size() != read_api_size + output_bytes)
            {
                return std::nullopt;
            }

            USHORT value{};
            std::memcpy(&value, comm.output.data() + read_api_size, sizeof(value));
            return value;
        };

        const auto a = read_one_char(COORD{ 0, 0 }, 81);
        const auto b = read_one_char(COORD{ 1, 0 }, 82);
        const auto r = read_one_char(COORD{ 5, 1 }, 83);
        if (!a || !b || !r || a.value() != L'A' || b.value() != L'B' || r.value() != L'R')
        {
            return false;
        }

        // "B" restored to default attributes; "R" is red.
        const auto b_attr = read_one_attr(COORD{ 1, 0 }, 84);
        const auto r_attr = read_one_attr(COORD{ 5, 1 }, 85);
        return b_attr && r_attr && b_attr.value() == 0x07 && r_attr.value() == FOREGROUND_RED;
    }

    bool test_write_console_vt_dectcem_toggles_cursor_visibility()
    {
        MemoryComm comm{};
        oc::condrv::ServerState state{};
        TestHostIo host_io{};

        auto connect_packet = make_connect_packet(20019, 20020);
        oc::condrv::BasicApiMessage<MemoryComm> connect_message(comm, connect_packet);
        auto connect_outcome = oc::condrv::dispatch_message(state, connect_message, host_io);
        if (!connect_outcome)
        {
            return false;
        }

        state.set_output_mode(
            ENABLE_PROCESSED_OUTPUT |
            ENABLE_WRAP_AT_EOL_OUTPUT |
            ENABLE_VIRTUAL_TERMINAL_PROCESSING);

        auto info = unpack_connection_information(connect_message.completion());

        const auto write_console_a = [&](const std::string_view text, const ULONG id) noexcept -> bool {
            constexpr ULONG api_size = sizeof(CONSOLE_WRITECONSOLE_MSG);
            constexpr ULONG header_size = sizeof(CONSOLE_MSG_HEADER);
            const ULONG read_offset = api_size + header_size;

            oc::condrv::IoPacket packet{};
            packet.payload.user_defined = oc::condrv::UserDefinedPacket{};
            packet.descriptor.identifier.LowPart = id;
            packet.descriptor.function = oc::condrv::console_io_user_defined;
            packet.descriptor.process = info.process;
            packet.descriptor.object = info.output;
            packet.descriptor.input_size = read_offset + static_cast<ULONG>(text.size());
            packet.descriptor.output_size = api_size;
            packet.payload.user_defined.msg_header.ApiNumber = static_cast<ULONG>(ConsolepWriteConsole);
            packet.payload.user_defined.msg_header.ApiDescriptorSize = api_size;
            packet.payload.user_defined.u.console_msg_l1.WriteConsole.Unicode = FALSE;

            comm.input.assign(packet.descriptor.input_size, std::byte{});
            std::memcpy(comm.input.data() + read_offset, text.data(), text.size());

            oc::condrv::BasicApiMessage<MemoryComm> message(comm, packet);
            auto outcome = oc::condrv::dispatch_message(state, message, host_io);
            return outcome && message.completion().io_status.Status == oc::core::status_success;
        };

        const auto get_cursor_visible = [&](const ULONG id) noexcept -> std::optional<bool> {
            oc::condrv::IoPacket packet{};
            packet.payload.user_defined = oc::condrv::UserDefinedPacket{};
            packet.descriptor.identifier.LowPart = id;
            packet.descriptor.function = oc::condrv::console_io_user_defined;
            packet.descriptor.process = info.process;
            packet.descriptor.object = info.output;
            packet.payload.user_defined.msg_header.ApiNumber = static_cast<ULONG>(ConsolepGetCursorInfo);
            packet.payload.user_defined.msg_header.ApiDescriptorSize = sizeof(CONSOLE_GETCURSORINFO_MSG);

            oc::condrv::BasicApiMessage<MemoryComm> message(comm, packet);
            auto outcome = oc::condrv::dispatch_message(state, message, host_io);
            if (!outcome || message.completion().io_status.Status != oc::core::status_success)
            {
                return std::nullopt;
            }

            const auto& cursor = message.packet().payload.user_defined.u.console_msg_l2.GetConsoleCursorInfo;
            return cursor.Visible != FALSE;
        };

        // Hide cursor: CSI ? 25 l.
        if (!write_console_a("\x1b[?25l", 86))
        {
            return false;
        }

        const auto hidden = get_cursor_visible(87);
        if (!hidden || hidden.value())
        {
            return false;
        }

        // Show cursor: CSI ? 25 h.
        if (!write_console_a("\x1b[?25h", 88))
        {
            return false;
        }

        const auto shown = get_cursor_visible(89);
        return shown && shown.value();
    }

    bool test_write_console_vt_delayed_wrap_allows_carriage_return_before_wrap()
    {
        MemoryComm comm{};
        oc::condrv::ServerState state{};
        TestHostIo host_io{};

        auto connect_packet = make_connect_packet(20049, 20050);
        oc::condrv::BasicApiMessage<MemoryComm> connect_message(comm, connect_packet);
        auto connect_outcome = oc::condrv::dispatch_message(state, connect_message, host_io);
        if (!connect_outcome)
        {
            return false;
        }

        state.set_output_mode(
            ENABLE_PROCESSED_OUTPUT |
            ENABLE_WRAP_AT_EOL_OUTPUT |
            ENABLE_VIRTUAL_TERMINAL_PROCESSING);

        const auto info = unpack_connection_information(connect_message.completion());

        if (!set_screen_buffer_size_user_defined(comm, state, host_io, info, COORD{ 5, 3 }, 213))
        {
            return false;
        }

        // When delayed wrap is implemented, printing the final column sets a wrap flag instead of
        // immediately moving the cursor. Carriage return should move within the current line and
        // clear the wrap condition before the next printable character is output.
        if (!write_console_user_defined_a(
                comm,
                state,
                host_io,
                info,
                "\x1b[2J\x1b[1;1HABCDE\rZ",
                214))
        {
            return false;
        }

        const auto row1_col1 = read_console_output_char(comm, state, host_io, info, COORD{ 0, 0 }, 215);
        const auto row2_col1 = read_console_output_char(comm, state, host_io, info, COORD{ 0, 1 }, 216);
        return row1_col1 && row2_col1 && row1_col1.value() == L'Z' && row2_col1.value() == L' ';
    }

    bool test_write_console_vt_decawm_disable_prevents_wrap_and_overwrites_last_column()
    {
        MemoryComm comm{};
        oc::condrv::ServerState state{};
        TestHostIo host_io{};

        auto connect_packet = make_connect_packet(20051, 20052);
        oc::condrv::BasicApiMessage<MemoryComm> connect_message(comm, connect_packet);
        auto connect_outcome = oc::condrv::dispatch_message(state, connect_message, host_io);
        if (!connect_outcome)
        {
            return false;
        }

        state.set_output_mode(
            ENABLE_PROCESSED_OUTPUT |
            ENABLE_WRAP_AT_EOL_OUTPUT |
            ENABLE_VIRTUAL_TERMINAL_PROCESSING);

        const auto info = unpack_connection_information(connect_message.completion());

        if (!set_screen_buffer_size_user_defined(comm, state, host_io, info, COORD{ 5, 3 }, 217))
        {
            return false;
        }

        // Disable VT autowrap (DECAWM). The final glyph should not trigger a delayed wrap; subsequent
        // output overwrites the last column instead of flowing to the next line.
        if (!write_console_user_defined_a(
                comm,
                state,
                host_io,
                info,
                "\x1b[?7l\x1b[2J\x1b[1;1HABCDEZ",
                218))
        {
            return false;
        }

        const auto last_column = read_console_output_char(comm, state, host_io, info, COORD{ 4, 0 }, 219);
        const auto next_row = read_console_output_char(comm, state, host_io, info, COORD{ 0, 1 }, 220);
        return last_column && next_row && last_column.value() == L'Z' && next_row.value() == L' ';
    }

    bool test_write_console_vt_origin_mode_homes_cursor_to_margin_top()
    {
        MemoryComm comm{};
        oc::condrv::ServerState state{};
        TestHostIo host_io{};

        auto connect_packet = make_connect_packet(20053, 20054);
        oc::condrv::BasicApiMessage<MemoryComm> connect_message(comm, connect_packet);
        auto connect_outcome = oc::condrv::dispatch_message(state, connect_message, host_io);
        if (!connect_outcome)
        {
            return false;
        }

        state.set_output_mode(
            ENABLE_PROCESSED_OUTPUT |
            ENABLE_WRAP_AT_EOL_OUTPUT |
            ENABLE_VIRTUAL_TERMINAL_PROCESSING);

        const auto info = unpack_connection_information(connect_message.completion());

        if (!set_screen_buffer_size_user_defined(comm, state, host_io, info, COORD{ 10, 5 }, 221))
        {
            return false;
        }

        if (!write_console_user_defined_a(
                comm,
                state,
                host_io,
                info,
                "\x1b[2J"
                "\x1b[2;4r"
                "\x1b[?6h"
                "A"
                "\x1b[?6l"
                "B",
                222))
        {
            return false;
        }

        const auto b = read_console_output_char(comm, state, host_io, info, COORD{ 0, 0 }, 223);
        const auto a = read_console_output_char(comm, state, host_io, info, COORD{ 0, 1 }, 224);
        return a && b && a.value() == L'A' && b.value() == L'B';
    }

    bool test_write_console_vt_origin_mode_clamps_cursor_to_bottom_margin()
    {
        MemoryComm comm{};
        oc::condrv::ServerState state{};
        TestHostIo host_io{};

        auto connect_packet = make_connect_packet(20055, 20056);
        oc::condrv::BasicApiMessage<MemoryComm> connect_message(comm, connect_packet);
        auto connect_outcome = oc::condrv::dispatch_message(state, connect_message, host_io);
        if (!connect_outcome)
        {
            return false;
        }

        state.set_output_mode(
            ENABLE_PROCESSED_OUTPUT |
            ENABLE_WRAP_AT_EOL_OUTPUT |
            ENABLE_VIRTUAL_TERMINAL_PROCESSING);

        const auto info = unpack_connection_information(connect_message.completion());

        if (!set_screen_buffer_size_user_defined(comm, state, host_io, info, COORD{ 10, 5 }, 225))
        {
            return false;
        }

        if (!write_console_user_defined_a(
                comm,
                state,
                host_io,
                info,
                "\x1b[2J"
                "\x1b[2;4r"
                "\x1b[?6h"
                "\x1b[3;1H"
                "\x1b[1B"
                "X",
                226))
        {
            return false;
        }

        const auto expected = read_console_output_char(comm, state, host_io, info, COORD{ 0, 3 }, 227);
        const auto out_of_region = read_console_output_char(comm, state, host_io, info, COORD{ 0, 4 }, 228);
        return expected && out_of_region && expected.value() == L'X' && out_of_region.value() == L' ';
    }

    bool test_write_console_vt_alt_buffer_1049_clears_and_restores_main()
    {
        MemoryComm comm{};
        oc::condrv::ServerState state{};
        TestHostIo host_io{};

        auto connect_packet = make_connect_packet(20045, 20046);
        oc::condrv::BasicApiMessage<MemoryComm> connect_message(comm, connect_packet);
        auto connect_outcome = oc::condrv::dispatch_message(state, connect_message, host_io);
        if (!connect_outcome)
        {
            return false;
        }

        state.set_output_mode(
            ENABLE_PROCESSED_OUTPUT |
            ENABLE_WRAP_AT_EOL_OUTPUT |
            ENABLE_VIRTUAL_TERMINAL_PROCESSING);

        const auto info = unpack_connection_information(connect_message.completion());

        // Seed main buffer state and park the cursor at (row=2,col=3).
        if (!write_console_user_defined_a(
                comm,
                state,
                host_io,
                info,
                "\x1b[2J"
                "\x1b[1;1HA"
                "\x1b[2;3H",
                200))
        {
            return false;
        }

        // Enter alternate screen buffer. It starts cleared with the cursor homed.
        if (!write_console_user_defined_a(comm, state, host_io, info, "\x1b[?1049h", 201))
        {
            return false;
        }

        const auto alt_clear = read_console_output_char(comm, state, host_io, info, COORD{ 0, 0 }, 202);
        if (!alt_clear || alt_clear.value() != L' ')
        {
            return false;
        }

        // Write within the alternate buffer.
        if (!write_console_user_defined_a(comm, state, host_io, info, "B", 203))
        {
            return false;
        }

        const auto alt_written = read_console_output_char(comm, state, host_io, info, COORD{ 0, 0 }, 204);
        if (!alt_written || alt_written.value() != L'B')
        {
            return false;
        }

        // Exit alternate screen buffer and continue rendering in the restored main buffer.
        if (!write_console_user_defined_a(comm, state, host_io, info, "\x1b[?1049lC", 205))
        {
            return false;
        }

        const auto main_restored = read_console_output_char(comm, state, host_io, info, COORD{ 0, 0 }, 206);
        const auto cursor_restored = read_console_output_char(comm, state, host_io, info, COORD{ 2, 1 }, 207);
        return main_restored && cursor_restored &&
            main_restored.value() == L'A' &&
            cursor_restored.value() == L'C';
    }

    bool test_write_console_vt_alt_buffer_1049_restores_cursor_visibility()
    {
        MemoryComm comm{};
        oc::condrv::ServerState state{};
        TestHostIo host_io{};

        auto connect_packet = make_connect_packet(20047, 20048);
        oc::condrv::BasicApiMessage<MemoryComm> connect_message(comm, connect_packet);
        auto connect_outcome = oc::condrv::dispatch_message(state, connect_message, host_io);
        if (!connect_outcome)
        {
            return false;
        }

        state.set_output_mode(
            ENABLE_PROCESSED_OUTPUT |
            ENABLE_WRAP_AT_EOL_OUTPUT |
            ENABLE_VIRTUAL_TERMINAL_PROCESSING);

        const auto info = unpack_connection_information(connect_message.completion());

        const auto get_cursor_visible = [&](const ULONG id) noexcept -> std::optional<bool> {
            oc::condrv::IoPacket packet{};
            packet.payload.user_defined = oc::condrv::UserDefinedPacket{};
            packet.descriptor.identifier.LowPart = id;
            packet.descriptor.function = oc::condrv::console_io_user_defined;
            packet.descriptor.process = info.process;
            packet.descriptor.object = info.output;
            packet.payload.user_defined.msg_header.ApiNumber = static_cast<ULONG>(ConsolepGetCursorInfo);
            packet.payload.user_defined.msg_header.ApiDescriptorSize = sizeof(CONSOLE_GETCURSORINFO_MSG);

            oc::condrv::BasicApiMessage<MemoryComm> message(comm, packet);
            auto outcome = oc::condrv::dispatch_message(state, message, host_io);
            if (!outcome || message.completion().io_status.Status != oc::core::status_success)
            {
                return std::nullopt;
            }

            const auto& cursor = message.packet().payload.user_defined.u.console_msg_l2.GetConsoleCursorInfo;
            return cursor.Visible != FALSE;
        };

        const auto visible_before = get_cursor_visible(208);
        if (!visible_before || !visible_before.value())
        {
            return false;
        }

        // Enter alt buffer and hide the cursor while in it.
        if (!write_console_user_defined_a(comm, state, host_io, info, "\x1b[?1049h\x1b[?25l", 209))
        {
            return false;
        }

        const auto hidden_in_alt = get_cursor_visible(210);
        if (!hidden_in_alt || hidden_in_alt.value())
        {
            return false;
        }

        // Exit alt buffer: the cursor visibility should restore to the main buffer's state.
        if (!write_console_user_defined_a(comm, state, host_io, info, "\x1b[?1049l", 211))
        {
            return false;
        }

        const auto visible_after = get_cursor_visible(212);
        return visible_after && visible_after.value();
    }

    bool test_write_console_vt_decstbm_linefeed_scrolls_within_margins()
    {
        MemoryComm comm{};
        oc::condrv::ServerState state{};
        TestHostIo host_io{};

        auto connect_packet = make_connect_packet(20021, 20022);
        oc::condrv::BasicApiMessage<MemoryComm> connect_message(comm, connect_packet);
        auto connect_outcome = oc::condrv::dispatch_message(state, connect_message, host_io);
        if (!connect_outcome)
        {
            return false;
        }

        state.set_output_mode(
            ENABLE_PROCESSED_OUTPUT |
            ENABLE_WRAP_AT_EOL_OUTPUT |
            ENABLE_VIRTUAL_TERMINAL_PROCESSING);

        const auto info = unpack_connection_information(connect_message.completion());

        // Seed distinct markers in the first column of rows 1-5.
        if (!write_console_user_defined_a(
                comm,
                state,
                host_io,
                info,
                "\x1b[2J"
                "\x1b[1;1HA"
                "\x1b[2;1HB"
                "\x1b[3;1HC"
                "\x1b[4;1HD"
                "\x1b[5;1HE",
                90))
        {
            return false;
        }

        // Set a scroll region to rows 2-4 (inclusive) and emit a line feed at the bottom margin.
        if (!write_console_user_defined_a(comm, state, host_io, info, "\x1b[2;4r\x1b[4;1H\n", 91))
        {
            return false;
        }

        const auto row1 = read_console_output_char(comm, state, host_io, info, COORD{ 0, 0 }, 92);
        const auto row2 = read_console_output_char(comm, state, host_io, info, COORD{ 0, 1 }, 93);
        const auto row3 = read_console_output_char(comm, state, host_io, info, COORD{ 0, 2 }, 94);
        const auto row4 = read_console_output_char(comm, state, host_io, info, COORD{ 0, 3 }, 95);
        const auto row5 = read_console_output_char(comm, state, host_io, info, COORD{ 0, 4 }, 96);

        return row1 && row2 && row3 && row4 && row5 &&
               row1.value() == L'A' &&
               row2.value() == L'C' &&
               row3.value() == L'D' &&
               row4.value() == L' ' &&
               row5.value() == L'E';
    }

    bool test_write_console_vt_su_sd_scrolls_within_margins()
    {
        MemoryComm comm{};
        oc::condrv::ServerState state{};
        TestHostIo host_io{};

        auto connect_packet = make_connect_packet(20023, 20024);
        oc::condrv::BasicApiMessage<MemoryComm> connect_message(comm, connect_packet);
        auto connect_outcome = oc::condrv::dispatch_message(state, connect_message, host_io);
        if (!connect_outcome)
        {
            return false;
        }

        state.set_output_mode(
            ENABLE_PROCESSED_OUTPUT |
            ENABLE_WRAP_AT_EOL_OUTPUT |
            ENABLE_VIRTUAL_TERMINAL_PROCESSING);

        const auto info = unpack_connection_information(connect_message.completion());

        if (!write_console_user_defined_a(comm, state, host_io, info, "\x1b[2J\x1b[2;1HB\x1b[3;1HC\x1b[4;1HD", 97))
        {
            return false;
        }

        if (!write_console_user_defined_a(comm, state, host_io, info, "\x1b[2;4r", 98))
        {
            return false;
        }

        // Scroll up: rows 2-4 become C, D, blank.
        if (!write_console_user_defined_a(comm, state, host_io, info, "\x1b[1S", 99))
        {
            return false;
        }

        const auto up_row2 = read_console_output_char(comm, state, host_io, info, COORD{ 0, 1 }, 100);
        const auto up_row3 = read_console_output_char(comm, state, host_io, info, COORD{ 0, 2 }, 101);
        const auto up_row4 = read_console_output_char(comm, state, host_io, info, COORD{ 0, 3 }, 102);
        if (!up_row2 || !up_row3 || !up_row4 || up_row2.value() != L'C' || up_row3.value() != L'D' || up_row4.value() != L' ')
        {
            return false;
        }

        // Scroll down: rows 2-4 become blank, C, D.
        if (!write_console_user_defined_a(comm, state, host_io, info, "\x1b[1T", 103))
        {
            return false;
        }

        const auto down_row2 = read_console_output_char(comm, state, host_io, info, COORD{ 0, 1 }, 104);
        const auto down_row3 = read_console_output_char(comm, state, host_io, info, COORD{ 0, 2 }, 105);
        const auto down_row4 = read_console_output_char(comm, state, host_io, info, COORD{ 0, 3 }, 106);
        return down_row2 && down_row3 && down_row4 &&
               down_row2.value() == L' ' &&
               down_row3.value() == L'C' &&
               down_row4.value() == L'D';
    }

    bool test_write_console_vt_il_inserts_lines_within_margins()
    {
        MemoryComm comm{};
        oc::condrv::ServerState state{};
        TestHostIo host_io{};

        auto connect_packet = make_connect_packet(20025, 20026);
        oc::condrv::BasicApiMessage<MemoryComm> connect_message(comm, connect_packet);
        auto connect_outcome = oc::condrv::dispatch_message(state, connect_message, host_io);
        if (!connect_outcome)
        {
            return false;
        }

        state.set_output_mode(
            ENABLE_PROCESSED_OUTPUT |
            ENABLE_WRAP_AT_EOL_OUTPUT |
            ENABLE_VIRTUAL_TERMINAL_PROCESSING);

        const auto info = unpack_connection_information(connect_message.completion());

        if (!write_console_user_defined_a(comm, state, host_io, info, "\x1b[2J\x1b[2;1HB\x1b[3;1HC\x1b[4;1HD", 107))
        {
            return false;
        }

        if (!write_console_user_defined_a(comm, state, host_io, info, "\x1b[2;4r\x1b[3;1H\x1b[1L", 108))
        {
            return false;
        }

        const auto row2 = read_console_output_char(comm, state, host_io, info, COORD{ 0, 1 }, 109);
        const auto row3 = read_console_output_char(comm, state, host_io, info, COORD{ 0, 2 }, 110);
        const auto row4 = read_console_output_char(comm, state, host_io, info, COORD{ 0, 3 }, 111);
        return row2 && row3 && row4 && row2.value() == L'B' && row3.value() == L' ' && row4.value() == L'C';
    }

    bool test_write_console_vt_dl_deletes_lines_within_margins()
    {
        MemoryComm comm{};
        oc::condrv::ServerState state{};
        TestHostIo host_io{};

        auto connect_packet = make_connect_packet(20027, 20028);
        oc::condrv::BasicApiMessage<MemoryComm> connect_message(comm, connect_packet);
        auto connect_outcome = oc::condrv::dispatch_message(state, connect_message, host_io);
        if (!connect_outcome)
        {
            return false;
        }

        state.set_output_mode(
            ENABLE_PROCESSED_OUTPUT |
            ENABLE_WRAP_AT_EOL_OUTPUT |
            ENABLE_VIRTUAL_TERMINAL_PROCESSING);

        const auto info = unpack_connection_information(connect_message.completion());

        if (!write_console_user_defined_a(comm, state, host_io, info, "\x1b[2J\x1b[2;1HB\x1b[3;1HC\x1b[4;1HD", 112))
        {
            return false;
        }

        if (!write_console_user_defined_a(comm, state, host_io, info, "\x1b[2;4r\x1b[3;1H\x1b[1M", 113))
        {
            return false;
        }

        const auto row2 = read_console_output_char(comm, state, host_io, info, COORD{ 0, 1 }, 114);
        const auto row3 = read_console_output_char(comm, state, host_io, info, COORD{ 0, 2 }, 115);
        const auto row4 = read_console_output_char(comm, state, host_io, info, COORD{ 0, 3 }, 116);
        return row2 && row3 && row4 && row2.value() == L'B' && row3.value() == L'D' && row4.value() == L' ';
    }

    bool test_write_console_vt_ind_preserves_column()
    {
        MemoryComm comm{};
        oc::condrv::ServerState state{};
        TestHostIo host_io{};

        auto connect_packet = make_connect_packet(20029, 20030);
        oc::condrv::BasicApiMessage<MemoryComm> connect_message(comm, connect_packet);
        auto connect_outcome = oc::condrv::dispatch_message(state, connect_message, host_io);
        if (!connect_outcome)
        {
            return false;
        }

        state.set_output_mode(
            ENABLE_PROCESSED_OUTPUT |
            ENABLE_WRAP_AT_EOL_OUTPUT |
            ENABLE_VIRTUAL_TERMINAL_PROCESSING);

        const auto info = unpack_connection_information(connect_message.completion());

        if (!write_console_user_defined_a(comm, state, host_io, info, "\x1b[2J\x1b[2;3HA\x1b" "DB", 117))
        {
            return false;
        }

        // ESC D performs a line feed without a carriage return, so the cursor column is preserved.
        const auto col1 = read_console_output_char(comm, state, host_io, info, COORD{ 0, 2 }, 118);
        const auto col4 = read_console_output_char(comm, state, host_io, info, COORD{ 3, 2 }, 119);
        return col1 && col4 && col1.value() == L' ' && col4.value() == L'B';
    }

    bool test_write_console_vt_ich_inserts_characters_in_line()
    {
        MemoryComm comm{};
        oc::condrv::ServerState state{};
        TestHostIo host_io{};

        auto connect_packet = make_connect_packet(20031, 20032);
        oc::condrv::BasicApiMessage<MemoryComm> connect_message(comm, connect_packet);
        auto connect_outcome = oc::condrv::dispatch_message(state, connect_message, host_io);
        if (!connect_outcome)
        {
            return false;
        }

        state.set_output_mode(
            ENABLE_PROCESSED_OUTPUT |
            ENABLE_WRAP_AT_EOL_OUTPUT |
            ENABLE_VIRTUAL_TERMINAL_PROCESSING);

        const auto info = unpack_connection_information(connect_message.completion());

        if (!write_console_user_defined_a(comm, state, host_io, info, "\x1b[2J\x1b[1;1HABCDE\x1b[1;3H\x1b[2@", 120))
        {
            return false;
        }

        const auto c0 = read_console_output_char(comm, state, host_io, info, COORD{ 0, 0 }, 121);
        const auto c1 = read_console_output_char(comm, state, host_io, info, COORD{ 1, 0 }, 122);
        const auto c2 = read_console_output_char(comm, state, host_io, info, COORD{ 2, 0 }, 123);
        const auto c3 = read_console_output_char(comm, state, host_io, info, COORD{ 3, 0 }, 124);
        const auto c4 = read_console_output_char(comm, state, host_io, info, COORD{ 4, 0 }, 125);
        const auto c5 = read_console_output_char(comm, state, host_io, info, COORD{ 5, 0 }, 126);
        const auto c6 = read_console_output_char(comm, state, host_io, info, COORD{ 6, 0 }, 127);

        return c0 && c1 && c2 && c3 && c4 && c5 && c6 &&
               c0.value() == L'A' &&
               c1.value() == L'B' &&
               c2.value() == L' ' &&
               c3.value() == L' ' &&
               c4.value() == L'C' &&
               c5.value() == L'D' &&
               c6.value() == L'E';
    }

    bool test_write_console_vt_dch_deletes_characters_in_line()
    {
        MemoryComm comm{};
        oc::condrv::ServerState state{};
        TestHostIo host_io{};

        auto connect_packet = make_connect_packet(20033, 20034);
        oc::condrv::BasicApiMessage<MemoryComm> connect_message(comm, connect_packet);
        auto connect_outcome = oc::condrv::dispatch_message(state, connect_message, host_io);
        if (!connect_outcome)
        {
            return false;
        }

        state.set_output_mode(
            ENABLE_PROCESSED_OUTPUT |
            ENABLE_WRAP_AT_EOL_OUTPUT |
            ENABLE_VIRTUAL_TERMINAL_PROCESSING);

        const auto info = unpack_connection_information(connect_message.completion());

        if (!write_console_user_defined_a(comm, state, host_io, info, "\x1b[2J\x1b[1;1HABCDE\x1b[1;3H\x1b[2P", 128))
        {
            return false;
        }

        const auto c0 = read_console_output_char(comm, state, host_io, info, COORD{ 0, 0 }, 129);
        const auto c1 = read_console_output_char(comm, state, host_io, info, COORD{ 1, 0 }, 130);
        const auto c2 = read_console_output_char(comm, state, host_io, info, COORD{ 2, 0 }, 131);
        const auto c3 = read_console_output_char(comm, state, host_io, info, COORD{ 3, 0 }, 132);
        const auto c4 = read_console_output_char(comm, state, host_io, info, COORD{ 4, 0 }, 133);

        return c0 && c1 && c2 && c3 && c4 &&
               c0.value() == L'A' &&
               c1.value() == L'B' &&
               c2.value() == L'E' &&
               c3.value() == L' ' &&
               c4.value() == L' ';
    }

    bool test_write_console_vt_ech_erases_characters_in_line()
    {
        MemoryComm comm{};
        oc::condrv::ServerState state{};
        TestHostIo host_io{};

        auto connect_packet = make_connect_packet(20035, 20036);
        oc::condrv::BasicApiMessage<MemoryComm> connect_message(comm, connect_packet);
        auto connect_outcome = oc::condrv::dispatch_message(state, connect_message, host_io);
        if (!connect_outcome)
        {
            return false;
        }

        state.set_output_mode(
            ENABLE_PROCESSED_OUTPUT |
            ENABLE_WRAP_AT_EOL_OUTPUT |
            ENABLE_VIRTUAL_TERMINAL_PROCESSING);

        const auto info = unpack_connection_information(connect_message.completion());

        if (!write_console_user_defined_a(comm, state, host_io, info, "\x1b[2J\x1b[1;1HABCDE\x1b[1;2H\x1b[3X", 134))
        {
            return false;
        }

        const auto c0 = read_console_output_char(comm, state, host_io, info, COORD{ 0, 0 }, 135);
        const auto c1 = read_console_output_char(comm, state, host_io, info, COORD{ 1, 0 }, 136);
        const auto c2 = read_console_output_char(comm, state, host_io, info, COORD{ 2, 0 }, 137);
        const auto c3 = read_console_output_char(comm, state, host_io, info, COORD{ 3, 0 }, 138);
        const auto c4 = read_console_output_char(comm, state, host_io, info, COORD{ 4, 0 }, 139);

        return c0 && c1 && c2 && c3 && c4 &&
               c0.value() == L'A' &&
               c1.value() == L' ' &&
               c2.value() == L' ' &&
               c3.value() == L' ' &&
               c4.value() == L'E';
    }

    bool test_write_console_vt_irm_insert_mode_inserts_printable_cells()
    {
        MemoryComm comm{};
        oc::condrv::ServerState state{};
        TestHostIo host_io{};

        auto connect_packet = make_connect_packet(20037, 20038);
        oc::condrv::BasicApiMessage<MemoryComm> connect_message(comm, connect_packet);
        auto connect_outcome = oc::condrv::dispatch_message(state, connect_message, host_io);
        if (!connect_outcome)
        {
            return false;
        }

        state.set_output_mode(
            ENABLE_PROCESSED_OUTPUT |
            ENABLE_WRAP_AT_EOL_OUTPUT |
            ENABLE_VIRTUAL_TERMINAL_PROCESSING);

        const auto info = unpack_connection_information(connect_message.completion());

        // Enter insert mode (IRM), insert a character, then leave insert mode and overwrite.
        if (!write_console_user_defined_a(
                comm,
                state,
                host_io,
                info,
                "\x1b[2J\x1b[1;1HABCDEZ\x1b[1;3H\x1b[4hX",
                140))
        {
            return false;
        }

        const auto c0 = read_console_output_char(comm, state, host_io, info, COORD{ 0, 0 }, 141);
        const auto c1 = read_console_output_char(comm, state, host_io, info, COORD{ 1, 0 }, 142);
        const auto c2 = read_console_output_char(comm, state, host_io, info, COORD{ 2, 0 }, 143);
        const auto c3 = read_console_output_char(comm, state, host_io, info, COORD{ 3, 0 }, 144);
        const auto c4 = read_console_output_char(comm, state, host_io, info, COORD{ 4, 0 }, 145);
        const auto c5 = read_console_output_char(comm, state, host_io, info, COORD{ 5, 0 }, 146);
        const auto c6 = read_console_output_char(comm, state, host_io, info, COORD{ 6, 0 }, 147);

        if (!(c0 && c1 && c2 && c3 && c4 && c5 && c6 &&
              c0.value() == L'A' &&
              c1.value() == L'B' &&
              c2.value() == L'X' &&
              c3.value() == L'C' &&
              c4.value() == L'D' &&
              c5.value() == L'E' &&
              c6.value() == L'Z'))
        {
            return false;
        }

        if (!write_console_user_defined_a(comm, state, host_io, info, "\x1b[4l\x1b[1;3HY", 148))
        {
            return false;
        }

        const auto y0 = read_console_output_char(comm, state, host_io, info, COORD{ 0, 0 }, 149);
        const auto y1 = read_console_output_char(comm, state, host_io, info, COORD{ 1, 0 }, 150);
        const auto y2 = read_console_output_char(comm, state, host_io, info, COORD{ 2, 0 }, 151);
        const auto y3 = read_console_output_char(comm, state, host_io, info, COORD{ 3, 0 }, 152);
        const auto y4 = read_console_output_char(comm, state, host_io, info, COORD{ 4, 0 }, 153);
        const auto y5 = read_console_output_char(comm, state, host_io, info, COORD{ 5, 0 }, 154);
        const auto y6 = read_console_output_char(comm, state, host_io, info, COORD{ 6, 0 }, 155);

        return y0 && y1 && y2 && y3 && y4 && y5 && y6 &&
               y0.value() == L'A' &&
               y1.value() == L'B' &&
               y2.value() == L'Y' &&
               y3.value() == L'C' &&
               y4.value() == L'D' &&
               y5.value() == L'E' &&
               y6.value() == L'Z';
    }

    bool test_write_console_vt_cuu_clamps_within_decstbm_when_origin_mode_disabled()
    {
        MemoryComm comm{};
        oc::condrv::ServerState state{};
        TestHostIo host_io{};

        auto connect_packet = make_connect_packet(20039, 20040);
        oc::condrv::BasicApiMessage<MemoryComm> connect_message(comm, connect_packet);
        auto connect_outcome = oc::condrv::dispatch_message(state, connect_message, host_io);
        if (!connect_outcome)
        {
            return false;
        }

        state.set_output_mode(
            ENABLE_PROCESSED_OUTPUT |
            ENABLE_WRAP_AT_EOL_OUTPUT |
            ENABLE_VIRTUAL_TERMINAL_PROCESSING);

        const auto info = unpack_connection_information(connect_message.completion());
        if (!set_screen_buffer_size_user_defined(comm, state, host_io, info, COORD{ 10, 5 }, 156))
        {
            return false;
        }

        if (!write_console_user_defined_a(
                comm,
                state,
                host_io,
                info,
                "\x1b[2J"
                "\x1b[2;4r"
                "\x1b[4;1H"
                "\x1b[10A"
                "X",
                157))
        {
            return false;
        }

        // With DECSTBM set, CUU should clamp at the top margin when the cursor starts inside the region.
        const auto top_row = read_console_output_char(comm, state, host_io, info, COORD{ 0, 0 }, 158);
        const auto expected = read_console_output_char(comm, state, host_io, info, COORD{ 0, 1 }, 159);
        return top_row && expected && top_row.value() == L' ' && expected.value() == L'X';
    }

    bool test_write_console_vt_cud_clamps_within_decstbm_when_origin_mode_disabled()
    {
        MemoryComm comm{};
        oc::condrv::ServerState state{};
        TestHostIo host_io{};

        auto connect_packet = make_connect_packet(20041, 20042);
        oc::condrv::BasicApiMessage<MemoryComm> connect_message(comm, connect_packet);
        auto connect_outcome = oc::condrv::dispatch_message(state, connect_message, host_io);
        if (!connect_outcome)
        {
            return false;
        }

        state.set_output_mode(
            ENABLE_PROCESSED_OUTPUT |
            ENABLE_WRAP_AT_EOL_OUTPUT |
            ENABLE_VIRTUAL_TERMINAL_PROCESSING);

        const auto info = unpack_connection_information(connect_message.completion());
        if (!set_screen_buffer_size_user_defined(comm, state, host_io, info, COORD{ 10, 5 }, 160))
        {
            return false;
        }

        if (!write_console_user_defined_a(
                comm,
                state,
                host_io,
                info,
                "\x1b[2J"
                "\x1b[2;4r"
                "\x1b[2;1H"
                "\x1b[10B"
                "Y",
                161))
        {
            return false;
        }

        // With DECSTBM set, CUD should clamp at the bottom margin when the cursor starts inside the region.
        const auto bottom_margin = read_console_output_char(comm, state, host_io, info, COORD{ 0, 3 }, 162);
        const auto below_margins = read_console_output_char(comm, state, host_io, info, COORD{ 0, 4 }, 163);
        return bottom_margin && below_margins && bottom_margin.value() == L'Y' && below_margins.value() == L' ';
    }

    bool test_write_console_vt_cnl_moves_to_column_one_and_respects_decstbm_margins()
    {
        MemoryComm comm{};
        oc::condrv::ServerState state{};
        TestHostIo host_io{};

        auto connect_packet = make_connect_packet(20043, 20044);
        oc::condrv::BasicApiMessage<MemoryComm> connect_message(comm, connect_packet);
        auto connect_outcome = oc::condrv::dispatch_message(state, connect_message, host_io);
        if (!connect_outcome)
        {
            return false;
        }

        state.set_output_mode(
            ENABLE_PROCESSED_OUTPUT |
            ENABLE_WRAP_AT_EOL_OUTPUT |
            ENABLE_VIRTUAL_TERMINAL_PROCESSING);

        const auto info = unpack_connection_information(connect_message.completion());
        if (!set_screen_buffer_size_user_defined(comm, state, host_io, info, COORD{ 10, 5 }, 164))
        {
            return false;
        }

        if (!write_console_user_defined_a(
                comm,
                state,
                host_io,
                info,
                "\x1b[2J"
                "\x1b[2;4r"
                "\x1b[3;5H"
                "\x1b[1E"
                "Z",
                165))
        {
            return false;
        }

        const auto original_position = read_console_output_char(comm, state, host_io, info, COORD{ 4, 2 }, 166);
        const auto expected = read_console_output_char(comm, state, host_io, info, COORD{ 0, 3 }, 167);
        return original_position && expected && original_position.value() == L' ' && expected.value() == L'Z';
    }

    bool test_write_console_vt_cpl_moves_to_column_one_and_respects_decstbm_margins()
    {
        MemoryComm comm{};
        oc::condrv::ServerState state{};
        TestHostIo host_io{};

        auto connect_packet = make_connect_packet(20045, 20046);
        oc::condrv::BasicApiMessage<MemoryComm> connect_message(comm, connect_packet);
        auto connect_outcome = oc::condrv::dispatch_message(state, connect_message, host_io);
        if (!connect_outcome)
        {
            return false;
        }

        state.set_output_mode(
            ENABLE_PROCESSED_OUTPUT |
            ENABLE_WRAP_AT_EOL_OUTPUT |
            ENABLE_VIRTUAL_TERMINAL_PROCESSING);

        const auto info = unpack_connection_information(connect_message.completion());
        if (!set_screen_buffer_size_user_defined(comm, state, host_io, info, COORD{ 10, 5 }, 168))
        {
            return false;
        }

        if (!write_console_user_defined_a(
                comm,
                state,
                host_io,
                info,
                "\x1b[2J"
                "\x1b[2;4r"
                "\x1b[2;5H"
                "\x1b[1F"
                "W",
                169))
        {
            return false;
        }

        const auto above_margin = read_console_output_char(comm, state, host_io, info, COORD{ 0, 0 }, 170);
        const auto expected = read_console_output_char(comm, state, host_io, info, COORD{ 0, 1 }, 171);
        return above_margin && expected && above_margin.value() == L' ' && expected.value() == L'W';
    }

    bool test_write_console_vt_decstr_soft_reset_disables_irm()
    {
        MemoryComm comm{};
        oc::condrv::ServerState state{};
        TestHostIo host_io{};

        auto connect_packet = make_connect_packet(20047, 20048);
        oc::condrv::BasicApiMessage<MemoryComm> connect_message(comm, connect_packet);
        auto connect_outcome = oc::condrv::dispatch_message(state, connect_message, host_io);
        if (!connect_outcome)
        {
            return false;
        }

        state.set_output_mode(
            ENABLE_PROCESSED_OUTPUT |
            ENABLE_WRAP_AT_EOL_OUTPUT |
            ENABLE_VIRTUAL_TERMINAL_PROCESSING);

        const auto info = unpack_connection_information(connect_message.completion());
        if (!set_screen_buffer_size_user_defined(comm, state, host_io, info, COORD{ 7, 3 }, 172))
        {
            return false;
        }

        // Enable IRM, then soft reset (DECSTR). After the reset, output should be in replace mode.
        if (!write_console_user_defined_a(
                comm,
                state,
                host_io,
                info,
                "\x1b[2J"
                "\x1b[1;1H"
                "ABCDEZ"
                "\x1b[1;3H"
                "\x1b[4h"
                "\x1b[!p"
                "X",
                173))
        {
            return false;
        }

        const auto col4 = read_console_output_char(comm, state, host_io, info, COORD{ 3, 0 }, 174);
        return col4 && col4.value() == L'D';
    }

    bool test_write_console_vt_decstr_soft_reset_resets_saved_cursor_state_to_home()
    {
        MemoryComm comm{};
        oc::condrv::ServerState state{};
        TestHostIo host_io{};

        auto connect_packet = make_connect_packet(20049, 20050);
        oc::condrv::BasicApiMessage<MemoryComm> connect_message(comm, connect_packet);
        auto connect_outcome = oc::condrv::dispatch_message(state, connect_message, host_io);
        if (!connect_outcome)
        {
            return false;
        }

        state.set_output_mode(
            ENABLE_PROCESSED_OUTPUT |
            ENABLE_WRAP_AT_EOL_OUTPUT |
            ENABLE_VIRTUAL_TERMINAL_PROCESSING);

        const auto info = unpack_connection_information(connect_message.completion());
        if (!set_screen_buffer_size_user_defined(comm, state, host_io, info, COORD{ 10, 3 }, 175))
        {
            return false;
        }

        if (!write_console_user_defined_a(
                comm,
                state,
                host_io,
                info,
                "\x1b[2J"
                "\x1b[1;6H"
                "\x1b" "7"
                "\x1b[1;1H"
                "\x1b[!p"
                "\x1b" "8"
                "Q",
                176))
        {
            return false;
        }

        const auto home = read_console_output_char(comm, state, host_io, info, COORD{ 0, 0 }, 177);
        const auto old_saved = read_console_output_char(comm, state, host_io, info, COORD{ 5, 0 }, 178);
        return home && old_saved && home.value() == L'Q' && old_saved.value() == L' ';
    }

    bool test_write_console_vt_ris_hard_reset_clears_screen_and_homes_cursor()
    {
        MemoryComm comm{};
        oc::condrv::ServerState state{};
        TestHostIo host_io{};

        auto connect_packet = make_connect_packet(20051, 20052);
        oc::condrv::BasicApiMessage<MemoryComm> connect_message(comm, connect_packet);
        auto connect_outcome = oc::condrv::dispatch_message(state, connect_message, host_io);
        if (!connect_outcome)
        {
            return false;
        }

        state.set_output_mode(
            ENABLE_PROCESSED_OUTPUT |
            ENABLE_WRAP_AT_EOL_OUTPUT |
            ENABLE_VIRTUAL_TERMINAL_PROCESSING);

        const auto info = unpack_connection_information(connect_message.completion());
        if (!set_screen_buffer_size_user_defined(comm, state, host_io, info, COORD{ 10, 5 }, 179))
        {
            return false;
        }

        if (!write_console_user_defined_a(
                comm,
                state,
                host_io,
                info,
                "\x1b[2J"
                "\x1b[2;3H"
                "A"
                "\x1b" "c"
                "B",
                180))
        {
            return false;
        }

        const auto home = read_console_output_char(comm, state, host_io, info, COORD{ 0, 0 }, 181);
        const auto cleared = read_console_output_char(comm, state, host_io, info, COORD{ 2, 1 }, 182);
        return home && cleared && home.value() == L'B' && cleared.value() == L' ';
    }

    bool test_user_defined_read_console_a_writes_after_descriptor_offset()
    {
        MemoryComm comm{};
        oc::condrv::ServerState state{};
        TestHostIo host_io{};

        auto connect_packet = make_connect_packet(9001, 9002);
        oc::condrv::BasicApiMessage<MemoryComm> connect_message(comm, connect_packet);
        auto connect_outcome = oc::condrv::dispatch_message(state, connect_message, host_io);
        if (!connect_outcome)
        {
            return false;
        }

        auto info = unpack_connection_information(connect_message.completion());
        state.set_input_mode(0); // raw ReadConsole behavior (no line buffering)
        host_io.input = { static_cast<std::byte>('o'), static_cast<std::byte>('k') };

        constexpr ULONG api_size = sizeof(CONSOLE_READCONSOLE_MSG);
        constexpr ULONG header_size = sizeof(CONSOLE_MSG_HEADER);
        constexpr ULONG read_offset = api_size + header_size;

        oc::condrv::IoPacket packet{};
        packet.payload.user_defined = oc::condrv::UserDefinedPacket{};
        packet.descriptor.identifier.LowPart = 7;
        packet.descriptor.function = oc::condrv::console_io_user_defined;
        packet.descriptor.process = info.process;
        packet.descriptor.object = info.input;
        packet.descriptor.input_size = read_offset;
        packet.descriptor.output_size = api_size + 16;
        packet.payload.user_defined.msg_header.ApiNumber = static_cast<ULONG>(ConsolepReadConsole);
        packet.payload.user_defined.msg_header.ApiDescriptorSize = api_size;
        packet.payload.user_defined.u.console_msg_l1.ReadConsole.Unicode = FALSE;

        oc::condrv::BasicApiMessage<MemoryComm> message(comm, packet);
        auto outcome = oc::condrv::dispatch_message(state, message, host_io);
        if (!outcome)
        {
            return false;
        }

        if (message.completion().io_status.Status != oc::core::status_success)
        {
            return false;
        }

        if (message.packet().payload.user_defined.u.console_msg_l1.ReadConsole.NumBytes != host_io.input.size())
        {
            return false;
        }

        if (auto released = message.release_message_buffers(); !released)
        {
            return false;
        }

        const size_t expected_size = api_size + host_io.input.size();
        if (comm.output.size() != expected_size)
        {
            return false;
        }

        return std::memcmp(comm.output.data() + api_size, host_io.input.data(), host_io.input.size()) == 0;
    }

    bool test_user_defined_read_console_w_widens_bytes()
    {
        MemoryComm comm{};
        oc::condrv::ServerState state{};
        TestHostIo host_io{};

        auto connect_packet = make_connect_packet(9101, 9102);
        oc::condrv::BasicApiMessage<MemoryComm> connect_message(comm, connect_packet);
        auto connect_outcome = oc::condrv::dispatch_message(state, connect_message, host_io);
        if (!connect_outcome)
        {
            return false;
        }

        auto info = unpack_connection_information(connect_message.completion());
        state.set_input_mode(0); // raw ReadConsole behavior (no line buffering)
        host_io.input = { static_cast<std::byte>('A') };

        constexpr ULONG api_size = sizeof(CONSOLE_READCONSOLE_MSG);
        constexpr ULONG header_size = sizeof(CONSOLE_MSG_HEADER);
        constexpr ULONG read_offset = api_size + header_size;

        oc::condrv::IoPacket packet{};
        packet.payload.user_defined = oc::condrv::UserDefinedPacket{};
        packet.descriptor.identifier.LowPart = 8;
        packet.descriptor.function = oc::condrv::console_io_user_defined;
        packet.descriptor.process = info.process;
        packet.descriptor.object = info.input;
        packet.descriptor.input_size = read_offset;
        packet.descriptor.output_size = api_size + 16;
        packet.payload.user_defined.msg_header.ApiNumber = static_cast<ULONG>(ConsolepReadConsole);
        packet.payload.user_defined.msg_header.ApiDescriptorSize = api_size;
        packet.payload.user_defined.u.console_msg_l1.ReadConsole.Unicode = TRUE;

        oc::condrv::BasicApiMessage<MemoryComm> message(comm, packet);
        auto outcome = oc::condrv::dispatch_message(state, message, host_io);
        if (!outcome)
        {
            return false;
        }

        if (message.completion().io_status.Status != oc::core::status_success)
        {
            return false;
        }

        if (message.packet().payload.user_defined.u.console_msg_l1.ReadConsole.NumBytes != sizeof(wchar_t))
        {
            return false;
        }

        if (auto released = message.release_message_buffers(); !released)
        {
            return false;
        }

        if (comm.output.size() != api_size + sizeof(wchar_t))
        {
            return false;
        }

        wchar_t value = 0;
        std::memcpy(&value, comm.output.data() + api_size, sizeof(value));
        return value == L'A';
    }

    bool test_user_defined_read_console_w_decodes_utf8_bytes()
    {
        MemoryComm comm{};
        oc::condrv::ServerState state{};
        TestHostIo host_io{};

        auto connect_packet = make_connect_packet(9103, 9104);
        oc::condrv::BasicApiMessage<MemoryComm> connect_message(comm, connect_packet);
        auto connect_outcome = oc::condrv::dispatch_message(state, connect_message, host_io);
        if (!connect_outcome)
        {
            return false;
        }

        const auto info = unpack_connection_information(connect_message.completion());
        state.set_input_code_page(CP_UTF8);

        state.set_input_mode(0); // raw ReadConsole behavior (no line buffering)
        host_io.input = { static_cast<std::byte>(0xC3), static_cast<std::byte>(0xA9) }; // U+00E9

        constexpr ULONG api_size = sizeof(CONSOLE_READCONSOLE_MSG);
        constexpr ULONG header_size = sizeof(CONSOLE_MSG_HEADER);
        constexpr ULONG read_offset = api_size + header_size;

        oc::condrv::IoPacket packet{};
        packet.payload.user_defined = oc::condrv::UserDefinedPacket{};
        packet.descriptor.identifier.LowPart = 9;
        packet.descriptor.function = oc::condrv::console_io_user_defined;
        packet.descriptor.process = info.process;
        packet.descriptor.object = info.input;
        packet.descriptor.input_size = read_offset;
        packet.descriptor.output_size = api_size + 16;
        packet.payload.user_defined.msg_header.ApiNumber = static_cast<ULONG>(ConsolepReadConsole);
        packet.payload.user_defined.msg_header.ApiDescriptorSize = api_size;
        packet.payload.user_defined.u.console_msg_l1.ReadConsole.Unicode = TRUE;

        oc::condrv::BasicApiMessage<MemoryComm> message(comm, packet);
        auto outcome = oc::condrv::dispatch_message(state, message, host_io);
        if (!outcome)
        {
            return false;
        }

        if (message.completion().io_status.Status != oc::core::status_success)
        {
            return false;
        }

        if (message.packet().payload.user_defined.u.console_msg_l1.ReadConsole.NumBytes != sizeof(wchar_t))
        {
            return false;
        }

        if (auto released = message.release_message_buffers(); !released)
        {
            return false;
        }

        if (comm.output.size() != api_size + sizeof(wchar_t))
        {
            return false;
        }

        wchar_t value = 0;
        std::memcpy(&value, comm.output.data() + api_size, sizeof(value));
        return value == static_cast<wchar_t>(0x00E9) && host_io.input_bytes_available() == 0;
    }

    bool test_user_defined_read_console_w_surrogate_pair_splits_across_reads()
    {
        MemoryComm comm{};
        oc::condrv::ServerState state{};
        TestHostIo host_io{};

        auto connect_packet = make_connect_packet(9105, 9106);
        oc::condrv::BasicApiMessage<MemoryComm> connect_message(comm, connect_packet);
        auto connect_outcome = oc::condrv::dispatch_message(state, connect_message, host_io);
        if (!connect_outcome)
        {
            return false;
        }

        const auto info = unpack_connection_information(connect_message.completion());
        state.set_input_code_page(CP_UTF8);
        state.set_input_mode(0); // raw ReadConsole behavior (no line buffering)

        // U+1F600 GRINNING FACE: UTF-8 F0 9F 98 80, UTF-16 D83D DE00.
        host_io.input = {
            static_cast<std::byte>(0xF0),
            static_cast<std::byte>(0x9F),
            static_cast<std::byte>(0x98),
            static_cast<std::byte>(0x80),
        };

        constexpr ULONG api_size = sizeof(CONSOLE_READCONSOLE_MSG);
        constexpr ULONG header_size = sizeof(CONSOLE_MSG_HEADER);
        constexpr ULONG read_offset = api_size + header_size;

        const auto make_packet = [&](const ULONG identifier) {
            oc::condrv::IoPacket packet{};
            packet.payload.user_defined = oc::condrv::UserDefinedPacket{};
            packet.descriptor.identifier.LowPart = identifier;
            packet.descriptor.function = oc::condrv::console_io_user_defined;
            packet.descriptor.process = info.process;
            packet.descriptor.object = info.input;
            packet.descriptor.input_size = read_offset;
            packet.descriptor.output_size = api_size + static_cast<ULONG>(sizeof(wchar_t)); // room for 1 unit
            packet.payload.user_defined.msg_header.ApiNumber = static_cast<ULONG>(ConsolepReadConsole);
            packet.payload.user_defined.msg_header.ApiDescriptorSize = api_size;
            packet.payload.user_defined.u.console_msg_l1.ReadConsole.Unicode = TRUE;
            return packet;
        };

        // First call returns the high surrogate and consumes the UTF-8 bytes.
        {
            comm.output.clear();
            oc::condrv::BasicApiMessage<MemoryComm> message(comm, make_packet(10));
            auto outcome = oc::condrv::dispatch_message(state, message, host_io);
            if (!outcome || message.completion().io_status.Status != oc::core::status_success)
            {
                return false;
            }

            if (message.packet().payload.user_defined.u.console_msg_l1.ReadConsole.NumBytes != sizeof(wchar_t))
            {
                return false;
            }

            if (auto released = message.release_message_buffers(); !released)
            {
                return false;
            }

            if (comm.output.size() != api_size + sizeof(wchar_t))
            {
                return false;
            }

            wchar_t returned = 0;
            std::memcpy(&returned, comm.output.data() + api_size, sizeof(returned));
            if (returned != static_cast<wchar_t>(0xD83D))
            {
                return false;
            }

            if (host_io.input_bytes_available() != 0)
            {
                return false;
            }
        }

        // Second call returns the pending low surrogate without blocking.
        {
            comm.output.clear();
            oc::condrv::BasicApiMessage<MemoryComm> message(comm, make_packet(11));
            auto outcome = oc::condrv::dispatch_message(state, message, host_io);
            if (!outcome || message.completion().io_status.Status != oc::core::status_success)
            {
                return false;
            }

            if (message.packet().payload.user_defined.u.console_msg_l1.ReadConsole.NumBytes != sizeof(wchar_t))
            {
                return false;
            }

            if (auto released = message.release_message_buffers(); !released)
            {
                return false;
            }

            if (comm.output.size() != api_size + sizeof(wchar_t))
            {
                return false;
            }

            wchar_t returned = 0;
            std::memcpy(&returned, comm.output.data() + api_size, sizeof(returned));
            return returned == static_cast<wchar_t>(0xDE00) && host_io.input_bytes_available() == 0;
        }
    }

    bool test_user_defined_read_console_w_raw_processed_input_skips_ctrl_c_mid_buffer_and_sends_end_task()
    {
        MemoryComm comm{};
        oc::condrv::ServerState state{};
        TestHostIo host_io{};

        auto connect_packet = make_connect_packet(9193, 9194);
        oc::condrv::BasicApiMessage<MemoryComm> connect_message(comm, connect_packet);
        auto connect_outcome = oc::condrv::dispatch_message(state, connect_message, host_io);
        if (!connect_outcome)
        {
            return false;
        }

        const auto info = unpack_connection_information(connect_message.completion());
        state.set_input_code_page(CP_UTF8);
        state.set_input_mode(ENABLE_PROCESSED_INPUT); // raw ReadConsole behavior, but processed input enabled.
        host_io.input = { static_cast<std::byte>('X'), static_cast<std::byte>(0x03), static_cast<std::byte>('Y') };

        constexpr ULONG api_size = sizeof(CONSOLE_READCONSOLE_MSG);
        constexpr ULONG header_size = sizeof(CONSOLE_MSG_HEADER);
        constexpr ULONG read_offset = api_size + header_size;
        constexpr ULONG output_bytes = 2 * sizeof(wchar_t);

        oc::condrv::IoPacket packet{};
        packet.payload.user_defined = oc::condrv::UserDefinedPacket{};
        packet.descriptor.identifier.LowPart = 90;
        packet.descriptor.function = oc::condrv::console_io_user_defined;
        packet.descriptor.process = info.process;
        packet.descriptor.object = info.input;
        packet.descriptor.input_size = read_offset;
        packet.descriptor.output_size = api_size + output_bytes;
        packet.payload.user_defined.msg_header.ApiNumber = static_cast<ULONG>(ConsolepReadConsole);
        packet.payload.user_defined.msg_header.ApiDescriptorSize = api_size;
        packet.payload.user_defined.u.console_msg_l1.ReadConsole.Unicode = TRUE;

        oc::condrv::BasicApiMessage<MemoryComm> message(comm, packet);
        auto outcome = oc::condrv::dispatch_message(state, message, host_io);
        if (!outcome || message.completion().io_status.Status != oc::core::status_success)
        {
            return false;
        }

        if (message.packet().payload.user_defined.u.console_msg_l1.ReadConsole.NumBytes != output_bytes)
        {
            return false;
        }

        if (host_io.end_task_pids.size() != 1 || host_io.end_task_pids[0] != 9193)
        {
            return false;
        }

        if (auto released = message.release_message_buffers(); !released)
        {
            return false;
        }

        if (comm.output.size() != api_size + output_bytes)
        {
            return false;
        }

        std::array<wchar_t, 2> value{};
        std::memcpy(value.data(), comm.output.data() + api_size, output_bytes);
        return value[0] == L'X' && value[1] == L'Y' && host_io.input_bytes_available() == 0;
    }

    bool test_user_defined_read_console_w_raw_processed_input_ctrl_break_returns_alerted_and_flushes_input()
    {
        MemoryComm comm{};
        oc::condrv::ServerState state{};
        TestHostIo host_io{};

        auto connect_packet = make_connect_packet(9195, 9196);
        oc::condrv::BasicApiMessage<MemoryComm> connect_message(comm, connect_packet);
        auto connect_outcome = oc::condrv::dispatch_message(state, connect_message, host_io);
        if (!connect_outcome)
        {
            return false;
        }

        const auto info = unpack_connection_information(connect_message.completion());
        state.set_input_code_page(CP_UTF8);
        state.set_input_mode(ENABLE_PROCESSED_INPUT); // raw ReadConsole behavior, but processed input enabled.

        constexpr std::string_view ctrl_break = "\x1b[3;0;0;1;8;1_";
        constexpr std::string_view tail = "Z";

        host_io.input.clear();
        host_io.input_offset = 0;
        for (const unsigned char ch : ctrl_break)
        {
            host_io.input.push_back(static_cast<std::byte>(ch));
        }
        for (const unsigned char ch : tail)
        {
            host_io.input.push_back(static_cast<std::byte>(ch));
        }

        constexpr ULONG api_size = sizeof(CONSOLE_READCONSOLE_MSG);
        constexpr ULONG header_size = sizeof(CONSOLE_MSG_HEADER);
        constexpr ULONG read_offset = api_size + header_size;

        oc::condrv::IoPacket packet{};
        packet.payload.user_defined = oc::condrv::UserDefinedPacket{};
        packet.descriptor.identifier.LowPart = 92;
        packet.descriptor.function = oc::condrv::console_io_user_defined;
        packet.descriptor.process = info.process;
        packet.descriptor.object = info.input;
        packet.descriptor.input_size = read_offset;
        packet.descriptor.output_size = api_size + 16;
        packet.payload.user_defined.msg_header.ApiNumber = static_cast<ULONG>(ConsolepReadConsole);
        packet.payload.user_defined.msg_header.ApiDescriptorSize = api_size;
        packet.payload.user_defined.u.console_msg_l1.ReadConsole.Unicode = TRUE;

        comm.input.assign(read_offset, std::byte{});
        comm.output.clear();

        oc::condrv::BasicApiMessage<MemoryComm> message(comm, packet);
        auto outcome = oc::condrv::dispatch_message(state, message, host_io);
        if (!outcome || outcome->reply_pending || message.completion().io_status.Status != oc::core::status_alerted)
        {
            return false;
        }

        if (message.packet().payload.user_defined.u.console_msg_l1.ReadConsole.NumBytes != 0)
        {
            return false;
        }

        if (host_io.end_task_pids.size() != 1 || host_io.end_task_pids[0] != 9195)
        {
            return false;
        }

        if (host_io.end_task_events.size() != 1 || host_io.end_task_events[0] != CTRL_BREAK_EVENT)
        {
            return false;
        }

        if (host_io.end_task_flags.size() != 1 || host_io.end_task_flags[0] != oc::core::console_ctrl_break_flag)
        {
            return false;
        }

        if (auto released = message.release_message_buffers(); !released)
        {
            return false;
        }

        return comm.output.size() == api_size && host_io.input_bytes_available() == 0;
    }

    bool test_user_defined_read_console_a_raw_processed_input_consumes_ctrl_c_and_sends_end_task()
    {
        MemoryComm comm{};
        oc::condrv::ServerState state{};
        TestHostIo host_io{};

        auto connect_packet = make_connect_packet(9191, 9192);
        oc::condrv::BasicApiMessage<MemoryComm> connect_message(comm, connect_packet);
        auto connect_outcome = oc::condrv::dispatch_message(state, connect_message, host_io);
        if (!connect_outcome)
        {
            return false;
        }

        const auto info = unpack_connection_information(connect_message.completion());
        state.set_input_mode(ENABLE_PROCESSED_INPUT); // raw ReadConsole behavior, but processed input enabled.
        host_io.input = { static_cast<std::byte>(0x03), static_cast<std::byte>('Z') };

        constexpr ULONG api_size = sizeof(CONSOLE_READCONSOLE_MSG);
        constexpr ULONG header_size = sizeof(CONSOLE_MSG_HEADER);
        constexpr ULONG read_offset = api_size + header_size;

        oc::condrv::IoPacket packet{};
        packet.payload.user_defined = oc::condrv::UserDefinedPacket{};
        packet.descriptor.identifier.LowPart = 89;
        packet.descriptor.function = oc::condrv::console_io_user_defined;
        packet.descriptor.process = info.process;
        packet.descriptor.object = info.input;
        packet.descriptor.input_size = read_offset;
        packet.descriptor.output_size = api_size + 16;
        packet.payload.user_defined.msg_header.ApiNumber = static_cast<ULONG>(ConsolepReadConsole);
        packet.payload.user_defined.msg_header.ApiDescriptorSize = api_size;
        packet.payload.user_defined.u.console_msg_l1.ReadConsole.Unicode = FALSE;

        oc::condrv::BasicApiMessage<MemoryComm> message(comm, packet);
        auto outcome = oc::condrv::dispatch_message(state, message, host_io);
        if (!outcome || message.completion().io_status.Status != oc::core::status_success)
        {
            return false;
        }

        if (message.packet().payload.user_defined.u.console_msg_l1.ReadConsole.NumBytes != 1)
        {
            return false;
        }

        if (host_io.end_task_pids.size() != 1 || host_io.end_task_pids[0] != 9191)
        {
            return false;
        }

        if (auto released = message.release_message_buffers(); !released)
        {
            return false;
        }

        if (comm.output.size() != api_size + 1)
        {
            return false;
        }

        return comm.output[api_size] == static_cast<std::byte>('Z') && host_io.input_bytes_available() == 0;
    }

    bool test_user_defined_read_console_a_raw_processed_input_skips_ctrl_c_mid_buffer_and_sends_end_task()
    {
        MemoryComm comm{};
        oc::condrv::ServerState state{};
        TestHostIo host_io{};

        auto connect_packet = make_connect_packet(9195, 9196);
        oc::condrv::BasicApiMessage<MemoryComm> connect_message(comm, connect_packet);
        auto connect_outcome = oc::condrv::dispatch_message(state, connect_message, host_io);
        if (!connect_outcome)
        {
            return false;
        }

        const auto info = unpack_connection_information(connect_message.completion());
        state.set_input_mode(ENABLE_PROCESSED_INPUT); // raw ReadConsole behavior, but processed input enabled.
        host_io.input = { static_cast<std::byte>('X'), static_cast<std::byte>(0x03), static_cast<std::byte>('Y') };

        constexpr ULONG api_size = sizeof(CONSOLE_READCONSOLE_MSG);
        constexpr ULONG header_size = sizeof(CONSOLE_MSG_HEADER);
        constexpr ULONG read_offset = api_size + header_size;
        constexpr ULONG output_bytes = 2;

        oc::condrv::IoPacket packet{};
        packet.payload.user_defined = oc::condrv::UserDefinedPacket{};
        packet.descriptor.identifier.LowPart = 91;
        packet.descriptor.function = oc::condrv::console_io_user_defined;
        packet.descriptor.process = info.process;
        packet.descriptor.object = info.input;
        packet.descriptor.input_size = read_offset;
        packet.descriptor.output_size = api_size + output_bytes;
        packet.payload.user_defined.msg_header.ApiNumber = static_cast<ULONG>(ConsolepReadConsole);
        packet.payload.user_defined.msg_header.ApiDescriptorSize = api_size;
        packet.payload.user_defined.u.console_msg_l1.ReadConsole.Unicode = FALSE;

        oc::condrv::BasicApiMessage<MemoryComm> message(comm, packet);
        auto outcome = oc::condrv::dispatch_message(state, message, host_io);
        if (!outcome || message.completion().io_status.Status != oc::core::status_success)
        {
            return false;
        }

        if (message.packet().payload.user_defined.u.console_msg_l1.ReadConsole.NumBytes != output_bytes)
        {
            return false;
        }

        if (host_io.end_task_pids.size() != 1 || host_io.end_task_pids[0] != 9195)
        {
            return false;
        }

        if (auto released = message.release_message_buffers(); !released)
        {
            return false;
        }

        if (comm.output.size() != api_size + output_bytes)
        {
            return false;
        }

        return comm.output[api_size] == static_cast<std::byte>('X') &&
               comm.output[api_size + 1] == static_cast<std::byte>('Y') &&
               host_io.input_bytes_available() == 0;
    }

    bool test_user_defined_read_console_w_line_input_returns_crlf_and_echoes()
    {
        MemoryComm comm{};
        oc::condrv::ServerState state{};
        TestHostIo host_io{};

        auto connect_packet = make_connect_packet(9201, 9202);
        oc::condrv::BasicApiMessage<MemoryComm> connect_message(comm, connect_packet);
        auto connect_outcome = oc::condrv::dispatch_message(state, connect_message, host_io);
        if (!connect_outcome)
        {
            return false;
        }

        const auto info = unpack_connection_information(connect_message.completion());
        if (!set_input_code_page(comm, state, host_io, info, CP_UTF8))
        {
            return false;
        }

        state.set_input_mode(ENABLE_LINE_INPUT | ENABLE_PROCESSED_INPUT | ENABLE_ECHO_INPUT);
        host_io.input = {
            static_cast<std::byte>('a'),
            static_cast<std::byte>('b'),
            static_cast<std::byte>('c'),
            static_cast<std::byte>('\r'),
        };

        constexpr ULONG api_size = sizeof(CONSOLE_READCONSOLE_MSG);
        constexpr ULONG header_size = sizeof(CONSOLE_MSG_HEADER);
        constexpr ULONG read_offset = api_size + header_size;
        constexpr size_t expected_wchars = 5; // abc + CRLF
        constexpr ULONG output_bytes = static_cast<ULONG>(expected_wchars * sizeof(wchar_t));

        oc::condrv::IoPacket packet{};
        packet.payload.user_defined = oc::condrv::UserDefinedPacket{};
        packet.descriptor.identifier.LowPart = 90;
        packet.descriptor.function = oc::condrv::console_io_user_defined;
        packet.descriptor.process = info.process;
        packet.descriptor.object = info.input;
        packet.descriptor.input_size = read_offset;
        packet.descriptor.output_size = api_size + output_bytes;
        packet.payload.user_defined.msg_header.ApiNumber = static_cast<ULONG>(ConsolepReadConsole);
        packet.payload.user_defined.msg_header.ApiDescriptorSize = api_size;
        packet.payload.user_defined.u.console_msg_l1.ReadConsole.Unicode = TRUE;
        packet.payload.user_defined.u.console_msg_l1.ReadConsole.ProcessControlZ = FALSE;

        oc::condrv::BasicApiMessage<MemoryComm> message(comm, packet);
        auto outcome = oc::condrv::dispatch_message(state, message, host_io);
        if (!outcome || message.completion().io_status.Status != oc::core::status_success)
        {
            return false;
        }

        if (message.packet().payload.user_defined.u.console_msg_l1.ReadConsole.NumBytes != output_bytes)
        {
            return false;
        }

        if (auto released = message.release_message_buffers(); !released)
        {
            return false;
        }

        if (comm.output.size() != api_size + output_bytes)
        {
            return false;
        }

        wchar_t returned[expected_wchars]{};
        std::memcpy(returned, comm.output.data() + api_size, output_bytes);
        if (!(returned[0] == L'a' && returned[1] == L'b' && returned[2] == L'c' && returned[3] == L'\r' && returned[4] == L'\n'))
        {
            return false;
        }

        // Echo should have updated the active screen buffer model.
        {
            constexpr ULONG read_api_size = sizeof(CONSOLE_READCONSOLEOUTPUTSTRING_MSG);
            constexpr ULONG read_header_size = sizeof(CONSOLE_MSG_HEADER);
            constexpr ULONG read_read_offset = read_api_size + read_header_size;
            constexpr ULONG read_bytes = 3 * sizeof(wchar_t);

            oc::condrv::IoPacket read_packet{};
            read_packet.payload.user_defined = oc::condrv::UserDefinedPacket{};
            read_packet.descriptor.identifier.LowPart = 91;
            read_packet.descriptor.function = oc::condrv::console_io_user_defined;
            read_packet.descriptor.process = info.process;
            read_packet.descriptor.object = info.output;
            read_packet.descriptor.input_size = read_read_offset;
            read_packet.descriptor.output_size = read_api_size + read_bytes;
            read_packet.payload.user_defined.msg_header.ApiNumber = static_cast<ULONG>(ConsolepReadConsoleOutputString);
            read_packet.payload.user_defined.msg_header.ApiDescriptorSize = read_api_size;

            auto& body = read_packet.payload.user_defined.u.console_msg_l2.ReadConsoleOutputString;
            body.ReadCoord = COORD{ 0, 0 };
            body.StringType = CONSOLE_REAL_UNICODE;
            body.NumRecords = 0;

            comm.input.assign(read_packet.descriptor.input_size, std::byte{});
            comm.output.clear();

            oc::condrv::BasicApiMessage<MemoryComm> read_message(comm, read_packet);
            auto read_outcome = oc::condrv::dispatch_message(state, read_message, host_io);
            if (!read_outcome || read_message.completion().io_status.Status != oc::core::status_success)
            {
                return false;
            }

            if (auto released = read_message.release_message_buffers(); !released)
            {
                return false;
            }

            if (comm.output.size() != read_api_size + read_bytes)
            {
                return false;
            }

            wchar_t echoed[3]{};
            std::memcpy(echoed, comm.output.data() + read_api_size, read_bytes);
            return echoed[0] == L'a' && echoed[1] == L'b' && echoed[2] == L'c';
        }
    }

    bool test_user_defined_read_console_w_line_input_ctrl_c_returns_alerted_and_sends_end_task()
    {
        MemoryComm comm{};
        oc::condrv::ServerState state{};
        TestHostIo host_io{};

        auto connect_packet = make_connect_packet(9211, 9212);
        oc::condrv::BasicApiMessage<MemoryComm> connect_message(comm, connect_packet);
        auto connect_outcome = oc::condrv::dispatch_message(state, connect_message, host_io);
        if (!connect_outcome)
        {
            return false;
        }

        const auto info = unpack_connection_information(connect_message.completion());
        state.set_input_code_page(CP_UTF8);
        state.set_input_mode(ENABLE_LINE_INPUT | ENABLE_PROCESSED_INPUT | ENABLE_ECHO_INPUT);
        host_io.input = { static_cast<std::byte>(0x03) };

        constexpr ULONG api_size = sizeof(CONSOLE_READCONSOLE_MSG);
        constexpr ULONG header_size = sizeof(CONSOLE_MSG_HEADER);
        constexpr ULONG read_offset = api_size + header_size;

        oc::condrv::IoPacket packet{};
        packet.payload.user_defined = oc::condrv::UserDefinedPacket{};
        packet.descriptor.identifier.LowPart = 91;
        packet.descriptor.function = oc::condrv::console_io_user_defined;
        packet.descriptor.process = info.process;
        packet.descriptor.object = info.input;
        packet.descriptor.input_size = read_offset;
        packet.descriptor.output_size = api_size + 16;
        packet.payload.user_defined.msg_header.ApiNumber = static_cast<ULONG>(ConsolepReadConsole);
        packet.payload.user_defined.msg_header.ApiDescriptorSize = api_size;
        packet.payload.user_defined.u.console_msg_l1.ReadConsole.Unicode = TRUE;
        packet.payload.user_defined.u.console_msg_l1.ReadConsole.ProcessControlZ = FALSE;

        oc::condrv::BasicApiMessage<MemoryComm> message(comm, packet);
        auto outcome = oc::condrv::dispatch_message(state, message, host_io);
        if (!outcome || message.completion().io_status.Status != oc::core::status_alerted)
        {
            return false;
        }

        if (message.packet().payload.user_defined.u.console_msg_l1.ReadConsole.NumBytes != 0)
        {
            return false;
        }

        if (host_io.end_task_pids.size() != 1 || host_io.end_task_pids[0] != 9211)
        {
            return false;
        }

        if (!host_io.written.empty())
        {
            return false;
        }

        if (auto released = message.release_message_buffers(); !released)
        {
            return false;
        }

        return comm.output.size() == api_size && host_io.input_bytes_available() == 0;
    }

    bool test_user_defined_read_console_w_line_input_ctrl_break_returns_alerted_and_flushes_input()
    {
        MemoryComm comm{};
        oc::condrv::ServerState state{};
        TestHostIo host_io{};

        auto connect_packet = make_connect_packet(9213, 9214);
        oc::condrv::BasicApiMessage<MemoryComm> connect_message(comm, connect_packet);
        auto connect_outcome = oc::condrv::dispatch_message(state, connect_message, host_io);
        if (!connect_outcome)
        {
            return false;
        }

        const auto info = unpack_connection_information(connect_message.completion());
        state.set_input_code_page(CP_UTF8);
        state.set_input_mode(ENABLE_LINE_INPUT | ENABLE_PROCESSED_INPUT | ENABLE_ECHO_INPUT);

        constexpr std::string_view ctrl_break = "\x1b[3;0;0;1;8;1_";
        constexpr std::string_view tail = "Z";

        host_io.input.clear();
        host_io.input_offset = 0;
        for (const unsigned char ch : ctrl_break)
        {
            host_io.input.push_back(static_cast<std::byte>(ch));
        }
        for (const unsigned char ch : tail)
        {
            host_io.input.push_back(static_cast<std::byte>(ch));
        }

        constexpr ULONG api_size = sizeof(CONSOLE_READCONSOLE_MSG);
        constexpr ULONG header_size = sizeof(CONSOLE_MSG_HEADER);
        constexpr ULONG read_offset = api_size + header_size;

        oc::condrv::IoPacket packet{};
        packet.payload.user_defined = oc::condrv::UserDefinedPacket{};
        packet.descriptor.identifier.LowPart = 93;
        packet.descriptor.function = oc::condrv::console_io_user_defined;
        packet.descriptor.process = info.process;
        packet.descriptor.object = info.input;
        packet.descriptor.input_size = read_offset;
        packet.descriptor.output_size = api_size + 16;
        packet.payload.user_defined.msg_header.ApiNumber = static_cast<ULONG>(ConsolepReadConsole);
        packet.payload.user_defined.msg_header.ApiDescriptorSize = api_size;
        packet.payload.user_defined.u.console_msg_l1.ReadConsole.Unicode = TRUE;
        packet.payload.user_defined.u.console_msg_l1.ReadConsole.ProcessControlZ = FALSE;

        oc::condrv::BasicApiMessage<MemoryComm> message(comm, packet);
        auto outcome = oc::condrv::dispatch_message(state, message, host_io);
        if (!outcome || outcome->reply_pending || message.completion().io_status.Status != oc::core::status_alerted)
        {
            return false;
        }

        if (message.packet().payload.user_defined.u.console_msg_l1.ReadConsole.NumBytes != 0)
        {
            return false;
        }

        if (host_io.end_task_pids.size() != 1 || host_io.end_task_pids[0] != 9213)
        {
            return false;
        }

        if (host_io.end_task_events.size() != 1 || host_io.end_task_events[0] != CTRL_BREAK_EVENT)
        {
            return false;
        }

        if (host_io.end_task_flags.size() != 1 || host_io.end_task_flags[0] != oc::core::console_ctrl_break_flag)
        {
            return false;
        }

        if (!host_io.written.empty())
        {
            return false;
        }

        if (auto released = message.release_message_buffers(); !released)
        {
            return false;
        }

        return comm.output.size() == api_size && host_io.input_bytes_available() == 0;
    }

    bool test_user_defined_read_console_w_line_input_backspace_edits_and_echoes()
    {
        MemoryComm comm{};
        oc::condrv::ServerState state{};
        TestHostIo host_io{};

        auto connect_packet = make_connect_packet(9203, 9204);
        oc::condrv::BasicApiMessage<MemoryComm> connect_message(comm, connect_packet);
        auto connect_outcome = oc::condrv::dispatch_message(state, connect_message, host_io);
        if (!connect_outcome)
        {
            return false;
        }

        const auto info = unpack_connection_information(connect_message.completion());
        state.set_input_mode(ENABLE_LINE_INPUT | ENABLE_PROCESSED_INPUT | ENABLE_ECHO_INPUT);
        host_io.input = {
            static_cast<std::byte>('a'),
            static_cast<std::byte>('b'),
            static_cast<std::byte>('\b'),
            static_cast<std::byte>('c'),
            static_cast<std::byte>('\r'),
        };

        constexpr ULONG api_size = sizeof(CONSOLE_READCONSOLE_MSG);
        constexpr ULONG header_size = sizeof(CONSOLE_MSG_HEADER);
        constexpr ULONG read_offset = api_size + header_size;
        constexpr size_t expected_wchars = 4; // ac + CRLF
        constexpr ULONG output_bytes = static_cast<ULONG>(expected_wchars * sizeof(wchar_t));

        oc::condrv::IoPacket packet{};
        packet.payload.user_defined = oc::condrv::UserDefinedPacket{};
        packet.descriptor.identifier.LowPart = 92;
        packet.descriptor.function = oc::condrv::console_io_user_defined;
        packet.descriptor.process = info.process;
        packet.descriptor.object = info.input;
        packet.descriptor.input_size = read_offset;
        packet.descriptor.output_size = api_size + output_bytes;
        packet.payload.user_defined.msg_header.ApiNumber = static_cast<ULONG>(ConsolepReadConsole);
        packet.payload.user_defined.msg_header.ApiDescriptorSize = api_size;
        packet.payload.user_defined.u.console_msg_l1.ReadConsole.Unicode = TRUE;

        oc::condrv::BasicApiMessage<MemoryComm> message(comm, packet);
        auto outcome = oc::condrv::dispatch_message(state, message, host_io);
        if (!outcome || message.completion().io_status.Status != oc::core::status_success)
        {
            return false;
        }

        if (auto released = message.release_message_buffers(); !released)
        {
            return false;
        }

        if (comm.output.size() != api_size + output_bytes)
        {
            return false;
        }

        wchar_t returned[expected_wchars]{};
        std::memcpy(returned, comm.output.data() + api_size, output_bytes);
        if (!(returned[0] == L'a' && returned[1] == L'c' && returned[2] == L'\r' && returned[3] == L'\n'))
        {
            return false;
        }

        // "b" should have been erased from the echoed buffer.
        {
            constexpr ULONG read_api_size = sizeof(CONSOLE_READCONSOLEOUTPUTSTRING_MSG);
            constexpr ULONG read_header_size = sizeof(CONSOLE_MSG_HEADER);
            constexpr ULONG read_read_offset = read_api_size + read_header_size;
            constexpr ULONG read_bytes = 2 * sizeof(wchar_t);

            oc::condrv::IoPacket read_packet{};
            read_packet.payload.user_defined = oc::condrv::UserDefinedPacket{};
            read_packet.descriptor.identifier.LowPart = 93;
            read_packet.descriptor.function = oc::condrv::console_io_user_defined;
            read_packet.descriptor.process = info.process;
            read_packet.descriptor.object = info.output;
            read_packet.descriptor.input_size = read_read_offset;
            read_packet.descriptor.output_size = read_api_size + read_bytes;
            read_packet.payload.user_defined.msg_header.ApiNumber = static_cast<ULONG>(ConsolepReadConsoleOutputString);
            read_packet.payload.user_defined.msg_header.ApiDescriptorSize = read_api_size;

            auto& body = read_packet.payload.user_defined.u.console_msg_l2.ReadConsoleOutputString;
            body.ReadCoord = COORD{ 0, 0 };
            body.StringType = CONSOLE_REAL_UNICODE;
            body.NumRecords = 0;

            comm.input.assign(read_packet.descriptor.input_size, std::byte{});
            comm.output.clear();

            oc::condrv::BasicApiMessage<MemoryComm> read_message(comm, read_packet);
            auto read_outcome = oc::condrv::dispatch_message(state, read_message, host_io);
            if (!read_outcome || read_message.completion().io_status.Status != oc::core::status_success)
            {
                return false;
            }

            if (auto released = read_message.release_message_buffers(); !released)
            {
                return false;
            }

            if (comm.output.size() != read_api_size + read_bytes)
            {
                return false;
            }

            wchar_t echoed[2]{};
            std::memcpy(echoed, comm.output.data() + read_api_size, read_bytes);
            return echoed[0] == L'a' && echoed[1] == L'c';
        }
    }

    bool test_user_defined_read_console_w_line_input_small_buffer_sets_pending()
    {
        MemoryComm comm{};
        oc::condrv::ServerState state{};
        TestHostIo host_io{};

        auto connect_packet = make_connect_packet(9205, 9206);
        oc::condrv::BasicApiMessage<MemoryComm> connect_message(comm, connect_packet);
        auto connect_outcome = oc::condrv::dispatch_message(state, connect_message, host_io);
        if (!connect_outcome)
        {
            return false;
        }

        const auto info = unpack_connection_information(connect_message.completion());
        state.set_input_mode(ENABLE_LINE_INPUT | ENABLE_PROCESSED_INPUT);
        host_io.input = {
            static_cast<std::byte>('h'),
            static_cast<std::byte>('e'),
            static_cast<std::byte>('l'),
            static_cast<std::byte>('l'),
            static_cast<std::byte>('o'),
            static_cast<std::byte>('\r'),
        };

        constexpr ULONG api_size = sizeof(CONSOLE_READCONSOLE_MSG);
        constexpr ULONG header_size = sizeof(CONSOLE_MSG_HEADER);
        constexpr ULONG read_offset = api_size + header_size;

        // First read only has room for 3 UTF-16 code units.
        {
            constexpr ULONG output_bytes = 3 * sizeof(wchar_t);

            oc::condrv::IoPacket packet{};
            packet.payload.user_defined = oc::condrv::UserDefinedPacket{};
            packet.descriptor.identifier.LowPart = 94;
            packet.descriptor.function = oc::condrv::console_io_user_defined;
            packet.descriptor.process = info.process;
            packet.descriptor.object = info.input;
            packet.descriptor.input_size = read_offset;
            packet.descriptor.output_size = api_size + output_bytes;
            packet.payload.user_defined.msg_header.ApiNumber = static_cast<ULONG>(ConsolepReadConsole);
            packet.payload.user_defined.msg_header.ApiDescriptorSize = api_size;
            packet.payload.user_defined.u.console_msg_l1.ReadConsole.Unicode = TRUE;

            oc::condrv::BasicApiMessage<MemoryComm> message(comm, packet);
            auto outcome = oc::condrv::dispatch_message(state, message, host_io);
            if (!outcome || message.completion().io_status.Status != oc::core::status_success)
            {
                return false;
            }

            if (message.packet().payload.user_defined.u.console_msg_l1.ReadConsole.NumBytes != output_bytes)
            {
                return false;
            }

            if (auto released = message.release_message_buffers(); !released)
            {
                return false;
            }

            if (comm.output.size() != api_size + output_bytes)
            {
                return false;
            }

            wchar_t returned[3]{};
            std::memcpy(returned, comm.output.data() + api_size, output_bytes);
            if (!(returned[0] == L'h' && returned[1] == L'e' && returned[2] == L'l'))
            {
                return false;
            }
        }

        // Second read returns the remainder ("lo\r\n").
        {
            constexpr ULONG output_bytes = 16;

            oc::condrv::IoPacket packet{};
            packet.payload.user_defined = oc::condrv::UserDefinedPacket{};
            packet.descriptor.identifier.LowPart = 95;
            packet.descriptor.function = oc::condrv::console_io_user_defined;
            packet.descriptor.process = info.process;
            packet.descriptor.object = info.input;
            packet.descriptor.input_size = read_offset;
            packet.descriptor.output_size = api_size + output_bytes;
            packet.payload.user_defined.msg_header.ApiNumber = static_cast<ULONG>(ConsolepReadConsole);
            packet.payload.user_defined.msg_header.ApiDescriptorSize = api_size;
            packet.payload.user_defined.u.console_msg_l1.ReadConsole.Unicode = TRUE;

            oc::condrv::BasicApiMessage<MemoryComm> message(comm, packet);
            auto outcome = oc::condrv::dispatch_message(state, message, host_io);
            if (!outcome || message.completion().io_status.Status != oc::core::status_success)
            {
                return false;
            }

            if (message.packet().payload.user_defined.u.console_msg_l1.ReadConsole.NumBytes != 4 * sizeof(wchar_t))
            {
                return false;
            }

            if (auto released = message.release_message_buffers(); !released)
            {
                return false;
            }

            if (comm.output.size() != api_size + 4 * sizeof(wchar_t))
            {
                return false;
            }

            wchar_t returned[4]{};
            std::memcpy(returned, comm.output.data() + api_size, 4 * sizeof(wchar_t));
            return returned[0] == L'l' && returned[1] == L'o' && returned[2] == L'\r' && returned[3] == L'\n';
        }
    }

    bool test_user_defined_read_console_w_line_input_without_processed_returns_cr()
    {
        MemoryComm comm{};
        oc::condrv::ServerState state{};
        TestHostIo host_io{};

        auto connect_packet = make_connect_packet(9207, 9208);
        oc::condrv::BasicApiMessage<MemoryComm> connect_message(comm, connect_packet);
        auto connect_outcome = oc::condrv::dispatch_message(state, connect_message, host_io);
        if (!connect_outcome)
        {
            return false;
        }

        const auto info = unpack_connection_information(connect_message.completion());
        state.set_input_mode(ENABLE_LINE_INPUT);
        host_io.input = {
            static_cast<std::byte>('x'),
            static_cast<std::byte>('\r'),
        };

        constexpr ULONG api_size = sizeof(CONSOLE_READCONSOLE_MSG);
        constexpr ULONG header_size = sizeof(CONSOLE_MSG_HEADER);
        constexpr ULONG read_offset = api_size + header_size;
        constexpr ULONG output_bytes = 2 * sizeof(wchar_t);

        oc::condrv::IoPacket packet{};
        packet.payload.user_defined = oc::condrv::UserDefinedPacket{};
        packet.descriptor.identifier.LowPart = 96;
        packet.descriptor.function = oc::condrv::console_io_user_defined;
        packet.descriptor.process = info.process;
        packet.descriptor.object = info.input;
        packet.descriptor.input_size = read_offset;
        packet.descriptor.output_size = api_size + output_bytes;
        packet.payload.user_defined.msg_header.ApiNumber = static_cast<ULONG>(ConsolepReadConsole);
        packet.payload.user_defined.msg_header.ApiDescriptorSize = api_size;
        packet.payload.user_defined.u.console_msg_l1.ReadConsole.Unicode = TRUE;

        oc::condrv::BasicApiMessage<MemoryComm> message(comm, packet);
        auto outcome = oc::condrv::dispatch_message(state, message, host_io);
        if (!outcome || message.completion().io_status.Status != oc::core::status_success)
        {
            return false;
        }

        if (auto released = message.release_message_buffers(); !released)
        {
            return false;
        }

        if (comm.output.size() != api_size + output_bytes)
        {
            return false;
        }

        wchar_t returned[2]{};
        std::memcpy(returned, comm.output.data() + api_size, output_bytes);
        return returned[0] == L'x' && returned[1] == L'\r';
    }

    bool test_user_defined_read_console_a_line_input_returns_crlf()
    {
        MemoryComm comm{};
        oc::condrv::ServerState state{};
        TestHostIo host_io{};

        auto connect_packet = make_connect_packet(9211, 9212);
        oc::condrv::BasicApiMessage<MemoryComm> connect_message(comm, connect_packet);
        auto connect_outcome = oc::condrv::dispatch_message(state, connect_message, host_io);
        if (!connect_outcome)
        {
            return false;
        }

        const auto info = unpack_connection_information(connect_message.completion());
        state.set_input_mode(ENABLE_LINE_INPUT | ENABLE_PROCESSED_INPUT);
        host_io.input = {
            static_cast<std::byte>('a'),
            static_cast<std::byte>('b'),
            static_cast<std::byte>('c'),
            static_cast<std::byte>('\r'),
        };

        constexpr ULONG api_size = sizeof(CONSOLE_READCONSOLE_MSG);
        constexpr ULONG header_size = sizeof(CONSOLE_MSG_HEADER);
        constexpr ULONG read_offset = api_size + header_size;
        constexpr ULONG output_bytes = 16;

        oc::condrv::IoPacket packet{};
        packet.payload.user_defined = oc::condrv::UserDefinedPacket{};
        packet.descriptor.identifier.LowPart = 98;
        packet.descriptor.function = oc::condrv::console_io_user_defined;
        packet.descriptor.process = info.process;
        packet.descriptor.object = info.input;
        packet.descriptor.input_size = read_offset;
        packet.descriptor.output_size = api_size + output_bytes;
        packet.payload.user_defined.msg_header.ApiNumber = static_cast<ULONG>(ConsolepReadConsole);
        packet.payload.user_defined.msg_header.ApiDescriptorSize = api_size;
        packet.payload.user_defined.u.console_msg_l1.ReadConsole.Unicode = FALSE;

        oc::condrv::BasicApiMessage<MemoryComm> message(comm, packet);
        auto outcome = oc::condrv::dispatch_message(state, message, host_io);
        if (!outcome || message.completion().io_status.Status != oc::core::status_success)
        {
            return false;
        }

        if (message.packet().payload.user_defined.u.console_msg_l1.ReadConsole.NumBytes != 5)
        {
            return false;
        }

        if (auto released = message.release_message_buffers(); !released)
        {
            return false;
        }

        if (comm.output.size() != api_size + 5)
        {
            return false;
        }

        const unsigned char* returned = reinterpret_cast<const unsigned char*>(comm.output.data() + api_size);
        return returned[0] == static_cast<unsigned char>('a') &&
               returned[1] == static_cast<unsigned char>('b') &&
               returned[2] == static_cast<unsigned char>('c') &&
               returned[3] == static_cast<unsigned char>('\r') &&
               returned[4] == static_cast<unsigned char>('\n');
    }

    bool test_user_defined_read_console_a_line_input_small_buffer_sets_pending()
    {
        MemoryComm comm{};
        oc::condrv::ServerState state{};
        TestHostIo host_io{};

        auto connect_packet = make_connect_packet(9213, 9214);
        oc::condrv::BasicApiMessage<MemoryComm> connect_message(comm, connect_packet);
        auto connect_outcome = oc::condrv::dispatch_message(state, connect_message, host_io);
        if (!connect_outcome)
        {
            return false;
        }

        const auto info = unpack_connection_information(connect_message.completion());
        state.set_input_mode(ENABLE_LINE_INPUT | ENABLE_PROCESSED_INPUT);
        host_io.input = {
            static_cast<std::byte>('h'),
            static_cast<std::byte>('e'),
            static_cast<std::byte>('l'),
            static_cast<std::byte>('l'),
            static_cast<std::byte>('o'),
            static_cast<std::byte>('\r'),
        };

        constexpr ULONG api_size = sizeof(CONSOLE_READCONSOLE_MSG);
        constexpr ULONG header_size = sizeof(CONSOLE_MSG_HEADER);
        constexpr ULONG read_offset = api_size + header_size;

        // First read has room for 3 bytes.
        {
            constexpr ULONG output_bytes = 3;

            oc::condrv::IoPacket packet{};
            packet.payload.user_defined = oc::condrv::UserDefinedPacket{};
            packet.descriptor.identifier.LowPart = 99;
            packet.descriptor.function = oc::condrv::console_io_user_defined;
            packet.descriptor.process = info.process;
            packet.descriptor.object = info.input;
            packet.descriptor.input_size = read_offset;
            packet.descriptor.output_size = api_size + output_bytes;
            packet.payload.user_defined.msg_header.ApiNumber = static_cast<ULONG>(ConsolepReadConsole);
            packet.payload.user_defined.msg_header.ApiDescriptorSize = api_size;
            packet.payload.user_defined.u.console_msg_l1.ReadConsole.Unicode = FALSE;

            oc::condrv::BasicApiMessage<MemoryComm> message(comm, packet);
            auto outcome = oc::condrv::dispatch_message(state, message, host_io);
            if (!outcome || message.completion().io_status.Status != oc::core::status_success)
            {
                return false;
            }

            if (message.packet().payload.user_defined.u.console_msg_l1.ReadConsole.NumBytes != output_bytes)
            {
                return false;
            }

            if (auto released = message.release_message_buffers(); !released)
            {
                return false;
            }

            if (comm.output.size() != api_size + output_bytes)
            {
                return false;
            }

            const unsigned char* returned = reinterpret_cast<const unsigned char*>(comm.output.data() + api_size);
            if (!(returned[0] == static_cast<unsigned char>('h') &&
                  returned[1] == static_cast<unsigned char>('e') &&
                  returned[2] == static_cast<unsigned char>('l')))
            {
                return false;
            }
        }

        // Second read returns the remainder ("lo\r\n").
        {
            constexpr ULONG output_bytes = 16;

            oc::condrv::IoPacket packet{};
            packet.payload.user_defined = oc::condrv::UserDefinedPacket{};
            packet.descriptor.identifier.LowPart = 100;
            packet.descriptor.function = oc::condrv::console_io_user_defined;
            packet.descriptor.process = info.process;
            packet.descriptor.object = info.input;
            packet.descriptor.input_size = read_offset;
            packet.descriptor.output_size = api_size + output_bytes;
            packet.payload.user_defined.msg_header.ApiNumber = static_cast<ULONG>(ConsolepReadConsole);
            packet.payload.user_defined.msg_header.ApiDescriptorSize = api_size;
            packet.payload.user_defined.u.console_msg_l1.ReadConsole.Unicode = FALSE;

            oc::condrv::BasicApiMessage<MemoryComm> message(comm, packet);
            auto outcome = oc::condrv::dispatch_message(state, message, host_io);
            if (!outcome || message.completion().io_status.Status != oc::core::status_success)
            {
                return false;
            }

            if (message.packet().payload.user_defined.u.console_msg_l1.ReadConsole.NumBytes != 4)
            {
                return false;
            }

            if (auto released = message.release_message_buffers(); !released)
            {
                return false;
            }

            if (comm.output.size() != api_size + 4)
            {
                return false;
            }

            const unsigned char* returned = reinterpret_cast<const unsigned char*>(comm.output.data() + api_size);
            return returned[0] == static_cast<unsigned char>('l') &&
                   returned[1] == static_cast<unsigned char>('o') &&
                   returned[2] == static_cast<unsigned char>('\r') &&
                   returned[3] == static_cast<unsigned char>('\n');
        }
    }

    bool test_user_defined_read_console_a_line_input_utf8_buffer_too_small_for_multibyte_char()
    {
        MemoryComm comm{};
        oc::condrv::ServerState state{};
        TestHostIo host_io{};

        auto connect_packet = make_connect_packet(9215, 9216);
        oc::condrv::BasicApiMessage<MemoryComm> connect_message(comm, connect_packet);
        auto connect_outcome = oc::condrv::dispatch_message(state, connect_message, host_io);
        if (!connect_outcome)
        {
            return false;
        }

        const auto info = unpack_connection_information(connect_message.completion());
        if (!set_input_code_page(comm, state, host_io, info, CP_UTF8))
        {
            return false;
        }

        state.set_input_mode(ENABLE_LINE_INPUT | ENABLE_PROCESSED_INPUT);
        host_io.input = {
            static_cast<std::byte>(0xC3),
            static_cast<std::byte>(0xA9), // U+00E9
            static_cast<std::byte>('\r'),
        };

        constexpr ULONG api_size = sizeof(CONSOLE_READCONSOLE_MSG);
        constexpr ULONG header_size = sizeof(CONSOLE_MSG_HEADER);
        constexpr ULONG read_offset = api_size + header_size;

        // Output buffer can hold only 1 byte (cannot hold UTF-8 for U+00E9).
        {
            oc::condrv::IoPacket packet{};
            packet.payload.user_defined = oc::condrv::UserDefinedPacket{};
            packet.descriptor.identifier.LowPart = 101;
            packet.descriptor.function = oc::condrv::console_io_user_defined;
            packet.descriptor.process = info.process;
            packet.descriptor.object = info.input;
            packet.descriptor.input_size = read_offset;
            packet.descriptor.output_size = api_size + 1;
            packet.payload.user_defined.msg_header.ApiNumber = static_cast<ULONG>(ConsolepReadConsole);
            packet.payload.user_defined.msg_header.ApiDescriptorSize = api_size;
            packet.payload.user_defined.u.console_msg_l1.ReadConsole.Unicode = FALSE;

            oc::condrv::BasicApiMessage<MemoryComm> message(comm, packet);
            auto outcome = oc::condrv::dispatch_message(state, message, host_io);
            if (!outcome || message.completion().io_status.Status != oc::core::status_buffer_too_small)
            {
                return false;
            }
        }

        // A larger follow-up read should succeed using the preserved pending buffer.
        {
            oc::condrv::IoPacket packet{};
            packet.payload.user_defined = oc::condrv::UserDefinedPacket{};
            packet.descriptor.identifier.LowPart = 102;
            packet.descriptor.function = oc::condrv::console_io_user_defined;
            packet.descriptor.process = info.process;
            packet.descriptor.object = info.input;
            packet.descriptor.input_size = read_offset;
            packet.descriptor.output_size = api_size + 16;
            packet.payload.user_defined.msg_header.ApiNumber = static_cast<ULONG>(ConsolepReadConsole);
            packet.payload.user_defined.msg_header.ApiDescriptorSize = api_size;
            packet.payload.user_defined.u.console_msg_l1.ReadConsole.Unicode = FALSE;

            oc::condrv::BasicApiMessage<MemoryComm> message(comm, packet);
            auto outcome = oc::condrv::dispatch_message(state, message, host_io);
            if (!outcome || message.completion().io_status.Status != oc::core::status_success)
            {
                return false;
            }

            if (message.packet().payload.user_defined.u.console_msg_l1.ReadConsole.NumBytes != 4)
            {
                return false;
            }

            if (auto released = message.release_message_buffers(); !released)
            {
                return false;
            }

            if (comm.output.size() != api_size + 4)
            {
                return false;
            }

            const unsigned char* returned = reinterpret_cast<const unsigned char*>(comm.output.data() + api_size);
            return returned[0] == 0xC3 && returned[1] == 0xA9 &&
                   returned[2] == static_cast<unsigned char>('\r') &&
                   returned[3] == static_cast<unsigned char>('\n');
        }
    }

    bool test_user_defined_read_console_w_line_input_handles_split_utf8_sequence()
    {
        MemoryComm comm{};
        oc::condrv::ServerState state{};
        TestHostIo host_io{};

        auto connect_packet = make_connect_packet(9209, 9210);
        oc::condrv::BasicApiMessage<MemoryComm> connect_message(comm, connect_packet);
        auto connect_outcome = oc::condrv::dispatch_message(state, connect_message, host_io);
        if (!connect_outcome)
        {
            return false;
        }

        const auto info = unpack_connection_information(connect_message.completion());
        state.set_input_code_page(CP_UTF8);

        state.set_input_mode(ENABLE_LINE_INPUT | ENABLE_PROCESSED_INPUT);

        // Provide only the first UTF-8 byte. The cooked line-input read should reply-pend
        // and drain the incomplete sequence into the per-handle prefix buffer.
        host_io.input = { static_cast<std::byte>(0xC3) };

        constexpr ULONG api_size = sizeof(CONSOLE_READCONSOLE_MSG);
        constexpr ULONG header_size = sizeof(CONSOLE_MSG_HEADER);
        constexpr ULONG read_offset = api_size + header_size;
        constexpr ULONG output_bytes = 16;

        oc::condrv::IoPacket packet{};
        packet.payload.user_defined = oc::condrv::UserDefinedPacket{};
        packet.descriptor.identifier.LowPart = 97;
        packet.descriptor.function = oc::condrv::console_io_user_defined;
        packet.descriptor.process = info.process;
        packet.descriptor.object = info.input;
        packet.descriptor.input_size = read_offset;
        packet.descriptor.output_size = api_size + output_bytes;
        packet.payload.user_defined.msg_header.ApiNumber = static_cast<ULONG>(ConsolepReadConsole);
        packet.payload.user_defined.msg_header.ApiDescriptorSize = api_size;
        packet.payload.user_defined.u.console_msg_l1.ReadConsole.Unicode = TRUE;

        comm.input.assign(read_offset, std::byte{});

        oc::condrv::BasicApiMessage<MemoryComm> message(comm, packet);
        auto outcome = oc::condrv::dispatch_message(state, message, host_io);
        if (!outcome || !outcome->reply_pending)
        {
            return false;
        }

        auto* handle = state.find_object(info.input);
        if (handle == nullptr || handle->pending_input_bytes.size() != 1)
        {
            return false;
        }

        if (host_io.input_bytes_available() != 0)
        {
            return false;
        }

        const std::array<std::byte, 2> remainder{ static_cast<std::byte>(0xA9), static_cast<std::byte>('\r') };
        if (!host_io.inject_input_bytes(remainder))
        {
            return false;
        }

        outcome = oc::condrv::dispatch_message(state, message, host_io);
        if (!outcome || outcome->reply_pending || message.completion().io_status.Status != oc::core::status_success)
        {
            return false;
        }

        if (message.packet().payload.user_defined.u.console_msg_l1.ReadConsole.NumBytes != 3 * sizeof(wchar_t))
        {
            return false;
        }

        if (auto released = message.release_message_buffers(); !released)
        {
            return false;
        }

        if (comm.output.size() != api_size + 3 * sizeof(wchar_t))
        {
            return false;
        }

        wchar_t returned[3]{};
        std::memcpy(returned, comm.output.data() + api_size, 3 * sizeof(wchar_t));
        return returned[0] == static_cast<wchar_t>(0x00E9) &&
               returned[1] == L'\r' &&
               returned[2] == L'\n' &&
               host_io.input_bytes_available() == 0 &&
               handle->pending_input_bytes.size() == 0;
    }

    bool test_user_defined_read_console_w_line_input_insert_in_middle()
    {
        MemoryComm comm{};
        oc::condrv::ServerState state{};
        TestHostIo host_io{};

        auto connect_packet = make_connect_packet(9221, 9222);
        oc::condrv::BasicApiMessage<MemoryComm> connect_message(comm, connect_packet);
        auto connect_outcome = oc::condrv::dispatch_message(state, connect_message, host_io);
        if (!connect_outcome)
        {
            return false;
        }

        const auto info = unpack_connection_information(connect_message.completion());
        if (!set_input_code_page(comm, state, host_io, info, CP_UTF8))
        {
            return false;
        }

        state.set_input_mode(ENABLE_LINE_INPUT | ENABLE_PROCESSED_INPUT | ENABLE_ECHO_INPUT);
        host_io.input = {
            static_cast<std::byte>('a'),
            static_cast<std::byte>('b'),
            static_cast<std::byte>(0x1b),
            static_cast<std::byte>('['),
            static_cast<std::byte>('D'),
            static_cast<std::byte>('X'),
            static_cast<std::byte>('\r'),
        };

        constexpr ULONG api_size = sizeof(CONSOLE_READCONSOLE_MSG);
        constexpr ULONG header_size = sizeof(CONSOLE_MSG_HEADER);
        constexpr ULONG read_offset = api_size + header_size;
        constexpr size_t expected_wchars = 5; // a X b CRLF
        constexpr ULONG output_bytes = static_cast<ULONG>(expected_wchars * sizeof(wchar_t));

        oc::condrv::IoPacket packet{};
        packet.payload.user_defined = oc::condrv::UserDefinedPacket{};
        packet.descriptor.identifier.LowPart = 98;
        packet.descriptor.function = oc::condrv::console_io_user_defined;
        packet.descriptor.process = info.process;
        packet.descriptor.object = info.input;
        packet.descriptor.input_size = read_offset;
        packet.descriptor.output_size = api_size + output_bytes;
        packet.payload.user_defined.msg_header.ApiNumber = static_cast<ULONG>(ConsolepReadConsole);
        packet.payload.user_defined.msg_header.ApiDescriptorSize = api_size;
        packet.payload.user_defined.u.console_msg_l1.ReadConsole.Unicode = TRUE;
        packet.payload.user_defined.u.console_msg_l1.ReadConsole.ProcessControlZ = FALSE;

        comm.input.assign(read_offset, std::byte{});
        comm.output.clear();

        oc::condrv::BasicApiMessage<MemoryComm> message(comm, packet);
        auto outcome = oc::condrv::dispatch_message(state, message, host_io);
        if (!outcome || message.completion().io_status.Status != oc::core::status_success)
        {
            return false;
        }

        if (message.packet().payload.user_defined.u.console_msg_l1.ReadConsole.NumBytes != output_bytes)
        {
            return false;
        }

        if (auto released = message.release_message_buffers(); !released)
        {
            return false;
        }

        if (comm.output.size() != api_size + output_bytes)
        {
            return false;
        }

        wchar_t returned[expected_wchars]{};
        std::memcpy(returned, comm.output.data() + api_size, output_bytes);
        if (!(returned[0] == L'a' && returned[1] == L'X' && returned[2] == L'b' && returned[3] == L'\r' && returned[4] == L'\n'))
        {
            return false;
        }

        // Echo should have updated the active screen buffer model.
        {
            constexpr ULONG read_api_size = sizeof(CONSOLE_READCONSOLEOUTPUTSTRING_MSG);
            constexpr ULONG read_header_size = sizeof(CONSOLE_MSG_HEADER);
            constexpr ULONG read_read_offset = read_api_size + read_header_size;
            constexpr ULONG read_bytes = 3 * sizeof(wchar_t);

            oc::condrv::IoPacket read_packet{};
            read_packet.payload.user_defined = oc::condrv::UserDefinedPacket{};
            read_packet.descriptor.identifier.LowPart = 99;
            read_packet.descriptor.function = oc::condrv::console_io_user_defined;
            read_packet.descriptor.process = info.process;
            read_packet.descriptor.object = info.output;
            read_packet.descriptor.input_size = read_read_offset;
            read_packet.descriptor.output_size = read_api_size + read_bytes;
            read_packet.payload.user_defined.msg_header.ApiNumber = static_cast<ULONG>(ConsolepReadConsoleOutputString);
            read_packet.payload.user_defined.msg_header.ApiDescriptorSize = read_api_size;

            auto& body = read_packet.payload.user_defined.u.console_msg_l2.ReadConsoleOutputString;
            body.ReadCoord = COORD{ 0, 0 };
            body.StringType = CONSOLE_REAL_UNICODE;
            body.NumRecords = 0;

            comm.input.assign(read_read_offset, std::byte{});
            comm.output.clear();

            oc::condrv::BasicApiMessage<MemoryComm> read_message(comm, read_packet);
            auto read_outcome = oc::condrv::dispatch_message(state, read_message, host_io);
            if (!read_outcome || read_message.completion().io_status.Status != oc::core::status_success)
            {
                return false;
            }

            if (auto released = read_message.release_message_buffers(); !released)
            {
                return false;
            }

            if (comm.output.size() != read_api_size + read_bytes)
            {
                return false;
            }

            wchar_t echoed[3]{};
            std::memcpy(echoed, comm.output.data() + read_api_size, read_bytes);
            if (!(echoed[0] == L'a' && echoed[1] == L'X' && echoed[2] == L'b'))
            {
                return false;
            }
        }

        return true;
    }

    bool test_user_defined_read_console_w_line_input_overwrite_toggle()
    {
        MemoryComm comm{};
        oc::condrv::ServerState state{};
        TestHostIo host_io{};

        auto connect_packet = make_connect_packet(9223, 9224);
        oc::condrv::BasicApiMessage<MemoryComm> connect_message(comm, connect_packet);
        auto connect_outcome = oc::condrv::dispatch_message(state, connect_message, host_io);
        if (!connect_outcome)
        {
            return false;
        }

        const auto info = unpack_connection_information(connect_message.completion());
        if (!set_input_code_page(comm, state, host_io, info, CP_UTF8))
        {
            return false;
        }

        state.set_input_mode(ENABLE_LINE_INPUT | ENABLE_PROCESSED_INPUT | ENABLE_ECHO_INPUT);
        host_io.input = {
            static_cast<std::byte>('a'),
            static_cast<std::byte>('b'),
            static_cast<std::byte>(0x1b),
            static_cast<std::byte>('['),
            static_cast<std::byte>('D'),
            static_cast<std::byte>(0x1b),
            static_cast<std::byte>('['),
            static_cast<std::byte>('2'),
            static_cast<std::byte>('~'),
            static_cast<std::byte>('X'),
            static_cast<std::byte>('\r'),
        };

        constexpr ULONG api_size = sizeof(CONSOLE_READCONSOLE_MSG);
        constexpr ULONG header_size = sizeof(CONSOLE_MSG_HEADER);
        constexpr ULONG read_offset = api_size + header_size;
        constexpr size_t expected_wchars = 4; // a X CRLF
        constexpr ULONG output_bytes = static_cast<ULONG>(expected_wchars * sizeof(wchar_t));

        oc::condrv::IoPacket packet{};
        packet.payload.user_defined = oc::condrv::UserDefinedPacket{};
        packet.descriptor.identifier.LowPart = 100;
        packet.descriptor.function = oc::condrv::console_io_user_defined;
        packet.descriptor.process = info.process;
        packet.descriptor.object = info.input;
        packet.descriptor.input_size = read_offset;
        packet.descriptor.output_size = api_size + output_bytes;
        packet.payload.user_defined.msg_header.ApiNumber = static_cast<ULONG>(ConsolepReadConsole);
        packet.payload.user_defined.msg_header.ApiDescriptorSize = api_size;
        packet.payload.user_defined.u.console_msg_l1.ReadConsole.Unicode = TRUE;
        packet.payload.user_defined.u.console_msg_l1.ReadConsole.ProcessControlZ = FALSE;

        comm.input.assign(read_offset, std::byte{});
        comm.output.clear();

        oc::condrv::BasicApiMessage<MemoryComm> message(comm, packet);
        auto outcome = oc::condrv::dispatch_message(state, message, host_io);
        if (!outcome || message.completion().io_status.Status != oc::core::status_success)
        {
            return false;
        }

        if (message.packet().payload.user_defined.u.console_msg_l1.ReadConsole.NumBytes != output_bytes)
        {
            return false;
        }

        if (auto released = message.release_message_buffers(); !released)
        {
            return false;
        }

        if (comm.output.size() != api_size + output_bytes)
        {
            return false;
        }

        wchar_t returned[expected_wchars]{};
        std::memcpy(returned, comm.output.data() + api_size, output_bytes);
        if (!(returned[0] == L'a' && returned[1] == L'X' && returned[2] == L'\r' && returned[3] == L'\n'))
        {
            return false;
        }

        // Echo should have updated the active screen buffer model.
        {
            constexpr ULONG read_api_size = sizeof(CONSOLE_READCONSOLEOUTPUTSTRING_MSG);
            constexpr ULONG read_header_size = sizeof(CONSOLE_MSG_HEADER);
            constexpr ULONG read_read_offset = read_api_size + read_header_size;
            constexpr ULONG read_bytes = 2 * sizeof(wchar_t);

            oc::condrv::IoPacket read_packet{};
            read_packet.payload.user_defined = oc::condrv::UserDefinedPacket{};
            read_packet.descriptor.identifier.LowPart = 101;
            read_packet.descriptor.function = oc::condrv::console_io_user_defined;
            read_packet.descriptor.process = info.process;
            read_packet.descriptor.object = info.output;
            read_packet.descriptor.input_size = read_read_offset;
            read_packet.descriptor.output_size = read_api_size + read_bytes;
            read_packet.payload.user_defined.msg_header.ApiNumber = static_cast<ULONG>(ConsolepReadConsoleOutputString);
            read_packet.payload.user_defined.msg_header.ApiDescriptorSize = read_api_size;

            auto& body = read_packet.payload.user_defined.u.console_msg_l2.ReadConsoleOutputString;
            body.ReadCoord = COORD{ 0, 0 };
            body.StringType = CONSOLE_REAL_UNICODE;
            body.NumRecords = 0;

            comm.input.assign(read_read_offset, std::byte{});
            comm.output.clear();

            oc::condrv::BasicApiMessage<MemoryComm> read_message(comm, read_packet);
            auto read_outcome = oc::condrv::dispatch_message(state, read_message, host_io);
            if (!read_outcome || read_message.completion().io_status.Status != oc::core::status_success)
            {
                return false;
            }

            if (auto released = read_message.release_message_buffers(); !released)
            {
                return false;
            }

            if (comm.output.size() != read_api_size + read_bytes)
            {
                return false;
            }

            wchar_t echoed[2]{};
            std::memcpy(echoed, comm.output.data() + read_api_size, read_bytes);
            if (!(echoed[0] == L'a' && echoed[1] == L'X'))
            {
                return false;
            }
        }

        return true;
    }

    bool test_user_defined_read_console_w_line_input_delete_in_middle()
    {
        MemoryComm comm{};
        oc::condrv::ServerState state{};
        TestHostIo host_io{};

        auto connect_packet = make_connect_packet(9225, 9226);
        oc::condrv::BasicApiMessage<MemoryComm> connect_message(comm, connect_packet);
        auto connect_outcome = oc::condrv::dispatch_message(state, connect_message, host_io);
        if (!connect_outcome)
        {
            return false;
        }

        const auto info = unpack_connection_information(connect_message.completion());
        if (!set_input_code_page(comm, state, host_io, info, CP_UTF8))
        {
            return false;
        }

        state.set_input_mode(ENABLE_LINE_INPUT | ENABLE_PROCESSED_INPUT | ENABLE_ECHO_INPUT);
        host_io.input = {
            static_cast<std::byte>('a'),
            static_cast<std::byte>('b'),
            static_cast<std::byte>('c'),
            static_cast<std::byte>(0x1b),
            static_cast<std::byte>('['),
            static_cast<std::byte>('D'),
            static_cast<std::byte>(0x1b),
            static_cast<std::byte>('['),
            static_cast<std::byte>('D'),
            static_cast<std::byte>(0x1b),
            static_cast<std::byte>('['),
            static_cast<std::byte>('3'),
            static_cast<std::byte>('~'),
            static_cast<std::byte>('\r'),
        };

        constexpr ULONG api_size = sizeof(CONSOLE_READCONSOLE_MSG);
        constexpr ULONG header_size = sizeof(CONSOLE_MSG_HEADER);
        constexpr ULONG read_offset = api_size + header_size;
        constexpr size_t expected_wchars = 4; // a c CRLF
        constexpr ULONG output_bytes = static_cast<ULONG>(expected_wchars * sizeof(wchar_t));

        oc::condrv::IoPacket packet{};
        packet.payload.user_defined = oc::condrv::UserDefinedPacket{};
        packet.descriptor.identifier.LowPart = 102;
        packet.descriptor.function = oc::condrv::console_io_user_defined;
        packet.descriptor.process = info.process;
        packet.descriptor.object = info.input;
        packet.descriptor.input_size = read_offset;
        packet.descriptor.output_size = api_size + output_bytes;
        packet.payload.user_defined.msg_header.ApiNumber = static_cast<ULONG>(ConsolepReadConsole);
        packet.payload.user_defined.msg_header.ApiDescriptorSize = api_size;
        packet.payload.user_defined.u.console_msg_l1.ReadConsole.Unicode = TRUE;
        packet.payload.user_defined.u.console_msg_l1.ReadConsole.ProcessControlZ = FALSE;

        comm.input.assign(read_offset, std::byte{});
        comm.output.clear();

        oc::condrv::BasicApiMessage<MemoryComm> message(comm, packet);
        auto outcome = oc::condrv::dispatch_message(state, message, host_io);
        if (!outcome || message.completion().io_status.Status != oc::core::status_success)
        {
            return false;
        }

        if (message.packet().payload.user_defined.u.console_msg_l1.ReadConsole.NumBytes != output_bytes)
        {
            return false;
        }

        if (auto released = message.release_message_buffers(); !released)
        {
            return false;
        }

        if (comm.output.size() != api_size + output_bytes)
        {
            return false;
        }

        wchar_t returned[expected_wchars]{};
        std::memcpy(returned, comm.output.data() + api_size, output_bytes);
        if (!(returned[0] == L'a' && returned[1] == L'c' && returned[2] == L'\r' && returned[3] == L'\n'))
        {
            return false;
        }

        return true;
    }

    bool test_user_defined_read_console_w_line_input_enter_with_cursor_mid_line()
    {
        MemoryComm comm{};
        oc::condrv::ServerState state{};
        TestHostIo host_io{};

        auto connect_packet = make_connect_packet(9227, 9228);
        oc::condrv::BasicApiMessage<MemoryComm> connect_message(comm, connect_packet);
        auto connect_outcome = oc::condrv::dispatch_message(state, connect_message, host_io);
        if (!connect_outcome)
        {
            return false;
        }

        const auto info = unpack_connection_information(connect_message.completion());
        if (!set_input_code_page(comm, state, host_io, info, CP_UTF8))
        {
            return false;
        }

        state.set_input_mode(ENABLE_LINE_INPUT | ENABLE_PROCESSED_INPUT | ENABLE_ECHO_INPUT);
        host_io.input = {
            static_cast<std::byte>('a'),
            static_cast<std::byte>('b'),
            static_cast<std::byte>('c'),
            static_cast<std::byte>(0x1b),
            static_cast<std::byte>('['),
            static_cast<std::byte>('D'),
            static_cast<std::byte>('\r'),
        };

        constexpr ULONG api_size = sizeof(CONSOLE_READCONSOLE_MSG);
        constexpr ULONG header_size = sizeof(CONSOLE_MSG_HEADER);
        constexpr ULONG read_offset = api_size + header_size;
        constexpr size_t expected_wchars = 5; // a b c CRLF
        constexpr ULONG output_bytes = static_cast<ULONG>(expected_wchars * sizeof(wchar_t));

        oc::condrv::IoPacket packet{};
        packet.payload.user_defined = oc::condrv::UserDefinedPacket{};
        packet.descriptor.identifier.LowPart = 104;
        packet.descriptor.function = oc::condrv::console_io_user_defined;
        packet.descriptor.process = info.process;
        packet.descriptor.object = info.input;
        packet.descriptor.input_size = read_offset;
        packet.descriptor.output_size = api_size + output_bytes;
        packet.payload.user_defined.msg_header.ApiNumber = static_cast<ULONG>(ConsolepReadConsole);
        packet.payload.user_defined.msg_header.ApiDescriptorSize = api_size;
        packet.payload.user_defined.u.console_msg_l1.ReadConsole.Unicode = TRUE;
        packet.payload.user_defined.u.console_msg_l1.ReadConsole.ProcessControlZ = FALSE;

        comm.input.assign(read_offset, std::byte{});
        comm.output.clear();

        oc::condrv::BasicApiMessage<MemoryComm> message(comm, packet);
        auto outcome = oc::condrv::dispatch_message(state, message, host_io);
        if (!outcome || message.completion().io_status.Status != oc::core::status_success)
        {
            return false;
        }

        if (message.packet().payload.user_defined.u.console_msg_l1.ReadConsole.NumBytes != output_bytes)
        {
            return false;
        }

        if (auto released = message.release_message_buffers(); !released)
        {
            return false;
        }

        if (comm.output.size() != api_size + output_bytes)
        {
            return false;
        }

        wchar_t returned[expected_wchars]{};
        std::memcpy(returned, comm.output.data() + api_size, output_bytes);
        if (!(returned[0] == L'a' && returned[1] == L'b' && returned[2] == L'c' && returned[3] == L'\r' && returned[4] == L'\n'))
        {
            return false;
        }

        return true;
    }

    bool test_user_defined_read_console_w_line_input_escape_clears_line()
    {
        MemoryComm comm{};
        oc::condrv::ServerState state{};
        TestHostIo host_io{};

        auto connect_packet = make_connect_packet(9229, 9230);
        oc::condrv::BasicApiMessage<MemoryComm> connect_message(comm, connect_packet);
        auto connect_outcome = oc::condrv::dispatch_message(state, connect_message, host_io);
        if (!connect_outcome)
        {
            return false;
        }

        const auto info = unpack_connection_information(connect_message.completion());
        if (!set_input_code_page(comm, state, host_io, info, CP_UTF8))
        {
            return false;
        }

        state.set_input_mode(ENABLE_LINE_INPUT | ENABLE_PROCESSED_INPUT | ENABLE_ECHO_INPUT);
        host_io.input = {
            static_cast<std::byte>('a'),
            static_cast<std::byte>('b'),
            static_cast<std::byte>('c'),
            static_cast<std::byte>(0x1b),
            static_cast<std::byte>('['),
            static_cast<std::byte>('2'),
            static_cast<std::byte>('7'),
            static_cast<std::byte>(';'),
            static_cast<std::byte>('0'),
            static_cast<std::byte>(';'),
            static_cast<std::byte>('2'),
            static_cast<std::byte>('7'),
            static_cast<std::byte>(';'),
            static_cast<std::byte>('1'),
            static_cast<std::byte>(';'),
            static_cast<std::byte>('0'),
            static_cast<std::byte>(';'),
            static_cast<std::byte>('1'),
            static_cast<std::byte>('_'),
            static_cast<std::byte>('X'),
            static_cast<std::byte>('\r'),
        };

        constexpr ULONG api_size = sizeof(CONSOLE_READCONSOLE_MSG);
        constexpr ULONG header_size = sizeof(CONSOLE_MSG_HEADER);
        constexpr ULONG read_offset = api_size + header_size;
        constexpr size_t expected_wchars = 3; // X CRLF
        constexpr ULONG output_bytes = static_cast<ULONG>(expected_wchars * sizeof(wchar_t));

        oc::condrv::IoPacket packet{};
        packet.payload.user_defined = oc::condrv::UserDefinedPacket{};
        packet.descriptor.identifier.LowPart = 106;
        packet.descriptor.function = oc::condrv::console_io_user_defined;
        packet.descriptor.process = info.process;
        packet.descriptor.object = info.input;
        packet.descriptor.input_size = read_offset;
        packet.descriptor.output_size = api_size + output_bytes;
        packet.payload.user_defined.msg_header.ApiNumber = static_cast<ULONG>(ConsolepReadConsole);
        packet.payload.user_defined.msg_header.ApiDescriptorSize = api_size;
        packet.payload.user_defined.u.console_msg_l1.ReadConsole.Unicode = TRUE;
        packet.payload.user_defined.u.console_msg_l1.ReadConsole.ProcessControlZ = FALSE;

        comm.input.assign(read_offset, std::byte{});
        comm.output.clear();

        oc::condrv::BasicApiMessage<MemoryComm> message(comm, packet);
        auto outcome = oc::condrv::dispatch_message(state, message, host_io);
        if (!outcome || message.completion().io_status.Status != oc::core::status_success)
        {
            return false;
        }

        if (message.packet().payload.user_defined.u.console_msg_l1.ReadConsole.NumBytes != output_bytes)
        {
            return false;
        }

        if (auto released = message.release_message_buffers(); !released)
        {
            return false;
        }

        if (comm.output.size() != api_size + output_bytes)
        {
            return false;
        }

        wchar_t returned[expected_wchars]{};
        std::memcpy(returned, comm.output.data() + api_size, output_bytes);
        if (!(returned[0] == L'X' && returned[1] == L'\r' && returned[2] == L'\n'))
        {
            return false;
        }

        return true;
    }

    bool test_user_defined_read_console_w_line_input_ctrl_home_deletes_to_start()
    {
        MemoryComm comm{};
        oc::condrv::ServerState state{};
        TestHostIo host_io{};

        auto connect_packet = make_connect_packet(9231, 9232);
        oc::condrv::BasicApiMessage<MemoryComm> connect_message(comm, connect_packet);
        auto connect_outcome = oc::condrv::dispatch_message(state, connect_message, host_io);
        if (!connect_outcome)
        {
            return false;
        }

        const auto info = unpack_connection_information(connect_message.completion());
        if (!set_input_code_page(comm, state, host_io, info, CP_UTF8))
        {
            return false;
        }

        state.set_input_mode(ENABLE_LINE_INPUT | ENABLE_PROCESSED_INPUT | ENABLE_ECHO_INPUT);
        host_io.input = {
            static_cast<std::byte>('a'),
            static_cast<std::byte>('b'),
            static_cast<std::byte>('c'),
            static_cast<std::byte>(0x1b),
            static_cast<std::byte>('['),
            static_cast<std::byte>('D'),
            static_cast<std::byte>(0x1b),
            static_cast<std::byte>('['),
            static_cast<std::byte>('3'),
            static_cast<std::byte>('6'),
            static_cast<std::byte>(';'),
            static_cast<std::byte>('0'),
            static_cast<std::byte>(';'),
            static_cast<std::byte>('0'),
            static_cast<std::byte>(';'),
            static_cast<std::byte>('1'),
            static_cast<std::byte>(';'),
            static_cast<std::byte>('8'),
            static_cast<std::byte>(';'),
            static_cast<std::byte>('1'),
            static_cast<std::byte>('_'),
            static_cast<std::byte>('X'),
            static_cast<std::byte>('\r'),
        };

        constexpr ULONG api_size = sizeof(CONSOLE_READCONSOLE_MSG);
        constexpr ULONG header_size = sizeof(CONSOLE_MSG_HEADER);
        constexpr ULONG read_offset = api_size + header_size;
        constexpr size_t expected_wchars = 4; // X c CRLF
        constexpr ULONG output_bytes = static_cast<ULONG>(expected_wchars * sizeof(wchar_t));

        oc::condrv::IoPacket packet{};
        packet.payload.user_defined = oc::condrv::UserDefinedPacket{};
        packet.descriptor.identifier.LowPart = 108;
        packet.descriptor.function = oc::condrv::console_io_user_defined;
        packet.descriptor.process = info.process;
        packet.descriptor.object = info.input;
        packet.descriptor.input_size = read_offset;
        packet.descriptor.output_size = api_size + output_bytes;
        packet.payload.user_defined.msg_header.ApiNumber = static_cast<ULONG>(ConsolepReadConsole);
        packet.payload.user_defined.msg_header.ApiDescriptorSize = api_size;
        packet.payload.user_defined.u.console_msg_l1.ReadConsole.Unicode = TRUE;
        packet.payload.user_defined.u.console_msg_l1.ReadConsole.ProcessControlZ = FALSE;

        comm.input.assign(read_offset, std::byte{});
        comm.output.clear();

        oc::condrv::BasicApiMessage<MemoryComm> message(comm, packet);
        auto outcome = oc::condrv::dispatch_message(state, message, host_io);
        if (!outcome || message.completion().io_status.Status != oc::core::status_success)
        {
            return false;
        }

        if (message.packet().payload.user_defined.u.console_msg_l1.ReadConsole.NumBytes != output_bytes)
        {
            return false;
        }

        if (auto released = message.release_message_buffers(); !released)
        {
            return false;
        }

        if (comm.output.size() != api_size + output_bytes)
        {
            return false;
        }

        wchar_t returned[expected_wchars]{};
        std::memcpy(returned, comm.output.data() + api_size, output_bytes);
        if (!(returned[0] == L'X' && returned[1] == L'c' && returned[2] == L'\r' && returned[3] == L'\n'))
        {
            return false;
        }

        return true;
    }

    bool test_user_defined_read_console_w_line_input_ctrl_end_deletes_to_end()
    {
        MemoryComm comm{};
        oc::condrv::ServerState state{};
        TestHostIo host_io{};

        auto connect_packet = make_connect_packet(9233, 9234);
        oc::condrv::BasicApiMessage<MemoryComm> connect_message(comm, connect_packet);
        auto connect_outcome = oc::condrv::dispatch_message(state, connect_message, host_io);
        if (!connect_outcome)
        {
            return false;
        }

        const auto info = unpack_connection_information(connect_message.completion());
        if (!set_input_code_page(comm, state, host_io, info, CP_UTF8))
        {
            return false;
        }

        state.set_input_mode(ENABLE_LINE_INPUT | ENABLE_PROCESSED_INPUT | ENABLE_ECHO_INPUT);
        host_io.input = {
            static_cast<std::byte>('a'),
            static_cast<std::byte>('b'),
            static_cast<std::byte>('c'),
            static_cast<std::byte>(0x1b),
            static_cast<std::byte>('['),
            static_cast<std::byte>('D'),
            static_cast<std::byte>(0x1b),
            static_cast<std::byte>('['),
            static_cast<std::byte>('3'),
            static_cast<std::byte>('5'),
            static_cast<std::byte>(';'),
            static_cast<std::byte>('0'),
            static_cast<std::byte>(';'),
            static_cast<std::byte>('0'),
            static_cast<std::byte>(';'),
            static_cast<std::byte>('1'),
            static_cast<std::byte>(';'),
            static_cast<std::byte>('8'),
            static_cast<std::byte>(';'),
            static_cast<std::byte>('1'),
            static_cast<std::byte>('_'),
            static_cast<std::byte>('X'),
            static_cast<std::byte>('\r'),
        };

        constexpr ULONG api_size = sizeof(CONSOLE_READCONSOLE_MSG);
        constexpr ULONG header_size = sizeof(CONSOLE_MSG_HEADER);
        constexpr ULONG read_offset = api_size + header_size;
        constexpr size_t expected_wchars = 5; // a b X CRLF
        constexpr ULONG output_bytes = static_cast<ULONG>(expected_wchars * sizeof(wchar_t));

        oc::condrv::IoPacket packet{};
        packet.payload.user_defined = oc::condrv::UserDefinedPacket{};
        packet.descriptor.identifier.LowPart = 110;
        packet.descriptor.function = oc::condrv::console_io_user_defined;
        packet.descriptor.process = info.process;
        packet.descriptor.object = info.input;
        packet.descriptor.input_size = read_offset;
        packet.descriptor.output_size = api_size + output_bytes;
        packet.payload.user_defined.msg_header.ApiNumber = static_cast<ULONG>(ConsolepReadConsole);
        packet.payload.user_defined.msg_header.ApiDescriptorSize = api_size;
        packet.payload.user_defined.u.console_msg_l1.ReadConsole.Unicode = TRUE;
        packet.payload.user_defined.u.console_msg_l1.ReadConsole.ProcessControlZ = FALSE;

        comm.input.assign(read_offset, std::byte{});
        comm.output.clear();

        oc::condrv::BasicApiMessage<MemoryComm> message(comm, packet);
        auto outcome = oc::condrv::dispatch_message(state, message, host_io);
        if (!outcome || message.completion().io_status.Status != oc::core::status_success)
        {
            return false;
        }

        if (message.packet().payload.user_defined.u.console_msg_l1.ReadConsole.NumBytes != output_bytes)
        {
            return false;
        }

        if (auto released = message.release_message_buffers(); !released)
        {
            return false;
        }

        if (comm.output.size() != api_size + output_bytes)
        {
            return false;
        }

        wchar_t returned[expected_wchars]{};
        std::memcpy(returned, comm.output.data() + api_size, output_bytes);
        if (!(returned[0] == L'a' && returned[1] == L'b' && returned[2] == L'X' && returned[3] == L'\r' && returned[4] == L'\n'))
        {
            return false;
        }

        return true;
    }

    bool test_l1_get_console_input_utf8_decodes_to_unicode_records()
    {
        MemoryComm comm{};
        oc::condrv::ServerState state{};
        TestHostIo host_io{};

        auto connect_packet = make_connect_packet(13003, 13004);
        oc::condrv::BasicApiMessage<MemoryComm> connect_message(comm, connect_packet);
        auto connect_outcome = oc::condrv::dispatch_message(state, connect_message, host_io);
        if (!connect_outcome)
        {
            return false;
        }

        const auto info = unpack_connection_information(connect_message.completion());
        if (!set_input_code_page(comm, state, host_io, info, CP_UTF8))
        {
            return false;
        }

        host_io.input = { static_cast<std::byte>(0xC3), static_cast<std::byte>(0xA9) }; // U+00E9

        constexpr ULONG api_size = sizeof(CONSOLE_GETCONSOLEINPUT_MSG);
        constexpr ULONG header_size = sizeof(CONSOLE_MSG_HEADER);
        constexpr ULONG read_offset = api_size + header_size;

        oc::condrv::IoPacket packet{};
        packet.payload.user_defined = oc::condrv::UserDefinedPacket{};
        packet.descriptor.identifier.LowPart = 212;
        packet.descriptor.function = oc::condrv::console_io_user_defined;
        packet.descriptor.process = info.process;
        packet.descriptor.object = info.input;
        packet.descriptor.input_size = read_offset;
        packet.descriptor.output_size = api_size + static_cast<ULONG>(sizeof(INPUT_RECORD));
        packet.payload.user_defined.msg_header.ApiNumber = static_cast<ULONG>(ConsolepGetConsoleInput);
        packet.payload.user_defined.msg_header.ApiDescriptorSize = api_size;

        auto& body = packet.payload.user_defined.u.console_msg_l1.GetConsoleInput;
        body.NumRecords = 0;
        body.Flags = 0;
        body.Unicode = TRUE;

        comm.input.assign(read_offset, std::byte{});

        oc::condrv::BasicApiMessage<MemoryComm> message(comm, packet);
        auto outcome = oc::condrv::dispatch_message(state, message, host_io);
        if (!outcome)
        {
            return false;
        }

        if (message.completion().io_status.Status != oc::core::status_success)
        {
            return false;
        }

        if (message.packet().payload.user_defined.u.console_msg_l1.GetConsoleInput.NumRecords != 1)
        {
            return false;
        }

        if (auto released = message.release_message_buffers(); !released)
        {
            return false;
        }

        INPUT_RECORD record{};
        std::memcpy(&record, comm.output.data() + api_size, sizeof(record));
        return record.EventType == KEY_EVENT &&
               record.Event.KeyEvent.uChar.UnicodeChar == static_cast<wchar_t>(0x00E9) &&
               host_io.input_bytes_available() == 0;
    }

    bool test_l1_get_console_input_utf8_surrogate_pair_splits_across_reads()
    {
        MemoryComm comm{};
        oc::condrv::ServerState state{};
        TestHostIo host_io{};

        auto connect_packet = make_connect_packet(13005, 13006);
        oc::condrv::BasicApiMessage<MemoryComm> connect_message(comm, connect_packet);
        auto connect_outcome = oc::condrv::dispatch_message(state, connect_message, host_io);
        if (!connect_outcome)
        {
            return false;
        }

        const auto info = unpack_connection_information(connect_message.completion());
        if (!set_input_code_page(comm, state, host_io, info, CP_UTF8))
        {
            return false;
        }

        // U+1F600 GRINNING FACE: UTF-8 F0 9F 98 80, UTF-16 D83D DE00.
        host_io.input = {
            static_cast<std::byte>(0xF0),
            static_cast<std::byte>(0x9F),
            static_cast<std::byte>(0x98),
            static_cast<std::byte>(0x80),
        };

        constexpr ULONG api_size = sizeof(CONSOLE_GETCONSOLEINPUT_MSG);
        constexpr ULONG header_size = sizeof(CONSOLE_MSG_HEADER);
        constexpr ULONG read_offset = api_size + header_size;

        const auto read_one = [&](const ULONG identifier) -> std::optional<wchar_t> {
            oc::condrv::IoPacket packet{};
            packet.payload.user_defined = oc::condrv::UserDefinedPacket{};
            packet.descriptor.identifier.LowPart = identifier;
            packet.descriptor.function = oc::condrv::console_io_user_defined;
            packet.descriptor.process = info.process;
            packet.descriptor.object = info.input;
            packet.descriptor.input_size = read_offset;
            packet.descriptor.output_size = api_size + static_cast<ULONG>(sizeof(INPUT_RECORD)); // room for 1 record
            packet.payload.user_defined.msg_header.ApiNumber = static_cast<ULONG>(ConsolepGetConsoleInput);
            packet.payload.user_defined.msg_header.ApiDescriptorSize = api_size;

            auto& body = packet.payload.user_defined.u.console_msg_l1.GetConsoleInput;
            body.NumRecords = 0;
            body.Flags = 0;
            body.Unicode = TRUE;

            comm.input.assign(read_offset, std::byte{});
            comm.output.clear();

            oc::condrv::BasicApiMessage<MemoryComm> message(comm, packet);
            auto outcome = oc::condrv::dispatch_message(state, message, host_io);
            if (!outcome || message.completion().io_status.Status != oc::core::status_success)
            {
                return std::nullopt;
            }

            if (message.packet().payload.user_defined.u.console_msg_l1.GetConsoleInput.NumRecords != 1)
            {
                return std::nullopt;
            }

            if (auto released = message.release_message_buffers(); !released)
            {
                return std::nullopt;
            }

            if (comm.output.size() != api_size + sizeof(INPUT_RECORD))
            {
                return std::nullopt;
            }

            INPUT_RECORD record{};
            std::memcpy(&record, comm.output.data() + api_size, sizeof(record));
            if (record.EventType != KEY_EVENT)
            {
                return std::nullopt;
            }

            return record.Event.KeyEvent.uChar.UnicodeChar;
        };

        const auto get_ready_events = [&](const ULONG identifier) -> std::optional<ULONG> {
            constexpr ULONG events_api_size = sizeof(CONSOLE_GETNUMBEROFINPUTEVENTS_MSG);
            constexpr ULONG events_read_offset = events_api_size + header_size;

            oc::condrv::IoPacket packet{};
            packet.payload.user_defined = oc::condrv::UserDefinedPacket{};
            packet.descriptor.identifier.LowPart = identifier;
            packet.descriptor.function = oc::condrv::console_io_user_defined;
            packet.descriptor.process = info.process;
            packet.descriptor.object = info.input;
            packet.descriptor.input_size = events_read_offset;
            packet.descriptor.output_size = events_api_size;
            packet.payload.user_defined.msg_header.ApiNumber = static_cast<ULONG>(ConsolepGetNumberOfInputEvents);
            packet.payload.user_defined.msg_header.ApiDescriptorSize = events_api_size;

            comm.input.assign(events_read_offset, std::byte{});
            oc::condrv::BasicApiMessage<MemoryComm> message(comm, packet);
            auto outcome = oc::condrv::dispatch_message(state, message, host_io);
            if (!outcome || message.completion().io_status.Status != oc::core::status_success)
            {
                return std::nullopt;
            }

            return message.packet().payload.user_defined.u.console_msg_l1.GetNumberOfConsoleInputEvents.ReadyEvents;
        };

        const auto first = read_one(213);
        if (!first || *first != static_cast<wchar_t>(0xD83D))
        {
            return false;
        }

        if (host_io.input_bytes_available() != 0)
        {
            return false;
        }

        const auto ready_after_first = get_ready_events(240);
        if (!ready_after_first || *ready_after_first != 1)
        {
            return false;
        }

        const auto second = read_one(214);
        if (!second || *second != static_cast<wchar_t>(0xDE00))
        {
            return false;
        }

        const auto ready_after_second = get_ready_events(241);
        return ready_after_second && *ready_after_second == 0;
    }

    bool test_l1_get_console_input_peek_does_not_consume()
    {
        MemoryComm comm{};
        oc::condrv::ServerState state{};
        TestHostIo host_io{};

        auto connect_packet = make_connect_packet(12001, 12002);
        oc::condrv::BasicApiMessage<MemoryComm> connect_message(comm, connect_packet);
        auto connect_outcome = oc::condrv::dispatch_message(state, connect_message, host_io);
        if (!connect_outcome)
        {
            return false;
        }

        auto info = unpack_connection_information(connect_message.completion());
        host_io.input = { static_cast<std::byte>('A'), static_cast<std::byte>('B') };

        auto make_packet = [&](const USHORT flags, const ULONG id) {
            constexpr ULONG api_size = sizeof(CONSOLE_GETCONSOLEINPUT_MSG);
            constexpr ULONG header_size = sizeof(CONSOLE_MSG_HEADER);
            constexpr ULONG read_offset = api_size + header_size;

            oc::condrv::IoPacket packet{};
            packet.payload.user_defined = oc::condrv::UserDefinedPacket{};
            packet.descriptor.identifier.LowPart = id;
            packet.descriptor.function = oc::condrv::console_io_user_defined;
            packet.descriptor.process = info.process;
            packet.descriptor.object = info.input;
            packet.descriptor.input_size = read_offset;
            packet.descriptor.output_size = api_size + static_cast<ULONG>(2 * sizeof(INPUT_RECORD));
            packet.payload.user_defined.msg_header.ApiNumber = static_cast<ULONG>(ConsolepGetConsoleInput);
            packet.payload.user_defined.msg_header.ApiDescriptorSize = api_size;

            auto& body = packet.payload.user_defined.u.console_msg_l1.GetConsoleInput;
            body.NumRecords = 0;
            body.Flags = flags;
            body.Unicode = TRUE;
            return packet;
        };

        comm.input.assign(sizeof(CONSOLE_GETCONSOLEINPUT_MSG) + sizeof(CONSOLE_MSG_HEADER), std::byte{});

        auto peek_packet = make_packet(CONSOLE_READ_NOREMOVE, 200);
        oc::condrv::BasicApiMessage<MemoryComm> peek_message(comm, peek_packet);
        auto peek_outcome = oc::condrv::dispatch_message(state, peek_message, host_io);
        if (!peek_outcome)
        {
            return false;
        }
        if (peek_message.completion().io_status.Status != oc::core::status_success)
        {
            return false;
        }
        if (peek_message.packet().payload.user_defined.u.console_msg_l1.GetConsoleInput.NumRecords != 2)
        {
            return false;
        }

        if (auto released = peek_message.release_message_buffers(); !released)
        {
            return false;
        }

        if (comm.output.size() != sizeof(CONSOLE_GETCONSOLEINPUT_MSG) + 2 * sizeof(INPUT_RECORD))
        {
            return false;
        }

        INPUT_RECORD first{};
        std::memcpy(&first, comm.output.data() + sizeof(CONSOLE_GETCONSOLEINPUT_MSG), sizeof(first));
        if (first.EventType != KEY_EVENT || first.Event.KeyEvent.uChar.UnicodeChar != L'A')
        {
            return false;
        }

        // Peek again and confirm it still returns the same first character (not consumed).
        comm.output.clear();
        auto peek_again_packet = make_packet(CONSOLE_READ_NOREMOVE, 201);
        oc::condrv::BasicApiMessage<MemoryComm> peek_again(comm, peek_again_packet);
        auto peek_again_outcome = oc::condrv::dispatch_message(state, peek_again, host_io);
        if (!peek_again_outcome)
        {
            return false;
        }
        if (peek_again.packet().payload.user_defined.u.console_msg_l1.GetConsoleInput.NumRecords != 2)
        {
            return false;
        }

        if (auto released = peek_again.release_message_buffers(); !released)
        {
            return false;
        }

        INPUT_RECORD first_again{};
        std::memcpy(&first_again, comm.output.data() + sizeof(CONSOLE_GETCONSOLEINPUT_MSG), sizeof(first_again));
        return first_again.EventType == KEY_EVENT && first_again.Event.KeyEvent.uChar.UnicodeChar == L'A';
    }

    bool test_l1_get_console_input_remove_consumes_bytes()
    {
        MemoryComm comm{};
        oc::condrv::ServerState state{};
        TestHostIo host_io{};

        auto connect_packet = make_connect_packet(13001, 13002);
        oc::condrv::BasicApiMessage<MemoryComm> connect_message(comm, connect_packet);
        auto connect_outcome = oc::condrv::dispatch_message(state, connect_message, host_io);
        if (!connect_outcome)
        {
            return false;
        }

        auto info = unpack_connection_information(connect_message.completion());
        host_io.input = { static_cast<std::byte>('C'), static_cast<std::byte>('D') };

        constexpr ULONG api_size = sizeof(CONSOLE_GETCONSOLEINPUT_MSG);
        constexpr ULONG header_size = sizeof(CONSOLE_MSG_HEADER);
        constexpr ULONG read_offset = api_size + header_size;

        auto make_packet = [&](const ULONG id) {
            oc::condrv::IoPacket packet{};
            packet.payload.user_defined = oc::condrv::UserDefinedPacket{};
            packet.descriptor.identifier.LowPart = id;
            packet.descriptor.function = oc::condrv::console_io_user_defined;
            packet.descriptor.process = info.process;
            packet.descriptor.object = info.input;
            packet.descriptor.input_size = read_offset;
            packet.descriptor.output_size = api_size + static_cast<ULONG>(sizeof(INPUT_RECORD));
            packet.payload.user_defined.msg_header.ApiNumber = static_cast<ULONG>(ConsolepGetConsoleInput);
            packet.payload.user_defined.msg_header.ApiDescriptorSize = api_size;

            auto& body = packet.payload.user_defined.u.console_msg_l1.GetConsoleInput;
            body.NumRecords = 0;
            body.Flags = 0;
            body.Unicode = TRUE;
            return packet;
        };

        comm.input.assign(read_offset, std::byte{});

        // First read consumes 'C'.
        auto first_packet = make_packet(210);
        oc::condrv::BasicApiMessage<MemoryComm> first(comm, first_packet);
        auto first_outcome = oc::condrv::dispatch_message(state, first, host_io);
        if (!first_outcome)
        {
            return false;
        }
        if (first.packet().payload.user_defined.u.console_msg_l1.GetConsoleInput.NumRecords != 1)
        {
            return false;
        }
        if (auto released = first.release_message_buffers(); !released)
        {
            return false;
        }

        INPUT_RECORD record{};
        std::memcpy(&record, comm.output.data() + api_size, sizeof(record));
        if (record.Event.KeyEvent.uChar.UnicodeChar != L'C')
        {
            return false;
        }

        // Second read consumes 'D'.
        comm.output.clear();
        auto second_packet = make_packet(211);
        oc::condrv::BasicApiMessage<MemoryComm> second(comm, second_packet);
        auto second_outcome = oc::condrv::dispatch_message(state, second, host_io);
        if (!second_outcome)
        {
            return false;
        }
        if (second.packet().payload.user_defined.u.console_msg_l1.GetConsoleInput.NumRecords != 1)
        {
            return false;
        }
        if (auto released = second.release_message_buffers(); !released)
        {
            return false;
        }

        INPUT_RECORD record2{};
        std::memcpy(&record2, comm.output.data() + api_size, sizeof(record2));
        return record2.Event.KeyEvent.uChar.UnicodeChar == L'D' && host_io.input_bytes_available() == 0;
    }

    bool test_l1_get_console_input_processed_input_skips_ctrl_c_on_remove_and_still_fills_records()
    {
        MemoryComm comm{};
        oc::condrv::ServerState state{};
        TestHostIo host_io{};

        auto connect_packet = make_connect_packet(13005, 13006);
        oc::condrv::BasicApiMessage<MemoryComm> connect_message(comm, connect_packet);
        auto connect_outcome = oc::condrv::dispatch_message(state, connect_message, host_io);
        if (!connect_outcome)
        {
            return false;
        }

        const auto info = unpack_connection_information(connect_message.completion());
        state.set_input_mode(ENABLE_PROCESSED_INPUT);
        host_io.input = { static_cast<std::byte>('X'), static_cast<std::byte>(0x03), static_cast<std::byte>('Y') };

        constexpr ULONG api_size = sizeof(CONSOLE_GETCONSOLEINPUT_MSG);
        constexpr ULONG header_size = sizeof(CONSOLE_MSG_HEADER);
        constexpr ULONG read_offset = api_size + header_size;
        constexpr ULONG record_bytes = static_cast<ULONG>(2 * sizeof(INPUT_RECORD));

        oc::condrv::IoPacket packet{};
        packet.payload.user_defined = oc::condrv::UserDefinedPacket{};
        packet.descriptor.identifier.LowPart = 213;
        packet.descriptor.function = oc::condrv::console_io_user_defined;
        packet.descriptor.process = info.process;
        packet.descriptor.object = info.input;
        packet.descriptor.input_size = read_offset;
        packet.descriptor.output_size = api_size + record_bytes;
        packet.payload.user_defined.msg_header.ApiNumber = static_cast<ULONG>(ConsolepGetConsoleInput);
        packet.payload.user_defined.msg_header.ApiDescriptorSize = api_size;

        auto& body = packet.payload.user_defined.u.console_msg_l1.GetConsoleInput;
        body.NumRecords = 0;
        body.Flags = 0; // remove + wait allowed
        body.Unicode = TRUE;

        comm.input.assign(read_offset, std::byte{});
        comm.output.clear();

        oc::condrv::BasicApiMessage<MemoryComm> message(comm, packet);
        auto outcome = oc::condrv::dispatch_message(state, message, host_io);
        if (!outcome || message.completion().io_status.Status != oc::core::status_success)
        {
            return false;
        }

        if (message.packet().payload.user_defined.u.console_msg_l1.GetConsoleInput.NumRecords != 2)
        {
            return false;
        }

        if (host_io.end_task_pids.size() != 1 || host_io.end_task_pids[0] != 13005)
        {
            return false;
        }

        if (auto released = message.release_message_buffers(); !released)
        {
            return false;
        }

        if (comm.output.size() != api_size + record_bytes)
        {
            return false;
        }

        INPUT_RECORD first{};
        INPUT_RECORD second{};
        std::memcpy(&first, comm.output.data() + api_size, sizeof(first));
        std::memcpy(&second, comm.output.data() + api_size + sizeof(first), sizeof(second));

        return first.EventType == KEY_EVENT &&
               second.EventType == KEY_EVENT &&
               first.Event.KeyEvent.uChar.UnicodeChar == L'X' &&
               second.Event.KeyEvent.uChar.UnicodeChar == L'Y' &&
               host_io.input_bytes_available() == 0;
    }

    bool test_l1_get_console_input_processed_input_ctrl_break_flushes_and_reply_pends()
    {
        MemoryComm comm{};
        oc::condrv::ServerState state{};
        TestHostIo host_io{};

        auto connect_packet = make_connect_packet(13009, 13010);
        oc::condrv::BasicApiMessage<MemoryComm> connect_message(comm, connect_packet);
        auto connect_outcome = oc::condrv::dispatch_message(state, connect_message, host_io);
        if (!connect_outcome)
        {
            return false;
        }

        const auto info = unpack_connection_information(connect_message.completion());
        state.set_input_mode(ENABLE_PROCESSED_INPUT);
        state.set_input_code_page(CP_UTF8);

        constexpr std::string_view ctrl_break = "\x1b[3;0;0;1;8;1_";
        host_io.input.clear();
        host_io.input_offset = 0;
        for (const unsigned char ch : ctrl_break)
        {
            host_io.input.push_back(static_cast<std::byte>(ch));
        }

        constexpr ULONG api_size = sizeof(CONSOLE_GETCONSOLEINPUT_MSG);
        constexpr ULONG header_size = sizeof(CONSOLE_MSG_HEADER);
        constexpr ULONG read_offset = api_size + header_size;

        oc::condrv::IoPacket packet{};
        packet.payload.user_defined = oc::condrv::UserDefinedPacket{};
        packet.descriptor.identifier.LowPart = 215;
        packet.descriptor.function = oc::condrv::console_io_user_defined;
        packet.descriptor.process = info.process;
        packet.descriptor.object = info.input;
        packet.descriptor.input_size = read_offset;
        packet.descriptor.output_size = api_size + static_cast<ULONG>(sizeof(INPUT_RECORD));
        packet.payload.user_defined.msg_header.ApiNumber = static_cast<ULONG>(ConsolepGetConsoleInput);
        packet.payload.user_defined.msg_header.ApiDescriptorSize = api_size;

        auto& body = packet.payload.user_defined.u.console_msg_l1.GetConsoleInput;
        body.NumRecords = 0;
        body.Flags = 0; // remove + wait allowed
        body.Unicode = TRUE;

        comm.input.assign(read_offset, std::byte{});
        comm.output.clear();

        oc::condrv::BasicApiMessage<MemoryComm> message(comm, packet);
        auto outcome = oc::condrv::dispatch_message(state, message, host_io);
        if (!outcome || !outcome->reply_pending)
        {
            return false;
        }

        if (host_io.input_bytes_available() != 0)
        {
            return false;
        }

        if (host_io.end_task_pids.size() != 1 || host_io.end_task_pids[0] != 13009)
        {
            return false;
        }

        if (host_io.end_task_events.size() != 1 || host_io.end_task_events[0] != CTRL_BREAK_EVENT)
        {
            return false;
        }

        if (host_io.end_task_flags.size() != 1 || host_io.end_task_flags[0] != oc::core::console_ctrl_break_flag)
        {
            return false;
        }

        const std::array<std::byte, 1> next{ static_cast<std::byte>('Z') };
        if (!host_io.inject_input_bytes(next))
        {
            return false;
        }

        outcome = oc::condrv::dispatch_message(state, message, host_io);
        if (!outcome || outcome->reply_pending || message.completion().io_status.Status != oc::core::status_success)
        {
            return false;
        }

        if (message.packet().payload.user_defined.u.console_msg_l1.GetConsoleInput.NumRecords != 1)
        {
            return false;
        }

        if (auto released = message.release_message_buffers(); !released)
        {
            return false;
        }

        if (comm.output.size() != api_size + sizeof(INPUT_RECORD))
        {
            return false;
        }

        INPUT_RECORD record{};
        std::memcpy(&record, comm.output.data() + api_size, sizeof(record));
        return record.EventType == KEY_EVENT &&
               record.Event.KeyEvent.uChar.UnicodeChar == L'Z' &&
               host_io.input_bytes_available() == 0;
    }

    bool test_l1_get_console_input_processed_input_skips_ctrl_c_on_peek_and_still_fills_records()
    {
        MemoryComm comm{};
        oc::condrv::ServerState state{};
        TestHostIo host_io{};

        auto connect_packet = make_connect_packet(13007, 13008);
        oc::condrv::BasicApiMessage<MemoryComm> connect_message(comm, connect_packet);
        auto connect_outcome = oc::condrv::dispatch_message(state, connect_message, host_io);
        if (!connect_outcome)
        {
            return false;
        }

        const auto info = unpack_connection_information(connect_message.completion());
        state.set_input_mode(ENABLE_PROCESSED_INPUT);
        host_io.input = { static_cast<std::byte>('X'), static_cast<std::byte>(0x03), static_cast<std::byte>('Y') };

        constexpr ULONG api_size = sizeof(CONSOLE_GETCONSOLEINPUT_MSG);
        constexpr ULONG header_size = sizeof(CONSOLE_MSG_HEADER);
        constexpr ULONG read_offset = api_size + header_size;
        constexpr ULONG record_bytes = static_cast<ULONG>(2 * sizeof(INPUT_RECORD));

        oc::condrv::IoPacket packet{};
        packet.payload.user_defined = oc::condrv::UserDefinedPacket{};
        packet.descriptor.identifier.LowPart = 214;
        packet.descriptor.function = oc::condrv::console_io_user_defined;
        packet.descriptor.process = info.process;
        packet.descriptor.object = info.input;
        packet.descriptor.input_size = read_offset;
        packet.descriptor.output_size = api_size + record_bytes;
        packet.payload.user_defined.msg_header.ApiNumber = static_cast<ULONG>(ConsolepGetConsoleInput);
        packet.payload.user_defined.msg_header.ApiDescriptorSize = api_size;

        auto& body = packet.payload.user_defined.u.console_msg_l1.GetConsoleInput;
        body.NumRecords = 0;
        body.Flags = CONSOLE_READ_NOREMOVE | CONSOLE_READ_NOWAIT;
        body.Unicode = TRUE;

        comm.input.assign(read_offset, std::byte{});
        comm.output.clear();

        oc::condrv::BasicApiMessage<MemoryComm> message(comm, packet);
        auto outcome = oc::condrv::dispatch_message(state, message, host_io);
        if (!outcome || message.completion().io_status.Status != oc::core::status_success)
        {
            return false;
        }

        if (message.packet().payload.user_defined.u.console_msg_l1.GetConsoleInput.NumRecords != 2)
        {
            return false;
        }

        if (!host_io.end_task_pids.empty())
        {
            return false;
        }

        if (auto released = message.release_message_buffers(); !released)
        {
            return false;
        }

        if (comm.output.size() != api_size + record_bytes)
        {
            return false;
        }

        INPUT_RECORD first{};
        INPUT_RECORD second{};
        std::memcpy(&first, comm.output.data() + api_size, sizeof(first));
        std::memcpy(&second, comm.output.data() + api_size + sizeof(first), sizeof(second));

        return first.EventType == KEY_EVENT &&
               second.EventType == KEY_EVENT &&
               first.Event.KeyEvent.uChar.UnicodeChar == L'X' &&
               second.Event.KeyEvent.uChar.UnicodeChar == L'Y' &&
               host_io.input_bytes_available() == 3;
    }

    bool test_l2_write_console_input_injects_bytes()
    {
        MemoryComm comm{};
        oc::condrv::ServerState state{};
        TestHostIo host_io{};

        auto connect_packet = make_connect_packet(14001, 14002);
        oc::condrv::BasicApiMessage<MemoryComm> connect_message(comm, connect_packet);
        auto connect_outcome = oc::condrv::dispatch_message(state, connect_message, host_io);
        if (!connect_outcome)
        {
            fwprintf(stderr, L"[condrv raw] connect_outcome was unexpected\n");
            return false;
        }

        auto info = unpack_connection_information(connect_message.completion());
        host_io.input = { static_cast<std::byte>('x') };

        INPUT_RECORD records[2]{};
        records[0].EventType = KEY_EVENT;
        records[0].Event.KeyEvent.bKeyDown = TRUE;
        records[0].Event.KeyEvent.wRepeatCount = 1;
        records[0].Event.KeyEvent.uChar.UnicodeChar = L'Q';

        records[1].EventType = KEY_EVENT;
        records[1].Event.KeyEvent.bKeyDown = TRUE;
        records[1].Event.KeyEvent.wRepeatCount = 2;
        records[1].Event.KeyEvent.uChar.UnicodeChar = L'R';

        constexpr ULONG api_size = sizeof(CONSOLE_WRITECONSOLEINPUT_MSG);
        constexpr ULONG header_size = sizeof(CONSOLE_MSG_HEADER);
        constexpr ULONG read_offset = api_size + header_size;
        constexpr ULONG input_bytes = static_cast<ULONG>(sizeof(records));

        oc::condrv::IoPacket packet{};
        packet.payload.user_defined = oc::condrv::UserDefinedPacket{};
        packet.descriptor.identifier.LowPart = 220;
        packet.descriptor.function = oc::condrv::console_io_user_defined;
        packet.descriptor.process = info.process;
        packet.descriptor.object = info.input;
        packet.descriptor.input_size = read_offset + input_bytes;
        packet.descriptor.output_size = api_size;
        packet.payload.user_defined.msg_header.ApiNumber = static_cast<ULONG>(ConsolepWriteConsoleInput);
        packet.payload.user_defined.msg_header.ApiDescriptorSize = api_size;

        packet.payload.user_defined.u.console_msg_l2.WriteConsoleInput.Unicode = TRUE;
        packet.payload.user_defined.u.console_msg_l2.WriteConsoleInput.Append = FALSE;

        comm.input.assign(packet.descriptor.input_size, std::byte{});
        std::memcpy(comm.input.data() + read_offset, records, sizeof(records));

        oc::condrv::BasicApiMessage<MemoryComm> message(comm, packet);
        auto outcome = oc::condrv::dispatch_message(state, message, host_io);
        if (!outcome)
        {
            fwprintf(stderr, L"[condrv raw] write_console_input outcome was unexpected\n");
            return false;
        }
        if (message.completion().io_status.Status != oc::core::status_success)
        {
            fwprintf(stderr, L"[condrv raw] write_console_input status was 0x%08X\n", static_cast<unsigned int>(message.completion().io_status.Status));
            return false;
        }
        if (message.packet().payload.user_defined.u.console_msg_l2.WriteConsoleInput.NumRecords != 2)
        {
            fwprintf(stderr, L"[condrv raw] write_console_input NumRecords was %lu\n",
                message.packet().payload.user_defined.u.console_msg_l2.WriteConsoleInput.NumRecords);
            return false;
        }

        // Append was FALSE: the initial 'x' should be dropped and replaced by QRR.
        if (host_io.input_bytes_available() != 3)
        {
            fwprintf(stderr, L"[condrv raw] input_bytes_available after write_console_input was %zu\n", host_io.input_bytes_available());
            return false;
        }

        // This test validates byte injection, so force raw `ReadConsoleA` behavior rather than
        // the default cooked line-input mode (which would reply-pend waiting for CR/LF).
        state.set_input_mode(0);

        constexpr ULONG read_api_size = sizeof(CONSOLE_READCONSOLE_MSG);
        constexpr ULONG read_header_size = sizeof(CONSOLE_MSG_HEADER);
        constexpr ULONG read_read_offset = read_api_size + read_header_size;

        oc::condrv::IoPacket read_packet{};
        read_packet.payload.user_defined = oc::condrv::UserDefinedPacket{};
        read_packet.descriptor.identifier.LowPart = 221;
        read_packet.descriptor.function = oc::condrv::console_io_user_defined;
        read_packet.descriptor.process = info.process;
        read_packet.descriptor.object = info.input;
        read_packet.descriptor.input_size = read_read_offset;
        read_packet.descriptor.output_size = read_api_size + 8;
        read_packet.payload.user_defined.msg_header.ApiNumber = static_cast<ULONG>(ConsolepReadConsole);
        read_packet.payload.user_defined.msg_header.ApiDescriptorSize = read_api_size;
        read_packet.payload.user_defined.u.console_msg_l1.ReadConsole.Unicode = FALSE;

        oc::condrv::BasicApiMessage<MemoryComm> read_message(comm, read_packet);
        auto read_outcome = oc::condrv::dispatch_message(state, read_message, host_io);
        if (!read_outcome)
        {
            fwprintf(stderr, L"[condrv raw] read_console outcome was unexpected\n");
            return false;
        }
        if (read_outcome->reply_pending)
        {
            fwprintf(stderr, L"[condrv raw] read_console returned reply_pending unexpectedly\n");
            return false;
        }
        if (read_message.completion().io_status.Status != oc::core::status_success)
        {
            fwprintf(stderr, L"[condrv raw] read_console status was 0x%08X\n", static_cast<unsigned int>(read_message.completion().io_status.Status));
            return false;
        }

        if (auto released = read_message.release_message_buffers(); !released)
        {
            fwprintf(stderr, L"[condrv raw] read_console release_message_buffers failed\n");
            return false;
        }

        if (comm.output.size() != read_api_size + 3)
        {
            fwprintf(stderr, L"[condrv raw] read_console comm.output.size was %zu\n", comm.output.size());
            return false;
        }

        return comm.output[read_api_size + 0] == static_cast<std::byte>('Q') &&
               comm.output[read_api_size + 1] == static_cast<std::byte>('R') &&
               comm.output[read_api_size + 2] == static_cast<std::byte>('R');
    }

    bool test_l1_get_number_of_input_events_reports_available_bytes()
    {
        MemoryComm comm{};
        oc::condrv::ServerState state{};
        TestHostIo host_io{};

        auto connect_packet = make_connect_packet(15001, 15002);
        oc::condrv::BasicApiMessage<MemoryComm> connect_message(comm, connect_packet);
        auto connect_outcome = oc::condrv::dispatch_message(state, connect_message, host_io);
        if (!connect_outcome)
        {
            return false;
        }

        auto info = unpack_connection_information(connect_message.completion());
        host_io.input = { static_cast<std::byte>('1'), static_cast<std::byte>('2'), static_cast<std::byte>('3') };

        constexpr ULONG api_size = sizeof(CONSOLE_GETNUMBEROFINPUTEVENTS_MSG);
        constexpr ULONG header_size = sizeof(CONSOLE_MSG_HEADER);
        constexpr ULONG read_offset = api_size + header_size;

        oc::condrv::IoPacket packet{};
        packet.payload.user_defined = oc::condrv::UserDefinedPacket{};
        packet.descriptor.identifier.LowPart = 230;
        packet.descriptor.function = oc::condrv::console_io_user_defined;
        packet.descriptor.process = info.process;
        packet.descriptor.object = info.input;
        packet.descriptor.input_size = read_offset;
        packet.descriptor.output_size = api_size;
        packet.payload.user_defined.msg_header.ApiNumber = static_cast<ULONG>(ConsolepGetNumberOfInputEvents);
        packet.payload.user_defined.msg_header.ApiDescriptorSize = api_size;

        comm.input.assign(packet.descriptor.input_size, std::byte{});

        oc::condrv::BasicApiMessage<MemoryComm> message(comm, packet);
        auto outcome = oc::condrv::dispatch_message(state, message, host_io);
        if (!outcome)
        {
            return false;
        }

        return message.completion().io_status.Status == oc::core::status_success &&
               message.packet().payload.user_defined.u.console_msg_l1.GetNumberOfConsoleInputEvents.ReadyEvents == 3;
    }

    bool test_l1_get_number_of_input_events_counts_utf8_code_units()
    {
        MemoryComm comm{};
        oc::condrv::ServerState state{};
        TestHostIo host_io{};

        auto connect_packet = make_connect_packet(15003, 15004);
        oc::condrv::BasicApiMessage<MemoryComm> connect_message(comm, connect_packet);
        auto connect_outcome = oc::condrv::dispatch_message(state, connect_message, host_io);
        if (!connect_outcome)
        {
            return false;
        }

        const auto info = unpack_connection_information(connect_message.completion());
        if (!set_input_code_page(comm, state, host_io, info, CP_UTF8))
        {
            return false;
        }

        host_io.input = { static_cast<std::byte>(0xC3), static_cast<std::byte>(0xA9) }; // U+00E9

        constexpr ULONG api_size = sizeof(CONSOLE_GETNUMBEROFINPUTEVENTS_MSG);
        constexpr ULONG header_size = sizeof(CONSOLE_MSG_HEADER);
        constexpr ULONG read_offset = api_size + header_size;

        oc::condrv::IoPacket packet{};
        packet.payload.user_defined = oc::condrv::UserDefinedPacket{};
        packet.descriptor.identifier.LowPart = 231;
        packet.descriptor.function = oc::condrv::console_io_user_defined;
        packet.descriptor.process = info.process;
        packet.descriptor.object = info.input;
        packet.descriptor.input_size = read_offset;
        packet.descriptor.output_size = api_size;
        packet.payload.user_defined.msg_header.ApiNumber = static_cast<ULONG>(ConsolepGetNumberOfInputEvents);
        packet.payload.user_defined.msg_header.ApiDescriptorSize = api_size;

        comm.input.assign(packet.descriptor.input_size, std::byte{});

        oc::condrv::BasicApiMessage<MemoryComm> message(comm, packet);
        auto outcome = oc::condrv::dispatch_message(state, message, host_io);
        if (!outcome)
        {
            return false;
        }

        return message.completion().io_status.Status == oc::core::status_success &&
               message.packet().payload.user_defined.u.console_msg_l1.GetNumberOfConsoleInputEvents.ReadyEvents == 1;
    }

    bool test_l2_fill_console_output_characters_round_trips()
    {
        MemoryComm comm{};
        oc::condrv::ServerState state{};
        TestHostIo host_io{};

        auto connect_packet = make_connect_packet(1111, 2222);
        oc::condrv::BasicApiMessage<MemoryComm> connect_message(comm, connect_packet);
        auto connect_outcome = oc::condrv::dispatch_message(state, connect_message, host_io);
        if (!connect_outcome)
        {
            return false;
        }

        auto info = unpack_connection_information(connect_message.completion());

        {
            constexpr ULONG api_size = sizeof(CONSOLE_FILLCONSOLEOUTPUT_MSG);
            constexpr ULONG header_size = sizeof(CONSOLE_MSG_HEADER);
            constexpr ULONG read_offset = api_size + header_size;

            oc::condrv::IoPacket packet{};
            packet.payload.user_defined = oc::condrv::UserDefinedPacket{};
            packet.descriptor.identifier.LowPart = 100;
            packet.descriptor.function = oc::condrv::console_io_user_defined;
            packet.descriptor.process = info.process;
            packet.descriptor.object = info.output;
            packet.descriptor.input_size = read_offset;
            packet.descriptor.output_size = api_size;
            packet.payload.user_defined.msg_header.ApiNumber = static_cast<ULONG>(ConsolepFillConsoleOutput);
            packet.payload.user_defined.msg_header.ApiDescriptorSize = api_size;

            auto& body = packet.payload.user_defined.u.console_msg_l2.FillConsoleOutput;
            body.WriteCoord = COORD{ 0, 0 };
            body.ElementType = CONSOLE_REAL_UNICODE;
            body.Element = static_cast<USHORT>(L'Z');
            body.Length = 3;

            comm.input.assign(packet.descriptor.input_size, std::byte{});

            oc::condrv::BasicApiMessage<MemoryComm> message(comm, packet);
            auto outcome = oc::condrv::dispatch_message(state, message, host_io);
            if (!outcome)
            {
                return false;
            }
            if (message.completion().io_status.Status != oc::core::status_success)
            {
                return false;
            }
            if (message.packet().payload.user_defined.u.console_msg_l2.FillConsoleOutput.Length != 3)
            {
                return false;
            }
        }

        {
            constexpr ULONG api_size = sizeof(CONSOLE_READCONSOLEOUTPUTSTRING_MSG);
            constexpr ULONG header_size = sizeof(CONSOLE_MSG_HEADER);
            constexpr ULONG read_offset = api_size + header_size;
            constexpr ULONG output_bytes = 3 * sizeof(wchar_t);

            oc::condrv::IoPacket packet{};
            packet.payload.user_defined = oc::condrv::UserDefinedPacket{};
            packet.descriptor.identifier.LowPart = 101;
            packet.descriptor.function = oc::condrv::console_io_user_defined;
            packet.descriptor.process = info.process;
            packet.descriptor.object = info.output;
            packet.descriptor.input_size = read_offset;
            packet.descriptor.output_size = api_size + output_bytes;
            packet.payload.user_defined.msg_header.ApiNumber = static_cast<ULONG>(ConsolepReadConsoleOutputString);
            packet.payload.user_defined.msg_header.ApiDescriptorSize = api_size;

            auto& body = packet.payload.user_defined.u.console_msg_l2.ReadConsoleOutputString;
            body.ReadCoord = COORD{ 0, 0 };
            body.StringType = CONSOLE_REAL_UNICODE;
            body.NumRecords = 0;

            comm.input.assign(packet.descriptor.input_size, std::byte{});
            comm.output.clear();

            oc::condrv::BasicApiMessage<MemoryComm> message(comm, packet);
            auto outcome = oc::condrv::dispatch_message(state, message, host_io);
            if (!outcome)
            {
                return false;
            }
            if (message.completion().io_status.Status != oc::core::status_success)
            {
                return false;
            }
            if (message.packet().payload.user_defined.u.console_msg_l2.ReadConsoleOutputString.NumRecords != 3)
            {
                return false;
            }

            if (auto released = message.release_message_buffers(); !released)
            {
                return false;
            }

            if (comm.output.size() != api_size + output_bytes)
            {
                return false;
            }

            wchar_t chars[3]{};
            std::memcpy(chars, comm.output.data() + api_size, output_bytes);
            return chars[0] == L'Z' && chars[1] == L'Z' && chars[2] == L'Z';
        }
    }

    bool test_l2_fill_console_output_attributes_round_trips()
    {
        MemoryComm comm{};
        oc::condrv::ServerState state{};
        TestHostIo host_io{};

        auto connect_packet = make_connect_packet(3333, 4444);
        oc::condrv::BasicApiMessage<MemoryComm> connect_message(comm, connect_packet);
        auto connect_outcome = oc::condrv::dispatch_message(state, connect_message, host_io);
        if (!connect_outcome)
        {
            return false;
        }

        auto info = unpack_connection_information(connect_message.completion());

        {
            constexpr ULONG api_size = sizeof(CONSOLE_FILLCONSOLEOUTPUT_MSG);
            constexpr ULONG header_size = sizeof(CONSOLE_MSG_HEADER);
            constexpr ULONG read_offset = api_size + header_size;

            oc::condrv::IoPacket packet{};
            packet.payload.user_defined = oc::condrv::UserDefinedPacket{};
            packet.descriptor.identifier.LowPart = 110;
            packet.descriptor.function = oc::condrv::console_io_user_defined;
            packet.descriptor.process = info.process;
            packet.descriptor.object = info.output;
            packet.descriptor.input_size = read_offset;
            packet.descriptor.output_size = api_size;
            packet.payload.user_defined.msg_header.ApiNumber = static_cast<ULONG>(ConsolepFillConsoleOutput);
            packet.payload.user_defined.msg_header.ApiDescriptorSize = api_size;

            auto& body = packet.payload.user_defined.u.console_msg_l2.FillConsoleOutput;
            body.WriteCoord = COORD{ 2, 0 };
            body.ElementType = CONSOLE_ATTRIBUTE;
            body.Element = 0x1E;
            body.Length = 2;

            comm.input.assign(packet.descriptor.input_size, std::byte{});

            oc::condrv::BasicApiMessage<MemoryComm> message(comm, packet);
            auto outcome = oc::condrv::dispatch_message(state, message, host_io);
            if (!outcome)
            {
                return false;
            }
            if (message.completion().io_status.Status != oc::core::status_success)
            {
                return false;
            }
            if (message.packet().payload.user_defined.u.console_msg_l2.FillConsoleOutput.Length != 2)
            {
                return false;
            }
        }

        {
            constexpr ULONG api_size = sizeof(CONSOLE_READCONSOLEOUTPUTSTRING_MSG);
            constexpr ULONG header_size = sizeof(CONSOLE_MSG_HEADER);
            constexpr ULONG read_offset = api_size + header_size;
            constexpr ULONG output_bytes = 2 * sizeof(USHORT);

            oc::condrv::IoPacket packet{};
            packet.payload.user_defined = oc::condrv::UserDefinedPacket{};
            packet.descriptor.identifier.LowPart = 111;
            packet.descriptor.function = oc::condrv::console_io_user_defined;
            packet.descriptor.process = info.process;
            packet.descriptor.object = info.output;
            packet.descriptor.input_size = read_offset;
            packet.descriptor.output_size = api_size + output_bytes;
            packet.payload.user_defined.msg_header.ApiNumber = static_cast<ULONG>(ConsolepReadConsoleOutputString);
            packet.payload.user_defined.msg_header.ApiDescriptorSize = api_size;

            auto& body = packet.payload.user_defined.u.console_msg_l2.ReadConsoleOutputString;
            body.ReadCoord = COORD{ 2, 0 };
            body.StringType = CONSOLE_ATTRIBUTE;
            body.NumRecords = 0;

            comm.input.assign(packet.descriptor.input_size, std::byte{});
            comm.output.clear();

            oc::condrv::BasicApiMessage<MemoryComm> message(comm, packet);
            auto outcome = oc::condrv::dispatch_message(state, message, host_io);
            if (!outcome)
            {
                return false;
            }
            if (message.completion().io_status.Status != oc::core::status_success)
            {
                return false;
            }
            if (message.packet().payload.user_defined.u.console_msg_l2.ReadConsoleOutputString.NumRecords != 2)
            {
                return false;
            }

            if (auto released = message.release_message_buffers(); !released)
            {
                return false;
            }

            if (comm.output.size() != api_size + output_bytes)
            {
                return false;
            }

            USHORT values[2]{};
            std::memcpy(values, comm.output.data() + api_size, output_bytes);
            return values[0] == 0x1E && values[1] == 0x1E;
        }
    }

    bool test_l2_write_console_output_string_unicode_round_trips()
    {
        MemoryComm comm{};
        oc::condrv::ServerState state{};
        TestHostIo host_io{};

        auto connect_packet = make_connect_packet(5556, 6667);
        oc::condrv::BasicApiMessage<MemoryComm> connect_message(comm, connect_packet);
        auto connect_outcome = oc::condrv::dispatch_message(state, connect_message, host_io);
        if (!connect_outcome)
        {
            return false;
        }

        auto info = unpack_connection_information(connect_message.completion());
        constexpr std::wstring_view text = L"Hi";

        {
            constexpr ULONG api_size = sizeof(CONSOLE_WRITECONSOLEOUTPUTSTRING_MSG);
            constexpr ULONG header_size = sizeof(CONSOLE_MSG_HEADER);
            constexpr ULONG read_offset = api_size + header_size;
            constexpr ULONG utf16_bytes = static_cast<ULONG>(text.size() * sizeof(wchar_t));

            oc::condrv::IoPacket packet{};
            packet.payload.user_defined = oc::condrv::UserDefinedPacket{};
            packet.descriptor.identifier.LowPart = 120;
            packet.descriptor.function = oc::condrv::console_io_user_defined;
            packet.descriptor.process = info.process;
            packet.descriptor.object = info.output;
            packet.descriptor.input_size = read_offset + utf16_bytes;
            packet.descriptor.output_size = api_size;
            packet.payload.user_defined.msg_header.ApiNumber = static_cast<ULONG>(ConsolepWriteConsoleOutputString);
            packet.payload.user_defined.msg_header.ApiDescriptorSize = api_size;

            auto& body = packet.payload.user_defined.u.console_msg_l2.WriteConsoleOutputString;
            body.WriteCoord = COORD{ 5, 3 };
            body.StringType = CONSOLE_REAL_UNICODE;
            body.NumRecords = 0;

            comm.input.assign(packet.descriptor.input_size, std::byte{});
            std::memcpy(comm.input.data() + read_offset, text.data(), utf16_bytes);

            oc::condrv::BasicApiMessage<MemoryComm> message(comm, packet);
            auto outcome = oc::condrv::dispatch_message(state, message, host_io);
            if (!outcome)
            {
                return false;
            }
            if (message.completion().io_status.Status != oc::core::status_success)
            {
                return false;
            }
            if (message.packet().payload.user_defined.u.console_msg_l2.WriteConsoleOutputString.NumRecords != text.size())
            {
                return false;
            }
        }

        {
            constexpr ULONG api_size = sizeof(CONSOLE_READCONSOLEOUTPUTSTRING_MSG);
            constexpr ULONG header_size = sizeof(CONSOLE_MSG_HEADER);
            constexpr ULONG read_offset = api_size + header_size;
            constexpr ULONG output_bytes = static_cast<ULONG>(text.size() * sizeof(wchar_t));

            oc::condrv::IoPacket packet{};
            packet.payload.user_defined = oc::condrv::UserDefinedPacket{};
            packet.descriptor.identifier.LowPart = 121;
            packet.descriptor.function = oc::condrv::console_io_user_defined;
            packet.descriptor.process = info.process;
            packet.descriptor.object = info.output;
            packet.descriptor.input_size = read_offset;
            packet.descriptor.output_size = api_size + output_bytes;
            packet.payload.user_defined.msg_header.ApiNumber = static_cast<ULONG>(ConsolepReadConsoleOutputString);
            packet.payload.user_defined.msg_header.ApiDescriptorSize = api_size;

            auto& body = packet.payload.user_defined.u.console_msg_l2.ReadConsoleOutputString;
            body.ReadCoord = COORD{ 5, 3 };
            body.StringType = CONSOLE_REAL_UNICODE;
            body.NumRecords = 0;

            comm.input.assign(packet.descriptor.input_size, std::byte{});
            comm.output.clear();

            oc::condrv::BasicApiMessage<MemoryComm> message(comm, packet);
            auto outcome = oc::condrv::dispatch_message(state, message, host_io);
            if (!outcome)
            {
                return false;
            }
            if (message.completion().io_status.Status != oc::core::status_success)
            {
                return false;
            }
            if (message.packet().payload.user_defined.u.console_msg_l2.ReadConsoleOutputString.NumRecords != text.size())
            {
                return false;
            }

            if (auto released = message.release_message_buffers(); !released)
            {
                return false;
            }

            if (comm.output.size() != api_size + output_bytes)
            {
                return false;
            }

            std::wstring actual(text.size(), L'\0');
            std::memcpy(actual.data(), comm.output.data() + api_size, output_bytes);
            return actual == text;
        }
    }

    bool test_l2_set_and_get_title_round_trips()
    {
        MemoryComm comm{};
        oc::condrv::ServerState state{};
        TestHostIo host_io{};

        auto connect_packet = make_connect_packet(7778, 8889);
        oc::condrv::BasicApiMessage<MemoryComm> connect_message(comm, connect_packet);
        auto connect_outcome = oc::condrv::dispatch_message(state, connect_message, host_io);
        if (!connect_outcome)
        {
            return false;
        }

        auto info = unpack_connection_information(connect_message.completion());
        constexpr std::wstring_view first = L"first";
        constexpr std::wstring_view second = L"second";

        auto set_title = [&](std::wstring_view title, const ULONG id) -> bool {
            const ULONG api_size = sizeof(CONSOLE_SETTITLE_MSG);
            const ULONG header_size = sizeof(CONSOLE_MSG_HEADER);
            const ULONG read_offset = api_size + header_size;
            const ULONG utf16_bytes = static_cast<ULONG>((title.size() + 1) * sizeof(wchar_t));

            oc::condrv::IoPacket packet{};
            packet.payload.user_defined = oc::condrv::UserDefinedPacket{};
            packet.descriptor.identifier.LowPart = id;
            packet.descriptor.function = oc::condrv::console_io_user_defined;
            packet.descriptor.process = info.process;
            packet.descriptor.object = info.output;
            packet.descriptor.input_size = read_offset + utf16_bytes;
            packet.descriptor.output_size = api_size;
            packet.payload.user_defined.msg_header.ApiNumber = static_cast<ULONG>(ConsolepSetTitle);
            packet.payload.user_defined.msg_header.ApiDescriptorSize = api_size;
            packet.payload.user_defined.u.console_msg_l2.SetConsoleTitle.Unicode = TRUE;

            comm.input.assign(packet.descriptor.input_size, std::byte{});
            std::memcpy(comm.input.data() + read_offset, title.data(), title.size() * sizeof(wchar_t));

            const wchar_t terminator = L'\0';
            std::memcpy(comm.input.data() + read_offset + (title.size() * sizeof(wchar_t)), &terminator, sizeof(terminator));

            oc::condrv::BasicApiMessage<MemoryComm> message(comm, packet);
            auto outcome = oc::condrv::dispatch_message(state, message, host_io);
            return outcome && message.completion().io_status.Status == oc::core::status_success;
        };

        auto get_title_w = [&](const bool original, std::wstring_view expected, const ULONG id) -> bool {
            const ULONG api_size = sizeof(CONSOLE_GETTITLE_MSG);
            const ULONG header_size = sizeof(CONSOLE_MSG_HEADER);
            const ULONG read_offset = api_size + header_size;
            const ULONG output_bytes = static_cast<ULONG>((expected.size() + 1) * sizeof(wchar_t));

            oc::condrv::IoPacket packet{};
            packet.payload.user_defined = oc::condrv::UserDefinedPacket{};
            packet.descriptor.identifier.LowPart = id;
            packet.descriptor.function = oc::condrv::console_io_user_defined;
            packet.descriptor.process = info.process;
            packet.descriptor.object = info.output;
            packet.descriptor.input_size = read_offset;
            packet.descriptor.output_size = api_size + output_bytes;
            packet.payload.user_defined.msg_header.ApiNumber = static_cast<ULONG>(ConsolepGetTitle);
            packet.payload.user_defined.msg_header.ApiDescriptorSize = api_size;

            auto& body = packet.payload.user_defined.u.console_msg_l2.GetConsoleTitle;
            body.TitleLength = 0;
            body.Unicode = TRUE;
            body.Original = original ? TRUE : FALSE;

            comm.input.assign(packet.descriptor.input_size, std::byte{});
            comm.output.clear();

            oc::condrv::BasicApiMessage<MemoryComm> message(comm, packet);
            auto outcome = oc::condrv::dispatch_message(state, message, host_io);
            if (!outcome)
            {
                return false;
            }
            if (message.completion().io_status.Status != oc::core::status_success)
            {
                return false;
            }
            if (message.packet().payload.user_defined.u.console_msg_l2.GetConsoleTitle.TitleLength != expected.size())
            {
                return false;
            }

            if (auto released = message.release_message_buffers(); !released)
            {
                return false;
            }

            const size_t expected_bytes = expected.size() * sizeof(wchar_t);
            if (comm.output.size() != api_size + expected_bytes)
            {
                return false;
            }

            std::wstring actual(expected.size(), L'\0');
            std::memcpy(actual.data(), comm.output.data() + api_size, expected_bytes);
            return actual == expected;
        };

        auto get_title_a = [&](std::string_view expected, const ULONG id) -> bool {
            const ULONG api_size = sizeof(CONSOLE_GETTITLE_MSG);
            const ULONG header_size = sizeof(CONSOLE_MSG_HEADER);
            const ULONG read_offset = api_size + header_size;
            const ULONG output_bytes = static_cast<ULONG>(expected.size() + 1);

            oc::condrv::IoPacket packet{};
            packet.payload.user_defined = oc::condrv::UserDefinedPacket{};
            packet.descriptor.identifier.LowPart = id;
            packet.descriptor.function = oc::condrv::console_io_user_defined;
            packet.descriptor.process = info.process;
            packet.descriptor.object = info.output;
            packet.descriptor.input_size = read_offset;
            packet.descriptor.output_size = api_size + output_bytes;
            packet.payload.user_defined.msg_header.ApiNumber = static_cast<ULONG>(ConsolepGetTitle);
            packet.payload.user_defined.msg_header.ApiDescriptorSize = api_size;

            auto& body = packet.payload.user_defined.u.console_msg_l2.GetConsoleTitle;
            body.TitleLength = 0;
            body.Unicode = FALSE;
            body.Original = FALSE;

            comm.input.assign(packet.descriptor.input_size, std::byte{});
            comm.output.clear();

            oc::condrv::BasicApiMessage<MemoryComm> message(comm, packet);
            auto outcome = oc::condrv::dispatch_message(state, message, host_io);
            if (!outcome)
            {
                return false;
            }
            if (message.completion().io_status.Status != oc::core::status_success)
            {
                return false;
            }

            if (auto released = message.release_message_buffers(); !released)
            {
                return false;
            }

            if (comm.output.size() != api_size + expected.size() + 1)
            {
                return false;
            }

            if (std::memcmp(comm.output.data() + api_size, expected.data(), expected.size()) != 0)
            {
                return false;
            }

            return comm.output[api_size + expected.size()] == static_cast<std::byte>('\0');
        };

        if (!set_title(first, 130))
        {
            return false;
        }
        if (!set_title(second, 131))
        {
            return false;
        }
        if (!get_title_w(false, second, 132))
        {
            return false;
        }
        if (!get_title_w(true, first, 133))
        {
            return false;
        }
        return get_title_a("second", 134);
    }

    bool test_l2_write_and_read_console_output_rect_round_trips()
    {
        MemoryComm comm{};
        oc::condrv::ServerState state{};
        TestHostIo host_io{};

        auto connect_packet = make_connect_packet(9001, 9002);
        oc::condrv::BasicApiMessage<MemoryComm> connect_message(comm, connect_packet);
        auto connect_outcome = oc::condrv::dispatch_message(state, connect_message, host_io);
        if (!connect_outcome)
        {
            return false;
        }

        auto info = unpack_connection_information(connect_message.completion());

        constexpr SMALL_RECT region{ 0, 0, 1, 0 };
        std::array<CHAR_INFO, 2> written{};
        written[0].Char.UnicodeChar = L'X';
        written[0].Attributes = 0x1E;
        written[1].Char.UnicodeChar = L'Y';
        written[1].Attributes = 0x2F;

        {
            constexpr ULONG api_size = sizeof(CONSOLE_WRITECONSOLEOUTPUT_MSG);
            constexpr ULONG header_size = sizeof(CONSOLE_MSG_HEADER);
            constexpr ULONG read_offset = api_size + header_size;
            constexpr ULONG buffer_bytes = static_cast<ULONG>(written.size() * sizeof(CHAR_INFO));

            oc::condrv::IoPacket packet{};
            packet.payload.user_defined = oc::condrv::UserDefinedPacket{};
            packet.descriptor.identifier.LowPart = 140;
            packet.descriptor.function = oc::condrv::console_io_user_defined;
            packet.descriptor.process = info.process;
            packet.descriptor.object = info.output;
            packet.descriptor.input_size = read_offset + buffer_bytes;
            packet.descriptor.output_size = api_size;
            packet.payload.user_defined.msg_header.ApiNumber = static_cast<ULONG>(ConsolepWriteConsoleOutput);
            packet.payload.user_defined.msg_header.ApiDescriptorSize = api_size;

            auto& body = packet.payload.user_defined.u.console_msg_l2.WriteConsoleOutput;
            body.CharRegion = region;
            body.Unicode = TRUE;

            comm.input.assign(packet.descriptor.input_size, std::byte{});
            std::memcpy(comm.input.data() + read_offset, written.data(), buffer_bytes);

            oc::condrv::BasicApiMessage<MemoryComm> message(comm, packet);
            auto outcome = oc::condrv::dispatch_message(state, message, host_io);
            if (!outcome)
            {
                return false;
            }
            if (message.completion().io_status.Status != oc::core::status_success)
            {
                return false;
            }
        }

        {
            constexpr ULONG api_size = sizeof(CONSOLE_READCONSOLEOUTPUT_MSG);
            constexpr ULONG header_size = sizeof(CONSOLE_MSG_HEADER);
            constexpr ULONG read_offset = api_size + header_size;
            constexpr ULONG buffer_bytes = static_cast<ULONG>(written.size() * sizeof(CHAR_INFO));

            oc::condrv::IoPacket packet{};
            packet.payload.user_defined = oc::condrv::UserDefinedPacket{};
            packet.descriptor.identifier.LowPart = 141;
            packet.descriptor.function = oc::condrv::console_io_user_defined;
            packet.descriptor.process = info.process;
            packet.descriptor.object = info.output;
            packet.descriptor.input_size = read_offset;
            packet.descriptor.output_size = api_size + buffer_bytes;
            packet.payload.user_defined.msg_header.ApiNumber = static_cast<ULONG>(ConsolepReadConsoleOutput);
            packet.payload.user_defined.msg_header.ApiDescriptorSize = api_size;

            auto& body = packet.payload.user_defined.u.console_msg_l2.ReadConsoleOutput;
            body.CharRegion = region;
            body.Unicode = TRUE;

            comm.input.assign(packet.descriptor.input_size, std::byte{});
            comm.output.clear();

            oc::condrv::BasicApiMessage<MemoryComm> message(comm, packet);
            auto outcome = oc::condrv::dispatch_message(state, message, host_io);
            if (!outcome)
            {
                return false;
            }
            if (message.completion().io_status.Status != oc::core::status_success)
            {
                return false;
            }
            if (message.completion().io_status.Information != buffer_bytes)
            {
                return false;
            }

            if (auto released = message.release_message_buffers(); !released)
            {
                return false;
            }

            if (comm.output.size() != api_size + buffer_bytes)
            {
                return false;
            }

            std::array<CHAR_INFO, 2> read{};
            std::memcpy(read.data(), comm.output.data() + api_size, buffer_bytes);

            return read[0].Char.UnicodeChar == written[0].Char.UnicodeChar &&
                   read[0].Attributes == written[0].Attributes &&
                   read[1].Char.UnicodeChar == written[1].Char.UnicodeChar &&
                   read[1].Attributes == written[1].Attributes;
        }
    }

    bool test_l2_scroll_console_screen_buffer_shifts_right()
    {
        MemoryComm comm{};
        oc::condrv::ServerState state{};
        TestHostIo host_io{};

        auto connect_packet = make_connect_packet(9101, 9102);
        oc::condrv::BasicApiMessage<MemoryComm> connect_message(comm, connect_packet);
        auto connect_outcome = oc::condrv::dispatch_message(state, connect_message, host_io);
        if (!connect_outcome)
        {
            return false;
        }

        auto info = unpack_connection_information(connect_message.completion());

        // Seed cells [0..2, 0] = "123".
        {
            constexpr std::wstring_view text = L"123";
            constexpr ULONG api_size = sizeof(CONSOLE_WRITECONSOLEOUTPUTSTRING_MSG);
            constexpr ULONG header_size = sizeof(CONSOLE_MSG_HEADER);
            constexpr ULONG read_offset = api_size + header_size;
            constexpr ULONG utf16_bytes = static_cast<ULONG>(text.size() * sizeof(wchar_t));

            oc::condrv::IoPacket packet{};
            packet.payload.user_defined = oc::condrv::UserDefinedPacket{};
            packet.descriptor.identifier.LowPart = 150;
            packet.descriptor.function = oc::condrv::console_io_user_defined;
            packet.descriptor.process = info.process;
            packet.descriptor.object = info.output;
            packet.descriptor.input_size = read_offset + utf16_bytes;
            packet.descriptor.output_size = api_size;
            packet.payload.user_defined.msg_header.ApiNumber = static_cast<ULONG>(ConsolepWriteConsoleOutputString);
            packet.payload.user_defined.msg_header.ApiDescriptorSize = api_size;

            auto& body = packet.payload.user_defined.u.console_msg_l2.WriteConsoleOutputString;
            body.WriteCoord = COORD{ 0, 0 };
            body.StringType = CONSOLE_REAL_UNICODE;
            body.NumRecords = 0;

            comm.input.assign(packet.descriptor.input_size, std::byte{});
            std::memcpy(comm.input.data() + read_offset, text.data(), utf16_bytes);

            oc::condrv::BasicApiMessage<MemoryComm> message(comm, packet);
            auto outcome = oc::condrv::dispatch_message(state, message, host_io);
            if (!outcome || message.completion().io_status.Status != oc::core::status_success)
            {
                return false;
            }
        }

        // Scroll that region right by one cell and fill vacated cells with '.'.
        {
            constexpr ULONG api_size = sizeof(CONSOLE_SCROLLSCREENBUFFER_MSG);
            constexpr ULONG header_size = sizeof(CONSOLE_MSG_HEADER);
            constexpr ULONG read_offset = api_size + header_size;

            oc::condrv::IoPacket packet{};
            packet.payload.user_defined = oc::condrv::UserDefinedPacket{};
            packet.descriptor.identifier.LowPart = 151;
            packet.descriptor.function = oc::condrv::console_io_user_defined;
            packet.descriptor.process = info.process;
            packet.descriptor.object = info.output;
            packet.descriptor.input_size = read_offset;
            packet.descriptor.output_size = api_size;
            packet.payload.user_defined.msg_header.ApiNumber = static_cast<ULONG>(ConsolepScrollScreenBuffer);
            packet.payload.user_defined.msg_header.ApiDescriptorSize = api_size;

            auto& body = packet.payload.user_defined.u.console_msg_l2.ScrollConsoleScreenBuffer;
            body.ScrollRectangle = SMALL_RECT{ 0, 0, 2, 0 };
            body.ClipRectangle = SMALL_RECT{ 0, 0, 0, 0 };
            body.Clip = FALSE;
            body.Unicode = TRUE;
            body.DestinationOrigin = COORD{ 1, 0 };
            body.Fill.Char.UnicodeChar = L'.';
            body.Fill.Attributes = 0x07;

            comm.input.assign(packet.descriptor.input_size, std::byte{});

            oc::condrv::BasicApiMessage<MemoryComm> message(comm, packet);
            auto outcome = oc::condrv::dispatch_message(state, message, host_io);
            if (!outcome || message.completion().io_status.Status != oc::core::status_success)
            {
                return false;
            }
        }

        // Read back 4 cells starting at origin.
        {
            constexpr ULONG api_size = sizeof(CONSOLE_READCONSOLEOUTPUTSTRING_MSG);
            constexpr ULONG header_size = sizeof(CONSOLE_MSG_HEADER);
            constexpr ULONG read_offset = api_size + header_size;
            constexpr ULONG output_bytes = 4 * sizeof(wchar_t);

            oc::condrv::IoPacket packet{};
            packet.payload.user_defined = oc::condrv::UserDefinedPacket{};
            packet.descriptor.identifier.LowPart = 152;
            packet.descriptor.function = oc::condrv::console_io_user_defined;
            packet.descriptor.process = info.process;
            packet.descriptor.object = info.output;
            packet.descriptor.input_size = read_offset;
            packet.descriptor.output_size = api_size + output_bytes;
            packet.payload.user_defined.msg_header.ApiNumber = static_cast<ULONG>(ConsolepReadConsoleOutputString);
            packet.payload.user_defined.msg_header.ApiDescriptorSize = api_size;

            auto& body = packet.payload.user_defined.u.console_msg_l2.ReadConsoleOutputString;
            body.ReadCoord = COORD{ 0, 0 };
            body.StringType = CONSOLE_REAL_UNICODE;
            body.NumRecords = 0;

            comm.input.assign(packet.descriptor.input_size, std::byte{});
            comm.output.clear();

            oc::condrv::BasicApiMessage<MemoryComm> message(comm, packet);
            auto outcome = oc::condrv::dispatch_message(state, message, host_io);
            if (!outcome || message.completion().io_status.Status != oc::core::status_success)
            {
                return false;
            }

            if (message.packet().payload.user_defined.u.console_msg_l2.ReadConsoleOutputString.NumRecords != 4)
            {
                return false;
            }

            if (auto released = message.release_message_buffers(); !released)
            {
                return false;
            }

            if (comm.output.size() != api_size + output_bytes)
            {
                return false;
            }

            std::wstring actual(4, L'\0');
            std::memcpy(actual.data(), comm.output.data() + api_size, output_bytes);
            return actual == L".123";
        }
    }

    bool test_l3_add_get_and_remove_console_alias_w_round_trips()
    {
        MemoryComm comm{};
        oc::condrv::ServerState state{};
        TestHostIo host_io{};

        auto connect_packet = make_connect_packet(16001, 16002);
        oc::condrv::BasicApiMessage<MemoryComm> connect_message(comm, connect_packet);
        auto connect_outcome = oc::condrv::dispatch_message(state, connect_message, host_io);
        if (!connect_outcome)
        {
            return false;
        }

        auto info = unpack_connection_information(connect_message.completion());

        constexpr std::wstring_view exe = L"cmd.exe";
        constexpr std::wstring_view source = L"ls";
        constexpr std::wstring_view target = L"dir";

        constexpr ULONG header_size = sizeof(CONSOLE_MSG_HEADER);

        // Add alias.
        {
            constexpr ULONG api_size = sizeof(CONSOLE_ADDALIAS_MSG);
            constexpr ULONG read_offset = api_size + header_size;
            const ULONG exe_bytes = static_cast<ULONG>(exe.size() * sizeof(wchar_t));
            const ULONG source_bytes = static_cast<ULONG>(source.size() * sizeof(wchar_t));
            const ULONG target_bytes = static_cast<ULONG>(target.size() * sizeof(wchar_t));

            oc::condrv::IoPacket packet{};
            packet.payload.user_defined = oc::condrv::UserDefinedPacket{};
            packet.descriptor.identifier.LowPart = 300;
            packet.descriptor.function = oc::condrv::console_io_user_defined;
            packet.descriptor.process = info.process;
            packet.descriptor.object = info.output;
            packet.descriptor.input_size = read_offset + exe_bytes + source_bytes + target_bytes;
            packet.descriptor.output_size = api_size;
            packet.payload.user_defined.msg_header.ApiNumber = static_cast<ULONG>(ConsolepAddAlias);
            packet.payload.user_defined.msg_header.ApiDescriptorSize = api_size;

            auto& body = packet.payload.user_defined.u.console_msg_l3.AddConsoleAliasW;
            body.Unicode = TRUE;
            body.ExeLength = static_cast<USHORT>(exe_bytes);
            body.SourceLength = static_cast<USHORT>(source_bytes);
            body.TargetLength = static_cast<USHORT>(target_bytes);

            comm.input.assign(packet.descriptor.input_size, std::byte{});
            std::memcpy(comm.input.data() + read_offset, exe.data(), exe_bytes);
            std::memcpy(comm.input.data() + read_offset + exe_bytes, source.data(), source_bytes);
            std::memcpy(comm.input.data() + read_offset + exe_bytes + source_bytes, target.data(), target_bytes);

            oc::condrv::BasicApiMessage<MemoryComm> message(comm, packet);
            auto outcome = oc::condrv::dispatch_message(state, message, host_io);
            if (!outcome || message.completion().io_status.Status != oc::core::status_success)
            {
                return false;
            }
        }

        // Get alias.
        bool got_alias = false;
        {
            constexpr ULONG api_size = sizeof(CONSOLE_GETALIAS_MSG);
            constexpr ULONG read_offset = api_size + header_size;
            const ULONG exe_bytes = static_cast<ULONG>(exe.size() * sizeof(wchar_t));
            const ULONG source_bytes = static_cast<ULONG>(source.size() * sizeof(wchar_t));
            const ULONG output_capacity = 64;

            oc::condrv::IoPacket packet{};
            packet.payload.user_defined = oc::condrv::UserDefinedPacket{};
            packet.descriptor.identifier.LowPart = 301;
            packet.descriptor.function = oc::condrv::console_io_user_defined;
            packet.descriptor.process = info.process;
            packet.descriptor.object = info.output;
            packet.descriptor.input_size = read_offset + exe_bytes + source_bytes;
            packet.descriptor.output_size = api_size + output_capacity;
            packet.payload.user_defined.msg_header.ApiNumber = static_cast<ULONG>(ConsolepGetAlias);
            packet.payload.user_defined.msg_header.ApiDescriptorSize = api_size;

            auto& body = packet.payload.user_defined.u.console_msg_l3.GetConsoleAliasW;
            body.Unicode = TRUE;
            body.ExeLength = static_cast<USHORT>(exe_bytes);
            body.SourceLength = static_cast<USHORT>(source_bytes);
            body.TargetLength = 0;

            comm.input.assign(packet.descriptor.input_size, std::byte{});
            std::memcpy(comm.input.data() + read_offset, exe.data(), exe_bytes);
            std::memcpy(comm.input.data() + read_offset + exe_bytes, source.data(), source_bytes);
            comm.output.clear();

            oc::condrv::BasicApiMessage<MemoryComm> message(comm, packet);
            auto outcome = oc::condrv::dispatch_message(state, message, host_io);
            if (!outcome || message.completion().io_status.Status != oc::core::status_success)
            {
                return false;
            }

            if (message.completion().io_status.Information != 8)
            {
                return false;
            }

            if (message.packet().payload.user_defined.u.console_msg_l3.GetConsoleAliasW.TargetLength != 8)
            {
                return false;
            }

            if (auto released = message.release_message_buffers(); !released)
            {
                return false;
            }

            if (comm.output.size() != api_size + 8)
            {
                return false;
            }

            std::wstring actual(4, L'\0');
            std::memcpy(actual.data(), comm.output.data() + api_size, 8);
            got_alias = actual[0] == L'd' && actual[1] == L'i' && actual[2] == L'r' && actual[3] == L'\0';
        }

        if (!got_alias)
        {
            return false;
        }

        // Remove alias by setting an empty target.
        {
            constexpr ULONG api_size = sizeof(CONSOLE_ADDALIAS_MSG);
            constexpr ULONG read_offset = api_size + header_size;
            const ULONG exe_bytes = static_cast<ULONG>(exe.size() * sizeof(wchar_t));
            const ULONG source_bytes = static_cast<ULONG>(source.size() * sizeof(wchar_t));

            oc::condrv::IoPacket packet{};
            packet.payload.user_defined = oc::condrv::UserDefinedPacket{};
            packet.descriptor.identifier.LowPart = 302;
            packet.descriptor.function = oc::condrv::console_io_user_defined;
            packet.descriptor.process = info.process;
            packet.descriptor.object = info.output;
            packet.descriptor.input_size = read_offset + exe_bytes + source_bytes;
            packet.descriptor.output_size = api_size;
            packet.payload.user_defined.msg_header.ApiNumber = static_cast<ULONG>(ConsolepAddAlias);
            packet.payload.user_defined.msg_header.ApiDescriptorSize = api_size;

            auto& body = packet.payload.user_defined.u.console_msg_l3.AddConsoleAliasW;
            body.Unicode = TRUE;
            body.ExeLength = static_cast<USHORT>(exe_bytes);
            body.SourceLength = static_cast<USHORT>(source_bytes);
            body.TargetLength = 0;

            comm.input.assign(packet.descriptor.input_size, std::byte{});
            std::memcpy(comm.input.data() + read_offset, exe.data(), exe_bytes);
            std::memcpy(comm.input.data() + read_offset + exe_bytes, source.data(), source_bytes);

            oc::condrv::BasicApiMessage<MemoryComm> message(comm, packet);
            auto outcome = oc::condrv::dispatch_message(state, message, host_io);
            if (!outcome || message.completion().io_status.Status != oc::core::status_success)
            {
                return false;
            }
        }

        // Get alias should now fail.
        {
            constexpr ULONG api_size = sizeof(CONSOLE_GETALIAS_MSG);
            constexpr ULONG read_offset = api_size + header_size;
            const ULONG exe_bytes = static_cast<ULONG>(exe.size() * sizeof(wchar_t));
            const ULONG source_bytes = static_cast<ULONG>(source.size() * sizeof(wchar_t));

            oc::condrv::IoPacket packet{};
            packet.payload.user_defined = oc::condrv::UserDefinedPacket{};
            packet.descriptor.identifier.LowPart = 303;
            packet.descriptor.function = oc::condrv::console_io_user_defined;
            packet.descriptor.process = info.process;
            packet.descriptor.object = info.output;
            packet.descriptor.input_size = read_offset + exe_bytes + source_bytes;
            packet.descriptor.output_size = api_size + 32;
            packet.payload.user_defined.msg_header.ApiNumber = static_cast<ULONG>(ConsolepGetAlias);
            packet.payload.user_defined.msg_header.ApiDescriptorSize = api_size;

            auto& body = packet.payload.user_defined.u.console_msg_l3.GetConsoleAliasW;
            body.Unicode = TRUE;
            body.ExeLength = static_cast<USHORT>(exe_bytes);
            body.SourceLength = static_cast<USHORT>(source_bytes);
            body.TargetLength = 0;

            comm.input.assign(packet.descriptor.input_size, std::byte{});
            std::memcpy(comm.input.data() + read_offset, exe.data(), exe_bytes);
            std::memcpy(comm.input.data() + read_offset + exe_bytes, source.data(), source_bytes);

            oc::condrv::BasicApiMessage<MemoryComm> message(comm, packet);
            auto outcome = oc::condrv::dispatch_message(state, message, host_io);
            return outcome &&
                   message.completion().io_status.Status == oc::core::status_unsuccessful &&
                   message.completion().io_status.Information == 0;
        }
    }

    bool test_l3_get_console_aliases_length_and_get_aliases_w_round_trips()
    {
        MemoryComm comm{};
        oc::condrv::ServerState state{};
        TestHostIo host_io{};

        auto connect_packet = make_connect_packet(16011, 16012);
        oc::condrv::BasicApiMessage<MemoryComm> connect_message(comm, connect_packet);
        auto connect_outcome = oc::condrv::dispatch_message(state, connect_message, host_io);
        if (!connect_outcome)
        {
            return false;
        }

        auto info = unpack_connection_information(connect_message.completion());

        constexpr std::wstring_view exe = L"cmd.exe";
        constexpr std::wstring_view source = L"ls";
        constexpr std::wstring_view target = L"dir";

        constexpr ULONG header_size = sizeof(CONSOLE_MSG_HEADER);

        // Add alias (W).
        {
            constexpr ULONG api_size = sizeof(CONSOLE_ADDALIAS_MSG);
            constexpr ULONG read_offset = api_size + header_size;
            const ULONG exe_bytes = static_cast<ULONG>(exe.size() * sizeof(wchar_t));
            const ULONG source_bytes = static_cast<ULONG>(source.size() * sizeof(wchar_t));
            const ULONG target_bytes = static_cast<ULONG>(target.size() * sizeof(wchar_t));

            oc::condrv::IoPacket packet{};
            packet.payload.user_defined = oc::condrv::UserDefinedPacket{};
            packet.descriptor.identifier.LowPart = 310;
            packet.descriptor.function = oc::condrv::console_io_user_defined;
            packet.descriptor.process = info.process;
            packet.descriptor.object = info.output;
            packet.descriptor.input_size = read_offset + exe_bytes + source_bytes + target_bytes;
            packet.descriptor.output_size = api_size;
            packet.payload.user_defined.msg_header.ApiNumber = static_cast<ULONG>(ConsolepAddAlias);
            packet.payload.user_defined.msg_header.ApiDescriptorSize = api_size;

            auto& body = packet.payload.user_defined.u.console_msg_l3.AddConsoleAliasW;
            body.Unicode = TRUE;
            body.ExeLength = static_cast<USHORT>(exe_bytes);
            body.SourceLength = static_cast<USHORT>(source_bytes);
            body.TargetLength = static_cast<USHORT>(target_bytes);

            comm.input.assign(packet.descriptor.input_size, std::byte{});
            std::memcpy(comm.input.data() + read_offset, exe.data(), exe_bytes);
            std::memcpy(comm.input.data() + read_offset + exe_bytes, source.data(), source_bytes);
            std::memcpy(comm.input.data() + read_offset + exe_bytes + source_bytes, target.data(), target_bytes);

            oc::condrv::BasicApiMessage<MemoryComm> message(comm, packet);
            auto outcome = oc::condrv::dispatch_message(state, message, host_io);
            if (!outcome || message.completion().io_status.Status != oc::core::status_success)
            {
                return false;
            }
        }

        // Query required length.
        constexpr ULONG expected_bytes = 14; // "ls=dir\\0" in UTF-16.
        {
            constexpr ULONG api_size = sizeof(CONSOLE_GETALIASESLENGTH_MSG);
            constexpr ULONG read_offset = api_size + header_size;
            const ULONG exe_bytes = static_cast<ULONG>(exe.size() * sizeof(wchar_t));

            oc::condrv::IoPacket packet{};
            packet.payload.user_defined = oc::condrv::UserDefinedPacket{};
            packet.descriptor.identifier.LowPart = 311;
            packet.descriptor.function = oc::condrv::console_io_user_defined;
            packet.descriptor.process = info.process;
            packet.descriptor.object = info.output;
            packet.descriptor.input_size = read_offset + exe_bytes;
            packet.descriptor.output_size = api_size;
            packet.payload.user_defined.msg_header.ApiNumber = static_cast<ULONG>(ConsolepGetAliasesLength);
            packet.payload.user_defined.msg_header.ApiDescriptorSize = api_size;

            auto& body = packet.payload.user_defined.u.console_msg_l3.GetConsoleAliasesLengthW;
            body.Unicode = TRUE;
            body.AliasesLength = 0;

            comm.input.assign(packet.descriptor.input_size, std::byte{});
            std::memcpy(comm.input.data() + read_offset, exe.data(), exe_bytes);

            oc::condrv::BasicApiMessage<MemoryComm> message(comm, packet);
            auto outcome = oc::condrv::dispatch_message(state, message, host_io);
            if (!outcome || message.completion().io_status.Status != oc::core::status_success)
            {
                return false;
            }

            if (message.packet().payload.user_defined.u.console_msg_l3.GetConsoleAliasesLengthW.AliasesLength != expected_bytes)
            {
                return false;
            }
        }

        // Fetch alias list.
        {
            constexpr ULONG api_size = sizeof(CONSOLE_GETALIASES_MSG);
            constexpr ULONG read_offset = api_size + header_size;
            const ULONG exe_bytes = static_cast<ULONG>(exe.size() * sizeof(wchar_t));

            oc::condrv::IoPacket packet{};
            packet.payload.user_defined = oc::condrv::UserDefinedPacket{};
            packet.descriptor.identifier.LowPart = 312;
            packet.descriptor.function = oc::condrv::console_io_user_defined;
            packet.descriptor.process = info.process;
            packet.descriptor.object = info.output;
            packet.descriptor.input_size = read_offset + exe_bytes;
            packet.descriptor.output_size = api_size + expected_bytes;
            packet.payload.user_defined.msg_header.ApiNumber = static_cast<ULONG>(ConsolepGetAliases);
            packet.payload.user_defined.msg_header.ApiDescriptorSize = api_size;

            auto& body = packet.payload.user_defined.u.console_msg_l3.GetConsoleAliasesW;
            body.Unicode = TRUE;
            body.AliasesBufferLength = 0;

            comm.input.assign(packet.descriptor.input_size, std::byte{});
            std::memcpy(comm.input.data() + read_offset, exe.data(), exe_bytes);
            comm.output.clear();

            oc::condrv::BasicApiMessage<MemoryComm> message(comm, packet);
            auto outcome = oc::condrv::dispatch_message(state, message, host_io);
            if (!outcome || message.completion().io_status.Status != oc::core::status_success)
            {
                return false;
            }

            if (message.completion().io_status.Information != expected_bytes)
            {
                return false;
            }

            if (auto released = message.release_message_buffers(); !released)
            {
                return false;
            }

            if (comm.output.size() != api_size + expected_bytes)
            {
                return false;
            }

            std::wstring actual(7, L'\0');
            std::memcpy(actual.data(), comm.output.data() + api_size, expected_bytes);
            return actual.substr(0, 6) == L"ls=dir" && actual[6] == L'\0';
        }
    }

    bool test_l3_get_console_alias_exes_length_and_get_alias_exes_w_round_trips()
    {
        MemoryComm comm{};
        oc::condrv::ServerState state{};
        TestHostIo host_io{};

        auto connect_packet = make_connect_packet(16021, 16022);
        oc::condrv::BasicApiMessage<MemoryComm> connect_message(comm, connect_packet);
        auto connect_outcome = oc::condrv::dispatch_message(state, connect_message, host_io);
        if (!connect_outcome)
        {
            return false;
        }

        auto info = unpack_connection_information(connect_message.completion());

        constexpr std::wstring_view exe = L"cmd.exe";
        constexpr std::wstring_view source = L"ls";
        constexpr std::wstring_view target = L"dir";

        constexpr ULONG header_size = sizeof(CONSOLE_MSG_HEADER);

        // Add alias (W) so the exe appears in the alias exe list.
        {
            constexpr ULONG api_size = sizeof(CONSOLE_ADDALIAS_MSG);
            constexpr ULONG read_offset = api_size + header_size;
            const ULONG exe_bytes = static_cast<ULONG>(exe.size() * sizeof(wchar_t));
            const ULONG source_bytes = static_cast<ULONG>(source.size() * sizeof(wchar_t));
            const ULONG target_bytes = static_cast<ULONG>(target.size() * sizeof(wchar_t));

            oc::condrv::IoPacket packet{};
            packet.payload.user_defined = oc::condrv::UserDefinedPacket{};
            packet.descriptor.identifier.LowPart = 320;
            packet.descriptor.function = oc::condrv::console_io_user_defined;
            packet.descriptor.process = info.process;
            packet.descriptor.object = info.output;
            packet.descriptor.input_size = read_offset + exe_bytes + source_bytes + target_bytes;
            packet.descriptor.output_size = api_size;
            packet.payload.user_defined.msg_header.ApiNumber = static_cast<ULONG>(ConsolepAddAlias);
            packet.payload.user_defined.msg_header.ApiDescriptorSize = api_size;

            auto& body = packet.payload.user_defined.u.console_msg_l3.AddConsoleAliasW;
            body.Unicode = TRUE;
            body.ExeLength = static_cast<USHORT>(exe_bytes);
            body.SourceLength = static_cast<USHORT>(source_bytes);
            body.TargetLength = static_cast<USHORT>(target_bytes);

            comm.input.assign(packet.descriptor.input_size, std::byte{});
            std::memcpy(comm.input.data() + read_offset, exe.data(), exe_bytes);
            std::memcpy(comm.input.data() + read_offset + exe_bytes, source.data(), source_bytes);
            std::memcpy(comm.input.data() + read_offset + exe_bytes + source_bytes, target.data(), target_bytes);

            oc::condrv::BasicApiMessage<MemoryComm> message(comm, packet);
            auto outcome = oc::condrv::dispatch_message(state, message, host_io);
            if (!outcome || message.completion().io_status.Status != oc::core::status_success)
            {
                return false;
            }
        }

        constexpr ULONG expected_bytes = 16; // "cmd.exe\\0" in UTF-16.

        // Query required length.
        {
            constexpr ULONG api_size = sizeof(CONSOLE_GETALIASEXESLENGTH_MSG);
            constexpr ULONG read_offset = api_size + header_size;

            oc::condrv::IoPacket packet{};
            packet.payload.user_defined = oc::condrv::UserDefinedPacket{};
            packet.descriptor.identifier.LowPart = 321;
            packet.descriptor.function = oc::condrv::console_io_user_defined;
            packet.descriptor.process = info.process;
            packet.descriptor.object = info.output;
            packet.descriptor.input_size = read_offset;
            packet.descriptor.output_size = api_size;
            packet.payload.user_defined.msg_header.ApiNumber = static_cast<ULONG>(ConsolepGetAliasExesLength);
            packet.payload.user_defined.msg_header.ApiDescriptorSize = api_size;

            auto& body = packet.payload.user_defined.u.console_msg_l3.GetConsoleAliasExesLengthW;
            body.Unicode = TRUE;
            body.AliasExesLength = 0;

            comm.input.assign(packet.descriptor.input_size, std::byte{});

            oc::condrv::BasicApiMessage<MemoryComm> message(comm, packet);
            auto outcome = oc::condrv::dispatch_message(state, message, host_io);
            if (!outcome || message.completion().io_status.Status != oc::core::status_success)
            {
                return false;
            }

            if (message.packet().payload.user_defined.u.console_msg_l3.GetConsoleAliasExesLengthW.AliasExesLength != expected_bytes)
            {
                return false;
            }
        }

        // Fetch alias exe list.
        {
            constexpr ULONG api_size = sizeof(CONSOLE_GETALIASEXES_MSG);
            constexpr ULONG read_offset = api_size + header_size;

            oc::condrv::IoPacket packet{};
            packet.payload.user_defined = oc::condrv::UserDefinedPacket{};
            packet.descriptor.identifier.LowPart = 322;
            packet.descriptor.function = oc::condrv::console_io_user_defined;
            packet.descriptor.process = info.process;
            packet.descriptor.object = info.output;
            packet.descriptor.input_size = read_offset;
            packet.descriptor.output_size = api_size + expected_bytes;
            packet.payload.user_defined.msg_header.ApiNumber = static_cast<ULONG>(ConsolepGetAliasExes);
            packet.payload.user_defined.msg_header.ApiDescriptorSize = api_size;

            auto& body = packet.payload.user_defined.u.console_msg_l3.GetConsoleAliasExesW;
            body.Unicode = TRUE;
            body.AliasExesBufferLength = 0;

            comm.input.assign(packet.descriptor.input_size, std::byte{});
            comm.output.clear();

            oc::condrv::BasicApiMessage<MemoryComm> message(comm, packet);
            auto outcome = oc::condrv::dispatch_message(state, message, host_io);
            if (!outcome || message.completion().io_status.Status != oc::core::status_success)
            {
                return false;
            }

            if (message.completion().io_status.Information != expected_bytes)
            {
                return false;
            }

            if (auto released = message.release_message_buffers(); !released)
            {
                return false;
            }

            if (comm.output.size() != api_size + expected_bytes)
            {
                return false;
            }

            std::wstring actual(8, L'\0');
            std::memcpy(actual.data(), comm.output.data() + api_size, expected_bytes);
            return actual.substr(0, 7) == L"cmd.exe" && actual[7] == L'\0';
        }
    }

    bool test_user_defined_deprecated_apis_return_not_implemented_and_zero_descriptor_bytes()
    {
        MemoryComm comm{};
        oc::condrv::ServerState state{};
        TestHostIo host_io{};

        auto connect_packet = make_connect_packet(9310, 9311);
        oc::condrv::BasicApiMessage<MemoryComm> connect_message(comm, connect_packet);
        auto connect_outcome = oc::condrv::dispatch_message(state, connect_message, host_io);
        if (!connect_outcome)
        {
            return false;
        }

        const auto info = unpack_connection_information(connect_message.completion());

        struct DeprecatedCase final
        {
            ULONG api_number{};
            ULONG api_size{};
        };

        static constexpr DeprecatedCase cases[] = {
            { static_cast<ULONG>(ConsolepMapBitmap), sizeof(CONSOLE_MAPBITMAP_MSG) },
            { static_cast<ULONG>(ConsolepSetIcon), sizeof(CONSOLE_SETICON_MSG) },
            { static_cast<ULONG>(ConsolepInvalidateBitmapRect), sizeof(CONSOLE_INVALIDATERECT_MSG) },
            { static_cast<ULONG>(ConsolepVDMOperation), sizeof(CONSOLE_VDM_MSG) },
            { static_cast<ULONG>(ConsolepSetCursor), sizeof(CONSOLE_SETCURSOR_MSG) },
            { static_cast<ULONG>(ConsolepShowCursor), sizeof(CONSOLE_SHOWCURSOR_MSG) },
            { static_cast<ULONG>(ConsolepMenuControl), sizeof(CONSOLE_MENUCONTROL_MSG) },
            { static_cast<ULONG>(ConsolepSetPalette), sizeof(CONSOLE_SETPALETTE_MSG) },
            { static_cast<ULONG>(ConsolepRegisterVDM), sizeof(CONSOLE_REGISTERVDM_MSG) },
            { static_cast<ULONG>(ConsolepGetHardwareState), sizeof(CONSOLE_GETHARDWARESTATE_MSG) },
            { static_cast<ULONG>(ConsolepSetHardwareState), sizeof(CONSOLE_SETHARDWARESTATE_MSG) },
        };

        const auto run_one = [&](const ULONG api_number, const ULONG api_size, const ULONG identifier) noexcept -> bool {
            constexpr ULONG header_size = sizeof(CONSOLE_MSG_HEADER);

            oc::condrv::IoPacket packet{};
            packet.payload.user_defined = oc::condrv::UserDefinedPacket{};
            packet.descriptor.identifier.LowPart = identifier;
            packet.descriptor.function = oc::condrv::console_io_user_defined;
            packet.descriptor.process = info.process;
            packet.descriptor.object = info.output;
            packet.descriptor.input_size = api_size + header_size;
            packet.descriptor.output_size = api_size;
            packet.payload.user_defined.msg_header.ApiNumber = api_number;
            packet.payload.user_defined.msg_header.ApiDescriptorSize = api_size;

            std::memset(&packet.payload.user_defined.u, 0xA5, api_size);

            oc::condrv::BasicApiMessage<MemoryComm> message(comm, packet);
            const auto outcome = oc::condrv::dispatch_message(state, message, host_io);
            if (!outcome)
            {
                return false;
            }

            if (message.completion().io_status.Status != oc::core::status_not_implemented)
            {
                return false;
            }

            if (message.completion().io_status.Information != 0)
            {
                return false;
            }

            if (message.completion().write.size != api_size)
            {
                return false;
            }

            const auto* bytes = static_cast<const std::byte*>(message.completion().write.data);
            if (bytes == nullptr)
            {
                return false;
            }

            for (size_t i = 0; i < api_size; ++i)
            {
                if (bytes[i] != std::byte{ 0 })
                {
                    return false;
                }
            }

            return true;
        };

        ULONG identifier = 7000;
        for (const auto& entry : cases)
        {
            if (!run_one(entry.api_number, entry.api_size, identifier++))
            {
                return false;
            }
        }

        // Unrecognized API numbers should also return sanitized descriptor bytes.
        return run_one(0xFFFF'FFFFu, 16, identifier);
    }
}

bool run_condrv_raw_io_tests()
{
    struct NamedTest final
    {
        const wchar_t* name{};
        bool (*run)(){};
    };

    static constexpr NamedTest tests[] = {
        { L"test_raw_write_forwards_bytes_and_sets_information", test_raw_write_forwards_bytes_and_sets_information },
        { L"test_raw_write_updates_screen_buffer_model", test_raw_write_updates_screen_buffer_model },
        { L"test_raw_read_copies_bytes_to_output_buffer", test_raw_read_copies_bytes_to_output_buffer },
        { L"test_raw_read_processed_input_consumes_ctrl_c_and_sends_end_task", test_raw_read_processed_input_consumes_ctrl_c_and_sends_end_task },
        { L"test_raw_read_processed_input_skips_ctrl_c_mid_buffer_and_still_fills_output", test_raw_read_processed_input_skips_ctrl_c_mid_buffer_and_still_fills_output },
        { L"test_raw_read_processed_input_ctrl_break_returns_alerted_and_flushes_input", test_raw_read_processed_input_ctrl_break_returns_alerted_and_flushes_input },
        { L"test_raw_read_decodes_win32_input_mode_character_key", test_raw_read_decodes_win32_input_mode_character_key },
        { L"test_raw_read_processed_input_consumes_win32_ctrl_c_and_sends_end_task", test_raw_read_processed_input_consumes_win32_ctrl_c_and_sends_end_task },
        { L"test_raw_read_consumes_da1_and_focus_sequences_before_character_key", test_raw_read_consumes_da1_and_focus_sequences_before_character_key },
        { L"test_raw_read_split_win32_sequence_reply_pends_and_drains_prefix", test_raw_read_split_win32_sequence_reply_pends_and_drains_prefix },
        { L"test_raw_read_process_control_z_consumes_one_byte_and_returns_zero", test_raw_read_process_control_z_consumes_one_byte_and_returns_zero },
        { L"test_raw_flush_clears_input_queue", test_raw_flush_clears_input_queue },
        { L"test_raw_write_rejects_input_handle", test_raw_write_rejects_input_handle },
        { L"test_user_defined_write_console_a_forwards_bytes", test_user_defined_write_console_a_forwards_bytes },
        { L"test_user_defined_write_console_w_utf8_encodes", test_user_defined_write_console_w_utf8_encodes },
        { L"test_user_defined_write_console_a_updates_screen_buffer_model", test_user_defined_write_console_a_updates_screen_buffer_model },
        { L"test_user_defined_write_console_w_updates_screen_buffer_model", test_user_defined_write_console_w_updates_screen_buffer_model },
        { L"test_write_console_newline_auto_return_resets_column", test_write_console_newline_auto_return_resets_column },
        { L"test_write_console_disable_newline_auto_return_preserves_column", test_write_console_disable_newline_auto_return_preserves_column },
        { L"test_write_console_vt_sgr_updates_attributes_and_strips_sequences", test_write_console_vt_sgr_updates_attributes_and_strips_sequences },
        { L"test_write_console_vt_sgr_normal_color_clears_bright_foreground_intensity", test_write_console_vt_sgr_normal_color_clears_bright_foreground_intensity },
        { L"test_write_console_vt_sgr_normal_color_clears_bright_background_intensity", test_write_console_vt_sgr_normal_color_clears_bright_background_intensity },
        { L"test_write_console_vt_sgr_extended_palette_index_sets_bright_red_foreground", test_write_console_vt_sgr_extended_palette_index_sets_bright_red_foreground },
        { L"test_write_console_vt_sgr_extended_truecolor_sets_bright_red_foreground", test_write_console_vt_sgr_extended_truecolor_sets_bright_red_foreground },
        { L"test_write_console_vt_sgr_extended_palette_index_sets_blue_background", test_write_console_vt_sgr_extended_palette_index_sets_blue_background },
        { L"test_write_console_vt_sgr_reverse_video_sets_common_lvb_reverse_video", test_write_console_vt_sgr_reverse_video_sets_common_lvb_reverse_video },
        { L"test_write_console_vt_sgr_underline_sets_common_lvb_underscore", test_write_console_vt_sgr_underline_sets_common_lvb_underscore },
        { L"test_write_console_vt_cup_moves_cursor", test_write_console_vt_cup_moves_cursor },
        { L"test_write_console_vt_c1_csi_cup_moves_cursor", test_write_console_vt_c1_csi_cup_moves_cursor },
        { L"test_write_console_vt_ed_clears_screen", test_write_console_vt_ed_clears_screen },
        { L"test_write_console_vt_c1_csi_ed_clears_screen", test_write_console_vt_c1_csi_ed_clears_screen },
        { L"test_write_console_vt_nel_moves_to_next_line_and_consumes_sequence", test_write_console_vt_nel_moves_to_next_line_and_consumes_sequence },
        { L"test_write_console_vt_charset_designation_is_consumed", test_write_console_vt_charset_designation_is_consumed },
        { L"test_write_console_vt_decaln_screen_alignment_pattern_fills_and_homes_cursor", test_write_console_vt_decaln_screen_alignment_pattern_fills_and_homes_cursor },
        { L"test_write_console_vt_el_clears_to_end_of_line", test_write_console_vt_el_clears_to_end_of_line },
        { L"test_write_console_vt_osc_title_updates_server_title_and_is_not_rendered", test_write_console_vt_osc_title_updates_server_title_and_is_not_rendered },
        { L"test_write_console_vt_split_osc_title_is_consumed_and_updates_state", test_write_console_vt_split_osc_title_is_consumed_and_updates_state },
        { L"test_write_console_vt_split_osc_st_terminator_is_consumed_and_updates_state", test_write_console_vt_split_osc_st_terminator_is_consumed_and_updates_state },
        { L"test_write_console_vt_split_csi_sequence_is_consumed", test_write_console_vt_split_csi_sequence_is_consumed },
        { L"test_write_console_vt_split_charset_designation_is_consumed", test_write_console_vt_split_charset_designation_is_consumed },
        { L"test_write_console_vt_split_dcs_string_is_consumed", test_write_console_vt_split_dcs_string_is_consumed },
        { L"test_write_console_vt_dsr_cpr_injects_response_into_input_queue", test_write_console_vt_dsr_cpr_injects_response_into_input_queue },
        { L"test_write_console_vt_dsr_cpr_respects_host_query_policy", test_write_console_vt_dsr_cpr_respects_host_query_policy },
        { L"test_write_console_vt_csi_save_restore_cursor_state", test_write_console_vt_csi_save_restore_cursor_state },
        { L"test_write_console_vt_decsc_decrc_save_restore_cursor_state", test_write_console_vt_decsc_decrc_save_restore_cursor_state },
        { L"test_write_console_vt_dectcem_toggles_cursor_visibility", test_write_console_vt_dectcem_toggles_cursor_visibility },
        { L"test_write_console_vt_delayed_wrap_allows_carriage_return_before_wrap", test_write_console_vt_delayed_wrap_allows_carriage_return_before_wrap },
        { L"test_write_console_vt_decawm_disable_prevents_wrap_and_overwrites_last_column", test_write_console_vt_decawm_disable_prevents_wrap_and_overwrites_last_column },
        { L"test_write_console_vt_origin_mode_homes_cursor_to_margin_top", test_write_console_vt_origin_mode_homes_cursor_to_margin_top },
        { L"test_write_console_vt_origin_mode_clamps_cursor_to_bottom_margin", test_write_console_vt_origin_mode_clamps_cursor_to_bottom_margin },
        { L"test_write_console_vt_alt_buffer_1049_clears_and_restores_main", test_write_console_vt_alt_buffer_1049_clears_and_restores_main },
        { L"test_write_console_vt_alt_buffer_1049_restores_cursor_visibility", test_write_console_vt_alt_buffer_1049_restores_cursor_visibility },
        { L"test_write_console_vt_decstbm_linefeed_scrolls_within_margins", test_write_console_vt_decstbm_linefeed_scrolls_within_margins },
        { L"test_write_console_vt_su_sd_scrolls_within_margins", test_write_console_vt_su_sd_scrolls_within_margins },
        { L"test_write_console_vt_il_inserts_lines_within_margins", test_write_console_vt_il_inserts_lines_within_margins },
        { L"test_write_console_vt_dl_deletes_lines_within_margins", test_write_console_vt_dl_deletes_lines_within_margins },
        { L"test_write_console_vt_ind_preserves_column", test_write_console_vt_ind_preserves_column },
        { L"test_write_console_vt_ich_inserts_characters_in_line", test_write_console_vt_ich_inserts_characters_in_line },
        { L"test_write_console_vt_dch_deletes_characters_in_line", test_write_console_vt_dch_deletes_characters_in_line },
        { L"test_write_console_vt_ech_erases_characters_in_line", test_write_console_vt_ech_erases_characters_in_line },
        { L"test_write_console_vt_irm_insert_mode_inserts_printable_cells", test_write_console_vt_irm_insert_mode_inserts_printable_cells },
        { L"test_write_console_vt_cuu_clamps_within_decstbm_when_origin_mode_disabled", test_write_console_vt_cuu_clamps_within_decstbm_when_origin_mode_disabled },
        { L"test_write_console_vt_cud_clamps_within_decstbm_when_origin_mode_disabled", test_write_console_vt_cud_clamps_within_decstbm_when_origin_mode_disabled },
        { L"test_write_console_vt_cnl_moves_to_column_one_and_respects_decstbm_margins", test_write_console_vt_cnl_moves_to_column_one_and_respects_decstbm_margins },
        { L"test_write_console_vt_cpl_moves_to_column_one_and_respects_decstbm_margins", test_write_console_vt_cpl_moves_to_column_one_and_respects_decstbm_margins },
        { L"test_write_console_vt_decstr_soft_reset_disables_irm", test_write_console_vt_decstr_soft_reset_disables_irm },
        { L"test_write_console_vt_decstr_soft_reset_resets_saved_cursor_state_to_home", test_write_console_vt_decstr_soft_reset_resets_saved_cursor_state_to_home },
        { L"test_write_console_vt_ris_hard_reset_clears_screen_and_homes_cursor", test_write_console_vt_ris_hard_reset_clears_screen_and_homes_cursor },
        { L"test_user_defined_read_console_a_writes_after_descriptor_offset", test_user_defined_read_console_a_writes_after_descriptor_offset },
        { L"test_user_defined_read_console_w_widens_bytes", test_user_defined_read_console_w_widens_bytes },
        { L"test_user_defined_read_console_w_decodes_utf8_bytes", test_user_defined_read_console_w_decodes_utf8_bytes },
        { L"test_user_defined_read_console_w_surrogate_pair_splits_across_reads", test_user_defined_read_console_w_surrogate_pair_splits_across_reads },
        { L"test_user_defined_read_console_w_raw_processed_input_skips_ctrl_c_mid_buffer_and_sends_end_task", test_user_defined_read_console_w_raw_processed_input_skips_ctrl_c_mid_buffer_and_sends_end_task },
        { L"test_user_defined_read_console_w_raw_processed_input_ctrl_break_returns_alerted_and_flushes_input", test_user_defined_read_console_w_raw_processed_input_ctrl_break_returns_alerted_and_flushes_input },
        { L"test_user_defined_read_console_a_raw_processed_input_consumes_ctrl_c_and_sends_end_task", test_user_defined_read_console_a_raw_processed_input_consumes_ctrl_c_and_sends_end_task },
        { L"test_user_defined_read_console_a_raw_processed_input_skips_ctrl_c_mid_buffer_and_sends_end_task", test_user_defined_read_console_a_raw_processed_input_skips_ctrl_c_mid_buffer_and_sends_end_task },
        { L"test_user_defined_read_console_w_line_input_returns_crlf_and_echoes", test_user_defined_read_console_w_line_input_returns_crlf_and_echoes },
        { L"test_user_defined_read_console_w_line_input_ctrl_c_returns_alerted_and_sends_end_task", test_user_defined_read_console_w_line_input_ctrl_c_returns_alerted_and_sends_end_task },
        { L"test_user_defined_read_console_w_line_input_ctrl_break_returns_alerted_and_flushes_input", test_user_defined_read_console_w_line_input_ctrl_break_returns_alerted_and_flushes_input },
        { L"test_user_defined_read_console_w_line_input_backspace_edits_and_echoes", test_user_defined_read_console_w_line_input_backspace_edits_and_echoes },
        { L"test_user_defined_read_console_w_line_input_small_buffer_sets_pending", test_user_defined_read_console_w_line_input_small_buffer_sets_pending },
        { L"test_user_defined_read_console_w_line_input_without_processed_returns_cr", test_user_defined_read_console_w_line_input_without_processed_returns_cr },
        { L"test_user_defined_read_console_a_line_input_returns_crlf", test_user_defined_read_console_a_line_input_returns_crlf },
        { L"test_user_defined_read_console_a_line_input_small_buffer_sets_pending", test_user_defined_read_console_a_line_input_small_buffer_sets_pending },
        { L"test_user_defined_read_console_a_line_input_utf8_buffer_too_small_for_multibyte_char", test_user_defined_read_console_a_line_input_utf8_buffer_too_small_for_multibyte_char },
        { L"test_user_defined_read_console_w_line_input_handles_split_utf8_sequence", test_user_defined_read_console_w_line_input_handles_split_utf8_sequence },
        { L"test_user_defined_read_console_w_line_input_insert_in_middle", test_user_defined_read_console_w_line_input_insert_in_middle },
        { L"test_user_defined_read_console_w_line_input_overwrite_toggle", test_user_defined_read_console_w_line_input_overwrite_toggle },
        { L"test_user_defined_read_console_w_line_input_delete_in_middle", test_user_defined_read_console_w_line_input_delete_in_middle },
        { L"test_user_defined_read_console_w_line_input_enter_with_cursor_mid_line", test_user_defined_read_console_w_line_input_enter_with_cursor_mid_line },
        { L"test_user_defined_read_console_w_line_input_escape_clears_line", test_user_defined_read_console_w_line_input_escape_clears_line },
        { L"test_user_defined_read_console_w_line_input_ctrl_home_deletes_to_start", test_user_defined_read_console_w_line_input_ctrl_home_deletes_to_start },
        { L"test_user_defined_read_console_w_line_input_ctrl_end_deletes_to_end", test_user_defined_read_console_w_line_input_ctrl_end_deletes_to_end },
        { L"test_l1_get_console_input_peek_does_not_consume", test_l1_get_console_input_peek_does_not_consume },
        { L"test_l1_get_console_input_remove_consumes_bytes", test_l1_get_console_input_remove_consumes_bytes },
        { L"test_l1_get_console_input_processed_input_skips_ctrl_c_on_remove_and_still_fills_records", test_l1_get_console_input_processed_input_skips_ctrl_c_on_remove_and_still_fills_records },
        { L"test_l1_get_console_input_processed_input_ctrl_break_flushes_and_reply_pends", test_l1_get_console_input_processed_input_ctrl_break_flushes_and_reply_pends },
        { L"test_l1_get_console_input_processed_input_skips_ctrl_c_on_peek_and_still_fills_records", test_l1_get_console_input_processed_input_skips_ctrl_c_on_peek_and_still_fills_records },
        { L"test_l1_get_console_input_utf8_decodes_to_unicode_records", test_l1_get_console_input_utf8_decodes_to_unicode_records },
        { L"test_l1_get_console_input_utf8_surrogate_pair_splits_across_reads", test_l1_get_console_input_utf8_surrogate_pair_splits_across_reads },
        { L"test_l2_write_console_input_injects_bytes", test_l2_write_console_input_injects_bytes },
        { L"test_l1_get_number_of_input_events_reports_available_bytes", test_l1_get_number_of_input_events_reports_available_bytes },
        { L"test_l1_get_number_of_input_events_counts_utf8_code_units", test_l1_get_number_of_input_events_counts_utf8_code_units },
        { L"test_l2_fill_console_output_characters_round_trips", test_l2_fill_console_output_characters_round_trips },
        { L"test_l2_fill_console_output_attributes_round_trips", test_l2_fill_console_output_attributes_round_trips },
        { L"test_l2_write_console_output_string_unicode_round_trips", test_l2_write_console_output_string_unicode_round_trips },
        { L"test_l2_set_and_get_title_round_trips", test_l2_set_and_get_title_round_trips },
        { L"test_l2_write_and_read_console_output_rect_round_trips", test_l2_write_and_read_console_output_rect_round_trips },
        { L"test_l2_scroll_console_screen_buffer_shifts_right", test_l2_scroll_console_screen_buffer_shifts_right },
        { L"test_l3_add_get_and_remove_console_alias_w_round_trips", test_l3_add_get_and_remove_console_alias_w_round_trips },
        { L"test_l3_get_console_aliases_length_and_get_aliases_w_round_trips", test_l3_get_console_aliases_length_and_get_aliases_w_round_trips },
        { L"test_l3_get_console_alias_exes_length_and_get_alias_exes_w_round_trips", test_l3_get_console_alias_exes_length_and_get_alias_exes_w_round_trips },
        { L"test_user_defined_deprecated_apis_return_not_implemented_and_zero_descriptor_bytes", test_user_defined_deprecated_apis_return_not_implemented_and_zero_descriptor_bytes },
    };

    for (const auto& test : tests)
    {
        if (test.run == nullptr)
        {
            fwprintf(stderr, L"[condrv raw] %ls was missing a runner\n", test.name);
            return false;
        }

        if (!test.run())
        {
            fwprintf(stderr, L"[condrv raw] %ls failed\n", test.name);
            return false;
        }
    }

    return true;
}
