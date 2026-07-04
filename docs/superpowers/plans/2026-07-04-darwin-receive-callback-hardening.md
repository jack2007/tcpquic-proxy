# Darwin Receive Callback Hardening Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 修正 Darwin QUIC receive callback 在入队失败时的 MsQuic ownership 语义，增加失败原因可观测性，并在 event queue 满时 pause receive + backpressure 暂存，而不是仅 `Errors++` 后返回 `QUIC_STATUS_SUCCESS`。

**Architecture:** `QueueDeferredQuicReceive()` 返回 typed enqueue result 并写入细分计数器；`StreamCallback()` RECEIVE 分支按结果决定 `ReceiveComplete()`、pause receive 和 QUIC status。queue 满路径在 `StreamBinding`（或 worker）上暂存 pending view，worker drain 后 retry 入队。fake FIN abort **不修改**（设计行为，见 `docs/relay_macos.md`）。

**Design:** `docs/superpowers/specs/2026-07-04-darwin-receive-callback-hardening-design.md`

**Tech Stack:** C++17, MsQuic stream callback, Darwin kqueue relay worker, CMake, `darwin_relay_worker_io_test` under `#if defined(__APPLE__)`.

---

## File Structure

- Modify: `src/tunnel/darwin_relay_event_queue.h`（如需暴露 pending receive 字段）
- Modify: `src/tunnel/darwin_relay_worker.h`
  - Add `TqDarwinQuicReceiveEnqueueResult` enum.
  - Add snapshot counters and `#if defined(TCPQUIC_TESTING)` accessors.
  - Add backpressure pending deque on `StreamBinding`（或 worker 私有结构 + 声明）。
- Modify: `src/tunnel/darwin_relay_worker.cpp`
  - Refactor `QueueDeferredQuicReceive()` to return enqueue result.
  - Update `StreamCallback()` RECEIVE handling.
  - Add `FlushCallbackPendingQuicReceives()`（worker 线程 retry 入队）。
  - Increment counters at failure sites; optional `FlushTcpWrites` optimization in Task 6.
- Modify: `src/unittest/darwin_relay_worker_io_test.cpp`
  - Characterization + regression tests for budget reject, queue full, successful PENDING path.
- Modify: `docs/relay_macos.md`
  - Mark P1 receive 语义 / P2 queue 背压为已完成或 in-progress 状态。

---

### Task 1: Add Receive Enqueue Observability

**Files:**
- Modify: `src/tunnel/darwin_relay_worker.h`
- Modify: `src/tunnel/darwin_relay_worker.cpp`
- Test: `src/unittest/darwin_relay_worker_io_test.cpp`

- [ ] **Step 1: Add enqueue result enum**

In `src/tunnel/darwin_relay_worker.h` (before `TqDarwinRelayWorker` class or near other Darwin types):

```cpp
enum class TqDarwinQuicReceiveEnqueueResult : uint8_t {
    Ok = 0,
    InvalidArgs,
    AllocationFailed,
    EmptyNonFin,
    NullBuffer,
    CallbackBudgetRejected,
    EventQueueFull,
};
```

- [ ] **Step 2: Add snapshot counters**

In `TqDarwinRelayWorkerSnapshot`:

```cpp
    uint64_t EventQueueFullErrors{0};
    uint64_t WakeFailures{0};
    uint64_t CallbackReceiveBudgetRejects{0};
    uint64_t QuicReceiveEnqueueFailures{0};
    uint64_t QuicReceiveViewBackpressureQueued{0};
```

On worker, add matching `std::atomic<uint64_t>` members and populate in `SnapshotLocal()`.

Under `#if defined(TCPQUIC_TESTING)` add accessors:

```cpp
    uint64_t EventQueueFullErrorsForTest() const;
    uint64_t CallbackReceiveBudgetRejectsForTest() const;
    uint64_t QuicReceiveEnqueueFailuresForTest() const;
    uint64_t QuicReceiveViewBackpressureQueuedForTest() const;
```

- [ ] **Step 3: Increment counters at current failure sites (pre-refactor)**

In `QueueDeferredQuicReceive()` and `EnqueueEvent()` / `Wake()`:

- `EventQueue.TryPush` failure -> `EventQueueFullErrors`
- `Wake()` failure -> `WakeFailures`（若当前未计数）
- `ReserveCallbackReceiveBudget` failure -> `CallbackReceiveBudgetRejects`
- any `return false` from `QueueDeferredQuicReceive` -> `QuicReceiveEnqueueFailures`

Keep existing `Errors++` for now so characterization matches current behavior.

- [ ] **Step 4: Add characterization test**

Add `ReceiveEnqueueFailureCurrentlyReturnsSuccess()`:

- Register relay with `StartForTest()`.
- Fill event queue (`EventQueueCapacity = 2` + two marker events) or exhaust callback budget.
- Invoke RECEIVE via `StreamCallback()`.
- Assert callback returns `QUIC_STATUS_SUCCESS` today and `QuicReceiveEnqueueFailuresForTest()` / `EventQueueFullErrorsForTest()` increased.

Register in `main()` near other receive tests.

- [ ] **Step 5: Run test on macOS**

```bash
cmake --build build --target tcpquic_darwin_relay_worker_io_test -j$(sysctl -n hw.ncpu)
./build/bin/Release/tcpquic_darwin_relay_worker_io_test
```

Expected: PASS (documents current behavior).

- [ ] **Step 6: Commit**

```bash
git add src/tunnel/darwin_relay_worker.h src/tunnel/darwin_relay_worker.cpp src/unittest/darwin_relay_worker_io_test.cpp
git commit -m "test: instrument darwin receive enqueue failures"
```

---

### Task 2: Return Typed Result from QueueDeferredQuicReceive

**Files:**
- Modify: `src/tunnel/darwin_relay_worker.h`
- Modify: `src/tunnel/darwin_relay_worker.cpp`

- [ ] **Step 1: Change signature**

Replace:

```cpp
bool QueueDeferredQuicReceive(...);
```

With:

```cpp
TqDarwinQuicReceiveEnqueueResult QueueDeferredQuicReceive(...);
```

Update declaration in header and all call sites (currently `StreamCallback()` only).

- [ ] **Step 2: Map each early return to a result**

| Current `return false` site | Result |
|----------------------------|--------|
| null binding/stream | `InvalidArgs` |
| bufferCount != 0 && buffers == nullptr | `InvalidArgs` |
| !fin && empty buffers | `InvalidArgs` |
| `new TqDarwinPendingQuicReceive` fails | `AllocationFailed` |
| null buffer in slice | `NullBuffer` |
| totalLength == 0 && !fin | `EmptyNonFin` |
| `ReserveCallbackReceiveBudget` fails | `CallbackBudgetRejected` |
| `EnqueueEvent` fails | `EventQueueFull` |
| success | `Ok` |

Remove generic `QuicReceiveEnqueueFailures++` per failure; increment only the specific counter (+ optional aggregate `QuicReceiveEnqueueFailures` for any non-Ok).

- [ ] **Step 3: Keep budget release on EventQueueFull**

On `EventQueueFull`, retain existing:

```cpp
ReleaseCallbackReceiveBudget(receive);
QuicReceiveViewCount/Bytes rollback;
```

- [ ] **Step 4: Build**

```bash
cmake --build build --target tcpquic_darwin_relay_worker_io_test -j$(sysctl -n hw.ncpu)
```

Fix compile errors in tests if any helper called `QueueDeferredQuicReceive` directly.

- [ ] **Step 5: Commit**

```bash
git add src/tunnel/darwin_relay_worker.h src/tunnel/darwin_relay_worker.cpp
git commit -m "refactor: return typed darwin receive enqueue result"
```

---

### Task 3: Fix StreamCallback RECEIVE Failure Semantics

**Files:**
- Modify: `src/tunnel/darwin_relay_worker.cpp`
- Test: `src/unittest/darwin_relay_worker_io_test.cpp`

- [ ] **Step 1: Handle each enqueue result in StreamCallback**

Replace:

```cpp
        if (!worker->QueueDeferredQuicReceive(...)) {
            worker->Errors.fetch_add(1, std::memory_order_relaxed);
            return QUIC_STATUS_SUCCESS;
        }
        return QUIC_STATUS_PENDING;
```

With a `switch (result)` (pseudo):

- `Ok` -> `QUIC_STATUS_PENDING`
- `InvalidArgs` / `EmptyNonFin` / `NullBuffer` -> log counter, `QUIC_STATUS_SUCCESS`（无 buffer 接管）
- `AllocationFailed` -> 关闭 relay（`Closing = true` 或 `EnqueueRelayCloseFromCallback`）；若有 `TotalBufferLength > 0` 或 FIN，调用 `stream->ReceiveComplete(totalLength)`；`QUIC_STATUS_SUCCESS`
- `CallbackBudgetRejected` -> pause receive via `ReceiveSetEnabled(false)`；`ReceiveComplete(totalLength)` on current event；`QUIC_STATUS_SUCCESS`
- `EventQueueFull` -> defer to Task 4 backpressure helper；Task 3 可暂用 pause + `ReceiveComplete` 作为 intermediate fix if Task 4 not ready

Use `event->RECEIVE.TotalBufferLength` for `ReceiveComplete` bytes when applicable; FIN-only non-fake receive with zero length follows existing tests (`QuicReceiveCallbackRejectsOversizedReceiveAfterPendingFinOnlyEvent`).

- [ ] **Step 2: Add ReceiveComplete helper**

Add private helper to avoid duplication:

```cpp
    void CompleteMsQuicReceiveFromCallback(MsQuicStream* stream, uint64_t totalLength);
```

Respect `SetReceiveCompleteForTest()` hook in `TCPQUIC_TESTING`.

- [ ] **Step 3: Update characterization test**

Rename/update `ReceiveEnqueueFailureCurrentlyReturnsSuccess()` to assert new semantics:

- budget reject / allocation fail: `ReceiveComplete` called (via test hook counter).
- callback no longer silent SUCCESS without cleanup on paths that held buffer ownership risk.

- [ ] **Step 4: Add budget reject regression test**

Add `ReceiveCallbackBudgetRejectPausesAndCompletes()`:

- Tiny `MaxPendingQuicReceiveBytesPerRelay`.
- First receive succeeds (PENDING).
- Second receive triggers `CallbackBudgetRejected`.
- Assert `CallbackReceiveBudgetRejectsForTest() == 1`, receive paused (`SetReceiveSetEnabledForTest` records `false`), `ReceiveComplete` called for second event bytes.

- [ ] **Step 5: Run tests**

```bash
cmake --build build --target tcpquic_darwin_relay_worker_io_test -j$(sysctl -n hw.ncpu)
./build/bin/Release/tcpquic_darwin_relay_worker_io_test
```

Expected: PASS including existing `QuicReceiveCallback*` tests.

- [ ] **Step 6: Commit**

```bash
git add src/tunnel/darwin_relay_worker.cpp src/unittest/darwin_relay_worker_io_test.cpp
git commit -m "fix: clarify darwin receive callback failure semantics"
```

---

### Task 4: Event Queue Full Backpressure

**Files:**
- Modify: `src/tunnel/darwin_relay_worker.h`
- Modify: `src/tunnel/darwin_relay_worker.cpp`
- Test: `src/unittest/darwin_relay_worker_io_test.cpp`

- [ ] **Step 1: Add backpressure storage on StreamBinding**

In `StreamBinding` (anonymous namespace struct in `.cpp` or header if needed):

```cpp
    std::deque<std::shared_ptr<TqDarwinPendingQuicReceive>> CallbackPendingQuicReceives;
    mutable std::mutex CallbackPendingQuicReceivesMutex;
```

Alternatively worker-thread-only deque if all pushes happen from callback with mutex — follow Linux pattern in `linux_relay_worker.cpp` relay struct.

- [ ] **Step 2: On EventQueueFull, queue view + pause receive**

When `QueueDeferredQuicReceive` would return `EventQueueFull`:

- Do **not** release budget if view is stashed for retry (align with Linux: budget stays until worker processes).
- Push `receive` to `CallbackPendingQuicReceives`.
- Pause receive: `ReceiveSetEnabled(false)` on stream.
- Increment `QuicReceiveViewBackpressureQueued`.
- Return new result `EventQueueFull` to callback -> `QUIC_STATUS_PENDING`（view 已被 relay 逻辑接管，与 Linux 一致）。

Re-read Linux lines 2500–2508 for exact budget accounting; adapt to Darwin `ReleaseCallbackReceiveBudget` timing.

- [ ] **Step 3: Flush backpressure on worker thread**

Add:

```cpp
void TqDarwinRelayWorker::FlushCallbackPendingQuicReceives(StreamBinding* binding);
```

Call from worker loop after draining events or at start of `ProcessQuicReceiveViewEvent` batch — retry `EnqueueEvent` for stashed views; on success resume receive via `MaybeResumeQuicReceive`.

- [ ] **Step 4: Add queue-full regression test**

Add `ReceiveCallbackQueueFullBackpressureRetriesAfterDrain()`:

- `EventQueueCapacity = 2`, pre-fill queue.
- RECEIVE callback returns `PENDING`, `QuicReceiveViewBackpressureQueuedForTest() == 1`.
- Drain events -> stashed receive lands in `PendingQuicReceives`.
- Assert receive unpaused after drain.

- [ ] **Step 5: Run tests**

```bash
cmake --build build --target tcpquic_darwin_relay_worker_io_test -j$(sysctl -n hw.ncpu)
./build/bin/Release/tcpquic_darwin_relay_worker_io_test
```

- [ ] **Step 6: Commit**

```bash
git add src/tunnel/darwin_relay_worker.h src/tunnel/darwin_relay_worker.cpp src/unittest/darwin_relay_worker_io_test.cpp
git commit -m "fix: add darwin receive queue-full backpressure"
```

---

### Task 5: Snapshot Aggregation and Documentation

**Files:**
- Modify: `src/tunnel/darwin_relay_worker.cpp`（`SnapshotLocal` / runtime aggregate if needed）
- Modify: `src/unittest/darwin_relay_metrics_test.cpp`（optional: assert new snapshot fields via worker API）
- Modify: `docs/relay_macos.md`

- [ ] **Step 1: Verify SnapshotLocal populates new counters**

Ensure runtime `TqDarwinRelayRuntime::Snapshot()` aggregates worker counters.

- [ ] **Step 2: Update relay_macos.md**

- P1 receive 失败语义：标记为已完成（简述 enqueue result + ReceiveComplete 边界）。
- P2 event queue 背压：标记 backpressure 暂存 + 细分计数。
- §2 跟进计划：指向本 plan 及 commit 范围。
- fake FIN 小节保持不变。

- [ ] **Step 3: Run Darwin test suite**

```bash
cmake --build build --target tcpquic_darwin_relay_worker_io_test tcpquic_darwin_relay_worker_queue_test tcpquic_darwin_relay_metrics_test tcpquic_darwin_reactor_test -j$(sysctl -n hw.ncpu)
./build/bin/Release/tcpquic_darwin_relay_worker_io_test
./build/bin/Release/tcpquic_darwin_relay_worker_queue_test
./build/bin/Release/tcpquic_darwin_relay_metrics_test
./build/bin/Release/tcpquic_darwin_reactor_test
```

- [ ] **Step 4: Commit**

```bash
git add src/tunnel/darwin_relay_worker.cpp docs/relay_macos.md
git commit -m "docs: record darwin receive callback hardening"
```

---

### Task 6 (Optional): FlushTcpWrites PendingTcpWriteRefs

**Files:**
- Modify: `src/tunnel/darwin_relay_event_queue.h`（`TqDarwinPendingQuicReceive`）
- Modify: `src/tunnel/darwin_relay_worker.cpp`
- Test: `src/unittest/darwin_relay_worker_io_test.cpp`

- [ ] **Step 1: Add ref counter**

On `TqDarwinPendingQuicReceive`:

```cpp
    uint32_t PendingTcpWriteRefs{0};
```

Increment when creating `TqDarwinRelayPendingTcpWrite` in `EnqueueQuicReceiveForTcp()`; decrement when write completes in `FlushTcpWrites()`; when zero and bytes complete, call existing receive completion path without scanning entire `PendingTcpWrites`.

- [ ] **Step 2: Remove O(n) scan**

Replace loop at `FlushTcpWrites` lines ~2540–2548:

```cpp
                    for (const auto& pendingWrite : relay->PendingTcpWrites) {
                        if (pendingWrite != nullptr && pendingWrite->Receive == receive &&
```

With ref-count zero check.

- [ ] **Step 3: Partial write regression test**

Extend existing partial-write receive test to assert `DeferredReceiveCompletes` still correct with multiple pending writes per receive.

- [ ] **Step 4: Run tests and commit**

```bash
git commit -m "perf: use pending tcp write refs in darwin flush path"
```

---

## Self-Review Checklist

- [ ] fake FIN abort 路径未被修改。
- [ ] `QueueDeferredQuicReceive()` 每个失败原因有可观测计数。
- [ ] callback 不在 buffer ownership 模糊时 silent SUCCESS。
- [ ] queue 满路径 pause receive + backpressure，不是仅 `Errors++`。
- [ ] 现有 `QuicReceiveCallback*` / pause-resume / partial write 测试仍 PASS。
- [ ] `docs/relay_macos.md` 与 design spec 一致。
- [ ] 未触碰 send completion active path（`ActiveSendOperations` / operation state）。

## Out of Scope Reminder

| Item | Track |
|------|-------|
| fake FIN graceful shutdown | 设计行为，不处理 |
| RelayState / RelayMutex 出清 | `darwin-relay-state-lock` plan |
| 平台中性 tuning/metrics 命名 | 独立 cleanup |
| macOS receive sink | 产品决策 |
| CI macOS runner | P3 |
