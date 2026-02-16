#pragma once

#include <Windows.h>

namespace oc::core
{
    class UniqueLocalPtr final
    {
    public:
        UniqueLocalPtr() noexcept = default;

        explicit UniqueLocalPtr(void* value) noexcept :
            _value(value)
        {
        }

        ~UniqueLocalPtr() noexcept
        {
            reset();
        }

        UniqueLocalPtr(const UniqueLocalPtr&) = delete;
        UniqueLocalPtr& operator=(const UniqueLocalPtr&) = delete;

        UniqueLocalPtr(UniqueLocalPtr&& other) noexcept :
            _value(other.release())
        {
        }

        UniqueLocalPtr& operator=(UniqueLocalPtr&& other) noexcept
        {
            if (this != &other)
            {
                reset(other.release());
            }
            return *this;
        }

        [[nodiscard]] void* get() const noexcept
        {
            return _value;
        }

        template<typename T>
        [[nodiscard]] T* as() const noexcept
        {
            return static_cast<T*>(_value);
        }

        void* release() noexcept
        {
            void* detached = _value;
            _value = nullptr;
            return detached;
        }

        void reset(void* replacement = nullptr) noexcept
        {
            if (_value != nullptr)
            {
                ::LocalFree(_value);
            }
            _value = replacement;
        }

    private:
        void* _value{ nullptr };
    };
}

