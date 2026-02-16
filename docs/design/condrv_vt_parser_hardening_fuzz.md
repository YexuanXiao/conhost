# ConDrv VT Parser Hardening: Deterministic Fuzz + Bounds Coverage

## Goal

Ensure the replacement's VT parsing logic is robust under malformed or adversarial inputs without expanding the supported
VT feature surface:

- no crashes,
- no out-of-bounds reads/writes,
- no "produced token with `bytes_consumed == 0`" stalls,
- key model invariants (cursor/window/buffer bounds) remain valid under arbitrary input,
- existing hard bounds behave deterministically (CSI length limit, ESC-dispatch length limit, OSC title payload capture).

This work is **non-GUI** and purely test-driven hardening.

## What Is Being Fuzzed

The fuzz tests target two independent parsers:

1. VT input decoding (`try_decode_vt`):
   - `new/src/condrv/vt_input_decoder.cpp`
   - Parses a single input token from a byte prefix (win32-input-mode, basic VT keys, DA1/focus ignored sequences).

2. Streaming VT output parsing (`apply_text_to_screen_buffer`):
   - `new/src/condrv/condrv_server.hpp`
   - A small state machine that consumes ESC/CSI/OSC/string sequences across split `WriteConsole*` chunks and updates the
     in-memory `ScreenBuffer` model.

## Determinism Rules

The fuzz is deterministic and does not use `<random>`:

- A tiny PRNG (`SplitMix64`) is implemented in the test module.
- Base seed is fixed: `0x4F434E45574F434FULL`.
- Each iteration derives its own seed by mixing the iteration index.

Optional scaling knob:

- `OPENCONSOLE_NEW_TEST_FUZZ_ITERS` (decimal) controls iteration count.
- Defaults to `800` when unset or invalid.
- Clamped to `[1, 20000]` to avoid accidental runaway.

## Invariants Enforced

### VT input decoder invariants

For `try_decode_vt(prefix)`:

- If `produced`:
  - `bytes_consumed > 0`
  - `bytes_consumed <= prefix.size()`
  - `TokenKind != text_units` (the VT decoder itself never emits `text_units`)
- If `need_more_data`:
  - `prefix.size() > 0`
  - `prefix[0]` is `ESC (0x1B)` or `C1 CSI (0x9B)`

### VT output streaming invariants

After applying a random chunk to `apply_text_to_screen_buffer`:

- `cursor_position` remains inside `[0, buffer_size - 1]`
- `window_rect` remains within buffer bounds and keeps valid inclusive ordering
- `read_output_characters({0,0}, ...)` returns exactly `buffer_w * buffer_h`
- `ScreenBuffer::revision()` is monotonic (never decreases)

### Targeted bounds coverage

Additional deterministic tests validate hard bounds:

- Overlong CSI sequences are abandoned and the parser returns to `ground` (the following `A` prints as a character).
- Overlong ESC-dispatch sequences are abandoned and the parser returns to `ground` (the following `A` prints).
- OSC title capture truncates to the fixed payload buffer size (currently `4096` UTF-16 units), and does not mutate
  screen buffer cells.

## Tests

Implemented in:

- `new/tests/condrv_vt_fuzz_tests.cpp`

Run with:

```powershell
cmake --build build-new --target oc_new_tests
ctest --test-dir build-new --output-on-failure
```

To scale iterations:

```powershell
$env:OPENCONSOLE_NEW_TEST_FUZZ_ITERS = "5000"
ctest --test-dir build-new --output-on-failure
```

## Limitations / Follow-Ups

- The fuzz harness is intentionally small and bounded; it is not a coverage-guided fuzzer.
- Unterminated OSC/string sequences are still modeled as "consume until terminator" (this is VT semantics); no timeout
  heuristics are introduced in this hardening increment.

