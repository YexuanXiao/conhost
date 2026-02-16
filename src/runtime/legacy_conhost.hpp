#pragma once

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
