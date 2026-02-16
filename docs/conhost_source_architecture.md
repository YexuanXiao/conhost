# Conhost Source Architecture Analysis (Local Repository)

This document summarizes the architecture of the conhost/OpenConsole source code in the current local folder (`src/`), based on direct code and build graph analysis.

## 1. Top-level subsystem layout

Main source roots:
- `src/host`: process startup, command-line parsing, session initialization, VT I/O integration, global orchestration.
- `src/server`: console driver API/I/O dispatch, object model, process/wait management, entrypoints.
- `src/buffer`: text buffer, console state, output mutation logic.
- `src/renderer`: rendering stack (`base`, `gdi`, `uia`, `wddmcon`, `atlas`).
- `src/terminal`: parser/adapter/input components (`parser`, `adapter`, `input`).
- `src/interactivity`: platform abstraction and host-interaction layers.
- `src/types`: shared primitive/domain types (attributes, viewport, width detection, conversion).
- `src/tsf`: text service framework integration.
- `src/audio`: terminal/console audio integration.
- `src/inc`, `src/internal`, `src/propslib`: shared headers, internal glue, properties/config support.

## 2. Build-time module graph (from root `CMakeLists.txt`)

Major targets:
- Core orchestration: `ConhostV2Lib`, `ConServer`
- Terminal stack: `ConTermParser`, `ConTermAdapt`, `TerminalInput`
- Rendering: `ConRenderBase`, `ConRenderGdi`, `ConRenderUia`, `ConRenderWddmCon`, `ConRenderAtlas`
- Type/util libs: `ConTypes`, `ConProps`, `ConInt`, `ConTSF`
- Final exe: `OpenConsole`
- Proxy/COM support: `OpenConsoleProxy`

Observed dependency shape:
- `OpenConsole` links orchestration, server, parser/adapter, renderer, and type/interactivity modules.
- `ConhostV2Lib` and `ConServer` form startup + server-lifetime backbone.
- Parser/adapter and renderer modules are independently testable/replaceable layers.

## 3. Startup architecture (host executable path)

Primary flow in `src/host/exe/exemain.cpp`:
1. Register tracing and initialize service/global state.
2. Parse command line via `ConsoleArguments`.
3. Choose mode:
   - COM server mode (`-Embedding`) for handoff registration.
   - Legacy v1 path (`ConhostV1.dll`) based on `-ForceV1`, `InConptyMode`, `HKCU\Console\ForceV2`.
   - V2 startup via server entrypoints.
4. Start session:
   - `StartConsoleForCmdLine(...)` for create-server path.
   - `StartConsoleForServerHandle(...)` for provided server-handle path.
5. On success, lower shutdown ordering pressure (`SetProcessShutdownParameters(0, 0)`), then hand lifetime to server threads.

## 4. Server runtime architecture

From `src/server` + `src/host/srvinit.cpp`:
- Driver communication object created/attached.
- Console API I/O thread is created.
- Optional handoff bootstrap path supports connect-message transfer.
- Input-available event is wired to the driver.
- VT I/O initialization and signal thread are started when in conpty mode.
- Main loop:
  - read driver message
  - dispatch via IoSorter/API routines
  - complete/release message buffers
  - terminate when disconnected.

## 5. VT/conpty architecture

From `src/host/VtIo.cpp`:
- Conpty mode initializes with provided input/output/signal handles.
- Startup negotiation sends:
  - cursor position query if inherit-cursor requested
  - DA1 report request
  - focus event mode
  - win32 input mode request
- Signal thread handles pty-side control events and close behavior.
- Writer subsystem handles UTF-16 -> UTF-8, batching/corking, control sanitization, and overlapped/broken-pipe behavior.

## 6. Rendering and terminal responsibilities

- Terminal parser/adapter convert VT sequences and input dispatch to internal operations.
- Buffer/types maintain canonical console model (cells, attributes, viewport, widths).
- Renderer stack emits visual output through selected backend (GDI/Atlas/UIA/etc.).
- Host/server orchestration mediates process/session lifecycle around these core components.

## 7. Architectural properties to preserve in replacement

The replacement should preserve:
- startup mode selection behavior (legacy/v2/conpty/com mode),
- CLI semantics (`ConsoleArguments`-compatible),
- robust resource ownership transfer during startup/handoff,
- separation of concerns:
  - startup/policy
  - session runtime
  - terminal I/O
  - rendering
  - server/process lifecycle.

