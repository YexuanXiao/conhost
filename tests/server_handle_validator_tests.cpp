#include "runtime/server_handle_validator.hpp"

#include "core/unique_handle.hpp"

#include <Windows.h>

#include <optional>
#include <string>

namespace
{
    [[nodiscard]] std::optional<std::wstring> read_environment(const wchar_t* const name)
    {
        const DWORD size = ::GetEnvironmentVariableW(name, nullptr, 0);
        if (size == 0)
        {
            return std::nullopt;
        }

        std::wstring value(size, L'\0');
        const DWORD written = ::GetEnvironmentVariableW(name, value.data(), size);
        if (written == 0)
        {
            return std::nullopt;
        }
        value.resize(written);
        return value;
    }

    [[nodiscard]] std::wstring normalize_temp_dir(std::wstring path)
    {
        if (!path.empty())
        {
            const wchar_t tail = path.back();
            if (tail != L'\\' && tail != L'/')
            {
                path.push_back(L'\\');
            }
        }
        return path;
    }

    [[nodiscard]] std::wstring maybe_add_extended_prefix(std::wstring path)
    {
        if (path.starts_with(L"\\\\?\\") || path.starts_with(L"\\\\.\\"))
        {
            return path;
        }

        if (path.size() > 3 && path[1] == L':' && (path[2] == L'\\' || path[2] == L'/'))
        {
            return std::wstring(L"\\\\?\\") + path;
        }

        if (path.starts_with(L"\\\\"))
        {
            std::wstring prefixed(L"\\\\?\\UNC\\");
            prefixed.append(path.substr(2));
            return prefixed;
        }

        return path;
    }

    [[nodiscard]] std::optional<std::wstring> pick_temp_base_directory()
    {
        if (const auto tmp = read_environment(L"TMP"))
        {
            return normalize_temp_dir(*tmp);
        }
        if (const auto temp = read_environment(L"TEMP"))
        {
            return normalize_temp_dir(*temp);
        }
        if (const auto profile = read_environment(L"USERPROFILE"))
        {
            std::wstring fallback = normalize_temp_dir(*profile);
            fallback.append(L"AppData\\Local\\Temp\\");
            return fallback;
        }
        return std::nullopt;
    }

    [[nodiscard]] std::optional<std::wstring> make_unique_temp_file_path()
    {
        const auto base = pick_temp_base_directory();
        if (!base)
        {
            return std::nullopt;
        }

        const DWORD pid = ::GetCurrentProcessId();
        const ULONGLONG tick = ::GetTickCount64();
        std::wstring candidate = *base;
        candidate.append(L"oc_new_validator_");
        candidate.append(std::to_wstring(static_cast<unsigned long long>(pid)));
        candidate.push_back(L'_');
        candidate.append(std::to_wstring(static_cast<unsigned long long>(tick)));
        candidate.append(L".tmp");
        return maybe_add_extended_prefix(std::move(candidate));
    }

    bool test_invalid_handle()
    {
        auto result = oc::runtime::ServerHandleValidator::validate(oc::core::HandleView(INVALID_HANDLE_VALUE));
        return !result.has_value();
    }

    bool test_optional_signal_accepts_null()
    {
        auto result = oc::runtime::ServerHandleValidator::validate_optional_signal(oc::core::HandleView{});
        return result.has_value();
    }

    bool test_file_handle_is_valid()
    {
        const auto path = make_unique_temp_file_path();
        if (!path)
        {
            return false;
        }

        oc::core::UniqueHandle file(::CreateFileW(
            path->c_str(),
            GENERIC_READ | GENERIC_WRITE,
            FILE_SHARE_READ | FILE_SHARE_WRITE,
            nullptr,
            CREATE_NEW,
            FILE_ATTRIBUTE_TEMPORARY | FILE_FLAG_DELETE_ON_CLOSE,
            nullptr));
        if (!file.valid())
        {
            return false;
        }

        auto result = oc::runtime::ServerHandleValidator::validate(oc::core::HandleView(file.get()));
        return result.has_value();
    }
}

bool run_server_handle_validator_tests()
{
    return test_invalid_handle() &&
           test_optional_signal_accepts_null() &&
           test_file_handle_is_valid();
}
