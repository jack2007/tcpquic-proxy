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
