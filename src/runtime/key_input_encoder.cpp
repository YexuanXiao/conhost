#include "runtime/key_input_encoder.hpp"

#include <array>

namespace oc::runtime
{
    namespace
    {
        [[nodiscard]] std::vector<char> narrow_utf16_code_unit(const wchar_t code_unit)
        {
            if (code_unit == L'\0')
            {
                return {};
            }

            std::array<char, 8> utf8{};
            const int utf8_size = ::WideCharToMultiByte(
                CP_UTF8,
                0,
                &code_unit,
                1,
                utf8.data(),
                static_cast<int>(utf8.size()),
                nullptr,
                nullptr);
            if (utf8_size <= 0)
            {
                return {};
            }

            return std::vector<char>(utf8.begin(), utf8.begin() + utf8_size);
        }

        [[nodiscard]] std::vector<char> sequence(const char* value, const size_t length)
        {
            return std::vector<char>(value, value + length);
        }

        [[nodiscard]] bool has_modifier(const DWORD state) noexcept
        {
            return (state & (SHIFT_PRESSED | LEFT_ALT_PRESSED | RIGHT_ALT_PRESSED | LEFT_CTRL_PRESSED | RIGHT_CTRL_PRESSED)) != 0;
        }
    }

    std::vector<char> KeyInputEncoder::encode(const KEY_EVENT_RECORD& key_event)
    {
        if (!key_event.bKeyDown)
        {
            return {};
        }

        const DWORD modifier_state = key_event.dwControlKeyState;
        const bool ctrl_pressed = (modifier_state & (LEFT_CTRL_PRESSED | RIGHT_CTRL_PRESSED)) != 0;
        const bool alt_pressed = (modifier_state & (LEFT_ALT_PRESSED | RIGHT_ALT_PRESSED)) != 0;

        // Ctrl+C in cooked console UX should still generate ETX for interactive
        // applications hosted through the pseudo console.
        if (ctrl_pressed && key_event.wVirtualKeyCode == 'C')
        {
            return { 0x03 };
        }

        if (ctrl_pressed && key_event.wVirtualKeyCode == 'D')
        {
            return { 0x04 };
        }

        // Special keys are mapped to CSI/SS3 style VT sequences.
        switch (key_event.wVirtualKeyCode)
        {
        case VK_UP:
            return sequence("\x1b[A", 3);
        case VK_DOWN:
            return sequence("\x1b[B", 3);
        case VK_RIGHT:
            return sequence("\x1b[C", 3);
        case VK_LEFT:
            return sequence("\x1b[D", 3);
        case VK_HOME:
            return sequence("\x1b[H", 3);
        case VK_END:
            return sequence("\x1b[F", 3);
        case VK_PRIOR:
            return sequence("\x1b[5~", 4);
        case VK_NEXT:
            return sequence("\x1b[6~", 4);
        case VK_DELETE:
            return sequence("\x1b[3~", 4);
        case VK_INSERT:
            return sequence("\x1b[2~", 4);
        case VK_F1:
            return sequence("\x1bOP", 3);
        case VK_F2:
            return sequence("\x1bOQ", 3);
        case VK_F3:
            return sequence("\x1bOR", 3);
        case VK_F4:
            return sequence("\x1bOS", 3);
        case VK_RETURN:
            return { '\r' };
        case VK_TAB:
            return { '\t' };
        case VK_BACK:
            return { 0x7f };
        case VK_ESCAPE:
            return { 0x1b };
        default:
            break;
        }

        if (key_event.uChar.UnicodeChar != 0)
        {
            auto utf8 = narrow_utf16_code_unit(key_event.uChar.UnicodeChar);
            if (!utf8.empty() && alt_pressed)
            {
                std::vector<char> prefixed;
                prefixed.reserve(utf8.size() + 1);
                prefixed.push_back(0x1b);
                prefixed.insert(prefixed.end(), utf8.begin(), utf8.end());
                return prefixed;
            }
            return utf8;
        }

        // Non-character keys with modifiers are currently ignored when we
        // don't have a stable VT sequence mapping.
        if (has_modifier(modifier_state))
        {
            return {};
        }

        return {};
    }
}

