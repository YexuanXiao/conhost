# ConDrv Output VT Processing: Cursor Movement Under Scroll Margins

## Goal

When `ENABLE_VIRTUAL_TERMINAL_PROCESSING` is enabled, terminal applications move the cursor using
CSI cursor movement sequences. Some of these sequences are expected to respect DECSTBM scroll
margins even when origin mode is disabled (the default).

This microtask improves compatibility by:

- Implementing additional cursor movement sequences:
  - CHA/HPA: `CSI <col> G` / `CSI <col> \``
  - VPA: `CSI <row> d`
  - CNL/CPL: `CSI <n> E` / `CSI <n> F` (also moves to column 1)
- Updating `CUU`/`CUD` clamping behavior (`CSI <n> A` / `CSI <n> B`) so movement clamps within the
  active DECSTBM region when the cursor starts inside that region, even when origin mode is off.

## Upstream Reference (Local Source Tree)

- Cursor movement dispatcher entry points:
  - `src/terminal/adapter/adaptDispatch.cpp`
    - `AdaptDispatch::CursorUp/Down/Forward/Backward` (CUU/CUD/CUF/CUB)
    - `AdaptDispatch::CursorNextLine` / `AdaptDispatch::CursorPrevLine` (CNL/CPL)
    - `AdaptDispatch::CursorHorizontalPositionAbsolute` (CHA/HPA)
    - `AdaptDispatch::VerticalLinePositionAbsolute` (VPA)
- Margin-aware clamping logic:
  - `src/terminal/adapter/adaptDispatch.cpp`
    - `AdaptDispatch::_CursorMovePosition(...)` (`clampInMargins` behavior)

Key upstream behavior:
- CUU/CUD/CNL/CPL pass `clampInMargins=true`, so when DECSTBM is active and the cursor begins
  inside the scroll region, movement is clamped to the region boundaries.
- CHA/HPA/VPA/CUP use absolute offsets and rely on origin mode to select between full-buffer and
  margin-relative addressing.

## Replacement Design

### State Inputs

The cursor movement logic uses the existing `ScreenBuffer` VT state:
- `vt_vertical_margins` (DECSTBM) as an inclusive `[top,bottom]` range in buffer coordinates
- `vt_origin_mode_enabled` (DECOM) to decide whether absolute addressing is margin-relative

### Clamping Rule (Simplified, No Horizontal Margins)

Because the replacement does not model horizontal margins (DECSLRM), the implementation uses a
vertical-only clamping rule:

- If origin mode is enabled: vertical cursor movement clamps to `[top,bottom]`.
- If origin mode is disabled:
  - For CUU/CUD/CNL/CPL, when DECSTBM is active and the cursor starts inside `[top,bottom]`, clamp
    within `[top,bottom]`.
  - Otherwise, clamp within the full buffer `[0,height-1]`.

This matches the upstream intent of `clampInMargins=true` without introducing horizontal margin
dependencies.

### Implemented Sequences

In `apply_text_to_screen_buffer(...)`:

- `CSI <row> ; <col> H` / `CSI <row> ; <col> f` (CUP/HVP)
- `CSI <n> A/B/C/D` (CUU/CUD/CUF/CUB), with DECSTBM clamping as described above
- `CSI <col> G` / `CSI <col> \`` (CHA/HPA)
- `CSI <row> d` (VPA)
- `CSI <n> E` / `CSI <n> F` (CNL/CPL), also sets column to 1

## Tests

`new/tests/condrv_raw_io_tests.cpp`
- `test_write_console_vt_cuu_clamps_within_decstbm_when_origin_mode_disabled`
- `test_write_console_vt_cud_clamps_within_decstbm_when_origin_mode_disabled`
- `test_write_console_vt_cnl_moves_to_column_one_and_respects_decstbm_margins`
- `test_write_console_vt_cpl_moves_to_column_one_and_respects_decstbm_margins`

## Limitations / Follow-Ups

- Horizontal margins (DECSLRM/DECLRMM) are not modeled; CHA/HPA always use column 0 as the origin.
- The replacement does not attempt to reproduce the upstream “only clamp when inside both margins”
  behavior because horizontal margins are absent.

