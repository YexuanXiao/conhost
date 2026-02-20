# OpenConsole Reimplementation (Incremental)

This folder contains a clean-room, incremental reimplementation of OpenConsole
using modern C++23, CMake, and Windows SDK APIs.

Current status:
- Foundation architecture and roadmap documentation are in `docs/`.
- Source architecture analysis is documented in:
  - `docs/conhost_source_architecture.md`
  - `docs/conhost_module_partition.md`
  - `docs/conhost_behavior_imitation_matrix.md`
  - `docs/number_serdes_research.md`
  - `docs/test_coverage_plan.md`
- A new executable entrypoint and compatibility-focused command-line parser are implemented.
- A ConPTY-backed runtime session host is implemented for headless/pipe modes.
- Non-GUI unit tests are included and wired through CTest.

Build:
```powershell
cmake -S new -B build-new -G Ninja -DCMAKE_CXX_COMPILER=clang-cl
cmake --build build-new
ctest --test-dir build-new --output-on-failure
```

Default terminal (dev):
- To register `openconsole_new` as the per-user Windows “default terminal” (classic `IConsoleHandoff` delegation), see `docs/howto/default_terminal.md`.

Key runtime configuration environment variables:
- `OPENCONSOLE_NEW_CONFIG`: optional config file path.
- `OPENCONSOLE_NEW_LOG_LEVEL`: `trace|debug|info|warning|error`.
- `OPENCONSOLE_NEW_ENABLE_FILE_LOGGING`: `1` to enable file logs (`0` default).
- `OPENCONSOLE_NEW_LOG_DIR`: optional log directory. If omitted and file logging is enabled, defaults to `%TEMP%\\console` (falls back to `%TMP%\\console` when `%TEMP%` is unset). Log filename is fixed to `console_<pid>_<process_start_filetime>.log`.
- `OPENCONSOLE_NEW_BREAK_ON_START`: `1` to wait for a debugger on startup, then break into it before normal execution continues.
- `OPENCONSOLE_NEW_HOLD_ON_EXIT`: `1` to keep the `openconsole_new` window open after the hosted client exits (`0` default).
- `OPENCONSOLE_NEW_PREFER_PTY`: `1` (default) or `0`.
- `OPENCONSOLE_NEW_ALLOW_EMBEDDING_PASSTHROUGH`: `1` (default) or `0`.
- `OPENCONSOLE_NEW_ENABLE_LEGACY_PATH`: `1` (default) or `0`.
- `OPENCONSOLE_NEW_EMBEDDING_WAIT_MS`: `0` (default, infinite) or timeout in milliseconds.
