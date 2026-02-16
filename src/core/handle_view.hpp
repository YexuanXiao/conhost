#pragma once

// A non-owning view of a Win32 HANDLE.
//
// Rationale:
// - Many Win32 APIs traffic in `HANDLE` values that are either borrowed (e.g.
//   `GetStdHandle`) or owned elsewhere (e.g. inherited handles).
// - We still want to avoid passing/storing raw HANDLEs directly to keep
//   ownership and lifetime decisions explicit.
//
// This type does not close the handle. For owning semantics use `UniqueHandle`.

#include <Windows.h>

#include <cstdint>
#include <type_traits>

namespace oc::core
{
    class HandleView final
    {
    public:
        constexpr HandleView() noexcept = default;

        explicit constexpr HandleView(const HANDLE value) noexcept :
            _value(value)
        {
        }

        [[nodiscard]] static constexpr HandleView from_uintptr(const std::uintptr_t value) noexcept
        {
            return HandleView(reinterpret_cast<HANDLE>(value));
        }

        [[nodiscard]] constexpr HANDLE get() const noexcept
        {
            return _value;
        }

        [[nodiscard]] constexpr std::uintptr_t as_uintptr() const noexcept
        {
            return reinterpret_cast<std::uintptr_t>(_value);
        }

        [[nodiscard]] constexpr bool valid() const noexcept
        {
            return _value != nullptr && _value != INVALID_HANDLE_VALUE;
        }

        [[nodiscard]] explicit constexpr operator bool() const noexcept
        {
            return valid();
        }

    private:
        HANDLE _value{ nullptr };
    };

    static_assert(sizeof(HandleView) == sizeof(HANDLE), "HandleView must remain layout-compatible with HANDLE");
    static_assert(alignof(HandleView) == alignof(HANDLE), "HandleView must remain layout-compatible with HANDLE");
    static_assert(std::is_standard_layout_v<HandleView>, "HandleView must remain standard-layout");
    static_assert(std::is_trivially_copyable_v<HandleView>, "HandleView must remain trivially copyable");
}
