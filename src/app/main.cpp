#include "app/application.hpp"

#include "core/console_writer.hpp"
#include "core/exception.hpp"

#include <Windows.h>

#include <cwchar>

int WINAPI wWinMain(HINSTANCE /*instance*/, HINSTANCE /*prev_instance*/, PWSTR /*command_line*/, int /*show_command*/)
{
    try
    {
        oc::app::Application application;
        return application.run();
    }
    catch (const oc::core::Win32Error error)
    {
        wchar_t message[512]{};
        const DWORD code = oc::core::to_dword(error);
        _snwprintf_s(
            message,
            _TRUNCATE,
            L"Unhandled Win32 error=%lu",
            static_cast<unsigned long>(code));
        oc::core::write_console_line(message);
        return static_cast<int>(code == 0 ? ERROR_GEN_FAILURE : code);
    }
    catch (const oc::core::AppException& error)
    {
        oc::core::write_console_line(error.message());
        return static_cast<int>(ERROR_GEN_FAILURE);
    }
    catch (...)
    {
        oc::core::write_console_line(L"Unhandled unknown exception");
        return static_cast<int>(ERROR_UNHANDLED_EXCEPTION);
    }
}
