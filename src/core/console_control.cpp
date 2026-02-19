#include "core/console_control.hpp"

#include <cstdint>

namespace oc::core
{
    namespace
    {
        // Numeric values match the inbox conhost implementation.
        enum class ConsoleControlCommand : DWORD
        {
            notify_console_application = 1,
            set_window_owner = 6,
            end_task = 7,
        };

        inline constexpr DWORD cpi_newprocesswindow = 0x0001u;

        struct ConsoleProcessInfo final
        {
            DWORD process_id{};
            DWORD flags{};
        };

        struct ConsoleEndTask final
        {
            HANDLE process_id{}; // Actually a PID, but the inbox struct uses HANDLE.
            HWND hwnd{};
            ULONG console_event_code{};
            ULONG console_flags{};
        };

        struct ConsoleWindowOwner final
        {
            HWND hwnd{};
            ULONG process_id{};
            ULONG thread_id{};
        };

        using RtlNtStatusToDosErrorFn = ULONG(WINAPI*)(NTSTATUS status);

        [[nodiscard]] DWORD ntstatus_to_win32_error(
            const NTSTATUS status,
            const RtlNtStatusToDosErrorFn converter) noexcept
        {
            if (converter == nullptr)
            {
                return ERROR_GEN_FAILURE;
            }

            const DWORD error = static_cast<DWORD>(converter(status));
            return error == 0 ? ERROR_GEN_FAILURE : error;
        }
    }

    ConsoleControl ConsoleControl::resolve() noexcept
    {
        ConsoleControl control{};

        HMODULE user32 = ::GetModuleHandleW(L"user32.dll");
        if (user32 == nullptr)
        {
            user32 = ::LoadLibraryExW(L"user32.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
        }

        if (user32 != nullptr)
        {
            control._console_control = reinterpret_cast<ConsoleControlFn>(::GetProcAddress(user32, "ConsoleControl"));
        }

        const HMODULE ntdll = ::GetModuleHandleW(L"ntdll.dll");
        if (ntdll != nullptr)
        {
            control._rtl_nt_status_to_dos_error =
                reinterpret_cast<RtlNtStatusToDosErrorFn>(::GetProcAddress(ntdll, "RtlNtStatusToDosError"));
        }

        return control;
    }

    bool ConsoleControl::available() const noexcept
    {
        return _console_control != nullptr;
    }

    std::expected<void, DWORD> ConsoleControl::notify_console_application(
        const DWORD process_id,
        NTSTATUS* const out_status) const noexcept
    {
        if (process_id == 0)
        {
            return {};
        }

        if (_console_control == nullptr)
        {
            return std::unexpected(ERROR_PROC_NOT_FOUND);
        }

        ConsoleProcessInfo info{};
        info.process_id = process_id;
        info.flags = cpi_newprocesswindow;

        const NTSTATUS status = _console_control(
            static_cast<DWORD>(ConsoleControlCommand::notify_console_application),
            &info,
            static_cast<DWORD>(sizeof(info)));
        if (out_status != nullptr)
        {
            *out_status = status;
        }

        if (status < 0)
        {
            return std::unexpected(ntstatus_to_win32_error(status, _rtl_nt_status_to_dos_error));
        }

        return {};
    }

    std::expected<void, DWORD> ConsoleControl::end_task(
        const DWORD process_id,
        const DWORD event_type,
        const DWORD ctrl_flags,
        const HWND hwnd,
        NTSTATUS* const out_status) const noexcept
    {
        if (process_id == 0)
        {
            return {};
        }

        if (_console_control == nullptr)
        {
            return std::unexpected(ERROR_PROC_NOT_FOUND);
        }

        ConsoleEndTask params{};
        params.process_id = reinterpret_cast<HANDLE>(static_cast<ULONG_PTR>(process_id));
        params.hwnd = hwnd;
        params.console_event_code = event_type;
        params.console_flags = ctrl_flags;

        const NTSTATUS status = _console_control(
            static_cast<DWORD>(ConsoleControlCommand::end_task),
            &params,
            static_cast<DWORD>(sizeof(params)));
        if (out_status != nullptr)
        {
            *out_status = status;
        }

        if (status < 0)
        {
            return std::unexpected(ntstatus_to_win32_error(status, _rtl_nt_status_to_dos_error));
        }

        return {};
    }

    std::expected<void, DWORD> ConsoleControl::set_window_owner(
        const HWND hwnd,
        const DWORD process_id,
        const DWORD thread_id,
        NTSTATUS* const out_status) const noexcept
    {
        if (hwnd == nullptr)
        {
            return std::unexpected(ERROR_INVALID_WINDOW_HANDLE);
        }

        if (_console_control == nullptr)
        {
            return std::unexpected(ERROR_PROC_NOT_FOUND);
        }

        ConsoleWindowOwner owner{};
        owner.hwnd = hwnd;
        owner.process_id = process_id;
        owner.thread_id = thread_id;

        const NTSTATUS status = _console_control(
            static_cast<DWORD>(ConsoleControlCommand::set_window_owner),
            &owner,
            static_cast<DWORD>(sizeof(owner)));
        if (out_status != nullptr)
        {
            *out_status = status;
        }

        if (status < 0)
        {
            return std::unexpected(ntstatus_to_win32_error(status, _rtl_nt_status_to_dos_error));
        }

        return {};
    }
}
