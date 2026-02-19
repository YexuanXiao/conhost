#pragma once

// Minimal wrapper around the private `user32!ConsoleControl` API used by the
// inbox console host to dispatch control events (for example Ctrl+C/Ctrl+Break)
// to console-attached processes.
//
// This is not a public Win32 API contract. The numeric command IDs and payload
// layout must match the inbox implementation to be effective.

#include "core/ntstatus.hpp"

#include <Windows.h>

#include <expected>

namespace oc::core
{
    class ConsoleControl final
    {
    public:
        ConsoleControl() noexcept = default;

        [[nodiscard]] static ConsoleControl resolve() noexcept;

        [[nodiscard]] bool available() const noexcept;

        [[nodiscard]] std::expected<void, DWORD> notify_console_application(
            DWORD process_id,
            NTSTATUS* out_status = nullptr) const noexcept;

        [[nodiscard]] std::expected<void, DWORD> set_window_owner(
            HWND hwnd,
            DWORD process_id,
            DWORD thread_id,
            NTSTATUS* out_status = nullptr) const noexcept;

        [[nodiscard]] std::expected<void, DWORD> end_task(
            DWORD process_id,
            DWORD event_type,
            DWORD ctrl_flags,
            HWND hwnd = nullptr,
            NTSTATUS* out_status = nullptr) const noexcept;

    private:
        using ConsoleControlFn = NTSTATUS(WINAPI*)(DWORD command, void* information, DWORD length);
        using RtlNtStatusToDosErrorFn = ULONG(WINAPI*)(NTSTATUS status);

        ConsoleControlFn _console_control{};
        RtlNtStatusToDosErrorFn _rtl_nt_status_to_dos_error{};
    };
}

