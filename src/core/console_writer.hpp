#pragma once

#include "core/handle_view.hpp"

#include <string_view>

namespace oc::core
{
    inline void write_console_line(const std::wstring_view message) noexcept
    {
        const HandleView stream(::GetStdHandle(STD_ERROR_HANDLE));
        if (!stream)
        {
            return;
        }

        DWORD mode = 0;
        const bool is_console = ::GetConsoleMode(stream.get(), &mode) != FALSE;
        if (is_console)
        {
            DWORD written = 0;
            const auto length = static_cast<DWORD>(message.size());
            ::WriteConsoleW(stream.get(), message.data(), length, &written, nullptr);
            ::WriteConsoleW(stream.get(), L"\r\n", 2, &written, nullptr);
            return;
        }

        DWORD written = 0;
        ::WriteFile(stream.get(), message.data(), static_cast<DWORD>(message.size() * sizeof(wchar_t)), &written, nullptr);
        ::WriteFile(stream.get(), L"\r\n", static_cast<DWORD>(2 * sizeof(wchar_t)), &written, nullptr);
    }
}
