#pragma once

// Thread-safe publication of `ScreenBuffer` viewport snapshots for a classic window renderer.
//
// The ConDrv server thread mutates an in-memory `ScreenBuffer`. A classic (non-headless) host
// needs to render that buffer on the UI thread without sharing mutable state across threads.
// This module provides:
// - An immutable snapshot type that contains the visible viewport data.
// - A lock-free publication container (`PublishedScreenBuffer`) that always exposes "latest".

#include "condrv/condrv_device_comm.hpp"

#include <Windows.h>

#include <atomic>
#include <array>
#include <cstdint>
#include <expected>
#include <memory>
#include <vector>

namespace oc::condrv
{
    class ScreenBuffer;

    struct ScreenBufferSnapshot final
    {
        uint64_t revision{};
        SMALL_RECT window_rect{}; // inclusive coordinates within the buffer
        COORD buffer_size{};
        COORD cursor_position{};
        bool cursor_visible{ true };
        ULONG cursor_size{ 25 };
        USHORT default_attributes{ 0x07 };
        std::array<COLORREF, 16> color_table{};

        // Derived from `window_rect`. `X`/`Y` are the viewport width/height.
        COORD viewport_size{};

        // Row-major viewport contents: row 0..H-1, col 0..W-1.
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
        // `std::atomic<std::shared_ptr<T>>` provides lock-free publication semantics for a single
        // pointer-like value. The snapshots themselves are immutable once published.
        std::atomic<std::shared_ptr<const ScreenBufferSnapshot>> _latest{};
    };

    [[nodiscard]] std::expected<std::shared_ptr<const ScreenBufferSnapshot>, DeviceCommError> make_viewport_snapshot(
        const ScreenBuffer& buffer) noexcept;
}
