#pragma once

// ConDrv -> view snapshot builder.
//
// The ConDrv server mutates an in-memory `condrv::ScreenBuffer` model. The UI must not read that
// mutable state directly across threads. Instead, the server thread periodically builds an immutable
// `view::ScreenBufferSnapshot` and publishes it to the renderer.
//
// The snapshot types live in `view/` to avoid coupling the renderer to the ConDrv implementation.

#include "condrv/condrv_device_comm.hpp"
#include "view/screen_buffer_snapshot.hpp"

#include <expected>
#include <memory>

namespace oc::condrv
{
    class ScreenBuffer;

    [[nodiscard]] std::expected<std::shared_ptr<const view::ScreenBufferSnapshot>, DeviceCommError> make_viewport_snapshot(
        const ScreenBuffer& buffer) noexcept;
}

