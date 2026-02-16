# ConDrv Reply-Pending Wait Queue (Design)

## Summary

The inbox conhost supports "reply pending" (`CONSOLE_STATUS_WAIT`) for input-dependent ConDrv requests. When a read-like
API cannot make progress yet (no input available, or the head of the byte stream is an incomplete multibyte sequence),
the request is **not completed**. Instead, it is deferred and retried later when input arrives.

This design implements the same high-level behavior in the replacement:

- `dispatch_message(...)` never blocks waiting for input.
- Input reads that would block return `reply_pending=true`.
- The ConDrv server loop queues reply-pending requests and retries them when input becomes available.

This prevents a single blocking read from stalling the entire server loop (`IOCTL_CONDRV_READ_IO` processing).

## Upstream Reference (Local Conhost Source Tree)

These files are the behavioral reference for the reply-pending pattern:

- `src/server/ApiSorter.cpp`
  - Returns `nullptr` when `ReplyPending == TRUE`, so the IO is not completed yet.
- `src/server/IoSorter.cpp`
  - Similar pattern for RAW_READ/RAW_WRITE.
- `src/server/ApiDispatchers.cpp`
  - Sets `*pbReplyPending = TRUE` and registers a wait via `ConsoleWaitQueue::s_CreateWait(...)`.
- `src/server/WaitQueue.*`, `src/server/WaitBlock.*`
  - Stores deferred requests and completes them later when a wait condition is satisfied.

The replacement does not attempt to port the full wait-graph and object model. It implements a minimal FIFO/round-robin
retry queue that preserves the key observable behavior: reply-pending requests do not complete until input is available.

## Replacement Architecture

### 1) Dispatch Outcome Contract

`dispatch_message(...)` now returns a `DispatchOutcome` with:

- `reply_pending=false`:
  - The caller must release message buffers and complete IO (`WRITE_OUTPUT` + `COMPLETE_IO`).
- `reply_pending=true`:
  - The caller must *not* release buffers or complete IO.
  - The message must be retained and retried later.

This mirrors the upstream "do not complete when reply is pending" rule.

### 2) Persisted Per-Handle Decode State (Required For Retry)

Reply-pending means an API call can be re-attempted later using the *same* ConDrv message. Any "in-progress" decode
state must survive across retries. The replacement stores this state in the per-input-handle `ObjectHandle`:

- `ObjectHandle::pending_input_bytes`
  - Fixed-size (64 bytes) prefix buffer used to persist drained incomplete VT/UTF-8/DBCS sequences.
- `ObjectHandle::cooked_line_in_progress`
  - Cooked `ReadConsole` line assembly buffer that persists across reply-pending waits.

Flush operations clear this state (see "Flush Semantics").

### 3) "Pend Instead Of Block" Rules

For input-dependent reads:

1. If the operation can complete immediately (pending UTF-16 unit, pending cooked output, or decodable bytes in queue),
   complete synchronously.
2. If it cannot produce any output now and waiting is allowed, return `reply_pending=true`.
3. If waiting is not allowed (NOWAIT flags), return success with 0 output (current conhost-like behavior).

Split multibyte sequences (`need_more_data`) are handled by draining any queued bytes for the incomplete prefix into
`ObjectHandle::pending_input_bytes`. If no output has been produced yet, the request reply-pends rather than busy
spinning on "bytes available but undecodable".

Cooked line-input reads (`ENABLE_LINE_INPUT`) are resumable:

- The implementation consumes only currently available bytes and appends decoded characters to
  `cooked_line_in_progress`.
- The request completes only when CR/LF termination is observed and the completed line is moved into
  `cooked_read_pending`.
- If the queue runs empty before the line is complete, the request reply-pends (no partial-line delivery).

### 4) Minimal Server-Side Pending Queue

The ConDrv server loop (`condrv_server.cpp`) maintains:

- `std::deque<ConDrvApiMessage> pending_replies;`

Behavior:

- When `dispatch_message(...)` returns `reply_pending=true`, the message is moved into `pending_replies` and not
  completed.
- Each main-loop iteration calls `service_pending_until_stalled()`:
  - Tries each pending message once (round-robin).
  - Completes any that are now ready.
  - Stops when a full pass makes no progress (prevents tight loops).

On shutdown / disconnect, all remaining pending requests are completed with `STATUS_UNSUCCESSFUL` and `Information=0`
so no client remains hung.

### 5) Waking The Server Thread

The server thread normally blocks in `IOCTL_CONDRV_READ_IO`. To retry reply-pending requests promptly when input
arrives, the input monitor thread can wake the server thread by canceling that synchronous device read.

The replacement uses `CancelSynchronousIo` guarded by two atomics:

- `has_pending_replies`:
  - True when the pending reply queue is non-empty.
- `in_driver_read_io`:
  - True only while the server thread is inside `comm->read_io(...)`.
  - Maintained via an RAII guard (`AtomicFlagGuard`).

The input monitor calls `CancelSynchronousIo(server_thread)` only when:

- there are pending replies, and
- the server thread is currently in the driver `read_io` call.

This avoids canceling unrelated synchronous IO and limits cancellation to the intended "wake-and-retry" point.

The server loop treats `ERROR_OPERATION_ABORTED` / `ERROR_CANCELLED` from `read_io` as a non-fatal wakeup signal when a
stop is not requested.

## Flush Semantics

Operations that flush the input queue also clear per-handle decode/cooked state:

- `ObjectHandle::decoded_input_pending`
- `ObjectHandle::pending_input_bytes`
- `ObjectHandle::cooked_read_pending`
- `ObjectHandle::cooked_line_in_progress`

This prevents stale partial-decode state from influencing subsequent reads after a flush.

## Limitations / Follow-Ups

- The pending queue is global FIFO/round-robin, not a full per-process/per-object wait-graph like upstream.
- There is no cancellation model for individual pending requests yet (beyond shutdown/disconnect failure completion).
- Input is still byte-stream-backed with minimal `KEY_EVENT` synthesis; richer input events remain future work.
