# Fast Number Stress Tests (Deterministic, Non-GUI)

## Goal

Harden the project's numeric parsing/formatting helpers in `serialization/fast_number` by expanding test coverage around:

- boundary values (overflow/underflow),
- sign handling,
- hex prefix requirements,
- deterministic stress-style round-trips (format -> parse).

This is test-only work: no production parsing semantics are changed.

## What Is Covered

Implemented in `new/tests/fast_number_tests.cpp`:

- Signed 32-bit parsing boundaries:
  - accepts `2147483647` and `-2147483648`
  - rejects `2147483648` (overflow) and `-2147483649` (underflow)
  - accepts `+0`
- Unsigned 32-bit parsing boundaries:
  - accepts `4294967295`
  - rejects `4294967296` (overflow) and `-1` (invalid)
- Hex parsing boundaries:
  - accepts `0xFFFFFFFF` / `0xFFFFFFFFFFFFFFFF`
  - rejects the first value past the max (`0x100000000`, `0x10000000000000000`)
  - enforces `0x` prefix when `require_prefix == true`
- Deterministic round-trip stress:
  - `format_i64` -> `parse_i32`
  - `format_u64` -> `parse_u32`
  - `format_f64` -> `parse_f64` (finite values only)

## Determinism

The stress loops do not use `<random>`. They use a tiny PRNG (SplitMix64) with fixed seeds and fixed iteration counts, so
failures are reproducible.

## Notes / Limitations

- The stress coverage validates correctness and robustness, not performance.
- Floating-point round-trips are asserted via `to_chars` + `from_chars` exactness for finite values.

