#include "renderer/dwrite_text_measurer.hpp"

#include "core/assert.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace oc::renderer
{
    namespace
    {
        constexpr wchar_t k_fallback_face_name[] = L"Consolas";

        [[nodiscard]] constexpr core::Win32Error win32_error_from_hresult(const HRESULT hr) noexcept
        {
            const DWORD code = static_cast<DWORD>(HRESULT_CODE(hr));
            return core::from_dword(code == 0 ? ERROR_GEN_FAILURE : code);
        }

        [[nodiscard]] constexpr DWRITE_FONT_WEIGHT to_dwrite_weight(const FontWeight weight) noexcept
        {
            return static_cast<DWRITE_FONT_WEIGHT>(static_cast<uint16_t>(weight));
        }

        [[nodiscard]] constexpr DWRITE_FONT_STYLE to_dwrite_style(const FontStyle style) noexcept
        {
            switch (style)
            {
            case FontStyle::italic:
                return DWRITE_FONT_STYLE_ITALIC;
            case FontStyle::oblique:
                return DWRITE_FONT_STYLE_OBLIQUE;
            case FontStyle::normal:
            default:
                return DWRITE_FONT_STYLE_NORMAL;
            }
        }

        [[nodiscard]] int round_to_int(const float value) noexcept
        {
            return static_cast<int>(std::lroundf(value));
        }
    }

    DwriteTextMeasurer::DwriteTextMeasurer(
        winrt::com_ptr<IDWriteFactory> factory,
        winrt::com_ptr<IDWriteFontCollection> system_fonts) noexcept :
        _factory(std::move(factory)),
        _system_fonts(std::move(system_fonts))
    {
    }

    std::expected<std::unique_ptr<TextMeasurer>, core::Win32Error> DwriteTextMeasurer::create() noexcept
    {
        winrt::com_ptr<IDWriteFactory> factory;
        HRESULT hr = ::DWriteCreateFactory(
            DWRITE_FACTORY_TYPE_SHARED,
            __uuidof(IDWriteFactory),
            reinterpret_cast<IUnknown**>(factory.put()));
        if (FAILED(hr))
        {
            return std::unexpected(win32_error_from_hresult(hr));
        }

        winrt::com_ptr<IDWriteFontCollection> system_fonts;
        hr = factory->GetSystemFontCollection(system_fonts.put(), FALSE);
        if (FAILED(hr))
        {
            return std::unexpected(win32_error_from_hresult(hr));
        }

        try
        {
            return std::unique_ptr<TextMeasurer>(
                new DwriteTextMeasurer(std::move(factory), std::move(system_fonts)));
        }
        catch (...)
        {
            return std::unexpected(core::from_dword(ERROR_OUTOFMEMORY));
        }
    }

    std::expected<FontMetrics, core::Win32Error> DwriteTextMeasurer::measure_font(const FontRequest& request) noexcept
    {
        if (!_factory || !_system_fonts)
        {
            return std::unexpected(core::from_dword(ERROR_INVALID_STATE));
        }

        std::wstring face_name = request.family_name;
        if (face_name.empty())
        {
            face_name = k_fallback_face_name;
        }

        float dpi = request.dpi;
        if (!(dpi > 0.0f))
        {
            dpi = 96.0f;
        }

        float size_points = request.size_points;
        if (!(size_points > 0.0f))
        {
            size_points = 12.0f;
        }

        size_points = std::clamp(size_points, 1.0f, 1000.0f);

        uint32_t family_index = 0;
        BOOL family_exists = FALSE;
        HRESULT hr = _system_fonts->FindFamilyName(face_name.c_str(), &family_index, &family_exists);
        if (FAILED(hr))
        {
            return std::unexpected(win32_error_from_hresult(hr));
        }

        if (!family_exists && face_name != k_fallback_face_name)
        {
            face_name = k_fallback_face_name;
            hr = _system_fonts->FindFamilyName(face_name.c_str(), &family_index, &family_exists);
            if (FAILED(hr))
            {
                return std::unexpected(win32_error_from_hresult(hr));
            }
        }

        if (!family_exists)
        {
            return std::unexpected(win32_error_from_hresult(DWRITE_E_NOFONT));
        }

        winrt::com_ptr<IDWriteFontFamily> family;
        hr = _system_fonts->GetFontFamily(family_index, family.put());
        if (FAILED(hr))
        {
            return std::unexpected(win32_error_from_hresult(hr));
        }

        winrt::com_ptr<IDWriteFont> font;
        hr = family->GetFirstMatchingFont(
            to_dwrite_weight(request.weight),
            DWRITE_FONT_STRETCH_NORMAL,
            to_dwrite_style(request.style),
            font.put());
        if (FAILED(hr))
        {
            return std::unexpected(win32_error_from_hresult(hr));
        }

        winrt::com_ptr<IDWriteFontFace> face;
        hr = font->CreateFontFace(face.put());
        if (FAILED(hr))
        {
            return std::unexpected(win32_error_from_hresult(hr));
        }

        DWRITE_FONT_METRICS metrics{};
        face->GetMetrics(&metrics);
        OC_ASSERT(metrics.designUnitsPerEm != 0);

        // Point sizes are commonly treated at a 72 DPI scale (including by OpenType),
        // whereas DirectWrite commonly operates in DIPs (96 DPI). We return pixel-sized
        // cell metrics by multiplying by the effective display DPI.
        const float font_size_px = size_points / 72.0f * dpi;
        const float design_units_per_px = font_size_px / static_cast<float>(metrics.designUnitsPerEm);

        const float ascent = static_cast<float>(metrics.ascent) * design_units_per_px;
        const float descent = static_cast<float>(metrics.descent) * design_units_per_px;
        const float line_gap = static_cast<float>(metrics.lineGap) * design_units_per_px;
        const float underline_position = static_cast<float>(-metrics.underlinePosition) * design_units_per_px;
        const float underline_thickness = static_cast<float>(metrics.underlineThickness) * design_units_per_px;
        const float advance_height = ascent + descent + line_gap;

        // Match the upstream Atlas renderer choice: use the "0" advance width ("ch" unit in CSS).
        float advance_width = 0.5f * font_size_px;
        {
            static constexpr uint32_t codepoint = '0';
            uint16_t glyph_index = 0;

            hr = face->GetGlyphIndicesW(&codepoint, 1, &glyph_index);
            if (SUCCEEDED(hr) && glyph_index != 0)
            {
                DWRITE_GLYPH_METRICS glyph_metrics{};
                hr = face->GetDesignGlyphMetrics(&glyph_index, 1, &glyph_metrics, FALSE);
                if (SUCCEEDED(hr))
                {
                    advance_width = static_cast<float>(glyph_metrics.advanceWidth) * design_units_per_px;
                }
            }
        }

        float cell_width_f = std::max(1.0f, std::roundf(advance_width));
        float cell_height_f = std::max(1.0f, std::roundf(advance_height));
        const float baseline_f = std::roundf(ascent + (line_gap + cell_height_f - advance_height) / 2.0f);

        const float underline_pos_f = std::roundf(baseline_f + underline_position);
        const float underline_thickness_f = std::max(1.0f, std::roundf(underline_thickness));

        CellMetrics cell{};
        cell.width_px = round_to_int(cell_width_f);
        cell.height_px = round_to_int(cell_height_f);
        cell.baseline_px = std::clamp(round_to_int(baseline_f), 0, std::max(0, cell.height_px));
        cell.underline_position_px = round_to_int(underline_pos_f);
        cell.underline_thickness_px = round_to_int(underline_thickness_f);

        FontMetrics result{};
        result.resolved_family_name = std::move(face_name);
        result.weight = request.weight;
        result.style = request.style;
        result.size_points = size_points;
        result.dpi = dpi;
        result.cell = cell;
        return result;
    }
}
