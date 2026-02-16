#include "serialization/fast_number.hpp"

#include <array>
#include <bit>
#include <charconv>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <limits>
#include <string>
#include <string_view>

namespace
{
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

    private:
        std::uint64_t _state{};
    };

    [[nodiscard]] std::wstring_view widen_ascii_into(const std::string_view ascii, std::wstring& storage)
    {
        storage.resize(ascii.size());
        for (size_t i = 0; i < ascii.size(); ++i)
        {
            storage[i] = static_cast<wchar_t>(static_cast<unsigned char>(ascii[i]));
        }
        return std::wstring_view(storage);
    }

    bool test_parse_i16_success()
    {
        const auto parsed = oc::serialization::parse_i16(L"-123");
        return parsed.has_value() && *parsed == static_cast<std::int16_t>(-123);
    }

    bool test_parse_i16_overflow()
    {
        const auto parsed = oc::serialization::parse_i16(L"99999");
        return !parsed.has_value();
    }

    bool test_parse_hex_u32()
    {
        const auto parsed = oc::serialization::parse_hex_u32(L"0x1A2B", true);
        return parsed.has_value() && *parsed == 0x1A2B;
    }

    bool test_parse_u32()
    {
        const auto parsed = oc::serialization::parse_u32(L"4294967295");
        return parsed.has_value() && *parsed == 0xFFFFFFFFu;
    }

    bool test_parse_f64()
    {
        const auto parsed = oc::serialization::parse_f64(L"3.5");
        return parsed.has_value() && std::fabs(*parsed - 3.5) < 1e-12;
    }

    bool test_format_i64()
    {
        const auto formatted = oc::serialization::format_i64(-9876543210LL);
        return formatted.has_value() && *formatted == "-9876543210";
    }

    bool test_format_f64_roundtrip()
    {
        const auto formatted = oc::serialization::format_f64(1.23456789012345);
        if (!formatted)
        {
            return false;
        }
        const auto parsed = oc::serialization::parse_f64(std::wstring(formatted->begin(), formatted->end()));
        if (!parsed)
        {
            return false;
        }
        return std::fabs(*parsed - 1.23456789012345) < 1e-15;
    }

    bool test_parse_hex_requires_prefix()
    {
        const auto parsed = oc::serialization::parse_hex_u32(L"FF", true);
        return !parsed.has_value();
    }

    bool test_parse_f64_invalid()
    {
        const auto parsed = oc::serialization::parse_f64(L"abc");
        return !parsed.has_value();
    }

    bool test_format_u64()
    {
        const auto formatted = oc::serialization::format_u64(18446744073709551615ull);
        return formatted.has_value() && *formatted == "18446744073709551615";
    }

    bool test_parse_i32_boundaries()
    {
        const auto max = oc::serialization::parse_i32(L"2147483647");
        if (!max || *max != std::numeric_limits<std::int32_t>::max())
        {
            return false;
        }
        if (oc::serialization::parse_i32(L"2147483648").has_value())
        {
            return false;
        }

        const auto min = oc::serialization::parse_i32(L"-2147483648");
        if (!min || *min != std::numeric_limits<std::int32_t>::min())
        {
            return false;
        }
        if (oc::serialization::parse_i32(L"-2147483649").has_value())
        {
            return false;
        }

        const auto plus = oc::serialization::parse_i32(L"+0");
        if (!plus || *plus != 0)
        {
            return false;
        }

        return true;
    }

    bool test_parse_u32_boundaries()
    {
        const auto max = oc::serialization::parse_u32(L"4294967295");
        if (!max || *max != std::numeric_limits<std::uint32_t>::max())
        {
            return false;
        }
        if (oc::serialization::parse_u32(L"4294967296").has_value())
        {
            return false;
        }
        if (oc::serialization::parse_u32(L"-1").has_value())
        {
            return false;
        }
        if (!oc::serialization::parse_u32(L"+42"))
        {
            return false;
        }
        return true;
    }

    bool test_parse_hex_boundaries()
    {
        const auto u32_max = oc::serialization::parse_hex_u32(L"0xFFFFFFFF", true);
        if (!u32_max || *u32_max != std::numeric_limits<std::uint32_t>::max())
        {
            return false;
        }
        if (oc::serialization::parse_hex_u32(L"0x100000000", true).has_value())
        {
            return false;
        }

        const auto u64_max = oc::serialization::parse_hex_u64(L"0xFFFFFFFFFFFFFFFF", true);
        if (!u64_max || *u64_max != std::numeric_limits<std::uint64_t>::max())
        {
            return false;
        }
        if (oc::serialization::parse_hex_u64(L"0x10000000000000000", true).has_value())
        {
            return false;
        }

        if (oc::serialization::parse_hex_u32(L"FF", true).has_value())
        {
            return false;
        }
        if (!oc::serialization::parse_hex_u32(L"FF", false))
        {
            return false;
        }

        return true;
    }

    bool test_integer_roundtrip_stress()
    {
        constexpr std::uint64_t seed = 0x4F434E45574F434FULL;
        constexpr size_t iters = 5000;

        SplitMix64 rng(seed);
        std::wstring widened;

        for (size_t i = 0; i < iters; ++i)
        {
            const std::uint32_t u = rng.next_u32();
            const std::int64_t centered = static_cast<std::int64_t>(u) - 0x80000000LL;
            const auto i32 = static_cast<std::int32_t>(centered);

            const auto formatted_i32 = oc::serialization::format_i64(static_cast<std::int64_t>(i32));
            if (!formatted_i32)
            {
                fwprintf(stderr, L"[DETAIL] format_i64 failed at iter=%zu\n", i);
                return false;
            }

            const auto parsed_i32 = oc::serialization::parse_i32(widen_ascii_into(*formatted_i32, widened));
            if (!parsed_i32 || *parsed_i32 != i32)
            {
                fwprintf(stderr, L"[DETAIL] parse_i32 roundtrip failed at iter=%zu\n", i);
                return false;
            }

            const auto formatted_u32 = oc::serialization::format_u64(static_cast<std::uint64_t>(u));
            if (!formatted_u32)
            {
                fwprintf(stderr, L"[DETAIL] format_u64 failed at iter=%zu\n", i);
                return false;
            }

            const auto parsed_u32 = oc::serialization::parse_u32(widen_ascii_into(*formatted_u32, widened));
            if (!parsed_u32 || *parsed_u32 != u)
            {
                fwprintf(stderr, L"[DETAIL] parse_u32 roundtrip failed at iter=%zu\n", i);
                return false;
            }
        }

        return true;
    }

    bool test_parse_f64_overflow_and_non_ascii()
    {
        const auto overflow = oc::serialization::parse_f64(L"1e309");
        if (overflow.has_value() || overflow.error().code != oc::serialization::NumberErrorCode::overflow)
        {
            return false;
        }

        std::wstring non_ascii;
        non_ascii.push_back(static_cast<wchar_t>(0x80));
        const auto invalid = oc::serialization::parse_f64(non_ascii);
        if (invalid.has_value() || invalid.error().code != oc::serialization::NumberErrorCode::invalid_character)
        {
            return false;
        }

        return true;
    }

    bool test_format_f64_roundtrip_stress()
    {
        constexpr std::uint64_t seed = 0x4F434E45574F434FULL ^ 0x123456789ABCDEF0ULL;
        constexpr size_t iters = 2000;

        SplitMix64 rng(seed);
        std::wstring widened;

        for (size_t i = 0; i < iters; ++i)
        {
            // Construct a finite IEEE-754 binary64 by ensuring the exponent is not all-ones.
            std::uint64_t bits = rng.next_u64();
            const std::uint64_t mantissa_mask = (1ULL << 52) - 1ULL;
            const std::uint64_t mantissa = bits & mantissa_mask;
            const std::uint64_t exponent = static_cast<std::uint64_t>(rng.next_u32() % 2047u); // 0..2046
            const std::uint64_t sign = bits & (1ULL << 63);
            bits = sign | (exponent << 52) | mantissa;

            const double value = std::bit_cast<double>(bits);
            if (!std::isfinite(value))
            {
                fwprintf(stderr, L"[DETAIL] generated non-finite value unexpectedly at iter=%zu\n", i);
                return false;
            }

            const auto formatted = oc::serialization::format_f64(value);
            if (!formatted)
            {
                fwprintf(stderr, L"[DETAIL] format_f64 failed at iter=%zu\n", i);
                return false;
            }

            const auto parsed = oc::serialization::parse_f64(widen_ascii_into(*formatted, widened));
            if (!parsed)
            {
                fwprintf(stderr, L"[DETAIL] parse_f64 failed at iter=%zu\n", i);
                return false;
            }

            if ((value != 0.0 || *parsed != 0.0) && *parsed != value)
            {
                fwprintf(stderr, L"[DETAIL] format_f64 roundtrip mismatch at iter=%zu\n", i);
                return false;
            }
        }

        return true;
    }
}

bool run_fast_number_tests()
{
    return test_parse_i16_success() &&
           test_parse_i16_overflow() &&
           test_parse_hex_u32() &&
           test_parse_u32() &&
           test_parse_f64() &&
           test_format_i64() &&
           test_format_f64_roundtrip() &&
           test_parse_hex_requires_prefix() &&
           test_parse_f64_invalid() &&
           test_format_u64() &&
           test_parse_i32_boundaries() &&
           test_parse_u32_boundaries() &&
           test_parse_hex_boundaries() &&
           test_integer_roundtrip_stress() &&
           test_parse_f64_overflow_and_non_ascii() &&
           test_format_f64_roundtrip_stress();
}
