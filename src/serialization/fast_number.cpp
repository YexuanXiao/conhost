#include "serialization/fast_number.hpp"

#include <array>
#include <limits>
#include <system_error>

namespace oc::serialization
{
    namespace
    {
        [[nodiscard]] constexpr NumberError make_error(const NumberErrorCode code) noexcept
        {
            return NumberError{ .code = code };
        }

        [[nodiscard]] std::expected<std::string, NumberError> narrow_ascii_numeric(std::wstring_view text) noexcept
        {
            if (text.empty())
            {
                return std::unexpected(make_error(NumberErrorCode::empty_input));
            }

            std::string ascii;
            ascii.reserve(text.size());
            for (const wchar_t ch : text)
            {
                if (ch > 0x7f)
                {
                    return std::unexpected(make_error(NumberErrorCode::invalid_character));
                }
                ascii.push_back(static_cast<char>(ch));
            }
            return ascii;
        }

        [[nodiscard]] std::expected<std::int32_t, NumberError> parse_signed_32(std::wstring_view text) noexcept
        {
            if (text.empty())
            {
                return std::unexpected(make_error(NumberErrorCode::empty_input));
            }

            size_t index = 0;
            bool negative = false;
            if (text[index] == L'+' || text[index] == L'-')
            {
                negative = (text[index] == L'-');
                ++index;
            }

            if (index >= text.size())
            {
                return std::unexpected(make_error(NumberErrorCode::invalid_character));
            }

            std::uint64_t accumulator = 0;
            for (; index < text.size(); ++index)
            {
                const wchar_t ch = text[index];
                if (ch < L'0' || ch > L'9')
                {
                    return std::unexpected(make_error(NumberErrorCode::invalid_character));
                }

                const std::uint32_t digit = static_cast<std::uint32_t>(ch - L'0');
                accumulator = accumulator * 10 + digit;

                if (!negative && accumulator > static_cast<std::uint64_t>(std::numeric_limits<std::int32_t>::max()))
                {
                    return std::unexpected(make_error(NumberErrorCode::overflow));
                }

                constexpr std::uint64_t kMinAbs = static_cast<std::uint64_t>(std::numeric_limits<std::int32_t>::max()) + 1;
                if (negative && accumulator > kMinAbs)
                {
                    return std::unexpected(make_error(NumberErrorCode::underflow));
                }
            }

            if (!negative)
            {
                return static_cast<std::int32_t>(accumulator);
            }

            if (accumulator == static_cast<std::uint64_t>(std::numeric_limits<std::int32_t>::max()) + 1)
            {
                return std::numeric_limits<std::int32_t>::min();
            }

            return -static_cast<std::int32_t>(accumulator);
        }

        [[nodiscard]] std::expected<std::uint32_t, NumberError> parse_unsigned_32(std::wstring_view text) noexcept
        {
            if (text.empty())
            {
                return std::unexpected(make_error(NumberErrorCode::empty_input));
            }

            size_t index = 0;
            if (text[index] == L'+')
            {
                ++index;
            }
            if (index >= text.size())
            {
                return std::unexpected(make_error(NumberErrorCode::invalid_character));
            }

            std::uint64_t accumulator = 0;
            for (; index < text.size(); ++index)
            {
                const wchar_t ch = text[index];
                if (ch < L'0' || ch > L'9')
                {
                    return std::unexpected(make_error(NumberErrorCode::invalid_character));
                }
                const std::uint32_t digit = static_cast<std::uint32_t>(ch - L'0');
                accumulator = accumulator * 10 + digit;
                if (accumulator > std::numeric_limits<std::uint32_t>::max())
                {
                    return std::unexpected(make_error(NumberErrorCode::overflow));
                }
            }

            return static_cast<std::uint32_t>(accumulator);
        }

        [[nodiscard]] std::expected<std::uint32_t, NumberError> parse_hex_unsigned_32(std::wstring_view text, const bool require_prefix) noexcept
        {
            if (text.empty())
            {
                return std::unexpected(make_error(NumberErrorCode::empty_input));
            }

            size_t index = 0;
            if (text.size() >= 2 && text[0] == L'0' && (text[1] == L'x' || text[1] == L'X'))
            {
                index = 2;
            }
            else if (require_prefix)
            {
                return std::unexpected(make_error(NumberErrorCode::invalid_character));
            }

            if (index >= text.size())
            {
                return std::unexpected(make_error(NumberErrorCode::invalid_character));
            }

            std::uint64_t accumulator = 0;
            for (; index < text.size(); ++index)
            {
                const wchar_t ch = text[index];
                std::uint32_t digit = 0;
                if (ch >= L'0' && ch <= L'9')
                {
                    digit = static_cast<std::uint32_t>(ch - L'0');
                }
                else if (ch >= L'a' && ch <= L'f')
                {
                    digit = static_cast<std::uint32_t>(ch - L'a' + 10);
                }
                else if (ch >= L'A' && ch <= L'F')
                {
                    digit = static_cast<std::uint32_t>(ch - L'A' + 10);
                }
                else
                {
                    return std::unexpected(make_error(NumberErrorCode::invalid_character));
                }

                accumulator = (accumulator << 4) | digit;
                if (accumulator > std::numeric_limits<std::uint32_t>::max())
                {
                    return std::unexpected(make_error(NumberErrorCode::overflow));
                }
            }

            return static_cast<std::uint32_t>(accumulator);
        }

        [[nodiscard]] std::expected<std::uint64_t, NumberError> parse_hex_unsigned_64(std::wstring_view text, const bool require_prefix) noexcept
        {
            if (text.empty())
            {
                return std::unexpected(make_error(NumberErrorCode::empty_input));
            }

            size_t index = 0;
            if (text.size() >= 2 && text[0] == L'0' && (text[1] == L'x' || text[1] == L'X'))
            {
                index = 2;
            }
            else if (require_prefix)
            {
                return std::unexpected(make_error(NumberErrorCode::invalid_character));
            }

            if (index >= text.size())
            {
                return std::unexpected(make_error(NumberErrorCode::invalid_character));
            }

            std::uint64_t accumulator = 0;
            for (; index < text.size(); ++index)
            {
                const wchar_t ch = text[index];
                std::uint64_t digit = 0;
                if (ch >= L'0' && ch <= L'9')
                {
                    digit = static_cast<std::uint64_t>(ch - L'0');
                }
                else if (ch >= L'a' && ch <= L'f')
                {
                    digit = static_cast<std::uint64_t>(ch - L'a' + 10);
                }
                else if (ch >= L'A' && ch <= L'F')
                {
                    digit = static_cast<std::uint64_t>(ch - L'A' + 10);
                }
                else
                {
                    return std::unexpected(make_error(NumberErrorCode::invalid_character));
                }

                if (accumulator > (std::numeric_limits<std::uint64_t>::max() >> 4))
                {
                    return std::unexpected(make_error(NumberErrorCode::overflow));
                }

                accumulator = (accumulator << 4) | digit;
            }

            return accumulator;
        }

        template<typename T>
        [[nodiscard]] std::expected<T, NumberError> parse_float_ascii(std::string_view ascii) noexcept
        {
            T value{};
            const auto result = std::from_chars(ascii.data(), ascii.data() + ascii.size(), value, std::chars_format::general);
            if (result.ec == std::errc::result_out_of_range)
            {
                return std::unexpected(make_error(NumberErrorCode::overflow));
            }
            if (result.ec != std::errc{} || result.ptr != ascii.data() + ascii.size())
            {
                return std::unexpected(make_error(NumberErrorCode::invalid_character));
            }
            return value;
        }

        template<typename Integer>
        [[nodiscard]] std::expected<std::string, NumberError> format_integer(Integer value) noexcept
        {
            std::array<char, 64> buffer{};
            const auto result = std::to_chars(buffer.data(), buffer.data() + buffer.size(), value);
            if (result.ec == std::errc::value_too_large)
            {
                return std::unexpected(make_error(NumberErrorCode::buffer_too_small));
            }
            if (result.ec != std::errc{})
            {
                return std::unexpected(make_error(NumberErrorCode::conversion_failure));
            }

            return std::string(buffer.data(), static_cast<size_t>(result.ptr - buffer.data()));
        }
    }

    std::expected<std::int16_t, NumberError> parse_i16(const std::wstring_view text) noexcept
    {
        auto parsed = parse_signed_32(text);
        if (!parsed)
        {
            return std::unexpected(parsed.error());
        }
        if (*parsed > std::numeric_limits<std::int16_t>::max())
        {
            return std::unexpected(make_error(NumberErrorCode::overflow));
        }
        if (*parsed < std::numeric_limits<std::int16_t>::min())
        {
            return std::unexpected(make_error(NumberErrorCode::underflow));
        }
        return static_cast<std::int16_t>(*parsed);
    }

    std::expected<std::int32_t, NumberError> parse_i32(const std::wstring_view text) noexcept
    {
        return parse_signed_32(text);
    }

    std::expected<std::uint32_t, NumberError> parse_u32(const std::wstring_view text) noexcept
    {
        return parse_unsigned_32(text);
    }

    std::expected<std::uint32_t, NumberError> parse_hex_u32(const std::wstring_view text, const bool require_prefix) noexcept
    {
        return parse_hex_unsigned_32(text, require_prefix);
    }

    std::expected<std::uint64_t, NumberError> parse_hex_u64(const std::wstring_view text, const bool require_prefix) noexcept
    {
        return parse_hex_unsigned_64(text, require_prefix);
    }

    std::expected<float, NumberError> parse_f32(const std::wstring_view text) noexcept
    {
        auto ascii = narrow_ascii_numeric(text);
        if (!ascii)
        {
            return std::unexpected(ascii.error());
        }
        return parse_float_ascii<float>(*ascii);
    }

    std::expected<double, NumberError> parse_f64(const std::wstring_view text) noexcept
    {
        auto ascii = narrow_ascii_numeric(text);
        if (!ascii)
        {
            return std::unexpected(ascii.error());
        }
        return parse_float_ascii<double>(*ascii);
    }

    std::expected<std::string, NumberError> format_i64(const std::int64_t value) noexcept
    {
        return format_integer(value);
    }

    std::expected<std::string, NumberError> format_u64(const std::uint64_t value) noexcept
    {
        return format_integer(value);
    }

    std::expected<std::string, NumberError> format_f64(const double value, const std::chars_format format, const int precision) noexcept
    {
        std::array<char, 128> buffer{};

        std::to_chars_result result{};
        if (precision < 0)
        {
            result = std::to_chars(buffer.data(), buffer.data() + buffer.size(), value, format);
        }
        else
        {
            result = std::to_chars(buffer.data(), buffer.data() + buffer.size(), value, format, precision);
        }

        if (result.ec == std::errc::value_too_large)
        {
            return std::unexpected(make_error(NumberErrorCode::buffer_too_small));
        }
        if (result.ec != std::errc{})
        {
            return std::unexpected(make_error(NumberErrorCode::conversion_failure));
        }

        return std::string(buffer.data(), static_cast<size_t>(result.ptr - buffer.data()));
    }
}
