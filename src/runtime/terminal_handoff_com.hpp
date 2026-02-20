#pragma once

// ConPTY terminal handoff COM interfaces.
//
// Upstream OpenConsole defines these interfaces in IDL (`src/host/proxy/ITerminalHandoff.idl`)
// and uses MIDL to generate C/C++ headers and a proxy/stub DLL for marshalling.
//
// `openconsole_new` avoids that *build-time* dependency by defining the required types
// directly in standard C++ (C++23) while preserving the exact ABI.

#include <Windows.h>
#include <unknwn.h>

#include <cstddef>

// Layout must match `TERMINAL_STARTUP_INFO` in the upstream IDL.
#pragma pack(push, 8)
struct TERMINAL_STARTUP_INFO final
{
    BSTR pszTitle{};
    BSTR pszIconPath{};
    LONG iconIndex{};
    DWORD dwX{};
    DWORD dwY{};
    DWORD dwXSize{};
    DWORD dwYSize{};
    DWORD dwXCountChars{};
    DWORD dwYCountChars{};
    DWORD dwFillAttribute{};
    DWORD dwFlags{};
    WORD wShowWindow{};
};
#pragma pack(pop)

static_assert(offsetof(TERMINAL_STARTUP_INFO, pszTitle) == 0x0);
static_assert(offsetof(TERMINAL_STARTUP_INFO, iconIndex) == 0x10);
static_assert(offsetof(TERMINAL_STARTUP_INFO, wShowWindow) == 0x34);
static_assert(sizeof(TERMINAL_STARTUP_INFO) == 0x38);

struct __declspec(uuid("59D55CCE-FC8A-48B4-ACE8-0A9286C6557F")) ITerminalHandoff : ::IUnknown
{
    virtual HRESULT __stdcall EstablishPtyHandoff(
        HANDLE in_pipe,
        HANDLE out_pipe,
        HANDLE signal_pipe,
        HANDLE reference,
        HANDLE server_process,
        HANDLE client_process) = 0;
};

struct __declspec(uuid("AA6B364F-4A50-4176-9002-0AE755E7B5EF")) ITerminalHandoff2 : ::IUnknown
{
    virtual HRESULT __stdcall EstablishPtyHandoff(
        HANDLE in_pipe,
        HANDLE out_pipe,
        HANDLE signal_pipe,
        HANDLE reference,
        HANDLE server_process,
        HANDLE client_process,
        TERMINAL_STARTUP_INFO startup_info) = 0;
};

struct __declspec(uuid("6F23DA90-15C5-4203-9DB0-64E73F1B1B00")) ITerminalHandoff3 : ::IUnknown
{
    virtual HRESULT __stdcall EstablishPtyHandoff(
        HANDLE* in_pipe,
        HANDLE* out_pipe,
        HANDLE signal_pipe,
        HANDLE reference,
        HANDLE server_process,
        HANDLE client_process,
        const TERMINAL_STARTUP_INFO* startup_info) = 0;
};
