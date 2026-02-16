#pragma once

// A small, testable message wrapper around the ConDrv protocol.
//
// The upstream conhost implementation uses a larger `CONSOLE_API_MSG` structure
// with additional state, helpers, and integration with the full console object
// model. The replacement begins with a minimal, deterministic wrapper that:
// - owns per-message input/output buffers (as `std::vector<std::byte>`)
// - reads input payload via `IOCTL_CONDRV_READ_INPUT`
// - writes output payload via `IOCTL_CONDRV_WRITE_OUTPUT`
// - exposes the completion structure for `IOCTL_CONDRV_COMPLETE_IO`
//
// This is the foundation for a future server-mode dispatcher implementation.

#include "condrv/condrv_device_comm.hpp"
#include "condrv/condrv_packet.hpp"
#include "core/assert.hpp"

#include <Windows.h>

#include <cstddef>
#include <cstring>
#include <expected>
#include <span>
#include <type_traits>
#include <vector>

namespace oc::condrv
{
    [[nodiscard]] inline bool nt_success(const NTSTATUS status) noexcept
    {
        return status >= 0;
    }

    template<typename Comm>
    class BasicApiMessage final
    {
    public:
        BasicApiMessage(Comm& comm, IoPacket packet) noexcept :
            _comm(&comm),
            _packet(packet)
        {
            _complete.identifier = _packet.descriptor.identifier;
        }

        [[nodiscard]] const IoDescriptor& descriptor() const noexcept
        {
            return _packet.descriptor;
        }

        [[nodiscard]] const IoPacket& packet() const noexcept
        {
            return _packet;
        }

        [[nodiscard]] IoPacket& packet() noexcept
        {
            return _packet;
        }

        [[nodiscard]] IoComplete& completion() noexcept
        {
            return _complete;
        }

        void set_reply_status(const NTSTATUS status) noexcept
        {
            _complete.io_status.Status = status;
        }

        void set_reply_information(const ULONG_PTR information) noexcept
        {
            _complete.io_status.Information = information;
        }

        template<typename T>
        void set_completion_write_data(const T& value) noexcept
        {
            static_assert(std::is_trivially_copyable_v<T>, "Completion write data must be trivially copyable");

            _completion_write_storage.resize(sizeof(T));
            std::memcpy(_completion_write_storage.data(), &value, sizeof(T));

            _complete.write.data = _completion_write_storage.data();
            _complete.write.size = static_cast<ULONG>(_completion_write_storage.size());
            _complete.write.offset = 0;
        }

        void set_read_offset(const ULONG offset) noexcept
        {
            _read_offset = offset;
        }

        void set_write_offset(const ULONG offset) noexcept
        {
            _write_offset = offset;
        }

        [[nodiscard]] std::expected<std::span<std::byte>, DeviceCommError> get_input_buffer() noexcept
        {
            if (_comm == nullptr)
            {
                return std::unexpected(DeviceCommError{ .context = L"Message comm was null", .win32_error = ERROR_INVALID_STATE });
            }

            if (_input_buffer.data() != nullptr)
            {
                return std::span<std::byte>(_input_buffer.data(), _input_buffer.size());
            }

            if (_read_offset > _packet.descriptor.input_size)
            {
                return std::unexpected(DeviceCommError{ .context = L"Input read offset exceeds input size", .win32_error = ERROR_INVALID_DATA });
            }

            const ULONG remaining = _packet.descriptor.input_size - _read_offset;
            _input_storage.resize(remaining);
            _input_buffer = std::span<std::byte>(_input_storage.data(), _input_storage.size());

            if (remaining == 0)
            {
                return _input_buffer;
            }

            IoOperation op{};
            op.identifier = _packet.descriptor.identifier;
            op.buffer.offset = _read_offset;
            op.buffer.data = _input_buffer.data();
            op.buffer.size = remaining;

            if (auto result = _comm->read_input(op); !result)
            {
                return std::unexpected(result.error());
            }

            return _input_buffer;
        }

        [[nodiscard]] std::expected<std::span<std::byte>, DeviceCommError> get_output_buffer() noexcept
        {
            if (_comm == nullptr)
            {
                return std::unexpected(DeviceCommError{ .context = L"Message comm was null", .win32_error = ERROR_INVALID_STATE });
            }

            if (_output_buffer.data() != nullptr)
            {
                return std::span<std::byte>(_output_buffer.data(), _output_buffer.size());
            }

            if (_write_offset > _packet.descriptor.output_size)
            {
                return std::unexpected(DeviceCommError{ .context = L"Output write offset exceeds output size", .win32_error = ERROR_INVALID_DATA });
            }

            const ULONG remaining = _packet.descriptor.output_size - _write_offset;
            _output_storage.assign(remaining, std::byte{});
            _output_buffer = std::span<std::byte>(_output_storage.data(), _output_storage.size());
            return _output_buffer;
        }

        [[nodiscard]] std::expected<void, DeviceCommError> release_message_buffers() noexcept
        {
            if (_comm == nullptr)
            {
                return std::unexpected(DeviceCommError{ .context = L"Message comm was null", .win32_error = ERROR_INVALID_STATE });
            }

            _input_storage.clear();
            _input_buffer = {};

            if (_output_buffer.data() == nullptr)
            {
                return {};
            }

            if (nt_success(_complete.io_status.Status))
            {
                const ULONG_PTR info = _complete.io_status.Information;
                if (info > static_cast<ULONG_PTR>(_output_buffer.size()))
                {
                    return std::unexpected(DeviceCommError{
                        .context = L"Completion information exceeds output buffer size",
                        .win32_error = ERROR_INVALID_DATA,
                    });
                }

                IoOperation op{};
                op.identifier = _packet.descriptor.identifier;
                op.buffer.offset = _write_offset;
                op.buffer.data = _output_buffer.data();
                op.buffer.size = static_cast<ULONG>(info);

                if (auto result = _comm->write_output(op); !result)
                {
                    return result;
                }
            }

            _output_storage.clear();
            _output_buffer = {};
            return {};
        }

        [[nodiscard]] std::expected<void, DeviceCommError> complete_io() noexcept
        {
            if (_comm == nullptr)
            {
                return std::unexpected(DeviceCommError{ .context = L"Message comm was null", .win32_error = ERROR_INVALID_STATE });
            }

            return _comm->complete_io(_complete);
        }

    private:
        Comm* _comm{ nullptr };
        IoPacket _packet{};
        IoComplete _complete{};
        ULONG _read_offset{ 0 };
        ULONG _write_offset{ 0 };

        std::vector<std::byte> _input_storage;
        std::vector<std::byte> _output_storage;
        std::vector<std::byte> _completion_write_storage;
        std::span<std::byte> _input_buffer{};
        std::span<std::byte> _output_buffer{};
    };

    using ConDrvApiMessage = BasicApiMessage<ConDrvDeviceComm>;
}
