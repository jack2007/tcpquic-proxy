# DGX iperf3 proxy bottleneck analysis

Date: 2026-06-15

## Question

Direct `iperf3` TCP can reach about 100 Gbps on the DGX link, while `tcpquic-proxy`
1x1 is around 15-17 Gbps. This note investigates why increasing TCP/QUIC
connection count improves throughput and where the current bottleneck is.

## Test Setup

- Local bind: `169.254.250.230`
- Remote target: `169.254.59.196`
- Tool: updated `iperf3` with `--proxy http://host:port`
- Proxy tuning:
  - `--tuning custom`
  - `--quic-fcw 1073741824`
  - `--quic-srw 1073741824`
  - `--quic-iw 4000`
  - `--quic-initrtt-ms 1`
  - `--relay-io-size 1048576`
  - `--compress off`
  - `--linux-relay-read-chunk-size 1048576`
  - `--linux-relay-worker-slots 1024`

Raw data:

- `docs/dgx-iperf-proxy-bottleneck-20260615-133917/`
- `docs/dgx-iperf-proxy-q16-20260615-134643/`
- `docs/dgx-iperf-proxy-perf-20260615-134906/`

## Throughput Results

### Direct TCP iperf3

| Case | Direction | TCP streams | Throughput |
|---|---|---:|---:|
| direct-upload-P1 | upload | 1 | 45.064 Gbps |
| direct-upload-P4 | upload | 4 | 106.354 Gbps |
| direct-upload-P16 | upload | 16 | 106.159 Gbps |
| direct-download-P1 | download | 1 | 59.166 Gbps |
| direct-download-P4 | download | 4 | 98.104 Gbps |
| direct-download-P16 | download | 16 | 98.285 Gbps |

Direct TCP proves the link, NIC, kernel TCP stack, routing, and updated iperf3
can drive about 100 Gbps with a few parallel TCP streams. Even direct single TCP
stream is 45-59 Gbps, far above proxy 1x1.

### tcpquic-proxy via iperf3 `--proxy`

| Case | Direction | QUIC conns | TCP streams | Throughput |
|---|---|---:|---:|---:|
| proxy-upload-q1-P1 | upload | 1 | 1 | 17.118 Gbps |
| proxy-download-q1-P1 | download | 1 | 1 | 16.793 Gbps |
| proxy-upload-q4-P4 | upload | 4 | 4 | 19.271 Gbps |
| proxy-upload-q4-P16 | upload | 4 | 16 | 20.632 Gbps |
| proxy-download-q4-P4 | download | 4 | 4 | 15.143 Gbps |
| proxy-upload-q16-P16 | upload | 16 | 16 | 23.471 Gbps |
| proxy-download-q16-P16 | download | 16 | 16 | 22.798 Gbps |

One q4/P16 reverse/download run timed out in the first matrix. A follow-up
q16/P16 reverse/download run completed successfully, so this is not a basic
functionality blocker, but it is a signal that high-concurrency reverse tests
stress close/drain behavior.

## Metrics Evidence

For upload, the local proxy client reads TCP from iperf3 and sends QUIC; the
remote proxy server receives QUIC and writes TCP to iperf3 server.

| Case | Side | TCP read | TCP write | sendmsg calls | avg write | read disabled | errors |
|---|---|---:|---:|---:|---:|---:|---:|
| proxy-upload-q1-P1 | client | 42.80 GB | 0 | 6 | - | 19637 | 0 |
| proxy-upload-q1-P1 | server | 0 | 42.80 GB | 585639 | 73 KiB | 2 | 0 |
| proxy-upload-q16-P16 | client | 41.00 GiB | 0 | 5 | - | 4501 | 0 |
| proxy-upload-q16-P16 | server | 0 | 40.96 GiB | 366612 | 117 KiB | 17 | 231 |

For download, the write side moves to the local proxy client:

| Case | Side | TCP read | TCP write | sendmsg calls | avg write | EAGAIN | max pending QUIC receive |
|---|---|---:|---:|---:|---:|---:|---:|
| proxy-download-q1-P1 | client | 0 | 41.42 GB | 381145 | 109 KiB | 4201 | 513 MiB |
| proxy-download-q1-P1 | server | 41.99 GB | 0 | 6 | - | 0 | 0 |
| proxy-download-q16-P16 | client | 0 | 31.10 GiB | 229007 | 142 KiB | 47386 | 566 MiB |
| proxy-download-q16-P16 | server | 39.93 GiB | 0 | 22 | - | 0 | 0 |

Observations:

1. The active TCP write side performs hundreds of thousands of `sendmsg` calls
   in 15-20 seconds. Average write size is only about 73-142 KiB, despite 1 MiB
   relay read chunks.
2. Download builds a large pending QUIC receive backlog on the TCP write side
   near the configured relay pending budget, and TCP write EAGAIN is frequent.
3. Upload q16 improves throughput while reducing TCP read disabled count on the
   sender. This suggests parallel tunnels smooth send-completion backpressure and
   keep more work in flight.
4. Error counters on the TCP write side appear mostly during high-speed test
   shutdown/close. They should be investigated separately, but they do not
   explain the steady-state 17-23 Gbps ceiling by themselves.

## Perf Evidence

Perf was captured for upload q1/P1 and q16/P16. Throughput under perf was lower
for q1 because sampling adds overhead, so use these results for hotspot shape,
not absolute throughput.

### q1/P1 upload

Client side, TCP read to QUIC send:

| Symbol | Overhead | Interpretation |
|---|---:|---|
| `__arch_copy_to_user` | 54.59% | Kernel TCP receive copy into proxy user buffers via `readv` |
| `check_heap_object` | 3.52% | Kernel copy safety/check overhead in same TCP read path |
| `aes_gcm_enc_256_kernel` | 2.23% | QUIC/TLS encryption |

Server side, QUIC receive to TCP write:

| Symbol | Overhead | Interpretation |
|---|---:|---|
| `__arch_copy_from_user` | 22.85% | Kernel TCP send copy from proxy buffers via `sendmsg` |
| `_raw_spin_unlock_irqrestore` | 5.81% | TCP/network stack contention/softirq |
| `__arch_copy_to_user` | 3.87% | UDP/MsQuic receive copy |

### q16/P16 upload

Client side:

| Symbol | Overhead | Interpretation |
|---|---:|---|
| `__arch_copy_to_user` | 31.77% | TCP read copy remains largest sender-side cost |
| `_raw_spin_lock` | 9.17% | More concurrent relays introduce more kernel/mm/network contention |
| `_raw_spin_unlock_irqrestore` | 4.26% | Network stack contention |

Server side:

| Symbol | Overhead | Interpretation |
|---|---:|---|
| `aes_gcm_dec_256_kernel` | 29.05% | QUIC/TLS decrypt becomes the top receive-side cost |
| `__aarch64_ldadd8_acq_rel` in MsQuic refcount path | 5.97% | MsQuic connection/receive frame processing overhead |
| `__memcpy_sve` in `QuicRecvBufferCopyIntoChunks` | 4.05% | MsQuic stream receive buffering copy |
| `__arch_copy_to_user` | 3.80% | UDP receive copy from kernel to MsQuic |
| `__arch_copy_from_user` | 3.51% | TCP write copy to iperf server |

## Root Cause

Increasing connection count helps because it adds parallel independent relay
pipelines:

1. More TCP sockets/tunnels give the Linux relay worker more independent readable
   and writable fds, so one stream's backpressure does not idle the whole proxy.
2. More QUIC connections split congestion control, stream scheduling, send
   completion, receive processing, and crypto work across more MsQuic connection
   state and worker activity.
3. More in-flight relay buffers keep both sides busy despite asynchronous QUIC
   send completion and TCP write EAGAIN.

However, the current bottleneck is not the network link and not iperf3. It is
the user-space proxy path:

1. TCP ingress into the proxy copies data from kernel to user (`readv`). In q1
   upload this dominates client CPU samples at 54.59%.
2. QUIC/TLS processing and MsQuic receive buffering become dominant after
   parallelism increases. In q16 upload the server spends 29.05% in AES-GCM
   decrypt and 4.05% in MsQuic stream receive copy.
3. TCP egress from proxy to iperf performs many `sendmsg` calls and kernel
   copies from user to kernel. The write side stays at about 70-140 KiB per
   `sendmsg`, so syscall/copy/event overhead remains high.
4. Download is additionally sensitive to TCP write backpressure: pending QUIC
   receive bytes reach about 500-560 MiB and EAGAIN counts are high on the local
   client write side.

This explains the observed shape:

- Direct TCP `-P4/-P16`: about 100 Gbps because the kernel TCP stack and iperf3
  can parallelize efficiently without QUIC/user-space relay overhead.
- Proxy 1x1: about 17 Gbps because one relay path is dominated by kernel copy,
  QUIC send/receive callbacks, crypto, and TCP write events.
- Proxy q16/P16: about 23 Gbps because parallelism hides some single-pipeline
  stalls, but total throughput remains CPU/copy/crypto/event limited.

## Implications

The next meaningful optimizations should reduce per-byte and per-event cost
rather than only increasing slots:

1. Reduce TCP read copy overhead.
   - Investigate Linux zero-copy or splice-style paths for TCP to QUIC if
     compatible with MsQuic send buffer lifetime requirements.
   - If zero-copy is not viable, improve buffer reuse and avoid page fault/cache
     churn in the read buffer pool.
2. Increase effective TCP write aggregation.
   - The code already uses vectored `sendmsg`, but observed average write size is
     far below 1 MiB.
   - Investigate why QUIC receive views are fragmented into 70-140 KiB writes:
     MsQuic receive buffer boundaries, stream framing, receive callback batch
     size, and worker flush budget.
3. Reduce QUIC/MsQuic receive-side CPU.
   - AES-GCM decrypt is a real q16 bottleneck. Check whether crypto acceleration
     is optimal on this ARM DGX kernel/OpenSSL path.
   - Revisit MsQuic UDP GRO/GSO, receive batch size, RSS/RPS, and CPU affinity.
4. Improve download backpressure handling.
   - Large pending QUIC receive backlog plus frequent TCP EAGAIN means the
     receive side can outrun TCP write drain.
   - Continue using pending receive, but consider more adaptive QUIC receive
     completion pacing when TCP write backlog grows.
5. Investigate high-concurrency shutdown errors separately.
   - The steady-state bottleneck is CPU/copy/crypto, but TCP write hard errors
     during iperf shutdown should be classified by errno and close state so they
     do not hide real relay errors.

## Next Tests

1. Run iperf proxy matrix with `q=1/2/4/8/16/32`, `P=q`, upload and download,
   30 seconds each, to find the true plateau.
2. Add CPU affinity experiments:
   - pin tcpquic-proxy client/server to isolated CPU sets;
   - compare MsQuic worker distribution and RSS queue mapping.
3. Capture perf for download q16/P16, because download shows larger pending
   receive backlog and EAGAIN pressure than upload.
4. Add a metric for QUIC receive callback buffer count/bytes per event and TCP
   write iov count distribution. Current max iov is not enough; we need
   histogram-style visibility to explain average `sendmsg` size.
