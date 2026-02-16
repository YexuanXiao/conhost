#pragma once

#include <Windows.h>

#include <cwchar>

namespace oc::core
{
    inline void fail_fast_assert(const wchar_t* expression, const wchar_t* file, const unsigned line) noexcept
    {
        wchar_t buffer[768]{};
        _snwprintf_s(
            buffer,
            _TRUNCATE,
            L"[openconsole_new] assertion failed: %ls (%ls:%u)\n",
            expression,
            file,
            line);
        ::OutputDebugStringW(buffer);
        __fastfail(FAST_FAIL_FATAL_APP_EXIT);
    }
}

#define OC_WIDEN_INNER(value) L##value
#define OC_WIDEN(value) OC_WIDEN_INNER(value)
#define OC_ASSERT(expr) ((expr) ? static_cast<void>(0) : ::oc::core::fail_fast_assert(OC_WIDEN(#expr), OC_WIDEN(__FILE__), __LINE__))

