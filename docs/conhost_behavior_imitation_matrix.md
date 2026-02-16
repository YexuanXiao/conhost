# Conhost Behavior Imitation Matrix

This matrix tracks behavior imitation status against the local conhost source.

## 1. Startup and mode selection

1. Parse host command line (`ConsoleArguments` semantics): Implemented.
2. Legacy selection (`-ForceV1`, `HKCU\Console\ForceV2`, disable legacy in conpty): Implemented.
3. Legacy activation via `ConhostV1.dll!ConsoleCreateIoThread`: Implemented.
4. V2 startup process path: Implemented (replacement runtime path).
5. Success-path shutdown priority call: Implemented (`SetProcessShutdownParameters(0, 0)`).

## 2. CLI switch behavior

1. `--server`: Parsed, validated, and serviced via a ConDrv server loop with a substantial USER_DEFINED dispatch surface (Read/WriteConsole, input queue + reply-pending, screen buffer model, VT output parsing, VT/win32-input-mode input decoding, cooked line editing, history, etc.). Covered by unit tests and a process-isolated `--server --headless` end-to-end smoke integration test.
2. `--signal`: Parsed, validated, and used to request termination (via a cancellation monitor) for ConDrv server-mode; also used for the compatibility wait path when no client command is provided in create-server mode.
3. `--headless`: Parsed and used for conpty mode selection.
4. `--width` / `--height`: Parsed and used for initial pseudo-console sizing.
5. `--inheritcursor`: Parsed and used to send startup cursor position request.
6. `--textMeasurement`: Parsed and propagated/logged.
7. `--feature pty`: Parsed and validated.
8. `-ForceV1`: Parsed and used in launch policy.
9. `-ForceNoHandoff`: Parsed and propagated.
10. `-Embedding`: COM local-server registration and `EstablishHandoff` path implemented (payload/handle duplication + handoff transfer into the ConDrv server loop). Covered by both unit tests and an out-of-proc COM integration harness.
11. `--` and implicit client command payload parsing: Implemented.

## 3. Process startup behavior

1. Default client command fallback when empty in create-server mode:
   - Original behavior: fallback to cmd.
   - Replacement: implemented (`%WINDIR%\\system32\\cmd.exe` fallback).
2. Environment-variable command expansion:
   - Original behavior: explicit expansion in entrypoint path.
   - Replacement: implemented in runtime process startup path.

## 4. ConPTY/runtime behavior

1. Pseudo console creation and lifecycle: Implemented.
2. Input conversion to VT: Implemented (key encoder subset, extensible).
3. Output pumping with broken-pipe handling: Implemented.
4. Startup negotiation (DA1/focus/win32 input mode): Implemented.
5. Signal-handle driven shutdown: Implemented.

## 5. Documentation and quality obligations

1. Roadmap + architecture docs: Implemented.
2. API inventory doc: Implemented and updated.
3. Progress log updates: Implemented and maintained.
4. Non-GUI tests: Implemented and expanded incrementally.

## 6. Known remaining parity gaps

1. Full handoff semantics parity after COM activation (native `ConsoleEstablishHandoff`-equivalent runtime transfer).
2. Full parity with internal console-driver protocol/object model (`ConServer` internals).
3. Full renderer backend parity and legacy UI behavior coverage.
