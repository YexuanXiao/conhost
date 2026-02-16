#include "logging/logger.hpp"

#include "core/assert.hpp"

#include <vector>

namespace oc::logging
{
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
