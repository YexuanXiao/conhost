#pragma once

// Minimal Win32 window host for the classic (non-headless) console.
//
// This is intentionally a small, self-contained message pump + paint loop.
// It does not attempt to replicate the full upstream conhost window behavior
// (scroll bars, selection, IME, accessibility, etc.). Those are follow-up
// microtasks.

#include "core/exception.hpp"
#include "core/handle_view.hpp"
#include "condrv/screen_buffer_snapshot.hpp"

#include <Windows.h>

#include <expected>
#include <memory>
#include <string>

namespace oc::renderer
{
    struct WindowHostConfig final
    {
        std::wstring title;
        int initial_width_px{ 800 };
        int initial_height_px{ 600 };
        int show_command{ SW_SHOWDEFAULT };

        // Optional output source for windowed `--server` mode.
        std::shared_ptr<condrv::PublishedScreenBuffer> published_screen;

        // Rendering defaults (monochrome snapshot rendering uses these knobs).
        std::wstring font_family{ L"Consolas" };
        float font_points{ 14.0f };
    };

    class WindowHost final
    {
    public:
        [[nodiscard]] static std::expected<std::unique_ptr<WindowHost>, core::Win32Error> create(
            WindowHostConfig config,
            core::HandleView stop_event) noexcept;

        WindowHost(const WindowHost&) = delete;
        WindowHost& operator=(const WindowHost&) = delete;
        WindowHost(WindowHost&&) = delete;
        WindowHost& operator=(WindowHost&&) = delete;

        ~WindowHost() noexcept;

        // Runs the message pump until the window is closed.
        [[nodiscard]] int run() noexcept;

        // Requests the window to close asynchronously.
        void request_close() noexcept;

        [[nodiscard]] HWND hwnd() const noexcept;

        // Window procedure used for the registered window class. This is an
        // implementation detail, but it must be a plain function pointer
        // target (`WNDPROC`).
        [[nodiscard]] static LRESULT CALLBACK window_proc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam) noexcept;

    private:
        WindowHost(WindowHostConfig config, core::HandleView stop_event) noexcept;

        [[nodiscard]] bool create_window() noexcept;

        [[nodiscard]] LRESULT handle_message(UINT msg, WPARAM wparam, LPARAM lparam) noexcept;

        void handle_paint() noexcept;
        void handle_resize(int width, int height) noexcept;

        void ensure_device_resources() noexcept;
        void discard_device_resources() noexcept;

        WindowHostConfig _config;
        core::HandleView _stop_event{};
        HWND _hwnd{};

        struct DeviceResources;
        std::unique_ptr<DeviceResources> _resources;
    };
}
