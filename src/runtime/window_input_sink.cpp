#include "runtime/window_input_sink.hpp"

#include "runtime/key_input_encoder.hpp"

#include <limits>

namespace oc::runtime
{
    WindowInputPipeSink::WindowInputPipeSink(core::UniqueHandle write_end) noexcept :
        _write_end(std::move(write_end))
    {
    }

    void WindowInputPipeSink::submit_key_event(const KEY_EVENT_RECORD& key_event) noexcept
    {
        if (!_write_end.valid())
        {
            return;
        }

        const auto encoded = KeyInputEncoder::encode(key_event);
        if (encoded.empty())
        {
            return;
        }

        const char* data = encoded.data();
        size_t remaining = encoded.size();

        while (remaining != 0)
        {
            const DWORD chunk = remaining > static_cast<size_t>(std::numeric_limits<DWORD>::max())
                ? std::numeric_limits<DWORD>::max()
                : static_cast<DWORD>(remaining);

            DWORD written = 0;
            if (::WriteFile(_write_end.get(), data, chunk, &written, nullptr) == FALSE)
            {
                const DWORD error = ::GetLastError();
                if (error == ERROR_BROKEN_PIPE ||
                    error == ERROR_NO_DATA ||
                    error == ERROR_PIPE_NOT_CONNECTED)
                {
                    _write_end.reset();
                    return;
                }

                return;
            }

            if (written == 0)
            {
                return;
            }

            data += written;
            remaining -= static_cast<size_t>(written);
        }
    }
}
