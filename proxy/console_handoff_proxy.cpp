// Proxy/stub implementation for `IConsoleHandoff` and `IDefaultTerminalMarker`.
//
// This project keeps a dedicated proxy/stub DLL so out-of-proc COM activation can
// marshal handle parameters for `IConsoleHandoff::EstablishHandoff`.
//
// Upstream OpenConsole generates this code with MIDL (`midl.exe`). `console_new`
// avoids that build-time dependency by keeping the required RPC/NDR wiring in-tree.
//
// If the IDL contract changes, regenerate with MIDL and update this file to match
// the new format/type strings and vtable wiring.

/* clang-format off */

/* This file mirrors MIDL-generated proxy/stub code. */

#if !defined(_WIN64)
#error oc_new_openconsole_proxy requires a 64-bit build (x64/arm64).
#endif

#if defined(_MSC_VER)
#pragma warning(push)
#pragma warning(disable : 4152) /* function/data pointer conversion in expression */
#endif

#pragma optimize("", off)

#define USE_STUBLESS_PROXY

/* verify that the <rpcproxy.h> version is high enough to compile this file */
#ifndef __REQUIRED_RPCPROXY_H_VERSION__
#define __REQUIRED_RPCPROXY_H_VERSION__ 475
#endif

#include <rpcproxy.h>
#ifndef __RPCPROXY_H_VERSION__
#error this stub requires an updated version of <rpcproxy.h>
#endif /* __RPCPROXY_H_VERSION__ */

#include <array>
#include <cstddef>

#include "runtime/console_handoff.hpp"

extern "C"
{
    extern const IID IID_IConsoleHandoff;
    extern const IID IID_IDefaultTerminalMarker;
}

namespace
{
    constexpr std::size_t kTypeFormatStringSize = 53;
    constexpr std::size_t kProcFormatStringSize = 69;
    constexpr std::ptrdiff_t kIUnknownMethodCount = 3;

    // These wrapper types match the shape of the MIDL-generated "format string"
    // structs (a small alignment pad followed by an inline byte buffer).
    //
    // We keep them as aggregates so the literal format bytes below remain easy
    // to compare against MIDL output during maintenance.
    struct TypeFormatString final
    {
        short pad{};
        std::array<unsigned char, kTypeFormatStringSize> format{};
    };

    struct ProcFormatString final
    {
        short pad{};
        std::array<unsigned char, kProcFormatStringSize> format{};
    };
}

[[maybe_unused]] static const RPC_SYNTAX_IDENTIFIER _RpcTransferSyntax_2_0 = {
    { 0x8A885D04, 0x1CEB, 0x11C9, { 0x9F, 0xE8, 0x08, 0x00, 0x2B, 0x10, 0x48, 0x60 } }, { 2, 0 }
};

namespace
{
    // `Object_StubDesc` is referenced by the proxy/stub tables below. Keep a
    // forward declaration so the definitions can remain grouped in the same
    // order as the classic MIDL output (format strings first, proxy tables later).
    extern const MIDL_STUB_DESC Object_StubDesc;
}

#if !(TARGET_IS_NT102_OR_LATER)
#error This proxy/stub requires NTDDI_VERSION >= NTDDI_WIN10_RS1 ([system_handle]).
#endif

static const ProcFormatString IConsoleHandoff__MIDL_ProcFormatString = {
    0,
    {
        /* Procedure EstablishHandoff */

        0x33, /* FC_AUTO_HANDLE */
        0x6c, /* Old Flags:  object, Oi2 */
        /*  2 */ NdrFcLong(0x0), /* 0 */
        /*  6 */ NdrFcShort(0x3), /* 3 */
        /*  8 */ NdrFcShort(0x40), /* x86 Stack size/offset = 64 */
        /* 10 */ NdrFcShort(0x0), /* 0 */
        /* 12 */ NdrFcShort(0x8), /* 8 */
        /* 14 */ 0x47, /* Oi2 Flags:  srv must size, clt must size, has return, has ext, */
        0x7, /* 7 */
        /* 16 */ 0xa, /* 10 */
        0x1, /* Ext Flags:  new corr desc, */
        /* 18 */ NdrFcShort(0x0), /* 0 */
        /* 20 */ NdrFcShort(0x0), /* 0 */
        /* 22 */ NdrFcShort(0x0), /* 0 */
        /* 24 */ NdrFcShort(0x0), /* 0 */

        /* Parameter server */

        /* 26 */ NdrFcShort(0x8b), /* Flags:  must size, must free, in, by val, */
        /* 28 */ NdrFcShort(0x8), /* x86 Stack size/offset = 8 */
        /* 30 */ NdrFcShort(0x2), /* Type Offset=2 */

        /* Parameter inputEvent */

        /* 32 */ NdrFcShort(0x8b), /* Flags:  must size, must free, in, by val, */
        /* 34 */ NdrFcShort(0x10), /* x86 Stack size/offset = 16 */
        /* 36 */ NdrFcShort(0x8), /* Type Offset=8 */

        /* Parameter msg */

        /* 38 */ NdrFcShort(0x10b), /* Flags:  must size, must free, in, simple ref, */
        /* 40 */ NdrFcShort(0x18), /* x86 Stack size/offset = 24 */
        /* 42 */ NdrFcShort(0x12), /* Type Offset=18 */

        /* Parameter signalPipe */

        /* 44 */ NdrFcShort(0x8b), /* Flags:  must size, must free, in, by val, */
        /* 46 */ NdrFcShort(0x20), /* x86 Stack size/offset = 32 */
        /* 48 */ NdrFcShort(0x24), /* Type Offset=36 */

        /* Parameter inboxProcess */

        /* 50 */ NdrFcShort(0x8b), /* Flags:  must size, must free, in, by val, */
        /* 52 */ NdrFcShort(0x28), /* x86 Stack size/offset = 40 */
        /* 54 */ NdrFcShort(0x2a), /* Type Offset=42 */

        /* Parameter process */

        /* 56 */ NdrFcShort(0x2113), /* Flags:  must size, must free, out, simple ref, srv alloc size=8 */
        /* 58 */ NdrFcShort(0x30), /* x86 Stack size/offset = 48 */
        /* 60 */ NdrFcShort(0x2a), /* Type Offset=42 */

        /* Return value */

        /* 62 */ NdrFcShort(0x70), /* Flags:  out, return, base type, */
        /* 64 */ NdrFcShort(0x38), /* x86 Stack size/offset = 56 */
        /* 66 */ 0x8, /* FC_LONG */
        0x0, /* 0 */

        0x0
    }
};

static const TypeFormatString IConsoleHandoff__MIDL_TypeFormatString = {
    0,
    {
        NdrFcShort(0x0), /* 0 */
        /*  2 */ 0x3c, /* FC_SYSTEM_HANDLE */
        0x0, /* 0 */
        /*  4 */ NdrFcLong(0x0), /* 0 */
        /*  8 */ 0x3c, /* FC_SYSTEM_HANDLE */
        0x2, /* 2 */
        /* 10 */ NdrFcLong(0x0), /* 0 */
        /* 14 */
        0x11, 0x0, /* FC_RP */
        /* 16 */ NdrFcShort(0x2), /* Offset= 2 (18) */
        /* 18 */
        0x1a, /* FC_BOGUS_STRUCT */
        0x7, /* 7 */
        /* 20 */ NdrFcShort(0x28), /* 40 */
        /* 22 */ NdrFcShort(0x0), /* 0 */
        /* 24 */ NdrFcShort(0x0), /* Offset= 0 (24) */
        /* 26 */ 0x8, /* FC_LONG */
        0x8, /* FC_LONG */
        /* 28 */ 0xb, /* FC_HYPER */
        0xb, /* FC_HYPER */
        /* 30 */ 0x8, /* FC_LONG */
        0x8, /* FC_LONG */
        /* 32 */ 0x8, /* FC_LONG */
        0x40, /* FC_STRUCTPAD4 */
        /* 34 */ 0x5c, /* FC_PAD */
        0x5b, /* FC_END */
        /* 36 */ 0x3c, /* FC_SYSTEM_HANDLE */
        0xc, /* 12 */
        /* 38 */ NdrFcLong(0x0), /* 0 */
        /* 42 */ 0x3c, /* FC_SYSTEM_HANDLE */
        0x4, /* 4 */
        /* 44 */ NdrFcLong(0x0), /* 0 */
        /* 48 */
        0x11, 0x4, /* FC_RP [alloced_on_stack] */
        /* 50 */ NdrFcShort(0xfff8), /* Offset= -8 (42) */

        0x0
    }
};

/* Standard interface: __MIDL_itf_IConsoleHandoff_0000_0000, ver. 0.0,
   GUID={0x00000000,0x0000,0x0000,{0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00}} */

/* Object interface: IUnknown, ver. 0.0,
   GUID={0x00000000,0x0000,0x0000,{0xC0,0x00,0x00,0x00,0x00,0x00,0x00,0x46}} */

/* Object interface: IConsoleHandoff, ver. 0.0,
   GUID={0xE686C757,0x9A35,0x4A1C,{0xB3,0xCE,0x0B,0xCC,0x8B,0x5C,0x69,0xF4}} */

#pragma code_seg(".orpc")
static const unsigned short IConsoleHandoff_FormatStringOffsetTable[] = { 0 };

static const MIDL_STUBLESS_PROXY_INFO IConsoleHandoff_ProxyInfo = {
    &Object_StubDesc,
    IConsoleHandoff__MIDL_ProcFormatString.format.data(),
    // The format-string offset table does not include IUnknown methods, so it is
    // indexed relative to method #3 (`IConsoleHandoff::EstablishHandoff`).
    &IConsoleHandoff_FormatStringOffsetTable[-kIUnknownMethodCount],
    nullptr,
    0,
    nullptr
};

static const MIDL_SERVER_INFO IConsoleHandoff_ServerInfo = {
    &Object_StubDesc,
    nullptr,
    IConsoleHandoff__MIDL_ProcFormatString.format.data(),
    &IConsoleHandoff_FormatStringOffsetTable[-kIUnknownMethodCount],
    nullptr,
    nullptr,
    0,
    nullptr
};

// rpcproxy.h uses `void*` for vtable entries (matching the MIDL-generated C code).
// This relies on Microsoft-specific behavior when initializing entries with
// function pointers. Keep that behavior local to this translation unit.
#if defined(__clang__)
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wmicrosoft-cast"
#endif

CINTERFACE_PROXY_VTABLE(4) _IConsoleHandoffProxyVtbl = {
    { &IConsoleHandoff_ProxyInfo, &IID_IConsoleHandoff },
    {
        reinterpret_cast<void*>(IUnknown_QueryInterface_Proxy),
        reinterpret_cast<void*>(IUnknown_AddRef_Proxy),
        reinterpret_cast<void*>(IUnknown_Release_Proxy),
        reinterpret_cast<void*>(static_cast<INT_PTR>(-1)) /* IConsoleHandoff::EstablishHandoff */
    }
};

const CInterfaceStubVtbl _IConsoleHandoffStubVtbl = {
    { &IID_IConsoleHandoff, &IConsoleHandoff_ServerInfo, 4, 0 /* pure interpreted */ },
    { CStdStubBuffer_METHODS_OPT }
};

/* Object interface: IDefaultTerminalMarker, ver. 0.0,
   GUID={0x746E6BC0,0xAB05,0x4E38,{0xAB,0x14,0x71,0xE8,0x67,0x63,0x14,0x1F}} */

#pragma code_seg(".orpc")
static const unsigned short IDefaultTerminalMarker_FormatStringOffsetTable[] = { 0 };

[[maybe_unused]] static const MIDL_STUBLESS_PROXY_INFO IDefaultTerminalMarker_ProxyInfo = {
    &Object_StubDesc,
    IConsoleHandoff__MIDL_ProcFormatString.format.data(),
    &IDefaultTerminalMarker_FormatStringOffsetTable[-kIUnknownMethodCount],
    nullptr,
    0,
    nullptr
};

static const MIDL_SERVER_INFO IDefaultTerminalMarker_ServerInfo = {
    &Object_StubDesc,
    nullptr,
    IConsoleHandoff__MIDL_ProcFormatString.format.data(),
    &IDefaultTerminalMarker_FormatStringOffsetTable[-kIUnknownMethodCount],
    nullptr,
    nullptr,
    0,
    nullptr
};

CINTERFACE_PROXY_VTABLE(3) _IDefaultTerminalMarkerProxyVtbl = {
    { nullptr, &IID_IDefaultTerminalMarker },
    {
        reinterpret_cast<void*>(IUnknown_QueryInterface_Proxy),
        reinterpret_cast<void*>(IUnknown_AddRef_Proxy),
        reinterpret_cast<void*>(IUnknown_Release_Proxy),
    }
};

#if defined(__clang__)
#pragma clang diagnostic pop
#endif

const CInterfaceStubVtbl _IDefaultTerminalMarkerStubVtbl = {
    { &IID_IDefaultTerminalMarker, &IDefaultTerminalMarker_ServerInfo, 3, 0 /* pure interpreted */ },
    { CStdStubBuffer_METHODS_OPT }
};

namespace
{
    const MIDL_STUB_DESC Object_StubDesc = {
        nullptr,
        NdrOleAllocate,
        NdrOleFree,
        { nullptr },
        nullptr,
        nullptr,
        nullptr,
        nullptr,
        IConsoleHandoff__MIDL_TypeFormatString.format.data(),
        1, /* -error bounds_check flag */
        0xa0000, /* Ndr library version */
        nullptr,
        0x8010274, /* MIDL Version 8.1.628 */
        nullptr,
        nullptr,
        nullptr, /* notify & notify_flag routine table */
        0x1, /* MIDL flag */
        nullptr, /* cs routines */
        nullptr, /* proxy/server info */
        nullptr /* Reserved5 */
    };
}

const CInterfaceProxyVtbl* _IConsoleHandoff_ProxyVtblList[] = {
    (CInterfaceProxyVtbl*)&_IConsoleHandoffProxyVtbl,
    (CInterfaceProxyVtbl*)&_IDefaultTerminalMarkerProxyVtbl,
    nullptr
};

const CInterfaceStubVtbl* _IConsoleHandoff_StubVtblList[] = {
    (CInterfaceStubVtbl*)&_IConsoleHandoffStubVtbl,
    (CInterfaceStubVtbl*)&_IDefaultTerminalMarkerStubVtbl,
    nullptr
};

PCInterfaceName const _IConsoleHandoff_InterfaceNamesList[] = {
    "IConsoleHandoff",
    "IDefaultTerminalMarker",
    nullptr
};

#define _IConsoleHandoff_CHECK_IID(n) IID_GENERIC_CHECK_IID(_IConsoleHandoff, pIID, n)

int __stdcall _IConsoleHandoff_IID_Lookup(const IID* pIID, int* pIndex)
{
    IID_BS_LOOKUP_SETUP

    IID_BS_LOOKUP_INITIAL_TEST(_IConsoleHandoff, 2, 1)
    IID_BS_LOOKUP_RETURN_RESULT(_IConsoleHandoff, 2, *pIndex)
}

EXTERN_C const ExtendedProxyFileInfo IConsoleHandoff_ProxyFileInfo = {
    (PCInterfaceProxyVtblList*)&_IConsoleHandoff_ProxyVtblList,
    (PCInterfaceStubVtblList*)&_IConsoleHandoff_StubVtblList,
    (const PCInterfaceName*)&_IConsoleHandoff_InterfaceNamesList,
    0, /* no delegation */
    &_IConsoleHandoff_IID_Lookup,
    2,
    2,
    0, /* table of [async_uuid] interfaces */
    0, /* Filler1 */
    0, /* Filler2 */
    0 /* Filler3 */
};

#if defined(_MSC_VER)
#pragma warning(pop)
#endif

/* clang-format on */
