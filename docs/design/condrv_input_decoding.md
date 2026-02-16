# ConDrv Input Decoding (Design)

## Problem Statement

The replacement ConDrv server currently models console input as a byte stream fed from `host_input` into an internal
queue. That simplification was useful to get basic APIs working, but it caused two correctness issues:

1. `ConsolepReadConsole` with `Unicode=TRUE` widened bytes 1:1 into UTF-16 code units, so UTF-8 sequences (e.g. `C3 A9`)
   became garbage (`U+00C3 U+00A9`) instead of `U+00E9`.
2. `ConsolepGetConsoleInput` treated each byte as a `KEY_EVENT` and filled only `UnicodeChar` as `wchar_t(byte)`, and
   `ConsolepWriteConsoleInput` narrowed `UnicodeChar` to a single byte. These behaviors ignored the configured input code
   page and made UTF-8 input unusable.

The inbox conhost does not treat console input as arbitrary bytes: it stores input as events/Unicode and converts to ANSI
for the `A` APIs based on the configured code pages. While we are still byte-stream-backed, we can match the observable
"Unicode read returns decoded text" behavior for the important UTF-8 case.

## Reference in the Local Conhost Source

Relevant entry points:

- `src/server/ApiDispatchers.cpp`:
  - `ApiDispatchers::ServerReadConsole` delegates to `ReadConsoleImpl` (cooked input) and supports reply-pending waits.
  - `ApiDispatchers::ServerGetConsoleInput` delegates to `GetConsoleInputImpl` and returns `INPUT_RECORD`s.

We are not porting the full input buffer + wait-queue stack yet. This module improves the simplified model without
changing the fundamental "byte queue + deterministic decoding" approach. A minimal reply-pending wait queue is now
implemented so input-dependent operations do not block inside dispatch (see
`new/docs/design/condrv_reply_pending_wait_queue.md`).

## Goals

1. Respect the configured input code page when converting queued bytes to Unicode.
2. For `ConsolepReadConsole` (`Unicode=TRUE`), decode UTF-8 and other code pages into UTF-16 output and consume the
   correct number of bytes from the input queue.
3. For `ConsolepGetConsoleInput`, decode queued bytes into `KEY_EVENT` records (one record per UTF-16 code unit), and
   consume the correct number of bytes when not peeking.
4. For `ConsolepWriteConsoleInput`, encode injected `KEY_EVENT` Unicode characters using the configured input code page.
5. Keep the implementation deterministic and exception-safe (`dispatch_message` is `noexcept`).

## Non-Goals (For This Increment)

- Full cooked-input semantics (`ENABLE_LINE_INPUT`, editing, history, ctrl wake masks, etc.).
- Full upstream wait-queue graph (`WaitQueue`/`WaitBlock`) and per-object wait conditions.
- Accurate virtual-key/scan-code/control-key-state synthesis.
- Mouse/window events and other `INPUT_RECORD` variants.

## Design

### 1. Decode One "Input Unit"

We introduce a small decoder that attempts to convert a single character worth of bytes into UTF-16 code units:

- For `CP_UTF8`:
  - Determine sequence length from the first byte (1..4).
  - If enough bytes are available, call `MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, ...)` on just that sequence.
  - On invalid sequences, consume 1 byte and emit `U+FFFD` to avoid getting stuck.
- For other code pages:
  - Use `IsDBCSLeadByteEx(code_page, byte0)` to decide 1 vs 2 byte sequences.
  - Convert the 1-2 byte chunk with `MultiByteToWideChar(code_page, 0, ...)`.
  - On failure, consume 1 byte and emit `U+FFFD`.

The output can contain 1 or 2 UTF-16 code units (surrogate pairs) depending on conversion.

### 2. Span Decode

We build two span-level helpers:

- Decode bytes -> `wchar_t` span (for `ReadConsoleW`).
- Decode bytes -> `INPUT_RECORD` span (for `GetConsoleInput`), writing one `KEY_EVENT` per UTF-16 code unit.

Both helpers return:

- `bytes_consumed`: the number of input bytes that correspond to fully-written output units
- `units_written`: number of UTF-16 units (or records) produced

If the next decoded character would not fit (e.g. surrogate pair but only 1 slot remains), the span decode helper stops
without consuming bytes. The API implementations then apply a compatibility rule so callers do not get stuck on
surrogate pairs when their buffers are too small:

- For removing reads (`ReadConsoleW` raw and `ReadConsoleInput`), if the next decoded character is a surrogate pair and
  only one slot remains, the server returns the first UTF-16 code unit, consumes the corresponding input bytes, and
  stashes the second code unit in `ObjectHandle::decoded_input_pending` for the next read.
- For peeking reads (`PeekConsoleInput`), the server never mutates state; it may return only the first code unit when the
  caller buffer is too small, leaving the bytes in the queue.

### 3. Queue Consumption Strategy

To avoid consuming the wrong number of bytes:

1. Peek a bounded prefix of the queued bytes.
2. Decode from the peeked snapshot to determine `bytes_consumed`.
3. If the call is not a peek (or for `ReadConsole`, always), discard exactly `bytes_consumed` bytes from the queue.

Discard uses small fixed-size stack buffers to avoid dynamic allocations inside `noexcept` dispatch paths.

#### Split Sequences (Reply-Pending Reads)

VT escape sequences and UTF-8/DBCS multibyte sequences can be split across reads. When the byte queue contains only a
*partial* sequence at the head, naive "peek + decode" logic can return success with 0 output while leaving the partial
bytes in the shared queue. Because the replacement signals input availability based on *queued bytes*, this results in a
permanently-signaled event and callers can busy-spin.

For input reads that have not produced any output yet, the replacement uses a small per-input-handle "pending bytes"
prefix buffer (`ObjectHandle::pending_input_bytes`) and reply-pending retries:

1. Detect `decode_one_console_input_unit(...) == need_more_data` at the head of the queue.
2. Drain the currently queued partial bytes into `pending_input_bytes` and remove them from the shared queue.
3. Return `reply_pending=true` so the server loop can retry once more input arrives.

This behavior is implemented for the input APIs that model the underlying input stream as bytes:

- raw `ReadConsoleW`
- `ConsolepGetConsoleInput` removing reads (ReadConsoleInput) with wait allowed

This prevents "success with 0 output" while partial bytes are queued, and avoids stalling the entire server loop by
never blocking inside `dispatch_message`.

For ConPTY win32-input-mode scenarios, a VT-first token decoder is now integrated into the relevant input APIs so
win32-input-mode key sequences (CSI ... `_`) and basic VT key sequences are synthesized into `KEY_EVENT` records, and
startup/control responses (DA1, focus in/out) are consumed without leaking to clients. See
`new/docs/design/condrv_vt_input_decoding.md`.

### 4. GetNumberOfInputEvents

`ConsolepGetNumberOfInputEvents` reports the number of input events, not raw bytes. The replacement approximates this by:

1. Peeking a bounded prefix of the queued bytes (and prepending any drained per-handle prefix bytes).
2. Scanning the prefix with the VT-first token decoder:
   - `key_event` tokens count as 1 event.
   - `text_units` tokens count as `char_count` UTF-16 code units.
   - ignored sequences (DA1/focus and similar) count as 0.
3. Stopping early on `need_more_data` to avoid claiming events for incomplete prefixes.

When processed input is enabled, Ctrl+C tokens are excluded from the count (they are control events, not input records).

## Tests

Added unit tests in `new/tests/condrv_raw_io_tests.cpp`:

- `test_user_defined_read_console_w_decodes_utf8_bytes`
- `test_user_defined_read_console_w_surrogate_pair_splits_across_reads`
- `test_l1_get_console_input_utf8_decodes_to_unicode_records`
- `test_l1_get_console_input_utf8_surrogate_pair_splits_across_reads`
- `test_l1_get_number_of_input_events_counts_utf8_code_units`

These tests set the input code page to `CP_UTF8`, inject `C3 A9` (U+00E9), and assert correct decoding and byte
consumption.

Added reply-pending wait tests in `new/tests/condrv_input_wait_tests.cpp`:

- `test_read_console_w_reply_pending_on_empty_input`
- `test_read_console_w_reply_pending_drains_split_utf8_sequence`
- `test_get_console_input_remove_reply_pending_drains_split_utf8_sequence`
- `test_dispatch_reply_pending_does_not_block_other_requests`
- `test_pending_read_completes_with_failure_when_input_disconnects`

## Follow-ups

1. Implement cooked input (`ReadConsoleImpl`-style) on top of an event queue instead of a raw byte queue.
2. Extend the minimal pending-reply queue into a closer upstream model (per-handle/per-process waits, cancellation).
3. Expand VT input decoding beyond the minimal supported set (modifier parameter variants, mouse input, bracketed paste,
   and richer `INPUT_RECORD` synthesis).
