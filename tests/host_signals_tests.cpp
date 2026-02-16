#include "core/host_signals.hpp"

#include "core/unique_handle.hpp"

#include <Windows.h>

#include <array>
#include <cstddef>
#include <cstdio>
#include <cstring>

namespace
{
    bool test_end_task_packet_write_layout()
    {
        HANDLE read_raw = nullptr;
        HANDLE write_raw = nullptr;
        if (::CreatePipe(&read_raw, &write_raw, nullptr, 0) == FALSE)
        {
            return false;
        }

        oc::core::UniqueHandle read_end(read_raw);
        oc::core::UniqueHandle write_end(write_raw);

        oc::core::HostSignalEndTaskData payload{};
        payload.sizeInBytes = sizeof(payload);
        payload.processId = 4242;
        payload.eventType = CTRL_C_EVENT;
        payload.ctrlFlags = oc::core::console_ctrl_c_flag;

        auto write_result = oc::core::write_host_signal_packet(
            write_end.view(),
            oc::core::HostSignals::end_task,
            payload);
        if (!write_result)
        {
            return false;
        }

        // Packet is: 1 byte signal code + payload.
        std::array<std::byte, 1 + sizeof(payload)> buffer{};
        size_t total_read = 0;
        while (total_read < buffer.size())
        {
            DWORD read = 0;
            if (::ReadFile(
                    read_end.get(),
                    buffer.data() + total_read,
                    static_cast<DWORD>(buffer.size() - total_read),
                    &read,
                    nullptr) == FALSE)
            {
                return false;
            }

            if (read == 0)
            {
                return false;
            }

            total_read += static_cast<size_t>(read);
        }

        if (buffer[0] != static_cast<std::byte>(oc::core::HostSignals::end_task))
        {
            return false;
        }

        oc::core::HostSignalEndTaskData decoded{};
        std::memcpy(&decoded, buffer.data() + 1, sizeof(decoded));
        if (decoded.sizeInBytes != sizeof(decoded))
        {
            return false;
        }
        if (decoded.processId != payload.processId)
        {
            return false;
        }
        if (decoded.eventType != payload.eventType)
        {
            return false;
        }
        if (decoded.ctrlFlags != payload.ctrlFlags)
        {
            return false;
        }

        return true;
    }
}

bool run_host_signals_tests()
{
    if (!test_end_task_packet_write_layout())
    {
        fwprintf(stderr, L"[host signals] test_end_task_packet_write_layout failed\n");
        return false;
    }

    return true;
}

