# Linux Relay Full Replacement Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Make the Linux worker relay the only production relay backend for `tcpquic-proxy`, including compressed tunnels, and move the old blocking relay into a demo/legacy target that is not linked by production binaries.

**Architecture:** Split relay selection from relay implementations. `src/relay.cpp` becomes a thin Linux production wrapper around `TqLinuxRelayRuntime`; `src/relay_blocking_demo.cpp` owns the old `TqTunnelRelay` implementation for legacy demo and comparison tests only. The Linux worker owns TCP reads, QUIC sends, QUIC receive copies, TCP writes, compression, decompression, FIN, shutdown, buffer lifetime, and metrics for every production relay.

**Tech Stack:** C++17, CMake, Linux `epoll`, `eventfd`, `readv`, `writev`, MsQuic stream callbacks, existing compression interfaces, existing assert-style unit tests.

---

## Scope

This plan changes the current state left by `docs/superpowers/plans/2026-06-09-linux-high-performance-threading-and-batching.md`.

Production scope:

- On Linux, `TqRelayStart()` always starts a `TqLinuxRelayWorker` relay.
- Compressed tunnels no longer fall back to `TqTunnelRelay`.
- `tcpquic-proxy`, `tcpquic_tunnel_test`, and router/runtime tests no longer compile or link the old blocking relay implementation.
- The worker supports `TqCompressAlgo::None`, `TqCompressAlgo::Zstd`, and `TqCompressAlgo::Lz4`.
- The worker performs compression/decompression inline on its owner worker for this replacement step. A later plan may offload compression CPU work to a dedicated pool.
- Old blocking relay code remains available only as a demo/legacy comparison implementation and must be visibly named as such.

Out of scope:

- Windows relay replacement.
- IOCP, WSAPoll, WSARecv, or WSASend.
- MsQuic deferred receive completion.
- Compression offload pools.
- Deleting `TqTcpWriteQueue`; it can stay for its own unit test and for legacy/demo code, but production relay must not use it.

## Current State Summary

- `src/relay.cpp` contains both `TqTunnelRelay` and `TqRelayStart()`.
- `TqRelayStart()` currently chooses `TqLinuxRelayWorker` only when `compressor == nullptr && decompressor == nullptr`.
- Compressed production relays still instantiate `TqTunnelRelay`.
- `TqRelayHandle` contains both `Relay` and `LinuxWorker` fields.
- `linux_relay_worker.cpp` already owns epoll/readv/writev for uncompressed relays and has test hooks for read/write behavior.

## Target File Structure

- Modify `src/relay.h`: remove `TqTunnelRelay` production pointer from `TqRelayHandle`; keep backend tag only for `None` and `LinuxWorker` in production.
- Rewrite `src/relay.cpp`: keep only production `TqRelayStart()`, `TqRelayStop()`, and `TqRelayLinuxFastPathEnabled()` logic for Linux worker relays.
- Create `src/relay_blocking_demo.h`: legacy demo-only API for the old blocking relay.
- Create `src/relay_blocking_demo.cpp`: move the current `TqTunnelRelay` implementation here and rename it to `TqBlockingDemoRelay`.
- Modify `src/linux_relay_worker.h`: add compression fields to `TqLinuxRelayRegistration`, test hooks for compressed paths, and worker counters.
- Modify `src/linux_relay_worker.cpp`: implement compressed TCP-to-QUIC and QUIC-to-TCP in the worker.
- Modify `src/server_metrics.h` and `src/server_metrics.cpp`: expose compressed worker bytes and errors if the worker adds new counters.
- Modify `src/CMakeLists.txt`: remove blocking relay sources from production targets; add a demo/legacy target for blocking relay comparison.
- Modify `src/unittest/linux_relay_worker_io_test.cpp`: add compressed worker tests.
- Modify `src/unittest/tcp_tunnel_test.cpp`: assert compressed relay handles use `LinuxWorker`.
- Create `src/unittest/relay_backend_selection_test.cpp`: focused tests that production relay selection never picks blocking relay on Linux.

## Task 1: Lock Production Backend Selection

**Files:**
- Modify: `src/relay.h`
- Modify: `src/relay.cpp`
- Create: `src/unittest/relay_backend_selection_test.cpp`
- Modify: `src/CMakeLists.txt`

- [ ] **Step 1: Write the failing backend selection test**

Create `src/unittest/relay_backend_selection_test.cpp`:

```cpp
#include "../relay.h"
#include "../tuning.h"

#include <cassert>

int main() {
#if defined(__linux__)
    TqRelayHandle handle{};
    assert(handle.Backend == TqRelayBackendType::None);
    assert(handle.LinuxWorker == nullptr);
    assert(handle.LinuxRelayId == 0);

    static_assert(static_cast<int>(TqRelayBackendType::None) == 0, "None backend must stay stable");
    static_assert(static_cast<int>(TqRelayBackendType::LinuxWorker) == 1, "Linux worker is the only production backend");

    assert(TqRelayLinuxFastPathEnabled(&handle) == false);
#endif
    return 0;
}
```

Add this target near the other relay tests in `src/CMakeLists.txt`:

```cmake
add_executable(tcpquic_relay_backend_selection_test
    unittest/relay_backend_selection_test.cpp
    relay.cpp
    tuning.cpp
    linux_relay_worker.cpp
    linux_relay_buffer_pool.cpp
)
target_include_directories(tcpquic_relay_backend_selection_test PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${MSQUIC_SOURCE_DIR}/src/inc)
target_link_libraries(tcpquic_relay_backend_selection_test Threads::Threads inc warnings msquic logging base_link)
set_property(TARGET tcpquic_relay_backend_selection_test PROPERTY FOLDER "tools")
set_property(TARGET tcpquic_relay_backend_selection_test APPEND PROPERTY BUILD_RPATH "$ORIGIN")
```

Append the target to `TCPQUIC_TEST_TARGETS`.

- [ ] **Step 2: Run the test to verify it fails**

Run:

```bash
rtk cmake --build build-gcc10-fixed --target tcpquic_relay_backend_selection_test
```

Expected: FAIL because `TqRelayBackendType::Blocking` still exists and shifts `LinuxWorker` to value `2`, and because `TqRelayHandle` still contains `Relay`.

- [ ] **Step 3: Change production relay handle shape**

In `src/relay.h`, replace the backend enum and handle with:

```cpp
class TqLinuxRelayWorker;

enum class TqRelayBackendType {
    None,
    LinuxWorker,
};

struct TqRelayHandle {
    std::atomic<bool> Stop{false};
    TqRelayBackendType Backend{TqRelayBackendType::None};
    TqLinuxRelayWorker* LinuxWorker{nullptr};
    uint64_t LinuxRelayId{0};
};
```

Remove the `class TqTunnelRelay;` forward declaration.

- [ ] **Step 4: Rewrite `TqRelayStart()` selection**

In `src/relay.cpp`, replace the production branch at the end of the file with this Linux-only selection logic:

```cpp
bool TqRelayStart(
    int tcpFd,
    MsQuicStream* stream,
    ITqCompressor* compressor,
    ITqDecompressor* decompressor,
    TqRelayHandle* handle,
    const TqTuningConfig& profileTuning,
    TqCompressAlgo compressAlgo) {
    if (handle == nullptr || handle->Backend != TqRelayBackendType::None) {
        return false;
    }

#if !defined(__linux__)
    (void)tcpFd;
    (void)stream;
    (void)compressor;
    (void)decompressor;
    (void)profileTuning;
    (void)compressAlgo;
    return false;
#else
    const uint32_t activeRelays = TqRelayRegisterActive();
    TqTuningConfig tuning = profileTuning;
    TqApplyRelayPoolBudget(tuning, activeRelays);

    if (!TqLinuxRelayRuntime::Instance().Start(tuning)) {
        TqRelayUnregisterActive();
        return false;
    }

    TqLinuxRelayWorker* worker = TqLinuxRelayRuntime::Instance().PickWorker();
    if (worker == nullptr) {
        TqRelayUnregisterActive();
        return false;
    }

    TqLinuxRelayRegistration registration{};
    registration.TcpFd = tcpFd;
    registration.Stream = stream;
    registration.Handle = handle;
    registration.Compressor = compressor;
    registration.Decompressor = decompressor;
    registration.CompressAlgo = compressAlgo;
    registration.EnableQuicSends = true;

    const auto registered = worker->RegisterRelayWithId(registration);
    if (!registered.Ok) {
        TqRelayUnregisterActive();
        return false;
    }

    handle->Stop.store(false);
    handle->Backend = TqRelayBackendType::LinuxWorker;
    handle->LinuxWorker = worker;
    handle->LinuxRelayId = registered.RelayId;
    return true;
#endif
}
```

- [ ] **Step 5: Remove blocking branch from `TqRelayStop()`**

Keep only the Linux worker unregister path:

```cpp
void TqRelayStop(TqRelayHandle* handle) {
    if (handle == nullptr) {
        return;
    }

#if defined(__linux__)
    if (handle->Backend == TqRelayBackendType::LinuxWorker) {
        TqLinuxRelayWorker* worker = handle->LinuxWorker;
        const uint64_t relayId = handle->LinuxRelayId;
        handle->Backend = TqRelayBackendType::None;
        handle->LinuxWorker = nullptr;
        handle->LinuxRelayId = 0;
        handle->Stop.store(true);
        if (worker != nullptr && relayId != 0) {
            worker->UnregisterRelay(relayId);
        }
        TqRelayUnregisterActive();
        return;
    }
#endif

    handle->Stop.store(true);
}
```

- [ ] **Step 6: Run the backend selection test**

Run:

```bash
rtk cmake --build build-gcc10-fixed --target tcpquic_relay_backend_selection_test
rtk ./build-gcc10-fixed/bin/Release/tcpquic_relay_backend_selection_test
```

Expected: PASS.

- [ ] **Step 7: Commit**

Run:

```bash
rtk git add src/relay.h src/relay.cpp src/unittest/relay_backend_selection_test.cpp src/CMakeLists.txt
rtk git commit -m "refactor: require linux worker relay backend in production"
```

## Task 2: Move Old Blocking Relay To Demo-Only Code

**Files:**
- Create: `src/relay_blocking_demo.h`
- Create: `src/relay_blocking_demo.cpp`
- Modify: `src/relay.cpp`
- Modify: `src/CMakeLists.txt`

- [ ] **Step 1: Create the demo API**

Create `src/relay_blocking_demo.h`:

```cpp
#pragma once

#include "compress.h"
#include "tuning.h"

struct MsQuicStream;

struct TqBlockingDemoRelayHandle;

TqBlockingDemoRelayHandle* TqBlockingDemoRelayStart(
    int tcpFd,
    MsQuicStream* stream,
    ITqCompressor* compressor,
    ITqDecompressor* decompressor,
    const TqTuningConfig& tuning,
    TqCompressAlgo compressAlgo);

void TqBlockingDemoRelayStop(TqBlockingDemoRelayHandle* handle);
```

- [ ] **Step 2: Move and rename the old implementation**

Create `src/relay_blocking_demo.cpp` by moving the current old `TqTunnelRelay` class and `TqRelaySendContext` helper out of `src/relay.cpp`.

Make these exact renames while moving:

```cpp
class TqTunnelRelay final
```

becomes:

```cpp
class TqBlockingDemoRelay final
```

The moved file must expose only:

```cpp
struct TqBlockingDemoRelayHandle {
    std::atomic<bool> Stop{false};
    TqBlockingDemoRelay* Relay{nullptr};
};
```

and:

```cpp
TqBlockingDemoRelayHandle* TqBlockingDemoRelayStart(
    int tcpFd,
    MsQuicStream* stream,
    ITqCompressor* compressor,
    ITqDecompressor* decompressor,
    const TqTuningConfig& tuning,
    TqCompressAlgo compressAlgo) {
    auto* handle = new (std::nothrow) TqBlockingDemoRelayHandle{};
    if (handle == nullptr) {
        return nullptr;
    }
    auto* relay = new (std::nothrow) TqBlockingDemoRelay(
        tcpFd, stream, compressor, decompressor, handle, tuning, compressAlgo);
    if (relay == nullptr) {
        delete handle;
        return nullptr;
    }
    if (!relay->Start()) {
        delete relay;
        delete handle;
        return nullptr;
    }
    handle->Relay = relay;
    return handle;
}

void TqBlockingDemoRelayStop(TqBlockingDemoRelayHandle* handle) {
    if (handle == nullptr) {
        return;
    }
    auto* relay = handle->Relay;
    handle->Relay = nullptr;
    if (relay != nullptr) {
        relay->Stop();
        delete relay;
    } else {
        handle->Stop.store(true);
    }
    delete handle;
}
```

- [ ] **Step 3: Remove old relay implementation from production `src/relay.cpp`**

After the move, `src/relay.cpp` must not contain these strings:

```text
class TqTunnelRelay
TqTcpWriteQueue
TcpToStreamLoop
TcpThread
```

Keep `TqRelayRegisterActive()`, `TqRelayUnregisterActive()`, and `TqApplyRelayPoolBudget()` if they are still used by production relay selection.

- [ ] **Step 4: Add demo target only**

In `src/CMakeLists.txt`, do not add `relay_blocking_demo.cpp` to `TCPQUIC_PROXY_SOURCES` or `TCPQUIC_TUNNEL_TEST_SOURCES`.

Add an explicit demo target:

```cmake
add_executable(tcpquic_blocking_relay_demo_test
    unittest/blocking_relay_demo_test.cpp
    relay_blocking_demo.cpp
    tcp_write_queue.cpp
    tuning.cpp
)
target_include_directories(tcpquic_blocking_relay_demo_test PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${MSQUIC_SOURCE_DIR}/src/inc)
target_link_libraries(tcpquic_blocking_relay_demo_test Threads::Threads inc warnings msquic logging base_link)
set_property(TARGET tcpquic_blocking_relay_demo_test PROPERTY FOLDER "tools")
set_property(TARGET tcpquic_blocking_relay_demo_test APPEND PROPERTY BUILD_RPATH "$ORIGIN")
```

If no demo unit test is needed yet, create `src/unittest/blocking_relay_demo_test.cpp`:

```cpp
#include "../relay_blocking_demo.h"

int main() {
    return 0;
}
```

- [ ] **Step 5: Verify production source isolation**

Run:

```bash
rtk rg "TqTunnelRelay|TqTcpWriteQueue|TcpToStreamLoop|TcpThread" src/relay.cpp src/linux_relay_worker.cpp src/tcp_tunnel.cpp
```

Expected: no matches.

Run:

```bash
rtk cmake --build build-gcc10-fixed --target tcpquic-proxy tcpquic_tunnel_test tcpquic_blocking_relay_demo_test
```

Expected: PASS.

- [ ] **Step 6: Commit**

Run:

```bash
rtk git add src/relay.cpp src/relay_blocking_demo.h src/relay_blocking_demo.cpp src/unittest/blocking_relay_demo_test.cpp src/CMakeLists.txt
rtk git commit -m "refactor: isolate blocking relay as demo code"
```

## Task 3: Add Compression Ownership To Worker Registration

**Files:**
- Modify: `src/linux_relay_worker.h`
- Modify: `src/linux_relay_worker.cpp`
- Modify: `src/unittest/linux_relay_worker_io_test.cpp`

- [ ] **Step 1: Write the failing compressed registration test**

Append this block before `return 0;` in `src/unittest/linux_relay_worker_io_test.cpp`:

```cpp
    {
        TqLinuxRelayWorkerConfig config{};
        config.EventBudget = 128;
        config.ReadChunkSize = 1024;
        config.ReadBatchBytes = 4096;
        config.MaxIov = 4;
        config.MaxPendingBytes = 64 * 1024;

        TqLinuxRelayWorker worker(config);
        assert(worker.Start());

        int fds[2]{-1, -1};
        assert(::socketpair(AF_UNIX, SOCK_STREAM, 0, fds) == 0);

        TqLinuxRelayRegistration registration{};
        registration.TcpFd = fds[0];
        registration.Stream = nullptr;
        registration.Handle = nullptr;
        registration.CompressAlgo = TqCompressAlgo::Zstd;
        registration.EnableQuicSends = false;
        assert(worker.RegisterRelayForTest(registration));

        const char payload[] = "compressed-registration-still-reads";
        assert(::write(fds[1], payload, sizeof(payload)) == static_cast<ssize_t>(sizeof(payload)));
        assert(worker.WaitForObservedTcpBytesForTest(sizeof(payload), 2000));

        worker.Stop();
        ::close(fds[1]);
    }
```

- [ ] **Step 2: Run the test to verify it fails**

Run:

```bash
rtk cmake --build build-gcc10-fixed --target tcpquic_linux_relay_worker_io_test
```

Expected: FAIL because `TqLinuxRelayRegistration` has no `CompressAlgo` member.

- [ ] **Step 3: Extend worker registration**

In `src/linux_relay_worker.h`, include compression and add fields:

```cpp
#include "compress.h"
```

Update `TqLinuxRelayRegistration`:

```cpp
struct TqLinuxRelayRegistration {
    int TcpFd{-1};
    MsQuicStream* Stream{nullptr};
    TqRelayHandle* Handle{nullptr};
    ITqCompressor* Compressor{nullptr};
    ITqDecompressor* Decompressor{nullptr};
    TqCompressAlgo CompressAlgo{TqCompressAlgo::None};
    bool EnableQuicSends{true};
};
```

Update `RelayState` in `src/linux_relay_worker.cpp`:

```cpp
    ITqCompressor* Compressor{nullptr};
    ITqDecompressor* Decompressor{nullptr};
    TqCompressAlgo CompressAlgo{TqCompressAlgo::None};
    std::vector<uint8_t> CompressionOutput;
    std::vector<uint8_t> DecompressionOutput;
```

and initialize them:

```cpp
          Compressor(registration.Compressor),
          Decompressor(registration.Decompressor),
          CompressAlgo(registration.CompressAlgo),
```

- [ ] **Step 4: Run the test**

Run:

```bash
rtk cmake --build build-gcc10-fixed --target tcpquic_linux_relay_worker_io_test
rtk ./build-gcc10-fixed/bin/Release/tcpquic_linux_relay_worker_io_test
```

Expected: PASS.

- [ ] **Step 5: Commit**

Run:

```bash
rtk git add src/linux_relay_worker.h src/linux_relay_worker.cpp src/unittest/linux_relay_worker_io_test.cpp
rtk git commit -m "feat: register compression state with linux relay worker"
```

## Task 4: Worker TCP-To-QUIC Compression

**Files:**
- Modify: `src/linux_relay_worker.h`
- Modify: `src/linux_relay_worker.cpp`
- Modify: `src/unittest/linux_relay_worker_io_test.cpp`
- Modify: `src/CMakeLists.txt`

- [ ] **Step 1: Add a fake stream send capture test**

Append a test helper to `src/unittest/linux_relay_worker_io_test.cpp`:

```cpp
struct TqCapturedSend {
    std::vector<uint8_t> Bytes;
    bool Fin{false};
};

struct TqFakeStreamContext {
    std::vector<TqCapturedSend> Sends;
};

static QUIC_STATUS QUIC_API TqCaptureSend(
    _In_ MsQuicStream*,
    _In_reads_(bufferCount) const QUIC_BUFFER* buffers,
    _In_ uint32_t bufferCount,
    _In_ QUIC_SEND_FLAGS flags,
    _In_opt_ void* clientContext) noexcept {
    auto* capture = static_cast<TqFakeStreamContext*>(clientContext);
    if (capture == nullptr) {
        return QUIC_STATUS_INVALID_PARAMETER;
    }
    TqCapturedSend send{};
    send.Fin = (flags & QUIC_SEND_FLAG_FIN) != 0;
    for (uint32_t i = 0; i < bufferCount; ++i) {
        send.Bytes.insert(send.Bytes.end(), buffers[i].Buffer, buffers[i].Buffer + buffers[i].Length);
    }
    capture->Sends.push_back(std::move(send));
    return QUIC_STATUS_SUCCESS;
}
```

If `MsQuicStream` does not allow replacing `Send`, add a worker test hook instead:

```cpp
bool TqLinuxRelayWorker::RegisterRelayForTest(const TqLinuxRelayRegistration& registration);
std::vector<uint8_t> TqLinuxRelayWorker::TakeCapturedQuicBytesForTest(int tcpFd);
```

The test must assert that compressed worker output can be decompressed:

```cpp
    {
        TqLinuxRelayWorkerConfig config{};
        config.EventBudget = 128;
        config.ReadChunkSize = 1024;
        config.ReadBatchBytes = 4096;
        config.MaxIov = 4;
        config.MaxPendingBytes = 64 * 1024;

        TqLinuxRelayWorker worker(config);
        assert(worker.Start());

        int fds[2]{-1, -1};
        assert(::socketpair(AF_UNIX, SOCK_STREAM, 0, fds) == 0);

        auto compressor = TqCreateCompressor(TqCompressAlgo::Zstd, 1);
        auto decompressor = TqCreateDecompressor(TqCompressAlgo::Zstd);
        assert(compressor);
        assert(decompressor);

        TqLinuxRelayRegistration registration{};
        registration.TcpFd = fds[0];
        registration.Stream = nullptr;
        registration.Handle = nullptr;
        registration.Compressor = compressor.get();
        registration.CompressAlgo = TqCompressAlgo::Zstd;
        registration.EnableQuicSends = false;
        assert(worker.RegisterRelayForTest(registration));

        std::vector<uint8_t> payload(4096, 0x42);
        assert(::write(fds[1], payload.data(), payload.size()) == static_cast<ssize_t>(payload.size()));
        assert(worker.WaitForObservedTcpBytesForTest(payload.size(), 2000));

        const std::vector<uint8_t> compressed = worker.TakeCapturedQuicBytesForTest(fds[0]);
        assert(!compressed.empty());

        std::vector<uint8_t> restored;
        assert(decompressor->Decompress(compressed.data(), compressed.size(), restored));
        assert(restored == payload);

        worker.Stop();
        ::close(fds[1]);
    }
```

- [ ] **Step 2: Run the test to verify it fails**

Run:

```bash
rtk cmake --build build-gcc10-fixed --target tcpquic_linux_relay_worker_io_test
```

Expected: FAIL because `TakeCapturedQuicBytesForTest` does not exist and compression is not applied.

- [ ] **Step 3: Add test capture storage**

In `RelayState`, add:

```cpp
    std::vector<uint8_t> CapturedQuicBytesForTest;
```

Declare in `src/linux_relay_worker.h`:

```cpp
    std::vector<uint8_t> TakeCapturedQuicBytesForTest(int tcpFd);
```

Implement:

```cpp
std::vector<uint8_t> TqLinuxRelayWorker::TakeCapturedQuicBytesForTest(int tcpFd) {
    RelayState* relay = FindRelayByFd(tcpFd);
    if (relay == nullptr) {
        return {};
    }
    std::vector<uint8_t> out;
    out.swap(relay->CapturedQuicBytesForTest);
    return out;
}
```

- [ ] **Step 4: Compress TCP read views before QUIC send**

Add helper in `src/linux_relay_worker.cpp`:

```cpp
bool TqLinuxRelayWorker::BuildTcpToQuicViews(
    RelayState* relay,
    std::vector<TqBufferView>& input,
    std::vector<TqBufferView>& output) {
    output.clear();
    if (relay == nullptr) {
        return false;
    }
    if (relay->Compressor == nullptr || relay->CompressAlgo == TqCompressAlgo::None) {
        output = std::move(input);
        return true;
    }

    relay->CompressionOutput.clear();
    for (const auto& view : input) {
        if (!relay->Compressor->Compress(view.Data, view.Len, relay->CompressionOutput, false)) {
            return false;
        }
    }
    if (relay->CompressionOutput.empty() &&
        !relay->Compressor->Flush(relay->CompressionOutput)) {
        return false;
    }
    if (relay->CompressionOutput.empty()) {
        input.clear();
        return true;
    }

    auto buffer = relay->Pool.Acquire();
    if (!buffer || relay->CompressionOutput.size() > buffer->Capacity()) {
        return false;
    }
    std::memcpy(buffer->Data(), relay->CompressionOutput.data(), relay->CompressionOutput.size());
    buffer->SetLength(relay->CompressionOutput.size());
    output.push_back(TqBufferView{buffer->Data(), relay->CompressionOutput.size(), buffer});
    input.clear();
    return true;
}
```

In `DrainTcpReadable()`, replace:

```cpp
if (!SubmitTcpBatchToQuic(relay, views)) {
```

with:

```cpp
std::vector<TqBufferView> sendViews;
if (!BuildTcpToQuicViews(relay, views, sendViews) ||
    !SubmitTcpBatchToQuic(relay, sendViews)) {
```

In `SubmitTcpBatchToQuic()`, when sends are disabled for tests, capture bytes before clearing:

```cpp
    if (!relay->EnableQuicSends || relay->Stream == nullptr) {
        for (const auto& view : views) {
            relay->CapturedQuicBytesForTest.insert(
                relay->CapturedQuicBytesForTest.end(),
                view.Data,
                view.Data + view.Len);
        }
        views.clear();
        return true;
    }
```

- [ ] **Step 5: Link compression libs into worker IO test**

In `src/CMakeLists.txt`, update `tcpquic_linux_relay_worker_io_test`:

```cmake
target_link_libraries(tcpquic_linux_relay_worker_io_test Threads::Threads ${TCPQUIC_TEST_LIBS})
target_compile_definitions(tcpquic_linux_relay_worker_io_test PRIVATE ${TCPQUIC_TEST_DEFS})
```

Add `compress.cpp` to that target if the test calls `TqCreateCompressor()`:

```cmake
add_executable(tcpquic_linux_relay_worker_io_test
    unittest/linux_relay_worker_io_test.cpp
    linux_relay_worker.cpp
    linux_relay_buffer_pool.cpp
    compress.cpp
)
```

- [ ] **Step 6: Run the test**

Run:

```bash
rtk cmake --build build-gcc10-fixed --target tcpquic_linux_relay_worker_io_test
rtk ./build-gcc10-fixed/bin/Release/tcpquic_linux_relay_worker_io_test
```

Expected: PASS.

- [ ] **Step 7: Commit**

Run:

```bash
rtk git add src/linux_relay_worker.h src/linux_relay_worker.cpp src/unittest/linux_relay_worker_io_test.cpp src/CMakeLists.txt
rtk git commit -m "feat: compress tcp reads in linux relay worker"
```

## Task 5: Worker QUIC-To-TCP Decompression

**Files:**
- Modify: `src/linux_relay_worker.cpp`
- Modify: `src/unittest/linux_relay_worker_io_test.cpp`

- [ ] **Step 1: Write the failing decompression test**

Append this block before `return 0;` in `src/unittest/linux_relay_worker_io_test.cpp`:

```cpp
    {
        TqLinuxRelayWorkerConfig config{};
        config.EventBudget = 128;
        config.ReadChunkSize = 1024;
        config.ReadBatchBytes = 4096;
        config.MaxIov = 4;
        config.MaxPendingBytes = 64 * 1024;

        TqLinuxRelayWorker worker(config);
        assert(worker.Start());

        int fds[2]{-1, -1};
        assert(::socketpair(AF_UNIX, SOCK_STREAM, 0, fds) == 0);

        auto compressor = TqCreateCompressor(TqCompressAlgo::Lz4, 1);
        auto decompressor = TqCreateDecompressor(TqCompressAlgo::Lz4);
        assert(compressor);
        assert(decompressor);

        TqLinuxRelayRegistration registration{};
        registration.TcpFd = fds[0];
        registration.Stream = nullptr;
        registration.Handle = nullptr;
        registration.Decompressor = decompressor.get();
        registration.CompressAlgo = TqCompressAlgo::Lz4;
        registration.EnableQuicSends = false;
        assert(worker.RegisterRelayForTest(registration));

        const std::vector<uint8_t> plain(2048, 0x7C);
        std::vector<uint8_t> compressed;
        assert(compressor->Compress(plain.data(), plain.size(), compressed, false));
        assert(compressor->Flush(compressed));
        assert(!compressed.empty());

        assert(worker.EnqueueQuicReceiveForTest(fds[0], compressed.data(), compressed.size(), true));

        std::vector<uint8_t> output(plain.size());
        size_t offset = 0;
        while (offset < output.size()) {
            const ssize_t received = ::read(fds[1], output.data() + offset, output.size() - offset);
            assert(received > 0);
            offset += static_cast<size_t>(received);
        }
        assert(output == plain);

        worker.Stop();
        ::close(fds[1]);
    }
```

- [ ] **Step 2: Run the test to verify it fails**

Run:

```bash
rtk cmake --build build-gcc10-fixed --target tcpquic_linux_relay_worker_io_test
rtk ./build-gcc10-fixed/bin/Release/tcpquic_linux_relay_worker_io_test
```

Expected: FAIL because QUIC receive bytes are written directly to TCP without decompression.

- [ ] **Step 3: Decompress QUIC receive before queueing TCP writes**

In `EnqueueQuicReceive()`, replace direct copy logic with:

```cpp
    const uint8_t* writeData = data;
    size_t writeLength = length;
    if (relay->Decompressor != nullptr && relay->CompressAlgo != TqCompressAlgo::None) {
        relay->DecompressionOutput.clear();
        if (!relay->Decompressor->Decompress(data, length, relay->DecompressionOutput)) {
            return false;
        }
        writeData = relay->DecompressionOutput.data();
        writeLength = relay->DecompressionOutput.size();
    }

    size_t offset = 0;
    while (offset < writeLength) {
        auto buffer = relay->Pool.Acquire();
        if (!buffer) {
            return false;
        }
        const size_t chunk = std::min(buffer->Capacity(), writeLength - offset);
        std::memcpy(buffer->Data(), writeData + offset, chunk);
        buffer->SetLength(chunk);
        relay->PendingTcpWrites.push_back(TqBufferView{buffer->Data(), chunk, buffer});
        offset += chunk;
    }
```

Keep the existing FIN handling:

```cpp
    if (fin) {
        relay->TcpWriteShutdownQueued = true;
    }
```

- [ ] **Step 4: Run the test**

Run:

```bash
rtk cmake --build build-gcc10-fixed --target tcpquic_linux_relay_worker_io_test
rtk ./build-gcc10-fixed/bin/Release/tcpquic_linux_relay_worker_io_test
```

Expected: PASS.

- [ ] **Step 5: Commit**

Run:

```bash
rtk git add src/linux_relay_worker.cpp src/unittest/linux_relay_worker_io_test.cpp
rtk git commit -m "feat: decompress quic receives in linux relay worker"
```

## Task 6: Production Tunnel Tests Must Use Worker For Compressed Relays

**Files:**
- Modify: `src/unittest/tcp_tunnel_test.cpp`
- Modify: `src/tcp_tunnel.cpp`
- Modify: `src/CMakeLists.txt`

- [ ] **Step 1: Write the failing production assertion**

In `src/unittest/tcp_tunnel_test.cpp`, add a test near the existing relay handle assertions:

```cpp
    {
        TqRelayHandle handle{};
        assert(!TqRelayLinuxFastPathEnabled(&handle));
        handle.Backend = TqRelayBackendType::LinuxWorker;
        assert(TqRelayLinuxFastPathEnabled(&handle));
        handle.Backend = TqRelayBackendType::None;
    }
```

Also replace any checks for `TqRelayBackendType::Blocking` with Linux worker expectations:

```cpp
if (handle.Backend != TqRelayBackendType::LinuxWorker) return 10;
```

- [ ] **Step 2: Run the test to verify it fails if old backend references remain**

Run:

```bash
rtk cmake --build build-gcc10-fixed --target tcpquic_tunnel_test
```

Expected: FAIL if any test or production code still references `TqRelayBackendType::Blocking` or `handle.Relay`.

- [ ] **Step 3: Remove production references to blocking handle state**

In `src/tcp_tunnel.cpp`, keep relay lifecycle calls unchanged:

```cpp
TqRelayStart(...);
TqRelayStop(&ctx->RelayHandle);
```

Do not inspect or branch on `handle.Relay`.

If `TqTunnelRelayStopped()` reads old state, change it to:

```cpp
bool TqTunnelRelayStopped(const TqTunnelContext* ctx) {
    if (ctx == nullptr) {
        return true;
    }
    return ctx->RelayHandle.Stop.load();
}
```

- [ ] **Step 4: Run the tunnel test**

Run:

```bash
rtk cmake --build build-gcc10-fixed --target tcpquic_tunnel_test
rtk ./build-gcc10-fixed/bin/Release/tcpquic_tunnel_test
```

Expected: PASS.

- [ ] **Step 5: Commit**

Run:

```bash
rtk git add src/unittest/tcp_tunnel_test.cpp src/tcp_tunnel.cpp src/CMakeLists.txt
rtk git commit -m "test: require linux worker relay in tunnel production path"
```

## Task 7: CMake Production Linkage Guard

**Files:**
- Modify: `src/CMakeLists.txt`
- Create: `src/unittest/production_linkage_guard_test.cpp`

- [ ] **Step 1: Add a source-level guard test**

Create `src/unittest/production_linkage_guard_test.cpp`:

```cpp
#include <fstream>
#include <sstream>
#include <string>

int main() {
    std::ifstream cmake("src/CMakeLists.txt");
    if (!cmake) return 1;
    std::ostringstream buffer;
    buffer << cmake.rdbuf();
    const std::string text = buffer.str();

    const size_t proxySources = text.find("set(TCPQUIC_PROXY_SOURCES");
    const size_t proxyTarget = text.find("add_tcpquic_executable(tcpquic-proxy");
    if (proxySources == std::string::npos || proxyTarget == std::string::npos) return 2;

    const std::string productionBlock = text.substr(proxySources, proxyTarget - proxySources);
    if (productionBlock.find("relay_blocking_demo.cpp") != std::string::npos) return 3;
    if (productionBlock.find("tcp_write_queue.cpp") != std::string::npos) return 4;
    if (productionBlock.find("linux_relay_worker.cpp") == std::string::npos) return 5;
    if (productionBlock.find("linux_relay_buffer_pool.cpp") == std::string::npos) return 6;

    return 0;
}
```

Add target:

```cmake
add_executable(tcpquic_production_linkage_guard_test
    unittest/production_linkage_guard_test.cpp
)
set_property(TARGET tcpquic_production_linkage_guard_test PROPERTY FOLDER "tools")
set_property(TARGET tcpquic_production_linkage_guard_test APPEND PROPERTY BUILD_RPATH "$ORIGIN")
```

Append it to `TCPQUIC_TEST_TARGETS`.

- [ ] **Step 2: Run the guard to verify it fails if old production linkage remains**

Run:

```bash
rtk cmake --build build-gcc10-fixed --target tcpquic_production_linkage_guard_test
rtk ./build-gcc10-fixed/bin/Release/tcpquic_production_linkage_guard_test
```

Expected: FAIL until `tcp_write_queue.cpp` and `relay_blocking_demo.cpp` are absent from `TCPQUIC_PROXY_SOURCES`.

- [ ] **Step 3: Remove old relay support sources from production target**

In `src/CMakeLists.txt`, remove this from `TCPQUIC_PROXY_SOURCES` if it is not needed elsewhere by production:

```cmake
    tcp_write_queue.cpp
```

Keep these Linux sources in production:

```cmake
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    list(APPEND TCPQUIC_PROXY_SOURCES
        linux_relay_buffer_pool.cpp
        linux_relay_worker.cpp
    )
endif()
```

Ensure `relay_blocking_demo.cpp` appears only in `tcpquic_blocking_relay_demo_test`.

- [ ] **Step 4: Run linkage guard and production build**

Run:

```bash
rtk cmake --build build-gcc10-fixed --target tcpquic_production_linkage_guard_test tcpquic-proxy
rtk ./build-gcc10-fixed/bin/Release/tcpquic_production_linkage_guard_test
```

Expected: PASS.

- [ ] **Step 5: Commit**

Run:

```bash
rtk git add src/CMakeLists.txt src/unittest/production_linkage_guard_test.cpp
rtk git commit -m "build: keep blocking relay out of production linkage"
```

## Task 8: Metrics And Runtime Observability

**Files:**
- Modify: `src/linux_relay_worker.h`
- Modify: `src/linux_relay_worker.cpp`
- Modify: `src/server_metrics.h`
- Modify: `src/server_metrics.cpp`
- Modify: `src/unittest/router_runtime_test.cpp`

- [ ] **Step 1: Write failing metrics assertions**

In `src/unittest/router_runtime_test.cpp`, add assertions that the JSON includes worker replacement fields:

```cpp
        assert(metricsJson.find("\"linux_relay_backend\":\"worker\"") != std::string::npos);
        assert(metricsJson.find("\"linux_relay_compressed_tcp_bytes\":") != std::string::npos);
        assert(metricsJson.find("\"linux_relay_decompressed_tcp_bytes\":") != std::string::npos);
        assert(metricsJson.find("\"linux_relay_errors\":") != std::string::npos);
```

- [ ] **Step 2: Run the test to verify it fails**

Run:

```bash
rtk cmake --build build-gcc10-fixed --target tcpquic_router_runtime_test
rtk ./build-gcc10-fixed/bin/Release/tcpquic_router_runtime_test
```

Expected: FAIL because the JSON fields are missing.

- [ ] **Step 3: Add worker counters**

In `TqLinuxRelayWorkerSnapshot`, add:

```cpp
    uint64_t CompressedTcpBytes{0};
    uint64_t DecompressedTcpBytes{0};
    uint64_t Errors{0};
```

In `TqLinuxRelayWorker`, add atomics:

```cpp
    std::atomic<uint64_t> CompressedTcpBytes{0};
    std::atomic<uint64_t> DecompressedTcpBytes{0};
    std::atomic<uint64_t> Errors{0};
```

Increment:

```cpp
CompressedTcpBytes.fetch_add(relay->CompressionOutput.size());
DecompressedTcpBytes.fetch_add(relay->DecompressionOutput.size());
Errors.fetch_add(1);
```

Use `Errors.fetch_add(1)` only at actual failure points: compression failure, decompression failure, send submission failure, pool acquisition failure during active relay processing.

- [ ] **Step 4: Serialize metrics**

In `src/server_metrics.cpp`, append:

```cpp
    out << ",\"linux_relay_backend\":\"worker\"";
    out << ",\"linux_relay_compressed_tcp_bytes\":" << linuxRelayCompressedTcpBytes;
    out << ",\"linux_relay_decompressed_tcp_bytes\":" << linuxRelayDecompressedTcpBytes;
    out << ",\"linux_relay_errors\":" << linuxRelayErrors;
```

For non-Linux builds, serialize zeros and `"unsupported"`:

```cpp
    const char* linuxRelayBackend = "unsupported";
    const uint64_t linuxRelayCompressedTcpBytes = 0;
    const uint64_t linuxRelayDecompressedTcpBytes = 0;
    const uint64_t linuxRelayErrors = 0;
```

For Linux builds, serialize:

```cpp
    const char* linuxRelayBackend = "worker";
    const uint64_t linuxRelayCompressedTcpBytes = linuxRelay.CompressedTcpBytes;
    const uint64_t linuxRelayDecompressedTcpBytes = linuxRelay.DecompressedTcpBytes;
    const uint64_t linuxRelayErrors = linuxRelay.Errors;
```

- [ ] **Step 5: Run metrics test**

Run:

```bash
rtk cmake --build build-gcc10-fixed --target tcpquic_router_runtime_test
rtk ./build-gcc10-fixed/bin/Release/tcpquic_router_runtime_test
```

Expected: PASS.

- [ ] **Step 6: Commit**

Run:

```bash
rtk git add src/linux_relay_worker.h src/linux_relay_worker.cpp src/server_metrics.h src/server_metrics.cpp src/unittest/router_runtime_test.cpp
rtk git commit -m "feat: expose worker relay replacement metrics"
```

## Task 9: Remove Production Old-Path Documentation Claims

**Files:**
- Modify: `src/thread_model_cn.md`
- Modify: `docs/superpowers/plans/2026-06-09-linux-high-performance-threading-and-batching.md`
- Create: `docs/superpowers/specs/2026-06-09-linux-relay-full-replacement-design.md`

- [ ] **Step 1: Write replacement design note**

Create `docs/superpowers/specs/2026-06-09-linux-relay-full-replacement-design.md`:

```markdown
# Linux Relay Full Replacement Design

## Decision

On Linux, the worker relay is the only production relay backend. The old blocking relay is retained only as demo/legacy comparison code and must not be linked into `tcpquic-proxy`.

## Production Data Path

- TCP to QUIC: worker-owned `epoll` readiness, `readv`, optional compression, multi-buffer `StreamSend`.
- QUIC to TCP: MsQuic callback copies receive data into worker-owned buffers, optional decompression, worker-owned `writev`.
- Shutdown: `TqRelayStop()` unregisters the worker relay and never deletes a blocking relay object.

## Compatibility

Compressed tunnels are supported in the worker path. Compression is inline on the owner worker in this replacement step.

## Legacy Demo

The old blocking relay is named `TqBlockingDemoRelay` and is available only through demo/test targets.
```

- [ ] **Step 2: Update thread model doc**

In `src/thread_model_cn.md`, replace statements that say every tunnel creates `TqTunnelRelay::TcpThread` with:

```markdown
Linux 生产路径不再为每条隧道创建 relay TCP 线程。所有 relay TCP fd 由 `TqLinuxRelayWorker` 分片持有，通过 `epoll`、`readv`、`writev` 处理。旧 `TqBlockingDemoRelay` 仅用于 demo/legacy 对比，不属于生产线程模型。
```

- [ ] **Step 3: Amend old plan note**

At the top of `docs/superpowers/plans/2026-06-09-linux-high-performance-threading-and-batching.md`, add:

```markdown
> Superseded production target: `docs/superpowers/plans/2026-06-09-linux-relay-full-replacement.md` changes the final target from "Linux worker fast path with blocking compressed fallback" to "Linux worker is the only production relay backend; blocking relay is demo-only."
```

- [ ] **Step 4: Search for stale production claims**

Run:

```bash
rtk rg "TqTunnelRelay|TcpThread|blocking relay|compressed relays keep|TqTcpWriteQueue" docs src/thread_model_cn.md
```

Expected: any remaining matches must explicitly describe demo/legacy behavior or historical context, not current Linux production behavior.

- [ ] **Step 5: Commit**

Run:

```bash
rtk git add src/thread_model_cn.md docs/superpowers/plans/2026-06-09-linux-high-performance-threading-and-batching.md docs/superpowers/specs/2026-06-09-linux-relay-full-replacement-design.md
rtk git commit -m "docs: document linux relay full replacement target"
```

## Task 10: Final Build And Test Gate

**Files:**
- No source changes expected.

- [ ] **Step 1: Run production build**

Run:

```bash
rtk cmake --build build-gcc10-fixed --target tcpquic-proxy
```

Expected: PASS.

- [ ] **Step 2: Run focused replacement tests**

Run:

```bash
rtk cmake --build build-gcc10-fixed --target \
  tcpquic_relay_backend_selection_test \
  tcpquic_linux_relay_worker_io_test \
  tcpquic_tunnel_test \
  tcpquic_router_runtime_test \
  tcpquic_production_linkage_guard_test
```

Then run:

```bash
rtk ./build-gcc10-fixed/bin/Release/tcpquic_relay_backend_selection_test
rtk ./build-gcc10-fixed/bin/Release/tcpquic_linux_relay_worker_io_test
rtk ./build-gcc10-fixed/bin/Release/tcpquic_tunnel_test
rtk ./build-gcc10-fixed/bin/Release/tcpquic_router_runtime_test
rtk ./build-gcc10-fixed/bin/Release/tcpquic_production_linkage_guard_test
```

Expected: all PASS.

- [ ] **Step 3: Run full build**

Run:

```bash
rtk cmake --build build-gcc10-fixed
```

Expected: PASS.

- [ ] **Step 4: Run all test binaries**

Run outside the restricted sandbox if local bind/listen tests fail with `Operation not permitted`:

```bash
rtk bash -lc 'set -e; for t in tcpquic_acl_test tcpquic_admin_http_test tcpquic_compress_test tcpquic_config_router_test tcpquic_control_test tcpquic_http_connect_test tcpquic_linux_relay_buffer_pool_test tcpquic_linux_relay_worker_io_test tcpquic_linux_relay_worker_queue_test tcpquic_relay_backend_selection_test tcpquic_router_runtime_test tcpquic_socks5_test tcpquic_tcp_write_queue_test tcpquic_thread_pool_test tcpquic_tuning_test tcpquic_tunnel_reaper_test tcpquic_tunnel_test tcpquic_production_linkage_guard_test; do echo RUN $t; ./build-gcc10-fixed/bin/Release/$t; done'
```

Expected: all PASS.

- [ ] **Step 5: Verify old relay is demo-only**

Run:

```bash
rtk rg "TqBlockingDemoRelay|relay_blocking_demo|TqTcpWriteQueue" src/CMakeLists.txt src --glob '!src/relay_blocking_demo.cpp' --glob '!src/relay_blocking_demo.h' --glob '!src/unittest/blocking_relay_demo_test.cpp' --glob '!src/unittest/tcp_write_queue_test.cpp'
```

Expected: no production references. `TqTcpWriteQueue` may appear only in its own unit test and demo relay files.

- [ ] **Step 6: Commit final verification note if docs changed**

If Task 10 required no source or doc changes, do not commit.

If a verification note was added to docs, run:

```bash
rtk git add docs
rtk git commit -m "docs: record linux relay replacement verification"
```

## Execution Notes

- Do not delete the old blocking relay in the first replacement pass. Move it and rename it so production cannot accidentally use it.
- Do not keep a compression fallback to blocking relay. If compressed worker relay fails to start, `TqRelayStart()` must fail rather than silently selecting demo code.
- Do not keep `TqRelayBackendType::Blocking` in production headers.
- Do not link `relay_blocking_demo.cpp` into `tcpquic-proxy`, `tcpquic_tunnel_test`, or router/runtime tests.
- Use `rtk` for all shell commands.
- If local socket tests fail in the restricted sandbox with `Operation not permitted`, rerun the same command with approved escalation rather than changing the test.

## Self-Review

- Spec coverage: The plan covers production backend selection, code isolation, compressed worker support, production linkage guards, metrics, documentation, and final verification.
- Placeholder scan: No unfinished placeholder markers or unspecified "add tests" steps remain.
- Type consistency: The plan consistently uses `TqRelayBackendType::LinuxWorker`, `TqLinuxRelayRegistration`, `TqBlockingDemoRelay`, and `TqLinuxRelayWorkerSnapshot`.
