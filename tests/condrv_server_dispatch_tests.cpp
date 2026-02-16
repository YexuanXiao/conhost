#include "condrv/condrv_server.hpp"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <optional>
#include <vector>

namespace
{
    struct DummyComm final
    {
        std::vector<std::byte> input;
        std::vector<std::byte> output;

        [[nodiscard]] std::expected<void, oc::condrv::DeviceCommError> read_input(oc::condrv::IoOperation& operation) noexcept
        {
            const auto offset = static_cast<size_t>(operation.buffer.offset);
            const auto size = static_cast<size_t>(operation.buffer.size);
            if (offset + size > input.size())
            {
                return std::unexpected(oc::condrv::DeviceCommError{
                    .context = L"DummyComm read_input out of range",
                    .win32_error = ERROR_INVALID_DATA,
                });
            }

            if (size != 0)
            {
                std::memcpy(operation.buffer.data, input.data() + offset, size);
            }

            return {};
        }

        [[nodiscard]] std::expected<void, oc::condrv::DeviceCommError> write_output(oc::condrv::IoOperation& operation) noexcept
        {
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

    [[nodiscard]] oc::condrv::IoPacket make_connect_packet(const DWORD pid, const DWORD tid) noexcept
    {
        oc::condrv::IoPacket packet{};
        packet.descriptor.identifier.LowPart = 1;
        packet.descriptor.function = oc::condrv::console_io_connect;
        packet.descriptor.process = pid;
        packet.descriptor.object = tid;
        return packet;
    }

    [[nodiscard]] oc::condrv::IoPacket make_disconnect_packet(const ULONG_PTR process_handle) noexcept
    {
        oc::condrv::IoPacket packet{};
        packet.descriptor.identifier.LowPart = 2;
        packet.descriptor.function = oc::condrv::console_io_disconnect;
        packet.descriptor.process = process_handle;
        return packet;
    }

    [[nodiscard]] oc::condrv::IoPacket make_create_object_packet(
        const ULONG_PTR process_handle,
        const ULONG object_type,
        const ACCESS_MASK desired_access,
        const ULONG share_mode) noexcept
    {
        oc::condrv::IoPacket packet{};
        packet.descriptor.identifier.LowPart = 3;
        packet.descriptor.function = oc::condrv::console_io_create_object;
        packet.descriptor.process = process_handle;
        packet.payload.create_object.create_object.object_type = object_type;
        packet.payload.create_object.create_object.desired_access = desired_access;
        packet.payload.create_object.create_object.share_mode = share_mode;
        return packet;
    }

    [[nodiscard]] oc::condrv::IoPacket make_close_object_packet(const ULONG_PTR handle_id) noexcept
    {
        oc::condrv::IoPacket packet{};
        packet.descriptor.identifier.LowPart = 4;
        packet.descriptor.function = oc::condrv::console_io_close_object;
        packet.descriptor.object = handle_id;
        return packet;
    }

    struct CtrlCaptureHostIo final
    {
        std::vector<DWORD> end_task_pids;

        [[nodiscard]] std::expected<size_t, oc::condrv::DeviceCommError> write_output_bytes(std::span<const std::byte> bytes) noexcept
        {
            return bytes.size();
        }

        [[nodiscard]] std::expected<size_t, oc::condrv::DeviceCommError> read_input_bytes(std::span<std::byte> /*dest*/) noexcept
        {
            return size_t{ 0 };
        }

        [[nodiscard]] std::expected<size_t, oc::condrv::DeviceCommError> peek_input_bytes(std::span<std::byte> /*dest*/) noexcept
        {
            return size_t{ 0 };
        }

        [[nodiscard]] size_t input_bytes_available() const noexcept
        {
            return 0;
        }

        [[nodiscard]] bool inject_input_bytes(std::span<const std::byte> /*bytes*/) noexcept
        {
            return true;
        }

        [[nodiscard]] bool vt_should_answer_queries() const noexcept
        {
            return true;
        }

        [[nodiscard]] std::expected<void, oc::condrv::DeviceCommError> flush_input_buffer() noexcept
        {
            return {};
        }

        [[nodiscard]] std::expected<bool, oc::condrv::DeviceCommError> wait_for_input(const DWORD /*timeout_ms*/) noexcept
        {
            return false;
        }

        [[nodiscard]] bool input_disconnected() const noexcept
        {
            return false;
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

    bool test_connect_and_disconnect_lifecycle()
    {
        DummyComm comm{};
        oc::condrv::ServerState state{};
        oc::condrv::NullHostIo host_io{};

        auto connect_packet = make_connect_packet(1234, 5678);
        oc::condrv::BasicApiMessage<DummyComm> connect_message(comm, connect_packet);

        auto connect_outcome = oc::condrv::dispatch_message(state, connect_message, host_io);
        if (!connect_outcome)
        {
            return false;
        }

        if (connect_message.completion().io_status.Status != oc::core::status_success)
        {
            return false;
        }

        if (connect_message.completion().io_status.Information != sizeof(oc::condrv::ConnectionInformation))
        {
            return false;
        }

        const auto& write = connect_message.completion().write;
        if (write.data == nullptr || write.size != sizeof(oc::condrv::ConnectionInformation))
        {
            return false;
        }

        oc::condrv::ConnectionInformation info{};
        std::memcpy(&info, write.data, sizeof(info));

        if (info.process == 0 || info.input == 0 || info.output == 0)
        {
            return false;
        }

        if (state.process_count() != 1)
        {
            return false;
        }

        auto disconnect_packet = make_disconnect_packet(info.process);
        oc::condrv::BasicApiMessage<DummyComm> disconnect_message(comm, disconnect_packet);

        auto disconnect_outcome = oc::condrv::dispatch_message(state, disconnect_message, host_io);
        if (!disconnect_outcome)
        {
            return false;
        }

        if (disconnect_message.completion().io_status.Status != oc::core::status_success)
        {
            return false;
        }

        return disconnect_outcome->request_exit && state.process_count() == 0;
    }

    bool test_create_and_close_object()
    {
        DummyComm comm{};
        oc::condrv::ServerState state{};
        oc::condrv::NullHostIo host_io{};

        auto connect_packet = make_connect_packet(42, 7);
        oc::condrv::BasicApiMessage<DummyComm> connect_message(comm, connect_packet);
        auto connect_outcome = oc::condrv::dispatch_message(state, connect_message, host_io);
        if (!connect_outcome)
        {
            return false;
        }

        oc::condrv::ConnectionInformation info{};
        std::memcpy(&info, connect_message.completion().write.data, sizeof(info));

        auto create_packet = make_create_object_packet(
            info.process,
            oc::condrv::io_object_type_generic,
            GENERIC_READ,
            FILE_SHARE_READ | FILE_SHARE_WRITE);

        oc::condrv::BasicApiMessage<DummyComm> create_message(comm, create_packet);
        auto create_outcome = oc::condrv::dispatch_message(state, create_message, host_io);
        if (!create_outcome)
        {
            return false;
        }

        if (create_message.completion().io_status.Status != oc::core::status_success)
        {
            return false;
        }

        const auto handle_id = static_cast<ULONG_PTR>(create_message.completion().io_status.Information);
        if (handle_id == 0)
        {
            return false;
        }

        auto close_packet = make_close_object_packet(handle_id);
        oc::condrv::BasicApiMessage<DummyComm> close_message(comm, close_packet);
        auto close_outcome = oc::condrv::dispatch_message(state, close_message, host_io);
        if (!close_outcome)
        {
            return false;
        }

        if (close_message.completion().io_status.Status != oc::core::status_success)
        {
            return false;
        }

        auto disconnect_packet = make_disconnect_packet(info.process);
        oc::condrv::BasicApiMessage<DummyComm> disconnect_message(comm, disconnect_packet);
        auto disconnect_outcome = oc::condrv::dispatch_message(state, disconnect_message, host_io);
        if (!disconnect_outcome)
        {
            return false;
        }

        return true;
    }

    bool test_create_object_requires_process_handle()
    {
        DummyComm comm{};
        oc::condrv::ServerState state{};
        oc::condrv::NullHostIo host_io{};

        auto create_packet = make_create_object_packet(
            0xDEADBEEF,
            oc::condrv::io_object_type_current_output,
            GENERIC_WRITE,
            FILE_SHARE_READ | FILE_SHARE_WRITE);

        oc::condrv::BasicApiMessage<DummyComm> message(comm, create_packet);
        auto outcome = oc::condrv::dispatch_message(state, message, host_io);
        if (!outcome)
        {
            return false;
        }

        return message.completion().io_status.Status == oc::core::status_invalid_handle;
    }

    bool test_new_output_is_supported()
    {
        DummyComm comm{};
        oc::condrv::ServerState state{};
        oc::condrv::NullHostIo host_io{};

        auto connect_packet = make_connect_packet(88, 99);
        oc::condrv::BasicApiMessage<DummyComm> connect_message(comm, connect_packet);
        auto connect_outcome = oc::condrv::dispatch_message(state, connect_message, host_io);
        if (!connect_outcome)
        {
            return false;
        }

        oc::condrv::ConnectionInformation info{};
        std::memcpy(&info, connect_message.completion().write.data, sizeof(info));

        auto create_packet = make_create_object_packet(
            info.process,
            oc::condrv::io_object_type_new_output,
            GENERIC_WRITE,
            FILE_SHARE_READ | FILE_SHARE_WRITE);

        oc::condrv::BasicApiMessage<DummyComm> message(comm, create_packet);
        auto outcome = oc::condrv::dispatch_message(state, message, host_io);
        if (!outcome)
        {
            return false;
        }

        if (message.completion().io_status.Status != oc::core::status_success)
        {
            return false;
        }

        const auto handle_id = static_cast<ULONG_PTR>(message.completion().io_status.Information);
        if (handle_id == 0)
        {
            return false;
        }

        auto close_packet = make_close_object_packet(handle_id);
        oc::condrv::BasicApiMessage<DummyComm> close_message(comm, close_packet);
        auto close_outcome = oc::condrv::dispatch_message(state, close_message, host_io);
        if (!close_outcome)
        {
            return false;
        }

        return close_message.completion().io_status.Status == oc::core::status_success;
    }

    bool test_disconnect_closes_owned_objects()
    {
        DummyComm comm{};
        oc::condrv::ServerState state{};
        oc::condrv::NullHostIo host_io{};

        auto connect_packet = make_connect_packet(200, 300);
        oc::condrv::BasicApiMessage<DummyComm> connect_message(comm, connect_packet);
        auto connect_outcome = oc::condrv::dispatch_message(state, connect_message, host_io);
        if (!connect_outcome || connect_message.completion().io_status.Status != oc::core::status_success)
        {
            return false;
        }

        oc::condrv::ConnectionInformation info{};
        std::memcpy(&info, connect_message.completion().write.data, sizeof(info));

        auto create_packet = make_create_object_packet(
            info.process,
            oc::condrv::io_object_type_new_output,
            GENERIC_WRITE,
            FILE_SHARE_READ | FILE_SHARE_WRITE);
        oc::condrv::BasicApiMessage<DummyComm> create_message(comm, create_packet);
        auto create_outcome = oc::condrv::dispatch_message(state, create_message, host_io);
        if (!create_outcome || create_message.completion().io_status.Status != oc::core::status_success)
        {
            return false;
        }

        const auto new_output = static_cast<ULONG_PTR>(create_message.completion().io_status.Information);
        if (new_output == 0)
        {
            return false;
        }

        auto disconnect_packet = make_disconnect_packet(info.process);
        oc::condrv::BasicApiMessage<DummyComm> disconnect_message(comm, disconnect_packet);
        auto disconnect_outcome = oc::condrv::dispatch_message(state, disconnect_message, host_io);
        if (!disconnect_outcome || disconnect_message.completion().io_status.Status != oc::core::status_success)
        {
            return false;
        }

        auto close_packet = make_close_object_packet(new_output);
        oc::condrv::BasicApiMessage<DummyComm> close_message(comm, close_packet);
        auto close_outcome = oc::condrv::dispatch_message(state, close_message, host_io);
        return close_outcome && close_message.completion().io_status.Status == oc::core::status_invalid_handle;
    }

    bool test_new_output_has_independent_screen_buffer_state()
    {
        DummyComm comm{};
        oc::condrv::ServerState state{};
        oc::condrv::NullHostIo host_io{};

        auto connect_packet = make_connect_packet(13, 37);
        oc::condrv::BasicApiMessage<DummyComm> connect_message(comm, connect_packet);
        auto connect_outcome = oc::condrv::dispatch_message(state, connect_message, host_io);
        if (!connect_outcome || connect_message.completion().io_status.Status != oc::core::status_success)
        {
            return false;
        }

        oc::condrv::ConnectionInformation info{};
        std::memcpy(&info, connect_message.completion().write.data, sizeof(info));

        auto create_packet = make_create_object_packet(
            info.process,
            oc::condrv::io_object_type_new_output,
            GENERIC_WRITE,
            FILE_SHARE_READ | FILE_SHARE_WRITE);
        oc::condrv::BasicApiMessage<DummyComm> create_message(comm, create_packet);
        auto create_outcome = oc::condrv::dispatch_message(state, create_message, host_io);
        if (!create_outcome || create_message.completion().io_status.Status != oc::core::status_success)
        {
            return false;
        }

        const auto new_output = static_cast<ULONG_PTR>(create_message.completion().io_status.Information);
        if (new_output == 0)
        {
            return false;
        }

        auto fill_packet = [&](const ULONG_PTR output_handle, const wchar_t value) -> bool {
            oc::condrv::IoPacket packet{};
            packet.payload.user_defined = oc::condrv::UserDefinedPacket{};
            packet.descriptor.identifier.LowPart = 40;
            packet.descriptor.function = oc::condrv::console_io_user_defined;
            packet.descriptor.process = info.process;
            packet.descriptor.object = output_handle;
            packet.payload.user_defined.msg_header.ApiNumber = static_cast<ULONG>(ConsolepFillConsoleOutput);
            packet.payload.user_defined.msg_header.ApiDescriptorSize = sizeof(CONSOLE_FILLCONSOLEOUTPUT_MSG);

            auto& body = packet.payload.user_defined.u.console_msg_l2.FillConsoleOutput;
            body.WriteCoord = COORD{ 0, 0 };
            body.ElementType = CONSOLE_REAL_UNICODE;
            body.Element = static_cast<USHORT>(value);
            body.Length = 1;

            oc::condrv::BasicApiMessage<DummyComm> message(comm, packet);
            auto outcome = oc::condrv::dispatch_message(state, message, host_io);
            const bool success = outcome &&
                                 message.completion().io_status.Status == oc::core::status_success &&
                                 message.packet().payload.user_defined.u.console_msg_l2.FillConsoleOutput.Length == 1;
            if (!success)
            {
                fwprintf(stderr, L"[condrv dispatch] FillConsoleOutput failed: handle=%p status=0x%08X len=%lu\n",
                    reinterpret_cast<void*>(output_handle),
                    static_cast<unsigned>(message.completion().io_status.Status),
                    static_cast<unsigned long>(message.packet().payload.user_defined.u.console_msg_l2.FillConsoleOutput.Length));
            }
            return success;
        };

        if (!fill_packet(info.output, L'A'))
        {
            return false;
        }
        if (!fill_packet(new_output, L'B'))
        {
            return false;
        }

        auto read_packet = [&](const ULONG_PTR output_handle) -> std::optional<wchar_t> {
            constexpr ULONG api_size = sizeof(CONSOLE_READCONSOLEOUTPUTSTRING_MSG);
            constexpr ULONG header_size = sizeof(CONSOLE_MSG_HEADER);

            oc::condrv::IoPacket packet{};
            packet.payload.user_defined = oc::condrv::UserDefinedPacket{};
            packet.descriptor.identifier.LowPart = 41;
            packet.descriptor.function = oc::condrv::console_io_user_defined;
            packet.descriptor.process = info.process;
            packet.descriptor.object = output_handle;
            packet.descriptor.input_size = api_size + header_size;
            packet.descriptor.output_size = api_size + sizeof(wchar_t);
            packet.payload.user_defined.msg_header.ApiNumber = static_cast<ULONG>(ConsolepReadConsoleOutputString);
            packet.payload.user_defined.msg_header.ApiDescriptorSize = api_size;

            auto& body = packet.payload.user_defined.u.console_msg_l2.ReadConsoleOutputString;
            body.ReadCoord = COORD{ 0, 0 };
            body.StringType = CONSOLE_REAL_UNICODE;

            oc::condrv::BasicApiMessage<DummyComm> message(comm, packet);
            auto outcome = oc::condrv::dispatch_message(state, message, host_io);
            if (!outcome || message.completion().io_status.Status != oc::core::status_success)
            {
                if (!outcome)
                {
                    fwprintf(stderr, L"[condrv dispatch] ReadConsoleOutputString dispatch error: handle=%p win32=%lu\n",
                        reinterpret_cast<void*>(output_handle),
                        static_cast<unsigned long>(outcome.error().win32_error));
                }
                else
                {
                    fwprintf(stderr, L"[condrv dispatch] ReadConsoleOutputString failed: handle=%p status=0x%08X\n",
                        reinterpret_cast<void*>(output_handle),
                        static_cast<unsigned>(message.completion().io_status.Status));
                }
                return std::nullopt;
            }

            auto output = message.get_output_buffer();
            if (!output || output->size() < sizeof(wchar_t))
            {
                fwprintf(stderr, L"[condrv dispatch] ReadConsoleOutputString missing output buffer: handle=%p\n",
                    reinterpret_cast<void*>(output_handle));
                return std::nullopt;
            }

            wchar_t result{};
            std::memcpy(&result, output->data(), sizeof(result));
            return result;
        };

        const auto first = read_packet(info.output);
        const auto second = read_packet(new_output);
        if (!first || !second)
        {
            return false;
        }

        if (*first != L'A' || *second != L'B')
        {
            return false;
        }

        auto close_packet = make_close_object_packet(new_output);
        oc::condrv::BasicApiMessage<DummyComm> close_message(comm, close_packet);
        auto close_outcome = oc::condrv::dispatch_message(state, close_message, host_io);
        return close_outcome && close_message.completion().io_status.Status == oc::core::status_success;
    }

    bool test_set_active_screen_buffer_affects_current_output_creation()
    {
        DummyComm comm{};
        oc::condrv::ServerState state{};
        oc::condrv::NullHostIo host_io{};

        auto connect_packet = make_connect_packet(17, 23);
        oc::condrv::BasicApiMessage<DummyComm> connect_message(comm, connect_packet);
        auto connect_outcome = oc::condrv::dispatch_message(state, connect_message, host_io);
        if (!connect_outcome || connect_message.completion().io_status.Status != oc::core::status_success)
        {
            return false;
        }

        oc::condrv::ConnectionInformation info{};
        std::memcpy(&info, connect_message.completion().write.data, sizeof(info));

        auto create_packet = make_create_object_packet(
            info.process,
            oc::condrv::io_object_type_new_output,
            GENERIC_WRITE,
            FILE_SHARE_READ | FILE_SHARE_WRITE);
        oc::condrv::BasicApiMessage<DummyComm> create_message(comm, create_packet);
        auto create_outcome = oc::condrv::dispatch_message(state, create_message, host_io);
        if (!create_outcome || create_message.completion().io_status.Status != oc::core::status_success)
        {
            return false;
        }

        const auto new_output = static_cast<ULONG_PTR>(create_message.completion().io_status.Information);
        if (new_output == 0)
        {
            return false;
        }

        oc::condrv::IoPacket set_active_packet{};
        set_active_packet.payload.user_defined = oc::condrv::UserDefinedPacket{};
        set_active_packet.descriptor.identifier.LowPart = 50;
        set_active_packet.descriptor.function = oc::condrv::console_io_user_defined;
        set_active_packet.descriptor.process = info.process;
        set_active_packet.descriptor.object = new_output;
        set_active_packet.payload.user_defined.msg_header.ApiNumber = static_cast<ULONG>(ConsolepSetActiveScreenBuffer);
        set_active_packet.payload.user_defined.msg_header.ApiDescriptorSize = 0;

        oc::condrv::BasicApiMessage<DummyComm> set_active_message(comm, set_active_packet);
        auto set_active_outcome = oc::condrv::dispatch_message(state, set_active_message, host_io);
        if (!set_active_outcome || set_active_message.completion().io_status.Status != oc::core::status_success)
        {
            return false;
        }

        auto current_packet = make_create_object_packet(
            info.process,
            oc::condrv::io_object_type_current_output,
            GENERIC_WRITE,
            FILE_SHARE_READ | FILE_SHARE_WRITE);
        oc::condrv::BasicApiMessage<DummyComm> current_message(comm, current_packet);
        auto current_outcome = oc::condrv::dispatch_message(state, current_message, host_io);
        if (!current_outcome || current_message.completion().io_status.Status != oc::core::status_success)
        {
            return false;
        }

        const auto current_output = static_cast<ULONG_PTR>(current_message.completion().io_status.Information);
        if (current_output == 0)
        {
            return false;
        }

        oc::condrv::IoPacket fill_packet{};
        fill_packet.payload.user_defined = oc::condrv::UserDefinedPacket{};
        fill_packet.descriptor.identifier.LowPart = 51;
        fill_packet.descriptor.function = oc::condrv::console_io_user_defined;
        fill_packet.descriptor.process = info.process;
        fill_packet.descriptor.object = current_output;
        fill_packet.payload.user_defined.msg_header.ApiNumber = static_cast<ULONG>(ConsolepFillConsoleOutput);
        fill_packet.payload.user_defined.msg_header.ApiDescriptorSize = sizeof(CONSOLE_FILLCONSOLEOUTPUT_MSG);

        auto& fill_body = fill_packet.payload.user_defined.u.console_msg_l2.FillConsoleOutput;
        fill_body.WriteCoord = COORD{ 1, 0 };
        fill_body.ElementType = CONSOLE_REAL_UNICODE;
        fill_body.Element = static_cast<USHORT>(L'Z');
        fill_body.Length = 1;

        oc::condrv::BasicApiMessage<DummyComm> fill_message(comm, fill_packet);
        auto fill_outcome = oc::condrv::dispatch_message(state, fill_message, host_io);
        if (!fill_outcome || fill_message.completion().io_status.Status != oc::core::status_success)
        {
            return false;
        }

        auto read_char = [&](const ULONG_PTR output_handle) -> std::optional<wchar_t> {
            constexpr ULONG api_size = sizeof(CONSOLE_READCONSOLEOUTPUTSTRING_MSG);
            constexpr ULONG header_size = sizeof(CONSOLE_MSG_HEADER);

            oc::condrv::IoPacket read_packet{};
            read_packet.payload.user_defined = oc::condrv::UserDefinedPacket{};
            read_packet.descriptor.identifier.LowPart = 52;
            read_packet.descriptor.function = oc::condrv::console_io_user_defined;
            read_packet.descriptor.process = info.process;
            read_packet.descriptor.object = output_handle;
            read_packet.descriptor.input_size = api_size + header_size;
            read_packet.descriptor.output_size = api_size + sizeof(wchar_t);
            read_packet.payload.user_defined.msg_header.ApiNumber = static_cast<ULONG>(ConsolepReadConsoleOutputString);
            read_packet.payload.user_defined.msg_header.ApiDescriptorSize = api_size;

            auto& body = read_packet.payload.user_defined.u.console_msg_l2.ReadConsoleOutputString;
            body.ReadCoord = COORD{ 1, 0 };
            body.StringType = CONSOLE_REAL_UNICODE;

            oc::condrv::BasicApiMessage<DummyComm> message(comm, read_packet);
            auto outcome = oc::condrv::dispatch_message(state, message, host_io);
            if (!outcome || message.completion().io_status.Status != oc::core::status_success)
            {
                return std::nullopt;
            }

            auto output = message.get_output_buffer();
            if (!output || output->size() < sizeof(wchar_t))
            {
                return std::nullopt;
            }

            wchar_t result{};
            std::memcpy(&result, output->data(), sizeof(result));
            return result;
        };

        const auto active_value = read_char(new_output);
        const auto inactive_value = read_char(info.output);
        if (!active_value || !inactive_value)
        {
            return false;
        }

        if (*active_value != L'Z' || *inactive_value == L'Z')
        {
            return false;
        }

        auto close_current_packet = make_close_object_packet(current_output);
        oc::condrv::BasicApiMessage<DummyComm> close_current_message(comm, close_current_packet);
        auto close_current_outcome = oc::condrv::dispatch_message(state, close_current_message, host_io);

        auto close_new_packet = make_close_object_packet(new_output);
        oc::condrv::BasicApiMessage<DummyComm> close_new_message(comm, close_new_packet);
        auto close_new_outcome = oc::condrv::dispatch_message(state, close_new_message, host_io);

        return close_current_outcome &&
               close_current_message.completion().io_status.Status == oc::core::status_success &&
               close_new_outcome &&
               close_new_message.completion().io_status.Status == oc::core::status_success;
    }

    bool test_user_defined_get_set_mode()
    {
        DummyComm comm{};
        oc::condrv::ServerState state{};
        oc::condrv::NullHostIo host_io{};

        auto connect_packet = make_connect_packet(111, 222);
        oc::condrv::BasicApiMessage<DummyComm> connect_message(comm, connect_packet);
        auto connect_outcome = oc::condrv::dispatch_message(state, connect_message, host_io);
        if (!connect_outcome)
        {
            return false;
        }

        oc::condrv::ConnectionInformation info{};
        std::memcpy(&info, connect_message.completion().write.data, sizeof(info));

        auto get_mode_packet = oc::condrv::IoPacket{};
        get_mode_packet.payload.user_defined = oc::condrv::UserDefinedPacket{};
        get_mode_packet.descriptor.identifier.LowPart = 10;
        get_mode_packet.descriptor.function = oc::condrv::console_io_user_defined;
        get_mode_packet.descriptor.process = info.process;
        get_mode_packet.descriptor.object = info.input;
        get_mode_packet.payload.user_defined.msg_header.ApiNumber = static_cast<ULONG>(ConsolepGetMode);
        get_mode_packet.payload.user_defined.msg_header.ApiDescriptorSize = sizeof(CONSOLE_MODE_MSG);

        oc::condrv::BasicApiMessage<DummyComm> get_mode_message(comm, get_mode_packet);
        auto get_mode_outcome = oc::condrv::dispatch_message(state, get_mode_message, host_io);
        if (!get_mode_outcome)
        {
            return false;
        }

        const ULONG expected_default =
            ENABLE_PROCESSED_INPUT | ENABLE_LINE_INPUT | ENABLE_ECHO_INPUT | ENABLE_MOUSE_INPUT | ENABLE_EXTENDED_FLAGS;

        if (get_mode_message.completion().io_status.Status != oc::core::status_success)
        {
            return false;
        }

        if (get_mode_message.packet().payload.user_defined.u.console_msg_l1.GetConsoleMode.Mode != expected_default)
        {
            return false;
        }

        auto set_mode_packet = oc::condrv::IoPacket{};
        set_mode_packet.payload.user_defined = oc::condrv::UserDefinedPacket{};
        set_mode_packet.descriptor.identifier.LowPart = 11;
        set_mode_packet.descriptor.function = oc::condrv::console_io_user_defined;
        set_mode_packet.descriptor.process = info.process;
        set_mode_packet.descriptor.object = info.input;
        set_mode_packet.payload.user_defined.msg_header.ApiNumber = static_cast<ULONG>(ConsolepSetMode);
        set_mode_packet.payload.user_defined.msg_header.ApiDescriptorSize = sizeof(CONSOLE_MODE_MSG);
        set_mode_packet.payload.user_defined.u.console_msg_l1.SetConsoleMode.Mode = 0x1234;

        oc::condrv::BasicApiMessage<DummyComm> set_mode_message(comm, set_mode_packet);
        auto set_mode_outcome = oc::condrv::dispatch_message(state, set_mode_message, host_io);
        if (!set_mode_outcome)
        {
            return false;
        }

        // Input mode applies even if the call returns invalid parameter (conhost compatibility).
        if (set_mode_message.completion().io_status.Status != oc::core::status_invalid_parameter)
        {
            return false;
        }

        // Read it back.
        oc::condrv::BasicApiMessage<DummyComm> get_mode_again(comm, get_mode_packet);
        auto get_mode_again_outcome = oc::condrv::dispatch_message(state, get_mode_again, host_io);
        if (!get_mode_again_outcome)
        {
            return false;
        }

        return get_mode_again.packet().payload.user_defined.u.console_msg_l1.GetConsoleMode.Mode == 0x1234;
    }

    bool test_user_defined_set_output_mode_validates_flags()
    {
        DummyComm comm{};
        oc::condrv::ServerState state{};
        oc::condrv::NullHostIo host_io{};

        auto connect_packet = make_connect_packet(123, 456);
        oc::condrv::BasicApiMessage<DummyComm> connect_message(comm, connect_packet);
        auto connect_outcome = oc::condrv::dispatch_message(state, connect_message, host_io);
        if (!connect_outcome || connect_message.completion().io_status.Status != oc::core::status_success)
        {
            return false;
        }

        oc::condrv::ConnectionInformation info{};
        std::memcpy(&info, connect_message.completion().write.data, sizeof(info));

        auto get_mode_packet = oc::condrv::IoPacket{};
        get_mode_packet.payload.user_defined = oc::condrv::UserDefinedPacket{};
        get_mode_packet.descriptor.identifier.LowPart = 200;
        get_mode_packet.descriptor.function = oc::condrv::console_io_user_defined;
        get_mode_packet.descriptor.process = info.process;
        get_mode_packet.descriptor.object = info.output;
        get_mode_packet.payload.user_defined.msg_header.ApiNumber = static_cast<ULONG>(ConsolepGetMode);
        get_mode_packet.payload.user_defined.msg_header.ApiDescriptorSize = sizeof(CONSOLE_MODE_MSG);

        oc::condrv::BasicApiMessage<DummyComm> get_mode_message(comm, get_mode_packet);
        auto get_mode_outcome = oc::condrv::dispatch_message(state, get_mode_message, host_io);
        if (!get_mode_outcome || get_mode_message.completion().io_status.Status != oc::core::status_success)
        {
            return false;
        }

        const ULONG initial = get_mode_message.packet().payload.user_defined.u.console_msg_l1.GetConsoleMode.Mode;

        auto set_mode_packet = oc::condrv::IoPacket{};
        set_mode_packet.payload.user_defined = oc::condrv::UserDefinedPacket{};
        set_mode_packet.descriptor.identifier.LowPart = 201;
        set_mode_packet.descriptor.function = oc::condrv::console_io_user_defined;
        set_mode_packet.descriptor.process = info.process;
        set_mode_packet.descriptor.object = info.output;
        set_mode_packet.payload.user_defined.msg_header.ApiNumber = static_cast<ULONG>(ConsolepSetMode);
        set_mode_packet.payload.user_defined.msg_header.ApiDescriptorSize = sizeof(CONSOLE_MODE_MSG);

        const ULONG valid =
            ENABLE_PROCESSED_OUTPUT |
            ENABLE_WRAP_AT_EOL_OUTPUT |
            ENABLE_VIRTUAL_TERMINAL_PROCESSING |
            DISABLE_NEWLINE_AUTO_RETURN;
        set_mode_packet.payload.user_defined.u.console_msg_l1.SetConsoleMode.Mode = valid;

        oc::condrv::BasicApiMessage<DummyComm> set_mode_message(comm, set_mode_packet);
        auto set_mode_outcome = oc::condrv::dispatch_message(state, set_mode_message, host_io);
        if (!set_mode_outcome || set_mode_message.completion().io_status.Status != oc::core::status_success)
        {
            return false;
        }

        // Invalid bits should be rejected and must not change the mode.
        set_mode_packet.descriptor.identifier.LowPart = 202;
        set_mode_packet.payload.user_defined.u.console_msg_l1.SetConsoleMode.Mode = valid | 0x8000'0000UL;

        oc::condrv::BasicApiMessage<DummyComm> invalid_message(comm, set_mode_packet);
        auto invalid_outcome = oc::condrv::dispatch_message(state, invalid_message, host_io);
        if (!invalid_outcome || invalid_message.completion().io_status.Status != oc::core::status_invalid_parameter)
        {
            return false;
        }

        oc::condrv::BasicApiMessage<DummyComm> get_after(comm, get_mode_packet);
        auto get_after_outcome = oc::condrv::dispatch_message(state, get_after, host_io);
        if (!get_after_outcome || get_after.completion().io_status.Status != oc::core::status_success)
        {
            return false;
        }

        const ULONG after = get_after.packet().payload.user_defined.u.console_msg_l1.GetConsoleMode.Mode;
        return initial != 0 && after == valid;
    }

    bool test_user_defined_get_cp()
    {
        DummyComm comm{};
        oc::condrv::ServerState state{};
        oc::condrv::NullHostIo host_io{};

        auto connect_packet = make_connect_packet(333, 444);
        oc::condrv::BasicApiMessage<DummyComm> connect_message(comm, connect_packet);
        auto connect_outcome = oc::condrv::dispatch_message(state, connect_message, host_io);
        if (!connect_outcome)
        {
            return false;
        }

        oc::condrv::ConnectionInformation info{};
        std::memcpy(&info, connect_message.completion().write.data, sizeof(info));

        auto get_cp_packet = oc::condrv::IoPacket{};
        get_cp_packet.payload.user_defined = oc::condrv::UserDefinedPacket{};
        get_cp_packet.descriptor.identifier.LowPart = 12;
        get_cp_packet.descriptor.function = oc::condrv::console_io_user_defined;
        get_cp_packet.descriptor.process = info.process;
        get_cp_packet.descriptor.object = info.output;
        get_cp_packet.payload.user_defined.msg_header.ApiNumber = static_cast<ULONG>(ConsolepGetCP);
        get_cp_packet.payload.user_defined.msg_header.ApiDescriptorSize = sizeof(CONSOLE_GETCP_MSG);
        get_cp_packet.payload.user_defined.u.console_msg_l1.GetConsoleCP.Output = FALSE;

        oc::condrv::BasicApiMessage<DummyComm> message(comm, get_cp_packet);
        auto outcome = oc::condrv::dispatch_message(state, message, host_io);
        if (!outcome)
        {
            return false;
        }

        if (message.completion().io_status.Status != oc::core::status_success)
        {
            return false;
        }

        const auto original = message.packet().payload.user_defined.u.console_msg_l1.GetConsoleCP.CodePage;
        if (original == 0)
        {
            return false;
        }

        auto set_cp_packet = oc::condrv::IoPacket{};
        set_cp_packet.payload.user_defined = oc::condrv::UserDefinedPacket{};
        set_cp_packet.descriptor.identifier.LowPart = 13;
        set_cp_packet.descriptor.function = oc::condrv::console_io_user_defined;
        set_cp_packet.descriptor.process = info.process;
        set_cp_packet.descriptor.object = info.output;
        set_cp_packet.payload.user_defined.msg_header.ApiNumber = static_cast<ULONG>(ConsolepSetCP);
        set_cp_packet.payload.user_defined.msg_header.ApiDescriptorSize = sizeof(CONSOLE_SETCP_MSG);
        set_cp_packet.payload.user_defined.u.console_msg_l2.SetConsoleCP.CodePage = 65001;
        set_cp_packet.payload.user_defined.u.console_msg_l2.SetConsoleCP.Output = FALSE;

        oc::condrv::BasicApiMessage<DummyComm> set_message(comm, set_cp_packet);
        auto set_outcome = oc::condrv::dispatch_message(state, set_message, host_io);
        if (!set_outcome)
        {
            return false;
        }

        if (set_message.completion().io_status.Status != oc::core::status_success)
        {
            return false;
        }

        oc::condrv::BasicApiMessage<DummyComm> get_again(comm, get_cp_packet);
        auto get_again_outcome = oc::condrv::dispatch_message(state, get_again, host_io);
        if (!get_again_outcome)
        {
            return false;
        }

        return get_again.completion().io_status.Status == oc::core::status_success &&
               get_again.packet().payload.user_defined.u.console_msg_l1.GetConsoleCP.CodePage == 65001;
    }

    bool test_user_defined_get_console_window_returns_null()
    {
        DummyComm comm{};
        oc::condrv::ServerState state{};
        oc::condrv::NullHostIo host_io{};

        auto connect_packet = make_connect_packet(5001, 5002);
        oc::condrv::BasicApiMessage<DummyComm> connect_message(comm, connect_packet);
        auto connect_outcome = oc::condrv::dispatch_message(state, connect_message, host_io);
        if (!connect_outcome)
        {
            return false;
        }

        oc::condrv::ConnectionInformation info{};
        std::memcpy(&info, connect_message.completion().write.data, sizeof(info));

        auto packet = oc::condrv::IoPacket{};
        packet.payload.user_defined = oc::condrv::UserDefinedPacket{};
        packet.descriptor.identifier.LowPart = 90;
        packet.descriptor.function = oc::condrv::console_io_user_defined;
        packet.descriptor.process = info.process;
        packet.descriptor.object = info.output;
        packet.payload.user_defined.msg_header.ApiNumber = static_cast<ULONG>(ConsolepGetConsoleWindow);
        packet.payload.user_defined.msg_header.ApiDescriptorSize = sizeof(decltype(packet.payload.user_defined.u.console_msg_l3.GetConsoleWindow));

        oc::condrv::BasicApiMessage<DummyComm> message(comm, packet);
        auto outcome = oc::condrv::dispatch_message(state, message, host_io);
        if (!outcome)
        {
            return false;
        }

        if (message.completion().io_status.Status != oc::core::status_success)
        {
            return false;
        }

        const auto hwnd = message.packet().payload.user_defined.u.console_msg_l3.GetConsoleWindow.hwnd;
        return hwnd == decltype(hwnd){};
    }

    bool test_user_defined_get_display_mode_returns_zero()
    {
        DummyComm comm{};
        oc::condrv::ServerState state{};
        oc::condrv::NullHostIo host_io{};

        auto connect_packet = make_connect_packet(5003, 5004);
        oc::condrv::BasicApiMessage<DummyComm> connect_message(comm, connect_packet);
        auto connect_outcome = oc::condrv::dispatch_message(state, connect_message, host_io);
        if (!connect_outcome)
        {
            return false;
        }

        oc::condrv::ConnectionInformation info{};
        std::memcpy(&info, connect_message.completion().write.data, sizeof(info));

        auto packet = oc::condrv::IoPacket{};
        packet.payload.user_defined = oc::condrv::UserDefinedPacket{};
        packet.descriptor.identifier.LowPart = 95;
        packet.descriptor.function = oc::condrv::console_io_user_defined;
        packet.descriptor.process = info.process;
        packet.descriptor.object = info.output;
        packet.payload.user_defined.msg_header.ApiNumber = static_cast<ULONG>(ConsolepGetDisplayMode);
        packet.payload.user_defined.msg_header.ApiDescriptorSize = sizeof(CONSOLE_GETDISPLAYMODE_MSG);

        oc::condrv::BasicApiMessage<DummyComm> message(comm, packet);
        auto outcome = oc::condrv::dispatch_message(state, message, host_io);
        if (!outcome)
        {
            return false;
        }

        if (message.completion().io_status.Status != oc::core::status_success)
        {
            return false;
        }

        return message.packet().payload.user_defined.u.console_msg_l3.GetConsoleDisplayMode.ModeFlags == 0;
    }

    [[nodiscard]] bool wchar_buffer_has_nul_terminator(const wchar_t* const buffer, const size_t length) noexcept
    {
        if (buffer == nullptr)
        {
            return false;
        }

        for (size_t i = 0; i < length; ++i)
        {
            if (buffer[i] == L'\0')
            {
                return true;
            }
        }
        return false;
    }

    [[nodiscard]] bool wchar_buffer_starts_with(const wchar_t* const buffer, const size_t length, const wchar_t* const prefix) noexcept
    {
        if (buffer == nullptr || prefix == nullptr)
        {
            return false;
        }

        size_t idx = 0;
        while (prefix[idx] != L'\0')
        {
            if (idx >= length)
            {
                return false;
            }
            if (buffer[idx] != prefix[idx])
            {
                return false;
            }
            ++idx;
        }
        return true;
    }

    bool test_user_defined_font_apis_round_trip()
    {
        DummyComm comm{};
        oc::condrv::ServerState state{};
        oc::condrv::NullHostIo host_io{};

        auto connect_packet = make_connect_packet(5007, 5008);
        oc::condrv::BasicApiMessage<DummyComm> connect_message(comm, connect_packet);
        auto connect_outcome = oc::condrv::dispatch_message(state, connect_message, host_io);
        if (!connect_outcome || connect_message.completion().io_status.Status != oc::core::status_success)
        {
            return false;
        }

        oc::condrv::ConnectionInformation info{};
        std::memcpy(&info, connect_message.completion().write.data, sizeof(info));

        // Get number of fonts.
        {
            auto packet = oc::condrv::IoPacket{};
            packet.payload.user_defined = oc::condrv::UserDefinedPacket{};
            packet.descriptor.identifier.LowPart = 96;
            packet.descriptor.function = oc::condrv::console_io_user_defined;
            packet.descriptor.process = info.process;
            packet.descriptor.object = info.output;
            packet.payload.user_defined.msg_header.ApiNumber = static_cast<ULONG>(ConsolepGetNumberOfFonts);
            packet.payload.user_defined.msg_header.ApiDescriptorSize = sizeof(CONSOLE_GETNUMBEROFFONTS_MSG);

            oc::condrv::BasicApiMessage<DummyComm> message(comm, packet);
            auto outcome = oc::condrv::dispatch_message(state, message, host_io);
            if (!outcome || message.completion().io_status.Status != oc::core::status_success)
            {
                return false;
            }

            if (message.packet().payload.user_defined.u.console_msg_l3.GetNumberOfConsoleFonts.NumberOfFonts != 1)
            {
                return false;
            }
        }

        // Get font info.
        {
            constexpr ULONG api_size = sizeof(CONSOLE_GETFONTINFO_MSG);
            auto packet = oc::condrv::IoPacket{};
            packet.payload.user_defined = oc::condrv::UserDefinedPacket{};
            packet.descriptor.identifier.LowPart = 97;
            packet.descriptor.function = oc::condrv::console_io_user_defined;
            packet.descriptor.process = info.process;
            packet.descriptor.object = info.output;
            packet.descriptor.output_size = api_size + sizeof(CONSOLE_FONT_INFO);
            packet.payload.user_defined.msg_header.ApiNumber = static_cast<ULONG>(ConsolepGetFontInfo);
            packet.payload.user_defined.msg_header.ApiDescriptorSize = api_size;

            oc::condrv::BasicApiMessage<DummyComm> message(comm, packet);
            auto outcome = oc::condrv::dispatch_message(state, message, host_io);
            if (!outcome || message.completion().io_status.Status != oc::core::status_success)
            {
                return false;
            }

            if (message.packet().payload.user_defined.u.console_msg_l3.GetConsoleFontInfo.NumFonts != 1)
            {
                return false;
            }

            if (message.completion().io_status.Information != sizeof(CONSOLE_FONT_INFO))
            {
                return false;
            }

            auto out = message.get_output_buffer();
            if (!out || out->size() < sizeof(CONSOLE_FONT_INFO))
            {
                return false;
            }

            CONSOLE_FONT_INFO font_info{};
            std::memcpy(&font_info, out->data(), sizeof(font_info));
            if (font_info.nFont != 0 || font_info.dwFontSize.X <= 0 || font_info.dwFontSize.Y <= 0)
            {
                return false;
            }
        }

        // Get current font.
        {
            auto packet = oc::condrv::IoPacket{};
            packet.payload.user_defined = oc::condrv::UserDefinedPacket{};
            packet.descriptor.identifier.LowPart = 98;
            packet.descriptor.function = oc::condrv::console_io_user_defined;
            packet.descriptor.process = info.process;
            packet.descriptor.object = info.output;
            packet.payload.user_defined.msg_header.ApiNumber = static_cast<ULONG>(ConsolepGetCurrentFont);
            packet.payload.user_defined.msg_header.ApiDescriptorSize = sizeof(CONSOLE_CURRENTFONT_MSG);

            oc::condrv::BasicApiMessage<DummyComm> message(comm, packet);
            auto outcome = oc::condrv::dispatch_message(state, message, host_io);
            if (!outcome || message.completion().io_status.Status != oc::core::status_success)
            {
                return false;
            }

            const auto& body = message.packet().payload.user_defined.u.console_msg_l3.GetCurrentConsoleFont;
            if (body.FontIndex != 0 || body.FontSize.X != 8 || body.FontSize.Y != 16)
            {
                return false;
            }
            if (!wchar_buffer_has_nul_terminator(body.FaceName, LF_FACESIZE))
            {
                return false;
            }
            if (!wchar_buffer_starts_with(body.FaceName, LF_FACESIZE, L"Consolas"))
            {
                return false;
            }
        }

        // Set current font and observe get-current-font changes.
        {
            auto packet = oc::condrv::IoPacket{};
            packet.payload.user_defined = oc::condrv::UserDefinedPacket{};
            packet.descriptor.identifier.LowPart = 99;
            packet.descriptor.function = oc::condrv::console_io_user_defined;
            packet.descriptor.process = info.process;
            packet.descriptor.object = info.output;
            packet.payload.user_defined.msg_header.ApiNumber = static_cast<ULONG>(ConsolepSetCurrentFont);
            packet.payload.user_defined.msg_header.ApiDescriptorSize = sizeof(CONSOLE_CURRENTFONT_MSG);

            auto& body = packet.payload.user_defined.u.console_msg_l3.SetCurrentConsoleFont;
            body.MaximumWindow = FALSE;
            body.FontIndex = 0;
            body.FontSize = COORD{ 9, 18 };
            body.FontFamily = FF_MODERN;
            body.FontWeight = FW_BOLD;
            std::memset(body.FaceName, 0, sizeof(body.FaceName));
            static constexpr wchar_t face_name[] = L"TestFace";
            static_assert((sizeof(face_name) / sizeof(face_name[0])) <= LF_FACESIZE);
            std::copy_n(face_name, sizeof(face_name) / sizeof(face_name[0]), &body.FaceName[0]);

            oc::condrv::BasicApiMessage<DummyComm> message(comm, packet);
            auto outcome = oc::condrv::dispatch_message(state, message, host_io);
            if (!outcome || message.completion().io_status.Status != oc::core::status_success)
            {
                return false;
            }

            auto get_packet = oc::condrv::IoPacket{};
            get_packet.payload.user_defined = oc::condrv::UserDefinedPacket{};
            get_packet.descriptor.identifier.LowPart = 100;
            get_packet.descriptor.function = oc::condrv::console_io_user_defined;
            get_packet.descriptor.process = info.process;
            get_packet.descriptor.object = info.output;
            get_packet.payload.user_defined.msg_header.ApiNumber = static_cast<ULONG>(ConsolepGetCurrentFont);
            get_packet.payload.user_defined.msg_header.ApiDescriptorSize = sizeof(CONSOLE_CURRENTFONT_MSG);

            oc::condrv::BasicApiMessage<DummyComm> get_message(comm, get_packet);
            auto get_outcome = oc::condrv::dispatch_message(state, get_message, host_io);
            if (!get_outcome || get_message.completion().io_status.Status != oc::core::status_success)
            {
                return false;
            }

            const auto& after = get_message.packet().payload.user_defined.u.console_msg_l3.GetCurrentConsoleFont;
            if (after.FontIndex != 0 || after.FontSize.X != 9 || after.FontSize.Y != 18 || after.FontWeight != FW_BOLD)
            {
                return false;
            }
            if (!wchar_buffer_has_nul_terminator(after.FaceName, LF_FACESIZE))
            {
                return false;
            }
            if (!wchar_buffer_starts_with(after.FaceName, LF_FACESIZE, L"TestFace"))
            {
                return false;
            }
        }

        // Set display mode: should report the current buffer dimensions without failing.
        {
            auto packet = oc::condrv::IoPacket{};
            packet.payload.user_defined = oc::condrv::UserDefinedPacket{};
            packet.descriptor.identifier.LowPart = 101;
            packet.descriptor.function = oc::condrv::console_io_user_defined;
            packet.descriptor.process = info.process;
            packet.descriptor.object = info.output;
            packet.payload.user_defined.msg_header.ApiNumber = static_cast<ULONG>(ConsolepSetDisplayMode);
            packet.payload.user_defined.msg_header.ApiDescriptorSize = sizeof(CONSOLE_SETDISPLAYMODE_MSG);

            oc::condrv::BasicApiMessage<DummyComm> message(comm, packet);
            auto outcome = oc::condrv::dispatch_message(state, message, host_io);
            if (!outcome || message.completion().io_status.Status != oc::core::status_success)
            {
                return false;
            }

            const auto active = state.active_screen_buffer();
            if (!active)
            {
                return false;
            }
            const auto expected = active->screen_buffer_size();
            const auto got = message.packet().payload.user_defined.u.console_msg_l3.SetConsoleDisplayMode.ScreenBufferDimensions;
            return got.X == expected.X && got.Y == expected.Y;
        }
    }

    bool test_user_defined_set_window_info_relative_resizes_window()
    {
        DummyComm comm{};
        oc::condrv::ServerState state{};
        oc::condrv::NullHostIo host_io{};

        auto connect_packet = make_connect_packet(5009, 5010);
        oc::condrv::BasicApiMessage<DummyComm> connect_message(comm, connect_packet);
        auto connect_outcome = oc::condrv::dispatch_message(state, connect_message, host_io);
        if (!connect_outcome || connect_message.completion().io_status.Status != oc::core::status_success)
        {
            return false;
        }

        oc::condrv::ConnectionInformation info{};
        std::memcpy(&info, connect_message.completion().write.data, sizeof(info));

        auto get_packet = oc::condrv::IoPacket{};
        get_packet.payload.user_defined = oc::condrv::UserDefinedPacket{};
        get_packet.descriptor.identifier.LowPart = 102;
        get_packet.descriptor.function = oc::condrv::console_io_user_defined;
        get_packet.descriptor.process = info.process;
        get_packet.descriptor.object = info.output;
        get_packet.payload.user_defined.msg_header.ApiNumber = static_cast<ULONG>(ConsolepGetScreenBufferInfo);
        get_packet.payload.user_defined.msg_header.ApiDescriptorSize = sizeof(CONSOLE_SCREENBUFFERINFO_MSG);

        oc::condrv::BasicApiMessage<DummyComm> get_message(comm, get_packet);
        auto get_outcome = oc::condrv::dispatch_message(state, get_message, host_io);
        if (!get_outcome || get_message.completion().io_status.Status != oc::core::status_success)
        {
            return false;
        }

        const auto initial = get_message.packet().payload.user_defined.u.console_msg_l2.GetConsoleScreenBufferInfo.CurrentWindowSize;
        if (initial.X <= 1 || initial.Y <= 1)
        {
            return false;
        }

        auto set_packet = oc::condrv::IoPacket{};
        set_packet.payload.user_defined = oc::condrv::UserDefinedPacket{};
        set_packet.descriptor.identifier.LowPart = 103;
        set_packet.descriptor.function = oc::condrv::console_io_user_defined;
        set_packet.descriptor.process = info.process;
        set_packet.descriptor.object = info.output;
        set_packet.payload.user_defined.msg_header.ApiNumber = static_cast<ULONG>(ConsolepSetWindowInfo);
        set_packet.payload.user_defined.msg_header.ApiDescriptorSize = sizeof(CONSOLE_SETWINDOWINFO_MSG);

        auto& set_body = set_packet.payload.user_defined.u.console_msg_l2.SetConsoleWindowInfo;
        set_body.Absolute = FALSE;
        set_body.Window.Left = 0;
        set_body.Window.Top = 0;
        set_body.Window.Right = -1;
        set_body.Window.Bottom = -1;

        oc::condrv::BasicApiMessage<DummyComm> set_message(comm, set_packet);
        auto set_outcome = oc::condrv::dispatch_message(state, set_message, host_io);
        if (!set_outcome || set_message.completion().io_status.Status != oc::core::status_success)
        {
            return false;
        }

        auto get_packet_again = oc::condrv::IoPacket{};
        get_packet_again.payload.user_defined = oc::condrv::UserDefinedPacket{};
        get_packet_again.descriptor.identifier.LowPart = 104;
        get_packet_again.descriptor.function = oc::condrv::console_io_user_defined;
        get_packet_again.descriptor.process = info.process;
        get_packet_again.descriptor.object = info.output;
        get_packet_again.payload.user_defined.msg_header.ApiNumber = static_cast<ULONG>(ConsolepGetScreenBufferInfo);
        get_packet_again.payload.user_defined.msg_header.ApiDescriptorSize = sizeof(CONSOLE_SCREENBUFFERINFO_MSG);

        oc::condrv::BasicApiMessage<DummyComm> get_message_again(comm, get_packet_again);
        auto get_again_outcome = oc::condrv::dispatch_message(state, get_message_again, host_io);
        if (!get_again_outcome || get_message_again.completion().io_status.Status != oc::core::status_success)
        {
            return false;
        }

        const auto after = get_message_again.packet().payload.user_defined.u.console_msg_l2.GetConsoleScreenBufferInfo.CurrentWindowSize;
        return after.X == initial.X - 1 && after.Y == initial.Y - 1;
    }

    bool test_user_defined_window_info_updates_scroll_position()
    {
        DummyComm comm{};
        oc::condrv::ServerState state{};
        oc::condrv::NullHostIo host_io{};

        auto connect_packet = make_connect_packet(5021, 5022);
        oc::condrv::BasicApiMessage<DummyComm> connect_message(comm, connect_packet);
        auto connect_outcome = oc::condrv::dispatch_message(state, connect_message, host_io);
        if (!connect_outcome || connect_message.completion().io_status.Status != oc::core::status_success)
        {
            return false;
        }

        oc::condrv::ConnectionInformation info{};
        std::memcpy(&info, connect_message.completion().write.data, sizeof(info));

        // Set an absolute window rectangle with a non-zero origin.
        oc::condrv::IoPacket set_packet{};
        set_packet.payload.user_defined = oc::condrv::UserDefinedPacket{};
        set_packet.descriptor.identifier.LowPart = 111;
        set_packet.descriptor.function = oc::condrv::console_io_user_defined;
        set_packet.descriptor.process = info.process;
        set_packet.descriptor.object = info.output;
        set_packet.payload.user_defined.msg_header.ApiNumber = static_cast<ULONG>(ConsolepSetWindowInfo);
        set_packet.payload.user_defined.msg_header.ApiDescriptorSize = sizeof(CONSOLE_SETWINDOWINFO_MSG);

        auto& set_body = set_packet.payload.user_defined.u.console_msg_l2.SetConsoleWindowInfo;
        set_body.Absolute = TRUE;
        set_body.Window.Left = 5;
        set_body.Window.Top = 6;
        set_body.Window.Right = 84;  // width 80 -> delta 79
        set_body.Window.Bottom = 30; // height 25 -> delta 24

        oc::condrv::BasicApiMessage<DummyComm> set_message(comm, set_packet);
        auto set_outcome = oc::condrv::dispatch_message(state, set_message, host_io);
        if (!set_outcome || set_message.completion().io_status.Status != oc::core::status_success)
        {
            return false;
        }

        // Query info and verify that scroll position and window delta match.
        oc::condrv::IoPacket get_packet{};
        get_packet.payload.user_defined = oc::condrv::UserDefinedPacket{};
        get_packet.descriptor.identifier.LowPart = 112;
        get_packet.descriptor.function = oc::condrv::console_io_user_defined;
        get_packet.descriptor.process = info.process;
        get_packet.descriptor.object = info.output;
        get_packet.payload.user_defined.msg_header.ApiNumber = static_cast<ULONG>(ConsolepGetScreenBufferInfo);
        get_packet.payload.user_defined.msg_header.ApiDescriptorSize = sizeof(CONSOLE_SCREENBUFFERINFO_MSG);

        oc::condrv::BasicApiMessage<DummyComm> get_message(comm, get_packet);
        auto get_outcome = oc::condrv::dispatch_message(state, get_message, host_io);
        if (!get_outcome || get_message.completion().io_status.Status != oc::core::status_success)
        {
            return false;
        }

        const auto& info_msg = get_message.packet().payload.user_defined.u.console_msg_l2.GetConsoleScreenBufferInfo;
        if (info_msg.ScrollPosition.X != 5 || info_msg.ScrollPosition.Y != 6)
        {
            return false;
        }
        if (info_msg.CurrentWindowSize.X != 79 || info_msg.CurrentWindowSize.Y != 24)
        {
            return false;
        }

        return true;
    }

    bool test_user_defined_cursor_position_snaps_viewport()
    {
        DummyComm comm{};
        oc::condrv::ServerState state{};
        oc::condrv::NullHostIo host_io{};

        auto connect_packet = make_connect_packet(5023, 5024);
        oc::condrv::BasicApiMessage<DummyComm> connect_message(comm, connect_packet);
        auto connect_outcome = oc::condrv::dispatch_message(state, connect_message, host_io);
        if (!connect_outcome || connect_message.completion().io_status.Status != oc::core::status_success)
        {
            return false;
        }

        oc::condrv::ConnectionInformation info{};
        std::memcpy(&info, connect_message.completion().write.data, sizeof(info));

        // Create a 10x10 window at the top-left so that moving the cursor down forces a scroll.
        oc::condrv::IoPacket set_window_packet{};
        set_window_packet.payload.user_defined = oc::condrv::UserDefinedPacket{};
        set_window_packet.descriptor.identifier.LowPart = 113;
        set_window_packet.descriptor.function = oc::condrv::console_io_user_defined;
        set_window_packet.descriptor.process = info.process;
        set_window_packet.descriptor.object = info.output;
        set_window_packet.payload.user_defined.msg_header.ApiNumber = static_cast<ULONG>(ConsolepSetWindowInfo);
        set_window_packet.payload.user_defined.msg_header.ApiDescriptorSize = sizeof(CONSOLE_SETWINDOWINFO_MSG);

        auto& window_body = set_window_packet.payload.user_defined.u.console_msg_l2.SetConsoleWindowInfo;
        window_body.Absolute = TRUE;
        window_body.Window.Left = 0;
        window_body.Window.Top = 0;
        window_body.Window.Right = 9;
        window_body.Window.Bottom = 9;

        oc::condrv::BasicApiMessage<DummyComm> set_window_message(comm, set_window_packet);
        auto set_window_outcome = oc::condrv::dispatch_message(state, set_window_message, host_io);
        if (!set_window_outcome || set_window_message.completion().io_status.Status != oc::core::status_success)
        {
            return false;
        }

        // Move the cursor to a row outside the 0..9 window, which should snap the viewport.
        oc::condrv::IoPacket set_cursor_packet{};
        set_cursor_packet.payload.user_defined = oc::condrv::UserDefinedPacket{};
        set_cursor_packet.descriptor.identifier.LowPart = 114;
        set_cursor_packet.descriptor.function = oc::condrv::console_io_user_defined;
        set_cursor_packet.descriptor.process = info.process;
        set_cursor_packet.descriptor.object = info.output;
        set_cursor_packet.payload.user_defined.msg_header.ApiNumber = static_cast<ULONG>(ConsolepSetCursorPosition);
        set_cursor_packet.payload.user_defined.msg_header.ApiDescriptorSize = sizeof(CONSOLE_SETCURSORPOSITION_MSG);
        set_cursor_packet.payload.user_defined.u.console_msg_l2.SetConsoleCursorPosition.CursorPosition.X = 0;
        set_cursor_packet.payload.user_defined.u.console_msg_l2.SetConsoleCursorPosition.CursorPosition.Y = 15;

        oc::condrv::BasicApiMessage<DummyComm> set_cursor_message(comm, set_cursor_packet);
        auto set_cursor_outcome = oc::condrv::dispatch_message(state, set_cursor_message, host_io);
        if (!set_cursor_outcome || set_cursor_message.completion().io_status.Status != oc::core::status_success)
        {
            return false;
        }

        // Verify that the scroll position moved so the cursor is visible.
        oc::condrv::IoPacket get_packet{};
        get_packet.payload.user_defined = oc::condrv::UserDefinedPacket{};
        get_packet.descriptor.identifier.LowPart = 115;
        get_packet.descriptor.function = oc::condrv::console_io_user_defined;
        get_packet.descriptor.process = info.process;
        get_packet.descriptor.object = info.output;
        get_packet.payload.user_defined.msg_header.ApiNumber = static_cast<ULONG>(ConsolepGetScreenBufferInfo);
        get_packet.payload.user_defined.msg_header.ApiDescriptorSize = sizeof(CONSOLE_SCREENBUFFERINFO_MSG);

        oc::condrv::BasicApiMessage<DummyComm> get_message(comm, get_packet);
        auto get_outcome = oc::condrv::dispatch_message(state, get_message, host_io);
        if (!get_outcome || get_message.completion().io_status.Status != oc::core::status_success)
        {
            return false;
        }

        const auto& info_msg = get_message.packet().payload.user_defined.u.console_msg_l2.GetConsoleScreenBufferInfo;
        if (info_msg.ScrollPosition.X != 0 || info_msg.ScrollPosition.Y != 6)
        {
            return false;
        }
        if (info_msg.CurrentWindowSize.X != 9 || info_msg.CurrentWindowSize.Y != 9)
        {
            return false;
        }

        return true;
    }

    bool test_user_defined_cursor_mode_round_trips()
    {
        DummyComm comm{};
        oc::condrv::ServerState state{};
        oc::condrv::NullHostIo host_io{};

        auto connect_packet = make_connect_packet(5011, 5012);
        oc::condrv::BasicApiMessage<DummyComm> connect_message(comm, connect_packet);
        auto connect_outcome = oc::condrv::dispatch_message(state, connect_message, host_io);
        if (!connect_outcome || connect_message.completion().io_status.Status != oc::core::status_success)
        {
            return false;
        }

        oc::condrv::ConnectionInformation info{};
        std::memcpy(&info, connect_message.completion().write.data, sizeof(info));

        // Set.
        oc::condrv::IoPacket set_packet{};
        set_packet.payload.user_defined = oc::condrv::UserDefinedPacket{};
        set_packet.descriptor.identifier.LowPart = 105;
        set_packet.descriptor.function = oc::condrv::console_io_user_defined;
        set_packet.descriptor.process = info.process;
        set_packet.descriptor.object = info.output;
        set_packet.payload.user_defined.msg_header.ApiNumber = static_cast<ULONG>(ConsolepSetCursorMode);
        set_packet.payload.user_defined.msg_header.ApiDescriptorSize = sizeof(CONSOLE_CURSOR_MODE_MSG);

        auto& set_body = set_packet.payload.user_defined.u.console_msg_l3.SetConsoleCursorMode;
        set_body.Blink = FALSE;
        set_body.DBEnable = TRUE;

        oc::condrv::BasicApiMessage<DummyComm> set_message(comm, set_packet);
        auto set_outcome = oc::condrv::dispatch_message(state, set_message, host_io);
        if (!set_outcome || set_message.completion().io_status.Status != oc::core::status_success)
        {
            return false;
        }

        // Get.
        oc::condrv::IoPacket get_packet{};
        get_packet.payload.user_defined = oc::condrv::UserDefinedPacket{};
        get_packet.descriptor.identifier.LowPart = 106;
        get_packet.descriptor.function = oc::condrv::console_io_user_defined;
        get_packet.descriptor.process = info.process;
        get_packet.descriptor.object = info.output;
        get_packet.payload.user_defined.msg_header.ApiNumber = static_cast<ULONG>(ConsolepGetCursorMode);
        get_packet.payload.user_defined.msg_header.ApiDescriptorSize = sizeof(CONSOLE_CURSOR_MODE_MSG);

        oc::condrv::BasicApiMessage<DummyComm> get_message(comm, get_packet);
        auto get_outcome = oc::condrv::dispatch_message(state, get_message, host_io);
        if (!get_outcome || get_message.completion().io_status.Status != oc::core::status_success)
        {
            return false;
        }

        const auto& get_body = get_message.packet().payload.user_defined.u.console_msg_l3.GetConsoleCursorMode;
        return get_body.Blink == FALSE && get_body.DBEnable == TRUE;
    }

    bool test_user_defined_nls_mode_round_trips()
    {
        DummyComm comm{};
        oc::condrv::ServerState state{};
        oc::condrv::NullHostIo host_io{};

        auto connect_packet = make_connect_packet(5013, 5014);
        oc::condrv::BasicApiMessage<DummyComm> connect_message(comm, connect_packet);
        auto connect_outcome = oc::condrv::dispatch_message(state, connect_message, host_io);
        if (!connect_outcome || connect_message.completion().io_status.Status != oc::core::status_success)
        {
            return false;
        }

        oc::condrv::ConnectionInformation info{};
        std::memcpy(&info, connect_message.completion().write.data, sizeof(info));

        // Set.
        oc::condrv::IoPacket set_packet{};
        set_packet.payload.user_defined = oc::condrv::UserDefinedPacket{};
        set_packet.descriptor.identifier.LowPart = 107;
        set_packet.descriptor.function = oc::condrv::console_io_user_defined;
        set_packet.descriptor.process = info.process;
        set_packet.descriptor.object = info.output;
        set_packet.payload.user_defined.msg_header.ApiNumber = static_cast<ULONG>(ConsolepSetNlsMode);
        set_packet.payload.user_defined.msg_header.ApiDescriptorSize = sizeof(CONSOLE_NLS_MODE_MSG);

        auto& set_body = set_packet.payload.user_defined.u.console_msg_l3.SetConsoleNlsMode;
        set_body.Ready = FALSE;
        set_body.NlsMode = 42;

        oc::condrv::BasicApiMessage<DummyComm> set_message(comm, set_packet);
        auto set_outcome = oc::condrv::dispatch_message(state, set_message, host_io);
        if (!set_outcome || set_message.completion().io_status.Status != oc::core::status_success)
        {
            return false;
        }

        // Get.
        oc::condrv::IoPacket get_packet{};
        get_packet.payload.user_defined = oc::condrv::UserDefinedPacket{};
        get_packet.descriptor.identifier.LowPart = 108;
        get_packet.descriptor.function = oc::condrv::console_io_user_defined;
        get_packet.descriptor.process = info.process;
        get_packet.descriptor.object = info.output;
        get_packet.payload.user_defined.msg_header.ApiNumber = static_cast<ULONG>(ConsolepGetNlsMode);
        get_packet.payload.user_defined.msg_header.ApiDescriptorSize = sizeof(CONSOLE_NLS_MODE_MSG);

        oc::condrv::BasicApiMessage<DummyComm> get_message(comm, get_packet);
        auto get_outcome = oc::condrv::dispatch_message(state, get_message, host_io);
        if (!get_outcome || get_message.completion().io_status.Status != oc::core::status_success)
        {
            return false;
        }

        const auto& get_body = get_message.packet().payload.user_defined.u.console_msg_l3.GetConsoleNlsMode;
        return get_body.Ready == TRUE && get_body.NlsMode == 42;
    }

    bool test_user_defined_char_type_returns_sbcs_and_validates_coords()
    {
        DummyComm comm{};
        oc::condrv::ServerState state{};
        oc::condrv::NullHostIo host_io{};

        auto connect_packet = make_connect_packet(5015, 5016);
        oc::condrv::BasicApiMessage<DummyComm> connect_message(comm, connect_packet);
        auto connect_outcome = oc::condrv::dispatch_message(state, connect_message, host_io);
        if (!connect_outcome || connect_message.completion().io_status.Status != oc::core::status_success)
        {
            return false;
        }

        oc::condrv::ConnectionInformation info{};
        std::memcpy(&info, connect_message.completion().write.data, sizeof(info));

        // In-range.
        oc::condrv::IoPacket packet{};
        packet.payload.user_defined = oc::condrv::UserDefinedPacket{};
        packet.descriptor.identifier.LowPart = 109;
        packet.descriptor.function = oc::condrv::console_io_user_defined;
        packet.descriptor.process = info.process;
        packet.descriptor.object = info.output;
        packet.payload.user_defined.msg_header.ApiNumber = static_cast<ULONG>(ConsolepCharType);
        packet.payload.user_defined.msg_header.ApiDescriptorSize = sizeof(CONSOLE_CHAR_TYPE_MSG);

        auto& body = packet.payload.user_defined.u.console_msg_l3.GetConsoleCharType;
        body.coordCheck = COORD{ 0, 0 };

        oc::condrv::BasicApiMessage<DummyComm> message(comm, packet);
        auto outcome = oc::condrv::dispatch_message(state, message, host_io);
        if (!outcome || message.completion().io_status.Status != oc::core::status_success)
        {
            return false;
        }

        if (message.packet().payload.user_defined.u.console_msg_l3.GetConsoleCharType.dwType != CHAR_TYPE_SBCS)
        {
            return false;
        }

        // Out-of-range.
        oc::condrv::IoPacket bad_packet{};
        bad_packet.payload.user_defined = oc::condrv::UserDefinedPacket{};
        bad_packet.descriptor.identifier.LowPart = 110;
        bad_packet.descriptor.function = oc::condrv::console_io_user_defined;
        bad_packet.descriptor.process = info.process;
        bad_packet.descriptor.object = info.output;
        bad_packet.payload.user_defined.msg_header.ApiNumber = static_cast<ULONG>(ConsolepCharType);
        bad_packet.payload.user_defined.msg_header.ApiDescriptorSize = sizeof(CONSOLE_CHAR_TYPE_MSG);
        bad_packet.payload.user_defined.u.console_msg_l3.GetConsoleCharType.coordCheck = COORD{ 30000, 30000 };

        oc::condrv::BasicApiMessage<DummyComm> bad_message(comm, bad_packet);
        auto bad_outcome = oc::condrv::dispatch_message(state, bad_message, host_io);
        return bad_outcome && bad_message.completion().io_status.Status == oc::core::status_invalid_parameter;
    }

    bool test_user_defined_compat_misc_stubs_succeed()
    {
        DummyComm comm{};
        oc::condrv::ServerState state{};
        oc::condrv::NullHostIo host_io{};

        auto connect_packet = make_connect_packet(5017, 5018);
        oc::condrv::BasicApiMessage<DummyComm> connect_message(comm, connect_packet);
        auto connect_outcome = oc::condrv::dispatch_message(state, connect_message, host_io);
        if (!connect_outcome || connect_message.completion().io_status.Status != oc::core::status_success)
        {
            return false;
        }

        oc::condrv::ConnectionInformation info{};
        std::memcpy(&info, connect_message.completion().write.data, sizeof(info));

        auto send = [&](ULONG api_number, ULONG api_size, auto&& fill_body) noexcept -> bool {
            oc::condrv::IoPacket packet{};
            packet.payload.user_defined = oc::condrv::UserDefinedPacket{};
            packet.descriptor.identifier.LowPart = 111;
            packet.descriptor.function = oc::condrv::console_io_user_defined;
            packet.descriptor.process = info.process;
            packet.descriptor.object = info.output;
            packet.payload.user_defined.msg_header.ApiNumber = api_number;
            packet.payload.user_defined.msg_header.ApiDescriptorSize = api_size;

            fill_body(packet.payload.user_defined.u.console_msg_l3);

            oc::condrv::BasicApiMessage<DummyComm> message(comm, packet);
            auto outcome = oc::condrv::dispatch_message(state, message, host_io);
            return outcome && message.completion().io_status.Status == oc::core::status_success;
        };

        if (!send(static_cast<ULONG>(ConsolepSetKeyShortcuts), sizeof(CONSOLE_SETKEYSHORTCUTS_MSG), [](auto& body) noexcept {
                body.SetConsoleKeyShortcuts.Set = TRUE;
                body.SetConsoleKeyShortcuts.ReserveKeys = 0;
            }))
        {
            return false;
        }

        if (!send(static_cast<ULONG>(ConsolepSetMenuClose), sizeof(CONSOLE_SETMENUCLOSE_MSG), [](auto& body) noexcept {
                body.SetConsoleMenuClose.Enable = TRUE;
            }))
        {
            return false;
        }

        if (!send(static_cast<ULONG>(ConsolepSetLocalEUDC), sizeof(CONSOLE_LOCAL_EUDC_MSG), [](auto& body) noexcept {
                body.SetConsoleLocalEUDC.CodePoint = 0;
                body.SetConsoleLocalEUDC.FontSize = COORD{ 8, 16 };
            }))
        {
            return false;
        }

        if (!send(static_cast<ULONG>(ConsolepRegisterOS2), sizeof(CONSOLE_REGISTEROS2_MSG), [](auto& body) noexcept {
                body.RegisterConsoleOS2.fOs2Register = TRUE;
            }))
        {
            return false;
        }

        return send(static_cast<ULONG>(ConsolepSetOS2OemFormat), sizeof(CONSOLE_SETOS2OEMFORMAT_MSG), [](auto& body) noexcept {
            body.SetConsoleOS2OemFormat.fOs2OemFormat = TRUE;
        });
    }

    bool test_user_defined_get_keyboard_layout_name_returns_hex_string()
    {
        DummyComm comm{};
        oc::condrv::ServerState state{};
        oc::condrv::NullHostIo host_io{};

        auto connect_packet = make_connect_packet(5005, 5006);
        oc::condrv::BasicApiMessage<DummyComm> connect_message(comm, connect_packet);
        auto connect_outcome = oc::condrv::dispatch_message(state, connect_message, host_io);
        if (!connect_outcome)
        {
            return false;
        }

        oc::condrv::ConnectionInformation info{};
        std::memcpy(&info, connect_message.completion().write.data, sizeof(info));

        const auto is_hex_w = [](const wchar_t ch) noexcept {
            return (ch >= L'0' && ch <= L'9') ||
                   (ch >= L'a' && ch <= L'f') ||
                   (ch >= L'A' && ch <= L'F');
        };

        const auto is_hex_a = [](const char ch) noexcept {
            return (ch >= '0' && ch <= '9') ||
                   (ch >= 'a' && ch <= 'f') ||
                   (ch >= 'A' && ch <= 'F');
        };

        auto packet = oc::condrv::IoPacket{};
        packet.payload.user_defined = oc::condrv::UserDefinedPacket{};
        packet.descriptor.identifier.LowPart = 96;
        packet.descriptor.function = oc::condrv::console_io_user_defined;
        packet.descriptor.process = info.process;
        packet.descriptor.object = info.output;
        packet.payload.user_defined.msg_header.ApiNumber = static_cast<ULONG>(ConsolepGetKeyboardLayoutName);
        packet.payload.user_defined.msg_header.ApiDescriptorSize = sizeof(CONSOLE_GETKEYBOARDLAYOUTNAME_MSG);

        // Wide.
        packet.payload.user_defined.u.console_msg_l3.GetKeyboardLayoutName.bAnsi = FALSE;
        oc::condrv::BasicApiMessage<DummyComm> message(comm, packet);
        auto outcome = oc::condrv::dispatch_message(state, message, host_io);
        if (!outcome || message.completion().io_status.Status != oc::core::status_success)
        {
            return false;
        }

        const auto& wide = message.packet().payload.user_defined.u.console_msg_l3.GetKeyboardLayoutName;
        if (wide.awchLayout[8] != L'\0')
        {
            return false;
        }

        for (int i = 0; i < 8; ++i)
        {
            if (!is_hex_w(wide.awchLayout[i]))
            {
                return false;
            }
        }

        // ANSI.
        packet.payload.user_defined.u.console_msg_l3.GetKeyboardLayoutName.bAnsi = TRUE;
        oc::condrv::BasicApiMessage<DummyComm> message_a(comm, packet);
        auto outcome_a = oc::condrv::dispatch_message(state, message_a, host_io);
        if (!outcome_a || message_a.completion().io_status.Status != oc::core::status_success)
        {
            return false;
        }

        const auto& ansi = message_a.packet().payload.user_defined.u.console_msg_l3.GetKeyboardLayoutName;
        if (ansi.achLayout[8] != '\0')
        {
            return false;
        }

        for (int i = 0; i < 8; ++i)
        {
            if (!is_hex_a(ansi.achLayout[i]))
            {
                return false;
            }
        }

        return true;
    }

    bool test_user_defined_get_mouse_info_matches_system_metrics()
    {
        DummyComm comm{};
        oc::condrv::ServerState state{};
        oc::condrv::NullHostIo host_io{};

        auto connect_packet = make_connect_packet(6001, 6002);
        oc::condrv::BasicApiMessage<DummyComm> connect_message(comm, connect_packet);
        auto connect_outcome = oc::condrv::dispatch_message(state, connect_message, host_io);
        if (!connect_outcome)
        {
            return false;
        }

        oc::condrv::ConnectionInformation info{};
        std::memcpy(&info, connect_message.completion().write.data, sizeof(info));

        auto packet = oc::condrv::IoPacket{};
        packet.payload.user_defined = oc::condrv::UserDefinedPacket{};
        packet.descriptor.identifier.LowPart = 91;
        packet.descriptor.function = oc::condrv::console_io_user_defined;
        packet.descriptor.process = info.process;
        packet.descriptor.object = info.output;
        packet.payload.user_defined.msg_header.ApiNumber = static_cast<ULONG>(ConsolepGetMouseInfo);
        packet.payload.user_defined.msg_header.ApiDescriptorSize = sizeof(CONSOLE_GETMOUSEINFO_MSG);

        oc::condrv::BasicApiMessage<DummyComm> message(comm, packet);
        auto outcome = oc::condrv::dispatch_message(state, message, host_io);
        if (!outcome)
        {
            return false;
        }

        if (message.completion().io_status.Status != oc::core::status_success)
        {
            return false;
        }

        const int expected = ::GetSystemMetrics(SM_CMOUSEBUTTONS);
        const ULONG expected_buttons = expected > 0 ? static_cast<ULONG>(expected) : 0;
        return message.packet().payload.user_defined.u.console_msg_l3.GetConsoleMouseInfo.NumButtons == expected_buttons;
    }

    bool test_user_defined_get_selection_info_defaults_to_none()
    {
        DummyComm comm{};
        oc::condrv::ServerState state{};
        oc::condrv::NullHostIo host_io{};

        auto connect_packet = make_connect_packet(7001, 7002);
        oc::condrv::BasicApiMessage<DummyComm> connect_message(comm, connect_packet);
        auto connect_outcome = oc::condrv::dispatch_message(state, connect_message, host_io);
        if (!connect_outcome)
        {
            return false;
        }

        oc::condrv::ConnectionInformation info{};
        std::memcpy(&info, connect_message.completion().write.data, sizeof(info));

        auto packet = oc::condrv::IoPacket{};
        packet.payload.user_defined = oc::condrv::UserDefinedPacket{};
        packet.descriptor.identifier.LowPart = 92;
        packet.descriptor.function = oc::condrv::console_io_user_defined;
        packet.descriptor.process = info.process;
        packet.descriptor.object = info.output;
        packet.payload.user_defined.msg_header.ApiNumber = static_cast<ULONG>(ConsolepGetSelectionInfo);
        packet.payload.user_defined.msg_header.ApiDescriptorSize = sizeof(CONSOLE_GETSELECTIONINFO_MSG);

        oc::condrv::BasicApiMessage<DummyComm> message(comm, packet);
        auto outcome = oc::condrv::dispatch_message(state, message, host_io);
        if (!outcome)
        {
            return false;
        }

        if (message.completion().io_status.Status != oc::core::status_success)
        {
            return false;
        }

        const auto info_out = message.packet().payload.user_defined.u.console_msg_l3.GetConsoleSelectionInfo.SelectionInfo;
        return info_out.dwFlags == 0 &&
               info_out.dwSelectionAnchor.X == 0 &&
               info_out.dwSelectionAnchor.Y == 0 &&
               info_out.srSelection.Left == 0 &&
               info_out.srSelection.Top == 0 &&
               info_out.srSelection.Right == 0 &&
               info_out.srSelection.Bottom == 0;
    }

    bool test_user_defined_get_console_process_list_reports_required_size_and_orders_newest_first()
    {
        DummyComm comm{};
        oc::condrv::ServerState state{};
        oc::condrv::NullHostIo host_io{};

        auto connect_a = make_connect_packet(101, 201);
        oc::condrv::BasicApiMessage<DummyComm> connect_a_message(comm, connect_a);
        auto connect_a_outcome = oc::condrv::dispatch_message(state, connect_a_message, host_io);
        if (!connect_a_outcome)
        {
            return false;
        }

        oc::condrv::ConnectionInformation info_a{};
        std::memcpy(&info_a, connect_a_message.completion().write.data, sizeof(info_a));

        auto connect_b = make_connect_packet(102, 202);
        oc::condrv::BasicApiMessage<DummyComm> connect_b_message(comm, connect_b);
        auto connect_b_outcome = oc::condrv::dispatch_message(state, connect_b_message, host_io);
        if (!connect_b_outcome)
        {
            return false;
        }

        auto connect_c = make_connect_packet(103, 203);
        oc::condrv::BasicApiMessage<DummyComm> connect_c_message(comm, connect_c);
        auto connect_c_outcome = oc::condrv::dispatch_message(state, connect_c_message, host_io);
        if (!connect_c_outcome)
        {
            return false;
        }

        const ULONG api_size = sizeof(CONSOLE_GETCONSOLEPROCESSLIST_MSG);

        // Insufficient buffer: should return required count > capacity and not write any PIDs.
        {
            auto packet = oc::condrv::IoPacket{};
            packet.payload.user_defined = oc::condrv::UserDefinedPacket{};
            packet.descriptor.identifier.LowPart = 93;
            packet.descriptor.function = oc::condrv::console_io_user_defined;
            packet.descriptor.process = info_a.process;
            packet.descriptor.object = info_a.output;
            packet.descriptor.output_size = api_size + (sizeof(DWORD) * 2);
            packet.payload.user_defined.msg_header.ApiNumber = static_cast<ULONG>(ConsolepGetConsoleProcessList);
            packet.payload.user_defined.msg_header.ApiDescriptorSize = api_size;

            oc::condrv::BasicApiMessage<DummyComm> message(comm, packet);
            auto outcome = oc::condrv::dispatch_message(state, message, host_io);
            if (!outcome || message.completion().io_status.Status != oc::core::status_success)
            {
                return false;
            }

            if (message.packet().payload.user_defined.u.console_msg_l3.GetConsoleProcessList.dwProcessCount != 3)
            {
                return false;
            }

            if (message.completion().io_status.Information != 0)
            {
                return false;
            }

            auto out = message.get_output_buffer();
            if (!out)
            {
                return false;
            }

            if (!std::all_of(out->begin(), out->end(), [](const std::byte value) noexcept { return value == std::byte{}; }))
            {
                return false;
            }
        }

        // Sufficient buffer: should write all PIDs newest-to-oldest.
        {
            auto packet = oc::condrv::IoPacket{};
            packet.payload.user_defined = oc::condrv::UserDefinedPacket{};
            packet.descriptor.identifier.LowPart = 94;
            packet.descriptor.function = oc::condrv::console_io_user_defined;
            packet.descriptor.process = info_a.process;
            packet.descriptor.object = info_a.output;
            packet.descriptor.output_size = api_size + (sizeof(DWORD) * 3);
            packet.payload.user_defined.msg_header.ApiNumber = static_cast<ULONG>(ConsolepGetConsoleProcessList);
            packet.payload.user_defined.msg_header.ApiDescriptorSize = api_size;

            oc::condrv::BasicApiMessage<DummyComm> message(comm, packet);
            auto outcome = oc::condrv::dispatch_message(state, message, host_io);
            if (!outcome || message.completion().io_status.Status != oc::core::status_success)
            {
                return false;
            }

            if (message.packet().payload.user_defined.u.console_msg_l3.GetConsoleProcessList.dwProcessCount != 3)
            {
                return false;
            }

            if (message.completion().io_status.Information != sizeof(DWORD) * 3)
            {
                return false;
            }

            auto out = message.get_output_buffer();
            if (!out)
            {
                return false;
            }

            std::array<DWORD, 3> pids{};
            std::memcpy(pids.data(), out->data(), sizeof(pids));
            return pids == std::array<DWORD, 3>{ 103, 102, 101 };
        }
    }

    bool test_user_defined_get_set_history_round_trips()
    {
        DummyComm comm{};
        oc::condrv::ServerState state{};
        oc::condrv::NullHostIo host_io{};

        auto connect_packet = make_connect_packet(8001, 8002);
        oc::condrv::BasicApiMessage<DummyComm> connect_message(comm, connect_packet);
        auto connect_outcome = oc::condrv::dispatch_message(state, connect_message, host_io);
        if (!connect_outcome)
        {
            return false;
        }

        oc::condrv::ConnectionInformation info{};
        std::memcpy(&info, connect_message.completion().write.data, sizeof(info));

        // Default get.
        oc::condrv::IoPacket get_packet{};
        get_packet.payload.user_defined = oc::condrv::UserDefinedPacket{};
        get_packet.descriptor.identifier.LowPart = 200;
        get_packet.descriptor.function = oc::condrv::console_io_user_defined;
        get_packet.descriptor.process = info.process;
        get_packet.descriptor.object = info.output;
        get_packet.payload.user_defined.msg_header.ApiNumber = static_cast<ULONG>(ConsolepGetHistory);
        get_packet.payload.user_defined.msg_header.ApiDescriptorSize = sizeof(CONSOLE_HISTORY_MSG);

        oc::condrv::BasicApiMessage<DummyComm> get_message(comm, get_packet);
        auto get_outcome = oc::condrv::dispatch_message(state, get_message, host_io);
        if (!get_outcome || get_message.completion().io_status.Status != oc::core::status_success)
        {
            return false;
        }

        const auto& defaults = get_message.packet().payload.user_defined.u.console_msg_l3.GetConsoleHistory;
        if (defaults.HistoryBufferSize != 50 || defaults.NumberOfHistoryBuffers != 4 || defaults.dwFlags != 0)
        {
            return false;
        }

        // Set.
        oc::condrv::IoPacket set_packet{};
        set_packet.payload.user_defined = oc::condrv::UserDefinedPacket{};
        set_packet.descriptor.identifier.LowPart = 201;
        set_packet.descriptor.function = oc::condrv::console_io_user_defined;
        set_packet.descriptor.process = info.process;
        set_packet.descriptor.object = info.output;
        set_packet.payload.user_defined.msg_header.ApiNumber = static_cast<ULONG>(ConsolepSetHistory);
        set_packet.payload.user_defined.msg_header.ApiDescriptorSize = sizeof(CONSOLE_HISTORY_MSG);

        auto& set_body = set_packet.payload.user_defined.u.console_msg_l3.SetConsoleHistory;
        set_body.HistoryBufferSize = 123;
        set_body.NumberOfHistoryBuffers = 9;
        set_body.dwFlags = 0x55AA;

        oc::condrv::BasicApiMessage<DummyComm> set_message(comm, set_packet);
        auto set_outcome = oc::condrv::dispatch_message(state, set_message, host_io);
        if (!set_outcome || set_message.completion().io_status.Status != oc::core::status_success)
        {
            return false;
        }

        // Get again.
        oc::condrv::BasicApiMessage<DummyComm> get_again(comm, get_packet);
        auto get_again_outcome = oc::condrv::dispatch_message(state, get_again, host_io);
        if (!get_again_outcome || get_again.completion().io_status.Status != oc::core::status_success)
        {
            return false;
        }

        const auto& after = get_again.packet().payload.user_defined.u.console_msg_l3.GetConsoleHistory;
        return after.HistoryBufferSize == 123 && after.NumberOfHistoryBuffers == 9 && after.dwFlags == 0x55AA;
    }

    bool test_user_defined_command_history_apis_succeed_with_empty_history()
    {
        DummyComm comm{};
        oc::condrv::ServerState state{};
        oc::condrv::NullHostIo host_io{};

        auto connect_packet = make_connect_packet(8101, 8102);
        oc::condrv::BasicApiMessage<DummyComm> connect_message(comm, connect_packet);
        auto connect_outcome = oc::condrv::dispatch_message(state, connect_message, host_io);
        if (!connect_outcome)
        {
            return false;
        }

        oc::condrv::ConnectionInformation info{};
        std::memcpy(&info, connect_message.completion().write.data, sizeof(info));

        constexpr std::wstring_view exe = L"cmd.exe";
        constexpr ULONG header_size = sizeof(CONSOLE_MSG_HEADER);
        const ULONG exe_bytes = static_cast<ULONG>(exe.size() * sizeof(wchar_t));

        // Get length.
        {
            constexpr ULONG api_size = sizeof(CONSOLE_GETCOMMANDHISTORYLENGTH_MSG);
            constexpr ULONG read_offset = api_size + header_size;

            oc::condrv::IoPacket length_packet{};
            length_packet.payload.user_defined = oc::condrv::UserDefinedPacket{};
            length_packet.descriptor.identifier.LowPart = 210;
            length_packet.descriptor.function = oc::condrv::console_io_user_defined;
            length_packet.descriptor.process = info.process;
            length_packet.descriptor.object = info.output;
            length_packet.descriptor.input_size = read_offset + exe_bytes;
            length_packet.descriptor.output_size = api_size;
            length_packet.payload.user_defined.msg_header.ApiNumber = static_cast<ULONG>(ConsolepGetCommandHistoryLength);
            length_packet.payload.user_defined.msg_header.ApiDescriptorSize = api_size;

            auto& length_body = length_packet.payload.user_defined.u.console_msg_l3.GetConsoleCommandHistoryLengthW;
            length_body.Unicode = TRUE;
            length_body.CommandHistoryLength = 0;

            comm.input.assign(length_packet.descriptor.input_size, std::byte{});
            std::memcpy(comm.input.data() + read_offset, exe.data(), exe_bytes);

            oc::condrv::BasicApiMessage<DummyComm> length_message(comm, length_packet);
            auto length_outcome = oc::condrv::dispatch_message(state, length_message, host_io);
            if (!length_outcome || length_message.completion().io_status.Status != oc::core::status_success)
            {
                return false;
            }

            if (length_message.packet().payload.user_defined.u.console_msg_l3.GetConsoleCommandHistoryLengthW.CommandHistoryLength != 0)
            {
                return false;
            }
        }

        // Get history.
        {
            constexpr ULONG api_size = sizeof(CONSOLE_GETCOMMANDHISTORY_MSG);
            constexpr ULONG read_offset = api_size + header_size;

            oc::condrv::IoPacket history_packet{};
            history_packet.payload.user_defined = oc::condrv::UserDefinedPacket{};
            history_packet.descriptor.identifier.LowPart = 211;
            history_packet.descriptor.function = oc::condrv::console_io_user_defined;
            history_packet.descriptor.process = info.process;
            history_packet.descriptor.object = info.output;
            history_packet.descriptor.input_size = read_offset + exe_bytes;
            history_packet.descriptor.output_size = api_size + 64;
            history_packet.payload.user_defined.msg_header.ApiNumber = static_cast<ULONG>(ConsolepGetCommandHistory);
            history_packet.payload.user_defined.msg_header.ApiDescriptorSize = api_size;

            auto& history_body = history_packet.payload.user_defined.u.console_msg_l3.GetConsoleCommandHistoryW;
            history_body.Unicode = TRUE;
            history_body.CommandBufferLength = 0;

            comm.input.assign(history_packet.descriptor.input_size, std::byte{});
            std::memcpy(comm.input.data() + read_offset, exe.data(), exe_bytes);

            oc::condrv::BasicApiMessage<DummyComm> history_message(comm, history_packet);
            auto history_outcome = oc::condrv::dispatch_message(state, history_message, host_io);
            if (!history_outcome || history_message.completion().io_status.Status != oc::core::status_success)
            {
                return false;
            }

            if (history_message.packet().payload.user_defined.u.console_msg_l3.GetConsoleCommandHistoryW.CommandBufferLength != 0)
            {
                return false;
            }
        }

        // Set number of commands.
        {
            constexpr ULONG api_size = sizeof(CONSOLE_SETNUMBEROFCOMMANDS_MSG);
            constexpr ULONG read_offset = api_size + header_size;

            oc::condrv::IoPacket set_packet{};
            set_packet.payload.user_defined = oc::condrv::UserDefinedPacket{};
            set_packet.descriptor.identifier.LowPart = 212;
            set_packet.descriptor.function = oc::condrv::console_io_user_defined;
            set_packet.descriptor.process = info.process;
            set_packet.descriptor.object = info.output;
            set_packet.descriptor.input_size = read_offset + exe_bytes;
            set_packet.descriptor.output_size = api_size;
            set_packet.payload.user_defined.msg_header.ApiNumber = static_cast<ULONG>(ConsolepSetNumberOfCommands);
            set_packet.payload.user_defined.msg_header.ApiDescriptorSize = api_size;

            auto& set_body = set_packet.payload.user_defined.u.console_msg_l3.SetConsoleNumberOfCommandsW;
            set_body.Unicode = TRUE;
            set_body.NumCommands = 10;

            comm.input.assign(set_packet.descriptor.input_size, std::byte{});
            std::memcpy(comm.input.data() + read_offset, exe.data(), exe_bytes);

            oc::condrv::BasicApiMessage<DummyComm> set_message(comm, set_packet);
            auto set_outcome = oc::condrv::dispatch_message(state, set_message, host_io);
            if (!set_outcome || set_message.completion().io_status.Status != oc::core::status_success)
            {
                return false;
            }
        }

        // Expunge.
        {
            constexpr ULONG api_size = sizeof(CONSOLE_EXPUNGECOMMANDHISTORY_MSG);
            constexpr ULONG read_offset = api_size + header_size;

            oc::condrv::IoPacket expunge_packet{};
            expunge_packet.payload.user_defined = oc::condrv::UserDefinedPacket{};
            expunge_packet.descriptor.identifier.LowPart = 213;
            expunge_packet.descriptor.function = oc::condrv::console_io_user_defined;
            expunge_packet.descriptor.process = info.process;
            expunge_packet.descriptor.object = info.output;
            expunge_packet.descriptor.input_size = read_offset + exe_bytes;
            expunge_packet.descriptor.output_size = api_size;
            expunge_packet.payload.user_defined.msg_header.ApiNumber = static_cast<ULONG>(ConsolepExpungeCommandHistory);
            expunge_packet.payload.user_defined.msg_header.ApiDescriptorSize = api_size;

            auto& expunge_body = expunge_packet.payload.user_defined.u.console_msg_l3.ExpungeConsoleCommandHistoryW;
            expunge_body.Unicode = TRUE;

            comm.input.assign(expunge_packet.descriptor.input_size, std::byte{});
            std::memcpy(comm.input.data() + read_offset, exe.data(), exe_bytes);

            oc::condrv::BasicApiMessage<DummyComm> expunge_message(comm, expunge_packet);
            auto expunge_outcome = oc::condrv::dispatch_message(state, expunge_message, host_io);
            return expunge_outcome && expunge_message.completion().io_status.Status == oc::core::status_success;
        }
    }

    bool test_user_defined_screen_buffer_info_and_cursor_roundtrip()
    {
        DummyComm comm{};
        oc::condrv::ServerState state{};
        oc::condrv::NullHostIo host_io{};

        auto connect_packet = make_connect_packet(555, 666);
        oc::condrv::BasicApiMessage<DummyComm> connect_message(comm, connect_packet);
        auto connect_outcome = oc::condrv::dispatch_message(state, connect_message, host_io);
        if (!connect_outcome)
        {
            return false;
        }

        oc::condrv::ConnectionInformation info{};
        std::memcpy(&info, connect_message.completion().write.data, sizeof(info));

        auto get_info_packet = oc::condrv::IoPacket{};
        get_info_packet.payload.user_defined = oc::condrv::UserDefinedPacket{};
        get_info_packet.descriptor.identifier.LowPart = 20;
        get_info_packet.descriptor.function = oc::condrv::console_io_user_defined;
        get_info_packet.descriptor.process = info.process;
        get_info_packet.descriptor.object = info.output;
        get_info_packet.payload.user_defined.msg_header.ApiNumber = static_cast<ULONG>(ConsolepGetScreenBufferInfo);
        get_info_packet.payload.user_defined.msg_header.ApiDescriptorSize = sizeof(CONSOLE_SCREENBUFFERINFO_MSG);

        oc::condrv::BasicApiMessage<DummyComm> get_info(comm, get_info_packet);
        auto get_info_outcome = oc::condrv::dispatch_message(state, get_info, host_io);
        if (!get_info_outcome)
        {
            return false;
        }

        if (get_info.completion().io_status.Status != oc::core::status_success)
        {
            return false;
        }

        const auto& initial = get_info.packet().payload.user_defined.u.console_msg_l2.GetConsoleScreenBufferInfo;
        if (initial.Size.X != 120 || initial.Size.Y != 40)
        {
            return false;
        }
        if (initial.Attributes != 0x07)
        {
            return false;
        }

        auto set_cursor_packet = oc::condrv::IoPacket{};
        set_cursor_packet.payload.user_defined = oc::condrv::UserDefinedPacket{};
        set_cursor_packet.descriptor.identifier.LowPart = 21;
        set_cursor_packet.descriptor.function = oc::condrv::console_io_user_defined;
        set_cursor_packet.descriptor.process = info.process;
        set_cursor_packet.descriptor.object = info.output;
        set_cursor_packet.payload.user_defined.msg_header.ApiNumber = static_cast<ULONG>(ConsolepSetCursorPosition);
        set_cursor_packet.payload.user_defined.msg_header.ApiDescriptorSize = sizeof(CONSOLE_SETCURSORPOSITION_MSG);
        set_cursor_packet.payload.user_defined.u.console_msg_l2.SetConsoleCursorPosition.CursorPosition.X = 10;
        set_cursor_packet.payload.user_defined.u.console_msg_l2.SetConsoleCursorPosition.CursorPosition.Y = 5;

        oc::condrv::BasicApiMessage<DummyComm> set_cursor_message(comm, set_cursor_packet);
        auto set_cursor_outcome = oc::condrv::dispatch_message(state, set_cursor_message, host_io);
        if (!set_cursor_outcome)
        {
            return false;
        }
        if (set_cursor_message.completion().io_status.Status != oc::core::status_success)
        {
            return false;
        }

        oc::condrv::BasicApiMessage<DummyComm> get_again(comm, get_info_packet);
        auto get_again_outcome = oc::condrv::dispatch_message(state, get_again, host_io);
        if (!get_again_outcome)
        {
            return false;
        }
        if (get_again.completion().io_status.Status != oc::core::status_success)
        {
            return false;
        }

        const auto& after_cursor = get_again.packet().payload.user_defined.u.console_msg_l2.GetConsoleScreenBufferInfo;
        if (after_cursor.CursorPosition.X != 10 || after_cursor.CursorPosition.Y != 5)
        {
            return false;
        }

        auto set_attr_packet = oc::condrv::IoPacket{};
        set_attr_packet.payload.user_defined = oc::condrv::UserDefinedPacket{};
        set_attr_packet.descriptor.identifier.LowPart = 22;
        set_attr_packet.descriptor.function = oc::condrv::console_io_user_defined;
        set_attr_packet.descriptor.process = info.process;
        set_attr_packet.descriptor.object = info.output;
        set_attr_packet.payload.user_defined.msg_header.ApiNumber = static_cast<ULONG>(ConsolepSetTextAttribute);
        set_attr_packet.payload.user_defined.msg_header.ApiDescriptorSize = sizeof(CONSOLE_SETTEXTATTRIBUTE_MSG);
        set_attr_packet.payload.user_defined.u.console_msg_l2.SetConsoleTextAttribute.Attributes = 0x1E;

        oc::condrv::BasicApiMessage<DummyComm> set_attr_message(comm, set_attr_packet);
        auto set_attr_outcome = oc::condrv::dispatch_message(state, set_attr_message, host_io);
        if (!set_attr_outcome)
        {
            return false;
        }
        if (set_attr_message.completion().io_status.Status != oc::core::status_success)
        {
            return false;
        }

        oc::condrv::BasicApiMessage<DummyComm> get_after_attr(comm, get_info_packet);
        auto get_after_attr_outcome = oc::condrv::dispatch_message(state, get_after_attr, host_io);
        if (!get_after_attr_outcome)
        {
            return false;
        }

        const auto& after_attr = get_after_attr.packet().payload.user_defined.u.console_msg_l2.GetConsoleScreenBufferInfo;
        return after_attr.Attributes == 0x1E;
    }

    bool test_user_defined_cursor_info_roundtrip()
    {
        DummyComm comm{};
        oc::condrv::ServerState state{};
        oc::condrv::NullHostIo host_io{};

        auto connect_packet = make_connect_packet(777, 888);
        oc::condrv::BasicApiMessage<DummyComm> connect_message(comm, connect_packet);
        auto connect_outcome = oc::condrv::dispatch_message(state, connect_message, host_io);
        if (!connect_outcome)
        {
            return false;
        }

        oc::condrv::ConnectionInformation info{};
        std::memcpy(&info, connect_message.completion().write.data, sizeof(info));

        auto get_cursor_packet = oc::condrv::IoPacket{};
        get_cursor_packet.payload.user_defined = oc::condrv::UserDefinedPacket{};
        get_cursor_packet.descriptor.identifier.LowPart = 23;
        get_cursor_packet.descriptor.function = oc::condrv::console_io_user_defined;
        get_cursor_packet.descriptor.process = info.process;
        get_cursor_packet.descriptor.object = info.output;
        get_cursor_packet.payload.user_defined.msg_header.ApiNumber = static_cast<ULONG>(ConsolepGetCursorInfo);
        get_cursor_packet.payload.user_defined.msg_header.ApiDescriptorSize = sizeof(CONSOLE_GETCURSORINFO_MSG);

        oc::condrv::BasicApiMessage<DummyComm> get_cursor(comm, get_cursor_packet);
        auto get_cursor_outcome = oc::condrv::dispatch_message(state, get_cursor, host_io);
        if (!get_cursor_outcome)
        {
            return false;
        }

        const auto& initial = get_cursor.packet().payload.user_defined.u.console_msg_l2.GetConsoleCursorInfo;
        if (get_cursor.completion().io_status.Status != oc::core::status_success ||
            initial.CursorSize != 25 ||
            initial.Visible == FALSE)
        {
            return false;
        }

        auto set_cursor_packet = oc::condrv::IoPacket{};
        set_cursor_packet.payload.user_defined = oc::condrv::UserDefinedPacket{};
        set_cursor_packet.descriptor.identifier.LowPart = 24;
        set_cursor_packet.descriptor.function = oc::condrv::console_io_user_defined;
        set_cursor_packet.descriptor.process = info.process;
        set_cursor_packet.descriptor.object = info.output;
        set_cursor_packet.payload.user_defined.msg_header.ApiNumber = static_cast<ULONG>(ConsolepSetCursorInfo);
        set_cursor_packet.payload.user_defined.msg_header.ApiDescriptorSize = sizeof(CONSOLE_SETCURSORINFO_MSG);
        set_cursor_packet.payload.user_defined.u.console_msg_l2.SetConsoleCursorInfo.CursorSize = 50;
        set_cursor_packet.payload.user_defined.u.console_msg_l2.SetConsoleCursorInfo.Visible = FALSE;

        oc::condrv::BasicApiMessage<DummyComm> set_cursor(comm, set_cursor_packet);
        auto set_cursor_outcome = oc::condrv::dispatch_message(state, set_cursor, host_io);
        if (!set_cursor_outcome || set_cursor.completion().io_status.Status != oc::core::status_success)
        {
            return false;
        }

        oc::condrv::BasicApiMessage<DummyComm> get_again(comm, get_cursor_packet);
        auto get_again_outcome = oc::condrv::dispatch_message(state, get_again, host_io);
        if (!get_again_outcome || get_again.completion().io_status.Status != oc::core::status_success)
        {
            return false;
        }

        const auto& after = get_again.packet().payload.user_defined.u.console_msg_l2.GetConsoleCursorInfo;
        return after.CursorSize == 50 && after.Visible == FALSE;
    }

    bool test_user_defined_get_largest_window_size()
    {
        DummyComm comm{};
        oc::condrv::ServerState state{};
        oc::condrv::NullHostIo host_io{};

        auto connect_packet = make_connect_packet(999, 1000);
        oc::condrv::BasicApiMessage<DummyComm> connect_message(comm, connect_packet);
        auto connect_outcome = oc::condrv::dispatch_message(state, connect_message, host_io);
        if (!connect_outcome)
        {
            return false;
        }

        oc::condrv::ConnectionInformation info{};
        std::memcpy(&info, connect_message.completion().write.data, sizeof(info));

        auto get_largest_packet = oc::condrv::IoPacket{};
        get_largest_packet.payload.user_defined = oc::condrv::UserDefinedPacket{};
        get_largest_packet.descriptor.identifier.LowPart = 25;
        get_largest_packet.descriptor.function = oc::condrv::console_io_user_defined;
        get_largest_packet.descriptor.process = info.process;
        get_largest_packet.descriptor.object = info.output;
        get_largest_packet.payload.user_defined.msg_header.ApiNumber = static_cast<ULONG>(ConsolepGetLargestWindowSize);
        get_largest_packet.payload.user_defined.msg_header.ApiDescriptorSize = sizeof(CONSOLE_GETLARGESTWINDOWSIZE_MSG);

        oc::condrv::BasicApiMessage<DummyComm> message(comm, get_largest_packet);
        auto outcome = oc::condrv::dispatch_message(state, message, host_io);
        if (!outcome)
        {
            return false;
        }

        const auto size = message.packet().payload.user_defined.u.console_msg_l2.GetLargestConsoleWindowSize.Size;
        return message.completion().io_status.Status == oc::core::status_success &&
               size.X == 120 &&
               size.Y == 40;
    }

    bool test_user_defined_set_screen_buffer_info_round_trips()
    {
        DummyComm comm{};
        oc::condrv::ServerState state{};
        oc::condrv::NullHostIo host_io{};

        auto connect_packet = make_connect_packet(123, 456);
        oc::condrv::BasicApiMessage<DummyComm> connect_message(comm, connect_packet);
        auto connect_outcome = oc::condrv::dispatch_message(state, connect_message, host_io);
        if (!connect_outcome)
        {
            return false;
        }

        oc::condrv::ConnectionInformation info{};
        std::memcpy(&info, connect_message.completion().write.data, sizeof(info));

        auto set_info_packet = oc::condrv::IoPacket{};
        set_info_packet.payload.user_defined = oc::condrv::UserDefinedPacket{};
        set_info_packet.descriptor.identifier.LowPart = 26;
        set_info_packet.descriptor.function = oc::condrv::console_io_user_defined;
        set_info_packet.descriptor.process = info.process;
        set_info_packet.descriptor.object = info.output;
        set_info_packet.payload.user_defined.msg_header.ApiNumber = static_cast<ULONG>(ConsolepSetScreenBufferInfo);
        set_info_packet.payload.user_defined.msg_header.ApiDescriptorSize = sizeof(CONSOLE_SCREENBUFFERINFO_MSG);

        auto& body = set_info_packet.payload.user_defined.u.console_msg_l2.SetConsoleScreenBufferInfo;
        body.Size = COORD{ 80, 25 };
        body.CursorPosition = COORD{ 3, 4 };
        body.Attributes = 0x1E;
        body.ScrollPosition = COORD{ 0, 0 };
        body.CurrentWindowSize = COORD{ 79, 24 };
        body.MaximumWindowSize = COORD{ 120, 40 };
        body.PopupAttributes = 0;
        body.FullscreenSupported = FALSE;
        for (size_t i = 0; i < 16; ++i)
        {
            body.ColorTable[i] = RGB(static_cast<BYTE>(i), static_cast<BYTE>(i + 1), static_cast<BYTE>(i + 2));
        }

        oc::condrv::BasicApiMessage<DummyComm> set_info(comm, set_info_packet);
        auto set_outcome = oc::condrv::dispatch_message(state, set_info, host_io);
        if (!set_outcome || set_info.completion().io_status.Status != oc::core::status_success)
        {
            return false;
        }

        auto get_info_packet = oc::condrv::IoPacket{};
        get_info_packet.payload.user_defined = oc::condrv::UserDefinedPacket{};
        get_info_packet.descriptor.identifier.LowPart = 27;
        get_info_packet.descriptor.function = oc::condrv::console_io_user_defined;
        get_info_packet.descriptor.process = info.process;
        get_info_packet.descriptor.object = info.output;
        get_info_packet.payload.user_defined.msg_header.ApiNumber = static_cast<ULONG>(ConsolepGetScreenBufferInfo);
        get_info_packet.payload.user_defined.msg_header.ApiDescriptorSize = sizeof(CONSOLE_SCREENBUFFERINFO_MSG);

        oc::condrv::BasicApiMessage<DummyComm> get_info(comm, get_info_packet);
        auto get_outcome = oc::condrv::dispatch_message(state, get_info, host_io);
        if (!get_outcome || get_info.completion().io_status.Status != oc::core::status_success)
        {
            return false;
        }

        const auto& returned = get_info.packet().payload.user_defined.u.console_msg_l2.GetConsoleScreenBufferInfo;
        if (returned.Size.X != 80 || returned.Size.Y != 25)
        {
            return false;
        }
        if (returned.CursorPosition.X != 3 || returned.CursorPosition.Y != 4)
        {
            return false;
        }
        if (returned.Attributes != 0x1E)
        {
            return false;
        }
        if (returned.ScrollPosition.X != 0 || returned.ScrollPosition.Y != 0)
        {
            return false;
        }
        if (returned.CurrentWindowSize.X != 79 || returned.CurrentWindowSize.Y != 24)
        {
            return false;
        }
        if (returned.ColorTable[0] != body.ColorTable[0] || returned.ColorTable[15] != body.ColorTable[15])
        {
            return false;
        }

        return true;
    }

    bool test_generate_ctrl_event_sends_end_task_for_connected_processes()
    {
        DummyComm comm{};
        oc::condrv::ServerState state{};
        CtrlCaptureHostIo host_io{};

        auto connect_first = make_connect_packet(101, 201);
        oc::condrv::BasicApiMessage<DummyComm> connect_message(comm, connect_first);
        auto connect_outcome = oc::condrv::dispatch_message(state, connect_message, host_io);
        if (!connect_outcome || connect_message.completion().io_status.Status != oc::core::status_success)
        {
            return false;
        }

        oc::condrv::ConnectionInformation info{};
        std::memcpy(&info, connect_message.completion().write.data, sizeof(info));

        auto connect_second = make_connect_packet(102, 202);
        connect_second.descriptor.identifier.LowPart = 2;
        oc::condrv::BasicApiMessage<DummyComm> connect_second_message(comm, connect_second);
        auto connect_second_outcome = oc::condrv::dispatch_message(state, connect_second_message, host_io);
        if (!connect_second_outcome || connect_second_message.completion().io_status.Status != oc::core::status_success)
        {
            return false;
        }

        oc::condrv::IoPacket ctrl_packet{};
        ctrl_packet.payload.user_defined = oc::condrv::UserDefinedPacket{};
        ctrl_packet.descriptor.identifier.LowPart = 50;
        ctrl_packet.descriptor.function = oc::condrv::console_io_user_defined;
        ctrl_packet.descriptor.process = info.process;
        ctrl_packet.descriptor.object = info.input;
        ctrl_packet.payload.user_defined.msg_header.ApiNumber = static_cast<ULONG>(ConsolepGenerateCtrlEvent);
        ctrl_packet.payload.user_defined.msg_header.ApiDescriptorSize = sizeof(CONSOLE_CTRLEVENT_MSG);

        auto& body = ctrl_packet.payload.user_defined.u.console_msg_l2.GenerateConsoleCtrlEvent;
        body.CtrlEvent = CTRL_C_EVENT;
        body.ProcessGroupId = 0;

        oc::condrv::BasicApiMessage<DummyComm> ctrl_message(comm, ctrl_packet);
        auto ctrl_outcome = oc::condrv::dispatch_message(state, ctrl_message, host_io);
        if (!ctrl_outcome || ctrl_message.completion().io_status.Status != oc::core::status_success)
        {
            return false;
        }

        std::sort(host_io.end_task_pids.begin(), host_io.end_task_pids.end());
        return host_io.end_task_pids == std::vector<DWORD>{ 101, 102 };
    }
}

bool run_condrv_server_dispatch_tests()
{
    if (!test_connect_and_disconnect_lifecycle())
    {
        fwprintf(stderr, L"[condrv dispatch] test_connect_and_disconnect_lifecycle failed\n");
        return false;
    }
    if (!test_create_and_close_object())
    {
        fwprintf(stderr, L"[condrv dispatch] test_create_and_close_object failed\n");
        return false;
    }
    if (!test_create_object_requires_process_handle())
    {
        fwprintf(stderr, L"[condrv dispatch] test_create_object_requires_process_handle failed\n");
        return false;
    }
    if (!test_new_output_is_supported())
    {
        fwprintf(stderr, L"[condrv dispatch] test_new_output_is_supported failed\n");
        return false;
    }
    if (!test_disconnect_closes_owned_objects())
    {
        fwprintf(stderr, L"[condrv dispatch] test_disconnect_closes_owned_objects failed\n");
        return false;
    }
    if (!test_new_output_has_independent_screen_buffer_state())
    {
        fwprintf(stderr, L"[condrv dispatch] test_new_output_has_independent_screen_buffer_state failed\n");
        return false;
    }
    if (!test_set_active_screen_buffer_affects_current_output_creation())
    {
        fwprintf(stderr, L"[condrv dispatch] test_set_active_screen_buffer_affects_current_output_creation failed\n");
        return false;
    }
    if (!test_user_defined_get_set_mode())
    {
        fwprintf(stderr, L"[condrv dispatch] test_user_defined_get_set_mode failed\n");
        return false;
    }
    if (!test_user_defined_set_output_mode_validates_flags())
    {
        fwprintf(stderr, L"[condrv dispatch] test_user_defined_set_output_mode_validates_flags failed\n");
        return false;
    }
    if (!test_user_defined_get_cp())
    {
        fwprintf(stderr, L"[condrv dispatch] test_user_defined_get_cp failed\n");
        return false;
    }
    if (!test_user_defined_get_console_window_returns_null())
    {
        fwprintf(stderr, L"[condrv dispatch] test_user_defined_get_console_window_returns_null failed\n");
        return false;
    }
    if (!test_user_defined_get_display_mode_returns_zero())
    {
        fwprintf(stderr, L"[condrv dispatch] test_user_defined_get_display_mode_returns_zero failed\n");
        return false;
    }
    if (!test_user_defined_font_apis_round_trip())
    {
        fwprintf(stderr, L"[condrv dispatch] test_user_defined_font_apis_round_trip failed\n");
        return false;
    }
    if (!test_user_defined_set_window_info_relative_resizes_window())
    {
        fwprintf(stderr, L"[condrv dispatch] test_user_defined_set_window_info_relative_resizes_window failed\n");
        return false;
    }
    if (!test_user_defined_window_info_updates_scroll_position())
    {
        fwprintf(stderr, L"[condrv dispatch] test_user_defined_window_info_updates_scroll_position failed\n");
        return false;
    }
    if (!test_user_defined_cursor_position_snaps_viewport())
    {
        fwprintf(stderr, L"[condrv dispatch] test_user_defined_cursor_position_snaps_viewport failed\n");
        return false;
    }
    if (!test_user_defined_cursor_mode_round_trips())
    {
        fwprintf(stderr, L"[condrv dispatch] test_user_defined_cursor_mode_round_trips failed\n");
        return false;
    }
    if (!test_user_defined_nls_mode_round_trips())
    {
        fwprintf(stderr, L"[condrv dispatch] test_user_defined_nls_mode_round_trips failed\n");
        return false;
    }
    if (!test_user_defined_char_type_returns_sbcs_and_validates_coords())
    {
        fwprintf(stderr, L"[condrv dispatch] test_user_defined_char_type_returns_sbcs_and_validates_coords failed\n");
        return false;
    }
    if (!test_user_defined_compat_misc_stubs_succeed())
    {
        fwprintf(stderr, L"[condrv dispatch] test_user_defined_compat_misc_stubs_succeed failed\n");
        return false;
    }
    if (!test_user_defined_get_keyboard_layout_name_returns_hex_string())
    {
        fwprintf(stderr, L"[condrv dispatch] test_user_defined_get_keyboard_layout_name_returns_hex_string failed\n");
        return false;
    }
    if (!test_user_defined_get_mouse_info_matches_system_metrics())
    {
        fwprintf(stderr, L"[condrv dispatch] test_user_defined_get_mouse_info_matches_system_metrics failed\n");
        return false;
    }
    if (!test_user_defined_get_selection_info_defaults_to_none())
    {
        fwprintf(stderr, L"[condrv dispatch] test_user_defined_get_selection_info_defaults_to_none failed\n");
        return false;
    }
    if (!test_user_defined_get_console_process_list_reports_required_size_and_orders_newest_first())
    {
        fwprintf(stderr, L"[condrv dispatch] test_user_defined_get_console_process_list_reports_required_size_and_orders_newest_first failed\n");
        return false;
    }
    if (!test_user_defined_get_set_history_round_trips())
    {
        fwprintf(stderr, L"[condrv dispatch] test_user_defined_get_set_history_round_trips failed\n");
        return false;
    }
    if (!test_user_defined_command_history_apis_succeed_with_empty_history())
    {
        fwprintf(stderr, L"[condrv dispatch] test_user_defined_command_history_apis_succeed_with_empty_history failed\n");
        return false;
    }
    if (!test_user_defined_screen_buffer_info_and_cursor_roundtrip())
    {
        fwprintf(stderr, L"[condrv dispatch] test_user_defined_screen_buffer_info_and_cursor_roundtrip failed\n");
        return false;
    }
    if (!test_user_defined_cursor_info_roundtrip())
    {
        fwprintf(stderr, L"[condrv dispatch] test_user_defined_cursor_info_roundtrip failed\n");
        return false;
    }
    if (!test_user_defined_get_largest_window_size())
    {
        fwprintf(stderr, L"[condrv dispatch] test_user_defined_get_largest_window_size failed\n");
        return false;
    }
    if (!test_user_defined_set_screen_buffer_info_round_trips())
    {
        fwprintf(stderr, L"[condrv dispatch] test_user_defined_set_screen_buffer_info_round_trips failed\n");
        return false;
    }
    if (!test_generate_ctrl_event_sends_end_task_for_connected_processes())
    {
        fwprintf(stderr, L"[condrv dispatch] test_generate_ctrl_event_sends_end_task_for_connected_processes failed\n");
        return false;
    }

    return true;
}
