# DGX built-in speed-test download producer fix

- Date: 2026-06-13
- Topology: 2x DGX 1x1, `169.254.250.230` -> `169.254.59.196:4433`
- Mode: built-in `--download-test 30` / `--upload-test 30`
- Tuning: custom 1GiB QUIC windows, 1MiB relay IO, 4GiB memory cap, active TCP socket buffer `67108864`
- Result directory: `docs/dgx-built-in-speed-test-fix-20260613-120601/`

## Change Under Test

The built-in speed-test loopback producer no longer hard-codes `SO_SNDBUF=256KiB`.

Both server accepted loopback sockets and client socketpair endpoints now use the active throughput TCP buffer. On this DGX run the requested buffer was `67108864` and Linux reported effective local speed socket buffers of `134217728`.

The upload byte-mismatch tolerance was also adjusted for high-throughput stop boundaries. The local upload worker counts bytes accepted into the local socketpair, while the server counts bytes after relay delivery. With 64MiB requested buffers, Linux can expose roughly 128MiB effective socket buffering, so a fixed 16MiB mismatch limit was too strict.

## Download Verification

| Run | Exit | Throughput | Local bytes | Server bytes |
| --- | ---: | ---: | ---: | ---: |
| run-1 | 0 | 17.407 Gbps | 65,276,514,805 | 65,525,141,132 |
| run-2 | 0 | 16.582 Gbps | 62,181,127,615 | 62,454,774,228 |
| run-3 | 0 | 16.265 Gbps | 60,992,963,976 | 61,234,367,557 |
| final2 | 0 | 16.712 Gbps | 62,671,371,932 | 62,932,865,611 |

The previous unstable built-in download results were `1.102`, `0.098`, and `17.118` Gbps. After the loopback socket buffer fix, the repeated download runs stayed in the 16-17 Gbps range.

Final2 server metrics delta:

| Metric | Delta |
| --- | ---: |
| `linux_relay_tcp_read_bytes` | 62,932,865,611 |
| `linux_relay_tcp_write_bytes` | 0 |
| `linux_relay_errors` | 0 |
| `linux_relay_tcp_write_hard_errors` | 0 |
| `linux_relay_read_disabled_count` | 15,002 |
| `linux_relay_max_tcp_read_iov_used` | 32 |
| `linux_relay_max_pending_quic_receive_bytes` | 3,546,392 |

## Upload Sanity

| Run | Exit | Throughput | Local bytes | Server bytes |
| --- | ---: | ---: | ---: | ---: |
| final2 | 0 | 16.057 Gbps | 60,302,659,264 | 60,219,834,822 |

Final2 server metrics delta:

| Metric | Delta |
| --- | ---: |
| `linux_relay_tcp_read_bytes` | 0 |
| `linux_relay_tcp_write_bytes` | 60,220,023,582 |
| `linux_relay_errors` | 30 |
| `linux_relay_tcp_write_hard_errors` | 30 |
| `linux_relay_read_disabled_count` | 1 |
| `linux_relay_max_tcp_write_iov_used` | 3 |
| `linux_relay_max_tcp_write_sendmsg_bytes` | 3,546,392 |

The upload hard errors occur at test teardown after the receiving side closes; the run still exits `0` and reports normal throughput.

## Conclusion

The hypothesis was confirmed: the built-in download instability was caused by the internal loopback producer path using much smaller socket buffers than the external curl path. Once the built-in speed-test local sockets use the active throughput buffer, download throughput becomes stable and comparable to external curl.
