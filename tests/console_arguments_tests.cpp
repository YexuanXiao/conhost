#include "cli/console_arguments.hpp"

#include "core/unique_local.hpp"

#include <Windows.h>
#include <shellapi.h>

#include <cstdio>
#include <string>
#include <vector>

namespace
{
    [[nodiscard]] std::vector<std::wstring> split_command_line(const std::wstring_view command_line)
    {
        std::wstring mutable_command_line(command_line);
        int argc = 0;
        oc::core::UniqueLocalPtr argv(::CommandLineToArgvW(mutable_command_line.c_str(), &argc));
        if (argv.get() == nullptr || argc < 0)
        {
            return {};
        }

        std::vector<std::wstring> args;
        args.reserve(static_cast<size_t>(argc));
        auto** values = argv.as<wchar_t*>();
        for (int i = 0; i < argc; ++i)
        {
            args.emplace_back(values[i]);
        }
        return args;
    }

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

    bool test_explicit_client_commandline_roundtrips_tokens()
    {
        auto parsed = oc::cli::ConsoleArguments::parse(
            L"openconsole.exe -- cmd /c \"echo hello\"",
            oc::core::HandleView{},
            oc::core::HandleView{});
        if (!parsed)
        {
            return false;
        }

        const auto argv = split_command_line(parsed->client_command_line());
        if (argv.size() != 3)
        {
            return false;
        }

        return argv[0] == L"cmd" && argv[1] == L"/c" && argv[2] == L"echo hello";
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

    bool test_unknown_token_stops_parsing_host_flags()
    {
        auto parsed = oc::cli::ConsoleArguments::parse(
            L"openconsole.exe cmd --headless --vtmode --width 100",
            oc::core::HandleView{},
            oc::core::HandleView{});
        if (!parsed)
        {
            return false;
        }

        return parsed->client_command_line() == L"cmd --headless --vtmode --width 100" &&
               !parsed->is_headless() &&
               !parsed->vt_mode_requested() &&
               parsed->width() == 0;
    }

    bool test_double_dash_forces_client_payload()
    {
        auto parsed = oc::cli::ConsoleArguments::parse(
            L"openconsole.exe -- --headless --vtmode",
            oc::core::HandleView{},
            oc::core::HandleView{});
        if (!parsed)
        {
            return false;
        }

        return parsed->client_command_line() == L"--headless --vtmode" &&
               !parsed->is_headless() &&
               !parsed->vt_mode_requested();
    }

    bool test_explicit_client_commandline_quotes_space_token()
    {
        auto parsed = oc::cli::ConsoleArguments::parse(
            L"openconsole.exe -- \"a b\"",
            oc::core::HandleView{},
            oc::core::HandleView{});
        if (!parsed)
        {
            return false;
        }

        if (parsed->client_command_line() != L"\"a b\"")
        {
            return false;
        }

        const auto argv = split_command_line(parsed->client_command_line());
        return argv.size() == 1 && argv[0] == L"a b";
    }

    bool test_explicit_client_commandline_quotes_trailing_backslash()
    {
        auto parsed = oc::cli::ConsoleArguments::parse(
            L"openconsole.exe -- cmd \"C:\\Program Files\\\\\"",
            oc::core::HandleView{},
            oc::core::HandleView{});
        if (!parsed)
        {
            return false;
        }

        const std::wstring_view expected = L"cmd \"C:\\Program Files\\\\\"";
        if (parsed->client_command_line() != expected)
        {
            fwprintf(stderr, L"[DETAIL] trailing-backslash escape mismatch: expected=%ls actual=%ls\n",
                     expected.data(),
                     parsed->client_command_line().c_str());
            return false;
        }

        const auto argv = split_command_line(parsed->client_command_line());
        if (argv.size() != 2)
        {
            fwprintf(stderr, L"[DETAIL] trailing-backslash tokenization produced %zu argv entries\n", argv.size());
            for (size_t i = 0; i < argv.size(); ++i)
            {
                fwprintf(stderr, L"[DETAIL] argv[%zu]=%ls\n", i, argv[i].c_str());
            }
            return false;
        }

        if (argv[0] != L"cmd" || argv[1] != L"C:\\Program Files\\")
        {
            fwprintf(stderr, L"[DETAIL] trailing-backslash token mismatch: argv[0]=%ls argv[1]=%ls\n",
                     argv[0].c_str(),
                     argv[1].c_str());
            return false;
        }

        return true;
    }

    bool test_missing_server_handle_value_fails()
    {
        auto parsed = oc::cli::ConsoleArguments::parse(
            L"openconsole.exe --server",
            oc::core::HandleView{},
            oc::core::HandleView{});
        return !parsed.has_value();
    }

    bool test_zero_server_handle_value_fails()
    {
        auto parsed = oc::cli::ConsoleArguments::parse(
            L"openconsole.exe 0x0",
            oc::core::HandleView{},
            oc::core::HandleView{});
        return !parsed.has_value();
    }

    bool test_explicit_client_allows_0x_token()
    {
        auto parsed = oc::cli::ConsoleArguments::parse(
            L"openconsole.exe -- 0x123",
            oc::core::HandleView{},
            oc::core::HandleView{});
        if (!parsed)
        {
            return false;
        }

        return parsed->should_create_server_handle() && parsed->client_command_line() == L"0x123";
    }

    bool test_explicit_client_allows_filepath_leader_token()
    {
        auto parsed = oc::cli::ConsoleArguments::parse(
            L"openconsole.exe -- \\??\\C:\\foo",
            oc::core::HandleView{},
            oc::core::HandleView{});
        if (!parsed)
        {
            return false;
        }

        return parsed->client_command_line() == L"\\??\\C:\\foo";
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

    bool test_delegated_window_flag_parses_before_embedding()
    {
        auto parsed = oc::cli::ConsoleArguments::parse(
            L"openconsole.exe --delegated-window /Embedding",
            oc::core::HandleView{},
            oc::core::HandleView{});
        if (!parsed)
        {
            return false;
        }

        return parsed->should_run_as_com_server() &&
               parsed->delegated_window_requested() &&
               parsed->client_command_line().empty();
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
        else
        {
            // 32-bit builds can't represent a >32-bit handle value.
            return value != expected;
        }
    }
}

bool run_console_arguments_tests()
{
    if (!test_explicit_client_commandline())
    {
        fwprintf(stderr, L"[DETAIL] test_explicit_client_commandline failed\n");
        return false;
    }
    if (!test_explicit_client_commandline_roundtrips_tokens())
    {
        fwprintf(stderr, L"[DETAIL] test_explicit_client_commandline_roundtrips_tokens failed\n");
        return false;
    }
    if (!test_unknown_token_starts_client_commandline())
    {
        fwprintf(stderr, L"[DETAIL] test_unknown_token_starts_client_commandline failed\n");
        return false;
    }
    if (!test_unknown_token_stops_parsing_host_flags())
    {
        fwprintf(stderr, L"[DETAIL] test_unknown_token_stops_parsing_host_flags failed\n");
        return false;
    }
    if (!test_double_dash_forces_client_payload())
    {
        fwprintf(stderr, L"[DETAIL] test_double_dash_forces_client_payload failed\n");
        return false;
    }
    if (!test_explicit_client_commandline_quotes_space_token())
    {
        fwprintf(stderr, L"[DETAIL] test_explicit_client_commandline_quotes_space_token failed\n");
        return false;
    }
    if (!test_explicit_client_commandline_quotes_trailing_backslash())
    {
        fwprintf(stderr, L"[DETAIL] test_explicit_client_commandline_quotes_trailing_backslash failed\n");
        return false;
    }
    if (!test_compatibility_flags())
    {
        fwprintf(stderr, L"[DETAIL] test_compatibility_flags failed\n");
        return false;
    }
    if (!test_invalid_feature_fails())
    {
        fwprintf(stderr, L"[DETAIL] test_invalid_feature_fails failed\n");
        return false;
    }
    if (!test_force_no_handoff_flag())
    {
        fwprintf(stderr, L"[DETAIL] test_force_no_handoff_flag failed\n");
        return false;
    }
    if (!test_delegated_window_flag_parses_before_embedding())
    {
        fwprintf(stderr, L"[DETAIL] test_delegated_window_flag_parses_before_embedding failed\n");
        return false;
    }
    if (!test_missing_server_handle_value_fails())
    {
        fwprintf(stderr, L"[DETAIL] test_missing_server_handle_value_fails failed\n");
        return false;
    }
    if (!test_duplicate_server_handle_fails())
    {
        fwprintf(stderr, L"[DETAIL] test_duplicate_server_handle_fails failed\n");
        return false;
    }
    if (!test_zero_server_handle_value_fails())
    {
        fwprintf(stderr, L"[DETAIL] test_zero_server_handle_value_fails failed\n");
        return false;
    }
    if (!test_bad_width_fails())
    {
        fwprintf(stderr, L"[DETAIL] test_bad_width_fails failed\n");
        return false;
    }
    if (!test_explicit_client_allows_0x_token())
    {
        fwprintf(stderr, L"[DETAIL] test_explicit_client_allows_0x_token failed\n");
        return false;
    }
    if (!test_explicit_client_allows_filepath_leader_token())
    {
        fwprintf(stderr, L"[DETAIL] test_explicit_client_allows_filepath_leader_token failed\n");
        return false;
    }
    if (!test_pointer_width_handle_parsing())
    {
        fwprintf(stderr, L"[DETAIL] test_pointer_width_handle_parsing failed\n");
        return false;
    }

    return true;
}
