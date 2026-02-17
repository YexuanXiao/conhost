#include "app/application.hpp"

#include "cli/console_arguments.hpp"
#include "config/app_config.hpp"
#include "core/console_writer.hpp"
#include "core/handle_view.hpp"
#include "localization/localizer.hpp"
#include "logging/logger.hpp"
#include "runtime/com_embedding_server.hpp"
#include "runtime/launch_policy.hpp"
#include "runtime/legacy_conhost.hpp"
#include "runtime/session.hpp"
#include "runtime/startup_command.hpp"

#include <Windows.h>

#include <memory>
#include <string>

// `app/application.cpp` is the top-level orchestration layer for the executable.
//
// Responsibilities:
// - Load configuration (environment + optional file) and select locale.
// - Initialize logging sinks (debug output and optional file).
// - Parse `openconsole_new` command line into a structured `ConsoleArguments`.
// - Construct `runtime::SessionOptions` and dispatch into `runtime::Session`.
//
// This file should remain intentionally free of low-level Win32 lifetime
// management. Those details live in `core` and `runtime` modules (see
// `new/docs/conhost_module_partition.md`).

namespace oc::app
{
    namespace
    {
        void write_localized_error(const localization::Localizer& localizer, const std::wstring_view detail)
        {
            std::wstring message(localizer.text(localization::StringId::parse_failed));
            message.append(L": ");
            message.append(detail);
            core::write_console_line(message);
        }

        [[nodiscard]] bool is_pipe_like_handle(const core::HandleView handle) noexcept
        {
            if (!handle)
            {
                return false;
            }
            return ::GetFileType(handle.get()) == FILE_TYPE_PIPE;
        }
    }

    int Application::run()
    {
        // Startup order is explicit and deterministic to keep behavior easy to
        // reason about during incremental porting:
        // config -> localization -> logging -> CLI parse -> runtime dispatch.
        auto config_result = config::ConfigLoader::load();
        if (!config_result)
        {
            localization::Localizer fallback_localizer(L"en-US");
            std::wstring error_message(fallback_localizer.text(localization::StringId::config_failed));
            error_message.append(L": ");
            error_message.append(config_result.error().message);
            core::write_console_line(error_message);
            return static_cast<int>(ERROR_BAD_CONFIGURATION);
        }

        config::AppConfig config = std::move(config_result.value());
        std::wstring locale = config.locale_override.empty() ? localization::Localizer::detect_user_locale() : config.locale_override;
        localization::Localizer localizer(std::move(locale));

        logging::Logger logger(config.minimum_log_level);
        if (config.enable_debug_sink)
        {
            logger.add_sink(std::make_shared<logging::DebugOutputSink>());
        }
        if (!config.log_file_path.empty())
        {
            auto file_sink = logging::FileLogSink::create(config.log_file_path);
            if (file_sink)
            {
                logger.add_sink(file_sink.value());
            }
            else
            {
                logger.log(logging::LogLevel::warning, L"File logging disabled; CreateFileW error={}", file_sink.error());
            }
        }

        logger.log(logging::LogLevel::info, L"{}", localizer.text(localization::StringId::startup));
        logger.log(logging::LogLevel::debug, L"Locale selected: {}", localizer.locale());

        auto parsed_args = cli::ConsoleArguments::parse(
            ::GetCommandLineW(),
            core::HandleView(::GetStdHandle(STD_INPUT_HANDLE)),
            core::HandleView(::GetStdHandle(STD_OUTPUT_HANDLE)));
        if (!parsed_args)
        {
            logger.log(logging::LogLevel::error, L"Parse error: {}", parsed_args.error().message);
            write_localized_error(localizer, parsed_args.error().message);
            return static_cast<int>(ERROR_INVALID_PARAMETER);
        }

        const cli::ConsoleArguments args = std::move(parsed_args.value());
        if (args.should_run_as_com_server())
        {
            logger.log(logging::LogLevel::info, L"Embedding mode requested; starting COM local server");
            auto com_server_result = runtime::ComEmbeddingServer::run(logger, config.embedding_wait_timeout_ms);
            if (!com_server_result)
            {
                logger.log(
                    logging::LogLevel::error,
                    L"COM server failed. context='{}', hr=0x{:08X}, error={}",
                    com_server_result.error().context,
                    static_cast<unsigned int>(com_server_result.error().hresult),
                    com_server_result.error().win32_error);
                if (!config.allow_embedding_passthrough)
                {
                    return static_cast<int>(com_server_result.error().win32_error == 0
                        ? ERROR_GEN_FAILURE
                        : com_server_result.error().win32_error);
                }

                logger.log(
                    logging::LogLevel::warning,
                    L"Falling back to embedding passthrough compatibility mode.");
            }
            else
            {
                ::SetProcessShutdownParameters(0, 0);
                return static_cast<int>(com_server_result.value());
            }
        }

        std::wstring effective_client_command = args.client_command_line();
        if (effective_client_command.empty())
        {
            if (args.should_create_server_handle())
            {
                effective_client_command = runtime::StartupCommand::resolve_default_client_command();
                logger.log(
                    logging::LogLevel::info,
                    L"No client command line specified; defaulting to {}",
                    effective_client_command);
            }
        }

        if (!effective_client_command.empty())
        {
            logger.log(logging::LogLevel::info, L"{}: {}", localizer.text(localization::StringId::launching_client), effective_client_command);
        }
        if (config.dry_run)
        {
            logger.log(logging::LogLevel::info, L"{}", localizer.text(localization::StringId::dry_run_notice));
            return 0;
        }

        runtime::SessionOptions session_options{};
        session_options.client_command_line = std::move(effective_client_command);
        session_options.create_server_handle = args.should_create_server_handle();
        session_options.server_handle = args.server_handle();
        session_options.signal_handle = args.signal_handle();
        session_options.host_input = args.vt_in_handle();
        session_options.host_output = args.vt_out_handle();
        session_options.width = args.width();
        session_options.height = args.height();
        session_options.headless = args.is_headless();
        session_options.inherit_cursor = args.inherit_cursor();
        session_options.text_measurement = args.text_measurement();
        session_options.force_no_handoff = args.force_no_handoff();

        if (!session_options.host_input)
        {
            session_options.host_input = core::HandleView(::GetStdHandle(STD_INPUT_HANDLE));
        }
        if (!session_options.host_output)
        {
            session_options.host_output = core::HandleView(::GetStdHandle(STD_OUTPUT_HANDLE));
        }

        // ConPTY mode is selected only when explicitly requested, headless
        // mode is active, or handles indicate a pipe-based terminal transport.
        session_options.in_conpty_mode =
            config.prefer_pseudoconsole &&
            (args.vt_mode_requested() ||
             args.is_headless() ||
             args.has_signal_handle() ||
             is_pipe_like_handle(session_options.host_input) ||
             is_pipe_like_handle(session_options.host_output));

        if (config.enable_legacy_conhost_path)
        {
            auto force_v2_value = runtime::LaunchPolicy::read_force_v2_registry();
            bool force_v2_enabled = true;
            if (force_v2_value)
            {
                force_v2_enabled = force_v2_value.value();
            }
            else
            {
                logger.log(
                    logging::LogLevel::warning,
                    L"Failed to read HKCU\\\\Console\\\\ForceV2 (error={}). Defaulting to V2.",
                    force_v2_value.error().win32_error);
            }

            const auto decision = runtime::LaunchPolicy::decide(
                session_options.in_conpty_mode,
                args.force_v1(),
                force_v2_enabled);

            if (decision.use_legacy_conhost)
            {
                if (args.should_create_server_handle())
                {
                    logger.log(logging::LogLevel::error, L"Legacy conhost path requires --server handle");
                    return static_cast<int>(ERROR_INVALID_PARAMETER);
                }

                auto activation = runtime::LegacyConhost::activate(args.server_handle());
                if (!activation)
                {
                    logger.log(
                        logging::LogLevel::error,
                        L"Legacy conhost activation failed. error={}",
                        activation.error().win32_error);
                    return static_cast<int>(activation.error().win32_error);
                }

                logger.log(logging::LogLevel::info, L"Legacy conhost activation succeeded");
                ::SetProcessShutdownParameters(0, 0);
                return 0;
            }
        }

        auto launch_result = runtime::Session::run(session_options, logger);
        if (!launch_result)
        {
            logger.log(
                logging::LogLevel::error,
                L"Launch failed. context='{}', error={}",
                launch_result.error().context,
                launch_result.error().win32_error);
            return static_cast<int>(launch_result.error().win32_error == 0 ? ERROR_GEN_FAILURE : launch_result.error().win32_error);
        }

        logger.log(logging::LogLevel::info, L"Client process exited with code {}", launch_result.value());
        ::SetProcessShutdownParameters(0, 0);
        return static_cast<int>(launch_result.value());
    }
}
