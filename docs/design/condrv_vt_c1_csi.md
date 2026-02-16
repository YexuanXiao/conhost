# ConDrv Output VT Processing: C1 CSI (U+009B)

## Goal
When the output mode includes `ENABLE_VIRTUAL_TERMINAL_PROCESSING`, conhost interprets VT control
sequences instead of writing literal escape bytes into the screen buffer.

The VT standard allows a "C1" single-code-unit encoding for some controls. In particular:

- `CSI` may be encoded as `ESC [` (7-bit) or as `U+009B` (C1 CSI) in a Unicode stream.

This microtask ensures the replacement's VT output parser treats `U+009B` exactly like `ESC [` so
applications that emit C1 CSI sequences behave as expected.

## Upstream Reference (Local Source Tree)
The inbox VT parser describes why `U+009B` is unambiguous once the stream is UTF-16:

- `src/terminal/parser/stateMachine.cpp`
  - `_isC1ControlCharacter` comment explains that `\x009B` can be treated as C1 CSI because
    single-byte codepage ambiguity is resolved before parsing (for example, CP_ACP `0x9B` becomes
    `U+203A`, not `U+009B`).

The upstream parser/engine also has unit tests covering C1 parsing behavior:

- `src/terminal/parser/ut_parser/OutputEngineTest.cpp` ("C1 parsing enabled/disabled" cases)

## Replacement Semantics
The replacement's VT-aware screen buffer update routine (`apply_text_to_screen_buffer(...)`) parses
CSI sequences with a bounded scan.

This microtask extends CSI prefix recognition to accept both encodings:

- 7-bit: `ESC [` (two code units)
- C1: `U+009B` (one code unit)

Everything after the prefix is interpreted using the same CSI grammar already implemented (private
marker `?`, intermediate `!`, decimal parameters separated by `;`, and a final byte in the
`0x40..0x7E` range).

Outside of `ENABLE_VIRTUAL_TERMINAL_PROCESSING`, the replacement does not interpret `U+009B` as a
control sequence; it is treated like any other input code unit.

## Tests
`new/tests/condrv_raw_io_tests.cpp`

- `test_write_console_vt_c1_csi_cup_moves_cursor`
  - writes `A`, then `U+009B 2;3H Z`
  - verifies the cursor move is applied and the buffer contains `A` at `(0,0)` and `Z` at `(2,1)`
- `test_write_console_vt_c1_csi_ed_clears_screen`
  - writes `A`, then `U+009B 2J Z`
  - verifies ED(2) clears the screen and does not move the cursor (expects `[space, 'Z']` on the
    first row)

## Limitations / Follow-ups
- This only adds C1 CSI (`U+009B`) support. Other C1 controls are still not modeled unless they are
  already handled as part of existing features (for example, OSC uses `0x9D` and ST uses `0x9C`).
- Like the rest of the simplified output parser, CSI consumption is bounded to the current
  `WriteConsole` chunk; split sequences across separate writes are not guaranteed to be consumed.

