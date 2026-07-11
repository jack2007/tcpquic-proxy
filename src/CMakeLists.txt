# Copyright (c) Microsoft Corporation.
# Licensed under the MIT License.

function(tcpquic_copy_msquic_runtime target)
    if(NOT WIN32)
        return()
    endif()
    if(NOT TARGET msquic)
        return()
    endif()
    get_target_property(TCPQUIC_MSQUIC_TARGET_TYPE msquic TYPE)
    if(NOT TCPQUIC_MSQUIC_TARGET_TYPE STREQUAL "EXECUTABLE" AND
       NOT TCPQUIC_MSQUIC_TARGET_TYPE STREQUAL "SHARED_LIBRARY" AND
       NOT TCPQUIC_MSQUIC_TARGET_TYPE STREQUAL "MODULE_LIBRARY")
        return()
    endif()
    add_custom_command(TARGET ${target} POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "$<TARGET_FILE:msquic>"
            "$<TARGET_FILE_DIR:${target}>"
        COMMENT "Copy msquic.dll next to ${target}")
    if(BUILD_SHARED_LIBS)
        add_custom_command(TARGET ${target} POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "$<TARGET_FILE:msquic_platform>"
                "$<TARGET_FILE_DIR:${target}>"
            COMMENT "Copy msquic_platform.dll next to ${target}")
    endif()
endfunction()

function(add_tcpquic_executable target)
    add_executable(${target} ${ARGN})
    target_link_libraries(${target} PRIVATE inc warnings msquic logging base_link)
    if(BUILD_SHARED_LIBS)
        target_link_libraries(${target} PRIVATE msquic_platform)
    endif()
    set_property(TARGET ${target} PROPERTY FOLDER "tools")
    set_property(TARGET ${target} APPEND PROPERTY BUILD_RPATH "$ORIGIN")
    tcpquic_copy_msquic_runtime(${target})
endfunction()

function(tcpquic_apply_windows_win7_app_target target)
    if(NOT WIN32 OR NOT TCPQUIC_WINDOWS_TARGET_WIN7)
        return()
    endif()
    target_compile_definitions(${target} PRIVATE TCPQUIC_WINDOWS_TARGET_WIN7=1)
endfunction()

find_package(Threads REQUIRED)

add_library(cpp_httplib INTERFACE)
target_include_directories(cpp_httplib INTERFACE
    ${TCPQUIC_CPP_HTTPLIB_SOURCE_DIR})
if(MSVC)
    target_compile_options(cpp_httplib INTERFACE /wd4127)
endif()

set(CARES_STATIC ON CACHE BOOL "" FORCE)
set(CARES_SHARED OFF CACHE BOOL "" FORCE)
set(CARES_BUILD_TOOLS OFF CACHE BOOL "" FORCE)
set(CARES_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(CARES_INSTALL OFF CACHE BOOL "" FORCE)
add_subdirectory(${CMAKE_SOURCE_DIR}/third_party/c-ares ${CMAKE_BINARY_DIR}/c-ares)
if(WIN32 AND TARGET c-ares)
    # c-ares CMake feature detection can miss Winsock's struct timeval on MSVC.
    target_compile_definitions(c-ares PRIVATE HAVE_STRUCT_TIMEVAL=1)
endif()
if(NOT TARGET c-ares::cares AND TARGET c-ares)
    add_library(c-ares::cares ALIAS c-ares)
endif()
set(TCPQUIC_CARES_TARGET c-ares::cares)

set(TCPQUIC_SOURCE_INCLUDE_DIRS
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/acl
    ${CMAKE_CURRENT_SOURCE_DIR}/config
    ${CMAKE_CURRENT_SOURCE_DIR}/protocol
    ${CMAKE_CURRENT_SOURCE_DIR}/platform
    ${CMAKE_CURRENT_SOURCE_DIR}/ingress
    ${CMAKE_CURRENT_SOURCE_DIR}/tunnel
    ${CMAKE_CURRENT_SOURCE_DIR}/runtime
    ${MSQUIC_SOURCE_DIR}/src/inc
)

function(tcpquic_target_include_dirs target)
    target_include_directories(${target} PRIVATE ${TCPQUIC_SOURCE_INCLUDE_DIRS})
endfunction()

function(tcpquic_link_mimalloc target)
    if(TCPQUIC_USE_MIMALLOC)
        target_link_libraries(${target} PRIVATE mimalloc-static)
        target_include_directories(${target} PRIVATE
            "${CMAKE_SOURCE_DIR}/third_party/mimalloc/include")
        target_compile_definitions(${target} PRIVATE TCPQUIC_USE_MIMALLOC=1)
    else()
        target_compile_definitions(${target} PRIVATE TCPQUIC_USE_MIMALLOC=0)
    endif()
endfunction()

function(tcpquic_link_quic_address_deps target)
    if(WIN32)
        target_link_libraries(${target} PRIVATE Iphlpapi ntdll)
    endif()
endfunction()

set(TCPQUIC_PLATFORM_SOURCES)
if(WIN32)
    list(APPEND TCPQUIC_PLATFORM_SOURCES platform/platform_socket_win.cpp)
else()
    list(APPEND TCPQUIC_PLATFORM_SOURCES platform/platform_socket_posix.cpp)
endif()

set(TCPQUIC_PROXY_SOURCES
    main.cpp
    config/config.cpp
    config/tuning.cpp
    protocol/control_protocol.cpp
    acl/acl.cpp
    protocol/compress.cpp
    protocol/quic_address.cpp
    protocol/quic_session.cpp
    tunnel/tcp_dialer.cpp
    tunnel/tcp_tunnel.cpp
    tunnel/stream_lifetime.cpp
    tunnel/terminal_convergence.cpp
    tunnel/tunnel_registry.cpp
    ingress/http_connect_server.cpp
    ingress/proxy_auth.cpp
    ingress/socks5_server.cpp
    tunnel/relay.cpp
    runtime/thread_pool.cpp
    runtime/listen_socket.cpp
    tunnel/tunnel_reaper.cpp
    runtime/admin_auth.cpp
    runtime/admin_console.cpp
    runtime/admin_http.cpp
    runtime/admin_memory.cpp
    runtime/admin_config.cpp
    runtime/client_peer_runtime.cpp
    runtime/memory_stats.cpp
    runtime/runtime_config_file_store.cpp
    runtime/router_runtime.cpp
    runtime/relay_metrics.cpp
    runtime/server_admin.cpp
    runtime/server_runtime_config.cpp
    runtime/server_metrics.cpp
    runtime/speed_test.cpp
    runtime/shutdown.cpp
    runtime/trace.cpp
    platform/crash_dump.cpp
    platform/exe_path.cpp
    ${TCPQUIC_PLATFORM_SOURCES}
)

if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    list(APPEND TCPQUIC_PROXY_SOURCES
        ingress/client_ingress_reactor.cpp
        ingress/client_ingress_state.cpp
        tunnel/server_dial_reactor.cpp
        tunnel/ares_dns_resolver.cpp
        acl/acl_filter.cpp
        tunnel/relay_alloc.cpp
        tunnel/relay_buffer.cpp
        tunnel/linux_relay_worker.cpp
        runtime/linux_reactor.cpp
    )
endif()

if(CMAKE_SYSTEM_NAME STREQUAL "Darwin")
    list(APPEND TCPQUIC_PROXY_SOURCES
        ingress/client_ingress_reactor.cpp
        ingress/client_ingress_state.cpp
        tunnel/client_tunnel_open.cpp
        tunnel/server_dial_reactor.cpp
        tunnel/ares_dns_resolver.cpp
        acl/acl_filter.cpp
        tunnel/relay_alloc.cpp
        tunnel/relay_buffer.cpp
        tunnel/darwin_relay_worker.cpp
        runtime/darwin_reactor.cpp
    )
endif()

if(WIN32)
    list(APPEND TCPQUIC_PROXY_SOURCES
        ingress/client_ingress_reactor.cpp
        ingress/client_ingress_state.cpp
        tunnel/client_tunnel_open.cpp
        tunnel/server_dial_reactor.cpp
        tunnel/ares_dns_resolver.cpp
        acl/acl_filter.cpp
        tunnel/relay_alloc.cpp
        tunnel/relay_buffer.cpp
        tunnel/windows_relay_worker.cpp
        runtime/windows_reactor.cpp
    )
endif()

add_tcpquic_executable(tcpquic-proxy ${TCPQUIC_PROXY_SOURCES})
set_target_properties(tcpquic-proxy PROPERTIES OUTPUT_NAME raypx2)

if(WIN32)
    target_compile_definitions(tcpquic-proxy PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN TCPQUIC_WINDOWS_TLS_QUICTLS)
    target_link_libraries(tcpquic-proxy PRIVATE ws2_32)
    tcpquic_apply_windows_win7_app_target(tcpquic-proxy)
    if(TCPQUIC_WINDOWS_TARGET_WIN7)
        target_link_options(tcpquic-proxy PRIVATE "/SUBSYSTEM:CONSOLE,6.01")
    endif()
endif()

tcpquic_target_include_dirs(tcpquic-proxy)
tcpquic_link_quic_address_deps(tcpquic-proxy)
target_link_libraries(tcpquic-proxy PRIVATE libzstd spdlog::spdlog ${TCPQUIC_CARES_TARGET} cpp_httplib nlohmann_json::nlohmann_json)
if(TCPQUIC_HAS_CRASHPAD)
    target_link_libraries(tcpquic-proxy PRIVATE tcpquic_crashpad_client)
    target_compile_definitions(tcpquic-proxy PRIVATE TCPQUIC_HAS_CRASHPAD=1)
    add_custom_command(TARGET tcpquic-proxy POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${TCPQUIC_CRASHPAD_HANDLER}"
            "$<TARGET_FILE_DIR:tcpquic-proxy>"
        COMMENT "Copy crashpad_handler next to tcpquic-proxy")
else()
    target_compile_definitions(tcpquic-proxy PRIVATE TCPQUIC_HAS_CRASHPAD=0)
endif()
if(CMAKE_SYSTEM_NAME STREQUAL "Linux" OR CMAKE_SYSTEM_NAME STREQUAL "Darwin")
    target_link_libraries(tcpquic-proxy PRIVATE Threads::Threads)
endif()
target_compile_definitions(tcpquic-proxy PRIVATE TCPQUIC_HAS_ZSTD=1)
tcpquic_link_mimalloc(tcpquic-proxy)

option(TCPQUIC_PREFER_STATIC_OPENSSL_HELPER "Prefer a statically linked vendored openssl helper when supported" ON)

if(TARGET OpenSSL_Target AND DEFINED OpenSSLQuic_BINARY_DIR)
    if(WIN32)
        find_program(TCPQUIC_OPENSSL_BUILD_PROGRAM jom)
        if(NOT TCPQUIC_OPENSSL_BUILD_PROGRAM)
            set(TCPQUIC_OPENSSL_BUILD_PROGRAM nmake)
        endif()
        set(TCPQUIC_QUICTLS_BUILD_DIR
            "${OpenSSLQuic_BINARY_DIR}/submodules/quictls/$<IF:$<CONFIG:Debug>,debug,release>")
        set(TCPQUIC_OPENSSL_APP "${TCPQUIC_QUICTLS_BUILD_DIR}/apps/openssl.exe")
        add_custom_target(tcpquic-openssl
            COMMAND ${TCPQUIC_OPENSSL_BUILD_PROGRAM} "apps\\openssl.exe"
            COMMAND ${CMAKE_COMMAND} -E make_directory "$<TARGET_FILE_DIR:tcpquic-proxy>"
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "${TCPQUIC_OPENSSL_APP}"
                "$<TARGET_FILE_DIR:tcpquic-proxy>/openssl.exe"
            WORKING_DIRECTORY "${TCPQUIC_QUICTLS_BUILD_DIR}"
            DEPENDS OpenSSL_Target
            COMMENT "Build OpenSSL CLI next to tcpquic-proxy")
        add_dependencies(tcpquic-proxy tcpquic-openssl)
    else()
        find_program(TCPQUIC_MAKE_PROGRAM make)
        if(TCPQUIC_MAKE_PROGRAM)
            set(TCPQUIC_QUICTLS_BUILD_DIR "${OpenSSLQuic_BINARY_DIR}/submodules/quictls")
            set(TCPQUIC_OPENSSL_APP "${TCPQUIC_QUICTLS_BUILD_DIR}/apps/openssl")
            set(TCPQUIC_OPENSSL_LINK_ENV)
            set(TCPQUIC_OPENSSL_HELPER_COMMENT "Build OpenSSL CLI next to tcpquic-proxy")
            if(TCPQUIC_PREFER_STATIC_OPENSSL_HELPER)
                include(CMakePushCheckState)
                include(CheckCSourceCompiles)
                cmake_push_check_state(RESET)
                set(CMAKE_REQUIRED_LINK_OPTIONS "-static")
                set(CMAKE_REQUIRED_LIBRARIES dl pthread)
                set(CMAKE_REQUIRED_QUIET TRUE)
                check_c_source_compiles("int main(void) { return 0; }" TCPQUIC_CAN_STATIC_LINK_OPENSSL_HELPER)
                cmake_pop_check_state()

                if(TCPQUIC_CAN_STATIC_LINK_OPENSSL_HELPER)
                    set(TCPQUIC_OPENSSL_LINK_ENV "LDCMD=${CMAKE_C_COMPILER} -static")
                    set(TCPQUIC_OPENSSL_HELPER_COMMENT "Build static OpenSSL CLI next to tcpquic-proxy")
                    message(STATUS "Building vendored openssl helper with static system libraries")
                else()
                    message(WARNING "Static system libraries are unavailable; building vendored openssl helper dynamically")
                endif()
            endif()
            add_custom_target(tcpquic-openssl
                COMMAND ${CMAKE_COMMAND} -E env
                    ${TCPQUIC_OPENSSL_LINK_ENV}
                    ${TCPQUIC_MAKE_PROGRAM} -C "${TCPQUIC_QUICTLS_BUILD_DIR}" apps/openssl
                COMMAND ${CMAKE_COMMAND} -E make_directory "$<TARGET_FILE_DIR:tcpquic-proxy>"
                COMMAND ${CMAKE_COMMAND} -E copy_if_different
                    "${TCPQUIC_OPENSSL_APP}"
                    "$<TARGET_FILE_DIR:tcpquic-proxy>/openssl"
                DEPENDS OpenSSL_Target
                COMMENT "${TCPQUIC_OPENSSL_HELPER_COMMENT}")
            add_dependencies(tcpquic-proxy tcpquic-openssl)
        else()
            message(WARNING "make not found; tcpquic openssl helper will not be built")
        endif()
    endif()
endif()

set(TCPQUIC_TEST_LIBS libzstd)
set(TCPQUIC_TEST_INCLUDES)
set(TCPQUIC_TEST_DEFS TCPQUIC_HAS_ZSTD=1)

add_executable(tcpquic_relay_runtime_snapshot_test
    unittest/relay_runtime_snapshot_test.cpp
)
tcpquic_target_include_dirs(tcpquic_relay_runtime_snapshot_test)
target_link_libraries(tcpquic_relay_runtime_snapshot_test PRIVATE Threads::Threads)
target_compile_definitions(tcpquic_relay_runtime_snapshot_test PRIVATE TQ_UNIT_TESTING=1)
set_property(TARGET tcpquic_relay_runtime_snapshot_test PROPERTY FOLDER "tools")
set_property(TARGET tcpquic_relay_runtime_snapshot_test APPEND PROPERTY BUILD_RPATH "$ORIGIN")

add_tcpquic_executable(tcpquic_stream_lifetime_test
    unittest/stream_lifetime_test.cpp
    tunnel/stream_lifetime.cpp
    tunnel/terminal_convergence.cpp
)
tcpquic_target_include_dirs(tcpquic_stream_lifetime_test)
target_link_libraries(tcpquic_stream_lifetime_test PRIVATE Threads::Threads)
target_compile_definitions(tcpquic_stream_lifetime_test PRIVATE TQ_UNIT_TESTING=1)
tcpquic_link_mimalloc(tcpquic_stream_lifetime_test)
add_tcpquic_executable(tcpquic_terminal_convergence_test
    unittest/terminal_convergence_test.cpp
    tunnel/terminal_convergence.cpp
    tunnel/stream_lifetime.cpp)
tcpquic_target_include_dirs(tcpquic_terminal_convergence_test)
target_link_libraries(tcpquic_terminal_convergence_test PRIVATE Threads::Threads)
target_compile_definitions(tcpquic_terminal_convergence_test PRIVATE TQ_UNIT_TESTING=1)
tcpquic_link_mimalloc(tcpquic_terminal_convergence_test)
set(TCPQUIC_SERVER_TEST_SOURCES
    runtime/thread_pool.cpp
    tunnel/tcp_dialer.cpp
    config/tuning.cpp
    ${TCPQUIC_PLATFORM_SOURCES}
)
add_executable(tcpquic_tuning_test
    unittest/tuning_test.cpp
    unittest/trace_proxy_stub.cpp
    config/config.cpp
    config/tuning.cpp
    acl/acl.cpp
    protocol/control_protocol.cpp
    ingress/proxy_auth.cpp
    ${TCPQUIC_PLATFORM_SOURCES}
)
tcpquic_target_include_dirs(tcpquic_tuning_test)
target_link_libraries(tcpquic_tuning_test PRIVATE spdlog::spdlog nlohmann_json::nlohmann_json)
set_property(TARGET tcpquic_tuning_test PROPERTY FOLDER "tools")
set_property(TARGET tcpquic_tuning_test APPEND PROPERTY BUILD_RPATH "$ORIGIN")

add_executable(tcpquic_quic_settings_test
    unittest/quic_settings_test.cpp
    protocol/control_protocol.cpp
    protocol/quic_address.cpp
    protocol/quic_session.cpp
    config/tuning.cpp
    unittest/trace_proxy_stub.cpp
    ${TCPQUIC_PLATFORM_SOURCES}
)
tcpquic_target_include_dirs(tcpquic_quic_settings_test)
tcpquic_link_quic_address_deps(tcpquic_quic_settings_test)
target_link_libraries(tcpquic_quic_settings_test PRIVATE Threads::Threads inc warnings msquic logging base_link)
if(BUILD_SHARED_LIBS)
        target_link_libraries(tcpquic_quic_settings_test PRIVATE msquic_platform)
endif()
set_property(TARGET tcpquic_quic_settings_test PROPERTY FOLDER "tools")
set_property(TARGET tcpquic_quic_settings_test APPEND PROPERTY BUILD_RPATH "$ORIGIN")

add_executable(tcpquic_trace_network_stats_test
    unittest/trace_network_stats_test.cpp
    unittest/trace_metrics_stub.cpp
    runtime/trace.cpp
    platform/exe_path.cpp
    ${TCPQUIC_PLATFORM_SOURCES}
)
tcpquic_target_include_dirs(tcpquic_trace_network_stats_test)
target_link_libraries(tcpquic_trace_network_stats_test PRIVATE Threads::Threads inc warnings msquic logging base_link spdlog::spdlog)
if(BUILD_SHARED_LIBS)
    target_link_libraries(tcpquic_trace_network_stats_test PRIVATE msquic_platform)
endif()
set_property(TARGET tcpquic_trace_network_stats_test PROPERTY FOLDER "tools")
set_property(TARGET tcpquic_trace_network_stats_test APPEND PROPERTY BUILD_RPATH "$ORIGIN")

add_executable(tcpquic_relay_buffer_test
    unittest/relay_buffer_test.cpp
    tunnel/relay_buffer.cpp
    tunnel/relay_alloc.cpp
)
tcpquic_target_include_dirs(tcpquic_relay_buffer_test)
tcpquic_link_mimalloc(tcpquic_relay_buffer_test)
set_property(TARGET tcpquic_relay_buffer_test PROPERTY FOLDER "tools")
set_property(TARGET tcpquic_relay_buffer_test APPEND PROPERTY BUILD_RPATH "$ORIGIN")

add_executable(tcpquic_relay_alloc_test
    unittest/relay_alloc_test.cpp
    tunnel/relay_alloc.cpp
)
tcpquic_target_include_dirs(tcpquic_relay_alloc_test)
tcpquic_link_mimalloc(tcpquic_relay_alloc_test)
set_property(TARGET tcpquic_relay_alloc_test PROPERTY FOLDER "tools")
set_property(TARGET tcpquic_relay_alloc_test APPEND PROPERTY BUILD_RPATH "$ORIGIN")

add_executable(tcpquic_memory_stats_test
    unittest/memory_stats_test.cpp
    runtime/memory_stats.cpp
    tunnel/relay_alloc.cpp
)
tcpquic_target_include_dirs(tcpquic_memory_stats_test)
target_link_libraries(tcpquic_memory_stats_test PRIVATE nlohmann_json::nlohmann_json)
tcpquic_link_mimalloc(tcpquic_memory_stats_test)
set_property(TARGET tcpquic_memory_stats_test PROPERTY FOLDER "tools")
set_property(TARGET tcpquic_memory_stats_test APPEND PROPERTY BUILD_RPATH "$ORIGIN")

add_executable(tcpquic_runtime_config_file_store_test
    unittest/runtime_config_file_store_test.cpp
    runtime/runtime_config_file_store.cpp
)
tcpquic_target_include_dirs(tcpquic_runtime_config_file_store_test)
target_link_libraries(tcpquic_runtime_config_file_store_test PRIVATE nlohmann_json::nlohmann_json)
set_property(TARGET tcpquic_runtime_config_file_store_test PROPERTY FOLDER "tools")
set_property(TARGET tcpquic_runtime_config_file_store_test APPEND PROPERTY BUILD_RPATH "$ORIGIN")

set(TCPQUIC_ARES_DNS_RESOLVER_TEST_SOURCES
    unittest/ares_dns_resolver_test.cpp
    tunnel/ares_dns_resolver.cpp
    tunnel/relay_alloc.cpp
    ${TCPQUIC_PLATFORM_SOURCES}
)
if(WIN32)
    list(APPEND TCPQUIC_ARES_DNS_RESOLVER_TEST_SOURCES runtime/windows_reactor.cpp)
elseif(CMAKE_SYSTEM_NAME STREQUAL "Darwin")
    list(APPEND TCPQUIC_ARES_DNS_RESOLVER_TEST_SOURCES runtime/darwin_reactor.cpp)
else()
    list(APPEND TCPQUIC_ARES_DNS_RESOLVER_TEST_SOURCES runtime/linux_reactor.cpp)
endif()
add_executable(tcpquic_ares_dns_resolver_test ${TCPQUIC_ARES_DNS_RESOLVER_TEST_SOURCES})
tcpquic_target_include_dirs(tcpquic_ares_dns_resolver_test)
target_link_libraries(tcpquic_ares_dns_resolver_test PRIVATE Threads::Threads ${TCPQUIC_CARES_TARGET})
target_compile_definitions(tcpquic_ares_dns_resolver_test PRIVATE TQ_UNIT_TESTING=1)
tcpquic_link_mimalloc(tcpquic_ares_dns_resolver_test)
set_property(TARGET tcpquic_ares_dns_resolver_test PROPERTY FOLDER "tools")
set_property(TARGET tcpquic_ares_dns_resolver_test APPEND PROPERTY BUILD_RPATH "$ORIGIN")
if(WIN32)
    target_compile_definitions(tcpquic_ares_dns_resolver_test PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
    target_link_libraries(tcpquic_ares_dns_resolver_test PRIVATE ws2_32)
endif()

set(TCPQUIC_SERVER_DIAL_REACTOR_TEST_SOURCES
    unittest/server_dial_reactor_test.cpp
    tunnel/server_dial_reactor.cpp
    tunnel/ares_dns_resolver.cpp
    tunnel/relay_alloc.cpp
    acl/acl.cpp
    acl/acl_filter.cpp
    ${TCPQUIC_PLATFORM_SOURCES}
)
if(WIN32)
    list(APPEND TCPQUIC_SERVER_DIAL_REACTOR_TEST_SOURCES runtime/windows_reactor.cpp)
elseif(CMAKE_SYSTEM_NAME STREQUAL "Darwin")
    list(APPEND TCPQUIC_SERVER_DIAL_REACTOR_TEST_SOURCES runtime/darwin_reactor.cpp)
else()
    list(APPEND TCPQUIC_SERVER_DIAL_REACTOR_TEST_SOURCES runtime/linux_reactor.cpp)
endif()
add_executable(tcpquic_server_dial_reactor_test ${TCPQUIC_SERVER_DIAL_REACTOR_TEST_SOURCES})
tcpquic_target_include_dirs(tcpquic_server_dial_reactor_test)
target_link_libraries(tcpquic_server_dial_reactor_test PRIVATE Threads::Threads ${TCPQUIC_CARES_TARGET})
if(WIN32)
    target_link_libraries(tcpquic_server_dial_reactor_test PRIVATE inc warnings msquic logging base_link)
    if(BUILD_SHARED_LIBS)
        target_link_libraries(tcpquic_server_dial_reactor_test PRIVATE msquic_platform)
    endif()
    tcpquic_copy_msquic_runtime(tcpquic_server_dial_reactor_test)
endif()
target_compile_definitions(tcpquic_server_dial_reactor_test PRIVATE TQ_UNIT_TESTING=1)
tcpquic_link_mimalloc(tcpquic_server_dial_reactor_test)
set_property(TARGET tcpquic_server_dial_reactor_test PROPERTY FOLDER "tools")
set_property(TARGET tcpquic_server_dial_reactor_test APPEND PROPERTY BUILD_RPATH "$ORIGIN")

set(TCPQUIC_LINUX_TEST_TARGETS)
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    add_executable(tcpquic_linux_relay_worker_queue_test
        unittest/linux_relay_worker_queue_test.cpp
        unittest/trace_proxy_stub.cpp
        tunnel/linux_relay_worker.cpp
        tunnel/stream_lifetime.cpp
        tunnel/terminal_convergence.cpp
        tunnel/relay_buffer.cpp
        tunnel/relay_alloc.cpp
        config/tuning.cpp
        ${TCPQUIC_PLATFORM_SOURCES}
    )
    tcpquic_target_include_dirs(tcpquic_linux_relay_worker_queue_test)
    target_link_libraries(tcpquic_linux_relay_worker_queue_test PRIVATE Threads::Threads spdlog::spdlog)
    target_compile_definitions(tcpquic_linux_relay_worker_queue_test PRIVATE TQ_UNIT_TESTING=1)
    tcpquic_link_mimalloc(tcpquic_linux_relay_worker_queue_test)
    set_property(TARGET tcpquic_linux_relay_worker_queue_test PROPERTY FOLDER "tools")
    set_property(TARGET tcpquic_linux_relay_worker_queue_test APPEND PROPERTY BUILD_RPATH "$ORIGIN")

    add_executable(tcpquic_linux_relay_worker_io_test
        unittest/linux_relay_worker_io_test.cpp
        unittest/trace_proxy_stub.cpp
        tunnel/linux_relay_worker.cpp
        tunnel/stream_lifetime.cpp
        tunnel/terminal_convergence.cpp
        tunnel/relay_buffer.cpp
        tunnel/relay_alloc.cpp
        protocol/compress.cpp
        config/tuning.cpp
        ${TCPQUIC_PLATFORM_SOURCES}
    )
    tcpquic_target_include_dirs(tcpquic_linux_relay_worker_io_test)
    target_link_libraries(tcpquic_linux_relay_worker_io_test PRIVATE Threads::Threads ${TCPQUIC_TEST_LIBS} spdlog::spdlog)
    target_compile_definitions(tcpquic_linux_relay_worker_io_test PRIVATE ${TCPQUIC_TEST_DEFS} TQ_UNIT_TESTING=1)
    tcpquic_link_mimalloc(tcpquic_linux_relay_worker_io_test)
    set_property(TARGET tcpquic_linux_relay_worker_io_test PROPERTY FOLDER "tools")
    set_property(TARGET tcpquic_linux_relay_worker_io_test APPEND PROPERTY BUILD_RPATH "$ORIGIN")

    add_executable(tcpquic_linux_reactor_test
        unittest/linux_reactor_test.cpp
        runtime/linux_reactor.cpp
        ${TCPQUIC_PLATFORM_SOURCES}
    )
    tcpquic_target_include_dirs(tcpquic_linux_reactor_test)
    target_link_libraries(tcpquic_linux_reactor_test PRIVATE Threads::Threads)
    set_property(TARGET tcpquic_linux_reactor_test PROPERTY FOLDER "tools")
    set_property(TARGET tcpquic_linux_reactor_test APPEND PROPERTY BUILD_RPATH "$ORIGIN")
    add_executable(tcpquic_server_dial_reactor_worker_test
        unittest/server_dial_reactor_worker_test.cpp
        tunnel/server_dial_reactor.cpp
        tunnel/ares_dns_resolver.cpp
        tunnel/relay_alloc.cpp
        runtime/linux_reactor.cpp
        acl/acl.cpp
        acl/acl_filter.cpp
        ${TCPQUIC_PLATFORM_SOURCES}
    )
    tcpquic_target_include_dirs(tcpquic_server_dial_reactor_worker_test)
    target_link_libraries(tcpquic_server_dial_reactor_worker_test PRIVATE Threads::Threads ${TCPQUIC_CARES_TARGET})
    tcpquic_link_mimalloc(tcpquic_server_dial_reactor_worker_test)
    set_property(TARGET tcpquic_server_dial_reactor_worker_test PROPERTY FOLDER "tools")
    set_property(TARGET tcpquic_server_dial_reactor_worker_test APPEND PROPERTY BUILD_RPATH "$ORIGIN")

    add_executable(tcpquic_client_ingress_reactor_test
        unittest/client_ingress_reactor_test.cpp
        unittest/trace_proxy_stub.cpp
        ingress/client_ingress_reactor.cpp
        runtime/listen_socket.cpp
        ingress/client_ingress_state.cpp
        ingress/socks5_server.cpp
        ingress/http_connect_server.cpp
        ingress/proxy_auth.cpp
        protocol/control_protocol.cpp
        ${TCPQUIC_SERVER_TEST_SOURCES}
        runtime/linux_reactor.cpp
    )
    tcpquic_target_include_dirs(tcpquic_client_ingress_reactor_test)
    target_link_libraries(tcpquic_client_ingress_reactor_test PRIVATE Threads::Threads)
    target_compile_definitions(tcpquic_client_ingress_reactor_test PRIVATE TQ_UNIT_TESTING=1)
    set_property(TARGET tcpquic_client_ingress_reactor_test PROPERTY FOLDER "tools")
    set_property(TARGET tcpquic_client_ingress_reactor_test APPEND PROPERTY BUILD_RPATH "$ORIGIN")

    add_executable(tcpquic_relay_backend_selection_test
        unittest/relay_backend_selection_test.cpp
        unittest/trace_proxy_stub.cpp
        tunnel/relay.cpp
        tunnel/stream_lifetime.cpp
        config/tuning.cpp
        tunnel/linux_relay_worker.cpp
        tunnel/relay_buffer.cpp
        tunnel/relay_alloc.cpp
        ${TCPQUIC_PLATFORM_SOURCES}
    )
    tcpquic_target_include_dirs(tcpquic_relay_backend_selection_test)
    target_link_libraries(tcpquic_relay_backend_selection_test PRIVATE Threads::Threads inc warnings msquic logging base_link spdlog::spdlog)
    target_compile_options(tcpquic_relay_backend_selection_test PRIVATE -UNDEBUG)
    tcpquic_link_mimalloc(tcpquic_relay_backend_selection_test)
    set_property(TARGET tcpquic_relay_backend_selection_test PROPERTY FOLDER "tools")
    set_property(TARGET tcpquic_relay_backend_selection_test APPEND PROPERTY BUILD_RPATH "$ORIGIN")

    list(APPEND TCPQUIC_LINUX_TEST_TARGETS
        tcpquic_linux_relay_worker_queue_test
        tcpquic_linux_relay_worker_io_test
        tcpquic_linux_reactor_test
        tcpquic_server_dial_reactor_worker_test
        tcpquic_client_ingress_reactor_test
        tcpquic_relay_backend_selection_test
    )
endif()

if(CMAKE_SYSTEM_NAME STREQUAL "Darwin")
    add_executable(tcpquic_darwin_reactor_test
        unittest/darwin_reactor_test.cpp
        runtime/darwin_reactor.cpp
        ${TCPQUIC_PLATFORM_SOURCES}
    )
    tcpquic_target_include_dirs(tcpquic_darwin_reactor_test)
    target_link_libraries(tcpquic_darwin_reactor_test PRIVATE Threads::Threads)
    set_property(TARGET tcpquic_darwin_reactor_test PROPERTY FOLDER "tools")
    set_property(TARGET tcpquic_darwin_reactor_test APPEND PROPERTY BUILD_RPATH "$ORIGIN")

    add_executable(tcpquic_client_ingress_reactor_test
        unittest/client_ingress_reactor_test.cpp
        unittest/trace_proxy_stub.cpp
        ingress/client_ingress_reactor.cpp
        runtime/listen_socket.cpp
        ingress/client_ingress_state.cpp
        ingress/socks5_server.cpp
        ingress/http_connect_server.cpp
        ingress/proxy_auth.cpp
        protocol/control_protocol.cpp
        ${TCPQUIC_SERVER_TEST_SOURCES}
        runtime/darwin_reactor.cpp
        ${TCPQUIC_PLATFORM_SOURCES}
    )
    tcpquic_target_include_dirs(tcpquic_client_ingress_reactor_test)
    target_link_libraries(tcpquic_client_ingress_reactor_test PRIVATE Threads::Threads)
    target_compile_definitions(tcpquic_client_ingress_reactor_test PRIVATE TQ_UNIT_TESTING=1)
    target_compile_features(tcpquic_client_ingress_reactor_test PRIVATE cxx_std_17)
    set_property(TARGET tcpquic_client_ingress_reactor_test PROPERTY FOLDER "tools")
    set_property(TARGET tcpquic_client_ingress_reactor_test APPEND PROPERTY BUILD_RPATH "$ORIGIN")

    add_executable(tcpquic_darwin_relay_worker_queue_test
        unittest/darwin_relay_worker_queue_test.cpp
        unittest/trace_proxy_stub.cpp
        tunnel/darwin_relay_worker.cpp
        tunnel/stream_lifetime.cpp
        tunnel/relay_buffer.cpp
        tunnel/relay_alloc.cpp
        ${TCPQUIC_PLATFORM_SOURCES}
    )
    tcpquic_target_include_dirs(tcpquic_darwin_relay_worker_queue_test)
    target_compile_definitions(tcpquic_darwin_relay_worker_queue_test PRIVATE TCPQUIC_TESTING=1)
    target_link_libraries(tcpquic_darwin_relay_worker_queue_test PRIVATE Threads::Threads inc warnings msquic logging base_link libzstd spdlog::spdlog)
    tcpquic_link_mimalloc(tcpquic_darwin_relay_worker_queue_test)
    set_property(TARGET tcpquic_darwin_relay_worker_queue_test PROPERTY FOLDER "tools")
    set_property(TARGET tcpquic_darwin_relay_worker_queue_test APPEND PROPERTY BUILD_RPATH "$ORIGIN")

    add_executable(tcpquic_darwin_relay_worker_io_test
        unittest/darwin_relay_worker_io_test.cpp
        unittest/trace_proxy_stub.cpp
        tunnel/darwin_relay_worker.cpp
        tunnel/stream_lifetime.cpp
        tunnel/relay_alloc.cpp
        tunnel/relay_buffer.cpp
        tunnel/relay.cpp
        config/tuning.cpp
        ${TCPQUIC_PLATFORM_SOURCES}
    )
    tcpquic_target_include_dirs(tcpquic_darwin_relay_worker_io_test)
    target_compile_definitions(tcpquic_darwin_relay_worker_io_test PRIVATE TCPQUIC_TESTING=1 TQ_UNIT_TESTING=1)
    target_link_libraries(tcpquic_darwin_relay_worker_io_test PRIVATE Threads::Threads libzstd spdlog::spdlog msquic logging base_link inc warnings)
    tcpquic_link_mimalloc(tcpquic_darwin_relay_worker_io_test)
    set_property(TARGET tcpquic_darwin_relay_worker_io_test PROPERTY FOLDER "tools")
    set_property(TARGET tcpquic_darwin_relay_worker_io_test APPEND PROPERTY BUILD_RPATH "$ORIGIN")

    add_executable(tcpquic_darwin_relay_metrics_test
        unittest/darwin_relay_metrics_test.cpp
        unittest/trace_proxy_stub.cpp
        runtime/relay_metrics.cpp
        tunnel/darwin_relay_worker.cpp
        tunnel/stream_lifetime.cpp
        tunnel/relay_alloc.cpp
        tunnel/relay_buffer.cpp
        config/tuning.cpp
        ${TCPQUIC_PLATFORM_SOURCES}
    )
    tcpquic_target_include_dirs(tcpquic_darwin_relay_metrics_test)
    target_compile_definitions(tcpquic_darwin_relay_metrics_test PRIVATE TCPQUIC_TESTING=1 TQ_UNIT_TESTING=1)
    target_link_libraries(tcpquic_darwin_relay_metrics_test PRIVATE Threads::Threads libzstd spdlog::spdlog msquic logging base_link inc warnings nlohmann_json::nlohmann_json)
    tcpquic_link_mimalloc(tcpquic_darwin_relay_metrics_test)
    set_property(TARGET tcpquic_darwin_relay_metrics_test PROPERTY FOLDER "tools")
    set_property(TARGET tcpquic_darwin_relay_metrics_test APPEND PROPERTY BUILD_RPATH "$ORIGIN")
endif()

add_executable(tcpquic_thread_pool_test
    unittest/thread_pool_test.cpp
    runtime/thread_pool.cpp
)
tcpquic_target_include_dirs(tcpquic_thread_pool_test)
target_link_libraries(tcpquic_thread_pool_test PRIVATE Threads::Threads)
set_property(TARGET tcpquic_thread_pool_test PROPERTY FOLDER "tools")
set_property(TARGET tcpquic_thread_pool_test APPEND PROPERTY BUILD_RPATH "$ORIGIN")

add_executable(tcpquic_tunnel_reaper_test
    unittest/tunnel_reaper_test.cpp
    tunnel/tunnel_reaper.cpp
)
tcpquic_target_include_dirs(tcpquic_tunnel_reaper_test)
target_link_libraries(tcpquic_tunnel_reaper_test PRIVATE Threads::Threads)
set_property(TARGET tcpquic_tunnel_reaper_test PROPERTY FOLDER "tools")
set_property(TARGET tcpquic_tunnel_reaper_test APPEND PROPERTY BUILD_RPATH "$ORIGIN")

add_executable(tcpquic_control_test
    unittest/control_protocol_test.cpp
    protocol/control_protocol.cpp
)
tcpquic_target_include_dirs(tcpquic_control_test)
set_property(TARGET tcpquic_control_test PROPERTY FOLDER "tools")
set_property(TARGET tcpquic_control_test APPEND PROPERTY BUILD_RPATH "$ORIGIN")

add_executable(tcpquic_speed_test_test
    unittest/speed_test_test.cpp
    runtime/speed_test.cpp
    tunnel/stream_lifetime.cpp
    tunnel/tcp_tunnel.cpp
    tunnel/tcp_dialer.cpp
    tunnel/tunnel_registry.cpp
    config/tuning.cpp
    protocol/control_protocol.cpp
    acl/acl.cpp
    protocol/compress.cpp
    tunnel/relay.cpp
    tunnel/tunnel_reaper.cpp
    ${TCPQUIC_PLATFORM_SOURCES}
)
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    target_sources(tcpquic_speed_test_test PRIVATE
        tunnel/server_dial_reactor.cpp
        tunnel/ares_dns_resolver.cpp
        acl/acl_filter.cpp
        runtime/linux_reactor.cpp
        tunnel/relay_buffer.cpp
        tunnel/relay_alloc.cpp
        tunnel/linux_relay_worker.cpp
        tunnel/stream_lifetime.cpp
    )
endif()
if(CMAKE_SYSTEM_NAME STREQUAL "Darwin")
    target_sources(tcpquic_speed_test_test PRIVATE
        tunnel/server_dial_reactor.cpp
        tunnel/ares_dns_resolver.cpp
        runtime/darwin_reactor.cpp
        acl/acl_filter.cpp
        tunnel/relay_alloc.cpp
        tunnel/relay_buffer.cpp
        tunnel/darwin_relay_worker.cpp
    )
endif()
if(WIN32)
    target_sources(tcpquic_speed_test_test PRIVATE
        tunnel/server_dial_reactor.cpp
        tunnel/ares_dns_resolver.cpp
        runtime/windows_reactor.cpp
        acl/acl_filter.cpp
        tunnel/relay_alloc.cpp
        tunnel/relay_buffer.cpp
        tunnel/windows_relay_worker.cpp
    )
endif()
tcpquic_target_include_dirs(tcpquic_speed_test_test)
target_link_libraries(tcpquic_speed_test_test PRIVATE Threads::Threads ${TCPQUIC_TEST_LIBS} inc warnings msquic logging base_link spdlog::spdlog)
if(CMAKE_SYSTEM_NAME STREQUAL "Linux" OR CMAKE_SYSTEM_NAME STREQUAL "Darwin" OR WIN32)
    target_link_libraries(tcpquic_speed_test_test PRIVATE ${TCPQUIC_CARES_TARGET})
endif()
if(BUILD_SHARED_LIBS)
    target_link_libraries(tcpquic_speed_test_test PRIVATE msquic_platform)
endif()
target_compile_definitions(tcpquic_speed_test_test PRIVATE ${TCPQUIC_TEST_DEFS})
set_property(TARGET tcpquic_speed_test_test PROPERTY FOLDER "tools")
set_property(TARGET tcpquic_speed_test_test APPEND PROPERTY BUILD_RPATH "$ORIGIN")
if(CMAKE_SYSTEM_NAME STREQUAL "Linux" OR CMAKE_SYSTEM_NAME STREQUAL "Darwin")
    tcpquic_link_mimalloc(tcpquic_speed_test_test)
endif()
if(WIN32)
    tcpquic_link_mimalloc(tcpquic_speed_test_test)
    target_link_libraries(tcpquic_speed_test_test PRIVATE ws2_32)
endif()

add_executable(tcpquic_acl_test
    unittest/acl_test.cpp
    acl/acl.cpp
    ${TCPQUIC_PLATFORM_SOURCES}
)
tcpquic_target_include_dirs(tcpquic_acl_test)
set_property(TARGET tcpquic_acl_test PROPERTY FOLDER "tools")
set_property(TARGET tcpquic_acl_test APPEND PROPERTY BUILD_RPATH "$ORIGIN")

add_executable(tcpquic_acl_filter_test
    unittest/acl_filter_test.cpp
    acl/acl.cpp
    acl/acl_filter.cpp
    ${TCPQUIC_PLATFORM_SOURCES}
)
tcpquic_target_include_dirs(tcpquic_acl_filter_test)
set_property(TARGET tcpquic_acl_filter_test PROPERTY FOLDER "tools")
set_property(TARGET tcpquic_acl_filter_test APPEND PROPERTY BUILD_RPATH "$ORIGIN")

add_executable(tcpquic_config_router_test
    unittest/config_router_test.cpp
    unittest/trace_proxy_stub.cpp
    config/config.cpp
    config/tuning.cpp
    acl/acl.cpp
    protocol/control_protocol.cpp
    protocol/quic_address.cpp
    runtime/server_metrics.cpp
    ingress/proxy_auth.cpp
    ${TCPQUIC_PLATFORM_SOURCES}
)
tcpquic_target_include_dirs(tcpquic_config_router_test)
tcpquic_link_quic_address_deps(tcpquic_config_router_test)
target_link_libraries(tcpquic_config_router_test PRIVATE spdlog::spdlog nlohmann_json::nlohmann_json)
set_property(TARGET tcpquic_config_router_test PROPERTY FOLDER "tools")
set_property(TARGET tcpquic_config_router_test APPEND PROPERTY BUILD_RPATH "$ORIGIN")

add_executable(tcpquic_compress_test
    unittest/compress_test.cpp
    protocol/compress.cpp
    tunnel/relay_alloc.cpp
)
tcpquic_target_include_dirs(tcpquic_compress_test)
if(TCPQUIC_TEST_INCLUDES)
    target_include_directories(tcpquic_compress_test PRIVATE ${TCPQUIC_TEST_INCLUDES})
endif()
target_link_libraries(tcpquic_compress_test PRIVATE ${TCPQUIC_TEST_LIBS})
target_compile_definitions(tcpquic_compress_test PRIVATE ${TCPQUIC_TEST_DEFS})
tcpquic_link_mimalloc(tcpquic_compress_test)
set_property(TARGET tcpquic_compress_test PROPERTY FOLDER "tools")
set_property(TARGET tcpquic_compress_test APPEND PROPERTY BUILD_RPATH "$ORIGIN")

set(TCPQUIC_TUNNEL_TEST_SOURCES
    unittest/tcp_tunnel_test.cpp
    runtime/speed_test.cpp
    tunnel/tcp_tunnel.cpp
    tunnel/stream_lifetime.cpp
    tunnel/terminal_convergence.cpp
    tunnel/tcp_dialer.cpp
    tunnel/tunnel_registry.cpp
    config/tuning.cpp
    protocol/control_protocol.cpp
    acl/acl.cpp
    protocol/compress.cpp
    tunnel/relay.cpp
    tunnel/tunnel_reaper.cpp
    ${TCPQUIC_PLATFORM_SOURCES}
)
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    list(APPEND TCPQUIC_TUNNEL_TEST_SOURCES
        tunnel/server_dial_reactor.cpp
        tunnel/ares_dns_resolver.cpp
        runtime/linux_reactor.cpp
        acl/acl_filter.cpp
        tunnel/relay_buffer.cpp
        tunnel/relay_alloc.cpp
        tunnel/linux_relay_worker.cpp
    )
endif()
if(CMAKE_SYSTEM_NAME STREQUAL "Darwin")
    list(APPEND TCPQUIC_TUNNEL_TEST_SOURCES
        tunnel/server_dial_reactor.cpp
        tunnel/ares_dns_resolver.cpp
        acl/acl_filter.cpp
        tunnel/relay_alloc.cpp
        tunnel/relay_buffer.cpp
        tunnel/darwin_relay_worker.cpp
        runtime/darwin_reactor.cpp
    )
endif()
if(WIN32)
    list(APPEND TCPQUIC_TUNNEL_TEST_SOURCES
        tunnel/server_dial_reactor.cpp
        tunnel/ares_dns_resolver.cpp
        runtime/windows_reactor.cpp
        acl/acl_filter.cpp
        tunnel/relay_alloc.cpp
        tunnel/relay_buffer.cpp
        tunnel/windows_relay_worker.cpp
    )
endif()
add_executable(tcpquic_tunnel_test ${TCPQUIC_TUNNEL_TEST_SOURCES})
tcpquic_target_include_dirs(tcpquic_tunnel_test)
if(TCPQUIC_TEST_INCLUDES)
    target_include_directories(tcpquic_tunnel_test PRIVATE ${TCPQUIC_TEST_INCLUDES})
endif()
target_link_libraries(tcpquic_tunnel_test PRIVATE ${TCPQUIC_TEST_LIBS} Threads::Threads inc warnings msquic logging base_link spdlog::spdlog)
if(CMAKE_SYSTEM_NAME STREQUAL "Linux" OR CMAKE_SYSTEM_NAME STREQUAL "Darwin" OR WIN32)
    target_link_libraries(tcpquic_tunnel_test PRIVATE ${TCPQUIC_CARES_TARGET})
endif()
if(BUILD_SHARED_LIBS)
    target_link_libraries(tcpquic_tunnel_test PRIVATE msquic_platform)
endif()
target_compile_definitions(tcpquic_tunnel_test PRIVATE ${TCPQUIC_TEST_DEFS} TCPQUIC_TUNNEL_TESTING=1)
if(CMAKE_SYSTEM_NAME STREQUAL "Linux" OR CMAKE_SYSTEM_NAME STREQUAL "Darwin" OR WIN32)
    target_compile_definitions(tcpquic_tunnel_test PRIVATE TQ_UNIT_TESTING=1)
endif()
set_property(TARGET tcpquic_tunnel_test PROPERTY FOLDER "tools")
set_property(TARGET tcpquic_tunnel_test APPEND PROPERTY BUILD_RPATH "$ORIGIN")
if(CMAKE_SYSTEM_NAME STREQUAL "Linux" OR CMAKE_SYSTEM_NAME STREQUAL "Darwin")
    tcpquic_link_mimalloc(tcpquic_tunnel_test)
endif()
if(WIN32)
    tcpquic_link_mimalloc(tcpquic_tunnel_test)
    add_custom_target(tcpquic_tcp_tunnel_test ALL
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            $<TARGET_FILE:tcpquic_tunnel_test>
            $<TARGET_FILE_DIR:tcpquic_tunnel_test>/tcpquic_tcp_tunnel_test$<TARGET_FILE_SUFFIX:tcpquic_tunnel_test>
        DEPENDS tcpquic_tunnel_test)
else()
    add_custom_target(tcpquic_tcp_tunnel_test ALL
        COMMAND ${CMAKE_COMMAND} -E create_symlink
            $<TARGET_FILE_NAME:tcpquic_tunnel_test>
            $<TARGET_FILE_DIR:tcpquic_tunnel_test>/tcpquic_tcp_tunnel_test
        DEPENDS tcpquic_tunnel_test
        WORKING_DIRECTORY "$<TARGET_FILE_DIR:tcpquic_tunnel_test>")
endif()
set_property(TARGET tcpquic_tcp_tunnel_test PROPERTY FOLDER "tools")

add_executable(tcpquic_quic_session_reconnect_test
    unittest/quic_session_reconnect_test.cpp
    tunnel/terminal_convergence.cpp
    tunnel/stream_lifetime.cpp
    protocol/control_protocol.cpp
    protocol/quic_address.cpp
    protocol/quic_session.cpp
    config/tuning.cpp
    unittest/trace_proxy_stub.cpp
    ${TCPQUIC_PLATFORM_SOURCES}
)
tcpquic_target_include_dirs(tcpquic_quic_session_reconnect_test)
tcpquic_link_quic_address_deps(tcpquic_quic_session_reconnect_test)
target_link_libraries(tcpquic_quic_session_reconnect_test PRIVATE Threads::Threads inc warnings msquic logging base_link)
if(BUILD_SHARED_LIBS)
    target_link_libraries(tcpquic_quic_session_reconnect_test PRIVATE msquic_platform)
endif()
target_compile_definitions(tcpquic_quic_session_reconnect_test PRIVATE TQ_UNIT_TESTING=1)
set_property(TARGET tcpquic_quic_session_reconnect_test PROPERTY FOLDER "tools")
set_property(TARGET tcpquic_quic_session_reconnect_test APPEND PROPERTY BUILD_RPATH "$ORIGIN")

set(TCPQUIC_CLIENT_TUNNEL_OPEN_TEST_SOURCES
    unittest/client_tunnel_open_test.cpp
    tunnel/client_tunnel_open.cpp
    tunnel/tcp_tunnel.cpp
    tunnel/stream_lifetime.cpp
    tunnel/tcp_dialer.cpp
    tunnel/tunnel_registry.cpp
    tunnel/tunnel_reaper.cpp
    config/tuning.cpp
    protocol/control_protocol.cpp
    acl/acl.cpp
    protocol/compress.cpp
    ${TCPQUIC_PLATFORM_SOURCES}
)
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    list(APPEND TCPQUIC_CLIENT_TUNNEL_OPEN_TEST_SOURCES
        tunnel/server_dial_reactor.cpp
        tunnel/ares_dns_resolver.cpp
        runtime/linux_reactor.cpp
        acl/acl_filter.cpp
        tunnel/relay_buffer.cpp
        tunnel/relay_alloc.cpp
        tunnel/linux_relay_worker.cpp
    )
endif()
if(CMAKE_SYSTEM_NAME STREQUAL "Darwin")
    list(APPEND TCPQUIC_CLIENT_TUNNEL_OPEN_TEST_SOURCES
        tunnel/server_dial_reactor.cpp
        tunnel/ares_dns_resolver.cpp
        acl/acl_filter.cpp
        tunnel/relay_alloc.cpp
        tunnel/relay_buffer.cpp
        tunnel/darwin_relay_worker.cpp
        runtime/darwin_reactor.cpp
    )
endif()
if(WIN32)
    list(APPEND TCPQUIC_CLIENT_TUNNEL_OPEN_TEST_SOURCES
        tunnel/server_dial_reactor.cpp
        tunnel/ares_dns_resolver.cpp
        runtime/windows_reactor.cpp
        acl/acl_filter.cpp
        tunnel/relay_buffer.cpp
        tunnel/relay_alloc.cpp
    )
endif()
add_executable(tcpquic_client_tunnel_open_test ${TCPQUIC_CLIENT_TUNNEL_OPEN_TEST_SOURCES})
tcpquic_target_include_dirs(tcpquic_client_tunnel_open_test)
if(TCPQUIC_TEST_INCLUDES)
    target_include_directories(tcpquic_client_tunnel_open_test PRIVATE ${TCPQUIC_TEST_INCLUDES})
endif()
target_link_libraries(tcpquic_client_tunnel_open_test PRIVATE ${TCPQUIC_TEST_LIBS} Threads::Threads inc warnings msquic logging base_link spdlog::spdlog)
if(CMAKE_SYSTEM_NAME STREQUAL "Linux" OR CMAKE_SYSTEM_NAME STREQUAL "Darwin" OR WIN32)
    target_link_libraries(tcpquic_client_tunnel_open_test PRIVATE ${TCPQUIC_CARES_TARGET})
endif()
if(BUILD_SHARED_LIBS)
    target_link_libraries(tcpquic_client_tunnel_open_test PRIVATE msquic_platform)
endif()
target_compile_definitions(tcpquic_client_tunnel_open_test PRIVATE ${TCPQUIC_TEST_DEFS} TCPQUIC_TUNNEL_TESTING=1)
if(CMAKE_SYSTEM_NAME STREQUAL "Linux" OR WIN32)
    target_compile_definitions(tcpquic_client_tunnel_open_test PRIVATE TQ_UNIT_TESTING=1)
endif()
set_property(TARGET tcpquic_client_tunnel_open_test PROPERTY FOLDER "tools")
set_property(TARGET tcpquic_client_tunnel_open_test APPEND PROPERTY BUILD_RPATH "$ORIGIN")
if(CMAKE_SYSTEM_NAME STREQUAL "Linux" OR CMAKE_SYSTEM_NAME STREQUAL "Darwin")
    tcpquic_link_mimalloc(tcpquic_client_tunnel_open_test)
endif()
if(WIN32)
    tcpquic_link_mimalloc(tcpquic_client_tunnel_open_test)
endif()

set(TCPQUIC_POSIX_TEST_TARGETS)
if(NOT WIN32)
    add_executable(tcpquic_tcp_sink_bench_test
        unittest/tcp_sink_bench_test.cpp
        runtime/tcp_sink_bench.cpp
    )
    tcpquic_target_include_dirs(tcpquic_tcp_sink_bench_test)
    target_link_libraries(tcpquic_tcp_sink_bench_test PRIVATE Threads::Threads nlohmann_json::nlohmann_json)
    set_property(TARGET tcpquic_tcp_sink_bench_test PROPERTY FOLDER "tools")
    set_property(TARGET tcpquic_tcp_sink_bench_test APPEND PROPERTY BUILD_RPATH "$ORIGIN")

    add_executable(tcpquic_tcp_sink_bench
        runtime/tcp_sink_bench_main.cpp
        runtime/tcp_sink_bench.cpp
    )
    tcpquic_target_include_dirs(tcpquic_tcp_sink_bench)
    target_link_libraries(tcpquic_tcp_sink_bench PRIVATE Threads::Threads nlohmann_json::nlohmann_json)
    set_property(TARGET tcpquic_tcp_sink_bench PROPERTY FOLDER "tools")
    set_property(TARGET tcpquic_tcp_sink_bench APPEND PROPERTY BUILD_RPATH "$ORIGIN")

    list(APPEND TCPQUIC_POSIX_TEST_TARGETS tcpquic_tcp_sink_bench_test)
endif()

add_executable(tcpquic_proxy_auth_test
    unittest/proxy_auth_test.cpp
    ingress/proxy_auth.cpp
)
tcpquic_target_include_dirs(tcpquic_proxy_auth_test)
set_property(TARGET tcpquic_proxy_auth_test PROPERTY FOLDER "tools")
set_property(TARGET tcpquic_proxy_auth_test APPEND PROPERTY BUILD_RPATH "$ORIGIN")

add_executable(tcpquic_http_connect_test
    unittest/http_connect_server_test.cpp
    unittest/trace_proxy_stub.cpp
    ingress/http_connect_server.cpp
    ingress/proxy_auth.cpp
    protocol/control_protocol.cpp
    ${TCPQUIC_SERVER_TEST_SOURCES}
)
tcpquic_target_include_dirs(tcpquic_http_connect_test)
target_link_libraries(tcpquic_http_connect_test PRIVATE Threads::Threads)
set_property(TARGET tcpquic_http_connect_test PROPERTY FOLDER "tools")
set_property(TARGET tcpquic_http_connect_test APPEND PROPERTY BUILD_RPATH "$ORIGIN")

add_executable(tcpquic_socks5_test
    unittest/socks5_server_test.cpp
    unittest/trace_proxy_stub.cpp
    ingress/socks5_server.cpp
    ingress/proxy_auth.cpp
    protocol/control_protocol.cpp
    ${TCPQUIC_SERVER_TEST_SOURCES}
)
tcpquic_target_include_dirs(tcpquic_socks5_test)
target_link_libraries(tcpquic_socks5_test PRIVATE Threads::Threads)
set_property(TARGET tcpquic_socks5_test PROPERTY FOLDER "tools")
set_property(TARGET tcpquic_socks5_test APPEND PROPERTY BUILD_RPATH "$ORIGIN")

add_executable(tcpquic_client_ingress_state_test
    unittest/client_ingress_state_test.cpp
    unittest/trace_proxy_stub.cpp
    ingress/client_ingress_state.cpp
    ingress/socks5_server.cpp
    ingress/http_connect_server.cpp
    ingress/proxy_auth.cpp
    protocol/control_protocol.cpp
    ${TCPQUIC_SERVER_TEST_SOURCES}
)
tcpquic_target_include_dirs(tcpquic_client_ingress_state_test)
target_link_libraries(tcpquic_client_ingress_state_test PRIVATE Threads::Threads)
set_property(TARGET tcpquic_client_ingress_state_test PROPERTY FOLDER "tools")
set_property(TARGET tcpquic_client_ingress_state_test APPEND PROPERTY BUILD_RPATH "$ORIGIN")

add_executable(tcpquic_admin_http_test
    unittest/admin_http_test.cpp
    runtime/admin_auth.cpp
    runtime/admin_console.cpp
    runtime/admin_http.cpp
    ${TCPQUIC_PLATFORM_SOURCES}
)
tcpquic_target_include_dirs(tcpquic_admin_http_test)
target_link_libraries(tcpquic_admin_http_test PRIVATE Threads::Threads inc cpp_httplib nlohmann_json::nlohmann_json)
if(WIN32)
    target_compile_definitions(tcpquic_admin_http_test PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
    target_link_libraries(tcpquic_admin_http_test PRIVATE ws2_32)
    tcpquic_apply_windows_win7_app_target(tcpquic_admin_http_test)
endif()
set_property(TARGET tcpquic_admin_http_test PROPERTY FOLDER "tools")
set_property(TARGET tcpquic_admin_http_test APPEND PROPERTY BUILD_RPATH "$ORIGIN")

set(TCPQUIC_ROUTER_RUNTIME_TEST_SOURCES
    unittest/router_runtime_test.cpp
    tunnel/terminal_convergence.cpp
    runtime/router_runtime.cpp
    runtime/relay_metrics.cpp
    runtime/server_metrics.cpp
    runtime/admin_memory.cpp
    runtime/admin_config.cpp
    runtime/memory_stats.cpp
    runtime/admin_auth.cpp
    runtime/admin_console.cpp
    runtime/admin_http.cpp
    tunnel/tunnel_registry.cpp
    config/config.cpp
    config/tuning.cpp
    acl/acl.cpp
    protocol/control_protocol.cpp
    ingress/proxy_auth.cpp
    ${TCPQUIC_PLATFORM_SOURCES}
)
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    list(APPEND TCPQUIC_ROUTER_RUNTIME_TEST_SOURCES
        tunnel/relay_buffer.cpp
        tunnel/relay_alloc.cpp
        tunnel/linux_relay_worker.cpp
        tunnel/stream_lifetime.cpp
    )
endif()
if(CMAKE_SYSTEM_NAME STREQUAL "Darwin")
    list(APPEND TCPQUIC_ROUTER_RUNTIME_TEST_SOURCES
        tunnel/relay_buffer.cpp
        tunnel/relay_alloc.cpp
        tunnel/darwin_relay_worker.cpp
        tunnel/stream_lifetime.cpp
        runtime/darwin_reactor.cpp
    )
endif()
if(WIN32)
    list(APPEND TCPQUIC_ROUTER_RUNTIME_TEST_SOURCES
        tunnel/relay_alloc.cpp
        tunnel/relay_buffer.cpp
        tunnel/windows_relay_worker.cpp
        tunnel/stream_lifetime.cpp
        unittest/router_windows_trace_stub.cpp
    )
endif()
add_executable(tcpquic_router_runtime_test ${TCPQUIC_ROUTER_RUNTIME_TEST_SOURCES})
tcpquic_target_include_dirs(tcpquic_router_runtime_test)
target_link_libraries(tcpquic_router_runtime_test PRIVATE Threads::Threads inc warnings msquic logging base_link spdlog::spdlog cpp_httplib nlohmann_json::nlohmann_json)
if(BUILD_SHARED_LIBS)
    target_link_libraries(tcpquic_router_runtime_test PRIVATE msquic_platform)
endif()
set_property(TARGET tcpquic_router_runtime_test PROPERTY FOLDER "tools")
set_property(TARGET tcpquic_router_runtime_test APPEND PROPERTY BUILD_RPATH "$ORIGIN")
if(CMAKE_SYSTEM_NAME STREQUAL "Linux" OR CMAKE_SYSTEM_NAME STREQUAL "Darwin")
    tcpquic_link_mimalloc(tcpquic_router_runtime_test)
endif()
if(WIN32)
    tcpquic_link_mimalloc(tcpquic_router_runtime_test)
    tcpquic_apply_windows_win7_app_target(tcpquic_router_runtime_test)
endif()

add_executable(tcpquic_tunnel_registry_test
    unittest/tunnel_registry_test.cpp
    tunnel/tunnel_registry.cpp
)
tcpquic_target_include_dirs(tcpquic_tunnel_registry_test)
target_link_libraries(tcpquic_tunnel_registry_test PRIVATE Threads::Threads inc warnings)
set_property(TARGET tcpquic_tunnel_registry_test PROPERTY FOLDER "tools")
set_property(TARGET tcpquic_tunnel_registry_test APPEND PROPERTY BUILD_RPATH "$ORIGIN")

add_executable(tcpquic_server_admin_test
    unittest/server_admin_test.cpp
    tunnel/terminal_convergence.cpp
    tunnel/stream_lifetime.cpp
    runtime/admin_memory.cpp
    runtime/admin_config.cpp
    runtime/memory_stats.cpp
    runtime/runtime_config_file_store.cpp
    runtime/server_admin.cpp
    runtime/server_runtime_config.cpp
    acl/acl.cpp
    tunnel/relay_alloc.cpp
    ${TCPQUIC_PLATFORM_SOURCES}
)
tcpquic_target_include_dirs(tcpquic_server_admin_test)
target_compile_definitions(tcpquic_server_admin_test PRIVATE TQ_DEFINE_MSQUIC_API=1)
target_link_libraries(tcpquic_server_admin_test PRIVATE Threads::Threads inc warnings msquic nlohmann_json::nlohmann_json)
tcpquic_link_mimalloc(tcpquic_server_admin_test)
set_property(TARGET tcpquic_server_admin_test PROPERTY FOLDER "tools")
set_property(TARGET tcpquic_server_admin_test APPEND PROPERTY BUILD_RPATH "$ORIGIN")

add_executable(tcpquic_client_peer_runtime_test
    unittest/client_peer_runtime_test.cpp
    runtime/client_peer_runtime.cpp
    ingress/client_ingress_reactor.cpp
    ingress/client_ingress_state.cpp
    ingress/http_connect_server.cpp
    ingress/proxy_auth.cpp
    ingress/socks5_server.cpp
    protocol/quic_address.cpp
    protocol/quic_session.cpp
    protocol/compress.cpp
    protocol/control_protocol.cpp
    config/config.cpp
    config/tuning.cpp
    acl/acl.cpp
    tunnel/tcp_tunnel.cpp
    tunnel/tcp_dialer.cpp
    tunnel/tunnel_registry.cpp
    tunnel/tunnel_reaper.cpp
    tunnel/relay.cpp
    runtime/listen_socket.cpp
    runtime/relay_metrics.cpp
    runtime/thread_pool.cpp
    runtime/trace.cpp
    platform/exe_path.cpp
    ${TCPQUIC_PLATFORM_SOURCES}
)
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    target_sources(tcpquic_client_peer_runtime_test PRIVATE
        tunnel/server_dial_reactor.cpp
        tunnel/ares_dns_resolver.cpp
        acl/acl_filter.cpp
        tunnel/relay_alloc.cpp
        tunnel/relay_buffer.cpp
        tunnel/linux_relay_worker.cpp
        runtime/linux_reactor.cpp
    )
endif()
if(CMAKE_SYSTEM_NAME STREQUAL "Darwin")
    target_sources(tcpquic_client_peer_runtime_test PRIVATE
        tunnel/server_dial_reactor.cpp
        tunnel/ares_dns_resolver.cpp
        acl/acl_filter.cpp
        tunnel/relay_alloc.cpp
        tunnel/relay_buffer.cpp
        tunnel/darwin_relay_worker.cpp
        runtime/darwin_reactor.cpp
    )
endif()
if(WIN32)
    target_sources(tcpquic_client_peer_runtime_test PRIVATE
        tunnel/server_dial_reactor.cpp
        tunnel/ares_dns_resolver.cpp
        acl/acl_filter.cpp
        tunnel/relay_alloc.cpp
        tunnel/relay_buffer.cpp
        tunnel/windows_relay_worker.cpp
        runtime/windows_reactor.cpp
    )
endif()
tcpquic_target_include_dirs(tcpquic_client_peer_runtime_test)
target_compile_definitions(tcpquic_client_peer_runtime_test PRIVATE ${TCPQUIC_TEST_DEFS} TQ_UNIT_TESTING=1)
target_link_libraries(tcpquic_client_peer_runtime_test PRIVATE ${TCPQUIC_TEST_LIBS} Threads::Threads inc warnings msquic logging base_link spdlog::spdlog cpp_httplib nlohmann_json::nlohmann_json)
tcpquic_link_quic_address_deps(tcpquic_client_peer_runtime_test)
if(CMAKE_SYSTEM_NAME STREQUAL "Linux" OR CMAKE_SYSTEM_NAME STREQUAL "Darwin" OR WIN32)
    target_link_libraries(tcpquic_client_peer_runtime_test PRIVATE ${TCPQUIC_CARES_TARGET})
endif()
if(BUILD_SHARED_LIBS)
    target_link_libraries(tcpquic_client_peer_runtime_test PRIVATE msquic_platform)
endif()
if(CMAKE_SYSTEM_NAME STREQUAL "Linux" OR CMAKE_SYSTEM_NAME STREQUAL "Darwin")
    tcpquic_link_mimalloc(tcpquic_client_peer_runtime_test)
endif()
if(WIN32)
    tcpquic_link_mimalloc(tcpquic_client_peer_runtime_test)
endif()
set_property(TARGET tcpquic_client_peer_runtime_test PROPERTY FOLDER "tools")
set_property(TARGET tcpquic_client_peer_runtime_test APPEND PROPERTY BUILD_RPATH "$ORIGIN")

add_executable(tcpquic_production_linkage_guard_test
    unittest/production_linkage_guard_test.cpp
)
set_property(TARGET tcpquic_production_linkage_guard_test PROPERTY FOLDER "tools")
set_property(TARGET tcpquic_production_linkage_guard_test APPEND PROPERTY BUILD_RPATH "$ORIGIN")

add_executable(tcpquic_platform_socket_test
    unittest/platform_socket_test.cpp
    ${TCPQUIC_PLATFORM_SOURCES}
)
tcpquic_target_include_dirs(tcpquic_platform_socket_test)
set_property(TARGET tcpquic_platform_socket_test PROPERTY FOLDER "tools")
set_property(TARGET tcpquic_platform_socket_test APPEND PROPERTY BUILD_RPATH "$ORIGIN")
if(WIN32)
    target_compile_definitions(tcpquic_platform_socket_test PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
    target_link_libraries(tcpquic_platform_socket_test PRIVATE ws2_32)
endif()

if(WIN32)
    add_executable(tcpquic_client_ingress_reactor_test
        unittest/client_ingress_reactor_test.cpp
        unittest/trace_proxy_stub.cpp
        ingress/client_ingress_reactor.cpp
        runtime/listen_socket.cpp
        ingress/client_ingress_state.cpp
        ingress/socks5_server.cpp
        ingress/http_connect_server.cpp
        ingress/proxy_auth.cpp
        protocol/control_protocol.cpp
        ${TCPQUIC_SERVER_TEST_SOURCES}
        runtime/windows_reactor.cpp
    )
    tcpquic_target_include_dirs(tcpquic_client_ingress_reactor_test)
    target_link_libraries(tcpquic_client_ingress_reactor_test PRIVATE Threads::Threads ws2_32)
    target_compile_definitions(tcpquic_client_ingress_reactor_test PRIVATE TQ_UNIT_TESTING=1)
    set_property(TARGET tcpquic_client_ingress_reactor_test PROPERTY FOLDER "tools")
    set_property(TARGET tcpquic_client_ingress_reactor_test APPEND PROPERTY BUILD_RPATH "$ORIGIN")
endif()

set(TCPQUIC_WINDOWS_TEST_TARGETS)
if(WIN32)
    add_executable(tcpquic_windows_reactor_test
        unittest/windows_reactor_test.cpp
        runtime/windows_reactor.cpp
        platform/platform_socket_win.cpp
    )
    tcpquic_target_include_dirs(tcpquic_windows_reactor_test)
    target_link_libraries(tcpquic_windows_reactor_test PRIVATE ws2_32)
    target_compile_definitions(tcpquic_windows_reactor_test PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN TQ_UNIT_TESTING)
    set_property(TARGET tcpquic_windows_reactor_test PROPERTY FOLDER "tools")
    set_property(TARGET tcpquic_windows_reactor_test APPEND PROPERTY BUILD_RPATH "$ORIGIN")

    add_executable(tcpquic_windows_relay_worker_test
        unittest/windows_relay_worker_test.cpp
        unittest/trace_metrics_stub.cpp
        tunnel/windows_relay_worker.cpp
        tunnel/stream_lifetime.cpp
        tunnel/relay_buffer.cpp
        tunnel/relay_alloc.cpp
        protocol/compress.cpp
        runtime/trace.cpp
        platform/exe_path.cpp
        platform/platform_socket_win.cpp
    )
    tcpquic_target_include_dirs(tcpquic_windows_relay_worker_test)
    target_link_libraries(tcpquic_windows_relay_worker_test PRIVATE ws2_32 inc warnings msquic logging base_link ${TCPQUIC_TEST_LIBS} spdlog::spdlog)
    target_compile_definitions(tcpquic_windows_relay_worker_test PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN TQ_UNIT_TESTING ${TCPQUIC_TEST_DEFS})
    tcpquic_link_mimalloc(tcpquic_windows_relay_worker_test)
    list(APPEND TCPQUIC_WINDOWS_TEST_TARGETS
        tcpquic_client_ingress_reactor_test
        tcpquic_windows_reactor_test
        tcpquic_windows_relay_worker_test
    )
endif()

set(TCPQUIC_TEST_TARGETS
    tcpquic_relay_runtime_snapshot_test
    tcpquic_relay_buffer_test
    tcpquic_relay_alloc_test
    tcpquic_memory_stats_test
    tcpquic_runtime_config_file_store_test
    tcpquic_tuning_test
    tcpquic_quic_settings_test
    tcpquic_thread_pool_test
    tcpquic_tunnel_reaper_test
    tcpquic_tunnel_registry_test
    tcpquic_server_admin_test
    tcpquic_control_test
    tcpquic_ares_dns_resolver_test
    tcpquic_speed_test_test
    tcpquic_acl_test
    tcpquic_acl_filter_test
    tcpquic_config_router_test
    tcpquic_proxy_auth_test
    tcpquic_compress_test
    tcpquic_tunnel_test
    tcpquic_quic_session_reconnect_test
    tcpquic_client_tunnel_open_test
    tcpquic_http_connect_test
    tcpquic_socks5_test
    tcpquic_client_ingress_state_test
    tcpquic_admin_http_test
    tcpquic_router_runtime_test
    tcpquic_client_peer_runtime_test
    tcpquic_production_linkage_guard_test
    tcpquic_platform_socket_test
)
list(APPEND TCPQUIC_TEST_TARGETS tcpquic_server_dial_reactor_test)
list(APPEND TCPQUIC_TEST_TARGETS ${TCPQUIC_LINUX_TEST_TARGETS})
list(APPEND TCPQUIC_TEST_TARGETS ${TCPQUIC_POSIX_TEST_TARGETS})
list(APPEND TCPQUIC_TEST_TARGETS ${TCPQUIC_WINDOWS_TEST_TARGETS})

foreach(target IN LISTS TCPQUIC_TEST_TARGETS)
    target_compile_features(${target} PRIVATE cxx_std_17)
endforeach()

if(WIN32)
    foreach(target IN LISTS TCPQUIC_TEST_TARGETS)
        target_compile_definitions(${target} PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN TCPQUIC_WINDOWS_TLS_QUICTLS)
        target_link_libraries(${target} PRIVATE ws2_32)
    endforeach()

    set(TCPQUIC_MSQUIC_RUNTIME_TARGETS
        tcpquic_tunnel_test
        tcpquic_router_runtime_test
        tcpquic_client_peer_runtime_test
        ${TCPQUIC_WINDOWS_TEST_TARGETS}
    )
    foreach(target IN LISTS TCPQUIC_MSQUIC_RUNTIME_TARGETS)
        if(TARGET ${target})
            tcpquic_copy_msquic_runtime(${target})
        endif()
    endforeach()
endif()
