#include "runtime/launch_policy.hpp"

// The legacy policy is intentionally small:
// - ForceV2 is read from `HKCU\\Console\\ForceV2` (DWORD, default enabled).
// - `-ForceV1` overrides and routes to legacy conhost (unless ConPTY mode is
//   requested, where legacy cannot host the pseudo console path).
//
// This mirrors the observable policy behavior without porting the entire set of
// historical conhost feature flags.

namespace oc::runtime
{
    std::expected<bool, LaunchPolicyError> LaunchPolicy::read_force_v2_registry() noexcept
    {
        HKEY console_key = nullptr;
        const LONG open_status = ::RegOpenKeyExW(HKEY_CURRENT_USER, L"Console", 0, KEY_READ, &console_key);
        if (open_status != ERROR_SUCCESS)
        {
            // Match original behavior: if the key is missing, default to v2.
            if (open_status == ERROR_FILE_NOT_FOUND)
            {
                return true;
            }
            return std::unexpected(LaunchPolicyError{ .win32_error = static_cast<DWORD>(open_status) });
        }

        DWORD value = 1;
        DWORD type = 0;
        DWORD bytes = sizeof(value);
        const LONG query_status = ::RegQueryValueExW(
            console_key,
            L"ForceV2",
            nullptr,
            &type,
            reinterpret_cast<BYTE*>(&value),
            &bytes);
        ::RegCloseKey(console_key);

        if (query_status == ERROR_FILE_NOT_FOUND)
        {
            return true;
        }
        if (query_status != ERROR_SUCCESS)
        {
            return std::unexpected(LaunchPolicyError{ .win32_error = static_cast<DWORD>(query_status) });
        }
        if (type != REG_DWORD || bytes != sizeof(value))
        {
            return std::unexpected(LaunchPolicyError{ .win32_error = ERROR_BAD_FORMAT });
        }

        return value != 0;
    }

    LaunchPolicyDecision LaunchPolicy::decide(const bool in_conpty_mode, const bool force_v1, const bool force_v2_registry_enabled) noexcept
    {
        if (in_conpty_mode)
        {
            return LaunchPolicyDecision{
                .use_legacy_conhost = false,
                .force_v2_registry_enabled = force_v2_registry_enabled,
            };
        }

        if (force_v1)
        {
            return LaunchPolicyDecision{
                .use_legacy_conhost = true,
                .force_v2_registry_enabled = force_v2_registry_enabled,
            };
        }

        return LaunchPolicyDecision{
            .use_legacy_conhost = !force_v2_registry_enabled,
            .force_v2_registry_enabled = force_v2_registry_enabled,
        };
    }
}
