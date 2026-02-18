#include "condrv/condrv_server.hpp"

#include "core/unique_handle.hpp"
#include "core/host_signals.hpp"
#include "core/win32_handle.hpp"
#include "core/win32_wait.hpp"
#include "logging/logger.hpp"

#include <Windows.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstring>
#include <deque>
#include <limits>
#include <mutex>
#include <optional>
#include <span>
#include <vector>

// `condrv/condrv_server.cpp` implements the classic ConDrv server loop used for
// `--server` startup.
//
// The core loop is structured to match the observable behavior of the inbox
// conhost IO thread:
// - Block on `IOCTL_CONDRV_READ_IO` to receive the next driver request.
// - Dispatch the request to the in-memory console model (`ServerState`,
//   `ScreenBuffer`, input queue, etc.).
// - Complete the request back to the driver (including output buffer writes).
//
// Key behavioral compatibility points:
// - Reply-pending: input-dependent operations do not block the loop; they return
//   `reply_pending=true` and are retried later (see
//   `new/docs/design/condrv_reply_pending_wait_queue.md`).
// - Shutdown signaling: the server may be asked to stop via a waitable event
//   (or, in ConPTY startups, an event derived from a signal pipe monitor).
//
// The implementation intentionally keeps raw HANDLE usage localized and relies
// on move-only RAII wrappers (`core::UniqueHandle`) for ownership safety.

namespace oc::condrv
{
    namespace
    {
        [[nodiscard]] ServerError make_error(std::wstring context, const DWORD win32_error) noexcept
        {
            return ServerError{
                .context = std::move(context),
                .win32_error = win32_error == 0 ? ERROR_GEN_FAILURE : win32_error,
            };
        }

        [[nodiscard]] std::expected<core::UniqueHandle, ServerError> create_manual_reset_event() noexcept
        {
            auto created = core::create_event(true, false, nullptr);
            if (!created)
            {
                return std::unexpected(make_error(L"CreateEventW failed", created.error()));
            }

            return std::move(created.value());
        }

        [[nodiscard]] std::expected<core::UniqueHandle, ServerError> duplicate_current_thread() noexcept
        {
            auto duplicated = core::duplicate_current_thread();
            if (!duplicated)
            {
                return std::unexpected(make_error(L"DuplicateHandle failed for current thread", duplicated.error()));
            }

            return std::move(duplicated.value());
        }

        struct SignalMonitorContext final
        {
            core::HandleView signal_handle{};
            core::HandleView stop_event{};
            core::HandleView target_thread{};
            std::atomic_bool* stop_requested{};
        };

        DWORD WINAPI signal_monitor_thread(void* param)
        {
            // This helper thread exists solely because the server thread blocks
            // in `IOCTL_CONDRV_READ_IO`. When an external stop is requested, we
            // set `stop_requested=true` and cancel the server thread's
            // synchronous device IO so it can observe the flag and exit.
            auto* context = static_cast<SignalMonitorContext*>(param);
            if (context == nullptr || context->stop_requested == nullptr)
            {
                return 0;
            }

            const DWORD result = core::wait_for_two_objects(
                context->signal_handle,
                context->stop_event,
                false,
                INFINITE);
            if (result == WAIT_OBJECT_0)
            {
                context->stop_requested->store(true, std::memory_order_release);
                ::CancelSynchronousIo(context->target_thread.get());
            }

            return 0;
        }

        class SignalMonitor final
        {
        public:
            SignalMonitor() noexcept = default;
            ~SignalMonitor() noexcept
            {
                stop_and_join();
            }

            SignalMonitor(const SignalMonitor&) = delete;
            SignalMonitor& operator=(const SignalMonitor&) = delete;

            SignalMonitor(SignalMonitor&& other) noexcept :
                _thread(std::move(other._thread)),
                _stop_event(std::move(other._stop_event)),
                _target_thread(std::move(other._target_thread)),
                _context(std::move(other._context))
            {
                other._context = nullptr;
            }

            SignalMonitor& operator=(SignalMonitor&& other) noexcept
            {
                if (this != &other)
                {
                    stop_and_join();
                    _thread = std::move(other._thread);
                    _stop_event = std::move(other._stop_event);
                    _target_thread = std::move(other._target_thread);
                    _context = std::move(other._context);
                    other._context = nullptr;
                }
                return *this;
            }

            [[nodiscard]] static std::expected<SignalMonitor, ServerError> start(
                const core::HandleView signal_handle,
                std::atomic_bool& stop_requested) noexcept
            {
                if (!signal_handle)
                {
                    return SignalMonitor{};
                }

                auto stop_event = create_manual_reset_event();
                if (!stop_event)
                {
                    return std::unexpected(stop_event.error());
                }

                auto target_thread = duplicate_current_thread();
                if (!target_thread)
                {
                    return std::unexpected(target_thread.error());
                }

                std::unique_ptr<SignalMonitorContext> context;
                try
                {
                    context = std::make_unique<SignalMonitorContext>();
                }
                catch (...)
                {
                    return std::unexpected(make_error(L"Failed to allocate signal monitor context", ERROR_OUTOFMEMORY));
                }
                context->signal_handle = signal_handle;
                context->stop_event = stop_event->view();
                context->target_thread = target_thread->view();
                context->stop_requested = &stop_requested;

                core::UniqueHandle thread(::CreateThread(
                    nullptr,
                    0,
                    &signal_monitor_thread,
                    context.get(),
                    0,
                    nullptr));
                if (!thread.valid())
                {
                    return std::unexpected(make_error(L"CreateThread failed for signal monitor", ::GetLastError()));
                }

                SignalMonitor monitor{};
                monitor._thread = std::move(thread);
                monitor._stop_event = std::move(stop_event.value());
                monitor._target_thread = std::move(target_thread.value());
                monitor._context = std::move(context);
                return monitor;
            }

            void request_stop() noexcept
            {
                if (_stop_event.valid())
                {
                    ::SetEvent(_stop_event.get());
                }
            }

        private:
            void stop_and_join() noexcept
            {
                request_stop();

                if (_thread.valid())
                {
                    ::WaitForSingleObject(_thread.get(), INFINITE);
                    _thread.reset();
                }

                _context.reset();
                _target_thread.reset();
                _stop_event.reset();
            }

            core::UniqueHandle _thread;
            core::UniqueHandle _stop_event;
            core::UniqueHandle _target_thread;
            std::unique_ptr<SignalMonitorContext> _context;
        };

        class InputQueue final
        {
        public:
            explicit InputQueue(const core::HandleView input_available_event) noexcept :
                _input_available_event(input_available_event)
            {
            }

            [[nodiscard]] bool disconnected() const noexcept
            {
                return _disconnected.load(std::memory_order_acquire);
            }

            void mark_disconnected() noexcept
            {
                _disconnected.store(true, std::memory_order_release);
                if (_input_available_event)
                {
                    (void)::SetEvent(_input_available_event.get());
                }
            }

            [[nodiscard]] size_t available() const noexcept
            {
                std::scoped_lock lock(_mutex);
                return _storage.size() - _read_offset;
            }

            void clear() noexcept
            {
                std::scoped_lock lock(_mutex);
                _storage.clear();
                _read_offset = 0;
                update_event_locked();
            }

            void push(const std::span<const std::byte> data) noexcept
            {
                if (data.empty())
                {
                    return;
                }

                try
                {
                    std::scoped_lock lock(_mutex);
                    if (_read_offset == _storage.size())
                    {
                        _storage.clear();
                        _read_offset = 0;
                    }

                    _storage.insert(_storage.end(), data.begin(), data.end());
                    update_event_locked();
                }
                catch (...)
                {
                    // Best-effort: input loss is preferable to crashing the monitor
                    // thread. Leave the queue in a consistent state.
                }
            }

            [[nodiscard]] size_t peek(const std::span<std::byte> dest) const noexcept
            {
                if (dest.empty())
                {
                    return 0;
                }

                std::scoped_lock lock(_mutex);
                const size_t available_bytes = _storage.size() - _read_offset;
                const size_t to_copy = std::min(available_bytes, dest.size());
                if (to_copy != 0)
                {
                    std::memcpy(dest.data(), _storage.data() + _read_offset, to_copy);
                }
                return to_copy;
            }

            [[nodiscard]] size_t pop(const std::span<std::byte> dest) noexcept
            {
                if (dest.empty())
                {
                    return 0;
                }

                std::scoped_lock lock(_mutex);
                const size_t available = _storage.size() - _read_offset;
                const size_t to_copy = std::min(available, dest.size());
                if (to_copy != 0)
                {
                    std::memcpy(dest.data(), _storage.data() + _read_offset, to_copy);
                    _read_offset += to_copy;
                }

                if (_read_offset == _storage.size())
                {
                    _storage.clear();
                    _read_offset = 0;
                }

                update_event_locked();
                return to_copy;
            }

        private:
            void update_event_locked() const noexcept
            {
                if (!_input_available_event)
                {
                    return;
                }

                const bool has_data = !_storage.empty();
                const bool should_signal = has_data || disconnected();
                if (should_signal)
                {
                    (void)::SetEvent(_input_available_event.get());
                }
                else
                {
                    (void)::ResetEvent(_input_available_event.get());
                }
            }

            core::HandleView _input_available_event{};
            std::atomic_bool _disconnected{ false };
            mutable std::mutex _mutex;
            std::vector<std::byte> _storage;
            size_t _read_offset{ 0 };
        };

        struct InputMonitorContext final
        {
            core::HandleView host_input{};
            core::HandleView target_thread{};
            InputQueue* queue{};
            std::atomic_bool* stop_requested{};
            std::atomic_bool* has_pending_replies{};
            std::atomic_bool* in_driver_read_io{};
        };

        DWORD WINAPI input_monitor_thread(void* param)
        {
            // Host input monitor:
            // - Reads from the host-side input byte stream (typically a pipe).
            // - Appends bytes into the in-memory `InputQueue`.
            // - If the ConDrv server thread is blocked inside `READ_IO` *and*
            //   there are reply-pending requests, cancel that device read so
            //   the server can retry pending requests promptly.
            //
            // See `new/docs/design/condrv_reply_pending_wait_queue.md`.
            auto* context = static_cast<InputMonitorContext*>(param);
            if (context == nullptr || context->queue == nullptr || context->stop_requested == nullptr)
            {
                return 0;
            }

            const auto maybe_wake_server = [&]() noexcept {
                // Best-effort wake: `CancelSynchronousIo` targets the server
                // thread's `IOCTL_CONDRV_READ_IO` call. Guard usage to the
                // intended "pending replies exist and server is currently
                // reading" case to avoid canceling unrelated synchronous IO.
                if (!context->target_thread ||
                    context->has_pending_replies == nullptr ||
                    context->in_driver_read_io == nullptr)
                {
                    return;
                }

                if (!context->has_pending_replies->load(std::memory_order_acquire))
                {
                    return;
                }

                if (!context->in_driver_read_io->load(std::memory_order_acquire))
                {
                    return;
                }

                (void)::CancelSynchronousIo(context->target_thread.get());
            };

            std::array<std::byte, 4096> buffer{};
            while (!context->stop_requested->load(std::memory_order_acquire))
            {
                DWORD read = 0;
                if (::ReadFile(
                        context->host_input.get(),
                        buffer.data(),
                        static_cast<DWORD>(buffer.size()),
                        &read,
                        nullptr) == FALSE)
                {
                    const DWORD error = ::GetLastError();
                    if ((error == ERROR_OPERATION_ABORTED || error == ERROR_CANCELLED) &&
                        context->stop_requested->load(std::memory_order_acquire))
                    {
                        break;
                    }

                    if (error == ERROR_BROKEN_PIPE || error == ERROR_PIPE_NOT_CONNECTED)
                    {
                        break;
                    }

                    // Treat other errors as terminal for now.
                    break;
                }

                if (read == 0)
                {
                    // EOF or no data; treat as disconnect.
                    break;
                }

                context->queue->push(std::span<const std::byte>(buffer.data(), static_cast<size_t>(read)));
                maybe_wake_server();
            }

            context->queue->mark_disconnected();
            maybe_wake_server();
            return 0;
        }

        class InputMonitor final
        {
        public:
            InputMonitor() noexcept = default;
            ~InputMonitor() noexcept
            {
                stop_and_join();
            }

            InputMonitor(const InputMonitor&) = delete;
            InputMonitor& operator=(const InputMonitor&) = delete;

            InputMonitor(InputMonitor&& other) noexcept :
                _thread(std::move(other._thread)),
                _context(std::move(other._context)),
                _stop_requested(other._stop_requested.load(std::memory_order_acquire))
            {
                other._context = nullptr;
                other._stop_requested.store(true, std::memory_order_release);
            }

            InputMonitor& operator=(InputMonitor&& other) noexcept
            {
                if (this != &other)
                {
                    stop_and_join();
                    _thread = std::move(other._thread);
                    _context = std::move(other._context);
                    _stop_requested.store(other._stop_requested.load(std::memory_order_acquire), std::memory_order_release);
                    other._context = nullptr;
                    other._stop_requested.store(true, std::memory_order_release);
                }
                return *this;
            }

            [[nodiscard]] static std::expected<InputMonitor, ServerError> start(
                const core::HandleView host_input,
                InputQueue& queue,
                const core::HandleView target_thread,
                std::atomic_bool& has_pending_replies,
                std::atomic_bool& in_driver_read_io) noexcept
            {
                if (!host_input)
                {
                    return InputMonitor{};
                }

                std::unique_ptr<InputMonitorContext> context;
                try
                {
                    context = std::make_unique<InputMonitorContext>();
                }
                catch (...)
                {
                    return std::unexpected(make_error(L"Failed to allocate input monitor context", ERROR_OUTOFMEMORY));
                }

                InputMonitor monitor{};
                context->host_input = host_input;
                context->target_thread = target_thread;
                context->queue = &queue;
                context->stop_requested = &monitor._stop_requested;
                context->has_pending_replies = &has_pending_replies;
                context->in_driver_read_io = &in_driver_read_io;

                core::UniqueHandle thread(::CreateThread(
                    nullptr,
                    0,
                    &input_monitor_thread,
                    context.get(),
                    0,
                    nullptr));
                if (!thread.valid())
                {
                    return std::unexpected(make_error(L"CreateThread failed for input monitor", ::GetLastError()));
                }

                monitor._thread = std::move(thread);
                monitor._context = std::move(context);
                return monitor;
            }

        private:
            void stop_and_join() noexcept
            {
                _stop_requested.store(true, std::memory_order_release);

                if (_thread.valid())
                {
                    // Best-effort cancellation: unblock a synchronous ReadFile on the thread.
                    (void)::CancelSynchronousIo(_thread.get());
                    ::WaitForSingleObject(_thread.get(), INFINITE);
                    _thread.reset();
                }

                _context.reset();
            }

            core::UniqueHandle _thread;
            std::unique_ptr<InputMonitorContext> _context;
            std::atomic_bool _stop_requested{ false };
        };

        class HostIo final
        {
        public:
            HostIo(
                const core::HandleView host_input,
                const core::HandleView host_output,
                const core::HandleView host_signal_pipe,
                const core::HandleView input_available_event,
                const core::HandleView signal_handle,
                InputQueue& input_queue) noexcept :
                _host_input(host_input),
                _host_output(host_output),
                _host_signal_pipe(host_signal_pipe),
                _input_available_event(input_available_event),
                _signal_handle(signal_handle),
                _input_queue(&input_queue)
            {
            }

            [[nodiscard]] std::expected<size_t, DeviceCommError> write_output_bytes(const std::span<const std::byte> bytes) noexcept
            {
                if (bytes.empty())
                {
                    return size_t{ 0 };
                }

                if (!_host_output)
                {
                    // No output target: treat as success and discard.
                    return bytes.size();
                }

                size_t total_written = 0;
                while (total_written < bytes.size())
                {
                    const size_t remaining = bytes.size() - total_written;
                    const DWORD chunk = remaining > static_cast<size_t>(std::numeric_limits<DWORD>::max())
                        ? std::numeric_limits<DWORD>::max()
                        : static_cast<DWORD>(remaining);

                    DWORD written = 0;
                    if (::WriteFile(
                            _host_output.get(),
                            bytes.data() + total_written,
                            chunk,
                            &written,
                            nullptr) == FALSE)
                    {
                        return std::unexpected(DeviceCommError{
                            .context = L"WriteFile failed for host output",
                            .win32_error = ::GetLastError(),
                        });
                    }

                    total_written += static_cast<size_t>(written);
                    if (written == 0)
                    {
                        break;
                    }
                }

                return total_written;
            }

            [[nodiscard]] std::expected<size_t, DeviceCommError> read_input_bytes(const std::span<std::byte> dest) noexcept
            {
                if (_input_queue == nullptr)
                {
                    return size_t{ 0 };
                }

                (void)_host_input;
                return _input_queue->pop(dest);
            }

            [[nodiscard]] std::expected<size_t, DeviceCommError> peek_input_bytes(const std::span<std::byte> dest) noexcept
            {
                if (_input_queue == nullptr)
                {
                    return size_t{ 0 };
                }

                (void)_host_input;
                return _input_queue->peek(dest);
            }

            [[nodiscard]] size_t input_bytes_available() const noexcept
            {
                if (_input_queue == nullptr)
                {
                    return 0;
                }

                return _input_queue->available();
            }

            [[nodiscard]] bool input_disconnected() const noexcept
            {
                if (!_input_available_event || _input_queue == nullptr)
                {
                    return true;
                }

                return _input_queue->disconnected();
            }

            [[nodiscard]] bool inject_input_bytes(const std::span<const std::byte> bytes) noexcept
            {
                if (_input_queue == nullptr || bytes.empty())
                {
                    return true;
                }

                (void)_host_input;
                _input_queue->push(bytes);
                return true;
            }

            [[nodiscard]] bool vt_should_answer_queries() const noexcept
            {
                // When output is forwarded to an external terminal (ConPTY), we expect the terminal
                // to answer status/position queries. When there is no output target (classic window),
                // the host answers queries itself.
                return !_host_output;
            }

            [[nodiscard]] std::expected<void, DeviceCommError> flush_input_buffer() noexcept
            {
                if (_input_queue == nullptr)
                {
                    return {};
                }

                _input_queue->clear();
                return {};
            }

            [[nodiscard]] std::expected<bool, DeviceCommError> wait_for_input(const DWORD timeout_ms) noexcept
            {
                if (!_host_input || !_input_available_event || _input_queue == nullptr)
                {
                    return false;
                }

                if (_input_queue->available() != 0)
                {
                    return true;
                }

                if (_input_queue->disconnected())
                {
                    return false;
                }

                DWORD wait_result = WAIT_FAILED;
                if (_signal_handle)
                {
                    wait_result = core::wait_for_two_objects(
                        _input_available_event,
                        _signal_handle,
                        false,
                        timeout_ms);
                }
                else
                {
                    wait_result = ::WaitForSingleObject(_input_available_event.get(), timeout_ms);
                }

                if (wait_result == WAIT_OBJECT_0)
                {
                    return !_input_queue->disconnected() && _input_queue->available() != 0;
                }

                if (_signal_handle && wait_result == WAIT_OBJECT_0 + 1)
                {
                    return false;
                }

                if (wait_result == WAIT_TIMEOUT)
                {
                    return false;
                }

                const DWORD error = ::GetLastError();
                return std::unexpected(DeviceCommError{
                    .context = L"WaitForSingleObject/WaitForMultipleObjects failed for input availability",
                    .win32_error = error,
                });
            }

            [[nodiscard]] std::expected<void, DeviceCommError> send_end_task(
                const DWORD process_id,
                const DWORD event_type,
                const DWORD ctrl_flags) noexcept
            {
                if (!_host_signal_pipe)
                {
                    return {};
                }

                core::HostSignalEndTaskData data{};
                data.sizeInBytes = sizeof(data);
                data.processId = process_id;
                data.eventType = event_type;
                data.ctrlFlags = ctrl_flags;

                auto result = core::write_host_signal_packet(
                    _host_signal_pipe,
                    core::HostSignals::end_task,
                    data);
                if (!result)
                {
                    return std::unexpected(DeviceCommError{
                        .context = L"WriteFile failed for host signal pipe (EndTask)",
                        .win32_error = result.error(),
                    });
                }

                return {};
            }

        private:
            core::HandleView _host_input{};
            core::HandleView _host_output{};
            core::HandleView _host_signal_pipe{};
            core::HandleView _input_available_event{};
            core::HandleView _signal_handle{};
            InputQueue* _input_queue{};
        };

        class AtomicFlagGuard final
        {
        public:
            explicit AtomicFlagGuard(std::atomic_bool& flag) noexcept :
                _flag(&flag)
            {
                _flag->store(true, std::memory_order_release);
            }

            ~AtomicFlagGuard() noexcept
            {
                if (_flag != nullptr)
                {
                    _flag->store(false, std::memory_order_release);
                }
            }

            AtomicFlagGuard(const AtomicFlagGuard&) = delete;
            AtomicFlagGuard& operator=(const AtomicFlagGuard&) = delete;

        private:
            std::atomic_bool* _flag{};
        };

        [[nodiscard]] std::expected<DWORD, ServerError> run_loop(
            const core::HandleView server_handle,
            const core::HandleView signal_handle,
            const core::HandleView input_available_event,
            const core::HandleView host_input,
            const core::HandleView host_output,
            const core::HandleView host_signal_pipe,
            const IoPacket* const initial_packet,
            std::shared_ptr<PublishedScreenBuffer> published_screen,
            const HWND paint_target,
            logging::Logger& logger) noexcept
        {
            if (!server_handle)
            {
                return std::unexpected(make_error(L"ConDrv server handle was invalid", ERROR_INVALID_HANDLE));
            }

            auto comm = ConDrvDeviceComm::from_server_handle(server_handle);
            if (!comm)
            {
                return std::unexpected(make_error(comm.error().context, comm.error().win32_error));
            }

            core::HandleView effective_input_event = input_available_event;
            core::UniqueHandle owned_input_event;
            if (!effective_input_event)
            {
                auto created = create_manual_reset_event();
                if (!created)
                {
                    return std::unexpected(created.error());
                }
                owned_input_event = std::move(created.value());
                effective_input_event = owned_input_event.view();
            }

            std::atomic_bool has_pending_replies{ false };
            std::atomic_bool in_driver_read_io{ false };

            core::UniqueHandle server_thread;
            {
                auto duplicated = duplicate_current_thread();
                if (!duplicated)
                {
                    return std::unexpected(duplicated.error());
                }
                server_thread = std::move(duplicated.value());
            }

            InputQueue input_queue(effective_input_event);
            auto input_monitor = InputMonitor::start(
                host_input,
                input_queue,
                server_thread.view(),
                has_pending_replies,
                in_driver_read_io);
            if (!input_monitor)
            {
                return std::unexpected(input_monitor.error());
            }

            if (auto server_info = comm->set_server_information(effective_input_event); !server_info)
            {
                // `IOCTL_CONDRV_SET_SERVER_INFORMATION` is expected to be issued once per session.
                // In handoff scenarios (default-terminal delegation or inbox-host fallback probing),
                // the previous host may have already set it. The ConDrv driver returns
                // `ERROR_BAD_COMMAND` for the redundant call; treat that as non-fatal so the server
                // can proceed with the inherited state.
                if (initial_packet != nullptr && server_info.error().win32_error == ERROR_BAD_COMMAND)
                {
                    logger.log(logging::LogLevel::debug, L"ConDrv server information was already set; continuing");
                }
                else
                {
                    return std::unexpected(make_error(server_info.error().context, server_info.error().win32_error));
                }
            }

            logger.log(logging::LogLevel::info, L"ConDrv server loop starting");

            std::atomic_bool stop_requested{ false };
            auto signal_monitor = SignalMonitor::start(signal_handle, stop_requested);
            if (!signal_monitor)
            {
                return std::unexpected(signal_monitor.error());
            }

            ServerState state{};
            HostIo host_io(
                host_input,
                host_output,
                host_signal_pipe,
                effective_input_event,
                signal_handle,
                input_queue);

            std::deque<ConDrvApiMessage> pending_replies;
            std::optional<ConDrvApiMessage> pending_completion;
            std::weak_ptr<ScreenBuffer> last_buffer;
            uint64_t last_revision = 0;

            const auto maybe_publish_snapshot = [&]() noexcept {
                if (!published_screen || paint_target == nullptr)
                {
                    return;
                }

                auto buffer = state.active_screen_buffer();
                if (!buffer)
                {
                    return;
                }

                bool buffer_changed = false;
                if (auto locked = last_buffer.lock(); !locked || locked.get() != buffer.get())
                {
                    buffer_changed = true;
                }

                const uint64_t revision = buffer->revision();
                if (!buffer_changed && revision == last_revision)
                {
                    return;
                }

                auto snapshot = make_viewport_snapshot(*buffer);
                if (!snapshot)
                {
                    return;
                }

                published_screen->publish(std::move(snapshot.value()));
                (void)::PostMessageW(paint_target, WM_APP + 1, 0, 0);

                last_buffer = buffer;
                last_revision = revision;
            };

            const auto release_message_buffers = [&](ConDrvApiMessage& message) noexcept -> std::expected<void, ServerError> {
                // `CancelSynchronousIo` is used to wake this thread when input arrives for reply-pending work.
                // Even with guarding, there is an unavoidable race where the cancellation lands while the
                // server is completing a different synchronous IOCTL. Treat cancellation errors as
                // transient and retry a few times so the server does not abort the process after it
                // already serviced a client request.
                constexpr int max_retries = 8;

                for (int attempt = 0;; ++attempt)
                {
                    auto released = message.release_message_buffers();
                    if (released)
                    {
                        break;
                    }

                    const DWORD error = released.error().win32_error;
                    if ((error == ERROR_OPERATION_ABORTED || error == ERROR_CANCELLED) && attempt < max_retries)
                    {
                        continue;
                    }

                    return std::unexpected(make_error(released.error().context, error));
                }

                return {};
            };

            const auto complete_io_direct = [&](ConDrvApiMessage& message) noexcept -> std::expected<void, ServerError> {
                // Slow-path fallback: in normal operation, completions are submitted via the optional
                // input buffer on `IOCTL_CONDRV_READ_IO`. Direct completion is kept only for shutdown
                // paths where there may be no subsequent ReadIo call.
                constexpr int max_retries = 8;

                for (int attempt = 0;; ++attempt)
                {
                    auto completed = message.complete_io();
                    if (completed)
                    {
                        break;
                    }

                    const DWORD error = completed.error().win32_error;
                    if ((error == ERROR_OPERATION_ABORTED || error == ERROR_CANCELLED) && attempt < max_retries)
                    {
                        continue;
                    }

                    return std::unexpected(make_error(completed.error().context, error));
                }

                return {};
            };

            const auto release_and_stage_completion = [&](ConDrvApiMessage& message) noexcept -> std::expected<void, ServerError> {
                OC_ASSERT(!pending_completion.has_value());

                if (auto released = release_message_buffers(message); !released)
                {
                    return std::unexpected(released.error());
                }

                pending_completion.emplace(std::move(message));
                return {};
            };

            const auto update_pending_flag = [&]() noexcept {
                has_pending_replies.store(!pending_replies.empty(), std::memory_order_release);
            };

            const auto service_pending_once = [&]() noexcept -> std::expected<bool, ServerError> {
                if (pending_replies.empty() || pending_completion.has_value())
                {
                    update_pending_flag();
                    return false;
                }

                bool progress = false;
                const size_t count = pending_replies.size();
                for (size_t i = 0; i < count; ++i)
                {
                    auto message = std::move(pending_replies.front());
                    pending_replies.pop_front();

                    auto outcome = dispatch_message(state, message, host_io);
                    if (!outcome)
                    {
                        return std::unexpected(make_error(outcome.error().context, outcome.error().win32_error));
                    }

                    if (outcome->reply_pending)
                    {
                        pending_replies.push_back(std::move(message));
                        continue;
                    }

                    if (auto finished = release_and_stage_completion(message); !finished)
                    {
                        return std::unexpected(finished.error());
                    }

                    progress = true;
                    break;
                }

                update_pending_flag();
                return progress;
            };

            const auto service_pending_until_stalled = [&]() noexcept -> std::expected<void, ServerError> {
                for (;;)
                {
                    auto progress = service_pending_once();
                    if (!progress)
                    {
                        return std::unexpected(progress.error());
                    }

                    if (!progress.value())
                    {
                        break;
                    }
                }

                return {};
            };

            const auto fail_all_pending = [&]() noexcept -> std::expected<void, ServerError> {
                for (auto& message : pending_replies)
                {
                    message.set_reply_status(core::status_unsuccessful);
                    message.set_reply_information(0);

                    if (auto released = release_message_buffers(message); !released)
                    {
                        return std::unexpected(released.error());
                    }

                    if (auto completed = complete_io_direct(message); !completed)
                    {
                        return std::unexpected(completed.error());
                    }
                }

                pending_replies.clear();
                update_pending_flag();
                return {};
            };

            // Publish the initial empty screen so a windowed host can paint immediately.
            maybe_publish_snapshot();

            bool exit_no_clients_requested = false;
            if (initial_packet != nullptr)
            {
                IoPacket packet_copy = *initial_packet;
                ConDrvApiMessage message(*comm, packet_copy);
                auto outcome = dispatch_message(state, message, host_io);
                if (!outcome)
                {
                    return std::unexpected(make_error(outcome.error().context, outcome.error().win32_error));
                }

                if (outcome->reply_pending)
                {
                    pending_replies.push_back(std::move(message));
                    update_pending_flag();
                }
                else
                {
                    if (auto finished = release_and_stage_completion(message); !finished)
                    {
                        return std::unexpected(finished.error());
                    }

                    maybe_publish_snapshot();
                    if (outcome->request_exit)
                    {
                        exit_no_clients_requested = true;
                    }
                }
            }

            bool exit_no_clients = false;
            bool exit_signal = false;
            bool exit_pipe = false;

            while (!stop_requested.load(std::memory_order_acquire) || pending_completion.has_value())
            {
                if (exit_no_clients_requested && !pending_completion.has_value())
                {
                    exit_no_clients = true;
                    break;
                }

                if (auto drained = service_pending_until_stalled(); !drained)
                {
                    return std::unexpected(drained.error());
                }
                maybe_publish_snapshot();

                IoPacket packet{};
                std::expected<void, DeviceCommError> read;
                {
                    AtomicFlagGuard guard(in_driver_read_io);
                    const IoComplete* reply = nullptr;
                    if (pending_completion.has_value())
                    {
                        reply = &pending_completion->completion();
                    }
                    read = comm->read_io(reply, packet);
                }

                if (!read)
                {
                    const DWORD error = read.error().win32_error;
                    if (error == ERROR_PIPE_NOT_CONNECTED)
                    {
                        exit_pipe = true;
                        break;
                    }

                    if (error == ERROR_OPERATION_ABORTED || error == ERROR_CANCELLED)
                    {
                        if (pending_completion.has_value())
                        {
                            // ReadIo submitted the completion as part of the input buffer before
                            // blocking for the next message. If the wait is canceled (to service
                            // reply-pending work), treat the completion as submitted and drop the
                            // staged message so we can continue making progress.
                            pending_completion.reset();
                        }

                        if (stop_requested.load(std::memory_order_acquire))
                        {
                            exit_signal = true;
                            break;
                        }

                        // The IO thread was canceled to service reply-pending operations.
                        continue;
                    }

                    return std::unexpected(make_error(read.error().context, error));
                }

                pending_completion.reset();

                ConDrvApiMessage message(*comm, packet);
                auto outcome = dispatch_message(state, message, host_io);
                if (!outcome)
                {
                    return std::unexpected(make_error(outcome.error().context, outcome.error().win32_error));
                }

                if (outcome->reply_pending)
                {
                    pending_replies.push_back(std::move(message));
                    update_pending_flag();
                    continue;
                }

                if (auto finished = release_and_stage_completion(message); !finished)
                {
                    return std::unexpected(finished.error());
                }

                maybe_publish_snapshot();
                if (outcome->request_exit)
                {
                    exit_no_clients_requested = true;
                }
            }

            if (pending_completion.has_value())
            {
                (void)complete_io_direct(*pending_completion);
                pending_completion.reset();
            }

            (void)fail_all_pending();

            if (exit_pipe)
            {
                logger.log(logging::LogLevel::info, L"ConDrv server disconnected (pipe not connected)");
            }
            else if (exit_no_clients)
            {
                logger.log(logging::LogLevel::info, L"ConDrv server exiting (no connected clients)");
            }
            else if (exit_signal || stop_requested.load(std::memory_order_acquire))
            {
                logger.log(logging::LogLevel::info, L"ConDrv server loop exiting (stop requested)");
            }
            else
            {
                logger.log(logging::LogLevel::info, L"ConDrv server loop exiting");
            }

            return DWORD{ 0 };
        }
    }

    ServerState::ServerState() noexcept :
        _input_code_page(::GetOEMCP()),
        _output_code_page(::GetOEMCP()),
        _font_size(COORD{ static_cast<SHORT>(8), static_cast<SHORT>(16) })
    {
        {
            // Keep a stable, deterministic default for font-related APIs even in headless mode.
            static constexpr wchar_t default_face_name[] = L"Consolas";
            static_assert((sizeof(default_face_name) / sizeof(default_face_name[0])) <= LF_FACESIZE,
                "Default font face name must fit within LF_FACESIZE");
            std::copy_n(
                default_face_name,
                sizeof(default_face_name) / sizeof(default_face_name[0]),
                _font_face_name.begin());
            _font_face_name.back() = L'\0';
        }

        auto settings = ScreenBuffer::default_settings();
        auto created = ScreenBuffer::create(settings);
        if (!created)
        {
            settings.buffer_size = {};
            settings.window_size = {};
            settings.maximum_window_size = {};
            settings.cursor_position = {};
            created = ScreenBuffer::create(settings);
        }

        if (created)
        {
            _main_screen_buffer = std::move(created.value());
            _active_screen_buffer = _main_screen_buffer;
        }
    }

    size_t ServerState::process_count() const noexcept
    {
        return _processes.size();
    }

    std::expected<std::unique_ptr<ProcessState>, DeviceCommError> ServerState::make_process_state(const DWORD pid, const DWORD tid) noexcept
    try
    {
        auto state = std::make_unique<ProcessState>();
        state->pid = pid;
        state->tid = tid;
        return state;
    }
    catch (...)
    {
        return std::unexpected(DeviceCommError{
            .context = L"Failed to allocate ProcessState",
            .win32_error = ERROR_OUTOFMEMORY,
        });
    }

    std::expected<std::unique_ptr<ObjectHandle>, DeviceCommError> ServerState::make_object_handle(ObjectHandle object) noexcept
    try
    {
        auto state = std::make_unique<ObjectHandle>();
        *state = object;
        return state;
    }
    catch (...)
    {
        return std::unexpected(DeviceCommError{
            .context = L"Failed to allocate ObjectHandle",
            .win32_error = ERROR_OUTOFMEMORY,
        });
    }

    std::expected<ConnectionInformation, DeviceCommError> ServerState::connect_client(
        const DWORD pid,
        const DWORD tid,
        const std::wstring_view app_name) noexcept
    {
        auto process_state = make_process_state(pid, tid);
        if (!process_state)
        {
            return std::unexpected(process_state.error());
        }

        auto process = std::move(process_state.value());
        const auto process_handle = reinterpret_cast<ULONG_PTR>(process.get());
        process->process_handle = process_handle;
        process->connect_sequence = _next_connect_sequence++;

        try
        {
            auto inserted = _processes.emplace(process_handle, std::move(process));
            if (!inserted.second)
            {
                return std::unexpected(DeviceCommError{
                    .context = L"Process handle already existed",
                    .win32_error = ERROR_ALREADY_EXISTS,
                });
            }
        }
        catch (...)
        {
            return std::unexpected(DeviceCommError{
                .context = L"Failed to insert process state",
                .win32_error = ERROR_OUTOFMEMORY,
            });
        }

        // Create initial input/output handles. In the upstream conhost these are
        // stored on the process record and used as the standard handles for the
        // connecting client.
        ObjectHandle input{};
        input.kind = ObjectKind::input;
        input.desired_access = GENERIC_READ | GENERIC_WRITE;
        input.share_mode = FILE_SHARE_READ | FILE_SHARE_WRITE;
        input.owning_process = process_handle;

        auto input_handle = create_object(input);
        if (!input_handle)
        {
            _processes.erase(process_handle);
            return std::unexpected(input_handle.error());
        }

        ObjectHandle output{};
        output.kind = ObjectKind::output;
        output.desired_access = GENERIC_READ | GENERIC_WRITE;
        output.share_mode = FILE_SHARE_READ | FILE_SHARE_WRITE;
        output.owning_process = process_handle;

        auto output_handle = create_object(output);
        if (!output_handle)
        {
            (void)close_object(input_handle.value());
            _processes.erase(process_handle);
            return std::unexpected(output_handle.error());
        }

        auto process_iter = _processes.find(process_handle);
        OC_ASSERT(process_iter != _processes.end());
        process_iter->second->input_handle = input_handle.value();
        process_iter->second->output_handle = output_handle.value();

        // Command history is best-effort: the CONNECT path should remain usable even if
        // history storage cannot be allocated. This mirrors the upstream behavior where
        // history allocation failures are caught and logged rather than failing the connect.
        _command_histories.allocate_for_process(
            app_name,
            process_handle,
            static_cast<size_t>(_history_buffer_count),
            static_cast<size_t>(_history_buffer_size));

        ConnectionInformation info{};
        info.process = process_handle;
        info.input = input_handle.value();
        info.output = output_handle.value();
        return info;
    }

    bool ServerState::disconnect_client(const ULONG_PTR process_handle) noexcept
    {
        auto iter = _processes.find(process_handle);
        if (iter == _processes.end())
        {
            return false;
        }

        _command_histories.free_for_process(process_handle);

        for (auto object_iter = _objects.begin(); object_iter != _objects.end();)
        {
            const auto& object = object_iter->second;
            if (object != nullptr && object->owning_process == process_handle)
            {
                object_iter = _objects.erase(object_iter);
                continue;
            }

            ++object_iter;
        }

        _processes.erase(iter);
        return true;
    }

    std::expected<ULONG_PTR, DeviceCommError> ServerState::create_object(ObjectHandle object) noexcept
    {
        if (object.kind == ObjectKind::output && !object.screen_buffer)
        {
            object.screen_buffer = _active_screen_buffer;
        }
        if (object.kind == ObjectKind::output && !object.screen_buffer)
        {
            return std::unexpected(DeviceCommError{
                .context = L"Output handle created without an active screen buffer",
                .win32_error = ERROR_INVALID_STATE,
            });
        }

        auto handle = make_object_handle(object);
        if (!handle)
        {
            return std::unexpected(handle.error());
        }

        const auto handle_id = reinterpret_cast<ULONG_PTR>(handle->get());
        try
        {
            auto inserted = _objects.emplace(handle_id, std::move(handle.value()));
            if (!inserted.second)
            {
                return std::unexpected(DeviceCommError{
                    .context = L"Object handle already existed",
                    .win32_error = ERROR_ALREADY_EXISTS,
                });
            }
        }
        catch (...)
        {
            return std::unexpected(DeviceCommError{
                .context = L"Failed to insert object handle",
                .win32_error = ERROR_OUTOFMEMORY,
            });
        }

        return handle_id;
    }

    bool ServerState::close_object(const ULONG_PTR handle_id) noexcept
    {
        auto iter = _objects.find(handle_id);
        if (iter == _objects.end())
        {
            return false;
        }

        _objects.erase(iter);
        return true;
    }

    bool ServerState::has_process(const ULONG_PTR process_handle) const noexcept
    {
        return _processes.find(process_handle) != _processes.end();
    }

    ObjectHandle* ServerState::find_object(const ULONG_PTR handle_id) noexcept
    {
        auto iter = _objects.find(handle_id);
        if (iter == _objects.end())
        {
            return nullptr;
        }
        return iter->second.get();
    }

    ULONG ServerState::input_mode() const noexcept
    {
        return _input_mode;
    }

    ULONG ServerState::output_mode() const noexcept
    {
        return _output_mode;
    }

    void ServerState::set_input_mode(const ULONG mode) noexcept
    {
        _input_mode = mode;
    }

    void ServerState::set_output_mode(const ULONG mode) noexcept
    {
        _output_mode = mode;
    }

    ULONG ServerState::input_code_page() const noexcept
    {
        return _input_code_page;
    }

    ULONG ServerState::output_code_page() const noexcept
    {
        return _output_code_page;
    }

    void ServerState::set_input_code_page(const ULONG code_page) noexcept
    {
        _input_code_page = code_page;
    }

    void ServerState::set_output_code_page(const ULONG code_page) noexcept
    {
        _output_code_page = code_page;
    }

    ULONG ServerState::font_index() const noexcept
    {
        return _font_index;
    }

    COORD ServerState::font_size() const noexcept
    {
        return _font_size;
    }

    void ServerState::fill_current_font(CONSOLE_CURRENTFONT_MSG& body) const noexcept
    {
        body.FontIndex = _font_index;
        body.FontSize = _font_size;
        body.FontFamily = _font_family;
        body.FontWeight = _font_weight;
        std::memcpy(body.FaceName, _font_face_name.data(), sizeof(body.FaceName));
    }

    void ServerState::apply_current_font(const CONSOLE_CURRENTFONT_MSG& body) noexcept
    {
        // The inbox host treats most of the legacy font APIs as deprecated, but
        // classic clients can still issue them. We keep a minimal state for
        // round-tripping and deterministic responses.
        _font_index = 0;

        if (body.FontSize.X > 0 && body.FontSize.Y > 0)
        {
            _font_size = body.FontSize;
        }

        if (body.FontFamily != 0)
        {
            _font_family = body.FontFamily;
        }

        if (body.FontWeight != 0)
        {
            _font_weight = body.FontWeight;
        }

        if (body.FaceName[0] != L'\0')
        {
            std::memcpy(_font_face_name.data(), body.FaceName, sizeof(body.FaceName));
            _font_face_name.back() = L'\0';
        }
    }

    void ServerState::set_cursor_mode(const bool blink, const bool db_enable) noexcept
    {
        _cursor_blink = blink;
        _cursor_db_enable = db_enable;
    }

    bool ServerState::cursor_blink() const noexcept
    {
        return _cursor_blink;
    }

    bool ServerState::cursor_db_enable() const noexcept
    {
        return _cursor_db_enable;
    }

    void ServerState::set_nls_mode(const ULONG mode) noexcept
    {
        _nls_mode = mode;
    }

    ULONG ServerState::nls_mode() const noexcept
    {
        return _nls_mode;
    }

    void ServerState::set_menu_close(const bool enable) noexcept
    {
        _menu_close = enable;
    }

    bool ServerState::menu_close() const noexcept
    {
        return _menu_close;
    }

    void ServerState::set_key_shortcuts(const bool enabled, const unsigned char reserved_keys) noexcept
    {
        _key_shortcuts_enabled = enabled;
        _reserved_keys = reserved_keys;
    }

    void ServerState::set_os2_registered(const bool registered) noexcept
    {
        _os2_registered = registered;
    }

    bool ServerState::os2_registered() const noexcept
    {
        return _os2_registered;
    }

    void ServerState::set_os2_oem_format(const bool enabled) noexcept
    {
        _os2_oem_format = enabled;
    }

    bool ServerState::os2_oem_format() const noexcept
    {
        return _os2_oem_format;
    }

    ULONG ServerState::history_buffer_size() const noexcept
    {
        return _history_buffer_size;
    }

    ULONG ServerState::history_buffer_count() const noexcept
    {
        return _history_buffer_count;
    }

    ULONG ServerState::history_flags() const noexcept
    {
        return _history_flags;
    }

    void ServerState::set_history_info(const ULONG buffer_size, const ULONG buffer_count, const ULONG flags) noexcept
    {
        _history_buffer_size = buffer_size;
        _history_buffer_count = buffer_count;
        _history_flags = flags;

        // Global history-buffer-size changes apply to all histories (allocated or cached).
        _command_histories.resize_all(static_cast<size_t>(buffer_size));
    }

    CommandHistory* ServerState::try_command_history_for_process(const ULONG_PTR process_handle) noexcept
    {
        return _command_histories.find_by_process(process_handle);
    }

    CommandHistory* ServerState::try_command_history_for_exe(const std::wstring_view exe_name) noexcept
    {
        return _command_histories.find_by_exe(exe_name);
    }

    const CommandHistory* ServerState::try_command_history_for_exe(const std::wstring_view exe_name) const noexcept
    {
        return _command_histories.find_by_exe(exe_name);
    }

    void ServerState::add_command_history_for_process(
        const ULONG_PTR process_handle,
        const std::wstring_view command,
        const bool suppress_duplicates) noexcept
    {
        auto* history = _command_histories.find_by_process(process_handle);
        if (history != nullptr)
        {
            history->add(command, suppress_duplicates);
        }
    }

    void ServerState::expunge_command_history(const std::wstring_view exe_name) noexcept
    {
        _command_histories.expunge_by_exe(exe_name);
    }

    void ServerState::set_command_history_number_of_commands(const std::wstring_view exe_name, const size_t max_commands) noexcept
    {
        _command_histories.set_number_of_commands_by_exe(exe_name, max_commands);
    }

    std::shared_ptr<ScreenBuffer> ServerState::active_screen_buffer() const noexcept
    {
        return _active_screen_buffer;
    }

    bool ServerState::set_active_screen_buffer(std::shared_ptr<ScreenBuffer> buffer) noexcept
    {
        if (!buffer)
        {
            return false;
        }

        _active_screen_buffer = std::move(buffer);
        if (!_main_screen_buffer)
        {
            _main_screen_buffer = _active_screen_buffer;
        }
        return true;
    }

    std::expected<std::shared_ptr<ScreenBuffer>, DeviceCommError> ServerState::create_screen_buffer_like_active() noexcept
    {
        if (!_active_screen_buffer)
        {
            return std::unexpected(DeviceCommError{
                .context = L"No active screen buffer",
                .win32_error = ERROR_INVALID_STATE,
            });
        }

        return ScreenBuffer::create_blank_like(*_active_screen_buffer);
    }

    ScreenBuffer::Settings ScreenBuffer::default_settings() noexcept
    {
        Settings settings{};
        settings.buffer_size = COORD{ 120, 40 };
        settings.cursor_position = COORD{ 0, 0 };
        settings.scroll_position = COORD{ 0, 0 };
        settings.window_size = settings.buffer_size;
        settings.maximum_window_size = settings.buffer_size;
        settings.text_attributes = 0x07;
        settings.cursor_size = 25;
        settings.cursor_visible = true;

        // Default color table values match the legacy Windows console palette.
        settings.color_table = {
            RGB(0, 0, 0),
            RGB(0, 0, 128),
            RGB(0, 128, 0),
            RGB(0, 128, 128),
            RGB(128, 0, 0),
            RGB(128, 0, 128),
            RGB(128, 128, 0),
            RGB(192, 192, 192),
            RGB(128, 128, 128),
            RGB(0, 0, 255),
            RGB(0, 255, 0),
            RGB(0, 255, 255),
            RGB(255, 0, 0),
            RGB(255, 0, 255),
            RGB(255, 255, 0),
            RGB(255, 255, 255),
        };

        return settings;
    }

    std::expected<std::shared_ptr<ScreenBuffer>, DeviceCommError> ScreenBuffer::create(Settings settings) noexcept
    try
    {
        return std::shared_ptr<ScreenBuffer>(new ScreenBuffer(std::move(settings)));
    }
    catch (...)
    {
        return std::unexpected(DeviceCommError{
            .context = L"Failed to allocate ScreenBuffer",
            .win32_error = ERROR_OUTOFMEMORY,
        });
    }

    std::expected<std::shared_ptr<ScreenBuffer>, DeviceCommError> ScreenBuffer::create_blank_like(
        const ScreenBuffer& template_buffer) noexcept
    {
        Settings settings{};
        settings.buffer_size = template_buffer.screen_buffer_size();
        settings.cursor_position = COORD{ 0, 0 };
        settings.scroll_position = template_buffer.scroll_position();
        settings.window_size = template_buffer.window_size();
        settings.maximum_window_size = template_buffer.maximum_window_size();
        settings.text_attributes = template_buffer.text_attributes();
        settings.cursor_size = template_buffer.cursor_size();
        settings.cursor_visible = template_buffer.cursor_visible();
        settings.color_table = template_buffer.color_table();
        return ScreenBuffer::create(std::move(settings));
    }

    ScreenBuffer::ScreenBuffer(Settings settings) :
        _buffer_size(settings.buffer_size),
        _cursor_position(settings.cursor_position),
        _maximum_window_size(settings.maximum_window_size),
        _text_attributes(settings.text_attributes),
        _default_text_attributes(settings.text_attributes),
        _cursor_size(settings.cursor_size),
        _cursor_visible(settings.cursor_visible),
        _color_table(settings.color_table)
    {
        if (_buffer_size.X <= 0 || _buffer_size.Y <= 0)
        {
            _buffer_size = {};
            _cursor_position = {};
            _window_rect = {};
            _maximum_window_size = {};
            return;
        }

        COORD desired_window_size = settings.window_size;
        if (desired_window_size.X <= 0 || desired_window_size.Y <= 0)
        {
            desired_window_size = _buffer_size;
        }
        if (_maximum_window_size.X <= 0 || _maximum_window_size.Y <= 0)
        {
            _maximum_window_size = _buffer_size;
        }

        const COORD desired_scroll = settings.scroll_position;
        const long origin_x = desired_scroll.X < 0 ? 0 : static_cast<long>(desired_scroll.X);
        const long origin_y = desired_scroll.Y < 0 ? 0 : static_cast<long>(desired_scroll.Y);

        if (_maximum_window_size.X < _buffer_size.X)
        {
            _maximum_window_size.X = _buffer_size.X;
        }
        if (_maximum_window_size.Y < _buffer_size.Y)
        {
            _maximum_window_size.Y = _buffer_size.Y;
        }

        if (_cursor_position.X < 0)
        {
            _cursor_position.X = 0;
        }
        if (_cursor_position.Y < 0)
        {
            _cursor_position.Y = 0;
        }
        if (_cursor_position.X >= _buffer_size.X)
        {
            _cursor_position.X = static_cast<SHORT>(_buffer_size.X - 1);
        }
        if (_cursor_position.Y >= _buffer_size.Y)
        {
            _cursor_position.Y = static_cast<SHORT>(_buffer_size.Y - 1);
        }

        // Initialize the viewport/window rectangle. In ConDrv the window is expressed as an
        // inclusive SMALL_RECT within the screen buffer. We keep it as primary state and
        // derive `ScrollPosition` and `CurrentWindowSize` from it when answering queries.
        //
        // Any invalid window parameters are clamped to a sensible default (the full buffer).
        const long buffer_w = static_cast<long>(_buffer_size.X);
        const long buffer_h = static_cast<long>(_buffer_size.Y);

        long window_w = desired_window_size.X <= 0 ? buffer_w : static_cast<long>(desired_window_size.X);
        long window_h = desired_window_size.Y <= 0 ? buffer_h : static_cast<long>(desired_window_size.Y);
        if (window_w > buffer_w)
        {
            window_w = buffer_w;
        }
        if (window_h > buffer_h)
        {
            window_h = buffer_h;
        }
        if (window_w <= 0)
        {
            window_w = 1;
        }
        if (window_h <= 0)
        {
            window_h = 1;
        }

        long left = origin_x;
        long top = origin_y;

        // Clamp origin so the computed rect fits within the buffer.
        if (left > buffer_w - window_w)
        {
            left = buffer_w - window_w;
        }
        if (top > buffer_h - window_h)
        {
            top = buffer_h - window_h;
        }
        if (left < 0)
        {
            left = 0;
        }
        if (top < 0)
        {
            top = 0;
        }

        const long right = left + window_w - 1;
        const long bottom = top + window_h - 1;

        _window_rect.Left = static_cast<SHORT>(left);
        _window_rect.Top = static_cast<SHORT>(top);
        _window_rect.Right = static_cast<SHORT>(right);
        _window_rect.Bottom = static_cast<SHORT>(bottom);

        const auto width = static_cast<size_t>(_buffer_size.X);
        const auto height = static_cast<size_t>(_buffer_size.Y);
        _cells.assign(width * height, ScreenCell{ .character = L' ', .attributes = _text_attributes });
    }

    COORD ScreenBuffer::screen_buffer_size() const noexcept
    {
        return _buffer_size;
    }

    bool ScreenBuffer::coord_in_range(const COORD coord) const noexcept
    {
        if (_cells.empty())
        {
            return false;
        }

        if (_buffer_size.X <= 0 || _buffer_size.Y <= 0)
        {
            return false;
        }

        if (coord.X < 0 || coord.Y < 0)
        {
            return false;
        }

        return coord.X < _buffer_size.X && coord.Y < _buffer_size.Y;
    }

    size_t ScreenBuffer::linear_index(const COORD coord) const noexcept
    {
        return static_cast<size_t>(coord.Y) * static_cast<size_t>(_buffer_size.X) + static_cast<size_t>(coord.X);
    }

    bool ScreenBuffer::set_screen_buffer_size(const COORD size) noexcept
    {
        if (size.X <= 0 || size.Y <= 0)
        {
            return false;
        }

        const size_t new_width = static_cast<size_t>(size.X);
        const size_t new_height = static_cast<size_t>(size.Y);

        const size_t old_width = _buffer_size.X > 0 ? static_cast<size_t>(_buffer_size.X) : 0;
        const size_t old_height = _buffer_size.Y > 0 ? static_cast<size_t>(_buffer_size.Y) : 0;

        std::vector<ScreenCell> new_cells;
        std::optional<std::vector<ScreenCell>> new_backup_cells;
        try
        {
            new_cells.assign(new_width * new_height, ScreenCell{ .character = L' ', .attributes = _text_attributes });

            if (!_cells.empty() && old_width != 0 && old_height != 0)
            {
                const size_t copy_width = std::min(old_width, new_width);
                const size_t copy_height = std::min(old_height, new_height);

                for (size_t y = 0; y < copy_height; ++y)
                {
                    for (size_t x = 0; x < copy_width; ++x)
                    {
                        new_cells[y * new_width + x] = _cells[y * old_width + x];
                    }
                }
            }

            if (_vt_main_backup)
            {
                std::vector<ScreenCell> resized;
                resized.assign(
                    new_width * new_height,
                    ScreenCell{ .character = L' ', .attributes = _vt_main_backup->text_attributes });

                const auto& old_backup = _vt_main_backup->cells;
                if (!old_backup.empty() && old_width != 0 && old_height != 0)
                {
                    const size_t copy_width = std::min(old_width, new_width);
                    const size_t copy_height = std::min(old_height, new_height);

                    for (size_t y = 0; y < copy_height; ++y)
                    {
                        for (size_t x = 0; x < copy_width; ++x)
                        {
                            resized[y * new_width + x] = old_backup[y * old_width + x];
                        }
                    }
                }

                new_backup_cells.emplace(std::move(resized));
            }
        }
        catch (...)
        {
            return false;
        }

        _cells = std::move(new_cells);
        if (_vt_main_backup && new_backup_cells)
        {
            _vt_main_backup->cells = std::move(*new_backup_cells);
        }

        _buffer_size = size;

        if (_maximum_window_size.X < _buffer_size.X)
        {
            _maximum_window_size.X = _buffer_size.X;
        }
        if (_maximum_window_size.Y < _buffer_size.Y)
        {
            _maximum_window_size.Y = _buffer_size.Y;
        }

        {
            const long buffer_w = static_cast<long>(_buffer_size.X);
            const long buffer_h = static_cast<long>(_buffer_size.Y);

            long window_w = static_cast<long>(_window_rect.Right) - static_cast<long>(_window_rect.Left) + 1;
            long window_h = static_cast<long>(_window_rect.Bottom) - static_cast<long>(_window_rect.Top) + 1;
            if (window_w <= 0 || window_h <= 0)
            {
                window_w = buffer_w;
                window_h = buffer_h;
            }

            if (window_w > buffer_w)
            {
                window_w = buffer_w;
            }
            if (window_h > buffer_h)
            {
                window_h = buffer_h;
            }
            if (window_w <= 0)
            {
                window_w = 1;
            }
            if (window_h <= 0)
            {
                window_h = 1;
            }

            long left = static_cast<long>(_window_rect.Left);
            long top = static_cast<long>(_window_rect.Top);

            if (left < 0)
            {
                left = 0;
            }
            if (top < 0)
            {
                top = 0;
            }

            const long max_left = buffer_w - window_w;
            const long max_top = buffer_h - window_h;

            if (left > max_left)
            {
                left = max_left;
            }
            if (top > max_top)
            {
                top = max_top;
            }
            if (left < 0)
            {
                left = 0;
            }
            if (top < 0)
            {
                top = 0;
            }

            const long right = left + window_w - 1;
            const long bottom = top + window_h - 1;

            _window_rect.Left = static_cast<SHORT>(left);
            _window_rect.Top = static_cast<SHORT>(top);
            _window_rect.Right = static_cast<SHORT>(right);
            _window_rect.Bottom = static_cast<SHORT>(bottom);
        }

        if (_cursor_position.X >= _buffer_size.X)
        {
            _cursor_position.X = static_cast<SHORT>(_buffer_size.X > 0 ? _buffer_size.X - 1 : 0);
        }
        if (_cursor_position.Y >= _buffer_size.Y)
        {
            _cursor_position.Y = static_cast<SHORT>(_buffer_size.Y > 0 ? _buffer_size.Y - 1 : 0);
        }

        if (_vt_vertical_margins)
        {
            auto margins = *_vt_vertical_margins;
            if (margins.top < 0)
            {
                margins.top = 0;
            }
            if (margins.bottom < 0)
            {
                _vt_vertical_margins.reset();
            }
            else
            {
                if (margins.top >= _buffer_size.Y)
                {
                    margins.top = static_cast<SHORT>(_buffer_size.Y - 1);
                }
                if (margins.bottom >= _buffer_size.Y)
                {
                    margins.bottom = static_cast<SHORT>(_buffer_size.Y - 1);
                }

                if (margins.top >= margins.bottom)
                {
                    _vt_vertical_margins.reset();
                }
                else
                {
                    _vt_vertical_margins = margins;
                }
            }
        }

        if (_vt_main_backup)
        {
            if (_vt_main_backup->cursor_position.X >= _buffer_size.X)
            {
                _vt_main_backup->cursor_position.X = static_cast<SHORT>(_buffer_size.X > 0 ? _buffer_size.X - 1 : 0);
            }
            if (_vt_main_backup->cursor_position.Y >= _buffer_size.Y)
            {
                _vt_main_backup->cursor_position.Y = static_cast<SHORT>(_buffer_size.Y > 0 ? _buffer_size.Y - 1 : 0);
            }

            if (_vt_main_backup->vt_vertical_margins)
            {
                auto margins = *_vt_main_backup->vt_vertical_margins;
                if (margins.top < 0)
                {
                    margins.top = 0;
                }
                if (margins.bottom < 0)
                {
                    _vt_main_backup->vt_vertical_margins.reset();
                }
                else
                {
                    if (margins.top >= _buffer_size.Y)
                    {
                        margins.top = static_cast<SHORT>(_buffer_size.Y - 1);
                    }
                    if (margins.bottom >= _buffer_size.Y)
                    {
                        margins.bottom = static_cast<SHORT>(_buffer_size.Y - 1);
                    }

                    if (margins.top >= margins.bottom)
                    {
                        _vt_main_backup->vt_vertical_margins.reset();
                    }
                    else
                    {
                        _vt_main_backup->vt_vertical_margins = margins;
                    }
                }
            }
        }

        // Resizing changes the end-of-line location and invalidates any delayed wrap state.
        _vt_delayed_wrap_position.reset();
        if (_vt_main_backup)
        {
            _vt_main_backup->vt_delayed_wrap_position.reset();
        }

        touch();
        snap_window_to_cursor();
        return true;
    }

    COORD ScreenBuffer::cursor_position() const noexcept
    {
        return _cursor_position;
    }

    void ScreenBuffer::set_cursor_position(const COORD position) noexcept
    {
        _cursor_position = position;
        touch();
    }

    SMALL_RECT ScreenBuffer::window_rect() const noexcept
    {
        return _window_rect;
    }

    COORD ScreenBuffer::scroll_position() const noexcept
    {
        return COORD{ _window_rect.Left, _window_rect.Top };
    }

    bool ScreenBuffer::set_window_rect(const SMALL_RECT rect) noexcept
    {
        if (_buffer_size.X <= 0 || _buffer_size.Y <= 0)
        {
            return false;
        }

        if (rect.Left > rect.Right || rect.Top > rect.Bottom)
        {
            return false;
        }

        const long buffer_w = static_cast<long>(_buffer_size.X);
        const long buffer_h = static_cast<long>(_buffer_size.Y);

        const long left = rect.Left;
        const long top = rect.Top;
        const long right = rect.Right;
        const long bottom = rect.Bottom;

        if (left < 0 || top < 0)
        {
            return false;
        }
        if (right < 0 || bottom < 0)
        {
            return false;
        }
        if (right >= buffer_w || bottom >= buffer_h)
        {
            return false;
        }

        _window_rect = rect;
        touch();
        return true;
    }

    COORD ScreenBuffer::window_size() const noexcept
    {
        const long width = static_cast<long>(_window_rect.Right) - static_cast<long>(_window_rect.Left) + 1;
        const long height = static_cast<long>(_window_rect.Bottom) - static_cast<long>(_window_rect.Top) + 1;
        if (width <= 0 || height <= 0)
        {
            return {};
        }

        COORD size{};
        size.X = width > std::numeric_limits<SHORT>::max() ? std::numeric_limits<SHORT>::max() : static_cast<SHORT>(width);
        size.Y = height > std::numeric_limits<SHORT>::max() ? std::numeric_limits<SHORT>::max() : static_cast<SHORT>(height);
        return size;
    }

    bool ScreenBuffer::set_window_size(const COORD size) noexcept
    {
        if (_buffer_size.X <= 0 || _buffer_size.Y <= 0)
        {
            return false;
        }

        if (size.X <= 0 || size.Y <= 0)
        {
            return false;
        }

        const long width = static_cast<long>(size.X);
        const long height = static_cast<long>(size.Y);
        const long buffer_w = static_cast<long>(_buffer_size.X);
        const long buffer_h = static_cast<long>(_buffer_size.Y);
        if (width > buffer_w || height > buffer_h)
        {
            return false;
        }

        const long left = static_cast<long>(_window_rect.Left);
        const long top = static_cast<long>(_window_rect.Top);
        const long right = left + width - 1;
        const long bottom = top + height - 1;

        if (left < 0 || top < 0)
        {
            return false;
        }
        if (right >= buffer_w || bottom >= buffer_h)
        {
            return false;
        }

        _window_rect.Left = static_cast<SHORT>(left);
        _window_rect.Top = static_cast<SHORT>(top);
        _window_rect.Right = static_cast<SHORT>(right);
        _window_rect.Bottom = static_cast<SHORT>(bottom);
        touch();
        return true;
    }

    void ScreenBuffer::snap_window_to_cursor() noexcept
    {
        if (_buffer_size.X <= 0 || _buffer_size.Y <= 0)
        {
            return;
        }

        const auto size = window_size();
        if (size.X <= 0 || size.Y <= 0)
        {
            return;
        }

        const long width = static_cast<long>(size.X);
        const long height = static_cast<long>(size.Y);
        const long buffer_w = static_cast<long>(_buffer_size.X);
        const long buffer_h = static_cast<long>(_buffer_size.Y);

        long left = static_cast<long>(_window_rect.Left);
        long top = static_cast<long>(_window_rect.Top);
        long right = static_cast<long>(_window_rect.Right);
        long bottom = static_cast<long>(_window_rect.Bottom);

        const long cursor_x = static_cast<long>(_cursor_position.X);
        const long cursor_y = static_cast<long>(_cursor_position.Y);

        if (cursor_x < left)
        {
            left = cursor_x;
        }
        else if (cursor_x > right)
        {
            left = cursor_x - (width - 1);
        }

        if (cursor_y < top)
        {
            top = cursor_y;
        }
        else if (cursor_y > bottom)
        {
            top = cursor_y - (height - 1);
        }

        if (left < 0)
        {
            left = 0;
        }
        if (top < 0)
        {
            top = 0;
        }

        const long max_left = buffer_w - width;
        const long max_top = buffer_h - height;
        if (left > max_left)
        {
            left = max_left;
        }
        if (top > max_top)
        {
            top = max_top;
        }

        right = left + width - 1;
        bottom = top + height - 1;

        _window_rect.Left = static_cast<SHORT>(left);
        _window_rect.Top = static_cast<SHORT>(top);
        _window_rect.Right = static_cast<SHORT>(right);
        _window_rect.Bottom = static_cast<SHORT>(bottom);
        touch();
    }

    COORD ScreenBuffer::maximum_window_size() const noexcept
    {
        return _maximum_window_size;
    }

    USHORT ScreenBuffer::text_attributes() const noexcept
    {
        return _text_attributes;
    }

    USHORT ScreenBuffer::default_text_attributes() const noexcept
    {
        return _default_text_attributes;
    }

    void ScreenBuffer::set_text_attributes(const USHORT attributes) noexcept
    {
        _text_attributes = attributes;
        touch();
    }

    void ScreenBuffer::set_default_text_attributes(const USHORT attributes) noexcept
    {
        _default_text_attributes = attributes;
        touch();
    }

    ULONG ScreenBuffer::cursor_size() const noexcept
    {
        return _cursor_size;
    }

    bool ScreenBuffer::cursor_visible() const noexcept
    {
        return _cursor_visible;
    }

    void ScreenBuffer::set_cursor_info(const ULONG size, const bool visible) noexcept
    {
        _cursor_size = size;
        _cursor_visible = visible;
        touch();
    }

    void ScreenBuffer::save_cursor_state(
        const COORD position,
        const USHORT attributes,
        const bool delayed_eol_wrap,
        const bool origin_mode_enabled) noexcept
    {
        touch();
        if (_buffer_size.X <= 0 || _buffer_size.Y <= 0)
        {
            _saved_cursor_state = SavedCursorState{
                .position = COORD{ 0, 0 },
                .attributes = attributes,
                .delayed_eol_wrap = false,
                .origin_mode_enabled = origin_mode_enabled,
            };
            return;
        }

        const auto clamp_component = [](const SHORT value, const SHORT max_value) noexcept -> SHORT {
            if (value < 0)
            {
                return 0;
            }
            if (value > max_value)
            {
                return max_value;
            }
            return value;
        };

        const SHORT max_x = static_cast<SHORT>(_buffer_size.X - 1);
        const SHORT max_y = static_cast<SHORT>(_buffer_size.Y - 1);
        COORD clamped{};
        clamped.X = clamp_component(position.X, max_x);
        clamped.Y = clamp_component(position.Y, max_y);

        _saved_cursor_state = SavedCursorState{
            .position = clamped,
            .attributes = attributes,
            .delayed_eol_wrap = delayed_eol_wrap && clamped.X == position.X && clamped.Y == position.Y,
            .origin_mode_enabled = origin_mode_enabled,
        };
    }

    bool ScreenBuffer::restore_cursor_state(
        COORD& position,
        USHORT& attributes,
        bool& delayed_eol_wrap,
        bool& origin_mode_enabled) const noexcept
    {
        if (!_saved_cursor_state)
        {
            return false;
        }

        position = _saved_cursor_state->position;
        attributes = _saved_cursor_state->attributes;
        delayed_eol_wrap = _saved_cursor_state->delayed_eol_wrap;
        origin_mode_enabled = _saved_cursor_state->origin_mode_enabled;

        if (_buffer_size.X <= 0 || _buffer_size.Y <= 0)
        {
            position = COORD{ 0, 0 };
            return true;
        }

        if (position.X < 0)
        {
            position.X = 0;
        }
        else if (position.X >= _buffer_size.X)
        {
            position.X = static_cast<SHORT>(_buffer_size.X - 1);
        }

        if (position.Y < 0)
        {
            position.Y = 0;
        }
        else if (position.Y >= _buffer_size.Y)
        {
            position.Y = static_cast<SHORT>(_buffer_size.Y - 1);
        }

        return true;
    }

    void ScreenBuffer::set_vt_autowrap_enabled(const bool enabled) noexcept
    {
        _vt_autowrap_enabled = enabled;
        _vt_delayed_wrap_position.reset();
        touch();
    }

    void ScreenBuffer::set_vt_delayed_wrap_position(std::optional<COORD> position) noexcept
    {
        _vt_delayed_wrap_position = position;
    }

    void ScreenBuffer::set_vt_origin_mode_enabled(const bool enabled) noexcept
    {
        _vt_origin_mode_enabled = enabled;
        touch();
    }

    void ScreenBuffer::set_vt_insert_mode_enabled(const bool enabled) noexcept
    {
        if (_vt_insert_mode_enabled == enabled)
        {
            return;
        }

        _vt_insert_mode_enabled = enabled;
        touch();
    }

    const std::array<COLORREF, 16>& ScreenBuffer::color_table() const noexcept
    {
        return _color_table;
    }

    void ScreenBuffer::set_color_table(const COLORREF (&table)[16]) noexcept
    {
        for (size_t i = 0; i < _color_table.size(); ++i)
        {
            _color_table[i] = table[i];
        }
        touch();
    }

    std::optional<ScreenBuffer::VtVerticalMargins> ScreenBuffer::vt_vertical_margins() const noexcept
    {
        return _vt_vertical_margins;
    }

    void ScreenBuffer::set_vt_vertical_margins(std::optional<VtVerticalMargins> margins) noexcept
    {
        if (margins)
        {
            OC_ASSERT(margins->top >= 0);
            OC_ASSERT(margins->bottom >= margins->top);
            OC_ASSERT(_buffer_size.Y > 0);
            OC_ASSERT(margins->bottom < _buffer_size.Y);
        }

        _vt_vertical_margins = margins;
        touch();
    }

    bool ScreenBuffer::set_vt_using_alternate_screen_buffer(
        const bool enable,
        const wchar_t fill_character,
        const USHORT fill_attributes) noexcept
    {
        if (enable)
        {
            if (_vt_main_backup)
            {
                return true;
            }

            std::vector<ScreenCell> alt_cells;
            try
            {
                alt_cells.assign(_cells.size(), ScreenCell{ .character = fill_character, .attributes = fill_attributes });
            }
            catch (...)
            {
                return false;
            }

            VtAlternateBufferBackup backup{};
            backup.cells = std::move(_cells);
            backup.cursor_position = _cursor_position;
            backup.text_attributes = _text_attributes;
            backup.default_text_attributes = _default_text_attributes;
            backup.cursor_size = _cursor_size;
            backup.cursor_visible = _cursor_visible;
            backup.saved_cursor_state = _saved_cursor_state;
            backup.vt_vertical_margins = _vt_vertical_margins;
            backup.vt_delayed_wrap_position = _vt_delayed_wrap_position;
            backup.vt_origin_mode_enabled = _vt_origin_mode_enabled;

            _vt_main_backup = std::move(backup);

            _cells = std::move(alt_cells);
            _cursor_position = COORD{ 0, 0 };
            _saved_cursor_state.reset();
            _vt_vertical_margins.reset();
            _vt_delayed_wrap_position.reset();
            touch();
            return true;
        }

        if (!_vt_main_backup)
        {
            return true;
        }

        auto backup = std::move(*_vt_main_backup);
        _vt_main_backup.reset();

        _cells = std::move(backup.cells);
        _cursor_position = backup.cursor_position;
        _text_attributes = backup.text_attributes;
        _default_text_attributes = backup.default_text_attributes;
        _cursor_size = backup.cursor_size;
        _cursor_visible = backup.cursor_visible;
        _saved_cursor_state = backup.saved_cursor_state;
        _vt_vertical_margins = backup.vt_vertical_margins;
        _vt_delayed_wrap_position = backup.vt_delayed_wrap_position;
        _vt_origin_mode_enabled = backup.vt_origin_mode_enabled;
        touch();
        return true;
    }

    std::wstring_view ServerState::title(const bool original) const noexcept
    {
        if (original)
        {
            return _original_title;
        }

        return _title;
    }

    bool ServerState::set_title(std::wstring title) noexcept
    {
        try
        {
            if (_original_title.empty())
            {
                _original_title = title;
            }
            _title = std::move(title);
            return true;
        }
        catch (...)
        {
            return false;
        }
    }

    bool ServerState::set_title(const std::wstring_view title) noexcept
    {
        try
        {
            if (_original_title.empty())
            {
                _original_title.assign(title);
            }
            _title.assign(title);
            return true;
        }
        catch (...)
        {
            return false;
        }
    }

    std::expected<void, DeviceCommError> ServerState::set_alias(
        std::wstring exe_name,
        std::wstring source,
        std::wstring target) noexcept
    try
    {
        if (source.empty())
        {
            return std::unexpected(DeviceCommError{
                .context = L"Console alias source was empty",
                .win32_error = ERROR_INVALID_PARAMETER,
            });
        }

        if (target.empty())
        {
            const auto exe_iter = _aliases.find(exe_name);
            if (exe_iter == _aliases.end())
            {
                return {};
            }

            auto& table = exe_iter->second;
            const auto source_iter = table.find(source);
            if (source_iter != table.end())
            {
                table.erase(source_iter);
                if (table.empty())
                {
                    _aliases.erase(exe_iter);
                }
            }

            return {};
        }

        auto [exe_iter, inserted] = _aliases.try_emplace(std::move(exe_name));
        (void)inserted;

        auto& table = exe_iter->second;
        table.insert_or_assign(std::move(source), std::move(target));
        return {};
    }
    catch (...)
    {
        return std::unexpected(DeviceCommError{
            .context = L"Failed to store console alias data",
            .win32_error = ERROR_OUTOFMEMORY,
        });
    }

    std::optional<std::wstring_view> ServerState::try_get_alias(
        const std::wstring_view exe_name,
        const std::wstring_view source) const noexcept
    {
        const auto exe_iter = _aliases.find(exe_name);
        if (exe_iter == _aliases.end())
        {
            return std::nullopt;
        }

        const auto& table = exe_iter->second;
        const auto source_iter = table.find(source);
        if (source_iter == table.end())
        {
            return std::nullopt;
        }

        if (source_iter->second.empty())
        {
            return std::nullopt;
        }

        return std::wstring_view(source_iter->second);
    }

    bool ScreenBuffer::write_cell(const COORD coord, const wchar_t character, const USHORT attributes) noexcept
    {
        if (!coord_in_range(coord))
        {
            return false;
        }

        const size_t index = linear_index(coord);
        _cells[index].character = character;
        _cells[index].attributes = attributes;
        touch();
        return true;
    }

    bool ScreenBuffer::insert_cell(const COORD coord, const wchar_t character, const USHORT attributes) noexcept
    {
        if (!coord_in_range(coord))
        {
            return false;
        }

        if (_buffer_size.X <= 1)
        {
            return write_cell(coord, character, attributes);
        }

        const size_t width = static_cast<size_t>(_buffer_size.X);
        const size_t row = static_cast<size_t>(coord.Y);
        const size_t column = static_cast<size_t>(coord.X);
        const size_t base = row * width;
        const size_t start = base + column;

        auto begin = _cells.begin();

        // Shift the remainder of the current line right by one cell and drop the final cell.
        std::move_backward(
            begin + start,
            begin + (base + width - 1),
            begin + (base + width));

        _cells[start] = ScreenCell{ .character = character, .attributes = attributes };
        touch();
        return true;
    }

    size_t ScreenBuffer::fill_output_characters(const COORD origin, const wchar_t value, const size_t length) noexcept
    {
        if (!coord_in_range(origin) || length == 0)
        {
            return 0;
        }

        touch();
        size_t index = linear_index(origin);
        size_t written = 0;
        while (written < length && index < _cells.size())
        {
            _cells[index].character = value;
            ++index;
            ++written;
        }
        return written;
    }

    size_t ScreenBuffer::fill_output_attributes(const COORD origin, const USHORT value, const size_t length) noexcept
    {
        if (!coord_in_range(origin) || length == 0)
        {
            return 0;
        }

        touch();
        size_t index = linear_index(origin);
        size_t written = 0;
        while (written < length && index < _cells.size())
        {
            _cells[index].attributes = value;
            ++index;
            ++written;
        }
        return written;
    }

    size_t ScreenBuffer::write_output_characters(const COORD origin, const std::span<const wchar_t> text) noexcept
    {
        if (!coord_in_range(origin) || text.empty())
        {
            return 0;
        }

        touch();
        size_t index = linear_index(origin);
        size_t written = 0;
        while (written < text.size() && index < _cells.size())
        {
            _cells[index].character = text[written];
            ++index;
            ++written;
        }
        return written;
    }

    size_t ScreenBuffer::write_output_attributes(const COORD origin, const std::span<const USHORT> attributes) noexcept
    {
        if (!coord_in_range(origin) || attributes.empty())
        {
            return 0;
        }

        touch();
        size_t index = linear_index(origin);
        size_t written = 0;
        while (written < attributes.size() && index < _cells.size())
        {
            _cells[index].attributes = attributes[written];
            ++index;
            ++written;
        }
        return written;
    }

    size_t ScreenBuffer::write_output_ascii(const COORD origin, const std::span<const std::byte> bytes) noexcept
    {
        if (!coord_in_range(origin) || bytes.empty())
        {
            return 0;
        }

        touch();
        size_t index = linear_index(origin);
        size_t written = 0;
        while (written < bytes.size() && index < _cells.size())
        {
            const auto value = static_cast<unsigned char>(bytes[written]);
            _cells[index].character = static_cast<wchar_t>(value);
            ++index;
            ++written;
        }
        return written;
    }

    size_t ScreenBuffer::read_output_characters(const COORD origin, const std::span<wchar_t> dest) const noexcept
    {
        if (!coord_in_range(origin) || dest.empty())
        {
            return 0;
        }

        size_t index = linear_index(origin);
        size_t read = 0;
        while (read < dest.size() && index < _cells.size())
        {
            dest[read] = _cells[index].character;
            ++index;
            ++read;
        }
        return read;
    }

    size_t ScreenBuffer::read_output_attributes(const COORD origin, const std::span<USHORT> dest) const noexcept
    {
        if (!coord_in_range(origin) || dest.empty())
        {
            return 0;
        }

        size_t index = linear_index(origin);
        size_t read = 0;
        while (read < dest.size() && index < _cells.size())
        {
            dest[read] = _cells[index].attributes;
            ++index;
            ++read;
        }
        return read;
    }

    size_t ScreenBuffer::read_output_ascii(const COORD origin, const std::span<std::byte> dest) const noexcept
    {
        if (!coord_in_range(origin) || dest.empty())
        {
            return 0;
        }

        size_t index = linear_index(origin);
        size_t read = 0;
        while (read < dest.size() && index < _cells.size())
        {
            const wchar_t value = _cells[index].character;
            const unsigned char narrowed = value <= 0xFF ? static_cast<unsigned char>(value) : static_cast<unsigned char>('?');
            dest[read] = static_cast<std::byte>(narrowed);
            ++index;
            ++read;
        }
        return read;
    }

    size_t ScreenBuffer::write_output_char_info_rect(
        const SMALL_RECT region,
        const std::span<const CHAR_INFO> records,
        const bool unicode) noexcept
    {
        if (_cells.empty())
        {
            return 0;
        }

        if (region.Left > region.Right || region.Top > region.Bottom)
        {
            return 0;
        }

        if (!coord_in_range(COORD{ region.Left, region.Top }) ||
            !coord_in_range(COORD{ region.Right, region.Bottom }))
        {
            return 0;
        }

        const long width_long = static_cast<long>(region.Right) - static_cast<long>(region.Left) + 1;
        const long height_long = static_cast<long>(region.Bottom) - static_cast<long>(region.Top) + 1;
        if (width_long <= 0 || height_long <= 0)
        {
            return 0;
        }

        const size_t width = static_cast<size_t>(width_long);
        const size_t height = static_cast<size_t>(height_long);
        const size_t needed = width * height;
        if (records.size() < needed)
        {
            return 0;
        }

        touch();
        size_t index = 0;
        for (SHORT y = region.Top; y <= region.Bottom; ++y)
        {
            for (SHORT x = region.Left; x <= region.Right; ++x)
            {
                const auto& info = records[index];
                const wchar_t value = unicode
                    ? info.Char.UnicodeChar
                    : static_cast<wchar_t>(static_cast<unsigned char>(info.Char.AsciiChar));

                const size_t cell_index = linear_index(COORD{ x, y });
                _cells[cell_index].character = value;
                _cells[cell_index].attributes = info.Attributes;
                ++index;
            }
        }

        return needed;
    }

    size_t ScreenBuffer::read_output_char_info_rect(
        const SMALL_RECT region,
        const std::span<CHAR_INFO> records,
        const bool unicode) const noexcept
    {
        if (_cells.empty())
        {
            return 0;
        }

        if (region.Left > region.Right || region.Top > region.Bottom)
        {
            return 0;
        }

        if (!coord_in_range(COORD{ region.Left, region.Top }) ||
            !coord_in_range(COORD{ region.Right, region.Bottom }))
        {
            return 0;
        }

        const long width_long = static_cast<long>(region.Right) - static_cast<long>(region.Left) + 1;
        const long height_long = static_cast<long>(region.Bottom) - static_cast<long>(region.Top) + 1;
        if (width_long <= 0 || height_long <= 0)
        {
            return 0;
        }

        const size_t width = static_cast<size_t>(width_long);
        const size_t height = static_cast<size_t>(height_long);
        const size_t needed = width * height;
        if (records.size() < needed)
        {
            return 0;
        }

        size_t index = 0;
        for (SHORT y = region.Top; y <= region.Bottom; ++y)
        {
            for (SHORT x = region.Left; x <= region.Right; ++x)
            {
                const size_t cell_index = linear_index(COORD{ x, y });
                const auto& cell = _cells[cell_index];

                CHAR_INFO info{};
                info.Attributes = cell.attributes;
                if (unicode)
                {
                    info.Char.UnicodeChar = cell.character;
                }
                else
                {
                    const unsigned char narrowed = cell.character <= 0xFF
                        ? static_cast<unsigned char>(cell.character)
                        : static_cast<unsigned char>('?');
                    info.Char.AsciiChar = static_cast<CHAR>(narrowed);
                }

                records[index] = info;
                ++index;
            }
        }

        return needed;
    }

    bool ScreenBuffer::scroll_screen_buffer(
        const SMALL_RECT scroll_rectangle,
        const SMALL_RECT clip_rectangle,
        const COORD destination_origin,
        const wchar_t fill_character,
        const USHORT fill_attributes) noexcept
    {
        if (_cells.empty())
        {
            return false;
        }

        if (scroll_rectangle.Left > scroll_rectangle.Right || scroll_rectangle.Top > scroll_rectangle.Bottom)
        {
            return true;
        }

        if (!coord_in_range(COORD{ scroll_rectangle.Left, scroll_rectangle.Top }) ||
            !coord_in_range(COORD{ scroll_rectangle.Right, scroll_rectangle.Bottom }))
        {
            return true;
        }

        const long width_long =
            static_cast<long>(scroll_rectangle.Right) - static_cast<long>(scroll_rectangle.Left) + 1;
        const long height_long =
            static_cast<long>(scroll_rectangle.Bottom) - static_cast<long>(scroll_rectangle.Top) + 1;
        if (width_long <= 0 || height_long <= 0)
        {
            return true;
        }

        touch();
        const size_t width = static_cast<size_t>(width_long);
        const size_t height = static_cast<size_t>(height_long);
        const size_t cell_count = width * height;

        std::vector<ScreenCell> saved;
        try
        {
            saved.resize(cell_count);
        }
        catch (...)
        {
            return false;
        }

        const auto clip_contains = [&](const SHORT x, const SHORT y) noexcept -> bool {
            return x >= clip_rectangle.Left &&
                   x <= clip_rectangle.Right &&
                   y >= clip_rectangle.Top &&
                   y <= clip_rectangle.Bottom;
        };

        size_t index = 0;
        for (SHORT y = scroll_rectangle.Top; y <= scroll_rectangle.Bottom; ++y)
        {
            for (SHORT x = scroll_rectangle.Left; x <= scroll_rectangle.Right; ++x)
            {
                saved[index] = _cells[linear_index(COORD{ x, y })];

                if (clip_contains(x, y))
                {
                    _cells[linear_index(COORD{ x, y })] = ScreenCell{ .character = fill_character, .attributes = fill_attributes };
                }

                ++index;
            }
        }

        const long delta_x = static_cast<long>(destination_origin.X) - static_cast<long>(scroll_rectangle.Left);
        const long delta_y = static_cast<long>(destination_origin.Y) - static_cast<long>(scroll_rectangle.Top);

        const long max_x = static_cast<long>(_buffer_size.X) - 1;
        const long max_y = static_cast<long>(_buffer_size.Y) - 1;

        index = 0;
        for (SHORT y = scroll_rectangle.Top; y <= scroll_rectangle.Bottom; ++y)
        {
            for (SHORT x = scroll_rectangle.Left; x <= scroll_rectangle.Right; ++x)
            {
                const long dest_x = static_cast<long>(x) + delta_x;
                const long dest_y = static_cast<long>(y) + delta_y;

                if (dest_x < 0 || dest_y < 0 || dest_x > max_x || dest_y > max_y)
                {
                    ++index;
                    continue;
                }

                const auto dx = static_cast<SHORT>(dest_x);
                const auto dy = static_cast<SHORT>(dest_y);
                if (!clip_contains(dx, dy))
                {
                    ++index;
                    continue;
                }

                _cells[linear_index(COORD{ dx, dy })] = saved[index];
                ++index;
            }
        }

        return true;
    }

    std::expected<DWORD, ServerError> ConDrvServer::run(
        const core::HandleView server_handle,
        const core::HandleView signal_handle,
        const core::HandleView host_input,
        const core::HandleView host_output,
        const core::HandleView host_signal_pipe,
        logging::Logger& logger) noexcept
    try
    {
        return run_loop(
            server_handle,
            signal_handle,
            core::HandleView{},
            host_input,
            host_output,
            host_signal_pipe,
            nullptr,
            {},
            nullptr,
            logger);
    }
    catch (...)
    {
        return std::unexpected(make_error(L"Unhandled exception in ConDrv server", ERROR_GEN_FAILURE));
    }

    std::expected<DWORD, ServerError> ConDrvServer::run(
        const core::HandleView server_handle,
        const core::HandleView signal_handle,
        const core::HandleView host_input,
        const core::HandleView host_output,
        const core::HandleView host_signal_pipe,
        logging::Logger& logger,
        std::shared_ptr<PublishedScreenBuffer> published,
        const HWND paint_target) noexcept
    try
    {
        return run_loop(
            server_handle,
            signal_handle,
            core::HandleView{},
            host_input,
            host_output,
            host_signal_pipe,
            nullptr,
            std::move(published),
            paint_target,
            logger);
    }
    catch (...)
    {
        return std::unexpected(make_error(L"Unhandled exception in ConDrv server (windowed)", ERROR_GEN_FAILURE));
    }

    std::expected<DWORD, ServerError> ConDrvServer::run_with_handoff(
        const core::HandleView server_handle,
        const core::HandleView signal_handle,
        const core::HandleView input_available_event,
        const core::HandleView host_input,
        const core::HandleView host_output,
        const core::HandleView host_signal_pipe,
        const IoPacket& initial_packet,
        logging::Logger& logger) noexcept
    try
    {
        return run_loop(
            server_handle,
            signal_handle,
            input_available_event,
            host_input,
            host_output,
            host_signal_pipe,
            &initial_packet,
            {},
            nullptr,
            logger);
    }
    catch (...)
    {
        return std::unexpected(make_error(L"Unhandled exception in ConDrv server handoff", ERROR_GEN_FAILURE));
    }

    std::expected<DWORD, ServerError> ConDrvServer::run_with_handoff(
        const core::HandleView server_handle,
        const core::HandleView signal_handle,
        const core::HandleView input_available_event,
        const core::HandleView host_input,
        const core::HandleView host_output,
        const core::HandleView host_signal_pipe,
        const IoPacket& initial_packet,
        logging::Logger& logger,
        std::shared_ptr<PublishedScreenBuffer> published,
        const HWND paint_target) noexcept
    try
    {
        return run_loop(
            server_handle,
            signal_handle,
            input_available_event,
            host_input,
            host_output,
            host_signal_pipe,
            &initial_packet,
            std::move(published),
            paint_target,
            logger);
    }
    catch (...)
    {
        return std::unexpected(make_error(L"Unhandled exception in ConDrv server handoff (windowed)", ERROR_GEN_FAILURE));
    }
}
