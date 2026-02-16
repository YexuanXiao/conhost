#include "cli/console_arguments.hpp"

namespace
{
    bool test_explicit_client_commandline()
    {
        auto parsed = oc::cli::ConsoleArguments::parse(
            L"openconsole.exe -- cmd /c \"echo hello\"",
            oc::core::HandleView{},
            oc::core::HandleView{});
        if (!parsed)
        {
            return false;
        }

        return parsed->client_command_line() == L"cmd /c \"echo hello\"";
    }

    bool test_unknown_token_starts_client_commandline()
    {
        auto parsed = oc::cli::ConsoleArguments::parse(
            L"openconsole.exe powershell -NoLogo",
            oc::core::HandleView{},
            oc::core::HandleView{});
        if (!parsed)
        {
            return false;
        }

        return parsed->client_command_line() == L"powershell -NoLogo";
    }

    bool test_compatibility_flags()
    {
        auto parsed = oc::cli::ConsoleArguments::parse(
            L"openconsole.exe --server 0x4 --signal 0x8 --width 120 --height 40 --headless --vtmode --inheritcursor --textMeasurement grapheme --feature pty",
            oc::core::HandleView::from_uintptr(static_cast<std::uintptr_t>(0x11)),
            oc::core::HandleView::from_uintptr(static_cast<std::uintptr_t>(0x12)));
        if (!parsed)
        {
            return false;
        }

        return !parsed->should_create_server_handle() &&
               parsed->has_signal_handle() &&
               parsed->width() == 120 &&
               parsed->height() == 40 &&
               parsed->is_headless() &&
               parsed->vt_mode_requested() &&
               !parsed->force_no_handoff() &&
               parsed->inherit_cursor() &&
               parsed->text_measurement() == L"grapheme";
    }

    bool test_force_no_handoff_flag()
    {
        auto parsed = oc::cli::ConsoleArguments::parse(
            L"openconsole.exe -ForceNoHandoff -- cmd /c echo ok",
            oc::core::HandleView{},
            oc::core::HandleView{});
        if (!parsed)
        {
            return false;
        }
        return parsed->force_no_handoff();
    }

    bool test_invalid_feature_fails()
    {
        auto parsed = oc::cli::ConsoleArguments::parse(
            L"openconsole.exe --feature unknown",
            oc::core::HandleView{},
            oc::core::HandleView{});
        return !parsed.has_value();
    }

    bool test_duplicate_server_handle_fails()
    {
        auto parsed = oc::cli::ConsoleArguments::parse(
            L"openconsole.exe --server 0x4 0x8",
            oc::core::HandleView{},
            oc::core::HandleView{});
        return !parsed.has_value();
    }

    bool test_bad_width_fails()
    {
        auto parsed = oc::cli::ConsoleArguments::parse(
            L"openconsole.exe --width abc",
            oc::core::HandleView{},
            oc::core::HandleView{});
        return !parsed.has_value();
    }

    bool test_pointer_width_handle_parsing()
    {
        // Conhost passes handles as hex pointer-sized values. We need to accept
        // values larger than 32 bits on 64-bit builds.
        constexpr unsigned long long expected = 0x123456789ULL;

        auto parsed = oc::cli::ConsoleArguments::parse(
            L"openconsole.exe --server 0x123456789",
            oc::core::HandleView{},
            oc::core::HandleView{});
        if (!parsed)
        {
            return false;
        }

        const auto value = static_cast<unsigned long long>(parsed->server_handle().as_uintptr());

        if constexpr (sizeof(void*) == 8)
        {
            return value == expected;
        }

        // 32-bit builds can't represent a >32-bit handle value.
        return value != expected;
    }
}

bool run_console_arguments_tests()
{
    return test_explicit_client_commandline() &&
           test_unknown_token_starts_client_commandline() &&
           test_compatibility_flags() &&
           test_invalid_feature_fails() &&
           test_force_no_handoff_flag() &&
           test_duplicate_server_handle_fails() &&
           test_bad_width_fails() &&
           test_pointer_width_handle_parsing();
}
