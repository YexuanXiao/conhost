add_executable(oc_new_tests
    test_main.cpp
    console_attributes_tests.cpp
    console_arguments_tests.cpp
    console_connection_policy_tests.cpp
    config_tests.cpp
    logger_tests.cpp
    key_input_encoder_tests.cpp
    launch_policy_tests.cpp
    server_handle_validator_tests.cpp
    startup_command_tests.cpp
    fast_number_tests.cpp
    session_tests.cpp
    utf8_stream_decoder_tests.cpp
    com_embedding_server_tests.cpp
    com_embedding_integration_tests.cpp
    host_signals_tests.cpp
    condrv_protocol_tests.cpp
    condrv_api_message_tests.cpp
    condrv_server_dispatch_tests.cpp
    condrv_input_wait_tests.cpp
    condrv_raw_io_tests.cpp
    condrv_screen_buffer_snapshot_tests.cpp
    condrv_vt_fuzz_tests.cpp
    dwrite_text_measurer_tests.cpp
    process_integration_tests.cpp
    signal_pipe_monitor_tests.cpp
)
target_link_libraries(oc_new_tests PRIVATE oc_new_core rpcrt4)

add_executable(oc_new_embedding_test_host WIN32
    embedding_test_host.cpp
)
target_link_libraries(oc_new_embedding_test_host PRIVATE oc_new_core)

add_executable(oc_new_stdio_probe
    stdio_probe.cpp
)

add_executable(oc_new_condrv_client_smoke
    condrv_client_smoke.cpp
)
target_link_libraries(oc_new_condrv_client_smoke PRIVATE oc_new_core)

add_executable(oc_new_condrv_client_input_events
    condrv_client_input_events.cpp
)
target_link_libraries(oc_new_condrv_client_input_events PRIVATE oc_new_core)

add_executable(oc_new_condrv_client_raw_read
    condrv_client_raw_read.cpp
)
target_link_libraries(oc_new_condrv_client_raw_read PRIVATE oc_new_core)

if(MSVC)
    target_compile_options(oc_new_tests PRIVATE
        /W4
        /WX
        /EHsc
        /GR-
        /permissive-
        /utf-8
        /Zc:__cplusplus
    )
endif()

add_dependencies(oc_new_tests openconsole_new oc_new_embedding_test_host oc_new_stdio_probe oc_new_condrv_client_smoke oc_new_condrv_client_input_events oc_new_condrv_client_raw_read console_new_proxy_dll)

add_test(NAME oc_new_tests COMMAND oc_new_tests)
