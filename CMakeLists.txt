cmake_minimum_required(VERSION 3.27)

project(OpenConsoleNew LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

option(OC_NEW_ENABLE_ASAN "Enable AddressSanitizer for MSVC (cl.exe) builds" OFF)

add_library(oc_new_core STATIC
    src/app/application.cpp
    src/cli/console_arguments.cpp
    src/config/app_config.cpp
    src/condrv/command_history.cpp
    src/condrv/condrv_device_comm.cpp
    src/condrv/screen_buffer_snapshot.cpp
    src/condrv/condrv_server.cpp
    src/condrv/vt_input_decoder.cpp
    src/core/process_launcher.cpp
    src/localization/localizer.cpp
    src/logging/logger.cpp
    src/renderer/dwrite_text_measurer.cpp
    src/renderer/window_host.cpp
    src/runtime/key_input_encoder.cpp
    src/runtime/com_embedding_server.cpp
    src/runtime/default_terminal_host.cpp
    src/runtime/launch_policy.cpp
    src/runtime/legacy_conhost.cpp
    src/runtime/host_signal_input_thread.cpp
    src/runtime/signal_pipe_monitor.cpp
    src/runtime/session.cpp
    src/runtime/server_handle_validator.cpp
    src/runtime/startup_command.cpp
    src/runtime/terminal_handoff_host.cpp
    src/runtime/window_input_sink.cpp
    src/serialization/fast_number.cpp
)

target_include_directories(oc_new_core
    PUBLIC
        src
        src/Console
)

target_compile_definitions(oc_new_core
    PUBLIC
        UNICODE
        _UNICODE
        WIN32_LEAN_AND_MEAN
        NOMINMAX
        _WIN32_WINNT=0x0A00
)

if(MSVC)
    target_compile_options(oc_new_core PRIVATE
        /W4
        /WX
        /EHsc
        /GR-
        /permissive-
        /utf-8
        /Zc:__cplusplus
    )
else()
    target_compile_options(oc_new_core PRIVATE
        -Wall
        -Wextra
        -Wpedantic
        -Werror
        -fno-rtti
    )
endif()

target_link_libraries(oc_new_core
    PUBLIC
        advapi32
        d2d1
        dwrite
        kernel32
        shell32
        user32
)

add_executable(openconsole_new WIN32
    src/app/main.cpp
)

target_link_libraries(openconsole_new PRIVATE oc_new_core)

if(MSVC)
    target_compile_options(openconsole_new PRIVATE
        /W4
        /WX
        /EHsc
        /GR-
        /permissive-
        /utf-8
        /Zc:__cplusplus
    )
endif()

add_subdirectory(proxy)

enable_testing()
add_subdirectory(tests)

if(OC_NEW_ENABLE_ASAN)
    if(MSVC AND CMAKE_CXX_COMPILER_ID STREQUAL "MSVC")
        if(CMAKE_SIZEOF_VOID_P EQUAL 8)
            function(oc_new_enable_msvc_asan target_name)
                if(NOT TARGET ${target_name})
                    message(FATAL_ERROR "OC_NEW_ENABLE_ASAN expected target '${target_name}' to exist")
                endif()

                target_compile_options(${target_name} PRIVATE /fsanitize=address)
                if(TARGET ${target_name} AND (TARGET ${target_name}))
                    get_target_property(target_type ${target_name} TYPE)
                    if(NOT target_type STREQUAL "STATIC_LIBRARY")
                        target_link_options(${target_name} PRIVATE /fsanitize=address /INCREMENTAL:NO)
                    endif()
                endif()
            endfunction()

            oc_new_enable_msvc_asan(oc_new_core)
            oc_new_enable_msvc_asan(openconsole_new)
            oc_new_enable_msvc_asan(oc_new_tests)
            oc_new_enable_msvc_asan(oc_new_embedding_test_host)
            oc_new_enable_msvc_asan(oc_new_stdio_probe)
            oc_new_enable_msvc_asan(oc_new_condrv_client_smoke)
            oc_new_enable_msvc_asan(oc_new_condrv_client_input_events)
            oc_new_enable_msvc_asan(oc_new_condrv_client_raw_read)
        else()
            message(WARNING "OC_NEW_ENABLE_ASAN requested, but MSVC AddressSanitizer requires a 64-bit build.")
        endif()
    else()
        message(WARNING "OC_NEW_ENABLE_ASAN requested, but MSVC AddressSanitizer is supported only when using cl.exe.")
    endif()
endif()
