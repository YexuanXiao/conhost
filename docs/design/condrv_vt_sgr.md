# ConDrv Output VT Processing: Minimal CSI Support

## Goal
When the output mode includes `ENABLE_VIRTUAL_TERMINAL_PROCESSING`, conhost interprets VT escape sequences (notably SGR color changes) instead of writing the raw escape bytes into the screen buffer.

The replacement needs a compatible in-memory `ScreenBuffer` model so that:

- buffer inspection APIs (`ReadConsoleOutputString`, `ReadConsoleOutput`) observe the rendered content, not the escape sequences
- SGR-modified attributes are reflected in `CONSOLE_ATTRIBUTE` reads

This microtask implements a minimal subset of VT processing for the `ScreenBuffer` write path:

- CSI SGR (`ESC[...m`) to update attributes
- CSI cursor positioning/movement (`H`/`f`, `A`/`B`/`C`/`D`) to update the cursor position

## Upstream Reference (Local Source Tree)
In the inbox host, VT processing is handled by the state machine and adapter dispatch:

- `src/host/_stream.cpp`: `WriteCharsVT(...)` uses the VT state machine for parsing and transforms output.
- `src/terminal/adapter/adaptDispatchGraphics.cpp`: `AdaptDispatch::SetGraphicsRendition(...)` applies SGR options to the active page attributes.

## Replacement Design
The replacement currently updates the buffer model via `apply_text_to_screen_buffer(...)` (shared by USER_DEFINED `ConsolepWriteConsole` and `CONSOLE_IO_RAW_WRITE`).

When `ENABLE_VIRTUAL_TERMINAL_PROCESSING` is set:

1. CSI sequences of the form `ESC[` ... `<final>` are consumed (not printed into the buffer).
2. A minimal subset is interpreted (others are ignored after being consumed):
   - `m` (SGR): updates the current text attributes.
   - `H`/`f` (CUP/HVP): moves the cursor to an absolute 1-based row/column.
   - `A`/`B`/`C`/`D` (CUU/CUD/CUF/CUB): moves the cursor relative to its current position.
   - `J` (ED): erases portions of the display by filling cells with spaces and the current attributes.
   - `K` (EL): erases portions of the current line by filling cells with spaces and the current attributes.
3. For SGR, a small mapping is applied to the legacy `USHORT` attribute model:
   - `0` resets to the screen buffer's default attributes.
   - `30-37` / `90-97` update the foreground RGB bits (and intensity for 90-97).
   - `40-47` / `100-107` update the background RGB bits (and intensity for 100-107).
   - when applying a new foreground/background color, the replacement clears the corresponding
     intensity bit before setting the new value (so `ESC[31m` clears the intensity bit set by
     `ESC[91m`, and similarly for background `41` vs `101`).
   - `39` resets the foreground RGB bits to the defaults.
   - `49` resets the background RGB bits to the defaults.
   - `1` and `22` approximate bold/normal by toggling `FOREGROUND_INTENSITY`.
   - `4` and `24` toggle underline via `COMMON_LVB_UNDERSCORE`.
   - `7` and `27` toggle reverse video via `COMMON_LVB_REVERSE_VIDEO`.
   - extended colors (`38/48`) are approximated to the nearest legacy palette entry
     (see `condrv_vt_sgr_extended_colors.md`).
4. Unsupported CSI sequences (and unsupported SGR params) are ignored after being consumed.

### Default Attributes Source
SGR "reset" needs a stable notion of defaults that is independent of the current attribute value (because SGR changes are persistent).

The replacement therefore stores:

- current attributes: `ScreenBuffer::text_attributes()`
- defaults: `ScreenBuffer::default_text_attributes()`

The defaults are initialized from the creation settings and updated via `ConsolepSetScreenBufferInfo` (which is the replacement's analog of `SetConsoleScreenBufferInfoEx` default updates).

## Implementation
- Buffer update path: `new/src/condrv/condrv_server.hpp` (`apply_text_to_screen_buffer`)
- Default attribute storage: `new/src/condrv/condrv_server.hpp` + `new/src/condrv/condrv_server.cpp` (`ScreenBuffer`)

## Tests
`new/tests/condrv_raw_io_tests.cpp`

- `test_write_console_vt_sgr_updates_attributes_and_strips_sequences`
  - writes `A ESC[31m B ESC[0m C` with VT processing enabled
  - verifies the buffer contains `ABC` (no escape chars)
  - verifies attributes for `B` are red and `A`/`C` are defaults
- `test_write_console_vt_sgr_reverse_video_sets_common_lvb_reverse_video`
  - writes `A ESC[7m B ESC[27m C` with VT processing enabled
  - verifies the reverse video bit is present only on `B`
- `test_write_console_vt_sgr_underline_sets_common_lvb_underscore`
  - writes `A ESC[4m B ESC[24m C` with VT processing enabled
  - verifies the underline bit is present only on `B`
- `test_write_console_vt_sgr_normal_color_clears_bright_foreground_intensity`
  - writes `A ESC[91m B ESC[31m C` with VT processing enabled
  - verifies the intensity bit is cleared when switching back to normal colors
- `test_write_console_vt_sgr_normal_color_clears_bright_background_intensity`
  - writes `A ESC[101m B ESC[41m C` with VT processing enabled
  - verifies the background intensity bit is cleared when switching back to normal colors
- `test_write_console_vt_sgr_extended_palette_index_sets_bright_red_foreground`
  - writes `A ESC[38;5;9m B ESC[0m C` with VT processing enabled
  - verifies xterm indexed colors update legacy attributes (bright red)
- `test_write_console_vt_sgr_extended_truecolor_sets_bright_red_foreground`
  - writes `A ESC[38;2;255;0;0m B ESC[0m C` with VT processing enabled
  - verifies truecolor is approximated to the nearest legacy palette entry
- `test_write_console_vt_sgr_extended_palette_index_sets_blue_background`
  - writes `A ESC[48;5;4m B ESC[0m C` with VT processing enabled
  - verifies indexed background colors update legacy attributes (blue background)
- `test_write_console_vt_cup_moves_cursor`
  - writes `A ESC[2;3H Z` with VT processing enabled
  - verifies `Z` is placed at coordinate `(2, 1)` (CUP is 1-based)
- `test_write_console_vt_ed_clears_screen`
  - writes `A ESC[2J Z` with VT processing enabled
  - verifies ED clears the first cell while leaving the cursor position unchanged for the following write
- `test_write_console_vt_el_clears_to_end_of_line`
  - writes `HELLO ESC[1;3H ESC[K` with VT processing enabled
  - verifies EL clears from the cursor to the end of the line

## Limitations / Follow-ups
- This is not a full VT implementation. Extended colors are approximated to the nearest legacy 16-color
  attribute (see `condrv_vt_sgr_extended_colors.md`), and most OSC/DEC private modes are not implemented yet
  (except window-title OSC sequences; see `condrv_vt_osc_window_title.md`, and DSR/CPR; see `condrv_vt_dsr.md`).
- The replacement currently consumes CSI sequences it doesn't implement, matching the typical "no visible output" behavior, but additional sequences should be supported incrementally with dedicated tests.
