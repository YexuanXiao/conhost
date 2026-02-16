# ConDrv Output VT Processing: Alternate Screen Buffer (DECSET 1049)

## Goal
Full-screen console applications (for example `vim`, `less`, `htop`) rely on the VT "alternate
screen buffer" mode to draw transient UI state without destroying the main console contents.

This microtask adds minimal, deterministic alternate-buffer semantics to the replacement's
in-memory `condrv::ScreenBuffer` model when `ENABLE_VIRTUAL_TERMINAL_PROCESSING` is enabled:

- `CSI ? 1049 h`: save main buffer state, switch to a cleared alternate buffer, home the cursor.
- `CSI ? 1049 l`: discard the alternate buffer and restore the saved main buffer state.

## Upstream Reference (Local Source Tree)
The inbox host defines and implements this mode in the VT adapter layer:

- Mode constant:
  - `src/terminal/adapter/DispatchTypes.hpp` (`DECPrivateMode(1049)`)
- Toggle behavior:
  - `src/terminal/adapter/adaptDispatch.cpp` (`_SetAlternateScreenBufferMode(...)`)
    - enable: `CursorSaveState(); UseAlternateScreenBuffer(...);`
    - disable: `UseMainScreenBuffer(); CursorRestoreState();`

## Replacement Design
### 1) Swap-based model (no scrollback)
The replacement keeps a single `ScreenBuffer` object per output handle. Alternate-buffer mode is
modeled by swapping the active cell storage and preserving a "main-screen backup" payload:

- Active cells live in `ScreenBuffer::_cells`.
- While the alternate screen buffer is active, `ScreenBuffer::_vt_main_backup` holds:
  - the main buffer cell vector
  - cursor position
  - active/default text attributes
  - cursor size/visibility
  - saved cursor state (`DECSC`/`CSI s`)
  - VT vertical margins (DECSTBM)
  - delayed wrap state (DECAWM "last column flag")
  - origin mode state (DECOM)

On entry (`CSI ?1049 h`):
- Allocate a fresh alternate cell vector sized to the current buffer and fill it with:
  - `fill_character` (currently space)
  - `fill_attributes` (the current text attributes)
- Move the current main `_cells` into `_vt_main_backup`.
- Activate the alternate buffer:
  - `_cells = alt_cells`
  - `_cursor_position = {0,0}`
  - `_saved_cursor_state.reset()`
  - `_vt_vertical_margins.reset()`
  - `_vt_delayed_wrap_position.reset()`

On exit (`CSI ?1049 l`):
- Restore the full saved main state from `_vt_main_backup` and clear it.
- The alternate buffer cell vector is discarded.

### 2) Allocation failure behavior
Entering the alternate screen buffer requires allocating a full cell vector. If allocation fails,
the operation is treated as a no-op and the model remains consistent.

Exiting the alternate screen buffer does not allocate.

### 3) Resize consistency
`ScreenBuffer::set_screen_buffer_size(...)` resizes:
- the active `_cells`, and
- the `_vt_main_backup->cells` vector when the alternate buffer is active,

preparing both resized vectors first and committing only when all allocations succeed. Cursor
position and VT vertical margins are clamped for both active and saved state after a successful
resize.

## Integration
`apply_text_to_screen_buffer(...)` recognizes `CSI h/l` mode toggles by numeric parameter value.
The CSI parser intentionally discards the private `?` marker, so the implementation matches on
`1049` directly (the same strategy already used for `25` cursor visibility).

Implementation files:
- `new/src/condrv/condrv_server.hpp` (`ScreenBuffer` API + `apply_text_to_screen_buffer`)
- `new/src/condrv/condrv_server.cpp` (`ScreenBuffer` storage + resize behavior)

## Tests
`new/tests/condrv_raw_io_tests.cpp`

- `test_write_console_vt_alt_buffer_1049_clears_and_restores_main`
  - writes content in main, enters alternate buffer and verifies it is cleared, writes in alt,
    exits alt and verifies main contents + cursor position are restored.
- `test_write_console_vt_alt_buffer_1049_restores_cursor_visibility`
  - hides the cursor in the alternate buffer and verifies cursor visibility restores on exit.

## Limitations / Follow-ups
- Only `DECSET/DECRST 1049` is implemented (no `?47`/`?1047` aliases yet).
- No scrollback semantics are modeled.
- The viewport/window rectangle is not altered by mode switches (cursor homing + content swap only).
- Other VT modes are modeled separately; 1049 preserves/restores the main-buffer origin-mode and
  delayed-wrap state on exit.
