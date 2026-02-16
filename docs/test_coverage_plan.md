# Test Coverage Plan and Status

This document tracks non-GUI test coverage of the replacement implementation in `new/`.

## 1. Coverage goals

1. Validate command-line compatibility parsing logic.
2. Validate runtime launch/session paths (including failure behavior).
3. Validate configuration/environment parsing and overrides.
4. Validate logging and serialization primitives.
5. Validate launch policy and server-handle validation behavior.
6. Exercise COM embedding path in deterministic failure and success modes.
7. Exercise ConDrv server-mode dispatch logic and buffer manipulation paths used by classic console clients.

## 2. Implemented test modules

1. `console_arguments_tests.cpp`
- Explicit command payload parsing (`--`)
- Unknown-token command start behavior
- `--` and unknown-token boundaries stop further host-option parsing (flags after the boundary remain client tokens)
- Win32-style escaping reconstruction for tricky tokens (space + trailing backslash) round-trips via `CommandLineToArgvW`
- Compatibility flags (`--server`, `--signal`, `--width`, `--height`, `--headless`, `--vtmode`, `--inheritcursor`, `--textMeasurement`, `--feature pty`)
- `-ForceNoHandoff`
- Invalid feature handling
- Duplicate server-handle handling
- Invalid numeric argument handling

2. `config_tests.cpp`
- Config text parsing
- Environment override precedence
- Invalid line rejection
- Embedding timeout config parsing

3. `logger_tests.cpp`
- Log-level filtering
- File sink creation/write path

4. `key_input_encoder_tests.cpp`
- Printable key encoding
- Arrow key VT encoding
- Ctrl+C mapping
- Key-up suppression
- Alt-prefix behavior
- Backspace mapping

5. `launch_policy_tests.cpp`
- ForceV1 legacy selection
- ConPTY disables legacy
- ForceV2 registry semantics

6. `server_handle_validator_tests.cpp`
- Invalid handle rejection
- Optional null signal acceptance
- Valid file handle acceptance

7. `startup_command_tests.cpp`
- Default command non-empty
- `cmd.exe` inclusion
- `WINDIR`-based command construction

8. `fast_number_tests.cpp`
- Signed/unsigned parse
- Hex parse with prefix enforcement
- Float parse success/failure
- Integer and float formatting round-trip checks
- Deterministic stress coverage for edge ranges (i32/u32/hex boundaries + round-trip loops)

9. `session_tests.cpp`
- Inherited-stdio runtime path exit-code behavior
- Pseudo-console runtime path exit-code behavior
- Empty command + signaled event behavior
- Server-handle validation failure path

10. `com_embedding_server_tests.cpp`
- COM embedding timeout/failure path behavior
- COM embedding successful activation + `EstablishHandoff` path behavior

11. `host_signals_tests.cpp`
- Host signal pipe packet wire encoding (`HostSignals::end_task`)

12. `condrv_protocol_tests.cpp`
- ConDrv protocol POD/layout sanity checks
- ConDrv device-comm wrapper invalid-handle behavior

13. `condrv_api_message_tests.cpp`
- ConDrv API message input buffer acquisition + caching
- ConDrv API message output buffer release/write behavior

14. `condrv_server_dispatch_tests.cpp`
- CONNECT/DISCONNECT lifecycle
- CREATE_OBJECT/CLOSE_OBJECT (current input/output handles + new output buffers)
- USER_DEFINED mode/codepage APIs (Get/SetMode validation semantics for input/output; GetCP/SetCP)
- USER_DEFINED screen buffer state APIs (Get/SetCursorInfo, Get/SetCursorPosition with viewport snapping, Get/SetScreenBufferInfo including `ScrollPosition` + delta `CurrentWindowSize`, SetWindowInfo (absolute+relative), GetLargestWindowSize)
- USER_DEFINED multi-screen-buffer behavior (per-buffer content isolation, `ConsolepSetActiveScreenBuffer` affects `io_object_type_current_output`)
- USER_DEFINED L3 query APIs (GetConsoleWindow, GetDisplayMode, GetKeyboardLayoutName, GetMouseInfo, GetSelectionInfo, GetConsoleProcessList)
- USER_DEFINED L3 font/display APIs (GetNumberOfFonts/GetFontInfo/GetFontSize, Get/SetCurrentFont, SetDisplayMode)
- USER_DEFINED L3 legacy compatibility stubs (SetKeyShortcuts, SetMenuClose, CharType, CursorMode, NlsMode, OS2 toggles, LocalEUDC)
- USER_DEFINED L3 history APIs (GetHistory/SetHistory + command history APIs: Expunge/SetNumber/GetLength/GetHistory)
- USER_DEFINED `ConsolepGenerateCtrlEvent` best-effort forwarding via host IO (`send_end_task`)

15. `condrv_input_wait_tests.cpp`
- Reply-pending + retry behavior for input-dependent reads (non-blocking dispatch):
  - `ConsolepReadConsoleW` reply-pends on empty input and completes on retry after injection.
  - Split UTF-8 sequences drain into per-handle prefix storage and reply-pend until complete (no busy-spin on undecodable bytes).
  - `ConsolepGetConsoleInput` removing reads reply-pend on split UTF-8 sequences and complete on retry.
  - VT input decoding in ConPTY scenarios:
    - win32-input-mode (`CSI ... _`) decodes to `KEY_EVENT` records.
    - DA1 and focus in/out sequences are consumed (ignored) and do not leak to clients.
    - fallback VT cursor-key sequences (for example `ESC[A`) are treated as non-character keys and do not produce `ReadConsoleW` output (the read reply-pends after consuming them).
    - split win32-input-mode sequences reply-pend and drain into the per-handle prefix buffer until complete.
    - raw `ReadConsoleA` encodes win32-input-mode character key events via `WideCharToMultiByte` and does not leak escape bytes.
  - A regression check that a reply-pending read does not prevent dispatching unrelated requests.
  - Disconnect behavior: pending reads complete with failure when input is marked disconnected.

16. `condrv_raw_io_tests.cpp`
- RAW_READ/RAW_WRITE forwarding and invalid-handle behavior
- RAW_WRITE updates the in-memory screen buffer model (validated via ReadConsoleOutputString)
- RAW_READ `ProcessControlZ` behavior (CTRL+Z returns 0 bytes but only consumes the marker)
- RAW_READ processed-input Ctrl+C behavior (filters `0x03` bytes, forwards `CTRL_C_EVENT` without terminating the read, and continues reading to fill output when possible)
- RAW_READ processed-input Ctrl+Break behavior (when observed as a win32-input-mode key event: flushes input, forwards `CTRL_BREAK_EVENT`, and terminates with `STATUS_ALERTED`)
- RAW_READ ConPTY VT input consumption:
  - win32-input-mode (`CSI ... _`) character key events decode and are returned as bytes (no escape-byte leakage)
  - DA1 and focus in/out sequences are consumed (ignored)
  - split win32-input-mode sequences reply-pend and drain into the per-handle prefix buffer until complete
  - processed-input Ctrl+C recognizes win32-input-mode Ctrl+C key events and forwards `CTRL_C_EVENT`
- RAW_FLUSH clears the host input queue for input handles
- USER_DEFINED WriteConsole/ReadConsole buffer offset behavior
- USER_DEFINED WriteConsole updates the in-memory screen buffer model (validated via ReadConsoleOutputString)
- USER_DEFINED WriteConsole respects `DISABLE_NEWLINE_AUTO_RETURN` (LF-only vs CRLF translation in the buffer model)
- USER_DEFINED WriteConsole in `ENABLE_VIRTUAL_TERMINAL_PROCESSING` mode consumes CSI/ESC sequences and applies:
  - C1 CSI (`U+009B`) is interpreted like 7-bit `ESC [` when writing UTF-16 output
  - SGR (attributes, including `COMMON_LVB_REVERSE_VIDEO` and `COMMON_LVB_UNDERSCORE`, with correct bright/normal intensity transitions, plus `38/48` extended colors approximated to the nearest legacy palette entry), CUP (cursor positioning), and ED/EL (clearing)
  - ESC dispatch sequences are consumed (NEL `ESC E`, DECALN `ESC # 8`, and charset designation no-ops) so escape bytes do not leak to the buffer model
  - split VT sequences across separate `WriteConsole` calls (CSI/OSC/ESC dispatch/string ST) are consumed via a streaming parser state machine (no escape-byte leakage)
  - cursor movement additions: CHA/HPA (`G`/`` ` ``), VPA (`d`), CNL/CPL (`E`/`F`), and DECSTBM-aware CUU/CUD clamping
  - DECSTBM scroll margins (CSI `r`) and vertical scrolling/line ops (CSI `S/T`, CSI `L/M`, `ESC D`/`ESC M`)
  - horizontal editing within a line (CSI `@`/`P`/`X`)
  - insert/replace mode (IRM, CSI `4h`/`4l`) for printable output
  - DECSTR/RIS reset sequences (DECSTR `CSI ! p`, RIS `ESC c`)
- USER_DEFINED WriteConsole in VT mode consumes cursor save/restore sequences (CSI `s/u` and DECSC/DECRC `ESC7/ESC8`) and restores both cursor position and attributes
- USER_DEFINED WriteConsole in VT mode consumes DECTCEM cursor visibility sequences (CSI `?25h`/`?25l`) and updates `ConsolepGetCursorInfo` visibility state
- USER_DEFINED WriteConsole in VT mode consumes DECOM origin mode toggles (CSI `?6h`/`?6l`) and applies origin-mode-relative cursor addressing/clamping within DECSTBM margins
- USER_DEFINED WriteConsole in VT mode consumes DECAWM autowrap mode toggles (CSI `?7h`/`?7l`) and implements delayed-wrap behavior at the final column
- USER_DEFINED WriteConsole in VT mode consumes alternate screen buffer mode toggles (DECSET/DECRST `CSI ?1049h/l`) and restores main contents/state on exit
- USER_DEFINED WriteConsole in VT mode consumes OSC window-title sequences (OSC `0/1/2/21`) and updates `ConsolepGetTitle` state without rendering escape bytes
- USER_DEFINED WriteConsole in VT mode consumes DSR requests (CSI `5n`/`6n`) and injects the expected replies into the input queue when configured to answer queries
- USER_DEFINED ReadConsoleW decodes UTF-8/code-page bytes into UTF-16 (and consumes correct byte counts)
- USER_DEFINED ReadConsoleW + GetConsoleInput split surrogate pairs across calls when the caller buffer can hold only one UTF-16 unit/record (pending decoded unit)
- USER_DEFINED ReadConsoleW/A implements cooked line input when `ENABLE_LINE_INPUT` is set (CR/LF termination, CRLF suffix when processed, backspace edits, echo, pending output for small caller buffers, and VK-based line editing keys; ANSI reads return `STATUS_BUFFER_TOO_SMALL` when the buffer cannot hold one encoded character)
- USER_DEFINED ReadConsole processed-input Ctrl+C behavior (cooked reads return `STATUS_ALERTED` and forward `CTRL_C_EVENT`; raw ReadConsole filters `0x03` and continues)
- USER_DEFINED GetConsoleInput (peek/remove) + GetNumberOfInputEvents behavior (byte-queue-backed)
- USER_DEFINED GetConsoleInput processed-input Ctrl+C filtering (skips `0x03` and continues decoding to fill the caller's record buffer)
- USER_DEFINED GetConsoleInput + GetNumberOfInputEvents decode/count UTF-8 input as UTF-16 code units
- USER_DEFINED WriteConsoleInput injection behavior (Append vs replace)
- USER_DEFINED FillConsoleOutput + Read/WriteConsoleOutputString round-trips
- USER_DEFINED Read/WriteConsoleOutput (`CHAR_INFO`) round-trips
- USER_DEFINED ScrollConsoleScreenBuffer behavior (basic shift + fill)
- USER_DEFINED GetTitle/SetTitle round-trips (Unicode + ANSI)
- USER_DEFINED L3 aliases (AddAlias/GetAlias, GetAliasesLength/GetAliases, GetAliasExesLength/GetAliasExes)
- USER_DEFINED deprecated legacy APIs return `STATUS_NOT_IMPLEMENTED` with sanitized (zero-filled) descriptor bytes (MapBitmap + legacy L3 UI/VDM/hardware-state APIs, plus unknown API fallback)

17. `dwrite_text_measurer_tests.cpp`
- DirectWrite-backed font/cell measurement:
  - returns non-zero cell metrics for `Consolas`
  - falls back to `Consolas` when the requested family name is missing
  - DPI scaling is approximately linear (within rounding tolerance)
  - point-size scaling is approximately linear (within rounding tolerance)

18. `condrv_screen_buffer_snapshot_tests.cpp`
- Viewport snapshot extraction:
  - reads a cropped viewport sub-rect row-by-row (no cross-row leakage)
  - captures viewport attributes and color table
  - revision counter increments on visible mutations

19. `process_integration_tests.cpp`
- Process-isolated runtime validation for the `openconsole_new.exe` executable:
  - `--headless --vtmode` ConPTY path emits output and propagates child exit code.
  - piped stdin reaches the hosted client (smoke coverage for the input pump + ConPTY attach semantics).
  - `--server --headless` ConDrv server-mode path hosts a real console client using ConDrv handles and forwards basic input/output end-to-end.
  - `--server --headless` ConDrv server-mode path decodes win32-input-mode sequences into `ReadConsoleInputW` key events (virtual-key metadata + Unicode payload).

20. `com_embedding_integration_tests.cpp`
- Out-of-proc COM `-Embedding` end-to-end harness:
  - activates the COM local server and calls `IConsoleHandoff::EstablishHandoff`.
  - validates that the COM handshake does not crash and the server remains responsive (beyond unit-only activation coverage).

21. `condrv_vt_fuzz_tests.cpp`
- Deterministic fuzz/bounds hardening for VT parsing:
  - fuzzes `vt_input::try_decode_vt` with biased random prefixes and asserts token invariants (`bytes_consumed`, no `text_units`, and `need_more_data` only on ESC/C1 CSI).
  - fuzzes the streaming VT output parser (`apply_text_to_screen_buffer`) across randomized chunk boundaries and asserts `ScreenBuffer` invariants (cursor/window bounds, full buffer readback, monotonic revision).
  - adds targeted bounds tests for overlong CSI/ESC-dispatch abandonment and OSC title payload truncation.

## 3. Execution

Run:
```powershell
cmake --build build-new --target oc_new_tests
ctest --test-dir build-new --output-on-failure
```

## 4. Remaining test expansion opportunities

1. Expand process-isolated integration coverage for the full executable startup matrix (`-Embedding`, `--server`, `--headless`, legacy policy combinations).
2. Add stress tests for malformed Unicode input.
3. Add high-volume runtime I/O pump tests with synthetic pipe traffic.
