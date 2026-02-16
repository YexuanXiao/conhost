#pragma once

#include <string>

namespace oc::runtime
{
    class StartupCommand final
    {
    public:
        [[nodiscard]] static std::wstring resolve_default_client_command();
    };
}

