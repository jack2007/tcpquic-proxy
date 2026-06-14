# DGX built-in speed test 1x1

- quic connections: 1
- compress: off
- duration: 12s
- note: client speed-test mode exits before client admin starts, so client-side admin metrics are unavailable in current implementation.

## download

client rc: `1`
speed output parse: failed

client stdout:
```text

```

client stderr:
```text
tcpquic-proxy QUIC execution profile: max-throughput
tcpquic-proxy tuning: srw=536870912 fcw=500000000 iw=2000 initrtt=100ms relay_io=1048576 ideal_send=67108864 inflight=64 tcp_buf=4194304 relay_pending=4194304 tick_budget=67108864 read_batch=1048576 read_chunk=131072 worker_slots=128 ingress_slots=128 quic_complete_batch=0
tcpquic-proxy runtime tuning: enabled (RTT/throughput feed next QUIC connection)
tcpquic-proxy runtime: rtt=3ms throughput=0Mbps (pending) ideal_send=0 (pending) compress_ratio=0.0% (pending) samples=1
tcpquic-proxy: timed out waiting for SPEED_RESULT
```

### server metrics delta

| field | before | after | delta |
|---|---:|---:|---:|
| `accepted_connections` | 0 | 1 | 1 |
| `active_streams` | 0 | 2 | 2 |
| `total_streams` | 0 | 2 | 2 |
| `linux_relay_wakeups` | 0 | 2013 | 2013 |
| `linux_relay_events_processed` | 0 | 8560 | 8560 |
| `linux_relay_pending_bytes` | 0 | 14942208 | 14942208 |
| `linux_relay_tcp_read_bytes` | 0 | 1344929739 | 1344929739 |
| `linux_relay_tcp_write_bytes` | 0 | 0 | 0 |
| `linux_relay_max_tcp_read_iov_used` | 0 | 16 | 16 |
| `linux_relay_read_disabled_count` | 0 | 1567 | 1567 |
| `linux_relay_errors` | 0 | 0 | 0 |

## upload

client rc: `0`
throughput: **14.744 Gbps** (1757.65 MiB/s)
local/server bytes: `22126788608` / `22126788608`
seconds: local `12.000`, server `12.006`; accepted/closed `1`/`1`

client stderr:
```text
tcpquic-proxy QUIC execution profile: max-throughput
tcpquic-proxy tuning: srw=536870912 fcw=500000000 iw=2000 initrtt=100ms relay_io=1048576 ideal_send=67108864 inflight=64 tcp_buf=4194304 relay_pending=4194304 tick_budget=67108864 read_batch=1048576 read_chunk=131072 worker_slots=128 ingress_slots=128 quic_complete_batch=0
tcpquic-proxy runtime tuning: enabled (RTT/throughput feed next QUIC connection)
tcpquic-proxy runtime: rtt=1ms throughput=0Mbps (pending) ideal_send=0 (pending) compress_ratio=0.0% (pending) samples=1
```

### server metrics delta

| field | before | after | delta |
|---|---:|---:|---:|
| `accepted_connections` | 0 | 1 | 1 |
| `active_streams` | 0 | 1 | 1 |
| `total_streams` | 0 | 2 | 2 |
| `linux_relay_wakeups` | 0 | 50083 | 50083 |
| `linux_relay_events_processed` | 0 | 186357 | 186357 |
| `linux_relay_tcp_read_bytes` | 0 | 0 | 0 |
| `linux_relay_tcp_write_bytes` | 0 | 22126788608 | 22126788608 |
| `linux_relay_max_tcp_write_iov_used` | 0 | 3 | 3 |
| `linux_relay_tcp_write_sendmsg_calls` | 0 | 186355 | 186355 |
| `linux_relay_max_tcp_write_sendmsg_bytes` | 0 | 2553972 | 2553972 |
| `linux_relay_read_disabled_count` | 0 | 1 | 1 |
| `linux_relay_deferred_receive_complete_bytes` | 0 | 22126788608 | 22126788608 |
| `linux_relay_deferred_receive_completes` | 0 | 186355 | 186355 |
| `linux_relay_deferred_receive_completion_flushes` | 0 | 186355 | 186355 |
| `linux_relay_max_pending_quic_receive_bytes` | 0 | 2553972 | 2553972 |
| `linux_relay_max_pending_quic_receive_queue` | 0 | 1 | 1 |
| `linux_relay_errors` | 0 | 0 | 0 |

