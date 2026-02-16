# Architecture

## Design Principles
- Strict RAII for every owning Win32 resource.
- Exceptions for non-local failures; `std::expected` for immediate parse/config errors.
- `wchar_t` and `W`-suffix APIs for all non-Unicode-conversion operations.
- Functional decomposition into standard-library-style classes with clear ownership.

## Module Layout
- `src/core`
  - Low-level utilities: assertions, exceptions, owning handle wrappers, process launching.
  - Host handoff wire-protocol definitions (`core/host_signals.hpp`) for `-Embedding` signal pipe interop.
- `src/condrv`
  - Minimal ConDrv protocol definitions, `DeviceIoControl` wrapper, and an initial server-mode dispatcher/loop for `--server` startup.
- `src/cli`
  - Compatibility parser for OpenConsole command-line behavior.
- `src/config`
  - Runtime configuration loading from environment and optional config file.
- `src/localization`
  - Locale selection and localized message retrieval.
- `src/logging`
  - Internal logging library with pluggable sinks.
- `src/serialization`
  - Fast integer/floating-point serialization/deserialization module for non-locale numeric conversion.
- `src/runtime`
  - Session runtime, pseudo console process hosting, handle validation, keyboard-to-VT encoding, and startup launch-policy compatibility (`ForceV2`/`ForceV1` legacy decision path).
- `src/renderer`
  - Non-WinUI rendering building blocks for the classic conhost window (font/cell metrics, minimal Win32 window host skeleton, future layout/paint backends).
- `src/app`
  - High-level orchestration and executable entrypoint.
- `tests`
  - Non-GUI unit tests for parser, logger, and configuration/localization behavior.

## Ownership and Lifetime
- `Application` owns:
  - `AppConfig`
  - `Localizer`
  - `Logger`
  - Parsed `ConsoleArguments`
- `Logger` owns sink objects through `std::shared_ptr`.
- All Win32 handles are wrapped in dedicated move-only RAII classes.

## Error Model
- Immediate caller-recoverable operations return `std::expected<T, E>`.
- Non-recoverable operational failures throw custom exceptions carrying Win32 error data.
- Assertions are used to catch invariant violations during development.

## Increment Strategy
- Keep each phase buildable and testable.
- Introduce behavior-compatible slices first (CLI, startup contract).
- Port deeper runtime behavior only after baseline correctness and observability are established.
