# secnetperf BBR netstats and window check

Date: 2026-06-21

## tcpquic-proxy sender loss

The tcpquic-proxy sender logs match the expected 5% netem loss once startup samples are excluded.
Samples below use only `pkts` lines with `tx >= 10000`.

| Scenario | Log | Samples | Min | P50 | Max | Avg |
| --- | --- | ---: | ---: | ---: | ---: | ---: |
| 100ms + 5% loss | `docs/dgx-netem-100ms-5loss-proxy-trace-20260621/server-trace.log` | 37 | 3.06% | 4.96% | 5.18% | 4.9116% |
| 200ms + 5% loss | `docs/dgx-netem-200ms-5loss-proxy-trace-initrtt-compare-20260621/initrtt-400/trace-log/server-trace.log` | 67 | 4.84% | 5.14% | 5.51% | 5.1415% |

Conclusion: the proxy sender-side loss statistics are consistent with the configured 5% netem loss. The loss setting is not the explanation for the 100ms/200ms throughput gap by itself.

## secnetperf window check

Before this change, secnetperf was not aligned with tcpquic-proxy's large-window test conditions:

- `PERF_DEFAULT_CONN_FLOW_CONTROL` was `0x8000000` (128 MiB).
- secnetperf server set connection flow control, but did not set stream receive window.
- secnetperf client download-side settings were overwritten by a local `MsQuicSettings` object in `PerfClient::Init`, so the receiving side could fall back to MsQuic defaults.

After this change, secnetperf explicitly uses:

- connection flow control window: `0x40000000` (1 GiB)
- stream receive window default: `0x40000000` (1 GiB)
- initial congestion window: 4000 packets
- initial RTT: 100 ms

These are applied on both server and client configuration paths.

## secnetperf network stats

Added `-pnet:<0/1>` and `-pnetinterval:<ms>` to print periodic connection network statistics.
The output includes RTT, smoothed RTT, cwnd, bytes in flight, posted/ideal send-buffer bytes, BBR bandwidth estimate, total sent packets, lost packets, spurious lost packets, loss rate, and congestion event count.

Example output from a local smoke test:

```text
NetStats[server]: rtt_us=305 min_rtt_us=42 smoothed_rtt_us=305 cwnd=373034 bytes_in_flight=2944 posted_bytes=131072 ideal_bytes=131072 bbr_bw_bytes_per_sec=508429657 bbr_bw_mbps=4067.437 tx_packets=37794 lost_packets=0 spurious_lost_packets=0 loss_rate=0.000% congestion_events=0
```

## Verification

- Build: `cmake --build build-msquic-perf/msquic -j$(nproc)` completed successfully and rebuilt `secnetperf`.
- Help output: `secnetperf -help` shows `-pnet` and `-pnetinterval`.
- Local smoke test: 1 connection, 1 stream, short download with `-cc:bbr -pnet:1 -pnetinterval:1000` printed `NetStats[server]` and `NetStats[client]` lines.
