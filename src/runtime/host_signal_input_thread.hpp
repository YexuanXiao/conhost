#pragma once

// Host signal pipe reader used during default-terminal delegation ("handoff").
//
// When the inbox console host (openconsole_new in `--server` startup mode) delegates
// UI hosting to a third-party terminal via `IConsoleHandoff::EstablishHandoff`, it
// provides the delegated host with a write-only pipe handle. The delegated host
// uses this pipe to request that the inbox host performs certain privileged console
// control operations on its behalf (e.g. EndTask).
//
// This module reads that pipe on a dedicated Win32 thread and dispatches decoded
// packets to an injected target interface.

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
    struct HostSignalInputThreadError final
    {
        std::wstring context;
        DWORD win32_error{ ERROR_GEN_FAILURE };
    };

    class HostSignalTarget
    {
    public:
        virtual ~HostSignalTarget() = default;

        virtual void notify_console_application(DWORD process_id) noexcept = 0;
        virtual void set_foreground(DWORD process_handle_value, bool is_foreground) noexcept = 0;
        virtual void end_task(DWORD process_id, DWORD event_type, DWORD ctrl_flags) noexcept = 0;

        // Called when the signal pipe is disconnected (the delegated host exited)
        // or when the reader is otherwise unable to continue.
        virtual void signal_pipe_disconnected() noexcept = 0;
    };

    class HostSignalInputThread final
    {
    public:
        HostSignalInputThread() noexcept = default;
        ~HostSignalInputThread() noexcept
        {
            stop_and_join();
        }

        HostSignalInputThread(const HostSignalInputThread&) = delete;
        HostSignalInputThread& operator=(const HostSignalInputThread&) = delete;

        HostSignalInputThread(HostSignalInputThread&& other) noexcept :
            _thread(std::move(other._thread)),
            _pipe(std::move(other._pipe)),
            _stop_event(std::move(other._stop_event)),
            _context(std::move(other._context))
        {
            other._context = nullptr;
        }

        HostSignalInputThread& operator=(HostSignalInputThread&& other) noexcept
        {
            if (this != &other)
            {
                stop_and_join();
                _thread = std::move(other._thread);
                _pipe = std::move(other._pipe);
                _stop_event = std::move(other._stop_event);
                _context = std::move(other._context);
                other._context = nullptr;
            }
            return *this;
        }

        [[nodiscard]] static std::expected<HostSignalInputThread, HostSignalInputThreadError> start(
            core::HandleView pipe_read_end,
            HostSignalTarget& target,
            logging::Logger* logger) noexcept;

        [[nodiscard]] core::HandleView thread_handle() const noexcept
        {
            return _thread.view();
        }

        void stop_and_join() noexcept;

    private:
        struct Context final
        {
            core::HandleView pipe{};
            core::HandleView stop_event{};
            HostSignalTarget* target{};
            logging::Logger* logger{};
        };

        static DWORD WINAPI thread_proc(void* param) noexcept;

        core::UniqueHandle _thread;
        core::UniqueHandle _pipe;
        core::UniqueHandle _stop_event;
        std::unique_ptr<Context> _context;
    };
}
