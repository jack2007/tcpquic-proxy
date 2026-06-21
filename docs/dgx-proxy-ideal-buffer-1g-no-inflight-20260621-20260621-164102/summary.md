# tcpquic-proxy ideal-send backpressure DGX validation

- topology: 2*DGX, 1 QUIC connection, 1 stream, 1 HTTP CONNECT download
- netem: sender side, `limit 5000000`, no `rate` cap
- payload: 6000MiB repeat payload
- final tuning: `srw=2GiB`, `fcw=2GiB`, `initial_read_ahead=128MiB`, `max_pending_buffer=1GiB`, `relay_pending=1GiB`
- backpressure: TCP read is driven by `QUIC_STREAM_EVENT_IDEAL_SEND_BUFFER_SIZE`; `RelayMaxInFlightSends=64` is no longer used as TCP-read backpressure.

| case | stage | throughput | curl exit | notes |
|---|---:|---:|---:|---|
| 100ms + 5% loss | send cap 1GiB, recv pending 64MiB | 4.57Gbps / 4.76Gbps | 18 | throughput improved, but receive side still paused and transfer ended incomplete |
| 200ms + 5% loss | send cap 1GiB, recv pending 64MiB | 3.30Gbps | 0 | complete |
| 100ms + 5% loss | send cap 1GiB, recv pending 1GiB | 6.89Gbps | 0 | complete, `quic_receive_paused=0` |
| 200ms + 5% loss | send cap 1GiB, recv pending 1GiB | 3.40Gbps | 0 | complete, `quic_receive_paused=0` |

## Key Findings

1. The old 64 send-operation cap was a real sender-side posting bottleneck. After removing it from TCP-read backpressure, sender-side outstanding send operations rose from 64 to hundreds/thousands and posted bytes tracked the MsQuic ideal-send threshold.
2. `max_pending_buffer=512MiB` was too small once MsQuic raised `IDEAL_SEND_BUFFER_SIZE` above 512MiB in 200ms loss tests. Raising the per-relay send buffer cap to 1GiB removed that local proxy cap.
3. Keeping `relay_pending=64MiB` left the receive side inconsistent with 2GiB QUIC flow windows. In the 100ms case, the client hit repeated QUIC receive pause/resume and curl ended with incomplete transfer. Raising `relay_pending` to 1GiB removed receive pauses and fixed the incomplete transfer.
4. With both send and receive relay caps at 1GiB, no MsQuic send resource backpressure or relay errors were observed in the final 100ms/200ms runs.

## Final Trace Highlights

- 100ms final:
  - server read full payload: `tcp_read_bytes=6291456208`
  - sender ideal threshold: `ideal_send_threshold_bytes=435848049`
  - sender max outstanding posted bytes: `outstanding_quic_send_bytes=290704025`
  - receive pause: `quic_receive_paused=0`
- 200ms final:
  - server read full payload: `tcp_read_bytes=6291456208`
  - sender ideal threshold: `ideal_send_threshold_bytes=653772073`
  - sender max outstanding posted bytes: `outstanding_quic_send_bytes=654092269`
  - receive pause: `quic_receive_paused=0`

## Verification

- `cmake --build build-plan --target tcpquic_linux_relay_worker_io_test tcpquic_tuning_test tcpquic_quic_settings_test tcpquic-proxy --parallel 8`
- `./build-plan/bin/Release/tcpquic_linux_relay_worker_io_test`
- `./build-plan/bin/Release/tcpquic_tuning_test`
- `./build-plan/bin/Release/tcpquic_quic_settings_test`
