#include "core/process_launcher.hpp"

#include "core/assert.hpp"
#include "core/unique_handle.hpp"

#include <vector>

// `CreateProcessW` requires a mutable command line buffer when `lpCommandLine`
// is non-null. This helper therefore takes ownership of the command line and
// materializes it into a writable NUL-terminated buffer.

namespace oc::core
{
    std::expected<DWORD, ProcessLaunchError> ProcessLauncher::launch_and_wait(std::wstring command_line) noexcept
    {
        if (command_line.empty())
        {
            return DWORD{ 0 };
        }

        std::vector<wchar_t> mutable_command_line;
        mutable_command_line.reserve(command_line.size() + 1);
        mutable_command_line.insert(mutable_command_line.end(), command_line.begin(), command_line.end());
        mutable_command_line.push_back(L'\0');

        STARTUPINFOW startup_info{};
        startup_info.cb = sizeof(startup_info);

        PROCESS_INFORMATION process_info{};
        const BOOL create_result = ::CreateProcessW(
            nullptr,
            mutable_command_line.data(),
            nullptr,
            nullptr,
            TRUE,
            0,
            nullptr,
            nullptr,
            &startup_info,
            &process_info);
        if (create_result == FALSE)
        {
            return std::unexpected(ProcessLaunchError{
                .win32_error = ::GetLastError(),
                .context = L"CreateProcessW failed",
            });
        }

        UniqueHandle process_handle(process_info.hProcess);
        UniqueHandle thread_handle(process_info.hThread);
        OC_ASSERT(process_handle.valid());
        OC_ASSERT(thread_handle.valid());

        const DWORD wait_result = ::WaitForSingleObject(process_handle.get(), INFINITE);
        if (wait_result != WAIT_OBJECT_0)
        {
            return std::unexpected(ProcessLaunchError{
                .win32_error = ::GetLastError(),
                .context = L"WaitForSingleObject failed",
            });
        }

        DWORD exit_code = 0;
        if (::GetExitCodeProcess(process_handle.get(), &exit_code) == FALSE)
        {
            return std::unexpected(ProcessLaunchError{
                .win32_error = ::GetLastError(),
                .context = L"GetExitCodeProcess failed",
            });
        }

        return exit_code;
    }
}
