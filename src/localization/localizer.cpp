#include "localization/localizer.hpp"

namespace oc::localization
{
    Localizer::Localizer(std::wstring locale) :
        _locale(std::move(locale))
    {
        if (_locale.empty())
        {
            _locale = detect_user_locale();
        }

        _use_simplified_chinese = _locale.starts_with(L"zh");
    }

    const std::wstring& Localizer::locale() const noexcept
    {
        return _locale;
    }

    std::wstring_view Localizer::text(const StringId id) const noexcept
    {
        if (_use_simplified_chinese)
        {
            switch (id)
            {
            case StringId::startup:
                return L"新实现启动";
            case StringId::parse_failed:
                return L"命令行参数解析失败";
            case StringId::config_failed:
                return L"配置加载失败";
            case StringId::launch_not_implemented:
                return L"该运行模式尚未实现";
            case StringId::launching_client:
                return L"正在启动客户端命令";
            case StringId::dry_run_notice:
                return L"dry-run 已启用，跳过进程启动";
            default:
                return L"未知消息";
            }
        }

        switch (id)
        {
        case StringId::startup:
            return L"New runtime starting";
        case StringId::parse_failed:
            return L"Command line parsing failed";
        case StringId::config_failed:
            return L"Configuration loading failed";
        case StringId::launch_not_implemented:
            return L"This runtime mode is not implemented yet";
        case StringId::launching_client:
            return L"Launching client command line";
        case StringId::dry_run_notice:
            return L"Dry-run enabled; process launch skipped";
        default:
            return L"Unknown message";
        }
    }

    std::wstring Localizer::detect_user_locale()
    {
        wchar_t locale_buffer[LOCALE_NAME_MAX_LENGTH]{};
        const int size = ::GetUserDefaultLocaleName(locale_buffer, LOCALE_NAME_MAX_LENGTH);
        if (size <= 0)
        {
            return L"en-US";
        }
        return std::wstring(locale_buffer, locale_buffer + size - 1);
    }
}
