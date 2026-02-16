#pragma once

// VT input decoding helpers for the ConDrv replacement.
//
// In ConPTY scenarios, the hosting terminal can send key events encoded as
// "win32-input-mode" sequences (CSI ... _). It may also send classic VT escape
// sequences for special keys and startup control responses (DA1, focus events).
//
// This module parses a minimal subset of such sequences into KEY_EVENT_RECORDs
// or signals that the sequence should be ignored/consumed.

#include <Windows.h>

#include <array>
#include <cstddef>
#include <span>

namespace oc::condrv::vt_input
{
    enum class TokenKind : unsigned char
    {
        key_event,
        ignored_sequence,
        // `text_units` is produced by higher-level wrappers that fall back to
        // code-page decoding. `try_decode_vt` itself never produces this kind.
        text_units,
    };

    enum class DecodeResult : unsigned char
    {
        produced,
        need_more_data,
        no_match,
    };

    struct TextChunk final
    {
        std::array<wchar_t, 2> chars{};
        size_t char_count{};
        size_t bytes_consumed{};
    };

    struct DecodedToken final
    {
        TokenKind kind{ TokenKind::text_units };
        size_t bytes_consumed{};
        KEY_EVENT_RECORD key{};
        TextChunk text{};
    };

    // Attempts VT-first decoding:
    // - win32-input-mode: CSI Vk ; Sc ; Uc ; Kd ; Cs ; Rc _
    // - focus in/out: CSI I / CSI O (ignored)
    // - DA1 response: CSI ? ... c (ignored)
    // - basic fallback keys: arrows/home/end/ins/del/pgup/pgdn/F1-F4
    //
    // Returns `no_match` when the prefix is not a supported VT sequence.
    [[nodiscard]] DecodeResult try_decode_vt(std::span<const std::byte> bytes, DecodedToken& out) noexcept;
}

