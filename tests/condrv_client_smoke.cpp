#include <Windows.h>

#include <array>

namespace
{
    [[nodiscard]] bool write_console_text(const HANDLE handle, const wchar_t* text, const DWORD length) noexcept
    {
        DWORD written = 0;
        return ::WriteConsoleW(handle, text, length, &written, nullptr) != FALSE && written == length;
    }
}

int wmain() noexcept
{
    const HANDLE stdin_handle = ::GetStdHandle(STD_INPUT_HANDLE);
    const HANDLE stdout_handle = ::GetStdHandle(STD_OUTPUT_HANDLE);
    if (stdin_handle == nullptr || stdin_handle == INVALID_HANDLE_VALUE ||
        stdout_handle == nullptr || stdout_handle == INVALID_HANDLE_VALUE)
    {
        return 10;
    }

    DWORD input_mode = 0;
    if (::GetConsoleMode(stdin_handle, &input_mode) == FALSE)
    {
        // Ensure the handle is treated as a console input handle.
        return 11;
    }

    // Disable cooked editing/echo for deterministic raw reads.
    if (::SetConsoleMode(stdin_handle, ENABLE_PROCESSED_INPUT) == FALSE)
    {
        return 12;
    }

    if (!write_console_text(stdout_handle, L"HELLO", 5))
    {
        return 13;
    }

    std::array<wchar_t, 3> buffer{};
    DWORD read = 0;
    if (::ReadConsoleW(stdin_handle, buffer.data(), static_cast<DWORD>(buffer.size()), &read, nullptr) == FALSE || read != buffer.size())
    {
        return 14;
    }

    if (!write_console_text(stdout_handle, L"X", 1))
    {
        return 15;
    }

    if (!write_console_text(stdout_handle, buffer.data(), static_cast<DWORD>(buffer.size())))
    {
        return 16;
    }

    if (!write_console_text(stdout_handle, L"Y", 1))
    {
        return 17;
    }

    return 0;
}

