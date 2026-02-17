# Windows API Inventory

This document lists Windows APIs used by the reimplementation in `new/`.

## Process and Command Line
- `GetCommandLineW`
- `CommandLineToArgvW`
- `CreateProcessW`
- `CreateThread`
- `WaitForSingleObject`
- `WaitForMultipleObjects`
- `TerminateProcess`
- `Sleep`
- `GetExitCodeProcess`
- `GetStdHandle`
- `GetModuleFileNameW` (tests)
- `GetTickCount64` (tests)
- `InitializeProcThreadAttributeList`
- `UpdateProcThreadAttribute`
- `DeleteProcThreadAttributeList`
- `SetProcessShutdownParameters`

## COM
- `CoInitializeEx`
- `CoUninitialize`
- `CoRegisterClassObject`
- `CoRevokeClassObject`

## DirectWrite
- `DWriteCreateFactory`
- `IDWriteFactory::GetSystemFontCollection`
- `IDWriteFontCollection::FindFamilyName`
- `IDWriteFontCollection::GetFontFamily`
- `IDWriteFontFamily::GetFirstMatchingFont`
- `IDWriteFont::CreateFontFace`
- `IDWriteFontFace::GetMetrics`
- `IDWriteFontFace::GetGlyphIndicesW`
- `IDWriteFontFace::GetDesignGlyphMetrics`

## Handles and Memory
- `CloseHandle`
- `LocalFree`
- `CreateFileW`
- `CreateDirectoryW`
- `DeleteFileW` (tests)
- `GetFileAttributesW` (tests)
- `CreateEventW`
- `ReadFile`
- `GetFileSizeEx`
- `CreatePipe`
- `DuplicateHandle`
- `GetCurrentProcess`
- `CancelSynchronousIo`
- `DeviceIoControl`
- `GetHandleInformation`
- `SetHandleInformation`
- `PeekNamedPipe`
- `SetEvent`
- `ResetEvent`
- `SetFilePointerEx`
- `GetFileType`
- `GetModuleHandleW` (tests)
- `LoadLibraryW` (tests)
- `LoadLibraryExW`
- `GetProcAddress`
- `FreeLibrary`

## NTDLL (Tests Only)
- `NtOpenFile` (tests)
- `RtlNtStatusToDosError` (tests)

## Registry
- `RegOpenKeyExW`
- `RegQueryValueExW`
- `RegCloseKey`

## Environment and Locale
- `GetEnvironmentVariableW`
- `SetEnvironmentVariableW` (tests)
- `ExpandEnvironmentStringsW`
- `GetOEMCP`
- `GetUserDefaultLangID`
- `GetUserDefaultLocaleName`
- `IsDBCSLeadByteEx`
- `LCMapStringEx`
- `CompareStringOrdinal`
- `MultiByteToWideChar`
- `WideCharToMultiByte`

## Diagnostics and Logging
- `OutputDebugStringW`
- `GetLocalTime`
- `GetLastError`
- `GetCurrentProcessId`
- `GetProcessId`
- `GetProcessTimes`

## System Metrics
- `GetSystemMetrics`
- `GetKeyboardLayoutNameW`

## Win32 Windowing
- `RegisterClassExW`
- `CreateWindowExW`
- `DestroyWindow`
- `ShowWindow`
- `UpdateWindow`
- `GetMessageW`
- `TranslateMessage`
- `DispatchMessageW`
- `DefWindowProcW`
- `SetWindowLongPtrW`
- `GetWindowLongPtrW`
- `PostMessageW`
- `PostQuitMessage`
- `BeginPaint`
- `EndPaint`
- `InvalidateRect`
- `GetClientRect`
- `GetDpiForWindow`

## Direct2D
- `D2D1CreateFactory`
- `ID2D1Factory::CreateHwndRenderTarget`
- `ID2D1HwndRenderTarget::Resize`
- `ID2D1RenderTarget::BeginDraw`
- `ID2D1RenderTarget::EndDraw`
- `ID2D1RenderTarget::Clear`
- `ID2D1RenderTarget::DrawTextW`
- `ID2D1RenderTarget::CreateSolidColorBrush`

## Console Output
- `GetConsoleMode`
- `WriteConsoleW`
- `WriteFile`
- `ReadConsoleInputW`
- `GetNumberOfConsoleInputEvents`
- `GetConsoleScreenBufferInfo`
- `SetConsoleMode`
- `GetConsoleCP`
- `SetConsoleCP`
- `GetConsoleOutputCP`
- `SetConsoleOutputCP`

## Notes
- APIs are chosen to remain compatible with Windows 10/11.
- `W` variants are used consistently for UTF-16/wchar_t handling.
- No C++ filesystem APIs are used.

## ConPTY APIs
- `CreatePseudoConsole`
- `ResizePseudoConsole`
- `ClosePseudoConsole`
