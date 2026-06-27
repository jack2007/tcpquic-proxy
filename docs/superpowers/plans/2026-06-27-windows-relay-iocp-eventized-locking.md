# Windows Relay IOCP Eventized Locking Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 将 Windows relay 的热点数据结构操作收敛到 relay worker IOCP 线程，逐步移除 MsQuic callback 和数据面热路径上的 worker/per-relay mutex 竞争。

**Architecture:** 复用现有 Windows IOCP 作为唯一跨线程事件入口。第一阶段增加观测并拆分 snapshot 长临界区；第二阶段让 MsQuic callback 只携带 relay id/generation 和 payload 投递 IOCP；第三阶段把 register、stop、snapshot 变成同步 control command，由 worker 线程读写 `Relays_`、`RetiredCallbacks_` 和 per-relay 队列。

**Tech Stack:** C++17, Windows IOCP, MsQuic C API, Winsock overlapped I/O, CMake, existing `tcpquic_windows_relay_worker_test`, PowerShell loopback validation.

---

## 设计依据

本计划实现设计文档：

- `docs/superpowers/specs/2026-06-27-windows-relay-iocp-eventized-lock-design.md`

实现必须保持以下约束：

- 不改变 TCP/QUIC 背压策略。
- 不改变 `TqRelayStart()` / `TqRelayStop()` / `Snapshot()` 的上层同步语义。
- MsQuic receive callback 返回 `QUIC_STATUS_PENDING` 后，必须由 worker 随后调用 `ReceiveComplete()` 或在 stale/close 路径补齐 ownership。
- TCP overlapped operation 可以继续持有 `std::shared_ptr<RelayContext>`，callback operation 不能为了查 relay 而持有 relay shared pointer。
- 每个阶段必须能单独编译、单独测试、单独提交。

## 文件结构

- Modify: `src/tunnel/windows_relay_worker.h`
  - 增加 snapshot 观测字段、relay generation、callback-only operation declarations、control command declarations、测试 helper declarations。
- Modify: `src/tunnel/windows_relay_worker.cpp`
  - 实现 lock/snapshot 观测、callback-by-id 投递、generation 校验、worker-local receive 入队、control command。
- Modify: `src/runtime/relay_metrics.h`
  - 增加 Windows relay lock/callback dispatch 观测字段。
- Modify: `src/runtime/relay_metrics.cpp`
  - 聚合并输出新增 Windows relay metrics JSON。
- Modify: `src/unittest/windows_relay_worker_test.cpp`
  - 增加阶段性单测：snapshot 观测、send complete 纯投递、receive stale complete、callback stale drop、control command。
- Do not modify: `src/runtime/trace.cpp`
  - 本计划将新增指标收敛到 `relay_metrics` 输出；trace active relay/stats 的展示扩展另行设计。
- Do not modify: `docs/relay_win.md`
  - 该文档当前未跟踪，本计划不要求纳入实现提交。

## 通用验证命令

Windows 开发机上执行：

```powershell
cmake -S . -B build-x64 -A x64
cmake --build build-x64 --config Debug --target tcpquic_windows_relay_worker_test
.\build-x64\bin\Debug\tcpquic_windows_relay_worker_test.exe
```

预期：测试进程退出码为 `0`。

完整 Windows loopback 验证：

```powershell
cmake --build build-x64 --config Release --target tcpquic-proxy
.\scripts\test-tcpquic-proxy-windows.ps1 -Compress off
.\scripts\test-tcpquic-proxy-windows.ps1 -Compress zstd
```

预期：两个脚本均正常结束，未出现 relay fatal reset、TCP hard error 或 receive ownership 泄漏相关失败。

---

### Task 1: 增加 Windows relay lock/callback 观测指标

**Files:**
- Modify: `src/tunnel/windows_relay_worker.h`
- Modify: `src/tunnel/windows_relay_worker.cpp`
- Modify: `src/runtime/relay_metrics.h`
- Modify: `src/runtime/relay_metrics.cpp`
- Test: `src/unittest/windows_relay_worker_test.cpp`

- [x] **Step 1: 编写失败单测，验证 snapshot 暴露新增观测字段**

在 `src/unittest/windows_relay_worker_test.cpp` 增加测试函数：

```cpp
bool TestWindowsRelayLockAndCallbackMetricsInitialState() {
    TqWindowsRelayWorker worker;
    const TqWindowsRelayWorkerSnapshot snapshot = worker.Snapshot();
    return snapshot.WorkerLockAcquireCount == 0 &&
           snapshot.WorkerLockWaitNanos == 0 &&
           snapshot.FindRelayByIdCount == 0 &&
           snapshot.CallbackDispatchNanos == 0 &&
           snapshot.SnapshotBuildNanos > 0 &&
           snapshot.SnapshotActiveRelaysScanned == 0;
}
```

在 `main()` 的 Windows test list 中调用：

```cpp
    if (!TestWindowsRelayLockAndCallbackMetricsInitialState()) {
        return 47;
    }
```

- [x] **Step 2: 运行单测并确认编译失败**

Run:

```powershell
cmake --build build-x64 --config Debug --target tcpquic_windows_relay_worker_test
```

Expected: 编译失败，错误包含 `WorkerLockAcquireCount` 或 `WorkerLockWaitNanos` 不是 `TqWindowsRelayWorkerSnapshot` 成员。

- [x] **Step 3: 在 worker snapshot 结构中增加观测字段**

在 `src/tunnel/windows_relay_worker.h` 的 `TqWindowsRelayWorkerSnapshot` 中追加：

```cpp
    uint64_t WorkerLockAcquireCount{0};
    uint64_t WorkerLockWaitNanos{0};
    uint64_t FindRelayByIdCount{0};
    uint64_t CallbackDispatchNanos{0};
    uint64_t SnapshotBuildNanos{0};
    uint64_t SnapshotActiveRelaysScanned{0};
```

在 `TqRelayMetricsSnapshot` 中追加同名 Windows 聚合字段，使用 `WindowsRelay` 前缀：

```cpp
    uint64_t WindowsRelayWorkerLockAcquireCount{0};
    uint64_t WindowsRelayWorkerLockWaitNanos{0};
    uint64_t WindowsRelayFindRelayByIdCount{0};
    uint64_t WindowsRelayCallbackDispatchNanos{0};
    uint64_t WindowsRelaySnapshotBuildNanos{0};
    uint64_t WindowsRelaySnapshotActiveRelaysScanned{0};
```

- [x] **Step 4: 增加轻量计时 helper 和 worker 原子计数**

在 `src/tunnel/windows_relay_worker.cpp` anonymous namespace 中增加：

```cpp
uint64_t NowSteadyNanos() {
    return static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count());
}
```

在 `src/tunnel/windows_relay_worker.cpp` includes 中加入：

```cpp
#include <chrono>
```

在 `TqWindowsRelayWorker` private members 中增加：

```cpp
    std::atomic<uint64_t> WorkerLockAcquireCount_{0};
    std::atomic<uint64_t> WorkerLockWaitNanos_{0};
    std::atomic<uint64_t> FindRelayByIdCount_{0};
    std::atomic<uint64_t> CallbackDispatchNanos_{0};
    std::atomic<uint64_t> SnapshotBuildNanos_{0};
    std::atomic<uint64_t> SnapshotActiveRelaysScanned_{0};
```

- [x] **Step 5: 给 `Lock_` 热路径加观测**

在 `FindRelayById()` 中记录次数和 lock wait：

```cpp
std::shared_ptr<TqWindowsRelayWorker::RelayContext> TqWindowsRelayWorker::FindRelayById(
    uint64_t relayId) {
    FindRelayByIdCount_.fetch_add(1, std::memory_order_relaxed);
    const uint64_t waitStart = NowSteadyNanos();
    std::lock_guard<std::mutex> guard(Lock_);
    WorkerLockWaitNanos_.fetch_add(NowSteadyNanos() - waitStart, std::memory_order_relaxed);
    WorkerLockAcquireCount_.fetch_add(1, std::memory_order_relaxed);
    const auto it = Relays_.find(relayId);
    return it != Relays_.end() ? it->second : nullptr;
}
```

在 `StreamCallback()` 开始处记录 callback dispatch 时间：

```cpp
    const uint64_t callbackStartNanos = NowSteadyNanos();
```

在所有 return 前统一通过 guard 累计，使用局部 RAII：

```cpp
    struct CallbackDispatchGuard {
        TqWindowsRelayWorker* Worker{nullptr};
        uint64_t StartNanos{0};
        ~CallbackDispatchGuard() {
            if (Worker != nullptr && StartNanos != 0) {
                Worker->CallbackDispatchNanos_.fetch_add(
                    NowSteadyNanos() - StartNanos,
                    std::memory_order_relaxed);
            }
        }
    } callbackDispatchGuard{binding->Worker, callbackStartNanos};
```

- [x] **Step 6: 在 `Snapshot()` 中填充字段**

在 `TqWindowsRelayWorker::Snapshot()` 开头记录：

```cpp
    const uint64_t snapshotStartNanos = NowSteadyNanos();
```

在 return 前填充：

```cpp
    snapshot.WorkerLockAcquireCount = WorkerLockAcquireCount_.load(std::memory_order_relaxed);
    snapshot.WorkerLockWaitNanos = WorkerLockWaitNanos_.load(std::memory_order_relaxed);
    snapshot.FindRelayByIdCount = FindRelayByIdCount_.load(std::memory_order_relaxed);
    snapshot.CallbackDispatchNanos = CallbackDispatchNanos_.load(std::memory_order_relaxed);
    snapshot.SnapshotActiveRelaysScanned =
        SnapshotActiveRelaysScanned_.load(std::memory_order_relaxed);
    const uint64_t elapsed = NowSteadyNanos() - snapshotStartNanos;
    SnapshotBuildNanos_.fetch_add(elapsed, std::memory_order_relaxed);
    snapshot.SnapshotBuildNanos =
        SnapshotBuildNanos_.load(std::memory_order_relaxed);
```

在 snapshot 扫描 active relay 时累计：

```cpp
        SnapshotActiveRelaysScanned_.fetch_add(Relays_.size(), std::memory_order_relaxed);
```

- [x] **Step 7: 在 runtime metrics 中聚合并输出 JSON**

在 Windows snapshot 聚合处追加：

```cpp
    metrics.WindowsRelayWorkerLockAcquireCount = snapshot.WorkerLockAcquireCount;
    metrics.WindowsRelayWorkerLockWaitNanos = snapshot.WorkerLockWaitNanos;
    metrics.WindowsRelayFindRelayByIdCount = snapshot.FindRelayByIdCount;
    metrics.WindowsRelayCallbackDispatchNanos = snapshot.CallbackDispatchNanos;
    metrics.WindowsRelaySnapshotBuildNanos = snapshot.SnapshotBuildNanos;
    metrics.WindowsRelaySnapshotActiveRelaysScanned = snapshot.SnapshotActiveRelaysScanned;
```

在 JSON 输出处追加：

```cpp
    out << ",\"windows_relay_worker_lock_acquire_count\":"
        << metrics.WindowsRelayWorkerLockAcquireCount;
    out << ",\"windows_relay_worker_lock_wait_nanos\":"
        << metrics.WindowsRelayWorkerLockWaitNanos;
    out << ",\"windows_relay_find_relay_by_id_count\":"
        << metrics.WindowsRelayFindRelayByIdCount;
    out << ",\"windows_relay_callback_dispatch_nanos\":"
        << metrics.WindowsRelayCallbackDispatchNanos;
    out << ",\"windows_relay_snapshot_build_nanos\":"
        << metrics.WindowsRelaySnapshotBuildNanos;
    out << ",\"windows_relay_snapshot_active_relays_scanned\":"
        << metrics.WindowsRelaySnapshotActiveRelaysScanned;
```

- [ ] **Step 8: 运行单测并提交**

Run:

```powershell
cmake --build build-x64 --config Debug --target tcpquic_windows_relay_worker_test
.\build-x64\bin\Debug\tcpquic_windows_relay_worker_test.exe
```

Expected: exit code `0`.

Commit:

```bash
git add src/tunnel/windows_relay_worker.h src/tunnel/windows_relay_worker.cpp src/runtime/relay_metrics.h src/runtime/relay_metrics.cpp src/unittest/windows_relay_worker_test.cpp
git commit -m "test: observe windows relay lock contention"
```

---

### Task 2: 拆分 runtime 和 worker snapshot 长临界区

**Files:**
- Modify: `src/tunnel/windows_relay_worker.cpp`
- Test: `src/unittest/windows_relay_worker_test.cpp`

- [x] **Step 1: 添加 snapshot 并发压力单测**

在 `src/unittest/windows_relay_worker_test.cpp` 增加：

```cpp
bool TestWindowsRelaySnapshotConcurrentWithRegisterAndStop() {
    QUIC_API_TABLE fakeApi{};
    fakeApi.StreamReceiveComplete = FakeStreamReceiveComplete;
    fakeApi.StreamReceiveSetEnabled = FakeStreamReceiveSetEnabled;
    MsQuic = reinterpret_cast<const MsQuicApi*>(&fakeApi);

    TqWindowsRelayWorker worker;
    if (!worker.Start()) {
        MsQuic = nullptr;
        return false;
    }

    std::atomic<bool> stop{false};
    std::atomic<bool> failed{false};
    std::thread sampler([&] {
        while (!stop.load(std::memory_order_acquire)) {
            const TqWindowsRelayWorkerSnapshot snapshot = worker.Snapshot();
            if (snapshot.ActiveRelays > snapshot.ActiveRelayStates.size()) {
                failed.store(true, std::memory_order_release);
            }
        }
    });

    for (int i = 0; i < 16; ++i) {
        TqSocketHandle pair[2]{TqInvalidSocket, TqInvalidSocket};
        if (!TqSocketPair(pair)) {
            failed.store(true, std::memory_order_release);
            break;
        }
        alignas(MsQuicStream) unsigned char streamStorage[sizeof(MsQuicStream)]{};
        auto* stream = reinterpret_cast<MsQuicStream*>(streamStorage);
        stream->Callback = MsQuicStream::NoOpCallback;
        stream->Context = nullptr;
        stream->Handle = reinterpret_cast<HQUIC>(static_cast<uintptr_t>(1 + i));
        TqRelayHandle handle{};
        TqTuningConfig tuning{};
        tuning.RelayIoSize = 4096;
        if (!worker.RegisterRelay(pair[0], stream, nullptr, nullptr, &handle, tuning, TqCompressAlgo::None)) {
            failed.store(true, std::memory_order_release);
            TqCloseSocket(pair[0]);
            TqCloseSocket(pair[1]);
            break;
        }
        worker.StopRelay(handle.WindowsRelayId);
        TqCloseSocket(pair[1]);
    }

    stop.store(true, std::memory_order_release);
    sampler.join();
    worker.Stop();
    MsQuic = nullptr;
    return !failed.load(std::memory_order_acquire);
}
```

在 `main()` 中调用并分配返回码 `48`。

- [x] **Step 2: 运行测试确认当前实现能通过或暴露长临界区指标**

Run:

```powershell
cmake --build build-x64 --config Debug --target tcpquic_windows_relay_worker_test
.\build-x64\bin\Debug\tcpquic_windows_relay_worker_test.exe
```

Expected: exit code `0`。记录 `SnapshotBuildNanos` 和 `WorkerLockWaitNanos` 的当前值，作为拆锁前的基线。

- [x] **Step 3: 拆分 worker snapshot**

将 `TqWindowsRelayWorker::Snapshot()` 中对 `Relays_` 的遍历改成先复制 vector：

```cpp
    std::vector<std::shared_ptr<RelayContext>> relays;
    {
        const uint64_t waitStart = NowSteadyNanos();
        std::lock_guard<std::mutex> guard(Lock_);
        WorkerLockWaitNanos_.fetch_add(NowSteadyNanos() - waitStart, std::memory_order_relaxed);
        WorkerLockAcquireCount_.fetch_add(1, std::memory_order_relaxed);
        relays.reserve(Relays_.size());
        for (const auto& entry : Relays_) {
            if (entry.second) {
                relays.push_back(entry.second);
            }
        }
    }
    snapshot.ActiveRelays = relays.size();
    SnapshotActiveRelaysScanned_.fetch_add(relays.size(), std::memory_order_relaxed);
    snapshot.ActiveRelayStates.reserve(relays.size());
    for (const auto& relay : relays) {
        if (!relay) {
            continue;
        }
        // move the existing per-relay snapshot body here
    }
```

把原先 `for (const auto& entry : Relays_)` 内的 `entry.second` 改成 `relay`。

- [x] **Step 4: 拆分 runtime snapshot**

将 `TqWindowsRelayRuntime::Snapshot()` 改为先复制 worker 裸指针列表：

```cpp
    std::vector<TqWindowsRelayWorker*> workers;
    {
        std::lock_guard<std::mutex> guard(Lock_);
        workers.reserve(Workers_.size());
        for (const auto& worker : Workers_) {
            if (worker) {
                workers.push_back(worker.get());
            }
        }
    }
    for (const auto* worker : workers) {
        if (worker == nullptr) {
            continue;
        }
        const auto snapshot = worker->Snapshot();
        // keep existing aggregation body
    }
```

不要在持有 runtime `Lock_` 时调用 `worker->Snapshot()`。

- [ ] **Step 5: 运行测试并提交**

Run:

```powershell
cmake --build build-x64 --config Debug --target tcpquic_windows_relay_worker_test
.\build-x64\bin\Debug\tcpquic_windows_relay_worker_test.exe
```

Expected: exit code `0`。

Commit:

```bash
git add src/tunnel/windows_relay_worker.cpp src/unittest/windows_relay_worker_test.cpp
git commit -m "perf: shorten windows relay snapshot locks"
```

---

### Task 3: 引入 relay generation 和 callback-by-id operation 基础设施

**Files:**
- Modify: `src/tunnel/windows_relay_worker.h`
- Modify: `src/tunnel/windows_relay_worker.cpp`
- Test: `src/unittest/windows_relay_worker_test.cpp`

- [x] **Step 1: 添加 generation 测试 helper 的失败单测**

在 `src/unittest/windows_relay_worker_test.cpp` 增加：

```cpp
bool TestWindowsRelayCallbackOperationGenerationMismatchDropsForTest() {
    TqWindowsRelayWorker worker;
    alignas(MsQuicStream) unsigned char streamStorage[sizeof(MsQuicStream)]{};
    auto* stream = reinterpret_cast<MsQuicStream*>(streamStorage);
    stream->Callback = MsQuicStream::NoOpCallback;
    stream->Context = nullptr;
    stream->Handle = reinterpret_cast<HQUIC>(static_cast<uintptr_t>(1));
    TqRelayHandle handle{};
    TqTuningConfig tuning{};
    if (!worker.RegisterRelayForTest(stream, &handle, tuning, TqCompressAlgo::None)) {
        return false;
    }
    const uint64_t before = worker.Snapshot().WindowsPostedCallbackStaleDropCount;
    if (!worker.TestResolveStaleCallbackForTest(handle.WindowsRelayId)) {
        return false;
    }
    const uint64_t after = worker.Snapshot().WindowsPostedCallbackStaleDropCount;
    return after == before + 1;
}
```

在 `main()` 中调用并分配返回码 `49`。

- [x] **Step 2: 运行编译并确认失败**

Run:

```powershell
cmake --build build-x64 --config Debug --target tcpquic_windows_relay_worker_test
```

Expected: 编译失败，错误包含 `TestResolveStaleCallbackForTest` 未声明。

- [x] **Step 3: 增加 generation 字段**

在 `CallbackBinding`、`IoOperation`、`RelayContext`、`TqWindowsQuicSendOperation` 中加入：

```cpp
uint64_t Generation{0};
```

在 worker members 中加入：

```cpp
    std::atomic<uint64_t> NextGeneration_{1};
```

在 register 路径设置：

```cpp
    relay->Generation = NextGeneration_.fetch_add(1, std::memory_order_relaxed);
    relay->Callback->Generation = relay->Generation;
```

创建 `TqWindowsQuicSendOperation` 时设置：

```cpp
    sendOp->Generation = relay->Generation;
```

- [x] **Step 4: 增加 callback-by-id post helper**

在 header private declarations 中加入：

```cpp
    bool PostCallbackOperationById(
        TqWindowsIocpOperationType type,
        const CallbackBinding& binding,
        uint64_t value = 0,
        size_t length = 0,
        std::shared_ptr<TqWindowsPendingQuicReceive> receiveView = nullptr);
    std::shared_ptr<RelayContext> ResolveRelayForCallback(uint64_t relayId, uint64_t generation);
```

实现：

```cpp
bool TqWindowsRelayWorker::PostCallbackOperationById(
    TqWindowsIocpOperationType type,
    const CallbackBinding& binding,
    uint64_t value,
    size_t length,
    std::shared_ptr<TqWindowsPendingQuicReceive> receiveView) {
    if (Iocp_ == nullptr) {
        CallbackIocpPostFailedCount_.fetch_add(1, std::memory_order_relaxed);
        return false;
    }
    auto op = std::make_unique<IoOperation>();
    op->Event = type;
    op->RelayId = binding.RelayId;
    op->Generation = binding.Generation;
    op->Value = value;
    op->Length = length;
    op->ReceiveView = std::move(receiveView);
    IoOperation* raw = op.release();
    if (!::PostQueuedCompletionStatus(static_cast<HANDLE>(Iocp_), 0, 0, &raw->Overlapped)) {
        delete raw;
        CallbackIocpPostFailedCount_.fetch_add(1, std::memory_order_relaxed);
        return false;
    }
    CallbackIocpPostCount_.fetch_add(1, std::memory_order_relaxed);
    if (type == TqWindowsIocpOperationType::RelayReceiveReady) {
        ReceiveReadyPostCount_.fetch_add(1, std::memory_order_relaxed);
    }
    return true;
}
```

实现 resolver：

```cpp
std::shared_ptr<TqWindowsRelayWorker::RelayContext> TqWindowsRelayWorker::ResolveRelayForCallback(
    uint64_t relayId,
    uint64_t generation) {
    auto relay = FindRelayById(relayId);
    if (!relay || relay->Generation != generation ||
        relay->Closing.load(std::memory_order_acquire)) {
        PostedCallbackStaleDropCount_.fetch_add(1, std::memory_order_relaxed);
        return nullptr;
    }
    return relay;
}
```

- [x] **Step 5: 增加 stale resolver 测试 helper**

在 `TQ_UNIT_TESTING` public declarations 中加入：

```cpp
bool TestResolveStaleCallbackForTest(uint64_t relayId);
```

实现：

```cpp
bool TqWindowsRelayWorker::TestResolveStaleCallbackForTest(uint64_t relayId) {
    auto relay = FindRelayById(relayId);
    if (!relay || !relay->Callback) {
        return false;
    }
    const uint64_t before = PostedCallbackStaleDropCount_.load(std::memory_order_relaxed);
    auto staleRelay = ResolveRelayForCallback(relayId, relay->Generation + 1);
    return staleRelay == nullptr &&
           PostedCallbackStaleDropCount_.load(std::memory_order_relaxed) == before + 1;
}
```

Expected: helper 直接验证 generation mismatch 会经过 `ResolveRelayForCallback()`，且 stale drop 只增加一次。

- [x] **Step 6: Wire resolver into callback operation cases**

In `Run()` for callback-sourced operations, before calling handlers by `op->Relay`, add:

```cpp
auto callbackRelay = op->Relay ? op->Relay : ResolveRelayForCallback(op->RelayId, op->Generation);
```

Use `callbackRelay` for:

- `RelayReceiveReady`
- `QuicIdealSendBuffer`
- `QuicPeerSendAborted`
- `QuicPeerReceiveAborted`
- `QuicShutdownComplete`

Do not change TCP recv/send completion handling in this task.

- [ ] **Step 7: 运行测试并提交**

Run:

```powershell
cmake --build build-x64 --config Debug --target tcpquic_windows_relay_worker_test
.\build-x64\bin\Debug\tcpquic_windows_relay_worker_test.exe
```

Expected: exit code `0`。

Commit:

```bash
git add src/tunnel/windows_relay_worker.h src/tunnel/windows_relay_worker.cpp src/unittest/windows_relay_worker_test.cpp
git commit -m "feat: add windows relay callback generations"
```

---

### Task 4: 将 SEND_COMPLETE callback 改为纯 IOCP 投递

**Files:**
- Modify: `src/tunnel/windows_relay_worker.h`
- Modify: `src/tunnel/windows_relay_worker.cpp`
- Test: `src/unittest/windows_relay_worker_test.cpp`

- [x] **Step 1: 改写现有 send complete callback 测试期望**

更新 `TestWindowsRelaySendCompleteIocpCallbackQueue()` 或新增测试：

```cpp
bool TestWindowsRelaySendCompleteCallbackDoesNotFindRelayForTest() {
    TqWindowsRelayWorker worker;
    alignas(MsQuicStream) unsigned char streamStorage[sizeof(MsQuicStream)]{};
    auto* stream = reinterpret_cast<MsQuicStream*>(streamStorage);
    stream->Callback = MsQuicStream::NoOpCallback;
    stream->Context = nullptr;
    stream->Handle = reinterpret_cast<HQUIC>(static_cast<uintptr_t>(1));
    TqRelayHandle handle{};
    TqTuningConfig tuning{};
    if (!worker.RegisterRelayForTest(stream, &handle, tuning, TqCompressAlgo::None)) {
        return false;
    }
    auto* sendOp = worker.TestCreateQuicSendOperationForTest(handle.WindowsRelayId, 7);
    QUIC_STREAM_EVENT event{};
    event.Type = QUIC_STREAM_EVENT_SEND_COMPLETE;
    event.SEND_COMPLETE.ClientContext = sendOp;
    const uint64_t beforeFinds = worker.Snapshot().FindRelayByIdCount;
    if (TqWindowsRelayWorker::StreamCallback(stream, stream->Context, &event) != QUIC_STATUS_SUCCESS) {
        return false;
    }
    const uint64_t afterFinds = worker.Snapshot().FindRelayByIdCount;
    return afterFinds == beforeFinds;
}
```

在 `main()` 中调用并分配返回码 `50`。

- [x] **Step 2: 运行测试确认失败**

Run:

```powershell
cmake --build build-x64 --config Debug --target tcpquic_windows_relay_worker_test
.\build-x64\bin\Debug\tcpquic_windows_relay_worker_test.exe
```

Expected: 测试失败，因为当前 `QueueQuicSendCompleteFromCallback()` 调用 `FindRelayById()`。

- [x] **Step 3: 替换 send complete callback post 路径**

新增 helper：

```cpp
void TqWindowsRelayWorker::QueueQuicSendCompleteByIdFromCallback(
    const CallbackBinding& binding,
    TqWindowsQuicSendOperation* operation) {
    if (operation == nullptr) {
        return;
    }
    operation->RelayId = operation->RelayId != 0 ? operation->RelayId : binding.RelayId;
    operation->Generation = operation->Generation != 0 ? operation->Generation : binding.Generation;
    if (!PostCallbackOperationById(
            TqWindowsIocpOperationType::QuicSendComplete,
            binding,
            reinterpret_cast<uintptr_t>(operation))) {
        std::unique_ptr<TqWindowsQuicSendOperation> cleanup(operation);
        CallbackIocpPostFailedCount_.fetch_add(1, std::memory_order_relaxed);
        Errors_.fetch_add(1, std::memory_order_relaxed);
    }
}
```

在 `StreamCallback(SEND_COMPLETE)` 中调用：

```cpp
        auto* sendOp = static_cast<TqWindowsQuicSendOperation*>(event->SEND_COMPLETE.ClientContext);
        worker->QueueQuicSendCompleteByIdFromCallback(*binding, sendOp);
        return QUIC_STATUS_SUCCESS;
```

- [x] **Step 4: 更新 `ProcessQuicSendCompleteOperation()` 使用 generation**

Change signature:

```cpp
void ProcessQuicSendCompleteOperation(uint64_t relayId, uint64_t generation, uintptr_t operationValue);
```

Handler:

```cpp
auto relay = generation != 0
    ? ResolveRelayForCallback(relayId != 0 ? relayId : operation->RelayId, generation)
    : FindRelayById(relayId != 0 ? relayId : operation->RelayId);
```

If `relay == nullptr`, release operation and return after stale count has been incremented by resolver.

- [ ] **Step 5: 运行测试并提交**

Run:

```powershell
cmake --build build-x64 --config Debug --target tcpquic_windows_relay_worker_test
.\build-x64\bin\Debug\tcpquic_windows_relay_worker_test.exe
```

Expected: exit code `0`。

Commit:

```bash
git add src/tunnel/windows_relay_worker.h src/tunnel/windows_relay_worker.cpp src/unittest/windows_relay_worker_test.cpp
git commit -m "feat: route windows send complete callbacks by id"
```

---

### Task 5: 将 RECEIVE callback 改为 worker 入队并移除 `PendingReceiveLock` 主路径

**Files:**
- Modify: `src/tunnel/windows_relay_worker.h`
- Modify: `src/tunnel/windows_relay_worker.cpp`
- Test: `src/unittest/windows_relay_worker_test.cpp`

- [x] **Step 1: 添加 receive callback 不查 relay 的失败单测**

```cpp
bool TestWindowsRelayReceiveCallbackDoesNotFindRelayForTest() {
    QUIC_API_TABLE fakeApi{};
    fakeApi.StreamReceiveComplete = FakeStreamReceiveComplete;
    fakeApi.StreamReceiveSetEnabled = FakeStreamReceiveSetEnabled;
    MsQuic = reinterpret_cast<const MsQuicApi*>(&fakeApi);
    g_StreamReceiveCompleteBytes = 0;
    g_StreamReceiveCompleteCalls = 0;

    TqWindowsRelayWorker worker;
    alignas(MsQuicStream) unsigned char streamStorage[sizeof(MsQuicStream)]{};
    auto* stream = reinterpret_cast<MsQuicStream*>(streamStorage);
    stream->Callback = MsQuicStream::NoOpCallback;
    stream->Context = nullptr;
    stream->Handle = reinterpret_cast<HQUIC>(static_cast<uintptr_t>(1));
    TqRelayHandle handle{};
    TqTuningConfig tuning{};
    tuning.RelayIoSize = 4096;
    if (!worker.RegisterRelayForTest(stream, &handle, tuning, TqCompressAlgo::None)) {
        MsQuic = nullptr;
        return false;
    }
    uint8_t data[] = {'x', 'y', 'z'};
    QUIC_BUFFER buffer{};
    buffer.Buffer = data;
    buffer.Length = sizeof(data);
    QUIC_STREAM_EVENT event{};
    event.Type = QUIC_STREAM_EVENT_RECEIVE;
    event.RECEIVE.BufferCount = 1;
    event.RECEIVE.Buffers = &buffer;
    const uint64_t beforeFinds = worker.Snapshot().FindRelayByIdCount;
    const QUIC_STATUS status = TqWindowsRelayWorker::StreamCallback(stream, stream->Context, &event);
    const uint64_t afterFinds = worker.Snapshot().FindRelayByIdCount;
    MsQuic = nullptr;
    return status == QUIC_STATUS_PENDING && afterFinds == beforeFinds;
}
```

在 `main()` 中调用并分配返回码 `51`。

- [x] **Step 2: 运行测试确认失败**

Expected: 当前 receive callback 会查 `Relays_`，测试失败。

- [x] **Step 3: 拆分 receive view 构造和 worker 入队**

新增 callback 侧 helper：

```cpp
std::shared_ptr<TqWindowsPendingQuicReceive> TqWindowsRelayWorker::BuildDeferredQuicReceiveView(
    const CallbackBinding& binding,
    MsQuicStream* stream,
    const QUIC_BUFFER* buffers,
    uint32_t bufferCount,
    bool fin,
    uint64_t completeBatchBytes) {
    auto view = std::make_shared<TqWindowsPendingQuicReceive>();
    view->Stream = stream;
    view->RelayId = binding.RelayId;
    view->Generation = binding.Generation;
    view->Fin = fin;
    view->CompleteBatchBytes = completeBatchBytes;
    for (uint32_t i = 0; i < bufferCount; ++i) {
        const QUIC_BUFFER& buffer = buffers[i];
        if (buffer.Length == 0) {
            continue;
        }
        const size_t offset = view->OwnedBuffer.size();
        view->OwnedBuffer.insert(view->OwnedBuffer.end(), buffer.Buffer, buffer.Buffer + buffer.Length);
        view->Slices.push_back(TqWindowsQuicReceiveSlice{view->OwnedBuffer.data() + offset, buffer.Length});
        view->TotalLength += buffer.Length;
    }
    return view;
}
```

Add `Generation` to `TqWindowsPendingQuicReceive`.

新增 worker 侧 helper：

```cpp
bool TqWindowsRelayWorker::EnqueueDeferredQuicReceiveView(
    const std::shared_ptr<RelayContext>& relay,
    const std::shared_ptr<TqWindowsPendingQuicReceive>& view) {
    if (!relay || !view) {
        return false;
    }
    if (view->TotalLength == 0 && !view->Fin) {
        return true;
    }
    relay->PendingReceives.push_back(view);
    const uint64_t pendingBytes =
        relay->PendingQuicReceiveBytes.fetch_add(view->TotalLength, std::memory_order_relaxed) +
        view->TotalLength;
    const uint64_t pendingDepth =
        relay->PendingQuicReceiveQueueDepth.fetch_add(1, std::memory_order_relaxed) + 1;
    DeferredReceiveQueued_.fetch_add(1, std::memory_order_relaxed);
    DeferredReceiveBytesQueued_.fetch_add(view->TotalLength, std::memory_order_relaxed);
    UpdateAtomicMax(MaxPendingQuicReceiveBytesObserved_, pendingBytes);
    UpdateAtomicMax(MaxPendingQuicReceiveQueueObserved_, pendingDepth);
    return true;
}
```

This task intentionally keeps the `PendingReceiveLock` field until all call sites are converted; do not delete it yet.

- [x] **Step 4: 改写 receive callback**

In `StreamCallback(RECEIVE)`:

```cpp
        const bool fin = (event->RECEIVE.Flags & QUIC_RECEIVE_FLAG_FIN) != 0;
        auto view = worker->BuildDeferredQuicReceiveView(
            *binding,
            stream,
            event->RECEIVE.Buffers,
            event->RECEIVE.BufferCount,
            fin,
            0);
        if (!view) {
            return QUIC_STATUS_SUCCESS;
        }
        if (!worker->PostCallbackOperationById(
                TqWindowsIocpOperationType::RelayReceiveReady,
                *binding,
                0,
                0,
                view)) {
            worker->CompleteRemainingReceiveOwnership(*view);
            return QUIC_STATUS_SUCCESS;
        }
        return QUIC_STATUS_PENDING;
```

在 `EnqueueDeferredQuicReceiveView()` 中设置 relay 相关的 batch 大小：

```cpp
    view->CompleteBatchBytes = relay->Tuning.WindowsRelayQuicReceiveCompleteBatchBytes;
```

- [x] **Step 5: 改写 worker `RelayReceiveReady` handling**

In `Run()`:

```cpp
        case TqWindowsIocpOperationType::RelayReceiveReady: {
            auto relay = op->Relay ? op->Relay : ResolveRelayForCallback(op->RelayId, op->Generation);
            if (!relay) {
                if (op->ReceiveView) {
                    CompleteRemainingReceiveOwnership(*op->ReceiveView);
                }
                break;
            }
            if (op->ReceiveView) {
                if (!EnqueueDeferredQuicReceiveView(relay, op->ReceiveView)) {
                    CompleteRemainingReceiveOwnership(*op->ReceiveView);
                    FailRelayFatal(relay, "quic_receive_queue_failed");
                    break;
                }
            }
            DrainRelayReceives(relay);
            break;
        }
```

- [ ] **Step 6: 移除主路径的 `PendingReceiveLock` 使用**

In `DrainRelayReceives()`:

```cpp
            if (relay->PendingReceives.empty()) {
                return;
            }
            view = relay->PendingReceives.front();
            if (!view) {
                relay->PendingReceives.pop_front();
                continue;
            }
```

In `FinishReceiveView()`:

```cpp
    const auto it = std::find(relay->PendingReceives.begin(), relay->PendingReceives.end(), view);
    if (it == relay->PendingReceives.end()) {
        return view->Drained || view->CompletedLength >= view->TotalLength;
    }
    relay->PendingReceives.erase(it);
```

Keep test helper paths compiling by routing helper calls through worker-only methods or by retaining lock only inside `TQ_UNIT_TESTING` helpers during this task.

- [ ] **Step 7: 运行测试并提交**

Run:

```powershell
cmake --build build-x64 --config Debug --target tcpquic_windows_relay_worker_test
.\build-x64\bin\Debug\tcpquic_windows_relay_worker_test.exe
```

Expected: exit code `0`。

Commit:

```bash
git add src/tunnel/windows_relay_worker.h src/tunnel/windows_relay_worker.cpp src/unittest/windows_relay_worker_test.cpp
git commit -m "feat: enqueue windows quic receives on worker"
```

---

### Task 6: 将 ideal buffer、abort、shutdown callback 改为纯 id/generation 投递

**Files:**
- Modify: `src/tunnel/windows_relay_worker.cpp`
- Test: `src/unittest/windows_relay_worker_test.cpp`

- [x] **Step 1: 添加 callback 顺序测试的 find-count 断言**

在现有 `TestPostedCallbackSequenceForTest` 相关测试后增加：

```cpp
const uint64_t beforeFinds = callbackOrderWorker.Snapshot().FindRelayByIdCount;
// invoke receive, ideal, peer abort, shutdown callback sequence here
const uint64_t afterFinds = callbackOrderWorker.Snapshot().FindRelayByIdCount;
if (afterFinds != beforeFinds) {
    return false;
}
```

Expected: 当前实现失败，因为这些 callback 会查 relay。

- [x] **Step 2: 改写 non-receive callback**

Replace callback posts from relay-pointer form to binding form:

```cpp
PostCallbackOperationById(TqWindowsIocpOperationType::QuicIdealSendBuffer, *binding, byteCount);
PostCallbackOperationById(TqWindowsIocpOperationType::QuicPeerSendAborted, *binding);
PostCallbackOperationById(TqWindowsIocpOperationType::QuicPeerReceiveAborted, *binding);
PostCallbackOperationById(TqWindowsIocpOperationType::QuicShutdownComplete, *binding);
```

Apply the replacement to these event cases:

```cpp
QuicIdealSendBuffer
QuicPeerSendAborted
QuicPeerReceiveAborted
QuicShutdownComplete
```

The `PEER_SEND_SHUTDOWN` event still returns `QUIC_STATUS_SUCCESS` without posting.

- [x] **Step 3: 确认 worker handler 使用 resolver**

For each callback operation case in `Run()`, resolve with:

```cpp
auto relay = op->Relay ? op->Relay : ResolveRelayForCallback(op->RelayId, op->Generation);
if (!relay) {
    break;
}
```

Then call existing handler with `relay->Id` only if the handler still takes id, or refactor handler to accept `relay` directly to avoid a second lookup.

- [ ] **Step 4: 运行测试并提交**

Run:

```powershell
cmake --build build-x64 --config Debug --target tcpquic_windows_relay_worker_test
.\build-x64\bin\Debug\tcpquic_windows_relay_worker_test.exe
```

Expected: exit code `0`。

Commit:

```bash
git add src/tunnel/windows_relay_worker.cpp src/unittest/windows_relay_worker_test.cpp
git commit -m "feat: post windows stream callbacks by id"
```

---

### Task 7: 将 register、stop、snapshot 改为 IOCP control command

**Files:**
- Modify: `src/tunnel/windows_relay_worker.h`
- Modify: `src/tunnel/windows_relay_worker.cpp`
- Test: `src/unittest/windows_relay_worker_test.cpp`

- [x] **Step 1: 添加同步 register control command 测试**

```cpp
bool TestWindowsRelayRegisterRunsOnWorkerForTest() {
    TqWindowsRelayWorker worker;
    if (!worker.Start()) {
        return false;
    }
    TqSocketHandle pair[2]{TqInvalidSocket, TqInvalidSocket};
    if (!TqSocketPair(pair)) {
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
    const bool ok = worker.RegisterRelay(pair[0], stream, nullptr, nullptr, &handle, tuning, TqCompressAlgo::None);
    const TqWindowsRelayWorkerSnapshot snapshot = worker.Snapshot();
    worker.StopRelay(handle.WindowsRelayId);
    TqCloseSocket(pair[1]);
    worker.Stop();
    return ok && handle.Backend == TqRelayBackendType::WindowsWorker &&
           snapshot.ActiveRelays == 1 &&
           snapshot.ActiveRelayStates.size() == 1;
}
```

After implementation, this test still passes, but its purpose is to lock synchronous register semantics before moving the code.

- [x] **Step 2: 增加 operation types and command structs**

In `TqWindowsIocpOperationType` add:

```cpp
RegisterRelay,
Snapshot,
```

Add private structs:

```cpp
struct RegisterRelayCommand;
struct SnapshotCommand;
```

Define in `.cpp`:

```cpp
struct TqWindowsRelayWorker::RegisterRelayCommand {
    TqSocketHandle TcpFd{TqInvalidSocket};
    MsQuicStream* Stream{nullptr};
    ITqCompressor* Compressor{nullptr};
    ITqDecompressor* Decompressor{nullptr};
    TqRelayHandle* Handle{nullptr};
    TqTuningConfig Tuning;
    TqCompressAlgo CompressAlgo{TqCompressAlgo::None};
    bool Ok{false};
    std::mutex Mutex;
    std::condition_variable Cv;
    bool Done{false};
};

struct TqWindowsRelayWorker::SnapshotCommand {
    TqWindowsRelayWorkerSnapshot Result{};
    std::mutex Mutex;
    std::condition_variable Cv;
    bool Done{false};
};
```

Add `void* Control{nullptr};` to `IoOperation`.

- [x] **Step 3: Implement command completion helpers**

Add:

```cpp
template <typename Command>
void CompleteWindowsRelayCommand(Command& command) {
    {
        std::lock_guard<std::mutex> guard(command.Mutex);
        command.Done = true;
    }
    command.Cv.notify_one();
}
```

Add wait helper:

```cpp
template <typename Command>
void WaitWindowsRelayCommand(Command& command) {
    std::unique_lock<std::mutex> guard(command.Mutex);
    command.Cv.wait(guard, [&command] { return command.Done; });
}
```

- [x] **Step 4: Move register body into `RegisterRelayLocal()`**

Add private method:

```cpp
bool RegisterRelayLocal(RegisterRelayCommand& command);
```

Move current `RegisterRelay()` body into local method, replacing parameters with command fields. Keep these assignments:

```cpp
    command.Handle->Backend = TqRelayBackendType::WindowsWorker;
    command.Handle->WindowsWorker = this;
    command.Handle->WindowsRelayId = relay->Id;
    command.Handle->WindowsWorkerIndex = WorkerIndex_;
```

Set:

```cpp
    command.Ok = MaybePostTcpRecv(relay);
    return command.Ok;
```

- [x] **Step 5: Implement public `RegisterRelay()` as synchronous IOCP command**

```cpp
bool TqWindowsRelayWorker::RegisterRelay(
    TqSocketHandle tcpFd,
    MsQuicStream* stream,
    ITqCompressor* compressor,
    ITqDecompressor* decompressor,
    TqRelayHandle* handle,
    const TqTuningConfig& tuning,
    TqCompressAlgo compressAlgo) {
    if (!TqSocketValid(tcpFd) || stream == nullptr || handle == nullptr || Iocp_ == nullptr) {
        return false;
    }
    RegisterRelayCommand command{};
    command.TcpFd = tcpFd;
    command.Stream = stream;
    command.Compressor = compressor;
    command.Decompressor = decompressor;
    command.Handle = handle;
    command.Tuning = tuning;
    command.CompressAlgo = compressAlgo;
    auto op = std::make_unique<IoOperation>();
    op->Event = TqWindowsIocpOperationType::RegisterRelay;
    op->Control = &command;
    IoOperation* raw = op.release();
    if (!::PostQueuedCompletionStatus(static_cast<HANDLE>(Iocp_), 0, 0, &raw->Overlapped)) {
        delete raw;
        return false;
    }
    WaitWindowsRelayCommand(command);
    return command.Ok;
}
```

- [x] **Step 6: Handle register and snapshot operations in `Run()`**

Add cases:

```cpp
        case TqWindowsIocpOperationType::RegisterRelay: {
            auto* command = static_cast<RegisterRelayCommand*>(op->Control);
            if (command != nullptr) {
                command->Ok = RegisterRelayLocal(*command);
                CompleteWindowsRelayCommand(*command);
            }
            break;
        }
        case TqWindowsIocpOperationType::Snapshot: {
            auto* command = static_cast<SnapshotCommand*>(op->Control);
            if (command != nullptr) {
                command->Result = BuildSnapshotLocal();
                CompleteWindowsRelayCommand(*command);
            }
            break;
        }
```

Refactor current `Snapshot()` body into `BuildSnapshotLocal() const`.

- [x] **Step 7: Implement snapshot control command**

Public `Snapshot()`:

```cpp
TqWindowsRelayWorkerSnapshot TqWindowsRelayWorker::Snapshot() const {
    if (Iocp_ == nullptr || Stopping_.load(std::memory_order_acquire)) {
        return BuildSnapshotLocal();
    }
    SnapshotCommand command{};
    auto op = std::make_unique<IoOperation>();
    op->Event = TqWindowsIocpOperationType::Snapshot;
    op->Control = &command;
    IoOperation* raw = op.release();
    if (!::PostQueuedCompletionStatus(static_cast<HANDLE>(Iocp_), 0, 0, &raw->Overlapped)) {
        delete raw;
        return BuildSnapshotLocal();
    }
    WaitWindowsRelayCommand(command);
    return command.Result;
}
```

If `Snapshot()` is called before `Start()`, `BuildSnapshotLocal()` must return counters without accessing IOCP.

- [x] **Step 8: Update StopRelay to avoid external map lookup**

Change `StopRelay()` to post by id:

```cpp
void TqWindowsRelayWorker::StopRelay(uint64_t relayId) {
    if (relayId == 0 || Iocp_ == nullptr) {
        return;
    }
    auto op = std::make_unique<IoOperation>();
    op->Event = TqWindowsIocpOperationType::CloseRelay;
    op->RelayId = relayId;
    IoOperation* raw = op.release();
    if (!::PostQueuedCompletionStatus(static_cast<HANDLE>(Iocp_), 0, 0, &raw->Overlapped)) {
        delete raw;
        Errors_.fetch_add(1, std::memory_order_relaxed);
    }
}
```

In `Run()` `CloseRelay` case:

```cpp
            if (!op->Relay && op->RelayId != 0) {
                op->Relay = FindRelayById(op->RelayId);
            }
            CloseRelay(op->Relay, TqRelayCloseMode::GracefulDrain, nullptr);
```

Stage 7 still uses `FindRelayById()` inside worker; Task 8 removes the lock dependency.

- [ ] **Step 9: 运行测试并提交**

Run:

```powershell
cmake --build build-x64 --config Debug --target tcpquic_windows_relay_worker_test
.\build-x64\bin\Debug\tcpquic_windows_relay_worker_test.exe
```

Expected: exit code `0`。

Commit:

```bash
git add src/tunnel/windows_relay_worker.h src/tunnel/windows_relay_worker.cpp src/unittest/windows_relay_worker_test.cpp
git commit -m "feat: eventize windows relay control operations"
```

---

### Task 8: 收敛 worker-owned 容器并移除热路径 mutex

**Files:**
- Modify: `src/tunnel/windows_relay_worker.h`
- Modify: `src/tunnel/windows_relay_worker.cpp`
- Modify: `src/unittest/windows_relay_worker_test.cpp`

- [x] **Step 1: Add worker-local relay lookup**

Add private helper:

```cpp
std::shared_ptr<RelayContext> FindRelayByIdLocal(uint64_t relayId) const;
```

Implementation:

```cpp
std::shared_ptr<TqWindowsRelayWorker::RelayContext> TqWindowsRelayWorker::FindRelayByIdLocal(
    uint64_t relayId) const {
    const auto it = Relays_.find(relayId);
    return it != Relays_.end() ? it->second : nullptr;
}
```

Keep old `FindRelayById()` only for pre-start tests or transition helpers; production worker paths should call `FindRelayByIdLocal()` from `Run()`.

- [x] **Step 2: Convert worker paths to local lookup**

Replace worker-thread call sites:

```cpp
FindRelayById(op->RelayId)
```

with:

```cpp
FindRelayByIdLocal(op->RelayId)
```

for IOCP operation handling, maintenance, register local, retire, and snapshot local.

Keep external test helper calls on `FindRelayById()` until helper calls are eventized.

- [x] **Step 3: Remove `PendingReceiveLock` production usage**

Delete lock guards around `PendingReceives` in production methods:

- `DrainRelayReceives`
- `FinishReceiveView`
- `CompleteAllPendingQuicReceives`
- `HasPendingRelayDrainWork`
- `HasPendingAfterStreamShutdown`

Use direct queue access because all of these are now worker-thread-only or invoked through IOCP control/callback operation.

- [x] **Step 4: Remove `TcpRecvOpsLock` production usage**

In `PostTcpRecv()`, `HandleTcpRecv()`, and `HandleTcpReadClosed()`, replace locked freelist access with direct access:

```cpp
    if (!relay->TcpRecvOpsFree.empty()) {
        op = std::move(relay->TcpRecvOpsFree.back());
        relay->TcpRecvOpsFree.pop_back();
        TcpRecvOperationsReused_.fetch_add(1, std::memory_order_relaxed);
    }
```

And direct push:

```cpp
    op->BufferOwner.reset();
    op->Buffer.clear();
    op->Relay.reset();
    relay->TcpRecvOpsFree.push_back(std::move(op));
```

- [x] **Step 5: Remove `PendingQuicSendLock` production usage**

In `TrySubmitQuicSendOperation()`, `RetryPendingQuicSends()`, `FinalizeQuicSendAccountingOnClose()`, and `BuildRelayTraceState()`, use direct queue access on worker thread:

```cpp
relay->PendingQuicSendRetries.emplace_back(operation);
```

```cpp
operation = std::move(relay->PendingQuicSendRetries.front());
relay->PendingQuicSendRetries.pop_front();
```

For trace/snapshot from non-worker paths, use the eventized snapshot path so direct queue access remains worker-owned.

- [ ] **Step 6: Delete mutex members after all production references are gone**

From `RelayContext`, remove:

```cpp
std::mutex TcpRecvOpsLock;
std::mutex PendingReceiveLock;
std::mutex PendingQuicSendLock;
```

Keep `CallbackPendingQuicReceiveLock` only if tests or compatibility paths still use `CallbackPendingQuicReceives`; if no references remain except snapshot/test compatibility, remove the queue and lock together.

- [ ] **Step 7: Compile to catch stale lock references**

Run:

```powershell
cmake --build build-x64 --config Debug --target tcpquic_windows_relay_worker_test
```

Expected: compile succeeds. If it fails, every error should point to a remaining production/test reference to a removed mutex; route that path through worker command or remove the stale helper.

- [ ] **Step 8: Run unit and loopback validation**

Run:

```powershell
.\build-x64\bin\Debug\tcpquic_windows_relay_worker_test.exe
cmake --build build-x64 --config Release --target tcpquic-proxy
.\scripts\test-tcpquic-proxy-windows.ps1 -Compress off
.\scripts\test-tcpquic-proxy-windows.ps1 -Compress zstd
```

Expected: all commands exit `0`.

- [ ] **Step 9: Commit**

```bash
git add src/tunnel/windows_relay_worker.h src/tunnel/windows_relay_worker.cpp src/unittest/windows_relay_worker_test.cpp
git commit -m "perf: make windows relay queues worker owned"
```

---

## Final Verification

- [ ] Run Windows worker unit test:

```powershell
cmake --build build-x64 --config Debug --target tcpquic_windows_relay_worker_test
.\build-x64\bin\Debug\tcpquic_windows_relay_worker_test.exe
```

- [ ] Run Windows relay integration validation:

```powershell
cmake --build build-x64 --config Release --target tcpquic-proxy
.\scripts\test-tcpquic-proxy-windows.ps1 -Compress off
.\scripts\test-tcpquic-proxy-windows.ps1 -Compress zstd
```

- [ ] Capture admin metrics before and after a multi-connection callback-heavy run and compare:

```powershell
curl.exe -s http://127.0.0.1:<admin-port>/api/v1/relay/metrics
```

Expected directional changes:

- `windows_relay_find_relay_by_id_count` no longer increases for MsQuic callback-only traffic after Task 6.
- `windows_relay_worker_lock_wait_nanos` grows slower under multi-connection callback pressure.
- `windows_relay_callback_iocp_post_failures` remains `0`.
- `windows_relay_posted_callback_stale_drops` only increases in tests that intentionally close relay before late callbacks.

- [ ] Final commit if verification fixes were needed:

```bash
git status --short
git add src/tunnel/windows_relay_worker.h src/tunnel/windows_relay_worker.cpp src/runtime/relay_metrics.h src/runtime/relay_metrics.cpp src/unittest/windows_relay_worker_test.cpp
git commit -m "test: validate windows relay eventized locking"
```

## Rollback Plan

- If Task 1 or Task 2 introduces regressions, revert the corresponding metrics/snapshot commit; no data-plane semantics should have changed.
- If Task 4 send complete pure post regresses, revert only that commit; receive path remains unchanged.
- If Task 5 receive pure post regresses, revert Task 5 and keep generation infrastructure from Task 3.
- If Task 7 control command regresses under stop/register races, revert Task 7 and keep callback pure-post improvements.
- If Task 8 queue lock removal regresses, revert Task 8; prior tasks still reduce worker `Lock_` contention.
