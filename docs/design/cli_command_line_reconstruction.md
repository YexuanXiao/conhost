# CLI Command Line Reconstruction (Win32 Escaping)

## Goal

When `openconsole_new.exe` is started with a host/runtime prefix plus a client payload (either via `--` or because the
first unknown token begins the client command line), the replacement must reconstruct a *single* child command line
string that preserves tokenization when passed to `CreateProcessW`.

This is the behavior implemented by `oc::cli::ConsoleArguments::set_client_command_line` and its helper
`ConsoleArguments::escape_argument`.

## Behavior Summary

- Tokenization is obtained from `CommandLineToArgvW` (host side) and then reconstructed into a child command line.
- Host options are consumed only until:
  - `--` is encountered (explicit client payload), or
  - the first unknown token is encountered (implicit client payload start).
  After either boundary, remaining tokens are treated as client tokens and are not parsed as host options.

## Escaping Rules (Implemented)

`escape_argument` emits a token so that downstream Win32 parsing (CRT / `CommandLineToArgvW`) yields the original token:

- Empty token is represented as `""`.
- Tokens containing spaces/tabs are quoted.
- Quotes inside tokens are escaped with backslashes.
- Backslashes immediately preceding a quote are doubled (classic Win32 rule).
- Trailing backslashes in a quoted token are doubled before the closing quote so they are not consumed as escaping.

This mirrors the common `CreateProcessW`/CRT escaping rules used by conhost-style launchers.

## Tests

Covered in `new/tests/console_arguments_tests.cpp`:

- `--` boundary prevents host-flag parsing (`--headless`/`--vtmode` remain client tokens).
- Unknown-token boundary prevents host-flag parsing (flags after the program name remain client tokens).
- Round-trips of reconstructed command lines via `CommandLineToArgvW`, including:
  - a token containing spaces (requires quoting),
  - an argument with spaces and a trailing backslash (requires doubled trailing backslashes when quoted).

