#pragma once

// Small process-launch helper used by tests and tooling-style invocations.
//
// This wrapper provides:
// - Win32 `CreateProcessW` startup with inherited handles enabled (bInheritHandles=true),
// - a synchronous wait for process termination,
// - exit-code retrieval.
//
// The full console runtime uses a richer launch path (pseudo console setup,
// standard handle plumbing, etc.). This helper exists to keep those concerns
// out of non-runtime modules that just need to run a command line.

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
