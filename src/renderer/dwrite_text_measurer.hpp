#pragma once

// DirectWrite-backed implementation of `renderer::TextMeasurer`.
//
// This module is non-GUI and can be unit-tested. It resolves a font from the
// system font collection and computes console "cell metrics" from font-face
// design units, mirroring the approach used by the upstream Atlas renderer.

#include "renderer/text_measurer.hpp"

#include <dwrite.h>
#include <winrt/base.h>

#include <expected>
#include <memory>

namespace oc::renderer
{
    class DwriteTextMeasurer final : public TextMeasurer
    {
    public:
        [[nodiscard]] static std::expected<std::unique_ptr<TextMeasurer>, core::Win32Error> create() noexcept;

        [[nodiscard]] std::expected<FontMetrics, core::Win32Error> measure_font(const FontRequest& request) noexcept override;

    private:
        explicit DwriteTextMeasurer(
            winrt::com_ptr<IDWriteFactory> factory,
            winrt::com_ptr<IDWriteFontCollection> system_fonts) noexcept;

        winrt::com_ptr<IDWriteFactory> _factory;
        winrt::com_ptr<IDWriteFontCollection> _system_fonts;
    };
}
