#pragma once

// Console connection policy for classic `--server` startups.
//
// Upstream conhost uses the CONNECT payload (`CONSOLE_SERVER_MSG`) to decide:
// - whether the session deserves a visible window, and
// - whether to attempt default-terminal delegation ("defterm") via
//   `IConsoleHandoff::EstablishHandoff`.
//
// In particular:
// - `CreateProcessW(..., CREATE_NO_WINDOW, ...)` results in `WindowVisible==FALSE`
//   and must not attempt defterm delegation (no UI should appear).
// - Explicit `STARTF_USESHOWWINDOW` requests (e.g. `SW_HIDE`, minimize variants)
//   must also suppress defterm delegation.
//
// The replacement keeps this as a small deterministic module so the behavior
// can be tested without depending on ConDrv, COM, or GUI creation.
//
// Upstream reference: `src/server/IoDispatchers.cpp::_shouldAttemptHandoff`.

#include <Windows.h>

namespace oc::runtime
{
    struct ConsoleConnectionPolicyInput final
    {
        bool console_app{ true };
        bool window_visible{ true };
        DWORD startup_flags{ 0 };
        USHORT show_window{ SW_SHOWDEFAULT };
    };

    struct ConsoleConnectionPolicyDecision final
    {
        bool create_window{ true };
        int show_command{ SW_SHOWDEFAULT };
        bool attempt_default_terminal_handoff{ false };
    };

    class ConsoleConnectionPolicy final
    {
    public:
        [[nodiscard]] static bool is_hidden_or_minimized_show_command(const int show_command) noexcept
        {
            switch (show_command)
            {
            case SW_HIDE:
            case SW_SHOWMINIMIZED:
            case SW_MINIMIZE:
            case SW_SHOWMINNOACTIVE:
            case SW_FORCEMINIMIZE:
                return true;
            default:
                return false;
            }
        }

        [[nodiscard]] static ConsoleConnectionPolicyDecision decide(
            const ConsoleConnectionPolicyInput& connect,
            const bool force_no_handoff,
            const bool create_server_handle,
            const bool headless,
            const bool in_conpty_mode,
            const bool interactive_user_session) noexcept
        {
            ConsoleConnectionPolicyDecision decision{};

            decision.show_command = SW_SHOWDEFAULT;
            if ((connect.startup_flags & STARTF_USESHOWWINDOW) != 0)
            {
                decision.show_command = static_cast<int>(connect.show_window);
            }

            // If the session is not interactive (services, invisible window station),
            // we avoid creating a classic window and suppress defterm delegation.
            decision.create_window = connect.window_visible && interactive_user_session;

            // Default-terminal delegation is only appropriate for interactive, visible
            // console app startups that are not explicitly suppressed by the user or
            // by startup mode (ConPTY/headless).
            decision.attempt_default_terminal_handoff =
                !force_no_handoff &&
                !create_server_handle &&
                !headless &&
                !in_conpty_mode &&
                interactive_user_session &&
                connect.console_app &&
                connect.window_visible &&
                !((connect.startup_flags & STARTF_USESHOWWINDOW) != 0 && is_hidden_or_minimized_show_command(decision.show_command));

            return decision;
        }
    };
}

