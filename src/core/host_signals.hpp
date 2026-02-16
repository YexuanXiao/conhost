#pragma once

// Host signal wire protocol used during console handoff (-Embedding).
//
// When an inbox console host delegates ownership of a console session to another
// host (this project in -Embedding mode), it provides the delegated host with a
// write-only pipe handle ("signal pipe"). The delegated host uses this pipe to
// request that the inbox host performs certain privileged console control
// operations on its behalf.
//
// The upstream OpenConsole uses this to forward calls like EndTask/NotifyApp.
// The pipe format is a one-byte signal code followed by a packed payload.
//
// Important: This is not a public Win32 contract, but the field sizes and
// numeric values must remain stable to interoperate with the inbox host.

#include "core/handle_view.hpp"

#include <Windows.h>

#include <cstddef>
#include <cstdint>
#include <expected>
#include <type_traits>

namespace oc::core
{
    // Values match the private ConsoleControl control types (see upstream
    // conwinuserrefs.h) and are used as the first byte in a pipe packet.
    enum class HostSignals : std::uint8_t
    {
        notify_app = 1u,
        set_foreground = 5u,
        end_task = 7u,
    };

    // Payload structures follow the upstream HostSignals.hpp definitions.
    struct HostSignalNotifyAppData final
    {
        std::uint32_t sizeInBytes{};
        std::uint32_t processId{}; // PID
    };

    struct HostSignalSetForegroundData final
    {
        std::uint32_t sizeInBytes{};
        std::uint32_t processId{}; // HANDLE value, not PID
        bool isForeground{};
    };

    struct HostSignalEndTaskData final
    {
        std::uint32_t sizeInBytes{};
        std::uint32_t processId{}; // PID
        std::uint32_t eventType{};
        std::uint32_t ctrlFlags{};
    };

    static_assert(sizeof(HostSignalNotifyAppData) == 8, "Host signal payload layout must remain stable");
    static_assert(sizeof(HostSignalSetForegroundData) == 12, "Host signal payload layout must remain stable");
    static_assert(sizeof(HostSignalEndTaskData) == 16, "Host signal payload layout must remain stable");

    // CtrlFlags bit values used with HostSignalEndTaskData::ctrlFlags.
    inline constexpr std::uint32_t console_ctrl_c_flag = 0x00000001u;
    inline constexpr std::uint32_t console_ctrl_break_flag = 0x00000002u;
    inline constexpr std::uint32_t console_ctrl_close_flag = 0x00000004u;
    inline constexpr std::uint32_t console_ctrl_logoff_flag = 0x00000010u;
    inline constexpr std::uint32_t console_ctrl_shutdown_flag = 0x00000020u;

    template<typename T>
    [[nodiscard]] inline std::expected<void, DWORD> write_host_signal_packet(
        const HandleView pipe,
        const HostSignals code,
        const T& payload) noexcept
    {
        if (!pipe)
        {
            return std::unexpected(ERROR_INVALID_HANDLE);
        }

#pragma pack(push, 1)
        struct HostSignalPacket final
        {
            HostSignals code;
            T payload;
        };
#pragma pack(pop)

        static_assert(offsetof(HostSignalPacket, code) == 0, "HostSignalPacket must begin with code");
        static_assert(std::is_trivially_copyable_v<HostSignalPacket>, "HostSignalPacket must be trivially copyable");

        const HostSignalPacket packet{ code, payload };

        DWORD written = 0;
        if (::WriteFile(pipe.get(), &packet, static_cast<DWORD>(sizeof(packet)), &written, nullptr) == FALSE)
        {
            return std::unexpected(::GetLastError());
        }

        if (written != sizeof(packet))
        {
            return std::unexpected(ERROR_GEN_FAILURE);
        }

        return {};
    }
}
