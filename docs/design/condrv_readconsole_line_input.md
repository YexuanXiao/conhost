# ConDrv `ReadConsole`: Minimal Cooked Line Input

## Goal
When the input mode includes `ENABLE_LINE_INPUT`, conhost performs a "cooked" read for `ReadConsole`:

- it buffers input until an end-of-line is received
- it optionally echoes typed characters (`ENABLE_ECHO_INPUT`)
- it appends the expected newline suffix (`ENABLE_PROCESSED_INPUT` -> `CRLF`, otherwise `CR`)
- it may return partial data when the caller's output buffer is small, preserving the remainder for the next call

The replacement previously treated `ConsolepReadConsole` as a raw byte read, which is incompatible with default console modes (line input is enabled by default).

This microtask implements a minimal cooked line-input path for `ConsolepReadConsole` while keeping the rest of the input system byte-stream-backed.

## Upstream Reference (Local Source Tree)
- `src/host/stream.cpp`
  - `DoReadConsole(...)` routes reads to `_ReadLineInput(...)` when `ENABLE_LINE_INPUT` is set.
- `src/host/readDataCooked.cpp`
  - cooked read builds an editable line buffer and finalizes it on Enter
  - newline suffix behavior:
    - when `ENABLE_PROCESSED_INPUT` is set, the returned line ends with `"\r\n"`
    - otherwise it ends with `"\r"`
  - echo behavior is gated by `ENABLE_ECHO_INPUT`

The upstream implementation supports a large set of editing features and input events; the replacement intentionally implements only a small subset needed for basic CLI compatibility.

## Replacement Design
### Trigger
In the `ConsolepReadConsole` handler:

- if `state.input_mode()` has `ENABLE_LINE_INPUT`, the request is treated as a cooked line read
- otherwise, the existing raw behavior remains (byte read + UTF-8/code-page widening for Unicode reads)

### State and Exception Safety
To support partial reads when the caller buffer is smaller than the completed line, the replacement stores per-input-handle pending output:

- `ObjectHandle::cooked_read_pending` (UTF-16 code units; `std::wstring`)

To support reply-pending waits (so `dispatch_message` does not block when input is unavailable), the replacement also stores:

- `ObjectHandle::cooked_line_in_progress` (UTF-16 code units; `std::wstring`)
  - the in-progress cooked line being assembled before CR/LF termination.
- `ObjectHandle::pending_input_bytes` (fixed-size prefix buffer)
  - drained incomplete UTF-8/DBCS sequences that must persist across retries.

Because it is stored inside the `ObjectHandle`, it is released automatically when the handle is closed/disconnected (RAII by containment).

### Line Assembly
Input arrives as bytes from the internal queue. The line-input path decodes the byte stream into UTF-16 units using:

- `decode_one_console_input_unit(code_page, ...)` (already used for `GetConsoleInput` decoding)

Because the input queue is byte-stream-backed, multibyte sequences can be split across reads. The implementation drains incomplete prefixes into `ObjectHandle::pending_input_bytes` and reply-pends the operation until more bytes arrive, rather than blocking inside dispatch.

The read loop consumes one decoded unit at a time:

- `'\b'`: removes the last code point from the in-progress line buffer (surrogate-aware)
  - if echo is enabled, emits the conventional erase sequence `"\b \b"`
- `'\r'` or `'\n'`: finalizes the line
  - if the terminator is `'\r'` and the next input unit is `'\n'`, the `'\n'` is consumed (CRLF normalization)
  - appends the newline suffix:
    - processed input -> `"\r\n"`
    - otherwise -> `"\r"`
  - if echo is enabled, echoes the newline suffix
- otherwise: appends decoded units to the line and echoes them if enabled

### `ProcessControlZ`
If `ProcessControlZ` is set and the first character of an empty line is `U+001A` (CTRL+Z), the handler consumes it and returns 0 bytes (EOF-style behavior), matching the existing raw-path behavior used by conhost.

### Echo Implementation
Echo is implemented as:

1. Update the in-memory `ScreenBuffer` model (active buffer) via `apply_text_to_screen_buffer(...)`.
2. Forward the echoed UTF-16 text to the host output sink as UTF-8 bytes (`WideCharToMultiByte(CP_UTF8, ...)` then `host_io.write_output_bytes(...)`).

### Delivering Output to the Caller
If `cooked_read_pending` is non-empty, it is copied into the caller's output buffer:

- Unicode reads: copy UTF-16 code units directly.
- ANSI reads: convert as much of the pending UTF-16 buffer as fits into the output buffer using `WideCharToMultiByte(input_code_page, ...)`, without splitting surrogate pairs.

Any remaining UTF-16 units stay in `cooked_read_pending` for subsequent reads.

If an ANSI read's output buffer cannot hold even one encoded character (for example, a 1-byte buffer with UTF-8 input where the next character needs 2+ bytes), the replacement returns `STATUS_BUFFER_TOO_SMALL` instead of returning success with 0 bytes and leaving pending data intact.

## Implementation
- `new/src/condrv/condrv_server.hpp`
  - `ObjectHandle::cooked_read_pending`
  - `ConsolepReadConsole` dispatch handler now routes line-input reads to the cooked path.

## Tests
`new/tests/condrv_raw_io_tests.cpp`

- Updated existing "raw read" tests to disable line input (`state.set_input_mode(0)`) so they continue to validate raw behavior.
- Added cooked line-input tests:
  - `test_user_defined_read_console_w_line_input_returns_crlf_and_echoes`
  - `test_user_defined_read_console_w_line_input_backspace_edits_and_echoes`
  - `test_user_defined_read_console_w_line_input_small_buffer_sets_pending`
  - `test_user_defined_read_console_w_line_input_without_processed_returns_cr`
  - `test_user_defined_read_console_a_line_input_returns_crlf`
  - `test_user_defined_read_console_a_line_input_small_buffer_sets_pending`
  - `test_user_defined_read_console_a_line_input_utf8_buffer_too_small_for_multibyte_char`
  - `test_user_defined_read_console_w_line_input_handles_split_utf8_sequence`

## Limitations / Follow-ups
- This is not a full cooked-read implementation:
  - command history and popup editing are not implemented (Up/Down, F-keys, command list)
  - `ControlKeyState` is currently always reported as 0
  - Ctrl+C and Ctrl+Break are handled as processed control events, but Ctrl+Break requires virtual-key metadata (for example ConPTY win32-input-mode)
  - `InitialNumBytes`, `CtrlWakeupMask`, and `ExeNameLength` fields are not used yet
- The cooked read is still backed by a byte stream; a future step may promote the input queue to richer input events to match conhost more closely.
