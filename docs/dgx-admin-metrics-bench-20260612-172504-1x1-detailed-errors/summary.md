# DGX admin metrics bench

- duration: 12s
- case: proxy-1x1
- throughput: 16.594 Gbps
- client role/status: client / healthy
- client connected_connections: 1 / 1
- server role/status: server / healthy

### client metrics delta

| 字段 | before | after | delta |
|------|--------|-------|-------|
| `linux_relay_tcp_read_bytes` | 0 | 106 | 106 |
| `linux_relay_tcp_write_bytes` | 0 | 1572864198 | 1572864198 |
| `linux_relay_tcp_write_sendmsg_calls` | 0 | 23092 | 23092 |
| `linux_relay_max_tcp_write_sendmsg_bytes` | 0 | 2200569 | 2200569 |
| `linux_relay_tcp_write_eagain_count` | 0 | 0 | 0 |
| `linux_relay_tcp_write_partial_count` | 0 | 0 | 0 |
| `linux_relay_deferred_receive_complete_bytes` | 0 | 1572864198 | 1572864198 |
| `linux_relay_deferred_receive_completes` | 0 | 23092 | 23092 |
| `linux_relay_deferred_receive_completion_flushes` | 0 | 23092 | 23092 |
| `linux_relay_max_pending_quic_receive_bytes` | 0 | 2200569 | 2200569 |
| `linux_relay_max_pending_quic_receive_queue` | 0 | 1 | 1 |
| `linux_relay_inline_quic_receive_attempts` | 0 | 0 | 0 |
| `linux_relay_inline_quic_receive_full_writes` | 0 | 0 | 0 |
| `linux_relay_inline_quic_receive_partial_writes` | 0 | 0 | 0 |
| `linux_relay_inline_quic_receive_eagain_count` | 0 | 0 | 0 |
| `linux_relay_inline_quic_receive_budget_exceeded` | 0 | 0 | 0 |
| `linux_relay_inline_quic_receive_bytes` | 0 | 0 | 0 |
| `linux_relay_max_inline_quic_receive_bytes` | 0 | 0 | 0 |
| `linux_relay_errors` | 0 | 0 | 0 |

- average complete bytes: 68112.9
- average TCP write bytes/sendmsg: 68112.9

### server metrics delta

| 字段 | before | after | delta |
|------|--------|-------|-------|
| `linux_relay_tcp_read_bytes` | 0 | 1572864198 | 1572864198 |
| `linux_relay_tcp_write_bytes` | 0 | 106 | 106 |
| `linux_relay_tcp_write_sendmsg_calls` | 0 | 1 | 1 |
| `linux_relay_max_tcp_write_sendmsg_bytes` | 0 | 106 | 106 |
| `linux_relay_tcp_write_eagain_count` | 0 | 0 | 0 |
| `linux_relay_tcp_write_partial_count` | 0 | 0 | 0 |
| `linux_relay_deferred_receive_complete_bytes` | 0 | 106 | 106 |
| `linux_relay_deferred_receive_completes` | 0 | 1 | 1 |
| `linux_relay_deferred_receive_completion_flushes` | 0 | 1 | 1 |
| `linux_relay_max_pending_quic_receive_bytes` | 0 | 106 | 106 |
| `linux_relay_max_pending_quic_receive_queue` | 0 | 1 | 1 |
| `linux_relay_inline_quic_receive_attempts` | 0 | 0 | 0 |
| `linux_relay_inline_quic_receive_full_writes` | 0 | 0 | 0 |
| `linux_relay_inline_quic_receive_partial_writes` | 0 | 0 | 0 |
| `linux_relay_inline_quic_receive_eagain_count` | 0 | 0 | 0 |
| `linux_relay_inline_quic_receive_budget_exceeded` | 0 | 0 | 0 |
| `linux_relay_inline_quic_receive_bytes` | 0 | 0 | 0 |
| `linux_relay_max_inline_quic_receive_bytes` | 0 | 0 | 0 |
| `linux_relay_errors` | 0 | 1984331 | 1984331 |

- average complete bytes: 106.0
- average TCP write bytes/sendmsg: 106.0
