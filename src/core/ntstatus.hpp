#pragma once

// Minimal NTSTATUS constants used by the reimplementation.
//
// We avoid including <ntstatus.h> directly because it interacts poorly with
// <Windows.h> unless WIN32_NO_STATUS is configured consistently across all
// translation units. The condrv protocol is internal and only needs a small
// set of status codes for incremental server-mode work.
//
// Values are defined by Windows and are stable.

#include <Windows.h>
#include <winternl.h>

namespace oc::core
{
    inline constexpr NTSTATUS status_success = static_cast<NTSTATUS>(0x00000000L);
    // Used by the inbox console host to indicate that a wait was interrupted
    // (e.g. Ctrl+C/Ctrl+Break terminating a cooked read).
    inline constexpr NTSTATUS status_alerted = static_cast<NTSTATUS>(0x00000101L);
    inline constexpr NTSTATUS status_unsuccessful = static_cast<NTSTATUS>(0xC0000001L);
    inline constexpr NTSTATUS status_not_implemented = static_cast<NTSTATUS>(0xC0000002L);
    inline constexpr NTSTATUS status_invalid_handle = static_cast<NTSTATUS>(0xC0000008L);
    inline constexpr NTSTATUS status_invalid_parameter = static_cast<NTSTATUS>(0xC000000DL);
    inline constexpr NTSTATUS status_no_memory = static_cast<NTSTATUS>(0xC0000017L);
    inline constexpr NTSTATUS status_buffer_too_small = static_cast<NTSTATUS>(0xC0000023L);
}
