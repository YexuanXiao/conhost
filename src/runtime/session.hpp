#pragma once

// Session runtime entrypoint for `openconsole_new`.
//
// This module is the replacement's "mode switch" layer: it interprets the
// startup contract established by `cli::ConsoleArguments` and decides how the
// process should behave:
//
// - Client/child process hosting (`CreateProcessW`) using either:
//   - inherited stdio (classic console behavior), or
//   - a pseudo console (ConPTY) transport (headless/VT-mode scenarios).
// - Server-handle hosting (`--server 0x...`) where we service a ConDrv server
//   loop directly (classic conhost-style hosting).
// - Default-terminal delegation when started as a windowed ConDrv server
//   (`--server` + not headless + not ConPTY).
//
// See also:
// - `new/docs/architecture.md`
// - `new/docs/conhost_module_partition.md`
// - `new/docs/conhost_behavior_imitation_matrix.md`
//
// Notes on the error model:
// - The public surface returns `std::expected` because `Application::run` needs
//   to turn failures into an exit code and a localized log message.
// - Internally, most helper types use exceptions only for allocation failures
//   (vector/string growth). Operational Win32 failures are represented as
//   `SessionError` and propagated via `std::expected`.

#include "core/handle_view.hpp"

#include "logging/logger.hpp"

#include <expected>
#include <string>

namespace oc::runtime
{
    struct SessionOptions final
    {
        // Child/client command line to run when `create_server_handle==true`.
        // In `--server` startup mode, this is ignored for compatibility with
        // upstream OpenConsole (the server host is already created elsewhere).
        std::wstring client_command_line;

        // When true, this process creates a new console server instance and
        // then launches a client application into it (conhost-style EXE mode).
        // When false, this process was started in `--server` mode and must use
        // the provided `server_handle` to host an existing ConDrv session.
        bool create_server_handle{ true };

        // ConDrv server handle (only meaningful when `create_server_handle==false`).
        core::HandleView server_handle{};

        // "Signal" handle provided by conhost-style startups:
        // - In ConPTY scenarios this is a pipe whose lifetime is tied to the
        //   hosting terminal; disconnect indicates shutdown.
        // - In other scenarios it may be a waitable event.
        //
        // The runtime must not assume all signal handles are waitable events.
        core::HandleView signal_handle{};

        // Host-side stdio handles used by the ConPTY transport and by headless
        // `--server` runs. These are typically pipes connected to a terminal.
        core::HandleView host_input{};
        core::HandleView host_output{};

        // Desired initial ConPTY size. Zero uses system defaults.
        short width{ 0 };
        short height{ 0 };

        // When true, avoid creating a classic conhost window and instead use a
        // pipe-based transport (ConPTY) or a headless ConDrv server loop.
        bool headless{ false };

        // When true, run the client under a pseudo console (ConPTY). This is
        // selected by `Application` based on CLI switches and on whether the
        // standard handles are pipe-like.
        bool in_conpty_mode{ false };

        // When true, request cursor inheritance during ConPTY handshake
        // (mirrors upstream VtIo behavior).
        bool inherit_cursor{ false };

        // Runtime knob for renderer measurement (used only in classic-window
        // mode; kept as a string to match OpenConsole-style CLI wiring).
        std::wstring text_measurement;

        // When true, suppress default-terminal delegation for server startups.
        bool force_no_handoff{ false };
    };

    struct SessionError final
    {
        std::wstring context;
        DWORD win32_error{ ERROR_GEN_FAILURE };
    };

    class Session final
    {
    public:
        [[nodiscard]] static std::expected<DWORD, SessionError> run(const SessionOptions& options, logging::Logger& logger) noexcept;
    };
}
