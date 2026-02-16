#pragma once

#include "condrv/condrv_protocol.hpp"
#include "core/handle_view.hpp"
#include "core/unique_handle.hpp"

#include <Windows.h>

#include <expected>
#include <string>

namespace oc::condrv
{
    struct IoPacket;

    struct DeviceCommError final
    {
        std::wstring context;
        DWORD win32_error{ ERROR_GEN_FAILURE };
    };

    class ConDrvDeviceComm final
    {
    public:
        // Duplicates the supplied handle so the comm object owns its lifetime.
        [[nodiscard]] static std::expected<ConDrvDeviceComm, DeviceCommError> from_server_handle(core::HandleView server_handle) noexcept;

        ConDrvDeviceComm() noexcept = default;
        ~ConDrvDeviceComm() noexcept = default;

        ConDrvDeviceComm(const ConDrvDeviceComm&) = delete;
        ConDrvDeviceComm& operator=(const ConDrvDeviceComm&) = delete;

        ConDrvDeviceComm(ConDrvDeviceComm&&) noexcept = default;
        ConDrvDeviceComm& operator=(ConDrvDeviceComm&&) noexcept = default;

        [[nodiscard]] core::HandleView server_handle() const noexcept;

        [[nodiscard]] std::expected<void, DeviceCommError> set_server_information(core::HandleView input_available_event) const noexcept;
        [[nodiscard]] std::expected<void, DeviceCommError> allow_ui_access() const noexcept;

        [[nodiscard]] std::expected<void, DeviceCommError> read_io(
            const IoComplete* reply,
            IoDescriptor& out_descriptor,
            void* out_packet,
            DWORD out_packet_size) const noexcept;

        [[nodiscard]] std::expected<void, DeviceCommError> read_io(
            const IoComplete* reply,
            IoPacket& out_packet) const noexcept;

        [[nodiscard]] std::expected<void, DeviceCommError> complete_io(const IoComplete& completion) const noexcept;
        [[nodiscard]] std::expected<void, DeviceCommError> read_input(IoOperation& operation) const noexcept;
        [[nodiscard]] std::expected<void, DeviceCommError> write_output(IoOperation& operation) const noexcept;

    private:
        explicit ConDrvDeviceComm(core::UniqueHandle server) noexcept;

        [[nodiscard]] std::expected<void, DeviceCommError> call_ioctl(
            DWORD ioctl,
            void* in_buffer,
            DWORD in_buffer_size,
            void* out_buffer,
            DWORD out_buffer_size) const noexcept;

        core::UniqueHandle _server;
    };
}
