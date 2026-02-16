#pragma once

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

