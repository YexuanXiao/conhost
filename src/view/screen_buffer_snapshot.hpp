#pragma once

// Immutable viewport snapshot types shared between the non-GUI console model and the GUI renderer.
//
// Design goal:
// - The GUI layer should not depend on the ConDrv server implementation details.
// - The ConDrv server should not depend on the renderer implementation details.
//
// This header defines a stable, "view-model" snapshot that can be produced by the ConDrv server thread
// and consumed by the UI thread without sharing mutable state.
//
// The snapshot intentionally contains only *viewport* data (plus the small amount of global state needed
// to render it: palette, default attributes, cursor state). Rendering the full backing buffer would be
// unbounded and unnecessary for a classic window.

#include <Windows.h>

#include <array>
#include <atomic>
#include <cstdint>
#include <memory>
#include <vector>

namespace oc::view
{
    struct ScreenBufferSnapshot final
    {
        uint64_t revision{};

        // Viewport geometry in buffer coordinates.
        // `window_rect` uses inclusive coordinates (conhost/CONSOLE_SCREEN_BUFFER_INFO style).
        SMALL_RECT window_rect{};
        COORD buffer_size{};

        // Cursor state in buffer coordinates.
        COORD cursor_position{};
        bool cursor_visible{ true };
        ULONG cursor_size{ 25 }; // percent of the cell height (1..100)

        // Default text attributes and palette for legacy 16-color rendering.
        USHORT default_attributes{ 0x07 };
        std::array<COLORREF, 16> color_table{};

        // Derived from `window_rect`. `X`/`Y` are the viewport width/height.
        COORD viewport_size{};

        // Row-major viewport contents: row 0..H-1, col 0..W-1.
        // `text.size() == attributes.size() == viewport_size.X * viewport_size.Y`.
        std::vector<wchar_t> text;
        std::vector<USHORT> attributes;
    };

    class PublishedScreenBuffer final
    {
    public:
        PublishedScreenBuffer() noexcept = default;
        ~PublishedScreenBuffer() noexcept = default;

        PublishedScreenBuffer(const PublishedScreenBuffer&) = delete;
        PublishedScreenBuffer& operator=(const PublishedScreenBuffer&) = delete;
        PublishedScreenBuffer(PublishedScreenBuffer&&) = delete;
        PublishedScreenBuffer& operator=(PublishedScreenBuffer&&) = delete;

        void publish(std::shared_ptr<const ScreenBufferSnapshot> snapshot) noexcept
        {
            _latest.store(std::move(snapshot), std::memory_order_release);
        }

        [[nodiscard]] std::shared_ptr<const ScreenBufferSnapshot> latest() const noexcept
        {
            return _latest.load(std::memory_order_acquire);
        }

    private:
        // Lock-free "latest pointer" publication. Snapshots are immutable once published.
        std::atomic<std::shared_ptr<const ScreenBufferSnapshot>> _latest{};
    };
}

