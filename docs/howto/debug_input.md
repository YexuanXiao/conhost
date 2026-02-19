# Debugging Input (Headless ConPTY + Classic Window)

This guide is for diagnosing cases where output renders but **user input is not processed**.

## Enable Logs

Set environment variables (PowerShell examples):

- `setx OPENCONSOLE_NEW_ENABLE_FILE_LOGGING 1`
- `setx OPENCONSOLE_NEW_LOG_LEVEL trace`
- `setx OPENCONSOLE_NEW_LOG_DIR C:\\temp\\openconsole_new_logs` (optional)
- `setx OPENCONSOLE_NEW_DEBUG_SINK 1` (optional; also logs to `OutputDebugStringW`)
- `setx OPENCONSOLE_NEW_BREAK_ON_START 1` (optional; breaks into a debugger on startup)

On startup, the log prints the resolved file path as:

- `File logging enabled at ...`

## Headless ConPTY (`--server --headless`)

Look for these log lines:

- `Server-handle startup: ... host_input=... (type=...), host_output=... (type=...)`
  - In Windows Terminal / ConPTY, both input and output should typically be `FILE_TYPE_PIPE`.
- `Emitting VT handshake for headless server startup`
  - If it is skipped, the log explains why (for example output is not a pipe).
- `ConDrv host I/O: host_input=... type=..., host_output=... type=...`
- `Input monitor thread started ...`
- When typing, you should see:
  - `Input monitor read N bytes from host input`

If a client read reply-pends, the server reports it and the wake path becomes visible:

- `Reply-pending: function=... object=...` (and `api=...` for USER_DEFINED messages)
- `Input monitor wake: CancelSynchronousIo(...), CancelIoEx(...)`
- `ConDrv read_io canceled (wake)`

## Classic Window (`--server` without `--headless`)

Expected differences:

- `Skipping VT handshake for headless server startup: host output is not a pipe ...`
- `ConDrv host I/O: ... host_output=0x0 ...` (output is rendered via screen-buffer snapshots)

Typing in the window should still produce:

- `Input monitor read N bytes from host input`

If the input monitor never reads bytes in windowed mode, the window is not forwarding keyboard messages into the input sink.

