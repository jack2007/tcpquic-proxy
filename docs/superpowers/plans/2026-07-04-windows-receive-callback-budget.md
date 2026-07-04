# Windows Receive Callback Budget Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 在 Windows relay 的 MsQuic receive callback 复制 buffers 前执行轻量 pending-byte 预算判断，并补齐 callback copy 字节/耗时指标。

**Architecture:** `StreamCallback(RECEIVE)` 先计算本次 receive 字节数，再用 `CallbackBinding::RelayHint` 读取 callback-safe relay 状态。明显超过 `WindowsRelayMaxPendingQuicReceiveBytesPerRelay` 时暂停后续 QUIC receive 并跳过复制；预算允许时继续走现有 `BuildDeferredQuicReceiveView()` 和 IOCP 投递。worker 侧 `EnqueueDeferredQuicReceiveView()` 保留现有预算检查作为并发兜底。

**Tech Stack:** C++17, Windows IOCP, MsQuic stream callback, CMake, `tcpquic_windows_relay_worker_test`, Superpowers subagent-driven development。

---

## File Structure

- Modify: `src/tunnel/windows_relay_worker.h`
  - Add worker snapshot fields for callback receive budget/copy metrics.
  - Add helper declarations for receive byte calculation and callback budget rejection.
  - Add atomic counters.
- Modify: `src/tunnel/windows_relay_worker.cpp`
  - Implement callback-safe budget helper.
  - Call budget helper before `BuildDeferredQuicReceiveView()`.
  - Record copy bytes/nanos around actual buffer copy.
  - Populate snapshot and runtime aggregate.
- Modify: `src/runtime/relay_metrics.h`
  - Add public metrics fields.
- Modify: `src/runtime/relay_metrics.cpp`
  - Map Windows snapshot fields and emit JSON fields.
- Modify: `src/runtime/trace.cpp`
  - Add compact trace summary fields.
- Modify: `src/unittest/windows_relay_worker_test.cpp`
  - Add tests for budget reject, budget allow, FIN bypass, and metric initial state.
- Modify: `docs/relay_win.md`
  - Update 3.4, 3.8, and priority table after implementation.

## Task 1: Add Metrics Fields And Initial-State Test

**Files:**
- Modify: `src/tunnel/windows_relay_worker.h`
- Modify: `src/tunnel/windows_relay_worker.cpp`
- Modify: `src/runtime/relay_metrics.h`
- Modify: `src/runtime/relay_metrics.cpp`
- Modify: `src/runtime/trace.cpp`
- Modify: `src/unittest/windows_relay_worker_test.cpp`

- [ ] **Step 1: Extend the initial-state test**

In `src/unittest/windows_relay_worker_test.cpp`, extend `TestWindowsRelayLockAndCallbackMetricsInitialState()` so it also checks:

```cpp
       snapshot.CallbackReceiveBudgetRejectedCount == 0 &&
       snapshot.CallbackReceiveBudgetPausedCount == 0 &&
       snapshot.CallbackReceiveCopyBytes == 0 &&
       snapshot.CallbackReceiveCopyNanos == 0;
```

Add these conditions immediately after `snapshot.CallbackDispatchNanos == 0`.

- [ ] **Step 2: Run and confirm compile failure**

Run on Windows:

```bash
rtk cmake --build build --target tcpquic_windows_relay_worker_test -j
```

Expected: compile fails because `TqWindowsRelayWorkerSnapshot` does not have the new fields.

- [ ] **Step 3: Add worker snapshot fields**

In `src/tunnel/windows_relay_worker.h`, in `TqWindowsRelayWorkerSnapshot`, after `CallbackDispatchNanos`, add:

```cpp
    uint64_t CallbackReceiveBudgetRejectedCount{0};
    uint64_t CallbackReceiveBudgetPausedCount{0};
    uint64_t CallbackReceiveCopyBytes{0};
    uint64_t CallbackReceiveCopyNanos{0};
```

In the private counter block of `TqWindowsRelayWorker`, after `CallbackDispatchNanos_`, add:

```cpp
    std::atomic<uint64_t> CallbackReceiveBudgetRejectedCount_{0};
    std::atomic<uint64_t> CallbackReceiveBudgetPausedCount_{0};
    std::atomic<uint64_t> CallbackReceiveCopyBytes_{0};
    std::atomic<uint64_t> CallbackReceiveCopyNanos_{0};
```

- [ ] **Step 4: Populate worker snapshots**

In `TqWindowsRelayWorker::Snapshot()`, after `snapshot.CallbackDispatchNanos = ...`, add:

```cpp
    snapshot.CallbackReceiveBudgetRejectedCount =
        CallbackReceiveBudgetRejectedCount_.load(std::memory_order_relaxed);
    snapshot.CallbackReceiveBudgetPausedCount =
        CallbackReceiveBudgetPausedCount_.load(std::memory_order_relaxed);
    snapshot.CallbackReceiveCopyBytes =
        CallbackReceiveCopyBytes_.load(std::memory_order_relaxed);
    snapshot.CallbackReceiveCopyNanos =
        CallbackReceiveCopyNanos_.load(std::memory_order_relaxed);
```

In `TqWindowsRelayRuntime::Snapshot()` aggregation, after `total.CallbackDispatchNanos += snapshot.CallbackDispatchNanos;`, add:

```cpp
        total.CallbackReceiveBudgetRejectedCount += snapshot.CallbackReceiveBudgetRejectedCount;
        total.CallbackReceiveBudgetPausedCount += snapshot.CallbackReceiveBudgetPausedCount;
        total.CallbackReceiveCopyBytes += snapshot.CallbackReceiveCopyBytes;
        total.CallbackReceiveCopyNanos += snapshot.CallbackReceiveCopyNanos;
```

- [ ] **Step 5: Add public relay metrics fields**

In `src/runtime/relay_metrics.h`, after `WindowsRelayCallbackDispatchNanos`, add:

```cpp
    uint64_t WindowsRelayCallbackReceiveBudgetRejectedCount{0};
    uint64_t WindowsRelayCallbackReceiveBudgetPausedCount{0};
    uint64_t WindowsRelayCallbackReceiveCopyBytes{0};
    uint64_t WindowsRelayCallbackReceiveCopyNanos{0};
```

- [ ] **Step 6: Map snapshot to public metrics**

In `src/runtime/relay_metrics.cpp`, after:

```cpp
    metrics.WindowsRelayCallbackDispatchNanos = snapshot.CallbackDispatchNanos;
```

add:

```cpp
    metrics.WindowsRelayCallbackReceiveBudgetRejectedCount =
        snapshot.CallbackReceiveBudgetRejectedCount;
    metrics.WindowsRelayCallbackReceiveBudgetPausedCount =
        snapshot.CallbackReceiveBudgetPausedCount;
    metrics.WindowsRelayCallbackReceiveCopyBytes = snapshot.CallbackReceiveCopyBytes;
    metrics.WindowsRelayCallbackReceiveCopyNanos = snapshot.CallbackReceiveCopyNanos;
```

- [ ] **Step 7: Emit JSON fields**

In `TqAppendRelayMetricsJson()` after `windows_relay_callback_dispatch_nanos`, add:

```cpp
    out << ",\"windows_relay_callback_receive_budget_rejected_count\":"
        << metrics.WindowsRelayCallbackReceiveBudgetRejectedCount;
    out << ",\"windows_relay_callback_receive_budget_paused_count\":"
        << metrics.WindowsRelayCallbackReceiveBudgetPausedCount;
    out << ",\"windows_relay_callback_receive_copy_bytes\":"
        << metrics.WindowsRelayCallbackReceiveCopyBytes;
    out << ",\"windows_relay_callback_receive_copy_nanos\":"
        << metrics.WindowsRelayCallbackReceiveCopyNanos;
```

- [ ] **Step 8: Emit trace summary fields**

In `src/runtime/trace.cpp`, extend `TqFormatRelayMetricsSnapshotLine()` with:

```cpp
        " win_cb_recv_budget_rejected=%llu win_cb_recv_budget_paused=%llu"
        " win_cb_recv_copy_bytes=%llu win_cb_recv_copy_nanos=%llu",
```

Pass, in order:

```cpp
        static_cast<unsigned long long>(metrics.WindowsRelayCallbackReceiveBudgetRejectedCount),
        static_cast<unsigned long long>(metrics.WindowsRelayCallbackReceiveBudgetPausedCount),
        static_cast<unsigned long long>(metrics.WindowsRelayCallbackReceiveCopyBytes),
        static_cast<unsigned long long>(metrics.WindowsRelayCallbackReceiveCopyNanos),
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
rtk git add src/tunnel/windows_relay_worker.h src/tunnel/windows_relay_worker.cpp src/runtime/relay_metrics.h src/runtime/relay_metrics.cpp src/runtime/trace.cpp src/unittest/windows_relay_worker_test.cpp
rtk git commit -m "test: expose windows callback receive budget metrics"
```

## Task 2: Record Callback Receive Copy Metrics

**Files:**
- Modify: `src/tunnel/windows_relay_worker.cpp`
- Modify: `src/unittest/windows_relay_worker_test.cpp`

- [ ] **Step 1: Add a failing copy metrics test**

In `src/unittest/windows_relay_worker_test.cpp`, add:

```cpp
bool TestWindowsRelayCallbackReceiveCopyMetricsForTest() {
    QUIC_API_TABLE fakeApi{};
    fakeApi.StreamReceiveComplete = FakeStreamReceiveComplete;
    fakeApi.StreamReceiveSetEnabled = FakeStreamReceiveSetEnabled;
    MsQuic = reinterpret_cast<const MsQuicApi*>(&fakeApi);

    TqWindowsRelayWorker worker;
    if (!StartRelayWorkerForTest(worker)) {
        MsQuic = nullptr;
        worker.Stop();
        return false;
    }
    alignas(MsQuicStream) unsigned char streamStorage[sizeof(MsQuicStream)]{};
    auto* stream = reinterpret_cast<MsQuicStream*>(streamStorage);
    stream->Callback = MsQuicStream::NoOpCallback;
    stream->Context = nullptr;
    stream->Handle = reinterpret_cast<HQUIC>(static_cast<uintptr_t>(1));
    TqRelayHandle handle{};
    TqTuningConfig tuning{};
    tuning.RelayIoSize = 4096;
    tuning.WindowsRelayMaxPendingQuicReceiveBytesPerRelay = 4096;
    if (!worker.RegisterRelayForTest(stream, &handle, tuning, TqCompressAlgo::None)) {
        worker.Stop();
        MsQuic = nullptr;
        return false;
    }

    const auto before = worker.Snapshot();
    uint8_t payload[7]{1, 2, 3, 4, 5, 6, 7};
    QUIC_BUFFER buffer{sizeof(payload), payload};
    QUIC_STREAM_EVENT event{};
    event.Type = QUIC_STREAM_EVENT_RECEIVE;
    event.RECEIVE.Buffers = &buffer;
    event.RECEIVE.BufferCount = 1;
    event.RECEIVE.TotalBufferLength = sizeof(payload);
    const QUIC_STATUS status = TqWindowsRelayWorker::StreamCallback(stream, stream->Context, &event);
    const auto after = worker.Snapshot();
    worker.Stop();
    MsQuic = nullptr;
    return status == QUIC_STATUS_PENDING &&
           after.CallbackReceiveCopyBytes == before.CallbackReceiveCopyBytes + sizeof(payload) &&
           after.CallbackReceiveCopyNanos >= before.CallbackReceiveCopyNanos;
}
```

- [ ] **Step 2: Add the test to `main()`**

In `main()`, add:

```cpp
    if (!TestWindowsRelayCallbackReceiveCopyMetricsForTest()) {
        return 126;
    }
```

- [ ] **Step 3: Run and confirm failure**

Run on Windows:

```bash
rtk cmake --build build --target tcpquic_windows_relay_worker_test -j
rtk build/bin/Release/tcpquic_windows_relay_worker_test
```

Expected: test fails because copy metrics stay zero.

- [ ] **Step 4: Record copy metrics in `BuildDeferredQuicReceiveView()`**

In `src/tunnel/windows_relay_worker.cpp`, after validating `totalLength` and before allocating the view, add:

```cpp
    const uint64_t copyStartNanos = NowSteadyNanos();
```

Before `return view;`, add:

```cpp
    CallbackReceiveCopyBytes_.fetch_add(view->TotalLength, std::memory_order_relaxed);
    CallbackReceiveCopyNanos_.fetch_add(
        NowSteadyNanos() - copyStartNanos,
        std::memory_order_relaxed);
```

- [ ] **Step 5: Run tests**

Run on Windows:

```bash
rtk cmake --build build --target tcpquic_windows_relay_worker_test -j
rtk build/bin/Release/tcpquic_windows_relay_worker_test
```

Expected: test executable exits 0.

- [ ] **Step 6: Commit**

```bash
rtk git add src/tunnel/windows_relay_worker.cpp src/unittest/windows_relay_worker_test.cpp
rtk git commit -m "feat: count windows callback receive copy cost"
```

## Task 3: Add Pre-Copy Callback Budget Rejection

**Files:**
- Modify: `src/tunnel/windows_relay_worker.h`
- Modify: `src/tunnel/windows_relay_worker.cpp`
- Modify: `src/unittest/windows_relay_worker_test.cpp`

- [ ] **Step 1: Add helper declarations**

In `src/tunnel/windows_relay_worker.h`, add private declarations near `BuildDeferredQuicReceiveView()`:

```cpp
    static bool ComputeReceiveEventBytes(
        const QUIC_STREAM_EVENT& event,
        uint64_t* outBytes);
    bool ShouldRejectReceiveInCallback(
        const CallbackBinding& binding,
        MsQuicStream* stream,
        const QUIC_STREAM_EVENT& event,
        uint64_t receiveBytes);
    void PauseQuicReceiveFromCallback(
        const CallbackBinding& binding,
        RelayContext& relay,
        MsQuicStream* stream);
```

- [ ] **Step 2: Add a failing budget rejection test**

In `src/unittest/windows_relay_worker_test.cpp`, add:

```cpp
bool TestWindowsRelayCallbackReceiveBudgetRejectsBeforeCopyForTest() {
    QUIC_API_TABLE fakeApi{};
    fakeApi.StreamReceiveComplete = FakeStreamReceiveComplete;
    fakeApi.StreamReceiveSetEnabled = FakeStreamReceiveSetEnabled;
    MsQuic = reinterpret_cast<const MsQuicApi*>(&fakeApi);
    g_StreamReceiveSetEnabledCalls = 0;
    g_LastStreamReceiveEnabled = TRUE;

    TqWindowsRelayWorker worker;
    if (!StartRelayWorkerForTest(worker)) {
        MsQuic = nullptr;
        worker.Stop();
        return false;
    }
    alignas(MsQuicStream) unsigned char streamStorage[sizeof(MsQuicStream)]{};
    auto* stream = reinterpret_cast<MsQuicStream*>(streamStorage);
    stream->Callback = MsQuicStream::NoOpCallback;
    stream->Context = nullptr;
    stream->Handle = reinterpret_cast<HQUIC>(static_cast<uintptr_t>(1));
    TqRelayHandle handle{};
    TqTuningConfig tuning{};
    tuning.RelayIoSize = 4096;
    tuning.WindowsRelayMaxPendingQuicReceiveBytesPerRelay = 8;
    if (!worker.RegisterRelayForTest(stream, &handle, tuning, TqCompressAlgo::None)) {
        worker.Stop();
        MsQuic = nullptr;
        return false;
    }

    const auto before = worker.Snapshot();
    uint8_t payload[16]{};
    QUIC_BUFFER buffer{sizeof(payload), payload};
    QUIC_STREAM_EVENT event{};
    event.Type = QUIC_STREAM_EVENT_RECEIVE;
    event.RECEIVE.Buffers = &buffer;
    event.RECEIVE.BufferCount = 1;
    event.RECEIVE.TotalBufferLength = sizeof(payload);
    const QUIC_STATUS status = TqWindowsRelayWorker::StreamCallback(stream, stream->Context, &event);
    const auto after = worker.Snapshot();
    worker.Stop();
    MsQuic = nullptr;
    return status == QUIC_STATUS_SUCCESS &&
           after.CallbackReceiveBudgetRejectedCount == before.CallbackReceiveBudgetRejectedCount + 1 &&
           after.CallbackReceiveBudgetPausedCount == before.CallbackReceiveBudgetPausedCount + 1 &&
           after.CallbackReceiveCopyBytes == before.CallbackReceiveCopyBytes &&
           g_StreamReceiveSetEnabledCalls == 1 &&
           g_LastStreamReceiveEnabled == FALSE;
}
```

- [ ] **Step 3: Add the test to `main()`**

In `main()`, add:

```cpp
    if (!TestWindowsRelayCallbackReceiveBudgetRejectsBeforeCopyForTest()) {
        return 127;
    }
```

- [ ] **Step 4: Run and confirm failure**

Run on Windows:

```bash
rtk cmake --build build --target tcpquic_windows_relay_worker_test -j
rtk build/bin/Release/tcpquic_windows_relay_worker_test
```

Expected: test fails because callback still copies and returns `QUIC_STATUS_PENDING`.

- [ ] **Step 5: Implement receive byte calculation**

In `src/tunnel/windows_relay_worker.cpp`, add:

```cpp
bool TqWindowsRelayWorker::ComputeReceiveEventBytes(
    const QUIC_STREAM_EVENT& event,
    uint64_t* outBytes) {
    if (outBytes == nullptr || event.Type != QUIC_STREAM_EVENT_RECEIVE) {
        return false;
    }
    if (event.RECEIVE.BufferCount != 0 && event.RECEIVE.Buffers == nullptr) {
        return false;
    }
    uint64_t total = 0;
    for (uint32_t i = 0; i < event.RECEIVE.BufferCount; ++i) {
        const QUIC_BUFFER& buffer = event.RECEIVE.Buffers[i];
        if (buffer.Length != 0 && buffer.Buffer == nullptr) {
            return false;
        }
        const uint64_t next = total + buffer.Length;
        if (next < total) {
            return false;
        }
        total = next;
    }
    *outBytes = total;
    return true;
}
```

- [ ] **Step 6: Implement callback pause helper**

In `src/tunnel/windows_relay_worker.cpp`, add:

```cpp
void TqWindowsRelayWorker::PauseQuicReceiveFromCallback(
    const CallbackBinding& binding,
    RelayContext& relay,
    MsQuicStream* stream) {
    (void)binding;
    if (relay.QuicReceivePaused.exchange(true, std::memory_order_acq_rel)) {
        return;
    }
    CallbackReceiveBudgetPausedCount_.fetch_add(1, std::memory_order_relaxed);
    QuicReceivePausedCount_.fetch_add(1, std::memory_order_relaxed);
    if (MsQuic != nullptr && MsQuic->StreamReceiveSetEnabled != nullptr &&
        stream != nullptr && stream->Handle != nullptr) {
        const QUIC_STATUS status = MsQuic->StreamReceiveSetEnabled(stream->Handle, FALSE);
        if (QUIC_FAILED(status)) {
            Errors_.fetch_add(1, std::memory_order_relaxed);
        }
    }
}
```

- [ ] **Step 7: Implement budget rejection helper**

In `src/tunnel/windows_relay_worker.cpp`, add:

```cpp
bool TqWindowsRelayWorker::ShouldRejectReceiveInCallback(
    const CallbackBinding& binding,
    MsQuicStream* stream,
    const QUIC_STREAM_EVENT& event,
    uint64_t receiveBytes) {
    const bool fin = (event.RECEIVE.Flags & QUIC_RECEIVE_FLAG_FIN) != 0;
    if (fin || receiveBytes == 0) {
        return false;
    }
    auto* relay = binding.RelayHint.load(std::memory_order_acquire);
    if (relay == nullptr ||
        relay->Generation != binding.Generation ||
        relay->Closing.load(std::memory_order_acquire)) {
        return false;
    }
    const uint64_t limit = relay->Tuning.WindowsRelayMaxPendingQuicReceiveBytesPerRelay;
    if (limit == 0) {
        return false;
    }
    const uint64_t pending = relay->PendingQuicReceiveBytes.load(std::memory_order_acquire);
    if (pending < limit && receiveBytes <= limit - pending) {
        return false;
    }
    CallbackReceiveBudgetRejectedCount_.fetch_add(1, std::memory_order_relaxed);
    PauseQuicReceiveFromCallback(binding, *relay, stream);
    return true;
}
```

- [ ] **Step 8: Call budget helper before copy**

In `StreamCallback(RECEIVE)`, after `binding->Closing` check and before `BuildDeferredQuicReceiveView()`, add:

```cpp
        uint64_t receiveBytes = 0;
        if (!ComputeReceiveEventBytes(*event, &receiveBytes)) {
            worker->Errors_.fetch_add(1, std::memory_order_relaxed);
            return QUIC_STATUS_SUCCESS;
        }
        if (worker->ShouldRejectReceiveInCallback(*binding, stream, *event, receiveBytes)) {
            return QUIC_STATUS_SUCCESS;
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
rtk git add src/tunnel/windows_relay_worker.h src/tunnel/windows_relay_worker.cpp src/unittest/windows_relay_worker_test.cpp
rtk git commit -m "feat: budget windows callback receive before copy"
```

## Task 4: Cover FIN Bypass And Documentation

**Files:**
- Modify: `src/unittest/windows_relay_worker_test.cpp`
- Modify: `docs/relay_win.md`

- [ ] **Step 1: Add FIN bypass test**

In `src/unittest/windows_relay_worker_test.cpp`, add:

```cpp
bool TestWindowsRelayCallbackReceiveBudgetDoesNotRejectFinForTest() {
    QUIC_API_TABLE fakeApi{};
    fakeApi.StreamReceiveComplete = FakeStreamReceiveComplete;
    fakeApi.StreamReceiveSetEnabled = FakeStreamReceiveSetEnabled;
    MsQuic = reinterpret_cast<const MsQuicApi*>(&fakeApi);
    g_StreamReceiveSetEnabledCalls = 0;

    TqWindowsRelayWorker worker;
    if (!StartRelayWorkerForTest(worker)) {
        MsQuic = nullptr;
        worker.Stop();
        return false;
    }
    alignas(MsQuicStream) unsigned char streamStorage[sizeof(MsQuicStream)]{};
    auto* stream = reinterpret_cast<MsQuicStream*>(streamStorage);
    stream->Callback = MsQuicStream::NoOpCallback;
    stream->Context = nullptr;
    stream->Handle = reinterpret_cast<HQUIC>(static_cast<uintptr_t>(1));
    TqRelayHandle handle{};
    TqTuningConfig tuning{};
    tuning.RelayIoSize = 4096;
    tuning.WindowsRelayMaxPendingQuicReceiveBytesPerRelay = 8;
    if (!worker.RegisterRelayForTest(stream, &handle, tuning, TqCompressAlgo::None)) {
        worker.Stop();
        MsQuic = nullptr;
        return false;
    }

    uint8_t payload[16]{};
    QUIC_BUFFER buffer{sizeof(payload), payload};
    QUIC_STREAM_EVENT event{};
    event.Type = QUIC_STREAM_EVENT_RECEIVE;
    event.RECEIVE.Buffers = &buffer;
    event.RECEIVE.BufferCount = 1;
    event.RECEIVE.TotalBufferLength = sizeof(payload);
    event.RECEIVE.Flags = QUIC_RECEIVE_FLAG_FIN;
    const auto before = worker.Snapshot();
    const QUIC_STATUS status = TqWindowsRelayWorker::StreamCallback(stream, stream->Context, &event);
    const auto after = worker.Snapshot();
    worker.Stop();
    MsQuic = nullptr;
    return status == QUIC_STATUS_PENDING &&
           after.CallbackReceiveBudgetRejectedCount == before.CallbackReceiveBudgetRejectedCount &&
           after.CallbackReceiveCopyBytes >= before.CallbackReceiveCopyBytes + sizeof(payload) &&
           g_StreamReceiveSetEnabledCalls == 0;
}
```

- [ ] **Step 2: Add the test to `main()`**

In `main()`, add:

```cpp
    if (!TestWindowsRelayCallbackReceiveBudgetDoesNotRejectFinForTest()) {
        return 128;
    }
```

- [ ] **Step 3: Update `docs/relay_win.md` 3.4**

Replace the 3.4 `建议：` block with:

```markdown
当前状态：

- callback 线程在复制前通过 `CallbackBinding::RelayHint` 读取 relay generation、pending bytes 和上限，先做轻量预算判断。
- 对不含 FIN 且会超过 `WindowsRelayMaxPendingQuicReceiveBytesPerRelay` 的 receive，callback 线程会暂停后续 QUIC receive 并跳过复制。
- worker 侧 `EnqueueDeferredQuicReceiveView()` 仍保留预算检查，作为并发竞态兜底。
- copy 字节和耗时通过 `windows_relay_callback_receive_copy_bytes` / `windows_relay_callback_receive_copy_nanos` 观测，预算拒绝和暂停通过对应 callback receive budget 指标观测。
```

- [ ] **Step 4: Update `docs/relay_win.md` 3.8**

Replace `当前剩余缺口：` with:

```markdown
当前状态：

- maintenance 扫描耗时、每轮扫描 relay 数、receive view 线性查找成本、callback 复制字节/耗时均已有指标。
- 后续如果继续优化 receive callback，可评估 MsQuic buffer ownership 或分片处理，但当前观测缺口已关闭。
```

- [ ] **Step 5: Update priority table**

Change:

```markdown
| 中 | receive callback 复制前加入预算判断和指标 | 降低 callback 线程 CPU/内存尖峰。 |
| 部分完成 | 补齐 maintenance/copy/linear-search 指标 | maintenance 和 linear-search 指标已补齐，callback copy 指标留待 receive callback 预算方案处理。 |
```

to:

```markdown
| 已完成 | receive callback 复制前加入预算判断和指标 | 超限 receive 会在复制前被拒绝并暂停后续 receive，copy 成本已有指标。 |
| 已完成 | 补齐 maintenance/copy/linear-search 指标 | maintenance、linear-search 和 callback copy 指标均已补齐。 |
```

- [ ] **Step 6: Run verification**

Run on Windows:

```bash
rtk cmake --build build --target tcpquic_windows_relay_worker_test -j
rtk build/bin/Release/tcpquic_windows_relay_worker_test
```

Run on Linux:

```bash
rtk cmake --build build --target tcpquic-proxy -j$(nproc)
rtk cmake --build build --target tcpquic_platform_socket_test tcpquic_thread_pool_test -j$(nproc)
rtk proxy ./build/bin/Release/tcpquic_platform_socket_test
rtk proxy ./build/bin/Release/tcpquic_thread_pool_test
rtk git diff --check
```

Expected: all commands exit 0.

- [ ] **Step 7: Commit**

```bash
rtk git add src/unittest/windows_relay_worker_test.cpp docs/relay_win.md
rtk git commit -m "docs: update windows receive callback budget status"
```

## Execution Notes

- Execute this plan in a fresh worktree, for example `.worktrees/windows-receive-callback-budget`, because the main checkout may contain unrelated uncommitted docs/config changes.
- Prefer `superpowers:subagent-driven-development`: one implementer per task, spec review and code-quality review after each task.
- Do not merge until Windows-specific tests have been run on Windows or the residual gap is explicitly accepted.
