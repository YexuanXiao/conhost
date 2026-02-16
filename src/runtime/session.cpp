#include "runtime/session.hpp"

#include "condrv/condrv_server.hpp"
#include "core/assert.hpp"
#include "core/handle_view.hpp"
#include "core/unique_handle.hpp"
#include "core/win32_handle.hpp"
#include "core/win32_wait.hpp"
#include "renderer/window_host.hpp"
#include "runtime/key_input_encoder.hpp"
#include "runtime/server_handle_validator.hpp"

#include <array>
#include <cstddef>
#include <memory>
#include <vector>

namespace oc::runtime
{
    namespace
    {
        struct WindowedServerContext final
        {
            core::HandleView server_handle{};
            core::HandleView stop_event{};
            logging::Logger* logger{};
            HWND window{};
            std::shared_ptr<condrv::PublishedScreenBuffer> published_screen;

            DWORD exit_code{ 0 };
            SessionError error{};
            bool succeeded{ false };
        };

        DWORD WINAPI windowed_server_thread_proc(void* param)
        {
            auto* context = static_cast<WindowedServerContext*>(param);
            if (context == nullptr || context->logger == nullptr)
            {
                return 0;
            }

            auto result = condrv::ConDrvServer::run(
                context->server_handle,
                context->stop_event,
                core::HandleView{}, // windowed mode: input source is not a byte pipe yet
                core::HandleView{}, // windowed mode: output is rendered from published snapshots (no host output pipe)
                core::HandleView{},
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
                context->error = SessionError{
                    .context = result.error().context,
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

        [[nodiscard]] std::expected<DWORD, SessionError> run_windowed_server(
            const SessionOptions& options,
            logging::Logger& logger) noexcept
        {
            auto stop_event = core::create_event(true, false, nullptr);
            if (!stop_event)
            {
                return std::unexpected(SessionError{
                    .context = L"CreateEventW failed for windowed server stop event",
                    .win32_error = stop_event.error(),
                });
            }

            std::shared_ptr<condrv::PublishedScreenBuffer> published_screen;
            try
            {
                published_screen = std::make_shared<condrv::PublishedScreenBuffer>();
            }
            catch (...)
            {
                return std::unexpected(SessionError{
                    .context = L"Failed to allocate published screen buffer",
                    .win32_error = ERROR_OUTOFMEMORY,
                });
            }

            renderer::WindowHostConfig window_config{};
            window_config.title = L"openconsole_new";
            window_config.published_screen = published_screen;
            auto window = renderer::WindowHost::create(std::move(window_config), stop_event->view());
            if (!window)
            {
                return std::unexpected(SessionError{
                    .context = L"Failed to create window host",
                    .win32_error = core::to_dword(window.error()),
                });
            }

            std::unique_ptr<SignalBridgeContext> signal_bridge_context;
            core::UniqueHandle signal_bridge_thread;
            if (options.signal_handle)
            {
                try
                {
                    signal_bridge_context = std::make_unique<SignalBridgeContext>();
                }
                catch (...)
                {
                    return std::unexpected(SessionError{
                        .context = L"Failed to allocate signal bridge context",
                        .win32_error = ERROR_OUTOFMEMORY,
                    });
                }

                signal_bridge_context->signal_handle = options.signal_handle;
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
                    return std::unexpected(SessionError{
                        .context = L"CreateThread failed for signal bridge",
                        .win32_error = ::GetLastError(),
                    });
                }
            }

            std::unique_ptr<WindowedServerContext> server_context;
            try
            {
                server_context = std::make_unique<WindowedServerContext>();
            }
            catch (...)
            {
                return std::unexpected(SessionError{
                    .context = L"Failed to allocate windowed server context",
                    .win32_error = ERROR_OUTOFMEMORY,
                });
            }
            server_context->server_handle = options.server_handle;
            server_context->stop_event = stop_event->view();
            server_context->logger = &logger;
            server_context->window = (*window)->hwnd();
            server_context->published_screen = std::move(published_screen);

            core::UniqueHandle server_thread(::CreateThread(
                nullptr,
                0,
                &windowed_server_thread_proc,
                server_context.get(),
                0,
                nullptr));
            if (!server_thread.valid())
            {
                return std::unexpected(SessionError{
                    .context = L"CreateThread failed for ConDrv server worker",
                    .win32_error = ::GetLastError(),
                });
            }

            // Run the UI loop on the current thread. Closing the window signals
            // `stop_event`, which stops the server worker thread.
            (void)(*window)->run();

            (void)::SetEvent(stop_event->get());
            (void)::WaitForSingleObject(server_thread.get(), INFINITE);

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

        class UniquePseudoConsole final
        {
        public:
            UniquePseudoConsole() noexcept = default;

            explicit UniquePseudoConsole(HPCON value) noexcept :
                _value(value)
            {
            }

            ~UniquePseudoConsole() noexcept
            {
                reset();
            }

            UniquePseudoConsole(const UniquePseudoConsole&) = delete;
            UniquePseudoConsole& operator=(const UniquePseudoConsole&) = delete;

            UniquePseudoConsole(UniquePseudoConsole&& other) noexcept :
                _value(other.release())
            {
            }

            UniquePseudoConsole& operator=(UniquePseudoConsole&& other) noexcept
            {
                if (this != &other)
                {
                    reset(other.release());
                }
                return *this;
            }

            [[nodiscard]] HPCON get() const noexcept
            {
                return _value;
            }

            [[nodiscard]] bool valid() const noexcept
            {
                return _value != nullptr;
            }

            HPCON release() noexcept
            {
                HPCON detached = _value;
                _value = nullptr;
                return detached;
            }

            void reset(HPCON replacement = nullptr) noexcept
            {
                if (_value != nullptr)
                {
                    ::ClosePseudoConsole(_value);
                }
                _value = replacement;
            }

        private:
            HPCON _value{ nullptr };
        };

        class ProcThreadAttributeList final
        {
        public:
            [[nodiscard]] static std::expected<ProcThreadAttributeList, SessionError> create()
            {
                SIZE_T bytes_required = 0;
                ::InitializeProcThreadAttributeList(nullptr, 1, 0, &bytes_required);
                if (bytes_required == 0)
                {
                    return std::unexpected(SessionError{
                        .context = L"InitializeProcThreadAttributeList size query failed",
                        .win32_error = ::GetLastError(),
                    });
                }

                std::vector<std::byte> storage(bytes_required);
                auto* list = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(storage.data());
                if (::InitializeProcThreadAttributeList(list, 1, 0, &bytes_required) == FALSE)
                {
                    return std::unexpected(SessionError{
                        .context = L"InitializeProcThreadAttributeList initialization failed",
                        .win32_error = ::GetLastError(),
                    });
                }

                return ProcThreadAttributeList(std::move(storage));
            }

            ~ProcThreadAttributeList() noexcept
            {
                if (_list != nullptr)
                {
                    ::DeleteProcThreadAttributeList(_list);
                }
            }

            ProcThreadAttributeList(const ProcThreadAttributeList&) = delete;
            ProcThreadAttributeList& operator=(const ProcThreadAttributeList&) = delete;

            ProcThreadAttributeList(ProcThreadAttributeList&& other) noexcept :
                _storage(std::move(other._storage)),
                _list(reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(_storage.data()))
            {
                other._list = nullptr;
            }

            ProcThreadAttributeList& operator=(ProcThreadAttributeList&& other) noexcept
            {
                if (this != &other)
                {
                    if (_list != nullptr)
                    {
                        ::DeleteProcThreadAttributeList(_list);
                    }
                    _storage = std::move(other._storage);
                    _list = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(_storage.data());
                    other._list = nullptr;
                }
                return *this;
            }

            [[nodiscard]] std::expected<void, SessionError> set_pseudo_console(HPCON pseudo_console)
            {
                if (::UpdateProcThreadAttribute(
                        _list,
                        0,
                        PROC_THREAD_ATTRIBUTE_PSEUDOCONSOLE,
                        pseudo_console,
                        sizeof(pseudo_console),
                        nullptr,
                        nullptr) == FALSE)
                {
                    return std::unexpected(SessionError{
                        .context = L"UpdateProcThreadAttribute(PSEUDOCONSOLE) failed",
                        .win32_error = ::GetLastError(),
                    });
                }

                return {};
            }

            [[nodiscard]] LPPROC_THREAD_ATTRIBUTE_LIST get() const noexcept
            {
                return _list;
            }

        private:
            explicit ProcThreadAttributeList(std::vector<std::byte> storage) :
                _storage(std::move(storage)),
                _list(reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(_storage.data()))
            {
            }

            std::vector<std::byte> _storage;
            LPPROC_THREAD_ATTRIBUTE_LIST _list{ nullptr };
        };

        class ConsoleModeGuard final
        {
        public:
            ConsoleModeGuard(core::HandleView input, core::HandleView output) noexcept :
                _input(input),
                _output(output)
            {
                _input_is_console = (::GetConsoleMode(_input.get(), &_input_mode) != FALSE);
                _output_is_console = (::GetConsoleMode(_output.get(), &_output_mode) != FALSE);
                _output_cp = ::GetConsoleOutputCP();
                _input_cp = ::GetConsoleCP();

                if (_input_is_console)
                {
                    DWORD raw_input = _input_mode;
                    raw_input &= ~(ENABLE_LINE_INPUT | ENABLE_ECHO_INPUT);
                    raw_input |= ENABLE_EXTENDED_FLAGS | ENABLE_WINDOW_INPUT | ENABLE_PROCESSED_INPUT;
                    ::SetConsoleMode(_input.get(), raw_input);
                    ::SetConsoleCP(CP_UTF8);
                }

                if (_output_is_console)
                {
                    DWORD vt_output = _output_mode | ENABLE_VIRTUAL_TERMINAL_PROCESSING;
                    vt_output |= DISABLE_NEWLINE_AUTO_RETURN;
                    ::SetConsoleMode(_output.get(), vt_output);
                    ::SetConsoleOutputCP(CP_UTF8);
                }
            }

            ~ConsoleModeGuard() noexcept
            {
                if (_input_is_console)
                {
                    ::SetConsoleMode(_input.get(), _input_mode);
                    ::SetConsoleCP(_input_cp);
                }
                if (_output_is_console)
                {
                    ::SetConsoleMode(_output.get(), _output_mode);
                    ::SetConsoleOutputCP(_output_cp);
                }
            }

        private:
            core::HandleView _input{};
            core::HandleView _output{};
            DWORD _input_mode{ 0 };
            DWORD _output_mode{ 0 };
            UINT _input_cp{ 0 };
            UINT _output_cp{ 0 };
            bool _input_is_console{ false };
            bool _output_is_console{ false };
        };

        [[nodiscard]] std::expected<void, SessionError> create_pipe(core::UniqueHandle& read_end, core::UniqueHandle& write_end)
        {
            SECURITY_ATTRIBUTES security{};
            security.nLength = sizeof(security);
            security.lpSecurityDescriptor = nullptr;
            // `CreatePseudoConsole` spawns a conhost instance and inherits the
            // provided pipe handles. Mark the pipe handles inheritable so the
            // underlying ConPTY host can receive them.
            security.bInheritHandle = TRUE;

            if (::CreatePipe(read_end.put(), write_end.put(), &security, 0) == FALSE)
            {
                return std::unexpected(SessionError{
                    .context = L"CreatePipe failed",
                    .win32_error = ::GetLastError(),
                });
            }
            return {};
        }

        [[nodiscard]] std::expected<core::UniqueHandle, SessionError> spawn_process_with_pseudoconsole(
            const std::wstring& command_line,
            ProcThreadAttributeList& attributes)
        {
            const auto expanded_command_line = [&]() -> std::expected<std::wstring, SessionError> {
                const DWORD required = ::ExpandEnvironmentStringsW(command_line.c_str(), nullptr, 0);
                if (required == 0)
                {
                    return std::unexpected(SessionError{
                        .context = L"ExpandEnvironmentStringsW failed",
                        .win32_error = ::GetLastError(),
                    });
                }

                std::wstring expanded(required, L'\0');
                const DWORD written = ::ExpandEnvironmentStringsW(command_line.c_str(), expanded.data(), required);
                if (written == 0 || written > required)
                {
                    return std::unexpected(SessionError{
                        .context = L"ExpandEnvironmentStringsW write failed",
                        .win32_error = ::GetLastError(),
                    });
                }
                expanded.resize(written - 1);
                return expanded;
            }();
            if (!expanded_command_line)
            {
                return std::unexpected(expanded_command_line.error());
            }

            std::vector<wchar_t> mutable_command_line(
                expanded_command_line->begin(),
                expanded_command_line->end());
            mutable_command_line.push_back(L'\0');

            STARTUPINFOEXW startup{};
            startup.StartupInfo.cb = sizeof(startup);
            startup.lpAttributeList = attributes.get();

            const HANDLE host_stdin = ::GetStdHandle(STD_INPUT_HANDLE);
            const HANDLE host_stdout = ::GetStdHandle(STD_OUTPUT_HANDLE);
            const HANDLE host_stderr = ::GetStdHandle(STD_ERROR_HANDLE);

            // Force explicit stdio selection for the child process. When the
            // host itself is launched with redirected stdio pipes, inheriting
            // those handles into the ConPTY client makes it observe redirected
            // stdin/stdout and bypass the console input path. We keep stdout/
            // stderr directed to the host so output remains observable, but we
            // intentionally omit stdin to route input through the pseudo
            // console transport.
            startup.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
            startup.StartupInfo.hStdInput = nullptr;
            startup.StartupInfo.hStdOutput = host_stdout;
            startup.StartupInfo.hStdError = host_stderr;

            if (host_stdin != nullptr && host_stdin != INVALID_HANDLE_VALUE)
            {
                (void)::SetHandleInformation(host_stdin, HANDLE_FLAG_INHERIT, 0);
            }

            PROCESS_INFORMATION info{};
            const BOOL created = ::CreateProcessW(
                nullptr,
                mutable_command_line.data(),
                nullptr,
                nullptr,
                TRUE,
                EXTENDED_STARTUPINFO_PRESENT,
                nullptr,
                nullptr,
                &startup.StartupInfo,
                &info);
            if (created == FALSE)
            {
                return std::unexpected(SessionError{
                    .context = L"CreateProcessW with pseudo console failed",
                    .win32_error = ::GetLastError(),
                });
            }

            core::UniqueHandle process(info.hProcess);
            core::UniqueHandle thread(info.hThread);
            OC_ASSERT(process.valid());
            OC_ASSERT(thread.valid());

            return process;
        }

        [[nodiscard]] std::expected<core::UniqueHandle, SessionError> spawn_process_inherited_stdio(
            const std::wstring& command_line,
            core::HandleView std_in,
            core::HandleView std_out)
        {
            const auto expanded_command_line = [&]() -> std::expected<std::wstring, SessionError> {
                const DWORD required = ::ExpandEnvironmentStringsW(command_line.c_str(), nullptr, 0);
                if (required == 0)
                {
                    return std::unexpected(SessionError{
                        .context = L"ExpandEnvironmentStringsW failed",
                        .win32_error = ::GetLastError(),
                    });
                }

                std::wstring expanded(required, L'\0');
                const DWORD written = ::ExpandEnvironmentStringsW(command_line.c_str(), expanded.data(), required);
                if (written == 0 || written > required)
                {
                    return std::unexpected(SessionError{
                        .context = L"ExpandEnvironmentStringsW write failed",
                        .win32_error = ::GetLastError(),
                    });
                }
                expanded.resize(written - 1);
                return expanded;
            }();
            if (!expanded_command_line)
            {
                return std::unexpected(expanded_command_line.error());
            }

            std::vector<wchar_t> mutable_command_line(
                expanded_command_line->begin(),
                expanded_command_line->end());
            mutable_command_line.push_back(L'\0');

            STARTUPINFOW startup{};
            startup.cb = sizeof(startup);
            startup.dwFlags = STARTF_USESTDHANDLES;
            startup.hStdInput = std_in.get();
            startup.hStdOutput = std_out.get();
            startup.hStdError = std_out.get();

            PROCESS_INFORMATION info{};
            const BOOL created = ::CreateProcessW(
                nullptr,
                mutable_command_line.data(),
                nullptr,
                nullptr,
                TRUE,
                0,
                nullptr,
                nullptr,
                &startup,
                &info);
            if (created == FALSE)
            {
                return std::unexpected(SessionError{
                    .context = L"CreateProcessW inherited stdio failed",
                    .win32_error = ::GetLastError(),
                });
            }

            core::UniqueHandle process(info.hProcess);
            core::UniqueHandle thread(info.hThread);
            OC_ASSERT(process.valid());
            OC_ASSERT(thread.valid());
            return process;
        }

        [[nodiscard]] std::expected<void, SessionError> write_bytes(core::HandleView target, const char* data, const DWORD size)
        {
            if (size == 0)
            {
                return {};
            }

            DWORD total_written = 0;
            while (total_written < size)
            {
                DWORD written = 0;
                const BOOL success = ::WriteFile(target.get(), data + total_written, size - total_written, &written, nullptr);
                if (success == FALSE)
                {
                    return std::unexpected(SessionError{
                        .context = L"WriteFile failed",
                        .win32_error = ::GetLastError(),
                    });
                }
                total_written += written;
            }
            return {};
        }

        [[nodiscard]] std::expected<void, SessionError> send_initial_terminal_handshake(
            const SessionOptions& options,
            logging::Logger& logger)
        {
            if (!options.host_output)
            {
                return {};
            }

            if (options.inherit_cursor)
            {
                // Cursor Position Report (DSR CPR): mirrors conhost conpty startup behavior.
                constexpr char request_cursor[] = "\x1b[6n";
                auto result = write_bytes(options.host_output, request_cursor, static_cast<DWORD>(sizeof(request_cursor) - 1));
                if (!result)
                {
                    return std::unexpected(result.error());
                }
            }

            // DA1 + focus mode + win32-input-mode, matching conhost VT startup negotiation.
            constexpr char handshake[] = "\x1b[c\x1b[?1004h\x1b[?9001h";
            auto result = write_bytes(options.host_output, handshake, static_cast<DWORD>(sizeof(handshake) - 1));
            if (!result)
            {
                return std::unexpected(result.error());
            }

            if (!options.text_measurement.empty())
            {
                logger.log(logging::LogLevel::debug, L"Requested text measurement mode: {}", options.text_measurement);
            }

            return {};
        }

        [[nodiscard]] std::expected<void, SessionError> pump_output_from_pseudoconsole(
            core::HandleView pty_output_read,
            core::HandleView host_output,
            bool& had_data,
            bool& broken_pipe)
        {
            had_data = false;
            broken_pipe = false;

            DWORD available = 0;
            if (::PeekNamedPipe(pty_output_read.get(), nullptr, 0, nullptr, &available, nullptr) == FALSE)
            {
                const DWORD error = ::GetLastError();
                if (error == ERROR_BROKEN_PIPE)
                {
                    broken_pipe = true;
                    return {};
                }
                return std::unexpected(SessionError{
                    .context = L"PeekNamedPipe on pseudo console output failed",
                    .win32_error = error,
                });
            }

            if (available == 0)
            {
                return {};
            }

            std::array<char, 8192> buffer{};
            const DWORD to_read = available < buffer.size() ? available : static_cast<DWORD>(buffer.size());
            DWORD read = 0;
            if (::ReadFile(pty_output_read.get(), buffer.data(), to_read, &read, nullptr) == FALSE)
            {
                const DWORD error = ::GetLastError();
                if (error == ERROR_BROKEN_PIPE)
                {
                    broken_pipe = true;
                    return {};
                }
                return std::unexpected(SessionError{
                    .context = L"ReadFile on pseudo console output failed",
                    .win32_error = error,
                });
            }

            had_data = read > 0;
            if (read > 0)
            {
                auto write_result = write_bytes(host_output, buffer.data(), read);
                if (!write_result)
                {
                    return std::unexpected(write_result.error());
                }
            }

            return {};
        }

        [[nodiscard]] std::expected<void, SessionError> pump_console_input_to_pseudoconsole(
            core::HandleView host_input,
            core::UniqueHandle& pty_input_write,
            UniquePseudoConsole& pseudo_console,
            bool& had_data,
            logging::Logger& logger)
        {
            had_data = false;
            if (!pty_input_write.valid())
            {
                return {};
            }

            DWORD console_mode = 0;
            if (::GetConsoleMode(host_input.get(), &console_mode) != FALSE)
            {
                DWORD pending = 0;
                if (::GetNumberOfConsoleInputEvents(host_input.get(), &pending) == FALSE)
                {
                    return std::unexpected(SessionError{
                        .context = L"GetNumberOfConsoleInputEvents failed",
                        .win32_error = ::GetLastError(),
                    });
                }

                while (pending > 0)
                {
                    INPUT_RECORD record{};
                    DWORD read = 0;
                    if (::ReadConsoleInputW(host_input.get(), &record, 1, &read) == FALSE)
                    {
                        return std::unexpected(SessionError{
                            .context = L"ReadConsoleInputW failed",
                            .win32_error = ::GetLastError(),
                        });
                    }

                    if (record.EventType == KEY_EVENT)
                    {
                        auto encoded = KeyInputEncoder::encode(record.Event.KeyEvent);
                        if (!encoded.empty())
                        {
                            auto write_result = write_bytes(core::HandleView(pty_input_write.get()), encoded.data(), static_cast<DWORD>(encoded.size()));
                            if (!write_result)
                            {
                                return std::unexpected(write_result.error());
                            }
                            had_data = true;
                        }
                    }
                    else if (record.EventType == WINDOW_BUFFER_SIZE_EVENT && pseudo_console.valid())
                    {
                        const COORD size = record.Event.WindowBufferSizeEvent.dwSize;
                        ::ResizePseudoConsole(pseudo_console.get(), size);
                        had_data = true;
                    }

                    --pending;
                }

                return {};
            }

            const DWORD input_type = ::GetFileType(host_input.get());
            if (input_type == FILE_TYPE_PIPE)
            {
                DWORD available = 0;
                if (::PeekNamedPipe(host_input.get(), nullptr, 0, nullptr, &available, nullptr) == FALSE)
                {
                    const DWORD error = ::GetLastError();
                    if (error == ERROR_BROKEN_PIPE)
                    {
                        logger.log(logging::LogLevel::debug, L"Host input pipe reached EOF");
                        return {};
                    }

                    logger.log(logging::LogLevel::debug, L"PeekNamedPipe(host_input) failed (error={})", error);
                    return std::unexpected(SessionError{
                        .context = L"PeekNamedPipe on host input pipe failed",
                        .win32_error = error,
                    });
                }
                if (available == 0)
                {
                    return {};
                }

                std::array<char, 4096> buffer{};
                const DWORD to_read = available < buffer.size() ? available : static_cast<DWORD>(buffer.size());
                DWORD read = 0;
                if (::ReadFile(host_input.get(), buffer.data(), to_read, &read, nullptr) == FALSE)
                {
                    const DWORD error = ::GetLastError();
                    if (error == ERROR_BROKEN_PIPE)
                    {
                        logger.log(logging::LogLevel::debug, L"Host input pipe closed during read");
                        return {};
                    }
                    return std::unexpected(SessionError{
                        .context = L"ReadFile from host input pipe failed",
                        .win32_error = error,
                    });
                }
                if (read > 0)
                {
                    auto write_result = write_bytes(core::HandleView(pty_input_write.get()), buffer.data(), read);
                    if (!write_result)
                    {
                        return std::unexpected(write_result.error());
                    }
                    logger.log(logging::LogLevel::debug, L"Forwarded {} bytes of host input to pseudo console", static_cast<unsigned>(read));
                    had_data = true;
                }
            }

            return {};
        }

        [[nodiscard]] COORD calculate_initial_size(const SessionOptions& options)
        {
            COORD size{};
            size.X = options.width > 0 ? options.width : 120;
            size.Y = options.height > 0 ? options.height : 40;

            if (options.host_output)
            {
                CONSOLE_SCREEN_BUFFER_INFO info{};
                if (::GetConsoleScreenBufferInfo(options.host_output.get(), &info))
                {
                    const short width = static_cast<short>(info.srWindow.Right - info.srWindow.Left + 1);
                    const short height = static_cast<short>(info.srWindow.Bottom - info.srWindow.Top + 1);
                    if (width > 0 && height > 0)
                    {
                        size.X = width;
                        size.Y = height;
                    }
                }
            }

            return size;
        }

        [[nodiscard]] std::expected<DWORD, SessionError> run_with_pseudoconsole(const SessionOptions& options, logging::Logger& logger)
        {
            core::UniqueHandle pty_input_read;
            core::UniqueHandle pty_input_write;
            core::UniqueHandle pty_output_read;
            core::UniqueHandle pty_output_write;

            if (auto pipe_result = create_pipe(pty_input_read, pty_input_write); !pipe_result)
            {
                return std::unexpected(pipe_result.error());
            }
            if (auto pipe_result = create_pipe(pty_output_read, pty_output_write); !pipe_result)
            {
                return std::unexpected(pipe_result.error());
            }

            const COORD initial_size = calculate_initial_size(options);
            HPCON raw_pseudo_console = nullptr;
            const HRESULT pty_result = ::CreatePseudoConsole(
                initial_size,
                pty_input_read.get(),
                pty_output_write.get(),
                0,
                &raw_pseudo_console);
            if (FAILED(pty_result))
            {
                return std::unexpected(SessionError{
                    .context = L"CreatePseudoConsole failed",
                    .win32_error = static_cast<DWORD>(HRESULT_CODE(pty_result)),
                });
            }

            UniquePseudoConsole pseudo_console(raw_pseudo_console);
            // Keep the ConPTY host-side pipe endpoints alive for the lifetime
            // of the pseudo console. Some Windows builds rely on these handles
            // remaining open even after `CreatePseudoConsole` returns.

            // The ConPTY transport pipes must not leak into the client process.
            // We rely on `bInheritHandles=FALSE` for the client CreateProcessW
            // call, but additionally clear inheritance to keep behavior
            // deterministic when the host itself was launched with inheritable
            // handles.
            (void)::SetHandleInformation(pty_input_write.get(), HANDLE_FLAG_INHERIT, 0);
            (void)::SetHandleInformation(pty_output_read.get(), HANDLE_FLAG_INHERIT, 0);
            (void)::SetHandleInformation(pty_input_read.get(), HANDLE_FLAG_INHERIT, 0);
            (void)::SetHandleInformation(pty_output_write.get(), HANDLE_FLAG_INHERIT, 0);

            auto attributes_result = ProcThreadAttributeList::create();
            if (!attributes_result)
            {
                return std::unexpected(attributes_result.error());
            }
            ProcThreadAttributeList attributes = std::move(attributes_result.value());
            if (auto update_result = attributes.set_pseudo_console(pseudo_console.get()); !update_result)
            {
                return std::unexpected(update_result.error());
            }

            auto process_result = spawn_process_with_pseudoconsole(options.client_command_line, attributes);
            if (!process_result)
            {
                return std::unexpected(process_result.error());
            }
            core::UniqueHandle process = std::move(process_result.value());

            ConsoleModeGuard mode_guard(options.host_input, options.host_output);
            logger.log(
                logging::LogLevel::debug,
                L"Pseudo console started (size={}x{}, headless={}, conpty={})",
                static_cast<int>(initial_size.X),
                static_cast<int>(initial_size.Y),
                options.headless ? 1 : 0,
                options.in_conpty_mode ? 1 : 0);

            if (auto handshake = send_initial_terminal_handshake(options, logger); !handshake)
            {
                return std::unexpected(handshake.error());
            }

            bool signaled_termination = false;
            for (;;)
            {
                if (options.signal_handle)
                {
                    const DWORD signal_state = ::WaitForSingleObject(options.signal_handle.get(), 0);
                    if (signal_state == WAIT_OBJECT_0)
                    {
                        ::TerminateProcess(process.get(), ERROR_CANCELLED);
                        signaled_termination = true;
                    }
                }

                bool had_output = false;
                bool broken_pipe = false;
                if (auto pump_result = pump_output_from_pseudoconsole(
                        core::HandleView(pty_output_read.get()),
                        options.host_output,
                        had_output,
                        broken_pipe);
                    !pump_result)
                {
                    return std::unexpected(pump_result.error());
                }

                bool had_input = false;
                if (!signaled_termination)
                {
                    if (auto input_result = pump_console_input_to_pseudoconsole(
                             options.host_input,
                             pty_input_write,
                             pseudo_console,
                             had_input,
                             logger);
                         !input_result)
                    {
                        return std::unexpected(input_result.error());
                    }
                }

                const DWORD process_state = ::WaitForSingleObject(process.get(), 0);
                const bool process_exited = process_state == WAIT_OBJECT_0;
                if (process_exited && (!had_output || broken_pipe))
                {
                    break;
                }

                if (!had_output && !had_input)
                {
                    ::Sleep(1);
                }
            }

            DWORD exit_code = 0;
            if (::GetExitCodeProcess(process.get(), &exit_code) == FALSE)
            {
                return std::unexpected(SessionError{
                    .context = L"GetExitCodeProcess failed",
                    .win32_error = ::GetLastError(),
                });
            }

            return exit_code;
        }

        [[nodiscard]] std::expected<DWORD, SessionError> run_with_inherited_stdio(const SessionOptions& options)
        {
            auto process_result = spawn_process_inherited_stdio(
                options.client_command_line,
                options.host_input,
                options.host_output);
            if (!process_result)
            {
                return std::unexpected(process_result.error());
            }

            core::UniqueHandle process = std::move(process_result.value());

            if (options.signal_handle)
            {
                const DWORD wait_result = core::wait_for_two_objects(
                    process.view(),
                    options.signal_handle,
                    false,
                    INFINITE);
                if (wait_result == WAIT_OBJECT_0 + 1)
                {
                    ::TerminateProcess(process.get(), ERROR_CANCELLED);
                }
                else if (wait_result != WAIT_OBJECT_0)
                {
                    return std::unexpected(SessionError{
                        .context = L"WaitForMultipleObjects failed",
                        .win32_error = ::GetLastError(),
                    });
                }
            }
            else
            {
                const DWORD wait_result = ::WaitForSingleObject(process.get(), INFINITE);
                if (wait_result != WAIT_OBJECT_0)
                {
                    return std::unexpected(SessionError{
                        .context = L"WaitForSingleObject failed",
                        .win32_error = ::GetLastError(),
                    });
                }
            }

            DWORD exit_code = 0;
            if (::GetExitCodeProcess(process.get(), &exit_code) == FALSE)
            {
                return std::unexpected(SessionError{
                    .context = L"GetExitCodeProcess failed",
                    .win32_error = ::GetLastError(),
                });
            }

            return exit_code;
        }
    }

        std::expected<DWORD, SessionError> Session::run(const SessionOptions& options, logging::Logger& logger) noexcept
        {
        if (!options.create_server_handle)
        {
            auto server_result = ServerHandleValidator::validate(options.server_handle);
            if (!server_result)
            {
                return std::unexpected(SessionError{
                    .context = L"Server handle validation failed",
                    .win32_error = server_result.error().win32_error,
                });
            }
        }

        auto signal_result = ServerHandleValidator::validate_optional_signal(options.signal_handle);
        if (!signal_result)
        {
            return std::unexpected(SessionError{
                .context = L"Signal handle validation failed",
                .win32_error = signal_result.error().win32_error,
            });
        }

        if (!options.create_server_handle)
        {
            if (!options.client_command_line.empty())
            {
                logger.log(
                    logging::LogLevel::warning,
                    L"Ignoring client command line because --server startup is active: {}",
                    options.client_command_line);
            }

            if (!options.headless && !options.in_conpty_mode)
            {
                logger.log(logging::LogLevel::info, L"Starting windowed server host");
                return run_windowed_server(options, logger);
            }

            auto server_result = condrv::ConDrvServer::run(
                options.server_handle,
                options.signal_handle,
                options.host_input,
                options.host_output,
                core::HandleView{},
                logger);
            if (!server_result)
            {
                return std::unexpected(SessionError{
                    .context = server_result.error().context,
                    .win32_error = server_result.error().win32_error,
                });
            }

            return server_result.value();
        }

        if (options.client_command_line.empty())
        {
            // Compatibility behavior: server-only startup may run without a
            // direct client command line. If a signal handle is available,
            // block until signaled.
            if (options.signal_handle)
            {
                const DWORD wait_result = ::WaitForSingleObject(options.signal_handle.get(), INFINITE);
                if (wait_result != WAIT_OBJECT_0)
                {
                    return std::unexpected(SessionError{
                        .context = L"WaitForSingleObject on signal handle failed",
                        .win32_error = ::GetLastError(),
                    });
                }
            }
            return DWORD{ 0 };
        }

        // Prefer pseudo console whenever we are in conpty-like modes.
        const bool use_pseudoconsole = options.headless || options.in_conpty_mode;
        if (use_pseudoconsole)
        {
            return run_with_pseudoconsole(options, logger);
        }

        return run_with_inherited_stdio(options);
    }
}
