#include "core/unique_handle.hpp"

#include <Windows.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <expected>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace
{
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
        for (;;)
        {
            // Drain any available output.
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
                    captured.output.insert(captured.output.end(), buffer.begin(), buffer.begin() + static_cast<size_t>(read));
                }
                catch (...)
                {
                    return std::unexpected(ERROR_OUTOFMEMORY);
                }
            }

            if (process_exited)
            {
                break;
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
            L" --headless --vtmode -- %ComSpec% /c \"echo hello & exit /b 17\"";

        const std::wstring build_dir = directory_name(application);
        const std::wstring log_path = join_path(build_dir, L"oc_new_process_integration.log");
        (void)::DeleteFileW(log_path.c_str());
        const ScopedEnvironmentVariable log_level(L"OPENCONSOLE_NEW_LOG_LEVEL", L"debug");
        const ScopedEnvironmentVariable log_file(L"OPENCONSOLE_NEW_LOG_FILE", log_path);

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

        if (!bytes_contain_ascii(captured->output, "hello"))
        {
            fwprintf(stderr, L"[DETAIL] did not observe expected output token 'hello'\n");
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
            L"\"$x=[Console]::In.ReadLine(); [Console]::Out.WriteLine('X'+$x+'Y'); exit 0\"";

        const std::wstring build_dir = directory_name(application);
        const std::wstring log_path = join_path(build_dir, L"oc_new_process_integration.log");
        (void)::DeleteFileW(log_path.c_str());
        const ScopedEnvironmentVariable log_level(L"OPENCONSOLE_NEW_LOG_LEVEL", L"debug");
        const ScopedEnvironmentVariable log_file(L"OPENCONSOLE_NEW_LOG_FILE", log_path);

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
}

bool run_process_integration_tests()
{
    return test_openconsole_new_headless_conpty_emits_output_and_exit_code() &&
           test_openconsole_new_pipe_input_reaches_client();
}
