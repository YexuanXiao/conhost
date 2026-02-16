# ConDrv VT IRM Insert/Replace Mode (`CSI 4 h` / `CSI 4 l`)

## Goal

When `ENABLE_VIRTUAL_TERMINAL_PROCESSING` is enabled on the output handle, console clients may
toggle **Insert/Replace Mode** (IRM, ANSI Standard Mode 4). In insert mode, printable output
inserts cells at the cursor by shifting the current line to the right. In replace mode (the
default), printable output overwrites cells.

This feature is important for TUIs that expect "type-in-the-middle" behaviors without emitting
explicit `ICH` sequences.

## Upstream Reference (Local Source Ground Truth)

- `src/terminal/adapter/DispatchTypes.hpp`
  - `IRM_InsertReplaceMode = ANSIStandardMode(4)`
- `src/terminal/adapter/adaptDispatch.cpp`
  - `_modes.test(Mode::InsertReplace)` selects `textBuffer.Insert(...)` vs `textBuffer.Replace(...)`

## Replacement Design (This Project)

### State

`condrv::ScreenBuffer` stores a boolean:
- `vt_insert_mode_enabled()` / `set_vt_insert_mode_enabled(bool)`

It defaults to `false` (replace mode).

### Parsing

In `apply_text_to_screen_buffer(...)`, when VT processing is active:
- `CSI 4 h` enables insert mode.
- `CSI 4 l` disables insert mode.

The mode is applied only when the CSI sequence is **not** a DEC private mode sequence
(`ParsedCsi::private_marker == false`) to avoid treating unrelated `CSI ? ... h/l` toggles as IRM.

### Rendering Semantics (Monospace Cell Model)

For each printable character emitted while VT processing is enabled:
- If insert mode is enabled, the current row is shifted right by one cell starting at the cursor
  column (dropping the final cell in the row), then the new cell is written at the cursor.
- If insert mode is disabled, the cell at the cursor is overwritten.

Attributes move with cells during the shift (we shift `ScreenCell` entries, not just characters).

Cursor advance and autowrap/delayed-wrap behaviors remain unchanged. In particular, IRM does not
alter DECAWM semantics; it only changes whether the line is shifted prior to writing the cell.

## Tests

`new/tests/condrv_raw_io_tests.cpp`
- `test_write_console_vt_irm_insert_mode_inserts_printable_cells`
  - Verifies `CSI 4 h` inserts and shifts cells.
  - Verifies `CSI 4 l` returns to overwrite behavior without shifting.

## Limitations / Follow-Ups

- The model is per-cell (UTF-16 code unit per cell) and does not implement wide glyphs or
  grapheme clustering.
- Horizontal margins (DECSLRM) and other full VT mode interactions are not implemented here.
- This microtask does not add additional mode save/restore behavior beyond what already exists.

