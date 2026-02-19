#include "runtime/com_embedding_server.hpp"

#include "core/unique_handle.hpp"
#include "core/win32_handle.hpp"
#include "logging/logger.hpp"

#include <Windows.h>
#include <objbase.h>

#include "runtime/console_handoff.hpp"

#include <atomic>
#include <cstdio>

namespace
{
    constexpr CLSID kClsidConsoleHandoff = {
        0x1F9F2BF5, 0x5BC3, 0x4F17, { 0xB0, 0xE6, 0x91, 0x24, 0x13, 0xF1, 0xF4, 0x51 }
    };

    bool test_embedding_timeout_or_failure()
    {
        oc::logging::Logger logger(oc::logging::LogLevel::error);
        const auto result = oc::runtime::ComEmbeddingServer::run(logger, 1);
        if (result.has_value())
        {
            // A successful return would imply a real COM client connected during
            // the tiny timeout, which should not happen in this test.
            return false;
        }
        return true;
    }

    bool test_embedding_zero_timeout_not_used_in_test()
    {
        // Sanity that timeout-specific path maps WAIT_TIMEOUT when no client
        // connects; if registration fails due environment constraints, this still
        // verifies the error path.
        oc::logging::Logger logger(oc::logging::LogLevel::error);
        const auto result = oc::runtime::ComEmbeddingServer::run(logger, 2);
        return !result.has_value();
    }

    struct EmbeddingServerThreadContext final
    {
        oc::logging::Logger* logger{ nullptr };
        DWORD wait_timeout_ms{ 0 };
        std::atomic<bool> completed{ false };
        bool succeeded{ false };
        DWORD exit_code{ 0 };
    };

    struct HandoffCapture final
    {
        std::atomic<bool> invoked{ false };
        oc::runtime::PortableAttachMessage attach{};
        oc::core::UniqueHandle server_handle;
        oc::core::UniqueHandle signal_pipe;
    };

    HandoffCapture* g_capture{};

    std::expected<DWORD, oc::runtime::ComEmbeddingError> capture_runner(
        const oc::runtime::ComHandoffPayload& payload,
        oc::logging::Logger&) noexcept
    {
        if (g_capture != nullptr)
        {
            g_capture->attach = payload.attach;

            auto duplicated_server = oc::core::duplicate_handle_same_access(payload.server_handle, false);
            if (!duplicated_server)
            {
                return std::unexpected(oc::runtime::ComEmbeddingError{
                    .context = L"DuplicateHandle failed for server handle in test runner",
                    .hresult = HRESULT_FROM_WIN32(duplicated_server.error()),
                    .win32_error = duplicated_server.error(),
                });
            }

            auto duplicated_signal = oc::core::duplicate_handle_same_access(payload.signal_pipe, false);
            if (!duplicated_signal)
            {
                return std::unexpected(oc::runtime::ComEmbeddingError{
                    .context = L"DuplicateHandle failed for signal pipe in test runner",
                    .hresult = HRESULT_FROM_WIN32(duplicated_signal.error()),
                    .win32_error = duplicated_signal.error(),
                });
            }

            g_capture->server_handle = std::move(duplicated_server.value());
            g_capture->signal_pipe = std::move(duplicated_signal.value());
            g_capture->invoked.store(true, std::memory_order_release);
        }

        return DWORD{ 0 };
    }

    DWORD WINAPI embedding_server_thread_proc(LPVOID parameter)
    {
        auto* context = static_cast<EmbeddingServerThreadContext*>(parameter);
        if (context == nullptr || context->logger == nullptr)
        {
            return 0;
        }

        const auto result = oc::runtime::ComEmbeddingServer::run_with_runner(
            *context->logger,
            context->wait_timeout_ms,
            &capture_runner);
        context->succeeded = result.has_value();
        context->exit_code = result.has_value() ? *result : 0;
        context->completed.store(true, std::memory_order_release);
        return 0;
    }

    [[nodiscard]] bool create_named_pipe_pair(oc::core::UniqueHandle& read_end, oc::core::UniqueHandle& write_end) noexcept
    {
        SECURITY_ATTRIBUTES security{};
        security.nLength = sizeof(security);
        security.lpSecurityDescriptor = nullptr;
        security.bInheritHandle = FALSE;

        if (::CreatePipe(read_end.put(), write_end.put(), &security, 0) == FALSE)
        {
            return false;
        }
        return true;
    }

    bool test_embedding_success_path()
    {
        oc::logging::Logger logger(oc::logging::LogLevel::error);
        HandoffCapture capture{};
        g_capture = &capture;
        struct CaptureReset final
        {
            ~CaptureReset() noexcept
            {
                g_capture = nullptr;
            }
        } capture_reset{};

        EmbeddingServerThreadContext server_context{};
        server_context.logger = &logger;
        server_context.wait_timeout_ms = 5'000;

        oc::core::UniqueHandle server_thread(::CreateThread(
            nullptr,
            0,
            &embedding_server_thread_proc,
            &server_context,
            0,
            nullptr));
        if (!server_thread.valid())
        {
            fwprintf(stderr, L"[DETAIL] CreateThread failed\n");
            return false;
        }

        const HRESULT client_coinit = ::CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        if (FAILED(client_coinit))
        {
            fwprintf(stderr, L"[DETAIL] CoInitializeEx failed (hr=0x%08X)\n", static_cast<unsigned int>(client_coinit));
            return false;
        }

        IConsoleHandoff* handoff = nullptr;
        HRESULT activation_hr = E_FAIL;
        for (int attempt = 0; attempt < 200 && handoff == nullptr; ++attempt)
        {
            activation_hr = ::CoCreateInstance(
                kClsidConsoleHandoff,
                nullptr,
                CLSCTX_INPROC_SERVER,
                __uuidof(IConsoleHandoff),
                reinterpret_cast<void**>(&handoff));
            if (SUCCEEDED(activation_hr))
            {
                break;
            }

            ::Sleep(5);
        }

        if (handoff == nullptr)
        {
            fwprintf(stderr, L"[DETAIL] CoCreateInstance failed (last hr=0x%08X)\n", static_cast<unsigned int>(activation_hr));
            ::CoUninitialize();
            return false;
        }

        oc::core::UniqueHandle server_handle(::CreateFileW(
            L"NUL",
            GENERIC_READ,
            FILE_SHARE_READ | FILE_SHARE_WRITE,
            nullptr,
            OPEN_EXISTING,
            FILE_ATTRIBUTE_NORMAL,
            nullptr));
        if (!server_handle.valid())
        {
            fwprintf(stderr, L"[DETAIL] CreateFileW(NUL) failed (error=%lu)\n", ::GetLastError());
            handoff->Release();
            ::CoUninitialize();
            return false;
        }

        oc::core::UniqueHandle input_event(::CreateEventW(nullptr, TRUE, FALSE, nullptr));
        if (!input_event.valid())
        {
            fwprintf(stderr, L"[DETAIL] CreateEventW failed (error=%lu)\n", ::GetLastError());
            handoff->Release();
            ::CoUninitialize();
            return false;
        }

        oc::core::UniqueHandle signal_read;
        oc::core::UniqueHandle signal_write;
        if (!create_named_pipe_pair(signal_read, signal_write))
        {
            fwprintf(stderr, L"[DETAIL] CreatePipe failed (error=%lu)\n", ::GetLastError());
            handoff->Release();
            ::CoUninitialize();
            return false;
        }

        oc::core::UniqueHandle inbox_process_handle;
        if (::DuplicateHandle(
                ::GetCurrentProcess(),
                ::GetCurrentProcess(),
                ::GetCurrentProcess(),
                inbox_process_handle.put(),
                SYNCHRONIZE,
                FALSE,
                0) == FALSE)
        {
            fwprintf(stderr, L"[DETAIL] DuplicateHandle(current process) failed (error=%lu)\n", ::GetLastError());
            handoff->Release();
            ::CoUninitialize();
            return false;
        }

        CONSOLE_PORTABLE_ATTACH_MSG attach{};
        attach.IdLowPart = 1;
        attach.IdHighPart = 0;
        attach.Process = static_cast<ULONG64>(::GetCurrentProcessId());
        attach.Object = 0;
        attach.Function = 0;
        attach.InputSize = 0;
        attach.OutputSize = 0;

        oc::core::UniqueHandle returned_process_handle;
        const HRESULT handoff_hr = handoff->EstablishHandoff(
            server_handle.get(),
            input_event.get(),
            &attach,
            signal_write.get(),
            inbox_process_handle.get(),
            returned_process_handle.put());

        handoff->Release();
        ::CoUninitialize();

        if (FAILED(handoff_hr) || !returned_process_handle.valid())
        {
            fwprintf(stderr, L"[DETAIL] EstablishHandoff failed (hr=0x%08X, process_valid=%d)\n",
                     static_cast<unsigned int>(handoff_hr),
                     returned_process_handle.valid() ? 1 : 0);
            return false;
        }

        const DWORD server_wait = ::WaitForSingleObject(server_thread.get(), 10'000);
        if (server_wait != WAIT_OBJECT_0)
        {
            fwprintf(stderr, L"[DETAIL] WaitForSingleObject(server thread) failed (result=%lu, error=%lu)\n", server_wait, ::GetLastError());
            return false;
        }

        if (!server_context.completed.load(std::memory_order_acquire))
        {
            fwprintf(stderr, L"[DETAIL] Server thread did not mark completed\n");
            return false;
        }

        if (!server_context.succeeded || server_context.exit_code != 0)
        {
            fwprintf(stderr, L"[DETAIL] Server run failed (succeeded=%d, exit=%lu)\n", server_context.succeeded ? 1 : 0, server_context.exit_code);
            return false;
        }

        if (!capture.invoked.load(std::memory_order_acquire))
        {
            fwprintf(stderr, L"[DETAIL] Runner not invoked\n");
            return false;
        }

        if (capture.attach.Function != attach.Function ||
            capture.attach.IdLowPart != attach.IdLowPart ||
            capture.attach.IdHighPart != attach.IdHighPart)
        {
            fwprintf(stderr, L"[DETAIL] Attach message mismatch\n");
            return false;
        }

        DWORD flags = 0;
        if (::GetHandleInformation(capture.server_handle.get(), &flags) == FALSE)
        {
            fwprintf(stderr, L"[DETAIL] GetHandleInformation(server) failed (error=%lu)\n", ::GetLastError());
            return false;
        }
        if (::GetHandleInformation(capture.signal_pipe.get(), &flags) == FALSE)
        {
            fwprintf(stderr, L"[DETAIL] GetHandleInformation(signal) failed (error=%lu)\n", ::GetLastError());
            return false;
        }
        return true;
    }
}

bool run_com_embedding_server_tests()
{
    if (!test_embedding_timeout_or_failure())
    {
        fwprintf(stderr, L"[DETAIL] embedding timeout test failed\n");
        return false;
    }

    if (!test_embedding_zero_timeout_not_used_in_test())
    {
        fwprintf(stderr, L"[DETAIL] embedding short-timeout test failed\n");
        return false;
    }

    if (!test_embedding_success_path())
    {
        fwprintf(stderr, L"[DETAIL] embedding success-path test failed\n");
        return false;
    }

    return true;
}
