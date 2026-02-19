#include "core/unique_handle.hpp"

#include <Windows.h>
#include <winternl.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <expected>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace
{
#ifndef ProcThreadAttributeConsoleReference
    // Matches OpenConsole's legacy server startup (`src/server/winbasep.h`).
    // This attribute is consumed by the kernel console runtime to associate the
    // new process with a specific ConDrv server instance.
#define ProcThreadAttributeConsoleReference 10
#endif

#ifndef PROC_THREAD_ATTRIBUTE_CONSOLE_REFERENCE
#define PROC_THREAD_ATTRIBUTE_CONSOLE_REFERENCE ProcThreadAttributeValue(ProcThreadAttributeConsoleReference, FALSE, TRUE, FALSE)
#endif

    [[nodiscard]] std::wstring module_path() noexcept
    {
        // Avoid MAX_PATH by growing the buffer until GetModuleFileNameW succeeds.
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

            // Either truncated or exact fit without room for NUL; grow.
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

    [[nodiscard]] std::optional<std::wstring> locate_openconsole_new() noexcept
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

        const std::wstring build_dir = directory_name(test_dir);
        if (build_dir.empty())
        {
            return std::nullopt;
        }

        std::wstring candidate = join_path(build_dir, L"openconsole_new.exe");
        const DWORD attrs = ::GetFileAttributesW(candidate.c_str());
        if (attrs == INVALID_FILE_ATTRIBUTES)
        {
            return std::nullopt;
        }

        return candidate;
    }

    [[nodiscard]] std::optional<std::wstring> locate_condrv_client_smoke() noexcept
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

        std::wstring candidate = join_path(test_dir, L"oc_new_condrv_client_smoke.exe");
        const DWORD attrs = ::GetFileAttributesW(candidate.c_str());
        if (attrs == INVALID_FILE_ATTRIBUTES)
        {
            return std::nullopt;
        }

        return candidate;
    }

    [[nodiscard]] std::optional<std::wstring> locate_condrv_client_input_events() noexcept
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

        std::wstring candidate = join_path(test_dir, L"oc_new_condrv_client_input_events.exe");
        const DWORD attrs = ::GetFileAttributesW(candidate.c_str());
        if (attrs == INVALID_FILE_ATTRIBUTES)
        {
            return std::nullopt;
        }

        return candidate;
    }

    [[nodiscard]] std::optional<std::wstring> locate_condrv_client_raw_read() noexcept
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

        std::wstring candidate = join_path(test_dir, L"oc_new_condrv_client_raw_read.exe");
        const DWORD attrs = ::GetFileAttributesW(candidate.c_str());
        if (attrs == INVALID_FILE_ATTRIBUTES)
        {
            return std::nullopt;
        }

        return candidate;
    }

    [[nodiscard]] std::expected<void, DWORD> set_no_inherit(oc::core::HandleView handle) noexcept
    {
        if (!handle)
        {
            return std::unexpected(ERROR_INVALID_HANDLE);
        }
        if (::SetHandleInformation(handle.get(), HANDLE_FLAG_INHERIT, 0) == FALSE)
        {
            return std::unexpected(::GetLastError());
        }
        return {};
    }

    struct InheritablePipe final
    {
        oc::core::UniqueHandle read;
        oc::core::UniqueHandle write;
    };

    [[nodiscard]] std::expected<InheritablePipe, DWORD> create_pipe_inherit_write_end() noexcept
    {
        SECURITY_ATTRIBUTES security{};
        security.nLength = sizeof(security);
        security.lpSecurityDescriptor = nullptr;
        security.bInheritHandle = TRUE;

        InheritablePipe pipe{};
        if (::CreatePipe(pipe.read.put(), pipe.write.put(), &security, 0) == FALSE)
        {
            return std::unexpected(::GetLastError());
        }

        if (auto cleared = set_no_inherit(pipe.read.view()); !cleared)
        {
            return std::unexpected(cleared.error());
        }

        return pipe;
    }

    [[nodiscard]] std::expected<InheritablePipe, DWORD> create_pipe_inherit_read_end() noexcept
    {
        SECURITY_ATTRIBUTES security{};
        security.nLength = sizeof(security);
        security.lpSecurityDescriptor = nullptr;
        security.bInheritHandle = TRUE;

        InheritablePipe pipe{};
        if (::CreatePipe(pipe.read.put(), pipe.write.put(), &security, 0) == FALSE)
        {
            return std::unexpected(::GetLastError());
        }

        if (auto cleared = set_no_inherit(pipe.write.view()); !cleared)
        {
            return std::unexpected(cleared.error());
        }

        return pipe;
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

    [[nodiscard]] bool bytes_contain_ascii(const std::vector<std::byte>& haystack, const std::string_view needle) noexcept
    {
        if (needle.empty())
        {
            return true;
        }

        const auto* begin = reinterpret_cast<const char*>(haystack.data());
        const auto* end = begin + haystack.size();
        const auto* found = std::search(begin, end, needle.begin(), needle.end());
        return found != end;
    }

    struct ScopedEnvironmentVariable final
    {
        explicit ScopedEnvironmentVariable(std::wstring name, std::wstring value) :
            _name(std::move(name))
        {
            const DWORD required = ::GetEnvironmentVariableW(_name.c_str(), nullptr, 0);
            if (required != 0)
            {
                std::wstring buffer(required, L'\0');
                const DWORD written = ::GetEnvironmentVariableW(_name.c_str(), buffer.data(), required);
                if (written != 0 && written < required)
                {
                    buffer.resize(written);
                    _previous = std::move(buffer);
                }
            }

            (void)::SetEnvironmentVariableW(_name.c_str(), value.c_str());
        }

        ~ScopedEnvironmentVariable() noexcept
        {
            (void)::SetEnvironmentVariableW(_name.c_str(), _previous ? _previous->c_str() : nullptr);
        }

        ScopedEnvironmentVariable(const ScopedEnvironmentVariable&) = delete;
        ScopedEnvironmentVariable& operator=(const ScopedEnvironmentVariable&) = delete;

    private:
        std::wstring _name;
        std::optional<std::wstring> _previous;
    };

    struct Ntdll final
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

    [[nodiscard]] std::optional<Ntdll> load_ntdll() noexcept
    {
        HMODULE module = ::GetModuleHandleW(L"ntdll.dll");
        if (module == nullptr)
        {
            module = ::LoadLibraryW(L"ntdll.dll");
        }

        if (module == nullptr)
        {
            return std::nullopt;
        }

        auto* const nt_open_file = reinterpret_cast<Ntdll::NtOpenFileFn>(::GetProcAddress(module, "NtOpenFile"));
        auto* const rtl_nt_status_to_dos_error = reinterpret_cast<Ntdll::RtlNtStatusToDosErrorFn>(::GetProcAddress(module, "RtlNtStatusToDosError"));
        if (nt_open_file == nullptr || rtl_nt_status_to_dos_error == nullptr)
        {
            return std::nullopt;
        }

        return Ntdll{
            .nt_open_file = nt_open_file,
            .rtl_nt_status_to_dos_error = rtl_nt_status_to_dos_error,
        };
    }

    [[nodiscard]] std::expected<oc::core::UniqueHandle, DWORD> nt_open_file(
        const Ntdll& ntdll,
        const std::wstring_view device_name,
        const ACCESS_MASK desired_access,
        const HANDLE parent,
        const bool inheritable,
        const ULONG open_options,
        const bool verbose_failures) noexcept
    {
        if (device_name.size() > (std::numeric_limits<USHORT>::max() / sizeof(wchar_t)))
        {
            return std::unexpected(ERROR_FILENAME_EXCED_RANGE);
        }

        std::wstring name_storage(device_name);
        name_storage.push_back(L'\0');

        UNICODE_STRING name{};
        name.Buffer = name_storage.data();
        name.Length = static_cast<USHORT>(device_name.size() * sizeof(wchar_t));
        name.MaximumLength = static_cast<USHORT>(name.Length + sizeof(wchar_t));

        ULONG attributes = OBJ_CASE_INSENSITIVE;
        if (inheritable)
        {
            attributes |= OBJ_INHERIT;
        }

        OBJECT_ATTRIBUTES object_attributes{};
        InitializeObjectAttributes(
            &object_attributes,
            &name,
            attributes,
            parent,
            nullptr);

        IO_STATUS_BLOCK io_status{};
        HANDLE handle = nullptr;
        const NTSTATUS status = ntdll.nt_open_file(
            &handle,
            desired_access,
            &object_attributes,
            &io_status,
            FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
            open_options);
        if (status < 0)
        {
            const DWORD error = static_cast<DWORD>(ntdll.rtl_nt_status_to_dos_error(status));
            if (verbose_failures)
            {
                fwprintf(stderr,
                         L"[DETAIL] NtOpenFile(%ls) failed (status=0x%08X win32=%lu)\n",
                         name_storage.c_str(),
                         static_cast<unsigned int>(status),
                         error);
            }
            return std::unexpected(error == 0 ? ERROR_GEN_FAILURE : error);
        }

        return oc::core::UniqueHandle(handle);
    }

    struct ConDrvHandleBundle final
    {
        oc::core::UniqueHandle server;
        oc::core::UniqueHandle reference;
        oc::core::UniqueHandle input;
        oc::core::UniqueHandle output;
        oc::core::UniqueHandle error;
    };

    [[nodiscard]] std::expected<ConDrvHandleBundle, DWORD> create_condrv_handle_bundle(const Ntdll& ntdll) noexcept
    {
        constexpr ULONG kFileSynchronousIoNonAlert = 0x00000020;

        ConDrvHandleBundle bundle{};
        auto server = nt_open_file(ntdll, L"\\Device\\ConDrv\\Server", GENERIC_ALL, nullptr, true, 0, true);
        if (!server)
        {
            return std::unexpected(server.error());
        }
        bundle.server = std::move(server.value());

        auto reference = nt_open_file(ntdll, L"\\Reference", GENERIC_READ | GENERIC_WRITE | SYNCHRONIZE, bundle.server.get(), true, kFileSynchronousIoNonAlert, true);
        if (!reference)
        {
            return std::unexpected(reference.error());
        }
        bundle.reference = std::move(reference.value());

        return bundle;
    }

    [[nodiscard]] std::expected<void, DWORD> create_condrv_io_handles(const Ntdll& ntdll, ConDrvHandleBundle& bundle) noexcept
    {
        constexpr ULONG kFileSynchronousIoNonAlert = 0x00000020;

        auto input = nt_open_file(ntdll, L"\\Input", GENERIC_READ | GENERIC_WRITE | SYNCHRONIZE, bundle.server.get(), true, kFileSynchronousIoNonAlert, false);
        if (!input)
        {
            return std::unexpected(input.error());
        }
        bundle.input = std::move(input.value());

        auto output = nt_open_file(ntdll, L"\\Output", GENERIC_READ | GENERIC_WRITE | SYNCHRONIZE, bundle.server.get(), true, kFileSynchronousIoNonAlert, false);
        if (!output)
        {
            return std::unexpected(output.error());
        }
        bundle.output = std::move(output.value());

        if (::DuplicateHandle(
                ::GetCurrentProcess(),
                bundle.output.get(),
                ::GetCurrentProcess(),
                bundle.error.put(),
                0,
                TRUE,
                DUPLICATE_SAME_ACCESS) == FALSE)
        {
            return std::unexpected(::GetLastError());
        }

        return {};
    }

    struct ProcessInfo final
    {
        oc::core::UniqueHandle process;
        oc::core::UniqueHandle thread;
    };

    [[nodiscard]] std::expected<ProcessInfo, DWORD> spawn_process_with_attributes(
        const std::wstring& application,
        std::wstring command_line,
        const HANDLE stdin_handle,
        const HANDLE stdout_handle,
        const HANDLE stderr_handle,
        const std::vector<HANDLE>& handles_to_inherit,
        const HANDLE console_reference,
        const DWORD creation_flags) noexcept
    {
        // Prepare mutable command line buffer.
        std::vector<wchar_t> mutable_command_line;
        mutable_command_line.reserve(command_line.size() + 1);
        mutable_command_line.insert(mutable_command_line.end(), command_line.begin(), command_line.end());
        mutable_command_line.push_back(L'\0');

        const bool include_console_reference = console_reference != nullptr;
        const DWORD attribute_count = include_console_reference ? 2 : 1;

        SIZE_T attribute_list_size = 0;
        (void)::InitializeProcThreadAttributeList(nullptr, attribute_count, 0, &attribute_list_size);
        if (attribute_list_size == 0)
        {
            return std::unexpected(::GetLastError());
        }

        std::vector<std::byte> attribute_storage(attribute_list_size);
        auto* const attribute_list = reinterpret_cast<PPROC_THREAD_ATTRIBUTE_LIST>(attribute_storage.data());
        if (::InitializeProcThreadAttributeList(attribute_list, attribute_count, 0, &attribute_list_size) == FALSE)
        {
            return std::unexpected(::GetLastError());
        }
        struct AttributeCleanup final
        {
            PPROC_THREAD_ATTRIBUTE_LIST list{};
            ~AttributeCleanup() noexcept
            {
                if (list != nullptr)
                {
                    ::DeleteProcThreadAttributeList(list);
                }
            }
        } cleanup{ attribute_list };

        if (include_console_reference)
        {
            HANDLE reference_value = console_reference;
            if (::UpdateProcThreadAttribute(
                    attribute_list,
                    0,
                    PROC_THREAD_ATTRIBUTE_CONSOLE_REFERENCE,
                    &reference_value,
                    sizeof(reference_value),
                    nullptr,
                    nullptr) == FALSE)
            {
                return std::unexpected(::GetLastError());
            }
        }

        if (!handles_to_inherit.empty())
        {
            if (::UpdateProcThreadAttribute(
                    attribute_list,
                    0,
                    PROC_THREAD_ATTRIBUTE_HANDLE_LIST,
                    const_cast<HANDLE*>(handles_to_inherit.data()),
                    static_cast<DWORD>(handles_to_inherit.size() * sizeof(HANDLE)),
                    nullptr,
                    nullptr) == FALSE)
            {
                return std::unexpected(::GetLastError());
            }
        }

        STARTUPINFOEXW startup{};
        startup.StartupInfo.cb = sizeof(startup);
        startup.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
        startup.StartupInfo.hStdInput = stdin_handle;
        startup.StartupInfo.hStdOutput = stdout_handle;
        startup.StartupInfo.hStdError = stderr_handle;
        startup.lpAttributeList = attribute_list;

        PROCESS_INFORMATION info{};
        const DWORD flags = EXTENDED_STARTUPINFO_PRESENT | creation_flags;
        const BOOL created = ::CreateProcessW(
            application.c_str(),
            mutable_command_line.data(),
            nullptr,
            nullptr,
            TRUE,
            flags,
            nullptr,
            nullptr,
            &startup.StartupInfo,
            &info);
        if (created == FALSE)
        {
            return std::unexpected(::GetLastError());
        }

        return ProcessInfo{
            .process = oc::core::UniqueHandle(info.hProcess),
            .thread = oc::core::UniqueHandle(info.hThread),
        };
    }

    void dump_text_file_preview(std::wstring_view path) noexcept
    {
        oc::core::UniqueHandle file(::CreateFileW(
            path.data(),
            GENERIC_READ,
            FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
            nullptr,
            OPEN_EXISTING,
            FILE_ATTRIBUTE_NORMAL,
            nullptr));
        if (!file.valid())
        {
            fwprintf(stderr, L"[DETAIL] log file not available (CreateFileW error=%lu)\n", ::GetLastError());
            return;
        }

        LARGE_INTEGER size{};
        if (::GetFileSizeEx(file.get(), &size) == FALSE || size.QuadPart < 0)
        {
            fwprintf(stderr, L"[DETAIL] log file size query failed (error=%lu)\n", ::GetLastError());
            return;
        }

        constexpr DWORD kMaxBytes = 32 * 1024;
        const DWORD bytes_to_read = size.QuadPart > kMaxBytes ? kMaxBytes : static_cast<DWORD>(size.QuadPart);
        std::vector<char> bytes(bytes_to_read);
        DWORD read = 0;
        if (bytes_to_read > 0 &&
            (::ReadFile(file.get(), bytes.data(), bytes_to_read, &read, nullptr) == FALSE))
        {
            fwprintf(stderr, L"[DETAIL] log file ReadFile failed (error=%lu)\n", ::GetLastError());
            return;
        }
        bytes.resize(read);

        // Skip UTF-8 BOM if present.
        size_t offset = 0;
        if (bytes.size() >= 3 &&
            static_cast<unsigned char>(bytes[0]) == 0xEF &&
            static_cast<unsigned char>(bytes[1]) == 0xBB &&
            static_cast<unsigned char>(bytes[2]) == 0xBF)
        {
            offset = 3;
        }

        const int wide_required = ::MultiByteToWideChar(
            CP_UTF8,
            MB_ERR_INVALID_CHARS,
            bytes.data() + offset,
            static_cast<int>(bytes.size() - offset),
            nullptr,
            0);
        if (wide_required <= 0)
        {
            fwprintf(stderr, L"[DETAIL] log file is not valid UTF-8\n");
            return;
        }

        std::wstring wide(static_cast<size_t>(wide_required), L'\0');
        const int converted = ::MultiByteToWideChar(
            CP_UTF8,
            MB_ERR_INVALID_CHARS,
            bytes.data() + offset,
            static_cast<int>(bytes.size() - offset),
            wide.data(),
            wide_required);
        if (converted != wide_required)
        {
            fwprintf(stderr, L"[DETAIL] log file UTF-8 conversion failed\n");
            return;
        }

        fwprintf(stderr, L"[DETAIL] openconsole_new log preview (%zu chars):\n%ls\n", wide.size(), wide.c_str());
    }

    void dump_bytes_preview(const std::vector<std::byte>& bytes) noexcept
    {
        constexpr size_t kMaxBytes = 512;
        const size_t count = bytes.size() < kMaxBytes ? bytes.size() : kMaxBytes;

        fwprintf(stderr, L"[DETAIL] captured %zu bytes; showing first %zu bytes as hex:\n", bytes.size(), count);
        for (size_t i = 0; i < count; ++i)
        {
            const unsigned value = static_cast<unsigned>(bytes[i]);
            fwprintf(stderr, L"%02X ", value);
            if (((i + 1) % 16) == 0)
            {
                fwprintf(stderr, L"\n");
            }
        }
        if ((count % 16) != 0)
        {
            fwprintf(stderr, L"\n");
        }

        fwprintf(stderr, L"[DETAIL] ascii preview:\n");
        for (size_t i = 0; i < count; ++i)
        {
            const unsigned char ch = static_cast<unsigned char>(bytes[i]);
            const wchar_t out = (ch >= 0x20 && ch <= 0x7E) ? static_cast<wchar_t>(ch) : L'.';
            fputwc(out, stderr);
        }
        fwprintf(stderr, L"\n");
    }

    struct CapturedProcess final
    {
        DWORD exit_code{};
        std::vector<std::byte> output;
    };

    [[nodiscard]] std::expected<CapturedProcess, DWORD> run_process_capture_output(
        const std::wstring& application,
        std::wstring command_line,
        const std::optional<std::string_view> stdin_bytes,
        const DWORD timeout_ms) noexcept
    {
        auto stdout_pipe = create_pipe_inherit_write_end();
        if (!stdout_pipe)
        {
            return std::unexpected(stdout_pipe.error());
        }

        std::optional<InheritablePipe> stdin_pipe;
        oc::core::UniqueHandle nul_input;
        if (stdin_bytes)
        {
            auto created = create_pipe_inherit_read_end();
            if (!created)
            {
                return std::unexpected(created.error());
            }
            stdin_pipe = std::move(created.value());
        }
        else
        {
            SECURITY_ATTRIBUTES security{};
            security.nLength = sizeof(security);
            security.lpSecurityDescriptor = nullptr;
            security.bInheritHandle = TRUE;

            nul_input = oc::core::UniqueHandle(::CreateFileW(
                L"NUL",
                GENERIC_READ,
                FILE_SHARE_READ | FILE_SHARE_WRITE,
                &security,
                OPEN_EXISTING,
                FILE_ATTRIBUTE_NORMAL,
                nullptr));
            if (!nul_input.valid())
            {
                return std::unexpected(::GetLastError());
            }
        }

        // Prepare the mutable command line buffer.
        std::vector<wchar_t> mutable_command_line;
        mutable_command_line.reserve(command_line.size() + 1);
        mutable_command_line.insert(mutable_command_line.end(), command_line.begin(), command_line.end());
        mutable_command_line.push_back(L'\0');

        STARTUPINFOW startup{};
        startup.cb = sizeof(startup);
        startup.dwFlags = STARTF_USESTDHANDLES;
        startup.hStdInput = stdin_pipe ? stdin_pipe->read.get() : nul_input.get();
        startup.hStdOutput = stdout_pipe->write.get();
        startup.hStdError = stdout_pipe->write.get();

        PROCESS_INFORMATION info{};
        const BOOL created = ::CreateProcessW(
            application.c_str(),
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
            return std::unexpected(::GetLastError());
        }

        oc::core::UniqueHandle process(info.hProcess);
        oc::core::UniqueHandle thread(info.hThread);

        // Close our copies of the inherited ends promptly.
        stdout_pipe->write.reset();
        if (stdin_pipe)
        {
            stdin_pipe->read.reset();
        }

        if (stdin_pipe && stdin_bytes && !stdin_bytes->empty())
        {
            DWORD total_written = 0;
            const DWORD to_write = static_cast<DWORD>(stdin_bytes->size());
            while (total_written < to_write)
            {
                DWORD written = 0;
                const BOOL ok = ::WriteFile(
                    stdin_pipe->write.get(),
                    stdin_bytes->data() + total_written,
                    to_write - total_written,
                    &written,
                    nullptr);
                if (ok == FALSE)
                {
                    return std::unexpected(::GetLastError());
                }
                total_written += written;
            }
        }
        if (stdin_pipe)
        {
            // Signal EOF on the host input pipe.
            stdin_pipe->write.reset();
        }

        CapturedProcess captured{};
        captured.output.reserve(4096);

        const ULONGLONG start_tick = ::GetTickCount64();
        bool process_exited = false;
        bool stdout_pipe_broken = false;
        bool draining_after_exit = false;
        ULONGLONG drain_start_tick = 0;
        static constexpr ULONGLONG kDrainTimeoutMs = 250;
        for (;;)
        {
            // Drain any available output.
            bool had_output = false;
            for (;;)
            {
                DWORD available = 0;
                if (::PeekNamedPipe(stdout_pipe->read.get(), nullptr, 0, nullptr, &available, nullptr) == FALSE)
                {
                    const DWORD error = ::GetLastError();
                    if (error == ERROR_BROKEN_PIPE)
                    {
                        stdout_pipe_broken = true;
                    }
                    break;
                }
                if (available == 0)
                {
                    break;
                }

                std::array<std::byte, 8192> buffer{};
                const DWORD to_read = available < buffer.size() ? available : static_cast<DWORD>(buffer.size());
                DWORD read = 0;
                if (::ReadFile(stdout_pipe->read.get(), buffer.data(), to_read, &read, nullptr) == FALSE)
                {
                    const DWORD error = ::GetLastError();
                    if (error == ERROR_BROKEN_PIPE)
                    {
                        stdout_pipe_broken = true;
                    }
                    break;
                }
                if (read == 0)
                {
                    break;
                }

                had_output = true;
                try
                {
                    captured.output.insert(captured.output.end(), buffer.begin(), buffer.begin() + static_cast<size_t>(read));
                }
                catch (...)
                {
                    return std::unexpected(ERROR_OUTOFMEMORY);
                }
            }

            if (process_exited)
            {
                if (stdout_pipe_broken)
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
                    break;
                }

                ::Sleep(1);
                continue;
            }

            const DWORD wait_result = ::WaitForSingleObject(process.get(), 20);
            if (wait_result == WAIT_OBJECT_0)
            {
                process_exited = true;
                continue;
            }
            if (wait_result != WAIT_TIMEOUT)
            {
                return std::unexpected(::GetLastError());
            }

            if (timeout_ms != INFINITE)
            {
                const ULONGLONG now = ::GetTickCount64();
                const ULONGLONG elapsed = now - start_tick;
                if (elapsed >= timeout_ms)
                {
                    (void)::TerminateProcess(process.get(), 0xDEAD);
                    (void)::WaitForSingleObject(process.get(), 5'000);
                    return std::unexpected(WAIT_TIMEOUT);
                }
            }
        }

        DWORD exit_code = 0;
        if (::GetExitCodeProcess(process.get(), &exit_code) == FALSE)
        {
            return std::unexpected(::GetLastError());
        }

        captured.exit_code = exit_code;
        return captured;
    }

    bool test_openconsole_new_headless_conpty_emits_output_and_exit_code()
    {
        const auto openconsole_path = locate_openconsole_new();
        if (!openconsole_path)
        {
            fwprintf(stderr, L"[DETAIL] openconsole_new.exe was not found relative to test binary\n");
            return false;
        }

        const std::wstring application = *openconsole_path;
        const std::wstring cmd =
            quote(application) +
            L" --headless --vtmode -- %ComSpec% /c \"(for /L %i in (1,1,20) do @echo line%i) & exit /b 17\"";

        const std::wstring build_dir = directory_name(application);
        const std::wstring log_path = join_path(build_dir, L"oc_new_process_integration.log");
        (void)::DeleteFileW(log_path.c_str());
        const ScopedEnvironmentVariable log_level(L"OPENCONSOLE_NEW_LOG_LEVEL", L"debug");
        const ScopedEnvironmentVariable log_dir(L"OPENCONSOLE_NEW_LOG_DIR", build_dir);

        auto captured = run_process_capture_output(application, cmd, std::nullopt, 30'000);
        if (!captured)
        {
            fwprintf(stderr, L"[DETAIL] openconsole_new launch failed (error=%lu)\n", captured.error());
            dump_text_file_preview(log_path);
            return false;
        }

        if (captured->exit_code != 17)
        {
            fwprintf(stderr, L"[DETAIL] expected exit code 17, got %lu\n", captured->exit_code);
             dump_bytes_preview(captured->output);
            dump_text_file_preview(log_path);
            return false;
        }

        if (!bytes_contain_ascii(captured->output, "line20"))
        {
            fwprintf(stderr, L"[DETAIL] did not observe expected output token 'line20'\n");
            return false;
        }

        return true;
    }

    bool test_openconsole_new_pipe_input_reaches_client()
    {
        const auto openconsole_path = locate_openconsole_new();
        if (!openconsole_path)
        {
            fwprintf(stderr, L"[DETAIL] openconsole_new.exe was not found relative to test binary\n");
            return false;
        }

        const std::wstring application = *openconsole_path;

        // Avoid `%var%` expansions (the runtime expands env strings before CreateProcessW).
        const std::wstring cmd =
            quote(application) +
            L" --headless --vtmode -- powershell -NoLogo -NoProfile -Command "
            L"\"if ([Console]::IsOutputRedirected) { [Console]::Out.WriteLine('OUT_REDIRECTED'); exit 7 } "
            L"$x=[Console]::In.ReadLine(); [Console]::Out.WriteLine('X'+$x+'Y'); exit 0\"";

        const std::wstring build_dir = directory_name(application);
        const std::wstring log_path = join_path(build_dir, L"oc_new_process_integration.log");
        (void)::DeleteFileW(log_path.c_str());
        const ScopedEnvironmentVariable log_level(L"OPENCONSOLE_NEW_LOG_LEVEL", L"debug");
        const ScopedEnvironmentVariable log_dir(L"OPENCONSOLE_NEW_LOG_DIR", build_dir);

        constexpr std::string_view input = "abc\r\n";
        auto captured = run_process_capture_output(application, cmd, input, 30'000);
        if (!captured)
        {
            fwprintf(stderr, L"[DETAIL] openconsole_new launch failed (error=%lu)\n", captured.error());
            dump_text_file_preview(log_path);
            return false;
        }

        if (captured->exit_code != 0)
        {
            fwprintf(stderr, L"[DETAIL] expected exit code 0, got %lu\n", captured->exit_code);
            dump_bytes_preview(captured->output);
            dump_text_file_preview(log_path);
            return false;
        }

        if (!bytes_contain_ascii(captured->output, "XabcY"))
        {
            fwprintf(stderr, L"[DETAIL] did not observe expected output token 'XabcY'\n");
            dump_bytes_preview(captured->output);
            dump_text_file_preview(log_path);
            return false;
        }

        return true;
    }

    bool test_openconsole_new_headless_condrv_server_end_to_end_basic_io()
    {
        const auto openconsole_path = locate_openconsole_new();
        if (!openconsole_path)
        {
            fwprintf(stderr, L"[DETAIL] openconsole_new.exe was not found relative to test binary\n");
            return false;
        }

        const auto client_path = locate_condrv_client_smoke();
        if (!client_path)
        {
            fwprintf(stderr, L"[DETAIL] oc_new_condrv_client_smoke.exe was not found relative to test binary\n");
            return false;
        }

        const auto ntdll = load_ntdll();
        if (!ntdll)
        {
            fwprintf(stderr, L"[DETAIL] ntdll native entrypoints were unavailable\n");
            return false;
        }

        auto condrv_handles = create_condrv_handle_bundle(*ntdll);
        if (!condrv_handles)
        {
            fwprintf(stderr, L"[DETAIL] failed to create ConDrv handle bundle (error=%lu)\n", condrv_handles.error());
            return false;
        }

        auto stdout_pipe = create_pipe_inherit_write_end();
        if (!stdout_pipe)
        {
            fwprintf(stderr, L"[DETAIL] failed to create stdout pipe (error=%lu)\n", stdout_pipe.error());
            return false;
        }

        auto stdin_pipe = create_pipe_inherit_read_end();
        if (!stdin_pipe)
        {
            fwprintf(stderr, L"[DETAIL] failed to create stdin pipe (error=%lu)\n", stdin_pipe.error());
            return false;
        }

        const std::wstring application = *openconsole_path;
        const std::wstring build_dir = directory_name(application);
        const std::wstring log_path = join_path(build_dir, L"oc_new_condrv_process_integration.log");
        (void)::DeleteFileW(log_path.c_str());
        const ScopedEnvironmentVariable log_level(L"OPENCONSOLE_NEW_LOG_LEVEL", L"debug");
        const ScopedEnvironmentVariable log_dir(L"OPENCONSOLE_NEW_LOG_DIR", build_dir);

        const auto server_handle_value = static_cast<unsigned long long>(condrv_handles->server.view().as_uintptr());
        wchar_t server_handle_text[32]{};
        const int formatted = swprintf_s(server_handle_text, L"0x%llX", server_handle_value);
        if (formatted <= 0)
        {
            fwprintf(stderr, L"[DETAIL] failed to format server handle value\n");
            return false;
        }

        const std::wstring server_cmd =
            quote(application) +
            L" --server " +
            server_handle_text +
            L" --headless";

        std::vector<HANDLE> server_handle_list;
        server_handle_list.push_back(condrv_handles->server.get());
        server_handle_list.push_back(stdin_pipe->read.get());
        server_handle_list.push_back(stdout_pipe->write.get());

        auto server_process = spawn_process_with_attributes(
            application,
            server_cmd,
            stdin_pipe->read.get(),
            stdout_pipe->write.get(),
            stdout_pipe->write.get(),
            server_handle_list,
            nullptr,
            CREATE_NO_WINDOW);
        if (!server_process)
        {
            fwprintf(stderr, L"[DETAIL] failed to spawn server process (error=%lu)\n", server_process.error());
            dump_text_file_preview(log_path);
            return false;
        }

        // Close our copies of the ends that the server inherited.
        stdout_pipe->write.reset();
        stdin_pipe->read.reset();

        // ConDrv rejects opening the default I/O objects until the server has
        // registered and started its I/O loop. Wait briefly for it to become
        // ready before creating the client handles.
        const ULONGLONG io_start_tick = ::GetTickCount64();
        for (;;)
        {
            auto created = create_condrv_io_handles(*ntdll, *condrv_handles);
            if (created)
            {
                break;
            }

            if (created.error() != ERROR_BAD_COMMAND)
            {
                fwprintf(stderr, L"[DETAIL] failed to create ConDrv I/O handles (error=%lu)\n", created.error());
                (void)::TerminateProcess(server_process->process.get(), 0xBADC0DE);
                (void)::WaitForSingleObject(server_process->process.get(), 5'000);
                dump_text_file_preview(log_path);
                return false;
            }

            const ULONGLONG now = ::GetTickCount64();
            if ((now - io_start_tick) >= 5'000)
            {
                fwprintf(stderr, L"[DETAIL] timed out waiting for ConDrv server readiness\n");
                (void)::TerminateProcess(server_process->process.get(), 0xBADC0DE);
                (void)::WaitForSingleObject(server_process->process.get(), 5'000);
                dump_text_file_preview(log_path);
                return false;
            }

            ::Sleep(10);
        }

        const std::wstring client_application = *client_path;
        const std::wstring client_cmd = quote(client_application);

        std::vector<HANDLE> client_handle_list;
        client_handle_list.push_back(condrv_handles->input.get());
        client_handle_list.push_back(condrv_handles->output.get());
        client_handle_list.push_back(condrv_handles->error.get());
        client_handle_list.push_back(condrv_handles->reference.get());

        auto client_process = spawn_process_with_attributes(
            client_application,
            client_cmd,
            condrv_handles->input.get(),
            condrv_handles->output.get(),
            condrv_handles->error.get(),
            client_handle_list,
            condrv_handles->reference.get(),
            0);
        if (!client_process)
        {
            fwprintf(stderr, L"[DETAIL] failed to spawn client process (error=%lu)\n", client_process.error());
            (void)::TerminateProcess(server_process->process.get(), 0xBADC0DE);
            (void)::WaitForSingleObject(server_process->process.get(), 5'000);
            dump_text_file_preview(log_path);
            return false;
        }

        // The client now owns its inherited handles; close our extra references so
        // the driver can observe disconnect once the client exits.
        condrv_handles->reference.reset();
        condrv_handles->input.reset();
        condrv_handles->output.reset();
        condrv_handles->error.reset();

        constexpr std::string_view input_bytes = "abc";
        if (!input_bytes.empty())
        {
            DWORD total_written = 0;
            const DWORD to_write = static_cast<DWORD>(input_bytes.size());
            while (total_written < to_write)
            {
                DWORD written = 0;
                if (::WriteFile(
                        stdin_pipe->write.get(),
                        input_bytes.data() + total_written,
                        to_write - total_written,
                        &written,
                        nullptr) == FALSE)
                {
                    fwprintf(stderr, L"[DETAIL] failed to write host input (error=%lu)\n", ::GetLastError());
                    (void)::TerminateProcess(client_process->process.get(), 0xBADC0DE);
                    (void)::TerminateProcess(server_process->process.get(), 0xBADC0DE);
                    (void)::WaitForSingleObject(client_process->process.get(), 5'000);
                    (void)::WaitForSingleObject(server_process->process.get(), 5'000);
                    dump_text_file_preview(log_path);
                    return false;
                }
                total_written += written;
            }
        }

        // Signal EOF on host input so the input monitor can terminate cleanly.
        stdin_pipe->write.reset();

        std::vector<std::byte> captured;
        captured.reserve(4096);

        const ULONGLONG start_tick = ::GetTickCount64();
        bool client_exited = false;
        bool server_exited = false;

        for (;;)
        {
            // Drain any available server stdout output.
            for (;;)
            {
                DWORD available = 0;
                if (::PeekNamedPipe(stdout_pipe->read.get(), nullptr, 0, nullptr, &available, nullptr) == FALSE)
                {
                    break;
                }
                if (available == 0)
                {
                    break;
                }

                std::array<std::byte, 8192> buffer{};
                const DWORD to_read = available < buffer.size() ? available : static_cast<DWORD>(buffer.size());
                DWORD read = 0;
                if (::ReadFile(stdout_pipe->read.get(), buffer.data(), to_read, &read, nullptr) == FALSE)
                {
                    break;
                }
                if (read == 0)
                {
                    break;
                }

                try
                {
                    captured.insert(captured.end(), buffer.begin(), buffer.begin() + static_cast<size_t>(read));
                }
                catch (...)
                {
                    fwprintf(stderr, L"[DETAIL] out of memory capturing server output\n");
                    return false;
                }
            }

            if (!client_exited)
            {
                const DWORD wait_result = ::WaitForSingleObject(client_process->process.get(), 0);
                if (wait_result == WAIT_OBJECT_0)
                {
                    client_exited = true;
                }
                else if (wait_result != WAIT_TIMEOUT)
                {
                    fwprintf(stderr, L"[DETAIL] client WaitForSingleObject failed (wait=%lu error=%lu)\n", wait_result, ::GetLastError());
                    return false;
                }
            }

            if (!server_exited)
            {
                const DWORD wait_result = ::WaitForSingleObject(server_process->process.get(), 0);
                if (wait_result == WAIT_OBJECT_0)
                {
                    server_exited = true;
                }
                else if (wait_result != WAIT_TIMEOUT)
                {
                    fwprintf(stderr, L"[DETAIL] server WaitForSingleObject failed (wait=%lu error=%lu)\n", wait_result, ::GetLastError());
                    return false;
                }
            }

            if (client_exited && server_exited)
            {
                DWORD available = 0;
                if (::PeekNamedPipe(stdout_pipe->read.get(), nullptr, 0, nullptr, &available, nullptr) == FALSE || available == 0)
                {
                    break;
                }

                // Still have buffered output; drain it in the next iteration.
                continue;
            }

            const DWORD wait_handles_count = (client_exited ? 0 : 1) + (server_exited ? 0 : 1);
            if (wait_handles_count > 0)
            {
                HANDLE wait_handles[2]{};
                DWORD wait_index = 0;
                if (!client_exited)
                {
                    wait_handles[wait_index++] = client_process->process.get();
                }
                if (!server_exited)
                {
                    wait_handles[wait_index++] = server_process->process.get();
                }

                const DWORD wait_result = ::WaitForMultipleObjects(wait_handles_count, wait_handles, FALSE, 20);
                if (wait_result == WAIT_FAILED)
                {
                    fwprintf(stderr, L"[DETAIL] WaitForMultipleObjects failed (error=%lu)\n", ::GetLastError());
                    return false;
                }
            }

            const ULONGLONG now = ::GetTickCount64();
            if ((now - start_tick) >= 30'000)
            {
                fwprintf(stderr, L"[DETAIL] condrv integration timed out\n");
                (void)::TerminateProcess(client_process->process.get(), 0xDEAD);
                (void)::TerminateProcess(server_process->process.get(), 0xDEAD);
                (void)::WaitForSingleObject(client_process->process.get(), 5'000);
                (void)::WaitForSingleObject(server_process->process.get(), 5'000);
                dump_bytes_preview(captured);
                dump_text_file_preview(log_path);
                return false;
            }
        }

        DWORD client_exit_code = 0;
        DWORD server_exit_code = 0;
        if (::GetExitCodeProcess(client_process->process.get(), &client_exit_code) == FALSE ||
            ::GetExitCodeProcess(server_process->process.get(), &server_exit_code) == FALSE)
        {
            fwprintf(stderr, L"[DETAIL] GetExitCodeProcess failed (error=%lu)\n", ::GetLastError());
            return false;
        }

        if (client_exit_code != 0 || server_exit_code != 0)
        {
            fwprintf(stderr, L"[DETAIL] expected client/server exit codes 0/0, got %lu/%lu\n", client_exit_code, server_exit_code);
            dump_bytes_preview(captured);
            dump_text_file_preview(log_path);
            return false;
        }

        if (!bytes_contain_ascii(captured, "HELLO") || !bytes_contain_ascii(captured, "XabcY"))
        {
            fwprintf(stderr, L"[DETAIL] did not observe expected condrv output tokens\n");
            dump_bytes_preview(captured);
            dump_text_file_preview(log_path);
            return false;
        }

        return true;
    }

    bool test_openconsole_new_headless_condrv_server_end_to_end_input_events()
    {
        const auto openconsole_path = locate_openconsole_new();
        if (!openconsole_path)
        {
            fwprintf(stderr, L"[DETAIL] openconsole_new.exe was not found relative to test binary\n");
            return false;
        }

        const auto client_path = locate_condrv_client_input_events();
        if (!client_path)
        {
            fwprintf(stderr, L"[DETAIL] oc_new_condrv_client_input_events.exe was not found relative to test binary\n");
            return false;
        }

        const auto ntdll = load_ntdll();
        if (!ntdll)
        {
            fwprintf(stderr, L"[DETAIL] ntdll native entrypoints were unavailable\n");
            return false;
        }

        auto condrv_handles = create_condrv_handle_bundle(*ntdll);
        if (!condrv_handles)
        {
            fwprintf(stderr, L"[DETAIL] failed to create ConDrv handle bundle (error=%lu)\n", condrv_handles.error());
            return false;
        }

        auto stdout_pipe = create_pipe_inherit_write_end();
        if (!stdout_pipe)
        {
            fwprintf(stderr, L"[DETAIL] failed to create stdout pipe (error=%lu)\n", stdout_pipe.error());
            return false;
        }

        auto stdin_pipe = create_pipe_inherit_read_end();
        if (!stdin_pipe)
        {
            fwprintf(stderr, L"[DETAIL] failed to create stdin pipe (error=%lu)\n", stdin_pipe.error());
            return false;
        }

        const std::wstring application = *openconsole_path;
        const std::wstring build_dir = directory_name(application);
        const std::wstring log_path = join_path(build_dir, L"oc_new_condrv_process_integration_input_events.log");
        (void)::DeleteFileW(log_path.c_str());
        const ScopedEnvironmentVariable log_level(L"OPENCONSOLE_NEW_LOG_LEVEL", L"debug");
        const ScopedEnvironmentVariable log_dir(L"OPENCONSOLE_NEW_LOG_DIR", build_dir);

        const auto server_handle_value = static_cast<unsigned long long>(condrv_handles->server.view().as_uintptr());
        wchar_t server_handle_text[32]{};
        const int formatted = swprintf_s(server_handle_text, L"0x%llX", server_handle_value);
        if (formatted <= 0)
        {
            fwprintf(stderr, L"[DETAIL] failed to format server handle value\n");
            return false;
        }

        const std::wstring server_cmd =
            quote(application) +
            L" --server " +
            server_handle_text +
            L" --headless";

        std::vector<HANDLE> server_handle_list;
        server_handle_list.push_back(condrv_handles->server.get());
        server_handle_list.push_back(stdin_pipe->read.get());
        server_handle_list.push_back(stdout_pipe->write.get());

        auto server_process = spawn_process_with_attributes(
            application,
            server_cmd,
            stdin_pipe->read.get(),
            stdout_pipe->write.get(),
            stdout_pipe->write.get(),
            server_handle_list,
            nullptr,
            CREATE_NO_WINDOW);
        if (!server_process)
        {
            fwprintf(stderr, L"[DETAIL] failed to spawn server process (error=%lu)\n", server_process.error());
            dump_text_file_preview(log_path);
            return false;
        }

        stdout_pipe->write.reset();
        stdin_pipe->read.reset();

        const ULONGLONG io_start_tick = ::GetTickCount64();
        for (;;)
        {
            auto created = create_condrv_io_handles(*ntdll, *condrv_handles);
            if (created)
            {
                break;
            }

            if (created.error() != ERROR_BAD_COMMAND)
            {
                fwprintf(stderr, L"[DETAIL] failed to create ConDrv I/O handles (error=%lu)\n", created.error());
                (void)::TerminateProcess(server_process->process.get(), 0xBADC0DE);
                (void)::WaitForSingleObject(server_process->process.get(), 5'000);
                dump_text_file_preview(log_path);
                return false;
            }

            const ULONGLONG now = ::GetTickCount64();
            if ((now - io_start_tick) >= 5'000)
            {
                fwprintf(stderr, L"[DETAIL] timed out waiting for ConDrv server readiness\n");
                (void)::TerminateProcess(server_process->process.get(), 0xBADC0DE);
                (void)::WaitForSingleObject(server_process->process.get(), 5'000);
                dump_text_file_preview(log_path);
                return false;
            }

            ::Sleep(10);
        }

        const std::wstring client_application = *client_path;
        const std::wstring client_cmd = quote(client_application);

        std::vector<HANDLE> client_handle_list;
        client_handle_list.push_back(condrv_handles->input.get());
        client_handle_list.push_back(condrv_handles->output.get());
        client_handle_list.push_back(condrv_handles->error.get());
        client_handle_list.push_back(condrv_handles->reference.get());

        auto client_process = spawn_process_with_attributes(
            client_application,
            client_cmd,
            condrv_handles->input.get(),
            condrv_handles->output.get(),
            condrv_handles->error.get(),
            client_handle_list,
            condrv_handles->reference.get(),
            0);
        if (!client_process)
        {
            fwprintf(stderr, L"[DETAIL] failed to spawn client process (error=%lu)\n", client_process.error());
            (void)::TerminateProcess(server_process->process.get(), 0xBADC0DE);
            (void)::WaitForSingleObject(server_process->process.get(), 5'000);
            dump_text_file_preview(log_path);
            return false;
        }

        condrv_handles->reference.reset();
        condrv_handles->input.reset();
        condrv_handles->output.reset();
        condrv_handles->error.reset();

        constexpr std::string_view input_bytes =
            "\x1b[65;0;97;1;0;1_"  // 'a' keydown
            "\x1b[38;0;0;1;0;1_";  // VK_UP keydown

        if (!input_bytes.empty())
        {
            DWORD total_written = 0;
            const DWORD to_write = static_cast<DWORD>(input_bytes.size());
            while (total_written < to_write)
            {
                DWORD written = 0;
                if (::WriteFile(
                        stdin_pipe->write.get(),
                        input_bytes.data() + total_written,
                        to_write - total_written,
                        &written,
                        nullptr) == FALSE)
                {
                    fwprintf(stderr, L"[DETAIL] failed to write host input (error=%lu)\n", ::GetLastError());
                    (void)::TerminateProcess(client_process->process.get(), 0xBADC0DE);
                    (void)::TerminateProcess(server_process->process.get(), 0xBADC0DE);
                    (void)::WaitForSingleObject(client_process->process.get(), 5'000);
                    (void)::WaitForSingleObject(server_process->process.get(), 5'000);
                    dump_text_file_preview(log_path);
                    return false;
                }
                total_written += written;
            }
        }

        stdin_pipe->write.reset();

        std::vector<std::byte> captured;
        captured.reserve(2048);

        const ULONGLONG start_tick = ::GetTickCount64();
        bool client_exited = false;
        bool server_exited = false;

        for (;;)
        {
            for (;;)
            {
                DWORD available = 0;
                if (::PeekNamedPipe(stdout_pipe->read.get(), nullptr, 0, nullptr, &available, nullptr) == FALSE)
                {
                    break;
                }
                if (available == 0)
                {
                    break;
                }

                std::array<std::byte, 8192> buffer{};
                const DWORD to_read = available < buffer.size() ? available : static_cast<DWORD>(buffer.size());
                DWORD read = 0;
                if (::ReadFile(stdout_pipe->read.get(), buffer.data(), to_read, &read, nullptr) == FALSE)
                {
                    break;
                }
                if (read == 0)
                {
                    break;
                }

                try
                {
                    captured.insert(captured.end(), buffer.begin(), buffer.begin() + static_cast<size_t>(read));
                }
                catch (...)
                {
                    fwprintf(stderr, L"[DETAIL] out of memory capturing server output\n");
                    return false;
                }
            }

            if (!client_exited)
            {
                const DWORD wait_result = ::WaitForSingleObject(client_process->process.get(), 0);
                if (wait_result == WAIT_OBJECT_0)
                {
                    client_exited = true;
                }
                else if (wait_result != WAIT_TIMEOUT)
                {
                    fwprintf(stderr, L"[DETAIL] client WaitForSingleObject failed (wait=%lu error=%lu)\n", wait_result, ::GetLastError());
                    return false;
                }
            }

            if (!server_exited)
            {
                const DWORD wait_result = ::WaitForSingleObject(server_process->process.get(), 0);
                if (wait_result == WAIT_OBJECT_0)
                {
                    server_exited = true;
                }
                else if (wait_result != WAIT_TIMEOUT)
                {
                    fwprintf(stderr, L"[DETAIL] server WaitForSingleObject failed (wait=%lu error=%lu)\n", wait_result, ::GetLastError());
                    return false;
                }
            }

            if (client_exited && server_exited)
            {
                DWORD available = 0;
                if (::PeekNamedPipe(stdout_pipe->read.get(), nullptr, 0, nullptr, &available, nullptr) == FALSE || available == 0)
                {
                    break;
                }

                continue;
            }

            const DWORD wait_handles_count = (client_exited ? 0 : 1) + (server_exited ? 0 : 1);
            if (wait_handles_count > 0)
            {
                HANDLE wait_handles[2]{};
                DWORD wait_index = 0;
                if (!client_exited)
                {
                    wait_handles[wait_index++] = client_process->process.get();
                }
                if (!server_exited)
                {
                    wait_handles[wait_index++] = server_process->process.get();
                }

                const DWORD wait_result = ::WaitForMultipleObjects(wait_handles_count, wait_handles, FALSE, 20);
                if (wait_result == WAIT_FAILED)
                {
                    fwprintf(stderr, L"[DETAIL] WaitForMultipleObjects failed (error=%lu)\n", ::GetLastError());
                    return false;
                }
            }

            const ULONGLONG now = ::GetTickCount64();
            if ((now - start_tick) >= 30'000)
            {
                fwprintf(stderr, L"[DETAIL] condrv input-events integration timed out\n");
                (void)::TerminateProcess(client_process->process.get(), 0xDEAD);
                (void)::TerminateProcess(server_process->process.get(), 0xDEAD);
                (void)::WaitForSingleObject(client_process->process.get(), 5'000);
                (void)::WaitForSingleObject(server_process->process.get(), 5'000);
                dump_bytes_preview(captured);
                dump_text_file_preview(log_path);
                return false;
            }
        }

        DWORD client_exit_code = 0;
        DWORD server_exit_code = 0;
        if (::GetExitCodeProcess(client_process->process.get(), &client_exit_code) == FALSE ||
            ::GetExitCodeProcess(server_process->process.get(), &server_exit_code) == FALSE)
        {
            fwprintf(stderr, L"[DETAIL] GetExitCodeProcess failed (error=%lu)\n", ::GetLastError());
            return false;
        }

        if (client_exit_code != 0 || server_exit_code != 0)
        {
            fwprintf(stderr, L"[DETAIL] expected client/server exit codes 0/0, got %lu/%lu\n", client_exit_code, server_exit_code);
            dump_bytes_preview(captured);
            dump_text_file_preview(log_path);
            return false;
        }

        if (!bytes_contain_ascii(captured, "INPUTOK"))
        {
            fwprintf(stderr, L"[DETAIL] did not observe expected condrv input-events token\n");
            dump_bytes_preview(captured);
            dump_text_file_preview(log_path);
            return false;
        }

        return true;
    }

    bool test_openconsole_new_headless_condrv_server_end_to_end_raw_read()
    {
        const auto openconsole_path = locate_openconsole_new();
        if (!openconsole_path)
        {
            fwprintf(stderr, L"[DETAIL] openconsole_new.exe was not found relative to test binary\n");
            return false;
        }

        const auto client_path = locate_condrv_client_raw_read();
        if (!client_path)
        {
            fwprintf(stderr, L"[DETAIL] oc_new_condrv_client_raw_read.exe was not found relative to test binary\n");
            return false;
        }

        const auto ntdll = load_ntdll();
        if (!ntdll)
        {
            fwprintf(stderr, L"[DETAIL] ntdll native entrypoints were unavailable\n");
            return false;
        }

        auto condrv_handles = create_condrv_handle_bundle(*ntdll);
        if (!condrv_handles)
        {
            fwprintf(stderr, L"[DETAIL] failed to create ConDrv handle bundle (error=%lu)\n", condrv_handles.error());
            return false;
        }

        auto stdout_pipe = create_pipe_inherit_write_end();
        if (!stdout_pipe)
        {
            fwprintf(stderr, L"[DETAIL] failed to create stdout pipe (error=%lu)\n", stdout_pipe.error());
            return false;
        }

        auto stdin_pipe = create_pipe_inherit_read_end();
        if (!stdin_pipe)
        {
            fwprintf(stderr, L"[DETAIL] failed to create stdin pipe (error=%lu)\n", stdin_pipe.error());
            return false;
        }

        const std::wstring application = *openconsole_path;
        const std::wstring build_dir = directory_name(application);
        const std::wstring log_path = join_path(build_dir, L"oc_new_condrv_process_integration_raw_read.log");
        (void)::DeleteFileW(log_path.c_str());
        const ScopedEnvironmentVariable log_level(L"OPENCONSOLE_NEW_LOG_LEVEL", L"trace");
        const ScopedEnvironmentVariable log_dir(L"OPENCONSOLE_NEW_LOG_DIR", build_dir);

        const auto server_handle_value = static_cast<unsigned long long>(condrv_handles->server.view().as_uintptr());
        wchar_t server_handle_text[32]{};
        const int formatted = swprintf_s(server_handle_text, L"0x%llX", server_handle_value);
        if (formatted <= 0)
        {
            fwprintf(stderr, L"[DETAIL] failed to format server handle value\n");
            return false;
        }

        const std::wstring server_cmd =
            quote(application) +
            L" --server " +
            server_handle_text +
            L" --headless";

        std::vector<HANDLE> server_handle_list;
        server_handle_list.push_back(condrv_handles->server.get());
        server_handle_list.push_back(stdin_pipe->read.get());
        server_handle_list.push_back(stdout_pipe->write.get());

        auto server_process = spawn_process_with_attributes(
            application,
            server_cmd,
            stdin_pipe->read.get(),
            stdout_pipe->write.get(),
            stdout_pipe->write.get(),
            server_handle_list,
            nullptr,
            CREATE_NO_WINDOW);
        if (!server_process)
        {
            fwprintf(stderr, L"[DETAIL] failed to spawn server process (error=%lu)\n", server_process.error());
            dump_text_file_preview(log_path);
            return false;
        }

        stdout_pipe->write.reset();
        stdin_pipe->read.reset();

        const ULONGLONG io_start_tick = ::GetTickCount64();
        for (;;)
        {
            auto created = create_condrv_io_handles(*ntdll, *condrv_handles);
            if (created)
            {
                break;
            }

            if (created.error() != ERROR_BAD_COMMAND)
            {
                fwprintf(stderr, L"[DETAIL] failed to create ConDrv I/O handles (error=%lu)\n", created.error());
                (void)::TerminateProcess(server_process->process.get(), 0xBADC0DE);
                (void)::WaitForSingleObject(server_process->process.get(), 5'000);
                dump_text_file_preview(log_path);
                return false;
            }

            const ULONGLONG now = ::GetTickCount64();
            if ((now - io_start_tick) >= 5'000)
            {
                fwprintf(stderr, L"[DETAIL] timed out waiting for ConDrv server readiness\n");
                (void)::TerminateProcess(server_process->process.get(), 0xBADC0DE);
                (void)::WaitForSingleObject(server_process->process.get(), 5'000);
                dump_text_file_preview(log_path);
                return false;
            }

            ::Sleep(10);
        }

        const std::wstring client_application = *client_path;
        const std::wstring client_cmd = quote(client_application);

        std::vector<HANDLE> client_handle_list;
        client_handle_list.push_back(condrv_handles->input.get());
        client_handle_list.push_back(condrv_handles->output.get());
        client_handle_list.push_back(condrv_handles->error.get());
        client_handle_list.push_back(condrv_handles->reference.get());

        auto client_process = spawn_process_with_attributes(
            client_application,
            client_cmd,
            condrv_handles->input.get(),
            condrv_handles->output.get(),
            condrv_handles->error.get(),
            client_handle_list,
            condrv_handles->reference.get(),
            0);
        if (!client_process)
        {
            fwprintf(stderr, L"[DETAIL] failed to spawn client process (error=%lu)\n", client_process.error());
            (void)::TerminateProcess(server_process->process.get(), 0xBADC0DE);
            (void)::WaitForSingleObject(server_process->process.get(), 5'000);
            dump_text_file_preview(log_path);
            return false;
        }

        condrv_handles->reference.reset();
        condrv_handles->input.reset();
        condrv_handles->output.reset();
        condrv_handles->error.reset();

        constexpr std::string_view input_bytes =
            "\x1b[65;0;97;1;0;1_"; // 'a' keydown

        if (!input_bytes.empty())
        {
            DWORD total_written = 0;
            const DWORD to_write = static_cast<DWORD>(input_bytes.size());
            while (total_written < to_write)
            {
                DWORD written = 0;
                if (::WriteFile(
                        stdin_pipe->write.get(),
                        input_bytes.data() + total_written,
                        to_write - total_written,
                        &written,
                        nullptr) == FALSE)
                {
                    fwprintf(stderr, L"[DETAIL] failed to write host input (error=%lu)\n", ::GetLastError());
                    (void)::TerminateProcess(client_process->process.get(), 0xBADC0DE);
                    (void)::TerminateProcess(server_process->process.get(), 0xBADC0DE);
                    (void)::WaitForSingleObject(client_process->process.get(), 5'000);
                    (void)::WaitForSingleObject(server_process->process.get(), 5'000);
                    dump_text_file_preview(log_path);
                    return false;
                }
                total_written += written;
            }
        }

        stdin_pipe->write.reset();

        std::vector<std::byte> captured;
        captured.reserve(2048);

        const ULONGLONG start_tick = ::GetTickCount64();
        bool client_exited = false;
        bool server_exited = false;

        for (;;)
        {
            for (;;)
            {
                DWORD available = 0;
                if (::PeekNamedPipe(stdout_pipe->read.get(), nullptr, 0, nullptr, &available, nullptr) == FALSE)
                {
                    break;
                }
                if (available == 0)
                {
                    break;
                }

                std::array<std::byte, 8192> buffer{};
                const DWORD to_read = available < buffer.size() ? available : static_cast<DWORD>(buffer.size());
                DWORD read = 0;
                if (::ReadFile(stdout_pipe->read.get(), buffer.data(), to_read, &read, nullptr) == FALSE)
                {
                    break;
                }
                if (read == 0)
                {
                    break;
                }

                try
                {
                    captured.insert(captured.end(), buffer.begin(), buffer.begin() + static_cast<size_t>(read));
                }
                catch (...)
                {
                    fwprintf(stderr, L"[DETAIL] out of memory capturing server output\n");
                    return false;
                }
            }

            if (!client_exited)
            {
                const DWORD wait_result = ::WaitForSingleObject(client_process->process.get(), 0);
                if (wait_result == WAIT_OBJECT_0)
                {
                    client_exited = true;
                }
                else if (wait_result != WAIT_TIMEOUT)
                {
                    fwprintf(stderr, L"[DETAIL] client WaitForSingleObject failed (wait=%lu error=%lu)\n", wait_result, ::GetLastError());
                    return false;
                }
            }

            if (!server_exited)
            {
                const DWORD wait_result = ::WaitForSingleObject(server_process->process.get(), 0);
                if (wait_result == WAIT_OBJECT_0)
                {
                    server_exited = true;
                }
                else if (wait_result != WAIT_TIMEOUT)
                {
                    fwprintf(stderr, L"[DETAIL] server WaitForSingleObject failed (wait=%lu error=%lu)\n", wait_result, ::GetLastError());
                    return false;
                }
            }

            if (client_exited && server_exited)
            {
                DWORD available = 0;
                if (::PeekNamedPipe(stdout_pipe->read.get(), nullptr, 0, nullptr, &available, nullptr) == FALSE || available == 0)
                {
                    break;
                }

                continue;
            }

            const DWORD wait_handles_count = (client_exited ? 0 : 1) + (server_exited ? 0 : 1);
            if (wait_handles_count > 0)
            {
                HANDLE wait_handles[2]{};
                DWORD wait_index = 0;
                if (!client_exited)
                {
                    wait_handles[wait_index++] = client_process->process.get();
                }
                if (!server_exited)
                {
                    wait_handles[wait_index++] = server_process->process.get();
                }

                const DWORD wait_result = ::WaitForMultipleObjects(wait_handles_count, wait_handles, FALSE, 20);
                if (wait_result == WAIT_FAILED)
                {
                    fwprintf(stderr, L"[DETAIL] WaitForMultipleObjects failed (error=%lu)\n", ::GetLastError());
                    return false;
                }
            }

            const ULONGLONG now = ::GetTickCount64();
            if ((now - start_tick) >= 30'000)
            {
                fwprintf(stderr, L"[DETAIL] condrv raw-read integration timed out\n");
                (void)::TerminateProcess(client_process->process.get(), 0xDEAD);
                (void)::TerminateProcess(server_process->process.get(), 0xDEAD);
                (void)::WaitForSingleObject(client_process->process.get(), 5'000);
                (void)::WaitForSingleObject(server_process->process.get(), 5'000);
                dump_bytes_preview(captured);
                dump_text_file_preview(log_path);
                return false;
            }
        }

        DWORD client_exit_code = 0;
        DWORD server_exit_code = 0;
        if (::GetExitCodeProcess(client_process->process.get(), &client_exit_code) == FALSE ||
            ::GetExitCodeProcess(server_process->process.get(), &server_exit_code) == FALSE)
        {
            fwprintf(stderr, L"[DETAIL] GetExitCodeProcess failed (error=%lu)\n", ::GetLastError());
            return false;
        }

        if (client_exit_code != 0 || server_exit_code != 0)
        {
            fwprintf(stderr, L"[DETAIL] expected client/server exit codes 0/0, got %lu/%lu\n", client_exit_code, server_exit_code);
            dump_bytes_preview(captured);
            dump_text_file_preview(log_path);
            return false;
        }

        if (!bytes_contain_ascii(captured, "RAWOK"))
        {
            fwprintf(stderr, L"[DETAIL] did not observe expected condrv raw-read token\n");
            dump_bytes_preview(captured);
            dump_text_file_preview(log_path);
            return false;
        }

        return true;
    }
}

bool run_process_integration_tests()
{
    return test_openconsole_new_headless_conpty_emits_output_and_exit_code() &&
           test_openconsole_new_pipe_input_reaches_client() &&
           test_openconsole_new_headless_condrv_server_end_to_end_basic_io() &&
           test_openconsole_new_headless_condrv_server_end_to_end_input_events() &&
           test_openconsole_new_headless_condrv_server_end_to_end_raw_read();
}
