# ConDrv Output VT Processing: Cursor Save/Restore

## Goal
When `ENABLE_VIRTUAL_TERMINAL_PROCESSING` is enabled, conhost interprets VT escape sequences instead of writing the raw escape bytes into the screen buffer. Cursor save/restore is a common feature used by interactive terminal applications to temporarily move the cursor and then return to the original location.

The replacement's in-memory `ScreenBuffer` model therefore needs to consume and apply cursor save/restore sequences so that:

- buffer inspection APIs (`ReadConsoleOutputString`, `ReadConsoleOutput`) observe the rendered content (no literal `ESC`/`[` bytes)
- cursor-sensitive applications that use save/restore place text in the expected coordinates
- restored text attributes match the saved state (legacy `USHORT` attributes)

## Upstream Reference (Local Source Tree)
In the inbox host, cursor save/restore is handled by the VT output parser and dispatch:

- `src/terminal/parser/OutputStateMachineEngine.hpp` and `src/terminal/parser/OutputStateMachineEngine.cpp`
  - ESC dispatch: `DECSC_CursorSave` (`ESC 7`) and `DECRC_CursorRestore` (`ESC 8`)
  - CSI dispatch: `ANSISYSRC_CursorRestore` (`ESC [ u`)
  - `ESC [ s` is ambiguous in the upstream stack (commented as DECSLRM vs ANSISYSSC depending on DECLRMM state)
- `src/terminal/adapter/adaptDispatch.cpp`
  - `AdaptDispatch::CursorSaveState()` and `AdaptDispatch::CursorRestoreState()` capture and restore a broad "cursor state"

## Replacement Design
The replacement does not implement the full VT state machine. Instead, it updates the buffer model in `apply_text_to_screen_buffer(...)` which is shared by USER_DEFINED `ConsolepWriteConsole` and `CONSOLE_IO_RAW_WRITE`.

When `ENABLE_VIRTUAL_TERMINAL_PROCESSING` is set:

1. The following sequences are consumed (not printed into the buffer):
   - `ESC 7` (DECSC): save cursor state
   - `ESC 8` (DECRC): restore cursor state
   - `ESC [ s` (CSI `s`): save cursor state when there are no parameters
   - `ESC [ u` (CSI `u`): restore cursor state
2. A small subset of the upstream cursor state is modeled:
   - cursor position
   - the current legacy `USHORT` text attributes
   - delayed wrap pending (the "last column flag" from DECAWM)
   - origin mode enabled (DECOM)
3. Save stores `{ cursor_position, text_attributes, delayed_wrap_pending, origin_mode_enabled }`.
4. Restore updates the active cursor, active attributes, origin mode, and delayed-wrap state for
   subsequent writes.
5. Saved cursor positions are clamped to the current buffer bounds (and, when origin mode is
   enabled, clamped to the active DECSTBM margins) when restoring.

### Why Treat `ESC [ s` As Save-Cursor?
In the inbox implementation, the parser routes `CSI s` through a path that may represent DECSLRM or save-cursor depending on state. The replacement currently does not model left/right margins, so it treats the no-parameter form of `s` as save-cursor for compatibility with common terminal usage.

## Implementation
- Cursor-state storage:
  - `new/src/condrv/condrv_server.hpp`: `ScreenBuffer` declares an optional saved cursor state.
  - `new/src/condrv/condrv_server.cpp`: implements `ScreenBuffer::save_cursor_state(...)` and `ScreenBuffer::restore_cursor_state(...)`.
- VT consumption and application:
  - `new/src/condrv/condrv_server.hpp`: `apply_text_to_screen_buffer(...)` consumes `ESC7`/`ESC8` and applies CSI `s/u`.

## Tests
`new/tests/condrv_raw_io_tests.cpp`

- `test_write_console_vt_csi_save_restore_cursor_state`
  - verifies `ESC[s`/`ESC[u` restore both cursor position and attributes.
- `test_write_console_vt_decsc_decrc_save_restore_cursor_state`
  - verifies `ESC7`/`ESC8` restore both cursor position and attributes.

## Limitations / Follow-ups
- The upstream saved "cursor state" includes more state than the replacement models (for example,
  charset selections). The replacement currently models only the state required for buffer
  inspection and basic cursor positioning.
- `CSI s` with parameters (DECSLRM left/right margin setting) is not implemented.
