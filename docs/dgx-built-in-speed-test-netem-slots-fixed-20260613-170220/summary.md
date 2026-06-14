# DGX built-in speed-test with high-BDP relay slots

- Date: 2026-06-13
- Topology: 2x DGX 1x1, local `169.254.250.230` -> peer `169.254.59.196:4433`
- Tool: built-in `--download-test 30` / `--upload-test 30`
- Netem location: peer DGX `enp1s0f0np0` egress only
- Netem limit: `1000000`
- Netem loss: none
- Common tuning: custom, 1GiB QUIC windows, 4000 IW, 1MiB relay IO, 4GiB memory cap, 128KiB relay read chunk

## Test Parameters

| Delay | Worker slots | Ingress slots | Theoretical slot bytes |
| --- | ---: | ---: | ---: |
| 100ms | 2048 | 2048 | 256 MiB |
| 200ms | 4096 | 4096 | 512 MiB |

The first attempt showed `worker_slots=128 ingress_slots=128` in the startup log even when CLI overrides were passed. The cause was custom tuning override order: Linux relay defaults were applied after overrides. The local code was fixed so explicit custom overrides are replayed after Linux defaults/high-BDP scaling. The valid runs below use startup logs showing `2048/2048` and `4096/4096`.

## Netem Verification

| Delay | Peer qdisc | Ping avg |
| --- | --- | ---: |
| 100ms | `qdisc netem ... limit 1000000 delay 100ms` | 102.0 ms |
| 200ms | `qdisc netem ... limit 1000000 delay 200ms` | 201.7 ms |

## Throughput

| Peer delay | Direction | Exit | Throughput | Local bytes | Server bytes | Server seconds |
| --- | --- | ---: | ---: | ---: | ---: | ---: |
| 100ms | download | 0 | 12.405 Gbps | 46,517,500,370 | 47,007,203,562 | 30.105 |
| 100ms | upload | 0 | 14.848 Gbps | 55,965,748,928 | 55,872,118,199 | 30.104 |
| 200ms | download | 0 | 12.358 Gbps | 46,343,919,012 | 47,132,270,069 | 30.286 |
| 200ms | upload | 1 | 13.675 Gbps | 51,796,610,752 | 51,630,787,197 | 30.204 |

The 200ms upload returned `SPEED_RESULT` successfully and produced valid byte/throughput data, but exited `1` because the stop-boundary local/server byte mismatch was 165,823,555 bytes, slightly above the current 150,994,944-byte tolerance.

## Comparison Against 128 Slots

| Peer delay | Direction | 128 slots | Tuned slots | Change |
| --- | --- | ---: | ---: | ---: |
| 100ms | download | 1.186 Gbps | 12.405 Gbps | 10.5x |
| 100ms | upload | 1.178 Gbps | 14.848 Gbps | 12.6x |
| 200ms | download | 0.628 Gbps | 12.358 Gbps | 19.7x |
| 200ms | upload | 0.581 Gbps | 13.675 Gbps | 23.5x |

## Server Metrics Delta

| Delay | Direction | `tcp_read_bytes` | `tcp_write_bytes` | `errors` | `tcp_write_hard_errors` | `read_disabled` | `tcp_write_eagain` | slot failures | `max_tcp_read_iov` | `max_tcp_write_iov` | `max_sendmsg_bytes` | `max_pending_quic_rx` |
| --- | --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| 100ms | download | 47,007,203,562 | 0 | 0 | 0 | 10,537 | 0 | 0 | 32 | 0 | 0 | 0 |
| 100ms | upload | 0 | 55,872,492,859 | 710 | 710 | 1 | 0 | 0 | 32 | 3 | 257,097,255 | 257,097,255 |
| 200ms | download | 47,132,270,069 | 0 | 0 | 0 | 9,851 | 0 | 0 | 32 | 0 | 0 | 0 |
| 200ms | upload | 0 | 51,630,975,957 | 1,926 | 1,926 | 1 | 0 | 0 | 32 | 3 | 532,652,536 | 532,652,536 |

Upload `tcp_write_hard_errors` are teardown EPIPE-class errors after the receiving side closes. There were no TCP EAGAIN/partial writes and no slot acquire failures in the valid runs.

## Conclusion

The previous 100ms/200ms drop was primarily the 128-slot per-tunnel buffer depth limiting in-flight data to about 16MiB. Increasing slots to match BDP restored 1x1 throughput close to the no-delay 15Gbps baseline:

- 100ms: upload nearly returned to baseline; download reached about 12.4Gbps.
- 200ms: both directions reached 12-14Gbps, far above the 0.6Gbps 128-slot limit.

The remaining gap is likely congestion-control ramp/measurement-window behavior and stop-boundary accounting, not socket write blocking or slot exhaustion.
