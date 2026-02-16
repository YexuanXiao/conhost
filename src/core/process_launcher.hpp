#pragma once

#include <Windows.h>

#include <expected>
#include <string>

namespace oc::core
{
    struct ProcessLaunchError final
    {
        DWORD win32_error{ ERROR_SUCCESS };
        std::wstring context;
    };

    class ProcessLauncher final
    {
    public:
        [[nodiscard]] static std::expected<DWORD, ProcessLaunchError> launch_and_wait(std::wstring command_line) noexcept;
    };
}

