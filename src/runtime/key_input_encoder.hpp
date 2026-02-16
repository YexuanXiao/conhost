#pragma once

#include <Windows.h>

#include <vector>

namespace oc::runtime
{
    class KeyInputEncoder final
    {
    public:
        // Encodes a single console key event into VT-compatible UTF-8 bytes.
        // Returns an empty vector when the event should not be forwarded.
        [[nodiscard]] static std::vector<char> encode(const KEY_EVENT_RECORD& key_event);
    };
}

