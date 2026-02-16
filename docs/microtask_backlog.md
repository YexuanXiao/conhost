# Microtask Backlog

This document breaks the macro-goal ("replace conhost/openconsole") into microtasks that can be implemented and tested incrementally under the constraints in `AGENTS.md`.

## 1) ConDrv Server-Mode (Core Replacement)

Goal: handle `--server` startup and service ConDrv IOs.

- [x] Define minimal ConDrv protocol surface (IOCTLs + core structs).
- [x] Implement a `DeviceIoControl` wrapper with RAII-owned server handle.
- [x] Define a layout-stable "packet" struct for `IOCTL_CONDRV_READ_IO` (descriptor + message header union).
- [x] Implement a minimal `ConDrvApiMessage` class with:
  - input/output buffer acquire (`READ_INPUT`/`WRITE_OUTPUT`)
  - output write on buffer release (success-only)
  - explicit exception safety and cleanup
- [x] Add a `COMPLETE_IO` helper to the message wrapper.
- [x] Implement connect/disconnect handling:
  - accept connection and return `CD_CONNECTION_INFORMATION`
  - track per-process state and lifetime
- [x] Implement minimal handle/object model:
  - current input/output handles (placeholder objects)
  - create/close object (current input/output + new output)
- [ ] Implement minimal API dispatch set for basic CLI apps:
  - [x] USER_DEFINED subset: Get/SetMode, GetCP/SetCP, GetNumberOfInputEvents
  - [x] USER_DEFINED subset: WriteConsole / ReadConsole (forwarding to host IO + in-memory ScreenBuffer updates for WriteConsole; UTF-16 -> UTF-8 for host output)
    - Notes: the buffer model implements minimal processed-output control characters (`\\r`, `\\n`, `\\b`, `\\t`), respects `DISABLE_NEWLINE_AUTO_RETURN` for `\\n`, and consumes minimal VT sequences when `ENABLE_VIRTUAL_TERMINAL_PROCESSING` is set:
    - CSI SGR `m` (including extended colors `38/48` approximated to legacy attributes), cursor moves `H`/`f`/`A`/`B`/`C`/`D`/`E`/`F`/`G`/`` ` ``/`d`, ED/EL `J`/`K` (supports both 7-bit `ESC [` and C1 `U+009B` encodings)
    - consumes ESC dispatch sequences so common legacy controls do not render escape bytes:
      - charset designation (e.g. `ESC ( 0`, `ESC ( B`) is treated as a no-op but consumed
      - string introducers (DCS/PM/APC/SOS) are consumed (payload ignored) until ST, including when the terminator is split across writes
    - OSC window title: `OSC 0/1/2/21 ; <title> BEL/ST` (updates `ConsolepGetTitle` state; consumed, not rendered)
    - DSR/CPR: `CSI 5n` / `CSI 6n` (injects replies into the input queue when hosted without an external terminal)
    - DECSTR soft reset: `CSI ! p` (resets VT mode state without clearing the screen buffer)
    - RIS hard reset: `ESC c` (clears the screen buffer and homes the cursor)
    - DECSTBM scroll region: CSI `r`
    - vertical scrolling and line ops within margins:
      - scroll up/down: CSI `S`/`T`
      - insert/delete line: CSI `L`/`M`
      - index/reverse index: `ESC D` / `ESC M`
      - next line (with return): `ESC E` (NEL)
      - screen alignment pattern: `ESC # 8` (DECALN)
    - horizontal editing (line-local):
      - insert/delete characters: CSI `@` / CSI `P`
      - erase characters: CSI `X`
    - insert/replace mode: IRM (CSI `4h`/`4l`)
    - cursor save/restore: CSI `s`/`u` (no-parameter `s`), DECSC/DECRC `ESC7`/`ESC8`
    - cursor visibility: DECTCEM (CSI `?25h`/`?25l`)
    - origin mode: DECOM (CSI `?6h`/`?6l`) for cursor addressing relative to DECSTBM margins
    - alternate screen buffer: DECSET/DECRST (CSI `?1049h`/`?1049l`)
    - autowrap: DECAWM (CSI `?7h`/`?7l`) including delayed-wrap ("last column flag") semantics
  - Notes: `ReadConsole` now implements a minimal cooked line-input path when `ENABLE_LINE_INPUT` is set:
    - buffers input until CR/LF, appends CRLF when `ENABLE_PROCESSED_INPUT` is set
    - supports backspace editing and `ENABLE_ECHO_INPUT` echo to the output buffer model
    - supports basic VK-based line editing (Left/Right/Home/End/Insert/Delete/Escape and Ctrl+Left/Right/Home/End; ctrl variants require key-event metadata such as ConPTY win32-input-mode)
    - preserves leftover output when the caller buffer is too small (per-input-handle pending buffer)
    - ANSI reads encode UTF-16 pending output using the input code page and return `STATUS_BUFFER_TOO_SMALL` when the buffer cannot hold even one encoded character
  - Notes: raw `ReadConsoleW` avoids spinning on split UTF-8/DBCS sequences by draining partial bytes into a per-handle prefix buffer and reply-pending until the sequence completes.
  - Notes: raw `ReadConsoleW` and `ConsolepGetConsoleInput` split decoded surrogate pairs across calls when the caller buffer can hold only one UTF-16 unit/record, using per-input-handle `decoded_input_pending`.
  - Notes: processed input now handles Ctrl+C in `ReadConsole`:
    - forwards `CTRL_C_EVENT` via the host-signal pipe (`HostSignals::end_task`) when available
    - cooked line-input `ReadConsole` terminates with `STATUS_ALERTED` (0 bytes)
    - raw `ReadConsole` filters out `0x03` bytes (including mid-buffer) and continues waiting for real input
    - Ctrl+Break (`VK_CANCEL` with Ctrl pressed) is supported when virtual-key metadata is available (ConPTY win32-input-mode):
      - flushes the input buffer
      - forwards `CTRL_BREAK_EVENT`
      - terminates raw/cooked reads with `STATUS_ALERTED`
  - Notes: processed input now filters Ctrl+C out of `ConsolepGetConsoleInput` (ReadConsoleInput/PeekConsoleInput):
    - does not return `0x03` as a `KEY_EVENT`
    - continues decoding so callers can still receive the requested number of records when other input exists
  - [x] USER_DEFINED subset: Cursor + screen buffer state (Get/SetCursorInfo, Get/SetCursorPosition, Get/SetScreenBufferInfo, SetTextAttribute, SetScreenBufferSize, GetLargestWindowSize, SetWindowInfo)
  - Notes: viewport state now tracks an inclusive window rect; `ScrollPosition` reflects the viewport origin and `CurrentWindowSize` matches the inbox conhost delta encoding (`Right-Left`, `Bottom-Top`).
  - [x] USER_DEFINED subset: Output buffer contents (FillConsoleOutput, Read/WriteConsoleOutputString, Read/WriteConsoleOutput, ScrollConsoleScreenBuffer)
  - [x] USER_DEFINED subset: Title (GetTitle/SetTitle)
  - [x] USER_DEFINED subset: FlushInputBuffer (clears internal byte queue)
  - [x] RAW_READ/RAW_WRITE/RAW_FLUSH parity:
    - raw byte forwarding to host I/O
    - RAW_WRITE updates the in-memory ScreenBuffer model (code page decode + processed output)
    - RAW_READ implements `ProcessControlZ` (CTRL+Z at start returns 0 bytes but consumes only the marker)
    - RAW_READ filters Ctrl+C (`0x03`) anywhere in the queued byte stream when processed input is enabled, forwarding `CTRL_C_EVENT` via `HostSignals::end_task`
    - RAW_READ consumes supported ConPTY VT input sequences (win32-input-mode key sequences + DA1/focus responses) so escape bytes do not leak to applications; character key events are returned as bytes
    - RAW_FLUSH clears the host input queue for input handles
  - [x] get/set mode (conhost-compatible validation semantics: input applies even when returning invalid parameter; output rejects unknown bits)
  - [x] get/set code pages (GetCP/SetCP)
  - [x] USER_DEFINED subset: ReadConsoleInput/PeekConsoleInput + minimal `INPUT_RECORD` support
  - Notes: implemented via `ConsolepGetConsoleInput` using the internal byte queue, with UTF-8/code-page decoding for Unicode reads (bytes are consumed based on decoded UTF-16 units).
  - Notes: ConPTY VT input decoding is implemented for common sequences:
    - win32-input-mode (`CSI Vk;Sc;Uc;Kd;Cs;Rc _`) is decoded into `KEY_EVENT` records.
    - DA1 responses and focus in/out sequences are consumed (ignored) so they do not leak as input bytes.
    - common VT key sequences (arrows/home/end/ins/del/pgup/pgdn/F1-F4) are recognized as non-character key events.
    - partial VT sequences reply-pend and are drained into the per-handle prefix buffer; see `new/docs/design/condrv_vt_input_decoding.md`.
  - Notes: input-dependent reads use conhost-style reply-pending waits:
    - `dispatch_message(...)` returns `reply_pending=true` instead of blocking.
    - the server loop queues pending replies and retries them when input arrives.
    - split UTF-8/DBCS prefixes are drained into a per-handle prefix buffer to avoid busy-spins on undecodable bytes.
    - see `new/docs/design/condrv_reply_pending_wait_queue.md`.
  - [x] USER_DEFINED subset: WriteConsoleInput (injects keydown character events into the internal byte queue)
  - [x] USER_DEFINED subset: GenerateCtrlEvent / host-signal pipe semantics (CTRL events forwarded via `HostSignals::end_task` when a host-signal pipe is available; otherwise best-effort no-op)
  - [x] USER_DEFINED subset: L3 misc queries (GetConsoleWindow, GetDisplayMode, GetKeyboardLayoutName, GetMouseInfo, GetSelectionInfo, GetConsoleProcessList)
  - [x] USER_DEFINED subset: L3 aliases (AddAlias/GetAlias, GetAliasesLength/GetAliases, GetAliasExesLength/GetAliasExes)
  - [x] USER_DEFINED subset: L3 history (GetHistory/SetHistory; command history APIs stubbed to return empty history)
  - [x] USER_DEFINED subset: L3 font/display APIs (GetNumberOfFonts/GetFontInfo/GetFontSize, Get/SetCurrentFont, SetDisplayMode; SetFont accepted as a compatibility stub)
  - [x] USER_DEFINED subset: L3 legacy compatibility stubs (SetKeyShortcuts, SetMenuClose, CharType, CursorMode, NlsMode, OS2 toggles, LocalEUDC)
  - [x] USER_DEFINED deprecated legacy APIs (MapBitmap + legacy UI/VDM/hardware-state APIs): explicitly rejected with `STATUS_NOT_IMPLEMENTED` and sanitized descriptor bytes
  - [x] Multi-screen-buffer support: `io_object_type_new_output`, `ConsolepSetActiveScreenBuffer`, per-buffer state
  - Notes: each output handle now references a `ScreenBuffer` object (separate cell/state storage). `ConsolepSetActiveScreenBuffer` updates the active buffer used by subsequent `io_object_type_current_output` handle creation.
- [x] Add deterministic unit tests for message parsing and buffer routines.
- [ ] Add higher-level integration tests with process isolation once core IO paths exist.

## 2) COM `-Embedding` Handoff (Replacement Delegation)

Goal: accept the inbox conhost handoff and become the active console host.

- [x] Implement COM local-server registration (`REGCLS_SINGLEUSE`).
- [x] Implement `IConsoleHandoff` object and duplicate/capture handoff payload.
- [x] Implement `ConsoleEstablishHandoff`-equivalent transfer:
  - use captured attach message + server handle to resume servicing the pending IO using a single shared server state
  - start server-mode loop against the handed-off driver handle
- [x] Implement host-signal pipe write-side helpers (initial): `core::write_host_signal_packet` + `HostSignals::end_task` forwarding for `ConsolepGenerateCtrlEvent`.
- [ ] Add end-to-end tests (requires a harness that can simulate or observe handoff).

## 3) Renderer/UI (Classic Conhost Window)

Goal: render the console model without relying on WinUI.

- [x] Define renderer interfaces and a DWrite-backed text measurement implementation.
- [x] Implement a minimal windowed renderer skeleton (message pump, resize, paint).
- [x] Connect buffer model updates to renderer invalidation (snapshot publication + WM_APP repaint).
- [ ] Render attributes/colors and cursor (monochrome -> styled cells).
- [ ] Inject window keyboard input (win32-input-mode sequences) into the ConDrv input model.
- [ ] Expand tests for non-GUI subcomponents (measurement, layout, model diffs).

## 4) Hardening and Compatibility

- [ ] Expand CLI parity tests to include more edge-case quoting/escaping behavior.
- [ ] Add stress tests for numeric conversion and malformed input handling.
