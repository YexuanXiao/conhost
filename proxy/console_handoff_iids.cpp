// Defines the IID constants referenced by the proxy/stub implementation.
//
// MIDL typically emits an `*_i.c` file for these. We keep them in C++ to avoid
// a `midl.exe` build dependency while preserving the stable symbol names used
// by `rpcproxy.h`-based proxy DLLs.

#include "runtime/console_handoff.hpp"
#include "runtime/terminal_handoff_com.hpp"

extern "C"
{
    extern const IID IID_IConsoleHandoff = __uuidof(IConsoleHandoff);
    extern const IID IID_IDefaultTerminalMarker = __uuidof(IDefaultTerminalMarker);
    extern const IID IID_ITerminalHandoff = __uuidof(ITerminalHandoff);
    extern const IID IID_ITerminalHandoff2 = __uuidof(ITerminalHandoff2);
    extern const IID IID_ITerminalHandoff3 = __uuidof(ITerminalHandoff3);
}
