#pragma once

// This header defines a minimal subset of the ConDrv protocol surface that
// conhost/openconsole uses to communicate with the console driver.
//
// The full upstream implementation includes significantly more message bodies
// and dispatch logic. This module intentionally starts small and is expanded
// incrementally as additional server/runtime functionality is ported.
//
// Notes:
// - These definitions are based on the structures used by the inbox console
//   host and the driver. We keep them POD and layout-stable.
// - This is not part of the public Win32 API; it is an internal protocol.

#include "core/handle_view.hpp"

#include <winioctl.h>
#include <winternl.h>

namespace oc::condrv
{
    // CD_IO_DESCRIPTOR::Function values.
    // The driver sends one of these verbs to the server in ReadIo.
    inline constexpr ULONG console_io_connect = 0x01;
    inline constexpr ULONG console_io_disconnect = 0x02;
    inline constexpr ULONG console_io_create_object = 0x03;
    inline constexpr ULONG console_io_close_object = 0x04;
    inline constexpr ULONG console_io_raw_write = 0x05;
    inline constexpr ULONG console_io_raw_read = 0x06;
    inline constexpr ULONG console_io_user_defined = 0x07;
    inline constexpr ULONG console_io_raw_flush = 0x08;

    // CREATE_OBJECT_INFORMATION::ObjectType values.
    inline constexpr ULONG io_object_type_current_input = 0x01;
    inline constexpr ULONG io_object_type_current_output = 0x02;
    inline constexpr ULONG io_object_type_new_output = 0x03;
    inline constexpr ULONG io_object_type_generic = 0x04;

    struct IoDescriptor final
    {
        LUID identifier{};
        ULONG_PTR process{};
        ULONG_PTR object{};
        ULONG function{};
        ULONG input_size{};
        ULONG output_size{};
        ULONG reserved{};
    };

    struct CreateObjectInformation final
    {
        ULONG object_type{};
        ULONG share_mode{};
        ACCESS_MASK desired_access{};
    };

    struct ConnectionInformation final
    {
        ULONG_PTR process{};
        ULONG_PTR input{};
        ULONG_PTR output{};
    };

    struct IoBufferDescriptor final
    {
        void* data{};
        ULONG size{};
        ULONG offset{};
    };

    struct IoComplete final
    {
        LUID identifier{};
        IO_STATUS_BLOCK io_status{};
        IoBufferDescriptor write{};
    };

    struct IoOperation final
    {
        LUID identifier{};
        IoBufferDescriptor buffer{};
    };

    struct IoServerInformation final
    {
        core::HandleView input_available_event{};
    };

    // ConDrv IOCTL surface used by the server.
    // Keep these as macros/constexpr values compatible with DeviceIoControl.
    inline constexpr DWORD ioctl_read_io =
        CTL_CODE(FILE_DEVICE_CONSOLE, 1, METHOD_OUT_DIRECT, FILE_ANY_ACCESS);
    inline constexpr DWORD ioctl_complete_io =
        CTL_CODE(FILE_DEVICE_CONSOLE, 2, METHOD_NEITHER, FILE_ANY_ACCESS);
    inline constexpr DWORD ioctl_read_input =
        CTL_CODE(FILE_DEVICE_CONSOLE, 3, METHOD_NEITHER, FILE_ANY_ACCESS);
    inline constexpr DWORD ioctl_write_output =
        CTL_CODE(FILE_DEVICE_CONSOLE, 4, METHOD_NEITHER, FILE_ANY_ACCESS);
    inline constexpr DWORD ioctl_set_server_information =
        CTL_CODE(FILE_DEVICE_CONSOLE, 7, METHOD_NEITHER, FILE_ANY_ACCESS);
    inline constexpr DWORD ioctl_allow_via_uiaccess =
        CTL_CODE(FILE_DEVICE_CONSOLE, 12, METHOD_NEITHER, FILE_ANY_ACCESS);
}
