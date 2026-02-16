#pragma once

#include "core/unique_handle.hpp"
#include "logging/log_level.hpp"

#include <Windows.h>

#include <atomic>
#include <expected>
#include <format>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace oc::logging
{
    class ILogSink
    {
    public:
        virtual ~ILogSink() = default;
        virtual void write(std::wstring_view line) noexcept = 0;
    };

    class DebugOutputSink final : public ILogSink
    {
    public:
        void write(std::wstring_view line) noexcept override;
    };

    class FileLogSink final : public ILogSink
    {
    public:
        [[nodiscard]] static std::expected<std::shared_ptr<FileLogSink>, DWORD> create(std::wstring path) noexcept;
        void write(std::wstring_view line) noexcept override;

    private:
        explicit FileLogSink(core::UniqueHandle file_handle) noexcept;

        oc::core::UniqueHandle _file_handle;
        bool _utf8_bom_written{ false };
    };

    class Logger final
    {
    public:
        explicit Logger(LogLevel minimum_level);

        void add_sink(std::shared_ptr<ILogSink> sink);
        void set_minimum_level(LogLevel level) noexcept;
        [[nodiscard]] LogLevel minimum_level() const noexcept;

        template<typename... Args>
        void log(const LogLevel level, const std::wformat_string<Args...> format_text, Args&&... args)
        {
            if (level < _minimum_level.load(std::memory_order_relaxed))
            {
                return;
            }

            const std::wstring body = std::format(format_text, std::forward<Args>(args)...);
            log_preformatted(level, body);
        }

        void log_preformatted(LogLevel level, std::wstring_view body);

    private:
        static std::wstring_view level_to_string(LogLevel level) noexcept;
        static std::wstring build_timestamped_line(LogLevel level, std::wstring_view body);

        std::atomic<LogLevel> _minimum_level;
        std::vector<std::shared_ptr<ILogSink>> _sinks;
    };
}
