# secnetperf BBR netem NetStats comparison

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
- Result directory: `docs/dgx-msquic-secnetperf-bbr-netstats-limit5000000-sendbuf64m-20260621-100940/`

## Code note

The first run with the new NetStats build showed an invalid low-throughput result:

| Delay | Result | Cause |
| --- | ---: | --- |
| 100ms + 5% | 7.726 Mbps | server `posted_bytes/ideal_bytes` fixed at `131072` |
| 200ms + 5% | 4.024 Mbps | server `posted_bytes/ideal_bytes` fixed at `131072` |

That matched the old `PERF_DEFAULT_SEND_BUFFER_SIZE=128KiB` application outstanding limit (`128KiB / RTT`), not the network. I increased `PERF_DEFAULT_SEND_BUFFER_SIZE` to `64MiB` and reran the comparison below.

## Throughput

| Scenario | qdisc | Result |
| --- | --- | ---: |
| 100ms + 5% loss | `limit 5000000 delay 100ms loss 5%` | 3,593,174 kbps = 3.593 Gbps |
| 200ms + 5% loss | `limit 5000000 delay 200ms loss 5%` | 1,854,080 kbps = 1.854 Gbps |

200ms throughput is 51.6% of the 100ms throughput.

## Sender NetStats

Stable samples use server-side `NetStats[server]` rows with `tx_packets >= 10000`.

| Scenario | Samples | Avg BBR bw | Min/Max BBR bw | Avg loss | Last loss | Avg cwnd | Avg posted | Avg ideal |
| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| 100ms + 5% | 25 | 9,518.652 Mbps | 3,603.658 / 10,536.048 Mbps | 4.887% | 5.023% | 285,771,505 B | 132,293,591 B | 132,292,757 B |
| 200ms + 5% | 25 | 4,789.309 Mbps | 496.203 / 5,314.012 Mbps | 4.835% | 5.057% | 312,306,356 B | 131,331,523 B | 130,176,466 B |

NetStats confirms:

- Sender-observed QUIC loss is consistent with the expected 5% netem loss.
- The corrected run is no longer capped by the old 128KiB application outstanding limit; `posted_bytes` and `ideal_bytes` reach about 128MiB.
- BBR's estimated bandwidth scales roughly with RTT in these two loss scenarios: about 9.5Gbps at 100ms and 4.8Gbps at 200ms in stable samples.

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

