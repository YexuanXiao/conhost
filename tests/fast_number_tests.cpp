#include "serialization/fast_number.hpp"

#include <cmath>
#include <cstdint>
#include <string>

namespace
{
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
           test_format_u64();
}
