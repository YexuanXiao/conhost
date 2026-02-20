#pragma once

#pragma once

// Streaming UTF-8 -> UTF-16 decoder.
//
// Terminal-style streams arrive as arbitrary byte chunks (pipes, sockets, etc.).
// `MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, ...)` rejects both:
// - incomplete trailing sequences (common when a multi-byte code point is split across reads), and
// - invalid byte sequences.
//
// This helper provides a small stateful decoder that:
// - buffers incomplete trailing bytes, and
// - replaces malformed sequences with U+FFFD while guaranteeing forward progress.
//
// The replacement is intentionally conservative: it is not a full Unicode
// validation library. It is a pragmatic adapter for ConPTY/VT byte streams.

#include <Windows.h>

#include <algorithm>
#include <cstddef>
#include <limits>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace oc::core
{
    class Utf8StreamDecoder final
    {
    public:
        Utf8StreamDecoder() = default;
        ~Utf8StreamDecoder() = default;

        Utf8StreamDecoder(const Utf8StreamDecoder&) = delete;
        Utf8StreamDecoder& operator=(const Utf8StreamDecoder&) = delete;

        Utf8StreamDecoder(Utf8StreamDecoder&&) noexcept = default;
        Utf8StreamDecoder& operator=(Utf8StreamDecoder&&) noexcept = default;

        // Decodes the provided bytes plus any buffered pending bytes, returning
        // any UTF-16 output produced by this call.
        //
        // Incomplete trailing sequences are retained in the internal pending
        // buffer until sufficient bytes arrive in a later call.
        [[nodiscard]] std::wstring decode_append(std::span<const std::byte> bytes);

        [[nodiscard]] bool has_pending() const noexcept
        {
            return !_pending.empty();
        }

        void reset() noexcept
        {
            _pending.clear();
        }

    private:
        std::vector<std::byte> _pending;
    };

    namespace detail
    {
        [[nodiscard]] inline bool is_utf8_continuation_byte(const std::byte value) noexcept
        {
            const auto b = static_cast<unsigned char>(value);
            return b >= 0x80 && b <= 0xBF;
        }

        // Returns true when `bytes` looks like a *valid prefix* of a UTF-8 code
        // point but is shorter than the full sequence length.
        [[nodiscard]] inline bool looks_like_incomplete_utf8_sequence(const std::span<const std::byte> bytes) noexcept
        {
            if (bytes.empty())
            {
                return false;
            }

            const auto lead = static_cast<unsigned char>(bytes[0]);
            size_t expected = 0;

            if (lead >= 0xC2 && lead <= 0xDF)
            {
                expected = 2;
            }
            else if (lead >= 0xE0 && lead <= 0xEF)
            {
                expected = 3;
            }
            else if (lead >= 0xF0 && lead <= 0xF4)
            {
                expected = 4;
            }
            else
            {
                return false;
            }

            if (bytes.size() >= expected)
            {
                return false;
            }

            for (size_t i = 1; i < bytes.size(); ++i)
            {
                if (!is_utf8_continuation_byte(bytes[i]))
                {
                    return false;
                }
            }

            if (bytes.size() >= 2)
            {
                const auto first_cont = static_cast<unsigned char>(bytes[1]);
                // Minimal first-continuation validation for prefixes that must
                // avoid overlong encodings and surrogate ranges.
                if (lead == 0xE0 && first_cont < 0xA0)
                {
                    return false;
                }
                if (lead == 0xED && first_cont > 0x9F)
                {
                    return false;
                }
                if (lead == 0xF0 && first_cont < 0x90)
                {
                    return false;
                }
                if (lead == 0xF4 && first_cont > 0x8F)
                {
                    return false;
                }
            }

            return true;
        }
    }

    inline std::wstring Utf8StreamDecoder::decode_append(const std::span<const std::byte> bytes)
    {
        if (!bytes.empty())
        {
            _pending.insert(_pending.end(), bytes.begin(), bytes.end());
        }

        std::wstring output;

        size_t consumed = 0;
        const auto try_decode = [&](const size_t prefix_len) -> std::optional<std::wstring> {
            if (prefix_len == 0)
            {
                return std::nullopt;
            }

            const size_t max_int = static_cast<size_t>((std::numeric_limits<int>::max)());
            if (prefix_len > max_int)
            {
                return std::nullopt;
            }

            const auto* input = reinterpret_cast<const char*>(_pending.data() + consumed);
            const int input_bytes = static_cast<int>(prefix_len);

            const int required = ::MultiByteToWideChar(
                CP_UTF8,
                MB_ERR_INVALID_CHARS,
                input,
                input_bytes,
                nullptr,
                0);
            if (required <= 0)
            {
                return std::nullopt;
            }

            std::wstring decoded(static_cast<size_t>(required), L'\0');
            const int converted = ::MultiByteToWideChar(
                CP_UTF8,
                MB_ERR_INVALID_CHARS,
                input,
                input_bytes,
                decoded.data(),
                required);
            if (converted != required)
            {
                return std::nullopt;
            }

            return decoded;
        };

        while (consumed < _pending.size())
        {
            const size_t remaining = _pending.size() - consumed;
            const size_t attempt_len = std::min(
                remaining,
                static_cast<size_t>((std::numeric_limits<int>::max)()));

            if (auto decoded = try_decode(attempt_len))
            {
                output.append(*decoded);
                consumed += attempt_len;
                continue;
            }

            bool decoded_prefix = false;
            const size_t max_trim = std::min<size_t>(3, attempt_len);
            for (size_t trim = 1; trim <= max_trim; ++trim)
            {
                const size_t prefix_len = attempt_len - trim;
                if (prefix_len == 0)
                {
                    break;
                }

                if (auto decoded = try_decode(prefix_len))
                {
                    output.append(*decoded);
                    consumed += prefix_len;
                    decoded_prefix = true;
                    break;
                }
            }

            if (decoded_prefix)
            {
                continue;
            }

            // Incomplete trailing UTF-8 sequences are expected when reading from a
            // stream in chunks. Keep up to 3 bytes pending so the next call can
            // complete the code point.
            if (attempt_len <= 3)
            {
                const auto pending_span = std::span<const std::byte>(_pending.data(), _pending.size());
                const auto tail = pending_span.subspan(consumed, attempt_len);
                if (detail::looks_like_incomplete_utf8_sequence(tail))
                {
                    break;
                }
            }

            // Malformed sequence in the remaining byte stream. Replace with U+FFFD
            // and discard a single byte to guarantee forward progress.
            output.push_back(static_cast<wchar_t>(0xFFFD));
            consumed += 1;
        }

        if (consumed != 0)
        {
            _pending.erase(
                _pending.begin(),
                _pending.begin() + static_cast<std::ptrdiff_t>(consumed));
        }

        return output;
    }
}
