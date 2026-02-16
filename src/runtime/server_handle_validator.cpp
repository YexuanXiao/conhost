#include "runtime/server_handle_validator.hpp"

#include "core/unique_handle.hpp"
#include "core/win32_handle.hpp"

namespace oc::runtime
{
    namespace
    {
        [[nodiscard]] std::expected<void, HandleValidationError> validate_handle_basics(const core::HandleView handle) noexcept
        {
            if (!handle)
            {
                return std::unexpected(HandleValidationError{ .win32_error = ERROR_INVALID_HANDLE });
            }

            DWORD flags = 0;
            if (::GetHandleInformation(handle.get(), &flags) == FALSE)
            {
                return std::unexpected(HandleValidationError{ .win32_error = ::GetLastError() });
            }

            auto duplicate = core::duplicate_handle_same_access(handle, false);
            if (!duplicate)
            {
                return std::unexpected(HandleValidationError{ .win32_error = duplicate.error() });
            }

            return {};
        }
    }

    std::expected<void, HandleValidationError> ServerHandleValidator::validate(const core::HandleView server_handle) noexcept
    {
        auto basic_result = validate_handle_basics(server_handle);
        if (!basic_result)
        {
            return basic_result;
        }

        // Prefer supported Win32 validation methods instead of NT internal APIs.
        const DWORD file_type = ::GetFileType(server_handle.get());
        if (file_type == FILE_TYPE_UNKNOWN && ::GetLastError() != NO_ERROR)
        {
            return std::unexpected(HandleValidationError{ .win32_error = ::GetLastError() });
        }

        return {};
    }

    std::expected<void, HandleValidationError> ServerHandleValidator::validate_optional_signal(const core::HandleView signal_handle) noexcept
    {
        if (!signal_handle)
        {
            return {};
        }

        return validate_handle_basics(signal_handle);
    }
}
