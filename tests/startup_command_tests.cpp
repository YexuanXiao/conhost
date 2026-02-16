#include "runtime/startup_command.hpp"

#include <Windows.h>

#include <optional>
#include <string>

namespace
{
    [[nodiscard]] std::optional<std::wstring> read_environment(const wchar_t* const name)
    {
        const DWORD needed = ::GetEnvironmentVariableW(name, nullptr, 0);
        if (needed == 0)
        {
            return std::nullopt;
        }
        std::wstring value(needed, L'\0');
        const DWORD written = ::GetEnvironmentVariableW(name, value.data(), needed);
        if (written == 0)
        {
            return std::nullopt;
        }
        value.resize(written);
        return value;
    }

    bool test_default_command_not_empty()
    {
        const std::wstring command = oc::runtime::StartupCommand::resolve_default_client_command();
        return !command.empty();
    }

    bool test_default_command_contains_cmd()
    {
        const std::wstring command = oc::runtime::StartupCommand::resolve_default_client_command();
        return command.find(L"cmd.exe") != std::wstring::npos;
    }

    bool test_uses_windir_when_available()
    {
        const auto previous = read_environment(L"WINDIR");

        ::SetEnvironmentVariableW(L"WINDIR", L"C:\\TestWindows");
        const std::wstring command = oc::runtime::StartupCommand::resolve_default_client_command();

        if (!previous)
        {
            ::SetEnvironmentVariableW(L"WINDIR", nullptr);
        }
        else
        {
            ::SetEnvironmentVariableW(L"WINDIR", previous->c_str());
        }

        return command == L"C:\\TestWindows\\system32\\cmd.exe";
    }
}

bool run_startup_command_tests()
{
    return test_default_command_not_empty() &&
           test_default_command_contains_cmd() &&
           test_uses_windir_when_available();
}
