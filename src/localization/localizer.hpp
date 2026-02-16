#pragma once

#include <Windows.h>

#include <string>
#include <string_view>

namespace oc::localization
{
    enum class StringId
    {
        startup,
        parse_failed,
        config_failed,
        launch_not_implemented,
        launching_client,
        dry_run_notice,
    };

    class Localizer final
    {
    public:
        explicit Localizer(std::wstring locale);

        [[nodiscard]] const std::wstring& locale() const noexcept;
        [[nodiscard]] std::wstring_view text(StringId id) const noexcept;
        [[nodiscard]] static std::wstring detect_user_locale();

    private:
        bool _use_simplified_chinese{ false };
        std::wstring _locale;
    };
}

