#pragma once

#include <charconv>
#include <cstdint>
#include <expected>
#include <string>
#include <string_view>

namespace oc::serialization
{
    enum class NumberErrorCode
    {
        empty_input,
        invalid_character,
        overflow,
        underflow,
        buffer_too_small,
        conversion_failure,
    };

    struct NumberError final
    {
        NumberErrorCode code{ NumberErrorCode::conversion_failure };
    };

    // Integer parsing.
    [[nodiscard]] std::expected<std::int16_t, NumberError> parse_i16(std::wstring_view text) noexcept;
    [[nodiscard]] std::expected<std::int32_t, NumberError> parse_i32(std::wstring_view text) noexcept;
    [[nodiscard]] std::expected<std::uint32_t, NumberError> parse_u32(std::wstring_view text) noexcept;
    [[nodiscard]] std::expected<std::uint32_t, NumberError> parse_hex_u32(std::wstring_view text, bool require_prefix) noexcept;
    [[nodiscard]] std::expected<std::uint64_t, NumberError> parse_hex_u64(std::wstring_view text, bool require_prefix) noexcept;

    // Floating-point parsing.
    [[nodiscard]] std::expected<float, NumberError> parse_f32(std::wstring_view text) noexcept;
    [[nodiscard]] std::expected<double, NumberError> parse_f64(std::wstring_view text) noexcept;

    // Integer formatting.
    [[nodiscard]] std::expected<std::string, NumberError> format_i64(std::int64_t value) noexcept;
    [[nodiscard]] std::expected<std::string, NumberError> format_u64(std::uint64_t value) noexcept;

    // Floating-point formatting (shortest by default when precision < 0).
    [[nodiscard]] std::expected<std::string, NumberError> format_f64(
        double value,
        std::chars_format format = std::chars_format::general,
        int precision = -1) noexcept;
}
