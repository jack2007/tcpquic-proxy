list(APPEND TCPQUIC_LIBUV_RELAY_PRODUCTION_FILES
    tunnel/libuv_relay_tcp_to_quic.cpp)
set(TCPQUIC_LIBUV_TCP_TO_QUIC_READY 1)

tcpquic_add_libuv_relay_test(tcpquic_libuv_relay_tcp_to_quic_test
    unittest/libuv_relay_tcp_to_quic_test.cpp
    unittest/trace_proxy_stub.cpp
    tunnel/libuv_allocator.cpp
    tunnel/libuv_relay_quic_to_tcp.cpp
    tunnel/libuv_relay_tcp_to_quic.cpp
    tunnel/libuv_relay_worker.cpp
    tunnel/stream_lifetime.cpp
    tunnel/terminal_convergence.cpp
    tunnel/relay_buffer.cpp
    tunnel/relay_alloc.cpp
    protocol/compress.cpp
    config/tuning.cpp)
target_compile_definitions(
    tcpquic_libuv_relay_tcp_to_quic_test PRIVATE TCPQUIC_HAS_ZSTD=1)

target_sources(tcpquic_libuv_relay_worker_queue_test PRIVATE
    tunnel/libuv_relay_tcp_to_quic.cpp)
target_sources(tcpquic_libuv_relay_registration_test PRIVATE
    tunnel/libuv_relay_tcp_to_quic.cpp)
