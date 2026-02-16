#pragma once

#include "core/handle_view.hpp"

#include <expected>

namespace oc::runtime
{
    struct HandleValidationError final
    {
        DWORD win32_error{ ERROR_INVALID_HANDLE };
    };

    class ServerHandleValidator final
    {
    public:
        [[nodiscard]] static std::expected<void, HandleValidationError> validate(core::HandleView server_handle) noexcept;
        [[nodiscard]] static std::expected<void, HandleValidationError> validate_optional_signal(core::HandleView signal_handle) noexcept;
    };
}
