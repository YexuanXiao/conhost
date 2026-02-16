#pragma once

// Small wrappers around Win32 wait APIs.
//
// Rationale:
// - Wait APIs require `HANDLE` arrays; keep raw handle usage localized.
// - Provide standard-library-style, strongly typed wrappers that accept
//   `HandleView` to avoid passing raw HANDLE values through the codebase.

#include "core/handle_view.hpp"

#include <Windows.h>

#include <array>

namespace oc::core
{
    [[nodiscard]] inline DWORD wait_for_two_objects(
        const HandleView first,
        const HandleView second,
        const bool wait_all,
        const DWORD timeout_ms) noexcept
    {
        const std::array<HANDLE, 2> handles{ first.get(), second.get() };
        return ::WaitForMultipleObjects(
            static_cast<DWORD>(handles.size()),
            handles.data(),
            wait_all ? TRUE : FALSE,
            timeout_ms);
    }
}

