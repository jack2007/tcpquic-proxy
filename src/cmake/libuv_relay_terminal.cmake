list(APPEND TCPQUIC_LIBUV_RELAY_PRODUCTION_FILES
    tunnel/libuv_relay_terminal.cpp)

target_sources(tcpquic_libuv_relay_worker_queue_test PRIVATE
    tunnel/libuv_relay_terminal.cpp)
target_sources(tcpquic_libuv_relay_registration_test PRIVATE
    tunnel/libuv_relay_terminal.cpp)
target_sources(tcpquic_libuv_relay_quic_to_tcp_test PRIVATE
    tunnel/libuv_relay_terminal.cpp)
target_sources(tcpquic_libuv_relay_tcp_to_quic_test PRIVATE
    tunnel/libuv_relay_terminal.cpp)
target_sources(tcpquic_libuv_relay_metrics_test PRIVATE
    tunnel/libuv_relay_terminal.cpp)
target_sources(tcpquic_libuv_relay_metrics_system_allocator_test PRIVATE
    tunnel/libuv_relay_terminal.cpp)

tcpquic_add_libuv_relay_test(tcpquic_libuv_terminal_convergence_test
    unittest/libuv_terminal_convergence_test.cpp
    unittest/trace_proxy_stub.cpp
    tunnel/libuv_allocator.cpp
    tunnel/libuv_relay_worker.cpp
    tunnel/libuv_relay_quic_to_tcp.cpp
    tunnel/libuv_relay_tcp_to_quic.cpp
    tunnel/libuv_relay_terminal.cpp
    tunnel/stream_lifetime.cpp
    tunnel/terminal_convergence.cpp
    tunnel/relay_buffer.cpp
    tunnel/relay_alloc.cpp
    protocol/compress.cpp
    config/tuning.cpp)
target_compile_definitions(
    tcpquic_libuv_terminal_convergence_test PRIVATE TCPQUIC_HAS_ZSTD=1)

tcpquic_add_libuv_relay_test(tcpquic_libuv_relay_ready_contract_test
    unittest/libuv_relay_ready_contract_test.cpp
    unittest/trace_proxy_stub.cpp
    tunnel/libuv_allocator.cpp
    tunnel/libuv_relay_worker.cpp
    tunnel/libuv_relay_quic_to_tcp.cpp
    tunnel/libuv_relay_tcp_to_quic.cpp
    tunnel/libuv_relay_terminal.cpp
    tunnel/stream_lifetime.cpp
    tunnel/terminal_convergence.cpp
    tunnel/relay_buffer.cpp
    tunnel/relay_alloc.cpp
    protocol/compress.cpp
    config/tuning.cpp)
target_compile_definitions(tcpquic_libuv_relay_ready_contract_test PRIVATE
    TCPQUIC_HAS_ZSTD=1
    TCPQUIC_LIBUV_QUIC_TO_TCP_READY=1
    TCPQUIC_LIBUV_TCP_TO_QUIC_READY=1)
