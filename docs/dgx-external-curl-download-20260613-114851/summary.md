# DGX External Curl Download Comparison - 2026-06-13 11:48

## Scope

- Topology: 2 DGX hosts, 1 client host + 1 server host
- Scenario: external curl download through tcpquic-proxy HTTP CONNECT
- QUIC/data shape: `quic=1`, `curl=1`
- Duration window: `30s`
- Payload: remote sparse `64GiB` file, so curl is expected to time out at 30s while still reporting `speed_download`
- HTTP source: `busybox httpd` on `169.254.59.196:16001`
- Proxy target: `http://169.254.59.196:16001/tcpquic-dgx-payload-64g.bin`
- Proxy client: HTTP CONNECT on `127.0.0.1:18080`, admin on `127.0.0.1:18082`
- Proxy server: QUIC on `169.254.59.196:4433`, admin on `127.0.0.1:18081`

Tuning used by the external curl run follows the existing DGX perf/curl scripts:

```text
--tuning custom
--quic-fcw 1073741824
--quic-srw 1073741824
--quic-iw 4000
--quic-initrtt-ms 1
--relay-io-size 1048576
--relay-inflight-bytes 1073741824
--max-memory-mb 4096
--quic-connections 1
--compress off
```

Raw artifacts:

- Curl output: `curl/run-{1,2,3}.speed_bps`, `curl/run-{1,2,3}.stderr`, `curl/run-{1,2,3}.rc`
- Client metrics: `metrics/client-{1,2,3}-{before,after}.json`
- Server metrics: `metrics/server-{1,2,3}-{before,after}.json`
- Logs: `logs/client-final.log`, `logs/server.log`

## External Curl Throughput

`rc=28` is expected here: curl stopped because the 64GiB transfer exceeded the 30s measurement window. The byte count and `speed_download` still cover the active 30s interval.

| Run | curl rc | curl speed_download B/s | Gbps | Curl stderr |
|---:|---:|---:|---:|---|
| 1 | 28 | 2,035,438,284 | 16.284 | timed out after 30s with 61.06GB received |
| 2 | 28 | 1,945,467,772 | 15.564 | timed out after 30s with 58.36GB received |
| 3 | 28 | 2,058,540,846 | 16.468 | timed out after 30s with 61.75GB received |

Average external curl download throughput: **16.105 Gbps**.

## Metrics Delta

Delta is `after - before` for each run.

### Client Metrics

In external download, client relay receives QUIC data and writes to the curl TCP socket, so `tcp_write_bytes` is the dominant client-side byte counter.

| Run | tcp_read_bytes | tcp_write_bytes | tcp_write_sendmsg_calls | deferred_receive_complete_bytes | deferred_receive_completes | read_disabled_count | relay_errors | tcp_write_hard_errors | last_tcp_write_errno |
|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| 1 | 110 | 61,061,225,452 | 604,326 | 61,061,225,452 | 604,326 | 1 | 207 | 207 | 32 |
| 2 | 110 | 58,362,550,507 | 487,220 | 58,362,550,507 | 487,220 | 1 | 80 | 80 | 0 |
| 3 | 110 | 61,754,728,947 | 813,784 | 61,754,728,947 | 813,784 | 1 | 90 | 90 | 0 |

### Server Metrics

In external download, server relay reads from busybox HTTP over TCP and sends QUIC to the client, so `tcp_read_bytes` is the dominant server-side byte counter.

| Run | total_streams | active_streams delta | tcp_read_bytes | tcp_write_bytes | pending_bytes delta | read_disabled_count | relay_errors | last_quic_send_status |
|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| 1 | +1 | +1 | 61,715,251,348 | 110 | +16,777,216 | 18,705 | 0 | 0 |
| 2 | +1 | +1 | 59,268,462,721 | 110 | +16,777,216 | 14,407 | 0 | 0 |
| 3 | +1 | +1 | 62,440,011,481 | 110 | +16,777,216 | 14,941 | 0 | 0 |

## Comparison With Built-in Download

Most recent built-in `--download-test 30` results from `docs/dgx-built-in-speed-test-20260613-112327/summary.md`:

| Test | Run 1 | Run 2 | Run 3 | Average |
|---|---:|---:|---:|---:|
| built-in download | 1.102 Gbps | 0.098 Gbps | 17.118 Gbps | 6.106 Gbps |
| external curl download | 16.284 Gbps | 15.564 Gbps | 16.468 Gbps | 16.105 Gbps |

The external curl path is stable and reaches the expected `10Gbps+` range. The built-in download path is functionally completing but unstable.

## Key Difference

The proxy data direction is the same at a high level:

```text
server TCP source -> server TCP-to-QUIC relay -> QUIC -> client QUIC-to-TCP relay -> local reader
```

But the TCP source and socket setup are different.

### External curl download

- Server target is a real HTTP server (`busybox httpd`) serving a large file.
- The server relay dials that target through `TqDialTcp()`.
- `TqDialTcp()` calls `TqTuneTcpForThroughput()`, which applies the configured high-throughput TCP socket buffers.
- In this run the server relay consistently read `59-62GB` from TCP in each 30s window.

### Built-in download

- Server target is an internal loopback listener created by `TqServerSpeedTestController`.
- The download source is `TqRunDownloadWorker()`, which repeatedly sends a 64KiB buffer into the accepted loopback socket.
- The accepted socket is configured with:
  - `TqSetSpeedSocketTimeout(accepted, 100)`
  - `SO_SNDBUF = 256KiB`
- Client-side speed-test socket pairs are also fixed at `256KiB` buffers.
- The built-in source does **not** use the same `TqTuneTcpForThroughput()` path as external curl.

This explains the metrics:

- External curl low-level feed is healthy: server `tcp_read_bytes` is `59-62GB` per 30s run.
- Built-in low-throughput runs show low server `tcp_read_bytes` (`4.64GB` and `0.88GB` in the low runs), so the server-side source/loopback feed did not keep the relay full.

## Interpretation

The current evidence does **not** show a general download data-path regression: external curl 1x1 download remains stable at about `16Gbps`.

The instability is specific to the built-in download generator. The likely root cause is that the built-in loopback TCP source is not representative of the external curl path:

1. Built-in download uses a small `256KiB` source-side socket buffer.
2. Built-in download uses a 100ms send timeout.
3. Built-in download writes through an artificial loopback producer, while external curl uses a real HTTP target plus the normal tuned TCP dialer path.
4. Built-in speed-test currently has extra control/data session behavior that external curl does not use.

## Next Steps

To make built-in download comparable to external curl:

1. Apply the same throughput TCP tuning to speed-test loopback sockets that external target TCP sockets get from `TqTuneTcpForThroughput()`.
2. Avoid hard-coded `256KiB` buffers for speed-test source and socket pairs.
3. Add built-in speed-test source metrics: send calls, send bytes, send EAGAIN/timeouts, partial sends, and max effective socket buffer.
4. Re-run built-in download after tuning and compare server `tcp_read_bytes` against external curl.
5. Keep external curl 1x1 as the reference acceptance check for the real proxy download path.
