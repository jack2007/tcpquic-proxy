# Copyright (c) Microsoft Corporation.
# Licensed under the MIT License.

function(add_tcpquic_executable target)
    add_executable(${target} ${ARGN})
    target_link_libraries(${target} PRIVATE inc warnings msquic logging base_link)
    if(BUILD_SHARED_LIBS)
        target_link_libraries(${target} PRIVATE msquic_platform)
    endif()
    set_property(TARGET ${target} PROPERTY FOLDER "tools")
    set_property(TARGET ${target} APPEND PROPERTY BUILD_RPATH "$ORIGIN")
endfunction()

find_package(Threads REQUIRED)

set(TCPQUIC_PROXY_SOURCES
    main.cpp
    config.cpp
    tuning.cpp
    control_protocol.cpp
    acl.cpp
    compress.cpp
    quic_session.cpp
    tcp_dialer.cpp
    tcp_tunnel.cpp
    http_connect_server.cpp
    socks5_server.cpp
    relay.cpp
    tcp_write_queue.cpp
    thread_pool.cpp
    tunnel_reaper.cpp
    warmup.cpp
    admin_http.cpp
    router_runtime.cpp
    server_metrics.cpp
)

if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    list(APPEND TCPQUIC_PROXY_SOURCES
        linux_relay_buffer_pool.cpp
        linux_relay_worker.cpp
    )
endif()

add_tcpquic_executable(tcpquic-proxy ${TCPQUIC_PROXY_SOURCES})

target_include_directories(tcpquic-proxy PRIVATE ${MSQUIC_SOURCE_DIR}/src/inc)
target_link_libraries(tcpquic-proxy PRIVATE libzstd lz4)
target_compile_definitions(tcpquic-proxy PRIVATE TCPQUIC_HAS_ZSTD=1 TCPQUIC_HAS_LZ4=1)

set(TCPQUIC_TEST_LIBS libzstd lz4)
set(TCPQUIC_TEST_INCLUDES)
set(TCPQUIC_TEST_DEFS TCPQUIC_HAS_ZSTD=1 TCPQUIC_HAS_LZ4=1)
set(TCPQUIC_SERVER_TEST_SOURCES
    thread_pool.cpp
    tcp_dialer.cpp
    tuning.cpp
)
add_executable(tcpquic_tuning_test
    unittest/tuning_test.cpp
    config.cpp
    tuning.cpp
    acl.cpp
)
target_include_directories(tcpquic_tuning_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
set_property(TARGET tcpquic_tuning_test PROPERTY FOLDER "tools")
set_property(TARGET tcpquic_tuning_test APPEND PROPERTY BUILD_RPATH "$ORIGIN")

add_executable(tcpquic_tcp_write_queue_test
    unittest/tcp_write_queue_test.cpp
    tcp_write_queue.cpp
)
target_include_directories(tcpquic_tcp_write_queue_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(tcpquic_tcp_write_queue_test Threads::Threads)
set_property(TARGET tcpquic_tcp_write_queue_test PROPERTY FOLDER "tools")
set_property(TARGET tcpquic_tcp_write_queue_test APPEND PROPERTY BUILD_RPATH "$ORIGIN")

add_executable(tcpquic_linux_relay_buffer_pool_test
    unittest/linux_relay_buffer_pool_test.cpp
    linux_relay_buffer_pool.cpp
)
target_include_directories(tcpquic_linux_relay_buffer_pool_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
set_property(TARGET tcpquic_linux_relay_buffer_pool_test PROPERTY FOLDER "tools")
set_property(TARGET tcpquic_linux_relay_buffer_pool_test APPEND PROPERTY BUILD_RPATH "$ORIGIN")

add_executable(tcpquic_linux_relay_worker_queue_test
    unittest/linux_relay_worker_queue_test.cpp
    linux_relay_worker.cpp
    linux_relay_buffer_pool.cpp
)
target_include_directories(tcpquic_linux_relay_worker_queue_test PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${MSQUIC_SOURCE_DIR}/src/inc)
target_link_libraries(tcpquic_linux_relay_worker_queue_test Threads::Threads)
set_property(TARGET tcpquic_linux_relay_worker_queue_test PROPERTY FOLDER "tools")
set_property(TARGET tcpquic_linux_relay_worker_queue_test APPEND PROPERTY BUILD_RPATH "$ORIGIN")

add_executable(tcpquic_linux_relay_worker_io_test
    unittest/linux_relay_worker_io_test.cpp
    linux_relay_worker.cpp
    linux_relay_buffer_pool.cpp
)
target_include_directories(tcpquic_linux_relay_worker_io_test PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${MSQUIC_SOURCE_DIR}/src/inc)
target_link_libraries(tcpquic_linux_relay_worker_io_test Threads::Threads)
set_property(TARGET tcpquic_linux_relay_worker_io_test PROPERTY FOLDER "tools")
set_property(TARGET tcpquic_linux_relay_worker_io_test APPEND PROPERTY BUILD_RPATH "$ORIGIN")

add_executable(tcpquic_thread_pool_test
    unittest/thread_pool_test.cpp
    thread_pool.cpp
)
target_include_directories(tcpquic_thread_pool_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(tcpquic_thread_pool_test Threads::Threads)
set_property(TARGET tcpquic_thread_pool_test PROPERTY FOLDER "tools")
set_property(TARGET tcpquic_thread_pool_test APPEND PROPERTY BUILD_RPATH "$ORIGIN")

add_executable(tcpquic_tunnel_reaper_test
    unittest/tunnel_reaper_test.cpp
    tunnel_reaper.cpp
)
target_include_directories(tcpquic_tunnel_reaper_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(tcpquic_tunnel_reaper_test Threads::Threads)
set_property(TARGET tcpquic_tunnel_reaper_test PROPERTY FOLDER "tools")
set_property(TARGET tcpquic_tunnel_reaper_test APPEND PROPERTY BUILD_RPATH "$ORIGIN")

add_executable(tcpquic_control_test
    unittest/control_protocol_test.cpp
    control_protocol.cpp
)
target_include_directories(tcpquic_control_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
set_property(TARGET tcpquic_control_test PROPERTY FOLDER "tools")
set_property(TARGET tcpquic_control_test APPEND PROPERTY BUILD_RPATH "$ORIGIN")

add_executable(tcpquic_acl_test
    unittest/acl_test.cpp
    acl.cpp
)
target_include_directories(tcpquic_acl_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
set_property(TARGET tcpquic_acl_test PROPERTY FOLDER "tools")
set_property(TARGET tcpquic_acl_test APPEND PROPERTY BUILD_RPATH "$ORIGIN")

add_executable(tcpquic_config_router_test
    unittest/config_router_test.cpp
    config.cpp
    tuning.cpp
    acl.cpp
)
target_include_directories(tcpquic_config_router_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
set_property(TARGET tcpquic_config_router_test PROPERTY FOLDER "tools")
set_property(TARGET tcpquic_config_router_test APPEND PROPERTY BUILD_RPATH "$ORIGIN")

add_executable(tcpquic_compress_test
    unittest/compress_test.cpp
    compress.cpp
)
target_include_directories(tcpquic_compress_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR} ${TCPQUIC_TEST_INCLUDES})
target_link_libraries(tcpquic_compress_test ${TCPQUIC_TEST_LIBS})
target_compile_definitions(tcpquic_compress_test PRIVATE ${TCPQUIC_TEST_DEFS})
set_property(TARGET tcpquic_compress_test PROPERTY FOLDER "tools")
set_property(TARGET tcpquic_compress_test APPEND PROPERTY BUILD_RPATH "$ORIGIN")

set(TCPQUIC_TUNNEL_TEST_SOURCES
    unittest/tcp_tunnel_test.cpp
    tcp_tunnel.cpp
    tcp_dialer.cpp
    tuning.cpp
    control_protocol.cpp
    acl.cpp
    compress.cpp
    relay.cpp
    tcp_write_queue.cpp
    tunnel_reaper.cpp
)
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    list(APPEND TCPQUIC_TUNNEL_TEST_SOURCES
        linux_relay_buffer_pool.cpp
        linux_relay_worker.cpp
    )
endif()
add_executable(tcpquic_tunnel_test ${TCPQUIC_TUNNEL_TEST_SOURCES})
target_include_directories(tcpquic_tunnel_test PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${MSQUIC_SOURCE_DIR}/src/inc
    ${TCPQUIC_TEST_INCLUDES})
target_link_libraries(tcpquic_tunnel_test ${TCPQUIC_TEST_LIBS} Threads::Threads inc warnings msquic logging base_link)
if(BUILD_SHARED_LIBS)
    target_link_libraries(tcpquic_tunnel_test msquic_platform)
endif()
target_compile_definitions(tcpquic_tunnel_test PRIVATE ${TCPQUIC_TEST_DEFS})
set_property(TARGET tcpquic_tunnel_test PROPERTY FOLDER "tools")
set_property(TARGET tcpquic_tunnel_test APPEND PROPERTY BUILD_RPATH "$ORIGIN")

add_executable(tcpquic_http_connect_test
    unittest/http_connect_server_test.cpp
    http_connect_server.cpp
    control_protocol.cpp
    ${TCPQUIC_SERVER_TEST_SOURCES}
)
target_include_directories(tcpquic_http_connect_test PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${MSQUIC_SOURCE_DIR}/src/inc)
target_link_libraries(tcpquic_http_connect_test Threads::Threads)
set_property(TARGET tcpquic_http_connect_test PROPERTY FOLDER "tools")
set_property(TARGET tcpquic_http_connect_test APPEND PROPERTY BUILD_RPATH "$ORIGIN")

add_executable(tcpquic_socks5_test
    unittest/socks5_server_test.cpp
    socks5_server.cpp
    control_protocol.cpp
    ${TCPQUIC_SERVER_TEST_SOURCES}
)
target_include_directories(tcpquic_socks5_test PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${MSQUIC_SOURCE_DIR}/src/inc)
target_link_libraries(tcpquic_socks5_test Threads::Threads)
set_property(TARGET tcpquic_socks5_test PROPERTY FOLDER "tools")
set_property(TARGET tcpquic_socks5_test APPEND PROPERTY BUILD_RPATH "$ORIGIN")

add_executable(tcpquic_admin_http_test
    unittest/admin_http_test.cpp
    admin_http.cpp
)
target_include_directories(tcpquic_admin_http_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(tcpquic_admin_http_test Threads::Threads)
set_property(TARGET tcpquic_admin_http_test PROPERTY FOLDER "tools")
set_property(TARGET tcpquic_admin_http_test APPEND PROPERTY BUILD_RPATH "$ORIGIN")

set(TCPQUIC_ROUTER_RUNTIME_TEST_SOURCES
    unittest/router_runtime_test.cpp
    router_runtime.cpp
    server_metrics.cpp
    admin_http.cpp
    config.cpp
    tuning.cpp
    acl.cpp
)
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    list(APPEND TCPQUIC_ROUTER_RUNTIME_TEST_SOURCES
        linux_relay_buffer_pool.cpp
        linux_relay_worker.cpp
    )
endif()
add_executable(tcpquic_router_runtime_test ${TCPQUIC_ROUTER_RUNTIME_TEST_SOURCES})
target_include_directories(tcpquic_router_runtime_test PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${MSQUIC_SOURCE_DIR}/src/inc)
target_link_libraries(tcpquic_router_runtime_test Threads::Threads inc warnings msquic logging base_link)
if(BUILD_SHARED_LIBS)
    target_link_libraries(tcpquic_router_runtime_test msquic_platform)
endif()
set_property(TARGET tcpquic_router_runtime_test PROPERTY FOLDER "tools")
set_property(TARGET tcpquic_router_runtime_test APPEND PROPERTY BUILD_RPATH "$ORIGIN")

set(TCPQUIC_TEST_TARGETS
    tcpquic_tuning_test
    tcpquic_tcp_write_queue_test
    tcpquic_linux_relay_buffer_pool_test
    tcpquic_linux_relay_worker_queue_test
    tcpquic_linux_relay_worker_io_test
    tcpquic_thread_pool_test
    tcpquic_tunnel_reaper_test
    tcpquic_control_test
    tcpquic_acl_test
    tcpquic_config_router_test
    tcpquic_compress_test
    tcpquic_tunnel_test
    tcpquic_http_connect_test
    tcpquic_socks5_test
    tcpquic_admin_http_test
    tcpquic_router_runtime_test
)

foreach(target IN LISTS TCPQUIC_TEST_TARGETS)
    target_compile_features(${target} PRIVATE cxx_std_17)
endforeach()
