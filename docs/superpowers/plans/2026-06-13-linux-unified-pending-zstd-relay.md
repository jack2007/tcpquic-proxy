# Linux Unified Pending Zstd Relay Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** On Linux, make all QUIC receive callbacks return pending, move zstd decompression into the Linux relay worker with bounded worker-pool output, remove LZ4 support, and remove ingress buffer usage from the Linux relay data path.

**Architecture:** The Linux callback always creates a `TqPendingQuicReceive` view over MsQuic receive buffers and returns `QUIC_STATUS_PENDING`. The worker drains non-compressed views directly with `writev`; for zstd views, it feeds compressed slices into a bounded-output decompressor and queues worker-owned chunks for TCP writes. `TqRelayBufferPool` becomes a worker-only pool after the Linux data path no longer calls ingress APIs.

**Tech Stack:** C++17, CMake, MsQuic stream pending receive API, zstd streaming API, Linux `epoll`, `readv`/`writev`, existing assert-style unit tests.

---

## File Map

- Modify: `src/protocol/compress.h` and `src/protocol/compress.cpp`
  - Remove LZ4 enum and implementation.
  - Add bounded zstd decompression API.
- Modify: `src/config/config.cpp`, `src/config/tuning.cpp`, `src/config/tuning.h`, `src/config/config.h`
  - Reject `lz4`.
  - Make auto-compression resolve to `off` or `zstd`.
  - Keep CLI parsing deterministic.
- Modify: `src/tunnel/linux_relay_worker.h` and `src/tunnel/linux_relay_worker.cpp`
  - Route compressed receive callbacks through pending receive views.
  - Add worker-side zstd view drain.
  - Remove Linux receive callback ingress copy path.
- Modify: `src/tunnel/relay_buffer_pool.h` and `src/tunnel/relay_buffer_pool.cpp`
  - Remove ingress domain after Linux no longer uses it.
- Modify: `src/runtime/relay_metrics.h` and `src/runtime/relay_metrics.cpp`
  - Remove ingress metrics.
  - Add zstd decompression and worker-buffer failure metrics.
- Modify: `src/CMakeLists.txt`
  - Remove LZ4 link dependency and `TCPQUIC_HAS_LZ4`.
- Modify tests:
  - `src/unittest/compress_test.cpp`
  - `src/unittest/config_router_test.cpp`
  - `src/unittest/tuning_test.cpp`
  - `src/unittest/relay_buffer_pool_test.cpp`
  - `src/unittest/linux_relay_buffer_pool_test.cpp`
  - `src/unittest/linux_relay_worker_io_test.cpp`

## Task 1: Remove LZ4 From Config and Build

**Files:**
- Modify: `src/protocol/compress.h`
- Modify: `src/protocol/compress.cpp`
- Modify: `src/config/config.cpp`
- Modify: `src/config/tuning.cpp`
- Modify: `src/CMakeLists.txt`
- Test: `src/unittest/compress_test.cpp`
- Test: `src/unittest/config_router_test.cpp`
- Test: `src/unittest/tuning_test.cpp`

- [ ] **Step 1: Write failing config tests for rejected LZ4**

Add assertions in `src/unittest/config_router_test.cpp` near existing compression validation:

```cpp
{
    const char* argv[] = {
        "tcpquic-proxy",
        "client",
        "--quic-peer",
        "127.0.0.1:4433",
        "--compress",
        "lz4",
    };
    TqConfig cfg{};
    std::string err;
    assert(!TqParseArgs(static_cast<int>(std::size(argv)), const_cast<char**>(argv), cfg, err));
    assert(err.find("unsupported compress") != std::string::npos ||
           err.find("invalid compress") != std::string::npos);
}
```

Add router config validation for `"compress":"lz4"`:

```cpp
{
    TqRouterConfig router{};
    router.Peers.push_back(TqPeerConfig{});
    router.Peers.back().PeerId = "p1";
    router.Peers.back().QuicPeer = "127.0.0.1:4433";
    router.Peers.back().SocksListen = "127.0.0.1:1080";
    router.Peers.back().Compress = "lz4";
    std::string err;
    assert(!TqValidateRouterConfig(router, err));
    assert(err.find("compress") != std::string::npos);
}
```

- [ ] **Step 2: Run failing config tests**

Run:

```bash
rtk cmake --build build --target tcpquic_config_router_test tcpquic_tuning_test -j2
rtk ./build/bin/Release/tcpquic_config_router_test
```

Expected before implementation: `tcpquic_config_router_test` fails because `lz4` is still accepted.

- [ ] **Step 3: Remove LZ4 enum and implementation**

Change `src/protocol/compress.h`:

```cpp
enum class TqCompressAlgo { None, Zstd };
```

Remove `TqLz4Compressor`, `TqLz4Decompressor`, `#include <lz4frame.h>`, and all `TCPQUIC_HAS_LZ4` blocks from `src/protocol/compress.cpp`.

Update factory switches so only `None` and `Zstd` remain:

```cpp
case TqCompressAlgo::None:
    return std::make_unique<TqNoneCompressor>();
case TqCompressAlgo::Zstd:
#ifdef TCPQUIC_HAS_ZSTD
    return std::make_unique<TqZstdCompressor>(level);
#else
    return nullptr;
#endif
```

Apply the same two-case switch for `TqCreateDecompressor`.

- [ ] **Step 4: Reject LZ4 in config and auto tuning**

Change `IsValidCompress()` in `src/config/config.cpp`:

```cpp
bool IsValidCompress(const std::string& value) {
    return value.empty() || value == "auto" || value == "zstd" || value == "off";
}
```

Change usage text:

```cpp
"  --compress <mode>          auto|zstd|off (default auto)\n"
```

In `src/config/tuning.cpp`, change auto selection so it never returns `"lz4"`:

```cpp
const char* TqResolveAutoCompress(const TqConfig& cfg) {
    if (cfg.Compress != "auto") {
        return cfg.Compress.c_str();
    }
    TqCompressionObservations obs;
    {
        std::lock_guard<std::mutex> guard(g_Runtime.Lock);
        obs = g_Runtime.CompObs;
    }
    if (obs.Samples < 3) {
        return "off";
    }
    const uint64_t raw = obs.RawBytes;
    if (raw == 0) {
        return "off";
    }
    const uint64_t ratioPermille = (obs.CompressedBytes * 1000) / raw;
    return ratioPermille < kCompressRatioOffPermille ? "zstd" : "off";
}
```

Remove `kCompressRatioLz4Permille` and any helper branch that returns `"lz4"`.

- [ ] **Step 5: Remove LZ4 from CMake**

Change `src/CMakeLists.txt`:

```cmake
target_link_libraries(tcpquic-proxy PRIVATE libzstd spdlog::spdlog)
target_compile_definitions(tcpquic-proxy PRIVATE TCPQUIC_HAS_ZSTD=1)

set(TCPQUIC_TEST_LIBS libzstd)
set(TCPQUIC_TEST_DEFS TCPQUIC_HAS_ZSTD=1)
```

Remove `lz4` from any target-specific link line that only needs compression tests.

- [ ] **Step 6: Remove LZ4 tests and run compression/config tests**

Delete all `#ifdef TCPQUIC_HAS_LZ4` test sections from `src/unittest/compress_test.cpp`.

Run:

```bash
rtk cmake --build build --target tcpquic_compress_test tcpquic_config_router_test tcpquic_tuning_test -j2
rtk ./build/bin/Release/tcpquic_compress_test
rtk ./build/bin/Release/tcpquic_config_router_test
rtk ./build/bin/Release/tcpquic_tuning_test
```

Expected: all commands exit 0.

- [ ] **Step 7: Commit**

```bash
rtk git add src/protocol/compress.h src/protocol/compress.cpp src/config/config.cpp src/config/tuning.cpp src/CMakeLists.txt src/unittest/compress_test.cpp src/unittest/config_router_test.cpp src/unittest/tuning_test.cpp
rtk git commit -m "refactor: remove lz4 compression support"
```

## Task 2: Add Bounded Zstd Decompression API

**Files:**
- Modify: `src/protocol/compress.h`
- Modify: `src/protocol/compress.cpp`
- Test: `src/unittest/compress_test.cpp`

- [ ] **Step 1: Add failing bounded-output tests**

Add this helper to `src/unittest/compress_test.cpp`:

```cpp
static std::vector<uint8_t> CompressZstdPayload(const std::vector<uint8_t>& payload) {
    auto compressor = TqCreateCompressor(TqCompressAlgo::Zstd, 1);
    assert(compressor != nullptr);
    std::vector<uint8_t> compressed;
    for (size_t off = 0; off < payload.size(); off += 65536) {
        const size_t len = std::min<size_t>(65536, payload.size() - off);
        std::vector<uint8_t> chunk;
        assert(compressor->Compress(payload.data() + off, len, chunk, false));
        compressed.insert(compressed.end(), chunk.begin(), chunk.end());
    }
    std::vector<uint8_t> tail;
    assert(compressor->Compress(nullptr, 0, tail, true));
    compressed.insert(compressed.end(), tail.begin(), tail.end());
    return compressed;
}
```

Add bounded-output assertions:

```cpp
{
    std::vector<uint8_t> payload(1024 * 1024, 0x31);
    std::vector<uint8_t> compressed = CompressZstdPayload(payload);
    auto decompressor = TqCreateDecompressor(TqCompressAlgo::Zstd);
    assert(decompressor != nullptr);

    std::vector<uint8_t> output;
    size_t inputOffset = 0;
    while (inputOffset < compressed.size()) {
        uint8_t chunk[4096]{};
        TqDecompressResult result{};
        assert(decompressor->DecompressInto(
            compressed.data() + inputOffset,
            compressed.size() - inputOffset,
            chunk,
            sizeof(chunk),
            &result));
        assert(result.InputConsumed <= compressed.size() - inputOffset);
        assert(result.OutputProduced <= sizeof(chunk));
        inputOffset += result.InputConsumed;
        output.insert(output.end(), chunk, chunk + result.OutputProduced);
        assert(result.InputConsumed != 0 || result.OutputProduced != 0 || result.NeedsMoreOutput);
    }

    while (output.size() < payload.size()) {
        uint8_t chunk[4096]{};
        TqDecompressResult result{};
        assert(decompressor->DecompressInto(nullptr, 0, chunk, sizeof(chunk), &result));
        output.insert(output.end(), chunk, chunk + result.OutputProduced);
        if (result.OutputProduced == 0 && result.NeedsMoreInput) {
            break;
        }
    }

    assert(output == payload);
}
```

- [ ] **Step 2: Run failing compression test**

Run:

```bash
rtk cmake --build build --target tcpquic_compress_test -j2
```

Expected before implementation: compile fails because `TqDecompressResult` and `DecompressInto` do not exist.

- [ ] **Step 3: Add bounded API to interface**

Add to `src/protocol/compress.h`:

```cpp
struct TqDecompressResult {
    size_t InputConsumed{0};
    size_t OutputProduced{0};
    bool NeedsMoreInput{false};
    bool NeedsMoreOutput{false};
};
```

Add to `ITqDecompressor`:

```cpp
virtual bool DecompressInto(
    const uint8_t* input,
    size_t inputLength,
    uint8_t* output,
    size_t outputCapacity,
    TqDecompressResult* result) = 0;
```

- [ ] **Step 4: Implement bounded zstd**

In `TqZstdDecompressor`, implement:

```cpp
bool DecompressInto(
    const uint8_t* input,
    size_t inputLength,
    uint8_t* output,
    size_t outputCapacity,
    TqDecompressResult* result) override {
    if (result == nullptr || ctx_ == nullptr) {
        return false;
    }
    *result = TqDecompressResult{};
    if (inputLength > 0 && input == nullptr) {
        return false;
    }
    if (outputCapacity > 0 && output == nullptr) {
        return false;
    }

    ZSTD_inBuffer inBuffer = {input, inputLength, 0};
    ZSTD_outBuffer outBuffer = {output, outputCapacity, 0};
    const size_t ret = ZSTD_decompressStream(ctx_, &outBuffer, &inBuffer);
    if (ZSTD_isError(ret)) {
        return false;
    }
    result->InputConsumed = inBuffer.pos;
    result->OutputProduced = outBuffer.pos;
    result->NeedsMoreInput = (ret != 0 && inBuffer.pos == inBuffer.size);
    result->NeedsMoreOutput = (outBuffer.pos == outBuffer.size && ret != 0);
    return true;
}
```

For `TqNoneDecompressor`, implement bounded passthrough:

```cpp
bool DecompressInto(
    const uint8_t* input,
    size_t inputLength,
    uint8_t* output,
    size_t outputCapacity,
    TqDecompressResult* result) override {
    if (result == nullptr) {
        return false;
    }
    *result = TqDecompressResult{};
    if (inputLength > 0 && input == nullptr) {
        return false;
    }
    if (outputCapacity > 0 && output == nullptr) {
        return false;
    }
    const size_t n = std::min(inputLength, outputCapacity);
    if (n > 0) {
        std::memcpy(output, input, n);
    }
    result->InputConsumed = n;
    result->OutputProduced = n;
    result->NeedsMoreOutput = n < inputLength;
    result->NeedsMoreInput = n == inputLength;
    return true;
}
```

Keep existing `Decompress(..., vector&)` by looping over `DecompressInto()` into a scratch buffer.

- [ ] **Step 5: Run compression tests**

Run:

```bash
rtk cmake --build build --target tcpquic_compress_test -j2
rtk ./build/bin/Release/tcpquic_compress_test
```

Expected: exit 0.

- [ ] **Step 6: Commit**

```bash
rtk git add src/protocol/compress.h src/protocol/compress.cpp src/unittest/compress_test.cpp
rtk git commit -m "feat: add bounded zstd decompression"
```

## Task 3: Make Linux Compressed Receive Callback Pending

**Files:**
- Modify: `src/tunnel/linux_relay_worker.cpp`
- Modify: `src/tunnel/linux_relay_worker.h`
- Test: `src/unittest/linux_relay_worker_io_test.cpp`

- [ ] **Step 1: Add failing Linux callback test for zstd pending**

In `src/unittest/linux_relay_worker_io_test.cpp`, add a fake stream with zstd compression and dispatch a receive event:

```cpp
{
    TqLinuxRelayWorkerConfig config{};
    config.EventBudget = 128;
    config.ReadChunkSize = 4096;
    config.ReadBatchBytes = 4096;
    config.MaxIov = 4;
    config.WorkerSlots = 8;
    config.IngressSlots = 0;
    config.MaxPendingBytes = 64 * 1024;

    TqLinuxRelayWorker worker(config);
    assert(worker.Start());

    int fds[2]{-1, -1};
    assert(::socketpair(AF_UNIX, SOCK_STREAM, 0, fds) == 0);

    alignas(MsQuicStream) uint8_t fakeStreamStorage[sizeof(MsQuicStream)]{};
    std::memset(fakeStreamStorage, 0, sizeof(fakeStreamStorage));
    MsQuicStream* fakeStream = reinterpret_cast<MsQuicStream*>(fakeStreamStorage);
    fakeStream->Handle = reinterpret_cast<HQUIC>(static_cast<uintptr_t>(1));

    auto compressor = TqCreateCompressor(TqCompressAlgo::Zstd, 1);
    auto decompressor = TqCreateDecompressor(TqCompressAlgo::Zstd);
    assert(compressor != nullptr);
    assert(decompressor != nullptr);

    TqLinuxRelayRegistration registration{};
    registration.TcpFd = fds[0];
    registration.Stream = fakeStream;
    registration.Decompressor = decompressor.get();
    registration.CompressAlgo = TqCompressAlgo::Zstd;
    registration.EnableQuicSends = false;
    assert(worker.RegisterRelayForTest(registration));

    const char plain[] = "compressed receive must return pending";
    std::vector<uint8_t> compressed;
    assert(compressor->Compress(
        reinterpret_cast<const uint8_t*>(plain),
        sizeof(plain) - 1,
        compressed,
        true));
    QUIC_BUFFER buffer{};
    buffer.Buffer = compressed.data();
    buffer.Length = static_cast<uint32_t>(compressed.size());
    QUIC_STREAM_EVENT event{};
    event.Type = QUIC_STREAM_EVENT_RECEIVE;
    event.RECEIVE.Buffers = &buffer;
    event.RECEIVE.BufferCount = 1;

    const QUIC_STATUS status = worker.DispatchStreamEventForTest(fakeStream, &event);
    assert(status == QUIC_STATUS_PENDING);

    worker.Stop();
    ::close(fds[1]);
}
```

- [ ] **Step 2: Run failing Linux worker test**

Run:

```bash
rtk cmake --build build --target tcpquic_linux_relay_worker_io_test -j2
rtk ./build/bin/Release/tcpquic_linux_relay_worker_io_test
```

Expected before implementation: assertion fails because compressed callback returns success after copy path, or registration cannot progress with `IngressSlots = 0`.

- [ ] **Step 3: Route all receive callbacks through `QueueDeferredQuicReceive()`**

In `TqLinuxRelayWorker::StreamCallback`, replace the compression branch with:

```cpp
if (event->Type == QUIC_STREAM_EVENT_RECEIVE) {
    if (relay->Closing) {
        return QUIC_STATUS_SUCCESS;
    }
    const bool fin = (event->RECEIVE.Flags & QUIC_RECEIVE_FLAG_FIN) != 0;
    if (!QueueDeferredQuicReceive(
            relay,
            stream,
            event->RECEIVE.Buffers,
            event->RECEIVE.BufferCount,
            fin)) {
        AbortRelayFromCallback(relayId, stream);
    }
    return QUIC_STATUS_PENDING;
}
```

- [ ] **Step 4: Run Linux worker test**

Run:

```bash
rtk cmake --build build --target tcpquic_linux_relay_worker_io_test -j2
rtk ./build/bin/Release/tcpquic_linux_relay_worker_io_test
```

Expected: the new callback pending assertion passes. Some zstd drain assertions may still be absent until Task 4.

- [ ] **Step 5: Commit**

```bash
rtk git add src/tunnel/linux_relay_worker.cpp src/unittest/linux_relay_worker_io_test.cpp
rtk git commit -m "feat: make linux quic receive callback always pending"
```

## Task 4: Drain Zstd Pending Views in Linux Worker

**Files:**
- Modify: `src/tunnel/linux_relay_worker.cpp`
- Modify: `src/tunnel/linux_relay_worker.h`
- Test: `src/unittest/linux_relay_worker_io_test.cpp`

- [ ] **Step 1: Extend zstd pending test to verify TCP output**

Extend the Task 3 test after dispatch:

```cpp
char received[128]{};
const ssize_t n = ::read(fds[1], received, sizeof(received));
assert(n == static_cast<ssize_t>(sizeof(plain) - 1));
assert(std::memcmp(received, plain, sizeof(plain) - 1) == 0);
```

- [ ] **Step 2: Add large-output test**

Add a test that compresses 1 MiB of zeros, sets `ReadChunkSize = 4096`, and reads 1 MiB from the socketpair peer:

```cpp
std::vector<uint8_t> plain(1024 * 1024, 0);
std::vector<uint8_t> compressed;
for (size_t off = 0; off < plain.size(); off += 65536) {
    const size_t len = std::min<size_t>(65536, plain.size() - off);
    std::vector<uint8_t> out;
    assert(compressor->Compress(plain.data() + off, len, out, false));
    compressed.insert(compressed.end(), out.begin(), out.end());
}
std::vector<uint8_t> tail;
assert(compressor->Compress(nullptr, 0, tail, true));
compressed.insert(compressed.end(), tail.begin(), tail.end());
```

Feed `compressed` in a single receive event and read until `plain.size()` bytes are received. Assert equality.

- [ ] **Step 3: Run failing test**

Run:

```bash
rtk cmake --build build --target tcpquic_linux_relay_worker_io_test -j2
rtk ./build/bin/Release/tcpquic_linux_relay_worker_io_test
```

Expected before implementation: TCP output for zstd pending view is missing.

- [ ] **Step 4: Implement zstd drain path**

In `FlushDeferredQuicReceives()`, branch before the non-compressed `writev` path:

```cpp
const bool needsDecompress =
    relay->Decompressor != nullptr && relay->CompressAlgo == TqCompressAlgo::Zstd;
if (needsDecompress) {
    if (!DrainCompressedQuicReceiveView(relay, *view)) {
        break;
    }
    continue;
}
```

Add private helper in `linux_relay_worker.h`:

```cpp
bool DrainCompressedQuicReceiveView(RelayState* relay, TqPendingQuicReceive& view);
```

Implement helper in `linux_relay_worker.cpp`:

```cpp
bool TqLinuxRelayWorker::DrainCompressedQuicReceiveView(
    RelayState* relay,
    TqPendingQuicReceive& view) {
    while (view.SliceIndex < view.Slices.size()) {
        const auto& slice = view.Slices[view.SliceIndex];
        const uint8_t* input = slice.Data + view.SliceOffset;
        const size_t inputLength = slice.Length - view.SliceOffset;

        TqBufferAcquireFailure acquireFailure = TqBufferAcquireFailure::None;
        auto output = relay->Pool.AcquireWorker(&acquireFailure);
        if (!output) {
            RecordBufferAcquireFailure(
                RelayErrorKind::QuicReceiveTcpBufferAcquire, acquireFailure);
            ArmTcpWritable(relay, true);
            return false;
        }

        TqDecompressResult result{};
        if (!relay->Decompressor->DecompressInto(
                input,
                inputLength,
                output->Data(),
                output->Capacity(),
                &result)) {
            RecordError(RelayErrorKind::QuicReceiveDecompress);
            if (relay->Handle != nullptr) {
                relay->Handle->Stop.store(true);
            }
            return false;
        }

        if (result.OutputProduced > 0) {
            output->SetLength(result.OutputProduced);
            uint8_t* data = output->Data();
            relay->PendingTcpWrites.push_back(
                TqBufferView{data, result.OutputProduced, std::move(output)});
            DecompressedTcpBytes.fetch_add(result.OutputProduced);
        }

        if (result.InputConsumed > 0) {
            view.PendingCompleteBytes += result.InputConsumed;
            view.CompletedLength += result.InputConsumed;
            relay->PendingQuicReceiveBytes =
                relay->PendingQuicReceiveBytes >= result.InputConsumed
                    ? relay->PendingQuicReceiveBytes - result.InputConsumed
                    : 0;
            view.SliceOffset += result.InputConsumed;
            if (view.SliceOffset >= slice.Length) {
                view.SliceOffset = 0;
                ++view.SliceIndex;
            }
            FlushDeferredReceiveCompletion(view, false);
        }

        if (result.OutputProduced == 0) {
            output.reset();
        }

        FlushTcpWrites(relay);
        if (!relay->PendingTcpWrites.empty()) {
            return false;
        }

        if (result.InputConsumed == 0 && result.OutputProduced == 0) {
            return false;
        }
    }

    FlushDeferredReceiveCompletion(view, true);
    if (view.Fin) {
        relay->TcpWriteShutdownQueued = true;
    }
    return true;
}
```

After the helper returns true, remove the completed view from `PendingQuicReceives`.

- [ ] **Step 5: Run Linux worker test**

Run:

```bash
rtk cmake --build build --target tcpquic_linux_relay_worker_io_test -j2
rtk ./build/bin/Release/tcpquic_linux_relay_worker_io_test
```

Expected: zstd pending receive writes correct TCP bytes.

- [ ] **Step 6: Commit**

```bash
rtk git add src/tunnel/linux_relay_worker.h src/tunnel/linux_relay_worker.cpp src/unittest/linux_relay_worker_io_test.cpp
rtk git commit -m "feat: drain zstd pending receives on linux worker"
```

## Task 5: Remove Ingress From Linux Data Path and Buffer Pool

**Files:**
- Modify: `src/tunnel/relay_buffer_pool.h`
- Modify: `src/tunnel/relay_buffer_pool.cpp`
- Modify: `src/tunnel/linux_relay_worker.cpp`
- Modify: `src/tunnel/linux_relay_worker.h`
- Test: `src/unittest/relay_buffer_pool_test.cpp`
- Test: `src/unittest/linux_relay_buffer_pool_test.cpp`

- [ ] **Step 1: Change buffer pool tests to worker-only semantics**

In `src/unittest/relay_buffer_pool_test.cpp`, remove tests that call:

```cpp
Acquire(TqBufferDomain::Ingress)
AcquireIngress()
TransferToWorker()
Reserve(workerSlots, ingressSlots)
```

Add this worker-only slot-limit test:

```cpp
{
    TqRelayBufferPool pool(64, 2, 128);
    pool.Reserve(2);
    TqBufferAcquireFailure reason = TqBufferAcquireFailure::None;
    auto first = pool.AcquireWorker(&reason);
    auto second = pool.AcquireWorker(&reason);
    auto third = pool.AcquireWorker(&reason);
    assert(first);
    assert(second);
    assert(!third);
    assert(reason == TqBufferAcquireFailure::SlotLimit);
}
```

- [ ] **Step 2: Run failing buffer pool test**

Run:

```bash
rtk cmake --build build --target tcpquic_relay_buffer_pool_test tcpquic_linux_relay_buffer_pool_test -j2
rtk ./build/bin/Release/tcpquic_relay_buffer_pool_test
rtk ./build/bin/Release/tcpquic_linux_relay_buffer_pool_test
```

Expected before implementation: compile fails if tests refer to removed API, or old API remains unused.

- [ ] **Step 3: Remove ingress API**

In `src/tunnel/relay_buffer_pool.h`, remove:

```cpp
enum class TqBufferDomain
Acquire(TqBufferDomain domain, ...)
AcquireIngress(...)
TransferToWorker(...)
Reserve(size_t workerSlots, size_t ingressSlots)
IngressFree
IngressMaxSlots
IngressAllocated
IngressPending
IngressLock
ReleaseIngress(...)
```

Keep:

```cpp
void Reserve(size_t slotCount);
TqBufferRef AcquireWorker(TqBufferAcquireFailure* failure = nullptr);
TqBufferRef Acquire();
```

In `src/tunnel/relay_buffer_pool.cpp`, make all release paths return to the worker free-list.

- [ ] **Step 4: Update Linux worker pool construction**

Replace:

```cpp
Pool(config.ReadChunkSize, config.WorkerSlots + config.IngressSlots, config.MaxPendingBytes)
Pool.Reserve(config.WorkerSlots, config.IngressSlots)
```

with:

```cpp
Pool(config.ReadChunkSize, config.WorkerSlots, config.MaxPendingBytes)
Pool.Reserve(config.WorkerSlots)
```

Keep `IngressSlots` in config temporarily only if CLI compatibility still requires it; do not use it for pool reservation.

- [ ] **Step 5: Run buffer pool and Linux worker tests**

Run:

```bash
rtk cmake --build build --target tcpquic_relay_buffer_pool_test tcpquic_linux_relay_buffer_pool_test tcpquic_linux_relay_worker_io_test -j2
rtk ./build/bin/Release/tcpquic_relay_buffer_pool_test
rtk ./build/bin/Release/tcpquic_linux_relay_buffer_pool_test
rtk ./build/bin/Release/tcpquic_linux_relay_worker_io_test
```

Expected: all exit 0.

- [ ] **Step 6: Commit**

```bash
rtk git add src/tunnel/relay_buffer_pool.h src/tunnel/relay_buffer_pool.cpp src/tunnel/linux_relay_worker.h src/tunnel/linux_relay_worker.cpp src/unittest/relay_buffer_pool_test.cpp src/unittest/linux_relay_buffer_pool_test.cpp
rtk git commit -m "refactor: remove ingress relay buffer domain"
```

## Task 6: Update Linux Metrics

**Files:**
- Modify: `src/runtime/relay_metrics.h`
- Modify: `src/runtime/relay_metrics.cpp`
- Modify: `src/tunnel/linux_relay_worker.h`
- Modify: `src/tunnel/linux_relay_worker.cpp`
- Test: `src/unittest/linux_relay_worker_io_test.cpp`
- Test: `src/unittest/admin_http_test.cpp`

- [ ] **Step 1: Add metrics assertions**

In Linux zstd receive tests, assert:

```cpp
TqLinuxRelayWorkerSnapshot snapshot = worker.Snapshot();
assert(snapshot.DecompressedTcpBytes >= plain.size());
assert(snapshot.ZstdDecompressInputBytes >= compressed.size());
assert(snapshot.ZstdDecompressOutputBytes >= plain.size());
assert(snapshot.ZstdDecompressFailures == 0);
```

In admin metrics tests, assert JSON does not contain:

```cpp
assert(body.find("ingress_buffer") == std::string::npos);
```

and contains:

```cpp
assert(body.find("zstd_decompress_input_bytes") != std::string::npos);
assert(body.find("zstd_decompress_output_bytes") != std::string::npos);
```

- [ ] **Step 2: Run failing metrics tests**

Run:

```bash
rtk cmake --build build --target tcpquic_linux_relay_worker_io_test tcpquic_admin_http_test -j2
rtk ./build/bin/Release/tcpquic_linux_relay_worker_io_test
rtk ./build/bin/Release/tcpquic_admin_http_test
```

Expected before implementation: compile or assertion failure for missing zstd metrics.

- [ ] **Step 3: Add zstd metrics fields**

Add to Linux worker snapshot:

```cpp
uint64_t ZstdDecompressInputBytes{0};
uint64_t ZstdDecompressOutputBytes{0};
uint64_t ZstdDecompressCalls{0};
uint64_t ZstdDecompressNeedInput{0};
uint64_t ZstdDecompressNeedOutput{0};
uint64_t ZstdDecompressFailures{0};
```

Increment these counters inside `DrainCompressedQuicReceiveView()` based on each `TqDecompressResult`.

- [ ] **Step 4: Remove ingress metrics**

Remove fields and JSON output for:

```cpp
QuicReceiveIngressBufferAcquireFailures
QuicReceiveIngressBufferAcquirePendingBudgetFailures
QuicReceiveIngressBufferAcquireSlotLimitFailures
QuicReceiveIngressBufferAcquireAllocFailures
```

Keep worker acquire failure metrics for TCP read, TCP-to-QUIC, and QUIC receive TCP output buffers.

- [ ] **Step 5: Run metrics tests**

Run:

```bash
rtk cmake --build build --target tcpquic_linux_relay_worker_io_test tcpquic_admin_http_test -j2
rtk ./build/bin/Release/tcpquic_linux_relay_worker_io_test
rtk ./build/bin/Release/tcpquic_admin_http_test
```

Expected: all exit 0.

- [ ] **Step 6: Commit**

```bash
rtk git add src/runtime/relay_metrics.h src/runtime/relay_metrics.cpp src/tunnel/linux_relay_worker.h src/tunnel/linux_relay_worker.cpp src/unittest/linux_relay_worker_io_test.cpp src/unittest/admin_http_test.cpp
rtk git commit -m "feat: report linux zstd relay metrics"
```

## Task 7: Linux Verification

**Files:**
- Modify docs only if test results are saved.
- Test: Linux unit and smoke tests.

- [ ] **Step 1: Build Linux targets**

Run:

```bash
rtk cmake --build build --target tcpquic-proxy tcpquic_compress_test tcpquic_config_router_test tcpquic_tuning_test tcpquic_relay_buffer_pool_test tcpquic_linux_relay_buffer_pool_test tcpquic_linux_relay_worker_io_test tcpquic_admin_http_test -j2
```

Expected: build exits 0.

- [ ] **Step 2: Run unit tests**

Run:

```bash
rtk ./build/bin/Release/tcpquic_compress_test
rtk ./build/bin/Release/tcpquic_config_router_test
rtk ./build/bin/Release/tcpquic_tuning_test
rtk ./build/bin/Release/tcpquic_relay_buffer_pool_test
rtk ./build/bin/Release/tcpquic_linux_relay_buffer_pool_test
rtk ./build/bin/Release/tcpquic_linux_relay_worker_io_test
rtk ./build/bin/Release/tcpquic_admin_http_test
rtk git diff --check
```

Expected: all exit 0.

- [ ] **Step 3: Run Linux loopback smoke**

Run:

```bash
rtk scripts/test-tcpquic-proxy.sh
```

Expected: off and zstd smoke paths pass. Any lz4 expectation in the script must be removed before rerun.

- [ ] **Step 4: Run DGX 1x1 validation if hosts are available**

Run the existing DGX built-in speed-test flow for non-compressed upload/download and zstd upload/download. Save outputs under:

```text
docs/dgx-unified-pending-zstd-linux-YYYYMMDD-HHMMSS/
```

Record throughput and admin metrics. Expected:

1. Non-compressed callback pending metrics remain healthy.
2. Zstd decompression metrics increment only in zstd runs.
3. Ingress metrics are absent.
4. Worker slot-limit and pending-budget failures are zero in healthy runs.

- [ ] **Step 5: Commit validation docs if created**

```bash
rtk git add docs/dgx-unified-pending-zstd-linux-*/
rtk git commit -m "docs: record linux unified pending zstd validation"
```

