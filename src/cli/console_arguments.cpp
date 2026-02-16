#include "cli/console_arguments.hpp"

#include "core/assert.hpp"
#include "core/unique_local.hpp"
#include "serialization/fast_number.hpp"

#include <shellapi.h>

#include <climits>
#include <limits>
#include <string>
#include <vector>

namespace oc::cli
{
    namespace
    {
        constexpr std::wstring_view kSpace{ L" " };

        [[nodiscard]] bool starts_with(const std::wstring_view text, const std::wstring_view prefix) noexcept
        {
            return text.size() >= prefix.size() && text.substr(0, prefix.size()) == prefix;
        }
    }

    ConsoleArguments::ConsoleArguments(std::wstring command_line, const core::HandleView std_in, const core::HandleView std_out) :
        _command_line(std::move(command_line)),
        _vt_in_handle(std_in),
        _vt_out_handle(std_out)
    {
    }

    std::expected<ConsoleArguments, ParseError> ConsoleArguments::parse(const std::wstring_view command_line, const core::HandleView std_in, const core::HandleView std_out) noexcept
    {
        // Compatibility contract:
        // 1) parse with CommandLineToArgvW
        // 2) skip argv[0]
        // 3) consume known host/runtime switches
        // 4) treat first unknown token as start of client command line
        ConsoleArguments result(std::wstring{ command_line }, std_in, std_out);
        if (result._command_line.empty())
        {
            return result;
        }

        std::wstring mutable_command_line = result._command_line;
        int argc = 0;
        core::UniqueLocalPtr argv(::CommandLineToArgvW(mutable_command_line.c_str(), &argc));
        if (argv.get() == nullptr)
        {
            return std::unexpected(ParseError{ .message = L"CommandLineToArgvW failed" });
        }

        std::vector<std::wstring> args;
        args.reserve(argc > 0 ? static_cast<size_t>(argc) - 1 : 0);

        auto** argv_values = argv.as<wchar_t*>();
        for (int index = 1; index < argc; ++index)
        {
            args.emplace_back(argv_values[index]);
        }

        if (auto parse_result = result.parse_tokens(args); !parse_result)
        {
            return std::unexpected(parse_result.error());
        }

        if (!args.empty())
        {
            return std::unexpected(ParseError{ .message = L"Unexpected tokens remaining after parse" });
        }

        return result;
    }

    std::expected<void, ParseError> ConsoleArguments::parse_tokens(std::vector<std::wstring>& args) noexcept
    {
        for (size_t index = 0; index < args.size();)
        {
            const std::wstring arg = args[index];

            if (starts_with(arg, handle_prefix) || arg == server_handle_arg)
            {
                std::wstring server_handle_value;
                if (arg == server_handle_arg)
                {
                    auto parsed = get_string_argument(args, index);
                    if (!parsed)
                    {
                        return std::unexpected(parsed.error());
                    }
                    server_handle_value = std::move(parsed.value());
                }
                else
                {
                    server_handle_value = arg;
                    consume_arg(args, index);
                }

                if (auto parsed_handle = parse_handle_arg(server_handle_value, _server_handle); !parsed_handle)
                {
                    return std::unexpected(parsed_handle.error());
                }

                _create_server_handle = false;
                continue;
            }

            if (arg == signal_handle_arg)
            {
                auto parsed = get_string_argument(args, index);
                if (!parsed)
                {
                    return std::unexpected(parsed.error());
                }

                if (auto parsed_handle = parse_handle_arg(parsed.value(), _signal_handle); !parsed_handle)
                {
                    return std::unexpected(parsed_handle.error());
                }

                continue;
            }

            if (arg == force_v1_arg)
            {
                _force_v1 = true;
                consume_arg(args, index);
                continue;
            }

            if (arg == force_no_handoff_arg)
            {
                _force_no_handoff = true;
                consume_arg(args, index);
                continue;
            }

            if (arg == com_server_arg)
            {
                _run_as_com_server = true;
                consume_arg(args, index);
                continue;
            }

            if (starts_with(arg, filepath_leader_prefix))
            {
                consume_arg(args, index);
                continue;
            }

            if (arg == width_arg)
            {
                auto parsed = get_short_argument(args, index);
                if (!parsed)
                {
                    return std::unexpected(parsed.error());
                }
                _width = parsed.value();
                continue;
            }

            if (arg == height_arg)
            {
                auto parsed = get_short_argument(args, index);
                if (!parsed)
                {
                    return std::unexpected(parsed.error());
                }
                _height = parsed.value();
                continue;
            }

            if (arg == feature_arg)
            {
                if (auto feature_result = handle_feature_value(args, index); !feature_result)
                {
                    return std::unexpected(feature_result.error());
                }
                continue;
            }

            if (arg == headless_arg)
            {
                _headless = true;
                consume_arg(args, index);
                continue;
            }

            if (arg == vt_mode_arg)
            {
                _vt_mode_requested = true;
                consume_arg(args, index);
                continue;
            }

            if (arg == inherit_cursor_arg)
            {
                _inherit_cursor = true;
                consume_arg(args, index);
                continue;
            }

            if (arg == text_measurement_arg)
            {
                auto parsed = get_string_argument(args, index);
                if (!parsed)
                {
                    return std::unexpected(parsed.error());
                }
                _text_measurement = std::move(parsed.value());
                continue;
            }

            if (arg == client_commandline_arg)
            {
                return set_client_command_line(args, index, true);
            }

            // Unknown token: preserve original behavior and treat it as
            // the beginning of the client command line payload.
            return set_client_command_line(args, index, false);
        }

        return {};
    }

    std::expected<void, ParseError> ConsoleArguments::set_client_command_line(std::vector<std::wstring>& args, const size_t index, const bool skip_first_token) noexcept
    {
        if (index >= args.size())
        {
            return std::unexpected(ParseError{ .message = L"Client command line index out of range" });
        }

        auto start = args.begin() + static_cast<std::ptrdiff_t>(index);
        if (skip_first_token)
        {
            if (*start != client_commandline_arg)
            {
                return std::unexpected(ParseError{ .message = L"Expected -- token for explicit client command line" });
            }
            args.erase(start);
        }

        // Reconstruct child command line using Win32 escaping rules so
        // downstream CreateProcessW receives equivalent tokenization.
        _client_command_line.clear();
        for (size_t token_index = index; token_index < args.size(); ++token_index)
        {
            _client_command_line.append(escape_argument(args[token_index]));
            if (token_index + 1 < args.size())
            {
                _client_command_line.append(kSpace);
            }
        }

        args.erase(args.begin() + static_cast<std::ptrdiff_t>(index), args.end());
        return {};
    }

    void ConsoleArguments::consume_arg(std::vector<std::wstring>& args, size_t& index)
    {
        OC_ASSERT(index < args.size());
        args.erase(args.begin() + static_cast<std::ptrdiff_t>(index));
    }

    std::expected<std::wstring, ParseError> ConsoleArguments::get_string_argument(std::vector<std::wstring>& args, size_t& index) noexcept
    {
        if (index + 1 >= args.size())
        {
            return std::unexpected(ParseError{ .message = L"Expected value after argument" });
        }

        consume_arg(args, index);
        const std::wstring value = args[index];
        consume_arg(args, index);
        return value;
    }

    std::expected<short, ParseError> ConsoleArguments::get_short_argument(std::vector<std::wstring>& args, size_t& index) noexcept
    {
        auto parsed_string = get_string_argument(args, index);
        if (!parsed_string)
        {
            return std::unexpected(parsed_string.error());
        }

        auto parsed = serialization::parse_i16(*parsed_string);
        if (!parsed)
        {
            return std::unexpected(ParseError{ .message = L"Short argument was out of range or malformed" });
        }
        return static_cast<short>(*parsed);
    }

    std::expected<void, ParseError> ConsoleArguments::handle_feature_value(std::vector<std::wstring>& args, size_t& index) noexcept
    {
        auto value = get_string_argument(args, index);
        if (!value)
        {
            return std::unexpected(value.error());
        }

        if (value.value() != feature_pty_arg)
        {
            return std::unexpected(ParseError{ .message = L"Unsupported --feature value" });
        }
        return {};
    }

    std::expected<void, ParseError> ConsoleArguments::parse_handle_arg(const std::wstring_view handle_text, uintptr_t& handle_value) noexcept
    {
        if (handle_value != 0)
        {
            return std::unexpected(ParseError{ .message = L"Handle value was already set" });
        }

        auto parsed = serialization::parse_hex_u64(handle_text, true);
        if (!parsed || *parsed == 0)
        {
            return std::unexpected(ParseError{ .message = L"Invalid handle value" });
        }

        if (*parsed > static_cast<std::uint64_t>(std::numeric_limits<uintptr_t>::max()))
        {
            return std::unexpected(ParseError{ .message = L"Handle value was out of range" });
        }

        handle_value = static_cast<uintptr_t>(*parsed);
        return {};
    }

    std::wstring ConsoleArguments::escape_argument(const std::wstring_view arg)
    {
        if (arg.empty())
        {
            return L"\"\"";
        }

        bool has_space = false;
        size_t size = arg.size();
        for (const auto ch : arg)
        {
            if (ch == L'"' || ch == L'\\')
            {
                ++size;
            }
            if (ch == L' ' || ch == L'\t')
            {
                has_space = true;
            }
        }

        if (has_space)
        {
            size += 2;
        }

        if (size == arg.size())
        {
            return std::wstring{ arg };
        }

        std::wstring escaped;
        escaped.reserve(size);

        if (has_space)
        {
            escaped.push_back(L'"');
        }

        size_t slash_count = 0;
        for (const auto ch : arg)
        {
            if (ch == L'\\')
            {
                ++slash_count;
                escaped.push_back(L'\\');
                continue;
            }

            if (ch == L'"')
            {
                for (; slash_count > 0; --slash_count)
                {
                    escaped.push_back(L'\\');
                }
                escaped.push_back(L'\\');
                escaped.push_back(L'"');
                continue;
            }

            slash_count = 0;
            escaped.push_back(ch);
        }

        if (has_space)
        {
            for (; slash_count > 0; --slash_count)
            {
                escaped.push_back(L'\\');
            }
            escaped.push_back(L'"');
        }

        return escaped;
    }

    bool ConsoleArguments::is_valid_handle(const core::HandleView value) noexcept
    {
        return value.valid();
    }

    bool ConsoleArguments::has_vt_handles() const noexcept
    {
        return is_valid_handle(_vt_in_handle) && is_valid_handle(_vt_out_handle);
    }

    bool ConsoleArguments::in_conpty_mode() const noexcept
    {
        return is_valid_handle(_vt_in_handle) || is_valid_handle(_vt_out_handle) || has_signal_handle();
    }

    bool ConsoleArguments::is_headless() const noexcept
    {
        return _headless;
    }

    bool ConsoleArguments::should_create_server_handle() const noexcept
    {
        return _create_server_handle;
    }

    bool ConsoleArguments::should_run_as_com_server() const noexcept
    {
        return _run_as_com_server;
    }

    core::HandleView ConsoleArguments::server_handle() const noexcept
    {
        return core::HandleView::from_uintptr(_server_handle);
    }

    core::HandleView ConsoleArguments::vt_in_handle() const noexcept
    {
        return _vt_in_handle;
    }

    core::HandleView ConsoleArguments::vt_out_handle() const noexcept
    {
        return _vt_out_handle;
    }

    bool ConsoleArguments::has_signal_handle() const noexcept
    {
        return is_valid_handle(signal_handle());
    }

    core::HandleView ConsoleArguments::signal_handle() const noexcept
    {
        return core::HandleView::from_uintptr(_signal_handle);
    }

    const std::wstring& ConsoleArguments::original_command_line() const noexcept
    {
        return _command_line;
    }

    const std::wstring& ConsoleArguments::client_command_line() const noexcept
    {
        return _client_command_line;
    }

    const std::wstring& ConsoleArguments::text_measurement() const noexcept
    {
        return _text_measurement;
    }

    bool ConsoleArguments::vt_mode_requested() const noexcept
    {
        return _vt_mode_requested;
    }

    bool ConsoleArguments::force_v1() const noexcept
    {
        return _force_v1;
    }

    bool ConsoleArguments::force_no_handoff() const noexcept
    {
        return _force_no_handoff;
    }

    short ConsoleArguments::width() const noexcept
    {
        return _width;
    }

    short ConsoleArguments::height() const noexcept
    {
        return _height;
    }

    bool ConsoleArguments::inherit_cursor() const noexcept
    {
        return _inherit_cursor;
    }
}
