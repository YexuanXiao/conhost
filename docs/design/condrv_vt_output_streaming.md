# ConDrv Output VT Processing: Streaming Parser State (Split Writes)

## Goal
When `ENABLE_VIRTUAL_TERMINAL_PROCESSING` is enabled, conhost interprets VT escape sequences and
does not render the raw escape bytes into the screen buffer.

Many TUIs write control sequences in small chunks (for example, `WriteConsole` calls that split a
single OSC or CSI sequence across multiple writes). A chunk-local parser can accidentally render
literal `ESC` bytes into the in-memory `ScreenBuffer`.

This document describes the replacement's **streaming** VT output parser: a small state machine
that retains partial-sequence state across `WriteConsole*` calls so split sequences are consumed
deterministically.

## Upstream Reference (Local Source Tree)
The upstream VT parser is a streaming state machine that retains parse state across writes:

- `src/terminal/parser/OutputStateMachineEngine.cpp`
  - ESC dispatch, CSI dispatch, OSC dispatch, and string payload handling.
- `src/terminal/parser/stateMachine.cpp`
  - C1 control character handling (`CSI` as `0x9B`, `OSC` as `0x9D`, `ST` as `0x9C`).

The replacement does not attempt to match the full upstream dispatch surface; it only implements
the subset needed for the in-memory `ScreenBuffer` model and console API behavior.

## Replacement Design
All VT output parsing is performed inside `apply_text_to_screen_buffer(...)` in:

- `new/src/condrv/condrv_server.hpp`

### Persisted parse state
Each `condrv::ScreenBuffer` stores a small parse state payload:

- `condrv::detail::VtOutputParseState ScreenBuffer::_vt_output_parse_state`

This makes parsing deterministic and single-threaded:

- The ConDrv server thread mutates the buffer model and its parse state.
- There is no cross-thread sharing of this parser state (renderer reads snapshots instead).

### Phases
The state machine is intentionally small:

- `ground`: normal printing
- `escape`: the previous write ended with `ESC` or we just consumed an `ESC` introducer
- `esc_dispatch`: `ESC` + intermediates (`0x20..0x2F`) + final (`0x30..0x7E`) (charset designation, DECALN)
- `csi`: parsing `CSI ... final`
- `osc`: parsing `OSC Ps ; payload ... terminator`
- `osc_escape`: saw `ESC` inside OSC, waiting for `\\` (ST)
- `string`: DCS/PM/APC/SOS payload, ignored until ST
- `string_escape`: saw `ESC` inside string payload, waiting for `\\` (ST)

### Recognized introducers and terminators
Introducers consumed (7-bit and C1 forms):

- `ESC [` and `0x9B` for CSI
- `ESC ]` and `0x9D` for OSC
- `ESC P`, `ESC ^`, `ESC _`, `ESC X` and their C1 equivalents (`0x90`, `0x98`, `0x9E`, `0x9F`) for strings

Terminators for OSC and string payloads:

- BEL (`0x07`)
- ST as `ESC \\`
- C1 ST (`0x9C`)

### Allocation policy
The parser itself is allocation-free:

- CSI parameters are stored in a fixed array (`16` params).
- OSC payload is captured into a fixed buffer (`4096` UTF-16 code units) for title actions.
- String payload is ignored without buffering.

If a sequence exceeds the replacement's hard bounds, it is abandoned and the parser returns to
`ground` so the server continues making forward progress.

### Interaction with other output features
The streaming VT parser updates the in-memory `ScreenBuffer` model by:

- consuming escape sequences (so `ReadConsoleOutput*` does not observe escape bytes),
- applying the supported CSI/ESC/OSC semantics already documented in the VT feature design docs,
- maintaining VT mode state (origin mode, autowrap, insert mode, margins, saved cursor state).

When VT processing is disabled for output:

- `ScreenBuffer::_vt_output_parse_state` is reset to its default state.
- VT delayed-wrap state is cleared (it is only meaningful under VT processing).

## Tests
`new/tests/condrv_raw_io_tests.cpp`

- `test_write_console_vt_split_osc_title_is_consumed_and_updates_state`
- `test_write_console_vt_split_osc_st_terminator_is_consumed_and_updates_state`
- `test_write_console_vt_split_csi_sequence_is_consumed`
- `test_write_console_vt_split_charset_designation_is_consumed`
- `test_write_console_vt_split_dcs_string_is_consumed`

These tests validate that escape sequences split across separate `WriteConsole` calls are consumed
and do not shift/render into the screen buffer.

## Limitations / Follow-ups
- This is still a **minimal** VT output model (subset of conhost). Unsupported sequences are
  generally consumed as no-ops to avoid escape-byte leakage.
- OSC payload capture is bounded; very large titles are truncated to the fixed payload buffer.
- The replacement does not model the full DCS/PM/APC/SOS feature surface; these strings are only
  consumed so TUIs do not leak control bytes into the buffer.

