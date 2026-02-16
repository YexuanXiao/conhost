#include "condrv/screen_buffer_snapshot.hpp"

#include "condrv/condrv_server.hpp"

namespace
{
    [[nodiscard]] std::shared_ptr<oc::condrv::ScreenBuffer> make_buffer(const COORD size)
    {
        auto settings = oc::condrv::ScreenBuffer::default_settings();
        settings.buffer_size = size;
        settings.window_size = size;
        settings.maximum_window_size = size;

        auto created = oc::condrv::ScreenBuffer::create(settings);
        if (!created)
        {
            return {};
        }

        return std::move(created.value());
    }

    bool test_viewport_snapshot_reads_correct_subrect()
    {
        auto buffer = make_buffer(COORD{ 10, 5 });
        if (!buffer)
        {
            return false;
        }

        for (SHORT y = 0; y < 5; ++y)
        {
            for (SHORT x = 0; x < 10; ++x)
            {
                const wchar_t ch = static_cast<wchar_t>(L'!' + (y * 10 + x));
                if (!buffer->write_cell(COORD{ x, y }, ch, 0x07))
                {
                    return false;
                }
            }
        }

        const SMALL_RECT rect{ 2, 1, 6, 3 };
        if (!buffer->set_window_rect(rect))
        {
            return false;
        }

        auto snapshot = oc::condrv::make_viewport_snapshot(*buffer);
        if (!snapshot)
        {
            return false;
        }

        const auto snap = std::move(snapshot.value());
        if (snap->window_rect.Left != rect.Left ||
            snap->window_rect.Top != rect.Top ||
            snap->window_rect.Right != rect.Right ||
            snap->window_rect.Bottom != rect.Bottom)
        {
            return false;
        }

        if (snap->viewport_size.X != 5 || snap->viewport_size.Y != 3)
        {
            return false;
        }

        if (snap->text.size() != 15)
        {
            return false;
        }

        for (int row = 0; row < 3; ++row)
        {
            for (int col = 0; col < 5; ++col)
            {
                const SHORT y = static_cast<SHORT>(rect.Top + row);
                const SHORT x = static_cast<SHORT>(rect.Left + col);
                const wchar_t expected = static_cast<wchar_t>(L'!' + (y * 10 + x));
                const size_t index = static_cast<size_t>(row) * 5u + static_cast<size_t>(col);
                if (snap->text[index] != expected)
                {
                    return false;
                }
            }
        }

        return true;
    }

    bool test_snapshot_includes_attributes_and_color_table()
    {
        auto buffer = make_buffer(COORD{ 10, 5 });
        if (!buffer)
        {
            return false;
        }

        COLORREF table[16]{};
        for (int i = 0; i < 16; ++i)
        {
            table[i] = RGB(i * 10, i * 10, i * 10);
        }
        buffer->set_color_table(table);

        const SMALL_RECT rect{ 2, 1, 6, 3 };
        if (!buffer->set_window_rect(rect))
        {
            return false;
        }

        for (SHORT y = rect.Top; y <= rect.Bottom; ++y)
        {
            for (SHORT x = rect.Left; x <= rect.Right; ++x)
            {
                if (!buffer->write_cell(COORD{ x, y }, L'X', 0x1E))
                {
                    return false;
                }
            }
        }

        auto snapshot = oc::condrv::make_viewport_snapshot(*buffer);
        if (!snapshot)
        {
            return false;
        }

        const auto snap = std::move(snapshot.value());
        for (int i = 0; i < 16; ++i)
        {
            if (snap->color_table[static_cast<size_t>(i)] != table[i])
            {
                return false;
            }
        }

        if (snap->attributes.size() != snap->text.size() || snap->attributes.size() != 15)
        {
            return false;
        }

        for (const auto attr : snap->attributes)
        {
            if (attr != 0x1E)
            {
                return false;
            }
        }

        return true;
    }

    bool test_revision_increments_on_mutation()
    {
        auto buffer = make_buffer(COORD{ 10, 5 });
        if (!buffer)
        {
            return false;
        }

        const auto rev0 = buffer->revision();
        (void)buffer->set_cursor_position(COORD{ 1, 1 });
        const auto rev1 = buffer->revision();
        if (rev1 <= rev0)
        {
            return false;
        }

        (void)buffer->write_cell(COORD{ 0, 0 }, L'Z', 0x07);
        const auto rev2 = buffer->revision();
        return rev2 > rev1;
    }
}

bool run_condrv_screen_buffer_snapshot_tests()
{
    return test_viewport_snapshot_reads_correct_subrect() &&
           test_snapshot_includes_attributes_and_color_table() &&
           test_revision_increments_on_mutation();
}

