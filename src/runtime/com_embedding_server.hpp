#pragma once

// COM local-server implementation for `-Embedding`.
//
// Upstream OpenConsole uses `-Embedding` to register a local COM server that
// exposes `IConsoleHandoff`. The in-box console host can activate that COM
// server and pass ownership of a console session (ConDrv server handle + attach
// message) to the out-of-box console host.
//
// In this replacement:
// - `ComEmbeddingServer::run(...)` registers the class object for a single
//   handoff, waits for `IConsoleHandoff::EstablishHandoff`, duplicates the
//   incoming handles into this process, and then starts the ConDrv server loop
//   (`condrv::ConDrvServer::run_with_handoff`) to service the session.
//
// See also:
// - `generated/IConsoleHandoff.h` (IDL-generated interface definition)
// - `new/docs/conhost_source_architecture.md`
// - `new/docs/conhost_behavior_imitation_matrix.md`

#include "core/handle_view.hpp"
#include "logging/logger.hpp"

#include <Windows.h>

#include <cstdint>
#include <expected>
#include <string>

namespace oc::runtime
{
    struct PortableAttachMessage final
    {
        // Portable subset of the driver connect message descriptor. The layout
        // intentionally mirrors `CONSOLE_PORTABLE_ATTACH_MSG` from the IDL.
        std::uint32_t IdLowPart{};
        std::int32_t IdHighPart{};
        std::uint64_t Process{};
        std::uint64_t Object{};
        std::uint32_t Function{};
        std::uint32_t InputSize{};
        std::uint32_t OutputSize{};
    };

    struct ComHandoffPayload final
    {
        // Handles received from the inbox host. These values are duplicated
        // into this process before being stored here, so the caller may assume
        // they remain valid for the duration of the session.
        core::HandleView server_handle{};
        core::HandleView input_event{};
        core::HandleView signal_pipe{};
        core::HandleView inbox_process{};
        PortableAttachMessage attach{};
    };

    struct ComEmbeddingError final
    {
        std::wstring context;
        HRESULT hresult{ E_FAIL };
        DWORD win32_error{ ERROR_GEN_FAILURE };
    };

    class ComEmbeddingServer final
    {
    public:
        using HandoffRunner = std::expected<DWORD, ComEmbeddingError> (*)(const ComHandoffPayload& payload, logging::Logger& logger) noexcept;

        [[nodiscard]] static std::expected<DWORD, ComEmbeddingError> run(logging::Logger& logger, DWORD wait_timeout_ms) noexcept;

        // Test hook: runs the COM registration + handoff capture, then invokes
        // the provided runner with the duplicated handles and attach message.
        //
        // The production implementation wires this to the ConDrv server loop.
        [[nodiscard]] static std::expected<DWORD, ComEmbeddingError> run_with_runner(
            logging::Logger& logger,
            DWORD wait_timeout_ms,
            HandoffRunner runner) noexcept;
    };
}
