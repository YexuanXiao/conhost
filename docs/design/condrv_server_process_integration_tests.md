# ConDrv Server-Mode Process Integration Tests (`--server --headless`)

## Goal

Validate the classic ConDrv server-mode loop end-to-end with **process isolation**, beyond the in-memory `MemoryComm`
harness:

- a real ConDrv server instance exists,
- `openconsole_new.exe` can host it in `--server --headless` mode,
- a separate console client process can attach to that server and perform basic console I/O,
- the host input/output forwarding path works without deadlocks.

This is intentionally a **smoke** layer: it does not attempt to validate every Win32 console API or full conhost parity.

## Approach

Implemented in `new/tests/process_integration_tests.cpp`:

1. Create a ConDrv server instance in the test process.
2. Spawn `openconsole_new.exe` with:
   - `--server 0x...` pointing at the inherited server handle
   - `--headless` to ensure the non-window server path is exercised
   - redirected stdin/stdout pipes so the test can inject bytes and capture output.
3. Wait for the server to initialize, then create client I/O handles (`\\Input`, `\\Output`) for that server.
4. Spawn a console client process (`oc_new_condrv_client_smoke.exe`) with:
   - inherited ConDrv `\\Input`/`\\Output` handles as standard handles
   - a console reference attribute (`PROC_THREAD_ATTRIBUTE_CONSOLE_REFERENCE`) so the kernel associates the process
     with the correct server instance.
5. Inject host input bytes into the server's stdin pipe and assert the expected output tokens appear on the server's
   stdout pipe.

An additional scenario uses a second purpose-built client (`oc_new_condrv_client_input_events.exe`) to validate that
win32-input-mode sequences injected into the host input pipe are decoded into `ReadConsoleInputW` `KEY_EVENT` records
with virtual-key metadata.

## Why NTDLL Is Used (Tests Only)

Creating ConDrv client handles requires opening object-manager names (for example `\\Input`) **relative to** the ConDrv
server handle (as a root directory). Win32 `CreateFileW` does not support this "root handle + relative name" pattern.

The test harness therefore uses `NtOpenFile` (via `ntdll.dll`) to create:

- `\\Device\\ConDrv\\Server` (server instance)
- `\\Reference` (for `PROC_THREAD_ATTRIBUTE_CONSOLE_REFERENCE`)
- `\\Input` / `\\Output` (client standard handles, created after the server is running)

This usage is constrained to the integration test module and is not part of the production server/runtime code path.

## Limitations / Follow-Ups

- The current coverage is intentionally minimal; expand with additional client scenarios as needed (modes, larger I/O,
  VT output, reply-pending behavior under empty input, etc.).
- The harness uses purpose-built clients (`oc_new_condrv_client_smoke.exe`, `oc_new_condrv_client_input_events.exe`) to
  keep the tests deterministic and avoid relying on external binaries.
