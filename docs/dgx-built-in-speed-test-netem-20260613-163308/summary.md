# DGX built-in speed-test with peer netem delay

- Date: 2026-06-13
- Topology: 2x DGX 1x1, local `169.254.250.230` -> peer `169.254.59.196:4433`
- Tool: built-in `--download-test 30` / `--upload-test 30`
- Netem location: peer DGX `enp1s0f0np0` egress only
- Netem limit: `1000000`
- Netem loss: none
- QUIC/tunnel tuning: custom, 1GiB QUIC windows, 4000 IW, 1MiB relay IO, 4GiB memory cap

## Important Notes

The first attempt exposed two speed-test tool issues under high RTT:

1. The client connection wait loop stopped after a single 100ms `EnsureAnyConnected()` miss, so 100ms/200ms RTT could fail before the 10s deadline.
2. Upload byte mismatch tolerance still had a `high/100` cap, which is too small when high RTT lowers total transferred bytes but the local socket pipeline can still differ by tens of MiB at stop time.

Both were fixed locally before the final successful runs. The result directory keeps the failed first-attempt artifacts for diagnosis; the table below uses the successful retry/final runs.

## Netem Verification

| Delay | Peer qdisc | Ping avg |
| --- | --- | ---: |
| 100ms | `qdisc netem ... limit 1000000 delay 100ms` | 101.8 ms |
| 200ms | `qdisc netem ... limit 1000000 delay 200ms` | 201.6 ms |

Netem was removed after the runs. `logs/final-peer-qdisc-after-cleanup.txt` records the post-cleanup qdisc state.

## Throughput

| Peer delay | Direction | Exit | Throughput | Local bytes | Server bytes | Server seconds |
| --- | --- | ---: | ---: | ---: | ---: | ---: |
| 100ms | download | 0 | 1.186 Gbps | 4,445,962,240 | 4,707,673,916 | 30.105 |
| 100ms | upload | 0 | 1.178 Gbps | 4,493,250,240 | 4,433,379,328 | 30.108 |
| 200ms | download | 0 | 0.628 Gbps | 2,353,139,810 | 2,631,469,918 | 30.204 |
| 200ms | upload | 0 | 0.581 Gbps | 2,270,269,120 | 2,193,620,992 | 30.206 |

## Server Metrics Delta

| Delay | Direction | `tcp_read_bytes` | `tcp_write_bytes` | `errors` | `tcp_write_hard_errors` | `read_disabled` | `max_tcp_read_iov` | `max_tcp_write_iov` | `max_sendmsg_bytes` | `max_pending_quic_rx` |
| --- | --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| 100ms | download | 4,707,673,916 | 0 | 0 | 0 | 1,105 | 32 | 0 | 0 | 0 |
| 100ms | upload | 0 | 4,433,380,752 | 91 | 91 | 1 | 0 | 3 | 8,378,844 | 8,378,844 |
| 200ms | download | 2,631,469,918 | 0 | 0 | 0 | 606 | 32 | 3 | 6,602,664 | 6,602,664 |
| 200ms | upload | 0 | 2,193,683,912 | 131 | 131 | 1 | 0 | 3 | 8,378,844 | 8,378,844 |

Upload `tcp_write_hard_errors` are teardown EPIPE-class errors after the receiving side closes; the speed-test result had already returned and the final upload runs exited `0`.

## Takeaways

- Throughput scales roughly with added RTT: 100ms is about 1.18 Gbps, 200ms is about 0.58-0.63 Gbps.
- No `tcp_write_eagain` or `tcp_write_partial` was observed in these valid runs.
- Download uses server `tcp_read_bytes`; upload uses server `tcp_write_bytes`, matching the expected direction of relay work.
