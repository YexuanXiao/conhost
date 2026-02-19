#include "condrv/condrv_server.hpp"
#include "condrv/vt_input_decoder.hpp"

#include <cstddef>
#include <span>
#include <string_view>

namespace
{
    [[nodiscard]] std::span<const std::byte> as_bytes(const std::string_view text) noexcept
    {
        return std::span<const std::byte>(
            reinterpret_cast<const std::byte*>(text.data()),
            text.size());
    }

    [[nodiscard]] bool decode_key_event(const std::string_view sequence, KEY_EVENT_RECORD& out) noexcept
    {
        oc::condrv::vt_input::DecodedToken token{};
        const auto outcome = oc::condrv::vt_input::try_decode_vt(as_bytes(sequence), token);
        if (outcome != oc::condrv::vt_input::DecodeResult::produced)
        {
            return false;
        }

        if (token.kind != oc::condrv::vt_input::TokenKind::key_event)
        {
            return false;
        }

        out = token.key;
        return true;
    }

    bool test_enter_synthesizes_unicode_char()
    {
        KEY_EVENT_RECORD key{};
        if (!decode_key_event("\x1b[13;0;0;1;0;1_", key))
        {
            return false;
        }

        return key.bKeyDown == TRUE &&
            key.wVirtualKeyCode == VK_RETURN &&
            key.uChar.UnicodeChar == L'\r';
    }

    bool test_backspace_synthesizes_unicode_char()
    {
        KEY_EVENT_RECORD key{};
        if (!decode_key_event("\x1b[8;0;0;1;0;1_", key))
        {
            return false;
        }

        return key.bKeyDown == TRUE &&
            key.wVirtualKeyCode == VK_BACK &&
            key.uChar.UnicodeChar == L'\b';
    }

    bool test_repeat_count_is_never_zero()
    {
        KEY_EVENT_RECORD key{};
        if (!decode_key_event("\x1b[13;0;0;1;0;0_", key))
        {
            return false;
        }

        return key.bKeyDown == TRUE && key.wRepeatCount == 1;
    }

    bool test_ctrl_c_match_when_vk_missing()
    {
        KEY_EVENT_RECORD key{};
        if (!decode_key_event("\x1b[0;0;3;1;8;1_", key))
        {
            return false;
        }

        return key.bKeyDown == TRUE &&
            oc::condrv::key_event_matches_ctrl_c(key);
    }

    bool test_ctrl_c_synthesizes_control_code()
    {
        KEY_EVENT_RECORD key{};
        if (!decode_key_event("\x1b[67;0;0;1;8;1_", key))
        {
            return false;
        }

        return key.bKeyDown == TRUE &&
            key.wVirtualKeyCode == static_cast<WORD>('C') &&
            key.uChar.UnicodeChar == static_cast<wchar_t>(0x03) &&
            oc::condrv::key_event_matches_ctrl_c(key);
    }
}

bool run_condrv_vt_input_decoder_tests()
{
    return test_enter_synthesizes_unicode_char() &&
           test_backspace_synthesizes_unicode_char() &&
           test_repeat_count_is_never_zero() &&
           test_ctrl_c_match_when_vk_missing() &&
           test_ctrl_c_synthesizes_control_code();
}

