#pragma once

#include <Windows.h>

namespace oc::tests
{
    // Registers the COM proxy/stub metadata for IConsoleHandoff in the current
    // process so out-of-proc COM activation can marshal the interface without
    // relying on machine-wide registration.
    //
    // COM must be initialized (CoInitializeEx) before calling register_for_process.
    class ConsoleHandoffProxyRegistration final
    {
    public:
        ConsoleHandoffProxyRegistration() noexcept = default;
        ~ConsoleHandoffProxyRegistration() noexcept;

        ConsoleHandoffProxyRegistration(const ConsoleHandoffProxyRegistration&) = delete;
        ConsoleHandoffProxyRegistration& operator=(const ConsoleHandoffProxyRegistration&) = delete;

        [[nodiscard]] HRESULT register_for_process() noexcept;

    private:
        DWORD _cookie{ 0 };
        bool _registered{ false };
        void* _class_object{ nullptr };
    };
}
