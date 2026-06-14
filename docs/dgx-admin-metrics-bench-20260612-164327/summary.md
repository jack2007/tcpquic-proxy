# DGX admin metrics bench

- duration: 12s
- case: proxy-4x16
- throughput: 17.238 Gbps
- client role/status: client / healthy
- client connected_connections: 16 / 16
- server role/status: server / healthy

### client metrics delta

| 字段 | before | after | delta |
|------|--------|-------|-------|
| `linux_relay_tcp_read_bytes` | 0 | 424 | 424 |
| `linux_relay_tcp_write_bytes` | 0 | 6291456792 | 6291456792 |
| `linux_relay_tcp_write_sendmsg_calls` | 0 | 45494 | 45494 |
| `linux_relay_max_tcp_write_sendmsg_bytes` | 0 | 4154012 | 4154012 |
| `linux_relay_tcp_write_eagain_count` | 0 | 0 | 0 |
| `linux_relay_tcp_write_partial_count` | 0 | 0 | 0 |
| `linux_relay_deferred_receive_complete_bytes` | 0 | 6291456792 | 6291456792 |
| `linux_relay_deferred_receive_completes` | 0 | 45494 | 45494 |
| `linux_relay_deferred_receive_completion_flushes` | 0 | 45494 | 45494 |
| `linux_relay_max_pending_quic_receive_bytes` | 0 | 4154012 | 4154012 |
| `linux_relay_max_pending_quic_receive_queue` | 0 | 1 | 1 |
| `linux_relay_inline_quic_receive_attempts` | 0 | 0 | 0 |
| `linux_relay_inline_quic_receive_full_writes` | 0 | 0 | 0 |
| `linux_relay_inline_quic_receive_partial_writes` | 0 | 0 | 0 |
| `linux_relay_inline_quic_receive_eagain_count` | 0 | 0 | 0 |
| `linux_relay_inline_quic_receive_budget_exceeded` | 0 | 0 | 0 |
| `linux_relay_inline_quic_receive_bytes` | 0 | 0 | 0 |
| `linux_relay_max_inline_quic_receive_bytes` | 0 | 0 | 0 |
| `linux_relay_errors` | 0 | 0 | 0 |

- average complete bytes: 138292.0
- average TCP write bytes/sendmsg: 138292.0

### server metrics delta

| 字段 | before | after | delta |
|------|--------|-------|-------|
| `linux_relay_tcp_read_bytes` | 0 | 6291456792 | 6291456792 |
| `linux_relay_tcp_write_bytes` | 0 | 424 | 424 |
| `linux_relay_tcp_write_sendmsg_calls` | 0 | 4 | 4 |
| `linux_relay_max_tcp_write_sendmsg_bytes` | 0 | 106 | 106 |
| `linux_relay_tcp_write_eagain_count` | 0 | 0 | 0 |
| `linux_relay_tcp_write_partial_count` | 0 | 0 | 0 |
| `linux_relay_deferred_receive_complete_bytes` | 0 | 424 | 424 |
| `linux_relay_deferred_receive_completes` | 0 | 4 | 4 |
| `linux_relay_deferred_receive_completion_flushes` | 0 | 4 | 4 |
| `linux_relay_max_pending_quic_receive_bytes` | 0 | 106 | 106 |
| `linux_relay_max_pending_quic_receive_queue` | 0 | 1 | 1 |
| `linux_relay_inline_quic_receive_attempts` | 0 | 0 | 0 |
| `linux_relay_inline_quic_receive_full_writes` | 0 | 0 | 0 |
| `linux_relay_inline_quic_receive_partial_writes` | 0 | 0 | 0 |
| `linux_relay_inline_quic_receive_eagain_count` | 0 | 0 | 0 |
| `linux_relay_inline_quic_receive_budget_exceeded` | 0 | 0 | 0 |
| `linux_relay_inline_quic_receive_bytes` | 0 | 0 | 0 |
| `linux_relay_max_inline_quic_receive_bytes` | 0 | 0 | 0 |
| `linux_relay_errors` | 0 | 37116400 | 37116400 |

- average complete bytes: 106.0
- average TCP write bytes/sendmsg: 106.0
