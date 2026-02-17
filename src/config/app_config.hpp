#pragma once

#include "logging/log_level.hpp"

#include <Windows.h>

#include <expected>
#include <string>
#include <string_view>

namespace oc::config
{
    struct ConfigError final
    {
        std::wstring message;
        DWORD win32_error{ ERROR_SUCCESS };
    };

    struct AppConfig final
    {
        oc::logging::LogLevel minimum_log_level{ oc::logging::LogLevel::info };
        std::wstring locale_override;
        bool dry_run{ false };
        bool enable_debug_sink{ true };
        bool enable_file_logging{ false };
        std::wstring log_file_path;
        bool prefer_pseudoconsole{ true };
        bool allow_embedding_passthrough{ true };
        bool enable_legacy_conhost_path{ true };
        DWORD embedding_wait_timeout_ms{ 0 };
    };

    class ConfigLoader final
    {
    public:
        [[nodiscard]] static std::expected<AppConfig, ConfigError> load() noexcept;
        [[nodiscard]] static std::expected<AppConfig, ConfigError> parse_text(std::wstring_view text) noexcept;
    };
}
