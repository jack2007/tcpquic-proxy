# secnetperf BBR double-window netem comparison

Date: 2026-06-21

## Setup

- Tool: modified MsQuic `secnetperf`
- Direction: download
- Connections/streams: 1 connection, 1 stream
- Duration: `-down:30s`
- Congestion control: `-cc:bbr`
- NetStats: `-pnet:1 -pnetinterval:1000`
- Sender netem: peer egress `enp1s0f0np0`
- Netem loss: `5%`
- Netem limit: `5000000`
- Result directory: `docs/dgx-msquic-secnetperf-bbr-netstats-limit5000000-doublewindows-20260621-101820/`

## Doubled parameters

Compared with the previous corrected run:

- connection flow window: `1GiB -> 2GiB`
- stream receive window: `1GiB -> 2GiB`
- initial window: `4000 -> 8000 packets`
- application send outstanding/default send buffer: `64MiB -> 128MiB`
- initial RTT stayed at `100ms`

## Throughput

| Scenario | qdisc | Result |
| --- | --- | ---: |
| 100ms + 5% loss | `limit 5000000 delay 100ms loss 5%` | 3,684,736 kbps = 3.685 Gbps |
| 200ms + 5% loss | `limit 5000000 delay 200ms loss 5%` | 1,857,875 kbps = 1.858 Gbps |

200ms throughput is 50.4% of the 100ms throughput.

## Sender NetStats

Stable samples use server-side `NetStats[server]` rows with `tx_packets >= 10000`.

| Scenario | Samples | Avg BBR bw | Min/Max BBR bw | Avg loss | Last loss | Avg cwnd | Avg posted | Avg ideal | Avg inflight |
| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| 100ms + 5% | 25 | 9,313.230 Mbps | 3,357.602 / 10,624.094 Mbps | 4.946% | 5.056% | 275,766,496 B | 134,217,728 B | 134,014,625 B | 68,054,752 B |
| 200ms + 5% | 25 | 4,339.851 Mbps | 521.567 / 5,399.699 Mbps | 4.870% | 5.025% | 292,725,409 B | 134,217,728 B | 131,480,346 B | 67,133,290 B |

## Comparison with previous corrected run

| Scenario | Previous 64MiB outstanding | Double-window / 128MiB outstanding | Delta |
| --- | ---: | ---: | ---: |
| 100ms + 5% | 3.593 Gbps | 3.685 Gbps | +2.5% |
| 200ms + 5% | 1.854 Gbps | 1.858 Gbps | +0.2% |

Observation: doubling these limits did not materially improve the 200ms case, and only slightly improved the 100ms case. NetStats confirms the new `128MiB` send outstanding is active (`posted_bytes=134217728`), so the remaining 100ms/200ms gap is not explained by these window/cache/outstanding limits.

## Artifacts

- `100ms/client.log`
- `100ms/server.log`
- `100ms/qdisc-before.txt`
- `100ms/qdisc-stats-before-cleanup.txt`
- `200ms/client.log`
- `200ms/server.log`
- `200ms/qdisc-before.txt`
- `200ms/qdisc-stats-before-cleanup.txt`

After the tests, peer qdisc was restored to `qdisc mq 0: root`.

