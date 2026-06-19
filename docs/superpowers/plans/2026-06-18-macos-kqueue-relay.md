# macOS kqueue Relay Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Add a production macOS/Darwin relay backend using `kqueue`, with feature parity to the Linux relay fast path while preserving existing Linux epoll and Windows IOCP behavior.

**Architecture:** Add a Darwin-specific relay worker/runtime selected from `relay.cpp`. The Darwin worker uses one `kqueue` per worker thread, `EVFILT_USER` for cross-thread wakeups, `EVFILT_READ/WRITE` for TCP readiness, and the same pending QUIC receive/backpressure semantics as the Linux worker.

**Tech Stack:** C++17, CMake, Darwin `kqueue`, POSIX sockets, MsQuic, zstd, existing relay buffer/alloc utilities, assert-style unit tests, local proxy integration scripts.

---

## File Map

- Create: `src/tunnel/darwin_relay_event_queue.h` — Darwin queue/event types equivalent to Linux event queue, kept separate for first implementation.
- Create: `src/tunnel/darwin_relay_worker.h` — Darwin runtime/worker public interface, config, registration, snapshot, and test hooks.
- Create: `src/tunnel/darwin_relay_worker.cpp` — `kqueue` worker implementation.
- Create: `src/unittest/darwin_relay_worker_queue_test.cpp` — Darwin event queue tests.
- Create: `src/unittest/darwin_relay_worker_io_test.cpp` — kqueue worker lifecycle/readiness tests.
- Modify: `src/tunnel/relay.h` — add `DarwinWorker` backend fields.
- Modify: `src/tunnel/relay.cpp` — select Darwin runtime on `__APPLE__`.
- Modify: `src/CMakeLists.txt` — add Darwin relay sources and tests.
- Modify: `src/runtime/relay_metrics.cpp` and related metrics files only if current metric aggregation needs explicit Darwin snapshot plumbing.
- Modify: `scripts/test-tcpquic-proxy.sh` only if macOS shell portability issues block integration validation.

## Prerequisites

- Run from macOS/Darwin for Darwin-only tests.
- Existing CMake configure should succeed:

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
```

- Existing Linux/Windows behavior must remain compile-time isolated. Avoid including `<sys/event.h>` outside `#if defined(__APPLE__)` or Darwin-only sources.

## Task 1: Add Darwin Backend Selection Skeleton

**Files:**
- Modify: `src/tunnel/relay.h`
- Modify: `src/tunnel/relay.cpp`
- Create: `src/tunnel/darwin_relay_worker.h`
- Create: `src/tunnel/darwin_relay_worker.cpp`
- Modify: `src/CMakeLists.txt`

- [ ] **Step 1: Add Darwin backend declarations**

In `src/tunnel/relay.h`, add a forward declaration and backend enum value:

```cpp
class TqDarwinRelayWorker;

enum class TqRelayBackendType {
    None,
    LinuxWorker,
    WindowsWorker,
    DarwinWorker,
};
```

Add Darwin fields to `TqRelayHandle`:

```cpp
TqDarwinRelayWorker* DarwinWorker{nullptr};
uint64_t DarwinRelayId{0};
```

- [ ] **Step 2: Create Darwin worker header skeleton**

Create `src/tunnel/darwin_relay_worker.h`:

```cpp
#pragma once

#if defined(__APPLE__)

#include "compress.h"
#include "platform_socket.h"
#include "relay.h"
#include "tuning.h"

#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

struct MsQuicStream;
struct QUIC_STREAM_EVENT;

struct TqDarwinRelayRegistration {
    TqSocketHandle TcpFd{TqInvalidSocket};
    MsQuicStream* Stream{nullptr};
    TqRelayHandle* Handle{nullptr};
    ITqCompressor* Compressor{nullptr};
    ITqDecompressor* Decompressor{nullptr};
    TqCompressAlgo CompressAlgo{TqCompressAlgo::None};
    bool EnableQuicSends{true};
};

struct TqDarwinRelayRegistrationResult {
    bool Ok{false};
    uint64_t RelayId{0};
};

struct TqDarwinRelayWorkerConfig {
    uint32_t WorkerIndex{0};
    uint32_t EventBudget{4096};
    uint64_t ByteBudgetPerTick{64ull * 1024 * 1024};
    size_t ReadChunkSize{128 * 1024};
    size_t ReadBatchBytes{1024 * 1024};
    uint32_t MaxIov{16};
    uint64_t TcpWriteMaxBytes{0};
    uint64_t MaxPendingQuicReceiveBytesPerRelay{0};
    uint64_t DeferredReceiveCompleteBatchBytes{0};
    uint32_t MaxInFlightQuicSends{0};
    uint64_t MaxBufferedQuicSendBytes{0};
    size_t EventQueueCapacity{4096};
};

struct TqDarwinRelayWorkerSnapshot {
    uint64_t EventsProcessed{0};
    uint64_t Wakeups{0};
    uint64_t PendingEvents{0};
    uint64_t ActiveRelays{0};
    uint64_t TcpReadArmedRelays{0};
    uint64_t TcpWriteArmedRelays{0};
    uint64_t QuicReceivePausedCount{0};
    uint64_t QuicReceiveResumedCount{0};
    uint64_t Errors{0};
};

class TqDarwinRelayWorker final {
public:
    explicit TqDarwinRelayWorker(const TqDarwinRelayWorkerConfig& config);
    ~TqDarwinRelayWorker();

    TqDarwinRelayWorker(const TqDarwinRelayWorker&) = delete;
    TqDarwinRelayWorker& operator=(const TqDarwinRelayWorker&) = delete;

    bool Start();
    void Stop();
    TqDarwinRelayRegistrationResult RegisterRelayWithId(const TqDarwinRelayRegistration& registration);
    void UnregisterRelay(uint64_t relayId);
    TqDarwinRelayWorkerSnapshot Snapshot() const;

    static QUIC_STATUS StreamCallback(MsQuicStream* stream, void* context, QUIC_STREAM_EVENT* event);

private:
    void Run();
    bool Wake();

    TqDarwinRelayWorkerConfig Config;
    int KqueueFd{-1};
    std::atomic<bool> Running{false};
    std::thread Thread;
    mutable std::mutex Mutex;
    uint64_t NextRelayId{1};
    std::atomic<uint64_t> Wakeups{0};
    std::atomic<uint64_t> Errors{0};
};

class TqDarwinRelayRuntime final {
public:
    static TqDarwinRelayRuntime& Instance();

    bool Start(const TqTuningConfig& tuning);
    void Stop();
    TqDarwinRelayWorker* PickWorker();
    void StopRelay(TqRelayHandle* handle);

private:
    TqDarwinRelayRuntime() = default;

    std::mutex Mutex;
    bool Started{false};
    uint32_t NextWorker{0};
    std::vector<std::unique_ptr<TqDarwinRelayWorker>> Workers;
};

#endif
```

- [ ] **Step 3: Create Darwin worker cpp skeleton**

Create `src/tunnel/darwin_relay_worker.cpp`:

```cpp
#if defined(__APPLE__)

#include "darwin_relay_worker.h"

#include <sys/event.h>
#include <sys/time.h>
#include <unistd.h>

#include <cerrno>

namespace {
constexpr uintptr_t kWakeIdent = 1;
}

TqDarwinRelayWorker::TqDarwinRelayWorker(const TqDarwinRelayWorkerConfig& config)
    : Config(config) {}

TqDarwinRelayWorker::~TqDarwinRelayWorker() {
    Stop();
}

bool TqDarwinRelayWorker::Start() {
    if (Running.load(std::memory_order_acquire)) {
        return true;
    }
    KqueueFd = kqueue();
    if (KqueueFd < 0) {
        Errors.fetch_add(1, std::memory_order_relaxed);
        return false;
    }
    struct kevent change;
    EV_SET(&change, kWakeIdent, EVFILT_USER, EV_ADD | EV_CLEAR, 0, 0, nullptr);
    if (kevent(KqueueFd, &change, 1, nullptr, 0, nullptr) != 0) {
        Errors.fetch_add(1, std::memory_order_relaxed);
        close(KqueueFd);
        KqueueFd = -1;
        return false;
    }
    Running.store(true, std::memory_order_release);
    Thread = std::thread(&TqDarwinRelayWorker::Run, this);
    return true;
}

void TqDarwinRelayWorker::Stop() {
    if (!Running.exchange(false, std::memory_order_acq_rel)) {
        return;
    }
    (void)Wake();
    if (Thread.joinable()) {
        Thread.join();
    }
    if (KqueueFd >= 0) {
        close(KqueueFd);
        KqueueFd = -1;
    }
}

bool TqDarwinRelayWorker::Wake() {
    if (KqueueFd < 0) {
        return false;
    }
    struct kevent change;
    EV_SET(&change, kWakeIdent, EVFILT_USER, 0, NOTE_TRIGGER, 0, nullptr);
    if (kevent(KqueueFd, &change, 1, nullptr, 0, nullptr) != 0) {
        Errors.fetch_add(1, std::memory_order_relaxed);
        return false;
    }
    Wakeups.fetch_add(1, std::memory_order_relaxed);
    return true;
}

void TqDarwinRelayWorker::Run() {
    struct kevent events[16];
    while (Running.load(std::memory_order_acquire)) {
        const int count = kevent(KqueueFd, nullptr, 0, events, 16, nullptr);
        if (count < 0) {
            if (errno == EINTR) {
                continue;
            }
            Errors.fetch_add(1, std::memory_order_relaxed);
            continue;
        }
        for (int i = 0; i < count; ++i) {
            if (events[i].filter == EVFILT_USER && events[i].ident == kWakeIdent) {
                continue;
            }
        }
    }
}

TqDarwinRelayRegistrationResult TqDarwinRelayWorker::RegisterRelayWithId(
    const TqDarwinRelayRegistration& registration) {
    (void)registration;
    return {};
}

void TqDarwinRelayWorker::UnregisterRelay(uint64_t relayId) {
    (void)relayId;
}

TqDarwinRelayWorkerSnapshot TqDarwinRelayWorker::Snapshot() const {
    TqDarwinRelayWorkerSnapshot snapshot{};
    snapshot.Wakeups = Wakeups.load(std::memory_order_relaxed);
    snapshot.Errors = Errors.load(std::memory_order_relaxed);
    return snapshot;
}

QUIC_STATUS TqDarwinRelayWorker::StreamCallback(
    MsQuicStream* stream,
    void* context,
    QUIC_STREAM_EVENT* event) {
    (void)stream;
    (void)context;
    (void)event;
    return QUIC_STATUS_SUCCESS;
}

TqDarwinRelayRuntime& TqDarwinRelayRuntime::Instance() {
    static TqDarwinRelayRuntime runtime;
    return runtime;
}

bool TqDarwinRelayRuntime::Start(const TqTuningConfig& tuning) {
    std::lock_guard<std::mutex> lock(Mutex);
    if (Started) {
        return true;
    }
    const uint32_t count = tuning.LinuxRelayWorkerCount == 0 ? 1 : tuning.LinuxRelayWorkerCount;
    Workers.reserve(count);
    for (uint32_t i = 0; i < count; ++i) {
        TqDarwinRelayWorkerConfig config{};
        config.WorkerIndex = i;
        config.EventQueueCapacity = tuning.LinuxRelayEventQueueCapacity;
        auto worker = std::make_unique<TqDarwinRelayWorker>(config);
        if (!worker->Start()) {
            Workers.clear();
            return false;
        }
        Workers.push_back(std::move(worker));
    }
    Started = true;
    return true;
}

void TqDarwinRelayRuntime::Stop() {
    std::lock_guard<std::mutex> lock(Mutex);
    for (auto& worker : Workers) {
        worker->Stop();
    }
    Workers.clear();
    Started = false;
    NextWorker = 0;
}

TqDarwinRelayWorker* TqDarwinRelayRuntime::PickWorker() {
    std::lock_guard<std::mutex> lock(Mutex);
    if (Workers.empty()) {
        return nullptr;
    }
    TqDarwinRelayWorker* worker = Workers[NextWorker % Workers.size()].get();
    ++NextWorker;
    return worker;
}

void TqDarwinRelayRuntime::StopRelay(TqRelayHandle* handle) {
    if (handle == nullptr || handle->DarwinWorker == nullptr || handle->DarwinRelayId == 0) {
        return;
    }
    handle->DarwinWorker->UnregisterRelay(handle->DarwinRelayId);
}

#endif
```

- [ ] **Step 4: Add Darwin sources to CMake**

In `src/CMakeLists.txt`, after the Linux source block, add:

```cmake
if(CMAKE_SYSTEM_NAME STREQUAL "Darwin")
    list(APPEND TCPQUIC_PROXY_SOURCES
        tunnel/relay_alloc.cpp
        tunnel/relay_buffer.cpp
        tunnel/darwin_relay_worker.cpp
    )
endif()
```

- [ ] **Step 5: Wire Darwin in `relay.cpp`**

Add include guard:

```cpp
#if defined(__APPLE__)
#include "darwin_relay_worker.h"
#endif
```

Add `__APPLE__` branch after Linux branch in `TqRelayStart`:

```cpp
#elif defined(__APPLE__)
    if (!TqDarwinRelayRuntime::Instance().Start(tuning)) {
        TqRelayUnregisterActive();
        return false;
    }

    TqDarwinRelayWorker* worker = TqDarwinRelayRuntime::Instance().PickWorker();
    if (worker == nullptr) {
        TqRelayUnregisterActive();
        return false;
    }

    TqDarwinRelayRegistration registration{};
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
    handle->Backend = TqRelayBackendType::DarwinWorker;
    handle->DarwinWorker = worker;
    handle->DarwinRelayId = registered.RelayId;
    return true;
```

Add Darwin stop branch in `TqRelayStop`:

```cpp
#if defined(__APPLE__)
    if (handle->Backend == TqRelayBackendType::DarwinWorker) {
        TqDarwinRelayWorker* worker = handle->DarwinWorker;
        const uint64_t relayId = handle->DarwinRelayId;
        handle->Backend = TqRelayBackendType::None;
        handle->DarwinWorker = nullptr;
        handle->DarwinRelayId = 0;
        handle->Stop.store(true);
        if (worker != nullptr && relayId != 0) {
            worker->UnregisterRelay(relayId);
        }
        TqRelayUnregisterActive();
        return;
    }
#endif
```

- [ ] **Step 6: Configure and build skeleton**

Run on macOS:

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --target tcpquic-proxy -j$(sysctl -n hw.ncpu)
```

Expected: compile succeeds or fails only on missing names from the skeleton. Fix compile errors by matching existing `TqTuningConfig` field names in `src/config/tuning.h`.

- [ ] **Step 7: Commit skeleton**

```bash
git add src/tunnel/relay.h src/tunnel/relay.cpp src/tunnel/darwin_relay_worker.h src/tunnel/darwin_relay_worker.cpp src/CMakeLists.txt
git commit -m "feat: add darwin relay backend skeleton"
```

## Task 2: Add Darwin Event Queue and Queue Tests

**Files:**
- Create: `src/tunnel/darwin_relay_event_queue.h`
- Create: `src/unittest/darwin_relay_worker_queue_test.cpp`
- Modify: `src/tunnel/darwin_relay_worker.h`
- Modify: `src/tunnel/darwin_relay_worker.cpp`
- Modify: `src/CMakeLists.txt`

- [ ] **Step 1: Create event queue header**

Create `src/tunnel/darwin_relay_event_queue.h` by copying the structure of `src/tunnel/linux_relay_event_queue.h` and renaming types:

```cpp
#pragma once

#include "relay_buffer.h"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

struct MsQuicStream;

struct TqDarwinQuicReceiveSlice {
    const uint8_t* Data{nullptr};
    uint32_t Length{0};
};

struct TqDarwinPendingQuicReceive {
    MsQuicStream* Stream{nullptr};
    uint64_t RelayId{0};
    std::vector<TqDarwinQuicReceiveSlice> Slices;
    size_t SliceIndex{0};
    size_t SliceOffset{0};
    uint64_t TotalLength{0};
    uint64_t CompletedLength{0};
    uint64_t PendingCompleteBytes{0};
    bool Fin{false};
};

enum class TqDarwinRelayEventType {
    TestMarker,
    TcpWritable,
    QuicReceive,
    QuicReceiveView,
    QuicSendComplete,
    QuicIdealSendBuffer,
    Shutdown,
    StopRelay,
};

struct TqDarwinRelayEvent {
    TqDarwinRelayEventType Type{TqDarwinRelayEventType::Shutdown};
    uint64_t Value{0};
    uint64_t RelayId{0};
    void* Relay{nullptr};
    TqBufferRef Buffer;
    std::vector<TqBufferRef> Buffers;
    size_t Length{0};
    size_t TotalLength{0};
    bool Fin{false};
    std::shared_ptr<TqDarwinPendingQuicReceive> ReceiveView;
};

class TqDarwinRelayEventQueue final {
public:
    explicit TqDarwinRelayEventQueue(size_t capacity)
        : CapacityValue(NormalizeCapacity(capacity)),
          Mask(CapacityValue - 1),
          Slots(new Cell[CapacityValue]) {
        for (size_t i = 0; i < CapacityValue; ++i) {
            Slots[i].Sequence.store(i, std::memory_order_relaxed);
        }
    }

    TqDarwinRelayEventQueue(const TqDarwinRelayEventQueue&) = delete;
    TqDarwinRelayEventQueue& operator=(const TqDarwinRelayEventQueue&) = delete;

    bool TryPush(TqDarwinRelayEvent&& event) {
        Cell* cell = nullptr;
        size_t position = EnqueuePos.load(std::memory_order_relaxed);
        for (;;) {
            cell = &Slots[position & Mask];
            const size_t sequence = cell->Sequence.load(std::memory_order_acquire);
            const intptr_t diff = static_cast<intptr_t>(sequence) - static_cast<intptr_t>(position);
            if (diff == 0) {
                if (EnqueuePos.compare_exchange_weak(
                        position,
                        position + 1,
                        std::memory_order_relaxed,
                        std::memory_order_relaxed)) {
                    break;
                }
            } else if (diff < 0) {
                return false;
            } else {
                position = EnqueuePos.load(std::memory_order_relaxed);
            }
        }
        cell->Event = std::move(event);
        cell->Sequence.store(position + 1, std::memory_order_release);
        return true;
    }

    bool TryPop(TqDarwinRelayEvent& event) {
        Cell* cell = nullptr;
        size_t position = DequeuePos.load(std::memory_order_relaxed);
        for (;;) {
            cell = &Slots[position & Mask];
            const size_t sequence = cell->Sequence.load(std::memory_order_acquire);
            const intptr_t diff = static_cast<intptr_t>(sequence) - static_cast<intptr_t>(position + 1);
            if (diff == 0) {
                if (DequeuePos.compare_exchange_weak(
                        position,
                        position + 1,
                        std::memory_order_relaxed,
                        std::memory_order_relaxed)) {
                    break;
                }
            } else if (diff < 0) {
                return false;
            } else {
                position = DequeuePos.load(std::memory_order_relaxed);
            }
        }
        event = std::move(cell->Event);
        cell->Event = TqDarwinRelayEvent{};
        cell->Sequence.store(position + CapacityValue, std::memory_order_release);
        return true;
    }

    size_t SizeApprox() const {
        const size_t enqueued = EnqueuePos.load(std::memory_order_acquire);
        const size_t dequeued = DequeuePos.load(std::memory_order_acquire);
        return enqueued >= dequeued ? enqueued - dequeued : 0;
    }

    size_t Capacity() const {
        return CapacityValue;
    }

private:
    struct Cell {
        std::atomic<size_t> Sequence{0};
        TqDarwinRelayEvent Event;
    };

    static size_t NormalizeCapacity(size_t capacity) {
        size_t normalized = 2;
        while (normalized < capacity) {
            normalized <<= 1;
        }
        return normalized;
    }

    const size_t CapacityValue;
    const size_t Mask;
    std::unique_ptr<Cell[]> Slots;
    alignas(64) std::atomic<size_t> EnqueuePos{0};
    alignas(64) std::atomic<size_t> DequeuePos{0};
};
```

- [ ] **Step 2: Add queue tests**

Create `src/unittest/darwin_relay_worker_queue_test.cpp`:

```cpp
#include "darwin_relay_event_queue.h"

#include <cassert>
#include <cstdint>

int main() {
    TqDarwinRelayEventQueue queue(3);
    assert(queue.Capacity() == 4);
    assert(queue.SizeApprox() == 0);

    for (uint64_t i = 1; i <= 4; ++i) {
        TqDarwinRelayEvent event{};
        event.Type = TqDarwinRelayEventType::TestMarker;
        event.Value = i;
        assert(queue.TryPush(std::move(event)));
    }

    TqDarwinRelayEvent overflow{};
    overflow.Type = TqDarwinRelayEventType::TestMarker;
    overflow.Value = 5;
    assert(!queue.TryPush(std::move(overflow)));

    for (uint64_t i = 1; i <= 4; ++i) {
        TqDarwinRelayEvent event{};
        assert(queue.TryPop(event));
        assert(event.Type == TqDarwinRelayEventType::TestMarker);
        assert(event.Value == i);
    }

    TqDarwinRelayEvent empty{};
    assert(!queue.TryPop(empty));
    return 0;
}
```

- [ ] **Step 3: Add queue to worker**

Include `darwin_relay_event_queue.h` in `darwin_relay_worker.h` and add member:

```cpp
TqDarwinRelayEventQueue EventQueue;
```

Initialize it in the constructor:

```cpp
TqDarwinRelayWorker::TqDarwinRelayWorker(const TqDarwinRelayWorkerConfig& config)
    : Config(config), EventQueue(config.EventQueueCapacity) {}
```

Add private helper declarations:

```cpp
bool EnqueueEvent(TqDarwinRelayEvent&& event);
uint32_t DrainEvents(uint32_t budget);
```

Add implementations:

```cpp
bool TqDarwinRelayWorker::EnqueueEvent(TqDarwinRelayEvent&& event) {
    if (!EventQueue.TryPush(std::move(event))) {
        Errors.fetch_add(1, std::memory_order_relaxed);
        return false;
    }
    return Wake();
}

uint32_t TqDarwinRelayWorker::DrainEvents(uint32_t budget) {
    uint32_t processed = 0;
    TqDarwinRelayEvent event{};
    while (processed < budget && EventQueue.TryPop(event)) {
        ++processed;
        if (event.Type == TqDarwinRelayEventType::Shutdown) {
            Running.store(false, std::memory_order_release);
        }
    }
    return processed;
}
```

In `Run()`, replace the user-event no-op with:

```cpp
(void)DrainEvents(Config.EventBudget);
```

- [ ] **Step 4: Add Darwin queue test target**

In `src/CMakeLists.txt`, add inside a Darwin-only block near other unit tests:

```cmake
if(CMAKE_SYSTEM_NAME STREQUAL "Darwin")
    add_executable(tcpquic_darwin_relay_worker_queue_test
        unittest/darwin_relay_worker_queue_test.cpp)
    tcpquic_target_include_dirs(tcpquic_darwin_relay_worker_queue_test)
    target_link_libraries(tcpquic_darwin_relay_worker_queue_test PRIVATE libzstd spdlog::spdlog)
    set_property(TARGET tcpquic_darwin_relay_worker_queue_test PROPERTY FOLDER "tools")
endif()
```

- [ ] **Step 5: Build and run queue test**

Run:

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --target tcpquic_darwin_relay_worker_queue_test -j$(sysctl -n hw.ncpu)
./build/bin/Release/tcpquic_darwin_relay_worker_queue_test
```

Expected: command exits 0.

- [ ] **Step 6: Commit queue**

```bash
git add src/tunnel/darwin_relay_event_queue.h src/tunnel/darwin_relay_worker.h src/tunnel/darwin_relay_worker.cpp src/unittest/darwin_relay_worker_queue_test.cpp src/CMakeLists.txt
git commit -m "feat: add darwin relay event queue"
```

## Task 3: Implement kqueue Worker Lifecycle and TCP Readiness Tests

**Files:**
- Modify: `src/tunnel/darwin_relay_worker.h`
- Modify: `src/tunnel/darwin_relay_worker.cpp`
- Create: `src/unittest/darwin_relay_worker_io_test.cpp`
- Modify: `src/CMakeLists.txt`

- [ ] **Step 1: Add a Darwin worker lifecycle test**

Create `src/unittest/darwin_relay_worker_io_test.cpp`:

```cpp
#include "darwin_relay_worker.h"

#include <cassert>
#include <sys/socket.h>
#include <unistd.h>

int main() {
    TqDarwinRelayWorkerConfig config{};
    config.WorkerIndex = 0;
    config.EventQueueCapacity = 16;
    TqDarwinRelayWorker worker(config);
    assert(worker.Start());
    auto snapshot = worker.Snapshot();
    assert(snapshot.Errors == 0);
    worker.Stop();

    int fds[2]{-1, -1};
    assert(socketpair(AF_UNIX, SOCK_STREAM, 0, fds) == 0);
    close(fds[0]);
    close(fds[1]);
    return 0;
}
```

- [ ] **Step 2: Add test target**

In the Darwin-only CMake test block:

```cmake
add_executable(tcpquic_darwin_relay_worker_io_test
    unittest/darwin_relay_worker_io_test.cpp
    tunnel/darwin_relay_worker.cpp
    tunnel/relay_alloc.cpp
    tunnel/relay_buffer.cpp)
tcpquic_target_include_dirs(tcpquic_darwin_relay_worker_io_test)
target_link_libraries(tcpquic_darwin_relay_worker_io_test PRIVATE Threads::Threads libzstd spdlog::spdlog msquic logging base_link)
set_property(TARGET tcpquic_darwin_relay_worker_io_test PROPERTY FOLDER "tools")
```

- [ ] **Step 3: Add relay state shell**

In `darwin_relay_worker.cpp`, add an internal `RelayState` with enough fields for readiness registration:

```cpp
struct TqDarwinRelayWorker::RelayState {
    uint64_t Id{0};
    TqSocketHandle TcpFd{TqInvalidSocket};
    MsQuicStream* Stream{nullptr};
    TqRelayHandle* PublicHandle{nullptr};
    ITqCompressor* Compressor{nullptr};
    ITqDecompressor* Decompressor{nullptr};
    TqCompressAlgo CompressAlgo{TqCompressAlgo::None};
    bool TcpReadArmed{false};
    bool TcpWriteArmed{false};
    bool Closing{false};
};
```

Add private members:

```cpp
std::unordered_map<uint64_t, std::shared_ptr<RelayState>> Relays;
```

Add private helpers:

```cpp
bool RegisterTcpFilters(const std::shared_ptr<RelayState>& relay);
bool UpdateTcpInterest(const std::shared_ptr<RelayState>& relay);
void RemoveTcpFilters(const std::shared_ptr<RelayState>& relay);
std::shared_ptr<RelayState> FindRelay(uint64_t relayId);
```

- [ ] **Step 4: Implement `RegisterRelayWithId` for TCP filter registration**

Use this shape:

```cpp
TqDarwinRelayRegistrationResult TqDarwinRelayWorker::RegisterRelayWithId(
    const TqDarwinRelayRegistration& registration) {
    if (registration.TcpFd == TqInvalidSocket || registration.Stream == nullptr || registration.Handle == nullptr) {
        return {};
    }
    if (!TqSetNonBlocking(registration.TcpFd)) {
        Errors.fetch_add(1, std::memory_order_relaxed);
        return {};
    }

    auto relay = std::make_shared<RelayState>();
    {
        std::lock_guard<std::mutex> lock(Mutex);
        relay->Id = NextRelayId++;
        relay->TcpFd = registration.TcpFd;
        relay->Stream = registration.Stream;
        relay->PublicHandle = registration.Handle;
        relay->Compressor = registration.Compressor;
        relay->Decompressor = registration.Decompressor;
        relay->CompressAlgo = registration.CompressAlgo;
        relay->TcpReadArmed = true;
        Relays.emplace(relay->Id, relay);
    }

    if (!RegisterTcpFilters(relay)) {
        std::lock_guard<std::mutex> lock(Mutex);
        Relays.erase(relay->Id);
        return {};
    }
    return {true, relay->Id};
}
```

- [ ] **Step 5: Implement kqueue filter helpers**

Implement with relay id stored in `udata`:

```cpp
bool TqDarwinRelayWorker::RegisterTcpFilters(const std::shared_ptr<RelayState>& relay) {
    struct kevent changes[2];
    EV_SET(&changes[0], static_cast<uintptr_t>(relay->TcpFd), EVFILT_READ,
        EV_ADD | EV_ENABLE, 0, 0, reinterpret_cast<void*>(relay->Id));
    EV_SET(&changes[1], static_cast<uintptr_t>(relay->TcpFd), EVFILT_WRITE,
        EV_ADD | EV_DISABLE, 0, 0, reinterpret_cast<void*>(relay->Id));
    if (kevent(KqueueFd, changes, 2, nullptr, 0, nullptr) != 0) {
        Errors.fetch_add(1, std::memory_order_relaxed);
        return false;
    }
    return true;
}

bool TqDarwinRelayWorker::UpdateTcpInterest(const std::shared_ptr<RelayState>& relay) {
    struct kevent changes[2];
    EV_SET(&changes[0], static_cast<uintptr_t>(relay->TcpFd), EVFILT_READ,
        relay->TcpReadArmed ? EV_ENABLE : EV_DISABLE, 0, 0, reinterpret_cast<void*>(relay->Id));
    EV_SET(&changes[1], static_cast<uintptr_t>(relay->TcpFd), EVFILT_WRITE,
        relay->TcpWriteArmed ? EV_ENABLE : EV_DISABLE, 0, 0, reinterpret_cast<void*>(relay->Id));
    if (kevent(KqueueFd, changes, 2, nullptr, 0, nullptr) != 0) {
        Errors.fetch_add(1, std::memory_order_relaxed);
        return false;
    }
    return true;
}
```

- [ ] **Step 6: Dispatch TCP readiness in `Run()`**

Add helper declarations:

```cpp
void ProcessKqueueEvent(const struct kevent& event);
void ProcessTcpEvents(uint64_t relayId, int16_t filter, uint16_t flags, intptr_t data);
```

Use:

```cpp
void TqDarwinRelayWorker::ProcessKqueueEvent(const struct kevent& event) {
    if (event.filter == EVFILT_USER && event.ident == kWakeIdent) {
        (void)DrainEvents(Config.EventBudget);
        return;
    }
    const uint64_t relayId = reinterpret_cast<uint64_t>(event.udata);
    ProcessTcpEvents(relayId, event.filter, event.flags, event.data);
}
```

- [ ] **Step 7: Build and run lifecycle test**

Run:

```bash
cmake --build build --target tcpquic_darwin_relay_worker_io_test -j$(sysctl -n hw.ncpu)
./build/bin/Release/tcpquic_darwin_relay_worker_io_test
```

Expected: command exits 0.

- [ ] **Step 8: Commit lifecycle/readiness skeleton**

```bash
git add src/tunnel/darwin_relay_worker.h src/tunnel/darwin_relay_worker.cpp src/unittest/darwin_relay_worker_io_test.cpp src/CMakeLists.txt
git commit -m "feat: add darwin kqueue relay worker lifecycle"
```

## Task 4: Port TCP to QUIC Send Path

**Files:**
- Modify: `src/tunnel/darwin_relay_worker.h`
- Modify: `src/tunnel/darwin_relay_worker.cpp`
- Modify: `src/unittest/darwin_relay_worker_io_test.cpp`

- [ ] **Step 1: Add failing TCP read accounting test**

Extend `darwin_relay_worker_io_test.cpp` with a socketpair registration test that writes bytes to one socket and verifies the worker observes read readiness. Use a test-only snapshot counter `TcpReadBytes`:

```cpp
const char payload[] = "darwin-readiness";
assert(write(fds[0], payload, sizeof(payload) - 1) == static_cast<ssize_t>(sizeof(payload) - 1));
```

Expected before implementation: `TcpReadBytes` remains 0.

- [ ] **Step 2: Add send operation and buffer fields**

In `darwin_relay_worker.h`, add a Darwin send operation equivalent to Linux:

```cpp
struct TqDarwinRelaySendOperation {
    static constexpr uint64_t MagicValue = 0x545144415257534eULL;
    uint64_t Magic{MagicValue};
    uint64_t RelayId{0};
    uint64_t TotalBytes{0};
    bool Fin{false};
    std::vector<TqBufferView> Views;
    std::vector<QUIC_BUFFER> QuicBuffers;
};
```

In `RelayState`, add:

```cpp
TqRelayBufferPool TcpReadBuffers;
uint64_t InFlightQuicSends{0};
uint64_t InFlightQuicSendBytes{0};
std::deque<std::unique_ptr<TqDarwinRelaySendOperation>> PendingQuicSends;
```

Initialize `TcpReadBuffers` from tuning using the same relay buffer/alloc conventions as Linux.

- [ ] **Step 3: Implement `DrainTcpReadable`**

Add helper:

```cpp
bool DrainTcpReadable(const std::shared_ptr<RelayState>& relay);
```

Implementation strategy:

1. Acquire `TqBufferRef` owners sized by `Config.ReadChunkSize`.
2. Read with `readv` until would-block, EOF, byte budget, or buffer budget.
3. On positive bytes, build QUIC send views.
4. On `EAGAIN`/`EWOULDBLOCK`, stop normally.
5. On `readv == 0`, submit QUIC FIN after flushing compressor state.
6. On hard error, close the relay.

- [ ] **Step 4: Implement `SubmitTcpBatchToQuic`**

Add helper:

```cpp
bool SubmitTcpBatchToQuic(
    const std::shared_ptr<RelayState>& relay,
    std::vector<TqBufferRef>&& buffers,
    bool fin);
```

The helper should:

1. Build `TqDarwinRelaySendOperation`.
2. Populate `QUIC_BUFFER` views.
3. Call `relay->Stream->Send(...)` using the same MsQuic API shape as Linux.
4. On success, increment in-flight counters.
5. On transient backpressure, queue to `PendingQuicSends` and arm retry through event queue.
6. On fatal error, close the relay.

- [ ] **Step 5: Handle QUIC send complete callback**

In `StreamCallback`, handle `QUIC_STREAM_EVENT_SEND_COMPLETE`:

```cpp
TqDarwinRelayEvent event{};
event.Type = TqDarwinRelayEventType::QuicSendComplete;
event.Value = reinterpret_cast<uint64_t>(event->SEND_COMPLETE.ClientContext);
worker->EnqueueEvent(std::move(event));
```

In `DrainEvents`, dispatch to:

```cpp
void CompleteQuicSend(TqDarwinRelaySendOperation* operation);
```

`CompleteQuicSend` should validate `Magic`, decrement in-flight counters, delete operation, retry pending sends, and resume TCP reads if backpressure cleared.

- [ ] **Step 6: Implement TCP read backpressure**

Add helpers:

```cpp
bool ShouldPauseTcpReadForQuicBacklog(const std::shared_ptr<RelayState>& relay) const;
bool ShouldResumeTcpReadForQuicBacklog(const std::shared_ptr<RelayState>& relay) const;
void SetTcpReadBackpressure(const std::shared_ptr<RelayState>& relay, bool paused);
```

When paused, set `TcpReadArmed = false` and call `UpdateTcpInterest`. When resumed, set `TcpReadArmed = true`.

- [ ] **Step 7: Run TCP-to-QUIC tests**

Run:

```bash
cmake --build build --target tcpquic_darwin_relay_worker_io_test -j$(sysctl -n hw.ncpu)
./build/bin/Release/tcpquic_darwin_relay_worker_io_test
```

Expected: TCP readiness and read accounting test passes.

- [ ] **Step 8: Commit TCP-to-QUIC path**

```bash
git add src/tunnel/darwin_relay_worker.h src/tunnel/darwin_relay_worker.cpp src/unittest/darwin_relay_worker_io_test.cpp
git commit -m "feat: add darwin tcp to quic relay path"
```

## Task 5: Port QUIC to TCP Pending Receive Path

**Files:**
- Modify: `src/tunnel/darwin_relay_worker.h`
- Modify: `src/tunnel/darwin_relay_worker.cpp`
- Modify: `src/unittest/darwin_relay_worker_io_test.cpp`

- [ ] **Step 1: Add failing pending receive test**

Add a test that invokes `TqDarwinRelayWorker::StreamCallback` with `QUIC_STREAM_EVENT_RECEIVE` and asserts it returns `QUIC_STATUS_PENDING`.

Expected before implementation: callback returns success or does not queue receive.

- [ ] **Step 2: Add pending receive state to relay**

In `RelayState`, add:

```cpp
std::deque<std::shared_ptr<TqDarwinPendingQuicReceive>> PendingQuicReceives;
uint64_t PendingQuicReceiveBytes{0};
bool QuicReceivePaused{false};
std::deque<TqBufferRef> PendingTcpWrites;
uint64_t PendingTcpWriteBytes{0};
```

- [ ] **Step 3: Implement `QueueDeferredQuicReceive`**

Add helper:

```cpp
bool QueueDeferredQuicReceive(
    const std::shared_ptr<RelayState>& relay,
    MsQuicStream* stream,
    const QUIC_BUFFER* buffers,
    uint32_t bufferCount,
    bool fin);
```

It should copy slice metadata into `TqDarwinPendingQuicReceive`, update pending byte counters, enqueue `QuicReceiveView`, wake worker, and return true. The stream callback returns `QUIC_STATUS_PENDING` only after this succeeds.

- [ ] **Step 4: Implement receive processing on worker**

Add helpers:

```cpp
void ProcessQuicReceiveViewEvent(const std::shared_ptr<TqDarwinPendingQuicReceive>& receive);
bool EnqueueQuicReceiveForTcp(const std::shared_ptr<RelayState>& relay, const std::shared_ptr<TqDarwinPendingQuicReceive>& receive);
bool FlushTcpWrites(const std::shared_ptr<RelayState>& relay);
void CompleteDeferredQuicReceive(const std::shared_ptr<RelayState>& relay, const std::shared_ptr<TqDarwinPendingQuicReceive>& receive);
```

For non-compressed traffic, build TCP write buffers from receive slices. For zstd traffic, call the existing decompressor on the worker thread and enqueue worker-owned output buffers.

- [ ] **Step 5: Implement Darwin no-SIGPIPE send helper**

Add local helpers in `darwin_relay_worker.cpp`:

```cpp
bool SetNoSigPipe(TqSocketHandle fd) {
    int enabled = 1;
    return setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &enabled, sizeof(enabled)) == 0;
}

ssize_t SendMsgNoSignal(TqSocketHandle fd, const struct msghdr* msg) {
    return sendmsg(fd, msg, 0);
}
```

Call `SetNoSigPipe` during relay registration. Treat failure as non-fatal only if `errno == ENOPROTOOPT`; on Darwin it should normally succeed.

- [ ] **Step 6: Implement QUIC receive pause/resume**

Add helpers:

```cpp
void SetQuicReceiveEnabled(const std::shared_ptr<RelayState>& relay, bool enabled);
void MaybePauseQuicReceive(const std::shared_ptr<RelayState>& relay);
void MaybeResumeQuicReceive(const std::shared_ptr<RelayState>& relay);
```

Use `Config.MaxPendingQuicReceiveBytesPerRelay` as pause threshold. Resume after pending bytes drop below half the threshold or the existing Linux resume threshold if one exists in tuning.

- [ ] **Step 7: Run pending receive tests**

Run:

```bash
cmake --build build --target tcpquic_darwin_relay_worker_io_test -j$(sysctl -n hw.ncpu)
./build/bin/Release/tcpquic_darwin_relay_worker_io_test
```

Expected: receive callback returns `QUIC_STATUS_PENDING`, TCP peer receives expected bytes, and pending receive counters drain to zero.

- [ ] **Step 8: Commit QUIC-to-TCP path**

```bash
git add src/tunnel/darwin_relay_worker.h src/tunnel/darwin_relay_worker.cpp src/unittest/darwin_relay_worker_io_test.cpp
git commit -m "feat: add darwin quic to tcp pending receive path"
```

## Task 6: Implement Close, Drain, and Lifetime Safety

**Files:**
- Modify: `src/tunnel/darwin_relay_worker.h`
- Modify: `src/tunnel/darwin_relay_worker.cpp`
- Modify: `src/unittest/darwin_relay_worker_io_test.cpp`

- [ ] **Step 1: Add failing stop safety test**

Add a test that registers a relay, calls `UnregisterRelay`, writes to the peer socket, wakes the worker, and asserts no crash and no active relay remains in snapshot.

Expected before implementation: active relay may remain or stale event handling is unsafe.

- [ ] **Step 2: Add retired state**

Add members:

```cpp
std::vector<std::shared_ptr<RelayState>> RetiredRelays;
```

In `RelayState`, add:

```cpp
bool TcpReadClosed{false};
bool TcpWriteClosed{false};
bool QuicSendClosed{false};
bool QuicReceiveClosed{false};
```

- [ ] **Step 3: Implement filter removal and close**

Implement:

```cpp
void RemoveTcpFilters(const std::shared_ptr<RelayState>& relay) {
    if (relay->TcpFd == TqInvalidSocket) {
        return;
    }
    struct kevent changes[2];
    EV_SET(&changes[0], static_cast<uintptr_t>(relay->TcpFd), EVFILT_READ, EV_DELETE, 0, 0, nullptr);
    EV_SET(&changes[1], static_cast<uintptr_t>(relay->TcpFd), EVFILT_WRITE, EV_DELETE, 0, 0, nullptr);
    (void)kevent(KqueueFd, changes, 2, nullptr, 0, nullptr);
}
```

Implement `CloseRelay` to mark closing, remove filters, close socket via `TqCloseSocket`, complete pending receives, and move relay to `RetiredRelays`.

- [ ] **Step 4: Make late events safe**

Every event dispatch should call `FindRelay(relayId)` and return if no live relay exists. If an event owns a receive view or send operation, release/complete that ownership before returning.

- [ ] **Step 5: Implement runtime stop**

Ensure `TqDarwinRelayRuntime::Stop()` stops all workers, workers close all relays, and `TqRelayStop` clears public handle fields exactly once.

- [ ] **Step 6: Run close tests**

Run:

```bash
cmake --build build --target tcpquic_darwin_relay_worker_io_test -j$(sysctl -n hw.ncpu)
./build/bin/Release/tcpquic_darwin_relay_worker_io_test
```

Expected: stop safety test passes with no crash and active relay count returns to zero.

- [ ] **Step 7: Commit lifetime handling**

```bash
git add src/tunnel/darwin_relay_worker.h src/tunnel/darwin_relay_worker.cpp src/unittest/darwin_relay_worker_io_test.cpp
git commit -m "feat: harden darwin relay close handling"
```

## Task 7: Add Metrics Snapshot and Backend Tests

**Files:**
- Modify: `src/tunnel/darwin_relay_worker.h`
- Modify: `src/tunnel/darwin_relay_worker.cpp`
- Modify: `src/runtime/relay_metrics.cpp` if required
- Modify: `src/unittest/darwin_relay_worker_io_test.cpp`
- Modify: `src/unittest/relay_backend_selection_test.cpp` if the existing test can be extended safely

- [ ] **Step 1: Complete Darwin snapshot fields**

Expand `TqDarwinRelayWorkerSnapshot` to include the Linux-equivalent fields used by admin metrics:

```cpp
uint64_t PendingBytes{0};
uint64_t CurrentPendingQuicReceiveBytes{0};
uint64_t PendingTcpWriteQueue{0};
uint64_t PendingTcpWriteBytes{0};
uint64_t OutstandingQuicSends{0};
uint64_t OutstandingQuicSendBytes{0};
uint64_t TcpReadBatches{0};
uint64_t TcpReadBytes{0};
uint64_t TcpWriteBatches{0};
uint64_t TcpWriteBytes{0};
uint64_t QuicReceiveViewCount{0};
uint64_t QuicReceiveViewBytes{0};
uint64_t DeferredReceiveCompletes{0};
uint64_t QuicSendBackpressureEvents{0};
```

- [ ] **Step 2: Aggregate snapshot under lock**

In `Snapshot()`, iterate live relays under `Mutex`, aggregate counters, and include `EventQueue.SizeApprox()`.

- [ ] **Step 3: Add backend selection test**

On Darwin, add an assertion that a successful relay registration sets:

```cpp
assert(handle.Backend == TqRelayBackendType::DarwinWorker);
assert(handle.DarwinWorker != nullptr);
assert(handle.DarwinRelayId != 0);
```

If existing `relay_backend_selection_test.cpp` is Linux-only, create Darwin coverage in `darwin_relay_worker_io_test.cpp` instead.

- [ ] **Step 4: Run metric/backend tests**

Run:

```bash
cmake --build build --target tcpquic_darwin_relay_worker_io_test -j$(sysctl -n hw.ncpu)
./build/bin/Release/tcpquic_darwin_relay_worker_io_test
```

Expected: snapshot assertions and backend selection assertions pass.

- [ ] **Step 5: Commit metrics/backend tests**

```bash
git add src/tunnel/darwin_relay_worker.h src/tunnel/darwin_relay_worker.cpp src/unittest/darwin_relay_worker_io_test.cpp src/runtime/relay_metrics.cpp src/unittest/relay_backend_selection_test.cpp
git commit -m "feat: expose darwin relay metrics"
```

If `src/runtime/relay_metrics.cpp` or `src/unittest/relay_backend_selection_test.cpp` were not changed, omit them from `git add`.

## Task 8: macOS Integration Validation and Script Portability

**Files:**
- Modify: `scripts/test-tcpquic-proxy.sh` only if needed
- Modify: `README.md` only if documenting macOS build/run becomes necessary

- [ ] **Step 1: Build proxy**

Run:

```bash
cmake --build build --target tcpquic-proxy -j$(sysctl -n hw.ncpu)
```

Expected: `build/bin/Release/tcpquic-proxy` exists.

- [ ] **Step 2: Run unit tests**

Run:

```bash
cmake --build build --target tcpquic_darwin_relay_worker_queue_test tcpquic_darwin_relay_worker_io_test -j$(sysctl -n hw.ncpu)
./build/bin/Release/tcpquic_darwin_relay_worker_queue_test
./build/bin/Release/tcpquic_darwin_relay_worker_io_test
```

Expected: both test executables exit 0.

- [ ] **Step 3: Run local proxy integration with compression off**

Run:

```bash
COMPRESS=off ./scripts/test-tcpquic-proxy.sh
```

Expected: SOCKS5, HTTP CONNECT, ACL/DNS/refused cases, and multi-connection checks pass. If the script fails only because of Linux-only shell commands such as `nproc`, patch the script with Darwin equivalents and rerun.

- [ ] **Step 4: Run local proxy integration with zstd**

Run:

```bash
COMPRESS=zstd ./scripts/test-tcpquic-proxy.sh
```

Expected: zstd end-to-end relay passes.

- [ ] **Step 5: Run built-in speed tests**

Run the documented local built-in download/upload speed paths using `build/bin/Release/tcpquic-proxy` and the certificates under `cert/`. Expected: both download and upload tests complete without relay failure and without unbounded pending receive metrics.

- [ ] **Step 6: Run Linux/Windows compile guard check if available**

If Linux or Windows builders are available, run their existing configure/build commands. If not available, inspect CMake guards and includes to confirm Darwin-only files are not pulled into non-Darwin targets.

- [ ] **Step 7: Commit integration portability changes**

If scripts or docs changed:

```bash
git add scripts/test-tcpquic-proxy.sh README.md
git commit -m "test: support macos relay integration validation"
```

If no scripts or docs changed, skip this commit.

## Task 9: Final Verification

**Files:**
- No source changes expected unless verification finds defects.

- [ ] **Step 1: Run fresh macOS build**

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --target tcpquic-proxy -j$(sysctl -n hw.ncpu)
```

Expected: build exits 0.

- [ ] **Step 2: Run fresh Darwin unit tests**

```bash
cmake --build build --target tcpquic_darwin_relay_worker_queue_test tcpquic_darwin_relay_worker_io_test -j$(sysctl -n hw.ncpu)
./build/bin/Release/tcpquic_darwin_relay_worker_queue_test
./build/bin/Release/tcpquic_darwin_relay_worker_io_test
```

Expected: both tests exit 0.

- [ ] **Step 3: Run fresh local integration tests**

```bash
COMPRESS=off ./scripts/test-tcpquic-proxy.sh
COMPRESS=zstd ./scripts/test-tcpquic-proxy.sh
```

Expected: both integration runs exit 0.

- [ ] **Step 4: Inspect git diff**

```bash
git status --short
git diff --stat
```

Expected: only planned macOS relay/backend/test/script/doc files are changed.

- [ ] **Step 5: Record verification evidence**

Add a short verification note under `docs/finished/` only if the project convention expects a verification artifact for platform work. Include exact commands, exit codes, and any skipped Linux/Windows checks with reasons.

- [ ] **Step 6: Commit final fixes or verification note**

If final verification required fixes or a note:

```bash
git add <changed-files>
git commit -m "test: verify darwin kqueue relay backend"
```

Skip this commit if no files changed.

## Self-Review

- Spec coverage: covers Darwin backend selection, `kqueue` eventing, event queue, TCP-to-QUIC path, QUIC-to-TCP pending receive path, SIGPIPE handling, backpressure, close/lifetime, metrics, unit tests, integration tests, and final verification.
- Placeholder scan: no unresolved placeholders are present. Conditional steps identify exact skip conditions and exact follow-up commands.
- Type consistency: planned public types use `TqDarwinRelayWorker`, `TqDarwinRelayRuntime`, `TqDarwinRelayRegistration`, `TqDarwinRelayWorkerConfig`, `TqDarwinRelayEventQueue`, and `TqDarwinRelayEvent`, matching the design document.
- Scope check: plan avoids Linux epoll refactoring and Windows IOCP changes except compile-guard verification.
