# Windows Relay Maintenance Queue Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 将 Windows relay 的常规维护路径从每个 IOCP event 全量扫描改为事件驱动 maintenance queue，并把 receive view 完成路径的正常删除改为 O(1) 队首 pop，同时补齐相关指标。

**Architecture:** `TqWindowsRelayWorker` 增加 worker-thread-owned maintenance queue，只在 worker 线程入队/出队；跨线程 callback 继续通过 IOCP operation 回到 worker 后调度维护。`FinishReceiveView()` 正常路径校验队首并 `pop_front()`，异常路径保留线性查找和 trace。所有新增观测值通过 worker snapshot、relay metrics JSON 和 trace summary 输出。

**Tech Stack:** C++17, Windows IOCP, MsQuic stream callback, CMake, `tcpquic_windows_relay_worker_test`, Superpowers subagent-driven development。

---

## File Structure

- Modify: `src/tunnel/windows_relay_worker.h`
  - Add maintenance and receive-finish metrics to `TqWindowsRelayWorkerSnapshot`.
  - Add maintenance queue declarations and helper methods.
  - Add worker-owned queue fields.
- Modify: `src/tunnel/windows_relay_worker.cpp`
  - Implement maintenance queue scheduling/draining.
  - Replace `FinishReceiveView()` normal erase path with queue-front pop.
  - Update snapshot and total aggregation.
- Modify: `src/runtime/relay_metrics.h`
  - Add public relay metrics fields for new Windows counters.
- Modify: `src/runtime/relay_metrics.cpp`
  - Map worker snapshot fields into metrics snapshot.
  - Emit JSON fields.
- Modify: `src/runtime/trace.cpp`
  - Add new Windows counters to relay metrics trace summary.
- Modify: `src/unittest/windows_relay_worker_test.cpp`
  - Add unit coverage for queue-front finish path, non-front diagnostic path, maintenance queue budget, and metrics.
- Modify: `docs/relay_win.md`
  - Update 3.2, 3.3, 3.8 and priority table after implementation.

## Task 1: Add Observable Metrics With Failing Tests

**Files:**
- Modify: `src/tunnel/windows_relay_worker.h`
- Modify: `src/tunnel/windows_relay_worker.cpp`
- Modify: `src/runtime/relay_metrics.h`
- Modify: `src/runtime/relay_metrics.cpp`
- Modify: `src/runtime/trace.cpp`
- Modify: `src/unittest/windows_relay_worker_test.cpp`

- [ ] **Step 1: Add failing worker snapshot assertions**

In `src/unittest/windows_relay_worker_test.cpp`, extend `TestWindowsRelayLockAndCallbackMetricsInitialState()` so new counters are expected to start at zero:

```cpp
return snapshot.WorkerLockAcquireCount == 0 &&
       snapshot.WorkerLockWaitNanos == 0 &&
       snapshot.FindRelayByIdCount == 0 &&
       snapshot.CallbackDispatchNanos == 0 &&
       snapshot.SnapshotBuildNanos == 0 &&
       snapshot.SnapshotActiveRelaysScanned == 0 &&
       snapshot.MaintenanceDrainCount == 0 &&
       snapshot.MaintenanceDrainNanos == 0 &&
       snapshot.MaintenanceRelaysProcessed == 0 &&
       snapshot.MaintenanceFullScanCount == 0 &&
       snapshot.MaintenanceFullScanRelaysScanned == 0 &&
       snapshot.ReceiveViewFinishLinearSearchCount == 0 &&
       snapshot.ReceiveViewFinishLinearSearchNanos == 0 &&
       snapshot.ReceiveViewFinishNotFrontCount == 0;
```

- [ ] **Step 2: Run the Windows worker test to confirm it fails to compile**

Run on Windows:

```bash
rtk cmake --build build --target tcpquic_windows_relay_worker_test -j
```

Expected: compile fails because `TqWindowsRelayWorkerSnapshot` does not have the new fields.

- [ ] **Step 3: Add snapshot fields**

In `src/tunnel/windows_relay_worker.h`, after `SnapshotActiveRelaysScanned` add:

```cpp
    uint64_t MaintenanceDrainCount{0};
    uint64_t MaintenanceDrainNanos{0};
    uint64_t MaintenanceRelaysProcessed{0};
    uint64_t MaintenanceFullScanCount{0};
    uint64_t MaintenanceFullScanRelaysScanned{0};
    uint64_t ReceiveViewFinishLinearSearchCount{0};
    uint64_t ReceiveViewFinishLinearSearchNanos{0};
    uint64_t ReceiveViewFinishNotFrontCount{0};
```

In the private counter block of `TqWindowsRelayWorker`, after `SnapshotActiveRelaysScanned_` add:

```cpp
    std::atomic<uint64_t> MaintenanceDrainCount_{0};
    std::atomic<uint64_t> MaintenanceDrainNanos_{0};
    std::atomic<uint64_t> MaintenanceRelaysProcessed_{0};
    std::atomic<uint64_t> MaintenanceFullScanCount_{0};
    std::atomic<uint64_t> MaintenanceFullScanRelaysScanned_{0};
    std::atomic<uint64_t> ReceiveViewFinishLinearSearchCount_{0};
    std::atomic<uint64_t> ReceiveViewFinishLinearSearchNanos_{0};
    std::atomic<uint64_t> ReceiveViewFinishNotFrontCount_{0};
```

- [ ] **Step 4: Populate snapshot fields**

In `TqWindowsRelayWorker::Snapshot()`, after `SnapshotActiveRelaysScanned` assignment, add:

```cpp
    snapshot.MaintenanceDrainCount =
        MaintenanceDrainCount_.load(std::memory_order_relaxed);
    snapshot.MaintenanceDrainNanos =
        MaintenanceDrainNanos_.load(std::memory_order_relaxed);
    snapshot.MaintenanceRelaysProcessed =
        MaintenanceRelaysProcessed_.load(std::memory_order_relaxed);
    snapshot.MaintenanceFullScanCount =
        MaintenanceFullScanCount_.load(std::memory_order_relaxed);
    snapshot.MaintenanceFullScanRelaysScanned =
        MaintenanceFullScanRelaysScanned_.load(std::memory_order_relaxed);
    snapshot.ReceiveViewFinishLinearSearchCount =
        ReceiveViewFinishLinearSearchCount_.load(std::memory_order_relaxed);
    snapshot.ReceiveViewFinishLinearSearchNanos =
        ReceiveViewFinishLinearSearchNanos_.load(std::memory_order_relaxed);
    snapshot.ReceiveViewFinishNotFrontCount =
        ReceiveViewFinishNotFrontCount_.load(std::memory_order_relaxed);
```

In `TqWindowsRelayRuntime::Snapshot()` total aggregation, after `SnapshotActiveRelaysScanned` add:

```cpp
        total.MaintenanceDrainCount += snapshot.MaintenanceDrainCount;
        total.MaintenanceDrainNanos += snapshot.MaintenanceDrainNanos;
        total.MaintenanceRelaysProcessed += snapshot.MaintenanceRelaysProcessed;
        total.MaintenanceFullScanCount += snapshot.MaintenanceFullScanCount;
        total.MaintenanceFullScanRelaysScanned += snapshot.MaintenanceFullScanRelaysScanned;
        total.ReceiveViewFinishLinearSearchCount += snapshot.ReceiveViewFinishLinearSearchCount;
        total.ReceiveViewFinishLinearSearchNanos += snapshot.ReceiveViewFinishLinearSearchNanos;
        total.ReceiveViewFinishNotFrontCount += snapshot.ReceiveViewFinishNotFrontCount;
```

- [ ] **Step 5: Expose metrics snapshot fields**

In `src/runtime/relay_metrics.h`, after `WindowsRelaySnapshotActiveRelaysScanned`, add:

```cpp
    uint64_t WindowsRelayMaintenanceDrainCount{0};
    uint64_t WindowsRelayMaintenanceDrainNanos{0};
    uint64_t WindowsRelayMaintenanceRelaysProcessed{0};
    uint64_t WindowsRelayMaintenanceFullScanCount{0};
    uint64_t WindowsRelayMaintenanceFullScanRelaysScanned{0};
    uint64_t WindowsRelayReceiveViewFinishLinearSearchCount{0};
    uint64_t WindowsRelayReceiveViewFinishLinearSearchNanos{0};
    uint64_t WindowsRelayReceiveViewFinishNotFrontCount{0};
```

In `src/runtime/relay_metrics.cpp`, in the Windows snapshot mapping block after `WindowsRelaySnapshotActiveRelaysScanned`, add:

```cpp
    metrics.WindowsRelayMaintenanceDrainCount = snapshot.MaintenanceDrainCount;
    metrics.WindowsRelayMaintenanceDrainNanos = snapshot.MaintenanceDrainNanos;
    metrics.WindowsRelayMaintenanceRelaysProcessed = snapshot.MaintenanceRelaysProcessed;
    metrics.WindowsRelayMaintenanceFullScanCount = snapshot.MaintenanceFullScanCount;
    metrics.WindowsRelayMaintenanceFullScanRelaysScanned =
        snapshot.MaintenanceFullScanRelaysScanned;
    metrics.WindowsRelayReceiveViewFinishLinearSearchCount =
        snapshot.ReceiveViewFinishLinearSearchCount;
    metrics.WindowsRelayReceiveViewFinishLinearSearchNanos =
        snapshot.ReceiveViewFinishLinearSearchNanos;
    metrics.WindowsRelayReceiveViewFinishNotFrontCount =
        snapshot.ReceiveViewFinishNotFrontCount;
```

- [ ] **Step 6: Emit metrics JSON fields**

In `TqAppendRelayMetricsJson()` after `windows_relay_snapshot_active_relays_scanned`, add:

```cpp
    out << ",\"windows_relay_maintenance_drain_count\":"
        << metrics.WindowsRelayMaintenanceDrainCount;
    out << ",\"windows_relay_maintenance_drain_nanos\":"
        << metrics.WindowsRelayMaintenanceDrainNanos;
    out << ",\"windows_relay_maintenance_relays_processed\":"
        << metrics.WindowsRelayMaintenanceRelaysProcessed;
    out << ",\"windows_relay_maintenance_full_scan_count\":"
        << metrics.WindowsRelayMaintenanceFullScanCount;
    out << ",\"windows_relay_maintenance_full_scan_relays_scanned\":"
        << metrics.WindowsRelayMaintenanceFullScanRelaysScanned;
    out << ",\"windows_relay_receive_view_finish_linear_search_count\":"
        << metrics.WindowsRelayReceiveViewFinishLinearSearchCount;
    out << ",\"windows_relay_receive_view_finish_linear_search_nanos\":"
        << metrics.WindowsRelayReceiveViewFinishLinearSearchNanos;
    out << ",\"windows_relay_receive_view_finish_not_front_count\":"
        << metrics.WindowsRelayReceiveViewFinishNotFrontCount;
```

- [ ] **Step 7: Emit trace summary fields**

In `src/runtime/trace.cpp`, extend `TqFormatRelayMetricsSnapshotLine()` with the same counters. Append compact key/value fields to the existing relay metrics trace string:

```cpp
        " win_maint_drains=%llu win_maint_nanos=%llu win_maint_relays=%llu"
        " win_maint_full_scans=%llu win_maint_full_scan_relays=%llu"
        " win_recv_finish_linear=%llu win_recv_finish_linear_nanos=%llu"
        " win_recv_finish_not_front=%llu",
```

Pass the corresponding `metrics.WindowsRelay...` values in the same order.

- [ ] **Step 8: Run tests**

Run on Windows:

```bash
rtk cmake --build build --target tcpquic_windows_relay_worker_test -j
rtk build/bin/Release/tcpquic_windows_relay_worker_test
```

Expected: test executable exits 0.

- [ ] **Step 9: Commit**

```bash
rtk git add src/tunnel/windows_relay_worker.h src/tunnel/windows_relay_worker.cpp src/runtime/relay_metrics.h src/runtime/relay_metrics.cpp src/runtime/trace.cpp src/unittest/windows_relay_worker_test.cpp
rtk git commit -m "test: expose windows relay maintenance metrics"
```

## Task 2: Make Receive View Finish O(1) On The Normal Path

**Files:**
- Modify: `src/tunnel/windows_relay_worker.h`
- Modify: `src/tunnel/windows_relay_worker.cpp`
- Modify: `src/unittest/windows_relay_worker_test.cpp`

- [ ] **Step 1: Add test helper declarations**

In `src/tunnel/windows_relay_worker.h`, under the existing `#if defined(TQ_UNIT_TESTING)` public helper block, add:

```cpp
    bool TestEnqueueReceiveViewForTest(uint64_t relayId, uint64_t byteCount);
    bool TestCompleteSecondReceiveViewForTest(uint64_t relayId, uint64_t completedLength);
```

- [ ] **Step 2: Implement test helpers**

In `src/tunnel/windows_relay_worker.cpp`, under the existing `#if defined(TQ_UNIT_TESTING)` helper implementations, add:

```cpp
bool TqWindowsRelayWorker::TestEnqueueReceiveViewForTest(uint64_t relayId, uint64_t byteCount) {
    auto relay = FindRelayByIdLocal(relayId);
    if (!relay || byteCount == 0) {
        return false;
    }
    auto view = std::make_shared<TqWindowsPendingQuicReceive>();
    view->OwnedBuffer.resize(static_cast<size_t>(byteCount), 'x');
    view->TotalLength = byteCount;
    view->AccountedLength = byteCount;
    view->SliceOffset = 0;
    view->CompletedLength = 0;
    view->PendingCompleteBytes = 0;
    view->Drained = false;
    view->Fin = false;
    view->Stream = relay->Stream;
    relay->PendingQuicReceiveBytes.fetch_add(byteCount, std::memory_order_acq_rel);
    relay->PendingQuicReceiveQueueDepth.fetch_add(1, std::memory_order_acq_rel);
    relay->PendingReceives.push_back(view);
    return true;
}

bool TqWindowsRelayWorker::TestCompleteSecondReceiveViewForTest(
    uint64_t relayId,
    uint64_t completedLength) {
    auto relay = FindRelayByIdLocal(relayId);
    if (!relay) {
        return false;
    }
    if (relay->PendingReceives.size() < 2) {
        return false;
    }
    auto it = relay->PendingReceives.begin();
    ++it;
    auto view = *it;
    if (!view || completedLength == 0 || completedLength > view->TotalLength) {
        return false;
    }
    view->CompletedLength = completedLength;
    view->AccountedLength = completedLength;
    SaturatingFetchSub(relay->PendingQuicReceiveBytes, completedLength);
    return CompletePendingQuicReceive(relay, view);
}
```

- [ ] **Step 3: Add a failing queue-front test**

In `src/unittest/windows_relay_worker_test.cpp`, add a test function near existing receive-view tests:

```cpp
bool TestWindowsRelayFinishReceiveViewFrontPathAvoidsLinearSearch() {
    TqWindowsRelayWorker worker;
    if (!StartRelayWorkerForTest(worker)) {
        worker.Stop();
        return false;
    }
    alignas(MsQuicStream) unsigned char streamStorage[sizeof(MsQuicStream)]{};
    auto* stream = reinterpret_cast<MsQuicStream*>(streamStorage);
    stream->Callback = MsQuicStream::NoOpCallback;
    stream->Context = nullptr;
    TqRelayHandle handle{};
    TqTuningConfig tuning{};
    tuning.RelayIoSize = 4096;
    if (!worker.RegisterRelayForTest(stream, &handle, tuning, TqCompressAlgo::None)) {
        worker.Stop();
        return false;
    }
    if (!worker.TestEnqueueReceiveViewForTest(handle.WindowsRelayId, 128)) {
        worker.Stop();
        return false;
    }
    const auto before = worker.Snapshot();
    if (!worker.TestCompleteReceiveViewForCleanup(handle.WindowsRelayId, 128)) {
        worker.Stop();
        return false;
    }
    const auto after = worker.Snapshot();
    worker.Stop();
    return after.ReceiveViewFinishLinearSearchCount == before.ReceiveViewFinishLinearSearchCount &&
           after.ReceiveViewFinishNotFrontCount == before.ReceiveViewFinishNotFrontCount;
}
```

- [ ] **Step 4: Add the test to `main()`**

In `main()`, add:

```cpp
    if (!TestWindowsRelayFinishReceiveViewFrontPathAvoidsLinearSearch()) {
        return 124;
    }
```

- [ ] **Step 5: Run and confirm the test fails**

Run on Windows:

```bash
rtk cmake --build build --target tcpquic_windows_relay_worker_test -j
rtk build/bin/Release/tcpquic_windows_relay_worker_test
```

Expected: fails because current `FinishReceiveView()` always uses linear search once counters are wired.

- [ ] **Step 6: Update `FinishReceiveView()` removal logic**

Replace the `std::find()` block in `TqWindowsRelayWorker::FinishReceiveView()` with:

```cpp
    bool removed = false;
    if (!relay->PendingReceives.empty() && relay->PendingReceives.front() == view) {
        relay->PendingReceives.pop_front();
        removed = true;
        TraceReceiveViewEvent(relay, view, "finish_pop_front");
    } else {
        const uint64_t searchStartNanos = NowSteadyNanos();
        const auto it = std::find(relay->PendingReceives.begin(), relay->PendingReceives.end(), view);
        ReceiveViewFinishLinearSearchNanos_.fetch_add(
            NowSteadyNanos() - searchStartNanos,
            std::memory_order_relaxed);
        ReceiveViewFinishLinearSearchCount_.fetch_add(1, std::memory_order_relaxed);
        if (it != relay->PendingReceives.end()) {
            ReceiveViewFinishNotFrontCount_.fetch_add(1, std::memory_order_relaxed);
            relay->PendingReceives.erase(it);
            removed = true;
            TraceReceiveViewEvent(relay, view, "finish_remove_not_front");
        }
    }
    if (!removed) {
        if (view->Drained || view->CompletedLength >= view->TotalLength) {
            TraceReceiveViewEvent(relay, view, "finish_already_drained");
            TqTraceRelayStopCondition(
                "windows",
                WorkerIndex_,
                "finish_receive_view_already_drained",
                BuildRelayTraceState(relay));
            return true;
        }
        TraceReceiveViewEvent(relay, view, "finish_missing");
        TqTraceRelayStopCondition(
            "windows",
            WorkerIndex_,
            "finish_receive_view_missing",
            BuildRelayTraceState(relay));
        return false;
    }
```

Keep the existing accounting code after removal unchanged.

- [ ] **Step 7: Add a non-front diagnostic test**

Add a unit-test-only helper that enqueues two synthetic receive views and completes the second first:

```cpp
bool TestWindowsRelayFinishReceiveViewNotFrontIsCounted() {
    TqWindowsRelayWorker worker;
    if (!StartRelayWorkerForTest(worker)) {
        worker.Stop();
        return false;
    }
    alignas(MsQuicStream) unsigned char streamStorage[sizeof(MsQuicStream)]{};
    auto* stream = reinterpret_cast<MsQuicStream*>(streamStorage);
    stream->Callback = MsQuicStream::NoOpCallback;
    stream->Context = nullptr;
    TqRelayHandle handle{};
    TqTuningConfig tuning{};
    tuning.RelayIoSize = 4096;
    if (!worker.RegisterRelayForTest(stream, &handle, tuning, TqCompressAlgo::None)) {
        worker.Stop();
        return false;
    }
    if (!worker.TestEnqueueReceiveViewForTest(handle.WindowsRelayId, 64) ||
        !worker.TestEnqueueReceiveViewForTest(handle.WindowsRelayId, 96)) {
        worker.Stop();
        return false;
    }
    const auto before = worker.Snapshot();
    if (!worker.TestCompleteSecondReceiveViewForTest(handle.WindowsRelayId, 96)) {
        worker.Stop();
        return false;
    }
    const auto after = worker.Snapshot();
    worker.Stop();
    return after.ReceiveViewFinishLinearSearchCount == before.ReceiveViewFinishLinearSearchCount + 1 &&
           after.ReceiveViewFinishNotFrontCount == before.ReceiveViewFinishNotFrontCount + 1;
}
```

- [ ] **Step 8: Add the non-front test to `main()`**

In `main()`, add:

```cpp
    if (!TestWindowsRelayFinishReceiveViewNotFrontIsCounted()) {
        return 125;
    }
```

- [ ] **Step 9: Run tests**

Run on Windows:

```bash
rtk cmake --build build --target tcpquic_windows_relay_worker_test -j
rtk build/bin/Release/tcpquic_windows_relay_worker_test
```

Expected: test executable exits 0.

- [ ] **Step 10: Commit**

```bash
rtk git add src/tunnel/windows_relay_worker.cpp src/tunnel/windows_relay_worker.h src/unittest/windows_relay_worker_test.cpp
rtk git commit -m "fix: make windows receive finish use queue front"
```

## Task 3: Add Event-Driven Maintenance Queue

**Files:**
- Modify: `src/tunnel/windows_relay_worker.h`
- Modify: `src/tunnel/windows_relay_worker.cpp`
- Modify: `src/unittest/windows_relay_worker_test.cpp`

- [ ] **Step 1: Add failing maintenance budget test**

In `src/unittest/windows_relay_worker_test.cpp`, add:

```cpp
bool TestWindowsRelayMaintenanceQueueBudgetForTest() {
    TqWindowsRelayWorker worker;
    worker.SetMaintenanceBudgetForTest(1);
    if (!StartRelayWorkerForTest(worker)) {
        worker.Stop();
        return false;
    }
    alignas(MsQuicStream) unsigned char streamStorageA[sizeof(MsQuicStream)]{};
    alignas(MsQuicStream) unsigned char streamStorageB[sizeof(MsQuicStream)]{};
    auto* streamA = reinterpret_cast<MsQuicStream*>(streamStorageA);
    auto* streamB = reinterpret_cast<MsQuicStream*>(streamStorageB);
    streamA->Callback = MsQuicStream::NoOpCallback;
    streamB->Callback = MsQuicStream::NoOpCallback;
    TqRelayHandle handleA{};
    TqRelayHandle handleB{};
    TqTuningConfig tuning{};
    tuning.RelayIoSize = 4096;
    if (!worker.RegisterRelayForTest(streamA, &handleA, tuning, TqCompressAlgo::None) ||
        !worker.RegisterRelayForTest(streamB, &handleB, tuning, TqCompressAlgo::None)) {
        worker.Stop();
        return false;
    }
    worker.TestScheduleMaintenanceForTest(handleA.WindowsRelayId);
    worker.TestScheduleMaintenanceForTest(handleB.WindowsRelayId);
    const auto before = worker.Snapshot();
    worker.TestDrainMaintenanceForTest();
    const auto afterOne = worker.Snapshot();
    worker.TestDrainMaintenanceForTest();
    const auto afterTwo = worker.Snapshot();
    worker.Stop();
    return afterOne.MaintenanceRelaysProcessed == before.MaintenanceRelaysProcessed + 1 &&
           afterTwo.MaintenanceRelaysProcessed == before.MaintenanceRelaysProcessed + 2;
}
```

- [ ] **Step 2: Add unit-test helper declarations**

In `src/tunnel/windows_relay_worker.h`, under `#if defined(TQ_UNIT_TESTING)` public helpers, add:

```cpp
    void SetMaintenanceBudgetForTest(size_t budget);
    bool TestScheduleMaintenanceForTest(uint64_t relayId);
    void TestDrainMaintenanceForTest();
```

- [ ] **Step 3: Run and confirm compile failure**

Run on Windows:

```bash
rtk cmake --build build --target tcpquic_windows_relay_worker_test -j
```

Expected: compile fails because maintenance helpers and queue fields do not exist.

- [ ] **Step 4: Add queue fields and helper declarations**

In `src/tunnel/windows_relay_worker.h`, include the required containers if missing:

```cpp
#include <deque>
#include <unordered_set>
```

In `TqWindowsRelayWorker` private methods, add:

```cpp
    void ScheduleRelayMaintenance(const std::shared_ptr<RelayContext>& relay);
    bool RelayNeedsMaintenance(const std::shared_ptr<RelayContext>& relay) const;
    void EnqueueFullMaintenanceScan();
```

In private fields, add:

```cpp
    std::deque<std::shared_ptr<RelayContext>> MaintenanceQueue_;
    std::unordered_set<uint64_t> MaintenanceQueuedRelayIds_;
    size_t MaintenanceBudget_{64};
    uint32_t MaintenanceFullScanCountdown_{0};
```

- [ ] **Step 5: Implement queue scheduling**

In `src/tunnel/windows_relay_worker.cpp`, add:

```cpp
void TqWindowsRelayWorker::ScheduleRelayMaintenance(const std::shared_ptr<RelayContext>& relay) {
    if (!relay || relay->StopPublished.load(std::memory_order_acquire)) {
        return;
    }
    if (!MaintenanceQueuedRelayIds_.insert(relay->Id).second) {
        return;
    }
    MaintenanceQueue_.push_back(relay);
}

bool TqWindowsRelayWorker::RelayNeedsMaintenance(const std::shared_ptr<RelayContext>& relay) const {
    if (!relay || relay->StopPublished.load(std::memory_order_acquire)) {
        return false;
    }
    return relay->Closing.load(std::memory_order_acquire) ||
           relay->CloseAfterDrained.load(std::memory_order_acquire) ||
           relay->TcpReadPausedByQuicBacklog.load(std::memory_order_acquire) ||
           !relay->PendingQuicSendRetries.empty() ||
           relay->PendingQuicReceiveQueueDepth.load(std::memory_order_acquire) != 0;
}

void TqWindowsRelayWorker::EnqueueFullMaintenanceScan() {
    std::vector<std::shared_ptr<RelayContext>> relays;
    {
        std::lock_guard<std::mutex> guard(Lock_);
        relays.reserve(Relays_.size());
        for (const auto& entry : Relays_) {
            relays.push_back(entry.second);
        }
    }
    MaintenanceFullScanCount_.fetch_add(1, std::memory_order_relaxed);
    MaintenanceFullScanRelaysScanned_.fetch_add(relays.size(), std::memory_order_relaxed);
    for (const auto& relay : relays) {
        if (RelayNeedsMaintenance(relay)) {
            ScheduleRelayMaintenance(relay);
        }
    }
}
```

- [ ] **Step 6: Rewrite `DrainPerRelayMaintenance()`**

Replace `DrainPerRelayMaintenance()` with:

```cpp
void TqWindowsRelayWorker::DrainPerRelayMaintenance() {
    const uint64_t startNanos = NowSteadyNanos();
    MaintenanceDrainCount_.fetch_add(1, std::memory_order_relaxed);
    size_t processed = 0;
    while (processed < MaintenanceBudget_ && !MaintenanceQueue_.empty()) {
        auto relay = MaintenanceQueue_.front();
        MaintenanceQueue_.pop_front();
        if (relay) {
            MaintenanceQueuedRelayIds_.erase(relay->Id);
        }
        if (!relay || relay->StopPublished.load(std::memory_order_acquire)) {
            continue;
        }
        ++processed;
        RetryPendingQuicSends(relay);
        MaybePostTcpRecv(relay);
        (void)CloseRelayIfDrained(relay);
        TryRetireRelay(relay);
        if (RelayNeedsMaintenance(relay)) {
            ScheduleRelayMaintenance(relay);
        }
    }
    MaintenanceRelaysProcessed_.fetch_add(processed, std::memory_order_relaxed);
    MaintenanceDrainNanos_.fetch_add(NowSteadyNanos() - startNanos, std::memory_order_relaxed);
}
```

- [ ] **Step 7: Add full-scan fallback in `Run()`**

In `Run()`, replace the unconditional pre-wait `DrainPerRelayMaintenance();` with:

```cpp
        if (++MaintenanceFullScanCountdown_ >= 256) {
            MaintenanceFullScanCountdown_ = 0;
            EnqueueFullMaintenanceScan();
        }
        DrainPerRelayMaintenance();
```

Keep the post-null-overlapped drain, but it now drains the queue rather than scanning all relay.

- [ ] **Step 8: Add scheduling trigger points**

Add `ScheduleRelayMaintenance(relay);` after state changes that can require maintenance:

```cpp
// RegisterRelayLocal / RegisterRelayForTest after relay is stored.
ScheduleRelayMaintenance(relay);

// HandleQuicIdealSendBuffer after ideal send buffer update.
ScheduleRelayMaintenance(relay);

// ProcessQuicSendCompleteOperation after accounting.
ScheduleRelayMaintenance(relay);

// FinishReceiveView before return.
ScheduleRelayMaintenance(relay);

// CloseRelay after close state is set and before TryRetireRelay.
ScheduleRelayMaintenance(relay);

// HandleTcpRecv / HandleTcpSend after completion accounting.
ScheduleRelayMaintenance(relay);
```

Do not call this from MsQuic callback before the callback operation has been posted to worker.

- [ ] **Step 9: Implement unit-test helpers**

Under `#if defined(TQ_UNIT_TESTING)` in `windows_relay_worker.cpp`, add:

```cpp
void TqWindowsRelayWorker::SetMaintenanceBudgetForTest(size_t budget) {
    MaintenanceBudget_ = budget == 0 ? 1 : budget;
}

bool TqWindowsRelayWorker::TestScheduleMaintenanceForTest(uint64_t relayId) {
    auto relay = FindRelayByIdLocal(relayId);
    if (!relay) {
        return false;
    }
    ScheduleRelayMaintenance(relay);
    return true;
}

void TqWindowsRelayWorker::TestDrainMaintenanceForTest() {
    DrainPerRelayMaintenance();
}
```

- [ ] **Step 10: Run tests**

Run on Windows:

```bash
rtk cmake --build build --target tcpquic_windows_relay_worker_test -j
rtk build/bin/Release/tcpquic_windows_relay_worker_test
```

Expected: test executable exits 0.

- [ ] **Step 11: Commit**

```bash
rtk git add src/tunnel/windows_relay_worker.h src/tunnel/windows_relay_worker.cpp src/unittest/windows_relay_worker_test.cpp
rtk git commit -m "perf: queue windows relay maintenance work"
```

## Task 4: Update Documentation And Run Verification

**Files:**
- Modify: `docs/relay_win.md`

- [ ] **Step 1: Update 3.2**

Replace the recommendation text under 3.2 with current status:

```markdown
当前状态：

- 常规维护已改为 worker-owned maintenance queue，只有状态变化后需要 retry、resume、close 或 retire 的 relay 会入队。
- 每轮 maintenance drain 有预算，避免单个 IOCP event 后处理完整 worker relay map。
- 仍保留低频 full-scan fallback，只把需要维护的 relay 入队，用于兜底异常状态。
```

- [ ] **Step 2: Update 3.3**

Replace the recommendation text under 3.3 with:

```markdown
当前状态：

- 正常完成路径要求 `PendingReceives.front() == view`，然后 O(1) `pop_front()`。
- 非队首完成或 missing view 保留线性查找/诊断路径，并通过 `ReceiveViewFinishLinearSearchCount`、`ReceiveViewFinishNotFrontCount`、`ReceiveViewFinishLinearSearchNanos` 观测。
- Windows QUIC -> TCP 仍保持每条 relay 单队首 drain，不支持乱序 TCP 写。
```

- [ ] **Step 3: Update 3.8**

Remove maintenance and linear-search metrics from the missing list. Keep callback copy metrics as remaining gap:

```markdown
当前剩余缺口：

- callback 复制字节/耗时还没有直接指标。
- receive callback 复制前预算控制仍未实现，相关观测可在后续 3.4 方案中补齐。
```

- [ ] **Step 4: Update priority table**

Change these rows:

```markdown
| 已完成 | 优化 `DrainPerRelayMaintenance()` 全量扫描 | 常规路径已改为事件驱动 maintenance queue，并保留低频兜底扫描。 |
| 已完成 | `FinishReceiveView()` 队首 pop 优先，异常才线性查找 | 正常路径 O(1)，异常路径有计数和 trace。 |
| 部分完成 | 补齐 maintenance/copy/linear-search 指标 | maintenance 和 linear-search 指标已补齐，callback copy 指标留待 receive callback 预算方案处理。 |
```

- [ ] **Step 5: Run local Linux verification**

Run:

```bash
rtk cmake --build build --target tcpquic-proxy -j$(nproc)
rtk cmake --build build --target tcpquic_platform_socket_test tcpquic_thread_pool_test -j$(nproc)
rtk proxy ./build/bin/Release/tcpquic_platform_socket_test
rtk proxy ./build/bin/Release/tcpquic_thread_pool_test
rtk git diff --check
```

Expected: all commands exit 0. Note that `tcpquic_windows_relay_worker_test` is Windows-only and must be run on Windows.

- [ ] **Step 6: Commit**

```bash
rtk git add docs/relay_win.md
rtk git commit -m "docs: update windows relay maintenance status"
```

## Execution Notes

- Execute this plan in a fresh worktree, for example `.worktrees/windows-relay-maintenance-queue`, because the main checkout may contain unrelated uncommitted docs/config changes.
- Prefer `superpowers:subagent-driven-development`: one implementer per task, spec review and code-quality review after each task.
- Do not merge until Windows-specific tests have been run on a Windows environment or the residual gap is explicitly accepted.
