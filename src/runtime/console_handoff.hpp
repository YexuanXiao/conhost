#pragma once

// Classic console-host handoff COM interfaces.
//
// Upstream OpenConsole defines these interfaces in IDL (`src/host/proxy/IConsoleHandoff.idl`)
// and uses MIDL to generate C/C++ headers and a proxy/stub DLL for marshalling.
//
// `console_new` avoids that *build-time* dependency by defining the required types
// directly in standard C++ (C++23) while preserving the exact ABI.

#include <Windows.h>
#include <unknwn.h>

#include <cstddef>

// Portable subset of the driver attach message descriptor.
// Layout must match `CONSOLE_PORTABLE_ATTACH_MSG` in the upstream IDL.
#pragma pack(push, 8)
struct CONSOLE_PORTABLE_ATTACH_MSG final
{
    DWORD IdLowPart{};
    LONG IdHighPart{};
    ULONG64 Process{};
    ULONG64 Object{};
    ULONG Function{};
    ULONG InputSize{};
    ULONG OutputSize{};
};
#pragma pack(pop)

static_assert(sizeof(CONSOLE_PORTABLE_ATTACH_MSG) == 0x28);
static_assert(offsetof(CONSOLE_PORTABLE_ATTACH_MSG, Process) == 0x8);
static_assert(offsetof(CONSOLE_PORTABLE_ATTACH_MSG, InputSize) == 0x1C);
static_assert(offsetof(CONSOLE_PORTABLE_ATTACH_MSG, OutputSize) == 0x20);

using PCONSOLE_PORTABLE_ATTACH_MSG = CONSOLE_PORTABLE_ATTACH_MSG*;
using PCCONSOLE_PORTABLE_ATTACH_MSG = const CONSOLE_PORTABLE_ATTACH_MSG*;

struct __declspec(uuid("E686C757-9A35-4A1C-B3CE-0BCC8B5C69F4")) IConsoleHandoff : ::IUnknown
{
    virtual HRESULT __stdcall EstablishHandoff(
        HANDLE server,
        HANDLE inputEvent,
        PCCONSOLE_PORTABLE_ATTACH_MSG msg,
        HANDLE signalPipe,
        HANDLE inboxProcess,
        HANDLE* process) = 0;
};

// Marker interface used by the inbox host to validate "default terminal" COM servers.
struct __declspec(uuid("746E6BC0-AB05-4E38-AB14-71E86763141F")) IDefaultTerminalMarker : ::IUnknown
{
};
