# Darwin Relay State/Map Lock Hot-Path Cleanup Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 从当前 Darwin relay 代码出发，完成 `RelayState::Mutex` / `RelayMutex` 在稳定转发热路径上的出清，并将剩余锁点限制在 lifecycle、fallback、retired cleanup、snapshot map scan 和 test helper 边界。

**Architecture:** active relay 数据面字段归 worker thread 独占；callback 不直接写 worker-only `RelayState` 字段，只投递事件、管理 callback-local backpressure 或在 worker stopped 后走 fallback close。worker kqueue/event 路径使用 `FindRelayLocal()` 或事件 owner；running `Snapshot()` 继续事件化到 worker 线程构造 best-effort 快照。

**Design:** `docs/superpowers/specs/2026-07-04-darwin-relay-state-lock-design.md`

**Tech Stack:** C++17, Darwin kqueue, MsQuic stream callback, CMake, existing Darwin relay unit tests.

---

## File Structure

- Modify: `src/tunnel/darwin_relay_worker.h`
  - Keep existing test-only lookup counters and accessors (`FindRelayLockedCountForTest()` / `FindRelayLocalCountForTest()`).
  - Do not add new public production API for this plan.
- Modify: `src/tunnel/darwin_relay_worker.cpp`
  - Remove callback direct writes to worker-only relay fields.
  - Ensure worker data-plane paths use local lookup and local interest update.
  - Audit remaining relay/map locks.
- Modify: `src/unittest/darwin_relay_worker_io_test.cpp`
  - Add regression tests for callback no direct locked lookup, queue-full/budget-reject resume, allocation-failure close event, and worker data-plane lookup counts.
- Modify: `docs/relay_macos.md`
  - Mark rank 1–2 lock cleanup as implemented/in-progress after code changes.

---

## Current-Code Baseline

Before starting, verify the repository already contains the receive hardening and partial state-lock groundwork:

```bash
rtk rg -n "TqDarwinQuicReceiveEnqueueResult|CallbackPendingQuicReceives|std::weak_ptr<RelayState> Relay|FindRelayLocal\(|RelayMapAccessMutex" src/tunnel/darwin_relay_worker.* src/tunnel/darwin_relay_event_queue.h
```

Expected: matches exist for typed receive result, callback pending receive deque, `StreamBinding::Relay`, worker-local lookup, and `RelayMapAccessMutex`.

If any of these are missing, stop and rebase onto the branch containing receive callback hardening before executing this plan.

---

### Task 1: Add Lock-Boundary Regression Tests for Current Hot Paths

**Files:**
- Modify: `src/unittest/darwin_relay_worker_io_test.cpp`

- [ ] **Step 1: Add callback receive locked-lookup regression test**

Add this test near the existing receive callback tests:

```cpp
void CallbackReceiveDoesNotUseLockedRelayLookup() {
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
    CHECK(worker.FindRelayLockedCountForTest() == before);

    worker.UnregisterRelay(result.RelayId);
    worker.Stop();
    CloseSocketPairAfterRelayOwned(registration.TcpFd, fds);
}
```

Register it in `main()`.

- [ ] **Step 2: Add callback close locked-lookup regression test**

Add:

```cpp
void CallbackShutdownDoesNotUseLockedRelayLookup() {
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

Register it in `main()`.

- [ ] **Step 3: Add TCP worker path locked-lookup regression assertion**

Use an existing TCP read or TCP->QUIC test that writes to the socket peer and lets the worker process `EVFILT_READ`. Do not create a duplicate full relay setup; add these assertions to the existing path test.

```cpp
const uint64_t lockedBefore = worker.FindRelayLockedCountForTest();
```

After the worker has processed the read and produced the existing expected result, add:

```cpp
CHECK(worker.FindRelayLockedCountForTest() == lockedBefore);
CHECK(worker.FindRelayLocalCountForTest() > 0);
```

- [ ] **Step 4: Run focused test binary**

Run on macOS:

```bash
rtk cmake --build build --target tcpquic_darwin_relay_worker_io_test -j$(sysctl -n hw.ncpu)
rtk build/src/tcpquic_darwin_relay_worker_io_test
```

Expected: PASS. If an assertion fails because the current code still uses locked lookup, keep the test failure as the Task 2 red state and fix it there.

- [ ] **Step 5: Commit**

```bash
rtk git add src/unittest/darwin_relay_worker_io_test.cpp
rtk git commit -m "test: cover darwin relay hot-path lookup boundaries"
```

---

### Task 2: Remove Callback Direct Writes to Worker-Owned Relay State

**Files:**
- Modify: `src/tunnel/darwin_relay_worker.cpp`
- Test: `src/unittest/darwin_relay_worker_io_test.cpp`

- [ ] **Step 1: Remove queue-full direct pause-state write**

In `QueueDeferredQuicReceive()`, replace the queue-full branch shape:

```cpp
if (auto relay = binding->Relay.lock()) {
    relay->QuicReceivePaused = true;
}
PauseMsQuicReceiveFromCallback(stream);
(void)Wake();
return TqDarwinQuicReceiveEnqueueResult::EventQueueFull;
```

with:

```cpp
PauseMsQuicReceiveFromCallback(stream);
(void)Wake();
return TqDarwinQuicReceiveEnqueueResult::EventQueueFull;
```

Rationale: callback-local queue-full backpressure may disable MsQuic receive immediately, but the worker-owned `QuicReceivePaused` flag is maintained only by worker-thread code.

- [ ] **Step 2: Remove allocation-failure direct close-state write**

In `StreamCallback()` `AllocationFailed` case, replace:

```cpp
std::shared_ptr<RelayState> relay = binding->Relay.lock();
if (relay != nullptr) {
    relay->Closing = true;
    (void)worker->EnqueueRelayCloseFromCallback(
        relay,
        TqDarwinRelayEventType::QuicShutdownComplete);
}
```

with:

```cpp
if (std::shared_ptr<RelayState> relay = binding->Relay.lock()) {
    (void)worker->EnqueueRelayCloseFromCallback(
        relay,
        TqDarwinRelayEventType::QuicShutdownComplete);
}
```

- [ ] **Step 3: Remove budget-reject direct pause-state write**

In `StreamCallback()` `CallbackBudgetRejected` case, replace:

```cpp
if (std::shared_ptr<RelayState> relay = binding->Relay.lock()) {
    relay->QuicReceivePaused = true;
}
worker->PauseMsQuicReceiveFromCallback(stream);
worker->CompleteMsQuicReceiveFromCallback(stream, totalLength);
return QUIC_STATUS_SUCCESS;
```

with:

```cpp
worker->PauseMsQuicReceiveFromCallback(stream);
worker->CompleteMsQuicReceiveFromCallback(stream, totalLength);
return QUIC_STATUS_SUCCESS;
```

- [ ] **Step 4: Strengthen budget-reject resume test**

Find the existing `ReceiveCallbackBudgetRejectPausesAndCompletes()` test. Keep the existing pause and complete assertions, and preserve the existing worker-side resume assertions:

```cpp
CHECK(worker.DrainOneEventForTest());
CHECK(worker.FlushTcpWritableForTest(result.RelayId));
CHECK(g_receiveSetEnabledCalls.load(std::memory_order_acquire) == 2);
CHECK(g_lastReceiveSetEnabled.load(std::memory_order_acquire) == 1);
```

These assertions prove resume is driven by worker-side receive/write processing after the callback returns.

- [ ] **Step 5: Run receive callback tests**

Run:

```bash
rtk cmake --build build --target tcpquic_darwin_relay_worker_io_test -j$(sysctl -n hw.ncpu)
rtk build/src/tcpquic_darwin_relay_worker_io_test
```

Expected: PASS, including queue-full backpressure, budget reject, and allocation failure paths.

- [ ] **Step 6: Commit**

```bash
rtk git add src/tunnel/darwin_relay_worker.cpp src/unittest/darwin_relay_worker_io_test.cpp
rtk git commit -m "refactor: keep darwin receive callback state worker-owned"
```

---

### Task 3: Ensure TCP Interest Updates Use Worker-Local State on Data Plane

**Files:**
- Modify: `src/tunnel/darwin_relay_worker.cpp`
- Test: `src/unittest/darwin_relay_worker_io_test.cpp`

- [ ] **Step 1: Audit `UpdateTcpInterest()` call sites**

Run:

```bash
rtk rg -n "UpdateTcpInterest\(" src/tunnel/darwin_relay_worker.cpp
```

Expected before modification: definition of `UpdateTcpInterest()`, definition of `UpdateTcpInterestLocal()`, and call sites.

- [ ] **Step 2: Convert worker data-plane call sites to local variant**

For any call in worker data-plane functions, replace:

```cpp
UpdateTcpInterest(relay)
```

with:

```cpp
UpdateTcpInterestLocal(relay)
```

Worker data-plane functions to audit:

```text
SetTcpReadBackpressure
FlushTcpWrites
DrainTcpReadable
ProcessTcpEvents
```

Do not change lifecycle fallback code that runs off worker thread.

- [ ] **Step 3: Mark non-local helper as lifecycle-only**

Add a short comment above `UpdateTcpInterest()`:

```cpp
// Lifecycle/fallback helper: worker data-plane paths must use UpdateTcpInterestLocal().
```

Do not add comments that narrate obvious code; this comment records the concurrency boundary.

- [ ] **Step 4: Run worker IO tests**

Run:

```bash
rtk cmake --build build --target tcpquic_darwin_relay_worker_io_test -j$(sysctl -n hw.ncpu)
rtk build/src/tcpquic_darwin_relay_worker_io_test
```

Expected: PASS. TCP read pause/resume and TCP write EAGAIN tests still pass.

- [ ] **Step 5: Commit**

```bash
rtk git add src/tunnel/darwin_relay_worker.cpp src/unittest/darwin_relay_worker_io_test.cpp
rtk git commit -m "refactor: use local tcp interest updates on darwin data path"
```

---

### Task 4: Tighten Snapshot and Lifecycle Lock Boundaries

**Files:**
- Modify: `src/tunnel/darwin_relay_worker.cpp`
- Test: `src/unittest/darwin_relay_worker_io_test.cpp`

- [ ] **Step 1: Confirm `SnapshotLocal()` has no per-relay mutex**

Run:

```bash
rtk rg -n "SnapshotLocal|relayLock\(relay->Mutex\)|relay->Mutex" src/tunnel/darwin_relay_worker.cpp
```

Expected: `SnapshotLocal()` does not contain `std::lock_guard<std::mutex> relayLock(relay->Mutex)`. The current code should already satisfy this. If the search shows a per-relay lock inside `SnapshotLocal()`, remove it and keep `RelayMutex` around the map iteration for this plan.

- [ ] **Step 2: Add concurrent snapshot/unregister regression test**

Add near lifecycle tests:

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

Register it in `main()`.

- [ ] **Step 3: Document lifecycle-only relay locks with local helper comments**

For `RetireRelay()`, `CloseRelay()`, and `PurgeRetiredRelaysIfSafe()`, add concise comments only if the boundary is not already clear:

```cpp
// Lifecycle cleanup may run after worker exit or during unregister fallback; data-plane paths must not enter here for normal forwarding.
```

Place it above the lifecycle lock block, not inside every statement.

- [ ] **Step 4: Run focused tests**

Run:

```bash
rtk cmake --build build --target tcpquic_darwin_relay_worker_io_test -j$(sysctl -n hw.ncpu)
rtk build/src/tcpquic_darwin_relay_worker_io_test
```

Expected: PASS without hangs or crashes.

- [ ] **Step 5: Commit**

```bash
rtk git add src/tunnel/darwin_relay_worker.cpp src/unittest/darwin_relay_worker_io_test.cpp
rtk git commit -m "test: cover darwin snapshot unregister boundary"
```

---

### Task 5: Audit Remaining `FindRelay()` and `RelayState::Mutex` Sites

**Files:**
- Modify: `src/tunnel/darwin_relay_worker.cpp`
- Modify: `docs/relay_macos.md`

- [ ] **Step 1: Audit locked relay lookup sites**

Run:

```bash
rtk rg -n "FindRelay\(" src/tunnel/darwin_relay_worker.cpp
```

Allowed remaining sites:

```text
FindRelay definition
TCPQUIC_TESTING helper functions
non-worker fallback or public lifecycle paths
CompleteQuicSend non-worker fallback branch
```

There must be no `FindRelay()` call in:

```text
StreamCallback RECEIVE
StreamCallback peer abort/shutdown handling while Running is true
ProcessTcpEvents
DrainEvents worker event handlers
ProcessQuicReceiveViewEvent
FlushAllCallbackPendingQuicReceivesLocal
```

- [ ] **Step 2: Audit relay mutex sites**

Run:

```bash
rtk rg -n "relay->Mutex|relayLock\(|\)->Mutex" src/tunnel/darwin_relay_worker.cpp
```

Allowed remaining sites:

```text
TCPQUIC_TESTING helper functions
UpdateTcpInterest lifecycle/fallback helper if still present
RegisterRelayWithIdLocal initial setup only if still necessary
RetireRelay lifecycle cleanup
PurgeRetiredRelaysIfSafe retired cleanup observation
CompleteQuicSend non-worker/fallback branch
```

There must be no relay mutex lock in:

```text
DrainTcpReadable
SubmitTcpBatchToQuic
TrySubmitQuicSendOperation
RetryPendingQuicSends
CompleteQuicSend worker active branch
ProcessQuicReceiveViewEvent
EnqueueQuicReceiveForTcp
DiscardDeferredQuicReceive worker relay branch
FlushTcpWrites
MaybePauseQuicReceive
MaybeResumeQuicReceive
SnapshotLocal active relay scan
StreamCallback
QueueDeferredQuicReceive
```

- [ ] **Step 3: Fix any disallowed sites**

For each disallowed `FindRelay()` in worker data-plane code, use:

```cpp
auto relay = FindRelayLocal(relayId);
```

For each disallowed relay mutex in worker data-plane code, remove the lock. Each edited worker data-plane function must call `AssertWorkerThreadForRelayState()` at its boundary before direct `RelayState` field access.

For each disallowed callback direct field access, replace it with event enqueue or callback-local action as in Task 2.

- [ ] **Step 4: Update `docs/relay_macos.md` rank 1–2 status**

In the lock ranking table:

- For `RelayState::Mutex`, change the suggestion/status to say active worker data-plane hot paths are cleared or being cleared by this plan; remaining locks are lifecycle/fallback/test/retired cleanup boundaries.
- For `TqDarwinRelayWorker::RelayMutex`, change the suggestion/status to say worker data-plane uses `FindRelayLocal()` / event owner; remaining uses are map structure lifecycle and snapshot boundaries.

Under section 2 follow-up note, reference this design and plan as the rank 1–2 implementation plan. Keep existing references to send completion and receive callback hardening.

- [ ] **Step 5: Run full Darwin relay test set**

Run on macOS:

```bash
rtk cmake --build build --target tcpquic_darwin_relay_worker_io_test tcpquic_darwin_relay_worker_queue_test tcpquic_darwin_relay_metrics_test tcpquic_darwin_reactor_test -j$(sysctl -n hw.ncpu)
rtk build/src/tcpquic_darwin_relay_worker_io_test
rtk build/src/tcpquic_darwin_relay_worker_queue_test
rtk build/src/tcpquic_darwin_relay_metrics_test
rtk build/src/tcpquic_darwin_reactor_test
```

Expected: all commands PASS.

- [ ] **Step 6: Commit**

```bash
rtk git add src/tunnel/darwin_relay_worker.cpp src/tunnel/darwin_relay_worker.h src/unittest/darwin_relay_worker_io_test.cpp docs/relay_macos.md
rtk git commit -m "refactor: clear darwin relay state locks from hot paths"
```

---

### Task 6: Final Verification and Handoff Notes

**Files:**
- Modify: `docs/relay_macos.md` if final audit changes status text.

- [ ] **Step 1: Verify plan acceptance criteria**

Run:

```bash
rtk rg -n "StreamCallback|QueueDeferredQuicReceive|FindRelay\(|relay->Mutex|relayLock\(" src/tunnel/darwin_relay_worker.cpp
```

Expected:

- `StreamCallback()` does not call `FindRelay()`.
- `QueueDeferredQuicReceive()` does not write `RelayState` fields.
- worker data-plane functions do not lock `relay->Mutex`.
- remaining `FindRelay()` and `relay->Mutex` sites match Task 5 allowed list.

- [ ] **Step 2: Verify docs mention remaining non-lock follow-ups**

Check `docs/relay_macos.md` still lists these future items:

```text
Darwin 配置复用 Linux 字段
TqRelayStartQuicReceiveSink Darwin support decision
QUIC->TCP pending receive lookup complexity
DeferredReceiveCompleteBatchBytes Darwin behavior
Darwin metrics naming
macOS CI / TSAN coverage
```

Do not implement these in this plan.

- [ ] **Step 3: Run final test command**

Run:

```bash
rtk cmake --build build --target tcpquic_darwin_relay_worker_io_test tcpquic_darwin_relay_worker_queue_test tcpquic_darwin_relay_metrics_test tcpquic_darwin_reactor_test -j$(sysctl -n hw.ncpu)
rtk build/src/tcpquic_darwin_relay_worker_io_test
rtk build/src/tcpquic_darwin_relay_worker_queue_test
rtk build/src/tcpquic_darwin_relay_metrics_test
rtk build/src/tcpquic_darwin_reactor_test
```

Expected: PASS.

- [ ] **Step 4: Linux sanity build for cross-platform header changes**

Run this step only when the implementation changes cross-platform headers or non-Darwin relay shared code. If the implementation only changes Darwin `.cpp` files and Darwin-only tests, record this step as not applicable in the handoff summary.

```bash
rtk cmake --build build --target tcpquic_relay_buffer_test tcpquic_relay_alloc_test -j$(nproc)
rtk build/src/tcpquic_relay_buffer_test
rtk build/src/tcpquic_relay_alloc_test
```

Expected: PASS.

- [ ] **Step 5: Commit final docs adjustments**

If Step 2 required docs changes after Task 5 commit, create a docs-only commit:

```bash
rtk git add docs/relay_macos.md
rtk git commit -m "docs: update darwin relay lock cleanup status"
```

---

## Self-Review Checklist

- [ ] Every task maps to `docs/superpowers/specs/2026-07-04-darwin-relay-state-lock-design.md`.
- [ ] The plan is written against the current code after receive callback hardening, not against pre-hardening APIs.
- [ ] The plan does not re-add old `bool QueueDeferredQuicReceive()` semantics.
- [ ] Callback receive ownership remains explicit for `Ok`, `EventQueueFull`, `CallbackBudgetRejected`, and `AllocationFailed`.
- [ ] The plan clears callback direct writes to worker-only relay fields.
- [ ] The plan preserves lifecycle/fallback locks where they are still needed.
- [ ] The plan does not implement P0/P2/P3 unrelated items from `docs/relay_macos.md`.
- [ ] Each task has exact files, commands, and expected results.
