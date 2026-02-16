# ConDrv Output VT Processing: Cursor Visibility (DECTCEM)

## Goal
Console clients commonly toggle cursor visibility while drawing full-screen UIs. When `ENABLE_VIRTUAL_TERMINAL_PROCESSING` is enabled, conhost interprets VT mode sequences that enable/disable the text cursor.

The replacement needs to reflect this in the in-memory `ScreenBuffer` cursor state so that:

- `ConsolepGetCursorInfo` reports visibility consistent with VT output
- buffer model state matches what a renderer would eventually consume

## Upstream Reference (Local Source Tree)
- `src/terminal/adapter/adaptDispatch.cpp`
  - `DispatchTypes::ModeParams::DECTCEM_TextCursorEnableMode` (DEC Private Mode 25)
  - `Cursor().SetIsVisible(enable)` in the SetMode/ResetMode handling

The corresponding VT sequences are:

- show cursor: `ESC [ ? 25 h`
- hide cursor: `ESC [ ? 25 l`

## Replacement Design
The replacement applies VT output sequences in `apply_text_to_screen_buffer(...)` (used by USER_DEFINED `ConsolepWriteConsole` and `CONSOLE_IO_RAW_WRITE`).

When `ENABLE_VIRTUAL_TERMINAL_PROCESSING` is set:

1. CSI mode toggles with final byte `h`/`l` are consumed (not printed).
2. If any parameter equals `25`, the active screen buffer's cursor visibility is toggled:
   - `h` -> visible
   - `l` -> hidden

The replacement currently keys only on the numeric parameter value and does not preserve the `?` private-mode marker in the CSI parser. This is sufficient for DECTCEM while keeping the parser small.

## Implementation
- `new/src/condrv/condrv_server.hpp`
  - `apply_text_to_screen_buffer(...)`: handles CSI `h`/`l` with parameter `25` by updating `ScreenBuffer::cursor_visible`.

## Tests
`new/tests/condrv_raw_io_tests.cpp`

- `test_write_console_vt_dectcem_toggles_cursor_visibility`
  - writes `ESC[?25l` then verifies `ConsolepGetCursorInfo.Visible == FALSE`
  - writes `ESC[?25h` then verifies `ConsolepGetCursorInfo.Visible == TRUE`

## Limitations / Follow-ups
- Other DEC private modes and ANSI standard modes are not modeled yet.
- The CSI parser currently ignores private-mode leader bytes (`?`, `>`, etc.). If more DEC private mode support is added, the parser should record the leader to avoid ambiguity.

