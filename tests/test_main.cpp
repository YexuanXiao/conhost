#include <cstdio>
#include <Windows.h>

bool run_console_arguments_tests();
bool run_console_attributes_tests();
bool run_console_connection_policy_tests();
bool run_config_tests();
bool run_logger_tests();
bool run_key_input_encoder_tests();
bool run_launch_policy_tests();
bool run_server_handle_validator_tests();
bool run_startup_command_tests();
bool run_fast_number_tests();
bool run_session_tests();
bool run_utf8_stream_decoder_tests();
bool run_signal_pipe_monitor_tests();
bool run_com_embedding_server_tests();
bool run_com_embedding_integration_tests();
bool run_host_signals_tests();
bool run_condrv_protocol_tests();
bool run_condrv_api_message_tests();
bool run_condrv_server_dispatch_tests();
bool run_condrv_input_wait_tests();
bool run_condrv_raw_io_tests();
bool run_condrv_screen_buffer_snapshot_tests();
bool run_condrv_vt_fuzz_tests();
bool run_dwrite_text_measurer_tests();
bool run_process_integration_tests();

int main()
{
    int failed = 0;
    const bool trace_enabled = ::GetEnvironmentVariableW(L"OPENCONSOLE_NEW_TEST_TRACE", nullptr, 0) != 0;
    const auto trace = [&](const wchar_t* name) {
        if (trace_enabled)
        {
            fwprintf(stderr, L"[TRACE] %ls\n", name);
            (void)fflush(stderr);
        }
    };

    trace(L"console arguments");
    if (!run_console_arguments_tests())
    {
        fwprintf(stderr, L"[FAIL] console arguments tests\n");
        ++failed;
    }

    trace(L"console attributes");
    if (!run_console_attributes_tests())
    {
        fwprintf(stderr, L"[FAIL] console attributes tests\n");
        ++failed;
    }

    trace(L"console connection policy");
    if (!run_console_connection_policy_tests())
    {
        fwprintf(stderr, L"[FAIL] console connection policy tests\n");
        ++failed;
    }

    trace(L"config");
    if (!run_config_tests())
    {
        fwprintf(stderr, L"[FAIL] config tests\n");
        ++failed;
    }

    trace(L"logger");
    if (!run_logger_tests())
    {
        fwprintf(stderr, L"[FAIL] logger tests\n");
        ++failed;
    }

    trace(L"key input encoder");
    if (!run_key_input_encoder_tests())
    {
        fwprintf(stderr, L"[FAIL] key input encoder tests\n");
        ++failed;
    }

    trace(L"launch policy");
    if (!run_launch_policy_tests())
    {
        fwprintf(stderr, L"[FAIL] launch policy tests\n");
        ++failed;
    }

    trace(L"server handle validator");
    if (!run_server_handle_validator_tests())
    {
        fwprintf(stderr, L"[FAIL] server handle validator tests\n");
        ++failed;
    }

    trace(L"startup command");
    if (!run_startup_command_tests())
    {
        fwprintf(stderr, L"[FAIL] startup command tests\n");
        ++failed;
    }

    trace(L"fast number");
    if (!run_fast_number_tests())
    {
        fwprintf(stderr, L"[FAIL] fast number tests\n");
        ++failed;
    }

    trace(L"session");
    if (!run_session_tests())
    {
        fwprintf(stderr, L"[FAIL] session tests\n");
        ++failed;
    }

    trace(L"utf8 stream decoder");
    if (!run_utf8_stream_decoder_tests())
    {
        fwprintf(stderr, L"[FAIL] utf8 stream decoder tests\n");
        ++failed;
    }

    trace(L"signal pipe monitor");
    if (!run_signal_pipe_monitor_tests())
    {
        fwprintf(stderr, L"[FAIL] signal pipe monitor tests\n");
        ++failed;
    }

    trace(L"com embedding server (in-proc)");
    if (!run_com_embedding_server_tests())
    {
        fwprintf(stderr, L"[FAIL] com embedding server tests\n");
        ++failed;
    }

    trace(L"com embedding integration (out-of-proc)");
    if (!run_com_embedding_integration_tests())
    {
        fwprintf(stderr, L"[FAIL] com embedding integration tests\n");
        ++failed;
    }

    trace(L"host signals");
    if (!run_host_signals_tests())
    {
        fwprintf(stderr, L"[FAIL] host signals tests\n");
        ++failed;
    }

    trace(L"condrv protocol");
    if (!run_condrv_protocol_tests())
    {
        fwprintf(stderr, L"[FAIL] condrv protocol tests\n");
        ++failed;
    }

    trace(L"condrv api message");
    if (!run_condrv_api_message_tests())
    {
        fwprintf(stderr, L"[FAIL] condrv api message tests\n");
        ++failed;
    }

    trace(L"condrv server dispatch");
    if (!run_condrv_server_dispatch_tests())
    {
        fwprintf(stderr, L"[FAIL] condrv server dispatch tests\n");
        ++failed;
    }

    trace(L"condrv input wait");
    if (!run_condrv_input_wait_tests())
    {
        fwprintf(stderr, L"[FAIL] condrv input wait tests\n");
        ++failed;
    }

    trace(L"condrv raw io");
    if (!run_condrv_raw_io_tests())
    {
        fwprintf(stderr, L"[FAIL] condrv raw io tests\n");
        ++failed;
    }

    trace(L"condrv screen buffer snapshot");
    if (!run_condrv_screen_buffer_snapshot_tests())
    {
        fwprintf(stderr, L"[FAIL] condrv screen buffer snapshot tests\n");
        ++failed;
    }

    trace(L"condrv vt fuzz");
    if (!run_condrv_vt_fuzz_tests())
    {
        fwprintf(stderr, L"[FAIL] condrv vt fuzz tests\n");
        ++failed;
    }

    trace(L"dwrite text measurer");
    if (!run_dwrite_text_measurer_tests())
    {
        fwprintf(stderr, L"[FAIL] dwrite text measurer tests\n");
        ++failed;
    }

    trace(L"process integration");
    if (!run_process_integration_tests())
    {
        fwprintf(stderr, L"[FAIL] process integration tests\n");
        ++failed;
    }

    if (failed == 0)
    {
        fwprintf(stderr, L"[PASS] all tests\n");
        return 0;
    }

    return 1;
}
