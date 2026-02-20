#include "runtime/session.hpp"

#include "core/unique_handle.hpp"
#include "logging/logger.hpp"

#include <Windows.h>

namespace
{
    oc::logging::Logger make_logger()
    {
        return oc::logging::Logger(oc::logging::LogLevel::error);
    }

    [[nodiscard]] bool create_pipe_pair(oc::core::UniqueHandle& read_end, oc::core::UniqueHandle& write_end) noexcept
    {
        SECURITY_ATTRIBUTES security{};
        security.nLength = sizeof(security);
        security.lpSecurityDescriptor = nullptr;
        security.bInheritHandle = FALSE;

        if (::CreatePipe(read_end.put(), write_end.put(), &security, 0) == FALSE)
        {
            return false;
        }
        return true;
    }

    bool test_pseudoconsole_exit_code()
    {
        oc::logging::Logger logger = make_logger();

        oc::core::UniqueHandle host_in_read;
        oc::core::UniqueHandle host_in_write;
        oc::core::UniqueHandle host_out_read;
        oc::core::UniqueHandle host_out_write;
        if (!create_pipe_pair(host_in_read, host_in_write))
        {
            return false;
        }
        if (!create_pipe_pair(host_out_read, host_out_write))
        {
            return false;
        }

        oc::runtime::SessionOptions options{};
        options.client_command_line = L"%ComSpec% /c exit 17";
        options.create_server_handle = true;
        options.host_input = oc::core::HandleView(host_in_read.get());
        options.host_output = oc::core::HandleView(host_out_write.get());
        options.in_conpty_mode = true;
        options.headless = true;

        const auto result = oc::runtime::Session::run(options, logger);
        return result.has_value() && *result == 17;
    }

    bool test_empty_command_with_signaled_event()
    {
        oc::logging::Logger logger = make_logger();

        oc::core::UniqueHandle event_handle(::CreateEventW(nullptr, TRUE, TRUE, nullptr));
        if (!event_handle.valid())
        {
            return false;
        }

        oc::runtime::SessionOptions options{};
        options.client_command_line = L"";
        options.create_server_handle = true;
        options.signal_handle = oc::core::HandleView(event_handle.get());

        const auto result = oc::runtime::Session::run(options, logger);

        return result.has_value() && *result == 0;
    }

    bool test_server_handle_validation_failure()
    {
        oc::logging::Logger logger = make_logger();

        oc::runtime::SessionOptions options{};
        options.client_command_line = L"";
        options.create_server_handle = false;
        options.server_handle = oc::core::HandleView(INVALID_HANDLE_VALUE);

        const auto result = oc::runtime::Session::run(options, logger);
        return !result.has_value();
    }
}

bool run_session_tests()
{
    return test_pseudoconsole_exit_code() &&
           test_empty_command_with_signaled_event() &&
           test_server_handle_validation_failure();
}
