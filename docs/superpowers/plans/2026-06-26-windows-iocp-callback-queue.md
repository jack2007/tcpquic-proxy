# Windows IOCP Callback Queue Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 将 Windows relay 的所有 MsQuic callback 事件一次性迁移到 IOCP posted operation，并把 QUIC receive drain 从 view 粒度改为 relay 粒度。

**Architecture:** IOCP 成为 Windows relay 唯一跨线程 worker dispatch 队列。RECEIVE callback 每次投递不携带 view 指针的 `RelayReceiveReady(relayId)` 顺序标记；worker 内部在 TCP send completion 或 receive view finish 后使用可合并的 `RelayReceiveDrain(relayId)` continuation。`relay->PendingReceives` 继续作为唯一持有 QUIC receive view 的队列。

**Tech Stack:** C++17, Windows IOCP, Winsock overlapped I/O, MsQuic stream callbacks, CMake, existing `src/unittest/windows_relay_worker_test.cpp` test binary.

---

## 文件结构

- Modify: `src/tunnel/windows_relay_worker.cpp`
  - 扩展 `TqWindowsIocpOperationType`。
  - 新增 IOCP posted callback helper。
  - 新增 `DrainRelayReceives()`、`PostCallbackOperation()`、`ScheduleRelayReceiveDrain()`。
  - 修改 `Run()` 的 posted operation dispatch。
  - 修改 `StreamCallback()`，所有 callback event 都走 IOCP。
  - 修改 receive completion 续跑逻辑，不再 enqueue view task。
- Modify: `src/tunnel/windows_relay_worker.h`
  - 删除或停止生产路径使用 `TqWindowsRelayEventQueue`。
  - 新增测试 helper 声明。
  - 更新 snapshot 字段命名。
- Modify: `src/tunnel/windows_relay_event_queue.h`
  - 如果测试仍需要队列单元测试，保留文件和 test-only 使用；如果生产 include 已移除但测试仍引用，保持原文件不动。
- Modify: `src/unittest/windows_relay_worker_test.cpp`
  - 将旧 EventQueue/QuicReceiveView task 相关测试迁移为 IOCP posted callback operation 和 receive drain 行为测试。
  - 新增 callback 顺序测试。
- Modify: `src/runtime/relay_metrics.cpp`
  - 更新 Windows callback IOCP post 指标输出。
- Modify: `src/runtime/relay_metrics.h`
  - 更新 metric snapshot 字段。
- Modify: `src/runtime/trace.cpp`
  - 更新 active relay trace 中 event queue 字段命名或语义。
- Modify: `src/docs/thread_model_cn.md`
  - 更新 Windows QUIC -> TCP callback/worker 流程描述。
- Modify: `docs/iocp-quic2tcp-queue_cn.md`
  - 增加“已采用设计/后续实现方向”说明。

## 验证命令

Windows 环境：

```powershell
cmake --build build --target tcpquic_windows_relay_worker_test --config Release
.\build\bin\Release\tcpquic_windows_relay_worker_test.exe
cmake --build build --target tcpquic_tunnel_test --config Release
.\build\bin\Release\tcpquic_tunnel_test.exe
```

Linux 当前工作机只能做非 Windows 目标和静态检查：

```bash
rtk cmake --build build-glibc --target tcpquic_production_linkage_guard_test
rtk ./build-glibc/bin/Release/tcpquic_production_linkage_guard_test
rtk git diff --check
```

---

### Task 1: 建立 IOCP posted callback operation 类型和测试入口

**Files:**
- Modify: `src/tunnel/windows_relay_worker.cpp`
- Modify: `src/tunnel/windows_relay_worker.h`
- Modify: `src/unittest/windows_relay_worker_test.cpp`

- [ ] **Step 1: 写 failing test，验证 receive callback posted operation 不携带 view 指针**

在 `src/unittest/windows_relay_worker_test.cpp` 末尾现有 Windows relay worker 测试 block 中新增一个独立测试段。测试先通过 callback queue 一个 receive，然后调用新的 test helper 检查最后投递的 callback operation 类型和 receive-view 指针状态。

```cpp
{
    QUIC_API_TABLE fakeApi{};
    fakeApi.StreamReceiveComplete = FakeStreamReceiveComplete;
    fakeApi.StreamReceiveSetEnabled = FakeStreamReceiveSetEnabled;
    MsQuic = reinterpret_cast<const MsQuicApi*>(&fakeApi);

    TqWindowsRelayWorker receiveWorker;
    alignas(MsQuicStream) unsigned char streamStorage[sizeof(MsQuicStream)]{};
    auto* stream = reinterpret_cast<MsQuicStream*>(streamStorage);
    stream->Callback = MsQuicStream::NoOpCallback;
    stream->Context = nullptr;
    stream->Handle = reinterpret_cast<HQUIC>(static_cast<uintptr_t>(1));

    TqRelayHandle handle{};
    TqTuningConfig tuning{};
    tuning.RelayIoSize = 64 * 1024;
    if (!receiveWorker.RegisterRelayForTest(stream, &handle, tuning, TqCompressAlgo::None)) {
        MsQuic = nullptr;
        return 130;
    }
    receiveWorker.SetQuicReceiveViewDrainEnabledForTest(false);
    if (!receiveWorker.Start()) {
        MsQuic = nullptr;
        return 133;
    }

    uint8_t data[] = {'r', 'e', 'a', 'd', 'y'};
    QUIC_BUFFER buffer{};
    buffer.Buffer = data;
    buffer.Length = sizeof(data);
    QUIC_STREAM_EVENT event{};
    event.Type = QUIC_STREAM_EVENT_RECEIVE;
    event.RECEIVE.BufferCount = 1;
    event.RECEIVE.Buffers = &buffer;

    if (TqWindowsRelayWorker::StreamCallback(stream, stream->Context, &event) != QUIC_STATUS_PENDING) {
        MsQuic = nullptr;
        return 131;
    }
    if (!receiveWorker.TestLastPostedCallbackWasReceiveReadyForTest(handle.WindowsRelayId)) {
        receiveWorker.Stop();
        MsQuic = nullptr;
        return 132;
    }
    receiveWorker.Stop();
    MsQuic = nullptr;
}
```

- [ ] **Step 2: 声明 test helper，让测试先编译失败**

在 `src/tunnel/windows_relay_worker.h` 的 `#if defined(TQ_UNIT_TESTING)` public 区域加入声明：

```cpp
bool TestLastPostedCallbackWasReceiveReadyForTest(uint64_t relayId) const;
```

- [ ] **Step 3: 运行测试并确认失败**

Run:

```powershell
cmake --build build --target tcpquic_windows_relay_worker_test --config Release
```

Expected: FAIL，错误包含 `TestLastPostedCallbackWasReceiveReadyForTest` 未定义，或 helper 还没有实现。

- [ ] **Step 4: 将 IOCP operation 类型移到 header 并扩展类型**

把 `src/tunnel/windows_relay_worker.cpp` 中现有 `enum class TqWindowsIocpOperationType` 移到 `src/tunnel/windows_relay_worker.h`，放在 `struct TqWindowsPendingQuicReceive;` 后面。移动后从 `.cpp` 删除原 enum 定义，避免重复定义。移动后的定义包含 callback operation：

```cpp
enum class TqWindowsIocpOperationType : uint32_t {
    TcpRecv,
    TcpSend,
    QuicReceiveQueued,
    QuicReceiveViewQueued,
    QuicSendComplete,
    QuicSendRetry,
    CloseRelay,
    StopWorker,
    RelayReceiveReady,
    RelayReceiveDrain,
    QuicIdealSendBuffer,
    QuicPeerSendAborted,
    QuicPeerReceiveAborted,
    QuicShutdownComplete,
};
```

这样 private helper、test-only 记录字段和 `.cpp` 实现都使用同一个类型。

在 `IoOperation` 中加入 callback payload 字段：

```cpp
uint64_t RelayId{0};
uint64_t Value{0};
size_t Length{0};
```

在 `TqWindowsRelayWorker` private 字段区加入 test-only 记录：

```cpp
#if defined(TQ_UNIT_TESTING)
    mutable std::mutex LastPostedCallbackLock_;
    TqWindowsIocpOperationType LastPostedCallbackType_{TqWindowsIocpOperationType::TcpRecv};
    uint64_t LastPostedCallbackRelayId_{0};
    bool LastPostedCallbackHadReceiveView_{false};
#endif
```

- [ ] **Step 5: 实现 `PostCallbackOperation()` skeleton 和 helper**

在 `src/tunnel/windows_relay_worker.h` private 区声明：

```cpp
bool PostCallbackOperation(
    TqWindowsIocpOperationType type,
    const std::shared_ptr<RelayContext>& relay,
    uint64_t value = 0,
    size_t length = 0);
```

在 `src/tunnel/windows_relay_worker.cpp` 中实现：

```cpp
bool TqWindowsRelayWorker::PostCallbackOperation(
    TqWindowsIocpOperationType type,
    const std::shared_ptr<RelayContext>& relay,
    uint64_t value,
    size_t length) {
    if (Iocp_ == nullptr || !relay) {
        EventQueueWakeFailedCount_.fetch_add(1, std::memory_order_relaxed);
        return false;
    }
    auto op = std::make_unique<IoOperation>();
    op->Event = type;
    op->Relay = relay;
    op->RelayId = relay->Id;
    op->Value = value;
    op->Length = length;
    IoOperation* raw = op.release();
    relay->QueuedWorkerOps.fetch_add(1, std::memory_order_acq_rel);
#if defined(TQ_UNIT_TESTING)
    {
        std::lock_guard<std::mutex> guard(LastPostedCallbackLock_);
        LastPostedCallbackType_ = type;
        LastPostedCallbackRelayId_ = raw->RelayId;
        LastPostedCallbackHadReceiveView_ = raw->ReceiveView != nullptr;
    }
#endif
    if (!::PostQueuedCompletionStatus(static_cast<HANDLE>(Iocp_), 0, 0, &raw->Overlapped)) {
        relay->QueuedWorkerOps.fetch_sub(1, std::memory_order_acq_rel);
        delete raw;
        EventQueueWakeFailedCount_.fetch_add(1, std::memory_order_relaxed);
        return false;
    }
    EventQueueWakeCount_.fetch_add(1, std::memory_order_relaxed);
    return true;
}
```

在 test-only 区实现 helper：

```cpp
bool TqWindowsRelayWorker::TestLastPostedCallbackWasReceiveReadyForTest(uint64_t relayId) const {
    std::lock_guard<std::mutex> guard(LastPostedCallbackLock_);
    return LastPostedCallbackType_ == TqWindowsIocpOperationType::RelayReceiveReady &&
        LastPostedCallbackRelayId_ == relayId &&
        !LastPostedCallbackHadReceiveView_;
}
```

- [ ] **Step 6: 运行测试并确认当前仍失败于 callback 未迁移**

Run:

```powershell
cmake --build build --target tcpquic_windows_relay_worker_test --config Release
.\build\bin\Release\tcpquic_windows_relay_worker_test.exe
```

Expected: FAIL，新增测试返回 `132`，因为 RECEIVE callback 仍走 `EnqueueEvent(QuicReceiveView)`。

- [ ] **Step 7: Commit**

```bash
git add src/tunnel/windows_relay_worker.cpp src/tunnel/windows_relay_worker.h src/unittest/windows_relay_worker_test.cpp
git commit -m "test(windows): cover IOCP receive-ready callback marker"
```

---

### Task 2: 迁移 RECEIVE callback 到 `RelayReceiveReady`

**Files:**
- Modify: `src/tunnel/windows_relay_worker.cpp`
- Modify: `src/tunnel/windows_relay_worker.h`
- Test: `src/unittest/windows_relay_worker_test.cpp`

- [ ] **Step 1: 为 relay 增加 receive continuation coalescing 状态**

在 `RelayContext` 中加入：

```cpp
std::atomic<bool> ReceiveDrainQueued{false};
```

- [ ] **Step 2: 声明 receive drain 方法**

在 `src/tunnel/windows_relay_worker.h` private 区加入：

```cpp
bool ScheduleRelayReceiveDrain(const std::shared_ptr<RelayContext>& relay);
void DrainRelayReceives(const std::shared_ptr<RelayContext>& relay);
```

- [ ] **Step 3: 实现 `DrainRelayReceives()`**

把当前 `ProcessQuicReceiveViewTask()` 中的“找 relay、检查 closing、锁 PendingReceives、只处理 front、调用 PostTcpSend...”逻辑改为 relay 驱动：

```cpp
void TqWindowsRelayWorker::DrainRelayReceives(const std::shared_ptr<RelayContext>& relay) {
    if (!relay) {
        return;
    }
    for (;;) {
        std::shared_ptr<TqWindowsPendingQuicReceive> view;
        {
            std::lock_guard<std::mutex> guard(relay->PendingReceiveLock);
            if (relay->PendingReceives.empty()) {
                return;
            }
            view = relay->PendingReceives.front();
            if (!view) {
                relay->PendingReceives.pop_front();
                continue;
            }
            if (view->TcpSendPending) {
                TraceReceiveViewEvent(relay, view, "drain_receive_send_pending");
                return;
            }
        }

        if (relay->Closing.load(std::memory_order_acquire)) {
            TraceReceiveViewEvent(relay, view, "drain_receive_closing");
            (void)CompletePendingQuicReceive(relay, view);
            continue;
        }

        TraceReceiveViewEvent(relay, view, "drain_receive_front");
#if defined(TQ_UNIT_TESTING)
        if (!QuicReceiveViewDrainEnabledForTest_.load(std::memory_order_relaxed)) {
            return;
        }
#endif
        const bool posted = relay->Decompressor != nullptr && relay->CompressAlgo == TqCompressAlgo::Zstd
            ? PostTcpSendFromCompressedReceiveView(relay, view)
            : PostTcpSendFromReceiveView(relay, view);
        if (!posted) {
            (void)CompletePendingQuicReceive(relay, view);
            CloseRelay(relay, TqRelayCloseMode::GracefulDrain, "post_tcp_send_receive_drain_failed");
        }
        return;
    }
}
```

- [ ] **Step 4: 实现 `ScheduleRelayReceiveDrain()`**

```cpp
bool TqWindowsRelayWorker::ScheduleRelayReceiveDrain(const std::shared_ptr<RelayContext>& relay) {
    if (Iocp_ == nullptr || !relay || relay->Closing.load(std::memory_order_acquire)) {
        return false;
    }
    if (relay->ReceiveDrainQueued.exchange(true, std::memory_order_acq_rel)) {
        return true;
    }
    auto op = std::make_unique<IoOperation>();
    op->Event = TqWindowsIocpOperationType::RelayReceiveDrain;
    op->Relay = relay;
    op->RelayId = relay->Id;
    IoOperation* raw = op.release();
    relay->QueuedWorkerOps.fetch_add(1, std::memory_order_acq_rel);
    TraceReceiveViewEvent(relay, nullptr, "schedule_receive_drain");
    if (!::PostQueuedCompletionStatus(static_cast<HANDLE>(Iocp_), 0, 0, &raw->Overlapped)) {
        relay->QueuedWorkerOps.fetch_sub(1, std::memory_order_acq_rel);
        relay->ReceiveDrainQueued.store(false, std::memory_order_release);
        delete raw;
        EventQueueWakeFailedCount_.fetch_add(1, std::memory_order_relaxed);
        return false;
    }
    EventQueueWakeCount_.fetch_add(1, std::memory_order_relaxed);
    return true;
}
```

- [ ] **Step 5: 修改 `Run()` dispatch**

在 `Run()` switch 中添加：

```cpp
case TqWindowsIocpOperationType::RelayReceiveReady:
    DrainRelayReceives(op->Relay);
    break;
case TqWindowsIocpOperationType::RelayReceiveDrain:
    if (op->Relay) {
        op->Relay->ReceiveDrainQueued.store(false, std::memory_order_release);
    }
    DrainRelayReceives(op->Relay);
    break;
```

- [ ] **Step 6: 修改 `QueueDeferredQuicReceive()`**

删除创建 `TqWindowsRelayTask` 的 block，替换为：

```cpp
TraceReceiveViewEvent(relay, view, "post_receive_ready", pendingDepth);
if (!PostCallbackOperation(TqWindowsIocpOperationType::RelayReceiveReady, relay)) {
    TraceReceiveViewEvent(relay, view, "queue_receive_callback_pending", pendingDepth);
    if (!relay->QuicReceivePaused.exchange(true, std::memory_order_acq_rel)) {
        SetQuicReceiveEnabled(relay, false);
    }
    (void)CompletePendingQuicReceive(relay, view);
    CloseRelay(relay, TqRelayCloseMode::GracefulDrain, "post_receive_ready_failed");
    return false;
}
```

- [ ] **Step 7: 运行测试并确认 Task 1 新测试通过**

Run:

```powershell
cmake --build build --target tcpquic_windows_relay_worker_test --config Release
.\build\bin\Release\tcpquic_windows_relay_worker_test.exe
```

Expected: 进程退出码 `0`，或至少不再返回 `132`。

- [ ] **Step 8: Commit**

```bash
git add src/tunnel/windows_relay_worker.cpp src/tunnel/windows_relay_worker.h
git commit -m "feat(windows): post receive callbacks through IOCP"
```

---

### Task 3: 移除 view 粒度 next-task 调度

**Files:**
- Modify: `src/tunnel/windows_relay_worker.cpp`
- Test: `src/unittest/windows_relay_worker_test.cpp`

- [ ] **Step 1: 写 failing test，验证 A 完成后不会产生 view-specific task**

在 `src/unittest/windows_relay_worker_test.cpp` 新增 test-only helper 使用场景：queue 两个 receive，禁用 drain，手动完成 A，检查 pending queue 还有 B 且没有 `QuicReceiveView` worker task 被投递。

```cpp
{
    QUIC_API_TABLE fakeApi{};
    fakeApi.StreamReceiveComplete = FakeStreamReceiveComplete;
    fakeApi.StreamReceiveSetEnabled = FakeStreamReceiveSetEnabled;
    MsQuic = reinterpret_cast<const MsQuicApi*>(&fakeApi);

    TqWindowsRelayWorker receiveWorker;
    alignas(MsQuicStream) unsigned char streamStorage[sizeof(MsQuicStream)]{};
    auto* stream = reinterpret_cast<MsQuicStream*>(streamStorage);
    stream->Callback = MsQuicStream::NoOpCallback;
    stream->Context = nullptr;
    stream->Handle = reinterpret_cast<HQUIC>(static_cast<uintptr_t>(1));
    TqRelayHandle handle{};
    TqTuningConfig tuning{};
    tuning.RelayIoSize = 64 * 1024;
    if (!receiveWorker.RegisterRelayForTest(stream, &handle, tuning, TqCompressAlgo::None)) {
        MsQuic = nullptr;
        return 133;
    }
    receiveWorker.SetQuicReceiveViewDrainEnabledForTest(false);

    uint8_t first[] = {'a'};
    uint8_t second[] = {'b'};
    QUIC_BUFFER firstBuffer{sizeof(first), first};
    QUIC_BUFFER secondBuffer{sizeof(second), second};
    QUIC_STREAM_EVENT event{};
    event.Type = QUIC_STREAM_EVENT_RECEIVE;
    event.RECEIVE.BufferCount = 1;

    event.RECEIVE.Buffers = &firstBuffer;
    if (TqWindowsRelayWorker::StreamCallback(stream, stream->Context, &event) != QUIC_STATUS_PENDING) {
        MsQuic = nullptr;
        return 134;
    }
    event.RECEIVE.Buffers = &secondBuffer;
    if (TqWindowsRelayWorker::StreamCallback(stream, stream->Context, &event) != QUIC_STATUS_PENDING) {
        MsQuic = nullptr;
        return 135;
    }
    if (!receiveWorker.TestAdvanceReceiveViewForCompletion(handle.WindowsRelayId, 1)) {
        MsQuic = nullptr;
        return 136;
    }
    if (!receiveWorker.TestNoWorkerEventQueueReceiveViewForTest()) {
        MsQuic = nullptr;
        return 137;
    }
    MsQuic = nullptr;
}
```

- [ ] **Step 2: 声明并实现 `TestNoWorkerEventQueueReceiveViewForTest()`**

Header:

```cpp
bool TestNoWorkerEventQueueReceiveViewForTest() const;
```

Implementation:

```cpp
bool TqWindowsRelayWorker::TestNoWorkerEventQueueReceiveViewForTest() const {
    return EventQueue_.SizeApprox() == 0;
}
```

- [ ] **Step 3: 运行测试确认失败**

Run:

```powershell
cmake --build build --target tcpquic_windows_relay_worker_test --config Release
.\build\bin\Release\tcpquic_windows_relay_worker_test.exe
```

Expected: FAIL，旧 `FinishReceiveView()` 仍会 enqueue `QuicReceiveView` task，测试返回 `137`。

- [ ] **Step 4: 修改 `FinishReceiveView()`**

删除 `nextView` 和 `EnqueueEvent(QuicReceiveView)` block，替换为：

```cpp
bool hasMoreReceives = false;
{
    std::lock_guard<std::mutex> guard(relay->PendingReceiveLock);
    hasMoreReceives = !relay->PendingReceives.empty();
}
if (hasMoreReceives && !relay->Closing.load(std::memory_order_acquire)) {
    (void)ScheduleRelayReceiveDrain(relay);
}
```

- [ ] **Step 5: 修改 `HandleTcpSend()` receive-view 完成路径**

在 full completion 后，`PostTcpSendFromReceiveView()` 自身可能 finish 当前 view。保留现有推进逻辑，但在 return 前保证触发 relay drain continuation：

```cpp
if (!PostTcpSendFromReceiveView(relay, view)) {
    CloseRelay(relay, TqRelayCloseMode::GracefulDrain, "post_tcp_send_receive_view_failed");
}
(void)ScheduleRelayReceiveDrain(relay);
return;
```

压缩路径同样在 `PostTcpSendFromCompressedReceiveView()` 后调用：

```cpp
if (!PostTcpSendFromCompressedReceiveView(relay, view)) {
    CloseRelay(relay, TqRelayCloseMode::GracefulDrain, "post_tcp_send_compressed_receive_view_failed");
}
(void)ScheduleRelayReceiveDrain(relay);
return;
```

- [ ] **Step 6: 运行测试确认通过**

Run:

```powershell
cmake --build build --target tcpquic_windows_relay_worker_test --config Release
.\build\bin\Release\tcpquic_windows_relay_worker_test.exe
```

Expected: PASS，退出码 `0`。

- [ ] **Step 7: Commit**

```bash
git add src/tunnel/windows_relay_worker.cpp src/tunnel/windows_relay_worker.h src/unittest/windows_relay_worker_test.cpp
git commit -m "fix(windows): drain receive queue by relay"
```

---

### Task 4: 迁移 send-complete callback 到 IOCP

**Files:**
- Modify: `src/tunnel/windows_relay_worker.cpp`
- Modify: `src/tunnel/windows_relay_worker.h`
- Test: `src/unittest/windows_relay_worker_test.cpp`

- [ ] **Step 1: 写 failing test，验证 SEND_COMPLETE 使用 IOCP posted op**

在 `src/unittest/windows_relay_worker_test.cpp` 新增：

```cpp
{
    TqWindowsRelayWorker worker;
    alignas(MsQuicStream) unsigned char streamStorage[sizeof(MsQuicStream)]{};
    auto* stream = reinterpret_cast<MsQuicStream*>(streamStorage);
    stream->Callback = MsQuicStream::NoOpCallback;
    stream->Context = nullptr;
    TqRelayHandle handle{};
    TqTuningConfig tuning{};
    if (!worker.RegisterRelayForTest(stream, &handle, tuning, TqCompressAlgo::None)) {
        return 138;
    }
    if (!worker.Start()) {
        return 143;
    }
    auto* operation = worker.TestCreateQuicSendOperationForTest(handle.WindowsRelayId, 7);
    QUIC_STREAM_EVENT complete{};
    complete.Type = QUIC_STREAM_EVENT_SEND_COMPLETE;
    complete.SEND_COMPLETE.ClientContext = operation;
    if (TqWindowsRelayWorker::StreamCallback(stream, stream->Context, &complete) != QUIC_STATUS_SUCCESS) {
        return 139;
    }
    if (!worker.TestLastPostedCallbackWasQuicSendCompleteForTest(handle.WindowsRelayId)) {
        worker.Stop();
        return 140;
    }
    worker.Stop();
}
```

- [ ] **Step 2: 使用 test helper 隐藏 operation 类型细节**

测试不直接比较 `TqWindowsIocpOperationType`，改为在 header 暴露 test helper：

```cpp
bool TestLastPostedCallbackWasQuicSendCompleteForTest(uint64_t relayId) const;
TqWindowsQuicSendOperation* TestCreateQuicSendOperationForTest(uint64_t relayId, uint64_t bytes);
```

测试使用：

```cpp
if (!worker.TestLastPostedCallbackWasQuicSendCompleteForTest(handle.WindowsRelayId)) {
    return 140;
}
```

- [ ] **Step 3: 实现 test helpers**

```cpp
bool TqWindowsRelayWorker::TestLastPostedCallbackWasQuicSendCompleteForTest(uint64_t relayId) const {
    std::lock_guard<std::mutex> guard(LastPostedCallbackLock_);
    return LastPostedCallbackType_ == TqWindowsIocpOperationType::QuicSendComplete &&
        LastPostedCallbackRelayId_ == relayId;
}

TqWindowsQuicSendOperation* TqWindowsRelayWorker::TestCreateQuicSendOperationForTest(
    uint64_t relayId,
    uint64_t bytes) {
    auto relay = FindRelayById(relayId);
    if (relay) {
        relay->InFlightQuicSends.fetch_add(1, std::memory_order_acq_rel);
        relay->OutstandingQuicSendBytes.fetch_add(bytes, std::memory_order_acq_rel);
    }
    auto* operation = new TqWindowsQuicSendOperation();
    operation->RelayId = relayId;
    operation->TotalBytes = bytes;
    return operation;
}
```

- [ ] **Step 4: 运行测试确认失败**

Run:

```powershell
cmake --build build --target tcpquic_windows_relay_worker_test --config Release
.\build\bin\Release\tcpquic_windows_relay_worker_test.exe
```

Expected: FAIL，SEND_COMPLETE 仍走 `QueueQuicSendCompleteFromCallback()` 和 `EventQueue_`。

- [ ] **Step 5: 修改 `QueueQuicSendCompleteFromCallback()`**

把 `TqWindowsRelayTask` enqueue 替换为 relay 查找和 IOCP post：

```cpp
void TqWindowsRelayWorker::QueueQuicSendCompleteFromCallback(
    TqWindowsQuicSendOperation* operation) {
    if (operation == nullptr) {
        return;
    }
    auto relay = FindRelayById(operation->RelayId);
    if (!relay) {
        std::unique_ptr<TqWindowsQuicSendOperation> cleanup(operation);
        return;
    }
    if (!PostCallbackOperation(
            TqWindowsIocpOperationType::QuicSendComplete,
            relay,
            reinterpret_cast<uintptr_t>(operation))) {
        std::unique_ptr<TqWindowsQuicSendOperation> cleanup(operation);
        FailRelayFatal(relay, "post_quic_send_complete_failed");
    }
}
```

- [ ] **Step 6: 修改 `Run()` dispatch**

添加：

```cpp
case TqWindowsIocpOperationType::QuicSendComplete: {
    TqWindowsRelayTask task{};
    task.Type = TqWindowsRelayTaskType::QuicSendComplete;
    task.RelayId = op->RelayId;
    task.Value = op->Value;
    op->Value = 0;
    ProcessQuicSendCompleteTask(task);
    break;
}
```

这个桥接只在迁移过程中复用现有处理逻辑。后续任务会移除 `TqWindowsRelayTask`。

- [ ] **Step 7: 运行测试确认通过**

Run:

```powershell
cmake --build build --target tcpquic_windows_relay_worker_test --config Release
.\build\bin\Release\tcpquic_windows_relay_worker_test.exe
```

Expected: PASS。

- [ ] **Step 8: Commit**

```bash
git add src/tunnel/windows_relay_worker.cpp src/tunnel/windows_relay_worker.h src/unittest/windows_relay_worker_test.cpp
git commit -m "feat(windows): post QUIC send completions through IOCP"
```

---

### Task 5: 迁移 ideal-buffer、peer-abort、shutdown-complete callback 到 IOCP

**Files:**
- Modify: `src/tunnel/windows_relay_worker.cpp`
- Modify: `src/tunnel/windows_relay_worker.h`
- Test: `src/unittest/windows_relay_worker_test.cpp`

- [ ] **Step 1: 写 callback event 顺序测试**

新增 test helper `TestPostedCallbackSequenceForTest()`，测试依次触发 RECEIVE、IDEAL_SEND_BUFFER、PEER_RECEIVE_ABORTED、SHUTDOWN_COMPLETE，并验证 posted callback 类型顺序为 receive-ready、ideal、abort、shutdown。

测试代码：

```cpp
{
    TqWindowsRelayWorker worker;
    alignas(MsQuicStream) unsigned char streamStorage[sizeof(MsQuicStream)]{};
    auto* stream = reinterpret_cast<MsQuicStream*>(streamStorage);
    stream->Callback = MsQuicStream::NoOpCallback;
    stream->Context = nullptr;
    stream->Handle = reinterpret_cast<HQUIC>(static_cast<uintptr_t>(1));
    TqRelayHandle handle{};
    TqTuningConfig tuning{};
    if (!worker.RegisterRelayForTest(stream, &handle, tuning, TqCompressAlgo::None)) {
        return 141;
    }
    worker.SetQuicReceiveViewDrainEnabledForTest(false);
    if (!worker.Start()) {
        return 144;
    }

    uint8_t data[] = {'x'};
    QUIC_BUFFER buffer{sizeof(data), data};
    QUIC_STREAM_EVENT receive{};
    receive.Type = QUIC_STREAM_EVENT_RECEIVE;
    receive.RECEIVE.BufferCount = 1;
    receive.RECEIVE.Buffers = &buffer;
    (void)TqWindowsRelayWorker::StreamCallback(stream, stream->Context, &receive);

    QUIC_STREAM_EVENT ideal{};
    ideal.Type = QUIC_STREAM_EVENT_IDEAL_SEND_BUFFER_SIZE;
    ideal.IDEAL_SEND_BUFFER_SIZE.ByteCount = 4096;
    (void)TqWindowsRelayWorker::StreamCallback(stream, stream->Context, &ideal);

    QUIC_STREAM_EVENT aborted{};
    aborted.Type = QUIC_STREAM_EVENT_PEER_RECEIVE_ABORTED;
    aborted.PEER_RECEIVE_ABORTED.ErrorCode = 42;
    (void)TqWindowsRelayWorker::StreamCallback(stream, stream->Context, &aborted);

    QUIC_STREAM_EVENT shutdown{};
    shutdown.Type = QUIC_STREAM_EVENT_SHUTDOWN_COMPLETE;
    shutdown.SHUTDOWN_COMPLETE.ConnectionErrorCode = 9;
    shutdown.SHUTDOWN_COMPLETE.ConnectionCloseStatus = QUIC_STATUS_SUCCESS;
    (void)TqWindowsRelayWorker::StreamCallback(stream, stream->Context, &shutdown);

    if (!worker.TestPostedCallbackSequenceForTest(
            "RelayReceiveReady,QuicIdealSendBuffer,QuicPeerReceiveAborted,QuicShutdownComplete")) {
        worker.Stop();
        return 142;
    }
    worker.Stop();
}
```

- [ ] **Step 2: 实现 test-only callback sequence 记录**

在 worker test-only private 字段加入：

```cpp
std::vector<TqWindowsIocpOperationType> PostedCallbackSequence_;
```

在 `PostCallbackOperation()` test-only 记录 block 中追加：

```cpp
PostedCallbackSequence_.push_back(type);
```

实现 helper：

```cpp
bool TqWindowsRelayWorker::TestPostedCallbackSequenceForTest(const char* expectedCsv) const {
    std::lock_guard<std::mutex> guard(LastPostedCallbackLock_);
    std::string actual;
    for (TqWindowsIocpOperationType type : PostedCallbackSequence_) {
        if (!actual.empty()) {
            actual.push_back(',');
        }
        actual += TestCallbackTypeName(type);
    }
    return actual == expectedCsv;
}
```

同时实现 file-local helper：

```cpp
const char* TestCallbackTypeName(TqWindowsIocpOperationType type) {
    switch (type) {
    case TqWindowsIocpOperationType::RelayReceiveReady:
        return "RelayReceiveReady";
    case TqWindowsIocpOperationType::QuicIdealSendBuffer:
        return "QuicIdealSendBuffer";
    case TqWindowsIocpOperationType::QuicPeerSendAborted:
        return "QuicPeerSendAborted";
    case TqWindowsIocpOperationType::QuicPeerReceiveAborted:
        return "QuicPeerReceiveAborted";
    case TqWindowsIocpOperationType::QuicShutdownComplete:
        return "QuicShutdownComplete";
    case TqWindowsIocpOperationType::QuicSendComplete:
        return "QuicSendComplete";
    default:
        return "Other";
    }
}
```

- [ ] **Step 3: 运行测试确认失败**

Run:

```powershell
cmake --build build --target tcpquic_windows_relay_worker_test --config Release
.\build\bin\Release\tcpquic_windows_relay_worker_test.exe
```

Expected: FAIL，sequence 只记录 receive-ready。

- [ ] **Step 4: 修改 `StreamCallback()` 的 ideal-buffer 分支**

替换 `TqWindowsRelayTask` enqueue：

```cpp
if (event->Type == QUIC_STREAM_EVENT_IDEAL_SEND_BUFFER_SIZE) {
    (void)worker->PostCallbackOperation(
        TqWindowsIocpOperationType::QuicIdealSendBuffer,
        relay,
        event->IDEAL_SEND_BUFFER_SIZE.ByteCount);
    return QUIC_STATUS_SUCCESS;
}
```

- [ ] **Step 5: 修改 peer abort 分支**

```cpp
if (event->Type == QUIC_STREAM_EVENT_PEER_SEND_ABORTED ||
    event->Type == QUIC_STREAM_EVENT_PEER_RECEIVE_ABORTED) {
    const bool sendAborted = event->Type == QUIC_STREAM_EVENT_PEER_SEND_ABORTED;
    (void)worker->PostCallbackOperation(
        sendAborted
            ? TqWindowsIocpOperationType::QuicPeerSendAborted
            : TqWindowsIocpOperationType::QuicPeerReceiveAborted,
        relay,
        sendAborted ? event->PEER_SEND_ABORTED.ErrorCode : event->PEER_RECEIVE_ABORTED.ErrorCode);
    return QUIC_STATUS_SUCCESS;
}
```

- [ ] **Step 6: 修改 shutdown complete 分支**

```cpp
if (event->Type == QUIC_STREAM_EVENT_SHUTDOWN_COMPLETE) {
    (void)worker->PostCallbackOperation(
        TqWindowsIocpOperationType::QuicShutdownComplete,
        relay,
        event->SHUTDOWN_COMPLETE.ConnectionErrorCode,
        static_cast<size_t>(event->SHUTDOWN_COMPLETE.ConnectionCloseStatus));
    return QUIC_STATUS_SUCCESS;
}
```

- [ ] **Step 7: 修改 `Run()` dispatch**

```cpp
case TqWindowsIocpOperationType::QuicIdealSendBuffer:
    HandleQuicIdealSendBuffer(op->RelayId, op->Value);
    break;
case TqWindowsIocpOperationType::QuicPeerSendAborted:
    ProcessQuicPeerAborted(op->RelayId, "stream_peer_send_aborted", op->Value);
    break;
case TqWindowsIocpOperationType::QuicPeerReceiveAborted:
    ProcessQuicPeerAborted(op->RelayId, "stream_peer_receive_aborted", op->Value);
    break;
case TqWindowsIocpOperationType::QuicShutdownComplete:
    ProcessQuicShutdownComplete(op->RelayId, op->Value, static_cast<uint32_t>(op->Length));
    break;
```

- [ ] **Step 8: 运行测试确认通过**

Run:

```powershell
cmake --build build --target tcpquic_windows_relay_worker_test --config Release
.\build\bin\Release\tcpquic_windows_relay_worker_test.exe
```

Expected: PASS。

- [ ] **Step 9: Commit**

```bash
git add src/tunnel/windows_relay_worker.cpp src/tunnel/windows_relay_worker.h src/unittest/windows_relay_worker_test.cpp
git commit -m "feat(windows): post stream callback events through IOCP"
```

---

### Task 6: 移除生产路径 worker event queue

**Files:**
- Modify: `src/tunnel/windows_relay_worker.cpp`
- Modify: `src/tunnel/windows_relay_worker.h`
- Modify: `src/unittest/windows_relay_worker_test.cpp`

- [ ] **Step 1: 写生产路径 guard 测试**

在 `src/unittest/production_linkage_guard_test.cpp` 或 `src/unittest/windows_relay_worker_test.cpp` 中增加文本扫描，验证 `windows_relay_worker.cpp` 不再调用 `EnqueueEvent(`、`DrainEvents(` 或 `ProcessRelayTask(`。

```cpp
const std::string workerSource = ReadFile("src/tunnel/windows_relay_worker.cpp");
if (workerSource.find("EnqueueEvent(") != std::string::npos) return 31;
if (workerSource.find("DrainEvents(") != std::string::npos) return 32;
if (workerSource.find("ProcessRelayTask(") != std::string::npos) return 33;
```

- [ ] **Step 2: 运行 guard 测试确认失败**

Run:

```bash
rtk cmake --build build-glibc --target tcpquic_production_linkage_guard_test
rtk ./build-glibc/bin/Release/tcpquic_production_linkage_guard_test
```

Expected: FAIL，旧函数名仍存在。

- [ ] **Step 3: 删除 production `EnqueueEvent()`、`Wake()`、`DrainEvents()`、`ProcessRelayTask()`**

从 `src/tunnel/windows_relay_worker.h` 删除这些 private 声明：

```cpp
bool EnqueueEvent(TqWindowsRelayTask&& task);
void Wake();
size_t DrainEvents(size_t budget);
void ProcessRelayTask(TqWindowsRelayTask& task);
void ProcessQuicReceiveViewTask(TqWindowsRelayTask& task);
```

从 `src/tunnel/windows_relay_worker.cpp` 删除对应函数实现。`ProcessQuicSendCompleteTask()` 可以在 Task 7 改为直接参数版本前暂留。

- [ ] **Step 4: 修改 `Run()` 循环入口**

删除每轮开头和 completion 后的：

```cpp
(void)DrainEvents(EventBudget_);
DrainPerRelayMaintenance();
```

改为只保留：

```cpp
DrainPerRelayMaintenance();
```

`overlapped == nullptr` 的 wake 分支只用于 stop：

```cpp
if (overlapped == nullptr) {
    DrainPerRelayMaintenance();
    if (Stopping_.load(std::memory_order_acquire)) {
        break;
    }
    continue;
}
```

- [ ] **Step 5: 保留 `TqWindowsRelayEventQueue` 单元测试但移出 worker**

如果 `windows_relay_worker.h` 不再 include `windows_relay_event_queue.h`，则让 `src/unittest/windows_relay_worker_test.cpp` 直接 include：

```cpp
#include "windows_relay_event_queue.h"
```

删除 `TqWindowsRelayWorker` 成员：

```cpp
TqWindowsRelayEventQueue EventQueue_;
```

构造函数初始化列表删除：

```cpp
EventQueue_(queueCapacity),
```

- [ ] **Step 6: 更新 constructor 参数**

保留 public constructor ABI，避免大范围调用点变化：

```cpp
TqWindowsRelayWorker::TqWindowsRelayWorker(
    uint32_t workerIndex,
    size_t queueCapacity,
    size_t eventBudget) :
    WorkerIndex_(workerIndex),
    EventBudget_(eventBudget) {
    (void)queueCapacity;
}
```

- [ ] **Step 7: 让 `Snapshot()` 在本任务中保持可编译**

删除 `Snapshot()` 里对 `EventQueue_.SizeApprox()` 和 `EventQueue_.Capacity()` 的直接访问。本任务先保持旧字段但赋零，Task 8 再统一重命名 metric：

```cpp
snapshot.WindowsEventQueueDepth = 0;
snapshot.WindowsEventQueueCapacity = 0;
snapshot.WindowsEventQueueFullCount = 0;
snapshot.WindowsEventQueueWakeCount =
    EventQueueWakeCount_.load(std::memory_order_relaxed);
snapshot.WindowsEventQueueWakeFailedCount =
    EventQueueWakeFailedCount_.load(std::memory_order_relaxed);
```

在 active relay snapshot 中也把旧 `WindowsEventQueueDepth` 设为 `0`：

```cpp
active.WindowsEventQueueDepth = 0;
```

- [ ] **Step 8: 运行 guard 和 Windows 测试**

Run:

```bash
rtk cmake --build build-glibc --target tcpquic_production_linkage_guard_test
rtk ./build-glibc/bin/Release/tcpquic_production_linkage_guard_test
```

Expected: PASS。

Run on Windows:

```powershell
cmake --build build --target tcpquic_windows_relay_worker_test --config Release
.\build\bin\Release\tcpquic_windows_relay_worker_test.exe
```

Expected: PASS。

- [ ] **Step 9: Commit**

```bash
git add src/tunnel/windows_relay_worker.cpp src/tunnel/windows_relay_worker.h src/unittest/windows_relay_worker_test.cpp src/unittest/production_linkage_guard_test.cpp
git commit -m "refactor(windows): remove relay worker event queue dispatch"
```

---

### Task 7: 清理 `TqWindowsRelayTask` 依赖和 send-complete 桥接

**Files:**
- Modify: `src/tunnel/windows_relay_worker.cpp`
- Modify: `src/tunnel/windows_relay_worker.h`
- Modify: `src/tunnel/windows_relay_event_queue.h`
- Modify: `src/unittest/windows_relay_worker_test.cpp`

- [ ] **Step 1: 改造 send-complete 处理函数签名**

在 header 中把：

```cpp
void ProcessQuicSendCompleteTask(TqWindowsRelayTask& task);
```

替换为：

```cpp
void ProcessQuicSendCompleteOperation(uint64_t relayId, uintptr_t operationValue);
```

实现替换为：

```cpp
void TqWindowsRelayWorker::ProcessQuicSendCompleteOperation(
    uint64_t relayId,
    uintptr_t operationValue) {
    QuicSendCompleteEvents_.fetch_add(1, std::memory_order_relaxed);
    std::unique_ptr<TqWindowsQuicSendOperation> operation(
        reinterpret_cast<TqWindowsQuicSendOperation*>(operationValue));
    if (!operation || operation->Magic != TqWindowsQuicSendOperation::MagicValue) {
        return;
    }
    auto relay = FindRelayById(relayId != 0 ? relayId : operation->RelayId);
    if (!relay) {
        return;
    }
    relay->InFlightQuicSends.fetch_sub(1, std::memory_order_acq_rel);
    SaturatingFetchSub(relay->OutstandingQuicSendBytes, operation->TotalBytes);
    if (operation->Fin) {
        relay->QuicSendFinCompleted.store(true, std::memory_order_release);
        TqTraceRelayStopCondition(
            "windows",
            WorkerIndex_,
            "quic_send_fin_completed",
            BuildRelayTraceState(relay));
        if (!relay->Closing.load(std::memory_order_acquire) &&
            relay->TcpRecvClosed.load(std::memory_order_acquire) &&
            relay->PendingQuicReceiveQueueDepth.load(std::memory_order_acquire) == 0 &&
            relay->InFlightTcpSends.load(std::memory_order_acquire) == 0 &&
            relay->PendingQuicReceiveBytes.load(std::memory_order_acquire) == 0 &&
            relay->TcpWriteBytes.load(std::memory_order_acquire) > 0) {
            relay->CloseAfterDrained.store(true, std::memory_order_release);
        }
    }
    if (relay->Closing.load(std::memory_order_acquire)) {
        TryRetireRelay(relay);
        return;
    }
    RetryPendingQuicSends(relay);
    MaybePostTcpRecv(relay);
    (void)CloseRelayIfDrained(relay);
}
```

- [ ] **Step 2: 修改 `Run()` 中 QuicSendComplete dispatch**

```cpp
case TqWindowsIocpOperationType::QuicSendComplete:
    ProcessQuicSendCompleteOperation(op->RelayId, static_cast<uintptr_t>(op->Value));
    op->Value = 0;
    break;
```

- [ ] **Step 3: 修改 test helper**

把 `TestProcessQuicSendCompleteForTest()` 中构造 `TqWindowsRelayTask` 的代码改为：

```cpp
auto* operation = new TqWindowsQuicSendOperation();
operation->RelayId = relayId;
operation->TotalBytes = completedBytes;
relay->InFlightQuicSends.fetch_add(1, std::memory_order_acq_rel);
ProcessQuicSendCompleteOperation(relayId, reinterpret_cast<uintptr_t>(operation));
```

- [ ] **Step 4: 从 worker header 移除 `windows_relay_event_queue.h` include**

删除：

```cpp
#include "windows_relay_event_queue.h"
```

如果只剩 `windows_relay_event_queue.h` 自身单测需要 `TqWindowsRelayTask`，该文件保留；worker 不再依赖。

- [ ] **Step 5: 运行全量 Windows relay worker 测试**

Run:

```powershell
cmake --build build --target tcpquic_windows_relay_worker_test --config Release
.\build\bin\Release\tcpquic_windows_relay_worker_test.exe
```

Expected: PASS。

- [ ] **Step 6: Commit**

```bash
git add src/tunnel/windows_relay_worker.cpp src/tunnel/windows_relay_worker.h src/tunnel/windows_relay_event_queue.h src/unittest/windows_relay_worker_test.cpp
git commit -m "refactor(windows): remove relay task bridge from worker"
```

---

### Task 8: 更新 metrics、trace 和文档

**Files:**
- Modify: `src/tunnel/windows_relay_worker.h`
- Modify: `src/tunnel/windows_relay_worker.cpp`
- Modify: `src/runtime/relay_metrics.h`
- Modify: `src/runtime/relay_metrics.cpp`
- Modify: `src/runtime/trace.cpp`
- Modify: `src/docs/thread_model_cn.md`
- Modify: `docs/iocp-quic2tcp-queue_cn.md`

- [ ] **Step 1: 重命名 snapshot 字段**

在 `TqWindowsRelayWorkerSnapshot` 中替换：

```cpp
uint64_t WindowsEventQueueDepth{0};
uint64_t WindowsEventQueueCapacity{0};
uint64_t WindowsEventQueueFullCount{0};
uint64_t WindowsEventQueueWakeCount{0};
uint64_t WindowsEventQueueWakeFailedCount{0};
```

为：

```cpp
uint64_t WindowsCallbackIocpPostCount{0};
uint64_t WindowsCallbackIocpPostFailedCount{0};
uint64_t WindowsReceiveReadyPostCount{0};
uint64_t WindowsReceiveDrainScheduledCount{0};
uint64_t WindowsReceiveDrainCoalescedCount{0};
uint64_t WindowsPostedCallbackStaleDropCount{0};
```

- [ ] **Step 2: 更新 worker counters**

在 worker private counters 中新增：

```cpp
std::atomic<uint64_t> CallbackIocpPostCount_{0};
std::atomic<uint64_t> CallbackIocpPostFailedCount_{0};
std::atomic<uint64_t> ReceiveReadyPostCount_{0};
std::atomic<uint64_t> ReceiveDrainScheduledCount_{0};
std::atomic<uint64_t> ReceiveDrainCoalescedCount_{0};
std::atomic<uint64_t> PostedCallbackStaleDropCount_{0};
```

在 `PostCallbackOperation()` 成功/失败路径更新：

```cpp
CallbackIocpPostCount_.fetch_add(1, std::memory_order_relaxed);
if (type == TqWindowsIocpOperationType::RelayReceiveReady) {
    ReceiveReadyPostCount_.fetch_add(1, std::memory_order_relaxed);
}
```

失败路径：

```cpp
CallbackIocpPostFailedCount_.fetch_add(1, std::memory_order_relaxed);
```

在 `ScheduleRelayReceiveDrain()` 中：

```cpp
if (relay->ReceiveDrainQueued.exchange(true, std::memory_order_acq_rel)) {
    ReceiveDrainCoalescedCount_.fetch_add(1, std::memory_order_relaxed);
    return true;
}
ReceiveDrainScheduledCount_.fetch_add(1, std::memory_order_relaxed);
```

- [ ] **Step 3: 更新 `Snapshot()`**

把旧 event queue 字段赋值替换为：

```cpp
snapshot.WindowsCallbackIocpPostCount =
    CallbackIocpPostCount_.load(std::memory_order_relaxed);
snapshot.WindowsCallbackIocpPostFailedCount =
    CallbackIocpPostFailedCount_.load(std::memory_order_relaxed);
snapshot.WindowsReceiveReadyPostCount =
    ReceiveReadyPostCount_.load(std::memory_order_relaxed);
snapshot.WindowsReceiveDrainScheduledCount =
    ReceiveDrainScheduledCount_.load(std::memory_order_relaxed);
snapshot.WindowsReceiveDrainCoalescedCount =
    ReceiveDrainCoalescedCount_.load(std::memory_order_relaxed);
snapshot.WindowsPostedCallbackStaleDropCount =
    PostedCallbackStaleDropCount_.load(std::memory_order_relaxed);
```

- [ ] **Step 4: 更新 runtime metrics JSON 输出**

在 `src/runtime/relay_metrics.cpp` 中把旧 `windows_relay_event_queue_*` 输出替换为：

```cpp
out << ",\"windows_relay_callback_iocp_posts\":" << metrics.WindowsCallbackIocpPostCount;
out << ",\"windows_relay_callback_iocp_post_failures\":" << metrics.WindowsCallbackIocpPostFailedCount;
out << ",\"windows_relay_receive_ready_posts\":" << metrics.WindowsReceiveReadyPostCount;
out << ",\"windows_relay_receive_drain_scheduled\":" << metrics.WindowsReceiveDrainScheduledCount;
out << ",\"windows_relay_receive_drain_coalesced\":" << metrics.WindowsReceiveDrainCoalescedCount;
out << ",\"windows_relay_posted_callback_stale_drops\":" << metrics.WindowsPostedCallbackStaleDropCount;
```

- [ ] **Step 5: 更新 trace active relay 字段**

在 `src/runtime/trace.cpp` 的 Windows active relay trace 中，将 `event_queue_depth=%llu` 改为：

```cpp
"callback_iocp_posts=%llu receive_ready_posts=%llu receive_drain_scheduled=%llu"
```

对应参数使用 snapshot 字段。

- [ ] **Step 6: 更新中文文档**

在 `src/docs/thread_model_cn.md` 的 Windows relay QUIC -> TCP 部分写入：

```text
Windows RECEIVE callback 不再投递 QuicReceiveView(view*) 到 worker event queue。
callback 线程先把 view 放入 relay->PendingReceives，然后向 IOCP 投递 RelayReceiveReady(relayId)。
worker 收到 ready marker 后从 relay->PendingReceives 队首 drain。
TCP send completion 或 FinishReceiveView 需要继续推进时，worker 内部投递可合并的 RelayReceiveDrain(relayId)。
```

在 `docs/iocp-quic2tcp-queue_cn.md` 末尾增加：

```text
后续设计已确定：Windows relay 将一次性迁移所有 MsQuic callback 事件到 IOCP posted operation。
RECEIVE callback 使用 RelayReceiveReady(relayId) 顺序标记，不携带 receive view 指针。
worker event queue 不再作为 callback 事件通道。
```

- [ ] **Step 7: 运行检查和测试**

Run:

```bash
rtk git diff --check
```

Expected: no output。

Run on Windows:

```powershell
cmake --build build --target tcpquic_windows_relay_worker_test --config Release
.\build\bin\Release\tcpquic_windows_relay_worker_test.exe
```

Expected: PASS。

- [ ] **Step 8: Commit**

```bash
git add src/tunnel/windows_relay_worker.cpp src/tunnel/windows_relay_worker.h src/runtime/relay_metrics.cpp src/runtime/relay_metrics.h src/runtime/trace.cpp src/docs/thread_model_cn.md docs/iocp-quic2tcp-queue_cn.md
git commit -m "docs(windows): describe IOCP callback relay scheduling"
```

---

### Task 9: 最终回归和清理

**Files:**
- Modify if needed: files touched in Tasks 1-8

- [ ] **Step 1: 搜索禁止残留**

Run:

```bash
rtk rg -n "EnqueueEvent\\(|DrainEvents\\(|ProcessRelayTask\\(|QuicReceiveViewQueued|QuicReceiveView\\)" src/tunnel/windows_relay_worker.cpp src/tunnel/windows_relay_worker.h
```

Expected: no production event-queue dispatch matches. `QuicReceiveView` may remain only in trace text or historical docs outside worker production code.

- [ ] **Step 2: 检查 receive ready 不携带 view 指针**

Run:

```bash
rtk rg -n "RelayReceiveReady|ReceiveView" src/tunnel/windows_relay_worker.cpp
```

Expected: `RelayReceiveReady` operation construction does not assign `op->ReceiveView`。

- [ ] **Step 3: Windows 测试**

Run:

```powershell
cmake --build build --target tcpquic_windows_relay_worker_test --config Release
.\build\bin\Release\tcpquic_windows_relay_worker_test.exe
cmake --build build --target tcpquic_tunnel_test --config Release
.\build\bin\Release\tcpquic_tunnel_test.exe
```

Expected: both tests exit `0`。

- [ ] **Step 4: Linux guard**

Run:

```bash
rtk cmake --build build-glibc --target tcpquic_production_linkage_guard_test
rtk ./build-glibc/bin/Release/tcpquic_production_linkage_guard_test
rtk git diff --check
```

Expected: all commands pass。

- [ ] **Step 5: Commit final cleanup**

如果 Step 1-4 发现并修复了残留，提交：

```bash
git add src docs
git commit -m "chore(windows): finalize IOCP callback queue migration"
```

如果没有额外修改，不创建空提交。
