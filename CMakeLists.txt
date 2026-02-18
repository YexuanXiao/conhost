cmake_minimum_required(VERSION 3.27)

project(OpenConsoleNew LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

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
    src/serialization/fast_number.cpp
)

target_include_directories(oc_new_core
    PUBLIC
        src
        ${CMAKE_CURRENT_SOURCE_DIR}/../generated
        ${CMAKE_CURRENT_SOURCE_DIR}/../dep/Console
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

enable_testing()
add_subdirectory(tests)
