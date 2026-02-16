# Renderer Text Measurement (Design)

## Problem Statement

The in-memory `ScreenBuffer` model is expressed in character-cell coordinates. A classic conhost window needs to map
those cells onto a pixel surface. That requires stable "cell metrics" derived from the selected font and the effective
display DPI:

- cell width/height in pixels
- baseline position within the cell (for text placement)
- underline position/thickness (for later decoration support)

This module is deliberately non-GUI so it can be unit-tested deterministically and used by both future renderer paths
(`GDI`, `D2D`, etc.) without entangling with a window/message pump.

## Upstream Reference (Local Source Ground Truth)

The upstream OpenConsole codebase computes cell metrics in its DirectWrite-backed Atlas renderer:

- `src/renderer/atlas/AtlasEngine.api.cpp`
  - `_resolveFontMetrics(...)`:
    - converts point size to pixels: `fontSizeInPx = fontSize / 72 * dpi`
    - uses `DWRITE_FONT_METRICS` (`ascent`, `descent`, `lineGap`, underline/strike metrics)
    - determines advance width from the glyph metrics of codepoint `'0'` (CSS "ch" unit behavior)
    - rounds to integer cell sizes and computes a baseline centered within the rounded cell height

The replacement reuses the same high-level approach for consistent layout semantics.

## Goals (This Increment)

1. Define a minimal renderer-facing interface for resolving font metrics from a font request.
2. Provide a DirectWrite-backed implementation that:
   - resolves the requested font family from the system font collection
   - falls back to `Consolas` when the family name is empty or missing
   - computes cell metrics from font-face design units and effective DPI
3. Keep the module non-GUI and unit-testable.

## Non-Goals (This Increment)

- Font fallback chains (comma-separated font lists, custom font sets).
- Full shaping/text layout measurement (`IDWriteTextLayout`) for arbitrary strings.
- Color glyphs, OpenType feature toggles, variable font axes.
- Renderer invalidation and painting (window integration is a later microtask).

## Design

### 1) Public Interface

`new/src/renderer/text_measurer.hpp` defines:

- `FontRequest`: `{ family_name, weight, style, size_points, dpi }`
- `FontMetrics`: resolved face name + `CellMetrics`
- `TextMeasurer`: abstract interface returning `std::expected<FontMetrics, core::Win32Error>`

The interface is intentionally small and stable: it returns the information needed to size the window and position text
without committing to a rendering backend.

### 2) DirectWrite Implementation

`new/src/renderer/dwrite_text_measurer.*` implements `TextMeasurer` using DirectWrite:

1. Create a shared `IDWriteFactory` and get the system font collection (`GetSystemFontCollection`).
2. Resolve the requested family via `FindFamilyName`. If missing, retry with the fallback family `Consolas`.
3. Select a matching font (`GetFirstMatchingFont`) and create an `IDWriteFontFace`.
4. Read `DWRITE_FONT_METRICS` and compute:
   - `font_size_px = size_points / 72 * dpi`
   - `design_units_per_px = font_size_px / designUnitsPerEm`
   - `advance_height = ascent + descent + lineGap`
5. Determine cell width from the `'0'` glyph advance width (`GetGlyphIndicesW` + `GetDesignGlyphMetrics`), falling back
   to `0.5em` if the glyph is missing.
6. Round to integer pixels:
   - `cell_width = round(advance_width)`
   - `cell_height = round(advance_height)`
7. Compute a baseline similar to upstream (center the rounded cell height around the font's ascent/descent):
   - `baseline = round(ascent + (lineGap + cell_height - advance_height) / 2)`

### 3) Error Handling

The interface returns `core::Win32Error` to keep error transport compact and consistent with the project-wide rule of
using enum-class errors. DirectWrite failures are mapped from `HRESULT` via `HRESULT_CODE` (with a generic failure code
when the low bits are 0).

COM ownership is handled with C++/WinRT's `winrt::com_ptr<T>` (part of the Windows SDK). This provides RAII semantics
for `IDWrite*` interfaces without introducing third-party dependencies.

No exceptions are required for normal operational failures here because callers may want to fall back to a different
measurement backend later.

## Tests

Added `new/tests/dwrite_text_measurer_tests.cpp`:

- Basic measurement (`Consolas`, 12pt, 96 DPI) returns non-zero cell size and a baseline in range.
- Missing face name falls back to `Consolas`.
- DPI scaling is approximately linear (96 -> 192 DPI yields ~2x cell metrics, within rounding tolerance).
- Point size scaling is approximately linear (12pt -> 24pt yields ~2x cell metrics, within rounding tolerance).

## Limitations / Follow-Ups

- Implement font-list parsing and a deterministic fallback policy (CSS-like `font-family` lists).
- Add text layout measurement APIs for cursor positioning and selection rectangles.
- Connect the cell metrics to a real windowed renderer (message pump + paint) and to model invalidation.
