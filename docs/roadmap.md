# Roadmap

## Goals
- Reimplement OpenConsole in `new/` with modern C++23 and strict resource safety.
- Keep command-line compatibility with the original host executable.
- Support Windows 10 and Windows 11 with Unicode-first behavior (`wchar_t`, `W` APIs).
- Track incremental implementation microtasks in `docs/microtask_backlog.md`.

## Phases
1. Foundation (completed in this increment)
- Standalone CMake/Ninja project with strict compiler settings.
- Core RAII wrappers for Win32 resources and error propagation.
- Compatibility command-line parser and baseline process launch flow.
- Logging, localization, configuration, and non-GUI tests.

2. Host Runtime Core
- Reimplement server/session lifecycle classes. (in progress)
- Rework object lifetime ownership graph with explicit RAII boundaries.
- Add richer diagnostics and structured log categories.

3. Console I/O and Rendering Integration
- Incrementally port buffer/input/output control paths.
- Introduce DWrite-backed text measurement and rendering abstractions.
- Add explicit feature toggles for legacy vs modern behavior where required.

4. ConPTY and Interop
- Implement server handle and signal handle workflows. (in progress)
- Add COM-based handoff path only where strictly necessary.
- Validate behavior against original CLI and startup scenarios.

5. Hardening and Validation
- Expand unit/integration tests for parser, state transitions, and I/O behavior.
- Document all used Windows APIs and localization assets.
- Final compatibility pass and performance profiling.
