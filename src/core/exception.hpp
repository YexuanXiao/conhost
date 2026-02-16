#pragma once

#include <Windows.h>

#include <string>
#include <string_view>

namespace oc::core
{
    // Strongly typed Win32 error codes for exception transport.
    //
    // Throwing an enum class keeps exception objects small and avoids extra
    // wrapper types for values that are already stable and well-defined by
    // the platform.
    enum class Win32Error : DWORD
    {
        success = ERROR_SUCCESS,
    };

    class AppException final
    {
    public:
        explicit AppException(std::wstring message) :
            _message(std::move(message))
        {
        }

        [[nodiscard]] const std::wstring& message() const noexcept
        {
            return _message;
        }

    private:
        std::wstring _message;
    };

    [[nodiscard]] constexpr DWORD to_dword(const Win32Error error) noexcept
    {
        return static_cast<DWORD>(error);
    }

    [[nodiscard]] constexpr Win32Error from_dword(const DWORD error) noexcept
    {
        return static_cast<Win32Error>(error);
    }

    [[noreturn]] inline void throw_last_error() // Win32 API failure transport
    {
        throw from_dword(::GetLastError());
    }

    [[noreturn]] inline void throw_last_error(const std::wstring_view /*context*/)
    {
        // Context should be logged at the call site when available. The thrown
        // value is intentionally just the Win32 error code.
        throw_last_error();
    }
}
