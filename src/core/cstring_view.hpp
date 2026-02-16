#pragma once

#include <cstddef>

namespace oc::core
{
    class cstring_view final
    {
    public:
        constexpr cstring_view() noexcept = default;

        constexpr explicit cstring_view(const wchar_t* const value) noexcept :
            _value(value)
        {
        }

        [[nodiscard]] constexpr const wchar_t* data() const noexcept
        {
            return _value;
        }

        [[nodiscard]] constexpr bool empty() const noexcept
        {
            return _value == nullptr || *_value == L'\0';
        }

    private:
        const wchar_t* _value{ nullptr };
    };
}

