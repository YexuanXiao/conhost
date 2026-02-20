#include "config/app_config.hpp"
#include "core/unique_handle.hpp"

#include <Windows.h>

#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace
{
    class ScopedEnvironmentVariable final
    {
    public:
        ScopedEnvironmentVariable(std::wstring name, std::optional<std::wstring> value) :
            _name(std::move(name))
        {
            const DWORD required = ::GetEnvironmentVariableW(_name.c_str(), nullptr, 0);
            if (required != 0)
            {
                std::wstring buffer(required, L'\0');
                const DWORD written = ::GetEnvironmentVariableW(_name.c_str(), buffer.data(), required);
                if (written != 0)
                {
                    buffer.resize(written);
                    _previous_value = std::move(buffer);
                    _had_previous = true;
                }
            }

            if (value.has_value())
            {
                (void)::SetEnvironmentVariableW(_name.c_str(), value->c_str());
            }
            else
            {
                (void)::SetEnvironmentVariableW(_name.c_str(), nullptr);
            }
        }

        ~ScopedEnvironmentVariable()
        {
            if (_had_previous)
            {
                (void)::SetEnvironmentVariableW(_name.c_str(), _previous_value.c_str());
            }
            else
            {
                (void)::SetEnvironmentVariableW(_name.c_str(), nullptr);
            }
        }

        ScopedEnvironmentVariable(const ScopedEnvironmentVariable&) = delete;
        ScopedEnvironmentVariable& operator=(const ScopedEnvironmentVariable&) = delete;

    private:
        std::wstring _name;
        std::wstring _previous_value;
        bool _had_previous{ false };
    };

    [[nodiscard]] std::optional<std::wstring> read_environment(std::wstring_view name)
    {
        const DWORD required = ::GetEnvironmentVariableW(name.data(), nullptr, 0);
        if (required == 0)
        {
            return std::nullopt;
        }

        std::wstring buffer(required, L'\0');
        const DWORD written = ::GetEnvironmentVariableW(name.data(), buffer.data(), required);
        if (written == 0)
        {
            return std::nullopt;
        }

        buffer.resize(written);
        return buffer;
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

    [[nodiscard]] std::optional<std::wstring> create_test_directory()
    {
        std::optional<std::wstring> base = read_environment(L"TEMP");
        if (!base || base->empty())
        {
            base = read_environment(L"TMP");
        }
        if (!base || base->empty())
        {
            return std::nullopt;
        }

        const DWORD pid = ::GetCurrentProcessId();
        const ULONGLONG ticks = ::GetTickCount64();

        std::wstring path = append_path_component(*base, L"oc_new_config_tests");
        path.push_back(L'_');
        path.append(std::to_wstring(pid));
        path.push_back(L'_');
        path.append(std::to_wstring(ticks));

        if (::CreateDirectoryW(path.c_str(), nullptr) == FALSE)
        {
            return std::nullopt;
        }

        return path;
    }

    [[nodiscard]] bool write_utf8_file(const std::wstring& path, const std::wstring_view text)
    {
        const int required = ::WideCharToMultiByte(
            CP_UTF8,
            0,
            text.data(),
            static_cast<int>(text.size()),
            nullptr,
            0,
            nullptr,
            nullptr);
        if (required < 0)
        {
            return false;
        }
        if (required == 0 && !text.empty())
        {
            return false;
        }

        std::vector<char> utf8(static_cast<size_t>(required));
        if (required > 0)
        {
            const int converted = ::WideCharToMultiByte(
                CP_UTF8,
                0,
                text.data(),
                static_cast<int>(text.size()),
                utf8.data(),
                required,
                nullptr,
                nullptr);
            if (converted != required)
            {
                return false;
            }
        }

        oc::core::UniqueHandle file(::CreateFileW(
            path.c_str(),
            GENERIC_WRITE,
            0,
            nullptr,
            CREATE_ALWAYS,
            FILE_ATTRIBUTE_NORMAL,
            nullptr));
        if (!file.valid())
        {
            return false;
        }

        DWORD written = 0;
        if (utf8.empty())
        {
            return true;
        }

        if (::WriteFile(
                file.get(),
                utf8.data(),
                static_cast<DWORD>(utf8.size()),
                &written,
                nullptr) == FALSE)
        {
            return false;
        }

        return written == static_cast<DWORD>(utf8.size());
    }

    class ScopedTestDirectory final
    {
    public:
        explicit ScopedTestDirectory(std::wstring path) :
            _path(std::move(path))
        {
        }

        ~ScopedTestDirectory()
        {
            if (_path.empty())
            {
                return;
            }

            (void)::DeleteFileW(append_path_component(_path, L".conhost").c_str());
            (void)::DeleteFileW(append_path_component(_path, L"explicit.conf").c_str());
            (void)::RemoveDirectoryW(_path.c_str());
        }

        [[nodiscard]] const std::wstring& path() const noexcept
        {
            return _path;
        }

    private:
        std::wstring _path;
    };

    bool test_parse_text()
    {
        const auto parsed = oc::config::ConfigLoader::parse_text(
            L"log_level=debug\n"
            L"locale=zh-CN\n"
            L"dry_run=true\n"
            L"log_dir=C:\\temp\\logs\n"
            L"enable_file_logging=1\n"
            L"break_on_start=true\n"
            L"debug_sink=0\n"
            L"prefer_pseudoconsole=0\n"
            L"hold_on_exit=1\n"
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
               parsed->log_directory_path == L"C:\\temp\\logs" &&
               parsed->enable_file_logging &&
               parsed->break_on_start &&
               !parsed->enable_debug_sink &&
               !parsed->prefer_pseudoconsole &&
               parsed->hold_window_on_exit &&
               !parsed->allow_embedding_passthrough &&
               !parsed->enable_legacy_conhost_path &&
               parsed->embedding_wait_timeout_ms == 1500;
    }

    bool test_environment_overrides()
    {
        const ScopedEnvironmentVariable user_profile(L"USERPROFILE", std::nullopt);
        const ScopedEnvironmentVariable home(L"HOME", std::nullopt);
        const ScopedEnvironmentVariable home_drive(L"HOMEDRIVE", std::nullopt);
        const ScopedEnvironmentVariable home_path(L"HOMEPATH", std::nullopt);

        const ScopedEnvironmentVariable config_path(L"OPENCONSOLE_NEW_CONFIG", std::nullopt);
        const ScopedEnvironmentVariable log_level(L"OPENCONSOLE_NEW_LOG_LEVEL", std::optional<std::wstring>(L"error"));
        const ScopedEnvironmentVariable dry_run(L"OPENCONSOLE_NEW_DRY_RUN", std::optional<std::wstring>(L"1"));
        const ScopedEnvironmentVariable locale(L"OPENCONSOLE_NEW_LOCALE", std::optional<std::wstring>(L"en-US"));
        const ScopedEnvironmentVariable log_dir(L"OPENCONSOLE_NEW_LOG_DIR", std::optional<std::wstring>(L"C:\\temp\\logs"));
        const ScopedEnvironmentVariable enable_file_logging(L"OPENCONSOLE_NEW_ENABLE_FILE_LOGGING", std::optional<std::wstring>(L"1"));
        const ScopedEnvironmentVariable break_on_start(L"OPENCONSOLE_NEW_BREAK_ON_START", std::optional<std::wstring>(L"1"));
        const ScopedEnvironmentVariable debug_sink(L"OPENCONSOLE_NEW_DEBUG_SINK", std::optional<std::wstring>(L"false"));
        const ScopedEnvironmentVariable prefer_pty(L"OPENCONSOLE_NEW_PREFER_PTY", std::optional<std::wstring>(L"0"));
        const ScopedEnvironmentVariable hold_on_exit(L"OPENCONSOLE_NEW_HOLD_ON_EXIT", std::optional<std::wstring>(L"1"));
        const ScopedEnvironmentVariable embedding_passthrough(L"OPENCONSOLE_NEW_ALLOW_EMBEDDING_PASSTHROUGH", std::optional<std::wstring>(L"0"));
        const ScopedEnvironmentVariable legacy_path(L"OPENCONSOLE_NEW_ENABLE_LEGACY_PATH", std::optional<std::wstring>(L"0"));
        const ScopedEnvironmentVariable embedding_wait(L"OPENCONSOLE_NEW_EMBEDDING_WAIT_MS", std::optional<std::wstring>(L"220"));

        const auto loaded = oc::config::ConfigLoader::load();

        if (!loaded)
        {
            return false;
        }

        return loaded->minimum_log_level == oc::logging::LogLevel::error &&
               loaded->dry_run &&
               loaded->locale_override == L"en-US" &&
               loaded->log_directory_path == L"C:\\temp\\logs" &&
               loaded->enable_file_logging &&
               loaded->break_on_start &&
               !loaded->enable_debug_sink &&
               !loaded->prefer_pseudoconsole &&
               loaded->hold_window_on_exit &&
               !loaded->allow_embedding_passthrough &&
               !loaded->enable_legacy_conhost_path &&
               loaded->embedding_wait_timeout_ms == 220;
    }

    bool test_parse_text_invalid_line_fails()
    {
        const auto parsed = oc::config::ConfigLoader::parse_text(L"this-is-invalid-line");
        return !parsed.has_value();
    }

    bool test_user_profile_config_is_loaded()
    {
        const auto created = create_test_directory();
        if (!created)
        {
            return false;
        }

        ScopedTestDirectory directory(*created);
        const std::wstring config_path = append_path_component(directory.path(), L".conhost");
        if (!write_utf8_file(
                config_path,
                L"log_level=debug\n"
                L"locale=fr-FR\n"
                L"dry_run=true\n"))
        {
            return false;
        }

        const ScopedEnvironmentVariable user_profile(L"USERPROFILE", directory.path());
        const ScopedEnvironmentVariable home(L"HOME", std::nullopt);
        const ScopedEnvironmentVariable home_drive(L"HOMEDRIVE", std::nullopt);
        const ScopedEnvironmentVariable home_path(L"HOMEPATH", std::nullopt);
        const ScopedEnvironmentVariable config_path_env(L"OPENCONSOLE_NEW_CONFIG", std::nullopt);
        const ScopedEnvironmentVariable log_level_env(L"OPENCONSOLE_NEW_LOG_LEVEL", std::nullopt);
        const ScopedEnvironmentVariable locale_env(L"OPENCONSOLE_NEW_LOCALE", std::nullopt);
        const ScopedEnvironmentVariable dry_run_env(L"OPENCONSOLE_NEW_DRY_RUN", std::nullopt);

        const auto loaded = oc::config::ConfigLoader::load();
        if (!loaded)
        {
            return false;
        }

        return loaded->minimum_log_level == oc::logging::LogLevel::debug &&
               loaded->locale_override == L"fr-FR" &&
               loaded->dry_run;
    }

    bool test_explicit_config_path_overrides_user_profile_config()
    {
        const auto created = create_test_directory();
        if (!created)
        {
            return false;
        }

        ScopedTestDirectory directory(*created);
        const std::wstring user_config_path = append_path_component(directory.path(), L".conhost");
        const std::wstring explicit_config_path = append_path_component(directory.path(), L"explicit.conf");

        if (!write_utf8_file(user_config_path, L"log_level=debug\nlocale=ja-JP\n"))
        {
            return false;
        }
        if (!write_utf8_file(explicit_config_path, L"log_level=error\nlocale=en-US\n"))
        {
            return false;
        }

        const ScopedEnvironmentVariable user_profile(L"USERPROFILE", directory.path());
        const ScopedEnvironmentVariable home(L"HOME", std::nullopt);
        const ScopedEnvironmentVariable home_drive(L"HOMEDRIVE", std::nullopt);
        const ScopedEnvironmentVariable home_path(L"HOMEPATH", std::nullopt);
        const ScopedEnvironmentVariable config_path_env(L"OPENCONSOLE_NEW_CONFIG", explicit_config_path);
        const ScopedEnvironmentVariable log_level_env(L"OPENCONSOLE_NEW_LOG_LEVEL", std::nullopt);
        const ScopedEnvironmentVariable locale_env(L"OPENCONSOLE_NEW_LOCALE", std::nullopt);

        const auto loaded = oc::config::ConfigLoader::load();
        if (!loaded)
        {
            return false;
        }

        return loaded->minimum_log_level == oc::logging::LogLevel::error &&
               loaded->locale_override == L"en-US";
    }

    bool test_missing_user_profile_config_is_ignored()
    {
        const auto created = create_test_directory();
        if (!created)
        {
            return false;
        }

        ScopedTestDirectory directory(*created);

        const ScopedEnvironmentVariable user_profile(L"USERPROFILE", directory.path());
        const ScopedEnvironmentVariable home(L"HOME", std::nullopt);
        const ScopedEnvironmentVariable home_drive(L"HOMEDRIVE", std::nullopt);
        const ScopedEnvironmentVariable home_path(L"HOMEPATH", std::nullopt);
        const ScopedEnvironmentVariable config_path_env(L"OPENCONSOLE_NEW_CONFIG", std::nullopt);

        const auto loaded = oc::config::ConfigLoader::load();
        return loaded.has_value();
    }
}

bool run_config_tests()
{
    return test_parse_text() &&
           test_environment_overrides() &&
           test_parse_text_invalid_line_fails() &&
           test_user_profile_config_is_loaded() &&
           test_explicit_config_path_overrides_user_profile_config() &&
           test_missing_user_profile_config_is_ignored();
}
