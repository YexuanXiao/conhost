#include "condrv/condrv_server.hpp"

#include <Windows.h>

#include <array>
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <expected>
#include <span>
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
            const size_t to_copy = remaining < size ? remaining : size;
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

    struct StrictHostIo final
    {
        std::vector<std::byte> written;
        std::vector<std::byte> queue;
        size_t queue_offset{ 0 };
        bool disconnected{ false };
        bool wait_called{ false };
        std::vector<DWORD> end_task_pids;

        [[nodiscard]] std::expected<size_t, oc::condrv::DeviceCommError> write_output_bytes(std::span<const std::byte> bytes) noexcept
        {
            try
            {
                written.insert(written.end(), bytes.begin(), bytes.end());
                return bytes.size();
            }
            catch (...)
            {
                return std::unexpected(oc::condrv::DeviceCommError{
                    .context = L"write_output_bytes failed",
                    .win32_error = ERROR_OUTOFMEMORY,
                });
            }
        }

        [[nodiscard]] std::expected<size_t, oc::condrv::DeviceCommError> read_input_bytes(std::span<std::byte> dest) noexcept
        {
            const size_t remaining = input_bytes_available();
            const size_t to_copy = remaining < dest.size() ? remaining : dest.size();
            if (to_copy != 0)
            {
                std::memcpy(dest.data(), queue.data() + queue_offset, to_copy);
                queue_offset += to_copy;
            }
            return to_copy;
        }

        [[nodiscard]] std::expected<size_t, oc::condrv::DeviceCommError> peek_input_bytes(std::span<std::byte> dest) noexcept
        {
            const size_t remaining = input_bytes_available();
            const size_t to_copy = remaining < dest.size() ? remaining : dest.size();
            if (to_copy != 0)
            {
                std::memcpy(dest.data(), queue.data() + queue_offset, to_copy);
            }
            return to_copy;
        }

        [[nodiscard]] size_t input_bytes_available() const noexcept
        {
            if (queue_offset >= queue.size())
            {
                return 0;
            }
            return queue.size() - queue_offset;
        }

        [[nodiscard]] bool inject_input_bytes(std::span<const std::byte> bytes) noexcept
        {
            if (bytes.empty())
            {
                return true;
            }

            try
            {
                queue.insert(queue.end(), bytes.begin(), bytes.end());
                return true;
            }
            catch (...)
            {
                return false;
            }
        }

        [[nodiscard]] bool vt_should_answer_queries() const noexcept
        {
            return true;
        }

        [[nodiscard]] std::expected<void, oc::condrv::DeviceCommError> flush_input_buffer() noexcept
        {
            queue.clear();
            queue_offset = 0;
            return {};
        }

        [[nodiscard]] std::expected<bool, oc::condrv::DeviceCommError> wait_for_input(const DWORD /*timeout_ms*/) noexcept
        {
            wait_called = true;
            return std::unexpected(oc::condrv::DeviceCommError{
                .context = L"wait_for_input must not be called from dispatch_message",
                .win32_error = ERROR_INVALID_STATE,
            });
        }

        [[nodiscard]] bool input_disconnected() const noexcept
        {
            return disconnected;
        }

        [[nodiscard]] std::expected<void, oc::condrv::DeviceCommError> send_end_task(
            const DWORD process_id,
            const DWORD /*event_type*/,
            const DWORD /*ctrl_flags*/) noexcept
        {
            end_task_pids.push_back(process_id);
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

    [[nodiscard]] bool connect_to_server(
        MemoryComm& comm,
        oc::condrv::ServerState& state,
        StrictHostIo& host_io,
        const DWORD pid,
        const DWORD tid,
        oc::condrv::ConnectionInformation& out_info) noexcept
    {
        auto connect_packet = make_connect_packet(pid, tid);
        oc::condrv::BasicApiMessage<MemoryComm> connect_message(comm, connect_packet);
        auto outcome = oc::condrv::dispatch_message(state, connect_message, host_io);
        if (!outcome || connect_message.completion().io_status.Status != oc::core::status_success)
        {
            return false;
        }

        std::memcpy(&out_info, connect_message.completion().write.data, sizeof(out_info));
        return true;
    }

    [[nodiscard]] bool test_read_console_w_reply_pending_on_empty_input()
    {
        MemoryComm connect_comm{};
        oc::condrv::ServerState state{};
        StrictHostIo host_io{};

        oc::condrv::ConnectionInformation info{};
        if (!connect_to_server(connect_comm, state, host_io, 2221, 2222, info))
        {
            return false;
        }

        state.set_input_code_page(CP_UTF8);
        state.set_input_mode(0); // raw ReadConsole behavior

        MemoryComm comm{};

        constexpr ULONG api_size = sizeof(CONSOLE_READCONSOLE_MSG);
        constexpr ULONG header_size = sizeof(CONSOLE_MSG_HEADER);
        constexpr ULONG read_offset = api_size + header_size;

        oc::condrv::IoPacket packet{};
        packet.payload.user_defined = oc::condrv::UserDefinedPacket{};
        packet.descriptor.identifier.LowPart = 99;
        packet.descriptor.function = oc::condrv::console_io_user_defined;
        packet.descriptor.process = info.process;
        packet.descriptor.object = info.input;
        packet.descriptor.input_size = read_offset;
        packet.descriptor.output_size = api_size + static_cast<ULONG>(sizeof(wchar_t));
        packet.payload.user_defined.msg_header.ApiNumber = static_cast<ULONG>(ConsolepReadConsole);
        packet.payload.user_defined.msg_header.ApiDescriptorSize = api_size;
        packet.payload.user_defined.u.console_msg_l1.ReadConsole.Unicode = TRUE;

        comm.input.assign(read_offset, std::byte{});

        oc::condrv::BasicApiMessage<MemoryComm> message(comm, packet);
        auto outcome = oc::condrv::dispatch_message(state, message, host_io);
        if (!outcome || !outcome->reply_pending || host_io.wait_called)
        {
            return false;
        }

        const std::array<std::byte, 1> payload{ static_cast<std::byte>('Z') };
        if (!host_io.inject_input_bytes(payload))
        {
            return false;
        }

        outcome = oc::condrv::dispatch_message(state, message, host_io);
        if (!outcome || outcome->reply_pending)
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

        wchar_t value{};
        std::memcpy(&value, comm.output.data() + api_size, sizeof(value));
        return value == L'Z' && host_io.input_bytes_available() == 0;
    }

    [[nodiscard]] bool test_read_console_w_reply_pending_drains_split_utf8_sequence()
    {
        MemoryComm connect_comm{};
        oc::condrv::ServerState state{};
        StrictHostIo host_io{};

        oc::condrv::ConnectionInformation info{};
        if (!connect_to_server(connect_comm, state, host_io, 3331, 3332, info))
        {
            return false;
        }

        state.set_input_code_page(CP_UTF8);
        state.set_input_mode(0); // raw ReadConsole behavior

        const std::array<std::byte, 1> first{ static_cast<std::byte>(0xC3) };
        if (!host_io.inject_input_bytes(first))
        {
            return false;
        }

        MemoryComm comm{};

        constexpr ULONG api_size = sizeof(CONSOLE_READCONSOLE_MSG);
        constexpr ULONG header_size = sizeof(CONSOLE_MSG_HEADER);
        constexpr ULONG read_offset = api_size + header_size;

        oc::condrv::IoPacket packet{};
        packet.payload.user_defined = oc::condrv::UserDefinedPacket{};
        packet.descriptor.identifier.LowPart = 100;
        packet.descriptor.function = oc::condrv::console_io_user_defined;
        packet.descriptor.process = info.process;
        packet.descriptor.object = info.input;
        packet.descriptor.input_size = read_offset;
        packet.descriptor.output_size = api_size + static_cast<ULONG>(sizeof(wchar_t));
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

        const std::array<std::byte, 1> second{ static_cast<std::byte>(0xA9) };
        if (!host_io.inject_input_bytes(second))
        {
            return false;
        }

        outcome = oc::condrv::dispatch_message(state, message, host_io);
        if (!outcome || outcome->reply_pending)
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

        wchar_t value{};
        std::memcpy(&value, comm.output.data() + api_size, sizeof(value));
        return value == static_cast<wchar_t>(0x00E9) && host_io.input_bytes_available() == 0 && handle->pending_input_bytes.size() == 0;
    }

    [[nodiscard]] bool test_get_console_input_remove_reply_pending_drains_split_utf8_sequence()
    {
        MemoryComm connect_comm{};
        oc::condrv::ServerState state{};
        StrictHostIo host_io{};

        oc::condrv::ConnectionInformation info{};
        if (!connect_to_server(connect_comm, state, host_io, 4441, 4442, info))
        {
            return false;
        }

        state.set_input_code_page(CP_UTF8);
        state.set_input_mode(0);

        const std::array<std::byte, 1> first{ static_cast<std::byte>(0xC3) };
        if (!host_io.inject_input_bytes(first))
        {
            return false;
        }

        MemoryComm comm{};

        constexpr ULONG api_size = sizeof(CONSOLE_GETCONSOLEINPUT_MSG);
        constexpr ULONG header_size = sizeof(CONSOLE_MSG_HEADER);
        constexpr ULONG read_offset = api_size + header_size;

        oc::condrv::IoPacket packet{};
        packet.payload.user_defined = oc::condrv::UserDefinedPacket{};
        packet.descriptor.identifier.LowPart = 101;
        packet.descriptor.function = oc::condrv::console_io_user_defined;
        packet.descriptor.process = info.process;
        packet.descriptor.object = info.input;
        packet.descriptor.input_size = read_offset;
        packet.descriptor.output_size = api_size + static_cast<ULONG>(sizeof(INPUT_RECORD));
        packet.payload.user_defined.msg_header.ApiNumber = static_cast<ULONG>(ConsolepGetConsoleInput);
        packet.payload.user_defined.msg_header.ApiDescriptorSize = api_size;

        auto& body = packet.payload.user_defined.u.console_msg_l1.GetConsoleInput;
        body.NumRecords = 0;
        body.Flags = 0; // wait allowed + remove
        body.Unicode = TRUE;

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

        const std::array<std::byte, 1> second{ static_cast<std::byte>(0xA9) };
        if (!host_io.inject_input_bytes(second))
        {
            return false;
        }

        outcome = oc::condrv::dispatch_message(state, message, host_io);
        if (!outcome || outcome->reply_pending)
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

        if (comm.output.size() != api_size + sizeof(INPUT_RECORD))
        {
            return false;
        }

        INPUT_RECORD record{};
        std::memcpy(&record, comm.output.data() + api_size, sizeof(record));
        if (record.EventType != KEY_EVENT)
        {
            return false;
        }

        return record.Event.KeyEvent.uChar.UnicodeChar == static_cast<wchar_t>(0x00E9) &&
               host_io.input_bytes_available() == 0 &&
               handle->pending_input_bytes.size() == 0;
    }

    [[nodiscard]] bool test_get_console_input_decodes_win32_input_mode_key_event()
    {
        MemoryComm connect_comm{};
        oc::condrv::ServerState state{};
        StrictHostIo host_io{};

        oc::condrv::ConnectionInformation info{};
        if (!connect_to_server(connect_comm, state, host_io, 7771, 7772, info))
        {
            return false;
        }

        state.set_input_code_page(CP_UTF8);
        state.set_input_mode(0);

        constexpr std::string_view seq = "\x1b[65;0;97;1;0;1_";
        const auto bytes = std::span<const std::byte>(
            reinterpret_cast<const std::byte*>(seq.data()),
            seq.size());
        if (!host_io.inject_input_bytes(bytes))
        {
            return false;
        }

        MemoryComm comm{};

        constexpr ULONG api_size = sizeof(CONSOLE_GETCONSOLEINPUT_MSG);
        constexpr ULONG header_size = sizeof(CONSOLE_MSG_HEADER);
        constexpr ULONG read_offset = api_size + header_size;

        oc::condrv::IoPacket packet{};
        packet.payload.user_defined = oc::condrv::UserDefinedPacket{};
        packet.descriptor.identifier.LowPart = 110;
        packet.descriptor.function = oc::condrv::console_io_user_defined;
        packet.descriptor.process = info.process;
        packet.descriptor.object = info.input;
        packet.descriptor.input_size = read_offset;
        packet.descriptor.output_size = api_size + static_cast<ULONG>(sizeof(INPUT_RECORD));
        packet.payload.user_defined.msg_header.ApiNumber = static_cast<ULONG>(ConsolepGetConsoleInput);
        packet.payload.user_defined.msg_header.ApiDescriptorSize = api_size;

        auto& body = packet.payload.user_defined.u.console_msg_l1.GetConsoleInput;
        body.NumRecords = 0;
        body.Flags = 0; // wait allowed + remove
        body.Unicode = TRUE;

        comm.input.assign(read_offset, std::byte{});

        oc::condrv::BasicApiMessage<MemoryComm> message(comm, packet);
        auto outcome = oc::condrv::dispatch_message(state, message, host_io);
        if (!outcome || outcome->reply_pending)
        {
            return false;
        }

        if (message.completion().io_status.Status != oc::core::status_success ||
            message.packet().payload.user_defined.u.console_msg_l1.GetConsoleInput.NumRecords != 1)
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
        if (record.EventType != KEY_EVENT)
        {
            return false;
        }

        const auto& key = record.Event.KeyEvent;
        return key.bKeyDown == TRUE &&
               key.wVirtualKeyCode == 65 &&
               key.wVirtualScanCode == 0 &&
               key.wRepeatCount == 1 &&
               key.uChar.UnicodeChar == L'a' &&
               key.dwControlKeyState == 0 &&
               host_io.input_bytes_available() == 0;
    }

    [[nodiscard]] bool test_get_console_input_decodes_win32_input_mode_arrow_key()
    {
        MemoryComm connect_comm{};
        oc::condrv::ServerState state{};
        StrictHostIo host_io{};

        oc::condrv::ConnectionInformation info{};
        if (!connect_to_server(connect_comm, state, host_io, 7773, 7774, info))
        {
            return false;
        }

        state.set_input_code_page(CP_UTF8);
        state.set_input_mode(0);

        constexpr std::string_view seq = "\x1b[38;0;0;1;0;1_";
        const auto bytes = std::span<const std::byte>(
            reinterpret_cast<const std::byte*>(seq.data()),
            seq.size());
        if (!host_io.inject_input_bytes(bytes))
        {
            return false;
        }

        MemoryComm comm{};

        constexpr ULONG api_size = sizeof(CONSOLE_GETCONSOLEINPUT_MSG);
        constexpr ULONG header_size = sizeof(CONSOLE_MSG_HEADER);
        constexpr ULONG read_offset = api_size + header_size;

        oc::condrv::IoPacket packet{};
        packet.payload.user_defined = oc::condrv::UserDefinedPacket{};
        packet.descriptor.identifier.LowPart = 111;
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
        if (!outcome || outcome->reply_pending)
        {
            return false;
        }

        if (message.completion().io_status.Status != oc::core::status_success ||
            message.packet().payload.user_defined.u.console_msg_l1.GetConsoleInput.NumRecords != 1)
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
        if (record.EventType != KEY_EVENT)
        {
            return false;
        }

        const auto& key = record.Event.KeyEvent;
        return key.bKeyDown == TRUE &&
               key.wVirtualKeyCode == VK_UP &&
               key.uChar.UnicodeChar == 0 &&
               host_io.input_bytes_available() == 0;
    }

    [[nodiscard]] bool test_read_console_w_ignores_arrow_keys_and_pends()
    {
        MemoryComm connect_comm{};
        oc::condrv::ServerState state{};
        StrictHostIo host_io{};

        oc::condrv::ConnectionInformation info{};
        if (!connect_to_server(connect_comm, state, host_io, 8881, 8882, info))
        {
            return false;
        }

        state.set_input_code_page(CP_UTF8);
        state.set_input_mode(0);

        constexpr std::string_view seq = "\x1b[A";
        const auto bytes = std::span<const std::byte>(
            reinterpret_cast<const std::byte*>(seq.data()),
            seq.size());
        if (!host_io.inject_input_bytes(bytes))
        {
            return false;
        }

        MemoryComm comm{};

        constexpr ULONG api_size = sizeof(CONSOLE_READCONSOLE_MSG);
        constexpr ULONG header_size = sizeof(CONSOLE_MSG_HEADER);
        constexpr ULONG read_offset = api_size + header_size;

        oc::condrv::IoPacket packet{};
        packet.payload.user_defined = oc::condrv::UserDefinedPacket{};
        packet.descriptor.identifier.LowPart = 120;
        packet.descriptor.function = oc::condrv::console_io_user_defined;
        packet.descriptor.process = info.process;
        packet.descriptor.object = info.input;
        packet.descriptor.input_size = read_offset;
        packet.descriptor.output_size = api_size + static_cast<ULONG>(sizeof(wchar_t));
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
        return host_io.input_bytes_available() == 0 &&
               handle != nullptr &&
               handle->pending_input_bytes.size() == 0;
    }

    [[nodiscard]] bool test_split_win32_sequence_reply_pends_and_drains_prefix()
    {
        MemoryComm connect_comm{};
        oc::condrv::ServerState state{};
        StrictHostIo host_io{};

        oc::condrv::ConnectionInformation info{};
        if (!connect_to_server(connect_comm, state, host_io, 9991, 9992, info))
        {
            return false;
        }

        state.set_input_code_page(CP_UTF8);
        state.set_input_mode(0);

        constexpr std::string_view fragment1 = "\x1b[65;0;";
        const auto bytes1 = std::span<const std::byte>(
            reinterpret_cast<const std::byte*>(fragment1.data()),
            fragment1.size());
        if (!host_io.inject_input_bytes(bytes1))
        {
            return false;
        }

        MemoryComm comm{};

        constexpr ULONG api_size = sizeof(CONSOLE_GETCONSOLEINPUT_MSG);
        constexpr ULONG header_size = sizeof(CONSOLE_MSG_HEADER);
        constexpr ULONG read_offset = api_size + header_size;

        oc::condrv::IoPacket packet{};
        packet.payload.user_defined = oc::condrv::UserDefinedPacket{};
        packet.descriptor.identifier.LowPart = 121;
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
        if (!outcome || !outcome->reply_pending)
        {
            return false;
        }

        auto* handle = state.find_object(info.input);
        if (handle == nullptr || handle->pending_input_bytes.size() == 0 || host_io.input_bytes_available() != 0)
        {
            return false;
        }

        constexpr std::string_view fragment2 = "97;1;0;1_";
        const auto bytes2 = std::span<const std::byte>(
            reinterpret_cast<const std::byte*>(fragment2.data()),
            fragment2.size());
        if (!host_io.inject_input_bytes(bytes2))
        {
            return false;
        }

        outcome = oc::condrv::dispatch_message(state, message, host_io);
        if (!outcome || outcome->reply_pending)
        {
            return false;
        }

        if (message.completion().io_status.Status != oc::core::status_success ||
            message.packet().payload.user_defined.u.console_msg_l1.GetConsoleInput.NumRecords != 1)
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
        if (record.EventType != KEY_EVENT)
        {
            return false;
        }

        return record.Event.KeyEvent.bKeyDown == TRUE &&
               record.Event.KeyEvent.uChar.UnicodeChar == L'a' &&
               host_io.input_bytes_available() == 0 &&
               handle->pending_input_bytes.size() == 0;
    }

    [[nodiscard]] bool test_da1_and_focus_sequences_are_consumed_not_delivered()
    {
        MemoryComm connect_comm{};
        oc::condrv::ServerState state{};
        StrictHostIo host_io{};

        oc::condrv::ConnectionInformation info{};
        if (!connect_to_server(connect_comm, state, host_io, 10001, 10002, info))
        {
            return false;
        }

        state.set_input_code_page(CP_UTF8);
        state.set_input_mode(0);

        constexpr std::string_view seq =
            "\x1b[?62;c"
            "\x1b[I"
            "\x1b[O"
            "\x1b[65;0;97;1;0;1_";
        const auto bytes = std::span<const std::byte>(
            reinterpret_cast<const std::byte*>(seq.data()),
            seq.size());
        if (!host_io.inject_input_bytes(bytes))
        {
            return false;
        }

        MemoryComm comm{};

        constexpr ULONG api_size = sizeof(CONSOLE_GETCONSOLEINPUT_MSG);
        constexpr ULONG header_size = sizeof(CONSOLE_MSG_HEADER);
        constexpr ULONG read_offset = api_size + header_size;

        oc::condrv::IoPacket packet{};
        packet.payload.user_defined = oc::condrv::UserDefinedPacket{};
        packet.descriptor.identifier.LowPart = 122;
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
        if (!outcome || outcome->reply_pending)
        {
            return false;
        }

        if (message.completion().io_status.Status != oc::core::status_success ||
            message.packet().payload.user_defined.u.console_msg_l1.GetConsoleInput.NumRecords != 1)
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
        if (record.EventType != KEY_EVENT)
        {
            return false;
        }

        auto* handle = state.find_object(info.input);
        return record.Event.KeyEvent.uChar.UnicodeChar == L'a' &&
               host_io.input_bytes_available() == 0 &&
               handle != nullptr &&
               handle->pending_input_bytes.size() == 0;
    }

    [[nodiscard]] bool test_read_console_a_decodes_win32_input_mode_character_key()
    {
        MemoryComm connect_comm{};
        oc::condrv::ServerState state{};
        StrictHostIo host_io{};

        oc::condrv::ConnectionInformation info{};
        if (!connect_to_server(connect_comm, state, host_io, 11001, 11002, info))
        {
            return false;
        }

        state.set_input_code_page(CP_UTF8);
        state.set_input_mode(0);

        constexpr std::string_view seq = "\x1b[65;0;97;1;0;1_";
        const auto bytes = std::span<const std::byte>(
            reinterpret_cast<const std::byte*>(seq.data()),
            seq.size());
        if (!host_io.inject_input_bytes(bytes))
        {
            return false;
        }

        MemoryComm comm{};

        constexpr ULONG api_size = sizeof(CONSOLE_READCONSOLE_MSG);
        constexpr ULONG header_size = sizeof(CONSOLE_MSG_HEADER);
        constexpr ULONG read_offset = api_size + header_size;

        oc::condrv::IoPacket packet{};
        packet.payload.user_defined = oc::condrv::UserDefinedPacket{};
        packet.descriptor.identifier.LowPart = 123;
        packet.descriptor.function = oc::condrv::console_io_user_defined;
        packet.descriptor.process = info.process;
        packet.descriptor.object = info.input;
        packet.descriptor.input_size = read_offset;
        packet.descriptor.output_size = api_size + 1;
        packet.payload.user_defined.msg_header.ApiNumber = static_cast<ULONG>(ConsolepReadConsole);
        packet.payload.user_defined.msg_header.ApiDescriptorSize = api_size;
        packet.payload.user_defined.u.console_msg_l1.ReadConsole.Unicode = FALSE;

        comm.input.assign(read_offset, std::byte{});

        oc::condrv::BasicApiMessage<MemoryComm> message(comm, packet);
        auto outcome = oc::condrv::dispatch_message(state, message, host_io);
        if (!outcome || outcome->reply_pending)
        {
            return false;
        }

        if (message.completion().io_status.Status != oc::core::status_success ||
            message.packet().payload.user_defined.u.console_msg_l1.ReadConsole.NumBytes != 1)
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

        return comm.output[api_size] == static_cast<std::byte>('a') &&
               host_io.input_bytes_available() == 0;
    }

    [[nodiscard]] bool test_dispatch_reply_pending_does_not_block_other_requests()
    {
        MemoryComm connect_comm{};
        oc::condrv::ServerState state{};
        StrictHostIo host_io{};

        oc::condrv::ConnectionInformation info{};
        if (!connect_to_server(connect_comm, state, host_io, 5551, 5552, info))
        {
            return false;
        }

        state.set_input_code_page(CP_UTF8);
        state.set_input_mode(0); // raw reads pend when empty

        MemoryComm read_comm{};
        constexpr ULONG api_size = sizeof(CONSOLE_READCONSOLE_MSG);
        constexpr ULONG header_size = sizeof(CONSOLE_MSG_HEADER);
        constexpr ULONG read_offset = api_size + header_size;

        oc::condrv::IoPacket read_packet{};
        read_packet.payload.user_defined = oc::condrv::UserDefinedPacket{};
        read_packet.descriptor.identifier.LowPart = 200;
        read_packet.descriptor.function = oc::condrv::console_io_user_defined;
        read_packet.descriptor.process = info.process;
        read_packet.descriptor.object = info.input;
        read_packet.descriptor.input_size = read_offset;
        read_packet.descriptor.output_size = api_size + static_cast<ULONG>(sizeof(wchar_t));
        read_packet.payload.user_defined.msg_header.ApiNumber = static_cast<ULONG>(ConsolepReadConsole);
        read_packet.payload.user_defined.msg_header.ApiDescriptorSize = api_size;
        read_packet.payload.user_defined.u.console_msg_l1.ReadConsole.Unicode = TRUE;

        read_comm.input.assign(read_offset, std::byte{});

        oc::condrv::BasicApiMessage<MemoryComm> pending_message(read_comm, read_packet);
        auto outcome = oc::condrv::dispatch_message(state, pending_message, host_io);
        if (!outcome || !outcome->reply_pending)
        {
            return false;
        }

        MemoryComm write_comm{};
        write_comm.input = { static_cast<std::byte>('O'), static_cast<std::byte>('K') };

        oc::condrv::IoPacket write_packet{};
        write_packet.descriptor.identifier.LowPart = 201;
        write_packet.descriptor.function = oc::condrv::console_io_raw_write;
        write_packet.descriptor.process = info.process;
        write_packet.descriptor.object = info.output;
        write_packet.descriptor.input_size = static_cast<ULONG>(write_comm.input.size());
        write_packet.descriptor.output_size = 0;

        oc::condrv::BasicApiMessage<MemoryComm> write_message(write_comm, write_packet);
        outcome = oc::condrv::dispatch_message(state, write_message, host_io);
        if (!outcome || outcome->reply_pending)
        {
            return false;
        }

        return write_message.completion().io_status.Status == oc::core::status_success &&
               write_message.completion().io_status.Information == write_comm.input.size() &&
               host_io.written.size() >= write_comm.input.size();
    }

    [[nodiscard]] bool test_pending_read_completes_with_failure_when_input_disconnects()
    {
        MemoryComm connect_comm{};
        oc::condrv::ServerState state{};
        StrictHostIo host_io{};

        oc::condrv::ConnectionInformation info{};
        if (!connect_to_server(connect_comm, state, host_io, 6661, 6662, info))
        {
            return false;
        }

        state.set_input_code_page(CP_UTF8);
        state.set_input_mode(0);

        MemoryComm comm{};

        constexpr ULONG api_size = sizeof(CONSOLE_READCONSOLE_MSG);
        constexpr ULONG header_size = sizeof(CONSOLE_MSG_HEADER);
        constexpr ULONG read_offset = api_size + header_size;

        oc::condrv::IoPacket packet{};
        packet.payload.user_defined = oc::condrv::UserDefinedPacket{};
        packet.descriptor.identifier.LowPart = 300;
        packet.descriptor.function = oc::condrv::console_io_user_defined;
        packet.descriptor.process = info.process;
        packet.descriptor.object = info.input;
        packet.descriptor.input_size = read_offset;
        packet.descriptor.output_size = api_size + static_cast<ULONG>(sizeof(wchar_t));
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

        host_io.disconnected = true;
        outcome = oc::condrv::dispatch_message(state, message, host_io);
        if (!outcome || outcome->reply_pending)
        {
            return false;
        }

        return message.completion().io_status.Status == oc::core::status_unsuccessful &&
               message.completion().io_status.Information == 0;
    }
}

bool run_condrv_input_wait_tests()
{
    if (!test_read_console_w_reply_pending_on_empty_input())
    {
        fwprintf(stderr, L"[condrv wait] test_read_console_w_reply_pending_on_empty_input failed\n");
        return false;
    }

    if (!test_read_console_w_reply_pending_drains_split_utf8_sequence())
    {
        fwprintf(stderr, L"[condrv wait] test_read_console_w_reply_pending_drains_split_utf8_sequence failed\n");
        return false;
    }

    if (!test_get_console_input_remove_reply_pending_drains_split_utf8_sequence())
    {
        fwprintf(stderr, L"[condrv wait] test_get_console_input_remove_reply_pending_drains_split_utf8_sequence failed\n");
        return false;
    }

    if (!test_get_console_input_decodes_win32_input_mode_key_event())
    {
        fwprintf(stderr, L"[condrv wait] test_get_console_input_decodes_win32_input_mode_key_event failed\n");
        return false;
    }

    if (!test_get_console_input_decodes_win32_input_mode_arrow_key())
    {
        fwprintf(stderr, L"[condrv wait] test_get_console_input_decodes_win32_input_mode_arrow_key failed\n");
        return false;
    }

    if (!test_read_console_w_ignores_arrow_keys_and_pends())
    {
        fwprintf(stderr, L"[condrv wait] test_read_console_w_ignores_arrow_keys_and_pends failed\n");
        return false;
    }

    if (!test_split_win32_sequence_reply_pends_and_drains_prefix())
    {
        fwprintf(stderr, L"[condrv wait] test_split_win32_sequence_reply_pends_and_drains_prefix failed\n");
        return false;
    }

    if (!test_da1_and_focus_sequences_are_consumed_not_delivered())
    {
        fwprintf(stderr, L"[condrv wait] test_da1_and_focus_sequences_are_consumed_not_delivered failed\n");
        return false;
    }

    if (!test_read_console_a_decodes_win32_input_mode_character_key())
    {
        fwprintf(stderr, L"[condrv wait] test_read_console_a_decodes_win32_input_mode_character_key failed\n");
        return false;
    }

    if (!test_dispatch_reply_pending_does_not_block_other_requests())
    {
        fwprintf(stderr, L"[condrv wait] test_dispatch_reply_pending_does_not_block_other_requests failed\n");
        return false;
    }

    if (!test_pending_read_completes_with_failure_when_input_disconnects())
    {
        fwprintf(stderr, L"[condrv wait] test_pending_read_completes_with_failure_when_input_disconnects failed\n");
        return false;
    }

    return true;
}
