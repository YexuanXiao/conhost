#include "runtime/key_input_encoder.hpp"

#include <Windows.h>

namespace
{
    [[nodiscard]] KEY_EVENT_RECORD make_key(
        const bool down,
        const WORD virtual_key,
        const wchar_t unicode_char,
        const DWORD modifiers = 0)
    {
        KEY_EVENT_RECORD key{};
        key.bKeyDown = down ? TRUE : FALSE;
        key.wVirtualKeyCode = virtual_key;
        key.uChar.UnicodeChar = unicode_char;
        key.dwControlKeyState = modifiers;
        return key;
    }

    bool test_regular_character()
    {
        auto bytes = oc::runtime::KeyInputEncoder::encode(make_key(true, 'A', L'a'));
        return bytes.size() == 1 && bytes[0] == 'a';
    }

    bool test_arrow_key()
    {
        auto bytes = oc::runtime::KeyInputEncoder::encode(make_key(true, VK_UP, L'\0'));
        return bytes.size() == 3 && bytes[0] == 0x1b && bytes[1] == '[' && bytes[2] == 'A';
    }

    bool test_ctrl_c()
    {
        auto bytes = oc::runtime::KeyInputEncoder::encode(make_key(true, 'C', L'c', LEFT_CTRL_PRESSED));
        return bytes.size() == 1 && bytes[0] == 0x03;
    }

    bool test_key_up_ignored()
    {
        auto bytes = oc::runtime::KeyInputEncoder::encode(make_key(false, 'A', L'a'));
        return bytes.empty();
    }

    bool test_alt_prefixes_escape()
    {
        auto bytes = oc::runtime::KeyInputEncoder::encode(make_key(true, 'A', L'a', LEFT_ALT_PRESSED));
        return bytes.size() == 2 && bytes[0] == 0x1b && bytes[1] == 'a';
    }

    bool test_backspace_maps_del()
    {
        auto bytes = oc::runtime::KeyInputEncoder::encode(make_key(true, VK_BACK, L'\b'));
        return bytes.size() == 1 && static_cast<unsigned char>(bytes[0]) == 0x7f;
    }
}

bool run_key_input_encoder_tests()
{
    return test_regular_character() &&
           test_arrow_key() &&
           test_ctrl_c() &&
           test_key_up_ignored() &&
           test_alt_prefixes_escape() &&
           test_backspace_maps_del();
}
