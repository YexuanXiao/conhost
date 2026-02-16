# ConDrv Raw I/O Parity (RAW_READ / RAW_WRITE / RAW_FLUSH)

## Goal
Provide inbox-conhost-compatible semantics for ConDrv "raw" operations:

- `CONSOLE_IO_RAW_WRITE`
- `CONSOLE_IO_RAW_READ`
- `CONSOLE_IO_RAW_FLUSH`

These operations are used by classic clients that treat the console as a byte stream (for example CRT `printf` and `ReadFile`/`WriteFile` on console handles). For compatibility, raw operations must behave like the inbox conhost server, not like a simple pipe.

This module is part of the headless/host-IO-backed server implementation in `new/src/condrv/condrv_server.hpp`.

## Upstream Reference (Local Source Tree)
In the original OpenConsole conhost, the raw functions are routed through the same L1 console APIs that back `ReadConsole`/`WriteConsole`:

- `src/server/IoSorter.cpp`
  - `CONSOLE_IO_RAW_WRITE` clears `WriteConsole` state and calls `ApiDispatchers::ServerWriteConsole`.
  - `CONSOLE_IO_RAW_READ` clears `ReadConsole` state, sets `ProcessControlZ = TRUE`, and calls `ApiDispatchers::ServerReadConsole`.
  - `CONSOLE_IO_RAW_FLUSH` calls `ApiDispatchers::ServerFlushConsoleInputBuffer`.

Implications:

1. Raw writes update the screen buffer, advance the cursor, and apply output-mode processing (CR/LF, wrap, etc.).
2. Raw reads apply the "ProcessControlZ" behavior: CTRL+Z (`0x1a`) at the start of the read is treated as EOF (0 bytes).
3. Raw flush clears the input buffer.

## Replacement Design
The replacement server keeps:

- A host I/O sink/source (`HostIo`) that represents the environment-facing byte streams.
- An in-memory `ScreenBuffer` model that must reflect observable console state (so buffer reads are consistent with writes).

### RAW_WRITE
Behavior:

1. Validate the handle is an output object and has an associated `ScreenBuffer`.
2. Read the byte payload from the ConDrv input buffer.
3. Decode bytes to UTF-16 using the server's output code page (`state.output_code_page()`).
4. Forward the raw bytes to `host_io.write_output_bytes(...)`.
5. Apply the decoded UTF-16 text to the `ScreenBuffer` using `apply_text_to_screen_buffer(...)`:
   - Updates characters with current attributes.
   - Implements minimal processed-output semantics (`\\r`, `\\n`, `\\b`, `\\t`) when `ENABLE_PROCESSED_OUTPUT` is set.
   - Implements wrap-at-EOL behavior when `ENABLE_WRAP_AT_EOL_OUTPUT` is set.
   - Advances the cursor and snaps the viewport to keep the cursor visible.
6. Return `Information = bytes_written` and `Status = success`.

Notes:

- The host sink receives the original bytes, matching a "byte stream" writer.
- The screen buffer receives decoded characters, matching `ServerWriteConsole` behavior and enabling buffer query APIs to observe the write.

### RAW_READ
Behavior:

1. Validate the handle is an input object.
2. Obtain the ConDrv output buffer for this operation.
3. If a non-empty destination is provided and no bytes are available:
   - If the host input is disconnected, complete the request with `STATUS_UNSUCCESSFUL`.
   - Otherwise return `reply_pending=true` so the server loop can retry when input arrives (no blocking inside dispatch).
4. If bytes are available, implement "ProcessControlZ" compatibility:
   - Peek 1 byte.
   - If the first byte is `0x1a`, consume exactly 1 byte and return `Information = 0` with `Status = success`.
5. Otherwise read bytes into the destination via `host_io.read_input_bytes(...)` and return the count in `Information`.

ConPTY VT input note:

- When the head of the byte stream is a supported VT sequence (see `new/docs/design/condrv_vt_input_decoding.md`),
  `RAW_READ` must not leak the literal escape bytes to applications:
  - win32-input-mode keyboard sequences (`CSI ... _`) are decoded into `KEY_EVENT`s and only character keydowns
    (`UnicodeChar!=0`) are returned as bytes (encoded using the current input code page).
  - focus events and DA1 responses are consumed and ignored (not returned).
  - non-character keys (arrows/function keys) are consumed and ignored.
  - partial VT sequences are drained into the per-handle prefix buffer and the request reply-pends until more input arrives.
- For non-VT input, `RAW_READ` preserves the legacy "byte stream" behavior and returns bytes as-is (except processed-input
  Ctrl+C filtering when enabled, and Ctrl+Break termination when observed as a key event under ConPTY win32-input-mode).

Critical edge case:

- CTRL+Z handling must consume only the marker byte and must not drop any subsequent bytes in the queue. This matches the inbox behavior for raw reads and avoids losing data after an EOF marker.

### RAW_FLUSH
Behavior:

1. Validate the handle is an input object.
2. Call `host_io.flush_input_buffer()`.
3. Return `Information = 0` and `Status = success`.

## Tests
Unit tests live in `new/tests/condrv_raw_io_tests.cpp` and cover:

- Raw write forwards bytes and sets `Information` correctly.
- Raw write updates the in-memory screen buffer model (validated via `ConsolepReadConsoleOutputString`).
- Raw read copies bytes into the output buffer and reports the correct count.
- Raw read CTRL+Z behavior: consumes one byte and returns 0, leaving following bytes for subsequent reads.
- Raw flush clears the host input queue.

## Limitations / Follow-ups
- The replacement implements a minimal conhost-style reply-pending queue in the server loop rather than a full upstream wait-graph (`WaitQueue`/`WaitBlock`). See `new/docs/design/condrv_reply_pending_wait_queue.md`.
- The screen buffer update path implements a minimal subset of processed output control characters. Additional VT/CSI handling is out of scope for raw I/O parity and should be implemented in a dedicated text-render/VT module if needed.
- Code page decoding is delegated to the shared decode helper; full fidelity for all legacy code pages can be improved incrementally with targeted tests.
