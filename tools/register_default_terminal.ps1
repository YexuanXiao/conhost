<#
.SYNOPSIS
Registers `openconsole_new` as the per-user Windows "Default terminal" (classic console host handoff).

.DESCRIPTION
Windows 10/11 use default-terminal delegation to choose which out-of-proc COM local server
receives console host handoffs.

This script performs two operations (HKCU only, no admin):
1) Registers the COM local server for the `IConsoleHandoff` CLSID used by this repo.
2) Sets `HKCU\Console\%%Startup\DelegationConsole` to that CLSID.

This targets the *classic* console-host handoff (`IConsoleHandoff` / `DelegationConsole`).
It does NOT configure ConPTY terminal handoff (`DelegationTerminal` / `ITerminalHandoff*`).

.PARAMETER ExePath
Full path to `openconsole_new.exe`.
If omitted, the script tries to resolve `..\..\build-new\openconsole_new.exe` relative to this file.

.PARAMETER Uninstall
Removes the per-user `DelegationConsole` value and (if it looks like ours) the per-user COM registration.

.PARAMETER RegisterProxyStub
Optionally registers the proxy/stub DLL for `IConsoleHandoff` / `IDefaultTerminalMarker` marshalling in HKCU.
Most Windows 11 machines already have this registered system-wide, so you usually don't need this.

.PARAMETER ProxyStubPath
Full path to `oc_new_openconsole_proxy.dll` (built by the top-level `new/proxy` target).
If omitted and `-RegisterProxyStub` is set, the script tries to resolve `..\..\build-new\oc_new_openconsole_proxy.dll`
(and falls back to the legacy `..\..\build-new\tests\oc_new_openconsole_proxy.dll` location).

.EXAMPLE
.\new\tools\register_default_terminal.ps1 -ExePath (Resolve-Path .\build-new\openconsole_new.exe)

.EXAMPLE
.\new\tools\register_default_terminal.ps1 -Uninstall
#>

[CmdletBinding(SupportsShouldProcess = $true)]
param(
    [Parameter()]
    [string]$ExePath,

    [Parameter()]
    [switch]$Uninstall,

    [Parameter()]
    [switch]$RegisterProxyStub,

    [Parameter()]
    [string]$ProxyStubPath
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

# CLSID used by `openconsole_new`'s COM embedding server (`-Embedding`).
# See: `new/src/runtime/com_embedding_server.cpp`.
$ConsoleHandoffClsid = '{1F9F2BF5-5BC3-4F17-B0E6-912413F1F451}'

# IID values from `src/host/proxy/IConsoleHandoff.idl`.
$IConsoleHandoffIid = '{E686C757-9A35-4A1C-B3CE-0BCC8B5C69F4}'
$IDefaultTerminalMarkerIid = '{746E6BC0-AB05-4E38-AB14-71E86763141F}'

# Proxy/stub CLSID used by upstream OpenConsoleProxy for the unbranded/dev build.
# Matches `new/tests/com_embedding_integration_tests.cpp`.
$OpenConsoleProxyClsid = '{DEC4804D-56D1-4F73-9FBE-6828E7C85C56}'

function Resolve-DefaultExePath {
    $candidate = Join-Path $PSScriptRoot '..\..\build-new\openconsole_new.exe'
    $resolved = Resolve-Path -LiteralPath $candidate -ErrorAction SilentlyContinue
    if ($null -ne $resolved) {
        return $resolved.Path
    }
    return $null
}

function Resolve-DefaultProxyStubPath {
    # New location: emitted next to `openconsole_new.exe` in the build root.
    $candidates = @(
        (Join-Path $PSScriptRoot '..\..\build-new\oc_new_openconsole_proxy.dll'),
        # Legacy location used by the earlier test-only build.
        (Join-Path $PSScriptRoot '..\..\build-new\tests\oc_new_openconsole_proxy.dll')
    )

    foreach ($candidate in $candidates) {
        $resolved = Resolve-Path -LiteralPath $candidate -ErrorAction SilentlyContinue
        if ($null -ne $resolved) {
            return $resolved.Path
        }
    }
    return $null
}

function Set-RegistryDefaultValue {
    param(
        [Parameter(Mandatory = $true)]
        [string]$SubKeyPath,
        [Parameter(Mandatory = $true)]
        [string]$Value
    )

    $key = [Microsoft.Win32.Registry]::CurrentUser.CreateSubKey($SubKeyPath, $true)
    if ($null -eq $key) {
        throw "Failed to create/open HKCU\$SubKeyPath"
    }

    try {
        $key.SetValue('', $Value, [Microsoft.Win32.RegistryValueKind]::String)
    } finally {
        $key.Close()
    }
}

function Get-RegistryDefaultValue {
    param(
        [Parameter(Mandatory = $true)]
        [string]$SubKeyPath
    )

    $key = [Microsoft.Win32.Registry]::CurrentUser.OpenSubKey($SubKeyPath, $false)
    if ($null -eq $key) {
        return $null
    }

    try {
        return $key.GetValue('', $null, 'DoNotExpandEnvironmentNames')
    } finally {
        $key.Close()
    }
}

function Remove-RegistrySubTree {
    param(
        [Parameter(Mandatory = $true)]
        [string]$SubKeyPath
    )

    [Microsoft.Win32.Registry]::CurrentUser.DeleteSubKeyTree($SubKeyPath, $false)
}

function Ensure-DelegationConsoleValue {
    param(
        [Parameter(Mandatory = $true)]
        [string]$ClsidString
    )

    $startupKeyPath = 'Console\%%Startup'
    $key = [Microsoft.Win32.Registry]::CurrentUser.CreateSubKey($startupKeyPath, $true)
    if ($null -eq $key) {
        throw 'Failed to create/open HKCU\Console\%%Startup'
    }

    try {
        $key.SetValue('DelegationConsole', $ClsidString, [Microsoft.Win32.RegistryValueKind]::String)
    } finally {
        $key.Close()
    }
}

function Remove-DelegationConsoleValue {
    $startupKeyPath = 'Console\%%Startup'
    $key = [Microsoft.Win32.Registry]::CurrentUser.OpenSubKey($startupKeyPath, $true)
    if ($null -eq $key) {
        return
    }

    try {
        $key.DeleteValue('DelegationConsole', $false)
    } finally {
        $key.Close()
    }
}

function Register-ComLocalServerForHandoff {
    param(
        [Parameter(Mandatory = $true)]
        [string]$ExePathResolved
    )

    # COM local-server activation appends `-Embedding` or `/Embedding` automatically.
    # Register our own option to control whether the delegated host should create a classic window.
    $commandLine = '"' + $ExePathResolved + '" --delegated-window'
    $subKeyPath = "Software\Classes\CLSID\$ConsoleHandoffClsid\LocalServer32"

    if ($PSCmdlet.ShouldProcess("HKCU\\$subKeyPath", "Set default value to $commandLine")) {
        Set-RegistryDefaultValue -SubKeyPath $subKeyPath -Value $commandLine
    }
}

function Register-ProxyStubInHkcu {
    param(
        [Parameter(Mandatory = $true)]
        [string]$ProxyDllPathResolved
    )

    if (-not (Test-Path -LiteralPath $ProxyDllPathResolved)) {
        throw "Proxy/stub DLL not found: $ProxyDllPathResolved"
    }

    $proxyInproc = "Software\Classes\CLSID\$OpenConsoleProxyClsid\InprocServer32"
    if ($PSCmdlet.ShouldProcess("HKCU\\$proxyInproc", "Register proxy/stub InprocServer32")) {
        $key = [Microsoft.Win32.Registry]::CurrentUser.CreateSubKey($proxyInproc, $true)
        if ($null -eq $key) {
            throw "Failed to create/open HKCU\\$proxyInproc"
        }
        try {
            $key.SetValue('', $ProxyDllPathResolved, [Microsoft.Win32.RegistryValueKind]::String)
            $key.SetValue('ThreadingModel', 'Both', [Microsoft.Win32.RegistryValueKind]::String)
        } finally {
            $key.Close()
        }
    }

    $interfaceKeys = @(
        "Software\Classes\Interface\$IConsoleHandoffIid\ProxyStubClsid32",
        "Software\Classes\Interface\$IDefaultTerminalMarkerIid\ProxyStubClsid32"
    )

    foreach ($path in $interfaceKeys) {
        if ($PSCmdlet.ShouldProcess("HKCU\\$path", "Set ProxyStubClsid32 to $OpenConsoleProxyClsid")) {
            $key = [Microsoft.Win32.Registry]::CurrentUser.CreateSubKey($path, $true)
            if ($null -eq $key) {
                throw "Failed to create/open HKCU\\$path"
            }
            try {
                $key.SetValue('', $OpenConsoleProxyClsid, [Microsoft.Win32.RegistryValueKind]::String)
            } finally {
                $key.Close()
            }
        }
    }
}

function Unregister-ProxyStubInHkcu {
    $paths = @(
        "Software\Classes\Interface\$IConsoleHandoffIid\ProxyStubClsid32",
        "Software\Classes\Interface\$IDefaultTerminalMarkerIid\ProxyStubClsid32",
        "Software\Classes\CLSID\$OpenConsoleProxyClsid\InprocServer32"
    )

    foreach ($path in $paths) {
        if ($PSCmdlet.ShouldProcess("HKCU\\$path", 'Delete subtree')) {
            Remove-RegistrySubTree -SubKeyPath $path
        }
    }
}

if ($Uninstall) {
    if ($PSCmdlet.ShouldProcess('HKCU\\Console\\%%Startup', 'Remove DelegationConsole')) {
        Remove-DelegationConsoleValue
    }

    $handoffServerKey = "Software\Classes\CLSID\$ConsoleHandoffClsid\LocalServer32"
    $existing = Get-RegistryDefaultValue -SubKeyPath $handoffServerKey
    if ($null -ne $existing) {
        $looksLikeOurs = ($existing -like '*openconsole_new.exe*') -or ($existing -like '*conhost.exe*')
        if ($looksLikeOurs) {
            if ($PSCmdlet.ShouldProcess("HKCU\\Software\\Classes\\CLSID\\$ConsoleHandoffClsid", 'Delete subtree')) {
                Remove-RegistrySubTree -SubKeyPath "Software\Classes\CLSID\$ConsoleHandoffClsid"
            }
        } else {
            Write-Warning "HKCU\\$handoffServerKey exists but does not look like openconsole_new; leaving it untouched."
        }
    }

    if ($RegisterProxyStub) {
        Unregister-ProxyStubInHkcu
    }

    Write-Host 'Done. New console sessions should no longer delegate to openconsole_new.'
    exit 0
}

if ([string]::IsNullOrWhiteSpace($ExePath)) {
    $ExePath = Resolve-DefaultExePath
}

if ([string]::IsNullOrWhiteSpace($ExePath)) {
    throw 'ExePath is required (or build-new\openconsole_new.exe must exist).'
}

$ExePath = (Resolve-Path -LiteralPath $ExePath).Path
if (-not (Test-Path -LiteralPath $ExePath)) {
    throw "openconsole_new.exe not found: $ExePath"
}

Register-ComLocalServerForHandoff -ExePathResolved $ExePath

if ($PSCmdlet.ShouldProcess('HKCU\\Console\\%%Startup', "Set DelegationConsole to $ConsoleHandoffClsid")) {
    Ensure-DelegationConsoleValue -ClsidString $ConsoleHandoffClsid
}

if ($RegisterProxyStub) {
    if ([string]::IsNullOrWhiteSpace($ProxyStubPath)) {
        $ProxyStubPath = Resolve-DefaultProxyStubPath
    }

    if ([string]::IsNullOrWhiteSpace($ProxyStubPath)) {
        throw 'ProxyStubPath is required (or build-new\\oc_new_openconsole_proxy.dll / build-new\\tests\\oc_new_openconsole_proxy.dll must exist).'
    }

    $ProxyStubPath = (Resolve-Path -LiteralPath $ProxyStubPath).Path
    Register-ProxyStubInHkcu -ProxyDllPathResolved $ProxyStubPath
}

Write-Host 'Done. New console sessions should now delegate to openconsole_new.'
