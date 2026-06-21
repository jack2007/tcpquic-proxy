# tcpquic-proxy receive resume threshold validation

- change: Linux relay QUIC receive pause/resume threshold changed to:
  - pause MsQuic receive when `PendingQuicReceiveBytes >= relay_pending`
  - resume MsQuic receive when `PendingQuicReceiveBytes < relay_pending`
- previous behavior resumed only when `PendingQuicReceiveBytes <= relay_pending / 2`
- topology: 2*DGX, 1 QUIC connection, 1 stream, 1 HTTP CONNECT download
- netem: sender side, `limit 5000000`, no `rate` cap
- payload: 6000MiB repeat payload
- tuning: `srw=2GiB`, `fcw=2GiB`, `initial_read_ahead=128MiB`, `max_pending_buffer=1GiB`, `relay_pending=1GiB`

| case | throughput | curl exit |
|---|---:|---:|
| 100ms + 5% loss | 4.56Gbps | 0 |
| 200ms + 5% loss | 4.42Gbps | 0 |

## Trace Summary

| case | server max posted | server max ideal | server max relay buffer | client max pending receive | receive pause |
|---|---:|---:|---:|---:|---:|
| 100ms + 5% loss | 436,103,312 | 435,848,049 | 503,316,480 | 264,344,514 | 0 |
| 200ms + 5% loss | 873,293,594 | 980,658,109 | 1,073,741,824 | 461,500,454 | 0 |

## Interpretation

- The new resume condition works as requested and is covered by a regression test: pending bytes below `relay_pending` but above `relay_pending / 2` now resumes MsQuic receive.
- Both DGX runs completed successfully with `curl_exit=0`.
- No receive pause, MsQuic send resource backpressure, or relay errors were observed in either final run.
- 200ms improved versus the previous 1GiB receive-pending run; the sender posted more data and MsQuic raised ideal-send close to 1GiB.
- 100ms was lower than the previous best single run, so this should be treated as one sample rather than a final performance ranking.

## Verification

- RED: `tcpquic_linux_relay_worker_io_test` failed before the production change with `pending=28672 max=32768 pauses=1 resumes=0`.
- GREEN:
  - `cmake --build build-plan --target tcpquic_linux_relay_worker_io_test tcpquic_tuning_test tcpquic_quic_settings_test tcpquic-proxy --parallel 8`
  - `./build-plan/bin/Release/tcpquic_linux_relay_worker_io_test`
  - `./build-plan/bin/Release/tcpquic_tuning_test`
  - `./build-plan/bin/Release/tcpquic_quic_settings_test`
