#include "logging/logger.hpp"
#include "runtime/com_embedding_server.hpp"

#include <Windows.h>
#include <objbase.h>

namespace
{
    constexpr DWORD kDefaultTimeoutMs = 15'000;

    [[nodiscard]] DWORD resolve_timeout_ms() noexcept
    {
        // Allow tests to override the timeout without adding new CLI surface.
        constexpr wchar_t kEnvName[] = L"OPENCONSOLE_NEW_TEST_EMBED_WAIT_MS";
        const DWORD required = ::GetEnvironmentVariableW(kEnvName, nullptr, 0);
        if (required == 0)
        {
            return kDefaultTimeoutMs;
        }

        // Use a fixed-size buffer to keep this helper compact. If the variable is
        // longer, fall back to the default.
        wchar_t buffer[64]{};
        constexpr DWORD buffer_chars = static_cast<DWORD>(sizeof(buffer) / sizeof(buffer[0]));
        const DWORD written = ::GetEnvironmentVariableW(kEnvName, buffer, buffer_chars);
        if (written == 0 || written >= buffer_chars)
        {
            return kDefaultTimeoutMs;
        }

        wchar_t* end = nullptr;
        const auto parsed = ::wcstoul(buffer, &end, 10);
        if (end == buffer)
        {
            return kDefaultTimeoutMs;
        }
        if (parsed == 0 || parsed > 600'000)
        {
            return kDefaultTimeoutMs;
        }

        return static_cast<DWORD>(parsed);
    }

    [[nodiscard]] std::expected<DWORD, oc::runtime::ComEmbeddingError> integration_runner(
        const oc::runtime::ComHandoffPayload& payload,
        oc::logging::Logger& logger) noexcept
    {
        // The integration test encodes its expected exit code in the attach
        // message's Function field; treat it as a transport sanity check.
        (void)logger;
        return static_cast<DWORD>(payload.attach.Function);
    }
}

int WINAPI wWinMain(HINSTANCE /*instance*/, HINSTANCE /*prev_instance*/, PWSTR /*command_line*/, int /*show_command*/)
{
    oc::logging::Logger logger(oc::logging::LogLevel::error);
    const DWORD timeout_ms = resolve_timeout_ms();

    const HRESULT coinit_hr = ::CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    if (FAILED(coinit_hr))
    {
        return static_cast<int>(HRESULT_CODE(coinit_hr));
    }
    struct CoUninit final
    {
        ~CoUninit() noexcept
        {
            ::CoUninitialize();
        }
    } co_uninit{};

    const HRESULT security_hr = ::CoInitializeSecurity(
        nullptr,
        -1,
        nullptr,
        nullptr,
        RPC_C_AUTHN_LEVEL_DEFAULT,
        RPC_C_IMP_LEVEL_IDENTIFY,
        nullptr,
        EOAC_NONE,
        nullptr);
    if (FAILED(security_hr))
    {
        return static_cast<int>(HRESULT_CODE(security_hr));
    }

    const auto result = oc::runtime::ComEmbeddingServer::run_with_runner(
        logger,
        timeout_ms,
        &integration_runner);

    if (!result)
    {
        // In tests we only need a deterministic failure code.
        return static_cast<int>(result.error().win32_error == 0 ? ERROR_GEN_FAILURE : result.error().win32_error);
    }

    return static_cast<int>(*result);
}
