#include "runtime/console_connection_policy.hpp"

#include <cstdio>

namespace
{
    [[nodiscard]] bool test_create_no_window_disables_window_and_handoff()
    {
        oc::runtime::ConsoleConnectionPolicyInput input{};
        input.console_app = true;
        input.window_visible = false;
        input.startup_flags = 0;
        input.show_window = SW_SHOWDEFAULT;

        const auto decision = oc::runtime::ConsoleConnectionPolicy::decide(
            input,
            false,
            false,
            false,
            false,
            true);

        return !decision.create_window &&
               decision.show_command == SW_SHOWDEFAULT &&
               !decision.attempt_default_terminal_handoff;
    }

    [[nodiscard]] bool test_showwindow_hide_suppresses_handoff()
    {
        oc::runtime::ConsoleConnectionPolicyInput input{};
        input.console_app = true;
        input.window_visible = true;
        input.startup_flags = STARTF_USESHOWWINDOW;
        input.show_window = SW_HIDE;

        const auto decision = oc::runtime::ConsoleConnectionPolicy::decide(
            input,
            false,
            false,
            false,
            false,
            true);

        return decision.create_window &&
               decision.show_command == SW_HIDE &&
               !decision.attempt_default_terminal_handoff;
    }

    [[nodiscard]] bool test_visible_interactive_allows_handoff()
    {
        oc::runtime::ConsoleConnectionPolicyInput input{};
        input.console_app = true;
        input.window_visible = true;
        input.startup_flags = 0;
        input.show_window = SW_SHOWDEFAULT;

        const auto decision = oc::runtime::ConsoleConnectionPolicy::decide(
            input,
            false,
            false,
            false,
            false,
            true);

        return decision.create_window &&
               decision.show_command == SW_SHOWDEFAULT &&
               decision.attempt_default_terminal_handoff;
    }

    [[nodiscard]] bool test_noninteractive_suppresses_window_and_handoff()
    {
        oc::runtime::ConsoleConnectionPolicyInput input{};
        input.console_app = true;
        input.window_visible = true;
        input.startup_flags = 0;
        input.show_window = SW_SHOWDEFAULT;

        const auto decision = oc::runtime::ConsoleConnectionPolicy::decide(
            input,
            false,
            false,
            false,
            false,
            false);

        return !decision.create_window &&
               decision.show_command == SW_SHOWDEFAULT &&
               !decision.attempt_default_terminal_handoff;
    }
}

bool run_console_connection_policy_tests()
{
    if (!test_create_no_window_disables_window_and_handoff())
    {
        fwprintf(stderr, L"[console connection policy] test_create_no_window_disables_window_and_handoff failed\n");
        return false;
    }
    if (!test_showwindow_hide_suppresses_handoff())
    {
        fwprintf(stderr, L"[console connection policy] test_showwindow_hide_suppresses_handoff failed\n");
        return false;
    }
    if (!test_visible_interactive_allows_handoff())
    {
        fwprintf(stderr, L"[console connection policy] test_visible_interactive_allows_handoff failed\n");
        return false;
    }
    if (!test_noninteractive_suppresses_window_and_handoff())
    {
        fwprintf(stderr, L"[console connection policy] test_noninteractive_suppresses_window_and_handoff failed\n");
        return false;
    }

    return true;
}

