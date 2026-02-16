# Renderer ScreenBuffer Snapshot (Design)

## Problem Statement

The ConDrv server maintains an in-memory `condrv::ScreenBuffer` model that is mutated by the server IO thread as
ConDrv API packets are dispatched. A classic (non-headless) console host needs to render that model on a UI thread.

Sharing the mutable `ScreenBuffer` directly across threads would complicate invariants and make exception safety and
resource cleanup brittle. Instead, the renderer should consume an immutable snapshot of the *visible viewport*.

## Goals (This Increment)

1. Publish an immutable snapshot of the active `ScreenBuffer` viewport from the ConDrv server thread.
2. Expose the latest snapshot to the UI thread without locks in the UI paint path.
3. Trigger repaints from the server thread without calling into UI code directly.

## Non-Goals (This Increment)

- Dirty-rect tracking or partial redraw (full viewport snapshots only).
- Rendering of attributes/colors, cursor, selection, scroll bars, IME, accessibility.
- A full render backend architecture (this is the minimal bridge).

## Design

### 1) Snapshot Type

`condrv::ScreenBufferSnapshot` contains:

- revision number (monotonic best-effort)
- buffer + viewport geometry (`buffer_size`, `window_rect`, derived `viewport_size`)
- cursor state + default attributes + color table
- `text` and `attributes` arrays for the viewport (row-major)

Only the viewport is snapshotted because:

- the UI can only present the viewport
- snapshot size stays bounded by window size
- per-row extraction avoids accidentally reading across row boundaries

### 2) Publication Container

`condrv::PublishedScreenBuffer` stores:

- `std::atomic<std::shared_ptr<const ScreenBufferSnapshot>> _latest`

The server thread publishes a newly built snapshot via `store(release)`. The UI thread reads the latest snapshot via
`load(acquire)`. Snapshots are immutable after publication.

### 3) Change Detection via Revision

`condrv::ScreenBuffer` maintains a `revision()` counter that increments on every mutation of:

- viewport state (window rect/size, scroll snapping)
- cursor state
- attributes / color table
- cell contents

The ConDrv server loop publishes only when:

- the active buffer pointer changed, or
- the buffer revision changed since the last publish

### 4) UI Invalidation Strategy

The server thread never calls into the window code directly. Instead it posts a message:

- `PostMessageW(hwnd, WM_APP + 1, 0, 0)`

`renderer::WindowHost` handles `WM_APP + 1` by calling:

- `InvalidateRect(hwnd, nullptr, FALSE)`

This keeps the cross-thread contract narrow and avoids UI-thread reentrancy hazards.

## Limitations / Follow-Ups

1. Render attributes/colors (foreground/background) and cursor.
2. Add a stable "dirty region" model to avoid full snapshots/redraws on every write.
3. Integrate keyboard/mouse input injection into the ConDrv input model.

