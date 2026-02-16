#pragma once

// Small Win32 handle helper functions that return RAII wrappers.
//
// These helpers keep raw HANDLE manipulation localized and make ownership
// transfers explicit at call sites.

#include "core/handle_view.hpp"
#include "core/unique_handle.hpp"

#include <Windows.h>

#include <expected>

namespace oc::core
{
    [[nodiscard]] inline std::expected<UniqueHandle, DWORD> create_event(
        const bool manual_reset,
        const bool initial_state,
        const wchar_t* const name = nullptr) noexcept
    {
        UniqueHandle event(::CreateEventW(nullptr, manual_reset ? TRUE : FALSE, initial_state ? TRUE : FALSE, name));
        if (!event.valid())
        {
            return std::unexpected(::GetLastError());
        }
        return std::move(event);
    }

    [[nodiscard]] inline std::expected<UniqueHandle, DWORD> duplicate_handle(
        const HandleView source,
        const DWORD desired_access,
        const bool inherit_handle,
        const DWORD options) noexcept
    {
        if (!source)
        {
            return std::unexpected(ERROR_INVALID_HANDLE);
        }

        UniqueHandle duplicated{};
        if (::DuplicateHandle(
                ::GetCurrentProcess(),
                source.get(),
                ::GetCurrentProcess(),
                duplicated.put(),
                desired_access,
                inherit_handle ? TRUE : FALSE,
                options) == FALSE)
        {
            return std::unexpected(::GetLastError());
        }

        return std::move(duplicated);
    }

    [[nodiscard]] inline std::expected<UniqueHandle, DWORD> duplicate_handle_same_access(
        const HandleView source,
        const bool inherit_handle = false) noexcept
    {
        return duplicate_handle(source, 0, inherit_handle, DUPLICATE_SAME_ACCESS);
    }

    [[nodiscard]] inline std::expected<UniqueHandle, DWORD> duplicate_current_process(
        const DWORD desired_access,
        const bool inherit_handle = false) noexcept
    {
        // GetCurrentProcess() returns a pseudo-handle; duplicate it to obtain a
        // real handle that can be transferred and closed.
        UniqueHandle duplicated{};
        if (::DuplicateHandle(
                ::GetCurrentProcess(),
                ::GetCurrentProcess(),
                ::GetCurrentProcess(),
                duplicated.put(),
                desired_access,
                inherit_handle ? TRUE : FALSE,
                0) == FALSE)
        {
            return std::unexpected(::GetLastError());
        }

        return std::move(duplicated);
    }

    [[nodiscard]] inline std::expected<UniqueHandle, DWORD> duplicate_current_thread(
        const DWORD desired_access = 0,
        const bool inherit_handle = false,
        const DWORD options = DUPLICATE_SAME_ACCESS) noexcept
    {
        UniqueHandle duplicated{};
        if (::DuplicateHandle(
                ::GetCurrentProcess(),
                ::GetCurrentThread(),
                ::GetCurrentProcess(),
                duplicated.put(),
                desired_access,
                inherit_handle ? TRUE : FALSE,
                options) == FALSE)
        {
            return std::unexpected(::GetLastError());
        }

        return std::move(duplicated);
    }
}
