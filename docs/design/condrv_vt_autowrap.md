# ConDrv Output VT Processing: Autowrap and Delayed Wrap (DECAWM)

## Goal
When output mode includes `ENABLE_VIRTUAL_TERMINAL_PROCESSING`, applications expect the host to
emulate VT autowrap semantics (DECAWM, `CSI ? 7 h/l`) and the related "delayed EOL wrap" behavior
(sometimes described as the "last column flag").

This microtask implements a compact, deterministic version of that behavior in the replacement's
in-memory `condrv::ScreenBuffer` model so TUIs that write to the final column behave like conhost.

## Upstream Reference (Local Source Tree)
In the inbox conhost implementation:

- Mode parameter definition:
  - `src/terminal/adapter/DispatchTypes.hpp` (`DECAWM_AutoWrapMode = DECPrivateMode(7)`)
- Write path delayed-wrap handling:
  - `src/terminal/adapter/adaptDispatch.cpp` (`AdaptDispatch::_WriteToBuffer`)
    - checks `cursor.IsDelayedEOLWrap()` before writing the next glyph
    - clamps at the final column and sets `cursor.DelayEOLWrap()` when autowrap is enabled
- Reset points:
  - `src/terminal/adapter/adaptDispatch.cpp` resets delayed wrap for ED/EL/ECH/ICH/DCH and for DECAWM

## Replacement Semantics (Simplified but Compatible)
### 1) Autowrap mode toggle (`CSI ? 7 h/l`)
The replacement tracks VT autowrap as state on `ScreenBuffer`:

- default: enabled
- `CSI ? 7 h`: enable autowrap
- `CSI ? 7 l`: disable autowrap

Changing autowrap resets any pending delayed wrap state.

### 2) Delayed wrap ("last column flag")
When VT processing is enabled and autowrap is enabled:

1. Printing a printable character in the final column:
   - writes the cell at the final column
   - clamps the cursor at the final column
   - records a "delayed wrap position" (the current cursor position)
2. Before printing the next printable character:
   - if a delayed wrap position is recorded and the cursor is still at that position:
     - perform a wrap (`advance_line`: carriage return + line feed, respecting DECSTBM scrolling margins)
   - clear the delayed wrap position

If the cursor moves away from the recorded delayed-wrap position (for example via `\\r` or a CUP
sequence), the next printable character clears the pending wrap without wrapping.

### 3) Reset rules
To match upstream expectations, the replacement resets delayed wrap state for a subset of control
sequences that mutate cells without moving the cursor:

- ED/EL (`CSI J` / `CSI K`)
- ICH/DCH/ECH (`CSI @` / `CSI P` / `CSI X`)

Resizing the screen buffer also clears delayed wrap state (active + saved main buffer while in
alternate-screen mode).

### 4) Cursor save/restore includes delayed wrap
The inbox host preserves delayed wrap in cursor save/restore state. The replacement stores a
boolean "delayed wrap pending" flag alongside the saved cursor position/attributes for:

- DECSC/DECRC (`ESC 7` / `ESC 8`)
- CSI save/restore (`CSI s` / `CSI u`, no-parameter `s`)

When restoring cursor state with the delayed-wrap flag set, the delayed wrap position becomes the
restored cursor position.

## Implementation
- Model state:
  - `new/src/condrv/condrv_server.hpp` (`ScreenBuffer` stores autowrap + delayed-wrap position)
  - `new/src/condrv/condrv_server.cpp` (resets on resize; stored/restored across 1049 mode switches)
- VT application:
  - `new/src/condrv/condrv_server.hpp` (`apply_text_to_screen_buffer` implements delayed wrap + DECAWM)

## Tests
`new/tests/condrv_raw_io_tests.cpp`

- `test_write_console_vt_delayed_wrap_allows_carriage_return_before_wrap`
  - fills the final column, emits `\\r`, then writes another character and verifies it stays on the
    original row (no premature wrap)
- `test_write_console_vt_decawm_disable_prevents_wrap_and_overwrites_last_column`
  - disables autowrap with `CSI ? 7 l` and verifies output overwrites the final column instead of
    flowing to the next line

## Limitations / Follow-ups
- Wide glyphs and grapheme-cluster width are not modeled; the replacement treats each UTF-16 code
  unit as occupying one cell.
- Horizontal margins (DECSLRM) are not modeled.
- Origin mode (DECOM, `CSI ? 6 h/l`) is implemented separately; see `condrv_vt_origin_mode.md`.
