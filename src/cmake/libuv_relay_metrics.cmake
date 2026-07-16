tcpquic_add_libuv_relay_test(tcpquic_libuv_relay_metrics_test
    unittest/libuv_relay_metrics_test.cpp
    unittest/trace_proxy_stub.cpp
    runtime/relay_metrics.cpp
    runtime/memory_stats.cpp
    tunnel/libuv_allocator.cpp
    tunnel/libuv_relay.cpp
    tunnel/libuv_relay_worker.cpp
    tunnel/libuv_relay_quic_to_tcp.cpp
    tunnel/libuv_relay_tcp_to_quic.cpp
    tunnel/stream_lifetime.cpp
    tunnel/terminal_convergence.cpp
    tunnel/relay_buffer.cpp
    tunnel/relay_alloc.cpp
    protocol/compress.cpp
    config/tuning.cpp)
target_compile_definitions(
    tcpquic_libuv_relay_metrics_test PRIVATE TCPQUIC_HAS_ZSTD=1)
target_link_libraries(tcpquic_libuv_relay_metrics_test PRIVATE
    nlohmann_json::nlohmann_json)

add_tcpquic_executable(tcpquic_libuv_relay_metrics_system_allocator_test
    unittest/libuv_relay_metrics_test.cpp
    unittest/trace_proxy_stub.cpp
    runtime/relay_metrics.cpp
    runtime/memory_stats.cpp
    tunnel/libuv_allocator.cpp
    tunnel/libuv_relay.cpp
    tunnel/libuv_relay_worker.cpp
    tunnel/libuv_relay_quic_to_tcp.cpp
    tunnel/libuv_relay_tcp_to_quic.cpp
    tunnel/stream_lifetime.cpp
    tunnel/terminal_convergence.cpp
    tunnel/relay_buffer.cpp
    tunnel/relay_alloc.cpp
    protocol/compress.cpp
    config/tuning.cpp)
tcpquic_target_include_dirs(tcpquic_libuv_relay_metrics_system_allocator_test)
target_compile_definitions(tcpquic_libuv_relay_metrics_system_allocator_test PRIVATE
    TQ_UNIT_TESTING=1
    TCPQUIC_RELAY_BACKEND_LIBUV=1
    TCPQUIC_HAS_ZSTD=1
    TCPQUIC_USE_MIMALLOC=0)
target_link_libraries(tcpquic_libuv_relay_metrics_system_allocator_test PRIVATE
    uv_a libzstd spdlog::spdlog Threads::Threads nlohmann_json::nlohmann_json)
