# User Startup Config File (`~/.conhost`)

## Goal

Allow `openconsole_new` to load configuration automatically from a per-user file at startup, without requiring `OPENCONSOLE_NEW_CONFIG` to be set.

This provides a stable user-level baseline while keeping existing environment-variable controls for automation and test scenarios.

## Behavior Summary

`ConfigLoader::load()` applies configuration in this order:

1. User baseline file: `~/.conhost` (best effort).
2. Explicit file path from `OPENCONSOLE_NEW_CONFIG` (strict).
3. Environment variable overrides (`OPENCONSOLE_NEW_LOG_LEVEL`, `OPENCONSOLE_NEW_LOCALE`, etc.).

Later sources override earlier sources.

## User Home Resolution

The user config path is resolved in this priority order:

1. `USERPROFILE`
2. `HOME`
3. `HOMEDRIVE` + `HOMEPATH`

The filename `.conhost` is appended to the resolved directory.

## Error Policy

For the user baseline file (`~/.conhost`):

- `ERROR_FILE_NOT_FOUND` and `ERROR_PATH_NOT_FOUND` are ignored.
- Any other read/parse error is returned as `ConfigError`.

For `OPENCONSOLE_NEW_CONFIG`:

- Missing file and parse errors are returned as `ConfigError` (no silent ignore).

This keeps default startup resilient while preserving strict behavior for explicit operator-provided config paths.

## File Format

The file format is unchanged:

- `key=value` lines
- `#` and `;` comments
- UTF-8 text (or UTF-16LE with BOM)

Supported keys are handled by `apply_key_value(...)` in `new/src/config/app_config.cpp`.

Logging-specific keys:

- `enable_file_logging=0|1` (default `0`)
- `log_dir=<path>` (also implicitly enables file logging when non-empty)
- `break_on_start=0|1` (default `0`)

Environment overrides:

- `OPENCONSOLE_NEW_ENABLE_FILE_LOGGING`
- `OPENCONSOLE_NEW_LOG_DIR`
- `OPENCONSOLE_NEW_BREAK_ON_START`

When file logging is enabled and `log_dir` is empty, runtime chooses:

- `%TEMP%\\console\\console_<pid>_<process_start_filetime>.log`
- falls back to `%TMP%` when `%TEMP%` is unavailable.

The log filename is not configurable. Only the directory can be configured.

Startup debug hold behavior:

- when `break_on_start` is enabled, startup loops until `IsDebuggerPresent()` is true.
- while no debugger is attached, the process sleeps for 1 second per poll.
- once attached, `DebugBreak()` is raised so the debugger suspends execution.
- after the debugger continues execution, normal startup resumes.

## Tests

Coverage is implemented in `new/tests/config_tests.cpp`:

- `test_user_profile_config_is_loaded`
- `test_explicit_config_path_overrides_user_profile_config`
- `test_missing_user_profile_config_is_ignored`

## Implementation Reference

- `new/src/config/app_config.cpp`
  - `resolve_default_user_config_path()`
  - `load_config_file_into(...)`
  - `ConfigLoader::load()`
