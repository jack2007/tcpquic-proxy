# Unified QUIC Receive Pending and Zstd Relay Design

Date: 2026-06-13

## Summary

The relay should make QUIC receive callbacks lightweight and uniform. Every
`QUIC_STREAM_EVENT_RECEIVE` should be converted into a pending receive view and
handed to the relay worker. The callback must not write TCP, decompress payloads,
or copy large receive buffers. Non-compressed traffic keeps the existing
zero-copy pending view path. Zstd-compressed traffic is decompressed by the relay
worker into worker-owned buffers before TCP writes.

LZ4 support will be removed from the project. The supported compression modes
become `none` and `zstd`.

The relay buffer pool should remain a cross-platform worker-owned buffer pool,
but the `Ingress` domain is no longer part of the target design. Once every QUIC
receive callback uses pending views, the callback no longer needs to copy
receive bytes into project-owned ingress buffers.

## Goals

1. Make QUIC receive callback work bounded and lightweight.
2. Use `QUIC_STATUS_PENDING` for both compressed and non-compressed receives.
3. Move zstd decompression entirely into the relay worker before TCP writes.
4. Remove LZ4 from runtime configuration, build integration, tests, and docs.
5. Remove callback-side ingress buffer ownership from the relay data path.
6. Keep worker buffer pressure visible and controllable with detailed metrics.

## Non-Goals

1. Do not redesign MsQuic stream send semantics.
2. Do not change TCP socket tuning defaults except where tests need explicit
   coverage.
3. Do not implement a new compression algorithm.
4. Do not make Linux and Windows workers share their event-loop implementation.
   They should share concepts and reusable components where practical.

## Current State

Linux and Windows already have a pending view fast path for non-compressed
QUIC-to-TCP traffic:

1. The QUIC receive callback stores MsQuic `QUIC_BUFFER` pointers in a receive
   view.
2. The callback returns `QUIC_STATUS_PENDING`.
3. The worker writes those bytes to TCP.
4. The worker calls `ReceiveComplete()` as TCP accepts bytes.

Compressed receive traffic is different today:

1. The callback detects compression.
2. The callback copies receive bytes into project-owned buffers.
3. The worker later decompresses copied bytes and writes decompressed output to
   TCP.

That compressed path keeps decompression off the callback thread, but the
callback is not uniformly lightweight because it still copies large receive
payloads and consumes ingress buffer slots.

`TqRelayBufferPool` has already been moved to a cross-platform implementation in
`src/tunnel/relay_buffer_pool.{h,cpp}`. `src/tunnel/linux_relay_buffer_pool.h`
is now only a compatibility alias.

## Target Receive Data Flow

### Non-Compressed QUIC to TCP

1. MsQuic invokes `QUIC_STREAM_EVENT_RECEIVE`.
2. The callback creates a pending receive view containing MsQuic buffer slices.
3. The callback enqueues the view to the relay worker.
4. The callback returns `QUIC_STATUS_PENDING`.
5. The worker writes the slices directly to TCP using the platform mechanism:
   Linux uses `writev`/`sendmsg` style scatter-gather writes; Windows uses
   `WSASend`.
6. After bytes are accepted by TCP, the worker advances the view and calls
   `ReceiveComplete(compressed_or_plain_bytes)`.

For non-compressed traffic, the QUIC receive bytes and TCP write bytes are the
same byte stream, so the completion count is also the TCP accepted byte count.

### Zstd-Compressed QUIC to TCP

1. MsQuic invokes `QUIC_STREAM_EVENT_RECEIVE`.
2. The callback creates a pending receive view containing compressed MsQuic
   buffer slices.
3. The callback enqueues the view to the relay worker.
4. The callback returns `QUIC_STATUS_PENDING`.
5. The worker feeds compressed slices into a per-relay zstd decompressor.
6. The decompressor writes output into worker-owned `TqRelayBufferPool` chunks.
7. Full or partially filled chunks are queued as pending TCP writes.
8. The worker calls `ReceiveComplete(consumed_compressed_bytes)` only after the
   compressed input bytes have been consumed and any produced decompressed bytes
   have been copied into worker-owned buffers.
9. TCP writes drain the worker-owned buffers and release them back to the pool.

For zstd traffic, `ReceiveComplete()` is counted in compressed QUIC input bytes,
not decompressed TCP output bytes.

## Zstd Decompression Interface

The current decompressor interface appends all output to `std::vector<uint8_t>`.
That is not suitable for the target path because high-ratio compressed input can
produce output much larger than one relay buffer chunk and can bypass buffer pool
backpressure.

The target decompressor should expose bounded output:

```cpp
struct TqDecompressResult {
    size_t InputConsumed;
    size_t OutputProduced;
    bool NeedsMoreInput;
    bool NeedsMoreOutput;
};

bool DecompressInto(
    const uint8_t* input,
    size_t inputLength,
    uint8_t* output,
    size_t outputCapacity,
    TqDecompressResult* result);
```

The worker owns the loop around this interface:

1. Acquire one worker buffer chunk.
2. Feed compressed input and the chunk to zstd.
3. If output is produced, queue the chunk for TCP writes.
4. If the chunk fills, acquire another chunk and continue.
5. If no worker buffer is available, stop consuming compressed input.
6. Complete only the compressed input bytes reported as consumed.

This preserves bounded memory usage and lets pool pressure stop decompression
without losing MsQuic receive ownership.

## QUIC Receive Completion Rules

The relay must maintain separate progress counters for compressed receives:

1. Compressed input bytes consumed by zstd.
2. Decompressed output bytes queued to TCP.
3. Decompressed output bytes written to TCP.

`ReceiveComplete()` may be called for compressed input bytes when both are true:

1. zstd has consumed those input bytes and no longer needs the MsQuic buffer
   memory.
2. Any output produced from those bytes is already in worker-owned memory or the
   decompressor produced no output.

The relay must not call `ReceiveComplete()` for input bytes that are still only
referenced by MsQuic receive slices and have not been consumed by zstd.

## Backpressure

The unified path has two independent pressure points.

Pending receive view pressure:

1. Each relay tracks pending QUIC receive bytes and pending receive queue depth.
2. If pending receive bytes cross the configured high watermark, the worker
   disables QUIC receive for that stream.
3. When pending bytes fall below the low watermark, the worker re-enables QUIC
   receive.

Worker buffer pressure:

1. TCP-to-QUIC reads need worker buffers.
2. Zstd decompression output needs worker buffers.
3. QUIC-to-TCP copied or decompressed pending writes need worker buffers.
4. If worker buffer acquisition fails because of slot limit or pending byte
   budget, the worker stops the producing operation and waits for existing TCP
   writes or QUIC sends to release buffers.

Callback code must not allocate large fallback buffers to escape backpressure.

## Buffer Pool Design

`TqRelayBufferPool` remains cross-platform. It should provide:

1. Fixed-size worker chunks.
2. Worker slot limit.
3. Pending byte budget.
4. RAII handles that return chunks to the pool.
5. Detailed acquire failure reasons:
   `PendingBytesLimit`, `SlotLimit`, and `AllocationFailure`.

The target design removes these concepts:

1. `TqBufferDomain::Ingress`.
2. `AcquireIngress()`.
3. `TransferToWorker()`.
4. Ingress-specific metrics and failure counters.

The name `Ingress` may remain temporarily during migration only if needed to
avoid a risky all-at-once change. The final data path should not depend on it.

## LZ4 Removal

The compression modes become:

1. `none`
2. `zstd`

The implementation should remove:

1. LZ4 CMake detection and compile definitions.
2. `TqLz4Compressor` and `TqLz4Decompressor`.
3. Runtime parsing and docs that advertise `lz4`.
4. LZ4-specific tests.
5. Build/link dependencies on `lz4frame`.

If a config or command line requests `lz4`, parsing should fail with an explicit
unsupported compression error.

## Cross-Platform Boundary

Shared:

1. `TqRelayBufferPool`.
2. Compression algorithm selection and zstd codec interfaces.
3. Pending receive view concepts and metrics names where practical.
4. Backpressure semantics.

Linux-specific:

1. `epoll` and `eventfd`.
2. TCP fd read/write readiness re-arm.
3. `readv`/`writev`/scatter-gather write behavior.
4. Linux relay worker thread implementation.

Windows-specific:

1. IOCP.
2. `WSARecv`/`WSASend`.
3. `OVERLAPPED` operation ownership.
4. Windows relay worker implementation.

The workers may keep separate implementations, but their observable metrics and
receive ownership rules should match.

## Metrics

Remove or retire ingress-specific metrics:

1. `quic_receive_ingress_buffer_acquire_failures`
2. `quic_receive_ingress_buffer_acquire_pending_budget_failures`
3. `quic_receive_ingress_buffer_acquire_slot_limit_failures`
4. `quic_receive_ingress_buffer_acquire_alloc_failures`

Keep or add pending receive metrics:

1. `quic_receive_view_queued`
2. `quic_receive_view_bytes`
3. `pending_quic_receive_bytes`
4. `max_pending_quic_receive_bytes`
5. `pending_quic_receive_queue_depth`
6. `max_pending_quic_receive_queue_depth`
7. `quic_receive_paused_count`
8. `quic_receive_resumed_count`
9. `deferred_receive_complete_bytes`
10. `deferred_receive_completes`
11. `deferred_receive_completion_flushes`

Add zstd metrics:

1. `zstd_decompress_input_bytes`
2. `zstd_decompress_output_bytes`
3. `zstd_decompress_calls`
4. `zstd_decompress_need_input`
5. `zstd_decompress_need_output`
6. `zstd_decompress_failures`

Keep worker buffer acquire metrics with detailed failure reasons:

1. total failures
2. pending byte budget failures
3. slot limit failures
4. allocation failures

## Error Handling

The relay should close or abort a relay on unrecoverable errors:

1. zstd decode error
2. invalid receive buffer from MsQuic
3. enqueue failure for a pending receive view
4. hard TCP write error
5. hard QUIC send error

Recoverable pressure events should not be counted as data loss:

1. worker slot limit
2. pending byte budget limit
3. TCP `EAGAIN` or Windows pending sends
4. temporary lack of output buffer during decompression

Recoverable pressure should pause the relevant producer and resume when
resources are released.

## Testing Strategy

Unit tests:

1. Non-compressed receive callback always returns pending.
2. Zstd receive callback always returns pending.
3. Zstd decompression handles split compressed input across receive callbacks.
4. Zstd decompression handles multiple compressed inputs in one receive view.
5. Zstd output larger than one worker chunk is split across multiple chunks.
6. Worker buffer exhaustion stops decompression without completing unconsumed
   compressed bytes.
7. `ReceiveComplete()` counts compressed input bytes for zstd.
8. LZ4 config parsing fails explicitly.
9. Buffer pool no longer exposes ingress acquire behavior in the final API.

Integration tests:

1. Linux 1x1 non-compressed upload/download.
2. Linux 1x1 zstd upload/download with compressible data.
3. Linux high-RTT speed test with zstd enabled.
4. Windows worker unit tests for pending receive ownership.
5. Cross-platform config tests for accepted and rejected compression modes.

Performance validation:

1. Compare non-compressed 1x1 throughput before and after the callback
   unification.
2. Compare zstd 1x1 throughput and CPU profile before and after bounded-output
   decompression.
3. Verify ingress metrics disappear and worker/zstd metrics explain pressure.

## Migration Plan

1. Add bounded zstd decompression interface and tests.
2. Convert compressed receive callback path to pending receive view.
3. Implement worker-side zstd receive drain with worker buffer chunks.
4. Update `ReceiveComplete()` accounting for compressed input progress.
5. Remove ingress usage from Linux and Windows receive paths.
6. Simplify `TqRelayBufferPool` to worker-only semantics.
7. Remove LZ4 support from build, config, code, tests, and docs.
8. Update metrics and admin output.
9. Run unit, integration, and DGX throughput tests.

