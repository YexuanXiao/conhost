set(OC_NEW_PROXY_ARCH_DIR "x64")
if(CMAKE_SYSTEM_PROCESSOR MATCHES "ARM64" OR CMAKE_GENERATOR_PLATFORM STREQUAL "ARM64" OR CMAKE_VS_PLATFORM_NAME STREQUAL "ARM64")
    set(OC_NEW_PROXY_ARCH_DIR "arm64")
endif()

add_library(oc_new_openconsole_proxy SHARED
    ${OC_NEW_PROXY_ARCH_DIR}/IConsoleHandoff_p.c
    ${OC_NEW_PROXY_ARCH_DIR}/IConsoleHandoff_i.c
    ${OC_NEW_PROXY_ARCH_DIR}/ITerminalHandoff_p.c
    ${OC_NEW_PROXY_ARCH_DIR}/ITerminalHandoff_i.c
    ${OC_NEW_PROXY_ARCH_DIR}/dlldata.c
)

# These files are MIDL-generated C sources but are compiled as C++ to keep the
# overall project C++-only and avoid a dedicated C toolchain configuration.
set_source_files_properties(
    ${OC_NEW_PROXY_ARCH_DIR}/IConsoleHandoff_p.c
    ${OC_NEW_PROXY_ARCH_DIR}/IConsoleHandoff_i.c
    ${OC_NEW_PROXY_ARCH_DIR}/ITerminalHandoff_p.c
    ${OC_NEW_PROXY_ARCH_DIR}/ITerminalHandoff_i.c
    ${OC_NEW_PROXY_ARCH_DIR}/dlldata.c
    PROPERTIES
        LANGUAGE CXX
)

target_compile_definitions(oc_new_openconsole_proxy PRIVATE
    UNICODE
    _UNICODE
    WIN32_LEAN_AND_MEAN
    NOMINMAX
    _WIN32_WINNT=0x0A00
    CINTERFACE
    COBJMACROS
    REGISTER_PROXY_DLL
    PROXY_CLSID_IS={0xDEC4804D,0x56D1,0x4F73,{0x9F,0xBE,0x68,0x28,0xE7,0xC8,0x5C,0x56}}
)

target_link_libraries(oc_new_openconsole_proxy PRIVATE
    rpcrt4
    ole32
    oleaut32
)

set_target_properties(oc_new_openconsole_proxy PROPERTIES
    OUTPUT_NAME oc_new_openconsole_proxy
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}
    RUNTIME_OUTPUT_DIRECTORY_DEBUG ${CMAKE_BINARY_DIR}
    RUNTIME_OUTPUT_DIRECTORY_RELEASE ${CMAKE_BINARY_DIR}
    RUNTIME_OUTPUT_DIRECTORY_RELWITHDEBINFO ${CMAKE_BINARY_DIR}
    RUNTIME_OUTPUT_DIRECTORY_MINSIZEREL ${CMAKE_BINARY_DIR}
    LINK_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/oc_new_openconsole_proxy.def
)

target_link_options(oc_new_openconsole_proxy PRIVATE
    /DEF:${CMAKE_CURRENT_SOURCE_DIR}/oc_new_openconsole_proxy.def
    /SECTION:.orpc,ERW
)

if(MSVC)
    target_compile_options(oc_new_openconsole_proxy PRIVATE
        /W4
        /WX
        /EHsc
        /GR-
        /permissive-
        /utf-8
        /Zc:__cplusplus
    )
else()
    target_compile_options(oc_new_openconsole_proxy PRIVATE
        -Wall
        -Wextra
        -Wpedantic
        -Werror
        -fno-rtti
    )
endif()

if(CMAKE_CXX_COMPILER_ID STREQUAL "Clang")
    # MIDL-generated proxy sources rely on a few Microsoft/COM ABI patterns
    # that trigger clang-cl warnings under `/W4 /WX`. Keep the warnings on for
    # the rest of the project, but silence them for the proxy/stub target.
    target_compile_options(oc_new_openconsole_proxy PRIVATE
        -clang:-Wno-pragma-pack
        -clang:-Wno-missing-braces
        -clang:-Wno-microsoft-cast
        -clang:-Wno-unused-const-variable
    )
endif()

# Super-target used by tests and tools that want the proxy/stub DLL available
# as a standalone build artifact (independent of the test executable).
add_custom_target(console_new_proxy_dll DEPENDS oc_new_openconsole_proxy)

# Backwards-compatible alias (historical name from the earlier test-only build).
add_custom_target(oc_new_openconsole_proxy_dll DEPENDS console_new_proxy_dll)
