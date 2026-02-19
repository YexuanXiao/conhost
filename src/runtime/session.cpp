#include "runtime/session.hpp"

#include "condrv/condrv_device_comm.hpp"
#include "condrv/condrv_server.hpp"
#include "core/assert.hpp"
#include "core/handle_view.hpp"
#include "core/host_signals.hpp"
#include "core/unique_handle.hpp"
#include "core/win32_handle.hpp"
#include "core/win32_wait.hpp"
#include "renderer/window_host.hpp"
#include "runtime/host_signal_input_thread.hpp"
#include "runtime/key_input_encoder.hpp"
#include "runtime/console_connection_policy.hpp"
#include "runtime/server_handle_validator.hpp"
#include "runtime/signal_pipe_monitor.hpp"
#include "runtime/window_input_sink.hpp"

#include "IConsoleHandoff.h"

#include <Windows.h>
#include <conmsgl1.h>
#include <objbase.h>
#include <winternl.h>

#include <array>
#include <cstddef>
#include <limits>
#include <memory>
#include <optional>
#include <string_view>
#include <vector>

// `runtime/session.cpp` is the central runtime implementation for `openconsole_new`.
//
// High-level responsibilities (see `new/docs/architecture.md`):
// - Create-server ("EXE mode") startup:
//   - Choose legacy vs replacement behavior (LaunchPolicy) (in `Application`).
//   - Create a console server instance (ConDrv) and launch the requested client.
//   - If requested/necessary, host the client under a pseudo console (ConPTY),
//     forwarding bytes between the host and the client.
// - Server-handle ("--server") startup:
//   - Validate the inherited server/signal handles.
//   - Host the ConDrv server loop (`condrv::ConDrvServer`) either:
//     - with a classic window renderer (interactive), or
//     - in headless mode with host I/O pipes.
// - Default-terminal delegation (windowed "--server"):
//   - Probe `HKCU\\Console\\%%Startup\\DelegationConsole` for an out-of-proc COM
//     handler implementing `IConsoleHandoff`.
//   - If delegation succeeds, do not create a classic window. Instead, remain
//     alive for PID continuity and relay privileged host-control requests from
//     the delegated UI host via a "host signal" pipe.
//
// The implementation mirrors the upstream structure but is intentionally split
// into small, testable pieces (RAII wrappers, no raw HANDLE ownership in call
// sites). It is acceptable for the replacement to omit some of the upstream's
// historical workarounds as long as observable behavior remains compatible on
// Windows 10/11.
//
// See also:
// - `new/docs/conhost_source_architecture.md`
// - `new/docs/conhost_module_partition.md`
// - `new/docs/conhost_behavior_imitation_matrix.md`

namespace oc::runtime
{
    namespace
    {
        constexpr CLSID k_clsid_default{};
        constexpr CLSID k_clsid_conhost = {
            0xb23d10c0, 0xe52e, 0x411e, { 0x9d, 0x5b, 0xc0, 0x9f, 0xdf, 0x70, 0x9c, 0x7d }
        };

        constexpr std::wstring_view k_startup_key = L"Console\\%%Startup";
        constexpr std::wstring_view k_delegation_console_value = L"DelegationConsole";

        [[nodiscard]] bool guid_equal(const GUID& left, const GUID& right) noexcept
        {
            return ::InlineIsEqualGUID(left, right) != FALSE;
        }

        [[nodiscard]] DWORD to_win32_error_from_hresult(const HRESULT hr) noexcept
        {
            const DWORD code = static_cast<DWORD>(HRESULT_CODE(hr));
            return code == 0 ? ERROR_GEN_FAILURE : code;
        }

        // Private `user32!ConsoleControl` helper used to honor host-signal requests
        // coming from a delegated/default terminal.
        //
        // openconsole_new (in `--server` startup mode) reads the host-signal pipe and
        // must perform privileged console operations (like EndTask) on behalf of the
        // delegated UI host. The inbox conhost does the same.
        enum class ConsoleControlCommand : DWORD
        {
            reserved1 = 0,
            notify_console_application = 1,
            reserved2 = 2,
            set_caret_info = 3,
            reserved3 = 4,
            set_foreground = 5,
            set_window_owner = 6,
            end_task = 7,
        };

        struct ConsoleProcessInfo final
        {
            DWORD process_id{};
            DWORD flags{};
        };

        struct ConsoleEndTask final
        {
            HANDLE process_id{};
            HWND hwnd{};
            ULONG console_event_code{};
            ULONG console_flags{};
        };

        inline constexpr DWORD cpi_newprocesswindow = 0x0001u;

        using ConsoleControlFn = NTSTATUS(WINAPI*)(DWORD command, void* information, DWORD length);
        using NtOpenFileFn = NTSTATUS(NTAPI*)(
            PHANDLE file_handle,
            ACCESS_MASK desired_access,
            POBJECT_ATTRIBUTES object_attributes,
            PIO_STATUS_BLOCK io_status_block,
            ULONG share_access,
            ULONG open_options);
        using RtlNtStatusToDosErrorFn = ULONG(WINAPI*)(NTSTATUS status);

        [[nodiscard]] ConsoleControlFn resolve_console_control() noexcept
        {
            const HMODULE user32 = ::GetModuleHandleW(L"user32.dll");
            if (user32 == nullptr)
            {
                return nullptr;
            }

            return reinterpret_cast<ConsoleControlFn>(::GetProcAddress(user32, "ConsoleControl"));
        }

        [[nodiscard]] RtlNtStatusToDosErrorFn resolve_rtl_nt_status_to_dos_error() noexcept
        {
            const HMODULE ntdll = ::GetModuleHandleW(L"ntdll.dll");
            if (ntdll == nullptr)
            {
                return nullptr;
            }

            return reinterpret_cast<RtlNtStatusToDosErrorFn>(::GetProcAddress(ntdll, "RtlNtStatusToDosError"));
        }

        [[nodiscard]] NtOpenFileFn resolve_nt_open_file() noexcept
        {
            const HMODULE ntdll = ::GetModuleHandleW(L"ntdll.dll");
            if (ntdll == nullptr)
            {
                return nullptr;
            }

            return reinterpret_cast<NtOpenFileFn>(::GetProcAddress(ntdll, "NtOpenFile"));
        }

        [[nodiscard]] DWORD ntstatus_to_win32_error(const NTSTATUS status, const RtlNtStatusToDosErrorFn converter) noexcept
        {
            if (converter == nullptr)
            {
                return ERROR_GEN_FAILURE;
            }

            const DWORD error = static_cast<DWORD>(converter(status));
            return error == 0 ? ERROR_GEN_FAILURE : error;
        }

        [[nodiscard]] std::expected<core::UniqueHandle, SessionError> open_server_relative_file(
            const core::HandleView server_handle,
            const NtOpenFileFn nt_open_file,
            const RtlNtStatusToDosErrorFn rtl_nt_status_to_dos_error,
            const std::wstring_view child_name,
            const ACCESS_MASK desired_access,
            const ULONG open_options) noexcept
        {
            if (!server_handle)
            {
                return std::unexpected(SessionError{
                    .context = L"Server handle was invalid while opening server-relative path",
                    .win32_error = ERROR_INVALID_HANDLE,
                });
            }
            if (nt_open_file == nullptr || rtl_nt_status_to_dos_error == nullptr)
            {
                return std::unexpected(SessionError{
                    .context = L"NTDLL helpers were unavailable while opening server-relative path",
                    .win32_error = ERROR_PROC_NOT_FOUND,
                });
            }
            if (child_name.size() > (std::numeric_limits<USHORT>::max() / sizeof(wchar_t)))
            {
                return std::unexpected(SessionError{
                    .context = L"Server-relative path was too long",
                    .win32_error = ERROR_FILENAME_EXCED_RANGE,
                });
            }

            std::wstring child;
            try
            {
                child.assign(child_name);
                child.push_back(L'\0');
            }
            catch (...)
            {
                return std::unexpected(SessionError{
                    .context = L"Failed to allocate server-relative child path buffer",
                    .win32_error = ERROR_OUTOFMEMORY,
                });
            }

            UNICODE_STRING unicode_name{};
            unicode_name.Buffer = child.data();
            unicode_name.Length = static_cast<USHORT>(child_name.size() * sizeof(wchar_t));
            unicode_name.MaximumLength = static_cast<USHORT>(unicode_name.Length + sizeof(wchar_t));

            OBJECT_ATTRIBUTES object_attributes{};
            InitializeObjectAttributes(
                &object_attributes,
                &unicode_name,
                OBJ_CASE_INSENSITIVE,
                server_handle.get(),
                nullptr);

            IO_STATUS_BLOCK io_status{};
            HANDLE opened = nullptr;
            const NTSTATUS status = nt_open_file(
                &opened,
                desired_access,
                &object_attributes,
                &io_status,
                FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                open_options);
            if (status < 0)
            {
                return std::unexpected(SessionError{
                    .context = L"NtOpenFile failed for server-relative path",
                    .win32_error = ntstatus_to_win32_error(status, rtl_nt_status_to_dos_error),
                });
            }

            return core::UniqueHandle(opened);
        }

        void notify_console_application_best_effort(
            const ConsoleControlFn console_control,
            const RtlNtStatusToDosErrorFn rtl_nt_status_to_dos_error,
            logging::Logger& logger,
            const DWORD process_id) noexcept
        {
            if (console_control == nullptr || process_id == 0)
            {
                return;
            }

            ConsoleProcessInfo info{};
            info.process_id = process_id;
            info.flags = cpi_newprocesswindow;

            const NTSTATUS status = console_control(
                static_cast<DWORD>(ConsoleControlCommand::notify_console_application),
                &info,
                static_cast<DWORD>(sizeof(info)));
            if (status < 0)
            {
                const DWORD error = ntstatus_to_win32_error(status, rtl_nt_status_to_dos_error);
                try
                {
                    logger.log(
                        logging::LogLevel::debug,
                        L"ConsoleControl(NotifyConsoleApplication, pid={}) failed (ntstatus=0x{:08X}, error={})",
                        process_id,
                        static_cast<unsigned>(status),
                        error);
                }
                catch (...)
                {
                }
            }
            else
            {
                try
                {
                    logger.log(
                        logging::LogLevel::debug,
                        L"ConsoleControl(NotifyConsoleApplication) succeeded (pid={})",
                        process_id);
                }
                catch (...)
                {
                }
            }
        }

        void end_task_best_effort(
            const ConsoleControlFn console_control,
            const RtlNtStatusToDosErrorFn rtl_nt_status_to_dos_error,
            logging::Logger& logger,
            const DWORD process_id,
            const DWORD event_type,
            const DWORD ctrl_flags) noexcept
        {
            if (process_id == 0)
            {
                return;
            }

            bool ended = false;
            if (console_control != nullptr)
            {
                ConsoleEndTask params{};
                params.process_id = reinterpret_cast<HANDLE>(static_cast<ULONG_PTR>(process_id));
                params.hwnd = nullptr;
                params.console_event_code = event_type;
                params.console_flags = ctrl_flags;

                const NTSTATUS status = console_control(
                    static_cast<DWORD>(ConsoleControlCommand::end_task),
                    &params,
                    static_cast<DWORD>(sizeof(params)));
                if (status >= 0)
                {
                    ended = true;
                    try
                    {
                        logger.log(
                            logging::LogLevel::debug,
                            L"ConsoleControl(EndTask) succeeded (pid={}, event={}, flags={})",
                            process_id,
                            event_type,
                            ctrl_flags);
                    }
                    catch (...)
                    {
                    }
                }
                else
                {
                    const DWORD error = ntstatus_to_win32_error(status, rtl_nt_status_to_dos_error);
                    try
                    {
                        logger.log(
                            logging::LogLevel::debug,
                            L"ConsoleControl(EndTask, pid={}) failed (ntstatus=0x{:08X}, error={}); falling back to TerminateProcess",
                            process_id,
                            static_cast<unsigned>(status),
                            error);
                    }
                    catch (...)
                    {
                    }
                }
            }

            if (!ended)
            {
                core::UniqueHandle process(::OpenProcess(PROCESS_TERMINATE | SYNCHRONIZE, FALSE, process_id));
                if (!process.valid())
                {
                    try
                    {
                        logger.log(
                            logging::LogLevel::debug,
                            L"OpenProcess(PROCESS_TERMINATE) failed for EndTask fallback (pid={}, error={})",
                            process_id,
                            ::GetLastError());
                    }
                    catch (...)
                    {
                    }
                    return;
                }

                if (::TerminateProcess(process.get(), ERROR_CANCELLED) == FALSE)
                {
                    try
                    {
                        logger.log(
                            logging::LogLevel::debug,
                            L"TerminateProcess failed for EndTask fallback (pid={}, error={})",
                            process_id,
                            ::GetLastError());
                    }
                    catch (...)
                    {
                    }
                }
                else
                {
                    try
                    {
                        logger.log(
                            logging::LogLevel::debug,
                            L"TerminateProcess fallback succeeded for EndTask (pid={})",
                            process_id);
                    }
                    catch (...)
                    {
                    }
                }
            }
        }

        class UniqueRegistryKey final
        {
        public:
            UniqueRegistryKey() noexcept = default;

            explicit UniqueRegistryKey(HKEY value) noexcept :
                _value(value)
            {
            }

            ~UniqueRegistryKey() noexcept
            {
                reset();
            }

            UniqueRegistryKey(const UniqueRegistryKey&) = delete;
            UniqueRegistryKey& operator=(const UniqueRegistryKey&) = delete;

            UniqueRegistryKey(UniqueRegistryKey&& other) noexcept :
                _value(other.release())
            {
            }

            UniqueRegistryKey& operator=(UniqueRegistryKey&& other) noexcept
            {
                if (this != &other)
                {
                    reset(other.release());
                }
                return *this;
            }

            [[nodiscard]] HKEY get() const noexcept
            {
                return _value;
            }

            [[nodiscard]] HKEY* put() noexcept
            {
                reset();
                return &_value;
            }

            [[nodiscard]] bool valid() const noexcept
            {
                return _value != nullptr;
            }

            HKEY release() noexcept
            {
                HKEY detached = _value;
                _value = nullptr;
                return detached;
            }

            void reset(HKEY replacement = nullptr) noexcept
            {
                if (_value != nullptr)
                {
                    (void)::RegCloseKey(_value);
                }
                _value = replacement;
            }

        private:
            HKEY _value{ nullptr };
        };

        class CoInitScope final
        {
        public:
            explicit CoInitScope(const HRESULT result) noexcept :
                _result(result)
            {
            }

            ~CoInitScope() noexcept
            {
                if (SUCCEEDED(_result))
                {
                    ::CoUninitialize();
                }
            }

            [[nodiscard]] HRESULT result() const noexcept
            {
                return _result;
            }

        private:
            HRESULT _result{ E_FAIL };
        };

        template<typename T>
        class UniqueComInterface final
        {
        public:
            UniqueComInterface() noexcept = default;

            ~UniqueComInterface() noexcept
            {
                reset();
            }

            UniqueComInterface(const UniqueComInterface&) = delete;
            UniqueComInterface& operator=(const UniqueComInterface&) = delete;

            UniqueComInterface(UniqueComInterface&& other) noexcept :
                _value(other.release())
            {
            }

            UniqueComInterface& operator=(UniqueComInterface&& other) noexcept
            {
                if (this != &other)
                {
                    reset(other.release());
                }
                return *this;
            }

            [[nodiscard]] T* get() const noexcept
            {
                return _value;
            }

            [[nodiscard]] T** put() noexcept
            {
                reset();
                return &_value;
            }

            void reset(T* replacement = nullptr) noexcept
            {
                if (_value != nullptr)
                {
                    _value->Release();
                }
                _value = replacement;
            }

            T* release() noexcept
            {
                T* detached = _value;
                _value = nullptr;
                return detached;
            }

        private:
            T* _value{ nullptr };
        };

        struct PipePair final
        {
            core::UniqueHandle read_end;
            core::UniqueHandle write_end;
        };

        [[nodiscard]] std::expected<PipePair, SessionError> create_signal_pipe_pair() noexcept
        {
            SECURITY_ATTRIBUTES security{};
            security.nLength = sizeof(security);
            security.lpSecurityDescriptor = nullptr;
            security.bInheritHandle = FALSE;

            PipePair pair{};
            if (::CreatePipe(pair.read_end.put(), pair.write_end.put(), &security, 0) == FALSE)
            {
                return std::unexpected(SessionError{
                    .context = L"CreatePipe failed for default-terminal handoff signal pipe",
                    .win32_error = ::GetLastError(),
                });
            }

            return pair;
        }

        // Determines whether the current process is running in an interactive user
        // session suitable for UI hosting (visible window station, non-session-0).
        //
        // Upstream reference: `src/server/IoDispatchers.cpp::_isInteractiveUserSession`.
        [[nodiscard]] bool is_interactive_user_session() noexcept
        {
            DWORD session_id{};
            if (::ProcessIdToSessionId(::GetCurrentProcessId(), &session_id) != FALSE && session_id == 0)
            {
                return false;
            }

            const HWINSTA winsta = ::GetProcessWindowStation();
            if (winsta != nullptr)
            {
                USEROBJECTFLAGS flags{};
                if (::GetUserObjectInformationW(winsta, UOI_FLAGS, &flags, sizeof(flags), nullptr) != FALSE)
                {
                    return (flags.dwFlags & WSF_VISIBLE) != 0;
                }
            }

            // If we cannot determine visibility, assume interactive to preserve
            // compatibility with the inbox host.
            return true;
        }

        [[nodiscard]] std::optional<ConsoleConnectionPolicyInput> try_read_connect_policy_input(
            const condrv::ConDrvDeviceComm& comm,
            const condrv::IoPacket& packet,
            logging::Logger& logger) noexcept
        {
            if (packet.descriptor.function != condrv::console_io_connect)
            {
                return std::nullopt;
            }

            if (packet.descriptor.input_size < sizeof(CONSOLE_SERVER_MSG))
            {
                logger.log(
                    logging::LogLevel::debug,
                    L"CONNECT input buffer was too small for CONSOLE_SERVER_MSG; treating as unknown. bytes={}",
                    static_cast<unsigned long>(packet.descriptor.input_size));
                return std::nullopt;
            }

            CONSOLE_SERVER_MSG msg{};
            condrv::IoOperation op{};
            op.identifier = packet.descriptor.identifier;
            op.buffer.offset = 0;
            op.buffer.data = &msg;
            op.buffer.size = sizeof(msg);

            if (auto read = comm.read_input(op); !read)
            {
                logger.log(
                    logging::LogLevel::warning,
                    L"Failed to read CONNECT input buffer; treating as unknown connect policy. context='{}', error={}",
                    read.error().context,
                    read.error().win32_error);
                return std::nullopt;
            }

            return ConsoleConnectionPolicyInput{
                .console_app = msg.ConsoleApp != FALSE,
                .window_visible = msg.WindowVisible != FALSE,
                .startup_flags = msg.StartupFlags,
                .show_window = msg.ShowWindow,
            };
        }

        [[nodiscard]] std::expected<std::optional<CLSID>, SessionError> resolve_console_handoff_clsid() noexcept
        {
            UniqueRegistryKey startup_key{};
            const LONG open_status = ::RegOpenKeyExW(
                HKEY_CURRENT_USER,
                k_startup_key.data(),
                0,
                KEY_QUERY_VALUE,
                startup_key.put());
            if (open_status == ERROR_FILE_NOT_FOUND)
            {
                return std::optional<CLSID>{};
            }
            if (open_status != ERROR_SUCCESS)
            {
                return std::unexpected(SessionError{
                    .context = L"RegOpenKeyExW failed for HKCU\\Console\\%%Startup",
                    .win32_error = static_cast<DWORD>(open_status),
                });
            }

            DWORD value_type = 0;
            DWORD value_bytes = 0;
            const LONG size_status = ::RegQueryValueExW(
                startup_key.get(),
                k_delegation_console_value.data(),
                nullptr,
                &value_type,
                nullptr,
                &value_bytes);
            if (size_status == ERROR_FILE_NOT_FOUND)
            {
                return std::optional<CLSID>{};
            }
            if (size_status != ERROR_SUCCESS)
            {
                return std::unexpected(SessionError{
                    .context = L"RegQueryValueExW size query failed for DelegationConsole",
                    .win32_error = static_cast<DWORD>(size_status),
                });
            }
            if (value_type != REG_SZ || value_bytes < sizeof(wchar_t))
            {
                return std::unexpected(SessionError{
                    .context = L"DelegationConsole value had an unexpected format",
                    .win32_error = ERROR_BAD_FORMAT,
                });
            }

            std::vector<wchar_t> text;
            try
            {
                text.assign((value_bytes / sizeof(wchar_t)) + 1, L'\0');
            }
            catch (...)
            {
                return std::unexpected(SessionError{
                    .context = L"Failed to allocate buffer for DelegationConsole registry value",
                    .win32_error = ERROR_OUTOFMEMORY,
                });
            }

            const LONG read_status = ::RegQueryValueExW(
                startup_key.get(),
                k_delegation_console_value.data(),
                nullptr,
                &value_type,
                reinterpret_cast<BYTE*>(text.data()),
                &value_bytes);
            if (read_status != ERROR_SUCCESS)
            {
                return std::unexpected(SessionError{
                    .context = L"RegQueryValueExW read failed for DelegationConsole",
                    .win32_error = static_cast<DWORD>(read_status),
                });
            }
            text.back() = L'\0';

            CLSID handoff_clsid{};
            const HRESULT parse_hr = ::CLSIDFromString(text.data(), &handoff_clsid);
            if (FAILED(parse_hr))
            {
                return std::unexpected(SessionError{
                    .context = L"CLSIDFromString failed for DelegationConsole",
                    .win32_error = to_win32_error_from_hresult(parse_hr),
                });
            }

            if (guid_equal(handoff_clsid, k_clsid_default) || guid_equal(handoff_clsid, k_clsid_conhost))
            {
                return std::optional<CLSID>{};
            }

            return handoff_clsid;
        }

        [[nodiscard]] std::expected<core::UniqueHandle, SessionError> invoke_console_handoff(
            const CLSID& handoff_clsid,
            const core::HandleView server_handle,
            const core::HandleView input_event,
            const CONSOLE_PORTABLE_ATTACH_MSG& attach_msg,
            const core::HandleView signal_pipe,
            const core::HandleView inbox_process,
            logging::Logger& logger) noexcept
        {
            logger.log(logging::LogLevel::info, L"Invoking IConsoleHandoff::EstablishHandoff");
            const CoInitScope coinit(::CoInitializeEx(nullptr, COINIT_MULTITHREADED));
            if (FAILED(coinit.result()))
            {
                return std::unexpected(SessionError{
                    .context = L"CoInitializeEx failed for console handoff",
                    .win32_error = to_win32_error_from_hresult(coinit.result()),
                });
            }

            UniqueComInterface<IConsoleHandoff> handoff{};
            const HRESULT create_hr = ::CoCreateInstance(
                handoff_clsid,
                nullptr,
                CLSCTX_LOCAL_SERVER,
                __uuidof(IConsoleHandoff),
                reinterpret_cast<void**>(handoff.put()));
            if (FAILED(create_hr))
            {
                return std::unexpected(SessionError{
                    .context = L"CoCreateInstance failed for IConsoleHandoff",
                    .win32_error = to_win32_error_from_hresult(create_hr),
                });
            }

            HANDLE delegated_process = nullptr;
            const HRESULT handoff_hr = handoff.get()->EstablishHandoff(
                server_handle.get(),
                input_event.get(),
                &attach_msg,
                signal_pipe.get(),
                inbox_process.get(),
                &delegated_process);
            if (FAILED(handoff_hr))
            {
                return std::unexpected(SessionError{
                    .context = L"IConsoleHandoff::EstablishHandoff failed",
                    .win32_error = to_win32_error_from_hresult(handoff_hr),
                });
            }

            core::UniqueHandle process(delegated_process);
            if (!process.valid())
            {
                return std::unexpected(SessionError{
                    .context = L"IConsoleHandoff returned an invalid process handle",
                    .win32_error = ERROR_INVALID_HANDLE,
                });
            }

            const DWORD delegated_pid = ::GetProcessId(process.get());
            if (delegated_pid != 0)
            {
                logger.log(
                    logging::LogLevel::info,
                    L"IConsoleHandoff::EstablishHandoff succeeded (delegated_host_pid={})",
                    delegated_pid);
            }
            else
            {
                logger.log(
                    logging::LogLevel::debug,
                    L"IConsoleHandoff::EstablishHandoff succeeded, delegated host PID unavailable (GetProcessId error={})",
                    ::GetLastError());
            }

            return process;
        }

        struct WindowedServerContext final
        {
            core::HandleView server_handle{};
            core::HandleView stop_event{};
            logging::Logger* logger{};
            HWND window{};
            std::shared_ptr<view::PublishedScreenBuffer> published_screen;
            core::UniqueHandle input_available_event;
            core::UniqueHandle host_input;
            std::optional<condrv::IoPacket> initial_packet;

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

            std::expected<DWORD, condrv::ServerError> result;
            if (context->initial_packet.has_value())
            {
                result = condrv::ConDrvServer::run_with_handoff(
                    context->server_handle,
                    context->stop_event,
                    context->input_available_event.view(),
                    context->host_input.view(), // windowed mode: input is fed from the classic window
                    core::HandleView{}, // windowed mode: output is rendered from published snapshots (no host output pipe)
                    core::HandleView{},
                    context->initial_packet.value(),
                    *context->logger,
                    context->published_screen,
                    context->window);
            }
            else
            {
                result = condrv::ConDrvServer::run(
                    context->server_handle,
                    context->stop_event,
                    context->host_input.view(), // windowed mode: input is fed from the classic window
                    core::HandleView{}, // windowed mode: output is rendered from published snapshots (no host output pipe)
                    core::HandleView{},
                    *context->logger,
                    context->published_screen,
                    context->window);
            }

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
            logging::Logger& logger,
            core::UniqueHandle input_available_event,
            std::optional<condrv::IoPacket> initial_packet,
            const int show_command) noexcept
        {
            auto stop_event = core::create_event(true, false, nullptr);
            if (!stop_event)
            {
                return std::unexpected(SessionError{
                    .context = L"CreateEventW failed for windowed server stop event",
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
                return std::unexpected(SessionError{
                    .context = L"Failed to allocate published screen buffer",
                    .win32_error = ERROR_OUTOFMEMORY,
                });
            }

            core::UniqueHandle host_input_read;
            core::UniqueHandle host_input_write;
            {
                SECURITY_ATTRIBUTES security{};
                security.nLength = sizeof(security);
                security.lpSecurityDescriptor = nullptr;
                security.bInheritHandle = FALSE;

                constexpr DWORD pipe_buffer_bytes = 64 * 1024;
                if (::CreatePipe(host_input_read.put(), host_input_write.put(), &security, pipe_buffer_bytes) == FALSE)
                {
                    return std::unexpected(SessionError{
                        .context = L"CreatePipe failed for windowed input pipe",
                        .win32_error = ::GetLastError(),
                    });
                }
            }

            std::shared_ptr<renderer::IWindowInputSink> input_sink;
            try
            {
                input_sink = std::make_shared<WindowInputPipeSink>(std::move(host_input_write));
            }
            catch (...)
            {
                return std::unexpected(SessionError{
                    .context = L"Failed to allocate windowed input sink",
                    .win32_error = ERROR_OUTOFMEMORY,
                });
            }

            renderer::WindowHostConfig window_config{};
            window_config.title = L"openconsole_new";
            window_config.show_command = show_command;
            window_config.published_screen = published_screen;
            window_config.input_sink = std::move(input_sink);
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
            server_context->input_available_event = std::move(input_available_event);
            server_context->host_input = std::move(host_input_read);
            server_context->initial_packet = std::move(initial_packet);

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
            // The ConDrv worker thread spends most of its time blocked in a synchronous
            // `IOCTL_CONDRV_READ_IO`. Ensure the stop request is observed promptly even
            // if the worker's internal signal monitor is unavailable or delayed.
            (void)::CancelSynchronousIo(server_thread.get());
            if (options.server_handle)
            {
                (void)::CancelIoEx(options.server_handle.get(), nullptr);
            }

            constexpr DWORD worker_shutdown_timeout_ms = 5'000;
            const DWORD wait_result = ::WaitForSingleObject(server_thread.get(), worker_shutdown_timeout_ms);
            if (wait_result == WAIT_TIMEOUT)
            {
                // This should not happen: closing the window must terminate the hosting process.
                // If the worker thread does not exit, force termination rather than leaving a
                // headless process behind.
                logger.log(logging::LogLevel::error, L"ConDrv windowed server worker did not exit within {}ms; forcing process exit", worker_shutdown_timeout_ms);
                ::ExitProcess(ERROR_TIMEOUT);
            }
            if (wait_result != WAIT_OBJECT_0)
            {
                const DWORD error = ::GetLastError();
                logger.log(logging::LogLevel::error, L"WaitForSingleObject failed for ConDrv windowed server worker (error={}); forcing process exit", error);
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

        [[nodiscard]] std::expected<DWORD, SessionError> run_windowed_server(
            const SessionOptions& options,
            logging::Logger& logger,
            const int show_command) noexcept
        {
            return run_windowed_server(options, logger, core::UniqueHandle{}, std::optional<condrv::IoPacket>{}, show_command);
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
            security.bInheritHandle = FALSE;

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
            ProcThreadAttributeList& attributes,
            logging::Logger& logger)
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

            logger.log(
                logging::LogLevel::info,
                L"Launching client process (ConPTY): command_line={}",
                expanded_command_line.value());

            std::vector<wchar_t> mutable_command_line(
                expanded_command_line->begin(),
                expanded_command_line->end());
            mutable_command_line.push_back(L'\0');

            STARTUPINFOEXW startup{};
            startup.StartupInfo.cb = sizeof(startup);
            startup.lpAttributeList = attributes.get();
            // Ensure the ConPTY client sees console-backed standard handles.
            //
            // In headless hosting, `openconsole_new` is typically launched with
            // pipe-like stdio (connected to a terminal). If the client inherits
            // those handles, it will observe redirected stdin/stdout and many
            // console applications will treat the session as non-interactive,
            // bypassing console I/O entirely.
            //
            // Passing null standard handles while the pseudo console attribute
            // is active lets the ConPTY infrastructure provide the appropriate
            // console handles for stdin/stdout/stderr.
            startup.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
            startup.StartupInfo.hStdInput = nullptr;
            startup.StartupInfo.hStdOutput = nullptr;
            startup.StartupInfo.hStdError = nullptr;

            PROCESS_INFORMATION info{};
            const BOOL created = ::CreateProcessW(
                nullptr,
                mutable_command_line.data(),
                nullptr,
                nullptr,
                FALSE,
                EXTENDED_STARTUPINFO_PRESENT,
                nullptr,
                nullptr,
                &startup.StartupInfo,
                &info);
            if (created == FALSE)
            {
                const DWORD create_error = ::GetLastError();
                logger.log(
                    logging::LogLevel::error,
                    L"CreateProcessW failed for ConPTY client launch: error={}, command_line={}",
                    create_error,
                    expanded_command_line.value());
                return std::unexpected(SessionError{
                    .context = L"CreateProcessW with pseudo console failed",
                    .win32_error = create_error,
                });
            }

            core::UniqueHandle process(info.hProcess);
            core::UniqueHandle thread(info.hThread);
            OC_ASSERT(process.valid());
            OC_ASSERT(thread.valid());
            logger.log(
                logging::LogLevel::info,
                L"Client process launched (ConPTY): pid={}",
                info.dwProcessId);

            return process;
        }

        [[nodiscard]] std::expected<core::UniqueHandle, SessionError> spawn_process_inherited_stdio(
            const std::wstring& command_line,
            core::HandleView std_in,
            core::HandleView std_out,
            logging::Logger& logger)
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

            logger.log(
                logging::LogLevel::info,
                L"Launching client process (inherited stdio): command_line={}",
                expanded_command_line.value());

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
                const DWORD create_error = ::GetLastError();
                logger.log(
                    logging::LogLevel::error,
                    L"CreateProcessW failed for inherited-stdio client launch: error={}, command_line={}",
                    create_error,
                    expanded_command_line.value());
                return std::unexpected(SessionError{
                    .context = L"CreateProcessW inherited stdio failed",
                    .win32_error = create_error,
                });
            }

            core::UniqueHandle process(info.hProcess);
            core::UniqueHandle thread(info.hThread);
            OC_ASSERT(process.valid());
            OC_ASSERT(thread.valid());
            logger.log(
                logging::LogLevel::info,
                L"Client process launched (inherited stdio): pid={}",
                info.dwProcessId);
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

        [[nodiscard]] std::expected<void, SessionError> send_headless_server_terminal_handshake(
            const SessionOptions& options,
            logging::Logger& logger) noexcept
        {
            // In server-handle headless startup (`--server` + `--headless`), this process *is* the
            // console server (the "ConPTY conhost"). It is responsible for negotiating the
            // terminal-side input encoding used by ConPTY.
            //
            // Without the win32-input-mode negotiation, many terminal hosts will fall back to
            // classic VT key sequences. While the replacement supports a minimal subset of those
            // sequences, richer key metadata (virtual keys, scan codes, modifier state) is required
            // for many console applications that use `ReadConsoleInput`.
            //
            // Upstream conhost uses DA1 + focus events + win32-input-mode as part of the initial
            // VT startup handshake. Emit the same control sequences here so that headless server
            // startups remain interactive.
            if (!options.host_output)
            {
                logger.log(logging::LogLevel::debug, L"Skipping VT handshake for headless server startup: no host output handle");
                return {};
            }

            // Restrict handshake emission to pipe-backed output handles. When running as a classic
            // windowed host, stdout is a console screen buffer handle and the downstream consumer
            // is not a VT terminal.
            const DWORD output_type = ::GetFileType(options.host_output.get());
            if (output_type != FILE_TYPE_PIPE)
            {
                logger.log(
                    logging::LogLevel::debug,
                    L"Skipping VT handshake for headless server startup: host output is not a pipe (type={})",
                    output_type);
                return {};
            }

            logger.log(logging::LogLevel::debug, L"Emitting VT handshake for headless server startup");

            // DA1 + focus mode + win32-input-mode, matching conhost VT startup negotiation.
            //
            // Note: `--inheritcursor` is intentionally not handled here yet. Cursor inheritance
            // requires a DSR CPR query/response exchange that must not leak into client input.
            // That negotiation is safe in the ConPTY-hosting path because the system conhost
            // consumes the response, but in server-handle mode we need dedicated handling.
            constexpr char handshake[] = "\x1b[c\x1b[?1004h\x1b[?9001h";
            auto result = write_bytes(options.host_output, handshake, static_cast<DWORD>(sizeof(handshake) - 1));
            if (!result)
            {
                return std::unexpected(result.error());
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
            bool& host_input_pipe_eof,
            logging::Logger& logger)
        {
            had_data = false;
            if (!pty_input_write.valid())
            {
                return {};
            }
            if (host_input_pipe_eof)
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
                        if (!host_input_pipe_eof)
                        {
                            logger.log(logging::LogLevel::debug, L"Host input pipe reached EOF");
                        }
                        host_input_pipe_eof = true;
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
                        if (!host_input_pipe_eof)
                        {
                            logger.log(logging::LogLevel::debug, L"Host input pipe reached EOF");
                        }
                        host_input_pipe_eof = true;
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

            auto process_result = spawn_process_with_pseudoconsole(options.client_command_line, attributes, logger);
            if (!process_result)
            {
                return std::unexpected(process_result.error());
            }
            core::UniqueHandle process = std::move(process_result.value());
            // These ends are owned by the pseudo console host after creation;
            // close our references once the client is started so broken pipe
            // detection behaves as expected. (Matches Microsoft guidance.)
            pty_input_read.reset();
            pty_output_write.reset();

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
            bool host_input_pipe_eof = false;
            bool process_exited = false;
            bool draining_after_exit = false;
            ULONGLONG drain_start_tick = 0;
            static constexpr ULONGLONG kDrainTimeoutMs = 2'000;
            for (;;)
            {
                if (options.signal_handle)
                {
                    const DWORD signal_state = ::WaitForSingleObject(options.signal_handle.get(), 0);
                    if (signal_state == WAIT_OBJECT_0)
                    {
                        if (::TerminateProcess(process.get(), ERROR_CANCELLED) == FALSE)
                        {
                            logger.log(
                                logging::LogLevel::warning,
                                L"TerminateProcess failed after signal-handle shutdown request (error={})",
                                ::GetLastError());
                        }
                        else
                        {
                            logger.log(
                                logging::LogLevel::info,
                                L"Signal handle requested shutdown; terminated ConPTY client process");
                        }
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
                if (!process_exited && !signaled_termination && !host_input_pipe_eof)
                {
                    if (auto input_result = pump_console_input_to_pseudoconsole(
                             options.host_input,
                             pty_input_write,
                             pseudo_console,
                             had_input,
                             host_input_pipe_eof,
                             logger);
                         !input_result)
                    {
                        return std::unexpected(input_result.error());
                    }
                }

                const DWORD process_state = ::WaitForSingleObject(process.get(), 0);
                if (process_state == WAIT_OBJECT_0)
                {
                    process_exited = true;
                }
                else if (process_state == WAIT_FAILED)
                {
                    return std::unexpected(SessionError{
                        .context = L"WaitForSingleObject on ConPTY client failed",
                        .win32_error = ::GetLastError(),
                    });
                }

                if (process_exited)
                {
                    if (broken_pipe)
                    {
                        break;
                    }

                    if (had_output)
                    {
                        draining_after_exit = false;
                    }
                    else if (!draining_after_exit)
                    {
                        draining_after_exit = true;
                        drain_start_tick = ::GetTickCount64();
                    }
                    else if ((::GetTickCount64() - drain_start_tick) >= kDrainTimeoutMs)
                    {
                        logger.log(
                            logging::LogLevel::debug,
                            L"ConPTY output drain timed out after {}ms; continuing shutdown",
                            static_cast<unsigned long long>(kDrainTimeoutMs));
                        break;
                    }
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

            logger.log(logging::LogLevel::info, L"ConPTY client process exited with code {}", exit_code);
            return exit_code;
        }

        [[nodiscard]] std::expected<DWORD, SessionError> run_with_inherited_stdio(const SessionOptions& options, logging::Logger& logger)
        {
            auto process_result = spawn_process_inherited_stdio(
                options.client_command_line,
                options.host_input,
                options.host_output,
                logger);
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
                    if (::TerminateProcess(process.get(), ERROR_CANCELLED) == FALSE)
                    {
                        logger.log(
                            logging::LogLevel::warning,
                            L"TerminateProcess failed after inherited-stdio signal shutdown request (error={})",
                            ::GetLastError());
                    }
                    else
                    {
                        logger.log(
                            logging::LogLevel::info,
                            L"Signal handle requested shutdown; terminated inherited-stdio client process");
                    }
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

            logger.log(logging::LogLevel::info, L"Inherited-stdio client process exited with code {}", exit_code);
            return exit_code;
        }
    }

        std::expected<DWORD, SessionError> Session::run(const SessionOptions& options, logging::Logger& logger) noexcept
        {
        // `Session::run` is intentionally a single, explicit decision tree.
        //
        // The upstream conhost implementation has a comparable "routing" role
        // (EXE mode vs --server vs -Embedding, ConPTY vs classic, etc.). Here we
        // keep the branching readable by:
        // - validating inherited handles up-front,
        // - keeping each branch in a compact helper (`run_with_pseudoconsole`,
        //   `run_windowed_server`, `run_with_inherited_stdio`),
        // - storing only the small pieces of state that need to survive across
        //   fallbacks (for example `initial_packet` when we already consumed a
        //   `READ_IO` during a delegation probe).
        //
        // See `new/docs/conhost_behavior_imitation_matrix.md` for the current
        // parity status of each startup mode.
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
            // Classic conhost server-handle startup: the OS or a parent process
            // already created the ConDrv server object and started us with
            // `--server 0x...`.
            //
            // This mode is used both for classic windowed hosting and for
            // headless hosting behind a third-party terminal.
            if (!options.client_command_line.empty())
            {
                logger.log(
                    logging::LogLevel::warning,
                    L"Ignoring client command line because --server startup is active: {}",
                    options.client_command_line);
            }

            if (!options.headless && !options.in_conpty_mode)
            {
                // Windowed server hosting. Before creating a classic window,
                // attempt default-terminal delegation ("defterm") so a third-
                // party UI host can take over interactive rendering.
                //
                // Upstream reference: `src/server/IoDispatchers.cpp::attemptHandoff`.
                // See also: `new/docs/conhost_behavior_imitation_matrix.md`.
                std::optional<condrv::IoPacket> initial_packet;
                core::UniqueHandle input_available_event;
                const bool interactive_session = is_interactive_user_session();
                ConsoleConnectionPolicyDecision policy_decision{
                    .create_window = interactive_session,
                    .show_command = SW_SHOWDEFAULT,
                    .attempt_default_terminal_handoff = interactive_session && !options.force_no_handoff,
                };

                // Read the initial CONNECT packet up-front so we can honor CREATE_NO_WINDOW / SW_HIDE
                // before creating a classic window and before attempting defterm delegation.
                auto device_comm = condrv::ConDrvDeviceComm::from_server_handle(options.server_handle);
                if (!device_comm)
                {
                    logger.log(
                        logging::LogLevel::warning,
                        L"ConDrvDeviceComm::from_server_handle failed; falling back to default window policy. context='{}', error={}",
                        device_comm.error().context,
                        device_comm.error().win32_error);
                }
                 else
                 {
                     // The driver expects the server to provide an event that clients implicitly wait on
                     // when input is unavailable. This is a manual-reset event because multiple clients
                     // can be unblocked by a single input arrival.
                     auto input_event = core::create_event(true, false, nullptr);
                     if (!input_event)
                     {
                         logger.log(
                             logging::LogLevel::warning,
                             L"CreateEventW failed for input-available event; continuing without early server information. error={}",
                             input_event.error());
                     }
                     else
                     {
                         input_available_event = std::move(input_event.value());
                        const auto server_info = device_comm->set_server_information(input_available_event.view());
                        if (!server_info)
                        {
                            logger.log(
                                logging::LogLevel::warning,
                                L"ConDrvDeviceComm::set_server_information failed; continuing. context='{}', error={}",
                                server_info.error().context,
                                server_info.error().win32_error);
                        }
                        else
                        {
                            condrv::IoPacket packet{};
                            if (auto read = device_comm->read_io(nullptr, packet); !read)
                            {
                                logger.log(
                                    logging::LogLevel::warning,
                                    L"Initial ConDrv read_io failed; falling back to default window policy. context='{}', error={}",
                                    read.error().context,
                                    read.error().win32_error);
                            }
                            else
                            {
                                initial_packet.emplace(packet);
                                if (auto connect = try_read_connect_policy_input(*device_comm, packet, logger))
                                {
                                    policy_decision = ConsoleConnectionPolicy::decide(
                                        *connect,
                                        options.force_no_handoff,
                                        options.create_server_handle,
                                        options.headless,
                                        options.in_conpty_mode,
                                        interactive_session);
                                }
                            }
                        }
                    }
                }

                if (!policy_decision.create_window)
                {
                    logger.log(logging::LogLevel::info, L"Starting server host without a window (no visible console window requested)");

                    if (initial_packet.has_value())
                    {
                        auto server_result = condrv::ConDrvServer::run_with_handoff(
                            options.server_handle,
                            options.signal_handle,
                            input_available_event.view(),
                            core::HandleView{},
                            core::HandleView{},
                            core::HandleView{},
                            initial_packet.value(),
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

                    auto server_result = condrv::ConDrvServer::run(
                        options.server_handle,
                        options.signal_handle,
                        core::HandleView{},
                        core::HandleView{},
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

                if (policy_decision.attempt_default_terminal_handoff)
                {
                    auto delegation_target = resolve_console_handoff_clsid();
                    if (!delegation_target)
                    {
                         logger.log(
                             logging::LogLevel::warning,
                             L"Default-terminal delegation probe failed; falling back to classic window. context='{}', error={}",
                             delegation_target.error().context,
                             delegation_target.error().win32_error);
                     }
                    else if (delegation_target->has_value())
                    {
                        if (!device_comm)
                        {
                            logger.log(
                                logging::LogLevel::warning,
                                L"Default-terminal delegation skipped because ConDrv device comm was unavailable; falling back to classic window");
                        }
                        else if (!initial_packet.has_value())
                        {
                            logger.log(
                                logging::LogLevel::warning,
                                L"Default-terminal delegation skipped because no initial ConDrv packet was available; falling back to classic window");
                        }
                        else
                        {
                            if (!input_available_event.valid())
                            {
                                auto input_event = core::create_event(true, false, nullptr);
                                if (!input_event)
                                {
                                    logger.log(
                                        logging::LogLevel::warning,
                                        L"Default-terminal delegation input event creation failed; falling back to classic window. error={}",
                                        input_event.error());
                                }
                                else
                                {
                                    input_available_event = std::move(input_event.value());
                                }
                            }

                             if (!input_available_event.valid())
                             {
                                 logger.log(
                                     logging::LogLevel::warning,
                                     L"Default-terminal delegation skipped because input event creation failed; falling back to classic window");
                             }
                            else
                            {
                                const auto packet = initial_packet.value();

                                        // Minimal attach payload (stable fields only) used by
                                        // `IConsoleHandoff::EstablishHandoff`.
                                        CONSOLE_PORTABLE_ATTACH_MSG attach{};
                                        attach.IdLowPart = packet.descriptor.identifier.LowPart;
                                        attach.IdHighPart = packet.descriptor.identifier.HighPart;
                                        attach.Process = static_cast<ULONG64>(packet.descriptor.process);
                                        attach.Object = static_cast<ULONG64>(packet.descriptor.object);
                                        attach.Function = packet.descriptor.function;
                                        attach.InputSize = packet.descriptor.input_size;
                                        attach.OutputSize = packet.descriptor.output_size;

                                        // Host-signal pipe: delegated UI host writes requests (EndTask,
                                        // NotifyApp, ...) and this inbox host reads and performs the
                                        // privileged operations on its behalf.
                                        //
                                        // We pass the write end to the delegated host and keep the read
                                        // end for ourselves.
                                        auto signal_pipe_pair = create_signal_pipe_pair();
                                        if (!signal_pipe_pair)
                                        {
                                            logger.log(
                                                logging::LogLevel::warning,
                                                L"Default-terminal delegation signal pipe creation failed; falling back to classic window. error={}",
                                                signal_pipe_pair.error().win32_error);
                                        }
                                        else
                                        {
                                            // Provide a real handle to this process. The delegated host
                                            // can use it to detect when the inbox host has exited.
                                            auto inbox_process = core::duplicate_current_process(
                                                PROCESS_DUP_HANDLE | PROCESS_QUERY_LIMITED_INFORMATION | SYNCHRONIZE,
                                                false);
                                            if (!inbox_process)
                                            {
                                                logger.log(
                                                    logging::LogLevel::warning,
                                                    L"Default-terminal delegation could not duplicate inbox process handle; falling back to classic window. error={}",
                                                    inbox_process.error());
                                            }
                                            else
                                            {
                                                // Perform the actual COM handoff. On success, COM returns
                                                // a handle to the delegated host process so we can wait
                                                // for it and keep PID continuity for clients that expect
                                                // the original host process to remain alive.
                                                auto delegated_process = invoke_console_handoff(
                                                    delegation_target->value(),
                                                    options.server_handle,
                                                    input_available_event.view(),
                                                    attach,
                                                    signal_pipe_pair->write_end.view(),
                                                    inbox_process->view(),
                                                    logger);
                                                if (delegated_process)
                                                {
                                                    const DWORD client_pid = attach.Process > std::numeric_limits<DWORD>::max()
                                                        ? 0
                                                        : static_cast<DWORD>(attach.Process);
                                                    const DWORD delegated_pid = ::GetProcessId(delegated_process->get());
                                                    if (delegated_pid != 0)
                                                    {
                                                        logger.log(
                                                            logging::LogLevel::info,
                                                            L"Default-terminal delegation established; delegated_host_pid={}, client_pid={}, waiting for delegated host exit",
                                                            delegated_pid,
                                                            client_pid);
                                                    }
                                                    else
                                                    {
                                                        logger.log(
                                                            logging::LogLevel::info,
                                                            L"Default-terminal delegation established; delegated_host_pid=<unavailable>, client_pid={}, waiting for delegated host exit",
                                                            client_pid);
                                                    }

                                                    // The delegated host owns the input event after a successful handoff.
                                                    input_available_event.reset();

                                                    struct DelegatedHostSignalTarget final : HostSignalTarget
                                                    {
                                                        logging::Logger& logger;
                                                        ConsoleControlFn console_control{};
                                                        RtlNtStatusToDosErrorFn rtl_nt_status_to_dos_error{};
                                                        DWORD fallback_pid{};

                                                        explicit DelegatedHostSignalTarget(
                                                            logging::Logger& logger_ref,
                                                            const ConsoleControlFn console_control_ref,
                                                            const RtlNtStatusToDosErrorFn rtl_nt_status_to_dos_error_ref,
                                                            const DWORD fallback_pid_ref) noexcept :
                                                            logger(logger_ref),
                                                            console_control(console_control_ref),
                                                            rtl_nt_status_to_dos_error(rtl_nt_status_to_dos_error_ref),
                                                            fallback_pid(fallback_pid_ref)
                                                        {
                                                        }

                                                        void notify_console_application(const DWORD process_id) noexcept override
                                                        {
                                                            logger.log(
                                                                logging::LogLevel::debug,
                                                                L"Host-signal request: notify_console_application(pid={})",
                                                                process_id);
                                                            notify_console_application_best_effort(
                                                                console_control,
                                                                rtl_nt_status_to_dos_error,
                                                                logger,
                                                                process_id);
                                                        }

                                                        void set_foreground(const DWORD /*process_handle_value*/, const bool /*is_foreground*/) noexcept override
                                                        {
                                                            // GH#13211 parity: upstream ignores this (legacy callers only).
                                                            logger.log(
                                                                logging::LogLevel::debug,
                                                                L"Host-signal request: set_foreground ignored for compatibility");
                                                        }

                                                        void end_task(const DWORD process_id, const DWORD event_type, const DWORD ctrl_flags) noexcept override
                                                        {
                                                            logger.log(
                                                                logging::LogLevel::debug,
                                                                L"Host-signal request: end_task(pid={}, event={}, flags={})",
                                                                process_id,
                                                                event_type,
                                                                ctrl_flags);
                                                            end_task_best_effort(
                                                                console_control,
                                                                rtl_nt_status_to_dos_error,
                                                                logger,
                                                                process_id,
                                                                event_type,
                                                                ctrl_flags);
                                                        }

                                                        void signal_pipe_disconnected() noexcept override
                                                        {
                                                            logger.log(
                                                                logging::LogLevel::info,
                                                                L"Host-signal pipe disconnected");
                                                            if (fallback_pid == 0)
                                                            {
                                                                return;
                                                            }

                                                            logger.log(
                                                                logging::LogLevel::info,
                                                                L"Host-signal disconnect fallback: end_task(pid={}, event={}, flags={})",
                                                                fallback_pid,
                                                                static_cast<unsigned long>(CTRL_CLOSE_EVENT),
                                                                static_cast<unsigned long>(core::console_ctrl_close_flag));
                                                            end_task_best_effort(
                                                                console_control,
                                                                rtl_nt_status_to_dos_error,
                                                                logger,
                                                                fallback_pid,
                                                                CTRL_CLOSE_EVENT,
                                                                core::console_ctrl_close_flag);
                                                        }
                                                    };

                                                    DelegatedHostSignalTarget signal_target(
                                                        logger,
                                                        resolve_console_control(),
                                                        resolve_rtl_nt_status_to_dos_error(),
                                                        client_pid);

                                                    // Start the host-signal reader thread. It duplicates the
                                                    // pipe handle, so it remains valid even if we reset our
                                                    // local `read_end` wrapper.
                                                    auto signal_input = HostSignalInputThread::start(
                                                        signal_pipe_pair->read_end.view(),
                                                        signal_target,
                                                        &logger);
                                                    if (!signal_input)
                                                    {
                                                        logger.log(
                                                            logging::LogLevel::warning,
                                                            L"Host signal input thread creation failed; continuing without host-signal handling. context='{}', error={}",
                                                            signal_input.error().context,
                                                            signal_input.error().win32_error);
                                                    }
                                                    else
                                                    {
                                                        // The host signal thread owns its own duplicated pipe handle.
                                                        signal_pipe_pair->read_end.reset();
                                                    }

                                                    // Allow the delegated host to observe pipe closure when it exits.
                                                    signal_pipe_pair->write_end.reset();

                                                    // Wait for the handoff lifetime to end.
                                                    //
                                                    // Upstream conhost exits after waiting on the delegated host
                                                    // process handle returned by `IConsoleHandoff::EstablishHandoff`.
                                                    //
                                                    // In practice, some delegation targets return a short-lived
                                                    // process handle (for example a broker process that spawns the
                                                    // real UI host and then terminates). Exiting at that point would
                                                    // tear down the console server and prevent the delegated UI from
                                                    // running the intended console application.
                                                    //
                                                    // We therefore treat the delegated process handle as an
                                                    // observation/logging source, but keep the inbox process alive
                                                    // until either:
                                                    // - the host-signal pipe reader terminates (writer side closed), or
                                                    // - a usable ConDrv server-relative `\\Reference` handle signals.
                                                    //
                                                    // If neither guard is available, we fall back to the upstream
                                                    // behavior and wait only on the delegated process handle.
                                                    std::optional<core::UniqueHandle> console_reference;
                                                    if (const auto nt_open_file = resolve_nt_open_file();
                                                        nt_open_file != nullptr)
                                                    {
                                                        if (auto reference = open_server_relative_file(
                                                                options.server_handle,
                                                                nt_open_file,
                                                                resolve_rtl_nt_status_to_dos_error(),
                                                                L"\\Reference",
                                                                GENERIC_READ | GENERIC_WRITE | SYNCHRONIZE,
                                                                FILE_SYNCHRONOUS_IO_NONALERT);
                                                            reference)
                                                        {
                                                            const DWORD state = ::WaitForSingleObject(reference->get(), 0);
                                                            if (state == WAIT_TIMEOUT)
                                                            {
                                                                console_reference.emplace(std::move(reference.value()));
                                                            }
                                                            else if (state == WAIT_OBJECT_0)
                                                            {
                                                                logger.log(
                                                                    logging::LogLevel::debug,
                                                                    L"ConDrv \\\\Reference handle was signaled immediately; ignoring it for delegation lifetime wait");
                                                            }
                                                            else if (state == WAIT_FAILED)
                                                            {
                                                                logger.log(
                                                                    logging::LogLevel::debug,
                                                                    L"WaitForSingleObject failed for ConDrv \\\\Reference handle; ignoring it for delegation lifetime wait (error={})",
                                                                    ::GetLastError());
                                                            }
                                                            else
                                                            {
                                                                logger.log(
                                                                    logging::LogLevel::debug,
                                                                    L"ConDrv \\\\Reference wait state was unexpected; ignoring it for delegation lifetime wait (state={})",
                                                                    static_cast<unsigned long>(state));
                                                            }
                                                        }
                                                        else
                                                        {
                                                            logger.log(
                                                                logging::LogLevel::debug,
                                                                L"Opening ConDrv \\\\Reference handle failed; ignoring it for delegation lifetime wait. context='{}', error={}",
                                                                reference.error().context,
                                                                reference.error().win32_error);
                                                        }
                                                    }

                                                    core::HandleView signal_thread_handle;
                                                    if (signal_input)
                                                    {
                                                        signal_thread_handle = signal_input->thread_handle();
                                                    }

                                                    bool have_guard = console_reference.has_value() || signal_thread_handle.valid();
                                                    bool observe_delegated = true;
                                                    bool delegated_signaled = false;
                                                    DWORD exit_code = 0;

                                                    for (;;)
                                                    {
                                                        std::array<HANDLE, 3> handles{};
                                                        std::array<unsigned char, 3> kinds{};
                                                        DWORD count = 0;

                                                        auto add_handle = [&](const HANDLE handle, const unsigned char kind) noexcept {
                                                            handles[count] = handle;
                                                            kinds[count] = kind;
                                                            ++count;
                                                        };

                                                        // Kinds:
                                                        // - 0: delegated host process handle
                                                        // - 1: ConDrv \\Reference lifetime handle
                                                        // - 2: host-signal input thread (pipe disconnect indicator)
                                                        if (observe_delegated)
                                                        {
                                                            add_handle(delegated_process->get(), 0);
                                                        }
                                                        if (console_reference)
                                                        {
                                                            add_handle(console_reference->get(), 1);
                                                        }
                                                        if (signal_thread_handle.valid())
                                                        {
                                                            add_handle(signal_thread_handle.get(), 2);
                                                        }

                                                        const DWORD wait_result = (count == 1)
                                                            ? ::WaitForSingleObject(handles[0], INFINITE)
                                                            : ::WaitForMultipleObjects(count, handles.data(), FALSE, INFINITE);

                                                        if (wait_result == WAIT_FAILED)
                                                        {
                                                            return std::unexpected(SessionError{
                                                                .context = L"WaitForMultipleObjects failed during delegation wait",
                                                                .win32_error = ::GetLastError(),
                                                            });
                                                        }

                                                        if (wait_result < WAIT_OBJECT_0 || wait_result >= WAIT_OBJECT_0 + count)
                                                        {
                                                            return std::unexpected(SessionError{
                                                                .context = L"WaitForMultipleObjects returned an unexpected result during delegation wait",
                                                                .win32_error = ::GetLastError(),
                                                            });
                                                        }

                                                        const DWORD index = wait_result - WAIT_OBJECT_0;
                                                        const unsigned char kind = kinds[index];

                                                        if (kind == 0)
                                                        {
                                                            delegated_signaled = true;
                                                            observe_delegated = false;

                                                            if (::GetExitCodeProcess(delegated_process->get(), &exit_code) == FALSE)
                                                            {
                                                                const DWORD error = ::GetLastError();
                                                                if (error == ERROR_ACCESS_DENIED)
                                                                {
                                                                    logger.log(
                                                                        logging::LogLevel::debug,
                                                                        L"Delegated host exit code unavailable (GetExitCodeProcess access denied); returning exit code 0.");
                                                                    exit_code = 0;
                                                                }
                                                                else
                                                                {
                                                                    return std::unexpected(SessionError{
                                                                        .context = L"GetExitCodeProcess failed for delegated host process",
                                                                        .win32_error = error,
                                                                    });
                                                                }
                                                            }

                                                            if (have_guard)
                                                            {
                                                                logger.log(
                                                                    logging::LogLevel::info,
                                                                    L"Delegated host process handle signaled; deferring exit until delegation lifetime guard ends");
                                                                continue;
                                                            }

                                                            if (delegated_pid != 0)
                                                            {
                                                                logger.log(
                                                                    logging::LogLevel::info,
                                                                    L"Delegated host process exited: pid={}, exit_code={}",
                                                                    delegated_pid,
                                                                    exit_code);
                                                            }
                                                            else
                                                            {
                                                                logger.log(
                                                                    logging::LogLevel::info,
                                                                    L"Delegated host process exited: pid=<unavailable>, exit_code={}",
                                                                    exit_code);
                                                            }
                                                            return exit_code;
                                                        }

                                                        if (kind == 1)
                                                        {
                                                            logger.log(
                                                                logging::LogLevel::info,
                                                                L"Delegation lifetime ended (ConDrv \\\\Reference signaled)");
                                                        }
                                                        else
                                                        {
                                                            logger.log(
                                                                logging::LogLevel::info,
                                                                L"Delegation lifetime ended (host-signal pipe closed)");
                                                        }

                                                        if (delegated_signaled)
                                                        {
                                                            if (delegated_pid != 0)
                                                            {
                                                                logger.log(
                                                                    logging::LogLevel::info,
                                                                    L"Delegated host process exited: pid={}, exit_code={}",
                                                                    delegated_pid,
                                                                    exit_code);
                                                            }
                                                            else
                                                            {
                                                                logger.log(
                                                                    logging::LogLevel::info,
                                                                    L"Delegated host process exited: pid=<unavailable>, exit_code={}",
                                                                    exit_code);
                                                            }
                                                            return exit_code;
                                                        }

                                                        // The delegation lifetime ended without us observing the
                                                        // delegated process handle. Return 0 to match the inbox host's
                                                        // ExitProcess(S_OK) behavior.
                                                        return 0;
                                                    }
                                                }

                                                logger.log(
                                                    logging::LogLevel::warning,
                                                    L"Default-terminal delegation failed; falling back to classic window. context='{}', error={}",
                                                    delegated_process.error().context,
                                                    delegated_process.error().win32_error);
                                            }
                                        }
                                    }
                                }
                            }
                        }

                logger.log(logging::LogLevel::info, L"Starting windowed server host");
                if (initial_packet.has_value())
                {
                    return run_windowed_server(
                        options,
                        logger,
                        std::move(input_available_event),
                        std::move(initial_packet),
                        policy_decision.show_command);
                }

                return run_windowed_server(options, logger, policy_decision.show_command);
            }

            {
                const DWORD input_type = options.host_input ? ::GetFileType(options.host_input.get()) : 0;
                const DWORD output_type = options.host_output ? ::GetFileType(options.host_output.get()) : 0;
                const DWORD signal_type = options.signal_handle ? ::GetFileType(options.signal_handle.get()) : 0;

                logger.log(
                    logging::LogLevel::debug,
                    L"Server-handle startup: headless={}, conpty={}, server_handle=0x{:X}, host_input=0x{:X}(type={}), host_output=0x{:X}(type={}), signal_handle=0x{:X}(type={})",
                    options.headless ? 1 : 0,
                    options.in_conpty_mode ? 1 : 0,
                    static_cast<unsigned long long>(options.server_handle.as_uintptr()),
                    static_cast<unsigned long long>(options.host_input.as_uintptr()),
                    static_cast<unsigned long>(input_type),
                    static_cast<unsigned long long>(options.host_output.as_uintptr()),
                    static_cast<unsigned long>(output_type),
                    static_cast<unsigned long long>(options.signal_handle.as_uintptr()),
                    static_cast<unsigned long>(signal_type));
            }

            core::HandleView stop_signal = options.signal_handle;
            core::HandleView host_signal_pipe{};
            core::UniqueHandle stop_event;
            std::optional<SignalPipeMonitor> signal_pipe_monitor;

            if (options.signal_handle)
            {
                const DWORD signal_type = ::GetFileType(options.signal_handle.get());
                if (signal_type == FILE_TYPE_PIPE)
                {
                    host_signal_pipe = options.signal_handle;

                    // In ConPTY/server-handle startup (commonly referred to as "0x4" in upstream),
                    // the `--signal` handle is a pipe. It is *not* a waitable shutdown event.
                    //
                    // A pipe becomes signaled when data is available, so passing the pipe handle
                    // into a wait set would spuriously request shutdown as soon as the hosting
                    // terminal writes any bytes. Instead, we drain the pipe on a helper thread
                    // and translate "disconnect" (broken pipe / EOF) into an explicit manual-
                    // reset event that the ConDrv server loop can wait on.
                    //
                    // See `new/src/runtime/signal_pipe_monitor.*` for details and tests.
                    if (auto created = core::create_event(true, false, nullptr))
                    {
                        stop_event = std::move(created.value());
                        auto monitor = SignalPipeMonitor::start(options.signal_handle, stop_event.view(), &logger);
                        if (monitor)
                        {
                            signal_pipe_monitor.emplace(std::move(monitor.value()));
                            stop_signal = stop_event.view();
                        }
                        else
                        {
                            logger.log(
                                logging::LogLevel::warning,
                                L"Signal pipe monitor failed; continuing without stop signal. context='{}', error={}",
                                monitor.error().context,
                                monitor.error().win32_error);
                            stop_signal = core::HandleView{};
                        }
                    }
                    else
                    {
                        logger.log(
                            logging::LogLevel::warning,
                            L"CreateEventW failed for signal pipe stop event; continuing without stop signal. error={}",
                            created.error());
                        stop_signal = core::HandleView{};
                    }
                }
            }

            logger.log(
                logging::LogLevel::debug,
                L"Server-handle stop signal: stop_signal=0x{:X}, signal_pipe_monitor_active={}",
                static_cast<unsigned long long>(stop_signal.as_uintptr()),
                signal_pipe_monitor.has_value() ? 1 : 0);

            if (auto handshake = send_headless_server_terminal_handshake(options, logger); !handshake)
            {
                return std::unexpected(handshake.error());
            }

            auto server_result = condrv::ConDrvServer::run(
                options.server_handle,
                stop_signal,
                options.host_input,
                options.host_output,
                host_signal_pipe,
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

        return run_with_inherited_stdio(options, logger);
    }
}
