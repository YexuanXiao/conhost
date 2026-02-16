#include "condrv/vt_input_decoder.hpp"

#include <cstdint>
#include <limits>

namespace oc::condrv::vt_input
{
    namespace
    {
        constexpr std::byte esc_byte{ static_cast<std::byte>(0x1b) };
        constexpr std::byte csi_byte{ static_cast<std::byte>(0x9b) }; // C1 CSI

        [[nodiscard]] constexpr unsigned char to_char(const std::byte value) noexcept
        {
            return static_cast<unsigned char>(value);
        }

        [[nodiscard]] constexpr uint32_t saturating_mul_add_10(const uint32_t value, const unsigned digit) noexcept
        {
            constexpr uint32_t max_value = std::numeric_limits<uint32_t>::max();
            if (value > (max_value - digit) / 10u)
            {
                return max_value;
            }
            return value * 10u + digit;
        }

        struct ParsedParam final
        {
            bool present{};
            uint32_t value{};
        };

        [[nodiscard]] constexpr WORD saturate_word(const uint32_t value) noexcept
        {
            return value > 0xFFFFu ? static_cast<WORD>(0xFFFFu) : static_cast<WORD>(value);
        }

        [[nodiscard]] constexpr wchar_t saturate_wchar(const uint32_t value) noexcept
        {
            return value > 0xFFFFu ? static_cast<wchar_t>(0xFFFFu) : static_cast<wchar_t>(value);
        }

        [[nodiscard]] KEY_EVENT_RECORD make_simple_key_event(const WORD virtual_key) noexcept
        {
            KEY_EVENT_RECORD key{};
            key.bKeyDown = TRUE;
            key.wRepeatCount = 1;
            key.wVirtualKeyCode = virtual_key;
            key.wVirtualScanCode = 0;
            key.dwControlKeyState = 0;
            key.uChar.UnicodeChar = L'\0';
            return key;
        }

        [[nodiscard]] DecodeResult decode_ss3(const std::span<const std::byte> bytes, DecodedToken& out) noexcept
        {
            // SS3 fallback: ESC O P/Q/R/S -> F1-F4.
            if (bytes.size() < 2)
            {
                return DecodeResult::need_more_data;
            }

            if (bytes[0] != esc_byte || to_char(bytes[1]) != 'O')
            {
                return DecodeResult::no_match;
            }

            if (bytes.size() < 3)
            {
                return DecodeResult::need_more_data;
            }

            WORD vk = 0;
            switch (to_char(bytes[2]))
            {
            case 'P':
                vk = VK_F1;
                break;
            case 'Q':
                vk = VK_F2;
                break;
            case 'R':
                vk = VK_F3;
                break;
            case 'S':
                vk = VK_F4;
                break;
            default:
                return DecodeResult::no_match;
            }

            out = {};
            out.kind = TokenKind::key_event;
            out.bytes_consumed = 3;
            out.key = make_simple_key_event(vk);
            return DecodeResult::produced;
        }

        [[nodiscard]] DecodeResult decode_csi(const std::span<const std::byte> bytes, DecodedToken& out) noexcept
        {
            size_t prefix_len = 0;
            if (!bytes.empty() && bytes[0] == csi_byte)
            {
                prefix_len = 1;
            }
            else if (bytes.size() >= 2 && bytes[0] == esc_byte && to_char(bytes[1]) == '[')
            {
                prefix_len = 2;
            }
            else
            {
                return DecodeResult::no_match;
            }

            if (bytes.size() <= prefix_len)
            {
                return DecodeResult::need_more_data;
            }

            const unsigned char first = to_char(bytes[prefix_len]);

            // Focus events (CSI I / CSI O) are not console input.
            if (first == 'I' || first == 'O')
            {
                out = {};
                out.kind = TokenKind::ignored_sequence;
                out.bytes_consumed = prefix_len + 1;
                return DecodeResult::produced;
            }

            // Basic cursor keys: CSI A/B/C/D, home/end: CSI H/F.
            if (first == 'A' || first == 'B' || first == 'C' || first == 'D' || first == 'H' || first == 'F')
            {
                WORD vk = 0;
                switch (first)
                {
                case 'A':
                    vk = VK_UP;
                    break;
                case 'B':
                    vk = VK_DOWN;
                    break;
                case 'C':
                    vk = VK_RIGHT;
                    break;
                case 'D':
                    vk = VK_LEFT;
                    break;
                case 'H':
                    vk = VK_HOME;
                    break;
                case 'F':
                    vk = VK_END;
                    break;
                default:
                    break;
                }

                out = {};
                out.kind = TokenKind::key_event;
                out.bytes_consumed = prefix_len + 1;
                out.key = make_simple_key_event(vk);
                return DecodeResult::produced;
            }

            // DA1 response: CSI ? ... c (ignored).
            if (first == '?')
            {
                size_t pos = prefix_len + 1;
                while (pos < bytes.size())
                {
                    const unsigned char ch = to_char(bytes[pos]);
                    if (ch == 'c')
                    {
                        out = {};
                        out.kind = TokenKind::ignored_sequence;
                        out.bytes_consumed = pos + 1;
                        return DecodeResult::produced;
                    }

                    if (!(ch == ';' || (ch >= '0' && ch <= '9')))
                    {
                        return DecodeResult::no_match;
                    }

                    ++pos;
                }

                return DecodeResult::need_more_data;
            }

            // Special sequences and win32-input-mode both start with digits/semicolons and have
            // distinctive terminators:
            // - CSI 2~ / 3~ / 5~ / 6~ (fallback insert/delete/page keys)
            // - CSI Vk;Sc;Uc;Kd;Cs;Rc _ (win32-input-mode)
            std::array<ParsedParam, 6> params{};
            size_t param_index = 0;
            uint32_t current = 0;
            bool current_present = false;

            size_t pos = prefix_len;
            while (pos < bytes.size())
            {
                const unsigned char ch = to_char(bytes[pos]);
                if (ch >= '0' && ch <= '9')
                {
                    current_present = true;
                    current = saturating_mul_add_10(current, static_cast<unsigned>(ch - '0'));
                    ++pos;
                    continue;
                }

                if (ch == ';')
                {
                    if (param_index < params.size())
                    {
                        params[param_index] = ParsedParam{ current_present, current };
                    }
                    ++param_index;
                    current = 0;
                    current_present = false;
                    ++pos;
                    continue;
                }

                break;
            }

            if (pos >= bytes.size())
            {
                // CSI introducer plus digits/semicolons but no terminator yet.
                return DecodeResult::need_more_data;
            }

            const unsigned char terminator = to_char(bytes[pos]);
            if (param_index < params.size())
            {
                params[param_index] = ParsedParam{ current_present, current };
            }

            if (terminator == '~')
            {
                // Fallback insert/delete/page keys.
                if (param_index != 0 || !params[0].present)
                {
                    return DecodeResult::no_match;
                }

                WORD vk = 0;
                switch (params[0].value)
                {
                case 2:
                    vk = VK_INSERT;
                    break;
                case 3:
                    vk = VK_DELETE;
                    break;
                case 5:
                    vk = VK_PRIOR;
                    break;
                case 6:
                    vk = VK_NEXT;
                    break;
                default:
                    return DecodeResult::no_match;
                }

                out = {};
                out.kind = TokenKind::key_event;
                out.bytes_consumed = pos + 1;
                out.key = make_simple_key_event(vk);
                return DecodeResult::produced;
            }

            if (terminator == '_')
            {
                // Win32-input-mode key serialization:
                // CSI Vk ; Sc ; Uc ; Kd ; Cs ; Rc _
                //
                // Default values follow the upstream implementation:
                // Vk/Sc/Uc/Kd/Cs default to 0, Rc defaults to 1.
                const uint32_t vk = params[0].present ? params[0].value : 0;
                const uint32_t sc = params[1].present ? params[1].value : 0;
                const uint32_t uc = params[2].present ? params[2].value : 0;
                const uint32_t kd = params[3].present ? params[3].value : 0;
                const uint32_t cs = params[4].present ? params[4].value : 0;
                const uint32_t rc = params[5].present ? params[5].value : 1;

                KEY_EVENT_RECORD key{};
                key.bKeyDown = kd != 0 ? TRUE : FALSE;
                key.wRepeatCount = saturate_word(rc);
                key.wVirtualKeyCode = saturate_word(vk);
                key.wVirtualScanCode = saturate_word(sc);
                key.uChar.UnicodeChar = saturate_wchar(uc);
                key.dwControlKeyState = static_cast<DWORD>(cs);

                out = {};
                out.kind = TokenKind::key_event;
                out.bytes_consumed = pos + 1;
                out.key = key;
                return DecodeResult::produced;
            }

            return DecodeResult::no_match;
        }
    }

    DecodeResult try_decode_vt(const std::span<const std::byte> bytes, DecodedToken& out) noexcept
    {
        out = {};
        if (bytes.empty())
        {
            return DecodeResult::no_match;
        }

        // Single ESC prefix is ambiguous: it could be a standalone Escape key or
        // the beginning of a longer VT sequence. Defer in that case.
        if (bytes.size() == 1 && bytes[0] == esc_byte)
        {
            return DecodeResult::need_more_data;
        }

        if (bytes[0] == esc_byte)
        {
            const unsigned char second = bytes.size() >= 2 ? to_char(bytes[1]) : 0;
            if (second == 'O')
            {
                return decode_ss3(bytes, out);
            }

            if (second == '[')
            {
                return decode_csi(bytes, out);
            }

            return DecodeResult::no_match;
        }

        if (bytes[0] == csi_byte)
        {
            return decode_csi(bytes, out);
        }

        return DecodeResult::no_match;
    }
}
