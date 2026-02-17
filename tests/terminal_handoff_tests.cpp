#include "runtime/terminal_handoff.hpp"

#include "core/unique_handle.hpp"
#include "logging/logger.hpp"

#include <Windows.h>

#include <cstdio>
#include <optional>

namespace
{
    constexpr CLSID k_test_terminal_clsid = {
        0x89b8f31d, 0xa53e, 0x4be8, { 0xbd, 0x56, 0xf8, 0xaf, 0x42, 0x78, 0xb0, 0x3d }
    };

    struct HookState final
    {
        int resolver_calls{ 0 };
        int invoker_calls{ 0 };
    };

    HookState* g_hooks = nullptr;

    [[nodiscard]] std::expected<std::optional<CLSID>, oc::runtime::TerminalHandoffError> resolve_none() noexcept
    {
        if (g_hooks != nullptr)
        {
            ++g_hooks->resolver_calls;
        }
        return std::optional<CLSID>{};
    }

    [[nodiscard]] std::expected<std::optional<CLSID>, oc::runtime::TerminalHandoffError> resolve_target() noexcept
    {
        if (g_hooks != nullptr)
        {
            ++g_hooks->resolver_calls;
        }
        return std::optional<CLSID>(k_test_terminal_clsid);
    }

    [[nodiscard]] std::expected<oc::runtime::TerminalHandoffChannels, oc::runtime::TerminalHandoffError> invoke_success(
        const CLSID& terminal_clsid,
        const oc::core::HandleView,
        oc::logging::Logger&) noexcept
    {
        if (g_hooks != nullptr)
        {
            ++g_hooks->invoker_calls;
        }
        if (::InlineIsEqualGUID(terminal_clsid, k_test_terminal_clsid) == FALSE)
        {
            return std::unexpected(oc::runtime::TerminalHandoffError{
                .context = L"unexpected test CLSID",
                .win32_error = ERROR_INVALID_PARAMETER,
                .hresult = E_INVALIDARG,
            });
        }

        HANDLE input_read = nullptr;
        HANDLE input_write = nullptr;
        if (::CreatePipe(&input_read, &input_write, nullptr, 0) == FALSE)
        {
            return std::unexpected(oc::runtime::TerminalHandoffError{
                .context = L"CreatePipe failed (input)",
                .win32_error = ::GetLastError(),
                .hresult = HRESULT_FROM_WIN32(::GetLastError()),
            });
        }

        HANDLE output_read = nullptr;
        HANDLE output_write = nullptr;
        if (::CreatePipe(&output_read, &output_write, nullptr, 0) == FALSE)
        {
            const DWORD error = ::GetLastError();
            (void)::CloseHandle(input_read);
            (void)::CloseHandle(input_write);
            return std::unexpected(oc::runtime::TerminalHandoffError{
                .context = L"CreatePipe failed (output)",
                .win32_error = error,
                .hresult = HRESULT_FROM_WIN32(error),
            });
        }

        HANDLE signal_read = nullptr;
        HANDLE signal_write = nullptr;
        if (::CreatePipe(&signal_read, &signal_write, nullptr, 0) == FALSE)
        {
            const DWORD error = ::GetLastError();
            (void)::CloseHandle(input_read);
            (void)::CloseHandle(input_write);
            (void)::CloseHandle(output_read);
            (void)::CloseHandle(output_write);
            return std::unexpected(oc::runtime::TerminalHandoffError{
                .context = L"CreatePipe failed (signal)",
                .win32_error = error,
                .hresult = HRESULT_FROM_WIN32(error),
            });
        }

        // Keep only the server-facing ends and close the opposite endpoints.
        oc::core::UniqueHandle unused_input_write(input_write);
        oc::core::UniqueHandle unused_output_read(output_read);
        oc::core::UniqueHandle unused_signal_write(signal_write);

        return oc::runtime::TerminalHandoffChannels{
            .host_input = oc::core::UniqueHandle(input_read),
            .host_output = oc::core::UniqueHandle(output_write),
            .signal_pipe = oc::core::UniqueHandle(signal_read),
        };
    }

    [[nodiscard]] std::expected<oc::runtime::TerminalHandoffChannels, oc::runtime::TerminalHandoffError> invoke_failure(
        const CLSID&,
        const oc::core::HandleView,
        oc::logging::Logger&) noexcept
    {
        if (g_hooks != nullptr)
        {
            ++g_hooks->invoker_calls;
        }
        return std::unexpected(oc::runtime::TerminalHandoffError{
            .context = L"test handoff failure",
            .win32_error = ERROR_GEN_FAILURE,
            .hresult = E_FAIL,
        });
    }

    [[nodiscard]] bool test_force_no_handoff_short_circuit()
    {
        HookState state{};
        g_hooks = &state;

        oc::logging::Logger logger(oc::logging::LogLevel::error);
        const auto result = oc::runtime::TerminalHandoff::try_establish_with(
            oc::core::HandleView(INVALID_HANDLE_VALUE),
            true,
            logger,
            &resolve_target,
            &invoke_success);

        g_hooks = nullptr;
        return result.has_value() && !result->has_value() && state.resolver_calls == 0 && state.invoker_calls == 0;
    }

    [[nodiscard]] bool test_no_target_skips_invoker()
    {
        HookState state{};
        g_hooks = &state;

        oc::logging::Logger logger(oc::logging::LogLevel::error);
        oc::core::UniqueHandle server(::CreateEventW(nullptr, TRUE, FALSE, nullptr));
        if (!server.valid())
        {
            g_hooks = nullptr;
            return false;
        }

        const auto result = oc::runtime::TerminalHandoff::try_establish_with(
            server.view(),
            false,
            logger,
            &resolve_none,
            &invoke_success);

        g_hooks = nullptr;
        return result.has_value() && !result->has_value() && state.resolver_calls == 1 && state.invoker_calls == 0;
    }

    [[nodiscard]] bool test_successful_invocation_returns_channels()
    {
        HookState state{};
        g_hooks = &state;

        oc::logging::Logger logger(oc::logging::LogLevel::error);
        oc::core::UniqueHandle server(::CreateEventW(nullptr, TRUE, FALSE, nullptr));
        if (!server.valid())
        {
            g_hooks = nullptr;
            return false;
        }

        auto result = oc::runtime::TerminalHandoff::try_establish_with(
            server.view(),
            false,
            logger,
            &resolve_target,
            &invoke_success);

        g_hooks = nullptr;
        if (!result.has_value() || !result->has_value())
        {
            return false;
        }

        auto channels = std::move(result->value());
        return channels.host_input.valid() &&
               channels.host_output.valid() &&
               channels.signal_pipe.valid() &&
               state.resolver_calls == 1 &&
               state.invoker_calls == 1;
    }

    [[nodiscard]] bool test_invoker_failure_propagates_error()
    {
        HookState state{};
        g_hooks = &state;

        oc::logging::Logger logger(oc::logging::LogLevel::error);
        oc::core::UniqueHandle server(::CreateEventW(nullptr, TRUE, FALSE, nullptr));
        if (!server.valid())
        {
            g_hooks = nullptr;
            return false;
        }

        const auto result = oc::runtime::TerminalHandoff::try_establish_with(
            server.view(),
            false,
            logger,
            &resolve_target,
            &invoke_failure);

        g_hooks = nullptr;
        return !result.has_value() &&
               result.error().context == L"test handoff failure" &&
               state.resolver_calls == 1 &&
               state.invoker_calls == 1;
    }
}

bool run_terminal_handoff_tests()
{
    if (!test_force_no_handoff_short_circuit())
    {
        fwprintf(stderr, L"[terminal handoff] test_force_no_handoff_short_circuit failed\n");
        return false;
    }
    if (!test_no_target_skips_invoker())
    {
        fwprintf(stderr, L"[terminal handoff] test_no_target_skips_invoker failed\n");
        return false;
    }
    if (!test_successful_invocation_returns_channels())
    {
        fwprintf(stderr, L"[terminal handoff] test_successful_invocation_returns_channels failed\n");
        return false;
    }
    if (!test_invoker_failure_propagates_error())
    {
        fwprintf(stderr, L"[terminal handoff] test_invoker_failure_propagates_error failed\n");
        return false;
    }

    return true;
}
