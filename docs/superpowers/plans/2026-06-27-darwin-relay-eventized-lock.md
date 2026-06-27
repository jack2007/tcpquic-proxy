# Darwin Relay Eventized Lock Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 分 4 个阶段把 macOS relay 热点数据结构的写入收敛到 Darwin relay worker 线程，移除 `RelayState::Mutex` 和 `RelayMutex` 对数据面热路径的影响。

**Architecture:** 阶段一先让 QUIC receive callback 纯投递事件，阶段二把 SEND_COMPLETE 和 QUIC control callback 事件化，阶段三把 register/unregister/snapshot 控制操作事件化，阶段四收缩或移除热路径锁。跨线程边界保留 `StreamBinding` 原子、event queue CAS 和同步 control command completion。

**Tech Stack:** C++17, macOS kqueue/EVFILT_USER, MsQuic stream callback, `TqDarwinRelayEventQueue`, `std::condition_variable`, existing CMake unit tests `tcpquic_darwin_relay_worker_queue_test`, `tcpquic_darwin_relay_worker_io_test`, `tcpquic_darwin_relay_metrics_test`.

---

## File Map

- Modify: `src/tunnel/darwin_relay_event_queue.h`
  - Add callback/control event types.
  - Add `void* Control` payload for synchronous register/unregister/snapshot commands.
- Modify: `src/tunnel/darwin_relay_worker.h`
  - Add control command structs and local helper declarations.
  - Add worker thread id tracking.
  - Add binding-level callback pending receive budget fields.
  - Add test helpers for eventized paths and lock-removal assertions.
- Modify: `src/tunnel/darwin_relay_worker.cpp`
  - Move receive pending byte accounting from callback to worker.
  - Eventize send complete and QUIC control callbacks.
  - Split register/unregister/snapshot into public synchronous wrappers and worker-local helpers.
  - Convert relay map and relay state hot fields to worker-local access.
- Modify: `src/unittest/darwin_relay_worker_queue_test.cpp`
  - Add event queue/control event coverage.
- Modify: `src/unittest/darwin_relay_worker_io_test.cpp`
  - Add receive callback budget, send complete eventization, sync completion, late completion and close tests.
- Modify: `src/unittest/darwin_relay_metrics_test.cpp`
  - Add snapshot control event coverage.
- Modify: `docs/relay_macos.md`
  - Update the lock section after implementation to reflect the final worker-local ownership model.

## Phase 1: Eventize QUIC receive callback state changes

### Task 1: Add tests for receive callback budget and worker-owned pending bytes

**Files:**
- Modify: `src/unittest/darwin_relay_worker_io_test.cpp`

- [ ] **Step 1: Add a test for receive callback not mutating relay pending bytes before worker drain**

Add a test block after the existing basic QUIC receive to TCP write test:

```cpp
    {
        ResetFakeReceiveSetEnabled(QUIC_STATUS_SUCCESS);
        g_receiveCompleteCalls.store(0, std::memory_order_release);
        g_receiveCompleteBytes.store(0, std::memory_order_release);

        TqDarwinRelayWorkerConfig config{};
        config.EventBudget = 64;
        config.ReadChunkSize = 1024;
        config.ReadBatchBytes = 4096;
        config.MaxIov = 4;
        config.MaxPendingQuicReceiveBytesPerRelay = 4096;

        TqDarwinRelayWorker worker(config);
        CHECK(worker.StartForTest());
        worker.SetReceiveCompleteForTest(FakeReceiveComplete);
        worker.SetReceiveSetEnabledForTest(FakeReceiveSetEnabled);

        int fds[2]{-1, -1};
        CHECK(::socketpair(AF_UNIX, SOCK_STREAM, 0, fds) == 0);
        MsQuicStream stream{};
        TqRelayHandle handle{};
        TqDarwinRelayRegistration registration{};
        registration.TcpFd = fds[0];
        registration.Stream = &stream;
        registration.Handle = &handle;

        const auto result = worker.RegisterRelayWithId(registration);
        CHECK(result.Ok);
        void* callbackContext = worker.StreamCallbackContextForTest(result.RelayId);
        CHECK(callbackContext != nullptr);

        const uint8_t payload[] = {'a', 'b', 'c', 'd'};
        QUIC_BUFFER buffer{};
        buffer.Buffer = const_cast<uint8_t*>(payload);
        buffer.Length = sizeof(payload);
        QUIC_STREAM_EVENT event{};
        event.Type = QUIC_STREAM_EVENT_RECEIVE;
        event.RECEIVE.BufferCount = 1;
        event.RECEIVE.Buffers = &buffer;

        CHECK(TqDarwinRelayWorker::StreamCallback(&stream, callbackContext, &event) == QUIC_STATUS_PENDING);
        CHECK(worker.PendingQuicReceiveBytesForTest(result.RelayId) == 0);
        CHECK(worker.DrainOneEventForTest());
        CHECK(worker.PendingQuicReceiveBytesForTest(result.RelayId) == 0);
        CHECK(g_receiveCompleteCalls.load(std::memory_order_acquire) == 1);
        CHECK(g_receiveCompleteBytes.load(std::memory_order_acquire) == sizeof(payload));

        worker.UnregisterRelay(result.RelayId);
        ::close(fds[1]);
        worker.Stop();
    }
```

- [ ] **Step 2: Add the missing test helper declaration**

Add a test helper in `TqDarwinRelayWorker`:

```cpp
#if defined(TCPQUIC_TESTING)
    bool DrainOneEventForTest();
#endif
```

Implementation:

```cpp
bool TqDarwinRelayWorker::DrainOneEventForTest() {
    return DrainEvents(1) == 1;
}
```

Then use `worker.DrainOneEventForTest()` in the test block.

- [ ] **Step 3: Run the test and capture current failure**

Run:

```bash
rtk cmake --build build --target tcpquic_darwin_relay_worker_io_test -j$(nproc)
rtk build/src/tcpquic_darwin_relay_worker_io_test
```

Expected before implementation: FAIL because current callback increments `PendingQuicReceiveBytes` before worker drains the event.

### Task 2: Add binding-level callback receive budget

**Files:**
- Modify: `src/tunnel/darwin_relay_worker.cpp`
- Modify: `src/tunnel/darwin_relay_worker.h`

- [ ] **Step 1: Add budget fields to `StreamBinding`**

In `TqDarwinRelayWorker::StreamBinding`, add:

```cpp
    std::atomic<uint64_t> CallbackPendingReceiveBytes{0};
    std::atomic<uint64_t> CallbackPendingReceiveEvents{0};
```

- [ ] **Step 2: Add private helper declarations**

In `darwin_relay_worker.h`, add:

```cpp
    bool ReserveCallbackReceiveBudget(StreamBinding* binding, uint64_t bytes);
    void ReleaseCallbackReceiveBudget(StreamBinding* binding, uint64_t bytes);
```

- [ ] **Step 3: Implement reserve/release**

In `darwin_relay_worker.cpp`, add:

```cpp
bool TqDarwinRelayWorker::ReserveCallbackReceiveBudget(StreamBinding* binding, uint64_t bytes) {
    if (binding == nullptr) {
        return false;
    }
    const uint64_t limit = MaxPendingQuicReceiveBytesPerRelay();
    uint64_t current = binding->CallbackPendingReceiveBytes.load(std::memory_order_relaxed);
    while (true) {
        if (limit != 0 && current + bytes > limit) {
            return false;
        }
        if (binding->CallbackPendingReceiveBytes.compare_exchange_weak(
                current,
                current + bytes,
                std::memory_order_acq_rel,
                std::memory_order_relaxed)) {
            binding->CallbackPendingReceiveEvents.fetch_add(1, std::memory_order_acq_rel);
            return true;
        }
    }
}

void TqDarwinRelayWorker::ReleaseCallbackReceiveBudget(StreamBinding* binding, uint64_t bytes) {
    if (binding == nullptr) {
        return;
    }
    uint64_t current = binding->CallbackPendingReceiveBytes.load(std::memory_order_relaxed);
    while (current != 0) {
        const uint64_t next = current > bytes ? current - bytes : 0;
        if (binding->CallbackPendingReceiveBytes.compare_exchange_weak(
                current,
                next,
                std::memory_order_acq_rel,
                std::memory_order_relaxed)) {
            break;
        }
    }
    uint64_t events = binding->CallbackPendingReceiveEvents.load(std::memory_order_relaxed);
    while (events != 0) {
        if (binding->CallbackPendingReceiveEvents.compare_exchange_weak(
                events,
                events - 1,
                std::memory_order_acq_rel,
                std::memory_order_relaxed)) {
            break;
        }
    }
}
```

- [ ] **Step 4: Run compile to catch declaration errors**

Run:

```bash
rtk cmake --build build --target tcpquic_darwin_relay_worker_io_test -j$(nproc)
```

Expected: build succeeds.

### Task 3: Move `PendingQuicReceiveBytes` mutation to worker event handling

**Files:**
- Modify: `src/tunnel/darwin_relay_worker.cpp`

- [ ] **Step 1: Update `QueueDeferredQuicReceive()`**

Replace the second relay lock block that increments `relay->PendingQuicReceiveBytes` with binding budget reserve:

```cpp
    StreamBinding* binding = nullptr;
    {
        std::lock_guard<std::mutex> relayLock(relay->Mutex);
        if (relay->Closing) {
            return false;
        }
        binding = relay->Binding.get();
    }
    if (!ReserveCallbackReceiveBudget(binding, receive->TotalLength)) {
        return false;
    }
```

If `EnqueueEvent()` fails, release budget:

```cpp
        ReleaseCallbackReceiveBudget(binding, receive->TotalLength);
        QuicReceiveViewCount.fetch_sub(1, std::memory_order_relaxed);
        QuicReceiveViewBytes.fetch_sub(receive->TotalLength, std::memory_order_relaxed);
        return false;
```

- [ ] **Step 2: Update `ProcessQuicReceiveViewEvent()`**

After finding relay and before pushing `PendingQuicReceives`, transfer budget into relay state:

```cpp
    std::shared_ptr<StreamBinding> binding;
    bool shouldDiscard = false;
    {
        std::lock_guard<std::mutex> relayLock(relay->Mutex);
        binding = relay->Binding;
        if (relay->Closing) {
            shouldDiscard = true;
        } else {
            relay->PendingQuicReceiveBytes += receive->TotalLength;
            relay->PendingQuicReceives.push_back(receive);
        }
    }
    if (binding != nullptr) {
        ReleaseCallbackReceiveBudget(binding.get(), receive->TotalLength);
    }
```

Keep the existing discard path, but make sure budget is released before discarding.

- [ ] **Step 3: Run focused tests**

Run:

```bash
rtk cmake --build build --target tcpquic_darwin_relay_worker_queue_test tcpquic_darwin_relay_worker_io_test -j$(nproc)
rtk build/src/tcpquic_darwin_relay_worker_queue_test
rtk build/src/tcpquic_darwin_relay_worker_io_test
```

Expected: both PASS, including the new receive callback pending bytes test.

- [ ] **Step 4: Commit phase 1**

Run:

```bash
git add src/tunnel/darwin_relay_worker.h src/tunnel/darwin_relay_worker.cpp src/unittest/darwin_relay_worker_io_test.cpp
git commit -m "perf: eventize Darwin QUIC receive accounting"
```

## Phase 2: Eventize SEND_COMPLETE and QUIC control callbacks

### Task 4: Add tests proving SEND_COMPLETE is handled by worker event

**Files:**
- Modify: `src/unittest/darwin_relay_worker_io_test.cpp`

- [ ] **Step 1: Add send complete callback queue test**

Add a test that sends TCP data, invokes `StreamCallback(SEND_COMPLETE)`, checks in-flight count remains until worker drains one event, then checks it drops:

```cpp
    {
        g_sendStatus.store(QUIC_STATUS_SUCCESS, std::memory_order_release);
        g_sendCalls.store(0, std::memory_order_release);
        g_lastSendContext.store(nullptr, std::memory_order_release);

        TqDarwinRelayWorkerConfig config{};
        config.EventBudget = 64;
        config.ReadChunkSize = 1024;
        config.ReadBatchBytes = 4096;
        config.MaxIov = 4;

        TqDarwinRelayWorker worker(config);
        CHECK(worker.StartForTest());
        worker.SetStreamSendForTest(FakeStreamSend);

        int fds[2]{-1, -1};
        CHECK(::socketpair(AF_UNIX, SOCK_STREAM, 0, fds) == 0);
        MsQuicStream stream{};
        TqRelayHandle handle{};
        TqDarwinRelayRegistration registration{};
        registration.TcpFd = fds[0];
        registration.Stream = &stream;
        registration.Handle = &handle;
        const auto result = worker.RegisterRelayWithId(registration);
        CHECK(result.Ok);

        const char payload[] = "send-complete-event";
        CHECK(::write(fds[1], payload, sizeof(payload)) == static_cast<ssize_t>(sizeof(payload)));
        CHECK(worker.InvokeTcpEventForTest(result.RelayId, EVFILT_READ, 0, 0));
        CHECK(worker.InFlightQuicSendCountForTest(result.RelayId) == 1);

        void* callbackContext = worker.StreamCallbackContextForTest(result.RelayId);
        auto* operation = static_cast<TqDarwinRelaySendOperation*>(
            g_lastSendContext.load(std::memory_order_acquire));
        CHECK(operation != nullptr);
        QUIC_STREAM_EVENT event{};
        event.Type = QUIC_STREAM_EVENT_SEND_COMPLETE;
        event.SEND_COMPLETE.ClientContext = operation;
        CHECK(TqDarwinRelayWorker::StreamCallback(&stream, callbackContext, &event) == QUIC_STATUS_SUCCESS);
        CHECK(worker.InFlightQuicSendCountForTest(result.RelayId) == 1);
        CHECK(worker.DrainOneEventForTest());
        CHECK(worker.InFlightQuicSendCountForTest(result.RelayId) == 0);

        worker.UnregisterRelay(result.RelayId);
        ::close(fds[1]);
        worker.Stop();
    }
```

- [ ] **Step 2: Run and capture current failure**

Run:

```bash
rtk cmake --build build --target tcpquic_darwin_relay_worker_io_test -j$(nproc)
rtk build/src/tcpquic_darwin_relay_worker_io_test
```

Expected before implementation: FAIL because current callback completes send immediately.

### Task 5: Make SEND_COMPLETE callback enqueue worker event

**Files:**
- Modify: `src/tunnel/darwin_relay_worker.cpp`

- [ ] **Step 1: Add helper for callback enqueue**

Add:

```cpp
bool TqDarwinRelayWorker::EnqueueQuicSendCompleteFromCallback(
    uint64_t relayId,
    TqDarwinRelaySendOperation* operation) {
    if (operation == nullptr) {
        return false;
    }
    TqDarwinRelayEvent event{};
    event.Type = TqDarwinRelayEventType::QuicSendComplete;
    event.RelayId = relayId;
    event.Value = reinterpret_cast<uintptr_t>(operation);
    return EnqueueEvent(std::move(event));
}
```

Declare it private in the header.

- [ ] **Step 2: Replace SEND_COMPLETE branch in `StreamCallback()`**

Change the SEND_COMPLETE branch to:

```cpp
    if (event->Type == QUIC_STREAM_EVENT_SEND_COMPLETE) {
        TqDarwinRelaySendOperation* operation =
            reinterpret_cast<TqDarwinRelaySendOperation*>(event->SEND_COMPLETE.ClientContext);
        TqDarwinRelayWorker* worker = binding->Worker.load(std::memory_order_acquire);
        if (worker != nullptr &&
            worker->EnqueueQuicSendCompleteFromCallback(binding->RelayId, operation)) {
            return QUIC_STATUS_SUCCESS;
        }
        if (CompleteDetachedQuicSend(binding, operation)) {
            return QUIC_STATUS_SUCCESS;
        }
        if (worker != nullptr) {
            worker->CompleteQuicSend(operation);
            return QUIC_STATUS_SUCCESS;
        }
        return QUIC_STATUS_SUCCESS;
    }
```

The direct `CompleteQuicSend()` fallback is only for enqueue failure while worker is still alive. It must remain until phase 4 replaces the fallback with a worker-local detached path.

- [ ] **Step 3: Run focused tests**

Run:

```bash
rtk cmake --build build --target tcpquic_darwin_relay_worker_io_test -j$(nproc)
rtk build/src/tcpquic_darwin_relay_worker_io_test
```

Expected: PASS.

### Task 6: Eventize QUIC ideal buffer and abort/shutdown callbacks

**Files:**
- Modify: `src/tunnel/darwin_relay_event_queue.h`
- Modify: `src/tunnel/darwin_relay_worker.cpp`
- Modify: `src/tunnel/darwin_relay_worker.h`

- [ ] **Step 1: Add event types**

In `TqDarwinRelayEventType`, add:

```cpp
    QuicPeerSendAborted,
    QuicPeerReceiveAborted,
    QuicShutdownComplete,
```

- [ ] **Step 2: Update `StreamCallback()` abort/shutdown branch**

Replace direct `FindRelay()`/`CloseRelay()` with event enqueue:

```cpp
    if (event->Type == QUIC_STREAM_EVENT_PEER_SEND_ABORTED ||
        event->Type == QUIC_STREAM_EVENT_PEER_RECEIVE_ABORTED ||
        event->Type == QUIC_STREAM_EVENT_SHUTDOWN_COMPLETE) {
        TqDarwinRelayWorker* worker = binding->Worker.load(std::memory_order_acquire);
        if (worker != nullptr) {
            TqDarwinRelayEvent queued{};
            queued.RelayId = binding->RelayId;
            if (event->Type == QUIC_STREAM_EVENT_PEER_SEND_ABORTED) {
                queued.Type = TqDarwinRelayEventType::QuicPeerSendAborted;
            } else if (event->Type == QUIC_STREAM_EVENT_PEER_RECEIVE_ABORTED) {
                queued.Type = TqDarwinRelayEventType::QuicPeerReceiveAborted;
            } else {
                queued.Type = TqDarwinRelayEventType::QuicShutdownComplete;
            }
            if (!worker->EnqueueEvent(std::move(queued))) {
                if (auto relay = worker->FindRelay(binding->RelayId)) {
                    worker->CloseRelay(relay, 1);
                }
            }
        }
        return QUIC_STATUS_SUCCESS;
    }
```

- [ ] **Step 3: Handle new events in `DrainEvents()`**

Add cases:

```cpp
        case TqDarwinRelayEventType::QuicPeerSendAborted:
        case TqDarwinRelayEventType::QuicPeerReceiveAborted:
        case TqDarwinRelayEventType::QuicShutdownComplete:
            if (auto relay = FindRelay(event.RelayId)) {
                CloseRelay(relay);
            }
            break;
```

- [ ] **Step 4: Add a test for shutdown callback delayed close**

In `darwin_relay_worker_io_test.cpp`, create a registered relay, invoke `StreamCallback()` with `QUIC_STREAM_EVENT_SHUTDOWN_COMPLETE`, assert `worker.Snapshot().ActiveRelays == 1`, drain one event, then assert `worker.Snapshot().ActiveRelays == 0`.

- [ ] **Step 5: Run focused tests**

Run:

```bash
rtk cmake --build build --target tcpquic_darwin_relay_worker_io_test -j$(nproc)
rtk build/src/tcpquic_darwin_relay_worker_io_test
```

Expected: PASS.

- [ ] **Step 6: Commit phase 2**

Run:

```bash
git add src/tunnel/darwin_relay_event_queue.h src/tunnel/darwin_relay_worker.h src/tunnel/darwin_relay_worker.cpp src/unittest/darwin_relay_worker_io_test.cpp
git commit -m "perf: eventize Darwin QUIC callbacks"
```

## Phase 3: Eventize register, unregister, and snapshot

### Task 7: Add control event types and command structs

**Files:**
- Modify: `src/tunnel/darwin_relay_event_queue.h`
- Modify: `src/tunnel/darwin_relay_worker.h`

- [ ] **Step 1: Add event types and control payload**

In `TqDarwinRelayEventType`, add:

```cpp
    RegisterRelay,
    UnregisterRelay,
    Snapshot,
```

In `TqDarwinRelayEvent`, add:

```cpp
    void* Control{nullptr};
```

- [ ] **Step 2: Add include for condition variable**

In `darwin_relay_worker.h`, add:

```cpp
#include <condition_variable>
```

- [ ] **Step 3: Add command structs**

Inside `TqDarwinRelayWorker` private section, add:

```cpp
    struct RegisterRelayCommand {
        TqDarwinRelayRegistration Registration;
        TqDarwinRelayRegistrationResult Result;
        std::mutex Mutex;
        std::condition_variable Cv;
        bool Done{false};
    };

    struct UnregisterRelayCommand {
        uint64_t RelayId{0};
        std::mutex Mutex;
        std::condition_variable Cv;
        bool Done{false};
    };

    struct SnapshotCommand {
        TqDarwinRelayWorkerSnapshot Result;
        std::mutex Mutex;
        std::condition_variable Cv;
        bool Done{false};
    };
```

- [ ] **Step 4: Add helper declarations and worker thread id**

Add private declarations:

```cpp
    bool IsWorkerThread() const;
    TqDarwinRelayRegistrationResult RegisterRelayWithIdLocal(
        const TqDarwinRelayRegistration& registration);
    void UnregisterRelayLocal(uint64_t relayId);
    TqDarwinRelayWorkerSnapshot SnapshotLocal() const;
    void CompleteRegisterCommand(RegisterRelayCommand* command, TqDarwinRelayRegistrationResult result);
    void CompleteUnregisterCommand(UnregisterRelayCommand* command);
    void CompleteSnapshotCommand(SnapshotCommand* command, TqDarwinRelayWorkerSnapshot snapshot);
```

Add field:

```cpp
    std::thread::id WorkerThreadId;
```

### Task 8: Split public methods into local helpers without behavior change

**Files:**
- Modify: `src/tunnel/darwin_relay_worker.cpp`

- [ ] **Step 1: Track worker thread id**

At the start of `Run()`, add:

```cpp
    WorkerThreadId = std::this_thread::get_id();
```

Implement:

```cpp
bool TqDarwinRelayWorker::IsWorkerThread() const {
    return std::this_thread::get_id() == WorkerThreadId;
}
```

Also update `StartForTest()` so worker-local checks work in unit tests:

```cpp
bool TqDarwinRelayWorker::StartForTest() {
    WorkerThreadId = std::this_thread::get_id();
    Running.store(true, std::memory_order_release);
    return true;
}
```

- [ ] **Step 2: Move current register body**

Move the body of `RegisterRelayWithId()` into:

```cpp
TqDarwinRelayRegistrationResult TqDarwinRelayWorker::RegisterRelayWithIdLocal(
    const TqDarwinRelayRegistration& registration) {
    std::lock_guard<std::mutex> lifecycleLock(LifecycleMutex);
    if (!Running.load(std::memory_order_acquire) || KqueueFd < 0) {
        return {};
    }
    if (!TqSocketValid(registration.TcpFd) || registration.Stream == nullptr || registration.Handle == nullptr) {
        return {};
    }
    if (!TqSetNonBlocking(registration.TcpFd)) {
        Errors.fetch_add(1, std::memory_order_relaxed);
        return {};
    }
    if (!SetNoSigPipe(registration.TcpFd) && errno != ENOPROTOOPT) {
        Errors.fetch_add(1, std::memory_order_relaxed);
        return {};
    }

    auto relay = std::make_shared<RelayState>(registration, Config);
    {
        std::lock_guard<std::mutex> relayLock(relay->Mutex);
        relay->TcpReadArmed = true;
    }
    auto binding = std::make_shared<StreamBinding>();
    binding->Worker.store(this, std::memory_order_release);
    binding->Completions = std::make_shared<CompletionState>();
    relay->Binding = binding;

    {
        std::lock_guard<std::mutex> lock(RelayMutex);
        relay->Id = NextRelayId++;
        binding->RelayId = relay->Id;
        Relays.emplace(relay->Id, relay);
    }

    if (!RegisterTcpFilters(relay)) {
        RemoveTcpFilters(relay);
        Errors.fetch_add(1, std::memory_order_relaxed);
        {
            std::lock_guard<std::mutex> lock(RelayMutex);
            Relays.erase(relay->Id);
        }
        ClearPublicHandle(relay);
        return {};
    }

    registration.Handle->Backend = TqRelayBackendType::DarwinWorker;
    registration.Handle->DarwinWorker = this;
    registration.Handle->DarwinRelayId = relay->Id;
#if defined(TCPQUIC_TESTING)
    if (TqDarwinRelayStreamUsableForTest(registration.Stream)) {
        registration.Stream->Callback = TqDarwinRelayWorker::StreamCallback;
        registration.Stream->Context = binding.get();
    }
#else
    registration.Stream->Callback = TqDarwinRelayWorker::StreamCallback;
    registration.Stream->Context = binding.get();
#endif
    return {true, relay->Id};
}
```

Temporarily implement public wrapper:

```cpp
TqDarwinRelayRegistrationResult TqDarwinRelayWorker::RegisterRelayWithId(
    const TqDarwinRelayRegistration& registration) {
    return RegisterRelayWithIdLocal(registration);
}
```

- [ ] **Step 3: Move current unregister body**

Move the body of `UnregisterRelay()` into:

```cpp
void TqDarwinRelayWorker::UnregisterRelayLocal(uint64_t relayId) {
    std::shared_ptr<RelayState> relay;
    {
        std::lock_guard<std::mutex> lifecycleLock(LifecycleMutex);
        std::lock_guard<std::mutex> lock(RelayMutex);
        const auto it = Relays.find(relayId);
        if (it == Relays.end()) {
            return;
        }
        relay = it->second;
        Relays.erase(it);
        RemoveTcpFilters(relay);
    }

    RetireRelay(relay);
    PurgeRetiredRelaysIfSafe();
}
```

Public wrapper:

```cpp
void TqDarwinRelayWorker::UnregisterRelay(uint64_t relayId) {
    UnregisterRelayLocal(relayId);
}
```

- [ ] **Step 4: Move current snapshot body**

Move the body of `Snapshot()` into:

```cpp
TqDarwinRelayWorkerSnapshot TqDarwinRelayWorker::SnapshotLocal() const {
    TqDarwinRelayWorkerSnapshot snapshot{};
    snapshot.EventsProcessed = EventsProcessed.load(std::memory_order_relaxed);
    snapshot.Wakeups = Wakeups.load(std::memory_order_relaxed);
    snapshot.PendingEvents = EventQueue.SizeApprox();
    snapshot.TcpReadBatches = TcpReadBatches.load(std::memory_order_relaxed);
    snapshot.TcpReadBytes = TcpReadBytes.load(std::memory_order_relaxed);
    snapshot.TcpWriteBatches = TcpWriteBatches.load(std::memory_order_relaxed);
    snapshot.TcpWriteBytes = TcpWriteBytes.load(std::memory_order_relaxed);
    snapshot.QuicReceiveViewCount = QuicReceiveViewCount.load(std::memory_order_relaxed);
    snapshot.QuicReceiveViewBytes = QuicReceiveViewBytes.load(std::memory_order_relaxed);
    snapshot.DeferredReceiveCompletes = DeferredReceiveCompletes.load(std::memory_order_relaxed);
    snapshot.QuicSendBackpressureEvents = QuicSendBackpressureEvents.load(std::memory_order_relaxed);
    snapshot.QuicReceivePausedCount = QuicReceivePausedCount.load(std::memory_order_relaxed);
    snapshot.QuicReceiveResumedCount = QuicReceiveResumedCount.load(std::memory_order_relaxed);
    {
        std::lock_guard<std::mutex> lock(RelayMutex);
        snapshot.ActiveRelays = Relays.size();
        for (const auto& entry : Relays) {
            const auto& relay = entry.second;
            if (relay == nullptr) {
                continue;
            }
            std::lock_guard<std::mutex> relayLock(relay->Mutex);
            if (relay->TcpReadArmed) {
                ++snapshot.TcpReadArmedRelays;
            }
            if (relay->TcpWriteArmed) {
                ++snapshot.TcpWriteArmedRelays;
            }
            snapshot.OutstandingQuicSends += relay->InFlightQuicSends;
            snapshot.OutstandingQuicSendBytes += relay->InFlightQuicSendBytes;
            snapshot.OutstandingQuicSends += relay->PendingQuicSends.size();
            const uint64_t receivePathPendingBytes = std::max(
                relay->PendingQuicReceiveBytes,
                relay->PendingTcpWriteBytes);
            uint64_t relayPendingBytes = receivePathPendingBytes;
            for (const auto& pendingSend : relay->PendingQuicSends) {
                if (pendingSend != nullptr) {
                    snapshot.OutstandingQuicSendBytes += pendingSend->TotalBytes;
                    relayPendingBytes += pendingSend->TotalBytes;
                }
            }
            snapshot.CurrentPendingQuicReceiveBytes += relay->PendingQuicReceiveBytes;
            snapshot.PendingTcpWriteQueue += relay->PendingTcpWrites.size();
            snapshot.PendingTcpWriteBytes += relay->PendingTcpWriteBytes;
            snapshot.PendingBytes += relayPendingBytes;
        }
    }
    snapshot.Errors = Errors.load(std::memory_order_relaxed);
    return snapshot;
}
```

Public wrapper:

```cpp
TqDarwinRelayWorkerSnapshot TqDarwinRelayWorker::Snapshot() const {
    return SnapshotLocal();
}
```

- [ ] **Step 5: Run focused tests**

Run:

```bash
rtk cmake --build build --target tcpquic_darwin_relay_worker_queue_test tcpquic_darwin_relay_worker_io_test tcpquic_darwin_relay_metrics_test -j$(nproc)
rtk build/src/tcpquic_darwin_relay_worker_queue_test
rtk build/src/tcpquic_darwin_relay_worker_io_test
rtk build/src/tcpquic_darwin_relay_metrics_test
```

Expected: all PASS.

### Task 9: Implement synchronous control event wrappers

**Files:**
- Modify: `src/tunnel/darwin_relay_worker.cpp`

- [ ] **Step 1: Add command completion helpers**

Add:

```cpp
void TqDarwinRelayWorker::CompleteRegisterCommand(
    RegisterRelayCommand* command,
    TqDarwinRelayRegistrationResult result) {
    if (command == nullptr) {
        return;
    }
    {
        std::lock_guard<std::mutex> lock(command->Mutex);
        command->Result = result;
        command->Done = true;
    }
    command->Cv.notify_one();
}

void TqDarwinRelayWorker::CompleteUnregisterCommand(UnregisterRelayCommand* command) {
    if (command == nullptr) {
        return;
    }
    {
        std::lock_guard<std::mutex> lock(command->Mutex);
        command->Done = true;
    }
    command->Cv.notify_one();
}

void TqDarwinRelayWorker::CompleteSnapshotCommand(
    SnapshotCommand* command,
    TqDarwinRelayWorkerSnapshot snapshot) {
    if (command == nullptr) {
        return;
    }
    {
        std::lock_guard<std::mutex> lock(command->Mutex);
        command->Result = snapshot;
        command->Done = true;
    }
    command->Cv.notify_one();
}
```

- [ ] **Step 2: Handle control events in `DrainEvents()`**

Add cases:

```cpp
        case TqDarwinRelayEventType::RegisterRelay: {
            auto* command = static_cast<RegisterRelayCommand*>(event.Control);
            const auto result = command != nullptr
                ? RegisterRelayWithIdLocal(command->Registration)
                : TqDarwinRelayRegistrationResult{};
            CompleteRegisterCommand(command, result);
            break;
        }
        case TqDarwinRelayEventType::UnregisterRelay: {
            auto* command = static_cast<UnregisterRelayCommand*>(event.Control);
            if (command != nullptr) {
                UnregisterRelayLocal(command->RelayId);
            }
            CompleteUnregisterCommand(command);
            break;
        }
        case TqDarwinRelayEventType::Snapshot: {
            auto* command = static_cast<SnapshotCommand*>(event.Control);
            CompleteSnapshotCommand(command, SnapshotLocal());
            break;
        }
```

- [ ] **Step 3: Update public `RegisterRelayWithId()`**

Replace wrapper with:

```cpp
TqDarwinRelayRegistrationResult TqDarwinRelayWorker::RegisterRelayWithId(
    const TqDarwinRelayRegistration& registration) {
    if (IsWorkerThread() || !Running.load(std::memory_order_acquire)) {
        return RegisterRelayWithIdLocal(registration);
    }
    RegisterRelayCommand command{};
    command.Registration = registration;
    TqDarwinRelayEvent event{};
    event.Type = TqDarwinRelayEventType::RegisterRelay;
    event.Control = &command;
    if (!EnqueueEvent(std::move(event))) {
        return {};
    }
    std::unique_lock<std::mutex> lock(command.Mutex);
    command.Cv.wait(lock, [&command] { return command.Done; });
    return command.Result;
}
```

- [ ] **Step 4: Update public `UnregisterRelay()`**

Replace wrapper with:

```cpp
void TqDarwinRelayWorker::UnregisterRelay(uint64_t relayId) {
    if (IsWorkerThread() || !Running.load(std::memory_order_acquire)) {
        UnregisterRelayLocal(relayId);
        return;
    }
    UnregisterRelayCommand command{};
    command.RelayId = relayId;
    TqDarwinRelayEvent event{};
    event.Type = TqDarwinRelayEventType::UnregisterRelay;
    event.Control = &command;
    if (!EnqueueEvent(std::move(event))) {
        Errors.fetch_add(1, std::memory_order_relaxed);
        UnregisterRelayLocal(relayId);
        return;
    }
    std::unique_lock<std::mutex> lock(command.Mutex);
    command.Cv.wait(lock, [&command] { return command.Done; });
}
```

- [ ] **Step 5: Update public `Snapshot()`**

Replace wrapper with:

```cpp
TqDarwinRelayWorkerSnapshot TqDarwinRelayWorker::Snapshot() const {
    if (IsWorkerThread() || !Running.load(std::memory_order_acquire)) {
        return SnapshotLocal();
    }
    auto* self = const_cast<TqDarwinRelayWorker*>(this);
    SnapshotCommand command{};
    TqDarwinRelayEvent event{};
    event.Type = TqDarwinRelayEventType::Snapshot;
    event.Control = &command;
    if (!self->EnqueueEvent(std::move(event))) {
        TqDarwinRelayWorkerSnapshot snapshot{};
        snapshot.Errors = Errors.load(std::memory_order_relaxed) + 1;
        return snapshot;
    }
    std::unique_lock<std::mutex> lock(command.Mutex);
    command.Cv.wait(lock, [&command] { return command.Done; });
    return command.Result;
}
```

- [ ] **Step 6: Add metrics test for eventized snapshot**

In `src/unittest/darwin_relay_metrics_test.cpp`, add a test that starts a worker, registers one relay, calls `Snapshot()` from the test thread, and checks `ActiveRelays == 1`.

- [ ] **Step 7: Run focused tests**

Run:

```bash
rtk cmake --build build --target tcpquic_darwin_relay_worker_queue_test tcpquic_darwin_relay_worker_io_test tcpquic_darwin_relay_metrics_test -j$(nproc)
rtk build/src/tcpquic_darwin_relay_worker_queue_test
rtk build/src/tcpquic_darwin_relay_worker_io_test
rtk build/src/tcpquic_darwin_relay_metrics_test
```

Expected: all PASS.

- [ ] **Step 8: Commit phase 3**

Run:

```bash
git add src/tunnel/darwin_relay_event_queue.h src/tunnel/darwin_relay_worker.h src/tunnel/darwin_relay_worker.cpp src/unittest/darwin_relay_metrics_test.cpp
git commit -m "perf: eventize Darwin relay control operations"
```

## Phase 4: Shrink hot locks to cold paths

### Task 10: Convert worker relay container lookup to worker-local helpers

**Files:**
- Modify: `src/tunnel/darwin_relay_worker.h`
- Modify: `src/tunnel/darwin_relay_worker.cpp`

- [ ] **Step 1: Add local lookup helper declarations**

Add:

```cpp
    std::shared_ptr<RelayState> FindRelayLocal(uint64_t relayId) const;
    std::shared_ptr<RelayState> FindRetiredRelayLocal(uint64_t relayId) const;
```

- [ ] **Step 2: Implement local helpers without `RelayMutex`**

Add:

```cpp
std::shared_ptr<TqDarwinRelayWorker::RelayState> TqDarwinRelayWorker::FindRelayLocal(uint64_t relayId) const {
    const auto it = Relays.find(relayId);
    return it != Relays.end() ? it->second : nullptr;
}

std::shared_ptr<TqDarwinRelayWorker::RelayState> TqDarwinRelayWorker::FindRetiredRelayLocal(uint64_t relayId) const {
    for (const auto& relay : RetiredRelays) {
        if (relay != nullptr && relay->Id == relayId) {
            return relay;
        }
    }
    return nullptr;
}
```

- [ ] **Step 3: Replace worker-thread call sites**

In event handlers that run on worker thread, replace `FindRelay()` with `FindRelayLocal()`:

```cpp
ProcessTcpEvents()
DrainEvents() event cases
ProcessQuicReceiveViewEvent()
CompleteQuicSend()
Retry paths reached from worker events
```

Keep external test helpers using `FindRelay()` until test helpers are converted or guarded.

- [ ] **Step 4: Run focused tests**

Run:

```bash
rtk cmake --build build --target tcpquic_darwin_relay_worker_io_test -j$(nproc)
rtk build/src/tcpquic_darwin_relay_worker_io_test
```

Expected: PASS.

### Task 11: Move `KnownSendOperations` to worker-local normal path

**Files:**
- Modify: `src/tunnel/darwin_relay_worker.cpp`

- [ ] **Step 1: Split known operation helpers into local and fallback paths**

Add local helpers that do not lock `RelayMutex`:

```cpp
void TqDarwinRelayWorker::RegisterKnownSendOperationLocal(
    TqDarwinRelaySendOperation* operation,
    const KnownSendOperationInfo& info) {
    KnownSendOperations.emplace(operation, info);
}

bool TqDarwinRelayWorker::UnregisterKnownSendOperationLocal(
    TqDarwinRelaySendOperation* operation,
    KnownSendOperationInfo* info) {
    const auto it = KnownSendOperations.find(operation);
    if (it == KnownSendOperations.end()) {
        return false;
    }
    if (info != nullptr) {
        *info = it->second;
    }
    KnownSendOperations.erase(it);
    return true;
}
```

Keep existing locked helpers for transitional external fallback and detached completion.

- [ ] **Step 2: Use local helpers in worker-owned submit and completion paths**

In `TrySubmitQuicSendOperation()` and worker event completion path, call local helpers when `IsWorkerThread()` is true:

```cpp
    if (IsWorkerThread()) {
        RegisterKnownSendOperationLocal(raw, info);
    } else {
        RegisterKnownSendOperation(raw, info);
    }
```

Use the same pattern for unregister.

- [ ] **Step 3: Run synchronous send complete test**

Run:

```bash
rtk cmake --build build --target tcpquic_darwin_relay_worker_io_test -j$(nproc)
rtk build/src/tcpquic_darwin_relay_worker_io_test
```

Expected: PASS, including tests where fake send completes before `Send()` returns.

### Task 12: Reduce `RelayState::Mutex` usage in worker hot paths

**Files:**
- Modify: `src/tunnel/darwin_relay_worker.cpp`

- [ ] **Step 1: Add worker-thread assertions for hot handlers**

At the start of worker-only handlers, add debug assertions under testing:

```cpp
#if defined(TCPQUIC_TESTING)
    if (!IsWorkerThread()) {
        Errors.fetch_add(1, std::memory_order_relaxed);
        return false;
    }
#endif
```

Apply to:

```text
DrainTcpReadable()
SubmitTcpBatchToQuic()
TrySubmitQuicSendOperation()
RetryPendingQuicSends()
ProcessQuicReceiveViewEvent()
EnqueueQuicReceiveForTcp()
FlushTcpWrites()
MaybePauseQuicReceive()
MaybeResumeQuicReceive()
```

- [ ] **Step 2: Remove lock blocks around worker-local queue mutations**

For each worker-only function, replace `std::lock_guard<std::mutex> relayLock(relay->Mutex)` blocks that only read/write hot fields with direct access. Keep locks only around paths callable from external test helpers or transitional fallback.

Example for `MaybePauseQuicReceive()`:

```cpp
    const uint64_t pendingPressure = relay->PendingQuicReceiveBytes + relay->PendingTcpWriteBytes;
    if (!relay->QuicReceivePaused && pendingPressure >= MaxPendingQuicReceiveBytesPerRelay()) {
        relay->QuicReceivePaused = true;
        shouldPause = true;
    }
```

- [ ] **Step 3: Keep `RetireRelay()` conservative**

Do not remove locks from `RetireRelay()` in this task. It is a close/cleanup path and needs separate review after hot path tests pass.

- [ ] **Step 4: Run focused tests**

Run:

```bash
rtk cmake --build build --target tcpquic_darwin_relay_worker_queue_test tcpquic_darwin_relay_worker_io_test tcpquic_darwin_relay_metrics_test -j$(nproc)
rtk build/src/tcpquic_darwin_relay_worker_queue_test
rtk build/src/tcpquic_darwin_relay_worker_io_test
rtk build/src/tcpquic_darwin_relay_metrics_test
```

Expected: all PASS.

### Task 13: Update documentation and verify lock removal boundary

**Files:**
- Modify: `docs/relay_macos.md`
- Modify: `src/tunnel/darwin_relay_worker.cpp`

- [ ] **Step 1: Search hot path lock usage**

Run:

```bash
rtk rg -n "std::lock_guard<std::mutex> relayLock\\(relay->Mutex\\)|std::unique_lock<std::mutex> relayLock\\(relay->Mutex\\)|std::lock_guard<std::mutex> lock\\(RelayMutex\\)" src/tunnel/darwin_relay_worker.cpp
```

Expected: remaining matches are limited to registration, unregister, snapshot fallback, retired cleanup, test helpers, and transitional fallback paths. No match should be in normal receive callback event handling, TCP read/write worker handling, or normal SEND_COMPLETE event handling.

- [ ] **Step 2: Update `docs/relay_macos.md`**

In the lock section, change the `RelayState::Mutex` and `RelayMutex` description to state:

```markdown
阶段四后，普通数据面路径由 Darwin relay worker 线程独占推进，`RelayState::Mutex` 不再保护 TCP read/write、QUIC receive view、SEND_COMPLETE 和 backpressure 热字段。`RelayMutex` 不再用于普通事件查找；它只保留在外部兼容、测试 helper 或关闭兜底路径中。
```

- [ ] **Step 3: Run all Darwin relay tests**

Run:

```bash
rtk cmake --build build --target tcpquic_darwin_relay_worker_queue_test tcpquic_darwin_relay_worker_io_test tcpquic_darwin_relay_metrics_test -j$(nproc)
rtk build/src/tcpquic_darwin_relay_worker_queue_test
rtk build/src/tcpquic_darwin_relay_worker_io_test
rtk build/src/tcpquic_darwin_relay_metrics_test
```

Expected: all PASS.

- [ ] **Step 4: Commit phase 4**

Run:

```bash
git add src/tunnel/darwin_relay_worker.h src/tunnel/darwin_relay_worker.cpp docs/relay_macos.md
git commit -m "perf: make Darwin relay hot state worker-local"
```

## Final Verification

- [ ] **Step 1: Run all Darwin relay targets**

Run:

```bash
rtk cmake --build build --target tcpquic_darwin_relay_worker_queue_test tcpquic_darwin_relay_worker_io_test tcpquic_darwin_relay_metrics_test -j$(nproc)
rtk build/src/tcpquic_darwin_relay_worker_queue_test
rtk build/src/tcpquic_darwin_relay_worker_io_test
rtk build/src/tcpquic_darwin_relay_metrics_test
```

Expected: all PASS.

- [ ] **Step 2: Run source checks for forbidden hot-path patterns**

Run:

```bash
rtk rg -n "QueueDeferredQuicReceive\\(|StreamCallback\\(|CompleteQuicSend\\(|ProcessQuicReceiveViewEvent\\(|FlushTcpWrites\\(" src/tunnel/darwin_relay_worker.cpp
rtk rg -n "std::lock_guard<std::mutex> lock\\(RelayMutex\\)|std::lock_guard<std::mutex> relayLock\\(relay->Mutex\\)" src/tunnel/darwin_relay_worker.cpp
```

Expected: manual inspection confirms callback entry and normal worker hot paths no longer take `RelayMutex` or `RelayState::Mutex`.

- [ ] **Step 3: Verify docs**

Run:

```bash
rtk rg -n "[[:blank:]]$" docs/superpowers/specs/2026-06-27-darwin-relay-eventized-lock-design.md docs/superpowers/plans/2026-06-27-darwin-relay-eventized-lock.md docs/relay_macos.md
rtk rg -n "TO""DO|FIX""ME" docs/superpowers/specs/2026-06-27-darwin-relay-eventized-lock-design.md docs/superpowers/plans/2026-06-27-darwin-relay-eventized-lock.md docs/relay_macos.md
```

Expected: no matches.

## Rollback Plan

- If phase 1 fails under pressure, revert receive budget movement and restore direct `PendingQuicReceiveBytes` accounting in callback.
- If phase 2 fails due to send operation lifetime, keep SEND_COMPLETE direct completion and retain only abort/shutdown eventization.
- If phase 3 introduces control command deadlock, revert public wrappers to local helpers while keeping phases 1 and 2.
- If phase 4 exposes hidden external relay state access, keep `RelayState::Mutex` for those paths and only remove locks from verified worker-only functions.
