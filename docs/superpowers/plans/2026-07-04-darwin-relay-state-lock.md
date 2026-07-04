# Darwin Relay State Lock Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 将 macOS relay 稳定转发热路径上的 `RelayState::Mutex` 和 `TqDarwinRelayWorker::RelayMutex` 出清。

**Architecture:** 保留现有 `RelayState` shared_ptr 生命周期模型和 `RelayMutex` map 所有权模型，但把 active relay 数据面状态定义为 worker-thread-only。worker kqueue/event 路径使用 `FindRelayLocal()`，callback 通过 `StreamBinding` 获取稳定 relay 信息并投递事件，不再查 worker relay map。

**Tech Stack:** C++17, kqueue, MsQuic stream callback, CMake, 现有 Darwin relay worker 单测。

---

## File Structure

- Modify: `src/tunnel/darwin_relay_worker.h`
- Modify: `src/tunnel/darwin_relay_worker.cpp`
- Modify: `src/unittest/darwin_relay_worker_io_test.cpp`
- Modify: `src/unittest/darwin_relay_worker_queue_test.cpp`
- Modify: `docs/relay_macos.md`

### Task 1: Add Lock-Path Observability and Boundary Helpers

**Files:**
- Modify: `src/tunnel/darwin_relay_worker.h`
- Modify: `src/tunnel/darwin_relay_worker.cpp`
- Test: `src/unittest/darwin_relay_worker_io_test.cpp`

- [ ] **Step 1: Add helper declarations**

In `src/tunnel/darwin_relay_worker.h`, add private helper declarations near `FindRelayLocal()`:

```cpp
    void AssertWorkerThreadForRelayState() const;
    bool IsRelayClosingLocal(const RelayState& relay) const;
    void MarkRelayClosingLocal(RelayState& relay) const;
    MsQuicStream* RelayStreamLocal(const RelayState& relay) const;
```

Under `#if defined(TCPQUIC_TESTING)`, add public test accessors:

```cpp
    uint64_t FindRelayLockedCountForTest() const;
    uint64_t FindRelayLocalCountForTest() const;
```

Add private counters:

```cpp
#if defined(TCPQUIC_TESTING)
    std::atomic<uint64_t> FindRelayLockedCount{0};
    std::atomic<uint64_t> FindRelayLocalCount{0};
#endif
```

- [ ] **Step 2: Implement helpers and counters**

In `src/tunnel/darwin_relay_worker.cpp`, add after `FindRetiredRelayLocal()`:

```cpp
void TqDarwinRelayWorker::AssertWorkerThreadForRelayState() const {
#if defined(TCPQUIC_TESTING) || !defined(NDEBUG)
    assert(IsWorkerThread() && "RelayState data-plane fields are worker-thread-only");
#endif
}

bool TqDarwinRelayWorker::IsRelayClosingLocal(const RelayState& relay) const {
    AssertWorkerThreadForRelayState();
    return relay.Closing;
}

void TqDarwinRelayWorker::MarkRelayClosingLocal(RelayState& relay) const {
    AssertWorkerThreadForRelayState();
    relay.Closing = true;
}

MsQuicStream* TqDarwinRelayWorker::RelayStreamLocal(const RelayState& relay) const {
    AssertWorkerThreadForRelayState();
    return relay.Stream;
}
```

In `FindRelay()`:

```cpp
#if defined(TCPQUIC_TESTING)
    FindRelayLockedCount.fetch_add(1, std::memory_order_relaxed);
#endif
```

In `FindRelayLocal()`:

```cpp
#if defined(TCPQUIC_TESTING)
    FindRelayLocalCount.fetch_add(1, std::memory_order_relaxed);
#endif
```

Implement test accessors:

```cpp
#if defined(TCPQUIC_TESTING)
uint64_t TqDarwinRelayWorker::FindRelayLockedCountForTest() const {
    return FindRelayLockedCount.load(std::memory_order_relaxed);
}

uint64_t TqDarwinRelayWorker::FindRelayLocalCountForTest() const {
    return FindRelayLocalCount.load(std::memory_order_relaxed);
}
#endif
```

- [ ] **Step 3: Add current-behavior lock-count test**

In `src/unittest/darwin_relay_worker_io_test.cpp`, add:

```cpp
void CallbackReceiveCurrentlyUsesLockedRelayLookup() {
    ResetFakeReceiveComplete();
    TqDarwinRelayWorkerConfig config{};
    config.MaxPendingQuicReceiveBytesPerRelay = 64 * 1024;
    TqDarwinRelayWorker worker(config);
    CHECK(worker.Start());

    int fds[2]{TqInvalidSocket, TqInvalidSocket};
    CHECK(socketpair(AF_UNIX, SOCK_STREAM, 0, fds) == 0);

    MsQuicStream stream{};
    TqRelayHandle handle{};
    TqDarwinRelayRegistration registration{};
    registration.TcpFd = fds[0];
    registration.Stream = &stream;
    registration.Handle = &handle;

    TqDarwinRelayRegistrationResult result = worker.RegisterRelayWithId(registration);
    CHECK(result.Ok);

    const uint64_t before = worker.FindRelayLockedCountForTest();
    uint8_t payload[] = {'h', 'i'};
    QUIC_BUFFER buffer{};
    buffer.Buffer = payload;
    buffer.Length = sizeof(payload);
    QUIC_STREAM_EVENT event{};
    event.Type = QUIC_STREAM_EVENT_RECEIVE;
    event.RECEIVE.Buffers = &buffer;
    event.RECEIVE.BufferCount = 1;
    event.RECEIVE.TotalBufferLength = sizeof(payload);
    CHECK(TqDarwinRelayWorker::StreamCallback(&stream, stream.Context, &event) == QUIC_STATUS_PENDING);
    CHECK(worker.FindRelayLockedCountForTest() > before);

    worker.UnregisterRelay(result.RelayId);
    worker.Stop();
    CloseSocketPairAfterRelayOwned(registration.TcpFd, fds);
}
```

Add it to `main()`. This is intentionally a characterization test; Task 3 will update the expectation to `== before`.

- [ ] **Step 4: Run the focused test**

Run on macOS:

```bash
rtk cmake --build build --target tcpquic_darwin_relay_worker_io_test -j$(sysctl -n hw.ncpu)
rtk build/src/tcpquic_darwin_relay_worker_io_test
```

Expected: PASS, proving current callback receive still uses locked map lookup.

- [ ] **Step 5: Commit**

```bash
rtk git add src/tunnel/darwin_relay_worker.h src/tunnel/darwin_relay_worker.cpp src/unittest/darwin_relay_worker_io_test.cpp
rtk git commit -m "test: instrument darwin relay locked lookup path"
```

### Task 2: Store Relay Reference on StreamBinding

**Files:**
- Modify: `src/tunnel/darwin_relay_worker.cpp`
- Test: `src/unittest/darwin_relay_worker_io_test.cpp`

- [ ] **Step 1: Add weak relay reference**

In `struct TqDarwinRelayWorker::StreamBinding`, add:

```cpp
    std::weak_ptr<RelayState> Relay;
```

- [ ] **Step 2: Populate binding relay reference at registration**

In `RegisterRelayWithIdLocal()`, after relay id assignment and before stream callback publication, set:

```cpp
        binding->RelayId = relay->Id;
        binding->Relay = relay;
```

Keep `relay->Binding = binding;` intact.

- [ ] **Step 3: Clear binding relay reference during retire**

In the lifecycle path that marks `binding->Active` false during retire/close, add:

```cpp
    binding->Relay.reset();
```

If the existing code handles binding cleanup in more than one function, apply this in the shared cleanup helper so both `CloseRelay()` and `RetireRelay()` clear it.

- [ ] **Step 4: Add registration reference smoke test**

In `src/unittest/darwin_relay_worker_io_test.cpp`, add:

```cpp
void RegisteredBindingSurvivesCallbackWithoutMapLookupRequirement() {
    TqDarwinRelayWorker worker(TqDarwinRelayWorkerConfig{});
    CHECK(worker.Start());

    int fds[2]{TqInvalidSocket, TqInvalidSocket};
    CHECK(socketpair(AF_UNIX, SOCK_STREAM, 0, fds) == 0);

    MsQuicStream stream{};
    TqRelayHandle handle{};
    TqDarwinRelayRegistration registration{};
    registration.TcpFd = fds[0];
    registration.Stream = &stream;
    registration.Handle = &handle;

    TqDarwinRelayRegistrationResult result = worker.RegisterRelayWithId(registration);
    CHECK(result.Ok);
    CHECK(stream.Context != nullptr);
    CHECK(stream.Callback == TqDarwinRelayWorker::StreamCallback);

    worker.UnregisterRelay(result.RelayId);
    worker.Stop();
    CloseSocketPairAfterRelayOwned(registration.TcpFd, fds);
}
```

- [ ] **Step 5: Run tests**

Run:

```bash
rtk cmake --build build --target tcpquic_darwin_relay_worker_io_test -j$(sysctl -n hw.ncpu)
rtk build/src/tcpquic_darwin_relay_worker_io_test
```

Expected: PASS.

- [ ] **Step 6: Commit**

```bash
rtk git add src/tunnel/darwin_relay_worker.cpp src/unittest/darwin_relay_worker_io_test.cpp
rtk git commit -m "refactor: attach darwin relay reference to stream binding"
```

### Task 3: Remove RelayMutex Lookup from Callback Receive

**Files:**
- Modify: `src/tunnel/darwin_relay_worker.h`
- Modify: `src/tunnel/darwin_relay_worker.cpp`
- Test: `src/unittest/darwin_relay_worker_io_test.cpp`

- [ ] **Step 1: Change `QueueDeferredQuicReceive()` declaration**

In `src/tunnel/darwin_relay_worker.h`, replace:

```cpp
    bool QueueDeferredQuicReceive(
        const std::shared_ptr<RelayState>& relay,
        MsQuicStream* stream,
        const QUIC_BUFFER* buffers,
        uint32_t bufferCount,
        bool fin);
```

with:

```cpp
    bool QueueDeferredQuicReceive(
        const std::shared_ptr<StreamBinding>& binding,
        MsQuicStream* stream,
        const QUIC_BUFFER* buffers,
        uint32_t bufferCount,
        bool fin);
```

- [ ] **Step 2: Update `QueueDeferredQuicReceive()` implementation**

At the start of the implementation, replace relay-derived fields with binding-derived fields:

```cpp
    if (binding == nullptr || stream == nullptr || buffers == nullptr || bufferCount == 0) {
        return false;
    }
    auto receive = std::make_shared<TqDarwinPendingQuicReceive>();
    receive->RelayId = binding->RelayId;
    receive->Stream = stream;
    receive->BindingOwner = binding;
    receive->Fin = fin;
```

Keep existing buffer slice copying, total length accounting, `ReserveCallbackReceiveBudget()`, event construction, `EnqueueEvent()`, and budget release-on-failure logic. Do not call `FindRelay()` in this function.

- [ ] **Step 3: Update RECEIVE callback**

In `StreamCallback()` RECEIVE handling, remove:

```cpp
        auto relay = worker->FindRelay(binding->RelayId);
        if (relay == nullptr) {
            return QUIC_STATUS_SUCCESS;
        }
```

Call:

```cpp
        if (!worker->QueueDeferredQuicReceive(
                bindingOwner,
                stream,
                event->RECEIVE.Buffers,
                event->RECEIVE.BufferCount,
                fin)) {
            worker->Errors.fetch_add(1, std::memory_order_relaxed);
            return QUIC_STATUS_SUCCESS;
        }
```

Do not write `relay->Closing` in this failure branch. The failure did not transfer receive ownership to worker; any close decision must happen through a later explicit event or lifecycle path.

- [ ] **Step 4: Update lock-count test expectation**

Rename `CallbackReceiveCurrentlyUsesLockedRelayLookup()` to:

```cpp
void CallbackReceiveDoesNotUseLockedRelayLookup()
```

Change:

```cpp
    CHECK(worker.FindRelayLockedCountForTest() > before);
```

to:

```cpp
    CHECK(worker.FindRelayLockedCountForTest() == before);
```

- [ ] **Step 5: Run receive callback tests**

Run:

```bash
rtk cmake --build build --target tcpquic_darwin_relay_worker_io_test -j$(sysctl -n hw.ncpu)
rtk build/src/tcpquic_darwin_relay_worker_io_test
```

Expected: PASS, including `CallbackReceiveDoesNotUseLockedRelayLookup`.

- [ ] **Step 6: Commit**

```bash
rtk git add src/tunnel/darwin_relay_worker.h src/tunnel/darwin_relay_worker.cpp src/unittest/darwin_relay_worker_io_test.cpp
rtk git commit -m "refactor: avoid relay map lookup in darwin receive callback"
```

### Task 4: Remove RelayMutex Lookup from Callback Close Events

**Files:**
- Modify: `src/tunnel/darwin_relay_worker.cpp`
- Test: `src/unittest/darwin_relay_worker_io_test.cpp`

- [ ] **Step 1: Add shutdown callback test**

In `src/unittest/darwin_relay_worker_io_test.cpp`, add:

```cpp
void QuicShutdownCallbackClosesViaWorkerEventWithoutLockedLookup() {
    TqDarwinRelayWorker worker(TqDarwinRelayWorkerConfig{});
    CHECK(worker.Start());

    int fds[2]{TqInvalidSocket, TqInvalidSocket};
    CHECK(socketpair(AF_UNIX, SOCK_STREAM, 0, fds) == 0);

    MsQuicStream stream{};
    TqRelayHandle handle{};
    TqDarwinRelayRegistration registration{};
    registration.TcpFd = fds[0];
    registration.Stream = &stream;
    registration.Handle = &handle;

    TqDarwinRelayRegistrationResult result = worker.RegisterRelayWithId(registration);
    CHECK(result.Ok);

    const uint64_t before = worker.FindRelayLockedCountForTest();
    QUIC_STREAM_EVENT event{};
    event.Type = QUIC_STREAM_EVENT_SHUTDOWN_COMPLETE;
    CHECK(TqDarwinRelayWorker::StreamCallback(&stream, stream.Context, &event) == QUIC_STATUS_SUCCESS);
    CHECK(worker.FindRelayLockedCountForTest() == before);

    TqDarwinRelayWorkerSnapshot snapshot{};
    for (int i = 0; i < 200; ++i) {
        snapshot = worker.Snapshot();
        if (snapshot.ActiveRelays == 0) {
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    CHECK(snapshot.ActiveRelays == 0);

    worker.Stop();
    CloseSocketPairAfterRelayOwned(registration.TcpFd, fds);
}
```

- [ ] **Step 2: Remove callback close map lookup**

In `StreamCallback()` handling of `QUIC_STREAM_EVENT_PEER_SEND_ABORTED`, `QUIC_STREAM_EVENT_PEER_RECEIVE_ABORTED`, and `QUIC_STREAM_EVENT_SHUTDOWN_COMPLETE`, remove running-path `worker->FindRelay(binding->RelayId)`.

Use this fallback-only shape:

```cpp
            std::shared_ptr<RelayState> relay;
            if (!worker->Running.load(std::memory_order_acquire)) {
                relay = binding->Relay.lock();
            }
```

Keep event construction and enqueue logic. If enqueue fails and `relay != nullptr`, keep:

```cpp
                    worker->CloseRelay(relay, 1);
```

- [ ] **Step 3: Route worker close events through local lookup**

In `DrainEvents()` and `DrainEventsLocalOnly()`, replace close-event handlers:

```cpp
            if (auto relay = FindRelay(event.RelayId)) {
                CloseRelay(relay);
            }
```

with:

```cpp
            if (auto relay = FindRelayLocal(event.RelayId)) {
                CloseRelay(relay);
            }
```

- [ ] **Step 4: Run tests**

Run:

```bash
rtk cmake --build build --target tcpquic_darwin_relay_worker_io_test -j$(sysctl -n hw.ncpu)
rtk build/src/tcpquic_darwin_relay_worker_io_test
```

Expected: PASS, including shutdown callback locked lookup count staying unchanged.

- [ ] **Step 5: Commit**

```bash
rtk git add src/tunnel/darwin_relay_worker.cpp src/unittest/darwin_relay_worker_io_test.cpp
rtk git commit -m "refactor: avoid relay map lookup in darwin close callbacks"
```

### Task 5: Use Local Relay Lookup on Worker Events

**Files:**
- Modify: `src/tunnel/darwin_relay_worker.cpp`
- Test: `src/unittest/darwin_relay_worker_io_test.cpp`

- [ ] **Step 1: Change kqueue event lookup**

In `ProcessTcpEvents()`, replace:

```cpp
    std::shared_ptr<RelayState> relay = FindRelay(relayId);
```

with:

```cpp
    AssertWorkerThreadForRelayState();
    std::shared_ptr<RelayState> relay = FindRelayLocal(relayId);
```

- [ ] **Step 2: Change worker event lookup**

In `DrainEvents()` and `DrainEventsLocalOnly()`, replace worker-thread uses of `FindRelay(event.RelayId)` with `FindRelayLocal(event.RelayId)` for:

```text
QuicReceiveView
QuicSendComplete
QuicPeerSendAborted
QuicPeerReceiveAborted
QuicShutdownComplete
StopRelay
IdealSendBuffer
```

Keep public non-worker methods such as `UnregisterRelay()` and fallback helpers unchanged.

- [ ] **Step 3: Add worker lookup regression assertion**

In an existing TCP read/write test, record locked lookup count before writing to the TCP peer:

```cpp
    const uint64_t lockedBefore = worker.FindRelayLockedCountForTest();
```

After the worker processes the read/write event and the test observes expected bytes, assert:

```cpp
    CHECK(worker.FindRelayLockedCountForTest() == lockedBefore);
    CHECK(worker.FindRelayLocalCountForTest() > 0);
```

- [ ] **Step 4: Run IO tests**

Run:

```bash
rtk cmake --build build --target tcpquic_darwin_relay_worker_io_test -j$(sysctl -n hw.ncpu)
rtk build/src/tcpquic_darwin_relay_worker_io_test
```

Expected: PASS.

- [ ] **Step 5: Commit**

```bash
rtk git add src/tunnel/darwin_relay_worker.cpp src/unittest/darwin_relay_worker_io_test.cpp
rtk git commit -m "refactor: use local darwin relay lookup on worker hot paths"
```

### Task 6: Remove RelayState Mutex from TCP-to-QUIC Worker Path

**Files:**
- Modify: `src/tunnel/darwin_relay_worker.cpp`
- Test: `src/unittest/darwin_relay_worker_io_test.cpp`

- [ ] **Step 1: Remove lock from `DrainTcpReadable()` entry and counters**

In `DrainTcpReadable()`, replace:

```cpp
    {
        std::lock_guard<std::mutex> relayLock(relay->Mutex);
        if (relay->Closing || relay->TcpReadClosed) {
            return true;
        }
    }
```

with:

```cpp
    AssertWorkerThreadForRelayState();
    if (relay->Closing || relay->TcpReadClosed) {
        return true;
    }
```

Replace:

```cpp
            {
                std::lock_guard<std::mutex> relayLock(relay->Mutex);
                relay->TcpReadBytes += static_cast<uint64_t>(received);
            }
```

with:

```cpp
            relay->TcpReadBytes += static_cast<uint64_t>(received);
```

- [ ] **Step 2: Remove lock from TCP EOF and hard error state**

Replace EOF state update:

```cpp
            {
                std::lock_guard<std::mutex> relayLock(relay->Mutex);
                relay->TcpReadClosed = true;
                relay->TcpReadArmed = false;
            }
```

with:

```cpp
            relay->TcpReadClosed = true;
            relay->TcpReadArmed = false;
```

Replace hard error close state:

```cpp
        {
            std::lock_guard<std::mutex> relayLock(relay->Mutex);
            relay->Closing = true;
        }
```

with:

```cpp
        relay->Closing = true;
```

- [ ] **Step 3: Remove lock from `SubmitTcpBatchToQuic()`**

Replace the initial state block with:

```cpp
    AssertWorkerThreadForRelayState();
    if (relay->Closing) {
        return false;
    }
    if (fin) {
        relay->QuicSendFinSubmitted = true;
    }
    const bool enableQuicSends = relay->EnableQuicSends;
```

Replace relay id/binding read with:

```cpp
    const uint64_t relayId = relay->Id;
    std::shared_ptr<StreamBinding> binding = relay->Binding;
```

- [ ] **Step 4: Remove lock from `TrySubmitQuicSendOperation()` accounting**

Replace stream/accounting setup with:

```cpp
    AssertWorkerThreadForRelayState();
    if (relay->Closing || relay->Stream == nullptr) {
        Errors.fetch_add(1, std::memory_order_relaxed);
        return false;
    }
    stream = relay->Stream;
    info.BindingOwner = relay->Binding;
    raw->BindingOwner = relay->Binding;
    ++relay->SubmittingQuicSends;
    ++relay->InFlightQuicSends;
    relay->InFlightQuicSendBytes += raw->TotalBytes;
```

Replace later decrement with:

```cpp
    if (relay->SubmittingQuicSends > 0) {
        --relay->SubmittingQuicSends;
    }
```

Apply the same direct-field style for failure rollback, pending retry enqueue, and fatal close inside this function.

- [ ] **Step 5: Run TCP-to-QUIC focused tests**

Run:

```bash
rtk cmake --build build --target tcpquic_darwin_relay_worker_io_test -j$(sysctl -n hw.ncpu)
rtk build/src/tcpquic_darwin_relay_worker_io_test
```

Expected: TCP read, QUIC send complete, EOF/FIN, and backpressure tests pass.

- [ ] **Step 6: Commit**

```bash
rtk git add src/tunnel/darwin_relay_worker.cpp
rtk git commit -m "refactor: remove darwin relay state lock from tcp send path"
```

### Task 7: Remove RelayState Mutex from QUIC-to-TCP Worker Path

**Files:**
- Modify: `src/tunnel/darwin_relay_worker.cpp`
- Test: `src/unittest/darwin_relay_worker_io_test.cpp`

- [ ] **Step 1: Remove lock from `SetQuicReceiveEnabled()`**

Replace:

```cpp
    MsQuicStream* stream = nullptr;
    {
        std::lock_guard<std::mutex> relayLock(relay->Mutex);
        stream = relay->Stream;
    }
```

with:

```cpp
    AssertWorkerThreadForRelayState();
    MsQuicStream* stream = relay->Stream;
```

- [ ] **Step 2: Remove lock from pause/resume helpers**

In `MaybePauseQuicReceive()`, use:

```cpp
    AssertWorkerThreadForRelayState();
    const uint64_t pendingPressure = relay->PendingQuicReceiveBytes + relay->PendingTcpWriteBytes;
    const bool shouldPause = !relay->QuicReceivePaused &&
        pendingPressure >= MaxPendingQuicReceiveBytesPerRelay();
    if (shouldPause) {
        relay->QuicReceivePaused = true;
    }
```

On pause failure:

```cpp
        relay->QuicReceivePaused = false;
        relay->Closing = true;
```

Apply the symmetric direct-field change in `MaybeResumeQuicReceive()`.

- [ ] **Step 3: Remove lock from `FlushTcpWrites()` selection**

Replace the first locked selection block in `FlushTcpWrites()` with direct worker-owned access:

```cpp
        AssertWorkerThreadForRelayState();
        if (relay->Closing) {
            return false;
        }
        if (Config.TcpWriteBurstBytes != 0 && burstBytes >= Config.TcpWriteBurstBytes) {
            relay->TcpWriteArmed = true;
            return true;
        }
        uint64_t maxWriteBytes = Config.TcpWriteMaxBytes;
        if (Config.TcpWriteBurstBytes != 0) {
            const uint64_t remainingBurst = Config.TcpWriteBurstBytes - burstBytes;
            maxWriteBytes = maxWriteBytes == 0 ? remainingBurst : std::min(maxWriteBytes, remainingBurst);
        }
        if (!relay->PendingTcpWrites.empty()) {
            for (const auto& write : relay->PendingTcpWrites) {
                if (write == nullptr) {
                    continue;
                }
                const auto& view = write->View;
                if (iov.size() >= std::max<uint32_t>(1, Config.MaxIov) ||
                    (maxWriteBytes != 0 && attemptedBytes >= maxWriteBytes)) {
                    break;
                }
                uint64_t length = view.Len;
                if (maxWriteBytes != 0 && attemptedBytes + length > maxWriteBytes) {
                    length = maxWriteBytes - attemptedBytes;
                }
                if (length == 0) {
                    break;
                }
                iovec item{};
                item.iov_base = view.Data;
                item.iov_len = static_cast<size_t>(length);
                iov.push_back(item);
                writeSnapshot.push_back(write);
                attemptedBytes += length;
            }
        } else if (relay->TcpWriteShutdownQueued) {
            (void)::shutdown(relay->TcpFd, SHUT_WR);
            relay->TcpWriteShutdownQueued = false;
            relay->TcpWriteArmed = false;
            relay->TcpWriteClosed = true;
            return true;
        } else {
            relay->TcpWriteArmed = false;
            return true;
        }
```

- [ ] **Step 4: Remove lock from receive enqueue/discard path**

In `ProcessQuicReceiveViewEvent()`, `EnqueueQuicReceiveForTcp()`, and the `relay != nullptr` branch of `DiscardDeferredQuicReceive()`, remove relay lock blocks and use direct worker-owned updates:

```cpp
    AssertWorkerThreadForRelayState();
    relay->PendingQuicReceives.push_back(receive);
    relay->PendingQuicReceiveBytes += receive->TotalLength;
```

Preserve the `relay == nullptr` branch for missing-relay cleanup.

- [ ] **Step 5: Run QUIC-to-TCP focused tests**

Run:

```bash
rtk cmake --build build --target tcpquic_darwin_relay_worker_io_test -j$(sysctl -n hw.ncpu)
rtk build/src/tcpquic_darwin_relay_worker_io_test
```

Expected: partial write, pause/resume, missing relay receive, compressed receive, and corrupt compressed receive tests pass.

- [ ] **Step 6: Commit**

```bash
rtk git add src/tunnel/darwin_relay_worker.cpp
rtk git commit -m "refactor: remove darwin relay state lock from receive write path"
```

### Task 8: Snapshot Without Per-Relay State Lock

**Files:**
- Modify: `src/tunnel/darwin_relay_worker.cpp`
- Test: `src/unittest/darwin_relay_worker_io_test.cpp`

- [ ] **Step 1: Remove relay state lock from `SnapshotLocal()`**

In `SnapshotLocal()`, delete:

```cpp
            std::lock_guard<std::mutex> relayLock(relay->Mutex);
```

Keep the surrounding `RelayMutex` while iterating `Relays`; this stage only removes per-relay state locking and hot-path map locking.

- [ ] **Step 2: Add concurrent snapshot/unregister test**

In `src/unittest/darwin_relay_worker_io_test.cpp`, add:

```cpp
void SnapshotDuringConcurrentUnregisterRemainsBestEffort() {
    TqDarwinRelayWorker worker(TqDarwinRelayWorkerConfig{});
    CHECK(worker.Start());

    int fds[2]{TqInvalidSocket, TqInvalidSocket};
    CHECK(socketpair(AF_UNIX, SOCK_STREAM, 0, fds) == 0);

    MsQuicStream stream{};
    TqRelayHandle handle{};
    TqDarwinRelayRegistration registration{};
    registration.TcpFd = fds[0];
    registration.Stream = &stream;
    registration.Handle = &handle;

    TqDarwinRelayRegistrationResult result = worker.RegisterRelayWithId(registration);
    CHECK(result.Ok);

    std::atomic<bool> done{false};
    std::thread snapshotter([&] {
        while (!done.load(std::memory_order_acquire)) {
            (void)worker.Snapshot();
        }
    });

    worker.UnregisterRelay(result.RelayId);
    done.store(true, std::memory_order_release);
    snapshotter.join();

    CHECK(worker.Snapshot().ActiveRelays == 0);
    worker.Stop();
    CloseSocketPairAfterRelayOwned(registration.TcpFd, fds);
}
```

- [ ] **Step 3: Run tests**

Run:

```bash
rtk cmake --build build --target tcpquic_darwin_relay_worker_io_test -j$(sysctl -n hw.ncpu)
rtk build/src/tcpquic_darwin_relay_worker_io_test
```

Expected: PASS without crashes or hangs.

- [ ] **Step 4: Commit**

```bash
rtk git add src/tunnel/darwin_relay_worker.cpp src/unittest/darwin_relay_worker_io_test.cpp
rtk git commit -m "refactor: snapshot darwin relay state on worker thread"
```

### Task 9: Audit Remaining Lock Sites and Update macOS Notes

**Files:**
- Modify: `src/tunnel/darwin_relay_worker.cpp`
- Modify: `docs/relay_macos.md`

- [ ] **Step 1: Audit remaining `RelayState::Mutex` usage**

Run:

```bash
rtk rg -n "relay->Mutex|RelayState::Mutex|std::lock_guard<std::mutex> relayLock" src/tunnel/darwin_relay_worker.cpp
```

Expected allowed remaining sites:

```text
RegisterRelayWithIdLocal initial setup during construction
UnregisterRelayLocal / RetireRelay / CloseRelay lifecycle cleanup
ClearPublicHandle
fallback paths that execute when worker is not running
```

There must be no matches in:

```text
DrainTcpReadable
SubmitTcpBatchToQuic
TrySubmitQuicSendOperation
CompleteQuicSend
ProcessQuicReceiveViewEvent
EnqueueQuicReceiveForTcp
FlushTcpWrites
MaybePauseQuicReceive
MaybeResumeQuicReceive
SnapshotLocal per-relay scan
```

- [ ] **Step 2: Audit remaining `FindRelay()` hot-path usage**

Run:

```bash
rtk rg -n "FindRelay\\(" src/tunnel/darwin_relay_worker.cpp
```

Expected allowed remaining direct calls:

```text
FindRelay definition
non-worker public/fallback lifecycle path
test-only helper path if present
```

There must be no `FindRelay()` call in:

```text
StreamCallback RECEIVE
StreamCallback peer abort/shutdown handling while worker is running
ProcessTcpEvents
DrainEvents worker event handlers
DrainEventsLocalOnly worker event handlers
```

- [ ] **Step 3: Update `docs/relay_macos.md`**

In the ranking table row for `RelayState::Mutex`, replace the suggestion text with:

```markdown
已规划分阶段实现：active relay 数据面字段归 worker thread 独占，callback 只投递事件，snapshot 继续事件化构造；`RelayState::Mutex` 保留为生命周期和非 worker fallback 边界。
```

In the ranking table row for `TqDarwinRelayWorker::RelayMutex`, replace the suggestion text with:

```markdown
已纳入同一阶段热路径出清：worker kqueue/event 路径使用 `FindRelayLocal()`；callback 通过 `StreamBinding` 持有稳定 relay 信息，不再为 RECEIVE/abort/shutdown 查 worker map；`RelayMutex` 保留 register/unregister/retire/purge 等生命周期 map 边界。
```

Add a short note under section 2:

```markdown
> 跟进计划：`docs/superpowers/specs/2026-07-04-darwin-relay-state-lock-design.md` 和 `docs/superpowers/plans/2026-07-04-darwin-relay-state-lock.md` 已覆盖排名 1、2 的锁热点热路径出清。排名 3、4 的 send completion 锁应作为下一组独立设计处理。
```

- [ ] **Step 4: Run full Darwin relay tests**

Run on macOS:

```bash
rtk cmake --build build --target tcpquic_darwin_relay_worker_io_test tcpquic_darwin_relay_worker_queue_test tcpquic_darwin_relay_metrics_test tcpquic_darwin_reactor_test -j$(sysctl -n hw.ncpu)
rtk build/src/tcpquic_darwin_relay_worker_io_test
rtk build/src/tcpquic_darwin_relay_worker_queue_test
rtk build/src/tcpquic_darwin_relay_metrics_test
rtk build/src/tcpquic_darwin_reactor_test
```

Expected: all commands PASS on macOS.

- [ ] **Step 5: Optional Linux sanity build**

Run on Linux if the implementation was edited there:

```bash
rtk cmake -S . -B build
rtk cmake --build build --target tcpquic_relay_buffer_test tcpquic_relay_alloc_test -j$(nproc)
rtk build/src/tcpquic_relay_buffer_test
rtk build/src/tcpquic_relay_alloc_test
```

Expected: PASS.

- [ ] **Step 6: Commit**

```bash
rtk git add src/tunnel/darwin_relay_worker.cpp src/tunnel/darwin_relay_worker.h src/unittest/darwin_relay_worker_io_test.cpp src/unittest/darwin_relay_worker_queue_test.cpp docs/relay_macos.md
rtk git commit -m "refactor: clear darwin relay state and map locks from hot paths"
```

## Self-Review Checklist

- [ ] Every task maps to `docs/superpowers/specs/2026-07-04-darwin-relay-state-lock-design.md`.
- [ ] The plan clears `RelayState::Mutex` from worker data-plane functions.
- [ ] The plan clears `RelayMutex` from callback receive/close and worker event/kqueue hot paths.
- [ ] The plan does not rewrite `KnownSendMutex` or `CompletionState::Mutex`.
- [ ] Every lock-path change has a focused test command.
- [ ] Callback receive ownership is still completed on missing relay or failed worker enqueue.
- [ ] `Snapshot()` remains eventized for running workers.
