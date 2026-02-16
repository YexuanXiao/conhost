#include "runtime/launch_policy.hpp"

namespace
{
    bool test_force_v1_prefers_legacy()
    {
        const auto decision = oc::runtime::LaunchPolicy::decide(false, true, true);
        return decision.use_legacy_conhost;
    }

    bool test_conpty_disables_legacy_even_with_force_v1()
    {
        const auto decision = oc::runtime::LaunchPolicy::decide(true, true, false);
        return !decision.use_legacy_conhost;
    }

    bool test_registry_forcev2_off_uses_legacy()
    {
        const auto decision = oc::runtime::LaunchPolicy::decide(false, false, false);
        return decision.use_legacy_conhost;
    }

    bool test_registry_forcev2_on_uses_v2()
    {
        const auto decision = oc::runtime::LaunchPolicy::decide(false, false, true);
        return !decision.use_legacy_conhost;
    }
}

bool run_launch_policy_tests()
{
    return test_force_v1_prefers_legacy() &&
           test_conpty_disables_legacy_even_with_force_v1() &&
           test_registry_forcev2_off_uses_legacy() &&
           test_registry_forcev2_on_uses_v2();
}

