#include "runtime/legacy_conhost.hpp"

// The legacy inbox conhost exposes a private entrypoint in `ConhostV1.dll` that
// starts the legacy IO thread for a given ConDrv server handle. This file
// provides the minimal glue needed to call that entrypoint when the selection
// policy requests legacy behavior.
//
// Note:
// - The replacement intentionally does not unload `ConhostV1.dll` after
//   activation. The legacy IO thread lives inside the DLL, so unloading would
//   be unsafe.

namespace oc::runtime
{
    std::expected<void, LegacyConhostError> LegacyConhost::activate(const core::HandleView server_handle) noexcept
    {
        HMODULE module = ::LoadLibraryExW(L"ConhostV1.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
        if (module == nullptr)
        {
            return std::unexpected(LegacyConhostError{ .win32_error = ::GetLastError() });
        }

        using ConsoleCreateIoThreadFn = LONG(WINAPI*)(HANDLE);
        const auto proc = reinterpret_cast<ConsoleCreateIoThreadFn>(::GetProcAddress(module, "ConsoleCreateIoThread"));
        if (proc == nullptr)
        {
            const DWORD error = ::GetLastError();
            ::FreeLibrary(module);
            return std::unexpected(LegacyConhostError{ .win32_error = error });
        }

        const LONG status = proc(server_handle.get());
        if (status < 0)
        {
            ::FreeLibrary(module);
            return std::unexpected(LegacyConhostError{ .win32_error = static_cast<DWORD>(status) });
        }

        // Intentionally keep ConhostV1 loaded for the lifetime of the process
        // once the legacy IO thread is transferred.
        return {};
    }
}
