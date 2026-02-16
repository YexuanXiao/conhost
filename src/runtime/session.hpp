#pragma once

#include "core/handle_view.hpp"

#include "logging/logger.hpp"

#include <expected>
#include <string>

namespace oc::runtime
{
    struct SessionOptions final
    {
        std::wstring client_command_line;
        bool create_server_handle{ true };
        core::HandleView server_handle{};
        core::HandleView signal_handle{};

        core::HandleView host_input{};
        core::HandleView host_output{};
        short width{ 0 };
        short height{ 0 };

        bool headless{ false };
        bool in_conpty_mode{ false };
        bool inherit_cursor{ false };
        std::wstring text_measurement;
        bool force_no_handoff{ false };
    };

    struct SessionError final
    {
        std::wstring context;
        DWORD win32_error{ ERROR_GEN_FAILURE };
    };

    class Session final
    {
    public:
        [[nodiscard]] static std::expected<DWORD, SessionError> run(const SessionOptions& options, logging::Logger& logger) noexcept;
    };
}
