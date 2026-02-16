#pragma once

// Renderer text measurement interfaces.
//
// The replacement needs to render the in-memory console model in a classic
// conhost window. Layout depends on "cell metrics" (width/height/baseline)
// derived from the selected font and DPI. This header defines the minimal
// stable interface for resolving font metrics.

#include "core/exception.hpp"

#include <cstdint>
#include <expected>
#include <string>

namespace oc::renderer
{
    enum class FontWeight : uint16_t
    {
        thin = 100,
        extra_light = 200,
        light = 300,
        normal = 400,
        medium = 500,
        semi_bold = 600,
        bold = 700,
        extra_bold = 800,
        black = 900,
    };

    enum class FontStyle : uint8_t
    {
        normal = 0,
        italic = 1,
        oblique = 2,
    };

    struct FontRequest final
    {
        // Requested face name. The implementation may fall back to an installed
        // monospace font when the name is empty or cannot be resolved.
        std::wstring family_name;

        FontWeight weight{ FontWeight::normal };
        FontStyle style{ FontStyle::normal };

        // Font size in typographic points. This mirrors the behavior of the
        // upstream DirectWrite renderer code, which converts points to pixels
        // using the effective display DPI.
        float size_points{ 12.0f };

        // Effective display DPI (96 is "1:1" with DIPs).
        float dpi{ 96.0f };
    };

    struct CellMetrics final
    {
        int width_px{};
        int height_px{};
        int baseline_px{};
        int underline_position_px{};
        int underline_thickness_px{};
    };

    struct FontMetrics final
    {
        std::wstring resolved_family_name;
        FontWeight weight{ FontWeight::normal };
        FontStyle style{ FontStyle::normal };
        float size_points{};
        float dpi{};
        CellMetrics cell{};
    };

    class TextMeasurer
    {
    public:
        TextMeasurer() = default;
        virtual ~TextMeasurer() = default;

        TextMeasurer(const TextMeasurer&) = delete;
        TextMeasurer& operator=(const TextMeasurer&) = delete;
        TextMeasurer(TextMeasurer&&) = delete;
        TextMeasurer& operator=(TextMeasurer&&) = delete;

        [[nodiscard]] virtual std::expected<FontMetrics, core::Win32Error> measure_font(
            const FontRequest& request) noexcept = 0;
    };
}

