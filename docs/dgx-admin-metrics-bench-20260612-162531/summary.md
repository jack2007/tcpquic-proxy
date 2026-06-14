# DGX admin metrics bench

- duration: 12s
- case: proxy-4x16
- throughput: 17.803 Gbps
- client role/status: client / healthy
- client connected_connections: 16 / 16
- server role/status: server / healthy

### client metrics delta

| 字段 | before | after | delta |
|------|--------|-------|-------|
| `linux_relay_tcp_read_bytes` | 0 | 424 | 424 |
| `linux_relay_tcp_write_bytes` | 0 | 6291456792 | 6291456792 |
| `linux_relay_tcp_write_sendmsg_calls` | 0 | 45463 | 45463 |
| `linux_relay_max_tcp_write_sendmsg_bytes` | 0 | 4152579 | 4152579 |
| `linux_relay_tcp_write_eagain_count` | 0 | 0 | 0 |
| `linux_relay_tcp_write_partial_count` | 0 | 0 | 0 |
| `linux_relay_deferred_receive_complete_bytes` | 0 | 6168013688 | 6168013688 |
| `linux_relay_deferred_receive_completes` | 0 | 2909 | 2909 |
| `linux_relay_deferred_receive_completion_flushes` | 0 | 2909 | 2909 |
| `linux_relay_max_pending_quic_receive_bytes` | 0 | 4152579 | 4152579 |
| `linux_relay_max_pending_quic_receive_queue` | 0 | 1 | 1 |
| `linux_relay_inline_quic_receive_attempts` | 0 | 42554 | 42554 |
| `linux_relay_inline_quic_receive_full_writes` | 0 | 42554 | 42554 |
| `linux_relay_inline_quic_receive_partial_writes` | 0 | 0 | 0 |
| `linux_relay_inline_quic_receive_eagain_count` | 0 | 0 | 0 |
| `linux_relay_inline_quic_receive_budget_exceeded` | 0 | 2909 | 2909 |
| `linux_relay_inline_quic_receive_bytes` | 0 | 123443104 | 123443104 |
| `linux_relay_max_inline_quic_receive_bytes` | 0 | 130840 | 130840 |
| `linux_relay_errors` | 0 | 0 | 0 |

- average complete bytes: 2120321.0
- average TCP write bytes/sendmsg: 138386.3

### server metrics delta

| 字段 | before | after | delta |
|------|--------|-------|-------|
| `linux_relay_tcp_read_bytes` | 0 | 6291456792 | 6291456792 |
| `linux_relay_tcp_write_bytes` | 0 | 424 | 424 |
| `linux_relay_tcp_write_sendmsg_calls` | 0 | 4 | 4 |
| `linux_relay_max_tcp_write_sendmsg_bytes` | 0 | 106 | 106 |
| `linux_relay_tcp_write_eagain_count` | 0 | 0 | 0 |
| `linux_relay_tcp_write_partial_count` | 0 | 0 | 0 |
| `linux_relay_deferred_receive_complete_bytes` | 0 | 0 | 0 |
| `linux_relay_deferred_receive_completes` | 0 | 0 | 0 |
| `linux_relay_deferred_receive_completion_flushes` | 0 | 0 | 0 |
| `linux_relay_max_pending_quic_receive_bytes` | 0 | 0 | 0 |
| `linux_relay_max_pending_quic_receive_queue` | 0 | 0 | 0 |
| `linux_relay_inline_quic_receive_attempts` | 0 | 4 | 4 |
| `linux_relay_inline_quic_receive_full_writes` | 0 | 4 | 4 |
| `linux_relay_inline_quic_receive_partial_writes` | 0 | 0 | 0 |
| `linux_relay_inline_quic_receive_eagain_count` | 0 | 0 | 0 |
| `linux_relay_inline_quic_receive_budget_exceeded` | 0 | 0 | 0 |
| `linux_relay_inline_quic_receive_bytes` | 0 | 424 | 424 |
| `linux_relay_max_inline_quic_receive_bytes` | 0 | 106 | 106 |
| `linux_relay_errors` | 0 | 35841262 | 35841262 |

- average complete bytes: 0.0
- average TCP write bytes/sendmsg: 106.0
