# Make `openconsole_new` the Windows “Default terminal” (Dev)

Windows 10/11 select the “default terminal application” for console sessions via **default-terminal delegation**:

- `HKCU\Console\%%Startup\DelegationConsole` → CLSID for a COM local server implementing `IConsoleHandoff`
- (separate) `HKCU\Console\%%Startup\DelegationTerminal` → CLSID for a COM local server implementing `ITerminalHandoff*` (ConPTY/terminal UI)

`openconsole_new` currently implements the **console host handoff** (`IConsoleHandoff`) COM server behind `-Embedding`.

It also implements the **terminal UI handoff** (`ITerminalHandoff*`, currently `ITerminalHandoff3`) behind `-Embedding`.

The CLSID used by this repo is the upstream “unbranded” OpenConsole handoff CLSID:

- `{1F9F2BF5-5BC3-4F17-B0E6-912413F1F451}` (see `new/src/runtime/com_embedding_server.cpp:56`)

## 1) Quick setup (per-user, no admin)

1. Build the project:
   ```powershell
   cmake -S new -B build-new -G Ninja -DCMAKE_CXX_COMPILER=clang-cl
   cmake --build build-new
   ```

2. Register `openconsole_new.exe` as the COM **local server** for the handoff CLSID (HKCU):
   - Registry key: `HKCU\Software\Classes\CLSID\{1F9F2BF5-5BC3-4F17-B0E6-912413F1F451}\LocalServer32`
   - Default value: `"C:\path\to\openconsole_new.exe" --delegated-window`
      - COM appends `-Embedding` (or `/Embedding`) automatically on activation.
      - `--delegated-window` is an `openconsole_new` option that enables classic window hosting for the delegated session.
        If you omit it, `-Embedding` remains headless by default.

3. Point Windows’ delegation setting at that CLSID:
   - Registry key: `HKCU\Console\%%Startup`
   - Value (REG_SZ): `DelegationConsole` = `{1F9F2BF5-5BC3-4F17-B0E6-912413F1F451}`

4. (Optional) Enable ConPTY terminal UI delegation too:
   - Registry key: `HKCU\Console\%%Startup`
   - Value (REG_SZ): `DelegationTerminal` = `{1F9F2BF5-5BC3-4F17-B0E6-912413F1F451}`

You can do steps (2) and (3) (and optionally (4)) with the helper script:
```powershell
.\new\tools\register_default_terminal.ps1 -ExePath (Resolve-Path .\build-new\openconsole_new.exe)
```
To enable `DelegationTerminal` too:
```powershell
.\new\tools\register_default_terminal.ps1 -ExePath (Resolve-Path .\build-new\openconsole_new.exe) -EnableDelegationTerminal
```

## 2) Verify

- Launch a console app from Explorer/Win+R (for example `cmd.exe`).
- If file logging is enabled (`OPENCONSOLE_NEW_ENABLE_FILE_LOGGING=1`), you should see a new `openconsole_new` log indicating `-Embedding` startup:
  - “Embedding mode requested; starting COM local server” (see `new/src/app/application.cpp:149`)
  - “Creating delegated window host (--delegated-window)” (see `new/src/runtime/default_terminal_host.cpp`)

## 3) Roll back

To revert to “Let Windows decide” / previous default, remove the override values/keys under HKCU:
```powershell
.\new\tools\register_default_terminal.ps1 -Uninstall
```

## Notes / limitations

- `DelegationTerminal` uses the ConPTY byte transport. Window resize propagation is still incremental.
- The classic window host is still incremental: rendering is implemented, but keyboard/mouse input injection is a follow-up
  (see `docs/design/renderer_window_host.md`).
- COM registration is per-user (HKCU) so it overrides machine registrations (HKLM) without admin rights.
