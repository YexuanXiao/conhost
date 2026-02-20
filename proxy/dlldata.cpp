// Proxy/stub DLL data wiring for `oc_new_openconsole_proxy`.
//
// This mirrors the tiny `dlldata.c` file normally produced by MIDL so the build
// does not depend on `midl.exe`.

#include <rpcproxy.h>

extern "C"
{
    EXTERN_PROXY_FILE(IConsoleHandoff)
    EXTERN_PROXY_FILE(ITerminalHandoff)

    PROXYFILE_LIST_START
        REFERENCE_PROXY_FILE(IConsoleHandoff),
        REFERENCE_PROXY_FILE(ITerminalHandoff),
    PROXYFILE_LIST_END

    DLLDATA_ROUTINES(aProxyFileList, GET_DLL_CLSID)
}
