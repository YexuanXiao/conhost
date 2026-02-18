# Renderer Window Host Skeleton (Design)

## Problem Statement

The replacement currently implements a ConDrv server and an in-memory screen buffer model, but it does not yet provide a
classic conhost-style window. For non-headless scenarios we need at least:

1. A Win32 window + message pump.
2. A paint path that can present text using modern APIs (Direct2D + DirectWrite).
3. A lifecycle model to stop the server loop when the window closes (and vice versa).

This increment introduces a minimal "window host" skeleton. It intentionally does **not** attempt to replicate
full conhost window behavior (scroll bars, selection, IME, accessibility, etc.). Those are follow-up microtasks.

## Upstream Reference (Local Source Ground Truth)

Relevant entry points and patterns:

- Window creation and class registration:
  - `src/interactivity/win32/window.cpp` (`RegisterClassExW`, `CreateWindowExW`)
- Message dispatch and WM_PAINT handling:
  - `src/interactivity/win32/windowproc.cpp` (`BeginPaint`/`EndPaint` discipline, deferring work to renderer)
- DirectWrite usage for rendering is primarily in the Atlas renderer stack, but the window plumbing is Win32.

The replacement copies only the minimal structural pattern: register a class, create a window, and handle paint/size.

## Goals (This Increment)

1. Provide a minimal Win32 window host (`WindowHost`) with:
   - `RegisterClassExW` + `CreateWindowExW`
   - message pump (`GetMessageW` / `DispatchMessageW`)
   - `WM_SIZE` and `WM_PAINT` handlers
2. Use Direct2D + DirectWrite for painting (no GDI text).
3. Render the active `condrv::ScreenBuffer` viewport contents (legacy 16-color attributes + cursor) via a published snapshot.
4. Integrate the window host into the server-handle startup path:
   - if `--server` is used and we are not in ConPTY/headless mode, run the ConDrv server on a worker thread and the UI
     message pump on the main thread.
   - closing the window stops the server loop via a shared manual-reset stop event.
5. Keep the implementation small and well-contained.

## Non-Goals (This Increment)

- Keyboard/mouse input injection into the ConDrv input model.
- High DPI handling (`WM_DPICHANGED`) beyond "it paints".
- Scroll bars, selection, clipboard, IME/TSF, accessibility, theming.

## Design

### 1) `renderer::WindowHost`

Files:

- `new/src/renderer/window_host.hpp`
- `new/src/renderer/window_host.cpp`

`WindowHost` owns:

- A Win32 window (`HWND`) and the message pump.
- A small Direct2D/DirectWrite device resource bundle:
  - `ID2D1Factory`
  - `ID2D1HwndRenderTarget`
  - `ID2D1SolidColorBrush`
  - `IDWriteFactory`
  - `IDWriteTextFormat`

COM ownership uses C++/WinRT's `winrt::com_ptr<T>` (Windows SDK provided).

Lifecycle:

- `WM_DESTROY` signals the supplied `stop_event` (if any) and posts `WM_QUIT`.
- Paint uses `BeginPaint/EndPaint` for correct invalidation behavior and renders either:
  - the latest published `ScreenBuffer` snapshot (viewport text + attributes + cursor), or
  - a placeholder message if no snapshot is available yet.
- Device-loss (`D2DERR_RECREATE_TARGET`) drops the render target and brush so the next paint recreates them.

Snapshot integration:

- The ConDrv server publishes viewport snapshots to a `view::PublishedScreenBuffer`.
- After publishing, the server thread posts `WM_APP + 1` to the window HWND.
- `WindowHost` handles `WM_APP + 1` by invalidating the window (`InvalidateRect`) to request a repaint.

The snapshot container type lives in `view/` so the renderer does not need to include ConDrv implementation headers.

### 2) Windowed Server Startup (`runtime::Session`)

When started in server-handle mode (`--server`) and *not* `--headless` and *not* `--vtmode`/ConPTY:

1. Create a manual-reset stop event.
2. Create `WindowHost` on the main thread and run its message pump.
3. Start the ConDrv server loop on a worker thread, passing the stop event as the `signal_handle` so the server can be
   stopped without blocking the UI thread.
4. If an external `--signal` handle exists, start a small "signal bridge" thread that waits for either:
   - the external signal handle, or
   - the stop event
   and signals the stop event when the external signal is triggered.

This keeps the server loop and UI responsive without introducing `<thread>`.

### 3) Input/Output Status

This skeleton currently:

- does not inject window keyboard/mouse input into the ConDrv input queue
- renders viewport text with legacy 16-color attributes and a basic cursor (no selection, scroll bars, IME, or accessibility)

The ConDrv server publishes snapshots of its in-memory model and triggers invalidation via `WM_APP + 1`.

## Tests

No automated tests are added for this microtask because it is GUI-only and the project's current test harness is
non-GUI. Non-GUI rendering components (like text measurement) remain unit-tested.

## Limitations / Follow-Ups

1. Implement selection/clipboard/scroll bars.
2. Inject window keyboard input as ConDrv input events.
3. Implement high DPI handling and proper resizing rules.
4. Add IME/TSF and accessibility hooks.
