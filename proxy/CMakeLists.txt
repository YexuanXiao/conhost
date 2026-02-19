add_library(oc_new_openconsole_proxy SHARED
    console_handoff_proxy.cpp
    console_handoff_iids.cpp
    dlldata.cpp
)

target_include_directories(oc_new_openconsole_proxy PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../src
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

# Super-target used by tests and tools that want the proxy/stub DLL available
# as a standalone build artifact (independent of the test executable).
add_custom_target(console_new_proxy_dll DEPENDS oc_new_openconsole_proxy)

# Backwards-compatible alias (historical name from the earlier test-only build).
add_custom_target(oc_new_openconsole_proxy_dll DEPENDS console_new_proxy_dll)
