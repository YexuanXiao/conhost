#include "runtime/terminal_handoff.hpp"

#include "core/win32_handle.hpp"
#include "logging/logger.hpp"

#include <Windows.h>
#include <objbase.h>
#include <winternl.h>

#include <optional>
#include <string>
#include <limits>
#include <vector>

// `runtime/terminal_handoff.cpp` implements the COM-based "terminal handoff"
// used by default-terminal integration for ConPTY sessions.
//
// When a terminal is registered (via `HKCU\\Console\\%%Startup\\DelegationTerminal`),
// the console host can ask that terminal to create the UI and to provide the
// pipe channels used for the ConPTY byte transport.
//
// The terminal returns:
// - host input pipe  (bytes -> pseudo console input),
// - host output pipe (bytes <- pseudo console output),
// - signal pipe      (used for lifetime/shutdown signaling).
//
// The replacement keeps this logic in a small, testable helper so the main
// session runtime (`session.cpp`) can treat the result as "just another set of
// host pipes" for the headless ConDrv server loop.
//
// Note on NTDLL usage:
// - Establishing a ConDrv "console reference" handle requires opening an object
//   manager name (e.g. `\\Reference`) *relative to* the ConDrv server handle.
// - Win32 `CreateFileW` does not support the "root handle + relative name"
//   pattern, so we use `NtOpenFile` via `ntdll.dll` for this one operation.
//
// See also:
// - `new/docs/conhost_source_architecture.md`
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
        constexpr std::wstring_view k_delegation_terminal_value = L"DelegationTerminal";

        [[nodiscard]] bool guid_equal(const GUID& left, const GUID& right) noexcept
        {
            return ::InlineIsEqualGUID(left, right) != FALSE;
        }

        [[nodiscard]] DWORD to_win32_error_from_hresult(const HRESULT hr) noexcept
        {
            const DWORD code = static_cast<DWORD>(HRESULT_CODE(hr));
            return code == 0 ? ERROR_GEN_FAILURE : code;
        }

        [[nodiscard]] TerminalHandoffError make_error(
            const std::wstring_view context,
            DWORD win32_error,
            const HRESULT hresult = E_FAIL) noexcept
        {
            const DWORD effective_win32 = win32_error == 0 ? ERROR_GEN_FAILURE : win32_error;
            const HRESULT effective_hr = (hresult == E_FAIL)
                ? HRESULT_FROM_WIN32(effective_win32)
                : hresult;

            std::wstring owned_context;
            try
            {
                owned_context.assign(context);
            }
            catch (...)
            {
                owned_context.clear();
            }
            return TerminalHandoffError{
                .context = std::move(owned_context),
                .win32_error = effective_win32,
                .hresult = effective_hr,
            };
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

            [[nodiscard]] T* operator->() const noexcept
            {
                return _value;
            }

            [[nodiscard]] explicit operator bool() const noexcept
            {
                return _value != nullptr;
            }

            T* release() noexcept
            {
                T* detached = _value;
                _value = nullptr;
                return detached;
            }

            void reset(T* replacement = nullptr) noexcept
            {
                if (_value != nullptr)
                {
                    _value->Release();
                }
                _value = replacement;
            }

        private:
            T* _value{ nullptr };
        };

        struct TerminalStartupInfo final
        {
            BSTR title{};
            BSTR icon_path{};
            LONG icon_index{};
            DWORD x{};
            DWORD y{};
            DWORD x_size{};
            DWORD y_size{};
            DWORD x_count_chars{};
            DWORD y_count_chars{};
            DWORD fill_attribute{};
            DWORD flags{};
            WORD show_window{};
        };

        struct __declspec(uuid("6F23DA90-15C5-4203-9DB0-64E73F1B1B00")) ITerminalHandoff3 : ::IUnknown
        {
            virtual HRESULT __stdcall EstablishPtyHandoff(
                HANDLE* in_pipe,
                HANDLE* out_pipe,
                HANDLE signal_pipe,
                HANDLE reference,
                HANDLE server,
                HANDLE client,
                const TerminalStartupInfo* startup_info) = 0;
        };

        struct NtdllApi final
        {
            using NtOpenFileFn = NTSTATUS(NTAPI*)(
                PHANDLE file_handle,
                ACCESS_MASK desired_access,
                POBJECT_ATTRIBUTES object_attributes,
                PIO_STATUS_BLOCK io_status_block,
                ULONG share_access,
                ULONG open_options);
            using RtlNtStatusToDosErrorFn = ULONG(WINAPI*)(NTSTATUS status);

            NtOpenFileFn nt_open_file{};
            RtlNtStatusToDosErrorFn rtl_nt_status_to_dos_error{};
        };

        struct PipePair final
        {
            core::UniqueHandle read_end;
            core::UniqueHandle write_end;
        };

        [[nodiscard]] std::expected<PipePair, TerminalHandoffError> create_pipe_pair() noexcept
        {
            SECURITY_ATTRIBUTES security{};
            security.nLength = sizeof(security);
            security.lpSecurityDescriptor = nullptr;
            security.bInheritHandle = FALSE;

            PipePair pair{};
            if (::CreatePipe(pair.read_end.put(), pair.write_end.put(), &security, 0) == FALSE)
            {
                return std::unexpected(make_error(
                    L"CreatePipe failed for terminal handoff signal pipe",
                    ::GetLastError()));
            }

            return pair;
        }

        [[nodiscard]] std::expected<NtdllApi, TerminalHandoffError> load_ntdll_api() noexcept
        {
            HMODULE ntdll = ::GetModuleHandleW(L"ntdll.dll");
            if (ntdll == nullptr)
            {
                return std::unexpected(make_error(
                    L"GetModuleHandleW(ntdll.dll) failed",
                    ::GetLastError()));
            }

            auto* const nt_open_file = reinterpret_cast<NtdllApi::NtOpenFileFn>(::GetProcAddress(ntdll, "NtOpenFile"));
            auto* const rtl_nt_status_to_dos_error = reinterpret_cast<NtdllApi::RtlNtStatusToDosErrorFn>(::GetProcAddress(ntdll, "RtlNtStatusToDosError"));
            if (nt_open_file == nullptr || rtl_nt_status_to_dos_error == nullptr)
            {
                return std::unexpected(make_error(
                    L"GetProcAddress failed for NTDLL handoff helpers",
                    ::GetLastError()));
            }

            return NtdllApi{
                .nt_open_file = nt_open_file,
                .rtl_nt_status_to_dos_error = rtl_nt_status_to_dos_error,
            };
        }

        [[nodiscard]] std::expected<core::UniqueHandle, TerminalHandoffError> open_server_relative_file(
            const NtdllApi& ntdll,
            const core::HandleView server_handle,
            const std::wstring_view child_name,
            const ACCESS_MASK desired_access,
            const ULONG open_options) noexcept
        {
            if (!server_handle)
            {
                return std::unexpected(make_error(
                    L"Server handle was invalid while opening server-relative path",
                    ERROR_INVALID_HANDLE));
            }
            if (child_name.size() > (std::numeric_limits<USHORT>::max() / sizeof(wchar_t)))
            {
                return std::unexpected(make_error(
                    L"Server-relative path was too long",
                    ERROR_FILENAME_EXCED_RANGE));
            }

            std::wstring child;
            try
            {
                child.assign(child_name);
                child.push_back(L'\0');
            }
            catch (...)
            {
                return std::unexpected(make_error(
                    L"Failed to allocate server-relative child path buffer",
                    ERROR_OUTOFMEMORY));
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
            const NTSTATUS status = ntdll.nt_open_file(
                &opened,
                desired_access,
                &object_attributes,
                &io_status,
                FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                open_options);
            if (status < 0)
            {
                const DWORD win32_error = static_cast<DWORD>(ntdll.rtl_nt_status_to_dos_error(status));
                return std::unexpected(make_error(
                    L"NtOpenFile failed for server-relative path",
                    win32_error == 0 ? ERROR_GEN_FAILURE : win32_error,
                    HRESULT_FROM_WIN32(win32_error)));
            }

            return core::UniqueHandle(opened);
        }

        [[nodiscard]] std::expected<std::optional<CLSID>, TerminalHandoffError> resolve_terminal_clsid_from_registry() noexcept
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
                return std::unexpected(make_error(
                    L"RegOpenKeyExW failed for HKCU\\Console\\%%Startup",
                    static_cast<DWORD>(open_status)));
            }

            DWORD value_type = 0;
            DWORD value_bytes = 0;
            const LONG size_status = ::RegQueryValueExW(
                startup_key.get(),
                k_delegation_terminal_value.data(),
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
                return std::unexpected(make_error(
                    L"RegQueryValueExW size query failed for DelegationTerminal",
                    static_cast<DWORD>(size_status)));
            }
            if (value_type != REG_SZ || value_bytes < sizeof(wchar_t))
            {
                return std::unexpected(make_error(
                    L"DelegationTerminal value had an unexpected format",
                    ERROR_BAD_FORMAT));
            }

            std::vector<wchar_t> text;
            try
            {
                text.assign((value_bytes / sizeof(wchar_t)) + 1, L'\0');
            }
            catch (...)
            {
                return std::unexpected(make_error(
                    L"Failed to allocate buffer for DelegationTerminal registry value",
                    ERROR_OUTOFMEMORY));
            }
            const LONG read_status = ::RegQueryValueExW(
                startup_key.get(),
                k_delegation_terminal_value.data(),
                nullptr,
                &value_type,
                reinterpret_cast<BYTE*>(text.data()),
                &value_bytes);
            if (read_status != ERROR_SUCCESS)
            {
                return std::unexpected(make_error(
                    L"RegQueryValueExW read failed for DelegationTerminal",
                    static_cast<DWORD>(read_status)));
            }
            text.back() = L'\0';

            CLSID terminal_clsid{};
            const HRESULT parse_hr = ::CLSIDFromString(text.data(), &terminal_clsid);
            if (FAILED(parse_hr))
            {
                return std::unexpected(make_error(
                    L"CLSIDFromString failed for DelegationTerminal",
                    to_win32_error_from_hresult(parse_hr),
                    parse_hr));
            }

            if (guid_equal(terminal_clsid, k_clsid_default) || guid_equal(terminal_clsid, k_clsid_conhost))
            {
                return std::optional<CLSID>{};
            }

            return terminal_clsid;
        }

        [[nodiscard]] std::expected<TerminalHandoffChannels, TerminalHandoffError> invoke_terminal_handoff(
            const CLSID& terminal_clsid,
            const core::HandleView server_handle,
            logging::Logger& logger) noexcept
        {
            // COM is used only as a local activation mechanism for the
            // registered terminal host. The returned channels are plain Win32
            // handles (pipes) and are subsequently driven without COM.
            const CoInitScope coinit(::CoInitializeEx(nullptr, COINIT_MULTITHREADED));
            if (FAILED(coinit.result()))
            {
                return std::unexpected(make_error(
                    L"CoInitializeEx failed for terminal handoff",
                    to_win32_error_from_hresult(coinit.result()),
                    coinit.result()));
            }

            UniqueComInterface<ITerminalHandoff3> handoff{};
            const HRESULT create_hr = ::CoCreateInstance(
                terminal_clsid,
                nullptr,
                CLSCTX_LOCAL_SERVER,
                __uuidof(ITerminalHandoff3),
                reinterpret_cast<void**>(handoff.put()));
            if (FAILED(create_hr))
            {
                return std::unexpected(make_error(
                    L"CoCreateInstance failed for ITerminalHandoff3",
                    to_win32_error_from_hresult(create_hr),
                    create_hr));
            }

            auto ntdll = load_ntdll_api();
            if (!ntdll)
            {
                return std::unexpected(ntdll.error());
            }

            // The console reference handle is passed to the terminal so it can
            // attach to the correct ConDrv server instance.
            auto reference = open_server_relative_file(
                ntdll.value(),
                server_handle,
                L"\\Reference",
                GENERIC_READ | GENERIC_WRITE | SYNCHRONIZE,
                FILE_SYNCHRONOUS_IO_NONALERT);
            if (!reference)
            {
                return std::unexpected(reference.error());
            }

            // Signal pipe for the terminal to request shutdown / signal events.
            // We pass the write end to the terminal and keep the read end.
            auto signal_pipe = create_pipe_pair();
            if (!signal_pipe)
            {
                return std::unexpected(signal_pipe.error());
            }

            // As with classic `IConsoleHandoff`, the terminal receives real
            // handles to both the server and client processes for lifetime
            // tracking. `GetCurrentProcess()` is a pseudo-handle, so we
            // duplicate it into real handles before passing them across COM.
            auto server_process = core::duplicate_current_process(
                PROCESS_DUP_HANDLE | PROCESS_QUERY_LIMITED_INFORMATION | SYNCHRONIZE,
                false);
            if (!server_process)
            {
                return std::unexpected(make_error(
                    L"DuplicateHandle failed for server process handle",
                    server_process.error()));
            }
            auto client_process = core::duplicate_current_process(PROCESS_QUERY_LIMITED_INFORMATION | SYNCHRONIZE, false);
            if (!client_process)
            {
                return std::unexpected(make_error(
                    L"DuplicateHandle failed for client process handle",
                    client_process.error()));
            }

            TerminalStartupInfo startup_info{};
            startup_info.show_window = SW_SHOWNORMAL;

            HANDLE host_input = nullptr;
            HANDLE host_output = nullptr;
            const HRESULT handoff_hr = handoff->EstablishPtyHandoff(
                &host_input,
                &host_output,
                signal_pipe->write_end.get(),
                reference->get(),
                server_process->get(),
                client_process->get(),
                &startup_info);
            if (FAILED(handoff_hr))
            {
                return std::unexpected(make_error(
                    L"ITerminalHandoff3::EstablishPtyHandoff failed",
                    to_win32_error_from_hresult(handoff_hr),
                    handoff_hr));
            }

            core::UniqueHandle input_pipe(host_input);
            core::UniqueHandle output_pipe(host_output);
            if (!input_pipe.valid() || !output_pipe.valid())
            {
                return std::unexpected(make_error(
                    L"Terminal handoff returned invalid in/out pipe handles",
                    ERROR_INVALID_HANDLE));
            }

            signal_pipe->write_end.reset();
            logger.log(logging::LogLevel::info, L"Terminal handoff established via ITerminalHandoff3");

            return TerminalHandoffChannels{
                .host_input = std::move(input_pipe),
                .host_output = std::move(output_pipe),
                .signal_pipe = std::move(signal_pipe->read_end),
            };
        }
    }

    std::expected<std::optional<TerminalHandoffChannels>, TerminalHandoffError> TerminalHandoff::try_establish(
        const core::HandleView server_handle,
        const bool force_no_handoff,
        logging::Logger& logger) noexcept
    {
        return try_establish_with(
            server_handle,
            force_no_handoff,
            logger,
            &resolve_terminal_clsid_from_registry,
            &invoke_terminal_handoff);
    }

    std::expected<std::optional<TerminalHandoffChannels>, TerminalHandoffError> TerminalHandoff::try_establish_with(
        const core::HandleView server_handle,
        const bool force_no_handoff,
        logging::Logger& logger,
        const DelegationResolver resolver,
        const HandoffInvoker invoker) noexcept
    {
        if (force_no_handoff)
        {
            return std::optional<TerminalHandoffChannels>{};
        }
        if (resolver == nullptr || invoker == nullptr)
        {
            return std::unexpected(make_error(
                L"Terminal handoff hooks were null",
                ERROR_INVALID_PARAMETER));
        }
        if (!server_handle)
        {
            return std::unexpected(make_error(
                L"Server handle was invalid for terminal handoff",
                ERROR_INVALID_HANDLE));
        }

        auto target = resolver();
        if (!target)
        {
            return std::unexpected(target.error());
        }
        if (!target->has_value())
        {
            return std::optional<TerminalHandoffChannels>{};
        }

        auto invoked = invoker(target->value(), server_handle, logger);
        if (!invoked)
        {
            return std::unexpected(invoked.error());
        }

        return std::optional<TerminalHandoffChannels>(std::move(invoked.value()));
    }
}
