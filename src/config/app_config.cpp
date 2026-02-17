#include "config/app_config.hpp"

#include "core/unique_handle.hpp"
#include "serialization/fast_number.hpp"

#include <Windows.h>

#include <algorithm>
#include <limits>
#include <optional>
#include <vector>

namespace oc::config
{
    namespace
    {
        constexpr std::wstring_view kConfigPathEnv = L"OPENCONSOLE_NEW_CONFIG";
        constexpr std::wstring_view kUserProfileEnv = L"USERPROFILE";
        constexpr std::wstring_view kHomeEnv = L"HOME";
        constexpr std::wstring_view kHomeDriveEnv = L"HOMEDRIVE";
        constexpr std::wstring_view kHomePathEnv = L"HOMEPATH";
        constexpr std::wstring_view kDefaultUserConfigName = L".conhost";
        constexpr std::wstring_view kLocaleEnv = L"OPENCONSOLE_NEW_LOCALE";
        constexpr std::wstring_view kDryRunEnv = L"OPENCONSOLE_NEW_DRY_RUN";
        constexpr std::wstring_view kLogLevelEnv = L"OPENCONSOLE_NEW_LOG_LEVEL";
        constexpr std::wstring_view kLogDirEnv = L"OPENCONSOLE_NEW_LOG_DIR";
        constexpr std::wstring_view kLegacyLogFileEnv = L"OPENCONSOLE_NEW_LOG_FILE";
        constexpr std::wstring_view kEnableFileLoggingEnv = L"OPENCONSOLE_NEW_ENABLE_FILE_LOGGING";
        constexpr std::wstring_view kBreakOnStartEnv = L"OPENCONSOLE_NEW_BREAK_ON_START";
        constexpr std::wstring_view kDebugSinkEnv = L"OPENCONSOLE_NEW_DEBUG_SINK";
        constexpr std::wstring_view kPreferPtyEnv = L"OPENCONSOLE_NEW_PREFER_PTY";
        constexpr std::wstring_view kEmbeddingPassthroughEnv = L"OPENCONSOLE_NEW_ALLOW_EMBEDDING_PASSTHROUGH";
        constexpr std::wstring_view kLegacyPathEnv = L"OPENCONSOLE_NEW_ENABLE_LEGACY_PATH";
        constexpr std::wstring_view kEmbeddingWaitEnv = L"OPENCONSOLE_NEW_EMBEDDING_WAIT_MS";

        [[nodiscard]] std::wstring trim(std::wstring value)
        {
            auto not_space = [](const wchar_t ch) {
                return ch != L' ' && ch != L'\t' && ch != L'\r' && ch != L'\n';
            };

            auto begin_it = std::find_if(value.begin(), value.end(), not_space);
            if (begin_it == value.end())
            {
                return {};
            }

            auto end_it = std::find_if(value.rbegin(), value.rend(), not_space).base();
            return std::wstring(begin_it, end_it);
        }

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

        [[nodiscard]] oc::logging::LogLevel parse_log_level(const std::wstring_view text)
        {
            if (text == L"trace")
            {
                return oc::logging::LogLevel::trace;
            }
            if (text == L"debug")
            {
                return oc::logging::LogLevel::debug;
            }
            if (text == L"warning")
            {
                return oc::logging::LogLevel::warning;
            }
            if (text == L"error")
            {
                return oc::logging::LogLevel::error;
            }
            return oc::logging::LogLevel::info;
        }

        [[nodiscard]] bool parse_bool(const std::wstring_view text)
        {
            return text == L"1" || text == L"true" || text == L"TRUE" || text == L"on" || text == L"ON";
        }

        [[nodiscard]] DWORD parse_dword_or_default(const std::wstring_view text, const DWORD fallback)
        {
            const auto parsed = serialization::parse_u32(text);
            if (!parsed)
            {
                return fallback;
            }
            return *parsed;
        }

        [[nodiscard]] std::expected<std::wstring, ConfigError> read_config_file(std::wstring path) noexcept
        {
            core::UniqueHandle file(::CreateFileW(
                path.c_str(),
                GENERIC_READ,
                FILE_SHARE_READ,
                nullptr,
                OPEN_EXISTING,
                FILE_ATTRIBUTE_NORMAL,
                nullptr));
            if (!file.valid())
            {
                return std::unexpected(ConfigError{
                    .message = L"CreateFileW failed for config path",
                    .win32_error = ::GetLastError(),
                });
            }

            LARGE_INTEGER file_size{};
            if (::GetFileSizeEx(file.get(), &file_size) == FALSE)
            {
                return std::unexpected(ConfigError{
                    .message = L"GetFileSizeEx failed for config path",
                    .win32_error = ::GetLastError(),
                });
            }

            if (file_size.QuadPart < 0 || file_size.QuadPart > 2 * 1024 * 1024)
            {
                return std::unexpected(ConfigError{
                    .message = L"Config file size is invalid",
                    .win32_error = ERROR_FILE_TOO_LARGE,
                });
            }

            const DWORD bytes_to_read = static_cast<DWORD>(file_size.QuadPart);
            std::vector<char> bytes(bytes_to_read);
            if (bytes_to_read > 0)
            {
                DWORD bytes_read = 0;
                if (::ReadFile(file.get(), bytes.data(), bytes_to_read, &bytes_read, nullptr) == FALSE || bytes_read != bytes_to_read)
                {
                    return std::unexpected(ConfigError{
                        .message = L"ReadFile failed for config path",
                        .win32_error = ::GetLastError(),
                    });
                }
            }

            if (bytes.size() >= 2 &&
                static_cast<unsigned char>(bytes[0]) == 0xFF &&
                static_cast<unsigned char>(bytes[1]) == 0xFE)
            {
                const size_t wchar_count = (bytes.size() - 2) / sizeof(wchar_t);
                const wchar_t* start = reinterpret_cast<const wchar_t*>(bytes.data() + 2);
                return std::wstring(start, start + wchar_count);
            }

            if (bytes.empty())
            {
                return std::wstring{};
            }

            const int wide_length = ::MultiByteToWideChar(
                CP_UTF8,
                MB_ERR_INVALID_CHARS,
                bytes.data(),
                static_cast<int>(bytes.size()),
                nullptr,
                0);
            if (wide_length <= 0)
            {
                return std::unexpected(ConfigError{
                    .message = L"Config is not UTF-8/UTF-16LE text",
                    .win32_error = ::GetLastError(),
                });
            }

            std::wstring wide(static_cast<size_t>(wide_length), L'\0');
            const int converted = ::MultiByteToWideChar(
                CP_UTF8,
                MB_ERR_INVALID_CHARS,
                bytes.data(),
                static_cast<int>(bytes.size()),
                wide.data(),
                wide_length);
            if (converted != wide_length)
            {
                return std::unexpected(ConfigError{
                    .message = L"Failed to convert config file text",
                    .win32_error = ::GetLastError(),
                });
            }

            return wide;
        }

        [[nodiscard]] std::wstring append_path_component(std::wstring base, const std::wstring_view component) noexcept
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

        [[nodiscard]] std::wstring directory_from_path(std::wstring value) noexcept
        {
            value = trim(std::move(value));
            if (value.empty())
            {
                return {};
            }

            const size_t separator = value.find_last_of(L"\\/");
            if (separator == std::wstring::npos)
            {
                return {};
            }

            return value.substr(0, separator);
        }

        [[nodiscard]] std::optional<std::wstring> resolve_default_user_config_path() noexcept
        {
            if (const auto user_profile = read_environment(kUserProfileEnv))
            {
                if (!user_profile->empty())
                {
                    return append_path_component(*user_profile, kDefaultUserConfigName);
                }
            }

            if (const auto home = read_environment(kHomeEnv))
            {
                if (!home->empty())
                {
                    return append_path_component(*home, kDefaultUserConfigName);
                }
            }

            const auto home_drive = read_environment(kHomeDriveEnv);
            const auto home_path = read_environment(kHomePathEnv);
            if (home_drive && home_path)
            {
                std::wstring combined;
                try
                {
                    combined.reserve(home_drive->size() + home_path->size());
                    combined.append(*home_drive);
                    combined.append(*home_path);
                }
                catch (...)
                {
                    return std::nullopt;
                }

                if (!combined.empty())
                {
                    return append_path_component(std::move(combined), kDefaultUserConfigName);
                }
            }

            return std::nullopt;
        }

        [[nodiscard]] bool is_missing_file_error(const DWORD error) noexcept
        {
            return error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND;
        }

        [[nodiscard]] std::expected<void, ConfigError> load_config_file_into(
            AppConfig& config,
            const std::wstring& config_path,
            const bool allow_missing_file) noexcept
        {
            auto file_text = read_config_file(config_path);
            if (!file_text)
            {
                if (allow_missing_file && is_missing_file_error(file_text.error().win32_error))
                {
                    return {};
                }

                return std::unexpected(file_text.error());
            }

            auto parsed = ConfigLoader::parse_text(file_text.value());
            if (!parsed)
            {
                return std::unexpected(parsed.error());
            }

            config = std::move(parsed.value());
            return {};
        }

        void apply_key_value(AppConfig& config, std::wstring key, std::wstring value)
        {
            key = trim(std::move(key));
            value = trim(std::move(value));

            if (key == L"locale")
            {
                config.locale_override = std::move(value);
                return;
            }
            if (key == L"dry_run")
            {
                config.dry_run = parse_bool(value);
                return;
            }
            if (key == L"log_level")
            {
                config.minimum_log_level = parse_log_level(value);
                return;
            }
            if (key == L"log_dir")
            {
                config.log_directory_path = std::move(value);
                config.enable_file_logging = !config.log_directory_path.empty();
                return;
            }
            if (key == L"enable_file_logging" || key == L"file_logging")
            {
                config.enable_file_logging = parse_bool(value);
                return;
            }
            if (key == L"break_on_start")
            {
                config.break_on_start = parse_bool(value);
                return;
            }
            if (key == L"debug_sink")
            {
                config.enable_debug_sink = parse_bool(value);
                return;
            }
            if (key == L"prefer_pseudoconsole")
            {
                config.prefer_pseudoconsole = parse_bool(value);
                return;
            }
            if (key == L"allow_embedding_passthrough")
            {
                config.allow_embedding_passthrough = parse_bool(value);
                return;
            }
            if (key == L"enable_legacy_conhost_path")
            {
                config.enable_legacy_conhost_path = parse_bool(value);
                return;
            }
            if (key == L"embedding_wait_timeout_ms")
            {
                config.embedding_wait_timeout_ms = parse_dword_or_default(value, config.embedding_wait_timeout_ms);
            }
        }

        void apply_environment_overrides(AppConfig& config)
        {
            if (const auto value = read_environment(kLocaleEnv))
            {
                config.locale_override = *value;
            }
            if (const auto value = read_environment(kDryRunEnv))
            {
                config.dry_run = parse_bool(*value);
            }
            if (const auto value = read_environment(kLogLevelEnv))
            {
                config.minimum_log_level = parse_log_level(*value);
            }
            if (const auto value = read_environment(kLogDirEnv))
            {
                config.log_directory_path = trim(*value);
                config.enable_file_logging = !config.log_directory_path.empty();
            }
            else if (const auto value = read_environment(kLegacyLogFileEnv))
            {
                config.log_directory_path = directory_from_path(*value);
                config.enable_file_logging = !config.log_directory_path.empty();
            }
            if (const auto value = read_environment(kEnableFileLoggingEnv))
            {
                config.enable_file_logging = parse_bool(*value);
            }
            if (const auto value = read_environment(kBreakOnStartEnv))
            {
                config.break_on_start = parse_bool(*value);
            }
            if (const auto value = read_environment(kDebugSinkEnv))
            {
                config.enable_debug_sink = parse_bool(*value);
            }
            if (const auto value = read_environment(kPreferPtyEnv))
            {
                config.prefer_pseudoconsole = parse_bool(*value);
            }
            if (const auto value = read_environment(kEmbeddingPassthroughEnv))
            {
                config.allow_embedding_passthrough = parse_bool(*value);
            }
            if (const auto value = read_environment(kLegacyPathEnv))
            {
                config.enable_legacy_conhost_path = parse_bool(*value);
            }
            if (const auto value = read_environment(kEmbeddingWaitEnv))
            {
                config.embedding_wait_timeout_ms = parse_dword_or_default(*value, config.embedding_wait_timeout_ms);
            }
        }
    }

    std::expected<AppConfig, ConfigError> ConfigLoader::load() noexcept
    {
        AppConfig config{};

        // Startup bootstrap order:
        // 1) Optional per-user baseline file: ~/.conhost (best-effort, missing file is ignored).
        // 2) Optional explicit file from OPENCONSOLE_NEW_CONFIG (strict, errors are surfaced).
        // 3) Environment overrides for CI/runtime control.
        if (const auto user_config_path = resolve_default_user_config_path())
        {
            if (auto loaded = load_config_file_into(config, *user_config_path, true); !loaded)
            {
                return std::unexpected(loaded.error());
            }
        }

        if (const auto config_path = read_environment(kConfigPathEnv))
        {
            if (auto loaded = load_config_file_into(config, *config_path, false); !loaded)
            {
                return std::unexpected(loaded.error());
            }
        }

        apply_environment_overrides(config);
        return config;
    }

    std::expected<AppConfig, ConfigError> ConfigLoader::parse_text(const std::wstring_view text) noexcept
    {
        AppConfig config{};
        size_t begin = 0;

        while (begin < text.size())
        {
            size_t end = text.find(L'\n', begin);
            if (end == std::wstring_view::npos)
            {
                end = text.size();
            }

            std::wstring line(text.substr(begin, end - begin));
            line = trim(std::move(line));
            if (!line.empty() && !line.starts_with(L"#") && !line.starts_with(L";"))
            {
                const size_t equals_index = line.find(L'=');
                if (equals_index == std::wstring::npos)
                {
                    return std::unexpected(ConfigError{
                        .message = L"Invalid config line (missing '=')",
                        .win32_error = ERROR_BAD_FORMAT,
                    });
                }

                std::wstring key = line.substr(0, equals_index);
                std::wstring value = line.substr(equals_index + 1);
                apply_key_value(config, std::move(key), std::move(value));
            }

            begin = end + 1;
        }

        return config;
    }
}
