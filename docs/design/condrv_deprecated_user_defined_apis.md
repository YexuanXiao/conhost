# ConDrv Deprecated USER_DEFINED APIs (Design)

## Summary

The inbox console host keeps a set of legacy USER_DEFINED ConDrv APIs for historical UI/VDM integration. In the
OpenConsole source tree these APIs are wired to a common deprecated handler that returns `E_NOTIMPL`.

The replacement does not implement these legacy features. Instead it:

1. Recognizes the deprecated API numbers explicitly.
2. Returns `STATUS_NOT_IMPLEMENTED` with `Information=0`.
3. Zero-fills the returned API-descriptor bytes so replies are deterministic and do not reflect meaningless
   client-provided input fields.

## Upstream Reference (Local Source Tree)

- `src/server/ApiSorter.cpp`
  - The following message types are mapped to `ApiDispatchers::ServerDeprecatedApi` via `CONSOLE_API_DEPRECATED(...)`.
- `src/server/ApiDispatchersInternal.cpp`
  - `ApiDispatchers::ServerDeprecatedApi` returns `E_NOTIMPL`.

## Deprecated APIs Covered

These API numbers exist in the local `dep/Console/conmsgl*.h` headers but are not otherwise required for core console
I/O compatibility:

- Layer 1:
  - `ConsolepMapBitmap`
- Layer 3:
  - `ConsolepSetIcon`
  - `ConsolepInvalidateBitmapRect`
  - `ConsolepVDMOperation`
  - `ConsolepSetCursor`
  - `ConsolepShowCursor`
  - `ConsolepMenuControl`
  - `ConsolepSetPalette`
  - `ConsolepRegisterVDM`
  - `ConsolepGetHardwareState`
  - `ConsolepSetHardwareState`

## Replacement Behavior

Implementation lives in `new/src/condrv/condrv_server.hpp` under the `console_io_user_defined` dispatcher.

For each deprecated API:

1. `packet.payload.user_defined.u` is zero-filled for `ApiDescriptorSize` bytes.
2. `message.set_reply_status(core::status_not_implemented)` is set.
3. `message.set_reply_information(0)` is set.

Additionally, the generic "unhandled USER_DEFINED API" fallback applies the same sanitization before returning
`STATUS_NOT_IMPLEMENTED`. This makes future incremental API expansion safe by default.

### Why Zero-Fill Descriptor Bytes?

The driver expects the API descriptor bytes to be returned even when the operation fails. For deprecated / unsupported
operations, those bytes do not have a meaningful contract. Zero-filling:

- avoids leaking arbitrary input field values back to callers
- makes test results deterministic
- makes behavior stable across future refactors (e.g. if the caller reuses a message buffer)

## Tests

`new/tests/condrv_raw_io_tests.cpp`

- `test_user_defined_deprecated_apis_return_not_implemented_and_zero_descriptor_bytes`
  - exercises all deprecated API numbers above
  - validates `STATUS_NOT_IMPLEMENTED`, `Information==0`
  - validates the returned descriptor bytes (`completion.write`) are all zero
  - validates an unknown API number follows the same sanitized fallback behavior

