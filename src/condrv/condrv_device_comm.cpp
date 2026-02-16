#include "condrv/condrv_device_comm.hpp"

#include "condrv/condrv_packet.hpp"
#include "core/assert.hpp"
#include "core/win32_handle.hpp"

namespace oc::condrv
{
    namespace
    {
        [[nodiscard]] DeviceCommError make_error(std::wstring context, const DWORD win32_error) noexcept
        {
            return DeviceCommError{
                .context = std::move(context),
                .win32_error = win32_error == 0 ? ERROR_GEN_FAILURE : win32_error,
            };
        }

        [[nodiscard]] std::expected<core::UniqueHandle, DeviceCommError> duplicate_into_self(const core::HandleView handle) noexcept
        {
            if (!handle)
            {
                return std::unexpected(make_error(L"Invalid server handle", ERROR_INVALID_HANDLE));
            }

            auto duplicated = core::duplicate_handle_same_access(handle, false);
            if (!duplicated)
            {
                return std::unexpected(make_error(L"DuplicateHandle failed for server handle", duplicated.error()));
            }

            return std::move(duplicated.value());
        }
    }

    std::expected<ConDrvDeviceComm, DeviceCommError> ConDrvDeviceComm::from_server_handle(const core::HandleView server_handle) noexcept
    {
        auto duplicated = duplicate_into_self(server_handle);
        if (!duplicated)
        {
            return std::unexpected(duplicated.error());
        }

        return ConDrvDeviceComm(std::move(duplicated.value()));
    }

    ConDrvDeviceComm::ConDrvDeviceComm(core::UniqueHandle server) noexcept :
        _server(std::move(server))
    {
        OC_ASSERT(_server.valid());
    }

    core::HandleView ConDrvDeviceComm::server_handle() const noexcept
    {
        return core::HandleView(_server.get());
    }

    std::expected<void, DeviceCommError> ConDrvDeviceComm::call_ioctl(
        const DWORD ioctl,
        void* const in_buffer,
        const DWORD in_buffer_size,
        void* const out_buffer,
        const DWORD out_buffer_size) const noexcept
    {
        DWORD written = 0;
        if (::DeviceIoControl(
                _server.get(),
                ioctl,
                in_buffer,
                in_buffer_size,
                out_buffer,
                out_buffer_size,
                &written,
                nullptr) == FALSE)
        {
            return std::unexpected(make_error(L"DeviceIoControl failed", ::GetLastError()));
        }

        return {};
    }

    std::expected<void, DeviceCommError> ConDrvDeviceComm::set_server_information(const core::HandleView input_available_event) const noexcept
    {
        IoServerInformation info{};
        info.input_available_event = input_available_event;
        return call_ioctl(
            ioctl_set_server_information,
            &info,
            sizeof(info),
            nullptr,
            0);
    }

    std::expected<void, DeviceCommError> ConDrvDeviceComm::allow_ui_access() const noexcept
    {
        return call_ioctl(
            ioctl_allow_via_uiaccess,
            nullptr,
            0,
            nullptr,
            0);
    }

    std::expected<void, DeviceCommError> ConDrvDeviceComm::read_io(
        const IoComplete* const reply,
        IoDescriptor& out_descriptor,
        void* const out_packet,
        const DWORD out_packet_size) const noexcept
    {
        if (out_packet == nullptr || out_packet_size < sizeof(IoDescriptor))
        {
            return std::unexpected(make_error(L"Invalid output buffer for read_io", ERROR_INVALID_PARAMETER));
        }

        const void* completion = nullptr;
        DWORD completion_size = 0;
        if (reply != nullptr)
        {
            completion = reply;
            completion_size = sizeof(*reply);
        }

        auto result = call_ioctl(
            ioctl_read_io,
            const_cast<void*>(completion),
            completion_size,
            out_packet,
            out_packet_size);
        if (!result)
        {
            return result;
        }

        out_descriptor = *static_cast<const IoDescriptor*>(out_packet);
        return {};
    }

    std::expected<void, DeviceCommError> ConDrvDeviceComm::read_io(
        const IoComplete* const reply,
        IoPacket& out_packet) const noexcept
    {
        IoDescriptor descriptor{};
        auto result = read_io(
            reply,
            descriptor,
            &out_packet,
            static_cast<DWORD>(sizeof(out_packet)));
        if (!result)
        {
            return result;
        }

        return {};
    }

    std::expected<void, DeviceCommError> ConDrvDeviceComm::complete_io(const IoComplete& completion) const noexcept
    {
        return call_ioctl(
            ioctl_complete_io,
            const_cast<IoComplete*>(&completion),
            sizeof(completion),
            nullptr,
            0);
    }

    std::expected<void, DeviceCommError> ConDrvDeviceComm::read_input(IoOperation& operation) const noexcept
    {
        return call_ioctl(
            ioctl_read_input,
            &operation,
            sizeof(operation),
            nullptr,
            0);
    }

    std::expected<void, DeviceCommError> ConDrvDeviceComm::write_output(IoOperation& operation) const noexcept
    {
        return call_ioctl(
            ioctl_write_output,
            &operation,
            sizeof(operation),
            nullptr,
            0);
    }
}
