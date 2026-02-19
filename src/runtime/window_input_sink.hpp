#pragma once

#include "core/unique_handle.hpp"
#include "renderer/window_host.hpp"

namespace oc::runtime
{
    class WindowInputPipeSink final : public renderer::IWindowInputSink
    {
    public:
        explicit WindowInputPipeSink(core::UniqueHandle write_end) noexcept;

        void submit_key_event(const KEY_EVENT_RECORD& key_event) noexcept override;

    private:
        core::UniqueHandle _write_end;
    };
}

