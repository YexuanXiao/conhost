# ConDrv Viewport/ScrollPosition Model (Design)

## Problem Statement

The initial `new/` ConDrv server implementation tracked only a `window_size` per `ScreenBuffer` and implicitly assumed
the viewport origin was always `(0, 0)`. As a result:

- `ConsolepGetScreenBufferInfo` always returned `ScrollPosition = (0, 0)`.
- `ConsolepSetWindowInfo` updated only the window *size* and discarded the window *origin* entirely.
- `ConsolepSetCursorPosition` moved the cursor but did not "snap" the viewport to keep the cursor visible.
- `CONSOLE_SCREENBUFFERINFO_MSG::CurrentWindowSize` was treated as `(width, height)` even though the inbox conhost uses
  an *inclusive delta* encoding (`Right-Left`, `Bottom-Top`), which is visible to clients via the ConDrv protocol.

This diverged from the inbox conhost behavior and could cause classic clients to observe incorrect viewport state
and/or compute the wrong `srWindow` rectangle on the client side.

## Reference Behavior in the Local Conhost Source

This module follows the behavior of the inbox conhost implementation:

- `src/server/ApiDispatchers.cpp`:
  - `ServerGetConsoleScreenBufferInfo` populates:
    - `ScrollPosition = srWindow.Left/Top`
    - `CurrentWindowSize = (srWindow.Right - srWindow.Left, srWindow.Bottom - srWindow.Top)`
      (note: **no** `+ 1`)
  - `ServerSetConsoleScreenBufferInfo` reconstructs:
    - `srWindow.Right = srWindow.Left + CurrentWindowSize.X`
    - `srWindow.Bottom = srWindow.Top + CurrentWindowSize.Y`
- `src/host/getset.cpp`:
  - `ApiRoutines::SetConsoleWindowInfoImpl` applies relative window moves by adding the delta rect to the current
    viewport edges (`left += current.left`, `right += current.right`, etc.).
- `src/host/getset.cpp`:
  - `ApiRoutines::SetConsoleCursorPositionImpl` attempts to "snap" the viewport to keep the cursor visible by adjusting
    the viewport origin when the cursor is outside the current viewport.

## Goals

1. Track viewport origin + size per `ScreenBuffer` with a correct ConDrv wire representation.
2. Implement `ConsolepSetWindowInfo` (absolute and relative) so that viewport origin changes are preserved.
3. Return `ScrollPosition` and `CurrentWindowSize` matching inbox conhost semantics.
4. Snap the viewport to the cursor position after `ConsolepSetCursorPosition` and after `WriteConsole` cursor advances
   (headless auto-follow behavior).
5. Keep the implementation deterministic and exception-safe (`dispatch_message` is `noexcept`).

## Non-Goals (For This Increment)

- Renderer integration (viewport changes do not invalidate a window yet).
- User-controlled scrollback / "user has scrolled up" state. The current model always auto-follows the cursor.
- Full conhost "virtual viewport" semantics and line rendition conversions.

## Data Model

`ScreenBuffer` now stores the viewport as an inclusive rectangle:

- `SMALL_RECT _window_rect` (in screen-buffer coordinates, inclusive edges)

Derived values:

- `scroll_position = (Left, Top)`
- `window_size = (Right-Left+1, Bottom-Top+1)` for internal computations
- ConDrv `CurrentWindowSize = (Right-Left, Bottom-Top)` (inclusive delta encoding)

Invariants enforced:

1. `_window_rect` is always within the buffer:
   - `0 <= Left <= Right < buffer_size.X`
   - `0 <= Top <= Bottom < buffer_size.Y`
2. The cursor position remains within the buffer.

## API / Behavior Changes in `new/`

### 1. ScreenBuffer surface

`ScreenBuffer` exposes:

- `window_rect()` / `set_window_rect(...)` for inclusive-rect state.
- `scroll_position()` for ConDrv `ScrollPosition`.
- `snap_window_to_cursor()` for auto-follow behavior (clamps origin so the cursor is inside the viewport).
- `set_window_size(...)` returns `bool` (validation-first instead of silent clamping).

### 2. ConDrv dispatch behavior

- `ConsolepGetScreenBufferInfo`:
  - `ScrollPosition` returns the viewport origin.
  - `CurrentWindowSize` returns `(Right-Left, Bottom-Top)`.
- `ConsolepSetWindowInfo`:
  - Absolute mode sets the viewport to the provided rect.
  - Relative mode applies the delta rect to the current viewport edges.
- `ConsolepSetCursorPosition`:
  - After moving the cursor, snaps the viewport to keep the cursor visible.
- `ConsolepWriteConsole`:
  - After updating the cursor, snaps the viewport to follow the cursor.

## Tests

Added/updated unit tests in `new/tests/condrv_server_dispatch_tests.cpp`:

- `test_user_defined_window_info_updates_scroll_position`
  - Verifies absolute `SetWindowInfo` updates `ScrollPosition` and `CurrentWindowSize` (delta form).
- `test_user_defined_cursor_position_snaps_viewport`
  - Verifies `SetCursorPosition` snaps `ScrollPosition` to keep the cursor visible when the window is smaller than
    the buffer.
- Updated `test_user_defined_set_screen_buffer_info_round_trips`
  - Uses the delta encoding for `CurrentWindowSize` and validates `ScrollPosition` round-trips.

## Follow-ups

1. Implement a user-scroll state and avoid auto-follow when the user is scrolled away from the cursor.
2. Expand output-driven viewport behavior (`WriteConsole` newline scroll regions, buffer height > window height) to more
   closely match conhost.
3. Add renderer invalidation hooks once a windowed renderer exists.

