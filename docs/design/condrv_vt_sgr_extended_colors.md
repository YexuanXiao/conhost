# ConDrv Output VT Processing: SGR Extended Colors (38/48) Approximation

## Goal
Many TUIs emit SGR extended color sequences:

- indexed colors: `CSI 38;5;<n> m` (foreground), `CSI 48;5;<n> m` (background)
- truecolor: `CSI 38;2;<r>;<g>;<b> m`, `CSI 48;2;<r>;<g>;<b> m`

The inbox conhost supports extended colors internally, but the replacement's `ScreenBuffer` cell
model currently stores legacy `USHORT` attributes (16-color, bitfield) only.

This microtask implements a **lossy but deterministic** approximation:

- consume the extended SGR sequences (do not render them to the buffer)
- map the requested color to the nearest entry in the current 16-color table and apply that legacy
  attribute to subsequent output

This improves compatibility for modern console apps while keeping the data model small and the
implementation testable.

## Upstream Reference (Local Source Tree)
Extended color parsing and application is implemented in the upstream adapter:

- `src/terminal/adapter/adaptDispatchGraphics.cpp`: `AdaptDispatch::SetGraphicsRendition(...)`
  - supports `38;5;n`, `48;5;n`, `38;2;r;g;b`, `48;2;r;g;b`
  - stores colors in `TextAttribute` with an extended-color representation

## Replacement Semantics
All behavior below applies only when `ENABLE_VIRTUAL_TERMINAL_PROCESSING` is enabled.

### 1) Indexed colors (`38;5;n` / `48;5;n`)
- For `n` in `[0, 15]`:
  - interpret the index using the standard VT base palette semantics (same ordering as `30-37` and
    `90-97`), then map to legacy attributes.
- For `n` in `[16, 255]`:
  - compute the corresponding xterm 256-color RGB value:
    - `16-231`: 6x6x6 color cube
    - `232-255`: grayscale ramp
  - pick the nearest entry from `ScreenBuffer::color_table()` (Euclidean distance in RGB space)
  - apply the resulting legacy palette index to the foreground/background bits.

### 2) Truecolor (`38;2;r;g;b` / `48;2;r;g;b`)
- Clamp `r`, `g`, `b` to `[0,255]`.
- Choose the nearest entry in `ScreenBuffer::color_table()`.
- Apply the legacy palette index as foreground/background attributes.

### 3) Limitations
- This is not full extended-color fidelity: the screen buffer stores only legacy 16-color attributes.
- The approximation respects the current 16-color table (so custom palettes influence mapping), but
  it cannot preserve the original 24-bit color value.

## Implementation
- `new/src/condrv/condrv_server.hpp` (`apply_text_to_screen_buffer(...)` -> `apply_sgr(...)`)

## Tests
`new/tests/condrv_raw_io_tests.cpp`

- `test_write_console_vt_sgr_extended_palette_index_sets_bright_red_foreground`
- `test_write_console_vt_sgr_extended_truecolor_sets_bright_red_foreground`
- `test_write_console_vt_sgr_extended_palette_index_sets_blue_background`

