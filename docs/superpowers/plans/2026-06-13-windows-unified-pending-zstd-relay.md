# Windows Unified Pending Zstd Relay Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** On Windows, make compressed QUIC receives use pending receive views, move zstd decompression into the IOCP relay worker with bounded worker-owned output buffers, and align metrics with the Linux unified pending design.

**Architecture:** The Windows callback always queues a `TqWindowsPendingQuicReceive` and returns `QUIC_STATUS_PENDING`. Non-compressed views post `WSASend` directly from MsQuic receive slices. Zstd views are drained by the IOCP worker into worker-owned buffers before posting TCP sends. LZ4 is removed by the shared compression/config work and should not be reintroduced.

**Tech Stack:** C++17, CMake/MSVC, MsQuic stream pending receive API, zstd streaming API, Windows IOCP, `WSARecv`, `WSASend`, existing assert-style unit tests.

---

## File Map

- Modify: `src/tunnel/windows_relay_worker.h`
- Modify: `src/tunnel/windows_relay_worker.cpp`
- Modify: `src/unittest/windows_relay_worker_test.cpp`
- Reuse shared Linux-plan changes in:
  - `src/protocol/compress.h`
  - `src/protocol/compress.cpp`
  - `src/tunnel/relay_buffer_pool.h`
  - `src/tunnel/relay_buffer_pool.cpp`
  - `src/runtime/relay_metrics.h`
  - `src/runtime/relay_metrics.cpp`

## Prerequisite

Complete or merge the shared changes from the Linux plan:

1. LZ4 removed.
2. `ITqDecompressor::DecompressInto()` available.
3. `TqRelayBufferPool` worker-only API available.
4. Shared metrics names finalized.

## Task 1: Make Windows Compressed Receive Callback Pending

**Files:**
- Modify: `src/tunnel/windows_relay_worker.cpp`
- Test: `src/unittest/windows_relay_worker_test.cpp`

- [ ] **Step 1: Add failing callback test**

Add a Windows worker test that registers a relay with `TqCompressAlgo::Zstd`, dispatches `QUIC_STREAM_EVENT_RECEIVE`, and asserts:

```cpp
const QUIC_STATUS status = TqWindowsRelayWorker::StreamCallback(stream, stream->Context, &event);
assert(status == QUIC_STATUS_PENDING);
```

Use a small zstd-compressed payload produced by `TqCreateCompressor(TqCompressAlgo::Zstd, 1)`.

- [ ] **Step 2: Run failing Windows test**

Run on Windows:

```powershell
cmake --build build-x64 --target tcpquic_windows_relay_worker_test --config Release
.\build-x64\bin\Release\tcpquic_windows_relay_worker_test.exe
```

Expected before implementation: compressed receive callback does not use pending view.

- [ ] **Step 3: Route all Windows receives through `QueueDeferredQuicReceive()`**

In `TqWindowsRelayWorker::StreamCallback`, replace:

```cpp
if (relay->Decompressor == nullptr && relay->CompressAlgo == TqCompressAlgo::None) {
    ...
}
```

with unconditional queueing:

```cpp
if (!worker->QueueDeferredQuicReceive(
        relay,
        stream,
        event->RECEIVE.Buffers,
        event->RECEIVE.BufferCount,
        fin)) {
    worker->CloseRelay(relay);
}
return QUIC_STATUS_PENDING;
```

- [ ] **Step 4: Run Windows callback test**

Run:

```powershell
cmake --build build-x64 --target tcpquic_windows_relay_worker_test --config Release
.\build-x64\bin\Release\tcpquic_windows_relay_worker_test.exe
```

Expected: callback pending assertion passes.

- [ ] **Step 5: Commit**

```powershell
git add src/tunnel/windows_relay_worker.cpp src/unittest/windows_relay_worker_test.cpp
git commit -m "feat: make windows quic receive callback always pending"
```

## Task 2: Add Windows Worker Zstd Receive Drain

**Files:**
- Modify: `src/tunnel/windows_relay_worker.h`
- Modify: `src/tunnel/windows_relay_worker.cpp`
- Test: `src/unittest/windows_relay_worker_test.cpp`

- [ ] **Step 1: Add failing zstd output test**

Add a test that:

1. Creates a socketpair-equivalent test socket.
2. Registers a Windows relay with zstd decompressor.
3. Sends one compressed receive event through the stream callback.
4. Reads from the TCP peer and asserts decompressed bytes match original bytes.

Use a payload larger than one worker chunk to force chunk splitting:

```cpp
std::vector<uint8_t> plain(1024 * 1024, 0x41);
```

- [ ] **Step 2: Run failing test**

Run:

```powershell
cmake --build build-x64 --target tcpquic_windows_relay_worker_test --config Release
.\build-x64\bin\Release\tcpquic_windows_relay_worker_test.exe
```

Expected before implementation: no decompressed TCP output.

- [ ] **Step 3: Add worker-owned zstd output buffers**

Add a worker buffer pool to `RelayContext` for decompressed output:

```cpp
TqRelayBufferPool TcpSendBuffers;
```

Initialize it from tuning with enough slots for pending sends:

```cpp
TcpSendBuffers(tuning.RelayIoSize, tuning.WindowsRelayWorkerSlots, tuning.WindowsRelayPerTunnelPendingBytes)
```

Reserve worker slots during relay construction.

- [ ] **Step 4: Implement `PostTcpSendFromCompressedReceiveView()`**

Add a private helper:

```cpp
bool PostTcpSendFromCompressedReceiveView(
    const std::shared_ptr<RelayContext>& relay,
    const std::shared_ptr<TqWindowsPendingQuicReceive>& view);
```

The helper should:

1. Acquire one worker buffer from `TcpSendBuffers`.
2. Call `DecompressInto()` with the current compressed slice and worker buffer.
3. If output is produced, post `WSASend` using the worker buffer owner in `IoOperation::BufferOwner`.
4. Advance compressed input progress by `InputConsumed`.
5. Call `FlushDeferredReceiveCompletion()` for consumed compressed input bytes.
6. Stop and return true if a TCP send is pending.
7. Return false only on unrecoverable zstd or socket errors.

- [ ] **Step 5: Branch from receive view drain**

In `HandleQuicReceiveViewQueued()`:

```cpp
if (relay->Decompressor != nullptr && relay->CompressAlgo == TqCompressAlgo::Zstd) {
    if (!PostTcpSendFromCompressedReceiveView(relay, view)) {
        (void)CompletePendingQuicReceive(relay, view);
        CloseRelay(relay);
    }
    return;
}
```

Keep existing `PostTcpSendFromReceiveView()` for non-compressed traffic.

- [ ] **Step 6: Continue compressed view after TCP send completion**

In `HandleTcpSend()`, when `op->ReceiveView` belongs to a zstd relay, call `PostTcpSendFromCompressedReceiveView()` again if the view still has compressed input. Complete the view only when all compressed input has been consumed and all output sends have completed.

- [ ] **Step 7: Run Windows worker tests**

Run:

```powershell
cmake --build build-x64 --target tcpquic_windows_relay_worker_test --config Release
.\build-x64\bin\Release\tcpquic_windows_relay_worker_test.exe
```

Expected: zstd receive view test passes.

- [ ] **Step 8: Commit**

```powershell
git add src/tunnel/windows_relay_worker.h src/tunnel/windows_relay_worker.cpp src/unittest/windows_relay_worker_test.cpp
git commit -m "feat: drain zstd pending receives on windows worker"
```

## Task 3: Align Windows Metrics

**Files:**
- Modify: `src/tunnel/windows_relay_worker.h`
- Modify: `src/tunnel/windows_relay_worker.cpp`
- Modify: `src/runtime/relay_metrics.h`
- Modify: `src/runtime/relay_metrics.cpp`
- Test: `src/unittest/windows_relay_worker_test.cpp`
- Test: `src/unittest/admin_http_test.cpp`

- [ ] **Step 1: Add metrics assertions**

Assert Windows snapshot increments:

```cpp
assert(snapshot.ZstdDecompressInputBytes >= compressed.size());
assert(snapshot.ZstdDecompressOutputBytes >= plain.size());
assert(snapshot.ZstdDecompressFailures == 0);
```

- [ ] **Step 2: Add zstd metric fields to Windows snapshot**

Add:

```cpp
uint64_t ZstdDecompressInputBytes{0};
uint64_t ZstdDecompressOutputBytes{0};
uint64_t ZstdDecompressCalls{0};
uint64_t ZstdDecompressNeedInput{0};
uint64_t ZstdDecompressNeedOutput{0};
uint64_t ZstdDecompressFailures{0};
```

Populate in `Snapshot()`.

- [ ] **Step 3: Emit admin metrics**

Map Windows snapshot fields into shared relay metrics JSON using the same names as Linux:

```text
zstd_decompress_input_bytes
zstd_decompress_output_bytes
zstd_decompress_calls
zstd_decompress_need_input
zstd_decompress_need_output
zstd_decompress_failures
```

- [ ] **Step 4: Run tests**

Run:

```powershell
cmake --build build-x64 --target tcpquic_windows_relay_worker_test tcpquic_admin_http_test --config Release
.\build-x64\bin\Release\tcpquic_windows_relay_worker_test.exe
.\build-x64\bin\Release\tcpquic_admin_http_test.exe
```

Expected: all exit 0.

- [ ] **Step 5: Commit**

```powershell
git add src/tunnel/windows_relay_worker.h src/tunnel/windows_relay_worker.cpp src/runtime/relay_metrics.h src/runtime/relay_metrics.cpp src/unittest/windows_relay_worker_test.cpp src/unittest/admin_http_test.cpp
git commit -m "feat: report windows zstd relay metrics"
```

## Task 4: Windows Verification

**Files:**
- Modify docs only if test results are saved.

- [ ] **Step 1: Build Windows targets**

Run:

```powershell
cmake --build build-x64 --target tcpquic-proxy tcpquic_compress_test tcpquic_config_router_test tcpquic_relay_buffer_pool_test tcpquic_windows_relay_worker_test tcpquic_admin_http_test --config Release
```

Expected: build exits 0.

- [ ] **Step 2: Run unit tests**

Run:

```powershell
.\build-x64\bin\Release\tcpquic_compress_test.exe
.\build-x64\bin\Release\tcpquic_config_router_test.exe
.\build-x64\bin\Release\tcpquic_relay_buffer_pool_test.exe
.\build-x64\bin\Release\tcpquic_windows_relay_worker_test.exe
.\build-x64\bin\Release\tcpquic_admin_http_test.exe
git diff --check
```

Expected: all exit 0.

- [ ] **Step 3: Run Windows loopback smoke**

Run:

```powershell
.\scripts\test-tcpquic-proxy-windows.ps1 -Compress off
.\scripts\test-tcpquic-proxy-windows.ps1 -Compress zstd
```

Expected: both pass. There is no LZ4 smoke path after this change.

- [ ] **Step 4: Save validation results**

If dual-machine Windows validation is available, save throughput and metrics under:

```text
docs/windows-unified-pending-zstd-YYYYMMDD-HHMMSS/
```

Commit those docs:

```powershell
git add docs/windows-unified-pending-zstd-*
git commit -m "docs: record windows unified pending zstd validation"
```

