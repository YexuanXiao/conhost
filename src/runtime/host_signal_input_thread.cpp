#include "runtime/host_signal_input_thread.hpp"

#include "core/host_signals.hpp"
#include "core/win32_handle.hpp"
#include "logging/logger.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <span>

// Implementation notes:
// - The host-signal pipe payloads are packed POD structs whose layout must match
//   the upstream OpenConsole contract (see `core/host_signals.hpp`).
// - All reads are performed on the dedicated thread. The owning `HostSignalInputThread`
//   instance never performs pipe I/O directly.
// - Shutdown is cooperative: we signal a private stop event and the worker thread
//   observes it between `PeekNamedPipe` polls.
//
// This module is intentionally resilient:
// - Pipe disconnect is treated as a normal terminal condition and triggers
//   `HostSignalTarget::signal_pipe_disconnected()`.
// - Unknown codes are ignored after draining the declared payload size so the
//   stream can continue.

namespace oc::runtime
{
    namespace
    {
        [[nodiscard]] HostSignalInputThreadError make_error(std::wstring context, const DWORD win32_error) noexcept
        {
            return HostSignalInputThreadError{
                .context = std::move(context),
                .win32_error = win32_error == 0 ? ERROR_GEN_FAILURE : win32_error,
            };
        }

        [[nodiscard]] bool is_pipe_disconnect_error(const DWORD error) noexcept
        {
            return error == ERROR_BROKEN_PIPE ||
                error == ERROR_PIPE_NOT_CONNECTED ||
                error == ERROR_NO_DATA;
        }

        [[nodiscard]] bool is_cancellation_error(const DWORD error) noexcept
        {
            return error == ERROR_OPERATION_ABORTED ||
                error == ERROR_CANCELLED;
        }

        [[nodiscard]] std::expected<void, DWORD> read_exact(
            const oc::core::HandleView pipe,
            const oc::core::HandleView stop_event,
            const std::span<std::byte> dest) noexcept
        {
            if (!pipe)
            {
                return std::unexpected(ERROR_INVALID_HANDLE);
            }
            if (!stop_event)
            {
                return std::unexpected(ERROR_INVALID_HANDLE);
            }

            size_t total_read = 0;
            while (total_read < dest.size())
            {
                const DWORD stop_state = ::WaitForSingleObject(stop_event.get(), 0);
                if (stop_state == WAIT_OBJECT_0)
                {
                    return std::unexpected(ERROR_OPERATION_ABORTED);
                }

                DWORD available = 0;
                if (::PeekNamedPipe(pipe.get(), nullptr, 0, nullptr, &available, nullptr) == FALSE)
                {
                    return std::unexpected(::GetLastError());
                }

                if (available == 0)
                {
                    const DWORD wait_result = ::WaitForSingleObject(stop_event.get(), 25);
                    if (wait_result == WAIT_OBJECT_0)
                    {
                        return std::unexpected(ERROR_OPERATION_ABORTED);
                    }
                    continue;
                }

                const auto remaining = dest.subspan(total_read);
                const size_t to_read_size = available < remaining.size()
                    ? static_cast<size_t>(available)
                    : remaining.size();
                const DWORD to_read = to_read_size > static_cast<size_t>(std::numeric_limits<DWORD>::max())
                    ? std::numeric_limits<DWORD>::max()
                    : static_cast<DWORD>(to_read_size);

                DWORD advanced = 0;
                if (::ReadFile(pipe.get(), remaining.data(), to_read, &advanced, nullptr) == FALSE)
                {
                    return std::unexpected(::GetLastError());
                }
                if (advanced == 0)
                {
                    return std::unexpected(ERROR_BROKEN_PIPE);
                }

                total_read += static_cast<size_t>(advanced);
            }

            return {};
        }

        [[nodiscard]] std::expected<void, DWORD> discard_exact(
            const oc::core::HandleView pipe,
            const oc::core::HandleView stop_event,
            size_t byte_count) noexcept
        {
            std::array<std::byte, 256> buffer{};
            while (byte_count > 0)
            {
                const size_t advance = byte_count < buffer.size() ? byte_count : buffer.size();
                auto read = read_exact(pipe, stop_event, std::span(buffer.data(), advance));
                if (!read)
                {
                    return std::unexpected(read.error());
                }
                byte_count -= advance;
            }

            return {};
        }

        template<typename T>
        [[nodiscard]] std::expected<T, DWORD> receive_typed_packet(
            const oc::core::HandleView pipe,
            const oc::core::HandleView stop_event) noexcept
        {
            T payload{};
            auto bytes = std::as_writable_bytes(std::span{ &payload, 1 });
            auto read = read_exact(pipe, stop_event, bytes);
            if (!read)
            {
                return std::unexpected(read.error());
            }

            if (payload.sizeInBytes < sizeof(payload))
            {
                return std::unexpected(ERROR_BAD_LENGTH);
            }

            const size_t extra_bytes = static_cast<size_t>(payload.sizeInBytes - sizeof(payload));
            if (extra_bytes != 0)
            {
                auto discarded = discard_exact(pipe, stop_event, extra_bytes);
                if (!discarded)
                {
                    return std::unexpected(discarded.error());
                }
            }

            return payload;
        }
    }

    DWORD WINAPI HostSignalInputThread::thread_proc(void* param) noexcept
    {
        auto* context = static_cast<Context*>(param);
        if (context == nullptr || !context->pipe || context->target == nullptr)
        {
            return 0;
        }

        try
        {
            for (;;)
            {
                std::uint8_t code_raw = 0;
                auto code_read = read_exact(
                    context->pipe,
                    context->stop_event,
                    std::as_writable_bytes(std::span{ &code_raw, 1 }));
                if (!code_read)
                {
                    const DWORD error = code_read.error();
                    if (is_cancellation_error(error))
                    {
                        return 0;
                    }

                    if (context->logger != nullptr && !is_pipe_disconnect_error(error))
                    {
                        context->logger->log(logging::LogLevel::debug, L"Host signal pipe read failed (error={})", error);
                    }

                    context->target->signal_pipe_disconnected();
                    return 0;
                }

                const auto code = static_cast<core::HostSignals>(code_raw);
                switch (code)
                {
                case core::HostSignals::notify_app:
                {
                    auto payload = receive_typed_packet<core::HostSignalNotifyAppData>(
                        context->pipe,
                        context->stop_event);
                    if (!payload)
                    {
                        const DWORD error = payload.error();
                        if (is_cancellation_error(error))
                        {
                            return 0;
                        }

                        if (context->logger != nullptr && !is_pipe_disconnect_error(error))
                        {
                            context->logger->log(logging::LogLevel::debug, L"Host signal NotifyApp read failed (error={})", error);
                        }
                        context->target->signal_pipe_disconnected();
                        return 0;
                    }

                    context->target->notify_console_application(payload->processId);
                    break;
                }
                case core::HostSignals::set_foreground:
                {
                    auto payload = receive_typed_packet<core::HostSignalSetForegroundData>(
                        context->pipe,
                        context->stop_event);
                    if (!payload)
                    {
                        const DWORD error = payload.error();
                        if (is_cancellation_error(error))
                        {
                            return 0;
                        }

                        if (context->logger != nullptr && !is_pipe_disconnect_error(error))
                        {
                            context->logger->log(logging::LogLevel::debug, L"Host signal SetForeground read failed (error={})", error);
                        }
                        context->target->signal_pipe_disconnected();
                        return 0;
                    }

                    context->target->set_foreground(payload->processId, payload->isForeground);
                    break;
                }
                case core::HostSignals::end_task:
                {
                    auto payload = receive_typed_packet<core::HostSignalEndTaskData>(
                        context->pipe,
                        context->stop_event);
                    if (!payload)
                    {
                        const DWORD error = payload.error();
                        if (is_cancellation_error(error))
                        {
                            return 0;
                        }

                        if (context->logger != nullptr && !is_pipe_disconnect_error(error))
                        {
                            context->logger->log(logging::LogLevel::debug, L"Host signal EndTask read failed (error={})", error);
                        }
                        context->target->signal_pipe_disconnected();
                        return 0;
                    }

                    context->target->end_task(payload->processId, payload->eventType, payload->ctrlFlags);
                    break;
                }
                default:
                {
                    if (context->logger != nullptr)
                    {
                        context->logger->log(logging::LogLevel::debug, L"Host signal pipe received unknown code {}", static_cast<unsigned>(code_raw));
                    }
                    context->target->signal_pipe_disconnected();
                    return 0;
                }
                }
            }
        }
        catch (...)
        {
            // Avoid propagating exceptions across the Win32 thread boundary.
            if (context->logger != nullptr)
            {
                try
                {
                    context->logger->log(logging::LogLevel::warning, L"Host signal input thread terminated due to an unexpected exception");
                }
                catch (...)
                {
                }
            }
            context->target->signal_pipe_disconnected();
            return 0;
        }
    }

    std::expected<HostSignalInputThread, HostSignalInputThreadError> HostSignalInputThread::start(
        const core::HandleView pipe_read_end,
        HostSignalTarget& target,
        logging::Logger* const logger) noexcept
    {
        if (!pipe_read_end)
        {
            return std::unexpected(make_error(L"Host signal pipe read handle was invalid", ERROR_INVALID_HANDLE));
        }

        auto stop_event = core::create_event(true, false, nullptr);
        if (!stop_event)
        {
            return std::unexpected(make_error(L"CreateEventW failed for host signal input thread stop event", stop_event.error()));
        }

        auto duplicated_pipe = core::duplicate_handle_same_access(pipe_read_end, false);
        if (!duplicated_pipe)
        {
            return std::unexpected(make_error(L"DuplicateHandle failed for host signal pipe read handle", duplicated_pipe.error()));
        }

        std::unique_ptr<Context> context;
        try
        {
            context = std::make_unique<Context>();
        }
        catch (...)
        {
            return std::unexpected(make_error(L"Failed to allocate host signal input context", ERROR_OUTOFMEMORY));
        }

        core::UniqueHandle owned_pipe = std::move(duplicated_pipe.value());
        context->pipe = owned_pipe.view();
        context->stop_event = stop_event->view();
        context->target = &target;
        context->logger = logger;

        core::UniqueHandle thread(::CreateThread(
            nullptr,
            0,
            &HostSignalInputThread::thread_proc,
            context.get(),
            0,
            nullptr));
        if (!thread.valid())
        {
            return std::unexpected(make_error(L"CreateThread failed for host signal input thread", ::GetLastError()));
        }

        HostSignalInputThread input{};
        input._thread = std::move(thread);
        input._pipe = std::move(owned_pipe);
        input._stop_event = std::move(stop_event.value());
        input._context = std::move(context);
        return input;
    }

    void HostSignalInputThread::stop_and_join() noexcept
    {
        if (_thread.valid())
        {
            if (_stop_event.valid())
            {
                (void)::SetEvent(_stop_event.get());
            }
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
        _stop_event.reset();
    }
}
