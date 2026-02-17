#pragma once

// A small helper that monitors a read-end of a pipe and signals an event when the
// pipe is disconnected.
//
// Why this exists:
// - In ConPTY scenarios, conhost/openconsole uses a "signal" pipe whose lifetime is
//   tied to the terminal/hosting side. When that side closes (or dies), the pipe
//   breaks and the console host should promptly begin shutdown.
// - Win32 wait APIs cannot wait on generic pipe handles, so we must drain/read to
//   observe disconnection.
//
// This class is intentionally minimal: it does not parse ConPTY signal payloads.
// It only drains bytes and turns broken-pipe into an event signal.
//
// See also:
// - `new/docs/conhost_behavior_imitation_matrix.md` (startup modes)
// - `new/tests/signal_pipe_monitor_tests.cpp`

#include "core/handle_view.hpp"
#include "core/unique_handle.hpp"

#include <Windows.h>

#include <expected>
#include <memory>
#include <string>

namespace oc::logging
{
    class Logger;
}

namespace oc::runtime
{
    struct SignalPipeMonitorError final
    {
        std::wstring context;
        DWORD win32_error{ ERROR_GEN_FAILURE };
    };

    class SignalPipeMonitor final
    {
    public:
        SignalPipeMonitor() noexcept = default;
        ~SignalPipeMonitor() noexcept
        {
            stop_and_join();
        }

        SignalPipeMonitor(const SignalPipeMonitor&) = delete;
        SignalPipeMonitor& operator=(const SignalPipeMonitor&) = delete;

        SignalPipeMonitor(SignalPipeMonitor&& other) noexcept :
            _thread(std::move(other._thread)),
            _pipe(std::move(other._pipe)),
            _context(std::move(other._context))
        {
            other._context = nullptr;
        }

        SignalPipeMonitor& operator=(SignalPipeMonitor&& other) noexcept
        {
            if (this != &other)
            {
                stop_and_join();
                _thread = std::move(other._thread);
                _pipe = std::move(other._pipe);
                _context = std::move(other._context);
                other._context = nullptr;
            }
            return *this;
        }

        [[nodiscard]] static std::expected<SignalPipeMonitor, SignalPipeMonitorError> start(
            core::HandleView signal_pipe,
            core::HandleView stop_event,
            logging::Logger* logger) noexcept;

        void stop_and_join() noexcept;

    private:
        struct Context final
        {
            core::HandleView pipe{};
            core::HandleView stop_event{};
            logging::Logger* logger{};
        };

        static DWORD WINAPI thread_proc(void* param) noexcept;

        core::UniqueHandle _thread;
        core::UniqueHandle _pipe;
        std::unique_ptr<Context> _context;
    };
}
