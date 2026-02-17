#pragma once

#include "core/handle_view.hpp"
#include "core/unique_handle.hpp"

#include <Windows.h>

#include <expected>
#include <optional>
#include <string>

namespace oc::logging
{
    class Logger;
}

namespace oc::runtime
{
    struct TerminalHandoffError final
    {
        std::wstring context;
        DWORD win32_error{ ERROR_GEN_FAILURE };
        HRESULT hresult{ E_FAIL };
    };

    struct TerminalHandoffChannels final
    {
        core::UniqueHandle host_input;
        core::UniqueHandle host_output;
        core::UniqueHandle signal_pipe;
    };

    class TerminalHandoff final
    {
    public:
        using DelegationResolver = std::expected<std::optional<CLSID>, TerminalHandoffError> (*)() noexcept;
        using HandoffInvoker = std::expected<TerminalHandoffChannels, TerminalHandoffError> (*)(
            const CLSID& terminal_clsid,
            core::HandleView server_handle,
            logging::Logger& logger) noexcept;

        // Attempts to delegate a `--server` startup to the configured default
        // terminal host. On success, returns host input/output/signal handles
        // that should be used for a headless ConDrv server run.
        //
        // Result contract:
        // - expected<optional<...>> success + empty optional: no delegation target
        //   (or handoff suppressed), caller should continue with classic window path.
        // - expected<optional<...>> success + value: handoff established.
        // - unexpected: handoff attempt failed; caller may log and fall back.
        [[nodiscard]] static std::expected<std::optional<TerminalHandoffChannels>, TerminalHandoffError> try_establish(
            core::HandleView server_handle,
            bool force_no_handoff,
            logging::Logger& logger) noexcept;

        // Test hook variant.
        [[nodiscard]] static std::expected<std::optional<TerminalHandoffChannels>, TerminalHandoffError> try_establish_with(
            core::HandleView server_handle,
            bool force_no_handoff,
            logging::Logger& logger,
            DelegationResolver resolver,
            HandoffInvoker invoker) noexcept;
    };
}
