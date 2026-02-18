#include "renderer/window_host.hpp"

#include "renderer/console_attributes.hpp"
#include "renderer/dwrite_text_measurer.hpp"

#include <d2d1.h>
#include <d2d1helper.h>
#include <dwrite.h>
#include <winrt/base.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cwchar>
#include <utility>

// Minimal classic-window host (non-WinUI).
//
// This window host exists for the "classic" interactive server-handle startup
// path where `openconsole_new` renders the screen buffer itself instead of
// delegating to an external terminal. It is intentionally small:
// - snapshot-based rendering (`PublishedScreenBuffer` -> paint thread),
// - basic text output (currently monochrome),
// - no selection/scrollbars/IME/accessibility parity yet.
//
// See `new/docs/design/renderer_window_host.md` for current scope and planned
// follow-ups.

namespace oc::renderer
{
    struct WindowHost::DeviceResources final
    {
        winrt::com_ptr<ID2D1Factory> d2d_factory;
        winrt::com_ptr<ID2D1HwndRenderTarget> render_target;
        winrt::com_ptr<ID2D1SolidColorBrush> text_brush;
        winrt::com_ptr<ID2D1SolidColorBrush> background_brush;

        winrt::com_ptr<IDWriteFactory> dwrite_factory;
        winrt::com_ptr<IDWriteTextFormat> text_format;

        std::unique_ptr<TextMeasurer> text_measurer;
        std::wstring requested_family;
        std::wstring resolved_family;
        float measured_points{};
        UINT measured_dpi{};
        CellMetrics cell_metrics{};
        bool has_metrics{ false };
    };

    namespace
    {
        constexpr wchar_t k_window_class_name[] = L"OpenConsoleNewWindowHost";
        constexpr UINT k_msg_invalidate = WM_APP + 1;

        [[nodiscard]] ATOM register_window_class() noexcept
        {
            WNDCLASSEXW wc{};
            wc.cbSize = sizeof(wc);
            wc.style = CS_HREDRAW | CS_VREDRAW;
            wc.lpfnWndProc = &WindowHost::window_proc;
            wc.hInstance = ::GetModuleHandleW(nullptr);
            wc.hCursor = ::LoadCursorW(nullptr, IDC_ARROW);
            wc.lpszClassName = k_window_class_name;

            return ::RegisterClassExW(&wc);
        }
    }

    WindowHost::WindowHost(WindowHostConfig config, const core::HandleView stop_event) noexcept :
        _config(std::move(config)),
        _stop_event(stop_event)
    {
    }

    WindowHost::~WindowHost() noexcept
    {
        discard_device_resources();
        if (_hwnd != nullptr)
        {
            ::DestroyWindow(_hwnd);
            _hwnd = nullptr;
        }
    }

    std::expected<std::unique_ptr<WindowHost>, core::Win32Error> WindowHost::create(
        WindowHostConfig config,
        const core::HandleView stop_event) noexcept
    {
        if (config.title.empty())
        {
            config.title = L"openconsole_new";
        }

        try
        {
            auto host = std::unique_ptr<WindowHost>(new WindowHost(std::move(config), stop_event));
            if (!host->create_window())
            {
                return std::unexpected(core::from_dword(::GetLastError()));
            }
            return host;
        }
        catch (...)
        {
            return std::unexpected(core::from_dword(ERROR_OUTOFMEMORY));
        }
    }

    bool WindowHost::create_window() noexcept
    {
        static ATOM atom = 0;
        if (atom == 0)
        {
            atom = register_window_class();
            if (atom == 0 && ::GetLastError() == ERROR_CLASS_ALREADY_EXISTS)
            {
                // We use the class name string in `CreateWindowExW`. The atom
                // value is only a guard for "registration attempted".
                atom = 1;
            }
        }

        if (atom == 0)
        {
            return false;
        }

        const int width = std::max(1, _config.initial_width_px);
        const int height = std::max(1, _config.initial_height_px);

        _hwnd = ::CreateWindowExW(
            0,
            k_window_class_name,
            _config.title.c_str(),
            WS_OVERLAPPEDWINDOW,
            CW_USEDEFAULT,
            CW_USEDEFAULT,
            width,
            height,
            nullptr,
            nullptr,
            ::GetModuleHandleW(nullptr),
            this);
        if (_hwnd == nullptr)
        {
            return false;
        }

        ::ShowWindow(_hwnd, _config.show_command);
        ::UpdateWindow(_hwnd);
        return true;
    }

    int WindowHost::run() noexcept
    {
        if (_hwnd == nullptr)
        {
            return static_cast<int>(ERROR_INVALID_WINDOW_HANDLE);
        }

        MSG msg{};
        while (true)
        {
            const BOOL result = ::GetMessageW(&msg, nullptr, 0, 0);
            if (result == 0)
            {
                break;
            }
            if (result == -1)
            {
                return static_cast<int>(::GetLastError());
            }

            ::TranslateMessage(&msg);
            ::DispatchMessageW(&msg);
        }

        return static_cast<int>(msg.wParam);
    }

    void WindowHost::request_close() noexcept
    {
        if (_hwnd != nullptr)
        {
            (void)::PostMessageW(_hwnd, WM_CLOSE, 0, 0);
        }
    }

    HWND WindowHost::hwnd() const noexcept
    {
        return _hwnd;
    }

    LRESULT CALLBACK WindowHost::window_proc(const HWND hwnd, const UINT msg, const WPARAM wparam, const LPARAM lparam) noexcept
    {
        if (msg == WM_NCCREATE)
        {
            const auto* create = reinterpret_cast<const CREATESTRUCTW*>(lparam);
            auto* self = static_cast<WindowHost*>(create->lpCreateParams);
            ::SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
            self->_hwnd = hwnd;
        }

        auto* self = reinterpret_cast<WindowHost*>(::GetWindowLongPtrW(hwnd, GWLP_USERDATA));
        if (self != nullptr)
        {
            return self->handle_message(msg, wparam, lparam);
        }

        return ::DefWindowProcW(hwnd, msg, wparam, lparam);
    }

    LRESULT WindowHost::handle_message(const UINT msg, const WPARAM wparam, const LPARAM lparam) noexcept
    {
        switch (msg)
        {
        case k_msg_invalidate:
            if (_hwnd != nullptr)
            {
                ::InvalidateRect(_hwnd, nullptr, FALSE);
            }
            return 0;
        case WM_SIZE:
            handle_resize(static_cast<int>(LOWORD(lparam)), static_cast<int>(HIWORD(lparam)));
            return 0;
        case WM_PAINT:
            handle_paint();
            return 0;
        case WM_CLOSE:
            ::DestroyWindow(_hwnd);
            return 0;
        case WM_DESTROY:
            if (_stop_event)
            {
                (void)::SetEvent(_stop_event.get());
            }
            _hwnd = nullptr;
            ::PostQuitMessage(0);
            return 0;
        default:
            break;
        }

        return ::DefWindowProcW(_hwnd, msg, wparam, lparam);
    }

    void WindowHost::handle_resize(const int width, const int height) noexcept
    {
        if (!_resources || !_resources->render_target)
        {
            return;
        }

        const UINT w = width > 0 ? static_cast<UINT>(width) : 0u;
        const UINT h = height > 0 ? static_cast<UINT>(height) : 0u;
        (void)_resources->render_target->Resize(D2D1::SizeU(w, h));
    }

    void WindowHost::handle_paint() noexcept
    {
        PAINTSTRUCT ps{};
        (void)::BeginPaint(_hwnd, &ps);

        const auto snapshot = _config.published_screen ? _config.published_screen->latest() : nullptr;

        ensure_device_resources();
        if (_resources && _resources->render_target && _resources->text_brush && _resources->background_brush && _resources->dwrite_factory)
        {
            RECT client{};
            ::GetClientRect(_hwnd, &client);
            const float width = static_cast<float>(client.right - client.left);
            const float height = static_cast<float>(client.bottom - client.top);

            UINT dpi = 96;
            if (_hwnd != nullptr)
            {
                dpi = ::GetDpiForWindow(_hwnd);
                if (dpi == 0)
                {
                    dpi = 96;
                }
            }

            // Draw in pixel space (DIPs == pixels) and scale text explicitly based on the
            // per-window DPI. This keeps the math consistent with the screen-buffer model,
            // which is expressed in character cells and later mapped to pixels.
            _resources->render_target->SetDpi(96.0f, 96.0f);

            // Lazily create the non-GUI measurer. If it fails, keep the placeholder text.
            if (!_resources->text_measurer)
            {
                if (auto created = DwriteTextMeasurer::create(); created)
                {
                    _resources->text_measurer = std::move(created.value());
                }
            }

            if (_resources->text_measurer)
            {
                const bool needs_metrics =
                    !_resources->has_metrics ||
                    _resources->measured_dpi != dpi ||
                    _resources->measured_points != _config.font_points ||
                    _resources->requested_family != _config.font_family;

                if (needs_metrics)
                {
                    FontRequest request{};
                    request.family_name = _config.font_family;
                    request.size_points = _config.font_points;
                    request.dpi = static_cast<float>(dpi);

                    if (auto measured = _resources->text_measurer->measure_font(request); measured)
                    {
                        _resources->cell_metrics = measured->cell;
                        _resources->requested_family = _config.font_family;
                        _resources->resolved_family = measured->resolved_family_name;
                        _resources->measured_points = request.size_points;
                        _resources->measured_dpi = dpi;
                        _resources->has_metrics = true;

                        const float font_size_px = request.size_points / 72.0f * static_cast<float>(dpi);
                        winrt::com_ptr<IDWriteTextFormat> format;
                        const HRESULT hr = _resources->dwrite_factory->CreateTextFormat(
                            measured->resolved_family_name.c_str(),
                            nullptr,
                            DWRITE_FONT_WEIGHT_NORMAL,
                            DWRITE_FONT_STYLE_NORMAL,
                            DWRITE_FONT_STRETCH_NORMAL,
                            font_size_px,
                            L"",
                            format.put());
                        if (SUCCEEDED(hr))
                        {
                            (void)format->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
                            _resources->text_format = std::move(format);
                        }
                    }
                }
            }

            // Ensure we always have a text format for the placeholder path, even if font
            // measurement fails (for example if DirectWrite cannot resolve a family name).
            if (!_resources->text_format)
            {
                const float requested_points = _config.font_points > 0.0f ? _config.font_points : 14.0f;
                const float font_size_px = requested_points / 72.0f * static_cast<float>(dpi);

                const auto try_create_format = [&](const wchar_t* const family) noexcept -> winrt::com_ptr<IDWriteTextFormat> {
                    winrt::com_ptr<IDWriteTextFormat> format;
                    if (!family || family[0] == L'\0')
                    {
                        return format;
                    }

                    const HRESULT hr = _resources->dwrite_factory->CreateTextFormat(
                        family,
                        nullptr,
                        DWRITE_FONT_WEIGHT_NORMAL,
                        DWRITE_FONT_STYLE_NORMAL,
                        DWRITE_FONT_STRETCH_NORMAL,
                        font_size_px,
                        L"",
                        format.put());
                    if (FAILED(hr))
                    {
                        return {};
                    }

                    (void)format->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
                    return format;
                };

                if (auto format = try_create_format(_config.font_family.c_str()); format)
                {
                    _resources->text_format = std::move(format);
                }
                else if (auto fallback = try_create_format(L"Consolas"); fallback)
                {
                    _resources->text_format = std::move(fallback);
                }
            }

            _resources->render_target->BeginDraw();

            const auto to_d2d = [](const COLORREF color) noexcept {
                constexpr float inv = 1.0f / 255.0f;
                return D2D1::ColorF(
                    static_cast<float>(GetRValue(color)) * inv,
                    static_cast<float>(GetGValue(color)) * inv,
                    static_cast<float>(GetBValue(color)) * inv);
            };

            const COLORREF clear_bg_ref = snapshot
                ? snapshot->color_table[decode_attributes(snapshot->default_attributes).background_index]
                : RGB(0, 0, 0);

            _resources->render_target->Clear(to_d2d(clear_bg_ref));

            bool drew_snapshot = false;
            if (snapshot && _resources->text_format && _resources->has_metrics)
            {
                const int viewport_w = snapshot->viewport_size.X > 0 ? snapshot->viewport_size.X : 0;
                const int viewport_h = snapshot->viewport_size.Y > 0 ? snapshot->viewport_size.Y : 0;

                const int cell_w = std::max(1, _resources->cell_metrics.width_px);
                const int cell_h = std::max(1, _resources->cell_metrics.height_px);

                const float margin_x = 0.0f;
                const float margin_y = 0.0f;
                const float row_height = static_cast<float>(cell_h);

                for (int row = 0; row < viewport_h; ++row)
                {
                    const size_t offset = static_cast<size_t>(row) * static_cast<size_t>(viewport_w);
                    if (offset + static_cast<size_t>(viewport_w) > snapshot->text.size())
                    {
                        break;
                    }

                    const wchar_t* row_ptr = snapshot->text.data() + offset;
                    const USHORT* attr_ptr = nullptr;
                    if (offset + static_cast<size_t>(viewport_w) <= snapshot->attributes.size())
                    {
                        attr_ptr = snapshot->attributes.data() + offset;
                    }
                    const float top = margin_y + static_cast<float>(row) * row_height;

                    int col = 0;
                    while (col < viewport_w)
                    {
                        const USHORT attributes = attr_ptr ? attr_ptr[col] : snapshot->default_attributes;
                        int run_start = col;
                        for (; col < viewport_w; ++col)
                        {
                            const USHORT current = attr_ptr ? attr_ptr[col] : snapshot->default_attributes;
                            if (current != attributes)
                            {
                                break;
                            }
                        }

                        const int run_len = col - run_start;
                        if (run_len <= 0)
                        {
                            continue;
                        }

                        const auto decoded = decode_attributes(attributes);
                        const COLORREF fg_ref = snapshot->color_table[decoded.foreground_index];
                        const COLORREF bg_ref = snapshot->color_table[decoded.background_index];

                        const float left = margin_x + static_cast<float>(run_start * cell_w);
                        const float right = left + static_cast<float>(run_len * cell_w);
                        const float bottom = top + row_height;

                        if (bg_ref != clear_bg_ref)
                        {
                            _resources->background_brush->SetColor(to_d2d(bg_ref));
                            _resources->render_target->FillRectangle(
                                D2D1::RectF(left, top, right, bottom),
                                _resources->background_brush.get());
                        }

                        bool has_text = false;
                        for (int i = 0; i < run_len; ++i)
                        {
                            if (row_ptr[run_start + i] != L' ')
                            {
                                has_text = true;
                                break;
                            }
                        }

                        if (has_text)
                        {
                            _resources->text_brush->SetColor(to_d2d(fg_ref));
                            const D2D1_RECT_F layout{
                                left,
                                top,
                                std::max(left, width),
                                std::max(bottom, top),
                            };

                            _resources->render_target->DrawTextW(
                                row_ptr + run_start,
                                static_cast<UINT32>(run_len),
                                _resources->text_format.get(),
                                layout,
                                _resources->text_brush.get());
                        }

                        if (decoded.underline)
                        {
                            const int underline_thickness = std::max(1, _resources->cell_metrics.underline_thickness_px);
                            const float underline_top = top + static_cast<float>(_resources->cell_metrics.underline_position_px);
                            const float underline_bottom = std::min(bottom, underline_top + static_cast<float>(underline_thickness));
                            if (underline_bottom > underline_top)
                            {
                                _resources->background_brush->SetColor(to_d2d(fg_ref));
                                _resources->render_target->FillRectangle(
                                    D2D1::RectF(left, underline_top, right, underline_bottom),
                                    _resources->background_brush.get());
                            }
                        }
                    }
                }

                if (snapshot->cursor_visible && viewport_w > 0 && viewport_h > 0 && !snapshot->text.empty())
                {
                    const int cursor_x = static_cast<int>(snapshot->cursor_position.X) - static_cast<int>(snapshot->window_rect.Left);
                    const int cursor_y = static_cast<int>(snapshot->cursor_position.Y) - static_cast<int>(snapshot->window_rect.Top);
                    if (cursor_x >= 0 && cursor_x < viewport_w && cursor_y >= 0 && cursor_y < viewport_h)
                    {
                        const size_t index = static_cast<size_t>(cursor_y) * static_cast<size_t>(viewport_w) + static_cast<size_t>(cursor_x);
                        const USHORT cursor_attr = index < snapshot->attributes.size() ? snapshot->attributes[index] : snapshot->default_attributes;
                        const auto decoded = decode_attributes(cursor_attr);
                        const COLORREF fg_ref = snapshot->color_table[decoded.foreground_index];

                        unsigned cursor_size = snapshot->cursor_size;
                        cursor_size = std::clamp(cursor_size, 1u, 100u);

                        const float cell_left = margin_x + static_cast<float>(cursor_x * cell_w);
                        const float cell_top = margin_y + static_cast<float>(cursor_y * cell_h);
                        const float cell_right = cell_left + static_cast<float>(cell_w);
                        const float cell_bottom = cell_top + static_cast<float>(cell_h);

                        const float cursor_height = static_cast<float>(cell_h) * (static_cast<float>(cursor_size) / 100.0f);
                        const float cursor_top = std::max(cell_top, cell_bottom - cursor_height);

                        _resources->background_brush->SetColor(to_d2d(fg_ref));
                        _resources->render_target->FillRectangle(
                            D2D1::RectF(cell_left, cursor_top, cell_right, cell_bottom),
                            _resources->background_brush.get());

                        // If the cursor is a full block, redraw the glyph with inverted colors for visibility.
                        if (cursor_height >= static_cast<float>(cell_h - 1))
                        {
                            const COLORREF bg_ref = snapshot->color_table[decoded.background_index];
                            wchar_t ch = index < snapshot->text.size() ? snapshot->text[index] : L' ';
                            if (ch == L'\0')
                            {
                                ch = L' ';
                            }

                            _resources->text_brush->SetColor(to_d2d(bg_ref));
                            const D2D1_RECT_F layout{
                                cell_left,
                                cell_top,
                                std::max(cell_left, width),
                                std::max(cell_bottom, cell_top),
                            };

                            _resources->render_target->DrawTextW(
                                &ch,
                                1,
                                _resources->text_format.get(),
                                layout,
                                _resources->text_brush.get());
                        }
                    }
                }

                drew_snapshot = true;
            }

            if (!drew_snapshot && _resources->text_format)
            {
                constexpr wchar_t message[] =
                    L"openconsole_new\n"
                    L"Waiting for console output...";

                const D2D1_RECT_F layout{
                    8.0f,
                    8.0f,
                    std::max(8.0f, width - 8.0f),
                    std::max(8.0f, height - 8.0f),
                };

                _resources->render_target->DrawTextW(
                    message,
                    static_cast<UINT32>(std::size(message) - 1),
                    _resources->text_format.get(),
                    layout,
                    _resources->text_brush.get());
            }

            const HRESULT hr = _resources->render_target->EndDraw();
            if (hr == D2DERR_RECREATE_TARGET)
            {
                discard_device_resources();
            }
        }

        ::EndPaint(_hwnd, &ps);
    }

    void WindowHost::ensure_device_resources() noexcept
    {
        if (_resources && _resources->render_target)
        {
            return;
        }

        if (!_resources)
        {
            try
            {
                _resources = std::make_unique<DeviceResources>();
            }
            catch (...)
            {
                return;
            }
        }

        if (!_resources->d2d_factory)
        {
            winrt::com_ptr<ID2D1Factory> factory;
            const HRESULT hr = ::D2D1CreateFactory(
                D2D1_FACTORY_TYPE_SINGLE_THREADED,
                __uuidof(ID2D1Factory),
                nullptr,
                reinterpret_cast<void**>(factory.put()));
            if (FAILED(hr))
            {
                return;
            }
            _resources->d2d_factory = std::move(factory);
        }

        if (!_resources->dwrite_factory)
        {
            winrt::com_ptr<IDWriteFactory> factory;
            const HRESULT hr = ::DWriteCreateFactory(
                DWRITE_FACTORY_TYPE_SHARED,
                __uuidof(IDWriteFactory),
                reinterpret_cast<IUnknown**>(factory.put()));
            if (FAILED(hr))
            {
                return;
            }
            _resources->dwrite_factory = std::move(factory);
        }

        if (!_resources->render_target)
        {
            RECT client{};
            ::GetClientRect(_hwnd, &client);
            const UINT width = static_cast<UINT>(std::max(1l, client.right - client.left));
            const UINT height = static_cast<UINT>(std::max(1l, client.bottom - client.top));

            const D2D1_RENDER_TARGET_PROPERTIES props = D2D1::RenderTargetProperties();
            const D2D1_HWND_RENDER_TARGET_PROPERTIES hwnd_props =
                D2D1::HwndRenderTargetProperties(_hwnd, D2D1::SizeU(width, height));

            winrt::com_ptr<ID2D1HwndRenderTarget> target;
            const HRESULT hr = _resources->d2d_factory->CreateHwndRenderTarget(
                props,
                hwnd_props,
                target.put());
            if (FAILED(hr))
            {
                return;
            }

            _resources->render_target = std::move(target);
        }

        if (!_resources->text_brush)
        {
            winrt::com_ptr<ID2D1SolidColorBrush> brush;
            const HRESULT hr = _resources->render_target->CreateSolidColorBrush(
                D2D1::ColorF(D2D1::ColorF::White),
                brush.put());
            if (FAILED(hr))
            {
                return;
            }
            _resources->text_brush = std::move(brush);
        }

        if (!_resources->background_brush)
        {
            winrt::com_ptr<ID2D1SolidColorBrush> brush;
            const HRESULT hr = _resources->render_target->CreateSolidColorBrush(
                D2D1::ColorF(D2D1::ColorF::Black),
                brush.put());
            if (FAILED(hr))
            {
                return;
            }
            _resources->background_brush = std::move(brush);
        }
    }

    void WindowHost::discard_device_resources() noexcept
    {
        if (_resources)
        {
            _resources->text_brush = nullptr;
            _resources->background_brush = nullptr;
            _resources->render_target = nullptr;
        }
    }
}
