# tcpquic-proxy download slow investigation

Date: 2026-06-15

## Summary

Current evidence strongly points to `tcpquic-proxy` download direction being
limited by the local proxy client `QUIC receive -> TCP write` path, not by the
new `iperf3 --proxy` read path.

The most important observation is:

- `iperf3 --proxy` through a normal high-performance proxy (`gost`) reaches
  near direct TCP throughput in both upload and download.
- `iperf3 --proxy` through `tcpquic-proxy` reaches much lower download
  throughput and shows heavy TCP write backpressure on the local
  `tcpquic-proxy client`.

This makes the current leading hypothesis:

> Download is slow because the local `tcpquic-proxy client` cannot drain QUIC
> receive data into the local TCP socket fast enough. The TCP write side hits
> frequent `EAGAIN`, pending QUIC receive bytes grow to about 500 MiB, and
> receive completion pacing starts throttling the relay.

## Direct TCP baseline

From `docs/dgx-iperf-proxy-bottleneck-analysis-20260615.md`:

| Case | Direction | TCP streams | Throughput |
|---|---|---:|---:|
| direct-upload-P1 | upload | 1 | 45.064 Gbps |
| direct-upload-P4 | upload | 4 | 106.354 Gbps |
| direct-upload-P16 | upload | 16 | 106.159 Gbps |
| direct-download-P1 | download | 1 | 59.166 Gbps |
| direct-download-P4 | download | 4 | 98.104 Gbps |
| direct-download-P16 | download | 16 | 98.285 Gbps |

This proves the DGX link, kernel TCP stack, and iperf3 itself can reach about
100 Gbps with multiple TCP streams.

## iperf3 through tcpquic-proxy

### Single server q16/P16

From `docs/dgx-iperf-proxy-q16-20260615-134643/summary.md`:

| Case | Direction | QUIC conns | TCP streams | Throughput |
|---|---|---:|---:|---:|
| proxy-upload-q16-P16 | upload | 16 | 16 | 23.471 Gbps |
| proxy-download-q16-P16 | download | 16 | 16 | 22.798 Gbps |

### Dual server q8/P8 + q8/P8

From `docs/dgx-dual-server-udp-socket-probe-20260615-144215/summary.md`:

| Case | Direction | Lane 1 | Lane 2 | Total | Single q16/P16 baseline | Delta |
|---|---|---:|---:|---:|---:|---:|
| dual-server q8/P8 + q8/P8 | upload | 18.330 Gbps | 18.246 Gbps | 36.575 Gbps | 23.471 Gbps | +13.104 Gbps |
| dual-server q8/P8 + q8/P8 | download | 11.588 Gbps | 11.260 Gbps | 22.848 Gbps | 22.798 Gbps | +0.050 Gbps |

Interpretation:

- Upload improves strongly when splitting into two server processes and two UDP
  sockets. This supports the hypothesis that upload was partly constrained by
  single server process / single UDP socket / single MsQuic datapath.
- Download does not improve. This means download is not primarily limited by
  remote server UDP socket fan-in. Its bottleneck remains on the local client
  side after QUIC receive.

## tcpquic-proxy metrics: upload vs download TCP write side

In upload, TCP write happens on the remote `tcpquic-proxy server`:

| Case | Lane | Side | TCP write | sendmsg calls | avg write | EAGAIN | max pending QUIC receive |
|---|---|---|---:|---:|---:|---:|---:|
| upload q8/P8 + q8/P8 | lane1 | server | 63.95 GiB | 611254 | 109.7 KiB | 0 | 1.3 MiB |
| upload q8/P8 + q8/P8 | lane2 | server | 63.58 GiB | 613890 | 108.6 KiB | 0 | 1.1 MiB |

In download, TCP write happens on the local `tcpquic-proxy client`:

| Case | Lane | Side | TCP write | sendmsg calls | avg write | EAGAIN | max pending QUIC receive |
|---|---|---|---:|---:|---:|---:|---:|
| download q8/P8 + q8/P8 | lane1 | client | 36.72 GiB | 274438 | 140.3 KiB | 22580 | 553.5 MiB |
| download q8/P8 + q8/P8 | lane2 | client | 35.65 GiB | 266490 | 140.3 KiB | 23677 | 532.2 MiB |

This is the key contrast:

- Upload TCP write has no `EAGAIN` and pending QUIC receive stays around 1 MiB.
- Download TCP write hits tens of thousands of `EAGAIN` events and pending QUIC
  receive grows to about 500 MiB.

Both sides use the same Linux relay write implementation:

- Non-compressed QUIC receive callback queues a pending receive view.
- Worker runs `FlushDeferredQuicReceives()`.
- It writes to TCP with `WritevNoSignal()` / `sendmsg`.
- On `EAGAIN`, it arms TCP writable and stops writing.
- On successful write, it advances receive completion and calls
  `StreamReceiveComplete`.

The code path is therefore essentially the same. The observed difference is the
runtime condition of the TCP write socket and the surrounding local process
load, not a separate server/client write implementation.

## Socket buffer tuning check

A possible explanation was that upload writes to a proxy-dialed TCP socket while
download writes to an accepted HTTP CONNECT socket, and perhaps only the dialed
socket had large buffers.

Code inspection rules this out:

- `src/tunnel/tcp_dialer.cpp` calls `TqTuneTcpForThroughput()` for dialed target
  sockets.
- `src/ingress/http_connect_server.cpp` calls `TqTuneTcpForThroughput(clientFd)`
  for HTTP CONNECT accepted sockets.
- `src/ingress/socks5_server.cpp` calls `TqTuneTcpForThroughput(clientFd)` for
  SOCKS5 accepted sockets.

Logs from the dual-server test confirm accepted sockets are tuned:

```text
tcpquic-proxy TCP socket tuning: requested=67108864 rcv=134217728 snd=134217728 ok_nodelay=1 ok_rcv=1 ok_snd=1
```

So the download EAGAIN issue is not explained by missing TCP socket buffer
tuning on accepted sockets.

## iperf3 through gost proxy

To isolate whether the new iperf3 proxy support is reading too slowly, iperf3
was tested through `gost` instead of `tcpquic-proxy`.

Setup:

- Remote: native `iperf3 -s`.
- Local: `gost` HTTP CONNECT or SOCKS5 proxy.
- Client: local `/home/jack/src/iperf/src/iperf3` with `--proxy`.
- No `tcpquic-proxy` in the data path.

Results:

| Proxy | Upload P16 | Download P16 |
|---|---:|---:|
| `gost` HTTP CONNECT | 106.205 Gbps | 98.216 Gbps |
| `gost` SOCKS5 | 106.276 Gbps | 98.146 Gbps |

These values are effectively the same as direct TCP P16. Therefore:

- The iperf3 `--proxy` stream receive loop can drain near 100 Gbps.
- The new SOCKS5 and HTTP CONNECT support does not appear to limit download
  throughput during the data phase.
- The problem is specific to the `tcpquic-proxy` path.

## iperf proxy code review notes

The proxy implementation in `/home/jack/src/iperf` changes connection setup:

- `iperf_tcp_connect()` connects to `settings->proxy_host:proxy_port` when
  `--proxy` is configured.
- It then calls `iperf_proxy_handshake()` to establish the tunnel.
- After the tunnel is established, the stream socket remains the same fd and
  the normal iperf TCP receive path is used.

The data receive path is unchanged:

- `iperf_tcp_recv()`
- `Nrecv_no_select()`
- `read()` / `recv()`

Minor robustness notes:

- `iperf_proxy.c` helper `read_full()` / `write_full()` does not retry on
  `EINTR`.
- HTTP CONNECT reads the response header one byte at a time.

These affect handshake robustness/latency, not steady-state throughput. They do
not explain the tcpquic-proxy download EAGAIN observed during the data phase.

## Current leading explanation

The strongest current explanation is:

1. In upload:
   - Local iperf3 writes into local `tcpquic-proxy client`.
   - Data crosses QUIC.
   - Remote `tcpquic-proxy server` writes TCP into remote iperf3 server.
   - Splitting server into two processes/two UDP sockets raises throughput from
     23.471 Gbps to 36.575 Gbps.
   - TCP write side has no EAGAIN.

2. In download:
   - Remote iperf3 server sends data to remote `tcpquic-proxy server`.
   - Data crosses QUIC.
   - Local `tcpquic-proxy client` writes TCP into local iperf3 client.
   - Splitting remote server into two processes does not improve throughput.
   - Local client TCP write side has tens of thousands of EAGAIN events and
     pending QUIC receive grows to about 500 MiB.

This points at local `tcpquic-proxy client` QUIC receive to TCP write as the
download bottleneck.

## Recommended next checks

1. Capture perf for download q8/P8 + q8/P8 on the local `tcpquic-proxy client`.
   - Expected hotspots: `tcp_sendmsg`, loopback/TCP stack, MsQuic decrypt,
     scheduler/locks, or relay worker write path.
2. During download, capture `ss -tinm` for the local proxy-to-iperf sockets.
   - Check `send-q`, `notsent`, `skmem`, `rcv_space`, and congestion state.
3. Replace iperf3 reverse receiver with a minimal local TCP sink.
   - If throughput improves, the local iperf3 reverse receiver interaction is
     still relevant even though iperf through `gost` is fine.
   - If throughput remains low, the bottleneck is inside tcpquic-proxy client
     relay/MsQuic path.
4. Add tcpquic-proxy metrics for TCP write byte histogram and iov histogram.
   - Current average write size is derived from totals.
   - Histograms would show whether download is dominated by many small writes,
     partial writes, or large writes that repeatedly hit EAGAIN.

## 2026-06-15 q16/P16 download diagnosis rerun

A focused 30s q16/P16 download rerun was captured under:

- `docs/dgx-download-slow-diagnose-20260615-175816/summary.md`
- client perf: `docs/dgx-download-slow-diagnose-20260615-175816/proxy-download-q16-P16/client.top.txt`
- loopback socket samples: `docs/dgx-download-slow-diagnose-20260615-175816/proxy-download-q16-P16/ss-loopback-samples.txt`

Result:

| Case | Direction | QUIC conns | TCP streams | Duration | Throughput |
|---|---|---:|---:|---:|---:|
| proxy-download-q16-P16 | download | 16 | 16 | 30s | 19.995 Gbps |

Metrics delta:

| Side | TCP read | TCP write | sendmsg calls | avg write | EAGAIN | partial | max pending QUIC receive | max pending queue | hard errors |
|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| client | 0.00 GiB | 61.82 GiB | 468802 | 138.3 KiB | 22652 | 60 | 500.9 MiB | 2488 | 1627 |
| server | 69.77 GiB | 0.00 GiB | 21 | 0.2 KiB | 0 | 0 | 0.0 MiB | 1 | 0 |

The rerun reproduces the same pattern as before:

- The remote server reads TCP and sends QUIC without TCP write pressure.
- The local client receives QUIC and writes local TCP; this is where EAGAIN and
  pending QUIC receive accumulation appear.
- The client still holds about 500 MiB of pending QUIC receive views at peak.

### Perf evidence

The local `tcpquic-proxy client` perf sample is dominated by the local TCP write
copy path:

| Overhead | Stack |
|---:|---|
| 32.97% | `__arch_copy_from_user -> skb_do_copy_data_nocache -> tcp_sendmsg_locked -> sendmsg -> TqLinuxRelayWorker::FlushDeferredQuicReceives` |

This is the direct cost of copying MsQuic receive buffers from user space into
the local loopback TCP socket. Other visible kernel TCP work in the same sample
also hangs below the `sendmsg` path (`tcp_write_xmit`, ACK processing,
`tcp_check_space`, skb release).

This matters because the hot path is not the iperf receive loop and not remote
TCP send. It is the local proxy client doing QUIC receive to TCP write.

### Socket-state evidence

`ss -tinmp` was sampled once per second for the local proxy-to-iperf loopback
sockets.

Important observations:

- `tcpquic-proxy` sockets had effective `tb134217728`, so the proxy-side
  `SO_SNDBUF` tuning is active.
- iperf-side receive buffers autotuned as high as `rb134217728`.
- steady-state `Send-Q` was usually 0; the maximum sampled `Send-Q` was about
  818 KiB.
- several sockets showed large receive windows and high per-socket delivery
  rates, so the local receiver is not obviously capped at a tiny buffer.

This makes the high EAGAIN count look like transient nonblocking write
backpressure under bursty multi-stream load, not a simple missing socket buffer
tuning issue.

### New concern: unbounded QUIC receive view write size

The non-compressed QUIC->TCP path queues MsQuic receive views and then calls
`sendmsg` from `FlushDeferredQuicReceives()`. It limits the number of iov
entries with `Config.MaxIov`, but it does not limit the total bytes attempted
per TCP write.

In this rerun, client metrics recorded:

- `linux_relay_max_tcp_write_iov_used = 3`
- `linux_relay_max_tcp_write_sendmsg_bytes = 278810878` bytes, about 266 MiB

That means a single successful `sendmsg` can copy a very large amount of data.
Even if this reduces syscall count, it can monopolize one relay worker inside
`tcp_sendmsg` for too long, delaying other relay events and deferred
`StreamReceiveComplete` calls. Under q16/P16 download this can amplify into:

1. QUIC receive views accumulate.
2. Pending receive bytes rise toward the per-relay pending budget.
3. Nonblocking local TCP writes periodically hit EAGAIN.
4. Receive completion pacing is delayed behind long TCP copy calls.

This is consistent with the perf and metrics evidence. It is not yet proven to
be the only bottleneck, but it is a stronger and more specific hypothesis than
"iperf proxy reads too slowly".

### Current best next probes

1. Add metrics for QUIC receive view size and TCP write distribution:
   - max/avg receive view total length
   - receive view slice count histogram
   - sendmsg attempted bytes histogram
   - sendmsg returned bytes histogram
   - EAGAIN by relay direction and after pending bytes bucket
2. Add a configurable cap for QUIC->TCP bytes per `sendmsg` as an experiment,
   for example 1 MiB, 4 MiB, 16 MiB, and uncapped.
3. Re-run q16/P16 download and dual q8/P8 download with that cap to check:
   - throughput
   - EAGAIN count
   - max pending QUIC receive bytes
   - perf share in `__arch_copy_from_user`
4. If capping improves fairness but increases syscall overhead too much,
   evaluate a middle value or adaptive cap based on pending queue depth.

## 2026-06-15 TCP write cap implementation and A/B results

The Linux relay now has an experimental TCP write cap:

```text
--linux-relay-tcp-write-max-bytes <bytes>
```

Default is unset/0, which preserves the previous uncapped behavior. When set,
both Linux QUIC->TCP paths cap each individual `sendmsg` attempt:

- non-compressed pending QUIC receive views in `FlushDeferredQuicReceives()`
- worker-owned pending TCP writes in `FlushTcpWrites()` used by compressed
  receive output

The cap limits total bytes in the current `iovec` list, not the logical receive
view. Completion accounting still advances by actual `sendmsg` return bytes.

### Added metrics

New metrics were added to make this class of issue diagnosable from
`/metrics`:

- TCP write attempt total/max and size buckets:
  - `linux_relay_tcp_write_attempt_bytes`
  - `linux_relay_max_tcp_write_attempt_bytes`
  - `linux_relay_tcp_write_attempt_bytes_le_64k`
  - `linux_relay_tcp_write_attempt_bytes_le_256k`
  - `linux_relay_tcp_write_attempt_bytes_le_1m`
  - `linux_relay_tcp_write_attempt_bytes_le_4m`
  - `linux_relay_tcp_write_attempt_bytes_gt_4m`
- TCP write returned size buckets:
  - `linux_relay_tcp_write_returned_bytes_le_64k`
  - `linux_relay_tcp_write_returned_bytes_le_256k`
  - `linux_relay_tcp_write_returned_bytes_le_1m`
  - `linux_relay_tcp_write_returned_bytes_le_4m`
  - `linux_relay_tcp_write_returned_bytes_gt_4m`
- QUIC receive view size/slice metrics:
  - `linux_relay_quic_receive_view_count`
  - `linux_relay_quic_receive_view_bytes`
  - `linux_relay_max_quic_receive_view_bytes`
  - `linux_relay_max_quic_receive_view_slices`
  - `linux_relay_quic_receive_view_bytes_le_64k`
  - `linux_relay_quic_receive_view_bytes_le_256k`
  - `linux_relay_quic_receive_view_bytes_le_1m`
  - `linux_relay_quic_receive_view_bytes_le_4m`
  - `linux_relay_quic_receive_view_bytes_gt_4m`
  - `linux_relay_quic_receive_view_slices_1`
  - `linux_relay_quic_receive_view_slices_2_to_4`
  - `linux_relay_quic_receive_view_slices_5_to_16`
  - `linux_relay_quic_receive_view_slices_gt_16`

### No-perf cap sweep

All rows below are 2-DGX q16/P16 download, 30s, no perf sampling:

| cap | throughput | sendmsg | avg write | EAGAIN | max pending QUIC receive | max returned sendmsg | max receive view |
|---|---:|---:|---:|---:|---:|---:|---:|
| uncapped | 19.868 Gbps | 457495 | 138.3 KiB | 36357 | 565.8 MiB | 81.7 MiB | 95.8 MiB |
| 1 MiB | 20.580 Gbps | 561622 | 118.0 KiB | 42524 | 564.5 MiB | 1.0 MiB | 163.6 MiB |
| 4 MiB | 21.269 Gbps | 505145 | 135.5 KiB | 60561 | 517.4 MiB | 4.0 MiB | 80.9 MiB |
| 16 MiB | 21.246 Gbps | 504823 | 136.0 KiB | 52963 | 526.9 MiB | 16.0 MiB | 76.0 MiB |

Result directories:

- uncapped: `docs/dgx-download-slow-diagnose-20260615-183933/`
- 1 MiB: `docs/dgx-download-slow-diagnose-20260615-183711/`
- 4 MiB: `docs/dgx-download-slow-diagnose-20260615-184025/`
- 16 MiB: `docs/dgx-download-slow-diagnose-20260615-183806/`

Interpretation:

- The cap works mechanically: max returned `sendmsg` is bounded exactly at the
  configured cap.
- 4 MiB and 16 MiB improved this run from about 19.9 Gbps to about 21.25 Gbps.
- The improvement is modest, and pending QUIC receive still stays around
  500 MiB, so the cap is not a complete fix.
- Large QUIC receive views still exist even with the write cap; the cap only
  controls how much of a view is consumed per TCP write attempt.
- `__arch_copy_from_user` remains the expected main CPU cost because the local
  QUIC->TCP direction still copies user-space buffers into the TCP stack.

Current recommendation:

- Keep `--linux-relay-tcp-write-max-bytes` as an experimental tuning knob.
- Use 4 MiB or 16 MiB for further DGX download tests, but do not make it the
  default yet.
- Next root-cause work should focus on why local QUIC receive backlog remains
  around 500 MiB even after bounding per-call TCP write size: worker scheduling,
  receive completion pacing, MsQuic receive view burst size, and local TCP copy
  CPU remain the active suspects.

## 2026-06-15 TCP write flush burst budget experiment

After the per-`sendmsg` cap, a second experiment added a separate per-flush
budget:

```text
--linux-relay-tcp-write-burst-bytes <bytes>
```

Default is unset/0, which preserves previous behavior. When set, a Linux relay
worker stops the current TCP write flush after writing that many bytes for one
relay, keeps TCP writable armed, and lets the remaining pending data continue on
the next writable turn. This applies to both:

- non-compressed QUIC receive views in `FlushDeferredQuicReceives()`
- worker-owned pending TCP writes in `FlushTcpWrites()`

This knob is different from `--linux-relay-tcp-write-max-bytes`:

- `tcp-write-max-bytes` limits one `sendmsg` attempt.
- `tcp-write-burst-bytes` limits how much one flush loop can write before
  yielding.

The intent was to test whether one relay with a large pending QUIC receive
queue can monopolize a worker for too long.

### Added metric

The following metric records how often the burst budget actually stopped a
flush while pending TCP write data remained:

```text
linux_relay_tcp_write_burst_stops
```

### q16/P16 download result

All rows below are 2-DGX q16/P16 download, 30s, no perf sampling, with
`--linux-relay-tcp-write-max-bytes 4194304`:

| burst budget | throughput | sendmsg | avg write | EAGAIN | burst stops | max pending QUIC receive |
|---|---:|---:|---:|---:|---:|---:|
| unset | 21.269 Gbps | 505145 | 135.5 KiB | 60561 | n/a | 517.4 MiB |
| 16 MiB | 20.627 Gbps | 486419 | 136.8 KiB | 48059 | 1046 | 558.7 MiB |
| 64 MiB | 20.950 Gbps | 496816 | 135.3 KiB | 60728 | 0 | 514.9 MiB |

Result directories:

- unset burst: `docs/dgx-download-slow-diagnose-20260615-184025/`
- 16 MiB burst: `docs/dgx-download-slow-diagnose-20260615-185640/`
- 64 MiB burst: `docs/dgx-download-slow-diagnose-20260615-185847/`

Interpretation:

- The 16 MiB budget does trigger (`burst_stops=1046`), but does not improve
  throughput or backlog. It lowers EAGAIN count in this run, yet max pending
  QUIC receive rises to 558.7 MiB.
- The 64 MiB budget is effectively non-intervening in this workload
  (`burst_stops=0`) and performs close to the 4 MiB per-`sendmsg` cap result.
- This weakens the hypothesis that single-relay flush monopolization is the
  main bottleneck for q16/P16 download.

Current recommendation:

- Keep `--linux-relay-tcp-write-burst-bytes` as a diagnostic knob, default off.
- Do not use the 16 MiB burst budget as a performance default.
- Continue investigating local `QUIC receive -> TCP sendmsg` cost/backpressure:
  perf still points at user-to-kernel TCP copy, and metrics still show about
  500 MiB of pending QUIC receive even when write call size and flush burst size
  are bounded.

## 2026-06-15 minimal TCP sink receiver probe

To rule out iperf3 receiver behavior, a small POSIX diagnostic tool was added:

```text
build/bin/Release/tcpquic_tcp_sink_bench
```

It has two modes:

- `server --listen <host:port>`: accepts N TCP streams and continuously writes
  fixed bytes.
- `client --connect <host:port> [--proxy http://host:port]`: opens N streams,
  optionally establishes HTTP CONNECT, then only reads and discards bytes.

The tool avoids iperf protocol/reporting logic. The client receive loop is
intentionally minimal, so it tests whether the local receiver can drain TCP data
fast enough.

The DGX orchestration script is:

```text
scripts/dgx-tcp-sink-download-probe.py
```

### No-perf result

Result directory:

```text
docs/dgx-tcp-sink-download-probe-20260615-191453/
```

Both cases use 16 TCP streams for 30s. The proxy case uses
`--quic-connections 16`, compression off, and
`--linux-relay-tcp-write-max-bytes 4194304`.

| Case | Throughput | Bytes |
|---|---:|---:|
| direct minimal TCP sink | 98.162 Gbps | 342.83 GiB |
| tcpquic-proxy HTTP CONNECT -> minimal TCP sink | 19.867 Gbps | 69.38 GiB |

Proxy metrics:

| Side | TCP read | TCP write | sendmsg | avg write | EAGAIN | max pending QUIC receive | max queue |
|---|---:|---:|---:|---:|---:|---:|---:|
| client | 0.00 GiB | 69.39 GiB | 522750 | 139.2 KiB | 29707 | 520.9 MiB | 3839 |
| server | 85.45 GiB | 0.00 GiB | 0 | 0.0 KiB | 0 | 0.0 MiB | 1 |

Interpretation:

- The minimal TCP sink can drain direct TCP at about 98 Gbps, matching previous
  direct TCP and `gost` results.
- Replacing iperf3 with the minimal sink does not improve tcpquic-proxy
  download throughput; the proxy path remains about 20 Gbps.
- Therefore iperf3 reverse/download receiver behavior is not the primary cause
  of the tcpquic-proxy download ceiling.
- The same signature remains on the local tcpquic-proxy client: many TCP write
  EAGAIN events and about 500 MiB of pending QUIC receive data.

### Perf result

Result directory:

```text
docs/dgx-tcp-sink-download-probe-20260615-191927/
```

Throughput:

| Case | Throughput | Bytes |
|---|---:|---:|
| direct minimal TCP sink | 98.241 Gbps | 343.10 GiB |
| tcpquic-proxy HTTP CONNECT -> minimal TCP sink | 18.605 Gbps | 64.98 GiB |

Local tcpquic-proxy client perf top:

| Overhead | Stack |
|---:|---|
| 28.18% | `__arch_copy_from_user -> skb_do_copy_data_nocache -> tcp_sendmsg_locked -> sendmsg -> TqLinuxRelayWorker::FlushDeferredQuicReceives` |
| 1.49% | `__arch_copy_to_user -> udpv6_recvmsg -> recvmmsg -> CxPlatSocketReceiveCoalesced` |

The perf sample confirms that in the minimal sink case the dominant local client
cost is still copying QUIC receive buffers into the local TCP socket via
`sendmsg`. UDP receive copy is visible but much smaller in this profile.

The proxy client shows TCP write hard errors near the end of these tests. The
last errno is `EPIPE`; this is expected when the synthetic sink client closes
all streams at the exact duration while tcpquic-proxy still has queued receive
data. It is not the throughput bottleneck; the bottleneck is present throughout
the run as EAGAIN + pending receive growth.

Updated conclusion:

- The download ceiling is inside tcpquic-proxy's local client relay path, not
  iperf3.
- The strongest current bottleneck is the local
  `QUIC receive view -> sendmsg(loopback TCP)` copy/backpressure path.
- Next useful probes should focus on reducing or bypassing this local TCP write
  cost, and on per-worker/per-relay attribution to see whether the 500 MiB
  backlog is evenly distributed or concentrated on a few relay workers.

## 2026-06-15 relay/worker distribution metrics

To distinguish uniform pressure from a small number of hot relays, Linux relay
metrics now include current snapshot distribution fields:

```text
linux_relay_active_relays
linux_relay_max_worker_pending_bytes
linux_relay_max_worker_active_relays
linux_relay_max_relay_pending_quic_receive_bytes
linux_relay_max_relay_pending_quic_receive_queue
linux_relay_max_relay_tcp_write_eagain_count
```

Meaning:

- `linux_relay_active_relays`: current active relays across all workers.
- `linux_relay_max_worker_pending_bytes`: current max pending bytes on a single
  worker.
- `linux_relay_max_worker_active_relays`: current max active relay count on a
  single worker.
- `linux_relay_max_relay_pending_quic_receive_bytes`: current max pending QUIC
  receive bytes on a single relay.
- `linux_relay_max_relay_pending_quic_receive_queue`: current max pending QUIC
  receive view queue depth on a single relay.
- `linux_relay_max_relay_tcp_write_eagain_count`: max TCP write EAGAIN count
  accumulated by a single relay.

These are snapshot distribution metrics, not historical global peaks. They
complement the older historical fields:

```text
linux_relay_max_pending_quic_receive_bytes
linux_relay_max_pending_quic_receive_queue
```

### First DGX result with distribution metrics

Result directory:

```text
docs/dgx-tcp-sink-download-probe-20260615-193932/
```

Throughput:

| Case | Throughput |
|---|---:|
| direct minimal TCP sink | 98.217 Gbps |
| tcpquic-proxy HTTP CONNECT -> minimal TCP sink | 18.682 Gbps |

Client-side distribution metrics at the end of the proxy run:

| Metric | Value |
|---|---:|
| active relays | 16 |
| current total pending bytes | 489744792 bytes |
| max worker pending bytes | 489744792 bytes |
| max worker active relays | 2 |
| max relay pending QUIC receive bytes | 489744792 bytes |
| max relay pending QUIC receive queue | 3710 |
| max relay TCP write EAGAIN count | 4639 |
| total TCP write EAGAIN count | 27496 |
| historical max pending QUIC receive bytes | 555425064 bytes |
| historical max pending QUIC receive queue | 4072 |

Interpretation:

- At the end of this run, essentially all current client-side pending bytes are
  concentrated in one relay: total pending bytes and max single-relay pending
  bytes are both about 467 MiB.
- The runtime has 16 active relays, and the busiest worker owns only 2 active
  relays, so this snapshot is not simply "one worker owns all streams".
- The backlog is therefore more likely a per-flow / per-relay TCP write
  backpressure problem than a perfectly uniform worker-wide bottleneck.

Updated next hypothesis:

- Investigate why one local client relay can accumulate nearly the entire
  pending QUIC receive budget while the other relays remain relatively light.
- Add per-relay identity diagnostics for short targeted runs only: relay id,
  TCP fd local/peer socket tuple, assigned worker, bytes written, EAGAIN count,
  pending receive bytes, pending queue depth.
- Use that to correlate hot relay with one HTTP CONNECT stream / one local TCP
  socket and inspect its `ss -tinm` state during the run.

### Repeat run after tcp-sink exit fix

Result directory:

```text
docs/dgx-tcp-sink-download-probe-20260615-194417/
```

Throughput:

| Case | Exit | Throughput |
|---|---:|---:|
| direct minimal TCP sink | 0 | 98.108 Gbps |
| tcpquic-proxy HTTP CONNECT -> minimal TCP sink | 0 | 19.259 Gbps |

Client-side distribution metrics:

| Metric | Value |
|---|---:|
| active relays | 16 |
| current total pending bytes | 498019816 bytes |
| max worker pending bytes | 498019816 bytes |
| max worker active relays | 2 |
| max relay pending QUIC receive bytes | 498019816 bytes |
| max relay pending QUIC receive queue | 3532 |
| max relay TCP write EAGAIN count | 4757 |
| total TCP write EAGAIN count | 29010 |

This repeat confirms the same shape with a clean direct-test exit: nearly all
current client-side pending bytes are concentrated in a single relay.

### Hot relay identity result

Result directory:

```text
docs/dgx-tcp-sink-download-probe-20260615-195004/
```

Throughput:

| Case | Exit | Throughput |
|---|---:|---:|
| direct minimal TCP sink | 0 | 98.193 Gbps |
| tcpquic-proxy HTTP CONNECT -> minimal TCP sink | 0 | 19.301 Gbps |

Client-side hot relay fields from `client.metrics.after.json`:

| Metric | Value |
|---|---:|
| hot relay id | 1 |
| hot relay worker index | 0 |
| hot relay TCP fd | 152 |
| hot relay local socket | `127.0.0.1:38216` |
| hot relay peer socket | `127.0.0.1:39230` |
| hot relay pending QUIC receive bytes | 489704604 bytes |
| hot relay pending QUIC receive queue | 3596 |
| hot relay TCP write bytes | 222153767 bytes |
| hot relay TCP write EAGAIN count | 4139 |
| total client pending bytes | 489704604 bytes |
| total client TCP write EAGAIN count | 31101 |

This identifies the hot relay as one local HTTP CONNECT client socket on the
tcpquic-proxy client process: local `127.0.0.1:38216` to peer
`127.0.0.1:39230`. In this snapshot, the hot relay's pending bytes equal the
entire client pending byte total, so the backlog is fully concentrated on that
single local TCP socket.

Next focused probe:

- Sample `ss -tinm` for the hot socket tuple during the run instead of broad
  loopback samples.
- Correlate socket state with relay id/worker id: send-q, notsent, skmem,
  rcv_space, and whether the reader side is advertising a constrained window.
- If the same first relay consistently becomes hot, inspect connection/stream
  assignment and startup ordering. If the hot relay changes between runs, focus
  on per-flow local TCP backpressure rather than a deterministic relay bug.

## 2026-06-15 EPOLLOUT re-arm bug

Result directory before the fix:

```text
docs/dgx-tcp-sink-download-probe-20260615-201642/
```

The focused hot-relay probe exposed a relay state machine bug:

| Case | Throughput | Client pending | Hot pending | Hot EAGAIN | Hot EPOLLOUT | Hot write armed |
|---|---:|---:|---:|---:|---:|---|
| proxy HTTP CONNECT -> minimal TCP sink | 18.241 Gbps | 480.6 MiB | 480.6 MiB | 4081 | 1 | false |

The `ss -tinm` samples showed the same hot socket had already become writable:

```text
Send-Q 0
skmem write 0
notsent absent
```

So this was not a kernel socket that remained full. The relay still had about
480 MiB of pending QUIC receive views, but TCP writable interest had been
disabled. That explains why the pending queue stopped draining.

Root cause:

- The EPOLLOUT path ran:
  `FlushTcpWrites() -> FlushDeferredQuicReceives() -> FlushTcpWrites()`.
- `FlushDeferredQuicReceives()` correctly armed TCP writable after `EAGAIN`.
- The final `FlushTcpWrites()` saw its own `PendingTcpWrites` queue empty and
  unconditionally called `ArmTcpWritable(false)`.
- That ignored `PendingQuicReceives`, so a relay with QUIC receive backlog could
  lose EPOLLOUT interest permanently.

Fix:

- `FlushTcpWrites()` now disables TCP writable only when both queues are empty:
  `PendingTcpWrites.empty() && PendingQuicReceives.empty()`.
- A Linux relay worker regression test now simulates this writable flush order
  and asserts TCP write interest remains armed while QUIC receive pending bytes
  remain.

Verification after the fix:

```text
docs/dgx-tcp-sink-download-probe-20260615-202439/
```

| Case | Throughput | Client pending | Hot pending | Pause/resume |
|---|---:|---:|---:|---:|
| proxy HTTP CONNECT -> minimal TCP sink | 18.797 Gbps | 0.0 MiB | 0.0 MiB | 5 / 5 |

This confirms the stuck-pending bug is fixed: the client no longer ends with
hundreds of MiB of pending QUIC receive data and a disabled write event.

However, throughput improved only slightly, from 18.241 Gbps to 18.797 Gbps in
this probe. Therefore the re-arm bug was a correctness/stall bug, not the main
throughput ceiling. The remaining bottleneck is still the local client
`QUIC receive -> TCP write` path:

- direct minimal TCP sink remains about 98.2 Gbps;
- proxy minimal TCP sink remains about 18.8 Gbps;
- client TCP write still records about 29k `EAGAIN` events in 30s;
- perf previously showed the dominant cost in
  `__arch_copy_from_user -> tcp_sendmsg_locked -> sendmsg ->
  TqLinuxRelayWorker::FlushDeferredQuicReceives`.

The next investigation should focus on why local loopback TCP writes from the
proxy client are limited to about 19 Gbps even when pending drains eventually:

- compare `sendmsg` batch size and partial-write distribution before/after the
  re-arm fix;
- capture perf again after the fix to confirm the hotspot stayed in
  `tcp_sendmsg` copy;
- evaluate whether the local TCP handoff can be replaced or reduced for the
  built-in speed test path, because proxy download currently pays an extra
  user-space to kernel TCP copy after QUIC decrypt.

### Perf after the re-arm fix

Perf run:

```text
docs/dgx-tcp-sink-download-probe-20260615-202805/
```

Throughput:

| Case | Throughput |
|---|---:|
| direct minimal TCP sink | 98.233 Gbps |
| proxy HTTP CONNECT -> minimal TCP sink | 18.834 Gbps |

Client metrics:

| Metric | Value |
|---|---:|
| TCP write bytes | 70,629,502,708 |
| sendmsg calls | 494,529 |
| average returned write | 139.5 KiB |
| TCP write EAGAIN | 28,387 |
| TCP write partial | 114 |
| attempted write bytes | 156,160,539,911 |
| max attempted write | 4 MiB |
| max returned write | 4 MiB |
| final pending bytes | 0 |
| pause/resume | 4 / 4 |

Perf still points at the same dominant cost:

| Overhead | Stack |
|---:|---|
| 36.86% | `__arch_copy_from_user -> skb_do_copy_data_nocache -> tcp_sendmsg_locked -> sendmsg -> TqLinuxRelayWorker::FlushDeferredQuicReceives` |
| 5.26% | TCP receive/wakeup path below `sock_def_readable` and `tcp_data_ready`, reached from the same `tcp_sendmsg` path |
| 2.25% | `skb_page_frag_refill -> sk_page_frag_refill -> tcp_sendmsg_locked` |
| 2.19% | `_raw_spin_lock`, mostly under TCP transmit/softirq and a smaller relay buffer pool reserve path |

Interpretation:

- The re-arm fix removed the permanent pending backlog, but not the throughput
  ceiling.
- The dominant cost is still copying QUIC receive bytes from user memory into
  the local TCP socket send path.
- The relay often attempts up to the configured 4 MiB write cap, but only about
  70.6 GiB is actually accepted by TCP while 156.2 GiB is attempted over the
  run. The high `EAGAIN` count is expected under this local TCP backpressure.
- The remaining bottleneck is not MsQuic UDP receive, not iperf proxy read
  logic, and not the fixed EPOLLOUT re-arm bug. It is the local client TCP
  handoff after QUIC receive.

Practical next directions:

1. Tune or redesign the local TCP handoff:
   - test smaller `--linux-relay-tcp-write-max-bytes` values such as 256 KiB
     and 1 MiB to see whether reducing oversized attempts lowers wasted
     `sendmsg` work without hurting throughput;
   - test a write burst budget again after the re-arm fix, because the previous
     result was contaminated by the writable-interest bug;
   - inspect whether accepted loopback sockets should use different
     `TCP_NOTSENT_LOWAT`, pacing, or buffer settings than remote TCP sockets.
2. For built-in speed-test only, avoid the local TCP handoff entirely:
   - measure QUIC receive drain speed inside the proxy process without writing
     to loopback TCP;
   - this would separate QUIC datapath capacity from proxy API compatibility
     cost.
3. For real proxy download, the extra local TCP copy is structurally required by
   HTTP CONNECT/SOCKS compatibility. Optimizations should focus on making that
   copy path cheaper or better paced, not on MsQuic UDP socket tuning.

### TCP write cap sweep

After the re-arm fix, the same 2-DGX q16/P16 minimal TCP sink download probe was
run with different `--linux-relay-tcp-write-max-bytes` values.

| Cap | Proxy throughput | Direct baseline | TCP write | sendmsg calls | Avg returned write | EAGAIN | Partial | Attempted write | Final pending |
|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| 64 KiB | 19.099 Gbps | 98.118 Gbps | 66.70 GiB | 1,563,782 | 44.7 KiB | 27,552 | 16 | 68.37 GiB | 0.0 MiB |
| 128 KiB | 17.715 Gbps | 98.131 Gbps | 61.87 GiB | 936,907 | 69.2 KiB | 29,169 | 22 | 64.96 GiB | 0.0 MiB |
| 256 KiB | 19.650 Gbps | 98.130 Gbps | 68.63 GiB | 771,810 | 93.2 KiB | 29,487 | 16 | 75.22 GiB | 0.0 MiB |
| 512 KiB | 19.268 Gbps | 98.209 Gbps | 67.29 GiB | 634,831 | 111.1 KiB | 29,028 | 92 | 78.62 GiB | 0.0 MiB |
| 1 MiB | 19.149 Gbps | 98.103 Gbps | 66.88 GiB | 555,155 | 126.3 KiB | 30,574 | 72 | 90.66 GiB | 0.0 MiB |
| 4 MiB | 18.797 Gbps | 98.223 Gbps | 65.65 GiB | 495,306 | 139.0 KiB | 29,604 | 104 | 131.86 GiB | 0.0 MiB |

Result directories:

```text
64 KiB:  docs/dgx-tcp-sink-download-probe-20260615-203329/
128 KiB: docs/dgx-tcp-sink-download-probe-20260615-203449/
256 KiB: docs/dgx-tcp-sink-download-probe-20260615-203026/
512 KiB: docs/dgx-tcp-sink-download-probe-20260615-203702/
1 MiB:   docs/dgx-tcp-sink-download-probe-20260615-203146/
4 MiB:   docs/dgx-tcp-sink-download-probe-20260615-202439/
```

Interpretation:

- 256 KiB was the best single run at 19.650 Gbps, about 4.5% above the 4 MiB
  run and about 2.0% above the 512 KiB run.
- 4 MiB attempted far more bytes than TCP accepted: 131.86 GiB attempted for
  65.65 GiB written. 256 KiB reduced this gap to 75.22 GiB attempted for
  68.63 GiB written.
- 64 KiB keeps attempted bytes close to written bytes, but sendmsg calls more
  than double versus 256 KiB. That syscall rate likely offsets the pacing
  benefit.
- 128 KiB was unexpectedly lower in this single run. Treat it as a data point
  that needs repeat testing, not as a definitive valley.

Current conclusion:

- A smaller TCP write cap is likely better than the 4 MiB cap for download
  under local TCP backpressure.
- 256 KiB is the current best candidate, but do not change the default based on
  one sweep. Repeat 64 KiB / 128 KiB / 256 KiB / 512 KiB / 1 MiB for at least
  three rounds and compare median throughput plus perf samples.
- Even the best cap only reaches about 20 Gbps, so this is a pacing/cost
  improvement, not a solution to the 100 Gbps gap.

## 2026-06-15 dual-proxy repeat after EPOLLOUT re-arm fix

After the TCP writable re-arm fix, the 2-DGX dual-proxy case was repeated three
times with the same shape as the original UDP socket fan-out probe:

```text
2 lanes
per lane: --quic-connections 8, iperf3 -P 8
duration: 30s per direction
compression: off
linux relay worker slots: 1024
linux relay read chunk: 1 MiB
tcp write cap: uncapped
```

Result directories:

```text
docs/dgx-dual-proxy-iperf-probe-20260615-211911/
docs/dgx-dual-proxy-iperf-probe-20260615-212046/
docs/dgx-dual-proxy-iperf-probe-20260615-212221/
```

Throughput:

| Run | Upload lane1 | Upload lane2 | Upload total | Download lane1 | Download lane2 | Download total |
|---|---:|---:|---:|---:|---:|---:|
| 21:19:11 | 18.788 Gbps | 18.956 Gbps | 37.744 Gbps | 11.497 Gbps | 11.482 Gbps | 22.979 Gbps |
| 21:20:46 | 19.146 Gbps | 18.161 Gbps | 37.307 Gbps | 11.880 Gbps | 11.588 Gbps | 23.469 Gbps |
| 21:22:21 | 18.184 Gbps | 18.720 Gbps | 36.904 Gbps | 11.971 Gbps | 11.152 Gbps | 23.123 Gbps |

Summary:

| Direction | Range | Average |
|---|---:|---:|
| upload | 36.904-37.744 Gbps | 37.318 Gbps |
| download | 22.979-23.469 Gbps | 23.190 Gbps |

The new repeat is consistent with the earlier dual-server observation:

- Splitting into two `tcpquic-proxy server` processes and two UDP listening
  sockets raises upload to about 37 Gbps.
- The same split does not materially raise download, which stays around
  23 Gbps.
- Therefore the download ceiling is unlikely to be primarily caused by the
  remote server's single UDP socket.

Download client-side TCP write metrics still show strong local backpressure:

| Run | Lane | Client TCP write | sendmsg calls | Avg write | EAGAIN |
|---|---|---:|---:|---:|---:|
| 21:19:11 | lane1 | 36.44 GiB | 270746 | 141.1 KiB | 14028 |
| 21:19:11 | lane2 | 36.23 GiB | 258613 | 146.9 KiB | 20182 |
| 21:20:46 | lane1 | 37.54 GiB | 283537 | 138.8 KiB | 12801 |
| 21:20:46 | lane2 | 36.58 GiB | 275477 | 139.2 KiB | 18631 |
| 21:22:21 | lane1 | 37.92 GiB | 279800 | 142.1 KiB | 12767 |
| 21:22:21 | lane2 | 35.26 GiB | 269080 | 137.4 KiB | 14066 |

This reinforces the current leading explanation:

- Upload scales when server-side UDP/process pressure is split.
- Download remains limited by the local client
  `QUIC receive -> sendmsg(loopback TCP)` path.
- The re-arm fix removed a pending-queue correctness bug, but the steady-state
  throughput ceiling remains dominated by local TCP handoff cost/backpressure.

Next focused diagnostic:

- Add a diagnostic-only download receive sink that keeps the remote producer and
  QUIC receive path active but bypasses the local loopback TCP write on the
  client.
- If this sink exceeds the current 20-23 Gbps ceiling, the local TCP handoff is
  confirmed as the dominant cost.
- If it remains near 20-23 Gbps, the bottleneck is earlier in the QUIC receive
  path or worker scheduling, not the local TCP write.

## 2026-06-15 built-in download receive-sink probe

A diagnostic-only client mode was added:

```text
--download-sink-test <sec>
```

It uses the normal built-in speed-test control flow and the normal server-side
download producer. The client opens normal tunnel streams, but the Linux relay
registers a receive sink instead of a local TCP fd:

- MsQuic receive callback still returns `QUIC_STATUS_PENDING`.
- The callback only queues the receive view.
- The Linux relay worker accounts the bytes, calls `StreamReceiveComplete`, and
  discards the data.
- No local loopback TCP `sendmsg` is performed on the client side.

Result directory:

```text
docs/dgx-download-sink-probe-20260615-214934/
```

2-DGX q16, 30s, compression off, 1MiB read chunk, 1024 worker slots:

| Case | Client local bytes | Server bytes | Throughput |
|---|---:|---:|---:|
| normal `--download-test 30` | 60,064,602,547 | 72,433,886,459 | 16.017 Gbps |
| diagnostic `--download-sink-test 30` | 66,613,521,718 | 79,077,131,609 | 17.764 Gbps |

Interpretation:

- Bypassing local loopback TCP write improved this built-in q16 download run by
  about 10.9%.
- That confirms the local TCP handoff has measurable cost.
- However, the result did not jump to tens of Gbps beyond the normal path. So
  the client loopback TCP write is not the sole dominant bottleneck for built-in
  q16 download.
- The remaining ceiling is earlier or broader in the path: server producer,
  server TCP-read-to-QUIC-send relay, MsQuic send/receive processing, client
  receive queue/worker scheduling, or receive-complete pacing.

Updated next direction:

- Capture perf for `--download-sink-test` and normal `--download-test` under
  the same q16 parameters.
- If sink perf shifts away from `tcp_sendmsg` but stays around 18 Gbps, focus
  on MsQuic receive/send and relay worker scheduling rather than local TCP
  handoff.
- Add a scripted receive-sink DGX probe so client stdout, server stdout, and
  full admin metrics JSON are captured without manual `rtk` output truncation.

## 2026-06-15 dual-proxy q1/q2/q8 scaling check

To separate "download direction is slow" from "download does not scale with
inner parallelism", dual-proxy tests were repeated with two client/server pairs
and different per-pair concurrency. Upload and download were run in separate
fresh process sets for q1 and q2 so that one direction did not contaminate the
next iperf server run.

Common settings:

```text
2 lanes
duration: 30s
compression: off
linux relay read chunk: 1 MiB
linux relay worker slots: 1024
tcp write cap/burst: unset
```

Results:

| Per lane | Direction | Lane 1 | Lane 2 | Total | Result directory |
|---|---|---:|---:|---:|---|
| q1/P1 | upload | 15.802 Gbps | 15.727 Gbps | 31.528 Gbps | `docs/dgx-dual-proxy-iperf-probe-20260615-223200/` |
| q1/P1 | download | 14.700 Gbps | 16.727 Gbps | 31.427 Gbps | `docs/dgx-dual-proxy-iperf-probe-20260615-223057/` |
| q2/P2 | upload | 17.593 Gbps | 17.453 Gbps | 35.046 Gbps | `docs/dgx-dual-proxy-iperf-probe-20260615-223445/` |
| q2/P2 | download | 12.796 Gbps | 16.244 Gbps | 29.040 Gbps | `docs/dgx-dual-proxy-iperf-probe-20260615-223532/` |
| q8/P8 | upload | 18.2-19.1 Gbps | 18.2-19.0 Gbps | 36.9-37.7 Gbps | `docs/dgx-dual-proxy-iperf-probe-20260615-211911/` etc. |
| q8/P8 | download | 11.5-12.0 Gbps | 11.2-11.6 Gbps | 23.0-23.5 Gbps | `docs/dgx-dual-proxy-iperf-probe-20260615-211911/` etc. |

Interpretation:

- q1/P1 dual-proxy is balanced and symmetric: upload and download are both
  about 31.5 Gbps total, with each lane around 15-17 Gbps.
- q2/P2 upload improves to about 35 Gbps total, but q2/P2 download drops to
  about 29 Gbps total and becomes less balanced.
- q8/P8 upload reaches about 37 Gbps total, but q8/P8 download collapses to
  about 23 Gbps total, around 11-12 Gbps per lane.

This changes the problem statement:

- Download can scale across two client/server pairs at q1/P1.
- The gap appears when increasing inner per-pair parallelism from q1/P1 to
  q2/P2 and especially q8/P8.
- Therefore the current suspect is not "download direction is inherently slow";
  it is "download has poor scaling with multiple simultaneous streams per
  client/server pair".

The next useful probes should target q1/P1 vs q2/P2 vs q8/P8 download on the
client side:

- per-worker relay bytes/events;
- per-connection/per-stream distribution if available from MsQuic or local
  metrics;
- CPU affinity and RSS/IRQ sharing between the two local client processes;
- perf comparison for q1/P1 and q8/P8 download, especially MsQuic receive,
  decrypt, relay worker scheduling, and local TCP write paths.

## 2026-06-15 dual-proxy q2/P2 repeat

The 2-lane q2/P2 case was repeated with upload and download in separate fresh
process sets:

```text
LANES=2
Q_PER_LANE=2
P_PER_LANE=2
DURATION_SEC=30
DIRECTIONS=upload or download
```

Result directories:

- `docs/dgx-dual-proxy-iperf-probe-20260615-223848/` for upload
- `docs/dgx-dual-proxy-iperf-probe-20260615-223936/` for download

Throughput:

| Per lane | Direction | Lane 1 | Lane 2 | Total |
|---|---|---:|---:|---:|
| q2/P2 | upload | 17.524 Gbps | 17.842 Gbps | 35.366 Gbps |
| q2/P2 | download | 16.681 Gbps | 14.684 Gbps | 31.365 Gbps |

Selected relay metrics:

| Direction | Lane | Client tcp_write | Client sendmsg | Client EAGAIN | Server tcp_read/write | Server errors |
|---|---|---:|---:|---:|---:|---:|
| upload | lane1 | 0.00 GiB | 5 | 0 | 61.17 GiB write | 288 |
| upload | lane2 | 0.00 GiB | 5 | 0 | 62.31 GiB write | 0 |
| download | lane1 | 57.24 GiB | 442,116 | 4,346 | 58.27 GiB read | 0 |
| download | lane2 | 50.42 GiB | 404,447 | 6,669 | 51.29 GiB read | 0 |

Interpretation update:

- q2/P2 is not consistently collapsed. In this repeat, download reached
  31.365 Gbps, close to q1/P1's 31.427 Gbps, but still below upload's
  35.366 Gbps.
- The larger and more repeatable collapse remains q8/P8 download, which was
  around 23.0-23.5 Gbps in the previous three repeats.
- The working hypothesis should be narrowed again: the strongest signal is
  download degradation under higher inner parallelism, especially q8/P8, not a
  deterministic q2/P2 cliff.

## 2026-06-15 dual-proxy q4/P4 probe

The midpoint between q2/P2 and q8/P8 was tested with the same two-lane topology
and separate fresh process sets per direction:

```text
LANES=2
Q_PER_LANE=4
P_PER_LANE=4
DURATION_SEC=30
DIRECTIONS=upload or download
```

Result directories:

- `docs/dgx-dual-proxy-iperf-probe-20260615-224149/` for upload
- `docs/dgx-dual-proxy-iperf-probe-20260615-224237/` for download

Throughput:

| Per lane | Direction | Lane 1 | Lane 2 | Total |
|---|---|---:|---:|---:|
| q4/P4 | upload | 18.503 Gbps | 18.767 Gbps | 37.270 Gbps |
| q4/P4 | download | 15.943 Gbps | 15.892 Gbps | 31.835 Gbps |

Selected relay metrics:

| Direction | Lane | Client tcp_write | Client sendmsg | Client EAGAIN | Client pause/resume | Server tcp_read/write | Server errors |
|---|---|---:|---:|---:|---:|---:|---:|
| upload | lane1 | 0.00 GiB | 5 | 0 | 0/0 | 64.61 GiB write | 94 |
| upload | lane2 | 0.00 GiB | 5 | 0 | 0/0 | 65.49 GiB write | 502 |
| download | lane1 | 53.55 GiB | 434,372 | 15,190 | 3/3 | 55.69 GiB read | 0 |
| download | lane2 | 53.54 GiB | 411,927 | 14,613 | 1/1 | 55.51 GiB read | 0 |

Interpretation update:

- q4/P4 download is balanced across both lanes and reaches 31.835 Gbps, close
  to q1/P1 and the q2/P2 repeat.
- q4/P4 upload reaches 37.270 Gbps, essentially the same level as q8/P8 upload.
- The download gap is still visible against upload, but q4/P4 does not show the
  severe q8/P8 download collapse.
- Current best boundary: q1/P1, q2/P2, and q4/P4 download are around
  31-32 Gbps in the two-lane topology; q8/P8 download drops to around
  23-24 Gbps. The next diagnosis should compare q4/P4 vs q8/P8 client-side
  scheduling, per-stream fairness, receive queue pressure, and TCP write
  behavior.

## 2026-06-15 dual-proxy q6/P6 probe

The q6/P6 midpoint between q4/P4 and q8/P8 was tested with the same two-lane
topology and separate fresh process sets per direction:

```text
LANES=2
Q_PER_LANE=6
P_PER_LANE=6
DURATION_SEC=30
DIRECTIONS=upload or download
```

Result directories:

- `docs/dgx-dual-proxy-iperf-probe-20260615-224648/` for upload
- `docs/dgx-dual-proxy-iperf-probe-20260615-224740/` for download

Throughput:

| Per lane | Direction | Lane 1 | Lane 2 | Total |
|---|---|---:|---:|---:|
| q6/P6 | upload | 17.922 Gbps | 18.688 Gbps | 36.610 Gbps |
| q6/P6 | download | 17.942 Gbps | 12.606 Gbps | 30.547 Gbps |

Selected relay metrics:

| Direction | Lane | Client tcp_write | Client sendmsg | Client EAGAIN | Client pause/resume | Server tcp_read/write | Server errors |
|---|---|---:|---:|---:|---:|---:|---:|
| upload | lane1 | 0.00 GiB | 6 | 0 | 0/0 | 62.55 GiB write | 150 |
| upload | lane2 | 0.00 GiB | 6 | 0 | 0/0 | 65.00 GiB write | 1044 |
| download | lane1 | 59.64 GiB | 467,601 | 22,173 | 3/3 | 62.99 GiB read | 0 |
| download | lane2 | 40.93 GiB | 369,990 | 23,446 | 4/4 | 44.03 GiB read | 0 |

Interpretation update:

- q6/P6 upload remains near the q4/P4 and q8/P8 upload ceiling.
- q6/P6 download is still much better than the q8/P8 download repeats, but it
  becomes visibly imbalanced: lane1 reached 17.942 Gbps while lane2 only reached
  12.606 Gbps.
- Client-side TCP write EAGAIN counts are high on both download lanes, but the
  lower-throughput lane did not have a uniquely higher EAGAIN count. That points
  to broader scheduling/fairness or QUIC receive distribution effects, not just
  one local TCP fd being backpressured.
- The transition now looks gradual: q4/P4 is balanced around 31.8 Gbps total,
  q6/P6 remains around 30.5 Gbps but becomes imbalanced, and q8/P8 drops to
  23-24 Gbps total. The next probe should capture q6/P6 and q8/P8 perf plus
  per-connection/per-stream counters if available.

## 2026-06-15 dual-proxy q8/P8 fresh-process repeat

q8/P8 was repeated with the same method used for q2/q4/q6: upload and download
were run in separate fresh process sets instead of running both directions back
to back in the same proxy/iperf server set.

```text
LANES=2
Q_PER_LANE=8
P_PER_LANE=8
DURATION_SEC=30
DIRECTIONS=upload or download
```

Result directories:

- `docs/dgx-dual-proxy-iperf-probe-20260615-225008/` for upload
- `docs/dgx-dual-proxy-iperf-probe-20260615-225100/` for download

Throughput:

| Per lane | Direction | Lane 1 | Lane 2 | Total |
|---|---|---:|---:|---:|
| q8/P8 | upload | 18.621 Gbps | 19.131 Gbps | 37.751 Gbps |
| q8/P8 | download | 16.281 Gbps | 17.767 Gbps | 34.048 Gbps |

Selected relay metrics:

| Direction | Lane | Client tcp_write | Client sendmsg | Client EAGAIN | Client pause/resume | Server tcp_read/write | Server pending |
|---|---|---:|---:|---:|---:|---:|---:|
| upload | lane1 | 0.00 GiB | 5 | 0 | 0/0 | 65.04 GiB write | 0.0 MiB |
| upload | lane2 | 0.00 GiB | 5 | 0 | 0/0 | 66.80 GiB write | 0.0 MiB |
| download | lane1 | 52.80 GiB | 398,906 | 27,608 | 6/6 | 56.87 GiB read | 169.0 MiB |
| download | lane2 | 57.86 GiB | 447,683 | 33,655 | 7/7 | 62.06 GiB read | 113.0 MiB |

Interpretation update:

- This fresh-process q8/P8 download run did not reproduce the previous
  23-24 Gbps result. It reached 34.048 Gbps and both lanes were reasonably
  balanced.
- The earlier low q8/P8 runs were collected by running upload and download
  back to back in the same process set. Given the fresh-process repeat, those
  earlier results likely include test orchestration or process-reuse effects,
  not only steady-state relay behavior.
- The stronger current signal is:
  - upload scales from q1/P1 to q8/P8 and reaches about 37-38 Gbps;
  - fresh-process download also scales above 30 Gbps, but remains several Gbps
    below upload;
  - download shows high client-side TCP write EAGAIN and occasional pause/resume
    at q4/q6/q8, so client-side QUIC receive to local TCP write remains an
    important cost center.
- The next diagnosis should avoid mixed-direction process reuse unless that is
  explicitly the scenario being tested. If the old q8/P8 collapse is still
  important, it should be treated as a separate "upload then download reuse"
  bug and reproduced with targeted state/log capture.

## 2026-06-15 q8/P8 mixed-direction process-reuse reproduction

The original test shape was repeated explicitly: start one two-lane proxy/iperf
server process set, run upload first, then run download on the same process set
without restarting the proxy or iperf servers.

```text
LANES=2
Q_PER_LANE=8
P_PER_LANE=8
DURATION_SEC=30
DIRECTIONS=upload,download
```

Result directory:

- `docs/dgx-dual-proxy-iperf-probe-20260615-225918/`

Throughput:

| Per lane | Direction | Lane 1 | Lane 2 | Total |
|---|---|---:|---:|---:|
| q8/P8 | upload first | 19.126 Gbps | 19.248 Gbps | 38.375 Gbps |
| q8/P8 | download second, same processes | 10.586 Gbps | 11.148 Gbps | 21.734 Gbps |

Selected relay metrics:

| Direction | Lane | Client tcp_write | Client sendmsg | Client EAGAIN | Client pending | Client pause/resume | Server tcp_read/write |
|---|---|---:|---:|---:|---:|---:|---:|
| upload first | lane1 | 0.00 GiB | 6 | 0 | 80.0 MiB | 0/0 | 66.72 GiB write |
| upload first | lane2 | 0.00 GiB | 5 | 0 | 15.0 MiB | 0/0 | 67.23 GiB write |
| download second | lane1 | 33.25 GiB | 252,037 | 25,037 | 6.0 MiB | 1/1 | 36.98 GiB read |
| download second | lane2 | 35.29 GiB | 264,055 | 22,057 | 5.0 MiB | 1/1 | 39.04 GiB read |

Comparison with fresh-process q8/P8:

| Method | Upload total | Download total |
|---|---:|---:|
| Fresh upload-only/download-only process sets | 37.751 Gbps | 34.048 Gbps |
| Upload then download on same process set | 38.375 Gbps | 21.734 Gbps |

Interpretation update:

- The old q8/P8 collapse is reproducible when download runs second in the same
  process set after upload.
- The same q8/P8 download shape reaches 34.048 Gbps when run in a fresh process
  set. Therefore the 21-24 Gbps collapse is not a steady-state q8/P8 download
  ceiling.
- The problem should now be split:
  1. steady-state fresh-process download is still lower than upload by several
     Gbps;
  2. mixed-direction process reuse has a much larger regression when switching
     from upload to download without restart.
- For the process-reuse regression, the next evidence should be captured around
  the transition point after upload completes and before download starts:
  client/server metrics snapshot, per-tunnel lifecycle counters, active stream
  count, worker slot occupancy, pending receive/send counters, and iperf server
  state.

## 2026-06-15 q8/P8 reverse-order process-reuse check

The mixed-direction process-reuse test was repeated in the opposite direction:
start one two-lane proxy/iperf server process set, run download first, then run
upload on the same process set without restart.

The probe script was fixed so `DIRECTIONS` order is honored. Before that fix,
one accidental repeat still ran in the old upload-then-download order:

- `docs/dgx-dual-proxy-iperf-probe-20260615-230714/`
- upload first: 37.795 Gbps
- download second: 22.770 Gbps

That accidental repeat further confirms the upload-then-download regression.

The intended reverse-order result:

```text
LANES=2
Q_PER_LANE=8
P_PER_LANE=8
DURATION_SEC=30
DIRECTIONS=download,upload
```

Result directory:

- `docs/dgx-dual-proxy-iperf-probe-20260615-230915/`

Throughput:

| Per lane | Direction | Lane 1 | Lane 2 | Total |
|---|---|---:|---:|---:|
| q8/P8 | download first | 17.981 Gbps | 17.406 Gbps | 35.387 Gbps |
| q8/P8 | upload second, same processes | 18.361 Gbps | 17.829 Gbps | 36.189 Gbps |

Selected relay metrics:

| Direction | Lane | Client tcp_write | Client sendmsg | Client EAGAIN | Client pending | Client pause/resume | Server tcp_read/write |
|---|---|---:|---:|---:|---:|---:|---:|
| download first | lane1 | 58.78 GiB | 432,470 | 25,229 | 0.0 MiB | 0/0 | 62.82 GiB read |
| download first | lane2 | 56.72 GiB | 447,010 | 13,271 | 0.0 MiB | 0/0 | 60.79 GiB read |
| upload second | lane1 | 0.00 GiB | 6 | 0 | 126.0 MiB | 0/0 | 63.58 GiB write |
| upload second | lane2 | 0.00 GiB | 6 | 0 | 45.0 MiB | 0/0 | 61.90 GiB write |

Comparison:

| Method | First direction | Second direction |
|---|---:|---:|
| Fresh process sets | upload 37.751 Gbps | download 34.048 Gbps |
| Same process set, upload then download | upload 38.375 Gbps | download 21.734 Gbps |
| Same process set, download then upload | download 35.387 Gbps | upload 36.189 Gbps |

Interpretation update:

- Direction switching is asymmetric. Download first is healthy, and upload
  after download remains healthy.
- Upload first is healthy, but download after upload collapses to about
  21-23 Gbps.
- This points away from a generic "second run is slow" effect and toward
  upload-path state affecting the later download path.
- The next focused reproduction should instrument the boundary between upload
  completion and the following download start, especially lingering tunnels,
  stream/connection state, worker pending state, iperf server accepted
  connections, and whether old upload-side relay registrations remain active or
  consume scheduling/slot capacity.

## 2026-06-15 process-reuse diagnostics metrics

Additional relay state metrics were added to make the direction-switch boundary
observable from `/metrics`:

- active relay shape:
  - `linux_relay_active_tcp_relays`
  - `linux_relay_active_sink_relays`
  - `linux_relay_active_quic_send_relays`
- current queue/slot pressure:
  - `linux_relay_current_pending_quic_receive_bytes`
  - `linux_relay_current_pending_quic_receive_queue`
  - `linux_relay_worker_slots_allocated`
  - `linux_relay_worker_slots_free`
  - `linux_relay_pending_tcp_write_queue`
  - `linux_relay_pending_tcp_write_bytes`
- current fd/lifecycle state:
  - `linux_relay_tcp_read_armed_relays`
  - `linux_relay_tcp_read_disabled_relays`
  - `linux_relay_tcp_write_armed_relays`
  - `linux_relay_closing_relays`
  - `linux_relay_tcp_read_closed_relays`
  - `linux_relay_tcp_write_shutdown_queued_relays`
  - `linux_relay_outstanding_quic_sends`

The dual-proxy probe summary now includes a "State Snapshots" table with before
and after snapshots for each direction. That makes the second direction's
`before` rows the exact process-reuse transition state.

Validation commands:

```text
cmake --build build --config Release --target tcpquic-proxy tcpquic_router_runtime_test tcpquic_linux_relay_worker_io_test -j$(nproc)
build/bin/Release/tcpquic_router_runtime_test
build/bin/Release/tcpquic_linux_relay_worker_io_test
python3 -m py_compile scripts/dgx-dual-proxy-iperf-probe.py
git diff --check
```

`tcpquic_linux_relay_worker_io_test` had one transient timing-sensitive failure
on first run, then passed when rerun.

### q8/P8 upload then download with diagnostics

Result directory:

- `docs/dgx-dual-proxy-iperf-probe-20260615-232701/`

Throughput:

| Direction | Lane 1 | Lane 2 | Total |
|---|---:|---:|---:|
| upload first | 18.256 Gbps | 18.313 Gbps | 36.569 Gbps |
| download second | 12.023 Gbps | 11.426 Gbps | 23.448 Gbps |

Key transition state: download `before`, i.e. immediately after upload completed
and before download started.

| Side | Lane | active relays | pending MiB | slots allocated/free | read armed/disabled | read closed | closing | outstanding sends |
|---|---:|---:|---:|---:|---:|---:|---:|---:|
| client | 1 | 8 | 81.0 | 8192 / 8119 | 6 / 2 | 8 | 0 | 0 |
| client | 2 | 8 | 48.0 | 8192 / 8146 | 3 / 5 | 8 | 0 | 0 |
| server | 1 | 2 | 0.0 | 2048 / 2048 | 0 / 2 | 2 | 0 | 0 |
| server | 2 | 3 | 0.0 | 3072 / 3072 | 0 / 3 | 3 | 0 | 0 |

Interpretation:

- The upload-to-download collapse is reproduced with the new metrics:
  download second was 23.448 Gbps.
- The transition state is not clean. After upload, the client still has eight
  active relays per lane, and the server still has two or three active relays
  per lane.
- Those relays have `tcp_read_closed` set, but `closing` is still zero.
  This is the strongest current signal: the TCP read side has observed closure,
  but the relay has not entered/finished the cleanup path.
- The lingering client relays still reserve worker slots and pending bytes
  (about 48-81 MiB in this run). Even though pending TCP write queue and
  pending QUIC receive queue are zero, the relay registrations remain live and
  continue to count as active scheduling state.
- During the degraded download, client-side TCP write EAGAIN is again high
  (27,497 and 24,355), but the newly added transition metrics show the
  regression begins before download data transfer: the process starts download
  with stale upload relays still active.

Updated working hypothesis:

- Upload completion leaves relay registrations alive after `TcpReadClosed`.
- They are not marked `Closing`, not unregistered, and their buffer pools remain
  allocated.
- Later download creates additional server-side relay registrations on top of
  the stale state. This explains why fresh-process download is healthy while
  upload-then-download process reuse collapses.

Next code-level check:

- Trace the TCP EOF / FIN path in `DrainTcpReadable` and related QUIC FIN send
  completion handling.
- Verify that after `TcpReadClosed` and outstanding QUIC sends reach zero, the
  relay calls the same unregister/retire path used by explicit tunnel close.
- Add a unit test for "TCP EOF after upload drains relay registration" before
  changing cleanup behavior.

## 2026-06-15 root cause and fix

Root cause:

- `DrainTcpReadable()` correctly detected TCP EOF and set `TcpReadClosed`.
- It then called `FinishTcpToQuic()` to send QUIC FIN.
- For the common FIN-only case, `SubmitTcpBatchToQuic()` called
  `Stream->Send(nullptr, 0, QUIC_SEND_FLAG_FIN, nullptr)`.
- Because the send had no relay send context, there was no
  `SEND_COMPLETE.ClientContext` for `CompleteQuicSend()` to use.
- The relay never recorded that the TCP-to-QUIC FIN had completed.
- Separately, after QUIC receive FIN was drained to TCP and `shutdown(SHUT_WR)`
  was called, the relay did not retain a `TcpWriteClosed` state.
- As a result, the worker could observe `TcpReadClosed=1` and no pending data,
  but it had no state-machine condition that meant "both halves are done".
- `RelayHandle.Stop` stayed false, so `TqTunnelReaper` never called
  `TqRelayStop()`. The stale relay registrations remained active until process
  restart.

This exactly matched the DGX transition metrics before the fix:

| Side | Lane | active relays | pending MiB | read armed/disabled | read closed | closing |
|---|---:|---:|---:|---:|---:|---:|
| client | 1 | 8 | 81.0 | 6 / 2 | 8 | 0 |
| client | 2 | 8 | 48.0 | 3 / 5 | 8 | 0 |
| server | 1 | 2 | 0.0 | 0 / 2 | 2 | 0 |
| server | 2 | 3 | 0.0 | 0 / 3 | 3 | 0 |

Fix:

- Added relay lifecycle state:
  - `QuicSendFinSubmitted`
  - `QuicSendFinCompleted`
  - `TcpWriteClosed`
- FIN-only QUIC sends are marked completed immediately after successful
  `Stream->Send(... FIN ...)`, because there is no payload buffer context to
  wait for.
- FIN sends with data mark `QuicSendFinCompleted` in
  `CompleteQuicSend()` when the send-complete event arrives.
- QUIC receive FIN now records `TcpWriteClosed` after the pending TCP writes are
  drained and `shutdown(SHUT_WR)` is issued.
- `MaybeStopFullyClosedRelay()` sets `RelayHandle.Stop=true` only when:
  - TCP read side is closed;
  - TCP write side is closed;
  - QUIC send FIN has been submitted and completed;
  - no outstanding QUIC sends remain;
  - no pending TCP writes, pending QUIC receives, or pending receive bytes
    remain.

Regression test:

- Added a Linux relay worker unit test that simulates:
  1. local TCP half-close;
  2. TCP-to-QUIC FIN;
  3. remote QUIC FIN-only receive;
  4. TCP write half-close;
  5. expected `RelayHandle.Stop=true`.
- Before the fix it failed with:

```text
expected relay stop after both TCP read and QUIC receive FIN,
active=1 read_closed=1 write_shutdown=0 closing=0 sends=0 pending=0
```

After the fix it passes.

### DGX validation after fix

Result directory:

- `docs/dgx-dual-proxy-iperf-probe-20260615-233737/`

Same q8/P8 mixed-direction process-reuse test:

```text
LANES=2
Q_PER_LANE=8
P_PER_LANE=8
DURATION_SEC=30
DIRECTIONS=upload,download
```

Throughput:

| Direction | Lane 1 | Lane 2 | Total |
|---|---:|---:|---:|
| upload first | 18.287 Gbps | 18.126 Gbps | 36.413 Gbps |
| download second | 17.433 Gbps | 17.330 Gbps | 34.763 Gbps |

Key transition state: download `before`, after upload completed and before
download started.

| Side | Lane | active relays | pending MiB | read closed | closing |
|---|---:|---:|---:|---:|---:|
| client | 1 | 0 | 0.0 | 0 | 0 |
| client | 2 | 0 | 0.0 | 0 | 0 |
| server | 1 | 0 | 0.0 | 0 | 0 |
| server | 2 | 0 | 0.0 | 0 | 0 |

The original symptom is resolved in this run:

- Before fix: upload then download q8/P8 was about 21-23 Gbps.
- After fix: upload then download q8/P8 reached 34.763 Gbps.
- The second run starts from a clean relay state instead of inheriting stale
  upload relays.
