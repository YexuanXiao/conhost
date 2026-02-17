#include "runtime/startup_command.hpp"

#include <Windows.h>

// `cmd.exe` resolution deliberately avoids hard-coding `C:\\Windows` when
// possible and prefers the `WINDIR` environment variable. This mirrors
// conhost-style behavior while remaining robust in unusual Windows setups.

namespace oc::runtime
{
    std::wstring StartupCommand::resolve_default_client_command()
    {
        const DWORD required = ::GetEnvironmentVariableW(L"WINDIR", nullptr, 0);
        if (required > 1)
        {
            std::wstring windir(required, L'\0');
            const DWORD written = ::GetEnvironmentVariableW(L"WINDIR", windir.data(), required);
            if (written > 0)
            {
                windir.resize(written);
                if (!windir.empty())
                {
                    const wchar_t tail = windir.back();
                    if (tail != L'\\' && tail != L'/')
                    {
                        windir.push_back(L'\\');
                    }
                    windir.append(L"system32\\cmd.exe");
                    return windir;
                }
            }
        }

        // Stable fallback when WINDIR is unavailable.
        return L"C:\\Windows\\system32\\cmd.exe";
    }
}
