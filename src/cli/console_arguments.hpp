#pragma once

// CLI parser for `openconsole_new`.
//
// This type implements a compatibility-focused subset of the upstream
// OpenConsole argument parsing behavior:
// - `CommandLineToArgvW` is used to match Win32 tokenization rules.
// - Host/runtime switches are consumed from left to right.
// - The remaining tail is treated as the *client command line* payload, either:
//   - explicitly after `--`, or
//   - implicitly starting at the first unknown token.
//
// The client payload is reconstructed into a single `CreateProcessW` command
// line string using Win32 escaping rules so that a downstream parse yields the
// original tokens. See `new/docs/design/cli_command_line_reconstruction.md`.
//
// This module is "pure": it performs no process/session side effects.
// See `new/docs/conhost_module_partition.md` for module boundaries.

#include "core/handle_view.hpp"

#include <cstdint>
#include <expected>
#include <string>
#include <string_view>
#include <vector>

namespace oc::cli
{
    struct ParseError final
    {
        std::wstring message;
    };

    class ConsoleArguments final
    {
    public:
        static constexpr std::wstring_view vt_mode_arg = L"--vtmode";
        static constexpr std::wstring_view headless_arg = L"--headless";
        static constexpr std::wstring_view server_handle_arg = L"--server";
        static constexpr std::wstring_view signal_handle_arg = L"--signal";
        static constexpr std::wstring_view handle_prefix = L"0x";
        static constexpr std::wstring_view client_commandline_arg = L"--";
        static constexpr std::wstring_view force_v1_arg = L"-ForceV1";
        static constexpr std::wstring_view force_no_handoff_arg = L"-ForceNoHandoff";
        static constexpr std::wstring_view filepath_leader_prefix = L"\\??\\";
        static constexpr std::wstring_view width_arg = L"--width";
        static constexpr std::wstring_view height_arg = L"--height";
        static constexpr std::wstring_view inherit_cursor_arg = L"--inheritcursor";
        static constexpr std::wstring_view feature_arg = L"--feature";
        static constexpr std::wstring_view feature_pty_arg = L"pty";
        static constexpr std::wstring_view com_server_arg = L"-Embedding";
        static constexpr std::wstring_view delegated_window_arg = L"--delegated-window";
        static constexpr std::wstring_view text_measurement_arg = L"--textMeasurement";

        [[nodiscard]] static std::expected<ConsoleArguments, ParseError> parse(std::wstring_view command_line, core::HandleView std_in, core::HandleView std_out) noexcept;

        [[nodiscard]] bool has_vt_handles() const noexcept;
        [[nodiscard]] bool in_conpty_mode() const noexcept;
        [[nodiscard]] bool is_headless() const noexcept;
        [[nodiscard]] bool should_create_server_handle() const noexcept;
        [[nodiscard]] bool should_run_as_com_server() const noexcept;
        [[nodiscard]] bool delegated_window_requested() const noexcept;

        [[nodiscard]] core::HandleView server_handle() const noexcept;
        [[nodiscard]] core::HandleView vt_in_handle() const noexcept;
        [[nodiscard]] core::HandleView vt_out_handle() const noexcept;

        [[nodiscard]] bool has_signal_handle() const noexcept;
        [[nodiscard]] core::HandleView signal_handle() const noexcept;

        [[nodiscard]] const std::wstring& original_command_line() const noexcept;
        [[nodiscard]] const std::wstring& client_command_line() const noexcept;
        [[nodiscard]] const std::wstring& text_measurement() const noexcept;
        [[nodiscard]] bool vt_mode_requested() const noexcept;
        [[nodiscard]] bool force_v1() const noexcept;
        [[nodiscard]] bool force_no_handoff() const noexcept;
        [[nodiscard]] short width() const noexcept;
        [[nodiscard]] short height() const noexcept;
        [[nodiscard]] bool inherit_cursor() const noexcept;

    private:
        ConsoleArguments(std::wstring command_line, core::HandleView std_in, core::HandleView std_out);

        [[nodiscard]] std::expected<void, ParseError> parse_tokens(std::vector<std::wstring>& args) noexcept;
        [[nodiscard]] std::expected<void, ParseError> set_client_command_line(std::vector<std::wstring>& args, size_t index, bool skip_first_token) noexcept;

        static void consume_arg(std::vector<std::wstring>& args, size_t& index);
        [[nodiscard]] static std::expected<std::wstring, ParseError> get_string_argument(std::vector<std::wstring>& args, size_t& index) noexcept;
        [[nodiscard]] static std::expected<short, ParseError> get_short_argument(std::vector<std::wstring>& args, size_t& index) noexcept;
        [[nodiscard]] static std::expected<void, ParseError> handle_feature_value(std::vector<std::wstring>& args, size_t& index) noexcept;
        [[nodiscard]] static std::expected<void, ParseError> parse_handle_arg(std::wstring_view handle_text, uintptr_t& handle_value) noexcept;
        [[nodiscard]] static std::wstring escape_argument(std::wstring_view arg);
        [[nodiscard]] static bool is_valid_handle(core::HandleView value) noexcept;

        std::wstring _command_line;
        std::wstring _client_command_line;

        core::HandleView _vt_in_handle{};
        core::HandleView _vt_out_handle{};
        std::wstring _text_measurement;

        bool _force_no_handoff{ false };
        bool _force_v1{ false };
        bool _vt_mode_requested{ false };
        bool _headless{ false };

        short _width{ 0 };
        short _height{ 0 };

        bool _run_as_com_server{ false };
        bool _delegated_window{ false };
        bool _create_server_handle{ true };
        uintptr_t _server_handle{ 0 };
        uintptr_t _signal_handle{ 0 };
        bool _inherit_cursor{ false };
    };
}
