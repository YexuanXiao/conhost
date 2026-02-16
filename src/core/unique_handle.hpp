#pragma once

#include "core/handle_view.hpp"

#include <Windows.h>

namespace oc::core
{
    class UniqueHandle final
    {
    public:
        UniqueHandle() noexcept = default;

        explicit UniqueHandle(HANDLE value) noexcept :
            _value(value)
        {
        }

        ~UniqueHandle() noexcept
        {
            reset();
        }

        UniqueHandle(const UniqueHandle&) = delete;
        UniqueHandle& operator=(const UniqueHandle&) = delete;

        UniqueHandle(UniqueHandle&& other) noexcept :
            _value(other.release())
        {
        }

        UniqueHandle& operator=(UniqueHandle&& other) noexcept
        {
            if (this != &other)
            {
                reset(other.release());
            }
            return *this;
        }

        [[nodiscard]] HANDLE get() const noexcept
        {
            return _value;
        }

        [[nodiscard]] HandleView view() const noexcept
        {
            return HandleView(_value);
        }

        [[nodiscard]] HANDLE* put() noexcept
        {
            // Overwriting a live handle without closing would leak. Behave like
            // standard smart pointers (`unique_ptr::reset` before `&ptr`).
            reset();
            return &_value;
        }

        [[nodiscard]] bool valid() const noexcept
        {
            return _value != nullptr && _value != INVALID_HANDLE_VALUE;
        }

        HANDLE release() noexcept
        {
            HANDLE detached = _value;
            _value = nullptr;
            return detached;
        }

        void reset(HANDLE replacement = nullptr) noexcept
        {
            if (valid())
            {
                ::CloseHandle(_value);
            }
            _value = replacement;
        }

    private:
        HANDLE _value{ nullptr };
    };
}
