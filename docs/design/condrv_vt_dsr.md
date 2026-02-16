# ConDrv Output VT Processing: Device Status Report (DSR) and Cursor Position Report (CPR)

## Goal
Some VT-aware applications query terminal state by writing DSR sequences to the output stream and
then reading the reply from `STDIN`.

The most common request is CPR (Cursor Position Report):

- request: `CSI 6 n` (or `CSI ? 6 n` for the "extended" form)
- reply: `CSI <row> ; <col> R` (or `CSI ? <row> ; <col> ; <page> R`)

This microtask adds minimal DSR support to the replacement so interactive TUIs that probe the
cursor position do not hang waiting for a reply when running against the classic window host.

## Upstream Reference (Local Source Tree)
In the inbox host, DSR parsing and responses are implemented in the VT adapter layer:

- Parsing:
  - `src/terminal/parser/OutputStateMachineEngine.cpp` (`CsiActionCodes::DSR_DeviceStatusReport`)
- Dispatch and reply injection:
  - `src/terminal/adapter/adaptDispatch.cpp`
    - `AdaptDispatch::DeviceStatusReport(...)`
    - `AdaptDispatch::_CursorPositionReport(...)` (injects reply into the input channel)

The upstream adapter distinguishes ANSI vs DEC-private DSR (`CSI 6 n` vs `CSI ? 6 n`) and sends an
extended reply for the private form.

## Replacement Semantics (Compact)
The replacement consumes `CSI ... n` sequences when `ENABLE_VIRTUAL_TERMINAL_PROCESSING` is set and
implements only:

- `CSI 5 n` (Operating Status):
  - reply injected to input queue: `ESC[0n`
- `CSI 6 n` (CPR):
  - reply injected to input queue: `ESC[<row>;<col>R`
- `CSI ? 6 n` (extended CPR request):
  - reply injected to input queue: `ESC[?<row>;<col>;1R`
  - page is always reported as `1` (the replacement has no multi-page model)

### Cursor origin and origin-mode interaction
The replacement reports the cursor relative to the current window (viewport) origin:

- `row = (cursor_y - window_top) + 1`
- `col = (cursor_x - window_left) + 1`

When origin mode (DECOM, `CSI ? 6 h/l`) is enabled, the reported row becomes relative to the active
DECSTBM top margin (no horizontal margins are modeled):

- `row = (cursor_y - margin_top) + 1`

### When to answer queries
In ConPTY scenarios, output is forwarded to an external terminal that may answer status queries
itself. To avoid duplicate replies, answering DSR is gated by the host IO policy:

- `HostIo::vt_should_answer_queries() == true`: inject replies
- `false`: consume DSR with no injected reply

`ConDrvServer::HostIo` answers queries only when there is no external output target.

## Implementation
- `new/src/condrv/condrv_server.hpp`
  - `apply_text_to_screen_buffer(...)` recognizes `CSI ... n` and injects replies via
    `HostIo::inject_input_bytes(...)` when allowed by `vt_should_answer_queries()`.
  - The minimal CSI parser records a `private_marker` boolean when encountering `?` so we can
    distinguish `CSI ? 6 n` from `CSI 6 n`.

## Tests
`new/tests/condrv_raw_io_tests.cpp`

- `test_write_console_vt_dsr_cpr_injects_response_into_input_queue`
  - writes `A CSI 6 n B` and verifies a CPR reply (`ESC[1;2R`) is readable from `RAW_READ`
- `test_write_console_vt_dsr_cpr_respects_host_query_policy`
  - sets `answer_vt_queries=false`, writes `CSI 6 n`, and verifies no bytes are injected

## Limitations / Follow-ups
- Only DSR ids `5` and `6` are implemented.
- The "extended" reply always reports page `1` (no page model).
- DSR is only answered when the host IO policy opts in; external terminals are expected to answer
  in forwarded-output configurations.

