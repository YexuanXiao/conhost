# Renderer Text Attributes and Cursor (Design)

## Problem Statement

The window-host skeleton can present viewport text, but classic console applications expect:

- legacy 16-color text attributes (foreground/background + COMMON_LVB_* modifiers)
- a visible cursor whose height is controlled by the console cursor size

These behaviors must be implemented without coupling the UI to the mutable ConDrv object model.

## Scope

Implemented in `renderer::WindowHost` paint path:

- Fill background using the snapshot's `color_table` + per-cell attributes.
- Draw foreground text runs with DirectWrite.
- Render `COMMON_LVB_UNDERSCORE` as an underline rectangle.
- Render the cursor using `cursor_position`, `cursor_visible`, and `cursor_size`:
  - sizes < 100% become an underline-style cursor at the bottom of the cell
  - sizes ≈ 100% become a full-block cursor and redraw the glyph with inverted colors for visibility

Non-goals (still deferred):

- selection, scroll bars, IME/TSF, accessibility
- cursor blink
- truecolor/extended-color rendering (the current screen-buffer model stores legacy `USHORT` attributes only)

## Data Contract

The renderer consumes an immutable `view::ScreenBufferSnapshot` published by the server thread:

- `text` and `attributes` are row-major, sized to `viewport_size.X * viewport_size.Y`
- `default_attributes` and `color_table` provide the palette needed to map legacy indices to RGB

The ConDrv layer remains responsible for building the snapshot (`condrv::make_viewport_snapshot`).

## Attribute Decoding

We decode `USHORT` attributes into:

- `foreground_index = attributes & 0x0F`
- `background_index = (attributes >> 4) & 0x0F`

Then apply modifier flags:

- `COMMON_LVB_REVERSE_VIDEO`: swap foreground/background indices
- `COMMON_LVB_UNDERSCORE`: draw underline using measured underline position/thickness

The decoder lives in `new/src/renderer/console_attributes.hpp` and is unit-tested.

## Rendering Strategy

To balance clarity and performance:

1. Clear the entire render target to the default background color (from `default_attributes`).
2. For each viewport row:
   - compute attribute runs (consecutive cells with equal `USHORT` attributes)
   - for each run:
     - fill the background rectangle if it differs from the cleared background
     - draw the run's substring via `DrawTextW` if the run contains non-space glyphs
     - draw underline rectangle when requested
3. Draw the cursor on top of text/background.

This avoids per-cell `DrawTextW` calls while still supporting per-run attributes.

