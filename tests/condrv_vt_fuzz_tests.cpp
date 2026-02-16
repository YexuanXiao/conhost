#include "condrv/condrv_server.hpp"
#include "condrv/vt_input_decoder.hpp"

#include <Windows.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <string>
#include <string_view>
#include <vector>

namespace
{
    constexpr std::uint64_t k_base_seed = 0x4F434E45574F434FULL;
    constexpr std::uint64_t k_iteration_mix = 0x9E3779B97F4A7C15ULL;
    constexpr size_t k_default_iterations = 800;
    constexpr size_t k_max_iterations = 20'000;

    class SplitMix64 final
    {
    public:
        explicit SplitMix64(const std::uint64_t seed) noexcept :
            _state(seed)
        {
        }

        [[nodiscard]] std::uint64_t next_u64() noexcept
        {
            std::uint64_t z = (_state += 0x9E3779B97F4A7C15ULL);
            z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
            z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
            return z ^ (z >> 31);
        }

        [[nodiscard]] std::uint32_t next_u32() noexcept
        {
            return static_cast<std::uint32_t>(next_u64());
        }

        [[nodiscard]] std::byte next_byte() noexcept
        {
            return static_cast<std::byte>(next_u64() & 0xFFu);
        }

        [[nodiscard]] size_t next_size(const size_t max_inclusive) noexcept
        {
            if (max_inclusive == 0)
            {
                return 0;
            }

            const std::uint64_t bound = static_cast<std::uint64_t>(max_inclusive) + 1ULL;
            return static_cast<size_t>(next_u64() % bound);
        }

    private:
        std::uint64_t _state{};
    };

    [[nodiscard]] size_t read_iterations_from_env() noexcept
    {
        constexpr wchar_t name[] = L"OPENCONSOLE_NEW_TEST_FUZZ_ITERS";
        wchar_t buffer[64]{};
        const DWORD written = ::GetEnvironmentVariableW(name, buffer, static_cast<DWORD>(std::size(buffer)));
        if (written == 0 || written >= std::size(buffer))
        {
            return k_default_iterations;
        }

        size_t value = 0;
        for (DWORD i = 0; i < written; ++i)
        {
            const wchar_t ch = buffer[i];
            if (ch < L'0' || ch > L'9')
            {
                return k_default_iterations;
            }

            const size_t digit = static_cast<size_t>(ch - L'0');
            if (value > (k_max_iterations - digit) / 10)
            {
                value = k_max_iterations;
                break;
            }

            value = value * 10 + digit;
        }

        if (value < 1)
        {
            value = 1;
        }
        if (value > k_max_iterations)
        {
            value = k_max_iterations;
        }
        return value;
    }

    void dump_bytes(const std::span<const std::byte> bytes) noexcept
    {
        fwprintf(stderr, L"[DETAIL] prefix bytes (%zu):", bytes.size());
        const size_t limit = bytes.size() < 32 ? bytes.size() : 32;
        for (size_t i = 0; i < limit; ++i)
        {
            const unsigned value = static_cast<unsigned>(std::to_integer<unsigned char>(bytes[i]));
            fwprintf(stderr, L" %02X", value);
        }
        if (bytes.size() > limit)
        {
            fwprintf(stderr, L" ...");
        }
        fwprintf(stderr, L"\n");
    }

    void dump_wchars_low_byte(const std::wstring_view chunk) noexcept
    {
        fwprintf(stderr, L"[DETAIL] last chunk (%zu):", chunk.size());
        const size_t limit = chunk.size() < 32 ? chunk.size() : 32;
        for (size_t i = 0; i < limit; ++i)
        {
            const unsigned value = static_cast<unsigned>(chunk[i] & 0xFF);
            fwprintf(stderr, L" %02X", value);
        }
        if (chunk.size() > limit)
        {
            fwprintf(stderr, L" ...");
        }
        fwprintf(stderr, L"\n");
    }

    [[nodiscard]] bool cursor_in_range(const COORD cursor, const COORD size) noexcept
    {
        if (size.X <= 0 || size.Y <= 0)
        {
            return false;
        }

        const long x = cursor.X;
        const long y = cursor.Y;
        const long w = size.X;
        const long h = size.Y;
        return x >= 0 && y >= 0 && x < w && y < h;
    }

    [[nodiscard]] bool window_rect_in_range(const SMALL_RECT rect, const COORD size) noexcept
    {
        if (size.X <= 0 || size.Y <= 0)
        {
            return false;
        }

        if (rect.Left < 0 || rect.Top < 0)
        {
            return false;
        }
        if (rect.Right < rect.Left || rect.Bottom < rect.Top)
        {
            return false;
        }

        const long w = size.X;
        const long h = size.Y;

        return rect.Right < w && rect.Bottom < h;
    }

    [[nodiscard]] bool test_vt_input_decoder_fuzz_invariants()
    {
        constexpr std::array<unsigned char, 21> corpus{
            0x1B,
            0x9B,
            static_cast<unsigned char>('['),
            static_cast<unsigned char>('O'),
            static_cast<unsigned char>('?'),
            static_cast<unsigned char>(';'),
            static_cast<unsigned char>('_'),
            static_cast<unsigned char>('~'),
            static_cast<unsigned char>('I'),
            static_cast<unsigned char>('O'),
            static_cast<unsigned char>('c'),
            static_cast<unsigned char>('0'),
            static_cast<unsigned char>('1'),
            static_cast<unsigned char>('2'),
            static_cast<unsigned char>('3'),
            static_cast<unsigned char>('4'),
            static_cast<unsigned char>('5'),
            static_cast<unsigned char>('6'),
            static_cast<unsigned char>('7'),
            static_cast<unsigned char>('8'),
            static_cast<unsigned char>('9'),
        };

        const size_t iters = read_iterations_from_env();
        std::array<std::byte, 96> prefix{};

        for (size_t iter = 0; iter < iters; ++iter)
        {
            const std::uint64_t seed = k_base_seed ^ (static_cast<std::uint64_t>(iter) * k_iteration_mix);
            SplitMix64 rng(seed);

            const size_t len = rng.next_size(prefix.size());
            for (size_t i = 0; i < len; ++i)
            {
                if ((rng.next_u32() % 6u) == 0u)
                {
                    const size_t index = rng.next_size(corpus.size() - 1);
                    prefix[i] = static_cast<std::byte>(corpus[index]);
                }
                else
                {
                    prefix[i] = rng.next_byte();
                }
            }

            const auto bytes = std::span<const std::byte>(prefix.data(), len);
            oc::condrv::vt_input::DecodedToken token{};
            const auto result = oc::condrv::vt_input::try_decode_vt(bytes, token);

            switch (result)
            {
            case oc::condrv::vt_input::DecodeResult::produced:
                if (token.bytes_consumed == 0 || token.bytes_consumed > len)
                {
                    fwprintf(stderr, L"[DETAIL] vt_input produced invalid bytes_consumed (iter=%zu seed=0x%016llX len=%zu consumed=%zu)\n",
                             iter,
                             static_cast<unsigned long long>(seed),
                             len,
                             token.bytes_consumed);
                    dump_bytes(bytes);
                    return false;
                }
                if (token.kind == oc::condrv::vt_input::TokenKind::text_units)
                {
                    fwprintf(stderr, L"[DETAIL] vt_input produced unexpected text_units token (iter=%zu seed=0x%016llX)\n",
                             iter,
                             static_cast<unsigned long long>(seed));
                    dump_bytes(bytes);
                    return false;
                }
                break;
            case oc::condrv::vt_input::DecodeResult::need_more_data:
            {
                if (len == 0)
                {
                    fwprintf(stderr, L"[DETAIL] vt_input returned need_more_data on empty prefix (iter=%zu seed=0x%016llX)\n",
                             iter,
                             static_cast<unsigned long long>(seed));
                    return false;
                }

                const std::byte head = prefix[0];
                if (head != static_cast<std::byte>(0x1B) && head != static_cast<std::byte>(0x9B))
                {
                    fwprintf(stderr, L"[DETAIL] vt_input returned need_more_data on non-ESC head (iter=%zu seed=0x%016llX head=%02X)\n",
                             iter,
                             static_cast<unsigned long long>(seed),
                             static_cast<unsigned>(std::to_integer<unsigned char>(head)));
                    dump_bytes(bytes);
                    return false;
                }
                break;
            }
            case oc::condrv::vt_input::DecodeResult::no_match:
                break;
            default:
                fwprintf(stderr, L"[DETAIL] vt_input returned unknown enum (iter=%zu seed=0x%016llX)\n",
                         iter,
                         static_cast<unsigned long long>(seed));
                dump_bytes(bytes);
                return false;
            }
        }

        return true;
    }

    [[nodiscard]] bool make_fuzz_screen_buffer(const COORD cursor, std::shared_ptr<oc::condrv::ScreenBuffer>& out) noexcept
    {
        auto settings = oc::condrv::ScreenBuffer::default_settings();
        settings.buffer_size = COORD{ 64, 16 };
        settings.window_size = settings.buffer_size;
        settings.maximum_window_size = settings.buffer_size;
        settings.scroll_position = COORD{ 0, 0 };
        settings.cursor_position = cursor;
        settings.text_attributes = 0x07;

        auto created = oc::condrv::ScreenBuffer::create(std::move(settings));
        if (!created)
        {
            fwprintf(stderr, L"[DETAIL] ScreenBuffer::create failed in fuzz test\n");
            return false;
        }

        out = std::move(created.value());
        return static_cast<bool>(out);
    }

    [[nodiscard]] bool test_vt_output_streaming_fuzz_invariants()
    {
        constexpr std::array<wchar_t, 22> corpus{
            L'\x1b',
            L'\x009b',
            L'\x009d',
            L'\x009c',
            L'\x0007',
            L'[',
            L']',
            L'\\',
            L'?',
            L';',
            L'_',
            L'~',
            L'0',
            L'1',
            L'2',
            L'3',
            L'4',
            L'5',
            L'6',
            L'7',
            L'8',
            L'9',
        };

        const size_t iters = read_iterations_from_env();
        constexpr ULONG out_mode = ENABLE_VIRTUAL_TERMINAL_PROCESSING | ENABLE_PROCESSED_OUTPUT | ENABLE_WRAP_AT_EOL_OUTPUT;

        std::vector<wchar_t> snapshot;
        snapshot.resize(64u * 16u);

        for (size_t iter = 0; iter < iters; ++iter)
        {
            const std::uint64_t seed = k_base_seed ^ (static_cast<std::uint64_t>(iter) * k_iteration_mix);
            SplitMix64 rng(seed);

            std::shared_ptr<oc::condrv::ScreenBuffer> buffer;
            if (!make_fuzz_screen_buffer(COORD{ 0, 0 }, buffer))
            {
                return false;
            }

            oc::condrv::NullHostIo host_io{};
            std::wstring stream;
            try
            {
                const size_t stream_len = rng.next_size(512);
                stream.resize(stream_len);
            }
            catch (...)
            {
                fwprintf(stderr, L"[DETAIL] failed to allocate fuzz stream buffer\n");
                return false;
            }

            for (size_t i = 0; i < stream.size(); ++i)
            {
                if ((rng.next_u32() % 10u) < 7u)
                {
                    const unsigned char value = std::to_integer<unsigned char>(rng.next_byte());
                    stream[i] = static_cast<wchar_t>(value);
                }
                else
                {
                    const size_t index = rng.next_size(corpus.size() - 1);
                    stream[i] = corpus[index];
                }
            }

            uint64_t previous_revision = buffer->revision();
            const COORD size = buffer->screen_buffer_size();
            const size_t cell_count = static_cast<size_t>(size.X) * static_cast<size_t>(size.Y);
            if (snapshot.size() != cell_count)
            {
                snapshot.resize(cell_count);
            }

            size_t offset = 0;
            size_t chunk_index = 0;
            while (offset < stream.size())
            {
                const size_t remaining = stream.size() - offset;
                const size_t raw_chunk = 1 + rng.next_size(39);
                const size_t chunk_size = raw_chunk < remaining ? raw_chunk : remaining;
                const std::wstring_view chunk(stream.data() + offset, chunk_size);

                oc::condrv::apply_text_to_screen_buffer(*buffer, chunk, out_mode, nullptr, &host_io);

                const COORD cursor = buffer->cursor_position();
                const SMALL_RECT window = buffer->window_rect();
                const uint64_t revision = buffer->revision();

                if (revision < previous_revision)
                {
                    fwprintf(stderr, L"[DETAIL] revision regressed (iter=%zu seed=0x%016llX chunk=%zu prev=%llu now=%llu)\n",
                             iter,
                             static_cast<unsigned long long>(seed),
                             chunk_index,
                             static_cast<unsigned long long>(previous_revision),
                             static_cast<unsigned long long>(revision));
                    dump_wchars_low_byte(chunk);
                    return false;
                }
                previous_revision = revision;

                if (!cursor_in_range(cursor, size))
                {
                    fwprintf(stderr, L"[DETAIL] cursor out of range (iter=%zu seed=0x%016llX chunk=%zu cursor=(%d,%d) size=(%d,%d))\n",
                             iter,
                             static_cast<unsigned long long>(seed),
                             chunk_index,
                             cursor.X,
                             cursor.Y,
                             size.X,
                             size.Y);
                    dump_wchars_low_byte(chunk);
                    return false;
                }

                if (!window_rect_in_range(window, size))
                {
                    fwprintf(stderr, L"[DETAIL] window rect out of range (iter=%zu seed=0x%016llX chunk=%zu window=(%d,%d,%d,%d) size=(%d,%d))\n",
                             iter,
                             static_cast<unsigned long long>(seed),
                             chunk_index,
                             window.Left,
                             window.Top,
                             window.Right,
                             window.Bottom,
                             size.X,
                             size.Y);
                    dump_wchars_low_byte(chunk);
                    return false;
                }

                const size_t read = buffer->read_output_characters(COORD{ 0, 0 }, std::span<wchar_t>(snapshot));
                if (read != cell_count)
                {
                    fwprintf(stderr, L"[DETAIL] read_output_characters returned %zu expected %zu (iter=%zu seed=0x%016llX chunk=%zu)\n",
                             read,
                             cell_count,
                             iter,
                             static_cast<unsigned long long>(seed),
                             chunk_index);
                    dump_wchars_low_byte(chunk);
                    return false;
                }

                offset += chunk_size;
                ++chunk_index;
            }
        }

        return true;
    }

    [[nodiscard]] wchar_t read_cell(const oc::condrv::ScreenBuffer& buffer, const COORD coord) noexcept
    {
        std::array<wchar_t, 1> dest{};
        (void)buffer.read_output_characters(coord, dest);
        return dest[0];
    }

    [[nodiscard]] bool test_vt_output_csi_overlong_is_abandoned_and_does_not_move_cursor()
    {
        std::shared_ptr<oc::condrv::ScreenBuffer> buffer;
        if (!make_fuzz_screen_buffer(COORD{ 0, 0 }, buffer))
        {
            return false;
        }

        oc::condrv::NullHostIo host_io{};
        constexpr ULONG out_mode = ENABLE_VIRTUAL_TERMINAL_PROCESSING | ENABLE_PROCESSED_OUTPUT | ENABLE_WRAP_AT_EOL_OUTPUT;

        std::wstring input;
        input.reserve(2 + 129 + 1);
        input.push_back(L'\x1b');
        input.push_back(L'[');
        input.append(129, L'1');
        input.push_back(L'A');

        oc::condrv::apply_text_to_screen_buffer(*buffer, input, out_mode, nullptr, &host_io);

        const wchar_t head = read_cell(*buffer, COORD{ 0, 0 });
        if (head != L'A')
        {
            fwprintf(stderr, L"[DETAIL] overlong CSI did not abandon to ground (cell[0,0]=%04X)\n", static_cast<unsigned>(head));
            return false;
        }

        return true;
    }

    [[nodiscard]] bool test_vt_output_esc_dispatch_overlong_is_abandoned()
    {
        std::shared_ptr<oc::condrv::ScreenBuffer> buffer;
        if (!make_fuzz_screen_buffer(COORD{ 0, 0 }, buffer))
        {
            return false;
        }

        oc::condrv::NullHostIo host_io{};
        constexpr ULONG out_mode = ENABLE_VIRTUAL_TERMINAL_PROCESSING | ENABLE_PROCESSED_OUTPUT | ENABLE_WRAP_AT_EOL_OUTPUT;

        std::wstring input;
        input.reserve(1 + 16 + 1);
        input.push_back(L'\x1b');
        input.append(16, L'#');
        input.push_back(L'A');

        oc::condrv::apply_text_to_screen_buffer(*buffer, input, out_mode, nullptr, &host_io);

        const wchar_t head = read_cell(*buffer, COORD{ 0, 0 });
        if (head != L'A')
        {
            fwprintf(stderr, L"[DETAIL] overlong ESC dispatch did not abandon to ground (cell[0,0]=%04X)\n", static_cast<unsigned>(head));
            return false;
        }

        return true;
    }

    [[nodiscard]] bool test_vt_output_osc_title_payload_truncates_to_fixed_buffer()
    {
        std::shared_ptr<oc::condrv::ScreenBuffer> buffer;
        if (!make_fuzz_screen_buffer(COORD{ 0, 0 }, buffer))
        {
            return false;
        }

        oc::condrv::ServerState title_state;
        oc::condrv::NullHostIo host_io{};
        constexpr ULONG out_mode = ENABLE_VIRTUAL_TERMINAL_PROCESSING | ENABLE_PROCESSED_OUTPUT | ENABLE_WRAP_AT_EOL_OUTPUT;

        std::wstring input;
        input.reserve(4 + 6000 + 1);
        input.push_back(L'\x1b');
        input.push_back(L']');
        input.push_back(L'2');
        input.push_back(L';');
        input.append(6000, L'X');
        input.push_back(L'\x07');

        oc::condrv::apply_text_to_screen_buffer(*buffer, input, out_mode, &title_state, &host_io);

        const std::wstring_view title = title_state.title(false);
        if (title.size() > 4096)
        {
            fwprintf(stderr, L"[DETAIL] OSC title was not truncated (length=%zu)\n", title.size());
            return false;
        }

        std::vector<wchar_t> snapshot;
        snapshot.resize(64u * 16u);
        const size_t read = buffer->read_output_characters(COORD{ 0, 0 }, std::span<wchar_t>(snapshot));
        if (read != snapshot.size())
        {
            fwprintf(stderr, L"[DETAIL] OSC title snapshot read failed (read=%zu)\n", read);
            return false;
        }
        for (const auto ch : snapshot)
        {
            if (ch != L' ')
            {
                fwprintf(stderr, L"[DETAIL] OSC title leaked printable output (cell=%04X)\n", static_cast<unsigned>(ch));
                return false;
            }
        }

        return true;
    }
}

bool run_condrv_vt_fuzz_tests()
{
    if (!test_vt_input_decoder_fuzz_invariants())
    {
        fwprintf(stderr, L"[DETAIL] vt input decoder fuzz invariants failed\n");
        return false;
    }

    if (!test_vt_output_streaming_fuzz_invariants())
    {
        fwprintf(stderr, L"[DETAIL] vt output streaming fuzz invariants failed\n");
        return false;
    }

    if (!test_vt_output_csi_overlong_is_abandoned_and_does_not_move_cursor())
    {
        fwprintf(stderr, L"[DETAIL] overlong CSI bounds test failed\n");
        return false;
    }

    if (!test_vt_output_esc_dispatch_overlong_is_abandoned())
    {
        fwprintf(stderr, L"[DETAIL] overlong ESC dispatch bounds test failed\n");
        return false;
    }

    if (!test_vt_output_osc_title_payload_truncates_to_fixed_buffer())
    {
        fwprintf(stderr, L"[DETAIL] OSC title payload bounds test failed\n");
        return false;
    }

    return true;
}

