#include "core/utf8_stream_decoder.hpp"

#include <cstddef>
#include <initializer_list>
#include <string>
#include <vector>

namespace
{
    [[nodiscard]] std::vector<std::byte> as_bytes(const std::initializer_list<unsigned char> values)
    {
        std::vector<std::byte> bytes;
        bytes.reserve(values.size());
        for (const unsigned char value : values)
        {
            bytes.push_back(static_cast<std::byte>(value));
        }
        return bytes;
    }

    bool test_ascii_passthrough()
    {
        oc::core::Utf8StreamDecoder decoder;
        const std::wstring out = decoder.decode_append(as_bytes({ 'h', 'e', 'l', 'l', 'o' }));
        return out == L"hello" && !decoder.has_pending();
    }

    bool test_two_byte_code_point_split()
    {
        oc::core::Utf8StreamDecoder decoder;

        const std::wstring part1 = decoder.decode_append(as_bytes({ 0xC2 }));
        if (!part1.empty() || !decoder.has_pending())
        {
            return false;
        }

        const std::wstring part2 = decoder.decode_append(as_bytes({ 0xA2 }));
        return part2 == L"\u00A2" && !decoder.has_pending();
    }

    bool test_three_byte_code_point_split()
    {
        oc::core::Utf8StreamDecoder decoder;

        const std::wstring part1 = decoder.decode_append(as_bytes({ 0xE2, 0x82 }));
        if (!part1.empty() || !decoder.has_pending())
        {
            return false;
        }

        const std::wstring part2 = decoder.decode_append(as_bytes({ 0xAC }));
        return part2 == L"\u20AC" && !decoder.has_pending();
    }

    bool test_four_byte_code_point_split_across_calls()
    {
        oc::core::Utf8StreamDecoder decoder;

        std::wstring expected;
        expected.push_back(static_cast<wchar_t>(0xD83D));
        expected.push_back(static_cast<wchar_t>(0xDE00));

        if (!decoder.decode_append(as_bytes({ 0xF0, 0x9F })).empty() || !decoder.has_pending())
        {
            return false;
        }
        if (!decoder.decode_append(as_bytes({ 0x98 })).empty() || !decoder.has_pending())
        {
            return false;
        }

        const std::wstring out = decoder.decode_append(as_bytes({ 0x80 }));
        return out == expected && !decoder.has_pending();
    }

    bool test_invalid_sequences_replace_with_replacement_char()
    {
        oc::core::Utf8StreamDecoder decoder;

        const std::wstring out = decoder.decode_append(as_bytes({ 0xC3, 0x28, 'A' }));
        const std::wstring expected = std::wstring(1, static_cast<wchar_t>(0xFFFD)) + L"(A";
        return out == expected && !decoder.has_pending();
    }
}

bool run_utf8_stream_decoder_tests()
{
    return test_ascii_passthrough() &&
           test_two_byte_code_point_split() &&
           test_three_byte_code_point_split() &&
           test_four_byte_code_point_split_across_calls() &&
           test_invalid_sequences_replace_with_replacement_char();
}

