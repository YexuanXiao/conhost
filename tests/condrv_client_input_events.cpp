#include <Windows.h>

#include <array>

namespace
{
    [[nodiscard]] bool write_console_text(const HANDLE handle, const wchar_t* text, const DWORD length) noexcept
    {
        DWORD written = 0;
        return ::WriteConsoleW(handle, text, length, &written, nullptr) != FALSE && written == length;
    }

    [[nodiscard]] bool read_console_input_exact(const HANDLE handle, INPUT_RECORD* records, const DWORD expected) noexcept
    {
        DWORD total = 0;
        while (total < expected)
        {
            DWORD read = 0;
            if (::ReadConsoleInputW(handle, records + total, expected - total, &read) == FALSE)
            {
                return false;
            }
            total += read;
        }
        return true;
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

    // Read two KEY_EVENT records from the console input stream. The integration test injects
    // win32-input-mode VT sequences so the ConDrv server should decode them to full KEY_EVENT
    // metadata (virtual keys + Unicode payload).
    std::array<INPUT_RECORD, 2> records{};
    if (!read_console_input_exact(stdin_handle, records.data(), static_cast<DWORD>(records.size())))
    {
        return 12;
    }

    const auto expect_key = [](const INPUT_RECORD& record) noexcept -> const KEY_EVENT_RECORD*
    {
        if (record.EventType != KEY_EVENT)
        {
            return nullptr;
        }
        return &record.Event.KeyEvent;
    };

    const KEY_EVENT_RECORD* first = expect_key(records[0]);
    if (first == nullptr ||
        first->bKeyDown == FALSE ||
        first->wVirtualKeyCode != 65 ||
        first->wRepeatCount != 1 ||
        first->uChar.UnicodeChar != L'a')
    {
        return 13;
    }

    const KEY_EVENT_RECORD* second = expect_key(records[1]);
    if (second == nullptr ||
        second->bKeyDown == FALSE ||
        second->wVirtualKeyCode != VK_UP ||
        second->wRepeatCount != 1 ||
        second->uChar.UnicodeChar != 0)
    {
        return 14;
    }

    if (!write_console_text(stdout_handle, L"INPUTOK", 7))
    {
        return 15;
    }

    return 0;
}

