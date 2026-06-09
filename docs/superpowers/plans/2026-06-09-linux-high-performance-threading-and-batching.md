# Linux High Performance Threading and Batching Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Replace the Linux relay data path's per-tunnel TCP reader with worker-owned epoll/readv batching, then migrate QUIC receive writes into the same worker using bounded pooled buffers and writev.

**Architecture:** Implement Linux-only worker shards behind the existing `TqRelayStart` API so callers keep their current lifecycle. Phase B introduces one owner worker per relay for TCP-to-QUIC while QUIC-to-TCP keeps `TqTcpWriteQueue`; Phase C adds pooled readv and multi-buffer StreamSend; Phase D moves QUIC-to-TCP into the worker with copy-into-pool and writev. Windows and IOCP are intentionally out of scope for this plan.

**Tech Stack:** C++17, CMake, Linux `epoll`, `eventfd`, `readv`, `writev`, MsQuic stream callbacks, existing `TqTuningConfig`, existing assert-style unit tests.

---

## Scope

This plan implements Linux sections from `docs/specs/2026-06-09-high-performance-threading-and-batching-design.md` through Phase D:

- Phase A: make relay memory and batching boundaries observable in code and tests.
- Phase B: add Linux owner worker + epoll and remove per-tunnel TCP reader thread for the fast path.
- Phase C: add worker-local buffer pool, `readv`, multi-buffer `StreamSend`, send-completion buffer lifetime, and fairness budgets.
- Phase D: migrate QUIC-to-TCP from `TqTcpWriteQueue` to worker-owned copy-into-pool + `writev` for the fast path.

The plan does not implement:

- Windows backend, WSAPoll, IOCP, WSARecv, or WSASend.
- MsQuic deferred receive view / deferred `StreamReceiveComplete`.
- Async DNS or replacing the detached server-side dial thread.
- Per-worker QUIC connection sub-pools.
- Compression-pool offload. Compressed relays keep the current blocking relay path until a later plan.

## File Structure

- Create `src/linux_relay_buffer_pool.h`: worker-local fixed-size slab pool, `TqBufferRef`, `TqBufferView`, and release semantics for send/write completion.
- Create `src/linux_relay_buffer_pool.cpp`: pool allocation, recycling, byte accounting, and vector helpers for tests.
- Create `src/linux_relay_worker.h`: Linux worker API, relay registration, wake-coalesced event queue, test snapshot types.
- Create `src/linux_relay_worker.cpp`: `epoll` loop, `eventfd` wakeup, tunnel ready queue, TCP-to-QUIC readv path, QUIC callback event handling, and QUIC-to-TCP writev path.
- Modify `src/relay.h`: add an explicit relay backend tag and Linux worker identity fields while keeping the `TqRelayStart` function signature stable.
- Modify `src/relay.cpp`: select Linux worker fast path when no compression is active; keep existing `TqTunnelRelay` as fallback for compressed relays and non-Linux builds.
- Modify `src/tuning.h`: add Linux relay worker and batch budget fields.
- Modify `src/tuning.cpp`: derive Linux worker defaults from current tuning mode and `--max-memory-mb`.
- Modify `src/server_metrics.h` and `src/server_metrics.cpp`: expose worker counts, pending bytes, wakeups, and batch metrics in the existing server metrics JSON response.
- Modify `src/CMakeLists.txt`: compile new Linux files into `tcpquic-proxy`, `tcpquic_tunnel_test`, and focused Linux test binaries.
- Create `src/unittest/linux_relay_buffer_pool_test.cpp`: unit tests for pool reuse, budget rejection, and view release.
- Create `src/unittest/linux_relay_worker_queue_test.cpp`: unit tests for MPSC event queue and lost-wakeup protection using a test worker.
- Create `src/unittest/linux_relay_worker_io_test.cpp`: Linux socketpair tests for epoll/readv/writev behavior without real MsQuic network traffic.
- Modify `src/unittest/tuning_test.cpp`: assert Linux worker and budget defaults.
- Modify `src/unittest/router_runtime_test.cpp`: assert new metrics fields serialize with stable names.

## Task 1: Linux Relay Tuning Boundaries

**Files:**
- Modify: `src/tuning.h`
- Modify: `src/tuning.cpp`
- Modify: `src/unittest/tuning_test.cpp`
- Modify: `src/CMakeLists.txt`

- [ ] **Step 1: Write the failing tuning defaults test**

Add this block to `src/unittest/tuning_test.cpp` before `return 0;`:

```cpp
    {
        TqSetRelayMemoryBudget(512);
        TqConfig cfg{};
        cfg.TuningMode = TqTuningMode::Wan;
        TqComputeTuning(cfg, cfg.Tuning);

        assert(cfg.Tuning.LinuxRelayWorkerCount >= 1);
        assert(cfg.Tuning.LinuxRelayMaxIov == 16);
        assert(cfg.Tuning.LinuxRelayReadChunkSize == 64 * 1024);
        assert(cfg.Tuning.LinuxRelayReadBatchBytes == 1024 * 1024);
        assert(cfg.Tuning.LinuxRelayWorkerEventBudget == 4096);
        assert(cfg.Tuning.LinuxRelayWorkerByteBudgetPerTick == 64u * 1024 * 1024);
        assert(cfg.Tuning.LinuxRelayGlobalPendingBytes == 512ull * 1024 * 1024 / 2);
        assert(cfg.Tuning.LinuxRelayPerTunnelPendingBytes == 4u * 1024 * 1024);
        assert(cfg.Tuning.LinuxRelayPerWorkerPendingBytes >= cfg.Tuning.LinuxRelayPerTunnelPendingBytes);

        TqSetRelayMemoryBudget(0);
    }

    {
        TqConfig cfg{};
        cfg.TuningMode = TqTuningMode::Lan;
        TqComputeTuning(cfg, cfg.Tuning);

        assert(cfg.Tuning.LinuxRelayMaxIov == 8);
        assert(cfg.Tuning.LinuxRelayReadChunkSize == 32 * 1024);
        assert(cfg.Tuning.LinuxRelayReadBatchBytes == 256 * 1024);
        assert(cfg.Tuning.LinuxRelayWorkerEventBudget == 1024);
        assert(cfg.Tuning.LinuxRelayWorkerByteBudgetPerTick == 16u * 1024 * 1024);
    }
```

- [ ] **Step 2: Run the test to verify it fails**

Run:

```bash
rtk cmake --build build-gcc10-fixed --target tcpquic_tuning_test
rtk ./build-gcc10-fixed/bin/Release/tcpquic_tuning_test
```

Expected: build fails because `TqTuningConfig` has no `LinuxRelayWorkerCount`, `LinuxRelayMaxIov`, `LinuxRelayReadChunkSize`, `LinuxRelayReadBatchBytes`, `LinuxRelayWorkerEventBudget`, `LinuxRelayWorkerByteBudgetPerTick`, `LinuxRelayGlobalPendingBytes`, `LinuxRelayPerTunnelPendingBytes`, or `LinuxRelayPerWorkerPendingBytes` members.

- [ ] **Step 3: Add Linux relay fields to tuning**

Add these fields to `struct TqTuningConfig` in `src/tuning.h` after `TcpSocketBufferBytes`:

```cpp
    uint32_t LinuxRelayWorkerCount{0};
    uint32_t LinuxRelayMaxIov{16};
    size_t LinuxRelayReadChunkSize{64 * 1024};
    size_t LinuxRelayReadBatchBytes{1024 * 1024};
    size_t LinuxRelayQuicRecvBatchBytes{1024 * 1024};
    uint64_t LinuxRelayGlobalPendingBytes{256ull * 1024 * 1024};
    uint64_t LinuxRelayPerWorkerPendingBytes{32ull * 1024 * 1024};
    uint64_t LinuxRelayPerTunnelPendingBytes{4ull * 1024 * 1024};
    uint32_t LinuxRelayWorkerEventBudget{4096};
    uint64_t LinuxRelayWorkerByteBudgetPerTick{64ull * 1024 * 1024};
```

- [ ] **Step 4: Populate the fields in `TqComputeTuning`**

In `src/tuning.cpp`, add a helper near the existing tuning helper functions:

```cpp
namespace {

uint32_t TqDetectLinuxRelayWorkers() {
    const unsigned int detected = std::thread::hardware_concurrency();
    if (detected == 0) {
        return 1;
    }
    return std::max(1u, std::min(detected, 8u));
}

void TqApplyLinuxRelayDefaults(TqTuningConfig& out, TqTuningMode mode) {
    out.LinuxRelayWorkerCount = TqDetectLinuxRelayWorkers();

    if (mode == TqTuningMode::Lan) {
        out.LinuxRelayMaxIov = 8;
        out.LinuxRelayReadChunkSize = 32 * 1024;
        out.LinuxRelayReadBatchBytes = 256 * 1024;
        out.LinuxRelayQuicRecvBatchBytes = 256 * 1024;
        out.LinuxRelayWorkerEventBudget = 1024;
        out.LinuxRelayWorkerByteBudgetPerTick = 16ull * 1024 * 1024;
    } else {
        out.LinuxRelayMaxIov = 16;
        out.LinuxRelayReadChunkSize = 64 * 1024;
        out.LinuxRelayReadBatchBytes = 1024 * 1024;
        out.LinuxRelayQuicRecvBatchBytes = 1024 * 1024;
        out.LinuxRelayWorkerEventBudget = 4096;
        out.LinuxRelayWorkerByteBudgetPerTick = 64ull * 1024 * 1024;
    }

    const uint64_t configuredMemoryBytes =
        static_cast<uint64_t>(TqGetRelayMemoryBudget()) * 1024ull * 1024ull;
    const uint64_t relayMemoryBytes =
        configuredMemoryBytes == 0 ? 512ull * 1024 * 1024 : configuredMemoryBytes;
    out.LinuxRelayGlobalPendingBytes = relayMemoryBytes / 2;
    out.LinuxRelayPerTunnelPendingBytes = std::min<uint64_t>(
        16ull * 1024 * 1024,
        std::max<uint64_t>(4ull * 1024 * 1024, out.LinuxRelayReadBatchBytes * 4));
    out.LinuxRelayPerWorkerPendingBytes = std::max<uint64_t>(
        out.LinuxRelayPerTunnelPendingBytes,
        out.LinuxRelayGlobalPendingBytes / std::max<uint32_t>(out.LinuxRelayWorkerCount, 1));
}

} // namespace
```

Ensure `src/tuning.cpp` includes these headers:

```cpp
#include <algorithm>
#include <thread>
```

Call the helper at the end of `TqComputeTuning`, after mode/custom overrides and before runtime observations are applied:

```cpp
    TqApplyLinuxRelayDefaults(out, cfg.TuningMode);
```

- [ ] **Step 5: Run the tuning test**

Run:

```bash
rtk cmake --build build-gcc10-fixed --target tcpquic_tuning_test
rtk ./build-gcc10-fixed/bin/Release/tcpquic_tuning_test
```

Expected: PASS.

- [ ] **Step 6: Commit**

Run:

```bash
rtk git add src/tuning.h src/tuning.cpp src/unittest/tuning_test.cpp src/CMakeLists.txt
rtk git commit -m "feat: add linux relay tuning budgets"
```

## Task 2: Worker-Local Buffer Pool

**Files:**
- Create: `src/linux_relay_buffer_pool.h`
- Create: `src/linux_relay_buffer_pool.cpp`
- Create: `src/unittest/linux_relay_buffer_pool_test.cpp`
- Modify: `src/CMakeLists.txt`

- [ ] **Step 1: Write the failing buffer pool test**

Create `src/unittest/linux_relay_buffer_pool_test.cpp`:

```cpp
#include "../linux_relay_buffer_pool.h"

#include <cassert>
#include <cstring>

int main() {
    TqLinuxRelayBufferPool pool(64, 2, 128);
    assert(pool.PendingBytes() == 0);
    assert(pool.FreeCount() == 0);

    auto first = pool.Acquire();
    auto second = pool.Acquire();
    auto third = pool.Acquire();
    assert(first);
    assert(second);
    assert(!third);
    assert(pool.PendingBytes() == 128);

    std::memset(first->Data(), 0xAB, first->Capacity());
    TqBufferView view{first->Data(), 17, first};
    assert(view.Len == 17);
    assert(view.Owner);

    first.reset();
    assert(pool.PendingBytes() == 128);
    view.Owner.reset();
    assert(pool.PendingBytes() == 64);

    second.reset();
    assert(pool.PendingBytes() == 0);
    assert(pool.FreeCount() == 2);

    auto reused = pool.Acquire();
    assert(reused);
    assert(pool.PendingBytes() == 64);
    reused.reset();

    return 0;
}
```

Add this target to `src/CMakeLists.txt` after `tcpquic_tcp_write_queue_test`:

```cmake
add_executable(tcpquic_linux_relay_buffer_pool_test
    unittest/linux_relay_buffer_pool_test.cpp
    linux_relay_buffer_pool.cpp
)
target_include_directories(tcpquic_linux_relay_buffer_pool_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
set_property(TARGET tcpquic_linux_relay_buffer_pool_test PROPERTY FOLDER "tools")
set_property(TARGET tcpquic_linux_relay_buffer_pool_test APPEND PROPERTY BUILD_RPATH "$ORIGIN")
```

Add `tcpquic_linux_relay_buffer_pool_test` to `TCPQUIC_TEST_TARGETS`.

- [ ] **Step 2: Run the test to verify it fails**

Run:

```bash
rtk cmake --build build-gcc10-fixed --target tcpquic_linux_relay_buffer_pool_test
```

Expected: build fails because `linux_relay_buffer_pool.h` does not exist.

- [ ] **Step 3: Implement the buffer pool header**

Create `src/linux_relay_buffer_pool.h`:

```cpp
#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <vector>

class TqLinuxRelayBufferPool;

class TqRelayBuffer final {
public:
    TqRelayBuffer(TqLinuxRelayBufferPool* pool, size_t capacity);
    ~TqRelayBuffer();

    TqRelayBuffer(const TqRelayBuffer&) = delete;
    TqRelayBuffer& operator=(const TqRelayBuffer&) = delete;

    uint8_t* Data();
    const uint8_t* Data() const;
    size_t Capacity() const;
    void SetLength(size_t length);
    size_t Length() const;

private:
    friend class TqLinuxRelayBufferPool;

    TqLinuxRelayBufferPool* Pool;
    std::vector<uint8_t> Storage;
    size_t UsedLength{0};
};

using TqBufferRef = std::shared_ptr<TqRelayBuffer>;

struct TqBufferView {
    uint8_t* Data{nullptr};
    size_t Len{0};
    TqBufferRef Owner;
};

class TqLinuxRelayBufferPool final {
public:
    TqLinuxRelayBufferPool(size_t chunkSize, size_t maxBuffers, uint64_t maxPendingBytes);
    ~TqLinuxRelayBufferPool();

    TqLinuxRelayBufferPool(const TqLinuxRelayBufferPool&) = delete;
    TqLinuxRelayBufferPool& operator=(const TqLinuxRelayBufferPool&) = delete;

    TqBufferRef Acquire();
    size_t ChunkSize() const;
    size_t FreeCount() const;
    uint64_t PendingBytes() const;
    uint64_t MaxPendingBytes() const;

private:
    friend class TqRelayBuffer;

    void Release(TqRelayBuffer* buffer);

    size_t ChunkBytes;
    size_t MaxBuffers;
    uint64_t MaxBytes;
    mutable std::mutex Lock;
    std::vector<TqRelayBuffer*> Free;
    size_t AllocatedBuffers{0};
    uint64_t Pending{0};
};
```

- [ ] **Step 4: Implement the buffer pool source**

Create `src/linux_relay_buffer_pool.cpp`:

```cpp
#include "linux_relay_buffer_pool.h"

#include <algorithm>
#include <new>

TqRelayBuffer::TqRelayBuffer(TqLinuxRelayBufferPool* pool, size_t capacity)
    : Pool(pool), Storage(capacity) {}

TqRelayBuffer::~TqRelayBuffer() = default;

uint8_t* TqRelayBuffer::Data() {
    return Storage.data();
}

const uint8_t* TqRelayBuffer::Data() const {
    return Storage.data();
}

size_t TqRelayBuffer::Capacity() const {
    return Storage.size();
}

void TqRelayBuffer::SetLength(size_t length) {
    UsedLength = std::min(length, Storage.size());
}

size_t TqRelayBuffer::Length() const {
    return UsedLength;
}

TqLinuxRelayBufferPool::TqLinuxRelayBufferPool(
    size_t chunkSize,
    size_t maxBuffers,
    uint64_t maxPendingBytes)
    : ChunkBytes(chunkSize), MaxBuffers(maxBuffers), MaxBytes(maxPendingBytes) {}

TqLinuxRelayBufferPool::~TqLinuxRelayBufferPool() {
    for (auto* buffer : Free) {
        delete buffer;
    }
    Free.clear();
}

TqBufferRef TqLinuxRelayBufferPool::Acquire() {
    std::lock_guard<std::mutex> guard(Lock);
    if (Pending + ChunkBytes > MaxBytes) {
        return {};
    }

    TqRelayBuffer* buffer = nullptr;
    if (!Free.empty()) {
        buffer = Free.back();
        Free.pop_back();
    } else {
        if (AllocatedBuffers >= MaxBuffers) {
            return {};
        }
        buffer = new (std::nothrow) TqRelayBuffer(this, ChunkBytes);
        if (buffer == nullptr) {
            return {};
        }
        ++AllocatedBuffers;
    }

    buffer->UsedLength = 0;
    Pending += ChunkBytes;
    return TqBufferRef(buffer, [this](TqRelayBuffer* released) {
        Release(released);
    });
}

size_t TqLinuxRelayBufferPool::ChunkSize() const {
    return ChunkBytes;
}

size_t TqLinuxRelayBufferPool::FreeCount() const {
    std::lock_guard<std::mutex> guard(Lock);
    return Free.size();
}

uint64_t TqLinuxRelayBufferPool::PendingBytes() const {
    std::lock_guard<std::mutex> guard(Lock);
    return Pending;
}

uint64_t TqLinuxRelayBufferPool::MaxPendingBytes() const {
    return MaxBytes;
}

void TqLinuxRelayBufferPool::Release(TqRelayBuffer* buffer) {
    if (buffer == nullptr) {
        return;
    }
    std::lock_guard<std::mutex> guard(Lock);
    buffer->UsedLength = 0;
    Pending -= std::min<uint64_t>(Pending, ChunkBytes);
    Free.push_back(buffer);
}
```

- [ ] **Step 5: Run the buffer pool test**

Run:

```bash
rtk cmake --build build-gcc10-fixed --target tcpquic_linux_relay_buffer_pool_test
rtk ./build-gcc10-fixed/bin/Release/tcpquic_linux_relay_buffer_pool_test
```

Expected: PASS.

- [ ] **Step 6: Commit**

Run:

```bash
rtk git add src/linux_relay_buffer_pool.h src/linux_relay_buffer_pool.cpp src/unittest/linux_relay_buffer_pool_test.cpp src/CMakeLists.txt
rtk git commit -m "feat: add linux relay buffer pool"
```

## Task 3: Wake-Coalesced Linux Worker Queue

**Files:**
- Create: `src/linux_relay_worker.h`
- Create: `src/linux_relay_worker.cpp`
- Create: `src/unittest/linux_relay_worker_queue_test.cpp`
- Modify: `src/CMakeLists.txt`

- [ ] **Step 1: Write the failing worker queue test**

Create `src/unittest/linux_relay_worker_queue_test.cpp`:

```cpp
#include "../linux_relay_worker.h"

#include <cassert>
#include <cstdint>
#include <vector>

int main() {
    TqLinuxRelayWorker worker(TqLinuxRelayWorkerConfig{});
    assert(worker.StartForTest());

    for (uint64_t i = 0; i < 1000; ++i) {
        TqLinuxRelayEvent event{};
        event.Type = TqLinuxRelayEventType::TestMarker;
        event.Value = i + 1;
        worker.EnqueueForTest(event);
    }

    assert(worker.DrainForTest(1000) == 1000);
    TqLinuxRelayWorkerSnapshot snapshot = worker.Snapshot();
    assert(snapshot.EventsProcessed == 1000);
    assert(snapshot.WakeupWrites >= 1);
    assert(snapshot.WakeupWrites < 1000);
    assert(snapshot.PendingEvents == 0);

    worker.Stop();
    return 0;
}
```

Add this target to `src/CMakeLists.txt`:

```cmake
add_executable(tcpquic_linux_relay_worker_queue_test
    unittest/linux_relay_worker_queue_test.cpp
    linux_relay_worker.cpp
    linux_relay_buffer_pool.cpp
)
target_include_directories(tcpquic_linux_relay_worker_queue_test PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${MSQUIC_SOURCE_DIR}/src/inc)
target_link_libraries(tcpquic_linux_relay_worker_queue_test Threads::Threads)
set_property(TARGET tcpquic_linux_relay_worker_queue_test PROPERTY FOLDER "tools")
set_property(TARGET tcpquic_linux_relay_worker_queue_test APPEND PROPERTY BUILD_RPATH "$ORIGIN")
```

Add `tcpquic_linux_relay_worker_queue_test` to `TCPQUIC_TEST_TARGETS`.

- [ ] **Step 2: Run the test to verify it fails**

Run:

```bash
rtk cmake --build build-gcc10-fixed --target tcpquic_linux_relay_worker_queue_test
```

Expected: build fails because `linux_relay_worker.h` does not exist.

- [ ] **Step 3: Implement worker queue types**

Create `src/linux_relay_worker.h` with these public types and methods:

```cpp
#pragma once

#include "linux_relay_buffer_pool.h"
#include "tuning.h"

#include <atomic>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <thread>

struct MsQuicStream;
struct TqRelayHandle;

enum class TqLinuxRelayEventType {
    TestMarker,
    TcpWritable,
    QuicReceive,
    QuicSendComplete,
    QuicIdealSendBuffer,
    Shutdown,
};

struct TqLinuxRelayEvent {
    TqLinuxRelayEventType Type{TqLinuxRelayEventType::Shutdown};
    uint64_t Value{0};
    void* Relay{nullptr};
    TqBufferRef Buffer;
    size_t Length{0};
    bool Fin{false};
};

struct TqLinuxRelayWorkerConfig {
    uint32_t EventBudget{4096};
    uint64_t ByteBudgetPerTick{64ull * 1024 * 1024};
    size_t ReadChunkSize{64 * 1024};
    size_t ReadBatchBytes{1024 * 1024};
    uint32_t MaxIov{16};
    uint64_t MaxPendingBytes{256ull * 1024 * 1024};
};

struct TqLinuxRelayWorkerSnapshot {
    uint64_t EventsProcessed{0};
    uint64_t WakeupWrites{0};
    uint64_t PendingEvents{0};
    uint64_t PendingBytes{0};
};

class TqLinuxRelayWorker final {
public:
    explicit TqLinuxRelayWorker(const TqLinuxRelayWorkerConfig& config);
    ~TqLinuxRelayWorker();

    TqLinuxRelayWorker(const TqLinuxRelayWorker&) = delete;
    TqLinuxRelayWorker& operator=(const TqLinuxRelayWorker&) = delete;

    bool Start();
    bool StartForTest();
    void Stop();
    void Enqueue(const TqLinuxRelayEvent& event);
    void EnqueueForTest(const TqLinuxRelayEvent& event);
    size_t DrainForTest(size_t budget);
    TqLinuxRelayWorkerSnapshot Snapshot() const;

private:
    void Wake();
    size_t DrainEvents(size_t budget);
    void Run();

    TqLinuxRelayWorkerConfig Config;
    int WakeFd{-1};
    int EpollFd{-1};
    std::atomic<bool> Running{false};
    std::atomic<bool> WakeArmed{false};
    mutable std::mutex QueueLock;
    std::deque<TqLinuxRelayEvent> Queue;
    std::thread Thread;
    std::atomic<uint64_t> EventsProcessed{0};
    std::atomic<uint64_t> WakeupWrites{0};
};
```

- [ ] **Step 4: Implement queue, eventfd, and lost-wakeup protection**

Create `src/linux_relay_worker.cpp`:

```cpp
#include "linux_relay_worker.h"

#include <cerrno>
#include <cstring>

#include <sys/epoll.h>
#include <sys/eventfd.h>
#include <unistd.h>

TqLinuxRelayWorker::TqLinuxRelayWorker(const TqLinuxRelayWorkerConfig& config)
    : Config(config) {}

TqLinuxRelayWorker::~TqLinuxRelayWorker() {
    Stop();
}

bool TqLinuxRelayWorker::Start() {
    if (Running.load()) {
        return false;
    }
    WakeFd = ::eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
    if (WakeFd < 0) {
        return false;
    }
    EpollFd = ::epoll_create1(EPOLL_CLOEXEC);
    if (EpollFd < 0) {
        ::close(WakeFd);
        WakeFd = -1;
        return false;
    }
    epoll_event event{};
    event.events = EPOLLIN;
    event.data.fd = WakeFd;
    if (::epoll_ctl(EpollFd, EPOLL_CTL_ADD, WakeFd, &event) != 0) {
        ::close(EpollFd);
        ::close(WakeFd);
        EpollFd = -1;
        WakeFd = -1;
        return false;
    }
    Running.store(true);
    Thread = std::thread(&TqLinuxRelayWorker::Run, this);
    return true;
}

bool TqLinuxRelayWorker::StartForTest() {
    if (Running.exchange(true)) {
        return false;
    }
    WakeFd = ::eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
    if (WakeFd < 0) {
        Running.store(false);
        return false;
    }
    return true;
}

void TqLinuxRelayWorker::Stop() {
    if (!Running.exchange(false)) {
        return;
    }
    Wake();
    if (Thread.joinable()) {
        Thread.join();
    }
    if (EpollFd >= 0) {
        ::close(EpollFd);
        EpollFd = -1;
    }
    if (WakeFd >= 0) {
        ::close(WakeFd);
        WakeFd = -1;
    }
}

void TqLinuxRelayWorker::Enqueue(const TqLinuxRelayEvent& event) {
    {
        std::lock_guard<std::mutex> guard(QueueLock);
        Queue.push_back(event);
    }
    Wake();
}

void TqLinuxRelayWorker::EnqueueForTest(const TqLinuxRelayEvent& event) {
    Enqueue(event);
}

void TqLinuxRelayWorker::Wake() {
    if (WakeFd < 0) {
        return;
    }
    if (WakeArmed.exchange(true, std::memory_order_acq_rel)) {
        return;
    }
    const uint64_t one = 1;
    const ssize_t written = ::write(WakeFd, &one, sizeof(one));
    if (written == static_cast<ssize_t>(sizeof(one))) {
        WakeupWrites.fetch_add(1);
        return;
    }
    if (errno != EAGAIN && errno != EINTR) {
        WakeArmed.store(false, std::memory_order_release);
    }
}

size_t TqLinuxRelayWorker::DrainForTest(size_t budget) {
    return DrainEvents(budget);
}

size_t TqLinuxRelayWorker::DrainEvents(size_t budget) {
    size_t processed = 0;
    while (processed < budget) {
        TqLinuxRelayEvent event{};
        {
            std::lock_guard<std::mutex> guard(QueueLock);
            if (Queue.empty()) {
                break;
            }
            event = std::move(Queue.front());
            Queue.pop_front();
        }
        ++processed;
    }
    EventsProcessed.fetch_add(processed);

    WakeArmed.store(false, std::memory_order_release);
    {
        std::lock_guard<std::mutex> guard(QueueLock);
        if (!Queue.empty()) {
            Wake();
        }
    }
    return processed;
}

TqLinuxRelayWorkerSnapshot TqLinuxRelayWorker::Snapshot() const {
    std::lock_guard<std::mutex> guard(QueueLock);
    TqLinuxRelayWorkerSnapshot snapshot{};
    snapshot.EventsProcessed = EventsProcessed.load();
    snapshot.WakeupWrites = WakeupWrites.load();
    snapshot.PendingEvents = Queue.size();
    snapshot.PendingBytes = 0;
    return snapshot;
}

// PendingBytes is zero in this queue-only task because relay pools do not exist yet.
// Task 7 replaces this with real pool and pending-write accounting before metrics use it.

void TqLinuxRelayWorker::Run() {
    epoll_event events[16]{};
    while (Running.load()) {
        const int count = ::epoll_wait(EpollFd, events, 16, 100);
        if (count < 0 && errno == EINTR) {
            continue;
        }
        if (count <= 0) {
            continue;
        }
        uint64_t value = 0;
        while (::read(WakeFd, &value, sizeof(value)) > 0) {
        }
        DrainEvents(Config.EventBudget);
    }
}
```

- [ ] **Step 5: Run the worker queue test**

Run:

```bash
rtk cmake --build build-gcc10-fixed --target tcpquic_linux_relay_worker_queue_test
rtk ./build-gcc10-fixed/bin/Release/tcpquic_linux_relay_worker_queue_test
```

Expected: PASS.

- [ ] **Step 6: Commit**

Run:

```bash
rtk git add src/linux_relay_worker.h src/linux_relay_worker.cpp src/unittest/linux_relay_worker_queue_test.cpp src/CMakeLists.txt
rtk git commit -m "feat: add wake coalesced linux relay worker"
```

## Task 4: Linux Worker TCP-to-QUIC Registration Skeleton

**Files:**
- Modify: `src/linux_relay_worker.h`
- Modify: `src/linux_relay_worker.cpp`
- Create: `src/unittest/linux_relay_worker_io_test.cpp`
- Modify: `src/CMakeLists.txt`

- [ ] **Step 1: Write the failing epoll registration test**

Create `src/unittest/linux_relay_worker_io_test.cpp`:

```cpp
#include "../linux_relay_worker.h"

#include <cassert>
#include <cstring>
#include <sys/socket.h>
#include <unistd.h>

int main() {
    TqLinuxRelayWorkerConfig config{};
    config.EventBudget = 128;
    config.ReadChunkSize = 4096;
    config.ReadBatchBytes = 16 * 1024;
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
    registration.EnableQuicSends = false;
    assert(worker.RegisterRelayForTest(registration));

    const char payload[] = "linux-worker-epoll-read";
    assert(::write(fds[1], payload, sizeof(payload)) == static_cast<ssize_t>(sizeof(payload)));
    assert(worker.WaitForObservedTcpBytesForTest(sizeof(payload), 2000));

    TqLinuxRelayWorkerSnapshot snapshot = worker.Snapshot();
    assert(snapshot.TcpReadBatches >= 1);
    assert(snapshot.TcpReadBytes >= sizeof(payload));

    worker.Stop();
    ::close(fds[1]);
    return 0;
}
```

Add this CMake target:

```cmake
add_executable(tcpquic_linux_relay_worker_io_test
    unittest/linux_relay_worker_io_test.cpp
    linux_relay_worker.cpp
    linux_relay_buffer_pool.cpp
)
target_include_directories(tcpquic_linux_relay_worker_io_test PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${MSQUIC_SOURCE_DIR}/src/inc)
target_link_libraries(tcpquic_linux_relay_worker_io_test Threads::Threads)
set_property(TARGET tcpquic_linux_relay_worker_io_test PROPERTY FOLDER "tools")
set_property(TARGET tcpquic_linux_relay_worker_io_test APPEND PROPERTY BUILD_RPATH "$ORIGIN")
```

Add `tcpquic_linux_relay_worker_io_test` to `TCPQUIC_TEST_TARGETS`.

- [ ] **Step 2: Run the test to verify it fails**

Run:

```bash
rtk cmake --build build-gcc10-fixed --target tcpquic_linux_relay_worker_io_test
```

Expected: build fails because `TqLinuxRelayRegistration`, `RegisterRelayForTest`, `WaitForObservedTcpBytesForTest`, `TcpReadBatches`, and `TcpReadBytes` do not exist.

- [ ] **Step 3: Add relay registration API**

Add to `src/linux_relay_worker.h`:

```cpp
struct TqLinuxRelayRegistration {
    int TcpFd{-1};
    MsQuicStream* Stream{nullptr};
    TqRelayHandle* Handle{nullptr};
    bool EnableQuicSends{true};
};
```

Add these fields to `TqLinuxRelayWorkerSnapshot`:

```cpp
    uint64_t TcpReadBatches{0};
    uint64_t TcpReadBytes{0};
```

Add these public methods to `TqLinuxRelayWorker`:

```cpp
    bool RegisterRelay(const TqLinuxRelayRegistration& registration);
    bool RegisterRelayForTest(const TqLinuxRelayRegistration& registration);
    bool WaitForObservedTcpBytesForTest(uint64_t bytes, int timeoutMs);
```

Add these private members:

```cpp
    struct RelayState;
    std::mutex RelayLock;
    std::deque<std::unique_ptr<RelayState>> Relays;
    uint64_t NextRelayId{1};
    std::atomic<uint64_t> TcpReadBatches{0};
    std::atomic<uint64_t> TcpReadBytes{0};
```

- [ ] **Step 4: Implement epoll registration and nonblocking setup**

In `src/linux_relay_worker.cpp`, include:

```cpp
#include <chrono>
#include <condition_variable>
#include <fcntl.h>
#include <sys/uio.h>
```

Add the private state above the constructor:

```cpp
struct TqLinuxRelayWorker::RelayState {
    uint64_t Id{0};
    int TcpFd{-1};
    MsQuicStream* Stream{nullptr};
    TqRelayHandle* Handle{nullptr};
    bool EnableQuicSends{true};
    TqLinuxRelayBufferPool Pool;

    RelayState(const TqLinuxRelayRegistration& registration, const TqLinuxRelayWorkerConfig& config)
        : TcpFd(registration.TcpFd),
          Stream(registration.Stream),
          Handle(registration.Handle),
          EnableQuicSends(registration.EnableQuicSends),
          Pool(config.ReadChunkSize, config.MaxIov * 4, config.MaxPendingBytes) {}
};
```

Add helper functions:

```cpp
namespace {

bool TqSetNonBlocking(int fd) {
    const int flags = ::fcntl(fd, F_GETFL, 0);
    if (flags < 0) {
        return false;
    }
    return ::fcntl(fd, F_SETFL, flags | O_NONBLOCK) == 0;
}

} // namespace
```

Implement registration:

```cpp
bool TqLinuxRelayWorker::RegisterRelay(const TqLinuxRelayRegistration& registration) {
    if (registration.TcpFd < 0 || EpollFd < 0) {
        return false;
    }
    if (!TqSetNonBlocking(registration.TcpFd)) {
        return false;
    }

    auto relay = std::make_unique<RelayState>(registration, Config);
    relay->Id = NextRelayId++;
    RelayState* raw = relay.get();

    epoll_event event{};
    event.events = EPOLLIN | EPOLLRDHUP | EPOLLERR;
    event.data.ptr = raw;
    if (::epoll_ctl(EpollFd, EPOLL_CTL_ADD, registration.TcpFd, &event) != 0) {
        return false;
    }

    std::lock_guard<std::mutex> guard(RelayLock);
    Relays.push_back(std::move(relay));
    return true;
}

bool TqLinuxRelayWorker::RegisterRelayForTest(const TqLinuxRelayRegistration& registration) {
    return RegisterRelay(registration);
}
```

Add `DrainTcpReadable` and call it from `Run` for non-wakeup events:

```cpp
void TqLinuxRelayWorker::DrainTcpReadable(RelayState* relay) {
    if (relay == nullptr) {
        return;
    }

    uint64_t readBytes = 0;
    while (readBytes < Config.ReadBatchBytes) {
        std::vector<TqBufferRef> refs;
        std::vector<iovec> iov;
        const size_t maxIov = std::min<size_t>(Config.MaxIov, 1024);
        refs.reserve(maxIov);
        iov.reserve(maxIov);

        for (size_t i = 0; i < maxIov && readBytes + Config.ReadChunkSize <= Config.ReadBatchBytes; ++i) {
            auto buffer = relay->Pool.Acquire();
            if (!buffer) {
                break;
            }
            iovec item{};
            item.iov_base = buffer->Data();
            item.iov_len = buffer->Capacity();
            iov.push_back(item);
            refs.push_back(std::move(buffer));
        }
        if (iov.empty()) {
            break;
        }

        const ssize_t received = ::readv(relay->TcpFd, iov.data(), static_cast<int>(iov.size()));
        if (received > 0) {
            readBytes += static_cast<uint64_t>(received);
            TcpReadBytes.fetch_add(static_cast<uint64_t>(received));
            TcpReadBatches.fetch_add(1);
            continue;
        }
        if (received == 0) {
            break;
        }
        if (errno == EINTR) {
            continue;
        }
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            break;
        }
        break;
    }
}
```

Add the private declaration to the class:

```cpp
    void DrainTcpReadable(RelayState* relay);
```

Update `Run`:

```cpp
        for (int i = 0; i < count; ++i) {
            if (events[i].data.fd == WakeFd) {
                uint64_t value = 0;
                while (::read(WakeFd, &value, sizeof(value)) > 0) {
                }
                DrainEvents(Config.EventBudget);
            } else {
                auto* relay = static_cast<RelayState*>(events[i].data.ptr);
                DrainTcpReadable(relay);
            }
        }
```

Implement test wait:

```cpp
bool TqLinuxRelayWorker::WaitForObservedTcpBytesForTest(uint64_t bytes, int timeoutMs) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
    while (std::chrono::steady_clock::now() < deadline) {
        if (TcpReadBytes.load() >= bytes) {
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    return TcpReadBytes.load() >= bytes;
}
```

Update `Snapshot()`:

```cpp
    snapshot.TcpReadBatches = TcpReadBatches.load();
    snapshot.TcpReadBytes = TcpReadBytes.load();
```

- [ ] **Step 5: Run the worker I/O test**

Run:

```bash
rtk cmake --build build-gcc10-fixed --target tcpquic_linux_relay_worker_io_test
rtk ./build-gcc10-fixed/bin/Release/tcpquic_linux_relay_worker_io_test
```

Expected: PASS.

- [ ] **Step 6: Commit**

Run:

```bash
rtk git add src/linux_relay_worker.h src/linux_relay_worker.cpp src/unittest/linux_relay_worker_io_test.cpp src/CMakeLists.txt
rtk git commit -m "feat: register linux relay sockets with epoll"
```

## Task 5: TCP-to-QUIC Multi-Buffer Send Path

**Files:**
- Modify: `src/linux_relay_worker.h`
- Modify: `src/linux_relay_worker.cpp`
- Modify: `src/unittest/linux_relay_worker_io_test.cpp`

- [ ] **Step 1: Write the failing send batching assertions**

Append this second scenario to `src/unittest/linux_relay_worker_io_test.cpp` before `return 0;`:

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
        registration.EnableQuicSends = false;
        assert(worker.RegisterRelayForTest(registration));

        std::vector<uint8_t> payload(3000, 0x5A);
        assert(::write(fds[1], payload.data(), payload.size()) == static_cast<ssize_t>(payload.size()));
        assert(worker.WaitForObservedTcpBytesForTest(payload.size(), 2000));

        TqLinuxRelayWorkerSnapshot snapshot = worker.Snapshot();
        assert(snapshot.TcpReadBytes >= payload.size());
        assert(snapshot.QuicSendOperations == 0);
        assert(snapshot.MaxTcpReadIovUsed >= 2);
        assert(snapshot.PendingBytes == 0);

        worker.Stop();
        ::close(fds[1]);
    }
```

- [ ] **Step 2: Run the test to verify it fails**

Run:

```bash
rtk cmake --build build-gcc10-fixed --target tcpquic_linux_relay_worker_io_test
```

Expected: build fails because `QuicSendOperations` and `MaxTcpReadIovUsed` are not in the snapshot.

- [ ] **Step 3: Track batch views and send operation counts**

Add to `TqLinuxRelayWorkerSnapshot`:

```cpp
    uint64_t QuicSendOperations{0};
    uint64_t MaxTcpReadIovUsed{0};
```

Add to worker private members:

```cpp
    std::atomic<uint64_t> QuicSendOperations{0};
    std::atomic<uint64_t> MaxTcpReadIovUsed{0};
```

Add helper declaration:

```cpp
    bool SubmitTcpBatchToQuic(RelayState* relay, std::vector<TqBufferView>& views);
    void CompleteQuicSend(void* context);
```

Add a send operation context that owns only pooled buffer refs and an id, not a raw `RelayState*`:

```cpp
struct TqLinuxRelaySendOperation {
    uint64_t RelayId{0};
    std::vector<TqBufferView> Views;
};
```

Add send-completion accounting to `RelayState`:

```cpp
    uint64_t OutstandingQuicSends{0};
```

- [ ] **Step 4: Implement buffer views from `readv`**

Replace the positive `readv` branch in `DrainTcpReadable` with:

```cpp
        if (received > 0) {
            size_t remaining = static_cast<size_t>(received);
            std::vector<TqBufferView> views;
            views.reserve(refs.size());
            for (auto& ref : refs) {
                if (remaining == 0) {
                    break;
                }
                const size_t len = std::min(ref->Capacity(), remaining);
                ref->SetLength(len);
                views.push_back(TqBufferView{ref->Data(), len, ref});
                remaining -= len;
            }

            readBytes += static_cast<uint64_t>(received);
            TcpReadBytes.fetch_add(static_cast<uint64_t>(received));
            TcpReadBatches.fetch_add(1);
            uint64_t previous = MaxTcpReadIovUsed.load();
            while (previous < views.size() &&
                   !MaxTcpReadIovUsed.compare_exchange_weak(previous, views.size())) {
            }
            if (!SubmitTcpBatchToQuic(relay, views)) {
                break;
            }
            continue;
        }
```

Implement `SubmitTcpBatchToQuic`:

```cpp
bool TqLinuxRelayWorker::SubmitTcpBatchToQuic(RelayState* relay, std::vector<TqBufferView>& views) {
    if (relay == nullptr || views.empty()) {
        return true;
    }
    if (!relay->EnableQuicSends || relay->Stream == nullptr) {
        views.clear();
        return true;
    }

    std::vector<QUIC_BUFFER> quicBuffers;
    quicBuffers.reserve(views.size());
    for (const auto& view : views) {
        if (view.Len > UINT32_MAX) {
            return false;
        }
        QUIC_BUFFER buffer{};
        buffer.Buffer = view.Data;
        buffer.Length = static_cast<uint32_t>(view.Len);
        quicBuffers.push_back(buffer);
    }

    auto* operation = new (std::nothrow) TqLinuxRelaySendOperation{};
    if (operation == nullptr) {
        return false;
    }
    operation->RelayId = relay->Id;
    operation->Views = std::move(views);

    const QUIC_STATUS status = relay->Stream->Send(
        quicBuffers.data(),
        static_cast<uint32_t>(quicBuffers.size()),
        QUIC_SEND_FLAG_NONE,
        operation);
    if (QUIC_FAILED(status)) {
        delete operation;
        return false;
    }
    ++relay->OutstandingQuicSends;
    QuicSendOperations.fetch_add(1);
    return true;
}
```

Include `msquic.hpp` in `src/linux_relay_worker.cpp`.

- [ ] **Step 5: Handle send completion event**

When relay callbacks are integrated in Task 6, send completion will enqueue `TqLinuxRelayEventType::QuicSendComplete`; the worker deletes `TqLinuxRelaySendOperation` and releases its pooled buffers. For this task, test mode has `EnableQuicSends = false`, so views release immediately. Add this comment above `SubmitTcpBatchToQuic`:

```cpp
// In production, ClientContext owns TqLinuxRelaySendOperation until
// QUIC_STREAM_EVENT_SEND_COMPLETE is delivered back to the owner worker.
// Tests can disable sends to verify readv batching without a live MsQuic stream.
```

Add worker-side completion handling in Task 6 when callback events are wired:

```cpp
void TqLinuxRelayWorker::CompleteQuicSend(void* context) {
    auto* operation = static_cast<TqLinuxRelaySendOperation*>(context);
    if (operation == nullptr) {
        return;
    }
    RelayState* relay = FindRelayById(operation->RelayId);
    if (relay != nullptr && relay->OutstandingQuicSends > 0) {
        --relay->OutstandingQuicSends;
    }
    delete operation;
}
```

- [ ] **Step 6: Run the worker I/O test**

Run:

```bash
rtk cmake --build build-gcc10-fixed --target tcpquic_linux_relay_worker_io_test
rtk ./build-gcc10-fixed/bin/Release/tcpquic_linux_relay_worker_io_test
```

Expected: PASS.

- [ ] **Step 7: Commit**

Run:

```bash
rtk git add src/linux_relay_worker.h src/linux_relay_worker.cpp src/unittest/linux_relay_worker_io_test.cpp
rtk git commit -m "feat: batch tcp reads into quic buffer views"
```

## Task 6: Integrate Linux Worker With Relay Start

**Files:**
- Modify: `src/relay.h`
- Modify: `src/relay.cpp`
- Modify: `src/linux_relay_worker.h`
- Modify: `src/linux_relay_worker.cpp`
- Modify: `src/CMakeLists.txt`

- [ ] **Step 1: Write the failing backend-tag expectation**

Add these declarations to `src/relay.h` without changing the `TqRelayStart` function signature:

```cpp
class TqLinuxRelayWorker;

enum class TqRelayBackendType {
    None,
    Blocking,
    LinuxWorker,
};

bool TqRelayLinuxFastPathEnabled(const TqRelayHandle* handle);
```

Extend `TqRelayHandle` with explicit backend ownership fields:

```cpp
struct TqRelayHandle {
    std::atomic<bool> Stop{false};
    TqRelayBackendType Backend{TqRelayBackendType::None};
    TqTunnelRelay* Relay{nullptr};
    TqLinuxRelayWorker* LinuxWorker{nullptr};
    uint64_t LinuxRelayId{0};
};
```

Add this assertion to `src/unittest/tcp_tunnel_test.cpp` after `TqRelayHandle RelayHandle;` is available only if a test creates one. If no direct handle exists in that file, create a small new block before `return 0;`:

```cpp
    {
        TqRelayHandle handle{};
        assert(!TqRelayLinuxFastPathEnabled(&handle));
        assert(handle.Backend == TqRelayBackendType::None);
    }
```

- [ ] **Step 2: Run the test to verify it fails**

Run:

```bash
rtk cmake --build build-gcc10-fixed --target tcpquic_tunnel_test
```

Expected: build fails because the backend tag, Linux worker identity fields, and `TqRelayLinuxFastPathEnabled` are not implemented yet.

- [ ] **Step 3: Add worker relay object and global worker set**

In `src/linux_relay_worker.h`, add:

```cpp
class TqLinuxRelayRuntime final {
public:
    static TqLinuxRelayRuntime& Instance();
    bool Start(const TqTuningConfig& tuning);
    void Stop();
    TqLinuxRelayWorker* PickWorker();
    TqLinuxRelayWorkerSnapshot Snapshot() const;

private:
    TqLinuxRelayRuntime() = default;
    mutable std::mutex Lock;
    std::vector<std::unique_ptr<TqLinuxRelayWorker>> Workers;
    size_t NextWorker{0};
};
```

Add `#include <vector>` to the header.

Also add a production registration method that returns a per-worker relay id, while keeping the earlier bool-returning `RegisterRelayForTest` tests intact:

```cpp
struct TqLinuxRelayRegistrationResult {
    bool Ok{false};
    uint64_t RelayId{0};
};

TqLinuxRelayRegistrationResult RegisterRelayWithId(const TqLinuxRelayRegistration& registration);
void UnregisterRelay(uint64_t relayId);
```

Add these private members:

```cpp
    RelayState* FindRelayById(uint64_t relayId);
```

Implement it in `src/linux_relay_worker.cpp`:

```cpp
TqLinuxRelayRuntime& TqLinuxRelayRuntime::Instance() {
    static TqLinuxRelayRuntime runtime;
    return runtime;
}

bool TqLinuxRelayRuntime::Start(const TqTuningConfig& tuning) {
    std::lock_guard<std::mutex> guard(Lock);
    if (!Workers.empty()) {
        return true;
    }

    const uint32_t workerCount = std::max<uint32_t>(1, tuning.LinuxRelayWorkerCount);
    for (uint32_t i = 0; i < workerCount; ++i) {
        TqLinuxRelayWorkerConfig config{};
        config.EventBudget = tuning.LinuxRelayWorkerEventBudget;
        config.ByteBudgetPerTick = tuning.LinuxRelayWorkerByteBudgetPerTick;
        config.ReadChunkSize = tuning.LinuxRelayReadChunkSize;
        config.ReadBatchBytes = tuning.LinuxRelayReadBatchBytes;
        config.MaxIov = tuning.LinuxRelayMaxIov;
        config.MaxPendingBytes = tuning.LinuxRelayPerWorkerPendingBytes;
        auto worker = std::make_unique<TqLinuxRelayWorker>(config);
        if (!worker->Start()) {
            Workers.clear();
            return false;
        }
        Workers.push_back(std::move(worker));
    }
    return true;
}

void TqLinuxRelayRuntime::Stop() {
    std::lock_guard<std::mutex> guard(Lock);
    Workers.clear();
    NextWorker = 0;
}

TqLinuxRelayWorker* TqLinuxRelayRuntime::PickWorker() {
    std::lock_guard<std::mutex> guard(Lock);
    if (Workers.empty()) {
        return nullptr;
    }
    TqLinuxRelayWorker* worker = Workers[NextWorker % Workers.size()].get();
    ++NextWorker;
    return worker;
}

TqLinuxRelayWorkerSnapshot TqLinuxRelayRuntime::Snapshot() const {
    std::lock_guard<std::mutex> guard(Lock);
    TqLinuxRelayWorkerSnapshot total{};
    for (const auto& worker : Workers) {
        const auto snapshot = worker->Snapshot();
        total.EventsProcessed += snapshot.EventsProcessed;
        total.WakeupWrites += snapshot.WakeupWrites;
        total.PendingEvents += snapshot.PendingEvents;
        total.PendingBytes += snapshot.PendingBytes;
        total.TcpReadBatches += snapshot.TcpReadBatches;
        total.TcpReadBytes += snapshot.TcpReadBytes;
        total.QuicSendOperations += snapshot.QuicSendOperations;
        total.MaxTcpReadIovUsed = std::max(total.MaxTcpReadIovUsed, snapshot.MaxTcpReadIovUsed);
    }
    return total;
}
```

Implement `RegisterRelayWithId` by moving the existing `RegisterRelay` body into the new method. Assign `RelayState::Id = NextRelayId++` before `epoll_ctl`, return `{true, raw->Id}` after the relay is stored, and keep the test wrapper as:

```cpp
bool TqLinuxRelayWorker::RegisterRelay(const TqLinuxRelayRegistration& registration) {
    return RegisterRelayWithId(registration).Ok;
}
```

Add lifecycle and pending-write fields to `RelayState`:

```cpp
    uint64_t Id{0};
    bool Closing{false};
    std::deque<TqBufferView> PendingTcpWrites;
    bool TcpWriteShutdownQueued{false};
    bool TcpWriteArmed{false};
```

Implement `UnregisterRelay` so relay lifetime is actually removed from the worker:

```cpp
void TqLinuxRelayWorker::UnregisterRelay(uint64_t relayId) {
    std::unique_ptr<RelayState> removed;
    {
        std::lock_guard<std::mutex> guard(RelayLock);
        for (auto it = Relays.begin(); it != Relays.end(); ++it) {
            if ((*it)->Id == relayId) {
                removed = std::move(*it);
                Relays.erase(it);
                break;
            }
        }
    }
    if (!removed) {
        return;
    }
    removed->Closing = true;
    if (EpollFd >= 0 && removed->TcpFd >= 0) {
        ::epoll_ctl(EpollFd, EPOLL_CTL_DEL, removed->TcpFd, nullptr);
        ::shutdown(removed->TcpFd, SHUT_RDWR);
    }
    if (removed->Stream != nullptr && removed->Stream->Context == this) {
        removed->Stream->Callback = MsQuicStream::NoOpCallback;
        removed->Stream->Context = nullptr;
    }
    removed->PendingTcpWrites.clear();
}
```

If `OutstandingQuicSends` is non-zero during unregister, the send-completion client context must no longer dereference `RelayState`; store only pooled buffer refs and the relay id in the completion context, then ignore missing relay ids when completion arrives.

- [ ] **Step 4: Select Linux fast path in `TqRelayStart`**

In `src/relay.cpp`, include:

```cpp
#if defined(__linux__)
#include "linux_relay_worker.h"
#endif
```

In `src/CMakeLists.txt`, add the Linux worker sources to the product target on Linux after `TCPQUIC_PROXY_SOURCES` is defined and before `add_tcpquic_executable(tcpquic-proxy ...)`:

```cmake
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    list(APPEND TCPQUIC_PROXY_SOURCES
        linux_relay_buffer_pool.cpp
        linux_relay_worker.cpp
    )
endif()
```

Also add `linux_relay_buffer_pool.cpp` and `linux_relay_worker.cpp` to `tcpquic_tunnel_test` under the same Linux guard, because that target links `relay.cpp` directly.

Update `TqRelayStart` before constructing `TqTunnelRelay`:

```cpp
#if defined(__linux__)
    if (compressor == nullptr && decompressor == nullptr) {
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
        registration.EnableQuicSends = true;
        const auto registered = worker->RegisterRelayWithId(registration);
        if (!registered.Ok) {
            TqRelayUnregisterActive();
            return false;
        }
        handle->Backend = TqRelayBackendType::LinuxWorker;
        handle->LinuxWorker = worker;
        handle->LinuxRelayId = registered.RelayId;
        handle->Relay = nullptr;
        return true;
    }
#endif
```

Update `TqRelayStart` for the existing blocking path after `relay->Start()` succeeds:

```cpp
    handle->Backend = TqRelayBackendType::Blocking;
    handle->Relay = relay;
```

Update `TqRelayStop` before reading and clearing `handle->Relay`. The Linux branch must run first because Linux worker relays intentionally keep `handle->Relay == nullptr`:

```cpp
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
```

Only after that branch, run the old blocking cleanup:

```cpp
    auto* relay = handle->Relay;
    handle->Relay = nullptr;
    handle->Backend = TqRelayBackendType::None;
    if (relay != nullptr) {
        relay->Stop();
        delete relay;
        TqRelayUnregisterActive();
    } else {
        handle->Stop.store(true);
    }
```

Define the accessor:

```cpp
bool TqRelayLinuxFastPathEnabled(const TqRelayHandle* handle) {
#if defined(__linux__)
    return handle != nullptr && handle->Backend == TqRelayBackendType::LinuxWorker;
#else
    (void)handle;
    return false;
#endif
}
```

During implementation, never use `handle->Relay` to represent the Linux worker backend. `handle->Relay` remains owned by the blocking `TqTunnelRelay` path only.

- [ ] **Step 5: Wire callbacks without touching relay state on MsQuic threads**

In the Linux fast path registration, set the stream callback in `TqLinuxRelayWorker::RegisterRelayWithId`. The callback must enqueue worker events only; it must not mutate `RelayState`, write to TCP, or delete send contexts directly on the MsQuic callback thread.

```cpp
// Add static callback declarations to TqLinuxRelayWorker and implement:
QUIC_STATUS QUIC_API TqLinuxRelayWorker::StreamCallback(
    _In_ MsQuicStream* stream,
    _In_opt_ void* context,
    _Inout_ QUIC_STREAM_EVENT* event) noexcept {
    auto* worker = static_cast<TqLinuxRelayWorker*>(context);
    if (worker == nullptr) {
        return QUIC_STATUS_SUCCESS;
    }
    return worker->OnStreamEvent(stream, event);
}

QUIC_STATUS TqLinuxRelayWorker::OnStreamEvent(MsQuicStream* stream, QUIC_STREAM_EVENT* event) noexcept {
    if (event->Type == QUIC_STREAM_EVENT_SEND_COMPLETE) {
        TqLinuxRelayEvent queued{};
        queued.Type = TqLinuxRelayEventType::QuicSendComplete;
        queued.Value = reinterpret_cast<uintptr_t>(event->SEND_COMPLETE.ClientContext);
        Enqueue(queued);
        return QUIC_STATUS_SUCCESS;
    }
    if (event->Type == QUIC_STREAM_EVENT_RECEIVE) {
        const uint64_t relayId = FindRelayIdByStream(stream);
        if (relayId == 0) {
            return QUIC_STATUS_SUCCESS;
        }
        for (uint32_t i = 0; i < event->RECEIVE.BufferCount; ++i) {
            const auto& buffer = event->RECEIVE.Buffers[i];
            if (!CopyQuicReceiveToEvent(relayId, buffer.Buffer, buffer.Length, false)) {
                AbortRelayFromCallback(relayId, stream);
                return QUIC_STATUS_SUCCESS;
            }
        }
        if ((event->RECEIVE.Flags & QUIC_RECEIVE_FLAG_FIN) != 0) {
            TqLinuxRelayEvent fin{};
            fin.Type = TqLinuxRelayEventType::QuicReceive;
            fin.RelayId = relayId;
            fin.Fin = true;
            Enqueue(fin);
        }
        return QUIC_STATUS_SUCCESS;
    }
    if (event->Type == QUIC_STREAM_EVENT_SHUTDOWN_COMPLETE ||
        event->Type == QUIC_STREAM_EVENT_PEER_SEND_ABORTED ||
        event->Type == QUIC_STREAM_EVENT_PEER_RECEIVE_ABORTED) {
        const uint64_t relayId = FindRelayIdByStream(stream);
        if (relayId != 0) {
            TqLinuxRelayEvent shutdown{};
            shutdown.Type = TqLinuxRelayEventType::Shutdown;
            shutdown.RelayId = relayId;
            Enqueue(shutdown);
        }
        return QUIC_STATUS_SUCCESS;
    }
    return QUIC_STATUS_SUCCESS;
}
```

Add `RelayId` to `TqLinuxRelayEvent`:

```cpp
    uint64_t RelayId{0};
```

Add `FindRelayIdByStream`, `CopyQuicReceiveToEvent`, and `AbortRelayFromCallback`. `CopyQuicReceiveToEvent` may copy MsQuic receive bytes into pooled buffers under the pool mutex because MsQuic receive buffers are not safe to retain after callback return; the event must carry only owned `TqBufferRef` objects. The worker consumes those events later and appends them to `PendingTcpWrites`.

In `RegisterRelayWithId`, after epoll registration succeeds:

```cpp
    if (registration.Stream != nullptr) {
        registration.Stream->Callback = TqLinuxRelayWorker::StreamCallback;
        registration.Stream->Context = this;
    }
```

- [ ] **Step 6: Run relay and tunnel tests**

Run:

```bash
rtk cmake --build build-gcc10-fixed --target tcpquic_tunnel_test tcpquic_linux_relay_worker_io_test
rtk ./build-gcc10-fixed/bin/Release/tcpquic_tunnel_test
rtk ./build-gcc10-fixed/bin/Release/tcpquic_linux_relay_worker_io_test
```

Expected: PASS.

- [ ] **Step 7: Commit**

Run:

```bash
rtk git add src/relay.h src/relay.cpp src/linux_relay_worker.h src/linux_relay_worker.cpp src/unittest/tcp_tunnel_test.cpp src/CMakeLists.txt
rtk git commit -m "feat: route linux fast relays through epoll workers"
```

## Task 7: QUIC-to-TCP Copy-Into-Pool and writev

**Files:**
- Modify: `src/linux_relay_worker.h`
- Modify: `src/linux_relay_worker.cpp`
- Modify: `src/unittest/linux_relay_worker_io_test.cpp`

- [ ] **Step 1: Write the failing writev test**

Append this scenario before `return 0;` in `src/unittest/linux_relay_worker_io_test.cpp`:

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
        registration.EnableQuicSends = false;
        assert(worker.RegisterRelayForTest(registration));

        const uint8_t first[] = {1, 2, 3, 4};
        const uint8_t second[] = {5, 6, 7, 8, 9};
        assert(worker.EnqueueQuicReceiveForTest(fds[0], first, sizeof(first), false));
        assert(worker.EnqueueQuicReceiveForTest(fds[0], second, sizeof(second), true));

        uint8_t output[16]{};
        const ssize_t received = ::read(fds[1], output, sizeof(output));
        assert(received == 9);
        for (int i = 0; i < 9; ++i) {
            assert(output[i] == static_cast<uint8_t>(i + 1));
        }

        TqLinuxRelayWorkerSnapshot snapshot = worker.Snapshot();
        assert(snapshot.TcpWriteBatches >= 1);
        assert(snapshot.TcpWriteBytes == 9);
        assert(snapshot.MaxTcpWriteIovUsed >= 2);

        worker.Stop();
        ::close(fds[1]);
    }
```

- [ ] **Step 2: Run the test to verify it fails**

Run:

```bash
rtk cmake --build build-gcc10-fixed --target tcpquic_linux_relay_worker_io_test
```

Expected: build fails because `EnqueueQuicReceiveForTest`, `TcpWriteBatches`, `TcpWriteBytes`, and `MaxTcpWriteIovUsed` do not exist.

- [ ] **Step 3: Add QUIC receive queue API and metrics**

Add to `TqLinuxRelayWorkerSnapshot`:

```cpp
    uint64_t TcpWriteBatches{0};
    uint64_t TcpWriteBytes{0};
    uint64_t MaxTcpWriteIovUsed{0};
```

Add public test method:

```cpp
    bool EnqueueQuicReceiveForTest(int tcpFd, const uint8_t* data, size_t length, bool fin);
```

Ensure these Task 6 fields exist in `RelayState`:

```cpp
    std::deque<TqBufferView> PendingTcpWrites;
    bool TcpWriteShutdownQueued{false};
    bool TcpWriteArmed{false};
```

Add private methods and metrics:

```cpp
    RelayState* FindRelayByFd(int tcpFd);
    RelayState* FindRelayById(uint64_t relayId);
    bool EnqueueQuicReceive(RelayState* relay, const uint8_t* data, size_t length, bool fin);
    void ProcessQuicReceiveEvent(const TqLinuxRelayEvent& event);
    void FlushTcpWrites(RelayState* relay);
    void ArmTcpWritable(RelayState* relay, bool enabled);
    std::atomic<uint64_t> TcpWriteBatches{0};
    std::atomic<uint64_t> TcpWriteBytes{0};
    std::atomic<uint64_t> MaxTcpWriteIovUsed{0};
```

Replace the temporary Task 3 `Snapshot()` pending-byte placeholder with real relay accounting:

```cpp
    snapshot.PendingBytes = 0;
    {
        std::lock_guard<std::mutex> relayGuard(RelayLock);
        for (const auto& relay : Relays) {
            snapshot.PendingBytes += relay->Pool.PendingBytes();
            for (const auto& view : relay->PendingTcpWrites) {
                snapshot.PendingBytes += view.Len;
            }
        }
    }
```

Update `TqLinuxRelayRuntime::Snapshot()` to aggregate the new write metrics:

```cpp
        total.TcpWriteBatches += snapshot.TcpWriteBatches;
        total.TcpWriteBytes += snapshot.TcpWriteBytes;
        total.MaxTcpWriteIovUsed = std::max(total.MaxTcpWriteIovUsed, snapshot.MaxTcpWriteIovUsed);
```

- [ ] **Step 4: Implement copy-into-pool and writev**

Implement:

```cpp
TqLinuxRelayWorker::RelayState* TqLinuxRelayWorker::FindRelayByFd(int tcpFd) {
    std::lock_guard<std::mutex> guard(RelayLock);
    for (const auto& relay : Relays) {
        if (relay->TcpFd == tcpFd) {
            return relay.get();
        }
    }
    return nullptr;
}

bool TqLinuxRelayWorker::EnqueueQuicReceiveForTest(
    int tcpFd,
    const uint8_t* data,
    size_t length,
    bool fin) {
    RelayState* relay = FindRelayByFd(tcpFd);
    if (relay == nullptr) {
        return false;
    }
    if (!EnqueueQuicReceive(relay, data, length, fin)) {
        return false;
    }
    FlushTcpWrites(relay);
    return true;
}

bool TqLinuxRelayWorker::EnqueueQuicReceive(
    RelayState* relay,
    const uint8_t* data,
    size_t length,
    bool fin) {
    if (relay == nullptr || (length > 0 && data == nullptr)) {
        return false;
    }

    size_t offset = 0;
    while (offset < length) {
        auto buffer = relay->Pool.Acquire();
        if (!buffer) {
            return false;
        }
        const size_t chunk = std::min(buffer->Capacity(), length - offset);
        std::memcpy(buffer->Data(), data + offset, chunk);
        buffer->SetLength(chunk);
        relay->PendingTcpWrites.push_back(TqBufferView{buffer->Data(), chunk, buffer});
        offset += chunk;
    }
    if (fin) {
        relay->TcpWriteShutdownQueued = true;
    }
    return true;
}

void TqLinuxRelayWorker::FlushTcpWrites(RelayState* relay) {
    if (relay == nullptr) {
        return;
    }

    while (!relay->PendingTcpWrites.empty()) {
        std::vector<iovec> iov;
        iov.reserve(Config.MaxIov);
        for (const auto& view : relay->PendingTcpWrites) {
            if (iov.size() >= Config.MaxIov) {
                break;
            }
            iovec item{};
            item.iov_base = view.Data;
            item.iov_len = view.Len;
            iov.push_back(item);
        }

        const ssize_t sent = ::writev(relay->TcpFd, iov.data(), static_cast<int>(iov.size()));
        if (sent > 0) {
            size_t remaining = static_cast<size_t>(sent);
            TcpWriteBytes.fetch_add(static_cast<uint64_t>(sent));
            TcpWriteBatches.fetch_add(1);
            uint64_t previous = MaxTcpWriteIovUsed.load();
            while (previous < iov.size() &&
                   !MaxTcpWriteIovUsed.compare_exchange_weak(previous, iov.size())) {
            }
            while (remaining > 0 && !relay->PendingTcpWrites.empty()) {
                auto& front = relay->PendingTcpWrites.front();
                if (remaining >= front.Len) {
                    remaining -= front.Len;
                    relay->PendingTcpWrites.pop_front();
                } else {
                    front.Data += remaining;
                    front.Len -= remaining;
                    remaining = 0;
                }
            }
            continue;
        }
        if (sent < 0 && errno == EINTR) {
            continue;
        }
        if (sent < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            ArmTcpWritable(relay, true);
            break;
        }
        break;
    }

    if (relay->PendingTcpWrites.empty() && relay->TcpWriteShutdownQueued) {
        ::shutdown(relay->TcpFd, SHUT_WR);
        relay->TcpWriteShutdownQueued = false;
    }
    if (relay->PendingTcpWrites.empty()) {
        ArmTcpWritable(relay, false);
    }
}
```

Add `ArmTcpWritable`:

```cpp
void TqLinuxRelayWorker::ArmTcpWritable(RelayState* relay, bool enabled) {
    if (relay == nullptr || relay->TcpFd < 0 || EpollFd < 0 || relay->TcpWriteArmed == enabled) {
        return;
    }
    epoll_event event{};
    event.events = EPOLLIN | EPOLLRDHUP | EPOLLERR | (enabled ? EPOLLOUT : 0);
    event.data.ptr = relay;
    if (::epoll_ctl(EpollFd, EPOLL_CTL_MOD, relay->TcpFd, &event) == 0) {
        relay->TcpWriteArmed = enabled;
    }
}
```

Add `#include <cstring>` if not already present.

- [ ] **Step 5: Consume QUIC receive events on the worker**

Extend `DrainEvents` so it dispatches event types instead of just counting them:

```cpp
switch (event.Type) {
case TqLinuxRelayEventType::QuicReceive:
    ProcessQuicReceiveEvent(event);
    break;
case TqLinuxRelayEventType::QuicSendComplete:
    CompleteQuicSend(reinterpret_cast<void*>(event.Value));
    break;
case TqLinuxRelayEventType::Shutdown:
    UnregisterRelay(event.RelayId);
    break;
default:
    break;
}
```

Implement worker-side receive consumption:

```cpp
void TqLinuxRelayWorker::ProcessQuicReceiveEvent(const TqLinuxRelayEvent& event) {
    RelayState* relay = FindRelayById(event.RelayId);
    if (relay == nullptr || relay->Closing) {
        return;
    }
    if (event.Buffer) {
        relay->PendingTcpWrites.push_back(TqBufferView{
            event.Buffer->Data(),
            event.Length,
            event.Buffer});
    }
    if (event.Fin) {
        relay->TcpWriteShutdownQueued = true;
    }
    FlushTcpWrites(relay);
}
```

Update `Run` so `EPOLLOUT` events call `FlushTcpWrites(relay)` and `EPOLLIN` events call `DrainTcpReadable(relay)`. This is required because a slow TCP peer can return `EAGAIN`; pending write buffers must be retried when the socket becomes writable.

- [ ] **Step 6: Run writev and tunnel tests**

Run:

```bash
rtk cmake --build build-gcc10-fixed --target tcpquic_linux_relay_worker_io_test tcpquic_tunnel_test
rtk ./build-gcc10-fixed/bin/Release/tcpquic_linux_relay_worker_io_test
rtk ./build-gcc10-fixed/bin/Release/tcpquic_tunnel_test
```

Expected: PASS.

- [ ] **Step 7: Commit**

Run:

```bash
rtk git add src/linux_relay_worker.h src/linux_relay_worker.cpp src/unittest/linux_relay_worker_io_test.cpp
rtk git commit -m "feat: write quic receives from linux relay workers"
```

## Task 8: Backpressure and Fairness Budgets

**Files:**
- Modify: `src/linux_relay_worker.h`
- Modify: `src/linux_relay_worker.cpp`
- Modify: `src/unittest/linux_relay_worker_io_test.cpp`

- [ ] **Step 1: Write failing budget assertions**

Add this scenario before `return 0;` in `src/unittest/linux_relay_worker_io_test.cpp`:

```cpp
    {
        TqLinuxRelayWorkerConfig config{};
        config.EventBudget = 128;
        config.ReadChunkSize = 1024;
        config.ReadBatchBytes = 4096;
        config.MaxIov = 4;
        config.MaxPendingBytes = 2048;

        TqLinuxRelayWorker worker(config);
        assert(worker.Start());

        int fds[2]{-1, -1};
        assert(::socketpair(AF_UNIX, SOCK_STREAM, 0, fds) == 0);

        TqLinuxRelayRegistration registration{};
        registration.TcpFd = fds[0];
        registration.Stream = nullptr;
        registration.Handle = nullptr;
        registration.EnableQuicSends = false;
        assert(worker.RegisterRelayForTest(registration));

        std::vector<uint8_t> payload(8192, 0x11);
        assert(::write(fds[1], payload.data(), payload.size()) == static_cast<ssize_t>(payload.size()));
        assert(worker.WaitForObservedTcpBytesForTest(2048, 2000));

        TqLinuxRelayWorkerSnapshot snapshot = worker.Snapshot();
        assert(snapshot.TcpReadBytes <= 4096);
        assert(snapshot.ReadDisabledCount >= 1);

        worker.Stop();
        ::close(fds[1]);
    }
```

- [ ] **Step 2: Run the test to verify it fails**

Run:

```bash
rtk cmake --build build-gcc10-fixed --target tcpquic_linux_relay_worker_io_test
```

Expected: build fails because `ReadDisabledCount` does not exist.

- [ ] **Step 3: Add read disable metric and per-tick byte budget**

Add to `TqLinuxRelayWorkerSnapshot`:

```cpp
    uint64_t ReadDisabledCount{0};
```

Add private metric:

```cpp
    std::atomic<uint64_t> ReadDisabledCount{0};
```

Update `Snapshot()` to copy it, and update `TqLinuxRelayRuntime::Snapshot()` to aggregate it:

```cpp
        total.ReadDisabledCount += snapshot.ReadDisabledCount;
```

- [ ] **Step 4: Enforce pool budget and re-arm reads**

In `DrainTcpReadable`, before allocating iov buffers, add:

```cpp
        if (relay->Pool.PendingBytes() + Config.ReadChunkSize > relay->Pool.MaxPendingBytes()) {
            ReadDisabledCount.fetch_add(1);
            break;
        }
```

Cap each drain by worker byte budget:

```cpp
    const uint64_t tickBudget = std::min<uint64_t>(Config.ReadBatchBytes, Config.ByteBudgetPerTick);
    while (readBytes < tickBudget) {
```

Use `tickBudget` instead of `Config.ReadBatchBytes` in the iov allocation loop.

After `SubmitTcpBatchToQuic` returns false, set the handle stop flag if present:

```cpp
                if (relay->Handle != nullptr) {
                    relay->Handle->Stop.store(true);
                }
```

- [ ] **Step 5: Run budget tests**

Run:

```bash
rtk cmake --build build-gcc10-fixed --target tcpquic_linux_relay_worker_io_test
rtk ./build-gcc10-fixed/bin/Release/tcpquic_linux_relay_worker_io_test
```

Expected: PASS.

- [ ] **Step 6: Commit**

Run:

```bash
rtk git add src/linux_relay_worker.h src/linux_relay_worker.cpp src/unittest/linux_relay_worker_io_test.cpp
rtk git commit -m "feat: enforce linux relay worker budgets"
```

## Task 9: Metrics Exposure

**Files:**
- Modify: `src/server_metrics.h`
- Modify: `src/server_metrics.cpp`
- Modify: `src/unittest/router_runtime_test.cpp`

- [ ] **Step 1: Write failing metrics assertions**

In `src/unittest/router_runtime_test.cpp`, add assertions to the existing metrics response test. If there is no metrics response test, add this block before `return 0;`:

```cpp
    {
        TqServerMetrics metrics;
        const std::string body = TqServerMetricsJson(metrics, 0);
        assert(body.find("\"linux_relay_wakeups\"") != std::string::npos);
        assert(body.find("\"linux_relay_events_processed\"") != std::string::npos);
        assert(body.find("\"linux_relay_pending_events\"") != std::string::npos);
        assert(body.find("\"linux_relay_pending_bytes\"") != std::string::npos);
        assert(body.find("\"linux_relay_tcp_read_bytes\"") != std::string::npos);
        assert(body.find("\"linux_relay_tcp_write_bytes\"") != std::string::npos);
        assert(body.find("\"linux_relay_read_disabled_count\"") != std::string::npos);
    }
```

- [ ] **Step 2: Run the test to verify it fails**

Run:

```bash
rtk cmake --build build-gcc10-fixed --target tcpquic_router_runtime_test
```

Expected: build fails because `TqServerMetricsJson` does not include Linux relay metrics yet.

- [ ] **Step 3: Add Linux relay metrics fields**

Add to `TqServerMetrics` in `src/server_metrics.h` so tests and non-Linux builds have stable zero defaults:

```cpp
    uint64_t LinuxRelayWakeups{0};
    uint64_t LinuxRelayEventsProcessed{0};
    uint64_t LinuxRelayPendingEvents{0};
    uint64_t LinuxRelayPendingBytes{0};
    uint64_t LinuxRelayTcpReadBytes{0};
    uint64_t LinuxRelayTcpWriteBytes{0};
    uint64_t LinuxRelayReadDisabledCount{0};
```

- [ ] **Step 4: Populate metrics from runtime snapshot**

In `src/server_metrics.cpp`, include Linux runtime on Linux:

```cpp
#if defined(__linux__)
#include "linux_relay_worker.h"
#endif
```

In `TqServerMetricsJson`, take a Linux runtime snapshot and merge it with the passed metrics:

```cpp
#if defined(__linux__)
    const auto linuxRelay = TqLinuxRelayRuntime::Instance().Snapshot();
    const uint64_t linuxRelayWakeups = linuxRelay.WakeupWrites;
    const uint64_t linuxRelayEventsProcessed = linuxRelay.EventsProcessed;
    const uint64_t linuxRelayPendingEvents = linuxRelay.PendingEvents;
    const uint64_t linuxRelayPendingBytes = linuxRelay.PendingBytes;
    const uint64_t linuxRelayTcpReadBytes = linuxRelay.TcpReadBytes;
    const uint64_t linuxRelayTcpWriteBytes = linuxRelay.TcpWriteBytes;
    const uint64_t linuxRelayReadDisabledCount = linuxRelay.ReadDisabledCount;
#else
    const uint64_t linuxRelayWakeups = metrics.LinuxRelayWakeups;
    const uint64_t linuxRelayEventsProcessed = metrics.LinuxRelayEventsProcessed;
    const uint64_t linuxRelayPendingEvents = metrics.LinuxRelayPendingEvents;
    const uint64_t linuxRelayPendingBytes = metrics.LinuxRelayPendingBytes;
    const uint64_t linuxRelayTcpReadBytes = metrics.LinuxRelayTcpReadBytes;
    const uint64_t linuxRelayTcpWriteBytes = metrics.LinuxRelayTcpWriteBytes;
    const uint64_t linuxRelayReadDisabledCount = metrics.LinuxRelayReadDisabledCount;
#endif
```

In the JSON renderer, append fields before `last_error`:

```cpp
    out << ",\"linux_relay_wakeups\":" << linuxRelayWakeups;
    out << ",\"linux_relay_events_processed\":" << linuxRelayEventsProcessed;
    out << ",\"linux_relay_pending_events\":" << linuxRelayPendingEvents;
    out << ",\"linux_relay_pending_bytes\":" << linuxRelayPendingBytes;
    out << ",\"linux_relay_tcp_read_bytes\":" << linuxRelayTcpReadBytes;
    out << ",\"linux_relay_tcp_write_bytes\":" << linuxRelayTcpWriteBytes;
    out << ",\"linux_relay_read_disabled_count\":" << linuxRelayReadDisabledCount;
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
rtk git add src/server_metrics.h src/server_metrics.cpp src/unittest/router_runtime_test.cpp
rtk git commit -m "feat: expose linux relay worker metrics"
```

## Task 10: Regression and Linux-Only Documentation Note

**Files:**
- Modify: `src/thread_model_cn.md`
- Modify: `docs/tcpquic_next_steps.md`

- [ ] **Step 1: Update the thread model note**

Add this section near the existing future epoll discussion in `src/thread_model_cn.md`:

```markdown
## Linux Phase 5 relay worker status

Linux fast-path relays now use owner workers with `epoll`, `eventfd`, `readv`, pooled buffers, batched `StreamSend`, and worker-owned `writev` for QUIC receive data. Compressed relays keep the previous blocking relay implementation because compression output still uses separate contiguous buffers and requires a later compression-pool design.

Windows remains on the existing path in this phase. IOCP, WSARecv, and WSASend are not part of this Linux implementation.
```

- [ ] **Step 2: Update next steps**

Add this item to `docs/tcpquic_next_steps.md`:

```markdown
- Linux relay Phase E: validate MsQuic receive buffer lifetime and only then consider deferred receive views with explicit `StreamReceiveComplete`; keep copy-into-pool as the safe default until that test exists.
```

- [ ] **Step 3: Run all focused tests**

Run:

```bash
rtk cmake --build build-gcc10-fixed --target tcpquic_linux_relay_buffer_pool_test tcpquic_linux_relay_worker_queue_test tcpquic_linux_relay_worker_io_test tcpquic_tunnel_test tcpquic_tcp_write_queue_test tcpquic_tuning_test tcpquic_router_runtime_test
rtk ./build-gcc10-fixed/bin/Release/tcpquic_linux_relay_buffer_pool_test
rtk ./build-gcc10-fixed/bin/Release/tcpquic_linux_relay_worker_queue_test
rtk ./build-gcc10-fixed/bin/Release/tcpquic_linux_relay_worker_io_test
rtk ./build-gcc10-fixed/bin/Release/tcpquic_tunnel_test
rtk ./build-gcc10-fixed/bin/Release/tcpquic_tcp_write_queue_test
rtk ./build-gcc10-fixed/bin/Release/tcpquic_tuning_test
rtk ./build-gcc10-fixed/bin/Release/tcpquic_router_runtime_test
```

Expected: all listed tests PASS.

- [ ] **Step 4: Run broader build**

Run:

```bash
rtk cmake --build build-gcc10-fixed --target tcpquic-proxy
```

Expected: build succeeds.

- [ ] **Step 5: Commit**

Run:

```bash
rtk git add src/thread_model_cn.md docs/tcpquic_next_steps.md
rtk git commit -m "docs: record linux relay worker phase"
```

## Final Verification

- [ ] **Step 1: Run focused regression**

Run:

```bash
rtk cmake --build build-gcc10-fixed --target tcpquic_linux_relay_buffer_pool_test tcpquic_linux_relay_worker_queue_test tcpquic_linux_relay_worker_io_test tcpquic_tunnel_test tcpquic_http_connect_test tcpquic_socks5_test tcpquic_tcp_write_queue_test tcpquic_tuning_test tcpquic_router_runtime_test
rtk ./build-gcc10-fixed/bin/Release/tcpquic_linux_relay_buffer_pool_test
rtk ./build-gcc10-fixed/bin/Release/tcpquic_linux_relay_worker_queue_test
rtk ./build-gcc10-fixed/bin/Release/tcpquic_linux_relay_worker_io_test
rtk ./build-gcc10-fixed/bin/Release/tcpquic_tunnel_test
rtk ./build-gcc10-fixed/bin/Release/tcpquic_http_connect_test
rtk ./build-gcc10-fixed/bin/Release/tcpquic_socks5_test
rtk ./build-gcc10-fixed/bin/Release/tcpquic_tcp_write_queue_test
rtk ./build-gcc10-fixed/bin/Release/tcpquic_tuning_test
rtk ./build-gcc10-fixed/bin/Release/tcpquic_router_runtime_test
```

Expected: all tests PASS.

- [ ] **Step 2: Run product build**

Run:

```bash
rtk cmake --build build-gcc10-fixed --target tcpquic-proxy
```

Expected: build succeeds.

- [ ] **Step 3: Confirm Linux-only scope**

Run:

```bash
rtk rg -n "IOCP|WSARecv|WSASend|WSAPoll|Windows" src docs/superpowers/plans/2026-06-09-linux-high-performance-threading-and-batching.md
```

Expected: matches only documentation lines that explicitly state Windows/IOCP is out of scope; no implementation file introduces Windows backend code.

## Self-Review

- Spec coverage: Phase A through Phase D are covered by Tasks 1 through 10. Linux `epoll`, `eventfd` wake coalescing, `readv`, pooled buffer views, multi-buffer `StreamSend`, copy-into-pool QUIC receive, `writev`, backpressure budgets, fairness/tick budgets, and metrics are each assigned to concrete tasks. Phase E deferred receive views and Windows Phase F are explicitly excluded.
- Placeholder scan: The plan contains no unexpanded marker text and no unexpanded edge handling instruction. Each code-changing step includes concrete code or exact edits.
- Type consistency: The plan consistently uses `TqLinuxRelayBufferPool`, `TqBufferRef`, `TqBufferView`, `TqLinuxRelayWorker`, `TqLinuxRelayRuntime`, `TqLinuxRelayRegistration`, and `TqLinuxRelayWorkerSnapshot`.
