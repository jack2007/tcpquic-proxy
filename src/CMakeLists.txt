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

find_package(Threads REQUIRED)

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
    protocol/quic_session.cpp
    tunnel/tcp_dialer.cpp
    tunnel/tcp_tunnel.cpp
    tunnel/tunnel_registry.cpp
    ingress/http_connect_server.cpp
    ingress/proxy_auth.cpp
    ingress/socks5_server.cpp
    tunnel/relay.cpp
    runtime/thread_pool.cpp
    tunnel/tunnel_reaper.cpp
    runtime/warmup.cpp
    runtime/admin_http.cpp
    runtime/router_runtime.cpp
    runtime/relay_metrics.cpp
    runtime/server_metrics.cpp
    runtime/speed_test.cpp
    runtime/trace.cpp
    platform/exe_path.cpp
    ${TCPQUIC_PLATFORM_SOURCES}
)

if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    list(APPEND TCPQUIC_PROXY_SOURCES
        tunnel/relay_alloc.cpp
        tunnel/relay_buffer.cpp
        tunnel/linux_relay_worker.cpp
    )
endif()

if(WIN32)
    list(APPEND TCPQUIC_PROXY_SOURCES
        tunnel/relay_alloc.cpp
        tunnel/relay_buffer.cpp
        tunnel/windows_relay_worker.cpp
        platform/quic_credentials_win.cpp
    )
endif()

add_tcpquic_executable(tcpquic-proxy ${TCPQUIC_PROXY_SOURCES})

if(WIN32)
    target_compile_definitions(tcpquic-proxy PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN TCPQUIC_WINDOWS_TLS_QUICTLS)
    target_link_libraries(tcpquic-proxy PRIVATE ws2_32 crypt32)
endif()

tcpquic_target_include_dirs(tcpquic-proxy)
target_link_libraries(tcpquic-proxy PRIVATE libzstd spdlog::spdlog)
target_compile_definitions(tcpquic-proxy PRIVATE TCPQUIC_HAS_ZSTD=1)
tcpquic_link_mimalloc(tcpquic-proxy)

set(TCPQUIC_TEST_LIBS libzstd)
set(TCPQUIC_TEST_INCLUDES)
set(TCPQUIC_TEST_DEFS TCPQUIC_HAS_ZSTD=1)
set(TCPQUIC_SERVER_TEST_SOURCES
    runtime/thread_pool.cpp
    tunnel/tcp_dialer.cpp
    config/tuning.cpp
    ${TCPQUIC_PLATFORM_SOURCES}
)
add_executable(tcpquic_tuning_test
    unittest/tuning_test.cpp
    config/config.cpp
    config/tuning.cpp
    acl/acl.cpp
    ingress/proxy_auth.cpp
    ${TCPQUIC_PLATFORM_SOURCES}
)
tcpquic_target_include_dirs(tcpquic_tuning_test)
target_link_libraries(tcpquic_tuning_test PRIVATE spdlog::spdlog)
set_property(TARGET tcpquic_tuning_test PROPERTY FOLDER "tools")
set_property(TARGET tcpquic_tuning_test APPEND PROPERTY BUILD_RPATH "$ORIGIN")

add_executable(tcpquic_quic_settings_test
    unittest/quic_settings_test.cpp
    protocol/quic_session.cpp
    config/tuning.cpp
    unittest/trace_proxy_stub.cpp
    ${TCPQUIC_PLATFORM_SOURCES}
)
tcpquic_target_include_dirs(tcpquic_quic_settings_test)
target_link_libraries(tcpquic_quic_settings_test PRIVATE Threads::Threads inc warnings msquic logging base_link)
if(BUILD_SHARED_LIBS)
        target_link_libraries(tcpquic_quic_settings_test PRIVATE msquic_platform)
endif()
set_property(TARGET tcpquic_quic_settings_test PROPERTY FOLDER "tools")
set_property(TARGET tcpquic_quic_settings_test APPEND PROPERTY BUILD_RPATH "$ORIGIN")


add_executable(tcpquic_tcp_write_queue_test
    unittest/tcp_write_queue_test.cpp
    tunnel/tcp_write_queue.cpp
    ${TCPQUIC_PLATFORM_SOURCES}
)
tcpquic_target_include_dirs(tcpquic_tcp_write_queue_test)
target_link_libraries(tcpquic_tcp_write_queue_test PRIVATE Threads::Threads)
set_property(TARGET tcpquic_tcp_write_queue_test PROPERTY FOLDER "tools")
set_property(TARGET tcpquic_tcp_write_queue_test APPEND PROPERTY BUILD_RPATH "$ORIGIN")

add_executable(tcpquic_relay_buffer_test
    unittest/relay_buffer_test.cpp
    tunnel/relay_buffer.cpp
    tunnel/relay_alloc.cpp
)
tcpquic_target_include_dirs(tcpquic_relay_buffer_test)
tcpquic_link_mimalloc(tcpquic_relay_buffer_test)
set_property(TARGET tcpquic_relay_buffer_test PROPERTY FOLDER "tools")
set_property(TARGET tcpquic_relay_buffer_test APPEND PROPERTY BUILD_RPATH "$ORIGIN")

set(TCPQUIC_LINUX_TEST_TARGETS)
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    add_executable(tcpquic_linux_relay_worker_queue_test
        unittest/linux_relay_worker_queue_test.cpp
        tunnel/linux_relay_worker.cpp
        tunnel/relay_buffer.cpp
        tunnel/relay_alloc.cpp
        ${TCPQUIC_PLATFORM_SOURCES}
    )
    tcpquic_target_include_dirs(tcpquic_linux_relay_worker_queue_test)
    target_link_libraries(tcpquic_linux_relay_worker_queue_test PRIVATE Threads::Threads)
    tcpquic_link_mimalloc(tcpquic_linux_relay_worker_queue_test)
    set_property(TARGET tcpquic_linux_relay_worker_queue_test PROPERTY FOLDER "tools")
    set_property(TARGET tcpquic_linux_relay_worker_queue_test APPEND PROPERTY BUILD_RPATH "$ORIGIN")

    add_executable(tcpquic_linux_relay_worker_io_test
        unittest/linux_relay_worker_io_test.cpp
        tunnel/linux_relay_worker.cpp
        tunnel/relay_buffer.cpp
        tunnel/relay_alloc.cpp
        protocol/compress.cpp
        ${TCPQUIC_PLATFORM_SOURCES}
    )
    tcpquic_target_include_dirs(tcpquic_linux_relay_worker_io_test)
    target_link_libraries(tcpquic_linux_relay_worker_io_test PRIVATE Threads::Threads ${TCPQUIC_TEST_LIBS})
    target_compile_definitions(tcpquic_linux_relay_worker_io_test PRIVATE ${TCPQUIC_TEST_DEFS})
    tcpquic_link_mimalloc(tcpquic_linux_relay_worker_io_test)
    set_property(TARGET tcpquic_linux_relay_worker_io_test PROPERTY FOLDER "tools")
    set_property(TARGET tcpquic_linux_relay_worker_io_test APPEND PROPERTY BUILD_RPATH "$ORIGIN")

    add_executable(tcpquic_relay_backend_selection_test
        unittest/relay_backend_selection_test.cpp
        tunnel/relay.cpp
        config/tuning.cpp
        tunnel/linux_relay_worker.cpp
        tunnel/relay_buffer.cpp
        tunnel/relay_alloc.cpp
        ${TCPQUIC_PLATFORM_SOURCES}
    )
    tcpquic_target_include_dirs(tcpquic_relay_backend_selection_test)
    target_link_libraries(tcpquic_relay_backend_selection_test PRIVATE Threads::Threads inc warnings msquic logging base_link)
    target_compile_options(tcpquic_relay_backend_selection_test PRIVATE -UNDEBUG)
    tcpquic_link_mimalloc(tcpquic_relay_backend_selection_test)
    set_property(TARGET tcpquic_relay_backend_selection_test PROPERTY FOLDER "tools")
    set_property(TARGET tcpquic_relay_backend_selection_test APPEND PROPERTY BUILD_RPATH "$ORIGIN")

    list(APPEND TCPQUIC_LINUX_TEST_TARGETS
        tcpquic_linux_relay_worker_queue_test
        tcpquic_linux_relay_worker_io_test
        tcpquic_relay_backend_selection_test
    )
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
    tunnel/tcp_tunnel.cpp
    tunnel/tcp_dialer.cpp
    tunnel/tunnel_registry.cpp
    config/tuning.cpp
    protocol/control_protocol.cpp
    acl/acl.cpp
    protocol/compress.cpp
    tunnel/relay.cpp
    tunnel/tcp_write_queue.cpp
    tunnel/tunnel_reaper.cpp
    ${TCPQUIC_PLATFORM_SOURCES}
)
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    target_sources(tcpquic_speed_test_test PRIVATE
        tunnel/relay_buffer.cpp
        tunnel/relay_alloc.cpp
        tunnel/linux_relay_worker.cpp
    )
endif()
if(WIN32)
    target_sources(tcpquic_speed_test_test PRIVATE
        tunnel/relay_alloc.cpp
        tunnel/relay_buffer.cpp
        tunnel/windows_relay_worker.cpp
    )
endif()
tcpquic_target_include_dirs(tcpquic_speed_test_test)
target_link_libraries(tcpquic_speed_test_test PRIVATE Threads::Threads ${TCPQUIC_TEST_LIBS} inc warnings msquic logging base_link)
if(BUILD_SHARED_LIBS)
    target_link_libraries(tcpquic_speed_test_test PRIVATE msquic_platform)
endif()
target_compile_definitions(tcpquic_speed_test_test PRIVATE ${TCPQUIC_TEST_DEFS})
set_property(TARGET tcpquic_speed_test_test PROPERTY FOLDER "tools")
set_property(TARGET tcpquic_speed_test_test APPEND PROPERTY BUILD_RPATH "$ORIGIN")
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
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

add_executable(tcpquic_config_router_test
    unittest/config_router_test.cpp
    config/config.cpp
    config/tuning.cpp
    acl/acl.cpp
    ingress/proxy_auth.cpp
    ${TCPQUIC_PLATFORM_SOURCES}
)
tcpquic_target_include_dirs(tcpquic_config_router_test)
target_link_libraries(tcpquic_config_router_test PRIVATE spdlog::spdlog)
set_property(TARGET tcpquic_config_router_test PROPERTY FOLDER "tools")
set_property(TARGET tcpquic_config_router_test APPEND PROPERTY BUILD_RPATH "$ORIGIN")

add_executable(tcpquic_compress_test
    unittest/compress_test.cpp
    protocol/compress.cpp
)
tcpquic_target_include_dirs(tcpquic_compress_test)
if(TCPQUIC_TEST_INCLUDES)
    target_include_directories(tcpquic_compress_test PRIVATE ${TCPQUIC_TEST_INCLUDES})
endif()
target_link_libraries(tcpquic_compress_test PRIVATE ${TCPQUIC_TEST_LIBS})
target_compile_definitions(tcpquic_compress_test PRIVATE ${TCPQUIC_TEST_DEFS})
set_property(TARGET tcpquic_compress_test PROPERTY FOLDER "tools")
set_property(TARGET tcpquic_compress_test APPEND PROPERTY BUILD_RPATH "$ORIGIN")

set(TCPQUIC_TUNNEL_TEST_SOURCES
    unittest/tcp_tunnel_test.cpp
    runtime/speed_test.cpp
    tunnel/tcp_tunnel.cpp
    tunnel/tcp_dialer.cpp
    tunnel/tunnel_registry.cpp
    config/tuning.cpp
    protocol/control_protocol.cpp
    acl/acl.cpp
    protocol/compress.cpp
    tunnel/relay.cpp
    tunnel/tcp_write_queue.cpp
    tunnel/tunnel_reaper.cpp
    ${TCPQUIC_PLATFORM_SOURCES}
)
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    list(APPEND TCPQUIC_TUNNEL_TEST_SOURCES
        tunnel/relay_buffer.cpp
        tunnel/relay_alloc.cpp
        tunnel/linux_relay_worker.cpp
    )
endif()
if(WIN32)
    list(APPEND TCPQUIC_TUNNEL_TEST_SOURCES
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
target_link_libraries(tcpquic_tunnel_test PRIVATE ${TCPQUIC_TEST_LIBS} Threads::Threads inc warnings msquic logging base_link)
if(BUILD_SHARED_LIBS)
    target_link_libraries(tcpquic_tunnel_test PRIVATE msquic_platform)
endif()
target_compile_definitions(tcpquic_tunnel_test PRIVATE ${TCPQUIC_TEST_DEFS} TCPQUIC_TUNNEL_TESTING=1)
set_property(TARGET tcpquic_tunnel_test PROPERTY FOLDER "tools")
set_property(TARGET tcpquic_tunnel_test APPEND PROPERTY BUILD_RPATH "$ORIGIN")
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
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

set(TCPQUIC_POSIX_TEST_TARGETS)
if(NOT WIN32)
    add_executable(tcpquic_blocking_relay_demo_test
        unittest/blocking_relay_demo_test.cpp
        tunnel/relay_blocking_demo.cpp
        tunnel/tcp_write_queue.cpp
        config/tuning.cpp
        ${TCPQUIC_PLATFORM_SOURCES}
    )
    tcpquic_target_include_dirs(tcpquic_blocking_relay_demo_test)
    target_link_libraries(tcpquic_blocking_relay_demo_test PRIVATE Threads::Threads inc warnings msquic logging base_link)
    set_property(TARGET tcpquic_blocking_relay_demo_test PROPERTY FOLDER "tools")
    set_property(TARGET tcpquic_blocking_relay_demo_test APPEND PROPERTY BUILD_RPATH "$ORIGIN")
    list(APPEND TCPQUIC_POSIX_TEST_TARGETS tcpquic_blocking_relay_demo_test)

    add_executable(tcpquic_tcp_sink_bench_test
        unittest/tcp_sink_bench_test.cpp
        runtime/tcp_sink_bench.cpp
    )
    tcpquic_target_include_dirs(tcpquic_tcp_sink_bench_test)
    target_link_libraries(tcpquic_tcp_sink_bench_test PRIVATE Threads::Threads)
    set_property(TARGET tcpquic_tcp_sink_bench_test PROPERTY FOLDER "tools")
    set_property(TARGET tcpquic_tcp_sink_bench_test APPEND PROPERTY BUILD_RPATH "$ORIGIN")

    add_executable(tcpquic_tcp_sink_bench
        runtime/tcp_sink_bench_main.cpp
        runtime/tcp_sink_bench.cpp
    )
    tcpquic_target_include_dirs(tcpquic_tcp_sink_bench)
    target_link_libraries(tcpquic_tcp_sink_bench PRIVATE Threads::Threads)
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

add_executable(tcpquic_admin_http_test
    unittest/admin_http_test.cpp
    runtime/admin_http.cpp
    ${TCPQUIC_PLATFORM_SOURCES}
)
tcpquic_target_include_dirs(tcpquic_admin_http_test)
target_link_libraries(tcpquic_admin_http_test PRIVATE Threads::Threads)
if(WIN32)
    target_compile_definitions(tcpquic_admin_http_test PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
    target_link_libraries(tcpquic_admin_http_test PRIVATE ws2_32)
endif()
set_property(TARGET tcpquic_admin_http_test PROPERTY FOLDER "tools")
set_property(TARGET tcpquic_admin_http_test APPEND PROPERTY BUILD_RPATH "$ORIGIN")

set(TCPQUIC_ROUTER_RUNTIME_TEST_SOURCES
    unittest/router_runtime_test.cpp
    runtime/router_runtime.cpp
    runtime/relay_metrics.cpp
    runtime/server_metrics.cpp
    runtime/admin_http.cpp
    config/config.cpp
    config/tuning.cpp
    acl/acl.cpp
    ingress/proxy_auth.cpp
    ${TCPQUIC_PLATFORM_SOURCES}
)
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    list(APPEND TCPQUIC_ROUTER_RUNTIME_TEST_SOURCES
        tunnel/relay_buffer.cpp
        tunnel/relay_alloc.cpp
        tunnel/linux_relay_worker.cpp
    )
endif()
if(WIN32)
    list(APPEND TCPQUIC_ROUTER_RUNTIME_TEST_SOURCES
        tunnel/relay_alloc.cpp
        tunnel/relay_buffer.cpp
        tunnel/windows_relay_worker.cpp
    )
endif()
add_executable(tcpquic_router_runtime_test ${TCPQUIC_ROUTER_RUNTIME_TEST_SOURCES})
tcpquic_target_include_dirs(tcpquic_router_runtime_test)
target_link_libraries(tcpquic_router_runtime_test PRIVATE Threads::Threads inc warnings msquic logging base_link spdlog::spdlog)
if(BUILD_SHARED_LIBS)
    target_link_libraries(tcpquic_router_runtime_test PRIVATE msquic_platform)
endif()
set_property(TARGET tcpquic_router_runtime_test PROPERTY FOLDER "tools")
set_property(TARGET tcpquic_router_runtime_test APPEND PROPERTY BUILD_RPATH "$ORIGIN")
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    tcpquic_link_mimalloc(tcpquic_router_runtime_test)
endif()
if(WIN32)
    tcpquic_link_mimalloc(tcpquic_router_runtime_test)
endif()

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

set(TCPQUIC_WINDOWS_TEST_TARGETS)
if(WIN32)
    add_executable(tcpquic_windows_relay_worker_test
        unittest/windows_relay_worker_test.cpp
        tunnel/windows_relay_worker.cpp
        tunnel/relay_buffer.cpp
        tunnel/relay_alloc.cpp
        protocol/compress.cpp
        platform/platform_socket_win.cpp
    )
    tcpquic_target_include_dirs(tcpquic_windows_relay_worker_test)
    target_link_libraries(tcpquic_windows_relay_worker_test PRIVATE ws2_32 inc warnings msquic logging base_link ${TCPQUIC_TEST_LIBS})
    target_compile_definitions(tcpquic_windows_relay_worker_test PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN TQ_UNIT_TESTING ${TCPQUIC_TEST_DEFS})
    tcpquic_link_mimalloc(tcpquic_windows_relay_worker_test)
    list(APPEND TCPQUIC_WINDOWS_TEST_TARGETS tcpquic_windows_relay_worker_test)
endif()

set(TCPQUIC_TEST_TARGETS
    tcpquic_relay_buffer_test
    tcpquic_tuning_test
    tcpquic_quic_settings_test
    tcpquic_tcp_write_queue_test
    tcpquic_thread_pool_test
    tcpquic_tunnel_reaper_test
    tcpquic_control_test
    tcpquic_speed_test_test
    tcpquic_acl_test
    tcpquic_config_router_test
    tcpquic_proxy_auth_test
    tcpquic_compress_test
    tcpquic_tunnel_test
    tcpquic_http_connect_test
    tcpquic_socks5_test
    tcpquic_admin_http_test
    tcpquic_router_runtime_test
    tcpquic_production_linkage_guard_test
    tcpquic_platform_socket_test
)
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
        ${TCPQUIC_WINDOWS_TEST_TARGETS}
    )
    foreach(target IN LISTS TCPQUIC_MSQUIC_RUNTIME_TARGETS)
        if(TARGET ${target})
            tcpquic_copy_msquic_runtime(${target})
        endif()
    endforeach()
endif()
