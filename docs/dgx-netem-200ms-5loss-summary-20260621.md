# DGX netem 200ms 5% loss throughput summary

- Date: 2026-06-21
- Direction: download
- Sender netem: peer egress `enp1s0f0np0`
- Netem: `delay 200ms loss 5% limit 1000000`
- Client bind: `169.254.250.230`
- Server: `169.254.59.196`
- Proxy case: `tcpquic-proxy` 1 QUIC connection, 1 HTTP CONNECT stream

## Results

| Case | Object / window | Result | Throughput |
| --- | --- | --- | --- |
| TCP direct, cubic | 16 MiB complete | 16,777,216 bytes in 62.753665s, curl exit 0 | 267,350 B/s = 2.139 Mbps |
| TCP direct, cubic | large payload, 180s window | 52,963,212 bytes in 180.001656s, curl timeout | 294,237 B/s = 2.354 Mbps |
| TCP direct, bbr | 16 MiB complete | 16,777,216 bytes in 3.734434s, curl exit 0 | 4,492,572 B/s = 35.941 Mbps |
| TCP direct, bbr | large payload complete | 4,718,592,000 bytes in 48.042683s, curl exit 0 | 98,216,662 B/s = 785.733 Mbps |
| tcpquic-proxy 1x1 | script final download | `speed_download=75111887` | 75,111,887 B/s = 600.895 Mbps |

## Artifacts

- TCP direct raw output: `docs/dgx-netem-200ms-5loss-tcp-direct-20260621/`
- Proxy raw output and perf data: `docs/dgx-netem-200ms-5loss-proxy-1x1-20260621/`

## Cleanup

After the tests, peer qdisc was restored to `qdisc mq 0: root`.
The sender TCP congestion control remains `bbr`.
