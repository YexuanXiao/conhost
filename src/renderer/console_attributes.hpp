#pragma once

// Console text-attribute decoding helpers.
//
// The classic Windows console exposes legacy 16-color attributes in a `USHORT` bitfield:
// - low 4 bits: foreground palette index (0..15)
// - high 4 bits: background palette index (0..15)
// - high bits: COMMON_LVB_* flags (reverse video, underline, DBCS lead/trail, ...)
//
// The ConDrv replacement stores and snapshots those attributes. The renderer needs a small,
// deterministic decoder so UI code doesn't duplicate bit twiddling in multiple places.

#include <Windows.h>

#include <cstdint>
#include <utility>

namespace oc::renderer
{
    struct DecodedAttributes final
    {
        std::uint8_t foreground_index{};
        std::uint8_t background_index{};
        bool underline{};
    };

    [[nodiscard]] constexpr DecodedAttributes decode_attributes(const USHORT attributes) noexcept
    {
        std::uint8_t fg = static_cast<std::uint8_t>(attributes & 0x0F);
        std::uint8_t bg = static_cast<std::uint8_t>((attributes >> 4) & 0x0F);

        if ((attributes & COMMON_LVB_REVERSE_VIDEO) != 0)
        {
            std::swap(fg, bg);
        }

        DecodedAttributes decoded{};
        decoded.foreground_index = fg;
        decoded.background_index = bg;
        decoded.underline = (attributes & COMMON_LVB_UNDERSCORE) != 0;
        return decoded;
    }
}

