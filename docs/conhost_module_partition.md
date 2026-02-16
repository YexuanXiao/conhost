# Conhost Module Partition and Replacement Mapping

This document partitions the original conhost architecture into replacement modules in `new/`, and records intended ownership boundaries.

## 1. Original-to-replacement mapping

1. Startup + launch policy
- Original: `src/host/exe/exemain.cpp`, `ConsoleArguments`, legacy selection logic.
- Replacement: `new/src/app/application.cpp`, `new/src/cli/console_arguments.*`, `new/src/runtime/launch_policy.*`, `new/src/runtime/legacy_conhost.*`.

2. Session runtime / process hosting
- Original: server entrypoints + startup/create I/O thread paths.
- Replacement: `new/src/runtime/session.*`, `new/src/runtime/startup_command.*`.

3. VT/conpty handshake and signal integration
- Original: `src/host/VtIo.cpp`, signal input thread behavior.
- Replacement: `new/src/runtime/session.*` + `new/src/runtime/key_input_encoder.*`.

4. Handle/resource safety
- Original: wil smart handle usage and scoped guards.
- Replacement: `new/src/core/unique_handle.hpp`, `new/src/core/unique_local.hpp`, custom RAII wrappers.

5. Logging/diagnostics
- Original: tracelogging + host tracing.
- Replacement: `new/src/logging/logger.*` (debug/file sinks, format-restricted output).

6. Configuration/localization
- Original: registry/settings + text/locale layers.
- Replacement: `new/src/config/app_config.*`, `new/src/localization/localizer.*`.

## 2. Replacement module ownership boundaries

1. `app`
- Owns startup sequencing and top-level control flow.
- Must not absorb low-level Win32 lifecycle details.

2. `cli`
- Pure parsing and compatibility interpretation.
- No process/session side effects.

3. `runtime`
- Session setup, mode selection, process launching, conpty runtime loops.
- Encapsulates startup policy and legacy activation.

4. `core`
- Reusable low-level primitives:
  - RAII wrappers
  - assertions
  - non-domain-specific utilities.

5. `config` and `localization`
- Provide deterministic runtime configuration and message localization.

6. `logging`
- Centralized structured logging boundary.

## 3. Current parity level by module

Implemented:
- CLI compatibility parsing for major host switches.
- V2 startup path (child process + conpty runtime).
- Legacy-selection policy and legacy conhost activation.
- Server-handle and signal-handle validation path.
- Startup default command behavior (`cmd.exe` fallback).
- Conpty startup handshake including inherit-cursor request path.

Still partial:
- Full COM local-server registration/object model for `-Embedding`.
- Full console-driver protocol parity with native `ConServer` object model.
- Full renderer/terminal stack parity with all backends.

