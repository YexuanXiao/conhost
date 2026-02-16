# ConDrv Output VT Processing: Horizontal Editing (ICH/DCH/ECH)

## Goal
Full-screen console applications often update parts of a line in-place using VT "horizontal editing"
controls instead of rewriting whole lines.

This microtask implements a compact subset of VT output handling for the replacement's in-memory
`ScreenBuffer` model when `ENABLE_VIRTUAL_TERMINAL_PROCESSING` is set:

- ICH (Insert Character): `CSI <n> @`
- DCH (Delete Character): `CSI <n> P`
- ECH (Erase Characters): `CSI <n> X`

These sequences must be consumed (not printed) and must mutate the buffer contents so that Win32
inspection APIs (for example `ReadConsoleOutputString`) observe the edited line state.

## Upstream Reference (Local Source Tree)
In the inbox host, these operations are implemented in the VT adapter:

- `src/terminal/adapter/adaptDispatch.cpp`
  - `AdaptDispatch::InsertCharacter(...)` (ICH)
  - `AdaptDispatch::DeleteCharacter(...)` (DCH)
  - `AdaptDispatch::EraseCharacters(...)` (ECH)

## Replacement Semantics (Simplified)
The replacement operates on the legacy `ScreenBuffer` cell grid (UTF-16 code units + `USHORT`
attributes). We intentionally keep this microtask small:

- Horizontal margins (DECSLRM) are not modeled.
- Delayed wrap (the "last column flag") is modeled separately and is reset by ICH/DCH/ECH; see
  `condrv_vt_autowrap.md`.
- All operations apply to the full buffer width of the current row.
- The cursor position is not changed by ICH/DCH/ECH (matching VT behavior).

### Parameter defaults and clamping
For ICH/DCH/ECH:

- If `<n>` is omitted, default to 1.
- If `<n>` is 0, treat it as 1.
- Clamp `<n>` to the remaining characters in the line: `buffer_width - cursor_x`.

### ICH: `CSI n @`
Insert `n` blank cells at the cursor position and shift existing cells on the row to the right.
Cells shifted off the right edge are discarded.

The inserted cells are set to:

- character: space (`L' '`)
- attributes: current text attributes (the active SGR state)

### DCH: `CSI n P`
Delete `n` cells starting at the cursor position and shift the remainder of the row left.
Cells exposed on the right are filled with blank cells using the current text attributes.

### ECH: `CSI n X`
Replace `n` cells starting at the cursor position with space cells using the current text attributes.
This never wraps to the next line.

## Implementation
Implemented in the shared VT-aware buffer update path:

- `new/src/condrv/condrv_server.hpp` (`apply_text_to_screen_buffer(...)`)

The implementation uses the public `ScreenBuffer` cell accessors:

- read: `read_output_characters(...)` + `read_output_attributes(...)`
- write: `write_cell(...)`

This keeps the change localized to one module and avoids adding new Win32 dependencies.

## Tests
`new/tests/condrv_raw_io_tests.cpp`

- `test_write_console_vt_ich_inserts_characters_in_line`
- `test_write_console_vt_dch_deletes_characters_in_line`
- `test_write_console_vt_ech_erases_characters_in_line`

Each test uses `ConsolepWriteConsole` with VT processing enabled and validates the resulting line
contents via `ConsolepReadConsoleOutputString`.

## Limitations / Follow-ups
- Horizontal margins (DECSLRM) are not modeled yet (operations always apply to the full row width).
- ECH/ED/EL currently use the active attributes; conhost also has an "Erase Color" mode that can
  change which attributes are applied when erasing. That mode is deferred.
- Origin mode (DECOM, `CSI ? 6 h/l`) is implemented separately; see `condrv_vt_origin_mode.md`.
