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
        return 11;
    }

    // Disable cooked editing/echo for deterministic raw reads.
    if (::SetConsoleMode(stdin_handle, ENABLE_PROCESSED_INPUT) == FALSE)
    {
        return 12;
    }

    std::array<char, 1> byte{};
    DWORD read = 0;
    if (::ReadFile(stdin_handle, byte.data(), static_cast<DWORD>(byte.size()), &read, nullptr) == FALSE)
    {
        return 13;
    }

    if (read != 1 || byte[0] != 'a')
    {
        return 14;
    }

    if (!write_console_text(stdout_handle, L"RAWOK", 5))
    {
        return 15;
    }

    return 0;
}
