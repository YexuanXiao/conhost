# ConDrv Output VT Processing: ESC Dispatch Consumption (NEL/DECALN/Charsets)

## Goal
When `ENABLE_VIRTUAL_TERMINAL_PROCESSING` is enabled, the inbox conhost interprets VT escape
sequences and does not render the raw escape bytes into the screen buffer.

The replacement implements VT output processing in a simplified **streaming** parser
(`apply_text_to_screen_buffer(...)`). This microtask extends that parser to consume common
**ESC dispatch** sequences so TUIs do not accidentally print `ESC` bytes. See
`condrv_vt_output_streaming.md` for the retained parse-state model.

## Upstream Reference (Local Source Tree)
The upstream parser recognizes ESC dispatch sequences and routes them through a dispatch table:

- `src/terminal/parser/OutputStateMachineEngine.cpp`: `ActionEscDispatch(...)`
  - `NEL_NextLine` -> `LineFeed(WithReturn)` (ESC `E`)
  - `IND_Index` -> `LineFeed(WithoutReturn)` (ESC `D`)
  - `RI_ReverseLineFeed` -> `ReverseLineFeed()` (ESC `M`)
  - `DECALN_ScreenAlignmentPattern` -> `ScreenAlignmentPattern()` (ESC `#` `8`)

The DECALN behavior is implemented here:

- `src/terminal/adapter/adaptDispatch.cpp`: `AdaptDispatch::ScreenAlignmentPattern()`

## Replacement Semantics (Simplified)
All behavior described below only applies when `ENABLE_VIRTUAL_TERMINAL_PROCESSING` is set.

### Implemented ESC dispatch commands
The replacement consumes these ESC sequences and updates the in-memory `ScreenBuffer` model:

- **NEL** (`ESC E`): "next line with return"
  - sets the column to 0 and performs a VT-style line feed (including DECSTBM-aware scrolling).
- **IND** (`ESC D`) and **RI** (`ESC M`)
  - already implemented as VT-style line feed (without return) and reverse line feed.
- **RIS** (`ESC c`)
  - already implemented as a hard reset (see `condrv_vt_resets.md`).
- **DECALN** (`ESC # 8`): "screen alignment pattern"
  - fills the entire screen buffer with `L'E'` using `ScreenBuffer::default_text_attributes()`
  - clears scrolling margins and disables origin mode
  - clears `COMMON_LVB_REVERSE_VIDEO` and `COMMON_LVB_UNDERSCORE` in the current attributes
    (colors are preserved)
  - homes the cursor to `(0,0)`

### Consuming unsupported ESC dispatch sequences
The inbox conhost consumes many ESC dispatch sequences even when the console does not visibly
support the associated feature (for example, charset designation).

To avoid escape-byte leakage, the replacement consumes generic ESC dispatch sequences of the form:

- `ESC` + intermediates (`0x20..0x2F`) + final (`0x30..0x7E`)

and treats them as **no-ops**, except for DECALN which is implemented.

This ensures common sequences like `ESC ( 0` / `ESC ( B` (charset designation) do not render into
the buffer even though the replacement does not model line drawing.

### String introducers (DCS/PM/APC/SOS)
Some ESC sequences introduce strings (for example `ESC P` for DCS). The replacement consumes these
strings by entering an "ignore payload until ST" mode:

- introducers: `ESC P`/`ESC ^`/`ESC _`/`ESC X` (and their C1 equivalents)
- terminators: `C1 ST (0x9C)` or `ESC \\`

The payload is ignored and the terminator is recognized even when it is split across multiple
`WriteConsole` calls (streaming behavior).

### String terminator
`ESC \\` (ST) is consumed as a no-op when written directly so it does not leak to the buffer.

## Tests
`new/tests/condrv_raw_io_tests.cpp`

- `test_write_console_vt_nel_moves_to_next_line_and_consumes_sequence`
- `test_write_console_vt_charset_designation_is_consumed`
- `test_write_console_vt_split_charset_designation_is_consumed`
- `test_write_console_vt_decaln_screen_alignment_pattern_fills_and_homes_cursor`
- `test_write_console_vt_split_dcs_string_is_consumed`

## Limitations / Follow-ups
- Charset designation is consumed but not modeled (no line drawing).
- Most DCS/PM/APC/SOS payloads are ignored; they are only consumed so control bytes do not leak to
  the buffer.
