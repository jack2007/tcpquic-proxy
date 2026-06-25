# Windows Linux-Style Relay Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 将 Windows relay 改为 Linux-style 用户态事件队列和 worker-owned 背压状态机，修复网页代理在 Windows 下长期不稳定的问题。

**Architecture:** 新增 bounded `TqWindowsRelayEventQueue`，让 QUIC callback/control events 不再走 IOCP payload；IOCP 只处理真实 `WSARecv/WSASend` completion。`tcp->quic` 的 `WSARecv` 由 worker `MaybePostTcpRecv` 根据 outstanding QUIC send bytes 高低水位恢复，不再由 `SEND_COMPLETE` callback 直接提交。

**Tech Stack:** C++17、Windows IOCP、MsQuic、`tcpquic_windows_relay_worker_test`

**Design Spec:** `docs/superpowers/specs/2026-06-25-windows-linux-style-relay-design.md`

---

## 文件清单

| 文件 | 责任 |
|------|------|
| `src/tunnel/windows_relay_event_queue.h` | 新增 Windows 用户态 bounded event queue |
| `src/tunnel/windows_relay_worker.h` | 新增 queue、event drain、backpressure、test hook 声明 |
| `src/tunnel/windows_relay_worker.cpp` | 迁移 QUIC callback/control events，重写 TCP read 恢复状态机 |
| `src/config/tuning.h` | 新增 Windows event queue/event budget/send backlog tuning |
| `src/config/tuning.cpp` | 打印/计算新增 Windows tuning 字段 |
| `src/unittest/windows_relay_worker_test.cpp` | event queue、fallback、send-complete、backpressure、teardown 单测 |
| `docs/superpowers/specs/2026-06-25-windows-linux-style-relay-design.md` | 设计依据 |

---

## Phase 0 — 基线与保护

### Task 0: 记录当前测试基线

**Files:**
- Read: `src/unittest/windows_relay_worker_test.cpp`
- Read: `docs/windows/iocp-bug-cm25.md`
- Read: `docs/windows/iocp-bug-gpt55.md`

- [ ] **Step 1: 运行当前 Windows relay 测试**

Run:

```powershell
build-x64\bin\Release\tcpquic_windows_relay_worker_test.exe
```

Expected:

```text
记录当前 exit code；如果仍为 exit 202，记录失败用例名称和断言位置。
```

- [ ] **Step 2: 记录工作区状态**

Run:

```powershell
git status --short
```

Expected:

```text
确认本计划之外的未跟踪构建产物不纳入后续提交。
```

- [ ] **Step 3: Commit**

本 task 不修改代码，不提交。

---

## Phase 1 — Windows 用户态事件队列

### Task 1: 添加 `TqWindowsRelayEventQueue`

**Files:**
- Create: `src/tunnel/windows_relay_event_queue.h`
- Test: `src/unittest/windows_relay_worker_test.cpp`

- [ ] **Step 1: 写 event queue 基础单测**

在 `windows_relay_worker_test.cpp` 增加：

```cpp
bool TestWindowsRelayEventQueuePushPop() {
    TqWindowsRelayEventQueue queue(2);
    TqWindowsRelayEvent first{};
    first.Type = TqWindowsRelayEventType::ResumeTcpRecv;
    first.RelayId = 7;
    TqWindowsRelayEvent second{};
    second.Type = TqWindowsRelayEventType::CloseRelay;
    second.RelayId = 8;

    if (!queue.TryPush(std::move(first))) {
        return false;
    }
    if (!queue.TryPush(std::move(second))) {
        return false;
    }
    if (queue.TryPush(TqWindowsRelayEvent{})) {
        return false;
    }

    TqWindowsRelayEvent out{};
    if (!queue.TryPop(out) || out.Type != TqWindowsRelayEventType::ResumeTcpRecv || out.RelayId != 7) {
        return false;
    }
    if (!queue.TryPop(out) || out.Type != TqWindowsRelayEventType::CloseRelay || out.RelayId != 8) {
        return false;
    }
    return !queue.TryPop(out);
}
```

在 test runner 中调用该函数。

- [ ] **Step 2: 运行测试确认失败**

Run:

```powershell
MSBuild build-x64\src\tcpquic_windows_relay_worker_test.vcxproj /p:Configuration=Release /p:Platform=x64 /t:ClCompile
```

Expected:

```text
编译失败，提示 TqWindowsRelayEventQueue / TqWindowsRelayEventType 未定义。
```

- [ ] **Step 3: 新增 queue header**

创建 `src/tunnel/windows_relay_event_queue.h`：

```cpp
#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>

struct TqWindowsPendingQuicReceive;
struct TqWindowsQuicSendOperation;

enum class TqWindowsRelayEventType {
    Wake,
    QuicReceiveView,
    QuicSendComplete,
    QuicIdealSendBuffer,
    QuicSendRetry,
    ResumeTcpRecv,
    CloseRelay,
    QuicPeerSendAborted,
    QuicPeerReceiveAborted,
    QuicShutdownComplete,
    StopWorker,
};

struct TqWindowsRelayEvent {
    TqWindowsRelayEventType Type{TqWindowsRelayEventType::Wake};
    uint64_t RelayId{0};
    uint64_t Value{0};
    uint32_t Status{0};
    bool Fin{false};
    std::shared_ptr<TqWindowsPendingQuicReceive> ReceiveView;
    TqWindowsQuicSendOperation* SendOperation{nullptr};
};

class TqWindowsRelayEventQueue final {
public:
    explicit TqWindowsRelayEventQueue(size_t capacity)
        : CapacityValue(NormalizeCapacity(capacity)),
          Mask(CapacityValue - 1),
          Slots(new Cell[CapacityValue]) {
        for (size_t i = 0; i < CapacityValue; ++i) {
            Slots[i].Sequence.store(i, std::memory_order_relaxed);
        }
    }

    TqWindowsRelayEventQueue(const TqWindowsRelayEventQueue&) = delete;
    TqWindowsRelayEventQueue& operator=(const TqWindowsRelayEventQueue&) = delete;

    bool TryPush(TqWindowsRelayEvent&& event) {
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

    bool TryPop(TqWindowsRelayEvent& event) {
        Cell* cell = nullptr;
        size_t position = DequeuePos.load(std::memory_order_relaxed);
        for (;;) {
            cell = &Slots[position & Mask];
            const size_t sequence = cell->Sequence.load(std::memory_order_acquire);
            const intptr_t diff = static_cast<intptr_t>(sequence) -
                static_cast<intptr_t>(position + 1);
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
        cell->Event = TqWindowsRelayEvent{};
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
        TqWindowsRelayEvent Event;
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

- [ ] **Step 4: include header 并运行测试**

在 `windows_relay_worker_test.cpp` 加：

```cpp
#include "windows_relay_event_queue.h"
```

Run:

```powershell
MSBuild build-x64\src\tcpquic_windows_relay_worker_test.vcxproj /p:Configuration=Release /p:Platform=x64 /t:ClCompile
build-x64\bin\Release\tcpquic_windows_relay_worker_test.exe
```

Expected:

```text
event queue 新测试通过；若既有 half-close 测试仍失败，记录为既有失败，不在本 task 修复。
```

- [ ] **Step 5: Commit**

```bash
git add src/tunnel/windows_relay_event_queue.h src/unittest/windows_relay_worker_test.cpp
git commit -m "feat(windows): add relay event queue"
```

---

## Phase 2 — Worker queue/wake 骨架

### Task 2: 接入 event queue 但不迁移业务事件

**Files:**
- Modify: `src/config/tuning.h`
- Modify: `src/config/tuning.cpp`
- Modify: `src/tunnel/windows_relay_worker.h`
- Modify: `src/tunnel/windows_relay_worker.cpp`
- Test: `src/unittest/windows_relay_worker_test.cpp`

- [ ] **Step 1: 增加 tuning 字段**

在 `TqTuningConfig` 添加：

```cpp
    uint32_t WindowsRelayEventQueueCapacity{8192};
    uint32_t WindowsRelayWorkerEventBudget{4096};
    uint64_t WindowsRelayMaxBufferedQuicSendBytes{64ull * 1024 * 1024};
```

在 `TqPrintTuning` 输出这三个字段，格式沿用现有 tuning 输出。

- [ ] **Step 2: 在 worker header 添加成员/方法**

在 `windows_relay_worker.h` include：

```cpp
#include "windows_relay_event_queue.h"
```

添加私有方法：

```cpp
    bool EnqueueEvent(TqWindowsRelayEvent&& event);
    void Wake();
    size_t DrainEvents(size_t budget);
    void DrainPerRelayMaintenance();
    void ProcessRelayEvent(TqWindowsRelayEvent& event);
```

添加成员：

```cpp
    TqWindowsRelayEventQueue EventQueue_{8192};
    std::atomic<bool> WakeArmed_{false};
    std::atomic<uint64_t> EventQueueFullCount_{0};
    std::atomic<uint64_t> EventQueueWakeCount_{0};
    std::atomic<uint64_t> EventQueueWakeFailedCount_{0};
    std::atomic<uint64_t> EventsProcessed_{0};
```

构造函数后续 task 再从 tuning 注入 capacity；本 task 可先使用默认 8192。

- [ ] **Step 3: 实现 enqueue/wake/drain 空分发**

在 `windows_relay_worker.cpp` 添加：

```cpp
bool TqWindowsRelayWorker::EnqueueEvent(TqWindowsRelayEvent&& event) {
    if (!EventQueue_.TryPush(std::move(event))) {
        EventQueueFullCount_.fetch_add(1, std::memory_order_relaxed);
        return false;
    }
    Wake();
    return true;
}

void TqWindowsRelayWorker::Wake() {
    if (Iocp_ == nullptr) {
        return;
    }
    if (WakeArmed_.exchange(true, std::memory_order_acq_rel)) {
        return;
    }
    if (::PostQueuedCompletionStatus(static_cast<HANDLE>(Iocp_), 0, 0, nullptr)) {
        EventQueueWakeCount_.fetch_add(1, std::memory_order_relaxed);
    } else {
        WakeArmed_.store(false, std::memory_order_release);
        EventQueueWakeFailedCount_.fetch_add(1, std::memory_order_relaxed);
    }
}

size_t TqWindowsRelayWorker::DrainEvents(size_t budget) {
    size_t processed = 0;
    while (processed < budget) {
        TqWindowsRelayEvent event{};
        if (!EventQueue_.TryPop(event)) {
            break;
        }
        ProcessRelayEvent(event);
        ++processed;
    }
    EventsProcessed_.fetch_add(processed, std::memory_order_relaxed);
    return processed;
}

void TqWindowsRelayWorker::ProcessRelayEvent(TqWindowsRelayEvent& event) {
    switch (event.Type) {
    case TqWindowsRelayEventType::Wake:
        break;
    default:
        break;
    }
}
```

- [ ] **Step 4: 修改 `Run()` drain queue**

在 `Run()` loop 顶部和 IOCP completion 后添加：

```cpp
        (void)DrainEvents(4096);
        DrainPerRelayMaintenance();
```

在 `overlapped == nullptr` 分支：

```cpp
        if (overlapped == nullptr) {
            WakeArmed_.store(false, std::memory_order_release);
            (void)DrainEvents(4096);
            DrainPerRelayMaintenance();
            if (Stopping_.load(std::memory_order_acquire)) {
                break;
            }
            continue;
        }
```

- [ ] **Step 5: 实现 maintenance 空骨架**

```cpp
void TqWindowsRelayWorker::DrainPerRelayMaintenance() {
    std::vector<std::shared_ptr<RelayContext>> relays;
    {
        std::lock_guard<std::mutex> guard(Lock_);
        relays.reserve(Relays_.size());
        for (const auto& entry : Relays_) {
            relays.push_back(entry.second);
        }
    }
    for (const auto& relay : relays) {
        if (!relay) {
            continue;
        }
        RetryPendingQuicSends(relay);
        (void)CloseRelayIfDrained(relay);
        TryRetireRelay(relay);
    }
}
```

- [ ] **Step 6: 运行测试**

Run:

```powershell
MSBuild build-x64\src\tcpquic_windows_relay_worker_test.vcxproj /p:Configuration=Release /p:Platform=x64 /t:ClCompile
build-x64\bin\Release\tcpquic_windows_relay_worker_test.exe
```

Expected:

```text
不新增测试失败；stop/wake 不挂死。
```

- [ ] **Step 7: Commit**

```bash
git add src/config/tuning.h src/config/tuning.cpp src/tunnel/windows_relay_worker.h src/tunnel/windows_relay_worker.cpp src/unittest/windows_relay_worker_test.cpp
git commit -m "feat(windows): add relay worker event drain loop"
```

---

## Phase 3 — 迁移 quic->tcp receive view

### Task 3: `QuicReceiveView` 不再通过 IOCP 投递

**Files:**
- Modify: `src/tunnel/windows_relay_worker.h`
- Modify: `src/tunnel/windows_relay_worker.cpp`
- Test: `src/unittest/windows_relay_worker_test.cpp`

- [ ] **Step 1: 写 callback receive view 入队测试**

新增测试：构造 fake stream/relay，调用 `StreamCallback(RECEIVE)`，断言返回 `QUIC_STATUS_PENDING`，并通过 worker drain 后浏览器侧 socket 收到 bytes。

核心断言：

```cpp
const QUIC_STATUS status = TqWindowsRelayWorker::StreamCallback(&stream, stream.Context, &receiveEvent);
if (status != QUIC_STATUS_PENDING) {
    return false;
}
```

随后等待 TCP peer 收到 payload：

```cpp
std::vector<uint8_t> received;
if (!WaitForTcpBytes(tcpPeer, received, expected.size(), 2000)) {
    return false;
}
return received == expected;
```

- [ ] **Step 2: 运行测试确认当前路径仍依赖 IOCP posted op**

Run:

```powershell
build-x64\bin\Release\tcpquic_windows_relay_worker_test.exe
```

Expected:

```text
测试可失败或通过；记录当前行为。后续步骤必须让它在 event queue 路径下通过。
```

- [ ] **Step 3: 修改 `QueueDeferredQuicReceive`**

删除 `PostQueuedCompletionStatus(... QuicReceiveViewQueued ...)` payload 逻辑。保留 view 构造、pending bytes/depth accounting。替换为：

```cpp
    TqWindowsRelayEvent event{};
    event.Type = TqWindowsRelayEventType::QuicReceiveView;
    event.RelayId = relay->Id;
    event.ReceiveView = view;
    event.Fin = fin;
    if (!EnqueueEvent(std::move(event))) {
        {
            std::lock_guard<std::mutex> guard(relay->CallbackPendingQuicReceiveLock);
            relay->CallbackPendingQuicReceives.push_back(view);
        }
        if (!relay->QuicReceivePaused.exchange(true, std::memory_order_acq_rel)) {
            SetQuicReceiveEnabled(relay, false);
        }
        return true;
    }
    return true;
```

需要在 `RelayContext` 增加：

```cpp
    std::mutex CallbackPendingQuicReceiveLock;
    std::deque<std::shared_ptr<TqWindowsPendingQuicReceive>> CallbackPendingQuicReceives;
```

- [ ] **Step 4: 实现 `ProcessQuicReceiveViewEvent`**

新增方法：

```cpp
void TqWindowsRelayWorker::ProcessQuicReceiveViewEvent(TqWindowsRelayEvent& event) {
    auto relay = FindRelayById(event.RelayId);
    auto view = event.ReceiveView;
    if (!relay || !view) {
        if (view) {
            FlushDeferredReceiveCompletion(*view, true);
            CompleteRemainingReceiveOwnership(*view);
        }
        return;
    }
    if (relay->Closing.load(std::memory_order_acquire)) {
        (void)CompletePendingQuicReceive(relay, view);
        return;
    }
    {
        std::lock_guard<std::mutex> guard(relay->PendingReceiveLock);
        const bool alreadyQueued =
            std::find(relay->PendingReceives.begin(), relay->PendingReceives.end(), view) !=
            relay->PendingReceives.end();
        if (!alreadyQueued) {
            relay->PendingReceives.push_back(view);
        }
        if (relay->PendingReceives.front() != view) {
            return;
        }
    }
    if (relay->Decompressor != nullptr && relay->CompressAlgo == TqCompressAlgo::Zstd) {
        if (!PostTcpSendFromCompressedReceiveView(relay, view)) {
            (void)CompletePendingQuicReceive(relay, view);
            CloseRelay(relay, TqRelayCloseMode::GracefulDrain, "post_tcp_send_compressed_receive_view_failed");
        }
        return;
    }
    if (!PostTcpSendFromReceiveView(relay, view)) {
        (void)CompletePendingQuicReceive(relay, view);
        CloseRelay(relay, TqRelayCloseMode::GracefulDrain, "post_tcp_send_receive_view_failed");
    }
}
```

`FindRelayById` 如不存在则新增私有 helper。

- [ ] **Step 5: `FinishReceiveView` 调度下一 view 改为 event queue**

替换 `PostQueuedCompletionStatus(QuicReceiveViewQueued)`：

```cpp
        TqWindowsRelayEvent event{};
        event.Type = TqWindowsRelayEventType::QuicReceiveView;
        event.RelayId = relay->Id;
        event.ReceiveView = nextView;
        if (!EnqueueEvent(std::move(event))) {
            EventQueueFullCount_.fetch_add(1, std::memory_order_relaxed);
        }
```

- [ ] **Step 6: drain callback fallback**

实现：

```cpp
void TqWindowsRelayWorker::DrainCallbackPendingQuicReceives(
    const std::shared_ptr<RelayContext>& relay) {
    if (!relay || relay->Closing.load(std::memory_order_acquire)) {
        return;
    }
    for (;;) {
        std::shared_ptr<TqWindowsPendingQuicReceive> view;
        {
            std::lock_guard<std::mutex> guard(relay->CallbackPendingQuicReceiveLock);
            if (relay->CallbackPendingQuicReceives.empty()) {
                break;
            }
            view = relay->CallbackPendingQuicReceives.front();
            relay->CallbackPendingQuicReceives.pop_front();
        }
        TqWindowsRelayEvent event{};
        event.Type = TqWindowsRelayEventType::QuicReceiveView;
        event.RelayId = relay->Id;
        event.ReceiveView = view;
        if (!EnqueueEvent(std::move(event))) {
            std::lock_guard<std::mutex> guard(relay->CallbackPendingQuicReceiveLock);
            relay->CallbackPendingQuicReceives.push_front(view);
            break;
        }
    }
    MaybeResumeQuicReceive(relay);
}
```

在 `DrainPerRelayMaintenance` 调用它。

- [ ] **Step 7: 移除旧事件分支**

`Run()` 中删除或保留 no-op：

```cpp
case TqWindowsRelayEvent::QuicReceiveViewQueued:
```

新代码不应再创建非 TCP 的 `IoOperation` 来投递 receive view。

- [ ] **Step 8: 运行测试**

Run:

```powershell
MSBuild build-x64\src\tcpquic_windows_relay_worker_test.vcxproj /p:Configuration=Release /p:Platform=x64 /t:ClCompile
build-x64\bin\Release\tcpquic_windows_relay_worker_test.exe
```

Expected:

```text
receive view event queue 测试通过；无新增 fatal/error 计数。
```

- [ ] **Step 9: Commit**

```bash
git add src/tunnel/windows_relay_worker.h src/tunnel/windows_relay_worker.cpp src/unittest/windows_relay_worker_test.cpp
git commit -m "refactor(windows): route QUIC receives through relay event queue"
```

---

## Phase 4 — QUIC send operation 与 SEND_COMPLETE 迁移

### Task 4: `SEND_COMPLETE` 只入队，不直接 `PostTcpRecv`

**Files:**
- Modify: `src/tunnel/windows_relay_worker.h`
- Modify: `src/tunnel/windows_relay_worker.cpp`
- Test: `src/unittest/windows_relay_worker_test.cpp`

- [ ] **Step 1: 写防回归测试**

新增测试：设置 fake stream send 成功但不立即 complete，向 TCP 写入两批数据；确认第二次 `WSARecv` 的提交不依赖 callback 直接执行，而由 worker maintenance/backpressure 决定。测试可通过新增 test hook 暴露 `InFlightTcpRecvs` / `TcpReadPausedByQuicBacklog`。

关键断言：

```cpp
if (snapshot.ActiveRelayStates.front().TcpReadPausedByQuicBacklog) {
    return false;
}
```

`TcpReadPausedByQuicBacklog` 字段在 Task 5 补齐；本 task 可以先写 send complete 不直接 post 的更小测试：触发 `SEND_COMPLETE` 后只看到 `QuicSendCompleteEvents` 增加，不在 callback 栈内调用 `PostTcpRecv` test hook。

- [ ] **Step 2: 定义 `TqWindowsQuicSendOperation`**

在 `windows_relay_worker.cpp` 或独立内部 header 添加：

```cpp
struct TqWindowsQuicSendOperation {
    static constexpr uint64_t MagicValue = 0x54515753454e4431ull;
    uint64_t Magic{MagicValue};
    uint64_t RelayId{0};
    uint64_t TotalBytes{0};
    bool Fin{false};
    QUIC_SEND_FLAGS Flags{QUIC_SEND_FLAG_NONE};
    std::vector<TqBufferRef> Buffers;
    std::vector<QUIC_BUFFER> QuicBuffers;
};
```

- [ ] **Step 3: 修改 `PostQuicSend` 为 `TrySubmitQuicSendOperation`**

新增方法：

```cpp
bool TqWindowsRelayWorker::TrySubmitQuicSendOperation(
    const std::shared_ptr<RelayContext>& relay,
    TqWindowsQuicSendOperation* operation);
```

提交成功时：

```cpp
    relay->InFlightQuicSends.fetch_add(1, std::memory_order_acq_rel);
    relay->OutstandingQuicSendBytes.fetch_add(operation->TotalBytes, std::memory_order_acq_rel);
```

backpressure status 时：

```cpp
    {
        std::lock_guard<std::mutex> guard(relay->PendingQuicSendLock);
        relay->PendingQuicSends.emplace_back(operation);
    }
    SetTcpReadBackpressure(relay, true, "quic_send_resource");
    return true;
```

`PendingQuicSends` 类型改为：

```cpp
std::deque<std::unique_ptr<TqWindowsQuicSendOperation>> PendingQuicSends;
```

- [ ] **Step 4: 修改 `HandleTcpRecv` 构建 send operation**

非压缩路径使用 `BufferOwner` ownership：

```cpp
auto sendOp = std::make_unique<TqWindowsQuicSendOperation>();
sendOp->RelayId = relay->Id;
sendOp->Flags = QUIC_SEND_FLAG_NONE;
sendOp->TotalBytes = op->BufferOwner->Length();
sendOp->Buffers.push_back(std::move(op->BufferOwner));
QUIC_BUFFER buffer{};
buffer.Buffer = sendOp->Buffers.back()->Data();
buffer.Length = static_cast<uint32_t>(sendOp->Buffers.back()->Length());
sendOp->QuicBuffers.push_back(buffer);
if (!TrySubmitQuicSendOperation(relay, sendOp.release())) {
    CloseRelay(relay, TqRelayCloseMode::GracefulDrain, "post_quic_send_failed");
    return;
}
MaybePostTcpRecv(relay);
```

压缩路径把 compressed vector 拷入 `TqBufferRef` 或保留 `std::vector<uint8_t>` 字段；选择一种 ownership，不得引用局部变量。

- [ ] **Step 5: 修改 `StreamCallback::SEND_COMPLETE`**

替换原有直接处理和 `PostTcpRecv` 逻辑：

```cpp
if (event->Type == QUIC_STREAM_EVENT_SEND_COMPLETE) {
    auto* sendOp = static_cast<TqWindowsQuicSendOperation*>(
        event->SEND_COMPLETE.ClientContext);
    TqWindowsRelayEvent queued{};
    queued.Type = TqWindowsRelayEventType::QuicSendComplete;
    queued.RelayId = sendOp != nullptr ? sendOp->RelayId : binding->RelayId;
    queued.SendOperation = sendOp;
    if (!worker->EnqueueEvent(std::move(queued))) {
        delete sendOp;
    }
    return QUIC_STATUS_SUCCESS;
}
```

不要在 callback 中调用 `RetryPendingQuicSends`、`CloseRelayIfDrained`、`PostTcpRecv`。

- [ ] **Step 6: 实现 `ProcessQuicSendComplete`**

```cpp
void TqWindowsRelayWorker::ProcessQuicSendComplete(TqWindowsRelayEvent& event) {
    std::unique_ptr<TqWindowsQuicSendOperation> operation(event.SendOperation);
    event.SendOperation = nullptr;
    if (!operation || operation->Magic != TqWindowsQuicSendOperation::MagicValue) {
        return;
    }
    auto relay = FindRelayById(operation->RelayId);
    if (relay) {
        relay->InFlightQuicSends.fetch_sub(1, std::memory_order_acq_rel);
        SaturatingFetchSub(relay->OutstandingQuicSendBytes, operation->TotalBytes);
        if (operation->Fin) {
            relay->QuicSendFinCompleted.store(true, std::memory_order_release);
        }
        RetryPendingQuicSends(relay);
        MaybePostTcpRecv(relay);
        (void)CloseRelayIfDrained(relay);
    }
}
```

- [ ] **Step 7: 运行测试**

Run:

```powershell
MSBuild build-x64\src\tcpquic_windows_relay_worker_test.vcxproj /p:Configuration=Release /p:Platform=x64 /t:ClCompile
build-x64\bin\Release\tcpquic_windows_relay_worker_test.exe
```

Expected:

```text
SEND_COMPLETE 不再直接恢复 TCP recv；send accounting 仍能归零。
```

- [ ] **Step 8: Commit**

```bash
git add src/tunnel/windows_relay_worker.h src/tunnel/windows_relay_worker.cpp src/unittest/windows_relay_worker_test.cpp
git commit -m "refactor(windows): process QUIC send completions on relay worker"
```

---

## Phase 5 — TCP read 背压状态机

### Task 5: `MaybePostTcpRecv` 与高低水位

**Files:**
- Modify: `src/tunnel/windows_relay_worker.h`
- Modify: `src/tunnel/windows_relay_worker.cpp`
- Test: `src/unittest/windows_relay_worker_test.cpp`

- [ ] **Step 1: 扩展 relay 状态**

在 `RelayContext` 添加：

```cpp
    std::atomic<bool> TcpRecvPosted{false};
    std::atomic<bool> TcpReadPausedByQuicBacklog{false};
    std::atomic<uint64_t> OutstandingQuicSendBytes{0};
    std::atomic<uint64_t> MaxOutstandingQuicSendBytes{0};
    std::atomic<uint64_t> IdealSendBufferBytes{0};
```

- [ ] **Step 2: 新增 helper 声明**

```cpp
    uint64_t CurrentRelayIdealSendBytes(const std::shared_ptr<RelayContext>& relay) const;
    bool ShouldPauseTcpReadForQuicBacklog(const std::shared_ptr<RelayContext>& relay) const;
    bool ShouldResumeTcpReadForQuicBacklog(const std::shared_ptr<RelayContext>& relay) const;
    void SetTcpReadBackpressure(
        const std::shared_ptr<RelayContext>& relay,
        bool paused,
        const char* reason);
    bool MaybePostTcpRecv(const std::shared_ptr<RelayContext>& relay);
```

- [ ] **Step 3: 实现 helper**

```cpp
uint64_t TqWindowsRelayWorker::CurrentRelayIdealSendBytes(
    const std::shared_ptr<RelayContext>& relay) const {
    if (!relay) {
        return 0;
    }
    const uint64_t ideal = relay->IdealSendBufferBytes.load(std::memory_order_acquire);
    if (ideal != 0) {
        return ideal;
    }
    return relay->Tuning.WindowsRelayMaxBufferedQuicSendBytes;
}

bool TqWindowsRelayWorker::ShouldPauseTcpReadForQuicBacklog(
    const std::shared_ptr<RelayContext>& relay) const {
    const uint64_t threshold = CurrentRelayIdealSendBytes(relay);
    return relay && threshold != 0 &&
        relay->OutstandingQuicSendBytes.load(std::memory_order_acquire) >= threshold;
}

bool TqWindowsRelayWorker::ShouldResumeTcpReadForQuicBacklog(
    const std::shared_ptr<RelayContext>& relay) const {
    const uint64_t threshold = CurrentRelayIdealSendBytes(relay);
    if (!relay || threshold == 0) {
        return true;
    }
    return relay->OutstandingQuicSendBytes.load(std::memory_order_acquire) < threshold / 2;
}

void TqWindowsRelayWorker::SetTcpReadBackpressure(
    const std::shared_ptr<RelayContext>& relay,
    bool paused,
    const char* reason) {
    if (!relay) {
        return;
    }
    if (relay->TcpReadPausedByQuicBacklog.exchange(paused, std::memory_order_acq_rel) == paused) {
        return;
    }
    TraceRelayBackpressure(relay, paused ? "pause_tcp_read" : "resume_tcp_read", reason);
}
```

- [ ] **Step 4: 实现 `MaybePostTcpRecv`**

```cpp
bool TqWindowsRelayWorker::MaybePostTcpRecv(const std::shared_ptr<RelayContext>& relay) {
    if (!relay || relay->Closing.load(std::memory_order_acquire) ||
        relay->TcpRecvClosed.load(std::memory_order_acquire)) {
        return false;
    }
    if (ShouldPauseTcpReadForQuicBacklog(relay)) {
        SetTcpReadBackpressure(relay, true, "quic_send_backlog");
        return false;
    }
    if (relay->TcpReadPausedByQuicBacklog.load(std::memory_order_acquire) &&
        !ShouldResumeTcpReadForQuicBacklog(relay)) {
        return false;
    }
    SetTcpReadBackpressure(relay, false, "quic_send_backlog");
    bool expected = false;
    if (!relay->TcpRecvPosted.compare_exchange_strong(
            expected, true, std::memory_order_acq_rel, std::memory_order_acquire)) {
        return true;
    }
    if (!PostTcpRecv(relay)) {
        relay->TcpRecvPosted.store(false, std::memory_order_release);
        return false;
    }
    return true;
}
```

`HandleTcpRecv` completion 开始处清：

```cpp
relay->TcpRecvPosted.store(false, std::memory_order_release);
```

- [ ] **Step 5: 注册 relay 改为 `MaybePostTcpRecv`**

将 `RegisterRelay` 末尾：

```cpp
return PostTcpRecv(relay);
```

替换为：

```cpp
return MaybePostTcpRecv(relay);
```

- [ ] **Step 6: 移除 `SEND_COMPLETE` 里的 recv 恢复**

确认 `StreamCallback::SEND_COMPLETE` 不再调用：

```cpp
PostTcpRecv(relay)
```

恢复点只在：

- `HandleTcpRecv` 提交 QUIC send 后
- `ProcessQuicSendComplete`
- `RetryPendingQuicSends`
- `HandleQuicIdealSendBuffer`
- `DrainPerRelayMaintenance`

- [ ] **Step 7: 新增高低水位测试**

构造 relay，设置：

```cpp
relay->Tuning.WindowsRelayMaxBufferedQuicSendBytes = 8;
relay->OutstandingQuicSendBytes.store(8);
```

断言 `MaybePostTcpRecv(relay)` 不提交 recv，并置 pause。然后降到 3，处理 `QuicSendComplete`，断言可以恢复。

- [ ] **Step 8: 运行测试**

Run:

```powershell
MSBuild build-x64\src\tcpquic_windows_relay_worker_test.vcxproj /p:Configuration=Release /p:Platform=x64 /t:ClCompile
build-x64\bin\Release\tcpquic_windows_relay_worker_test.exe
```

Expected:

```text
tcp->quic read pause/resume 测试通过；无 send-complete callback 直接 post recv。
```

- [ ] **Step 9: Commit**

```bash
git add src/tunnel/windows_relay_worker.h src/tunnel/windows_relay_worker.cpp src/unittest/windows_relay_worker_test.cpp
git commit -m "feat(windows): gate TCP reads on QUIC send backlog"
```

---

## Phase 6 — QUIC control/teardown 事件迁移

### Task 6: QUIC peer abort/shutdown 统一回 worker

**Files:**
- Modify: `src/tunnel/windows_relay_worker.h`
- Modify: `src/tunnel/windows_relay_worker.cpp`
- Test: `src/unittest/windows_relay_worker_test.cpp`

- [ ] **Step 1: 写 late abort 降级测试**

构造 relay，使：

```cpp
PendingQuicReceiveBytes == 0
QueuedQuicReceives == 0
InFlightTcpSends == 0
InFlightQuicSends == 0
```

注入 `QUIC_STREAM_EVENT_PEER_RECEIVE_ABORTED`，drain event queue 后断言：

```cpp
snapshot.FatalRelayResets == beforeFatal;
snapshot.GracefulRelayDrains >= beforeGraceful;
```

- [ ] **Step 2: callback 中只入队 terminal event**

替换：

```cpp
worker->FailRelayFatal(relay, "stream_peer_receive_aborted");
```

为：

```cpp
TqWindowsRelayEvent queued{};
queued.Type = event->Type == QUIC_STREAM_EVENT_PEER_SEND_ABORTED
    ? TqWindowsRelayEventType::QuicPeerSendAborted
    : TqWindowsRelayEventType::QuicPeerReceiveAborted;
queued.RelayId = relay->Id;
queued.Value = event->PEER_RECEIVE_ABORTED.ErrorCode;
(void)worker->EnqueueEvent(std::move(queued));
```

`SHUTDOWN_COMPLETE` 同理入队 `QuicShutdownComplete`。

- [ ] **Step 3: worker 处理 terminal event**

新增：

```cpp
bool TqWindowsRelayWorker::HasPendingAfterStreamShutdown(
    const std::shared_ptr<RelayContext>& relay) const;
void TqWindowsRelayWorker::ProcessQuicPeerAborted(
    uint64_t relayId,
    const char* reason,
    uint64_t errorCode);
void TqWindowsRelayWorker::ProcessQuicShutdownComplete(
    uint64_t relayId,
    uint64_t errorCode,
    uint32_t status);
```

降级规则：

```cpp
if (!HasPendingAfterStreamShutdown(relay)) {
    LateTeardownDowngradedCount_.fetch_add(1, std::memory_order_relaxed);
    CloseRelay(relay, TqRelayCloseMode::GracefulDrain, reason);
    return;
}
relay->CloseAfterDrained.store(true, std::memory_order_release);
(void)CloseRelayIfDrained(relay);
```

- [ ] **Step 4: `HandleTcpPostFailure(10053)` 改为 orderly drain**

当 `IsTcpTeardownError(error)` 且存在 pending/in-flight：

```cpp
const bool hasDownstreamWork =
    relay->InFlightTcpSends.load(std::memory_order_acquire) != 0 ||
    relay->QueuedQuicReceives.load(std::memory_order_acquire) != 0 ||
    relay->PendingQuicReceiveBytes.load(std::memory_order_acquire) != 0;
if (hasDownstreamWork) {
    relay->TcpRecvClosed.store(true, std::memory_order_release);
    relay->CloseAfterDrained.store(true, std::memory_order_release);
    GracefulRelayDrains_.fetch_add(1, std::memory_order_relaxed);
    return true;
}
```

没有 pending work 时仍可 `CloseRelay(..., "wsa_recv_post_failed")`。

- [ ] **Step 5: 运行测试**

Run:

```powershell
MSBuild build-x64\src\tcpquic_windows_relay_worker_test.vcxproj /p:Configuration=Release /p:Platform=x64 /t:ClCompile
build-x64\bin\Release\tcpquic_windows_relay_worker_test.exe
```

Expected:

```text
late abort 降级；10053 + pending TCP send 不立即硬 close。
```

- [ ] **Step 6: Commit**

```bash
git add src/tunnel/windows_relay_worker.h src/tunnel/windows_relay_worker.cpp src/unittest/windows_relay_worker_test.cpp
git commit -m "fix(windows): process QUIC teardown on relay worker"
```

---

## Phase 7 — Snapshot 与 trace

### Task 7: 补齐 Windows event/backpressure 可观测性

**Files:**
- Modify: `src/tunnel/windows_relay_worker.h`
- Modify: `src/tunnel/windows_relay_worker.cpp`
- Modify: trace/stat 输出调用点
- Test: `src/unittest/windows_relay_worker_test.cpp`

- [ ] **Step 1: 扩展 snapshot struct**

在 `TqWindowsRelayWorkerSnapshot` 添加：

```cpp
    uint64_t WindowsEventQueueDepth{0};
    uint64_t WindowsEventQueueCapacity{0};
    uint64_t WindowsEventQueueFullCount{0};
    uint64_t WindowsEventQueueWakeCount{0};
    uint64_t WindowsEventQueueWakeFailedCount{0};
    uint64_t EventsProcessed{0};
    uint64_t QuicSendCompleteEvents{0};
    uint64_t ResumeTcpRecvEvents{0};
    uint64_t LateTeardownDowngradedCount{0};
```

在 `TqWindowsRelayActiveSnapshot` 添加：

```cpp
    bool TcpReadPausedByQuicBacklog{false};
    uint64_t OutstandingQuicSendBytes{0};
    uint64_t MaxOutstandingQuicSendBytes{0};
    uint64_t CallbackPendingQuicReceiveDepth{0};
```

- [ ] **Step 2: 填充 snapshot**

在 `Snapshot()` 中：

```cpp
snapshot.WindowsEventQueueDepth = EventQueue_.SizeApprox();
snapshot.WindowsEventQueueCapacity = EventQueue_.Capacity();
snapshot.WindowsEventQueueFullCount = EventQueueFullCount_.load(std::memory_order_relaxed);
snapshot.WindowsEventQueueWakeCount = EventQueueWakeCount_.load(std::memory_order_relaxed);
snapshot.WindowsEventQueueWakeFailedCount = EventQueueWakeFailedCount_.load(std::memory_order_relaxed);
snapshot.EventsProcessed = EventsProcessed_.load(std::memory_order_relaxed);
```

active relay callback pending depth 用 lock 读取。

- [ ] **Step 3: 更新 stats 输出**

找到 `stats_active_relay` 输出位置，将新增字段加入日志参数：

```text
event_queue_depth=%llu callback_pending_quic_receives=%llu tcp_read_paused_by_quic_backlog=%d outstanding_quic_send_bytes=%llu
```

- [ ] **Step 4: 运行测试**

Run:

```powershell
MSBuild build-x64\src\tcpquic_windows_relay_worker_test.vcxproj /p:Configuration=Release /p:Platform=x64 /t:ClCompile
build-x64\bin\Release\tcpquic_windows_relay_worker_test.exe
```

Expected:

```text
snapshot 字段可读；现有 stats 不崩溃。
```

- [ ] **Step 5: Commit**

```bash
git add src/tunnel/windows_relay_worker.h src/tunnel/windows_relay_worker.cpp src/unittest/windows_relay_worker_test.cpp
git commit -m "chore(windows): expose relay event queue metrics"
```

---

## Phase 8 — 代理场景验证

### Task 8: cursor.com/dashboard 手工验证

**Files:**
- Modify: `docs/windows/iocp-bug-cm25.md`
- Modify: `docs/windows/iocp-bug-gpt55.md`

- [ ] **Step 1: 构建 Release**

Run:

```powershell
MSBuild build-x64\src\tcpquic-proxy.vcxproj /p:Configuration=Release /p:Platform=x64 /p:BuildProjectReferences=false /t:ClCompile
MSBuild build-x64\src\tcpquic-proxy.vcxproj /p:Configuration=Release /p:Platform=x64 /p:BuildProjectReferences=false /t:Link
```

Expected:

```text
tcpquic-proxy.exe 链接成功。若遇 LNK1189，记录并使用已有可执行文件验证 ClCompile 产物不可接受。
```

- [ ] **Step 2: 运行 client**

Run:

```powershell
.\build-x64\bin\Release\tcpquic-proxy.exe client `
  --socks-listen 0.0.0.0:11080 `
  --peer 52.74.45.234:8443 `
  --compress off `
  --trace --trace-interval 30 `
  --ca cert\ca.crt
```

Expected:

```text
backend=windows-iocp；trace 输出包含 windows_event_queue_depth / outstanding_quic_send_bytes。
```

- [ ] **Step 3: 浏览器验证**

访问：

```text
https://cursor.com/dashboard
```

Expected:

```text
页面完整加载；没有资源长期 pending；浏览关闭后 active_relays 回落。
```

- [ ] **Step 4: 日志验收**

检查 `build-x64\bin\Release\log\client.log`：

```text
fatal_relay_resets=0
tcp_hard_errors=0
windows_event_queue_depth 不长期保持高位
callback_pending_quic_receive_depth 能回落
cursor.com 大包 tunnel 不出现中途 abrupt relay_stopping
```

- [ ] **Step 5: 更新问题文档**

在 `docs/windows/iocp-bug-cm25.md` 和 `docs/windows/iocp-bug-gpt55.md` 追加验证结果：

```markdown
## Linux-style Windows relay 验证结果（2026-06-25）

- relay event queue: `<观察到的队列深度范围>`
- callback pending receive depth: `<是否回落>`
- fatal relay resets: `<数值>`
- tcp hard errors: `<数值>`
- cursor.com/dashboard: `<通过/失败，失败时记录 tunnel id 和关键 trace>`
```

- [ ] **Step 6: Commit**

```bash
git add docs/windows/iocp-bug-cm25.md docs/windows/iocp-bug-gpt55.md
git commit -m "docs(windows): record linux-style relay validation"
```

---

## 执行顺序

1. Phase 1 只加 queue，保证最小可编译。
2. Phase 2 接入 drain/wake 骨架，但不改业务行为。
3. Phase 3 迁移 `quic->tcp` receive view，解决 IOCP 队列不可观测问题。
4. Phase 4 迁移 `SEND_COMPLETE`，移除 callback 直接 `PostTcpRecv`。
5. Phase 5 加 tcp->quic 高低水位背压。
6. Phase 6 收敛 teardown 语义。
7. Phase 7 补可观测性。
8. Phase 8 做真实网页代理验证。

---

## Self-Review

- Spec coverage：设计文档中的 event queue、quic->tcp fallback、tcp->quic read gating、teardown worker 化、metrics 均有对应 task。
- Placeholder scan：无未完成标记、延后实现字样或未定义文件路径。
- Type consistency：`TqWindowsRelayEventQueue`、`TqWindowsRelayEvent`、`TqWindowsQuicSendOperation` 在前置 task 定义，后续 task 只复用这些名字。
- Test coverage：每个行为阶段都有单测或手工验证；真实 dashboard 验证放在最后，避免用线上场景替代单元测试。
