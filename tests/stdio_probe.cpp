#include <Windows.h>

#include <cstdio>
#include <cstdint>

namespace
{
    void report_handle(const char* name, const DWORD std_id) noexcept
    {
        const HANDLE handle = ::GetStdHandle(static_cast<DWORD>(std_id));
        if (handle == nullptr)
        {
            std::printf("%s: null\r\n", name);
            return;
        }
        if (handle == INVALID_HANDLE_VALUE)
        {
            std::printf("%s: invalid\r\n", name);
            return;
        }

        const DWORD file_type = ::GetFileType(handle);
        DWORD mode = 0;
        const BOOL is_console = ::GetConsoleMode(handle, &mode);
        std::printf(
            "%s: handle=0x%p file_type=%lu console=%d mode=0x%08lX\r\n",
            name,
            handle,
            file_type,
            is_console ? 1 : 0,
            mode);
    }
}

int wmain() noexcept
{
    // This helper intentionally uses stdio so the process integration tests can
    // observe what standard handles the ConPTY client received.
    report_handle("stdin", STD_INPUT_HANDLE);
    report_handle("stdout", STD_OUTPUT_HANDLE);
    report_handle("stderr", STD_ERROR_HANDLE);
    std::printf("done\r\n");
    std::fflush(stdout);
    return 0;
}

