#pragma once

// Helpers for hosting a classic console window when acting as the user's "default terminal".
//
// Background:
// - When `HKCU\Console\%%Startup\DelegationConsole` is configured, the in-box console host
//   activates an out-of-proc COM local server implementing `IConsoleHandoff`.
// - `openconsole_new` implements that COM local server behind `-Embedding`.
//
// Design goal:
// Keep the COM embedding server (`runtime/com_embedding_server.*`) non-GUI and reusable.
// The window-hosted behavior is implemented here and can be enabled explicitly via a
// dedicated CLI option (see `cli/console_arguments.*`).
//
// This module intentionally depends on the renderer, but only through the published
// immutable screen snapshot boundary (`view::PublishedScreenBuffer`).

#include "logging/logger.hpp"
#include "runtime/com_embedding_server.hpp"

#include <Windows.h>

#include <expected>

namespace oc::runtime
{
    // `IConsoleHandoff` runner that hosts the delegated session in a classic Win32 window.
    //
    // This is intended to be passed to `ComEmbeddingServer::run_with_runner(...)` when
    // `openconsole_new` is launched as the user's configured default terminal.
    [[nodiscard]] std::expected<DWORD, ComEmbeddingError> run_windowed_default_terminal_host(
        const ComHandoffPayload& payload,
        logging::Logger& logger) noexcept;
}

