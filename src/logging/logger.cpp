#include "logging/logger.hpp"

#include "core/assert.hpp"

#include <optional>
#include <vector>

namespace oc::logging
{
    namespace
    {
        [[nodiscard]] std::optional<std::wstring> read_environment(const std::wstring_view name) noexcept
        {
            const DWORD required = ::GetEnvironmentVariableW(name.data(), nullptr, 0);
            if (required == 0)
            {
                return std::nullopt;
            }

            std::wstring value(required, L'\0');
            const DWORD written = ::GetEnvironmentVariableW(name.data(), value.data(), required);
            if (written == 0)
            {
                return std::nullopt;
            }

            value.resize(written);
            return value;
        }

        [[nodiscard]] std::wstring append_path_component(std::wstring base, const std::wstring_view component)
        {
            if (!base.empty())
            {
                const wchar_t tail = base.back();
                if (tail != L'\\' && tail != L'/')
                {
                    base.push_back(L'\\');
                }
            }

            base.append(component);
            return base;
        }

        [[nodiscard]] std::expected<ULONGLONG, DWORD> query_process_start_time() noexcept
        {
            FILETIME creation_time{};
            FILETIME exit_time{};
            FILETIME kernel_time{};
            FILETIME user_time{};
            if (::GetProcessTimes(
                    ::GetCurrentProcess(),
                    &creation_time,
                    &exit_time,
                    &kernel_time,
                    &user_time) == FALSE)
            {
                return std::unexpected(::GetLastError());
            }

            const ULONGLONG low = static_cast<ULONGLONG>(creation_time.dwLowDateTime);
            const ULONGLONG high = static_cast<ULONGLONG>(creation_time.dwHighDateTime) << 32;
            return high | low;
        }

        [[nodiscard]] std::expected<void, DWORD> ensure_directory_exists(const std::wstring& path) noexcept
        {
            if (::CreateDirectoryW(path.c_str(), nullptr) != FALSE)
            {
                return {};
            }

            const DWORD error = ::GetLastError();
            if (error != ERROR_ALREADY_EXISTS)
            {
                return std::unexpected(error);
            }

            const DWORD attributes = ::GetFileAttributesW(path.c_str());
            if (attributes == INVALID_FILE_ATTRIBUTES)
            {
                return std::unexpected(::GetLastError());
            }

            if ((attributes & FILE_ATTRIBUTE_DIRECTORY) == 0)
            {
                return std::unexpected(ERROR_DIRECTORY);
            }

            return {};
        }
    }

    void DebugOutputSink::write(const std::wstring_view line) noexcept
    {
        std::wstring with_newline{ line };
        with_newline.append(L"\n");
        ::OutputDebugStringW(with_newline.c_str());
    }

    FileLogSink::FileLogSink(core::UniqueHandle file_handle) noexcept :
        _file_handle(std::move(file_handle))
    {
    }

    std::expected<std::shared_ptr<FileLogSink>, DWORD> FileLogSink::create(std::wstring path) noexcept
    {
        core::UniqueHandle file(::CreateFileW(
            path.c_str(),
            FILE_APPEND_DATA,
            FILE_SHARE_READ,
            nullptr,
            OPEN_ALWAYS,
            FILE_ATTRIBUTE_NORMAL,
            nullptr));
        if (!file.valid())
        {
            return std::unexpected(::GetLastError());
        }
        return std::shared_ptr<FileLogSink>(new FileLogSink(std::move(file)));
    }

    std::expected<std::wstring, DWORD> FileLogSink::resolve_log_path(std::wstring directory_path) noexcept
    {
        if (directory_path.empty())
        {
            return std::unexpected(ERROR_INVALID_PARAMETER);
        }

        if (auto ensured = ensure_directory_exists(directory_path); !ensured)
        {
            return std::unexpected(ensured.error());
        }

        auto start_time = query_process_start_time();
        if (!start_time)
        {
            return std::unexpected(start_time.error());
        }

        std::wstring file_name;
        std::wstring path;
        try
        {
            file_name.append(L"console_");
            file_name.append(std::to_wstring(::GetCurrentProcessId()));
            file_name.push_back(L'_');
            file_name.append(std::to_wstring(*start_time));
            file_name.append(L".log");

            path = append_path_component(std::move(directory_path), file_name);
        }
        catch (...)
        {
            return std::unexpected(ERROR_OUTOFMEMORY);
        }

        return path;
    }

    std::expected<std::wstring, DWORD> FileLogSink::resolve_default_log_path() noexcept
    {
        std::optional<std::wstring> temp_root = read_environment(L"TEMP");
        if (!temp_root || temp_root->empty())
        {
            temp_root = read_environment(L"TMP");
        }
        if (!temp_root || temp_root->empty())
        {
            return std::unexpected(ERROR_ENVVAR_NOT_FOUND);
        }

        std::wstring console_directory;
        try
        {
            console_directory = append_path_component(*temp_root, L"console");
        }
        catch (...)
        {
            return std::unexpected(ERROR_OUTOFMEMORY);
        }

        return resolve_log_path(std::move(console_directory));
    }

    void FileLogSink::write(const std::wstring_view line) noexcept
    {
        if (!_file_handle.valid())
        {
            return;
        }

        if (!_utf8_bom_written)
        {
            LARGE_INTEGER position{};
            if (::SetFilePointerEx(_file_handle.get(), {}, &position, FILE_CURRENT) != FALSE && position.QuadPart == 0)
            {
                static constexpr unsigned char utf8_bom[] = { 0xEF, 0xBB, 0xBF };
                DWORD bom_written = 0;
                ::WriteFile(_file_handle.get(), utf8_bom, static_cast<DWORD>(sizeof(utf8_bom)), &bom_written, nullptr);
            }
            _utf8_bom_written = true;
        }

        std::wstring payload(line);
        payload.append(L"\r\n");

        const int utf8_size = ::WideCharToMultiByte(
            CP_UTF8,
            0,
            payload.c_str(),
            static_cast<int>(payload.size()),
            nullptr,
            0,
            nullptr,
            nullptr);
        if (utf8_size <= 0)
        {
            return;
        }

        std::vector<char> utf8(static_cast<size_t>(utf8_size));
        const int converted = ::WideCharToMultiByte(
            CP_UTF8,
            0,
            payload.c_str(),
            static_cast<int>(payload.size()),
            utf8.data(),
            utf8_size,
            nullptr,
            nullptr);
        if (converted <= 0)
        {
            return;
        }

        DWORD written = 0;
        ::WriteFile(
            _file_handle.get(),
            utf8.data(),
            static_cast<DWORD>(utf8.size()),
            &written,
            nullptr);
    }

    Logger::Logger(const LogLevel minimum_level) :
        _minimum_level(minimum_level)
    {
    }

    void Logger::add_sink(std::shared_ptr<ILogSink> sink)
    {
        OC_ASSERT(sink != nullptr);
        _sinks.push_back(std::move(sink));
    }

    void Logger::set_minimum_level(const LogLevel level) noexcept
    {
        _minimum_level.store(level, std::memory_order_relaxed);
    }

    LogLevel Logger::minimum_level() const noexcept
    {
        return _minimum_level.load(std::memory_order_relaxed);
    }

    void Logger::log_preformatted(const LogLevel level, const std::wstring_view body)
    {
        if (level < _minimum_level.load(std::memory_order_relaxed))
        {
            return;
        }

        const std::wstring line = build_timestamped_line(level, body);
        for (const auto& sink : _sinks)
        {
            sink->write(line);
        }
    }

    std::wstring_view Logger::level_to_string(const LogLevel level) noexcept
    {
        switch (level)
        {
        case LogLevel::trace:
            return L"TRACE";
        case LogLevel::debug:
            return L"DEBUG";
        case LogLevel::info:
            return L"INFO";
        case LogLevel::warning:
            return L"WARN";
        case LogLevel::error:
            return L"ERROR";
        default:
            return L"UNKNOWN";
        }
    }

    std::wstring Logger::build_timestamped_line(const LogLevel level, const std::wstring_view body)
    {
        SYSTEMTIME system_time{};
        ::GetLocalTime(&system_time);

        // std::format usage is intentionally confined to logging output as required.
        return std::format(
            L"{:04}-{:02}-{:02} {:02}:{:02}:{:02}.{:03} [{}] {}",
            system_time.wYear,
            system_time.wMonth,
            system_time.wDay,
            system_time.wHour,
            system_time.wMinute,
            system_time.wSecond,
            system_time.wMilliseconds,
            level_to_string(level),
            body);
    }
}
