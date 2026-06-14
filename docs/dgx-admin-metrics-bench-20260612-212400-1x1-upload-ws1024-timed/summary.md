# DGX admin metrics bench

- duration: 12s
- case: proxy-1x1
- throughput: 34962732.317 Gbps
- client role/status: client / healthy
- client connected_connections: 1 / 1
- server role/status: server / healthy

### client metrics delta

| 字段 | before | after | delta |
|------|--------|-------|-------|
| `linux_relay_tcp_read_bytes` | 0 | 536871100 | 536871100 |
| `linux_relay_tcp_write_bytes` | 0 | 138 | 138 |
| `linux_relay_tcp_write_sendmsg_calls` | 0 | 3 | 3 |
| `linux_relay_max_tcp_write_sendmsg_bytes` | 0 | 111 | 111 |
| `linux_relay_tcp_write_eagain_count` | 0 | 0 | 0 |
| `linux_relay_tcp_write_partial_count` | 0 | 0 | 0 |
| `linux_relay_deferred_receive_complete_bytes` | 0 | 138 | 138 |
| `linux_relay_deferred_receive_completes` | 0 | 3 | 3 |
| `linux_relay_deferred_receive_completion_flushes` | 0 | 3 | 3 |
| `linux_relay_max_pending_quic_receive_bytes` | 0 | 111 | 111 |
| `linux_relay_max_pending_quic_receive_queue` | 0 | 1 | 1 |
| `linux_relay_inline_quic_receive_attempts` | 0 | 0 | 0 |
| `linux_relay_inline_quic_receive_full_writes` | 0 | 0 | 0 |
| `linux_relay_inline_quic_receive_partial_writes` | 0 | 0 | 0 |
| `linux_relay_inline_quic_receive_eagain_count` | 0 | 0 | 0 |
| `linux_relay_inline_quic_receive_budget_exceeded` | 0 | 0 | 0 |
| `linux_relay_inline_quic_receive_bytes` | 0 | 0 | 0 |
| `linux_relay_max_inline_quic_receive_bytes` | 0 | 0 | 0 |
| `linux_relay_errors` | 0 | 716207 | 716207 |

- average complete bytes: 46.0
- average TCP write bytes/sendmsg: 46.0

### server metrics delta

| 字段 | before | after | delta |
|------|--------|-------|-------|
| `linux_relay_tcp_read_bytes` | 0 | 138 | 138 |
| `linux_relay_tcp_write_bytes` | 0 | 536871100 | 536871100 |
| `linux_relay_tcp_write_sendmsg_calls` | 0 | 7132 | 7132 |
| `linux_relay_max_tcp_write_sendmsg_bytes` | 0 | 749970 | 749970 |
| `linux_relay_tcp_write_eagain_count` | 0 | 0 | 0 |
| `linux_relay_tcp_write_partial_count` | 0 | 0 | 0 |
| `linux_relay_deferred_receive_complete_bytes` | 0 | 536871100 | 536871100 |
| `linux_relay_deferred_receive_completes` | 0 | 7132 | 7132 |
| `linux_relay_deferred_receive_completion_flushes` | 0 | 7132 | 7132 |
| `linux_relay_max_pending_quic_receive_bytes` | 0 | 749970 | 749970 |
| `linux_relay_max_pending_quic_receive_queue` | 0 | 1 | 1 |
| `linux_relay_inline_quic_receive_attempts` | 0 | 0 | 0 |
| `linux_relay_inline_quic_receive_full_writes` | 0 | 0 | 0 |
| `linux_relay_inline_quic_receive_partial_writes` | 0 | 0 | 0 |
| `linux_relay_inline_quic_receive_eagain_count` | 0 | 0 | 0 |
| `linux_relay_inline_quic_receive_budget_exceeded` | 0 | 0 | 0 |
| `linux_relay_inline_quic_receive_bytes` | 0 | 0 | 0 |
| `linux_relay_max_inline_quic_receive_bytes` | 0 | 0 | 0 |
| `linux_relay_errors` | 0 | 0 | 0 |

- average complete bytes: 75276.4
- average TCP write bytes/sendmsg: 75276.4
