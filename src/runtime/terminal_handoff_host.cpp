#include "runtime/terminal_handoff_host.hpp"

#include "condrv/condrv_server.hpp"
#include "core/utf8_stream_decoder.hpp"
#include "core/win32_handle.hpp"
#include "renderer/window_host.hpp"
#include "runtime/window_input_sink.hpp"

#include <Windows.h>

#include <array>
#include <memory>
#include <string_view>

namespace oc::runtime
{
    namespace
    {
        constexpr ULONG k_terminal_output_mode =
            ENABLE_PROCESSED_OUTPUT |
            ENABLE_WRAP_AT_EOL_OUTPUT |
            ENABLE_VIRTUAL_TERMINAL_PROCESSING |
            DISABLE_NEWLINE_AUTO_RETURN;

        struct WindowedTerminalContext final
        {
            core::HandleView stop_event{};
            logging::Logger* logger{};
            HWND window{};
            std::shared_ptr<view::PublishedScreenBuffer> published_screen;
            std::shared_ptr<condrv::ScreenBuffer> screen_buffer;

            core::UniqueHandle terminal_output_read;
            core::UniqueHandle client_process;

            DWORD exit_code{ 0 };
            ComEmbeddingError error{};
            bool succeeded{ false };
            bool hold_window_on_exit{ false };
        };

        void publish_snapshot_best_effort(WindowedTerminalContext& context) noexcept
        {
            if (!context.published_screen || !context.screen_buffer)
            {
                return;
            }

            const auto snapshot = condrv::make_viewport_snapshot(*context.screen_buffer);
            if (!snapshot)
            {
                return;
            }

            context.published_screen->publish(snapshot.value());
            if (context.window != nullptr)
            {
                (void)::PostMessageW(context.window, WM_APP + 1, 0, 0);
            }
        }

        DWORD WINAPI terminal_output_thread_proc(void* param)
        {
            auto* context = static_cast<WindowedTerminalContext*>(param);
            if (context == nullptr || context->logger == nullptr || !context->terminal_output_read.valid() || !context->screen_buffer ||
                !context->published_screen)
            {
                return 0;
            }

            bool canceled = false;
            try
            {
                core::Utf8StreamDecoder decoder;

                bool client_exited = false;
                bool draining_after_exit = false;
                ULONGLONG drain_start_tick = 0;
                static constexpr ULONGLONG kDrainTimeoutMs = 2'000;

                for (;;)
                {
                    if (context->stop_event)
                    {
                        const DWORD stop_state = ::WaitForSingleObject(context->stop_event.get(), 0);
                        if (stop_state == WAIT_OBJECT_0)
                        {
                            canceled = true;
                            break;
                        }
                        if (stop_state == WAIT_FAILED)
                        {
                            const DWORD error = ::GetLastError();
                            context->error = ComEmbeddingError{
                                .context = L"WaitForSingleObject failed for terminal-handoff stop event",
                                .hresult = HRESULT_FROM_WIN32(error),
                                .win32_error = error,
                            };
                            context->succeeded = false;
                            return 0;
                        }
                    }

                    if (context->client_process.valid() && !client_exited)
                    {
                        const DWORD process_state = ::WaitForSingleObject(context->client_process.get(), 0);
                        if (process_state == WAIT_OBJECT_0)
                        {
                            client_exited = true;
                        }
                        else if (process_state == WAIT_FAILED)
                        {
                            const DWORD error = ::GetLastError();
                            context->error = ComEmbeddingError{
                                .context = L"WaitForSingleObject failed for delegated client process",
                                .hresult = HRESULT_FROM_WIN32(error),
                                .win32_error = error,
                            };
                            context->succeeded = false;
                            return 0;
                        }
                    }

                    DWORD available = 0;
                    const BOOL peek_ok = ::PeekNamedPipe(
                        context->terminal_output_read.get(),
                        nullptr,
                        0,
                        nullptr,
                        &available,
                        nullptr);
                    if (peek_ok == FALSE)
                    {
                        const DWORD error = ::GetLastError();
                        if (error == ERROR_BROKEN_PIPE || error == ERROR_NO_DATA || error == ERROR_PIPE_NOT_CONNECTED || error == ERROR_OPERATION_ABORTED)
                        {
                            break;
                        }

                        context->error = ComEmbeddingError{
                            .context = L"PeekNamedPipe failed for terminal-handoff output",
                            .hresult = HRESULT_FROM_WIN32(error),
                            .win32_error = error,
                        };
                        context->succeeded = false;
                        return 0;
                    }

                    bool had_output = false;
                    if (available != 0)
                    {
                        std::array<std::byte, 8192> buffer{};
                        const DWORD to_read = available < buffer.size() ? available : static_cast<DWORD>(buffer.size());

                        DWORD read = 0;
                        if (::ReadFile(context->terminal_output_read.get(), buffer.data(), to_read, &read, nullptr) == FALSE)
                        {
                            const DWORD error = ::GetLastError();
                            if (error == ERROR_BROKEN_PIPE || error == ERROR_NO_DATA || error == ERROR_PIPE_NOT_CONNECTED || error == ERROR_OPERATION_ABORTED)
                            {
                                break;
                            }

                            context->error = ComEmbeddingError{
                                .context = L"ReadFile failed for terminal-handoff output",
                                .hresult = HRESULT_FROM_WIN32(error),
                                .win32_error = error,
                            };
                            context->succeeded = false;
                            return 0;
                        }

                        if (read != 0)
                        {
                            had_output = true;
                            const auto chunk = std::span<const std::byte>(buffer.data(), static_cast<size_t>(read));
                            std::wstring decoded = decoder.decode_append(chunk);
                            if (!decoded.empty())
                            {
                                condrv::apply_text_to_screen_buffer<condrv::NullHostIo>(
                                    *context->screen_buffer,
                                    decoded,
                                    k_terminal_output_mode,
                                    nullptr,
                                    nullptr);
                                publish_snapshot_best_effort(*context);
                            }
                        }

                        if (had_output)
                        {
                            draining_after_exit = false;
                        }
                    }

                    if (client_exited)
                    {
                        if (!had_output)
                        {
                            if (!draining_after_exit)
                            {
                                draining_after_exit = true;
                                drain_start_tick = ::GetTickCount64();
                            }
                            else if ((::GetTickCount64() - drain_start_tick) >= kDrainTimeoutMs)
                            {
                                break;
                            }
                        }
                    }

                    if (!had_output)
                    {
                        ::Sleep(1);
                    }
                }

                DWORD exit_code = 0;
                if (context->client_process.valid())
                {
                    if (::GetExitCodeProcess(context->client_process.get(), &exit_code) == FALSE)
                    {
                        const DWORD error = ::GetLastError();
                        context->error = ComEmbeddingError{
                            .context = L"GetExitCodeProcess failed for terminal-handoff client",
                            .hresult = HRESULT_FROM_WIN32(error),
                            .win32_error = error,
                        };
                        context->succeeded = false;
                        return 0;
                    }
                }

                context->exit_code = exit_code;
                context->succeeded = true;

                if (!canceled)
                {
                    if (context->hold_window_on_exit)
                    {
                        wchar_t message[96]{};
                        _snwprintf_s(
                            message,
                            _TRUNCATE,
                            L"\r\n[process exited with code %lu]\r\n",
                            static_cast<unsigned long>(exit_code));

                        condrv::apply_text_to_screen_buffer<condrv::NullHostIo>(
                            *context->screen_buffer,
                            std::wstring_view(message),
                            k_terminal_output_mode,
                            nullptr,
                            nullptr);
                        publish_snapshot_best_effort(*context);
                    }
                    else if (context->window != nullptr)
                    {
                        (void)::PostMessageW(context->window, WM_CLOSE, 0, 0);
                    }
                }

                return 0;
            }
            catch (...)
            {
                context->error = ComEmbeddingError{
                    .context = L"Unhandled exception in terminal-handoff output thread",
                    .hresult = E_FAIL,
                    .win32_error = ERROR_GEN_FAILURE,
                };
                context->succeeded = false;

                if (!canceled && context->window != nullptr)
                {
                    (void)::PostMessageW(context->window, WM_CLOSE, 0, 0);
                }

                return 0;
            }
        }

        [[nodiscard]] std::expected<DWORD, ComEmbeddingError> run_windowed_terminal_handoff_host_impl(
            TerminalHandoffPayload payload,
            logging::Logger& logger,
            const bool hold_window_on_exit) noexcept
        try
        {
            auto stop_event = core::create_event(true, false, nullptr);
            if (!stop_event)
            {
                return std::unexpected(ComEmbeddingError{
                    .context = L"CreateEventW failed for terminal-handoff stop event",
                    .hresult = HRESULT_FROM_WIN32(stop_event.error()),
                    .win32_error = stop_event.error(),
                });
            }

            std::shared_ptr<view::PublishedScreenBuffer> published_screen;
            try
            {
                published_screen = std::make_shared<view::PublishedScreenBuffer>();
            }
            catch (...)
            {
                return std::unexpected(ComEmbeddingError{
                    .context = L"Failed to allocate published screen buffer for terminal handoff",
                    .hresult = E_OUTOFMEMORY,
                    .win32_error = ERROR_OUTOFMEMORY,
                });
            }

            const COORD initial_size = payload.initial_size.X == 0 || payload.initial_size.Y == 0
                ? COORD{ 80, 25 }
                : payload.initial_size;

            condrv::ScreenBuffer::Settings settings = condrv::ScreenBuffer::default_settings();
            settings.buffer_size = initial_size;
            settings.window_size = initial_size;
            settings.maximum_window_size = initial_size;

            auto screen_buffer_result = condrv::ScreenBuffer::create(std::move(settings));
            if (!screen_buffer_result)
            {
                return std::unexpected(ComEmbeddingError{
                    .context = screen_buffer_result.error().context,
                    .hresult = HRESULT_FROM_WIN32(screen_buffer_result.error().win32_error),
                    .win32_error = screen_buffer_result.error().win32_error,
                });
            }

            std::shared_ptr<condrv::ScreenBuffer> screen_buffer = std::move(screen_buffer_result.value());

            std::shared_ptr<renderer::IWindowInputSink> input_sink;
            try
            {
                input_sink = std::make_shared<WindowInputPipeSink>(std::move(payload.terminal_input));
            }
            catch (...)
            {
                return std::unexpected(ComEmbeddingError{
                    .context = L"Failed to allocate terminal-handoff window input sink",
                    .hresult = E_OUTOFMEMORY,
                    .win32_error = ERROR_OUTOFMEMORY,
                });
            }

            renderer::WindowHostConfig window_config{};
            window_config.title = payload.title.empty() ? L"openconsole_new" : payload.title;
            window_config.show_command = payload.show_command;
            window_config.published_screen = published_screen;
            window_config.input_sink = std::move(input_sink);

            logger.log(logging::LogLevel::info, L"Creating terminal-handoff window host (-Embedding / ITerminalHandoff)");
            auto window = renderer::WindowHost::create(std::move(window_config), stop_event->view());
            if (!window)
            {
                return std::unexpected(ComEmbeddingError{
                    .context = L"Failed to create terminal-handoff window host",
                    .hresult = HRESULT_FROM_WIN32(core::to_dword(window.error())),
                    .win32_error = core::to_dword(window.error()),
                });
            }

            // Keep server-provided handles alive for the session duration. Some ConPTY
            // lifetimes are tied to handle references (e.g. the console reference handle).
            core::UniqueHandle signal_pipe = std::move(payload.signal_pipe);
            core::UniqueHandle reference = std::move(payload.reference);
            core::UniqueHandle server_process = std::move(payload.server_process);

            WindowedTerminalContext context{};
            context.stop_event = stop_event->view();
            context.logger = &logger;
            context.window = (*window)->hwnd();
            context.published_screen = published_screen;
            context.screen_buffer = screen_buffer;
            context.terminal_output_read = std::move(payload.terminal_output);
            context.client_process = std::move(payload.client_process);
            context.hold_window_on_exit = hold_window_on_exit;

            publish_snapshot_best_effort(context);

            logger.log(logging::LogLevel::info, L"Terminal-handoff output worker starting");
            core::UniqueHandle output_thread(::CreateThread(
                nullptr,
                0,
                &terminal_output_thread_proc,
                &context,
                0,
                nullptr));
            if (!output_thread.valid())
            {
                const DWORD error = ::GetLastError();
                return std::unexpected(ComEmbeddingError{
                    .context = L"CreateThread failed for terminal-handoff output worker",
                    .hresult = HRESULT_FROM_WIN32(error),
                    .win32_error = error,
                });
            }

            // Run the UI loop on the current thread. Closing the window signals
            // `stop_event`, which stops the output worker thread.
            (void)(*window)->run();

            (void)::SetEvent(stop_event->get());
            // Request termination from the server by closing the host-signal pipe.
            signal_pipe.reset();
            (void)reference;
            (void)server_process;

            (void)::CancelSynchronousIo(output_thread.get());
            if (context.terminal_output_read.valid())
            {
                (void)::CancelIoEx(context.terminal_output_read.get(), nullptr);
            }

            constexpr DWORD worker_shutdown_timeout_ms = 5'000;
            const DWORD wait_result = ::WaitForSingleObject(output_thread.get(), worker_shutdown_timeout_ms);
            if (wait_result == WAIT_TIMEOUT)
            {
                logger.log(
                    logging::LogLevel::error,
                    L"Terminal-handoff output worker did not exit within {}ms; forcing process exit",
                    worker_shutdown_timeout_ms);
                ::ExitProcess(ERROR_TIMEOUT);
            }
            if (wait_result != WAIT_OBJECT_0)
            {
                const DWORD error = ::GetLastError();
                logger.log(logging::LogLevel::error, L"WaitForSingleObject failed for terminal-handoff output worker (error={}); forcing process exit", error);
                ::ExitProcess(error == 0 ? ERROR_GEN_FAILURE : error);
            }

            if (!context.succeeded)
            {
                return std::unexpected(context.error);
            }

            return context.exit_code;
        }
        catch (...)
        {
            return std::unexpected(ComEmbeddingError{
                .context = L"Unhandled exception in windowed terminal-handoff host",
                .hresult = E_FAIL,
                .win32_error = ERROR_GEN_FAILURE,
            });
        }
    }

    std::expected<DWORD, ComEmbeddingError> run_windowed_terminal_handoff_host(
        TerminalHandoffPayload payload,
        logging::Logger& logger) noexcept
    {
        return run_windowed_terminal_handoff_host_impl(std::move(payload), logger, false);
    }

    std::expected<DWORD, ComEmbeddingError> run_windowed_terminal_handoff_host_hold(
        TerminalHandoffPayload payload,
        logging::Logger& logger) noexcept
    {
        return run_windowed_terminal_handoff_host_impl(std::move(payload), logger, true);
    }
}
