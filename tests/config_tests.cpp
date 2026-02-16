#include "config/app_config.hpp"

#include <Windows.h>

namespace
{
    bool test_parse_text()
    {
        const auto parsed = oc::config::ConfigLoader::parse_text(
            L"log_level=debug\n"
            L"locale=zh-CN\n"
            L"dry_run=true\n"
            L"log_file=C:\\temp\\oc.log\n"
            L"debug_sink=0\n"
            L"prefer_pseudoconsole=0\n"
            L"allow_embedding_passthrough=0\n"
            L"enable_legacy_conhost_path=0\n"
            L"embedding_wait_timeout_ms=1500\n");
        if (!parsed)
        {
            return false;
        }

        return parsed->minimum_log_level == oc::logging::LogLevel::debug &&
               parsed->locale_override == L"zh-CN" &&
               parsed->dry_run &&
               parsed->log_file_path == L"C:\\temp\\oc.log" &&
               !parsed->enable_debug_sink &&
               !parsed->prefer_pseudoconsole &&
               !parsed->allow_embedding_passthrough &&
               !parsed->enable_legacy_conhost_path &&
               parsed->embedding_wait_timeout_ms == 1500;
    }

    bool test_environment_overrides()
    {
        ::SetEnvironmentVariableW(L"OPENCONSOLE_NEW_CONFIG", nullptr);
        ::SetEnvironmentVariableW(L"OPENCONSOLE_NEW_LOG_LEVEL", L"error");
        ::SetEnvironmentVariableW(L"OPENCONSOLE_NEW_DRY_RUN", L"1");
        ::SetEnvironmentVariableW(L"OPENCONSOLE_NEW_LOCALE", L"en-US");
        ::SetEnvironmentVariableW(L"OPENCONSOLE_NEW_DEBUG_SINK", L"false");
        ::SetEnvironmentVariableW(L"OPENCONSOLE_NEW_PREFER_PTY", L"0");
        ::SetEnvironmentVariableW(L"OPENCONSOLE_NEW_ALLOW_EMBEDDING_PASSTHROUGH", L"0");
        ::SetEnvironmentVariableW(L"OPENCONSOLE_NEW_ENABLE_LEGACY_PATH", L"0");
        ::SetEnvironmentVariableW(L"OPENCONSOLE_NEW_EMBEDDING_WAIT_MS", L"220");

        const auto loaded = oc::config::ConfigLoader::load();

        ::SetEnvironmentVariableW(L"OPENCONSOLE_NEW_LOG_LEVEL", nullptr);
        ::SetEnvironmentVariableW(L"OPENCONSOLE_NEW_DRY_RUN", nullptr);
        ::SetEnvironmentVariableW(L"OPENCONSOLE_NEW_LOCALE", nullptr);
        ::SetEnvironmentVariableW(L"OPENCONSOLE_NEW_DEBUG_SINK", nullptr);
        ::SetEnvironmentVariableW(L"OPENCONSOLE_NEW_PREFER_PTY", nullptr);
        ::SetEnvironmentVariableW(L"OPENCONSOLE_NEW_ALLOW_EMBEDDING_PASSTHROUGH", nullptr);
        ::SetEnvironmentVariableW(L"OPENCONSOLE_NEW_ENABLE_LEGACY_PATH", nullptr);
        ::SetEnvironmentVariableW(L"OPENCONSOLE_NEW_EMBEDDING_WAIT_MS", nullptr);

        if (!loaded)
        {
            return false;
        }

        return loaded->minimum_log_level == oc::logging::LogLevel::error &&
               loaded->dry_run &&
               loaded->locale_override == L"en-US" &&
               !loaded->enable_debug_sink &&
               !loaded->prefer_pseudoconsole &&
               !loaded->allow_embedding_passthrough &&
               !loaded->enable_legacy_conhost_path &&
               loaded->embedding_wait_timeout_ms == 220;
    }

    bool test_parse_text_invalid_line_fails()
    {
        const auto parsed = oc::config::ConfigLoader::parse_text(L"this-is-invalid-line");
        return !parsed.has_value();
    }
}

bool run_config_tests()
{
    return test_parse_text() &&
           test_environment_overrides() &&
           test_parse_text_invalid_line_fails();
}
