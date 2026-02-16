# ConDrv Processed Input: Ctrl+C/Ctrl+Break Handling (Minimal)

## Goal
When `ENABLE_PROCESSED_INPUT` is set, the inbox console host treats Ctrl+C and Ctrl+Break as control events:

- it generates a `CTRL_C_EVENT` for attached processes
- it is not delivered to the client as an input character
- it terminates **cooked** `ReadConsole` waits with `STATUS_ALERTED`
- it does **not** terminate **raw** reads (the read continues waiting for real input)

Ctrl+Break additionally flushes the input buffer and terminates raw reads with `STATUS_ALERTED`.

The replacement uses a byte-stream-backed input queue, so Ctrl+C can arrive either as:

- a literal `0x03` byte (classic byte-stream input), or
- a ConPTY win32-input-mode key sequence (`CSI ... _`) that decodes to a `KEY_EVENT` with Ctrl pressed and `VK='C'`.

Ctrl+Break can arrive only as a key event (for example ConPTY win32-input-mode `VK_CANCEL`), because there is no
distinct byte value that represents Ctrl+Break in the classic byte-stream model.

This microtask adds the minimal Ctrl+C/Ctrl+Break behavior needed for classic CLI compatibility.

## Upstream Reference (Local Source Tree)
- `src/host/input.cpp`
  - `HandleGenericKeyEvent(...)`:
    - detects Ctrl+C in processed input mode
    - sets control flags and calls `InputBuffer::TerminateRead(WaitTerminationReason::CtrlC)`
- `src/host/readDataCooked.cpp`
  - `COOKED_READ_DATA::Notify(...)` returns `STATUS_ALERTED` when terminated due to Ctrl+C/Ctrl+Break
- `src/host/readDataRaw.cpp`
  - `RAW_READ_DATA::Notify(...)` ignores Ctrl+C termination (continues the read), but terminates on Ctrl+Break
- `src/host/input.cpp`
  - `HandleGenericKeyEvent(...)` detects Ctrl+C and Ctrl+Break, flushes input on Ctrl+Break, and calls `TerminateRead(...)`
  - `ProcessCtrlEvents()` dispatches control events via `ConsoleControl::EndTask(...)`

## Replacement Design
### Status Code
The ConDrv protocol uses `NTSTATUS`. The replacement adds:

- `core::status_alerted` (`STATUS_ALERTED`, `0x00000101`)

### Control Event Dispatch
In `-Embedding` handoff scenarios, the inbox host provides a write-only "host-signal pipe". The replacement already forwards `GenerateConsoleCtrlEvent` by writing `HostSignals::end_task` packets to that pipe.

Ctrl+C processing reuses the same forwarding mechanism:

- best-effort `HostIo::send_end_task(pid, CTRL_C_EVENT, console_ctrl_c_flag)` for each connected process

### ReadConsole Behavior
Implemented in the `ConsolepReadConsole` handler (`new/src/condrv/condrv_server.hpp`):

1. **Cooked line input (`ENABLE_LINE_INPUT`)**
   - after decoding each unit/token, if `ENABLE_PROCESSED_INPUT` is set and the input represents Ctrl+C
     (`U+0003` or a decoded win32-input-mode Ctrl+C key event):
     - forward `CTRL_C_EVENT` via the host-signal pipe
     - terminate the call with `STATUS_ALERTED`
     - return 0 bytes (`NumBytes = 0`)
   - if the input represents Ctrl+Break (win32-input-mode key event `VK_CANCEL` with Ctrl pressed):
     - flush the input buffer
     - forward `CTRL_BREAK_EVENT`
     - terminate the call with `STATUS_ALERTED`
     - return 0 bytes (`NumBytes = 0`)

2. **Raw ReadConsole (no `ENABLE_LINE_INPUT`)**
   - if `ENABLE_PROCESSED_INPUT` is set:
     - filter Ctrl+C out of the input stream:
       - `0x03` bytes (including mid-buffer occurrences), and
       - decoded win32-input-mode Ctrl+C key events
     - forward `CTRL_C_EVENT` for each consumed Ctrl+C
     - continue reading/decoding to fill the caller’s buffer with non-control input when available (the read is not terminated)
     - if Ctrl+Break is observed as a win32-input-mode key event (`VK_CANCEL` with Ctrl pressed):
       - flush the input buffer
       - forward `CTRL_BREAK_EVENT`
       - terminate the call with `STATUS_ALERTED` and 0 bytes

This matches the observable upstream behavior that Ctrl+C is not returned as input, while only cooked reads are interrupted.

### ReadConsoleInput/PeekConsoleInput (`ConsolepGetConsoleInput`)
Implemented in the `ConsolepGetConsoleInput` handler (`new/src/condrv/condrv_server.hpp`):

- when `ENABLE_PROCESSED_INPUT` is set:
  - Ctrl+C is *not* delivered as a `KEY_EVENT` record:
    - `0x03` bytes are filtered out, and
    - win32-input-mode Ctrl+C key events are filtered out
  - decoding continues past Ctrl+C so the caller still receives up to the requested number of records when other input is available
  - for removing reads (`CONSOLE_READ_NOREMOVE` clear):
    - consumed Ctrl+C bytes are forwarded as `CTRL_C_EVENT` via the host-signal pipe (`HostSignals::end_task`)
  - for peek reads (`CONSOLE_READ_NOREMOVE` set):
    - Ctrl+C is filtered out of the returned records without consuming it when it is not at the front of the byte queue
    - if Ctrl+C is at the front, it is consumed and forwarded (matching that it is not a peekable input record in the inbox host)
  - Ctrl+Break is also filtered out when observed as a win32-input-mode key event (`VK_CANCEL` with Ctrl pressed):
    - it flushes the input buffer
    - it is forwarded as `CTRL_BREAK_EVENT`
    - it is not delivered as an input record

### RAW_READ Behavior (`console_io_raw_read`)
Implemented in the `console_io_raw_read` handler (`new/src/condrv/condrv_server.hpp`):

- when `ENABLE_PROCESSED_INPUT` is set:
  - filter Ctrl+C out of the input stream (including mid-buffer occurrences), forwarding `CTRL_C_EVENT` for each consumed Ctrl+C:
    - legacy byte `0x03`, and
    - win32-input-mode Ctrl+C key events (`CSI ... _` decoding to Ctrl pressed + `VK=='C'`)
  - continue reading to fill the caller’s buffer with non-control bytes when available (the read is not terminated)
  - if Ctrl+Break is observed as a win32-input-mode key event (`VK_CANCEL` with Ctrl pressed):
    - flush the input buffer
    - forward `CTRL_BREAK_EVENT`
    - terminate the call with `STATUS_ALERTED` and 0 bytes

## Implementation
- `new/src/core/ntstatus.hpp`
  - added `core::status_alerted`
- `new/src/condrv/condrv_server.hpp`
  - `ConsolepReadConsole`:
    - cooked line-input path now handles Ctrl+C as a processed control event
    - raw ReadConsole path now consumes Ctrl+C bytes when processed input is enabled
  - `ConsolepGetConsoleInput`:
    - filters Ctrl+C out of returned `INPUT_RECORD` data in processed input mode
    - removing reads forward consumed Ctrl+C as `CTRL_C_EVENT`
  - `console_io_raw_read`:
    - consumes Ctrl+C bytes when processed input is enabled, forwarding `CTRL_C_EVENT`

## Tests
`new/tests/condrv_raw_io_tests.cpp`

- `test_raw_read_processed_input_consumes_ctrl_c_and_sends_end_task`
- `test_raw_read_processed_input_skips_ctrl_c_mid_buffer_and_still_fills_output`
- `test_user_defined_read_console_a_raw_processed_input_consumes_ctrl_c_and_sends_end_task`
- `test_user_defined_read_console_a_raw_processed_input_skips_ctrl_c_mid_buffer_and_sends_end_task`
- `test_user_defined_read_console_w_line_input_ctrl_c_returns_alerted_and_sends_end_task`
- `test_user_defined_read_console_w_raw_processed_input_skips_ctrl_c_mid_buffer_and_sends_end_task`
- `test_l1_get_console_input_processed_input_skips_ctrl_c_on_remove_and_still_fills_records`
- `test_l1_get_console_input_processed_input_skips_ctrl_c_on_peek_and_still_fills_records`
- `test_raw_read_processed_input_ctrl_break_returns_alerted_and_flushes_input`
- `test_user_defined_read_console_w_raw_processed_input_ctrl_break_returns_alerted_and_flushes_input`
- `test_user_defined_read_console_w_line_input_ctrl_break_returns_alerted_and_flushes_input`
- `test_l1_get_console_input_processed_input_ctrl_break_flushes_and_reply_pends`

## Limitations / Follow-ups
- Ctrl+Break can be detected only when virtual-key metadata is available (for example ConPTY win32-input-mode sequences).
- Processed Ctrl+C handling is still byte-stream-backed; a future step can move control-event detection earlier in the pipeline (input ingestion) to eliminate the remaining peek-mode corner cases.
- The replacement does not print `^C` to the output buffer; applications that display it (e.g. shells) are expected to do so when handling the control event.
