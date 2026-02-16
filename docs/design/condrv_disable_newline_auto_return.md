# Output Mode: DISABLE_NEWLINE_AUTO_RETURN

## Problem / Missing Behavior
The in-memory `ScreenBuffer` model is updated by `ConsolepWriteConsole` and `CONSOLE_IO_RAW_WRITE` via the shared helper `apply_text_to_screen_buffer(...)`.

Before this change, processed-output handling always treated `\\n` as "CRLF" (reset column to 0, then advance the line). That is correct for the default console mode, but it is incorrect when the output mode flag `DISABLE_NEWLINE_AUTO_RETURN` is set.

Incorrect newline translation breaks cursor positioning and therefore screen-buffer contents observed by APIs such as `ReadConsoleOutputString`.

## Upstream Behavior (Local Conhost Source)
The inbox host explicitly gates LF-to-CRLF translation on `DISABLE_NEWLINE_AUTO_RETURN`.

In the legacy output path (`src/host/_stream.cpp`):

- For `UNICODE_LINEFEED`:
  - If `DISABLE_NEWLINE_AUTO_RETURN` is clear and the cursor column is non-zero, the host resets `pos.x = 0` before advancing the row.
  - If the flag is set, the host advances the row and preserves `pos.x`.

In the VT output path (`WriteCharsVT` in the same file), the host chooses between `WriteUTF16TranslateCRLF(...)` and `WriteUTF16(...)` based on the same flag.

## Replacement Design
The replacement follows the same observable rules for the buffer model:

1. `DISABLE_NEWLINE_AUTO_RETURN` affects only how `\\n` is processed when `ENABLE_PROCESSED_OUTPUT` is set.
2. When `DISABLE_NEWLINE_AUTO_RETURN` is **not** set:
   - `\\n` performs an implicit CRLF for the buffer model:
     - column becomes `0`
     - row increments (with scroll if needed)
3. When `DISABLE_NEWLINE_AUTO_RETURN` **is** set:
   - `\\n` performs a line feed only:
     - column is preserved
     - row increments (with scroll if needed)
4. Wrap-at-EOL behavior is unchanged (wrapping still moves to the next line at column 0).

## Implementation Notes
Implementation lives in `new/src/condrv/condrv_server.hpp`:

- `apply_text_to_screen_buffer(...)` now reads `DISABLE_NEWLINE_AUTO_RETURN` from the passed `output_mode`.
- The newline case uses a `line_feed()` helper to advance rows and scroll if needed.
- Column reset for `\\n` is conditional on `DISABLE_NEWLINE_AUTO_RETURN` being clear.

The implementation is deliberately scoped to the in-memory buffer model. Translating bytes written to the host output stream (e.g. ConPTY transformations) is outside the scope of this microtask.

## Tests
Unit tests in `new/tests/condrv_raw_io_tests.cpp` cover:

- Default behavior (flag clear): `ab\\nc` places `c` at row 1, column 0.
- `DISABLE_NEWLINE_AUTO_RETURN` set: `ab\\nc` places `c` at row 1, column 2.

