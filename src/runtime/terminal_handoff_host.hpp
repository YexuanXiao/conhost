#pragma once

// Windowed host implementation for `ITerminalHandoff*` (ConPTY terminal UI handoff).
//
// When `openconsole_new` is registered as `DelegationTerminal`, a ConDrv server can
// activate `openconsole_new -Embedding` and call `ITerminalHandoff3::EstablishPtyHandoff`.
// The embedding server captures the pipe handles and forwards them into this module,
// which renders VT output inside an `openconsole_new` window and forwards keyboard
// input back to the server through the ConPTY byte transport.

#include "runtime/com_embedding_server.hpp"

#include <expected>

namespace oc::logging
{
    class Logger;
}

namespace oc::runtime
{
    // Default behavior: close the window when the delegated client exits.
    [[nodiscard]] std::expected<DWORD, ComEmbeddingError> run_windowed_terminal_handoff_host(
        TerminalHandoffPayload payload,
        logging::Logger& logger) noexcept;

    // Hold behavior: keep the window open after exit and append a final status line.
    [[nodiscard]] std::expected<DWORD, ComEmbeddingError> run_windowed_terminal_handoff_host_hold(
        TerminalHandoffPayload payload,
        logging::Logger& logger) noexcept;
}

