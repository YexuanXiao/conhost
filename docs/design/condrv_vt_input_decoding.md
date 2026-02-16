# ConDrv VT Input Decoding (Design)

## Problem Statement

When running under ConPTY, the hosting terminal can send key input using "win32-input-mode" (enabled by `ESC[?9001h`).
In that mode, key input is serialized as CSI sequences (terminated by `_`) rather than as plain code-page bytes.

Without a VT-aware decoder, the replacement ConDrv server would:

1. Leak escape bytes (`ESC`, `CSI`, `;`, digits, `_`) to applications via `ReadConsole*`, producing garbage text.
2. Return `KEY_EVENT` records with `wVirtualKeyCode=0` and no special-key handling for `ReadConsoleInput`/`PeekConsoleInput`.
3. Expose startup/control responses (DA1 device-attributes response, focus in/out) as literal input bytes.

The inbox host consumes and interprets these sequences. To match observable behavior for classic console clients in ConPTY
scenarios, the replacement needs a VT-first decoder in the input path.

## Reference in the Local Conhost Source

These local files are the behavioral ground truth for the format and decoding intent:

- Win32-input-mode format: `src/terminal/input/terminalInput.cpp`
  - `TerminalInput::_makeWin32Output` formats `CSI Vk;Sc;Uc;Kd;Cs;Rc _`.
- Input state machine behavior: `src/terminal/parser/InputStateMachineEngine.cpp`
  - `ActionCsiDispatch` cases for `Win32KeyboardInput`, `FocusIn`, `FocusOut`, `DA_DeviceAttributes`.

## Goals (This Increment)

1. Decode win32-input-mode key sequences into `KEY_EVENT_RECORD` `INPUT_RECORD`s for `ConsolepGetConsoleInput`
   (ReadConsoleInput/PeekConsoleInput).
2. Make `ConsolepReadConsole` treat decoded key events as character input:
   - deliver keydown events with `UnicodeChar!=0`
   - ignore non-character keys (arrows, function keys, etc.)
3. Consume/ignore terminal control responses that must not leak to applications:
   - DA1 response (`CSI ? ... c`)
   - focus in/out (`CSI I` / `CSI O`)
4. Preserve non-blocking dispatch: partial VT sequences reply-pend and are retried later (no blocking waits inside
   `dispatch_message`).
5. Prevent win32-input-mode sequences from leaking through raw byte reads (`CONSOLE_IO_RAW_READ`) by consuming VT
   sequences and returning only character key input as bytes.

## Non-Goals (This Increment)

- Full `ENABLE_VIRTUAL_TERMINAL_INPUT` semantics.
- Full CSI modifier variants (for example `CSI 1;2A`).
- Mouse/window/buffer-size `INPUT_RECORD` variants.
- Bracketed paste, OSC/DCS string parsing.
- Full VK/scan/control-state synthesis beyond what win32-input-mode provides.

## Design

### 1) VT Token Decoder Module

The replacement introduces a dedicated, allocation-free VT decoder:

- `new/src/condrv/vt_input_decoder.hpp`
- `new/src/condrv/vt_input_decoder.cpp`

This module parses a single "input token" from a byte prefix and returns one of:

- `TokenKind::key_event` with a filled `KEY_EVENT_RECORD`
- `TokenKind::ignored_sequence` (bytes consumed, no output)
- `DecodeResult::need_more_data` when the prefix is a supported VT sequence but the terminator is not present yet
- `DecodeResult::no_match` when the prefix is not a supported VT sequence

Supported sequences:

1. win32-input-mode keyboard input:
   - CSI `Vk;Sc;Uc;Kd;Cs;Rc` `_`
   - Accepts `ESC [` and C1 CSI (`0x9B`).
   - Defaults follow upstream: `Vk/Sc/Uc/Kd/Cs` default to 0, `Rc` defaults to 1.
2. Focus events (ignored):
   - `ESC [ I` (focus in)
   - `ESC [ O` (focus out)
3. DA1 response (ignored):
   - `ESC [ ? <digits/;> c`
4. Minimal fallback keys (produced as `KEY_EVENT` with `UnicodeChar=0`):
   - `ESC [ A/B/C/D` (arrows)
   - `ESC [ H/F` (Home/End)
   - `ESC [ 2~/3~/5~/6~` (Ins/Del/PgUp/PgDn)
   - `ESC O P/Q/R/S` (F1-F4, SS3 form)

ESC-at-buffer-end policy:

- A single `ESC` byte at the end of the available prefix yields `need_more_data`. This deterministic rule avoids
  guessing whether the byte is the Escape key or the start of a longer sequence. The server relies on reply-pending
  retries to resolve the ambiguity.

### 2) VT-First "One Token" Decode Wrapper

The dispatcher already had a per-code-page "decode one unit" helper for UTF-8/DBCS. We extend it with a VT-first
wrapper:

- `decode_one_input_token(code_page, bytes, vt_input::DecodedToken&)`

Behavior:

1. Try `vt_input::try_decode_vt`.
2. If no VT match, fall back to the existing code-page decoder (`decode_one_console_input_unit`), producing a
   `TokenKind::text_units` token.

This keeps input decoding deterministic and centralizes "VT vs bytes" decisions.

### 3) Integration Into ConDrv Input APIs

Integration is implemented in `new/src/condrv/condrv_server.hpp`:

1. `ConsolepGetConsoleInput`:
   - Decodes VT tokens and emits `KEY_EVENT` `INPUT_RECORD`s.
   - Consumes and skips ignored sequences (DA1/focus).
   - Implements processed-input Ctrl+C filtering for both:
     - legacy byte `0x03`
     - win32-input-mode key events where Ctrl is pressed and `wVirtualKeyCode=='C'`
2. `ConsolepReadConsole` cooked line input (`ENABLE_LINE_INPUT`):
   - Uses VT-first token decoding when consuming from the byte queue.
   - Key events contribute characters only when `bKeyDown` and `UnicodeChar!=0`.
   - Non-character keys are consumed/ignored (no echo, no line changes).
3. `ConsolepReadConsole` raw reads:
   - Unicode path uses VT-first token decoding and ignores non-character keys and ignored sequences.
   - ANSI path preserves raw-byte behavior for non-VT bytes, but consumes VT sequences so they never leak as escape
     bytes. Character key events are encoded with `WideCharToMultiByte` using the configured input code page.

### 4) Reply-Pending For Partial VT Sequences

Partial VT sequences must not cause busy-spins. The existing reply-pending model and per-handle prefix buffer are
reused:

- `ObjectHandle::pending_input_bytes` capacity was increased to 64 bytes to accommodate VT prefixes.
- When a removing read sees `need_more_data` at the head of the stream and has not produced output yet:
  1. drain queued bytes into `pending_input_bytes`
  2. remove them from the shared queue
  3. return `reply_pending=true`

This ties into the server-loop retry queue described in `new/docs/design/condrv_reply_pending_wait_queue.md`.

## Tests

Added deterministic, non-blocking tests in `new/tests/condrv_input_wait_tests.cpp`:

- `test_get_console_input_decodes_win32_input_mode_key_event`
- `test_get_console_input_decodes_win32_input_mode_arrow_key`
- `test_read_console_w_ignores_arrow_keys_and_pends` (fallback `ESC[A`)
- `test_split_win32_sequence_reply_pends_and_drains_prefix`
- `test_da1_and_focus_sequences_are_consumed_not_delivered`
- `test_read_console_a_decodes_win32_input_mode_character_key`

All tests use a `StrictHostIo` stub that fails if `wait_for_input(...)` is called from `dispatch_message`, ensuring the
dispatcher remains non-blocking and relies on reply-pending retries.

## Limitations / Follow-Ups

- Modified-key CSI variants (for example `CSI 1;2A`) are not parsed yet.
- Mouse input and richer `INPUT_RECORD` variants are not synthesized.
- There is no timeout-based disambiguation for a lone `ESC` byte.
- The ANSI raw-read path does not preserve `wRepeatCount` across calls for VT key events when the caller buffer is too
  small to hold all repeated output bytes.
