# tcpquic-proxy 1x1 under 200ms + 5% loss analysis

- Date: 2026-06-21
- Direction: download
- Case: `tcpquic-proxy` 1 QUIC connection, 1 HTTP CONNECT stream
- Netem location: peer egress `enp1s0f0np0`
- Netem: `loss 5% limit 1000000`, comparing `delay 100ms` and `delay 200ms`

## Observed Throughput

| Scenario | Source | Throughput |
| --- | --- | --- |
| 100ms + 5% loss | 2026-06-20 original perf run | 1,322,376,751 B/s = 10.579 Gbps |
| 100ms + 5% loss | 2026-06-21 rerun, same non-trace script style | 176,891,595 B/s = 1.415 Gbps |
| 100ms + 5% loss | 2026-06-21 trace run, server `stream_tx` active-window overall | 1.846 Gbps |
| 200ms + 5% loss | 2026-06-21 perf run | 75,111,887 B/s = 0.601 Gbps |
| 200ms + 5% loss | 2026-06-21 trace run final curl | 110,941,029 B/s = 0.888 Gbps |
| 200ms + 5% loss | 2026-06-21 trace run, server `stream_tx` active-window overall | 1.001 Gbps |

The original 10.579 Gbps 100ms result was not reproducible on 2026-06-21 with the same netem and proxy 1x1 parameters. The reproducible comparison is closer to:

- 100ms + 5% loss: about 1.4-1.8 Gbps
- 200ms + 5% loss: about 0.6-1.0 Gbps

## Trace Evidence

Server-side QUIC trace comparison:

| Metric | 100ms + 5% loss | 200ms + 5% loss |
| --- | ---: | ---: |
| Observed netem loss in QUIC stats | p50 4.96%, max 5.18% | p50 5.23%, max 5.32% |
| Active-window `stream_tx` overall | 1.846 Gbps | 1.001 Gbps |
| 1s `stream_tx` rate p50 | 1.733 Gbps | 0.986 Gbps |
| 1s `stream_tx` rate p90 | 2.429 Gbps | 1.540 Gbps |
| MsQuic BBR bandwidth estimate p50 | 4.882 Gbps | 2.653 Gbps |
| MsQuic BBR bandwidth estimate p90 | 7.830 Gbps | 5.243 Gbps |
| `cwnd` p50 | 151,339,963 bytes | 152,044,997 bytes |
| `posted` p50 | 49,439,665 bytes | 53,565,094 bytes |
| relay `pending_bytes` p50 | 54,657,024 bytes | 57,016,320 bytes |

The application/relay side keeps data queued (`posted` and relay `pending_bytes` remain in the tens of MiB), so the 200ms throughput reduction is not explained by the proxy failing to feed MsQuic. The server-side QUIC stats show the network model clearly:

- RTT doubles from about 100ms to about 200ms.
- Random loss remains about 5%.
- MsQuic BBR bandwidth estimate drops materially.
- `cwnd` and 1s transmit rate fluctuate heavily under loss.

## Perf Evidence

The 200ms perf profile does not show a new CPU-bound proxy hotspot. The main samples remain in expected copy/UDP/TCP relay paths:

- Client: `__arch_copy_to_user`, `__arch_copy_from_user`, `CxPlatWorkerThread`, `QuicWorkerThread`
- Server: `__arch_copy_to_user`, TCP `recvmsg/readv`, relay worker, and MsQuic BBR send path

The 200ms perf data files are much smaller than the 100ms original run because far fewer bytes were transferred. That is consistent with a transport/window limit rather than a hotter CPU bottleneck.

## Direct TCP Comparison

Under the same 200ms + 5% loss netem:

- TCP direct cubic: about 2.1-2.4 Mbps
- TCP direct BBR large payload: 785.733 Mbps
- tcpquic-proxy 1x1: about 600.895 Mbps by final curl, about 1.001 Gbps by trace active-window `stream_tx`

So tcpquic-proxy 1x1 is in the same order as a single BBR transport on this path. It is not behaving like the cubic direct TCP case.

## Conclusion

The most likely cause of the lower 200ms proxy throughput is the interaction of one QUIC connection with 5% random packet loss at doubled RTT. With one connection/stream, MsQuic BBR sees about the expected 5% loss and maintains a much lower and more variable delivery estimate than in the 100ms case. The proxy relay has queued data available, and perf does not identify a new CPU bottleneck.

The earlier 10.579 Gbps 100ms result should be treated as an outlier or at least not a stable baseline until it can be reproduced with trace/stat collection.

## Next Checks

1. Repeat each scenario 3 times and use active-window `stream_tx` from trace as the primary metric, not only the final short curl `speed_download`.
2. Test 200ms + 5% loss with multiple QUIC connections, for example 2x1 and 4x1, to see whether aggregate throughput recovers by using independent congestion controllers.
3. Test lower loss rates at 200ms, such as 1% and 2%, to confirm the sensitivity curve.
