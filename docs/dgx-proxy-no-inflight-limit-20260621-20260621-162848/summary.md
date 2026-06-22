# tcpquic-proxy no in-flight-send-count limit 1x1 DGX test

- change: TCP read backpressure no longer checks `MaxInFlightQuicSends`; it only checks the ideal-send byte threshold, except for real MsQuic send resource failures.
- netem: peer `enp1s0f0np0`, `limit 5000000`
- topology: 1 QUIC connection, 1 stream, 1 HTTP CONNECT download

| case | speed_download(B/s) | Mbps | Gbps | curl_exit |
|---|---:|---:|---:|---:|
| netem-100ms-5loss | 56521553 | 452.17 | 0.452 | 18 |
| netem-200ms-5loss | 88307785 | 706.46 | 0.706 | 18 |

## Observations

- The previous `outstanding_quic_sends=64` application-side bottleneck is removed.
- Sender-side posted data increases substantially:
  - 100ms case reaches `outstanding_quic_sends=271`, `outstanding_quic_send_bytes=124090285`.
  - 200ms case reaches `outstanding_quic_sends=1191`, `outstanding_quic_send_bytes=491973779`.
- MsQuic BBR estimates are much higher than curl throughput:
  - 200ms case sample: `bbr_bw_mbps=11677.14`, `posted=491973779`, `ideal=653772073`.
  - 200ms later sample: `bbr_bw_mbps=15976.69`, `posted=253811684`, `ideal=653772073`.
- Both tests fail with incomplete HTTP transfer (`curl_exit=18`).
- Client side reports `tcp_write_hard_error errno=32` after receiving partial data:
  - 100ms case: transfer closed with about 4.15GB remaining.
  - 200ms case: transfer closed with about 3.81GB remaining.

## Conclusion

Removing `MaxInFlightQuicSends` confirms the old 64-operation cap was suppressing application posting into MsQuic. However, removing the cap entirely is not stable for the current relay path: outstanding send operations can grow past 1000 and the client-side TCP write path hits EPIPE/closed-socket errors before the HTTP transfer completes.

The next safe direction is not to restore the 64-operation bottleneck, but to replace it with a higher resource guard that does not fire before the `IDEAL_SEND_BUFFER_SIZE` byte threshold is reached.
