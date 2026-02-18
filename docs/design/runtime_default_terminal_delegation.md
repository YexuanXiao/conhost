# Default-Terminal Delegation (IConsoleHandoff)

This document describes the **default-terminal delegation** path used by `openconsole_new` when it is started as a classic (windowed) ConDrv server (`--server` mode, not headless, not ConPTY).

It also records a bug analysis driven by a real log trace and the corresponding behavior parity decision relative to the in-tree (old) OpenConsole implementation under `src/`.

## 1. Terminology and registry keys

There are two distinct delegation mechanisms in the Windows console ecosystem:

1. **Console host handoff** (this document):
   - COM interface: `IConsoleHandoff`
   - Registry value: `HKCU\Console\%%Startup\DelegationConsole`
   - Purpose: hand off *hosting/rendering* of a ConDrv console session to another host process, while the original host stays alive for PID continuity and privileged operations.

2. **Terminal/ConPTY handoff** (separate module):
   - Implemented in `new/src/runtime/terminal_handoff.*`
   - Registry value: `HKCU\Console\%%Startup\DelegationTerminal`
   - Purpose: request pipes and lifetime signaling for ConPTY transport (headless VT mode).

This document is about (1).

## 2. Upstream (old) implementation summary

The in-tree OpenConsole implementation performs a handoff attempt during the client connect routine:

- `src/server/IoDispatchers.cpp::attemptHandoff`
  - Builds a `CONSOLE_PORTABLE_ATTACH_MSG` from the first `IOCTL_CONDRV_READ_IO` packet.
  - Obtains the ConDrv server handle via the device comm stack (`GetServerHandle`).
  - Creates a host-signal pipe (read end kept by the original host; write end given to the delegated host).
  - Duplicates a handle to the current process and passes it to the delegated host.
  - Calls `IConsoleHandoff::EstablishHandoff(...)`.
  - Starts `src/interactivity/base/HostSignalInputThread` to service host-signal requests from the delegated host.
  - Waits on the process handle returned by `EstablishHandoff`, then exits (`ExitProcess(S_OK)`).

Notable behavior:

- `HostSignalInputThread` treats pipe break (`ERROR_BROKEN_PIPE`) as a **shutdown** signal and calls `ServiceLocator::RundownAndExit(ERROR_BROKEN_PIPE)`.
- `ConsoleControl::EndTask` is treated as best-effort: the return NTSTATUS is not used to decide whether to forcibly terminate the process.

## 3. New implementation summary

The replacement (`new/src/runtime/session.cpp`) performs a compatibility-focused delegation probe before creating a classic window:

1. Creates an input-availability event and registers it with the driver (`IOCTL_CONDRV_SET_SERVER_INFORMATION`).
2. Reads the first driver packet (`IOCTL_CONDRV_READ_IO`) to build a minimal `CONSOLE_PORTABLE_ATTACH_MSG`.
3. Creates a host-signal pipe pair and duplicates a handle to the inbox host process.
4. Calls `IConsoleHandoff::EstablishHandoff(...)`.
5. On success:
   - starts `new/src/runtime/host_signal_input_thread.*` to service host-signal packets.
   - waits for the delegation lifetime to end (pipe closure and/or `\\Reference` handle, with delegated-process handle used only for observation/logging).
6. On failure:
   - falls back to the classic windowed server loop and replays the already-consumed initial packet via `ConDrvServer::run_with_handoff(...)`.

## 4. Bug analysis: noisy/incorrect shutdown fallback after pipe disconnect

### 4.1 Symptom (log excerpt)

When the delegated host closes or crashes, the host-signal pipe read loop terminates and the fallback path emits:

- `ConsoleControl(EndTask, pid=...) failed (ntstatus=0xC0000001, error=31); falling back to TerminateProcess`
- `TerminateProcess failed for EndTask fallback (..., error=5)`

### 4.2 Root cause (old vs new)

The old implementation documents an important quirk of `ConsoleControl(EndTask)`:

- In `src/host/input.cpp`, the OpenConsole code notes that `EndTask()` can return `STATUS_UNSUCCESSFUL` (`0xC0000001`) even when:
  - the target process is already dead, **or**
  - the request was accepted and will be acted on by CSRSS (which may wait and then force-kill).

Because of this, upstream treats `EndTask` as best-effort and does **not** use the return value to decide on a `TerminateProcess` fallback.

The new implementation initially treated any negative NTSTATUS as a hard failure and attempted `OpenProcess(PROCESS_TERMINATE)` + `TerminateProcess`. This frequently fails with `ERROR_ACCESS_DENIED` (5) for processes outside our termination rights, producing noisy logs and potentially diverging from the intended “send console close semantics” behavior.

### 4.3 Fix / parity decision

`openconsole_new` now mirrors inbox behavior:

- `ConsoleControl(EndTask, ...)` remains best-effort.
- The replacement **does not** fall back to `TerminateProcess` based on the NTSTATUS alone.

This removes spurious access-denied failures and aligns with the upstream observation that CSRSS is the actor responsible for enforcing shutdown.

## 5. References (local source)

- Upstream handoff: `src/server/IoDispatchers.cpp::attemptHandoff`
- Upstream host-signal reader: `src/interactivity/base/HostSignalInputThread.cpp`
- Upstream `EndTask` return value caveat: `src/host/input.cpp`
- Replacement implementation: `new/src/runtime/session.cpp`, `new/src/runtime/host_signal_input_thread.*`

