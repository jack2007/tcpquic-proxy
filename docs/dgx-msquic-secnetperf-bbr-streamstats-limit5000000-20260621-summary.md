# secnetperf BBR stream queue diagnostics

Date: 2026-06-21

## Setup

- Tool: modified MsQuic `secnetperf`
- Direction: download
- Connections/streams: 1 connection, 1 stream
- Duration: `-down:30s`
- Congestion control: `-cc:bbr`
- NetStats: `-pnet:1 -pnetinterval:1000`
- StreamStats: `-pstreamstats:1 -pstreamstatsinterval:1000`
- Sender netem: peer egress `enp1s0f0np0`
- Netem loss: `5%`
- Netem limit: `5000000`
- Result directory: `docs/dgx-msquic-secnetperf-bbr-streamstats-limit5000000-20260621-104038/`

## Added diagnostics

`StreamStats[server]` now prints:

- `outstanding_bytes`
- `ideal_send_buffer`
- `queue_fill_pct`
- `queue_free_bytes`
- `enqueued_delta`
- `completed_delta`
- stream blocked time counters: scheduling, pacing, congestion, connection flow control, stream flow control, app

These fields directly indicate whether the application/MsQuic send queue is being kept full.

## Throughput

| Scenario | qdisc | Result |
| --- | --- | ---: |
| 100ms + 5% loss | `limit 5000000 delay 100ms loss 5%` | 3,647,483 kbps = 3.647 Gbps |
| 200ms + 5% loss | `limit 5000000 delay 200ms loss 5%` | 1,814,597 kbps = 1.815 Gbps |

## Sender queue state

Stable stream samples use complete `StreamStats[server]` rows after the first two startup samples.

| Scenario | Samples | Avg fill | Avg free | Avg outstanding | Avg ideal | Avg enqueued delta | Avg completed delta |
| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| 100ms + 5% | 24 | 100.00% | 0 B | 134,217,728 B | 134,217,728 B | 486,593,877 B | 486,593,877 B |
| 200ms + 5% | 24 | 100.00% | 0 B | 134,217,728 B | 134,217,728 B | 241,765,035 B | 241,765,035 B |

Conclusion: the sender queue is full in both scenarios. New data is continuously entering the send queue at the same rate that send completions free space. The current bottleneck is not application enqueue starvation or insufficient send outstanding.

## Blocked time deltas

Stable stream blocked-time deltas:

| Scenario | Scheduling | Pacing | Congestion | Conn flow | Stream flow | App |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| 100ms + 5% | 272,170 us | 10,384 us | 359,884 us | 0 us | 0 us | 18,659,167 us |
| 200ms + 5% | 97,305 us | 3,143 us | 337,177 us | 0 us | 0 us | 20,983,598 us |

Flow control counters remain zero, so connection/stream receive windows are not limiting throughput. The large `APP` blocked time is consistent with MsQuic reporting periods where the stream cannot immediately make progress because the app-side send queue is already full; paired with `queue_fill_pct=100%`, this is not evidence of insufficient app supply.

## Sender NetStats

Stable server NetStats:

| Scenario | Avg BBR bw | Avg loss | Avg cwnd | Avg inflight | Avg posted | Avg ideal |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| 100ms + 5% | 9,829.365 Mbps | 4.963% | 300,886,084 B | 67,645,482 B | 134,217,728 B | 134,022,437 B |
| 200ms + 5% | 4,913.151 Mbps | 4.656% | 311,962,229 B | 59,137,313 B | 134,217,728 B | 131,976,212 B |

## Current interpretation

The working hypothesis that "new data cannot enter the send queue" is not supported by the new measurements. The queue stays full. The remaining gap is therefore likely below the application enqueue layer:

- BBR pacing / send allowance behavior under 5% random loss and 100-200ms RTT
- loss recovery and retransmission scheduling sharing the same congestion/pacing budget as new data
- single-stream in-order delivery and recovery gaps reducing application-observed throughput
- sender/receiver CPU or packet processing cost under high retransmission rate
- packetization/GSO/MTU behavior and packet-rate limits

Next useful probes should target these lower layers rather than enlarging flow-control or application queue limits again.

## Cleanup

After the tests, peer qdisc was restored to `qdisc mq 0: root`.

