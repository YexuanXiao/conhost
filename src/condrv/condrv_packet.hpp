#pragma once

// This header defines the on-the-wire packet layout returned by
// `IOCTL_CONDRV_READ_IO` for server-side processing.
//
// ConDrv returns a `CD_IO_DESCRIPTOR` header followed by a small, fixed-size
// payload used to identify the IO category (connect, create object, user IO,
// raw read/write, ...). Variable-sized input and output buffers are accessed
// separately via `IOCTL_CONDRV_READ_INPUT` and `IOCTL_CONDRV_WRITE_OUTPUT`.
//
// The upstream conhost implementation embeds this packet inside a larger
// `CONSOLE_API_MSG` object. The replacement keeps just the stable packet
// payload in a dedicated type.

#include "condrv/condrv_protocol.hpp"

#if defined(__clang__)
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wpragma-pack"
#endif

#include <conmsgl1.h>
#include <conmsgl2.h>
#include <conmsgl3.h>

#if defined(__clang__)
#pragma clang diagnostic pop
#endif

#include <cstddef>
#include <type_traits>

namespace oc::condrv
{
    struct CreateObjectPacket final
    {
        CreateObjectInformation create_object{};
        CONSOLE_CREATESCREENBUFFER_MSG create_screen_buffer{};
    };

    struct UserDefinedPacket final
    {
        CONSOLE_MSG_HEADER msg_header{};
        union
        {
            CONSOLE_MSG_BODY_L1 console_msg_l1;
            CONSOLE_MSG_BODY_L2 console_msg_l2;
            CONSOLE_MSG_BODY_L3 console_msg_l3;
        } u{};
    };

    union IoPacketPayload
    {
        CreateObjectPacket create_object;
        UserDefinedPacket user_defined;

        constexpr IoPacketPayload() noexcept :
            create_object{}
        {
        }
    };

    struct IoPacket final
    {
        IoDescriptor descriptor{};
        IoPacketPayload payload{};
    };

    static_assert(offsetof(IoPacket, descriptor) == 0, "ConDrv packet layout must begin with IoDescriptor");
    static_assert(std::is_standard_layout_v<IoPacket>, "ConDrv packet must remain standard-layout");
    static_assert(std::is_trivially_copyable_v<IoPacket>, "ConDrv packet must remain trivially copyable");
}
