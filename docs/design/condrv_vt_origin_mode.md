# ConDrv Output VT Processing: Origin Mode (DECOM, `CSI ? 6 h/l`)

## Goal
When `ENABLE_VIRTUAL_TERMINAL_PROCESSING` is enabled, VT applications may toggle the DEC private
origin mode (DECOM). Origin mode changes how cursor addressing behaves:

- CUP/HVP cursor positioning becomes relative to the current scrolling margins.
- Vertical cursor motion is constrained to the scrolling region.
- Toggling origin mode homes the cursor to the appropriate "home" position.

This microtask adds a minimal origin-mode implementation to the replacement's in-memory
`condrv::ScreenBuffer` model to improve compatibility with full-screen TUIs.

## Upstream Reference (Local Source Tree)
In the inbox host:

- Mode parameter definition:
  - `src/terminal/adapter/DispatchTypes.hpp` (`DECOM_OriginMode = DECPrivateMode(6)`)
- Mode toggle behavior:
  - `src/terminal/adapter/adaptDispatch.cpp` (`_ModeParamsHelper`)
    - sets `Mode::Origin`
    - homes cursor via `CursorPosition(1, 1)` on set and reset
- Cursor position logic (relative offsets + clamping):
  - `src/terminal/adapter/adaptDispatch.cpp` (`_CursorMovePosition`, `CursorPosition`)
- Cursor save/restore includes origin mode:
  - `src/terminal/adapter/adaptDispatch.cpp` (`CursorSaveState`, `CursorRestoreState`)

## Replacement Semantics (Simplified)
The replacement models origin mode as a boolean on `ScreenBuffer`:

- `CSI ? 6 h`: enable origin mode
- `CSI ? 6 l`: disable origin mode

Because the CSI parser does not preserve the private-mode marker (`?`), the implementation matches
on numeric parameter value `6`.

### Cursor homing on toggle
Toggling origin mode homes the cursor, matching upstream behavior:

- origin mode enabled: home row is the top scrolling margin (DECSTBM top), otherwise row 0
- origin mode disabled: home row is 0
- home column is always 0 in the replacement (horizontal margins are not modeled yet)

### CUP/HVP (`CSI H` / `CSI f`)
When origin mode is enabled, CUP row parameters are interpreted relative to the active DECSTBM
top margin:

- `CSI 1;1H` moves the cursor to `{x=0, y=top_margin}` when a scroll region is active
- the resulting `y` is clamped to the inclusive `[top_margin, bottom_margin]` range

When origin mode is disabled, CUP uses absolute buffer coordinates as before.

### Cursor relative motion (`CSI A/B/C/D`)
When origin mode is enabled, vertical cursor motion is clamped to the active DECSTBM region.
Horizontal motion remains clamped to the buffer width (no DECSLRM support yet).

### Cursor save/restore includes origin mode
DECSC/DECRC (`ESC 7` / `ESC 8`) and CSI save/restore (`CSI s` / `CSI u`) include the origin-mode
flag in the saved cursor state. Restoring cursor state restores origin mode and then clamps the
cursor to the appropriate range.

## Implementation
- Model state:
  - `new/src/condrv/condrv_server.hpp` (`ScreenBuffer` stores `vt_origin_mode_enabled`)
  - `new/src/condrv/condrv_server.cpp` (storage + mutation)
- VT application:
  - `new/src/condrv/condrv_server.hpp` (`apply_text_to_screen_buffer` applies DECOM and adjusts CUP/motion)

## Tests
`new/tests/condrv_raw_io_tests.cpp`

- `test_write_console_vt_origin_mode_homes_cursor_to_margin_top`
  - sets scroll margins, enables origin mode, writes `A` and verifies it appears at the top margin
  - disables origin mode, writes `B` and verifies it appears at row 0
- `test_write_console_vt_origin_mode_clamps_cursor_to_bottom_margin`
  - positions the cursor at the bottom margin, attempts to move down, and verifies output stays
    within the scrolling region

## Limitations / Follow-ups
- Horizontal margins (DECSLRM) are not modeled, so origin mode only affects the vertical (row) origin.
- The replacement stores cursor-save state in buffer coordinates; upstream stores row/column relative to
  page/margins. This is acceptable for the current in-memory model but may diverge in rare margin-change cases.

