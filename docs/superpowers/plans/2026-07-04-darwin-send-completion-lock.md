# Darwin Send Completion Lock Cleanup Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 将 macOS Darwin relay active worker send completion 主路径从 `KnownSendMutex` 和 `CompletionState::Mutex` 中移出，同时保留同步 completion、late completion、worker stop fallback 的安全性。

**Architecture:** 使用 `TqDarwinRelaySendOperation` 自带的原子状态和不可变 metadata 作为 callback claim 入口；active worker 使用 worker-only active send map，不再持有 `KnownSendMutex`。`CompletionState::Mutex` 和 fallback registry 只保留给 worker 停止、detached/retired completion、迟到 callback 等边界。

**Tech Stack:** C++17, MsQuic stream callback, Darwin kqueue relay worker, CMake, existing Darwin relay worker tests under `TCPQUIC_TESTING`.

---

## File Structure

- Modify: `src/tunnel/darwin_relay_worker.h`
  - Add send operation state enum/helpers.
  - Add active/fallback send operation map declarations.
  - Add `TCPQUIC_TESTING` lock-path counters and accessors.
- Modify: `src/tunnel/darwin_relay_worker.cpp`
  - Split active worker operation bookkeeping from fallback bookkeeping.
  - Move SEND_COMPLETE callback claim from `CompletionState::Mutex` map lookup to operation atomic state.
  - Preserve detached/fallback cleanup with explicit registry boundaries.
- Modify: `src/unittest/darwin_relay_worker_io_test.cpp`
  - Add characterization tests and update them as active path locks are removed.
  - Cover synchronous SEND_COMPLETE, queue-full callback, running=false/worker-exit, late detached completion.
- Modify: `docs/relay_macos.md`
  - Update rank 3/4 status after implementation.

### Task 1: Add Send Completion Lock Observability

**Files:**
- Modify: `src/tunnel/darwin_relay_worker.h`
- Modify: `src/tunnel/darwin_relay_worker.cpp`
- Test: `src/unittest/darwin_relay_worker_io_test.cpp`

- [ ] **Step 1: Add test counter accessors**

In `src/tunnel/darwin_relay_worker.h`, under existing `#if defined(TCPQUIC_TESTING)` public accessors, add:

```cpp
    uint64_t KnownSendLockedCountForTest() const;
    uint64_t CompletionStateLockedCountForTest() const;
    uint64_t ActiveSendLocalRegisterCountForTest() const;
    uint64_t ActiveSendLocalCompleteCountForTest() const;
    uint64_t FallbackSendCompletionCountForTest() const;
```

In the private `#if defined(TCPQUIC_TESTING)` counter block, add:

```cpp
    mutable std::atomic<uint64_t> KnownSendLockedCount{0};
    mutable std::atomic<uint64_t> CompletionStateLockedCount{0};
    mutable std::atomic<uint64_t> ActiveSendLocalRegisterCount{0};
    mutable std::atomic<uint64_t> ActiveSendLocalCompleteCount{0};
    mutable std::atomic<uint64_t> FallbackSendCompletionCount{0};
```

- [ ] **Step 2: Implement accessors**

In `src/tunnel/darwin_relay_worker.cpp`, near existing testing accessors, add:

```cpp
uint64_t TqDarwinRelayWorker::KnownSendLockedCountForTest() const {
    return KnownSendLockedCount.load(std::memory_order_relaxed);
}

uint64_t TqDarwinRelayWorker::CompletionStateLockedCountForTest() const {
    return CompletionStateLockedCount.load(std::memory_order_relaxed);
}

uint64_t TqDarwinRelayWorker::ActiveSendLocalRegisterCountForTest() const {
    return ActiveSendLocalRegisterCount.load(std::memory_order_relaxed);
}

uint64_t TqDarwinRelayWorker::ActiveSendLocalCompleteCountForTest() const {
    return ActiveSendLocalCompleteCount.load(std::memory_order_relaxed);
}

uint64_t TqDarwinRelayWorker::FallbackSendCompletionCountForTest() const {
    return FallbackSendCompletionCount.load(std::memory_order_relaxed);
}
```

- [ ] **Step 3: Increment counters at current lock sites**

In `RegisterKnownSendOperation()`, `RegisterKnownSendOperationLocal()`, `MarkKnownSendOperationSubmitted()`, `MarkKnownSendOperationSubmittedLocal()`, `UnregisterKnownSendOperation()`, and `UnregisterKnownSendOperationLocal()`, increment before taking `KnownSendMutex`:

```cpp
#if defined(TCPQUIC_TESTING)
    KnownSendLockedCount.fetch_add(1, std::memory_order_relaxed);
#endif
```

In every function that locks `CompletionState::Mutex` for send operation map access (`RegisterKnownSendOperation*`, `MarkKnownSendOperationSubmitted*`, `TryClaimKnownSendCompletionEvent()`, `UnregisterCompletionStateOperation()`), increment immediately before the lock:

```cpp
#if defined(TCPQUIC_TESTING)
    CompletionStateLockedCount.fetch_add(1, std::memory_order_relaxed);
#endif
```

In `RegisterKnownSendOperationLocal()` increment:

```cpp
#if defined(TCPQUIC_TESTING)
    ActiveSendLocalRegisterCount.fetch_add(1, std::memory_order_relaxed);
#endif
```

In `CompleteQuicSend()` after a worker-thread operation is successfully unregistered through the local path, increment:

```cpp
#if defined(TCPQUIC_TESTING)
    if (workerThread) {
        ActiveSendLocalCompleteCount.fetch_add(1, std::memory_order_relaxed);
    }
#endif
```

In fallback completion paths, increment `FallbackSendCompletionCount`:

```cpp
#if defined(TCPQUIC_TESTING)
    FallbackSendCompletionCount.fetch_add(1, std::memory_order_relaxed);
#endif
```

Add this increment in `CompleteDetachedQuicSend()` and in `CompleteQuicSend()` when `workerThread == false`.

- [ ] **Step 4: Add characterization test for current active worker lock usage**

In `src/unittest/darwin_relay_worker_io_test.cpp`, add:

```cpp
void ActiveWorkerSendCompleteCurrentlyUsesKnownSendLocks() {
    int fds[2]{TqInvalidSocket, TqInvalidSocket};
    CHECK(socketpair(AF_UNIX, SOCK_STREAM, 0, fds) == 0);

    ResetFakeStreamSend(QUIC_STATUS_SUCCESS);
    TqDarwinRelayWorkerConfig config{};
    config.ReadChunkSize = 4096;
    config.ReadBatchBytes = 4096;
    config.EventQueueCapacity = 16;
    config.MaxBufferedQuicSendBytes = 64 * 1024;
    TqDarwinRelayWorker worker(config);
    worker.SetStreamSendForTest(FakeStreamSend);
    TqRelayHandle handle{};
    CHECK(worker.StartForTest());

    TqDarwinRelayRegistration registration{};
    registration.TcpFd = fds[1];
    registration.Stream = reinterpret_cast<MsQuicStream*>(static_cast<uintptr_t>(1));
    registration.Handle = &handle;
    registration.EnableQuicSends = true;

    TqDarwinRelayRegistrationResult result = worker.RegisterRelayWithId(registration);
    CHECK(result.Ok);

    const uint64_t knownBefore = worker.KnownSendLockedCountForTest();
    const uint64_t completionBefore = worker.CompletionStateLockedCountForTest();
    const uint64_t localRegisterBefore = worker.ActiveSendLocalRegisterCountForTest();
    const uint64_t localCompleteBefore = worker.ActiveSendLocalCompleteCountForTest();

    const char payload[] = "active-worker-send-lock-characterization";
    CHECK(write(fds[0], payload, sizeof(payload) - 1) == static_cast<ssize_t>(sizeof(payload) - 1));
    CHECK(worker.InvokeTcpEventForTest(result.RelayId, EVFILT_READ, 0, 0));
    CHECK(g_sendCalls.load(std::memory_order_acquire) == 1);
    CHECK(worker.InFlightQuicSendCountForTest(result.RelayId) == 1);

    CHECK(worker.CompleteOneInFlightSendForTest(result.RelayId) != 0);
    CHECK(worker.InFlightQuicSendCountForTest(result.RelayId) == 0);
    CHECK(worker.KnownSendOperationCountForTest() == 0);
    CHECK(worker.ActiveSendLocalRegisterCountForTest() > localRegisterBefore);
    CHECK(worker.ActiveSendLocalCompleteCountForTest() > localCompleteBefore);
    CHECK(worker.KnownSendLockedCountForTest() > knownBefore);
    CHECK(worker.CompletionStateLockedCountForTest() > completionBefore);

    worker.UnregisterRelay(result.RelayId);
    worker.Stop();
    worker.SetStreamSendForTest(nullptr);
    CloseSocketPairAfterRelayOwned(registration.TcpFd, fds);
}
```

Register it in `main()` near the send completion tests:

```cpp
    ActiveWorkerSendCompleteCurrentlyUsesKnownSendLocks();
```

- [ ] **Step 5: Run focused test**

Run on macOS:

```bash
rtk cmake --build build --target tcpquic_darwin_relay_worker_io_test -j$(sysctl -n hw.ncpu)
rtk build/src/tcpquic_darwin_relay_worker_io_test
```

Expected: PASS, proving current active worker send completion still takes both send-operation locks.

On Linux, run:

```bash
rtk cmake --build build --target tcpquic_relay_buffer_test tcpquic_relay_alloc_test -j$(nproc)
rtk proxy ./build/bin/Release/tcpquic_relay_buffer_test
rtk proxy ./build/bin/Release/tcpquic_relay_alloc_test
```

Expected: both binaries exit 0. If the Darwin target is unavailable, record `No rule to make target 'tcpquic_darwin_relay_worker_io_test'`.

- [ ] **Step 6: Commit**

```bash
rtk git add src/tunnel/darwin_relay_worker.h src/tunnel/darwin_relay_worker.cpp src/unittest/darwin_relay_worker_io_test.cpp
rtk git commit -m "test: instrument darwin send completion locks"
```

### Task 2: Add Operation State and Immutable Metadata

**Files:**
- Modify: `src/tunnel/darwin_relay_worker.h`
- Modify: `src/tunnel/darwin_relay_worker.cpp`
- Test: `src/unittest/darwin_relay_worker_io_test.cpp`

- [ ] **Step 1: Add state enum and metadata fields**

In `src/tunnel/darwin_relay_worker.h`, immediately before `struct TqDarwinRelaySendOperation`, add:

```cpp
enum class TqDarwinSendOperationState : uint32_t {
    Created = 0,
    Registered = 1,
    Submitted = 2,
    CompletionClaimed = 3,
    Completed = 4,
    Detached = 5,
};
```

In `TqDarwinRelaySendOperation`, add state and immutable callback metadata:

```cpp
    std::atomic<uint32_t> State{static_cast<uint32_t>(TqDarwinSendOperationState::Created)};
    uint64_t CompletionRelayId{0};
    uint64_t CompletionTotalBytes{0};
    bool CompletionFin{false};
    std::shared_ptr<void> CompletionBindingOwner;
```

- [ ] **Step 2: Add operation state helpers**

In `src/tunnel/darwin_relay_worker.h`, add member functions inside `TqDarwinRelaySendOperation`:

```cpp
    bool TryTransition(TqDarwinSendOperationState expected, TqDarwinSendOperationState desired) {
        uint32_t value = static_cast<uint32_t>(expected);
        return State.compare_exchange_strong(
            value,
            static_cast<uint32_t>(desired),
            std::memory_order_acq_rel,
            std::memory_order_acquire);
    }

    bool TryMarkRegistered() {
        return TryTransition(TqDarwinSendOperationState::Created, TqDarwinSendOperationState::Registered);
    }

    bool TryMarkSubmitted() {
        uint32_t value = static_cast<uint32_t>(TqDarwinSendOperationState::Registered);
        return State.compare_exchange_strong(
            value,
            static_cast<uint32_t>(TqDarwinSendOperationState::Submitted),
            std::memory_order_acq_rel,
            std::memory_order_acquire);
    }

    bool TryClaimCompletion() {
        uint32_t value = State.load(std::memory_order_acquire);
        for (;;) {
            const auto state = static_cast<TqDarwinSendOperationState>(value);
            if (state == TqDarwinSendOperationState::CompletionClaimed ||
                state == TqDarwinSendOperationState::Completed ||
                state == TqDarwinSendOperationState::Detached) {
                return false;
            }
            if (State.compare_exchange_weak(
                    value,
                    static_cast<uint32_t>(TqDarwinSendOperationState::CompletionClaimed),
                    std::memory_order_acq_rel,
                    std::memory_order_acquire)) {
                return true;
            }
        }
    }

    bool TryMarkCompleted() {
        uint32_t value = State.load(std::memory_order_acquire);
        for (;;) {
            const auto state = static_cast<TqDarwinSendOperationState>(value);
            if (state == TqDarwinSendOperationState::Completed) {
                return false;
            }
            if (state == TqDarwinSendOperationState::Detached) {
                return false;
            }
            if (State.compare_exchange_weak(
                    value,
                    static_cast<uint32_t>(TqDarwinSendOperationState::Completed),
                    std::memory_order_acq_rel,
                    std::memory_order_acquire)) {
                return true;
            }
        }
    }

    bool MarkDetached() {
        uint32_t value = State.load(std::memory_order_acquire);
        for (;;) {
            const auto state = static_cast<TqDarwinSendOperationState>(value);
            if (state == TqDarwinSendOperationState::Completed ||
                state == TqDarwinSendOperationState::Detached) {
                return false;
            }
            if (State.compare_exchange_weak(
                    value,
                    static_cast<uint32_t>(TqDarwinSendOperationState::Detached),
                    std::memory_order_acq_rel,
                    std::memory_order_acquire)) {
                return true;
            }
        }
    }

    bool IsCompletionClaimed() const {
        return static_cast<TqDarwinSendOperationState>(State.load(std::memory_order_acquire)) ==
            TqDarwinSendOperationState::CompletionClaimed;
    }
```

- [ ] **Step 3: Populate immutable metadata during submit**

In `TrySubmitQuicSendOperation()`, after `info.BindingOwner = relay->Binding;` and before incrementing relay counters, add:

```cpp
    raw->CompletionRelayId = info.RelayId;
    raw->CompletionTotalBytes = info.TotalBytes;
    raw->CompletionFin = info.Fin;
    raw->CompletionBindingOwner = info.BindingOwner;
    if (!raw->TryMarkRegistered()) {
        Errors.fetch_add(1, std::memory_order_relaxed);
        return false;
    }
```

After `MarkKnownSendOperationSubmitted*` succeeds and before `SubmittingQuicSends` is decremented, call:

```cpp
    (void)raw->TryMarkSubmitted();
```

- [ ] **Step 4: Guard completion with state**

In `CompleteQuicSend()`, after operation null check and before unregistering, add:

```cpp
    if (!operation->TryMarkCompleted()) {
        Errors.fetch_add(1, std::memory_order_relaxed);
        return;
    }
```

If this breaks existing synchronous completion tests because callback has already claimed the operation, adjust `TryMarkCompleted()` in Step 2 to allow transition from `CompletionClaimed` to `Completed`, which the provided helper already does.

- [ ] **Step 5: Add state smoke test**

In `src/unittest/darwin_relay_worker_io_test.cpp`, add:

```cpp
void SendOperationStateTransitionsAreSingleClaim() {
    TqDarwinRelaySendOperation operation{};
    CHECK(operation.TryMarkRegistered());
    CHECK(!operation.TryMarkRegistered());
    CHECK(operation.TryClaimCompletion());
    CHECK(!operation.TryClaimCompletion());
    CHECK(operation.TryMarkCompleted());
    CHECK(!operation.TryMarkCompleted());
}
```

Register it in `main()` before worker-based send completion tests:

```cpp
    SendOperationStateTransitionsAreSingleClaim();
```

- [ ] **Step 6: Run tests**

Run on macOS:

```bash
rtk cmake --build build --target tcpquic_darwin_relay_worker_io_test -j$(sysctl -n hw.ncpu)
rtk build/src/tcpquic_darwin_relay_worker_io_test
```

Expected: PASS, including existing synchronous and late completion tests.

- [ ] **Step 7: Commit**

```bash
rtk git add src/tunnel/darwin_relay_worker.h src/tunnel/darwin_relay_worker.cpp src/unittest/darwin_relay_worker_io_test.cpp
rtk git commit -m "refactor: add darwin send operation state"
```

### Task 3: Split Worker-Local Active Send Operations

**Files:**
- Modify: `src/tunnel/darwin_relay_worker.h`
- Modify: `src/tunnel/darwin_relay_worker.cpp`
- Test: `src/unittest/darwin_relay_worker_io_test.cpp`

- [ ] **Step 1: Add worker-local active map**

In `src/tunnel/darwin_relay_worker.h`, keep `KnownSendOperations` for fallback and add a worker-only map:

```cpp
    std::unordered_map<TqDarwinRelaySendOperation*, KnownSendOperationInfo> ActiveSendOperations;
```

Place it next to `KnownSendOperations`.

- [ ] **Step 2: Make local register use active map**

Replace `RegisterKnownSendOperationLocal()` implementation with:

```cpp
void TqDarwinRelayWorker::RegisterKnownSendOperationLocal(
    TqDarwinRelaySendOperation* operation,
    const KnownSendOperationInfo& info) {
    AssertWorkerThreadForRelayState();
#if defined(TCPQUIC_TESTING)
    ActiveSendLocalRegisterCount.fetch_add(1, std::memory_order_relaxed);
#endif
    ActiveSendOperations.emplace(operation, info);
    if (auto binding = std::static_pointer_cast<StreamBinding>(info.BindingOwner)) {
        if (binding->Completions != nullptr) {
            binding->Completions->KnownSendOperationCount.fetch_add(1, std::memory_order_acq_rel);
        }
    }
}
```

This function must not increment `KnownSendLockedCount` and must not lock `CompletionState::Mutex`.

- [ ] **Step 3: Make local submitted use active map**

Replace `MarkKnownSendOperationSubmittedLocal()` implementation with:

```cpp
bool TqDarwinRelayWorker::MarkKnownSendOperationSubmittedLocal(
    TqDarwinRelaySendOperation* operation,
    KnownSendOperationInfo* info) {
    AssertWorkerThreadForRelayState();
    const auto it = ActiveSendOperations.find(operation);
    if (it == ActiveSendOperations.end()) {
        return false;
    }
    it->second.Submitting = false;
    if (info != nullptr) {
        *info = it->second;
    }
    return true;
}
```

- [ ] **Step 4: Make local unregister use active map**

Replace `UnregisterKnownSendOperationLocal()` implementation with:

```cpp
bool TqDarwinRelayWorker::UnregisterKnownSendOperationLocal(
    TqDarwinRelaySendOperation* operation,
    KnownSendOperationInfo* info) {
    AssertWorkerThreadForRelayState();
    const auto it = ActiveSendOperations.find(operation);
    if (it == ActiveSendOperations.end()) {
        return false;
    }
    KnownSendOperationInfo localInfo = it->second;
    ActiveSendOperations.erase(it);
    if (info != nullptr) {
        *info = localInfo;
    }
    if (auto binding = std::static_pointer_cast<StreamBinding>(localInfo.BindingOwner)) {
        const auto completions = binding->Completions;
        if (completions != nullptr) {
            completions->KnownSendOperationCount.fetch_sub(1, std::memory_order_acq_rel);
        }
        ClearRetiredStreamCallbackIfSafe(binding.get());
    }
#if defined(TCPQUIC_TESTING)
    ActiveSendLocalCompleteCount.fetch_add(1, std::memory_order_relaxed);
#endif
    return true;
}
```

- [ ] **Step 5: Update test helper count**

In `KnownSendOperationCountForTest()`, include both maps:

```cpp
uint64_t TqDarwinRelayWorker::KnownSendOperationCountForTest() {
    std::lock_guard<std::mutex> lock(KnownSendMutex);
    return KnownSendOperations.size() + ActiveSendOperations.size();
}
```

Because `ActiveSendOperations` is worker-only, tests using `StartForTest()` are worker-thread tests. For non-worker tests, this helper remains approximate for fallback assertions. If a test needs exact active count, add a worker-thread-specific helper in a later task.

- [ ] **Step 6: Update characterization test expectation**

Rename `ActiveWorkerSendCompleteCurrentlyUsesKnownSendLocks()` to:

```cpp
void ActiveWorkerSendCompleteDoesNotUseKnownSendLocks()
```

Change the final assertions:

```cpp
    CHECK(worker.KnownSendLockedCountForTest() == knownBefore);
    CHECK(worker.CompletionStateLockedCountForTest() == completionBefore);
```

Leave `ActiveSendLocalRegisterCountForTest()` and `ActiveSendLocalCompleteCountForTest()` assertions as `> before`.

- [ ] **Step 7: Run focused tests**

Run on macOS:

```bash
rtk cmake --build build --target tcpquic_darwin_relay_worker_io_test -j$(sysctl -n hw.ncpu)
rtk build/src/tcpquic_darwin_relay_worker_io_test
```

Expected: PASS. The renamed active worker test must prove no `KnownSendMutex` / `CompletionState::Mutex` activity for active worker completion.

- [ ] **Step 8: Commit**

```bash
rtk git add src/tunnel/darwin_relay_worker.h src/tunnel/darwin_relay_worker.cpp src/unittest/darwin_relay_worker_io_test.cpp
rtk git commit -m "refactor: use worker-local darwin send operations"
```

### Task 4: Move SEND_COMPLETE Claim to Operation State

**Files:**
- Modify: `src/tunnel/darwin_relay_worker.h`
- Modify: `src/tunnel/darwin_relay_worker.cpp`
- Test: `src/unittest/darwin_relay_worker_io_test.cpp`

- [ ] **Step 1: Change callback claim helper**

In `src/tunnel/darwin_relay_worker.h`, change `TryClaimKnownSendCompletionEvent()` declaration from:

```cpp
    bool TryClaimKnownSendCompletionEvent(
        StreamBinding* binding,
        TqDarwinRelaySendOperation* operation,
        KnownSendOperationInfo* info);
```

to:

```cpp
    bool TryClaimKnownSendCompletionEvent(
        TqDarwinRelaySendOperation* operation,
        KnownSendOperationInfo* info);
```

- [ ] **Step 2: Implement state-based claim**

Replace `TryClaimKnownSendCompletionEvent()` implementation with:

```cpp
bool TqDarwinRelayWorker::TryClaimKnownSendCompletionEvent(
    TqDarwinRelaySendOperation* operation,
    KnownSendOperationInfo* info) {
    if (operation == nullptr || operation->Magic != TqDarwinRelaySendOperation::MagicValue) {
        return false;
    }
    if (!operation->TryClaimCompletion()) {
        return false;
    }
    if (info != nullptr) {
        info->RelayId = operation->CompletionRelayId;
        info->TotalBytes = operation->CompletionTotalBytes;
        info->Fin = operation->CompletionFin;
        info->Submitting = !operation->IsSubmitted();
        info->CompletionEventClaimed = true;
        info->BindingOwner = operation->CompletionBindingOwner;
    }
    return true;
}
```

Add `IsSubmitted()` to `TqDarwinRelaySendOperation` if Task 2 did not add it:

```cpp
    bool IsSubmitted() const {
        const auto state = static_cast<TqDarwinSendOperationState>(State.load(std::memory_order_acquire));
        return state == TqDarwinSendOperationState::Submitted ||
            state == TqDarwinSendOperationState::CompletionClaimed ||
            state == TqDarwinSendOperationState::Completed;
    }
```

- [ ] **Step 3: Update callback call site**

In `StreamCallback()` SEND_COMPLETE handling, replace:

```cpp
        if (worker != nullptr && worker->TryClaimKnownSendCompletionEvent(binding, operation, &info)) {
```

with:

```cpp
        if (worker != nullptr && worker->TryClaimKnownSendCompletionEvent(operation, &info)) {
```

Keep the rest of the enqueue/fallback flow unchanged.

- [ ] **Step 4: Keep fallback map for detached completion**

Do not remove `UnregisterCompletionStateOperation()` yet. `CompleteDetachedQuicSend()` still uses it for operations that are no longer active worker-owned.

- [ ] **Step 5: Add callback claim lock regression test**

In the active worker async SEND_COMPLETE test, capture completion lock count before invoking callback:

```cpp
    const uint64_t completionBeforeCallback = worker.CompletionStateLockedCountForTest();
```

After `StreamCallback()` returns and worker drains completion event, assert:

```cpp
    CHECK(worker.CompletionStateLockedCountForTest() == completionBeforeCallback);
```

Add this focused test:

```cpp
void AsyncSendCompleteCallbackDoesNotLockCompletionState() {
    int fds[2]{TqInvalidSocket, TqInvalidSocket};
    CHECK(socketpair(AF_UNIX, SOCK_STREAM, 0, fds) == 0);

    ResetFakeStreamSend(QUIC_STATUS_SUCCESS);
    TqDarwinRelayWorkerConfig config{};
    config.ReadChunkSize = 4096;
    config.ReadBatchBytes = 4096;
    config.EventQueueCapacity = 16;
    config.MaxBufferedQuicSendBytes = 64 * 1024;
    TqDarwinRelayWorker worker(config);
    worker.SetStreamSendForTest(FakeStreamSend);
    TqRelayHandle handle{};
    CHECK(worker.StartForTest());

    TqDarwinRelayRegistration registration{};
    registration.TcpFd = fds[1];
    registration.Stream = reinterpret_cast<MsQuicStream*>(static_cast<uintptr_t>(1));
    registration.Handle = &handle;
    registration.EnableQuicSends = true;

    TqDarwinRelayRegistrationResult result = worker.RegisterRelayWithId(registration);
    CHECK(result.Ok);
    const char payload[] = "async-callback-no-completion-lock";
    CHECK(write(fds[0], payload, sizeof(payload) - 1) == static_cast<ssize_t>(sizeof(payload) - 1));
    CHECK(worker.InvokeTcpEventForTest(result.RelayId, EVFILT_READ, 0, 0));

    void* callbackContext = worker.StreamCallbackContextForTest(result.RelayId);
    void* sendContext = g_lastSendContext.load(std::memory_order_acquire);
    CHECK(callbackContext != nullptr);
    CHECK(sendContext != nullptr);
    const uint64_t completionBefore = worker.CompletionStateLockedCountForTest();

    QUIC_STREAM_EVENT event{};
    event.Type = QUIC_STREAM_EVENT_SEND_COMPLETE;
    event.SEND_COMPLETE.ClientContext = sendContext;
    CHECK(TqDarwinRelayWorker::StreamCallback(nullptr, callbackContext, &event) == QUIC_STATUS_SUCCESS);
    CHECK(worker.DrainOneEventForTest());
    CHECK(worker.CompletionStateLockedCountForTest() == completionBefore);
    CHECK(worker.KnownSendOperationCountForTest() == 0);

    worker.UnregisterRelay(result.RelayId);
    worker.Stop();
    worker.SetStreamSendForTest(nullptr);
    CloseSocketPairAfterRelayOwned(registration.TcpFd, fds);
}
```

- [ ] **Step 6: Run focused tests**

Run on macOS:

```bash
rtk cmake --build build --target tcpquic_darwin_relay_worker_io_test -j$(sysctl -n hw.ncpu)
rtk build/src/tcpquic_darwin_relay_worker_io_test
```

Expected: PASS; async callback claim does not increase `CompletionStateLockedCountForTest()`.

- [ ] **Step 7: Commit**

```bash
rtk git add src/tunnel/darwin_relay_worker.h src/tunnel/darwin_relay_worker.cpp src/unittest/darwin_relay_worker_io_test.cpp
rtk git commit -m "refactor: claim darwin send completion from operation state"
```

### Task 5: Preserve Detached and Late Completion Fallback

**Files:**
- Modify: `src/tunnel/darwin_relay_worker.h`
- Modify: `src/tunnel/darwin_relay_worker.cpp`
- Test: `src/unittest/darwin_relay_worker_io_test.cpp`

- [ ] **Step 1: Rename fallback map for clarity**

In `CompletionState`, rename `KnownSendOperations` to `FallbackSendOperations`:

```cpp
struct TqDarwinRelayWorker::CompletionState {
    mutable std::mutex Mutex;
    std::unordered_map<TqDarwinRelaySendOperation*, KnownSendOperationInfo> FallbackSendOperations;
    std::atomic<uint64_t> KnownSendOperationCount{0};
};
```

Update all fallback references in `RegisterKnownSendOperation()`, `UnregisterCompletionStateOperation()`, and `CompleteDetachedQuicSend()` to use `FallbackSendOperations`.

- [ ] **Step 2: Stop registering active local operations in fallback map**

Confirm `RegisterKnownSendOperationLocal()` does not insert into `FallbackSendOperations`. It should only increment `KnownSendOperationCount`.

For non-worker `RegisterKnownSendOperation()`, keep insertion into `FallbackSendOperations`:

```cpp
    if (binding != nullptr && binding->Completions != nullptr) {
#if defined(TCPQUIC_TESTING)
        CompletionStateLockedCount.fetch_add(1, std::memory_order_relaxed);
#endif
        std::lock_guard<std::mutex> completionLock(binding->Completions->Mutex);
        binding->Completions->FallbackSendOperations.emplace(operation, info);
        binding->Completions->KnownSendOperationCount.fetch_add(1, std::memory_order_acq_rel);
    }
```

- [ ] **Step 3: Add detach helper for stop/retire fallback**

Add a private helper declaration:

```cpp
    void DetachActiveSendOperationsForStop();
```

Implement:

```cpp
void TqDarwinRelayWorker::DetachActiveSendOperationsForStop() {
    AssertWorkerThreadForRelayState();
    for (auto& entry : ActiveSendOperations) {
        TqDarwinRelaySendOperation* operation = entry.first;
        KnownSendOperationInfo info = entry.second;
        if (operation != nullptr) {
            (void)operation->MarkDetached();
        }
        if (auto binding = std::static_pointer_cast<StreamBinding>(info.BindingOwner)) {
            if (binding->Completions != nullptr) {
#if defined(TCPQUIC_TESTING)
                CompletionStateLockedCount.fetch_add(1, std::memory_order_relaxed);
#endif
                std::lock_guard<std::mutex> completionLock(binding->Completions->Mutex);
                binding->Completions->FallbackSendOperations.emplace(operation, info);
            }
        }
    }
    ActiveSendOperations.clear();
}
```

- [ ] **Step 4: Call detach helper during worker shutdown**

In `Run()` exit path or `Stop()` worker-owned cleanup path where queued events are purged and known operations drain is awaited, call:

```cpp
    DetachActiveSendOperationsForStop();
```

Place it after no more worker events will process active completions and before `WaitForKnownOperationsToDrain()` depends on fallback completion. If the only safe location is in `Stop()` after the worker thread joins, use a worker-thread test-only path to verify no concurrent worker access remains.

- [ ] **Step 5: Preserve detached completion behavior**

In `CompleteDetachedQuicSend()`, keep:

```cpp
    if (!UnregisterCompletionStateOperation(binding->Completions, operation, &info)) {
        return false;
    }
```

After successful unregister, add:

```cpp
#if defined(TCPQUIC_TESTING)
    if (auto* worker = binding->Worker.load(std::memory_order_acquire)) {
        worker->FallbackSendCompletionCount.fetch_add(1, std::memory_order_relaxed);
    }
#endif
```

If `binding->Worker` is already null, skip the test counter.

- [ ] **Step 6: Run existing late completion tests**

Run on macOS:

```bash
rtk cmake --build build --target tcpquic_darwin_relay_worker_io_test -j$(sysctl -n hw.ncpu)
rtk build/src/tcpquic_darwin_relay_worker_io_test
```

Expected: existing tests covering late completion after stop, running=false wait, and binding relay fallback pass.

- [ ] **Step 7: Commit**

```bash
rtk git add src/tunnel/darwin_relay_worker.h src/tunnel/darwin_relay_worker.cpp src/unittest/darwin_relay_worker_io_test.cpp
rtk git commit -m "fix: preserve darwin detached send completion fallback"
```

### Task 6: Harden Synchronous SEND_COMPLETE Ordering

**Files:**
- Modify: `src/tunnel/darwin_relay_worker.cpp`
- Test: `src/unittest/darwin_relay_worker_io_test.cpp`

- [ ] **Step 1: Keep synchronous completion path explicit**

In `TrySubmitQuicSendOperation()`, after `TqDarwinRelayStreamSend()` returns and after `raw->TryMarkSubmitted()`, keep the existing logic that handles `CompletionEventClaimed`:

```cpp
    if (workerThread && submittedInfo.CompletionEventClaimed) {
        (void)DrainEvents(1);
    }
    if (completionAlreadyRan) {
        (void)operation.release();
        return true;
    }
```

If `CompletionEventClaimed` now comes from operation state instead of maps, set `submittedInfo.CompletionEventClaimed = raw->IsCompletionClaimed();` before this block.

- [ ] **Step 2: Use existing synchronous fake send controls**

`src/unittest/darwin_relay_worker_io_test.cpp` already has synchronous SEND_COMPLETE controls in `FakeStreamSend()`:

```cpp
    if (g_completeBeforeSendReturns.load(std::memory_order_acquire)) {
        void* callbackContext = g_syncCallbackContext.load(std::memory_order_acquire);
        CHECK(callbackContext != nullptr);
        QUIC_STREAM_EVENT event{};
        event.Type = QUIC_STREAM_EVENT_SEND_COMPLETE;
        event.SEND_COMPLETE.ClientContext = context;
        CHECK(TqDarwinRelayWorker::StreamCallback(nullptr, callbackContext, &event) == QUIC_STATUS_SUCCESS);
    }
```

Use these existing globals in the regression test:

```cpp
    g_syncCallbackContext.store(worker.StreamCallbackContextForTest(result.RelayId), std::memory_order_release);
    g_completeBeforeSendReturns.store(true, std::memory_order_release);
```

- [ ] **Step 3: Add synchronous completion regression test**

Add:

```cpp
void SynchronousSendCompleteDoesNotDoubleComplete() {
    int fds[2]{TqInvalidSocket, TqInvalidSocket};
    CHECK(socketpair(AF_UNIX, SOCK_STREAM, 0, fds) == 0);

    ResetFakeStreamSend(QUIC_STATUS_SUCCESS);
    TqDarwinRelayWorkerConfig config{};
    config.ReadChunkSize = 4096;
    config.ReadBatchBytes = 4096;
    config.EventQueueCapacity = 16;
    config.MaxBufferedQuicSendBytes = 64 * 1024;
    TqDarwinRelayWorker worker(config);
    worker.SetStreamSendForTest(FakeStreamSendCompletesSynchronously);
    TqRelayHandle handle{};
    CHECK(worker.StartForTest());

    TqDarwinRelayRegistration registration{};
    registration.TcpFd = fds[1];
    registration.Stream = reinterpret_cast<MsQuicStream*>(static_cast<uintptr_t>(1));
    registration.Handle = &handle;
    registration.EnableQuicSends = true;

    TqDarwinRelayRegistrationResult result = worker.RegisterRelayWithId(registration);
    CHECK(result.Ok);
    void* callbackContext = worker.StreamCallbackContextForTest(result.RelayId);
    CHECK(callbackContext != nullptr);
    g_syncCallbackContext.store(callbackContext, std::memory_order_release);
    g_completeBeforeSendReturns.store(true, std::memory_order_release);
    const char payload[] = "synchronous-send-complete";
    CHECK(write(fds[0], payload, sizeof(payload) - 1) == static_cast<ssize_t>(sizeof(payload) - 1));
    CHECK(worker.InvokeTcpEventForTest(result.RelayId, EVFILT_READ, 0, 0));
    CHECK(worker.DrainOneEventForTest());
    CHECK(worker.InFlightQuicSendCountForTest(result.RelayId) == 0);
    CHECK(worker.KnownSendOperationCountForTest() == 0);

    worker.UnregisterRelay(result.RelayId);
    worker.Stop();
    worker.SetStreamSendForTest(nullptr);
    g_completeBeforeSendReturns.store(false, std::memory_order_release);
    g_syncCallbackContext.store(nullptr, std::memory_order_release);
    CloseSocketPairAfterRelayOwned(registration.TcpFd, fds);
}
```

- [ ] **Step 4: Run tests**

Run on macOS:

```bash
rtk cmake --build build --target tcpquic_darwin_relay_worker_io_test -j$(sysctl -n hw.ncpu)
rtk build/src/tcpquic_darwin_relay_worker_io_test
```

Expected: synchronous completion test passes and `KnownSendOperationCountForTest()` returns 0.

- [ ] **Step 5: Commit**

```bash
rtk git add src/tunnel/darwin_relay_worker.cpp src/unittest/darwin_relay_worker_io_test.cpp
rtk git commit -m "test: cover darwin synchronous send completion ordering"
```

### Task 7: Audit Remaining Send Locks and Update macOS Notes

**Files:**
- Modify: `src/tunnel/darwin_relay_worker.cpp`
- Modify: `src/tunnel/darwin_relay_worker.h`
- Modify: `docs/relay_macos.md`
- Test: `src/unittest/darwin_relay_worker_io_test.cpp`

- [ ] **Step 1: Audit remaining KnownSendMutex usage**

Run:

```bash
rtk rg -n "KnownSendMutex|KnownSendOperations|FallbackSendOperations|CompletionStateLockedCount|KnownSendLockedCount" src/tunnel/darwin_relay_worker.cpp src/tunnel/darwin_relay_worker.h
```

Expected allowed remaining `KnownSendMutex` sites:

```text
KnownSendOperationCountForTest
non-worker RegisterKnownSendOperation fallback
non-worker MarkKnownSendOperationSubmitted fallback
non-worker UnregisterKnownSendOperation fallback
WaitForKnownOperationsToDrain / stop fallback
```

There must be no `KnownSendMutex` in:

```text
RegisterKnownSendOperationLocal
MarkKnownSendOperationSubmittedLocal
UnregisterKnownSendOperationLocal
active worker CompleteQuicSend
normal SEND_COMPLETE callback claim
```

- [ ] **Step 2: Audit remaining CompletionState::Mutex usage**

Run:

```bash
rtk rg -n "Completions->Mutex|CompletionState::Mutex|completionLock|FallbackSendOperations" src/tunnel/darwin_relay_worker.cpp src/tunnel/darwin_relay_worker.h
```

Expected allowed remaining sites:

```text
fallback RegisterKnownSendOperation
UnregisterCompletionStateOperation
CompleteDetachedQuicSend
DetachActiveSendOperationsForStop
retired stream cleanup support
```

There must be no `CompletionState::Mutex` in:

```text
TryClaimKnownSendCompletionEvent normal claim
RegisterKnownSendOperationLocal
MarkKnownSendOperationSubmittedLocal
UnregisterKnownSendOperationLocal
active worker CompleteQuicSend
```

- [ ] **Step 3: Update active worker lock test if needed**

Ensure `ActiveWorkerSendCompleteDoesNotUseKnownSendLocks()` asserts:

```cpp
    CHECK(worker.KnownSendLockedCountForTest() == knownBefore);
    CHECK(worker.CompletionStateLockedCountForTest() == completionBefore);
    CHECK(worker.FallbackSendCompletionCountForTest() == fallbackBefore);
```

Capture `fallbackBefore` before triggering the send:

```cpp
    const uint64_t fallbackBefore = worker.FallbackSendCompletionCountForTest();
```

- [ ] **Step 4: Update docs/relay_macos.md**

In `docs/relay_macos.md`, update rank 3 row suggestion from future tense to completed active-path cleanup:

```markdown
已完成 active worker 主路径出清：worker-local active send operation set 负责正常注册/提交/完成；`KnownSendMutex` 保留为非 worker fallback、stop drain 和 detached completion 边界。
```

Update rank 4 row suggestion:

```markdown
已完成正常 SEND_COMPLETE claim 热路径出清：callback 通过 operation 原子状态 claim completion，不再查 per-binding unordered_map；`CompletionState::Mutex` 保留为 fallback registry、detached/late completion 和 retired binding cleanup 边界。
```

Add a note below the table:

```markdown
> 跟进计划：send completion active worker 主路径已完成 rank 3、4 锁出清；剩余 fallback 锁只服务 worker stop、late callback、retired cleanup。下一组建议处理 receive callback 失败语义和 fake FIN abort。
```

- [ ] **Step 5: Run final Darwin relay tests**

Run on macOS:

```bash
rtk cmake --build build --target tcpquic_darwin_relay_worker_io_test tcpquic_darwin_relay_worker_queue_test tcpquic_darwin_relay_metrics_test tcpquic_darwin_reactor_test -j$(sysctl -n hw.ncpu)
rtk build/src/tcpquic_darwin_relay_worker_io_test
rtk build/src/tcpquic_darwin_relay_worker_queue_test
rtk build/src/tcpquic_darwin_relay_metrics_test
rtk build/src/tcpquic_darwin_reactor_test
```

Expected: all Darwin commands pass.

- [ ] **Step 6: Run Linux sanity**

Run on Linux development hosts:

```bash
rtk cmake --build build --target tcpquic_relay_buffer_test tcpquic_relay_alloc_test -j$(nproc)
rtk proxy ./build/bin/Release/tcpquic_relay_buffer_test
rtk proxy ./build/bin/Release/tcpquic_relay_alloc_test
```

Expected: both binaries exit 0.

- [ ] **Step 7: Commit**

```bash
rtk git add src/tunnel/darwin_relay_worker.h src/tunnel/darwin_relay_worker.cpp src/unittest/darwin_relay_worker_io_test.cpp docs/relay_macos.md
rtk git commit -m "refactor: clear darwin send completion locks from active path"
```

## Self-Review Checklist

- [ ] Every requirement in `docs/superpowers/specs/2026-07-04-darwin-send-completion-lock-design.md` maps to a task above.
- [ ] Active worker register/submit/complete stops using `KnownSendMutex`.
- [ ] Normal SEND_COMPLETE callback claim stops using `CompletionState::Mutex`.
- [ ] Fallback/detached/late completion remains synchronized and exactly-once.
- [ ] Synchronous SEND_COMPLETE is explicitly tested.
- [ ] Queue-full, running=false, worker-exit, late completion, retired cleanup behavior remains covered.
- [ ] `docs/relay_macos.md` documents completed rank 3/4 active-path cleanup and remaining fallback boundaries.
