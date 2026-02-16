# ConDrv `ReadConsole`: Cooked Line Editing (VK-Based)

## Goal
When `ENABLE_LINE_INPUT` is set, the inbox conhost performs a cooked read and supports command-line editing
keys (arrows, Home/End, Insert/Delete, Escape, and Ctrl-modified word movement).

The replacement previously implemented only minimal cooked reads (buffer until Enter + backspace-at-end),
which left common line-editing keys ineffective. This microtask adds a compact VK-based editor that works
with the existing byte-stream-backed input queue and the VT-aware input token decoder.

## Upstream Reference (Local Source Tree)
- `src/host/stream.cpp`
  - `IsCommandLineEditingKey(const KEY_EVENT_RECORD&)`
  - `GetChar(...)` (routes editing keys to cooked reads as VKs)
- `src/host/readDataCooked.cpp`
  - `COOKED_READ_DATA::_handleVkey(...)` (editing semantics for VKs)

## Replacement Design
### Trigger
Inside `ConsolepReadConsole` in `new/src/condrv/condrv_server.hpp`:
- if `state.input_mode()` contains `ENABLE_LINE_INPUT`, the request uses the cooked editor path
- otherwise the raw read path remains unchanged

### Persisted Per-Handle State
Editing needs resumable state across reply-pending retries, so state is stored on the input `ObjectHandle`:
- `ObjectHandle::cooked_line_in_progress` (UTF-16 code units)
- `ObjectHandle::cooked_line_cursor` (UTF-16 code-unit index; normalized to never split a surrogate pair)
- `ObjectHandle::cooked_insert_mode` (insert vs overwrite)

These are reset on input flush/replace operations (`ConsolepFlushInputBuffer`, `ConsolepWriteConsoleInput` replace,
and `RAW_FLUSH`).

### Cursor Model
The cursor is a UTF-16 code-unit index into `cooked_line_in_progress`, but we maintain the invariant:
- the cursor never points between a high surrogate and its trailing low surrogate

Movement uses surrogate-aware stepping:
- `prev_index(i)` steps left by 1, or by 2 when crossing a surrogate pair boundary
- `next_index(i)` steps right by 1, or by 2 when crossing a surrogate pair boundary

This is intentionally simpler than the upstream grapheme-based model.

### VT-Aware Input Tokens
The cooked editor consumes input via `decode_one_input_token(...)`:
- `text_units` are treated as typed characters
- `key_event` records are used for VK-based editing
- `ignored_sequence` tokens (DA1 responses, focus in/out) are consumed and discarded

### Supported Editing Keys
These are handled for cooked reads:
- `VK_LEFT` / `VK_RIGHT`: move by one code point
- `VK_HOME` / `VK_END`: move to start/end
- `VK_INSERT`: toggle insert/overwrite
- `VK_DELETE`: delete at cursor
- `VK_ESCAPE`: clear line

Ctrl-modified behavior:
- `Ctrl+HOME`: delete to start of line
- `Ctrl+END`: delete to end of line
- `Ctrl+LEFT` / `Ctrl+RIGHT`: word navigation using a simple delimiter heuristic:
  - delimiters are `' '` and `'\t'` only

Out of scope (deferred):
- command history navigation (`VK_UP`/`VK_DOWN`)
- popup keys, function keys, command list UI
- IME/composition and rich input events

### Echo Strategy (No VT Cursor Moves)
When `ENABLE_ECHO_INPUT` is set, editing updates are echoed using only printed characters plus backspace/space
rewrites. The intent is to mimic classic conhost behavior without requiring VT cursor movement sequences.

The replacement uses the classic "rewrite tail + backspace" algorithm:
- insert/overwrite: print inserted units + tail, then backspace over the tail to restore the logical cursor
- delete/backspace: move left as needed, print tail, print spaces to clear leftovers, then backspace to restore cursor
- cursor movement: left uses backspaces; right prints the traversed range

Echo output updates both:
- the in-memory `ScreenBuffer` model (`apply_text_to_screen_buffer`)
- the host output byte sink (UTF-8 via `WideCharToMultiByte(CP_UTF8, ...)`)

### Enter Finalization With Cursor Mid-Line
When Enter is received (`'\r'` or `'\n'`) and the cursor is not at end-of-line:
- echo the remaining tail (so the display cursor reaches end-of-line)
- finalize the line exactly like the minimal cooked implementation (`CRLF` when processed, otherwise `CR`)

## Tests
Non-GUI deterministic coverage was added in `new/tests/condrv_raw_io_tests.cpp`:
- insert in middle with `VK_LEFT`
- overwrite mode toggle (`VK_INSERT`)
- delete in middle (`VK_DELETE`)
- Enter with cursor mid-line (tail echo)
- `VK_ESCAPE` clear line (via win32-input-mode sequence)
- Ctrl+HOME / Ctrl+END delete-to-start/delete-to-end (via win32-input-mode sequences carrying ctrl state)

## Limitations / Follow-Ups
- No upstream history/edit popups, command list, or macro processing.
- Word navigation is space/tab based (not the upstream delimiter-class logic).
- Editing operates on UTF-16 code points (surrogate-pair aware) rather than full grapheme clusters.
