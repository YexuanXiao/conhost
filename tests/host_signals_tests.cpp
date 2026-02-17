#include "core/host_signals.hpp"

#include "core/unique_handle.hpp"
#include "runtime/host_signal_input_thread.hpp"

#include <Windows.h>

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <memory>

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

    class TestHostSignalTarget final : public oc::runtime::HostSignalTarget
    {
    public:
        void notify_console_application(const DWORD process_id) noexcept override
        {
            notify_calls.fetch_add(1, std::memory_order_relaxed);
            last_notify_pid.store(process_id, std::memory_order_relaxed);
        }

        void set_foreground(const DWORD process_handle_value, const bool is_foreground) noexcept override
        {
            set_foreground_calls.fetch_add(1, std::memory_order_relaxed);
            last_set_foreground_handle.store(process_handle_value, std::memory_order_relaxed);
            last_set_foreground_state.store(is_foreground ? 1 : 0, std::memory_order_relaxed);
        }

        void end_task(const DWORD process_id, const DWORD event_type, const DWORD ctrl_flags) noexcept override
        {
            end_task_calls.fetch_add(1, std::memory_order_relaxed);
            last_end_task_pid.store(process_id, std::memory_order_relaxed);
            last_end_task_event_type.store(event_type, std::memory_order_relaxed);
            last_end_task_ctrl_flags.store(ctrl_flags, std::memory_order_relaxed);
        }

        void signal_pipe_disconnected() noexcept override
        {
            disconnected_calls.fetch_add(1, std::memory_order_relaxed);
        }

        std::atomic<int> notify_calls{ 0 };
        std::atomic<DWORD> last_notify_pid{ 0 };

        std::atomic<int> set_foreground_calls{ 0 };
        std::atomic<DWORD> last_set_foreground_handle{ 0 };
        std::atomic<int> last_set_foreground_state{ 0 };

        std::atomic<int> end_task_calls{ 0 };
        std::atomic<DWORD> last_end_task_pid{ 0 };
        std::atomic<DWORD> last_end_task_event_type{ 0 };
        std::atomic<DWORD> last_end_task_ctrl_flags{ 0 };

        std::atomic<int> disconnected_calls{ 0 };
    };

    bool test_host_signal_input_thread_dispatches_end_task()
    {
        HANDLE read_raw = nullptr;
        HANDLE write_raw = nullptr;
        if (::CreatePipe(&read_raw, &write_raw, nullptr, 0) == FALSE)
        {
            return false;
        }

        oc::core::UniqueHandle read_end(read_raw);
        oc::core::UniqueHandle write_end(write_raw);

        TestHostSignalTarget target{};
        auto input_thread = oc::runtime::HostSignalInputThread::start(read_end.view(), target, nullptr);
        if (!input_thread)
        {
            return false;
        }

        oc::core::HostSignalEndTaskData payload{};
        payload.sizeInBytes = sizeof(payload);
        payload.processId = 1337;
        payload.eventType = CTRL_CLOSE_EVENT;
        payload.ctrlFlags = oc::core::console_ctrl_close_flag;

        auto write_result = oc::core::write_host_signal_packet(
            write_end.view(),
            oc::core::HostSignals::end_task,
            payload);
        if (!write_result)
        {
            return false;
        }

        write_end.reset();

        const DWORD wait_result = ::WaitForSingleObject(input_thread->thread_handle().get(), 2'000);
        if (wait_result != WAIT_OBJECT_0)
        {
            input_thread->stop_and_join();
            return false;
        }

        input_thread->stop_and_join();

        if (target.end_task_calls.load(std::memory_order_relaxed) != 1)
        {
            return false;
        }
        if (target.last_end_task_pid.load(std::memory_order_relaxed) != payload.processId)
        {
            return false;
        }
        if (target.last_end_task_event_type.load(std::memory_order_relaxed) != payload.eventType)
        {
            return false;
        }
        if (target.last_end_task_ctrl_flags.load(std::memory_order_relaxed) != payload.ctrlFlags)
        {
            return false;
        }
        if (target.disconnected_calls.load(std::memory_order_relaxed) == 0)
        {
            return false;
        }

        return true;
    }

    struct StopAndJoinContext final
    {
        oc::runtime::HostSignalInputThread* input_thread{};
    };

    DWORD WINAPI stop_and_join_thread_proc(void* param)
    {
        auto* context = static_cast<StopAndJoinContext*>(param);
        if (context == nullptr || context->input_thread == nullptr)
        {
            return 0;
        }

        context->input_thread->stop_and_join();
        return 0;
    }

    bool test_host_signal_input_thread_stop_and_join_does_not_hang_without_disconnect()
    {
        HANDLE read_raw = nullptr;
        HANDLE write_raw = nullptr;
        if (::CreatePipe(&read_raw, &write_raw, nullptr, 0) == FALSE)
        {
            return false;
        }

        oc::core::UniqueHandle read_end(read_raw);
        oc::core::UniqueHandle write_end(write_raw);

        TestHostSignalTarget target{};
        auto input_thread_result = oc::runtime::HostSignalInputThread::start(read_end.view(), target, nullptr);
        if (!input_thread_result)
        {
            return false;
        }

        auto input_thread = std::make_unique<oc::runtime::HostSignalInputThread>(std::move(input_thread_result.value()));
        const HANDLE host_signal_thread_handle = input_thread->thread_handle().get();

        StopAndJoinContext context{};
        context.input_thread = input_thread.get();

        oc::core::UniqueHandle join_thread(::CreateThread(
            nullptr,
            0,
            &stop_and_join_thread_proc,
            &context,
            0,
            nullptr));
        if (!join_thread.valid())
        {
            return false;
        }

        const DWORD wait_result = ::WaitForSingleObject(join_thread.get(), 2'000);
        if (wait_result != WAIT_OBJECT_0)
        {
            if (host_signal_thread_handle != nullptr && host_signal_thread_handle != INVALID_HANDLE_VALUE)
            {
                (void)::TerminateThread(host_signal_thread_handle, 0);
            }
            return false;
        }

        // Stopping should be treated as cancellation, not as a pipe disconnect.
        if (target.disconnected_calls.load(std::memory_order_relaxed) != 0)
        {
            return false;
        }

        // Ensure the writer side doesn't linger; best-effort cleanup.
        write_end.reset();
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

    if (!test_host_signal_input_thread_dispatches_end_task())
    {
        fwprintf(stderr, L"[host signals] test_host_signal_input_thread_dispatches_end_task failed\n");
        return false;
    }

    if (!test_host_signal_input_thread_stop_and_join_does_not_hang_without_disconnect())
    {
        fwprintf(stderr, L"[host signals] test_host_signal_input_thread_stop_and_join_does_not_hang_without_disconnect failed\n");
        return false;
    }

    return true;
}
