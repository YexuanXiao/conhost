add_executable(oc_new_tests
    test_main.cpp
    console_arguments_tests.cpp
    config_tests.cpp
    logger_tests.cpp
    key_input_encoder_tests.cpp
    launch_policy_tests.cpp
    server_handle_validator_tests.cpp
    startup_command_tests.cpp
    fast_number_tests.cpp
    session_tests.cpp
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

find_program(OC_NEW_MIDL_EXE midl.exe REQUIRED)

set(OC_NEW_MIDL_OUT_DIR ${CMAKE_CURRENT_BINARY_DIR}/midl)
set(OC_NEW_MIDL_IDL ${CMAKE_CURRENT_SOURCE_DIR}/../../src/host/proxy/IConsoleHandoff.idl)
set(OC_NEW_MIDL_P ${OC_NEW_MIDL_OUT_DIR}/IConsoleHandoff_p.c)
set(OC_NEW_MIDL_I ${OC_NEW_MIDL_OUT_DIR}/IConsoleHandoff_i.c)
set(OC_NEW_MIDL_DLLDATA ${OC_NEW_MIDL_OUT_DIR}/dlldata.c)
set(OC_NEW_MIDL_H ${OC_NEW_MIDL_OUT_DIR}/IConsoleHandoff.h)

add_custom_command(
    OUTPUT
        ${OC_NEW_MIDL_P}
        ${OC_NEW_MIDL_I}
        ${OC_NEW_MIDL_DLLDATA}
        ${OC_NEW_MIDL_H}
    COMMAND ${CMAKE_COMMAND} -E make_directory ${OC_NEW_MIDL_OUT_DIR}
    COMMAND ${OC_NEW_MIDL_EXE} /nologo /Oicf /out ${OC_NEW_MIDL_OUT_DIR} ${OC_NEW_MIDL_IDL}
    DEPENDS
        ${OC_NEW_MIDL_IDL}
    VERBATIM
)

set(OC_NEW_PROXY_OBJ_P ${OC_NEW_MIDL_OUT_DIR}/IConsoleHandoff_p.obj)
set(OC_NEW_PROXY_OBJ_I ${OC_NEW_MIDL_OUT_DIR}/IConsoleHandoff_i.obj)
set(OC_NEW_PROXY_OBJ_DLLDATA ${OC_NEW_MIDL_OUT_DIR}/dlldata.obj)
set(OC_NEW_PROXY_DLL ${CMAKE_CURRENT_BINARY_DIR}/oc_new_openconsole_proxy.dll)
set(OC_NEW_PROXY_DEF ${CMAKE_CURRENT_SOURCE_DIR}/oc_new_openconsole_proxy.def)
set(OC_NEW_PROXY_CLSID_IS "{0xDEC4804D,0x56D1,0x4F73,{0x9F,0xBE,0x68,0x28,0xE7,0xC8,0x5C,0x56}}")

set(OC_NEW_PROXY_RUNTIME_FLAG /MD)
if(CMAKE_BUILD_TYPE STREQUAL "Debug")
    set(OC_NEW_PROXY_RUNTIME_FLAG /MDd)
endif()

add_custom_command(
    OUTPUT
        ${OC_NEW_PROXY_OBJ_P}
        ${OC_NEW_PROXY_OBJ_I}
        ${OC_NEW_PROXY_OBJ_DLLDATA}
    COMMAND cl.exe /nologo ${OC_NEW_PROXY_RUNTIME_FLAG} /Z7 /TC /Gz /c ${OC_NEW_MIDL_P} /I${OC_NEW_MIDL_OUT_DIR} /DREGISTER_PROXY_DLL /DPROXY_CLSID_IS=${OC_NEW_PROXY_CLSID_IS} /DWIN32 /D_WINDOWS /DUNICODE /D_UNICODE /D_WIN32_LEAN_AND_MEAN /DNOMINMAX /D_WIN32_WINNT=0x0A00 /w /Fo${OC_NEW_PROXY_OBJ_P}
    COMMAND cl.exe /nologo ${OC_NEW_PROXY_RUNTIME_FLAG} /Z7 /TC /Gz /c ${OC_NEW_MIDL_I} /I${OC_NEW_MIDL_OUT_DIR} /DREGISTER_PROXY_DLL /DPROXY_CLSID_IS=${OC_NEW_PROXY_CLSID_IS} /DWIN32 /D_WINDOWS /DUNICODE /D_UNICODE /D_WIN32_LEAN_AND_MEAN /DNOMINMAX /D_WIN32_WINNT=0x0A00 /w /Fo${OC_NEW_PROXY_OBJ_I}
    COMMAND cl.exe /nologo ${OC_NEW_PROXY_RUNTIME_FLAG} /Z7 /TC /Gz /c ${OC_NEW_MIDL_DLLDATA} /I${OC_NEW_MIDL_OUT_DIR} /DREGISTER_PROXY_DLL /DPROXY_CLSID_IS=${OC_NEW_PROXY_CLSID_IS} /DWIN32 /D_WINDOWS /DUNICODE /D_UNICODE /D_WIN32_LEAN_AND_MEAN /DNOMINMAX /D_WIN32_WINNT=0x0A00 /w /Fo${OC_NEW_PROXY_OBJ_DLLDATA}
    DEPENDS
        ${OC_NEW_MIDL_P}
        ${OC_NEW_MIDL_I}
        ${OC_NEW_MIDL_DLLDATA}
        ${OC_NEW_MIDL_H}
    VERBATIM
)

add_custom_command(
    OUTPUT
        ${OC_NEW_PROXY_DLL}
    COMMAND link.exe /nologo /DLL /SECTION:.orpc,ERW /OUT:${OC_NEW_PROXY_DLL} /DEF:${OC_NEW_PROXY_DEF} ${OC_NEW_PROXY_OBJ_P} ${OC_NEW_PROXY_OBJ_I} ${OC_NEW_PROXY_OBJ_DLLDATA} rpcrt4.lib ole32.lib
    DEPENDS
        ${OC_NEW_PROXY_OBJ_P}
        ${OC_NEW_PROXY_OBJ_I}
        ${OC_NEW_PROXY_OBJ_DLLDATA}
        ${OC_NEW_PROXY_DEF}
    VERBATIM
)

add_custom_target(oc_new_openconsole_proxy_dll DEPENDS ${OC_NEW_PROXY_DLL})
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

add_dependencies(oc_new_tests openconsole_new oc_new_embedding_test_host oc_new_stdio_probe oc_new_condrv_client_smoke oc_new_condrv_client_input_events oc_new_openconsole_proxy_dll)
add_dependencies(oc_new_embedding_test_host oc_new_openconsole_proxy_dll)

add_test(NAME oc_new_tests COMMAND oc_new_tests)
