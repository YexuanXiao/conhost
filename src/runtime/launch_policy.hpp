#pragma once

// Legacy launch-policy compatibility (`ForceV2` / `-ForceV1`).
//
// Upstream conhost/OpenConsole supports a legacy selection policy where certain
// startups are routed to the "v1" host (legacy conhost) based on the ForceV2
// registry value and/or an explicit `-ForceV1` command line switch.
//
// The replacement keeps this policy as a separate, deterministic module so:
// - `Application` can make the decision early,
// - `Session` remains focused on executing a chosen runtime path,
// - tests can cover the selection matrix without spawning processes.
//
// See also:
// - `new/tests/launch_policy_tests.cpp`
// - `new/docs/conhost_behavior_imitation_matrix.md`

#include <Windows.h>

#include <expected>

namespace oc::runtime
{
    struct LaunchPolicyError final
    {
        DWORD win32_error{ ERROR_GEN_FAILURE };
    };

    struct LaunchPolicyDecision final
    {
        bool use_legacy_conhost{ false };
        bool force_v2_registry_enabled{ true };
    };

    class LaunchPolicy final
    {
    public:
        [[nodiscard]] static std::expected<bool, LaunchPolicyError> read_force_v2_registry() noexcept;
        [[nodiscard]] static LaunchPolicyDecision decide(bool in_conpty_mode, bool force_v1, bool force_v2_registry_enabled) noexcept;
    };
}
