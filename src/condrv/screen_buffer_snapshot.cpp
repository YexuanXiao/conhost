#include "condrv/screen_buffer_snapshot.hpp"

#include "condrv/condrv_server.hpp"

#include <algorithm>
#include <cstddef>
#include <span>
#include <limits>

namespace oc::condrv
{
    namespace
    {
        [[nodiscard]] bool rect_dimensions(const SMALL_RECT rect, size_t& out_width, size_t& out_height) noexcept
        {
            const long width_long = static_cast<long>(rect.Right) - static_cast<long>(rect.Left) + 1;
            const long height_long = static_cast<long>(rect.Bottom) - static_cast<long>(rect.Top) + 1;
            if (width_long <= 0 || height_long <= 0)
            {
                out_width = 0;
                out_height = 0;
                return true;
            }

            const size_t width = static_cast<size_t>(width_long);
            const size_t height = static_cast<size_t>(height_long);
            if (height != 0 && width > (std::numeric_limits<size_t>::max() / height))
            {
                return false;
            }

            out_width = width;
            out_height = height;
            return true;
        }

        [[nodiscard]] constexpr COORD to_coord_saturating(const size_t width, const size_t height) noexcept
        {
            COORD result{};
            const auto max_short = static_cast<size_t>(std::numeric_limits<SHORT>::max());
            result.X = static_cast<SHORT>(std::min(width, max_short));
            result.Y = static_cast<SHORT>(std::min(height, max_short));
            return result;
        }
    }

    std::expected<std::shared_ptr<const view::ScreenBufferSnapshot>, DeviceCommError> make_viewport_snapshot(
        const ScreenBuffer& buffer) noexcept
    try
    {
        auto snapshot = std::make_shared<view::ScreenBufferSnapshot>();
        snapshot->revision = buffer.revision();
        snapshot->window_rect = buffer.window_rect();
        snapshot->buffer_size = buffer.screen_buffer_size();
        snapshot->cursor_position = buffer.cursor_position();
        snapshot->cursor_visible = buffer.cursor_visible();
        snapshot->cursor_size = buffer.cursor_size();
        snapshot->default_attributes = buffer.default_text_attributes();
        snapshot->color_table = buffer.color_table();

        size_t viewport_w = 0;
        size_t viewport_h = 0;
        if (!rect_dimensions(snapshot->window_rect, viewport_w, viewport_h))
        {
            return std::unexpected(DeviceCommError{
                .context = L"Viewport dimensions overflow",
                .win32_error = ERROR_ARITHMETIC_OVERFLOW,
            });
        }

        snapshot->viewport_size = to_coord_saturating(viewport_w, viewport_h);

        const size_t cell_count = viewport_w * viewport_h;
        snapshot->text.assign(cell_count, L' ');
        snapshot->attributes.assign(cell_count, snapshot->default_attributes);

        if (viewport_w != 0 && viewport_h != 0)
        {
            for (size_t row = 0; row < viewport_h; ++row)
            {
                const SHORT y = static_cast<SHORT>(static_cast<long>(snapshot->window_rect.Top) + static_cast<long>(row));
                const COORD origin{ snapshot->window_rect.Left, y };

                const size_t offset = row * viewport_w;
                auto row_text = std::span<wchar_t>(snapshot->text).subspan(offset, viewport_w);
                auto row_attr = std::span<USHORT>(snapshot->attributes).subspan(offset, viewport_w);

                const size_t read_text = buffer.read_output_characters(origin, row_text);
                const size_t read_attr = buffer.read_output_attributes(origin, row_attr);

                if (read_text < row_text.size())
                {
                    std::fill(row_text.begin() + static_cast<ptrdiff_t>(read_text), row_text.end(), L' ');
                }
                if (read_attr < row_attr.size())
                {
                    std::fill(
                        row_attr.begin() + static_cast<ptrdiff_t>(read_attr),
                        row_attr.end(),
                        snapshot->default_attributes);
                }
            }
        }

        return std::shared_ptr<const view::ScreenBufferSnapshot>(std::move(snapshot));
    }
    catch (...)
    {
        return std::unexpected(DeviceCommError{
            .context = L"Failed to allocate ScreenBuffer snapshot",
            .win32_error = ERROR_OUTOFMEMORY,
        });
    }
}
