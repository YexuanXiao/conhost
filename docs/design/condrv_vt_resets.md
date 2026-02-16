# ConDrv Output VT Processing: Soft/Hard Reset (DECSTR / RIS)

## Goal

Full-screen terminal applications and wrappers commonly use VT reset sequences to return the
terminal to a known baseline.

This microtask implements the most common reset sequences in the replacement's in-memory
`ScreenBuffer` model:

- Soft reset: **DECSTR** (`CSI ! p`)
- Hard reset: **RIS** (`ESC c`)

Both sequences must be consumed (not rendered) when `ENABLE_VIRTUAL_TERMINAL_PROCESSING` is
enabled.

## Upstream Reference (Local Source Tree)

- Parser IDs:
  - `src/terminal/parser/OutputStateMachineEngine.hpp`
    - `DECSTR_SoftReset = VTID(\"!p\")`
- Dispatch implementations:
  - `src/terminal/adapter/adaptDispatch.cpp`
    - `AdaptDispatch::SoftReset()` (DECSTR)
    - `AdaptDispatch::HardReset()` (RIS)

Upstream behaviors we model:
- DECSTR resets key terminal modes (cursor visible, replace mode, absolute origin, autowrap, full
  margins, normal rendition) **without clearing** the screen buffer contents.
- RIS performs a hard reset and **clears** the screen, then homes the cursor.

## Replacement Design

The replacement models the pieces that are observable through `ScreenBuffer` operations and
existing USER_DEFINED query APIs.

### DECSTR (`CSI ! p`)

Applied in `apply_text_to_screen_buffer(...)`:
- Cursor is forced visible (`set_cursor_info(..., true)`).
- VT modes reset to defaults:
  - autowrap enabled
  - origin mode disabled
  - insert/replace set to replace (IRM disabled)
  - DECSTBM cleared (full buffer scroll region)
  - delayed-wrap flag cleared
- SGR state reset by setting active attributes back to `default_text_attributes()`.
- Saved cursor state is reset to home (0,0) with default attributes.
- The screen contents are preserved.

### RIS (`ESC c`)

Applied in `apply_text_to_screen_buffer(...)`:
- If an alternate screen buffer is active, switch back to the main buffer.
- Restore the default Windows console color table (legacy palette).
- Apply the same VT/mode resets as DECSTR.
- Clear the entire screen buffer to spaces with default attributes.
- Home the cursor to (0,0).
- Saved cursor state is reset to home.

## Tests

`new/tests/condrv_raw_io_tests.cpp`
- `test_write_console_vt_decstr_soft_reset_disables_irm`
- `test_write_console_vt_decstr_soft_reset_resets_saved_cursor_state_to_home`
- `test_write_console_vt_ris_hard_reset_clears_screen_and_homes_cursor`

## Limitations / Follow-Ups

- The replacement does not model input-mode reset side effects (ConPTY re-negotiation, bracketed
  paste, etc). This microtask is scoped to the output `ScreenBuffer` model only.
- The CSI parser tracks only the minimal intermediate marker needed for DECSTR (`!`).

