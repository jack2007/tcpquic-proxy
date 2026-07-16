list(APPEND TCPQUIC_LIBUV_RELAY_PRODUCTION_FILES
    tunnel/libuv_relay_quic_to_tcp.cpp)
set(TCPQUIC_LIBUV_QUIC_TO_TCP_READY 1)

# M2 tests compile the worker directly; once its Active callback routes into
# this lane they must link the direction implementation as well.
target_sources(tcpquic_libuv_relay_worker_queue_test PRIVATE
    tunnel/libuv_relay_quic_to_tcp.cpp)
target_sources(tcpquic_libuv_relay_registration_test PRIVATE
    tunnel/libuv_relay_quic_to_tcp.cpp)

tcpquic_add_libuv_relay_test(tcpquic_libuv_relay_quic_to_tcp_test
    unittest/libuv_relay_quic_to_tcp_test.cpp
    unittest/trace_proxy_stub.cpp
    tunnel/libuv_allocator.cpp
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
    tcpquic_libuv_relay_quic_to_tcp_test PRIVATE TCPQUIC_HAS_ZSTD=1)
