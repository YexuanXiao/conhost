#pragma once

// Validation helpers for inherited handle values (`--server`, `--signal`).
//
// The replacement treats inherited handles defensively:
// - `GetHandleInformation` verifies the value is a valid handle in this process.
// - `DuplicateHandle` verifies it can be duplicated with the same access.
// - `GetFileType` is used as a coarse, supported Win32 classification to catch
//   invalid or unsupported handle kinds early.
//
// Note: validation is intentionally conservative; it does not attempt to prove
// the handle refers to a ConDrv server object. The goal is to fail fast with a
// stable Win32 error if the host process passes an invalid value.

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
