# ConDrv `ConsolepSetMode`: Validation and Conhost Compatibility Quirks

## Goal
Improve parity with inbox conhost for `ConsolepSetMode` handling (ConDrv USER_DEFINED L1):

- Validate mode bitfields against the supported input/output mode flags.
- Match conhost's observable behavior difference between input and output mode setting:
  - Input mode: applies the mode even if the API returns an error for invalid bits/combinations.
  - Output mode: rejects invalid bits and does not apply them.

This matters for real-world clients that intentionally set "invalid" combinations and rely on conhost's historical behavior (notably PowerShell/PSReadLine patterns called out in the upstream code).

## Upstream Reference (Local Source Tree)
`src/host/getset.cpp`:

- `ApiRoutines::SetConsoleInputModeImpl(...)`
  - assigns the mode first
  - then returns `E_INVALIDARG` if:
    - unknown bits are set (`~(INPUT_MODES | PRIVATE_MODES)`)
    - `ENABLE_ECHO_INPUT` is set while `ENABLE_LINE_INPUT` is clear
  - note in code: compatibility requires "set then error" (PSReadLine example)

- `ApiRoutines::SetConsoleOutputModeImpl(...)`
  - returns `E_INVALIDARG` if unknown bits are set (`~OUTPUT_MODES`)
  - only applies the mode when valid

The relevant flag masks in the upstream file:

- `INPUT_MODES = ENABLE_LINE_INPUT | ENABLE_PROCESSED_INPUT | ENABLE_ECHO_INPUT | ENABLE_WINDOW_INPUT | ENABLE_MOUSE_INPUT | ENABLE_VIRTUAL_TERMINAL_INPUT`
- `PRIVATE_MODES = ENABLE_INSERT_MODE | ENABLE_QUICK_EDIT_MODE | ENABLE_AUTO_POSITION | ENABLE_EXTENDED_FLAGS`
- `OUTPUT_MODES = ENABLE_PROCESSED_OUTPUT | ENABLE_WRAP_AT_EOL_OUTPUT | ENABLE_VIRTUAL_TERMINAL_PROCESSING | DISABLE_NEWLINE_AUTO_RETURN | ENABLE_LVB_GRID_WORLDWIDE`

## Replacement Design
Implementation is in `new/src/condrv/condrv_server.hpp` in the `ConsolepSetMode` handler.

### Input handles (`ObjectKind::input`)
1. Always apply `requested` to `ServerState::input_mode`.
2. Determine whether the request is invalid:
   - any bits outside `(INPUT_MODES | PRIVATE_MODES)` are set
   - `ENABLE_ECHO_INPUT` is set while `ENABLE_LINE_INPUT` is clear
3. If invalid, return `STATUS_INVALID_PARAMETER` but keep the mode applied.
4. If valid, return success.

This matches the upstream "apply then error" quirk.

### Output handles (`ObjectKind::output`)
1. Validate that no bits outside `OUTPUT_MODES` are set.
2. If invalid, return `STATUS_INVALID_PARAMETER` and do not change `ServerState::output_mode`.
3. If valid, apply the mode and return success.

## Tests
`new/tests/condrv_server_dispatch_tests.cpp`:

- `test_user_defined_get_set_mode`
  - sets an intentionally invalid input mode and expects:
    - `ConsolepSetMode` returns invalid parameter
    - `ConsolepGetMode` reads back the invalid mode value (applied anyway)

- `test_user_defined_set_output_mode_validates_flags`
  - sets a valid output mode and verifies it is applied
  - attempts to set output mode with invalid bits and expects:
    - invalid parameter
    - mode remains unchanged

## Limitations / Follow-ups
- The replacement does not implement QuickEdit/Insert behavior side-effects; it only preserves the mode bits and error behavior.
- The replacement does not yet mirror VT-writer side effects from the upstream host when toggling mouse input or VT output mode.

