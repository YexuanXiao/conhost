#pragma once

// Default client command resolution.
//
// In EXE mode, `openconsole_new` may be started without an explicit client
// command line. For compatibility with conhost, the runtime then launches a
// default shell (typically `cmd.exe`).
//
// This module isolates that policy decision from the session runtime.

#include <string>

namespace oc::runtime
{
    class StartupCommand final
    {
    public:
        [[nodiscard]] static std::wstring resolve_default_client_command();
    };
}
