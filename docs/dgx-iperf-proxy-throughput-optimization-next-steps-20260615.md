# DGX tcpquic-proxy throughput optimization next steps

Date: 2026-06-15

## Background

Recent DGX tests with updated `iperf3 --proxy` show:

- Direct TCP `iperf3 -P4/-P16`: about 98-106 Gbps.
- `tcpquic-proxy` q1/P1: about 17 Gbps.
- `tcpquic-proxy` q16/P16: about 23 Gbps.

The proxy improves with more connections, but it does not approach direct TCP
bandwidth. Perf and metrics indicate the current ceiling is mainly in the
user-space proxy path: TCP kernel/user copies, QUIC/TLS crypto, MsQuic receive
buffering, and TCP write event/syscall overhead.

Detailed evidence is in:

- `docs/dgx-iperf-proxy-bottleneck-analysis-20260615.md`
- `docs/dgx-iperf-proxy-bottleneck-20260615-133917/`
- `docs/dgx-iperf-proxy-q16-20260615-134643/`
- `docs/dgx-iperf-proxy-perf-20260615-134906/`

## Priority 1: Reduce TCP ingress copy cost

Evidence:

- q1/P1 upload client perf: `__arch_copy_to_user` is 54.59%.
- q16/P16 upload client perf: `__arch_copy_to_user` is still 31.77%.
- This path is TCP `readv` copying kernel TCP data into proxy user-space relay
  buffers.

Optimization directions:

1. Audit relay buffer pool allocation and reuse.
   - Ensure hot-path buffers are preallocated and reused.
   - Avoid page fault and first-touch cost during the throughput run.
   - Consider pre-touching worker buffers before accepting traffic.
2. Check CPU and memory locality.
   - Pin relay workers and MsQuic workers to predictable CPU sets.
   - Keep NIC IRQ/RSS queues away from the same hottest relay cores where
     possible.
   - Check NUMA-like locality if the platform exposes multiple memory domains.
3. Evaluate Linux zero-copy or splice-style options.
   - TCP-to-QUIC true zero-copy is difficult because MsQuic send buffers must
     stay valid until send completion.
   - Any experiment must preserve MsQuic buffer lifetime and backpressure
     semantics.
   - This should be treated as a separate prototype, not a quick patch.
4. Reduce read event frequency.
   - Keep `--linux-relay-read-chunk-size 1MiB` as the current high-throughput
     baseline.
   - Measure whether larger chunks reduce syscall/copy overhead or only increase
     latency/backlog.

Required metrics before deeper tuning:

- TCP read bytes per `readv` histogram.
- Buffer pool acquire latency/failure histogram.
- Page fault counters during bench.
- Per-worker TCP read bytes and event counts.

Expected outcome:

- Better single-pipeline throughput if TCP read copy and buffer churn are the
  dominant q1 bottleneck.
- Better scaling if relay workers stop spending excessive time in kernel copy
  and memory management.

## Priority 2: Increase QUIC receive to TCP write aggregation

Evidence:

- Active TCP write side performs hundreds of thousands of `sendmsg` calls in
  15-20 seconds.
- Average TCP write size is only about 70-140 KiB, even with 1 MiB relay read
  chunks.
- q1/P1 server perf shows `__arch_copy_from_user` at 22.85%, which is TCP
  `sendmsg` copying proxy buffers into kernel.

Optimization directions:

1. Add histogram metrics for TCP write size.
   - Current max iov/max send bytes are insufficient.
   - Need distribution: 0-16K, 16-64K, 64-128K, 128-256K, 256K-1M, >1M.
2. Add histogram metrics for TCP write iov count.
   - Confirm whether vectored write aggregation is actually being used at high
     throughput.
3. Add QUIC receive callback metrics.
   - Buffer count per callback.
   - Total bytes per callback.
   - Per-buffer size distribution.
   - Receive callback queue depth and worker drain batch size.
4. Inspect `FlushDeferredQuicReceives`.
   - Determine whether it flushes too early.
   - Check whether multiple pending receive views can be merged into a larger
     single `sendmsg` without violating receive completion semantics.
5. Inspect MsQuic receive fragmentation.
   - Determine whether stream receive buffer/window settings or MsQuic internal
     chunking cap callback buffers around 64 KiB.

Required metrics:

- `tcp_write_sendmsg_bytes_bucket_*`
- `tcp_write_iov_count_bucket_*`
- `quic_receive_callback_bytes_bucket_*`
- `quic_receive_callback_buffer_count_bucket_*`
- `quic_receive_views_per_worker_flush_bucket_*`

Expected outcome:

- Fewer `sendmsg` calls per GiB.
- Larger average TCP write size.
- Lower kernel `__arch_copy_from_user` and syscall overhead.

## Priority 3: Reduce QUIC/MsQuic receive-side CPU cost

Evidence:

- q16/P16 upload server perf top symbol is `aes_gcm_dec_256_kernel` at 29.05%.
- q16/P16 server also shows:
  - MsQuic refcount/frame processing around 5.97%.
  - `QuicRecvBufferCopyIntoChunks` memcpy around 4.05%.
  - UDP receive copy around 3.80%.

Optimization directions:

1. Verify crypto acceleration.
   - Confirm OpenSSL/AES-GCM implementation selected on this ARM DGX platform.
   - Compare q16 throughput with a known MsQuic/secnetperf baseline.
2. Check MsQuic UDP GRO/GSO and batching.
   - Confirm receive batching is active.
   - Check NIC offload settings.
   - Confirm sysctl buffer tuning remains in place.
3. Tune CPU affinity.
   - Separate NIC IRQ/RSS, MsQuic workers, and relay workers.
   - Compare unpinned vs pinned q16/P16 throughput and perf.
4. Investigate MsQuic receive buffering copy.
   - Understand when `QuicRecvBufferCopyIntoChunks` is unavoidable.
   - Check whether app receive settings or stream window sizing affect copy
     frequency.

Required metrics/tests:

- secnetperf max-throughput baseline on the same DGX pair.
- `perf` q16/P16 upload and download with CPU affinity variants.
- `ethtool -k`, `ethtool -l`, `ethtool -x`, IRQ/RSS mapping snapshot.
- MsQuic worker thread CPU distribution.

Expected outcome:

- Higher q16/q32 throughput if crypto and MsQuic receive processing are the
  scaling limiter after TCP read bottleneck is reduced.

## Priority 4: Improve download backpressure behavior

Evidence:

- Download q1/P1 local client write side reaches about 513 MiB max pending QUIC
  receive bytes.
- Download q16/P16 local client write side reaches about 566 MiB max pending
  QUIC receive bytes.
- Download q16/P16 local client has 47386 TCP write EAGAIN events.

Optimization directions:

1. Make QUIC receive completion pacing more adaptive.
   - Continue using pending receive.
   - Delay `StreamReceiveComplete` more aggressively when TCP write backlog is
     high.
   - Resume completion when TCP write backlog drains.
2. Add backlog-driven metrics.
   - Pending QUIC receive bytes per tunnel.
   - Pending TCP write bytes per tunnel.
   - Time spent above 25%, 50%, 75%, and 100% of relay pending budget.
3. Classify TCP write hard errors.
   - Include errno and close state.
   - Separate expected shutdown/reset after iperf completion from real relay
     write failure.
4. Re-run download perf.
   - Current perf comparison focused on upload because it is cleaner.
   - Download has heavier pending receive and EAGAIN pressure, so it needs its
     own hotspot capture.

Expected outcome:

- Lower pending receive spikes.
- Fewer EAGAIN loops.
- More stable high-concurrency reverse/download tests.

## Priority 5: Use more parallelism as a probe, not the primary fix

Evidence:

- q1/P1: about 17 Gbps.
- q4/P16: about 20.6 Gbps upload.
- q16/P16: about 23.5 Gbps upload and 22.8 Gbps download.

Interpretation:

- More connections help because they add independent relay pipelines and spread
  work across more QUIC connection state and worker activity.
- Scaling is weak compared to direct TCP. That means connection count alone is
  not the root fix.

Next probe:

Run a clean matrix:

| QUIC conns | TCP streams | Direction | Duration |
|---:|---:|---|---:|
| 1 | 1 | upload/download | 30s |
| 2 | 2 | upload/download | 30s |
| 4 | 4 | upload/download | 30s |
| 8 | 8 | upload/download | 30s |
| 16 | 16 | upload/download | 30s |
| 32 | 32 | upload/download | 30s |

Expected interpretation:

- If q32 barely improves over q16, the dominant ceiling is CPU/copy/crypto/event
  overhead.
- If q32 improves significantly, investigate worker distribution, RSS, MsQuic
  worker scheduling, and per-connection lock contention.

## Recommended Execution Order

1. Add missing metrics for receive/write size distributions.
2. Run q1/q4/q8/q16/q32 iperf proxy matrix with the new metrics.
3. Capture download q16/P16 perf.
4. Run CPU affinity/RSS experiments.
5. Decide whether to prototype deeper TCP read or QUIC send zero-copy based on
   the post-metrics evidence.

## Current Working Hypothesis

`tcpquic-proxy` is currently bounded by user-space relay overhead rather than
link capacity:

- q1 is primarily limited by TCP read copy and single pipeline event flow.
- q16 exposes QUIC/TLS decrypt and MsQuic receive processing as the next major
  bottleneck.
- Download additionally suffers from TCP write backpressure and large pending
  QUIC receive backlog.

The most useful next code work is observability for aggregation and backlog,
because it will tell whether the next implementation should focus on larger
write batching, CPU affinity, MsQuic receive tuning, or a more invasive
zero-copy prototype.
