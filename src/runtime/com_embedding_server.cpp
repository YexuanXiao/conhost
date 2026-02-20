#include "runtime/com_embedding_server.hpp"

#include "condrv/condrv_packet.hpp"
#include "condrv/condrv_server.hpp"
#include "core/assert.hpp"
#include "core/handle_view.hpp"
#include "core/unique_handle.hpp"
#include "core/win32_handle.hpp"
#include "runtime/console_handoff.hpp"
#include "runtime/server_handle_validator.hpp"
#include "runtime/terminal_handoff_com.hpp"

#include <objbase.h>

#include <array>
#include <atomic>
#include <cwchar>
#include <new>
#include <optional>
#include <string_view>
#include <utility>

// `runtime/com_embedding_server.cpp` implements the out-of-proc COM local server
// for `-Embedding` startup.
//
// Responsibilities:
// - Register the `IConsoleHandoff` class object (single-use).
// - Receive an inbox-to-out-of-box handoff (`EstablishHandoff`):
//   - ConDrv server handle
//   - input-availability event (driver-registered)
//   - host-signal pipe (delegated host -> inbox host privileged requests)
//   - inbox process handle (for lifetime tracking)
//   - portable attach message (identifier + process/object + buffer sizes)
// - Run the ConDrv server loop with the provided initial packet so the client
//   connection that triggered the handoff is properly completed.
//
// Tests:
// - Unit tests validate COM registration and handle duplication.
// - Integration tests validate out-of-proc activation and a round-trip
//   `EstablishHandoff` call using the in-tree proxy/stub DLL.
//
// See also:
// - `new/docs/conhost_behavior_imitation_matrix.md`
// - `new/tests/com_embedding_integration_tests.cpp`

namespace oc::runtime
{
    namespace
    {
        // The embedding server accepts exactly one handoff and then exits.
        // This mirrors how upstream OpenConsole uses `REGCLS_SINGLEUSE` and
        // keeps lifecycle predictable for the inbox host.
        enum class EmbeddingCompletionState : LONG
        {
            pending = 0,
            console_succeeded = 1,
            terminal_succeeded = 2,
            failed = 3,
        };

        // Default OpenConsole class ID from upstream non-branded branch.
        // This class exposes IConsoleHandoff for inbox-to-out-of-box handoff.
        constexpr CLSID kClsidConsoleHandoff = {
            0x1F9F2BF5, 0x5BC3, 0x4F17, { 0xB0, 0xE6, 0x91, 0x24, 0x13, 0xF1, 0xF4, 0x51 }
        };

        constexpr std::wstring_view kTestReadyEventEnv = L"OPENCONSOLE_NEW_TEST_EMBED_READY_EVENT";

        [[nodiscard]] DWORD to_win32_error_from_hresult(const HRESULT hr) noexcept
        {
            const DWORD code = static_cast<DWORD>(HRESULT_CODE(hr));
            return code == 0 ? ERROR_GEN_FAILURE : code;
        }

        [[nodiscard]] std::wstring read_environment_variable(const std::wstring_view name) noexcept
        {
            const DWORD required = ::GetEnvironmentVariableW(name.data(), nullptr, 0);
            if (required == 0)
            {
                return {};
            }

            std::wstring buffer(required, L'\0');
            const DWORD written = ::GetEnvironmentVariableW(name.data(), buffer.data(), required);
            if (written == 0)
            {
                return {};
            }

            buffer.resize(written);
            return buffer;
        }

        void signal_test_ready_event(logging::Logger& logger) noexcept
        {
            const std::wstring event_name = read_environment_variable(kTestReadyEventEnv);
            if (event_name.empty())
            {
                return;
            }

            core::UniqueHandle ready_event(::OpenEventW(EVENT_MODIFY_STATE, FALSE, event_name.c_str()));
            if (!ready_event.valid())
            {
                logger.log(logging::LogLevel::debug, L"Test ready event '{}' could not be opened (error={})", event_name, ::GetLastError());
                return;
            }

            if (::SetEvent(ready_event.get()) == FALSE)
            {
                logger.log(logging::LogLevel::debug, L"Test ready event '{}' SetEvent failed (error={})", event_name, ::GetLastError());
            }
        }

        class CoInitScope final
        {
        public:
            explicit CoInitScope(const HRESULT hr) noexcept :
                _hr(hr)
            {
            }

            ~CoInitScope() noexcept
            {
                if (SUCCEEDED(_hr))
                {
                    ::CoUninitialize();
                }
            }

            [[nodiscard]] HRESULT result() const noexcept
            {
                return _hr;
            }

        private:
            HRESULT _hr{ E_FAIL };
        };

        class ClassObjectRegistration final
        {
        public:
            ClassObjectRegistration() noexcept = default;

            ~ClassObjectRegistration() noexcept
            {
                reset();
            }

            ClassObjectRegistration(const ClassObjectRegistration&) = delete;
            ClassObjectRegistration& operator=(const ClassObjectRegistration&) = delete;

            [[nodiscard]] HRESULT register_single_use(IUnknown* class_factory) noexcept
            {
                reset();
                if (class_factory == nullptr)
                {
                    return E_POINTER;
                }

                // Register as "single use": COM will revoke the class object
                // after one successful activation, which matches the desired
                // console handoff contract.
                const HRESULT hr = ::CoRegisterClassObject(
                    kClsidConsoleHandoff,
                    class_factory,
                    CLSCTX_LOCAL_SERVER | CLSCTX_INPROC_SERVER,
                    REGCLS_SINGLEUSE,
                    &_cookie);
                if (SUCCEEDED(hr))
                {
                    _active = true;
                }
                return hr;
            }

            void reset() noexcept
            {
                if (_active)
                {
                    ::CoRevokeClassObject(_cookie);
                    _cookie = 0;
                    _active = false;
                }
            }

        private:
            DWORD _cookie{ 0 };
            bool _active{ false };
        };

        class HandoffState final
        {
        public:
            explicit HandoffState(core::HandleView completion_event) noexcept :
                _completion_event(completion_event)
            {
            }

            // The inbox host may call `EstablishHandoff` only once. Guard the
            // implementation so test harnesses and unexpected COM retries do
            // not corrupt state.
            [[nodiscard]] bool try_begin_establish() noexcept
            {
                bool expected = false;
                return _establish_called.compare_exchange_strong(expected, true, std::memory_order_acq_rel);
            }

            void set_failed(const HRESULT hr) noexcept
            {
                _failure_hr = hr;
                _failure_context = L"COM embedding handoff failed";
                _state.store(static_cast<LONG>(EmbeddingCompletionState::failed), std::memory_order_release);
                signal_completion_event();
            }

            void set_failed_with_context(const wchar_t* const context, const HRESULT hr) noexcept
            {
                _failure_hr = hr;
                _failure_context = context;
                _state.store(static_cast<LONG>(EmbeddingCompletionState::failed), std::memory_order_release);
                signal_completion_event();
            }

            void set_console_succeeded() noexcept
            {
                _state.store(static_cast<LONG>(EmbeddingCompletionState::console_succeeded), std::memory_order_release);
                signal_completion_event();
            }

            void set_terminal_succeeded() noexcept
            {
                _state.store(static_cast<LONG>(EmbeddingCompletionState::terminal_succeeded), std::memory_order_release);
                signal_completion_event();
            }

            [[nodiscard]] EmbeddingCompletionState state() const noexcept
            {
                return static_cast<EmbeddingCompletionState>(_state.load(std::memory_order_acquire));
            }

            [[nodiscard]] HRESULT failure_hr() const noexcept
            {
                return _failure_hr;
            }

            [[nodiscard]] const wchar_t* failure_context() const noexcept
            {
                return _failure_context;
            }

            [[nodiscard]] HRESULT duplicate_incoming_handles(
                const core::HandleView server,
                const core::HandleView input_event,
                const core::HandleView signal_pipe,
                const core::HandleView inbox_process) noexcept
            {
                // `EstablishHandoff` provides handles that may not be safe to
                // close by the caller (they are owned by the COM server). We
                // duplicate them into this process so ownership is explicit
                // and the COM method can return promptly.
                const auto duplicate = [](const core::HandleView in, core::UniqueHandle& out) noexcept -> HRESULT {
                    if (!in)
                    {
                        return S_OK;
                    }

                    auto duplicated = core::duplicate_handle_same_access(in, false);
                    if (!duplicated)
                    {
                        return HRESULT_FROM_WIN32(duplicated.error());
                    }

                    out = std::move(duplicated.value());
                    return S_OK;
                };

                if (const HRESULT hr = duplicate(server, _server_handle); FAILED(hr))
                {
                    return hr;
                }
                if (const HRESULT hr = duplicate(input_event, _input_event); FAILED(hr))
                {
                    return hr;
                }
                if (const HRESULT hr = duplicate(signal_pipe, _signal_pipe); FAILED(hr))
                {
                    return hr;
                }
                if (const HRESULT hr = duplicate(inbox_process, _inbox_process); FAILED(hr))
                {
                    return hr;
                }

                return S_OK;
            }

            void copy_attach_message(const PCCONSOLE_PORTABLE_ATTACH_MSG msg) noexcept
            {
                OC_ASSERT(msg != nullptr);
                _attach_msg.IdLowPart = msg->IdLowPart;
                _attach_msg.IdHighPart = msg->IdHighPart;
                _attach_msg.Process = msg->Process;
                _attach_msg.Object = msg->Object;
                _attach_msg.Function = msg->Function;
                _attach_msg.InputSize = msg->InputSize;
                _attach_msg.OutputSize = msg->OutputSize;
            }

            [[nodiscard]] core::HandleView server_handle() const noexcept
            {
                return core::HandleView(_server_handle.get());
            }

            [[nodiscard]] core::HandleView input_event() const noexcept
            {
                return core::HandleView(_input_event.get());
            }

            [[nodiscard]] core::HandleView signal_pipe() const noexcept
            {
                return core::HandleView(_signal_pipe.get());
            }

            [[nodiscard]] core::HandleView inbox_process() const noexcept
            {
                return core::HandleView(_inbox_process.get());
            }

            [[nodiscard]] const PortableAttachMessage& attach_message() const noexcept
            {
                return _attach_msg;
            }

            [[nodiscard]] HRESULT duplicate_terminal_payload(
                const core::HandleView terminal_input,
                const core::HandleView terminal_output,
                const core::HandleView signal_pipe,
                const core::HandleView reference,
                const core::HandleView server_process,
                const core::HandleView client_process,
                const TERMINAL_STARTUP_INFO* const startup_info) noexcept
            {
                TerminalHandoffPayload payload{};

                const auto duplicate = [](const core::HandleView in, core::UniqueHandle& out) noexcept -> HRESULT {
                    if (!in)
                    {
                        return S_OK;
                    }

                    auto duplicated = core::duplicate_handle_same_access(in, false);
                    if (!duplicated)
                    {
                        return HRESULT_FROM_WIN32(duplicated.error());
                    }

                    out = std::move(duplicated.value());
                    return S_OK;
                };

                if (const HRESULT hr = duplicate(terminal_input, payload.terminal_input); FAILED(hr))
                {
                    return hr;
                }
                if (const HRESULT hr = duplicate(terminal_output, payload.terminal_output); FAILED(hr))
                {
                    return hr;
                }
                if (const HRESULT hr = duplicate(signal_pipe, payload.signal_pipe); FAILED(hr))
                {
                    return hr;
                }
                if (const HRESULT hr = duplicate(reference, payload.reference); FAILED(hr))
                {
                    return hr;
                }
                if (const HRESULT hr = duplicate(server_process, payload.server_process); FAILED(hr))
                {
                    return hr;
                }
                if (const HRESULT hr = duplicate(client_process, payload.client_process); FAILED(hr))
                {
                    return hr;
                }

                if (startup_info != nullptr)
                {
                    if (startup_info->pszTitle != nullptr)
                    {
                        payload.title.assign(startup_info->pszTitle, startup_info->pszTitle + ::wcslen(startup_info->pszTitle));
                    }

                    if (startup_info->dwXCountChars != 0 && startup_info->dwYCountChars != 0)
                    {
                        const DWORD width = startup_info->dwXCountChars > 32'767 ? 32'767 : startup_info->dwXCountChars;
                        const DWORD height = startup_info->dwYCountChars > 32'767 ? 32'767 : startup_info->dwYCountChars;
                        payload.initial_size = COORD{ static_cast<SHORT>(width), static_cast<SHORT>(height) };
                    }

                    payload.show_command = startup_info->wShowWindow == 0 ? SW_SHOWNORMAL : static_cast<int>(startup_info->wShowWindow);
                }

                _terminal_payload.emplace(std::move(payload));
                return S_OK;
            }

            void move_terminal_payload_into(TerminalHandoffPayload& out) noexcept
            {
                if (_terminal_payload.has_value())
                {
                    out = std::move(_terminal_payload.value());
                    _terminal_payload.reset();
                }
            }

        private:
            void signal_completion_event() const noexcept
            {
                if (_completion_event)
                {
                    ::SetEvent(_completion_event.get());
                }
            }

            core::HandleView _completion_event{};
            core::UniqueHandle _server_handle;
            core::UniqueHandle _input_event;
            core::UniqueHandle _signal_pipe;
            core::UniqueHandle _inbox_process;
            PortableAttachMessage _attach_msg{};
            std::optional<TerminalHandoffPayload> _terminal_payload;

            std::atomic<bool> _establish_called{ false };
            std::atomic<LONG> _state{ static_cast<LONG>(EmbeddingCompletionState::pending) };
            HRESULT _failure_hr{ S_OK };
            const wchar_t* _failure_context{ L"COM embedding handoff failed" };
        };

        [[nodiscard]] std::expected<DWORD, ComEmbeddingError> default_handoff_runner(
            const ComHandoffPayload& payload,
            logging::Logger& logger) noexcept
        {
            if (!payload.input_event)
            {
                logger.log(logging::LogLevel::warning, L"Handoff did not provide an input event handle");
            }
            if (!payload.signal_pipe)
            {
                logger.log(logging::LogLevel::warning, L"Handoff did not provide a signal pipe handle");
            }
            if (!payload.inbox_process)
            {
                logger.log(logging::LogLevel::warning, L"Handoff did not provide an inbox process handle");
            }

            auto server_validation = ServerHandleValidator::validate(payload.server_handle);
            if (!server_validation)
            {
                return std::unexpected(ComEmbeddingError{
                    .context = L"Handoff server handle validation failed",
                    .hresult = HRESULT_FROM_WIN32(server_validation.error().win32_error),
                    .win32_error = server_validation.error().win32_error,
                });
            }

            const auto& attach = payload.attach;
            logger.log(
                logging::LogLevel::debug,
                L"Handoff descriptor: id={}:{}, pid={}, tid={}, fn={}, in={}, out={}",
                static_cast<long long>(attach.IdHighPart),
                static_cast<unsigned long long>(attach.IdLowPart),
                static_cast<unsigned long long>(attach.Process),
                static_cast<unsigned long long>(attach.Object),
                static_cast<unsigned long>(attach.Function),
                static_cast<unsigned long>(attach.InputSize),
                static_cast<unsigned long>(attach.OutputSize));

            condrv::IoPacket initial{};
            initial.descriptor.identifier.LowPart = attach.IdLowPart;
            initial.descriptor.identifier.HighPart = attach.IdHighPart;
            initial.descriptor.process = static_cast<ULONG_PTR>(attach.Process);
            initial.descriptor.object = static_cast<ULONG_PTR>(attach.Object);
            initial.descriptor.function = attach.Function;
            initial.descriptor.input_size = attach.InputSize;
            initial.descriptor.output_size = attach.OutputSize;

            // Use the inbox process handle as a stop signal so the delegated host
            // exits promptly when the owning (handoff) process terminates.
            //
            // The signal pipe is a write-only channel used for forwarding
            // privileged control operations (e.g. CTRL event delivery) back to
            // the inbox host.
            auto result = condrv::ConDrvServer::run_with_handoff(
                payload.server_handle,
                payload.inbox_process,
                payload.input_event,
                core::HandleView{},
                core::HandleView{},
                payload.signal_pipe,
                initial,
                logger);
            if (!result)
            {
                return std::unexpected(ComEmbeddingError{
                    .context = result.error().context,
                    .hresult = HRESULT_FROM_WIN32(result.error().win32_error),
                    .win32_error = result.error().win32_error,
                });
            }

            return *result;
        }

        [[nodiscard]] HRESULT create_pipe_pair(core::UniqueHandle& read_end, core::UniqueHandle& write_end, const DWORD buffer_bytes) noexcept
        {
            SECURITY_ATTRIBUTES security{};
            security.nLength = sizeof(security);
            security.lpSecurityDescriptor = nullptr;
            security.bInheritHandle = FALSE;

            if (::CreatePipe(read_end.put(), write_end.put(), &security, buffer_bytes) == FALSE)
            {
                return HRESULT_FROM_WIN32(::GetLastError());
            }

            return S_OK;
        }

        class EmbeddingHandoffObject final :
            public IConsoleHandoff,
            public IDefaultTerminalMarker,
            public ITerminalHandoff,
            public ITerminalHandoff2,
            public ITerminalHandoff3
        {
        public:
            EmbeddingHandoffObject(HandoffState* const state, const bool supports_terminal_handoff) noexcept :
                _state(state),
                _supports_terminal_handoff(supports_terminal_handoff)
            {
            }

            HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** object) noexcept override
            {
                if (object == nullptr)
                {
                    return E_POINTER;
                }
                *object = nullptr;

                if (riid == IID_IUnknown || riid == __uuidof(IConsoleHandoff))
                {
                    *object = static_cast<IConsoleHandoff*>(this);
                }
                else if (riid == __uuidof(IDefaultTerminalMarker))
                {
                    *object = static_cast<IDefaultTerminalMarker*>(this);
                }
                else if (_supports_terminal_handoff && riid == __uuidof(ITerminalHandoff))
                {
                    *object = static_cast<ITerminalHandoff*>(this);
                }
                else if (_supports_terminal_handoff && riid == __uuidof(ITerminalHandoff2))
                {
                    *object = static_cast<ITerminalHandoff2*>(this);
                }
                else if (_supports_terminal_handoff && riid == __uuidof(ITerminalHandoff3))
                {
                    *object = static_cast<ITerminalHandoff3*>(this);
                }
                else
                {
                    return E_NOINTERFACE;
                }

                AddRef();
                return S_OK;
            }

            ULONG STDMETHODCALLTYPE AddRef() noexcept override
            {
                return _ref_count.fetch_add(1, std::memory_order_relaxed) + 1;
            }

            ULONG STDMETHODCALLTYPE Release() noexcept override
            {
                const ULONG remaining = _ref_count.fetch_sub(1, std::memory_order_acq_rel) - 1;
                if (remaining == 0)
                {
                    delete this;
                }
                return remaining;
            }

            HRESULT STDMETHODCALLTYPE EstablishHandoff(
                HANDLE server,
                HANDLE inputEvent,
                PCCONSOLE_PORTABLE_ATTACH_MSG msg,
                HANDLE signalPipe,
                HANDLE inboxProcess,
                HANDLE* process) override
            {
                if (_state == nullptr)
                {
                    return E_UNEXPECTED;
                }

                if (process == nullptr || msg == nullptr)
                {
                    _state->set_failed_with_context(L"IConsoleHandoff::EstablishHandoff failed", E_INVALIDARG);
                    return E_INVALIDARG;
                }

                if (!_state->try_begin_establish())
                {
                    const HRESULT hr = HRESULT_FROM_WIN32(ERROR_ALREADY_INITIALIZED);
                    _state->set_failed_with_context(L"IConsoleHandoff::EstablishHandoff already called", hr);
                    return hr;
                }

                _state->copy_attach_message(msg);
                if (const HRESULT hr = _state->duplicate_incoming_handles(
                        core::HandleView(server),
                        core::HandleView(inputEvent),
                        core::HandleView(signalPipe),
                        core::HandleView(inboxProcess));
                    FAILED(hr))
                {
                    _state->set_failed_with_context(L"IConsoleHandoff::EstablishHandoff handle duplication failed", hr);
                    return hr;
                }

                auto self_process = core::duplicate_current_process(SYNCHRONIZE, false);
                if (!self_process)
                {
                    const HRESULT hr = HRESULT_FROM_WIN32(self_process.error());
                    _state->set_failed_with_context(L"IConsoleHandoff::EstablishHandoff failed to duplicate process handle", hr);
                    return hr;
                }

                *process = self_process.value().release();
                _state->set_console_succeeded();
                return S_OK;
            }

            HRESULT STDMETHODCALLTYPE EstablishPtyHandoff(
                HANDLE in_pipe,
                HANDLE out_pipe,
                HANDLE signal_pipe,
                HANDLE reference,
                HANDLE server_process,
                HANDLE client_process) override
            {
                return establish_terminal_handoff_v1(
                    in_pipe,
                    out_pipe,
                    signal_pipe,
                    reference,
                    server_process,
                    client_process,
                    nullptr);
            }

            HRESULT STDMETHODCALLTYPE EstablishPtyHandoff(
                HANDLE in_pipe,
                HANDLE out_pipe,
                HANDLE signal_pipe,
                HANDLE reference,
                HANDLE server_process,
                HANDLE client_process,
                TERMINAL_STARTUP_INFO startup_info) override
            {
                return establish_terminal_handoff_v1(
                    in_pipe,
                    out_pipe,
                    signal_pipe,
                    reference,
                    server_process,
                    client_process,
                    &startup_info);
            }

            HRESULT STDMETHODCALLTYPE EstablishPtyHandoff(
                HANDLE* in_pipe,
                HANDLE* out_pipe,
                HANDLE signal_pipe,
                HANDLE reference,
                HANDLE server_process,
                HANDLE client_process,
                const TERMINAL_STARTUP_INFO* startup_info) override
            {
                if (_state == nullptr)
                {
                    return E_UNEXPECTED;
                }
                if (in_pipe == nullptr || out_pipe == nullptr)
                {
                    _state->set_failed_with_context(L"ITerminalHandoff3::EstablishPtyHandoff invalid args", E_INVALIDARG);
                    return E_INVALIDARG;
                }

                *in_pipe = nullptr;
                *out_pipe = nullptr;

                if (!_supports_terminal_handoff)
                {
                    _state->set_failed_with_context(L"ITerminalHandoff3 not supported in this mode", E_NOINTERFACE);
                    return E_NOINTERFACE;
                }

                if (!_state->try_begin_establish())
                {
                    const HRESULT hr = HRESULT_FROM_WIN32(ERROR_ALREADY_INITIALIZED);
                    _state->set_failed_with_context(L"ITerminalHandoff3::EstablishPtyHandoff already called", hr);
                    return hr;
                }

                // Create the ConPTY byte-transport pipes. The terminal keeps the write-end for
                // stdin and the read-end for stdout; the server receives the opposite ends.
                constexpr DWORD pipe_buffer_bytes = 64 * 1024;

                core::UniqueHandle server_input_read;
                core::UniqueHandle terminal_input_write;
                HRESULT pipe_hr = create_pipe_pair(server_input_read, terminal_input_write, pipe_buffer_bytes);
                if (FAILED(pipe_hr))
                {
                    _state->set_failed_with_context(L"ITerminalHandoff3::EstablishPtyHandoff CreatePipe(input) failed", pipe_hr);
                    return pipe_hr;
                }

                core::UniqueHandle terminal_output_read;
                core::UniqueHandle server_output_write;
                pipe_hr = create_pipe_pair(terminal_output_read, server_output_write, pipe_buffer_bytes);
                if (FAILED(pipe_hr))
                {
                    _state->set_failed_with_context(L"ITerminalHandoff3::EstablishPtyHandoff CreatePipe(output) failed", pipe_hr);
                    return pipe_hr;
                }

                // Duplicate the handles we intend to keep after returning from the COM call.
                const HRESULT dup_hr = _state->duplicate_terminal_payload(
                    terminal_input_write.view(),
                    terminal_output_read.view(),
                    core::HandleView(signal_pipe),
                    core::HandleView(reference),
                    core::HandleView(server_process),
                    core::HandleView(client_process),
                    startup_info);
                if (FAILED(dup_hr))
                {
                    _state->set_failed_with_context(L"ITerminalHandoff3::EstablishPtyHandoff handle duplication failed", dup_hr);
                    return dup_hr;
                }

                *in_pipe = server_input_read.release();
                *out_pipe = server_output_write.release();

                _state->set_terminal_succeeded();
                return S_OK;
            }

        private:
            HRESULT establish_terminal_handoff_v1(
                const HANDLE in_pipe,
                const HANDLE out_pipe,
                const HANDLE signal_pipe,
                const HANDLE reference,
                const HANDLE server_process,
                const HANDLE client_process,
                const TERMINAL_STARTUP_INFO* const startup_info) noexcept
            {
                if (_state == nullptr)
                {
                    return E_UNEXPECTED;
                }

                if (!_supports_terminal_handoff)
                {
                    _state->set_failed_with_context(L"ITerminalHandoff not supported in this mode", E_NOINTERFACE);
                    return E_NOINTERFACE;
                }

                if (in_pipe == nullptr || out_pipe == nullptr)
                {
                    _state->set_failed_with_context(L"ITerminalHandoff::EstablishPtyHandoff invalid in/out pipes", E_INVALIDARG);
                    return E_INVALIDARG;
                }

                if (!_state->try_begin_establish())
                {
                    const HRESULT hr = HRESULT_FROM_WIN32(ERROR_ALREADY_INITIALIZED);
                    _state->set_failed_with_context(L"ITerminalHandoff::EstablishPtyHandoff already called", hr);
                    return hr;
                }

                const HRESULT dup_hr = _state->duplicate_terminal_payload(
                    core::HandleView(in_pipe),
                    core::HandleView(out_pipe),
                    core::HandleView(signal_pipe),
                    core::HandleView(reference),
                    core::HandleView(server_process),
                    core::HandleView(client_process),
                    startup_info);
                if (FAILED(dup_hr))
                {
                    _state->set_failed_with_context(L"ITerminalHandoff::EstablishPtyHandoff handle duplication failed", dup_hr);
                    return dup_hr;
                }

                _state->set_terminal_succeeded();
                return S_OK;
            }

            std::atomic<ULONG> _ref_count{ 1 };
            HandoffState* _state{ nullptr };
            bool _supports_terminal_handoff{ false };
        };

        class EmbeddingHandoffFactory final : public IClassFactory
        {
        public:
            EmbeddingHandoffFactory(HandoffState* const state, const bool supports_terminal_handoff) noexcept :
                _state(state),
                _supports_terminal_handoff(supports_terminal_handoff)
            {
            }

            HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** object) noexcept override
            {
                if (object == nullptr)
                {
                    return E_POINTER;
                }
                *object = nullptr;

                if (riid == IID_IUnknown || riid == IID_IClassFactory)
                {
                    *object = static_cast<IClassFactory*>(this);
                    AddRef();
                    return S_OK;
                }

                return E_NOINTERFACE;
            }

            ULONG STDMETHODCALLTYPE AddRef() noexcept override
            {
                return _ref_count.fetch_add(1, std::memory_order_relaxed) + 1;
            }

            ULONG STDMETHODCALLTYPE Release() noexcept override
            {
                const ULONG remaining = _ref_count.fetch_sub(1, std::memory_order_acq_rel) - 1;
                if (remaining == 0)
                {
                    delete this;
                }
                return remaining;
            }

            HRESULT STDMETHODCALLTYPE CreateInstance(IUnknown* outer, REFIID riid, void** object) noexcept override
            {
                if (outer != nullptr)
                {
                    return CLASS_E_NOAGGREGATION;
                }
                if (object == nullptr)
                {
                    return E_POINTER;
                }

                auto* created = new (std::nothrow) EmbeddingHandoffObject(_state, _supports_terminal_handoff);
                if (created == nullptr)
                {
                    return E_OUTOFMEMORY;
                }

                const HRESULT hr = created->QueryInterface(riid, object);
                created->Release();
                return hr;
            }

            HRESULT STDMETHODCALLTYPE LockServer(BOOL) noexcept override
            {
                return S_OK;
            }

        private:
            std::atomic<ULONG> _ref_count{ 1 };
            HandoffState* _state{ nullptr };
            bool _supports_terminal_handoff{ false };
        };
    }

    std::expected<DWORD, ComEmbeddingError> ComEmbeddingServer::run(logging::Logger& logger, const DWORD wait_timeout_ms) noexcept
    {
        return run_with_runners(logger, wait_timeout_ms, nullptr, nullptr);
    }

    std::expected<DWORD, ComEmbeddingError> ComEmbeddingServer::run_with_runners(
        logging::Logger& logger,
        const DWORD wait_timeout_ms,
        const HandoffRunner console_runner,
        const TerminalHandoffRunner terminal_runner) noexcept
    {
        const bool supports_terminal_handoff = terminal_runner != nullptr;

        const CoInitScope coinit(::CoInitializeEx(nullptr, COINIT_MULTITHREADED));
        if (FAILED(coinit.result()))
        {
            return std::unexpected(ComEmbeddingError{
                .context = L"CoInitializeEx failed",
                .hresult = coinit.result(),
                .win32_error = static_cast<DWORD>(HRESULT_CODE(coinit.result())),
            });
        }

        core::UniqueHandle completion_event(::CreateEventW(nullptr, TRUE, FALSE, nullptr));
        if (!completion_event.valid())
        {
            return std::unexpected(ComEmbeddingError{
                .context = L"CreateEventW failed",
                .hresult = HRESULT_FROM_WIN32(::GetLastError()),
                .win32_error = ::GetLastError(),
            });
        }

        HandoffState handoff_state(core::HandleView(completion_event.get()));

        auto* factory = new (std::nothrow) EmbeddingHandoffFactory(&handoff_state, supports_terminal_handoff);
        if (factory == nullptr)
        {
            return std::unexpected(ComEmbeddingError{
                .context = L"Factory allocation failed",
                .hresult = E_OUTOFMEMORY,
                .win32_error = ERROR_OUTOFMEMORY,
            });
        }

        ClassObjectRegistration class_registration;
        const HRESULT register_hr = class_registration.register_single_use(static_cast<IUnknown*>(factory));
        factory->Release();

        if (FAILED(register_hr))
        {
            return std::unexpected(ComEmbeddingError{
                .context = L"CoRegisterClassObject failed",
                .hresult = register_hr,
                .win32_error = static_cast<DWORD>(HRESULT_CODE(register_hr)),
            });
        }

        logger.log(logging::LogLevel::info, L"COM embedding server registered (single-use)");
        signal_test_ready_event(logger);
        const DWORD timeout = wait_timeout_ms == 0 ? INFINITE : wait_timeout_ms;
        const DWORD wait_result = ::WaitForSingleObject(completion_event.get(), timeout);
        class_registration.reset();

        if (wait_result != WAIT_OBJECT_0)
        {
            const DWORD wait_error = wait_result == WAIT_TIMEOUT ? WAIT_TIMEOUT : ::GetLastError();
            return std::unexpected(ComEmbeddingError{
                .context = wait_result == WAIT_TIMEOUT
                    ? L"WaitForSingleObject timeout for COM completion event"
                    : L"WaitForSingleObject failed for COM completion event",
                .hresult = HRESULT_FROM_WIN32(wait_error),
                .win32_error = wait_error,
            });
        }

        if (handoff_state.state() == EmbeddingCompletionState::failed)
        {
            const HRESULT hr = handoff_state.failure_hr();
            const wchar_t* const ctx = handoff_state.failure_context();
            return std::unexpected(ComEmbeddingError{
                .context = ctx == nullptr ? std::wstring(L"COM embedding handoff failed") : std::wstring(ctx),
                .hresult = hr,
                .win32_error = to_win32_error_from_hresult(hr),
            });
        }

        if (handoff_state.state() == EmbeddingCompletionState::console_succeeded)
        {
            logger.log(logging::LogLevel::info, L"COM embedding handoff completed (IConsoleHandoff)");
            ComHandoffPayload payload{};
            payload.server_handle = handoff_state.server_handle();
            payload.input_event = handoff_state.input_event();
            payload.signal_pipe = handoff_state.signal_pipe();
            payload.inbox_process = handoff_state.inbox_process();
            payload.attach = handoff_state.attach_message();

            const auto runner = console_runner != nullptr ? console_runner : &default_handoff_runner;
            return runner(payload, logger);
        }

        if (handoff_state.state() == EmbeddingCompletionState::terminal_succeeded)
        {
            if (terminal_runner == nullptr)
            {
                return std::unexpected(ComEmbeddingError{
                    .context = L"Terminal handoff completed, but no ITerminalHandoff runner was configured",
                    .hresult = E_NOINTERFACE,
                    .win32_error = ERROR_NOT_SUPPORTED,
                });
            }

            logger.log(logging::LogLevel::info, L"COM embedding handoff completed (ITerminalHandoff)");
            TerminalHandoffPayload payload{};
            handoff_state.move_terminal_payload_into(payload);
            return terminal_runner(std::move(payload), logger);
        }

        return std::unexpected(ComEmbeddingError{
            .context = L"COM handoff completion state was not set",
            .hresult = E_UNEXPECTED,
            .win32_error = ERROR_GEN_FAILURE,
        });
    }

    std::expected<DWORD, ComEmbeddingError> ComEmbeddingServer::run_with_terminal_runner(
        logging::Logger& logger,
        const DWORD wait_timeout_ms,
        const TerminalHandoffRunner terminal_runner) noexcept
    {
        if (terminal_runner == nullptr)
        {
            return std::unexpected(ComEmbeddingError{
                .context = L"COM embedding terminal runner was null",
                .hresult = E_INVALIDARG,
                .win32_error = ERROR_INVALID_PARAMETER,
            });
        }

        return run_with_runners(logger, wait_timeout_ms, nullptr, terminal_runner);
    }

    std::expected<DWORD, ComEmbeddingError> ComEmbeddingServer::run_with_runner(
        logging::Logger& logger,
        const DWORD wait_timeout_ms,
        const HandoffRunner runner) noexcept
    {
        if (runner == nullptr)
        {
            return std::unexpected(ComEmbeddingError{
                .context = L"COM embedding runner was null",
                .hresult = E_INVALIDARG,
                .win32_error = ERROR_INVALID_PARAMETER,
            });
        }

        return run_with_runners(logger, wait_timeout_ms, runner, nullptr);
    }
}
