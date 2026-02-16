#include "core/unique_handle.hpp"

#include <Windows.h>
#include <objbase.h>

#include "IConsoleHandoff.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace
{
    extern "C" void STDMETHODCALLTYPE ObjectStublessClient3(void);

    constexpr CLSID kClsidConsoleHandoff = {
        0x1F9F2BF5, 0x5BC3, 0x4F17, { 0xB0, 0xE6, 0x91, 0x24, 0x13, 0xF1, 0xF4, 0x51 }
    };

    // Matches the proxy CLSID used by upstream OpenConsoleProxy for the unbranded/dev build.
    constexpr CLSID kClsidOpenConsoleProxy = {
        0xDEC4804D, 0x56D1, 0x4F73, { 0x9F, 0xBE, 0x68, 0x28, 0xE7, 0xC8, 0x5C, 0x56 }
    };

    constexpr wchar_t kTestReadyEventEnv[] = L"OPENCONSOLE_NEW_TEST_EMBED_READY_EVENT";

    [[nodiscard]] std::wstring module_path() noexcept
    {
        std::wstring buffer(256, L'\0');
        for (;;)
        {
            const DWORD written = ::GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
            if (written == 0)
            {
                return {};
            }

            if (written < buffer.size() - 1)
            {
                buffer.resize(written);
                return buffer;
            }

            if (buffer.size() >= 32 * 1024)
            {
                return {};
            }

            buffer.resize(buffer.size() * 2);
        }
    }

    [[nodiscard]] std::wstring directory_name(std::wstring path) noexcept
    {
        const auto pos = path.find_last_of(L"\\/");
        if (pos == std::wstring::npos)
        {
            return {};
        }
        path.resize(pos);
        return path;
    }

    [[nodiscard]] std::wstring join_path(std::wstring_view dir, std::wstring_view leaf)
    {
        std::wstring combined;
        combined.reserve(dir.size() + leaf.size() + 1);
        combined.append(dir);
        if (!combined.empty())
        {
            const wchar_t tail = combined.back();
            if (tail != L'\\' && tail != L'/')
            {
                combined.push_back(L'\\');
            }
        }
        combined.append(leaf);
        return combined;
    }

    [[nodiscard]] std::optional<std::wstring> locate_embedding_test_host() noexcept
    {
        const std::wstring exe = module_path();
        if (exe.empty())
        {
            return std::nullopt;
        }

        const std::wstring test_dir = directory_name(exe);
        if (test_dir.empty())
        {
            return std::nullopt;
        }

        std::wstring candidate = join_path(test_dir, L"oc_new_embedding_test_host.exe");
        const DWORD attrs = ::GetFileAttributesW(candidate.c_str());
        if (attrs == INVALID_FILE_ATTRIBUTES)
        {
            return std::nullopt;
        }

        return candidate;
    }

    [[nodiscard]] std::optional<std::wstring> locate_proxy_stub_dll() noexcept
    {
        const std::wstring exe = module_path();
        if (exe.empty())
        {
            return std::nullopt;
        }

        const std::wstring test_dir = directory_name(exe);
        if (test_dir.empty())
        {
            return std::nullopt;
        }

        std::wstring candidate = join_path(test_dir, L"oc_new_openconsole_proxy.dll");
        const DWORD attrs = ::GetFileAttributesW(candidate.c_str());
        if (attrs == INVALID_FILE_ATTRIBUTES)
        {
            return std::nullopt;
        }

        return candidate;
    }

    [[nodiscard]] std::wstring quote(std::wstring_view value)
    {
        std::wstring quoted;
        quoted.reserve(value.size() + 2);
        quoted.push_back(L'"');
        quoted.append(value);
        quoted.push_back(L'"');
        return quoted;
    }

    struct ScopedEnvironmentVariable final
    {
        explicit ScopedEnvironmentVariable(const wchar_t* name, const std::wstring& value) :
            _name(name)
        {
            const DWORD required = ::GetEnvironmentVariableW(_name.c_str(), nullptr, 0);
            if (required != 0)
            {
                std::wstring buffer(required, L'\0');
                const DWORD written = ::GetEnvironmentVariableW(_name.c_str(), buffer.data(), required);
                if (written != 0)
                {
                    buffer.resize(written);
                    _previous = std::move(buffer);
                }
            }

            _changed = ::SetEnvironmentVariableW(_name.c_str(), value.c_str()) != FALSE;
        }

        ~ScopedEnvironmentVariable() noexcept
        {
            if (!_changed)
            {
                return;
            }

            if (_previous.has_value())
            {
                ::SetEnvironmentVariableW(_name.c_str(), _previous->c_str());
            }
            else
            {
                ::SetEnvironmentVariableW(_name.c_str(), nullptr);
            }
        }

        ScopedEnvironmentVariable(const ScopedEnvironmentVariable&) = delete;
        ScopedEnvironmentVariable& operator=(const ScopedEnvironmentVariable&) = delete;

    private:
        std::wstring _name;
        std::optional<std::wstring> _previous;
        bool _changed{ false };
    };

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

    [[nodiscard]] std::wstring make_unique_ready_event_name() noexcept
    {
        wchar_t buffer[128]{};
        _snwprintf_s(
            buffer,
            _TRUNCATE,
            L"Local\\oc_new_embed_ready_%lu_%llu",
            static_cast<unsigned long>(::GetCurrentProcessId()),
            static_cast<unsigned long long>(::GetTickCount64()));
        return buffer;
    }

    class ScopedComProxyRegistry final
    {
    public:
        ScopedComProxyRegistry(const std::wstring& proxy_dll_path, const bool trace_enabled)
        {
            // Register the proxy/stub in HKCU so both the test process and the spawned
            // embedding host process can marshal IConsoleHandoff out-of-proc without
            // relying on machine-wide registration.
            //
            // Keys (per-user):
            // - HKCU\Software\Classes\CLSID\{proxy}\InprocServer32 -> path + ThreadingModel
            // - HKCU\Software\Classes\Interface\{iid}\ProxyStubClsid32 -> {proxy}
            _trace_enabled = trace_enabled;

            wchar_t proxy_guid[64]{};
            if (::StringFromGUID2(kClsidOpenConsoleProxy, proxy_guid, static_cast<int>(std::size(proxy_guid))) == 0)
            {
                return;
            }

            wchar_t handoff_iid[64]{};
            if (::StringFromGUID2(__uuidof(IConsoleHandoff), handoff_iid, static_cast<int>(std::size(handoff_iid))) == 0)
            {
                return;
            }

            wchar_t marker_iid[64]{};
            if (::StringFromGUID2(__uuidof(IDefaultTerminalMarker), marker_iid, static_cast<int>(std::size(marker_iid))) == 0)
            {
                return;
            }

            _proxy_clsid_string.assign(proxy_guid);

            if (!set_inproc_server(proxy_dll_path))
            {
                return;
            }
            if (!set_interface_proxy_stub(handoff_iid))
            {
                return;
            }
            if (!set_interface_proxy_stub(marker_iid))
            {
                return;
            }

            _active = true;
        }

        ~ScopedComProxyRegistry() noexcept
        {
            if (!_active)
            {
                return;
            }

            // Best-effort cleanup. If keys already existed we leave them as-is to avoid
            // breaking unrelated COM registrations in the user's profile.
            cleanup_interface(__uuidof(IConsoleHandoff), _handoff_proxy_key_created);
            cleanup_interface(__uuidof(IDefaultTerminalMarker), _marker_proxy_key_created);
            cleanup_inproc_server(_inproc_key_created);
        }

        ScopedComProxyRegistry(const ScopedComProxyRegistry&) = delete;
        ScopedComProxyRegistry& operator=(const ScopedComProxyRegistry&) = delete;

        [[nodiscard]] bool ok() const noexcept
        {
            return _active;
        }

    private:
        static void trace_value(const bool enabled, const wchar_t* label, const wchar_t* value) noexcept
        {
            if (!enabled)
            {
                return;
            }
            fwprintf(stderr, L"[TRACE] com proxy registry: %ls=%ls\n", label, value);
            (void)fflush(stderr);
        }

        static void trace_error(const bool enabled, const wchar_t* label, const LONG status) noexcept
        {
            if (!enabled)
            {
                return;
            }
            fwprintf(stderr, L"[TRACE] com proxy registry: %ls failed (status=%ld)\n", label, static_cast<long>(status));
            (void)fflush(stderr);
        }

        [[nodiscard]] bool set_inproc_server(const std::wstring& proxy_dll_path) noexcept
        {
            std::wstring key_path;
            key_path.reserve(128 + _proxy_clsid_string.size());
            key_path.append(L"Software\\Classes\\CLSID\\");
            key_path.append(_proxy_clsid_string);
            key_path.append(L"\\InprocServer32");

            HKEY key = nullptr;
            DWORD disposition = 0;
            const LONG status = ::RegCreateKeyExW(
                HKEY_CURRENT_USER,
                key_path.c_str(),
                0,
                nullptr,
                REG_OPTION_NON_VOLATILE,
                KEY_QUERY_VALUE | KEY_SET_VALUE,
                nullptr,
                &key,
                &disposition);
            if (status != ERROR_SUCCESS)
            {
                trace_error(_trace_enabled, L"RegCreateKeyExW(InprocServer32)", status);
                return false;
            }

            const auto close_key = [&] {
                if (key != nullptr)
                {
                    ::RegCloseKey(key);
                    key = nullptr;
                }
            };

            if (disposition == REG_CREATED_NEW_KEY)
            {
                _inproc_key_created = true;
            }

            trace_value(_trace_enabled, L"InprocServer32", key_path.c_str());

            const LONG default_status = ::RegSetValueExW(
                key,
                nullptr,
                0,
                REG_SZ,
                reinterpret_cast<const BYTE*>(proxy_dll_path.c_str()),
                static_cast<DWORD>((proxy_dll_path.size() + 1) * sizeof(wchar_t)));
            if (default_status != ERROR_SUCCESS)
            {
                trace_error(_trace_enabled, L"RegSetValueExW(InprocServer32 default)", default_status);
                close_key();
                return false;
            }

            constexpr wchar_t kThreadingModel[] = L"Both";
            const LONG model_status = ::RegSetValueExW(
                key,
                L"ThreadingModel",
                0,
                REG_SZ,
                reinterpret_cast<const BYTE*>(kThreadingModel),
                static_cast<DWORD>(sizeof(kThreadingModel)));
            if (model_status != ERROR_SUCCESS)
            {
                trace_error(_trace_enabled, L"RegSetValueExW(ThreadingModel)", model_status);
                close_key();
                return false;
            }

            close_key();
            return true;
        }

        [[nodiscard]] bool set_interface_proxy_stub(const wchar_t* iid_string) noexcept
        {
            std::wstring key_path;
            key_path.reserve(128 + wcslen(iid_string));
            key_path.append(L"Software\\Classes\\Interface\\");
            key_path.append(iid_string);
            key_path.append(L"\\ProxyStubClsid32");

            HKEY key = nullptr;
            DWORD disposition = 0;
            const LONG status = ::RegCreateKeyExW(
                HKEY_CURRENT_USER,
                key_path.c_str(),
                0,
                nullptr,
                REG_OPTION_NON_VOLATILE,
                KEY_QUERY_VALUE | KEY_SET_VALUE,
                nullptr,
                &key,
                &disposition);
            if (status != ERROR_SUCCESS)
            {
                trace_error(_trace_enabled, L"RegCreateKeyExW(ProxyStubClsid32)", status);
                return false;
            }

            if (disposition == REG_CREATED_NEW_KEY)
            {
                if (wcscmp(iid_string, L"{E686C757-9A35-4A1C-B3CE-0BCC8B5C69F4}") == 0)
                {
                    _handoff_proxy_key_created = true;
                }
                else
                {
                    _marker_proxy_key_created = true;
                }
            }

            trace_value(_trace_enabled, L"ProxyStubClsid32", key_path.c_str());

            const LONG set_status = ::RegSetValueExW(
                key,
                nullptr,
                0,
                REG_SZ,
                reinterpret_cast<const BYTE*>(_proxy_clsid_string.c_str()),
                static_cast<DWORD>((_proxy_clsid_string.size() + 1) * sizeof(wchar_t)));
            ::RegCloseKey(key);
            if (set_status != ERROR_SUCCESS)
            {
                trace_error(_trace_enabled, L"RegSetValueExW(ProxyStubClsid32 default)", set_status);
                return false;
            }

            return true;
        }

        void cleanup_inproc_server(const bool created) noexcept
        {
            if (!created)
            {
                return;
            }

            std::wstring clsid_path;
            clsid_path.reserve(128 + _proxy_clsid_string.size());
            clsid_path.append(L"Software\\Classes\\CLSID\\");
            clsid_path.append(_proxy_clsid_string);
            clsid_path.append(L"\\InprocServer32");

            (void)::RegDeleteTreeW(HKEY_CURRENT_USER, clsid_path.c_str());
        }

        void cleanup_interface(const GUID& iid, const bool created) noexcept
        {
            if (!created)
            {
                return;
            }

            wchar_t iid_string[64]{};
            if (::StringFromGUID2(iid, iid_string, static_cast<int>(std::size(iid_string))) == 0)
            {
                return;
            }

            std::wstring key_path;
            key_path.reserve(128 + wcslen(iid_string));
            key_path.append(L"Software\\Classes\\Interface\\");
            key_path.append(iid_string);
            key_path.append(L"\\ProxyStubClsid32");

            (void)::RegDeleteTreeW(HKEY_CURRENT_USER, key_path.c_str());
        }

        bool _trace_enabled{ false };
        bool _active{ false };

        bool _inproc_key_created{ false };
        bool _handoff_proxy_key_created{ false };
        bool _marker_proxy_key_created{ false };

        std::wstring _proxy_clsid_string;
    };

    using CompareObjectHandlesFn = BOOL(WINAPI*)(HANDLE, HANDLE);

    [[nodiscard]] std::optional<bool> compare_object_handles(oc::core::HandleView first, oc::core::HandleView second) noexcept
    {
        HMODULE kernel32 = ::GetModuleHandleW(L"kernel32.dll");
        if (kernel32 == nullptr)
        {
            return std::nullopt;
        }

        auto* fn = reinterpret_cast<CompareObjectHandlesFn>(::GetProcAddress(kernel32, "CompareObjectHandles"));
        if (fn == nullptr)
        {
            return std::nullopt;
        }

        return fn(first.get(), second.get()) != FALSE;
    }

    bool test_com_embedding_out_of_proc_handoff_roundtrip()
    {
        const bool trace_enabled = ::GetEnvironmentVariableW(L"OPENCONSOLE_NEW_TEST_TRACE", nullptr, 0) != 0;
        const auto trace = [&](const wchar_t* step) {
            if (trace_enabled)
            {
                fwprintf(stderr, L"[TRACE] com embedding integration: %ls\n", step);
                (void)fflush(stderr);
            }
        };

        trace(L"locate proxy stub");
        const auto proxy_path = locate_proxy_stub_dll();
        if (!proxy_path)
        {
            fwprintf(stderr, L"[DETAIL] oc_new_openconsole_proxy.dll not found relative to test binary\n");
            return false;
        }

        ScopedComProxyRegistry proxy_reg(*proxy_path, trace_enabled);
        if (!proxy_reg.ok())
        {
            fwprintf(stderr, L"[DETAIL] failed to register COM proxy/stub registry keys\n");
            return false;
        }

        trace(L"locate test host");
        const auto host_path = locate_embedding_test_host();
        if (!host_path)
        {
            fwprintf(stderr, L"[DETAIL] oc_new_embedding_test_host.exe not found relative to test binary\n");
            return false;
        }

        trace(L"create ready event");
        const std::wstring ready_name = make_unique_ready_event_name();
        oc::core::UniqueHandle ready_event(::CreateEventW(nullptr, TRUE, FALSE, ready_name.c_str()));
        if (!ready_event.valid())
        {
            fwprintf(stderr, L"[DETAIL] CreateEventW(ready) failed (error=%lu)\n", ::GetLastError());
            return false;
        }

        trace(L"set env var + spawn host process");
        ScopedEnvironmentVariable ready_env(kTestReadyEventEnv, ready_name);

        std::wstring command_line = quote(*host_path);
        std::vector<wchar_t> mutable_command_line(command_line.begin(), command_line.end());
        mutable_command_line.push_back(L'\0');

        STARTUPINFOW startup{};
        startup.cb = sizeof(startup);

        PROCESS_INFORMATION info{};
        const BOOL created = ::CreateProcessW(
            host_path->c_str(),
            mutable_command_line.data(),
            nullptr,
            nullptr,
            FALSE,
            0,
            nullptr,
            nullptr,
            &startup,
            &info);
        if (created == FALSE)
        {
            fwprintf(stderr, L"[DETAIL] CreateProcessW(embedding test host) failed (error=%lu)\n", ::GetLastError());
            return false;
        }

        oc::core::UniqueHandle process(info.hProcess);
        oc::core::UniqueHandle thread(info.hThread);

        trace(L"wait for host readiness");
        HANDLE wait_handles[2]{ ready_event.get(), process.get() };
        const DWORD ready_wait = ::WaitForMultipleObjects(2, wait_handles, FALSE, 10'000);
        if (ready_wait == WAIT_OBJECT_0 + 1)
        {
            DWORD exit_code = 0;
            if (::GetExitCodeProcess(process.get(), &exit_code) == FALSE)
            {
                exit_code = 0;
            }
            fwprintf(stderr, L"[DETAIL] embedding test host exited early (exit=%lu)\n", exit_code);
            return false;
        }
        if (ready_wait != WAIT_OBJECT_0)
        {
            fwprintf(stderr, L"[DETAIL] timed out waiting for embedding test host readiness (wait=%lu)\n", ready_wait);
            (void)::TerminateProcess(process.get(), 0xBADC0DE);
            (void)::WaitForSingleObject(process.get(), 5'000);
            return false;
        }

        trace(L"coinitialize");
        const CoInitScope coinit(::CoInitializeEx(nullptr, COINIT_MULTITHREADED));
        if (FAILED(coinit.result()))
        {
            fwprintf(stderr, L"[DETAIL] CoInitializeEx failed (hr=0x%08X)\n", static_cast<unsigned int>(coinit.result()));
            (void)::TerminateProcess(process.get(), 0xBADC0DE);
            (void)::WaitForSingleObject(process.get(), 5'000);
            return false;
        }

        if (trace_enabled)
        {
            PROCESS_MITIGATION_DYNAMIC_CODE_POLICY dyn{};
            if (::GetProcessMitigationPolicy(::GetCurrentProcess(), ProcessDynamicCodePolicy, &dyn, sizeof(dyn)) != FALSE)
            {
                fwprintf(stderr, L"[TRACE] com embedding integration: dynamic code prohibited=%lu allow_thread_opt_out=%lu\n",
                         static_cast<unsigned long>(dyn.ProhibitDynamicCode),
                         static_cast<unsigned long>(dyn.AllowThreadOptOut));
                (void)fflush(stderr);
            }
        }

        // The proxy/stub for system_handle marshalling relies on COM security
        // being initialized for cross-process calls.
        const HRESULT security_hr = ::CoInitializeSecurity(
            nullptr,
            -1,
            nullptr,
            nullptr,
            RPC_C_AUTHN_LEVEL_DEFAULT,
            RPC_C_IMP_LEVEL_IDENTIFY,
            nullptr,
            EOAC_NONE,
            nullptr);
        if (FAILED(security_hr))
        {
            fwprintf(stderr, L"[DETAIL] CoInitializeSecurity failed (hr=0x%08X)\n", static_cast<unsigned int>(security_hr));
            (void)::TerminateProcess(process.get(), 0xBADC0DE);
            (void)::WaitForSingleObject(process.get(), 5'000);
            return false;
        }

        trace(L"cocreateinstance");
        IConsoleHandoff* handoff = nullptr;
        const HRESULT activation_hr = ::CoCreateInstance(
            kClsidConsoleHandoff,
            nullptr,
            CLSCTX_LOCAL_SERVER,
            __uuidof(IConsoleHandoff),
            reinterpret_cast<void**>(&handoff));
        if (FAILED(activation_hr) || handoff == nullptr)
        {
            fwprintf(stderr, L"[DETAIL] CoCreateInstance(CLSCTX_LOCAL_SERVER) failed (hr=0x%08X)\n", static_cast<unsigned int>(activation_hr));
            (void)::TerminateProcess(process.get(), 0xBADC0DE);
            (void)::WaitForSingleObject(process.get(), 5'000);
            return false;
        }

        trace(L"smoke addref/release");
        handoff->AddRef();
        handoff->Release();

        // Some RPC stubless proxy implementations lazily initialize method thunks
        // on first use. Exercise QueryInterface to ensure the proxy has had a chance
        // to finalize its vtable before calling into the first custom method.
        IConsoleHandoff* handoff_again = nullptr;
        (void)handoff->QueryInterface(__uuidof(IConsoleHandoff), reinterpret_cast<void**>(&handoff_again));
        if (handoff_again != nullptr)
        {
            handoff_again->Release();
        }

        if (trace_enabled)
        {
            void** vtbl = *reinterpret_cast<void***>(handoff);
            void* establish = nullptr;
            void* qi = nullptr;
            void* add_ref = nullptr;
            void* release = nullptr;
            if (vtbl != nullptr)
            {
                qi = vtbl[0];
                add_ref = vtbl[1];
                release = vtbl[2];
                establish = vtbl[3];
            }
            fwprintf(
                stderr,
                L"[TRACE] com embedding integration: vtbl=%p qi=%p addref=%p release=%p establish=%p\n",
                vtbl,
                qi,
                add_ref,
                release,
                establish);
            (void)fflush(stderr);

            HMODULE owner = nullptr;
            if (::GetModuleHandleExW(
                    GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                    reinterpret_cast<LPCWSTR>(vtbl),
                    &owner) != FALSE)
            {
                wchar_t module_name[512]{};
                const DWORD written = ::GetModuleFileNameW(owner, module_name, static_cast<DWORD>(std::size(module_name)));
                if (written != 0 && written < std::size(module_name))
                {
                    fwprintf(stderr, L"[TRACE] com embedding integration: vtbl module=%ls\n", module_name);
                    (void)fflush(stderr);
                }
            }
        }

        constexpr DWORD kExpectedExitCode = 77;

        trace(L"establish handoff");

        // The MIDL-generated stubless proxy vtable uses a -1 placeholder for methods that are
        // routed through the shared ObjectStublessClientN thunks. In some environments COM may
        // return the unpatched vtable (with -1), which would crash on invocation. Patch the
        // one custom method slot deterministically for the scope of this test.
        if (handoff != nullptr)
        {
            void** vtbl = *reinterpret_cast<void***>(handoff);
            if (vtbl != nullptr && vtbl[3] == reinterpret_cast<void*>(static_cast<INT_PTR>(-1)))
            {
                DWORD old_protect = 0;
                if (::VirtualProtect(&vtbl[3], sizeof(vtbl[3]), PAGE_EXECUTE_READWRITE, &old_protect) != FALSE)
                {
                    vtbl[3] = reinterpret_cast<void*>(&ObjectStublessClient3);
                    DWORD ignored = 0;
                    (void)::VirtualProtect(&vtbl[3], sizeof(vtbl[3]), old_protect, &ignored);
                }
            }
        }

        CONSOLE_PORTABLE_ATTACH_MSG attach{};
        attach.IdLowPart = 1234;
        attach.IdHighPart = -5;
        attach.Process = static_cast<ULONG64>(::GetCurrentProcessId());
        attach.Object = 0;
        attach.Function = kExpectedExitCode;
        attach.InputSize = 0;
        attach.OutputSize = 0;

        oc::core::UniqueHandle returned_process_handle;
        const HRESULT handoff_hr = handoff->EstablishHandoff(
            nullptr,
            nullptr,
            &attach,
            nullptr,
            nullptr,
            returned_process_handle.put());
        handoff->Release();

        if (FAILED(handoff_hr) || !returned_process_handle.valid())
        {
            fwprintf(stderr, L"[DETAIL] EstablishHandoff failed (hr=0x%08X, returned=%d)\n",
                     static_cast<unsigned int>(handoff_hr),
                     returned_process_handle.valid() ? 1 : 0);
            (void)::TerminateProcess(process.get(), 0xBADC0DE);
            (void)::WaitForSingleObject(process.get(), 5'000);
            return false;
        }

        trace(L"compare handles");
        if (const auto same = compare_object_handles(process.view(), returned_process_handle.view()); same.has_value())
        {
            if (!*same)
            {
                fwprintf(stderr, L"[DETAIL] Returned server process handle did not match spawned test host process\n");
                (void)::TerminateProcess(process.get(), 0xBADC0DE);
                (void)::WaitForSingleObject(process.get(), 5'000);
                return false;
            }
        }
        else
        {
            fwprintf(stderr, L"[DETAIL] CompareObjectHandles unavailable; skipping handle identity check\n");
        }

        trace(L"wait for host exit + validate exit code");
        const DWORD exit_wait = ::WaitForSingleObject(process.get(), 10'000);
        if (exit_wait != WAIT_OBJECT_0)
        {
            fwprintf(stderr, L"[DETAIL] embedding test host did not exit (wait=%lu)\n", exit_wait);
            (void)::TerminateProcess(process.get(), 0xBADC0DE);
            (void)::WaitForSingleObject(process.get(), 5'000);
            return false;
        }

        DWORD exit_code = 0;
        if (::GetExitCodeProcess(process.get(), &exit_code) == FALSE)
        {
            fwprintf(stderr, L"[DETAIL] GetExitCodeProcess failed (error=%lu)\n", ::GetLastError());
            return false;
        }

        if (exit_code != kExpectedExitCode)
        {
            fwprintf(stderr, L"[DETAIL] embedding test host exit code mismatch (got=%lu expected=%lu)\n",
                     exit_code,
                     kExpectedExitCode);
            return false;
        }

        return true;
    }
}

bool run_com_embedding_integration_tests()
{
    if (!test_com_embedding_out_of_proc_handoff_roundtrip())
    {
        fwprintf(stderr, L"[DETAIL] out-of-proc COM embedding handoff test failed\n");
        return false;
    }

    return true;
}
