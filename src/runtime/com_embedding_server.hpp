#pragma once

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
