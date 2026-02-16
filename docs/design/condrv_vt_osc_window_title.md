# ConDrv Output VT Processing: OSC Window Title

## Goal
When the output mode includes `ENABLE_VIRTUAL_TERMINAL_PROCESSING`, conhost interprets VT "operating
system command" (OSC) sequences instead of writing the raw escape bytes into the screen buffer.

The most common OSC usage for console applications is setting the window/tab title:

- `OSC 0`: set icon name and window title
- `OSC 1`: set icon name
- `OSC 2`: set window title
- `OSC 21`: DECSWT "set window title"

This microtask implements a compact subset of OSC handling in the replacement so:

- `ReadConsoleOutputString` / `ReadConsoleOutput` do not observe literal `ESC ]` bytes,
- `ConsolepGetTitle` reflects title updates initiated by VT apps.

## Upstream Reference (Local Source Tree)
The inbox host implements OSC parsing and dispatch in the VT parser/adapter stack:

- Action code definitions:
  - `src/terminal/parser/OutputStateMachineEngine.hpp` (`enum OscActionCodes`)
- Dispatch behavior for titles:
  - `src/terminal/parser/OutputStateMachineEngine.cpp` (`OutputStateMachineEngine::ActionOscDispatch`)
    - `SetIconAndWindowTitle (0)`, `SetWindowIcon (1)`, `SetWindowTitle (2)`, and
      `DECSWT_SetWindowTitle (21)` all call `_dispatch->SetWindowTitle(string)`.

## Replacement Semantics (Simplified)
The replacement consumes a bounded subset of OSC sequences in the shared VT-aware buffer update
path (`apply_text_to_screen_buffer(...)`) using a streaming parser state machine (see
`condrv_vt_output_streaming.md`):

- Prefixes accepted:
  - 7-bit: `ESC ]`
  - C1: `0x9D` (OSC) as a single code unit
- Terminators accepted:
  - BEL (`0x07`)
  - ST (`ESC \\`)
  - C1 ST (`0x9C`)
- Parameter parsing:
  - Parse decimal `Ps` up to the first `;`.
  - Everything after the `;` up to the terminator is treated as the title payload (no further
    escaping is modeled).

Supported actions:
- `Ps` in `{0, 1, 2, 21}` updates the replacement's server title via `ServerState::set_title(...)`.
- Other OSC action codes are consumed (not rendered) but otherwise ignored.

Implementation note:
- `apply_text_to_screen_buffer(...)` takes an optional `ServerState*` so title updates can be
  applied without adding global state. All current call sites pass `&state`.

## Tests
`new/tests/condrv_raw_io_tests.cpp`

- `test_write_console_vt_osc_title_updates_server_title_and_is_not_rendered`
  - writes `A`, then OSC title sequences (BEL and ST terminated), then `B`
  - verifies the buffer contains `AB` (no escape bytes rendered)
  - verifies `ConsolepGetTitle` returns the last OSC-provided title
  - also verifies the C1 OSC introducer (`0x9D`) is consumed
- `test_write_console_vt_split_osc_title_is_consumed_and_updates_state`
- `test_write_console_vt_split_osc_st_terminator_is_consumed_and_updates_state`

## Limitations / Follow-ups
- OSC payload capture is bounded to a fixed buffer. Extremely large titles are truncated.
- Most OSC action codes are not implemented yet (color table manipulation, clipboard, hyperlinks).
- DSR/CPR request/response behavior remains out of scope for now.
