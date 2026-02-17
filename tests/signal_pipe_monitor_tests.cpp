#include "runtime/signal_pipe_monitor.hpp"

#include "core/unique_handle.hpp"

#include <Windows.h>

#include <cstdio>

namespace
{
    bool test_signal_pipe_monitor_signals_on_broken_pipe()
    {
        HANDLE read_raw = nullptr;
        HANDLE write_raw = nullptr;
        if (::CreatePipe(&read_raw, &write_raw, nullptr, 0) == FALSE)
        {
            return false;
        }

        oc::core::UniqueHandle read_end(read_raw);
        oc::core::UniqueHandle write_end(write_raw);

        oc::core::UniqueHandle stop_event(::CreateEventW(nullptr, TRUE, FALSE, nullptr));
        if (!stop_event.valid())
        {
            return false;
        }

        auto monitor = oc::runtime::SignalPipeMonitor::start(
            read_end.view(),
            stop_event.view(),
            nullptr);
        if (!monitor)
        {
            return false;
        }

        // Closing the write end should eventually surface as ERROR_BROKEN_PIPE on the reader.
        write_end.reset();

        const DWORD wait_result = ::WaitForSingleObject(stop_event.get(), 2'000);
        if (wait_result != WAIT_OBJECT_0)
        {
            return false;
        }

        monitor->stop_and_join();
        return true;
    }
}

bool run_signal_pipe_monitor_tests()
{
    if (!test_signal_pipe_monitor_signals_on_broken_pipe())
    {
        fwprintf(stderr, L"[signal pipe monitor] test_signal_pipe_monitor_signals_on_broken_pipe failed\n");
        return false;
    }

    return true;
}

