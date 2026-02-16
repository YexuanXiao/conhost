# ConDrv Command History (Non-GUI)

## Goal
Implement the classic conhost command history behavior used by legacy L3 APIs:

- `ConsolepExpungeCommandHistory`
- `ConsolepSetNumberOfCommands`
- `ConsolepGetCommandHistoryLength`
- `ConsolepGetCommandHistory`

and record cooked line-input reads (`ReadConsole` with `ENABLE_LINE_INPUT`) into per-executable histories.

This is a non-GUI feature and is purely state/model logic.

## Upstream Reference (Local Source)
- History storage and LRU pool:
  - `src/host/history.h`
  - `src/host/history.cpp`
- History allocation at connect:
  - `src/server/IoDispatchers.cpp` (calls `CommandHistory::s_Allocate` using `Cac.AppName`)
  - `dep/Console/conmsgl1.h` (`CONSOLE_SERVER_MSG` includes `ApplicationName[128]`)
- L3 API dispatch and input buffer usage:
  - `src/server/ApiDispatchers.cpp` (`Server*ConsoleCommandHistory*` functions)
- History insertion for cooked reads:
  - `src/host/readDataCooked.cpp` (`_history->Add(...)` on Enter when echo is enabled)

## Replacement Design

### Storage Model
The replacement keeps a small in-process pool of `CommandHistory` entries in `ServerState`:

- `CommandHistoryPool` stores a `std::list<CommandHistory>` so entries can be moved to the MRU front without invalidating pointers.
- Each `CommandHistory` contains:
  - `app_name` (the EXE/app key used for L3 history APIs)
  - `allocated` + `process_handle` (the currently associated process, if any)
  - `commands` (`std::vector<std::wstring>`, oldest-first)
  - `max_commands` (per-entry limit)

This mirrors the upstream "LRU pool of histories" design. A freed history keeps its command list for possible reuse by a future process with the same `app_name`.

### Allocation and Lifetime
- On `CONNECT`:
  - Parse the `CONSOLE_SERVER_MSG` input buffer and extract `ApplicationNameLength` + `ApplicationName`.
  - `ServerState::connect_client(...)` calls `CommandHistoryPool::allocate_for_process(app_name, process_handle, buffer_count, buffer_size)`.
  - History allocation is best-effort: failures must not block `CONNECT`.
- On `DISCONNECT`:
  - `ServerState::disconnect_client(...)` calls `CommandHistoryPool::free_for_process(process_handle)`.

### Recording Cooked Reads
When `ConsolepReadConsole` runs in cooked line-input mode (`ENABLE_LINE_INPUT`) and a line completes on Enter:

- If `ENABLE_ECHO_INPUT` is set, record the line (without the `\\r`/`\\r\\n` suffix) into the history for the owning process.
- Duplicate suppression uses the Win32 history flag:
  - `HISTORY_NO_DUP_FLAG` (from `ConsolepSetHistory` / `ConsolepGetHistory`)
- History insertion is best-effort: failures must not affect the read completion.

### L3 API Behavior
All four APIs take the EXE name in the message input buffer. The encoding is controlled by the API's `Unicode` flag (matching upstream behavior).

1. `ConsolepExpungeCommandHistory`
   - Decodes EXE name from input buffer.
   - Clears `commands` for the matching allocated history (if present).
   - Returns `STATUS_SUCCESS`, `Information=0`.

2. `ConsolepSetNumberOfCommands`
   - Decodes EXE name from input buffer.
   - If a matching allocated history exists, sets its per-entry `max_commands` and truncates stored commands (vector truncation semantics match upstream).
   - Returns `STATUS_SUCCESS`, `Information=0`.

3. `ConsolepGetCommandHistoryLength`
   - Decodes EXE name.
   - Returns the byte size required by `ConsolepGetCommandHistory`:
     - Unicode: `sum((cmd.size() + 1) * sizeof(wchar_t))`
     - ANSI: `sum(WideCharToMultiByte(cmd) + 1)`
   - Unknown EXE returns `0`.
   - Returns `STATUS_SUCCESS`, `Information=0`.

4. `ConsolepGetCommandHistory`
   - Decodes EXE name.
   - Writes the command history into the output buffer as a sequence of null-terminated strings:
     - Unicode output: UTF-16 units + `L'\\0'` after each command.
     - ANSI output: code-page bytes + `'\\0'` after each command.
   - `CommandBufferLength` and `Information` report the number of bytes written.
   - If the buffer is too small for the full history: return `STATUS_BUFFER_TOO_SMALL` and do not complete partial output (matches upstream overflow behavior).

## Limitations / Follow-ups
- No interactive history navigation (VK_UP/DOWN, F7 menu, etc.).
- No `doskey` alias expansion integration (separate microtask).
- No persistence beyond the lifetime of the running `--server` instance.

