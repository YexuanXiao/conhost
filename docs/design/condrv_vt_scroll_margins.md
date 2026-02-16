# ConDrv Output VT Processing: Scroll Margins and Vertical Line Operations

## Goal
When output mode includes `ENABLE_VIRTUAL_TERMINAL_PROCESSING`, full-screen terminal apps rely on
VT scrolling primitives instead of directly manipulating the console screen buffer via Win32 APIs.

This microtask implements a compact but high-impact VT output subset in the replacement's
in-memory `ScreenBuffer` model:

- DECSTBM (scroll region): `CSI <top> ; <bottom> r`
- SU/SD (scroll within margins): `CSI <n> S` / `CSI <n> T`
- IL/DL (insert/delete lines within margins): `CSI <n> L` / `CSI <n> M`
- IND/RI (index/reverse index): `ESC D` / `ESC M`

The primary observable effect is that line feeds at the bottom margin scroll only within the
defined region, allowing "fixed" status bars above/below the scrolling content.

## Upstream Reference (Local Source Tree)
The inbox conhost implements these behaviors in the VT adapter layer:

- DECSTBM parsing/dispatch:
  - `src/terminal/parser/OutputStateMachineEngine.*` (`DECSTBM_SetTopBottomMargins`)
  - `src/terminal/adapter/adaptDispatch.cpp`:
    - `AdaptDispatch::SetTopBottomScrollingMargins(...)`
    - `AdaptDispatch::_DoSetTopBottomScrollingMargins(...)`
- Vertical scrolling and line feed integration:
  - `src/terminal/adapter/adaptDispatch.cpp`:
    - `AdaptDispatch::_DoLineFeed(...)` (respects margins)
    - `AdaptDispatch::_ScrollRectVertically(...)` (scroll helper)
    - `AdaptDispatch::InsertLine(...)` / `AdaptDispatch::DeleteLine(...)` (IL/DL)
    - `AdaptDispatch::ScrollUp(...)` / `AdaptDispatch::ScrollDown(...)` (SU/SD)

## Replacement Design
### 1) Persisted DECSTBM state per `ScreenBuffer`
The replacement stores the active DECSTBM top/bottom margins on the `ScreenBuffer` object, because
it is a stateful VT feature that must persist across multiple `WriteConsole` calls.

- `ScreenBuffer::VtVerticalMargins` holds `{top,bottom}` in 0-based buffer coordinates (inclusive).
- `std::nullopt` means "no margins" (the full buffer height is scrollable).

### 2) Applying margins during line feed and IND/RI
`apply_text_to_screen_buffer(...)` resolves the active scroll region and uses it for:

- `\\n` line feeds (when `ENABLE_PROCESSED_OUTPUT` is enabled):
  - if the cursor is within the scroll region and at the bottom margin, scroll only within the region
  - otherwise, move the cursor down (and scroll the full buffer only when leaving the bottom)
- `ESC D` (IND): equivalent to a line feed that does not imply a carriage return
- `ESC M` (RI): reverse-index; moves up one line, scrolling down within the region when at the top margin

### 3) Scroll and line operations via `ScreenBuffer::scroll_screen_buffer`
All operations are implemented by mapping VT intents onto `ScreenBuffer::scroll_screen_buffer(...)`:

- SU/SD: scroll the entire active scroll region by `n` rows.
- IL/DL: scroll the sub-rectangle from the cursor row to the bottom margin, inserting or deleting `n` rows.

New lines introduced by scrolling are filled with spaces using the current text attributes.

## Implementation
- VT state storage:
  - `new/src/condrv/condrv_server.hpp` (`ScreenBuffer::VtVerticalMargins`, accessors)
  - `new/src/condrv/condrv_server.cpp` (storage, resize clamping)
- VT output application:
  - `new/src/condrv/condrv_server.hpp` (`apply_text_to_screen_buffer(...)`)

## Tests
`new/tests/condrv_raw_io_tests.cpp`

- `test_write_console_vt_decstbm_linefeed_scrolls_within_margins`
  - seeds distinct markers on rows 1-5, sets `CSI 2;4 r`, emits a line feed at the bottom margin
  - verifies only rows 2-4 scroll
- `test_write_console_vt_su_sd_scrolls_within_margins`
  - verifies `CSI S` and `CSI T` scroll only within the configured region
- `test_write_console_vt_il_inserts_lines_within_margins`
  - verifies `CSI L` inserts a blank line at the cursor row and shifts content down within margins
- `test_write_console_vt_dl_deletes_lines_within_margins`
  - verifies `CSI M` deletes the cursor row and shifts content up within margins
- `test_write_console_vt_ind_preserves_column`
  - verifies `ESC D` performs a line feed without resetting the cursor column

## Limitations / Follow-ups
- Horizontal margins (DECSLRM) are not modeled, so scrolling always spans the full buffer width.
- Origin mode (DECOM, `CSI ? 6 h/l`) is implemented separately; see `condrv_vt_origin_mode.md`.
- Extended color handling remains out of scope.
- Partial-width scrolling is deferred.
