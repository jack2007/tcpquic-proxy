# DGX Built-in Speed Test - 2026-06-13 11:23

## Scope

- Topology: 2 DGX hosts
- Client peer: `169.254.59.196:4433`
- QUIC connections requested by test: `--quic-connections 1`
- Compression: `--compress off`
- Test duration: `30s` per run
- Runs: upload x3, download x3
- Server admin metrics: captured before/after each run from `127.0.0.1:18081/metrics`

Raw artifacts:

- Upload client output: `upload/run-{1,2,3}.stdout`, `upload/run-{1,2,3}.stderr`, `upload/run-{1,2,3}.rc`
- Download client output: `download/run-{1,2,3}.stdout`, `download/run-{1,2,3}.stderr`, `download/run-{1,2,3}.rc`
- Server metrics snapshots: `metrics/{upload,download}-{1,2,3}-{before,after}.json`
- Server log: `logs/server.log`

Note: the speed-test client process does not expose an admin `/metrics` endpoint in this mode, so client-side metrics are represented by client stdout/stderr only.

## Throughput

| Direction | Run | RC | local_bytes | server_bytes | Gbps | MiB/s | Notes |
|---|---:|---:|---:|---:|---:|---:|---|
| upload | 1 | 0 | 56,975,003,328 | 56,962,618,932 | 15.189 | 1810.62 | ok |
| upload | 2 | 0 | 64,003,047,424 | 63,991,116,267 | 17.063 | 2034.02 | ok |
| upload | 3 | 0 | 65,003,978,752 | 64,991,537,257 | 17.329 | 2065.83 | ok |
| download | 1 | 0 | 4,131,892,005 | 4,650,172,098 | 1.102 | 131.35 | local/server mismatch warning |
| download | 2 | 0 | 367,148,510 | 888,405,698 | 0.098 | 11.67 | local/server mismatch warning |
| download | 3 | 0 | 64,192,551,306 | 64,213,090,304 | 17.118 | 2040.63 | local/server mismatch warning |

Average throughput:

| Direction | Average Gbps |
|---|---:|
| upload | 16.527 |
| download | 6.106 |

## Server Metrics Delta

Delta is `after - before` for each run.

| Direction | Run | accepted_connections | total_streams | active_streams delta | tcp_read_bytes | tcp_write_bytes | pending_bytes delta | read_disabled_count | relay_errors |
|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| upload | 1 | +2 | +2 | +1 | 0 | 56,962,807,692 | 0 | +1 | +22 |
| upload | 2 | +2 | +2 | -1 | 0 | 63,991,242,099 | 0 | +1 | +24 |
| upload | 3 | +2 | +2 | +1 | 0 | 64,991,583,369 | 0 | +1 | +17 |
| download | 1 | +2 | +2 | +1 | 4,641,390,539 | 0 | +15,204,352 | +8,172 | 0 |
| download | 2 | +2 | +2 | 0 | 879,624,139 | 0 | 0 | +1,053 | 0 |
| download | 3 | +2 | +2 | 0 | 64,213,090,304 | 0 | -15,204,352 | +132,011 | 0 |

Additional upload server deltas:

| Run | tcp_write_sendmsg_calls | deferred_receive_complete_bytes | deferred_receive_completes | tcp_write_hard_errors | last_tcp_write_errno |
|---:|---:|---:|---:|---:|---:|
| 1 | 527,773 | 56,962,807,692 | 527,773 | 22 | 32 |
| 2 | 699,034 | 63,991,242,099 | 699,034 | 24 | 0 |
| 3 | 623,545 | 64,991,583,369 | 623,545 | 17 | 0 |

Additional download server deltas:

| Run | tcp_write_sendmsg_calls | deferred_receive_complete_bytes | deferred_receive_completes | tcp_write_hard_errors | last_quic_send_status |
|---:|---:|---:|---:|---:|---:|
| 1 | 0 | 0 | 0 | 0 | 0 |
| 2 | 0 | 0 | 0 | 0 | 0 |
| 3 | 0 | 0 | 0 | 0 | 0 |

## Observations

1. Upload is stable across 30s runs: `15.189-17.329 Gbps`, average `16.527 Gbps`.
2. Download is highly unstable: `0.098-17.118 Gbps`, average `6.106 Gbps`.
3. No run timed out waiting for `SPEED_RESULT`; all six client runs exited with `rc=0`.
4. Download server-side relay errors stayed at `0`, so the low download runs are not explained by relay hard errors.
5. Download low-throughput runs also show low `tcp_read_bytes` on the server, meaning the server-side TCP-to-QUIC send path did not feed much data during those 30s windows.
6. Upload increments `linux_relay_tcp_write_hard_errors` with `EPIPE(32)`-style close behavior during test teardown. This should likely be split out from unexpected relay errors in future metrics.

## Interpretation

The current built-in speed-test path is functionally completing: all upload/download runs return `SPEED_RESULT` and exit `0`.

Performance is asymmetric:

- Upload is consistently high.
- Download can reach high throughput (`17.118 Gbps`) but can also collapse to near idle (`0.098 Gbps`) without relay errors.

The next debugging target should be download startup/data-slot behavior and server TCP-to-QUIC feed rate, not the control-plane result timeout.
