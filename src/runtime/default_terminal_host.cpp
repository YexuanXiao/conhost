#include "runtime/default_terminal_host.hpp"

#include "condrv/condrv_packet.hpp"
#include "condrv/condrv_server.hpp"
#include "core/unique_handle.hpp"
#include "core/win32_handle.hpp"
#include "core/win32_wait.hpp"
#include "renderer/window_host.hpp"

#include <Windows.h>

#include <memory>

namespace oc::runtime
{
    namespace
    {
        struct DelegatedWindowContext final
        {
            core::HandleView server_handle{};
            core::HandleView stop_event{};
            core::HandleView input_available_event{};
            core::HandleView host_signal_pipe{};
            logging::Logger* logger{};
            HWND window{};
            std::shared_ptr<view::PublishedScreenBuffer> published_screen;
            condrv::IoPacket initial_packet{};

            DWORD exit_code{ 0 };
            ComEmbeddingError error{};
            bool succeeded{ false };
        };

        DWORD WINAPI delegated_window_server_thread_proc(void* param)
        {
            auto* context = static_cast<DelegatedWindowContext*>(param);
            if (context == nullptr || context->logger == nullptr)
            {
                return 0;
            }

            auto result = condrv::ConDrvServer::run_with_handoff(
                context->server_handle,
                context->stop_event,
                context->input_available_event,
                core::HandleView{}, // windowed mode: input source is not a byte pipe yet
                core::HandleView{}, // windowed mode: output is rendered from published snapshots (no host output pipe)
                context->host_signal_pipe,
                context->initial_packet,
                *context->logger,
                context->published_screen,
                context->window);

            if (result)
            {
                context->exit_code = result.value();
                context->succeeded = true;
            }
            else
            {
                context->error = ComEmbeddingError{
                    .context = result.error().context,
                    .hresult = HRESULT_FROM_WIN32(result.error().win32_error),
                    .win32_error = result.error().win32_error,
                };
                context->succeeded = false;
            }

            if (context->window)
            {
                (void)::PostMessageW(context->window, WM_CLOSE, 0, 0);
            }

            return 0;
        }

        struct SignalBridgeContext final
        {
            core::HandleView signal_handle{};
            core::HandleView stop_event{};
        };

        DWORD WINAPI signal_bridge_thread_proc(void* param)
        {
            auto* context = static_cast<SignalBridgeContext*>(param);
            if (context == nullptr || !context->signal_handle || !context->stop_event)
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
                (void)::SetEvent(context->stop_event.get());
            }

            return 0;
        }

        [[nodiscard]] std::expected<condrv::IoPacket, ComEmbeddingError> make_initial_packet(const PortableAttachMessage& attach) noexcept
        {
            condrv::IoPacket initial{};
            initial.descriptor.identifier.LowPart = attach.IdLowPart;
            initial.descriptor.identifier.HighPart = attach.IdHighPart;
            initial.descriptor.process = static_cast<ULONG_PTR>(attach.Process);
            initial.descriptor.object = static_cast<ULONG_PTR>(attach.Object);
            initial.descriptor.function = attach.Function;
            initial.descriptor.input_size = attach.InputSize;
            initial.descriptor.output_size = attach.OutputSize;
            return initial;
        }
    }

    std::expected<DWORD, ComEmbeddingError> run_windowed_default_terminal_host(
        const ComHandoffPayload& payload,
        logging::Logger& logger) noexcept
    try
    {
        auto stop_event = core::create_event(true, false, nullptr);
        if (!stop_event)
        {
            return std::unexpected(ComEmbeddingError{
                .context = L"CreateEventW failed for delegated window stop event",
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
                .context = L"Failed to allocate published screen buffer for delegated window",
                .hresult = E_OUTOFMEMORY,
                .win32_error = ERROR_OUTOFMEMORY,
            });
        }

        renderer::WindowHostConfig window_config{};
        window_config.title = L"openconsole_new";
        window_config.show_command = SW_SHOWNORMAL;
        window_config.published_screen = published_screen;

        logger.log(logging::LogLevel::info, L"Creating delegated window host (--delegated-window)");
        auto window = renderer::WindowHost::create(std::move(window_config), stop_event->view());
        if (!window)
        {
            return std::unexpected(ComEmbeddingError{
                .context = L"Failed to create delegated window host",
                .hresult = HRESULT_FROM_WIN32(core::to_dword(window.error())),
                .win32_error = core::to_dword(window.error()),
            });
        }

        std::unique_ptr<SignalBridgeContext> signal_bridge_context;
        core::UniqueHandle signal_bridge_thread;
        if (payload.inbox_process)
        {
            try
            {
                signal_bridge_context = std::make_unique<SignalBridgeContext>();
            }
            catch (...)
            {
                return std::unexpected(ComEmbeddingError{
                    .context = L"Failed to allocate delegated window signal bridge context",
                    .hresult = E_OUTOFMEMORY,
                    .win32_error = ERROR_OUTOFMEMORY,
                });
            }

            signal_bridge_context->signal_handle = payload.inbox_process;
            signal_bridge_context->stop_event = stop_event->view();

            signal_bridge_thread = core::UniqueHandle(::CreateThread(
                nullptr,
                0,
                &signal_bridge_thread_proc,
                signal_bridge_context.get(),
                0,
                nullptr));
            if (!signal_bridge_thread.valid())
            {
                return std::unexpected(ComEmbeddingError{
                    .context = L"CreateThread failed for delegated window signal bridge",
                    .hresult = HRESULT_FROM_WIN32(::GetLastError()),
                    .win32_error = ::GetLastError(),
                });
            }
        }

        auto initial_packet = make_initial_packet(payload.attach);
        if (!initial_packet)
        {
            return std::unexpected(initial_packet.error());
        }

        std::unique_ptr<DelegatedWindowContext> server_context;
        try
        {
            server_context = std::make_unique<DelegatedWindowContext>();
        }
        catch (...)
        {
            return std::unexpected(ComEmbeddingError{
                .context = L"Failed to allocate delegated window server context",
                .hresult = E_OUTOFMEMORY,
                .win32_error = ERROR_OUTOFMEMORY,
            });
        }

        server_context->server_handle = payload.server_handle;
        server_context->stop_event = stop_event->view();
        server_context->input_available_event = payload.input_event;
        server_context->host_signal_pipe = payload.signal_pipe;
        server_context->logger = &logger;
        server_context->window = (*window)->hwnd();
        server_context->published_screen = std::move(published_screen);
        server_context->initial_packet = std::move(initial_packet.value());

        logger.log(logging::LogLevel::info, L"ConDrv delegated window server worker starting");
        core::UniqueHandle server_thread(::CreateThread(
            nullptr,
            0,
            &delegated_window_server_thread_proc,
            server_context.get(),
            0,
            nullptr));
        if (!server_thread.valid())
        {
            return std::unexpected(ComEmbeddingError{
                .context = L"CreateThread failed for delegated ConDrv server worker",
                .hresult = HRESULT_FROM_WIN32(::GetLastError()),
                .win32_error = ::GetLastError(),
            });
        }

        // Run the UI loop on the current thread. Closing the window signals
        // `stop_event`, which stops the server worker thread.
        (void)(*window)->run();

        (void)::SetEvent(stop_event->get());
        // Ensure the ConDrv worker thread unblocks from `IOCTL_CONDRV_READ_IO`
        // promptly on window close.
        (void)::CancelSynchronousIo(server_thread.get());
        if (payload.server_handle)
        {
            (void)::CancelIoEx(payload.server_handle.get(), nullptr);
        }

        constexpr DWORD worker_shutdown_timeout_ms = 5'000;
        const DWORD wait_result = ::WaitForSingleObject(server_thread.get(), worker_shutdown_timeout_ms);
        if (wait_result == WAIT_TIMEOUT)
        {
            logger.log(logging::LogLevel::error, L"Delegated ConDrv window worker did not exit within {}ms; forcing process exit", worker_shutdown_timeout_ms);
            ::ExitProcess(ERROR_TIMEOUT);
        }
        if (wait_result != WAIT_OBJECT_0)
        {
            const DWORD error = ::GetLastError();
            logger.log(logging::LogLevel::error, L"WaitForSingleObject failed for delegated ConDrv window worker (error={}); forcing process exit", error);
            ::ExitProcess(error == 0 ? ERROR_GEN_FAILURE : error);
        }

        if (signal_bridge_thread.valid())
        {
            (void)::WaitForSingleObject(signal_bridge_thread.get(), INFINITE);
        }

        if (!server_context->succeeded)
        {
            return std::unexpected(server_context->error);
        }

        return server_context->exit_code;
    }
    catch (...)
    {
        return std::unexpected(ComEmbeddingError{
            .context = L"Unhandled exception in windowed default terminal host",
            .hresult = E_FAIL,
            .win32_error = ERROR_GEN_FAILURE,
        });
    }
}
