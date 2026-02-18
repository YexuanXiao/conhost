#include "renderer/console_attributes.hpp"

namespace
{
    bool test_decode_basic_indices()
    {
        const auto decoded = oc::renderer::decode_attributes(0x1E);
        return decoded.foreground_index == 0x0E &&
               decoded.background_index == 0x01 &&
               !decoded.underline;
    }

    bool test_decode_reverse_video_swaps_indices()
    {
        const auto decoded = oc::renderer::decode_attributes(static_cast<USHORT>(0x1E | COMMON_LVB_REVERSE_VIDEO));
        return decoded.foreground_index == 0x01 &&
               decoded.background_index == 0x0E;
    }

    bool test_decode_underline_sets_flag()
    {
        const auto decoded = oc::renderer::decode_attributes(static_cast<USHORT>(0x07 | COMMON_LVB_UNDERSCORE));
        return decoded.underline;
    }
}

bool run_console_attributes_tests()
{
    return test_decode_basic_indices() &&
           test_decode_reverse_video_swaps_indices() &&
           test_decode_underline_sets_flag();
}

