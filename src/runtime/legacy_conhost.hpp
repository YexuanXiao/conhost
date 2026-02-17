#pragma once

// Legacy inbox conhost activation shim.
//
// When the launch policy decides that the legacy host should handle a session,
// the replacement does not attempt to reimplement legacy conhost. Instead it
// requests activation of the OS-provided in-box host for the inherited server
// handle.
//
// This module is intentionally tiny and exists only to keep the policy and the
// runtime glue out of `Application`.
//
// See also:
// - `new/src/runtime/launch_policy.*`
// - `new/tests/launch_policy_tests.cpp`

#include "core/handle_view.hpp"

#include <expected>

namespace oc::runtime
{
    struct LegacyConhostError final
    {
        DWORD win32_error{ ERROR_GEN_FAILURE };
    };

    class LegacyConhost final
    {
    public:
        [[nodiscard]] static std::expected<void, LegacyConhostError> activate(core::HandleView server_handle) noexcept;
    };
}
