#include "runtime/signal_pipe_monitor.hpp"

#include "core/win32_handle.hpp"
#include "core/win32_io.hpp"
#include "logging/logger.hpp"

#include <array>
#include <cstddef>
/*
Root cause (vs upstream conhost):

In ConPTY/server-handle startup (0x4), the --signal 0x... handle is a pipe (used by conhost’s VT/ConPTY signal thread). It is not a waitable event.
openconsole_new was treating options.signal_handle as a waitable “stop” handle and passing it into the ConDrv server loop. When the terminal/client side closed, we never reliably observed “broken pipe”, so the server could stay alive indefinitely.
Fix implemented:

Added a small non-GUI RAII module runtime::SignalPipeMonitor that drains the signal pipe and, on ERROR_BROKEN_PIPE/EOF, signals a manual-reset event.
Updated runtime::Session::run so that when options.signal_handle is a pipe (GetFileType(...) == FILE_TYPE_PIPE), we:
start SignalPipeMonitor, and
pass the event (not the pipe) to condrv::ConDrvServer::run as the stop signal.
This makes the ConDrv server exit promptly when the hosting side disappears, matching conhost’s “signal pipe break == shutdown” behavior.
*/
namespace oc::runtime
{
    namespace
    {
        [[nodiscard]] SignalPipeMonitorError make_error(std::wstring context, const DWORD win32_error) noexcept
        {
            return SignalPipeMonitorError{
                .context = std::move(context),
                .win32_error = win32_error == 0 ? ERROR_GEN_FAILURE : win32_error,
            };
        }
    }

    DWORD WINAPI SignalPipeMonitor::thread_proc(void* param) noexcept
    {
        auto* context = static_cast<Context*>(param);
        if (context == nullptr || !context->pipe || !context->stop_event)
        {
            return 0;
        }

        core::BlockingFileReader reader(context->pipe);

        std::array<std::byte, 256> buffer{};
        for (;;)
        {
            auto read = reader.read(buffer);
            if (!read)
            {
                const DWORD error = read.error();
                if (error == ERROR_BROKEN_PIPE || error == ERROR_PIPE_NOT_CONNECTED || error == ERROR_NO_DATA)
                {
                    if (context->logger != nullptr)
                    {
                        context->logger->log(logging::LogLevel::debug, L"Signal pipe disconnected");
                    }
                    (void)::SetEvent(context->stop_event.get());
                    return 0;
                }

                if (error == ERROR_OPERATION_ABORTED || error == ERROR_CANCELLED)
                {
                    // Cancellation during shutdown.
                    return 0;
                }

                if (context->logger != nullptr)
                {
                    context->logger->log(logging::LogLevel::debug, L"ReadFile failed for signal pipe (error={})", error);
                }
                (void)::SetEvent(context->stop_event.get());
                return 0;
            }

            if (read.value() == 0)
            {
                if (context->logger != nullptr)
                {
                    context->logger->log(logging::LogLevel::debug, L"Signal pipe reached EOF");
                }
                (void)::SetEvent(context->stop_event.get());
                return 0;
            }
        }
    }

    std::expected<SignalPipeMonitor, SignalPipeMonitorError> SignalPipeMonitor::start(
        const core::HandleView signal_pipe,
        const core::HandleView stop_event,
        logging::Logger* const logger) noexcept
    {
        if (!signal_pipe)
        {
            return std::unexpected(make_error(L"Signal pipe handle was invalid", ERROR_INVALID_HANDLE));
        }
        if (!stop_event)
        {
            return std::unexpected(make_error(L"Signal pipe monitor stop event was invalid", ERROR_INVALID_HANDLE));
        }

        auto duplicated_pipe = core::duplicate_handle_same_access(signal_pipe, false);
        if (!duplicated_pipe)
        {
            return std::unexpected(make_error(L"DuplicateHandle failed for signal pipe", duplicated_pipe.error()));
        }

        std::unique_ptr<Context> context;
        try
        {
            context = std::make_unique<Context>();
        }
        catch (...)
        {
            return std::unexpected(make_error(L"Failed to allocate signal pipe monitor context", ERROR_OUTOFMEMORY));
        }
        context->pipe = duplicated_pipe->view();
        context->stop_event = stop_event;
        context->logger = logger;

        core::UniqueHandle thread(::CreateThread(
            nullptr,
            0,
            &SignalPipeMonitor::thread_proc,
            context.get(),
            0,
            nullptr));
        if (!thread.valid())
        {
            return std::unexpected(make_error(L"CreateThread failed for signal pipe monitor", ::GetLastError()));
        }

        SignalPipeMonitor monitor{};
        monitor._thread = std::move(thread);
        monitor._pipe = std::move(duplicated_pipe.value());
        monitor._context = std::move(context);
        return monitor;
    }

    void SignalPipeMonitor::stop_and_join() noexcept
    {
        if (_thread.valid())
        {
            // Best-effort cancellation: unblock a synchronous ReadFile in the monitor thread.
            if (_pipe.valid())
            {
                (void)::CancelIoEx(_pipe.get(), nullptr);
            }
            (void)::CancelSynchronousIo(_thread.get());
            (void)::WaitForSingleObject(_thread.get(), INFINITE);
            _thread.reset();
        }

        _context.reset();
        _pipe.reset();
    }
}
