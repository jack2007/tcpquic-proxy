# Windows Relay Always-Pending Zero-Copy 实施计划

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 参考 Linux always-pending 分支，在 Windows IOCP relay 中实现非压缩 QUIC→TCP deferred receive view：callback 保存 `QUIC_BUFFER` slice 元数据并返回 `QUIC_STATUS_PENDING`，IOCP worker 借用 MsQuic buffer 写 TCP，按实际写入字节调用 `StreamReceiveComplete`。

**Architecture:** 保持 Windows IOCP 架构。Phase 1 建立 always-pending 可观测性、pending receive view、borrowed-buffer `WSASend` 和字节级 receive complete；Phase 2 增加 callback binding、pending budget、pause/resume 和 complete 聚合；Phase 3 先把 Linux 现有 `TqRelayBufferSlot` / `TqBufferHandle` / pending-bytes budget 抽成 OS 无关通用 buffer pool，再让 Windows TCP→QUIC recv buffer 复用该基础组件并验证 IOCP 并发深度。设计文档见 `docs/superpowers/specs/2026-06-12-windows-relay-perf-hotspot-optimization-design.md`。

**Tech Stack:** C++17、Windows Winsock IOCP (`WSARecv`/`WSASend`)、MsQuic C++ API、`QUIC_STATUS_PENDING` receive ownership、`StreamReceiveComplete`、现有 CMake/Ninja Windows 构建、`tcpquic_windows_relay_worker_test`。

---

## 文件结构预览

| 文件 | 职责 |
|------|------|
| `src/tunnel/windows_relay_worker.h` | 新增 snapshot、pending receive view、测试 helper 声明 |
| `src/tunnel/windows_relay_worker.cpp` | IOCP worker、deferred receive view、`StreamReceiveComplete`、budget、callback binding |
| `src/unittest/windows_relay_worker_test.cpp` | Windows always-pending 行为测试 |
| `src/CMakeLists.txt` | 如测试需要 MsQuic test definitions/includes，补充 Windows test target 配置 |
| `src/tunnel/relay_buffer_pool.h/.cpp` 或 `src/common/relay_buffer_pool.h/.cpp` | Phase 3：OS 无关固定 chunk buffer pool，从 Linux pool 抽取 slot/handle/budget 能力 |
| `src/tunnel/linux_relay_buffer_pool.h/.cpp` | Phase 3：改为兼容 wrapper 或迁移到通用 pool，保持 Linux 行为不变 |
| `src/config/tuning.h/.cpp` 或现有 tuning 定义文件 | Phase 2：Windows pending budget / complete batch tuning |
| `src/unittest/tuning_test.cpp` | Phase 2：新增 tuning 默认值与 override 测试 |

---

## Phase 1：Always-pending deferred receive view

### Task 1: 增加 Windows always-pending snapshot 指标

**Files:**
- Modify: `src/tunnel/windows_relay_worker.h`
- Modify: `src/tunnel/windows_relay_worker.cpp`
- Test: `src/unittest/windows_relay_worker_test.cpp`

- [ ] **Step 1: 写 snapshot smoke 测试**

在 `windows_relay_worker_test.cpp` 的现有 start/stop 块后增加：

```cpp
{
    TqWindowsRelayWorker worker;
    assert(worker.Start());
    const TqWindowsRelayWorkerSnapshot snapshot = worker.Snapshot();
    assert(snapshot.Errors == 0);
    assert(snapshot.DeferredReceiveQueued == 0);
    assert(snapshot.DeferredReceiveCompleteBytes == 0);
    assert(snapshot.PendingQuicReceiveBytes == 0);
    worker.Stop();
}
```

- [ ] **Step 2: 运行测试确认红灯**

Run:

```powershell
cmake --build build-win --target tcpquic_windows_relay_worker_test -j 2
.\build-win\src\Debug\tcpquic_windows_relay_worker_test.exe
```

Expected: 编译失败，缺少 `TqWindowsRelayWorkerSnapshot` 和 `Snapshot()`。

- [ ] **Step 3: 在 header 定义 snapshot**

在 `windows_relay_worker.h` 的 class 前新增：

```cpp
struct TqWindowsRelayWorkerSnapshot {
    uint64_t DeferredReceiveQueued{0};
    uint64_t DeferredReceiveBytesQueued{0};
    uint64_t DeferredReceiveCompleteBytes{0};
    uint64_t DeferredReceiveCompletes{0};
    uint64_t DeferredReceiveCompletionFlushes{0};
    uint64_t PendingQuicReceiveBytes{0};
    uint64_t MaxPendingQuicReceiveBytes{0};
    uint64_t PendingQuicReceiveQueueDepth{0};
    uint64_t MaxPendingQuicReceiveQueueDepth{0};
    uint64_t QuicReceivePausedCount{0};
    uint64_t QuicReceiveResumedCount{0};
    uint64_t TcpSendOperationsPosted{0};
    uint64_t TcpSendBytes{0};
    uint64_t TcpSendPartialCompletions{0};
    uint64_t TcpSendWouldBlockOrPendingCount{0};
    uint64_t TcpRecvOperationsCreated{0};
    uint64_t TcpRecvOperationsReused{0};
    uint64_t Errors{0};
};
```

在 `TqWindowsRelayWorker` public 区域新增：

```cpp
TqWindowsRelayWorkerSnapshot Snapshot() const;
```

private 区域新增对应 `std::atomic<uint64_t>` 计数。

- [ ] **Step 4: 实现 Snapshot**

`Snapshot()` 读取 worker 原子计数，并在 `Lock_` 下遍历 `Relays_` 聚合当前 pending bytes/queue：

```cpp
TqWindowsRelayWorkerSnapshot TqWindowsRelayWorker::Snapshot() const {
    TqWindowsRelayWorkerSnapshot snapshot{};
    snapshot.DeferredReceiveQueued = DeferredReceiveQueued_.load(std::memory_order_relaxed);
    snapshot.DeferredReceiveBytesQueued = DeferredReceiveBytesQueued_.load(std::memory_order_relaxed);
    snapshot.DeferredReceiveCompleteBytes = DeferredReceiveCompleteBytes_.load(std::memory_order_relaxed);
    snapshot.DeferredReceiveCompletes = DeferredReceiveCompletes_.load(std::memory_order_relaxed);
    snapshot.DeferredReceiveCompletionFlushes = DeferredReceiveCompletionFlushes_.load(std::memory_order_relaxed);
    snapshot.MaxPendingQuicReceiveBytes = MaxPendingQuicReceiveBytesObserved_.load(std::memory_order_relaxed);
    snapshot.MaxPendingQuicReceiveQueueDepth = MaxPendingQuicReceiveQueueObserved_.load(std::memory_order_relaxed);
    snapshot.QuicReceivePausedCount = QuicReceivePausedCount_.load(std::memory_order_relaxed);
    snapshot.QuicReceiveResumedCount = QuicReceiveResumedCount_.load(std::memory_order_relaxed);
    snapshot.TcpSendOperationsPosted = TcpSendOperationsPosted_.load(std::memory_order_relaxed);
    snapshot.TcpSendBytes = TcpSendBytes_.load(std::memory_order_relaxed);
    snapshot.TcpSendPartialCompletions = TcpSendPartialCompletions_.load(std::memory_order_relaxed);
    snapshot.TcpSendWouldBlockOrPendingCount = TcpSendWouldBlockOrPendingCount_.load(std::memory_order_relaxed);
    snapshot.TcpRecvOperationsCreated = TcpRecvOperationsCreated_.load(std::memory_order_relaxed);
    snapshot.TcpRecvOperationsReused = TcpRecvOperationsReused_.load(std::memory_order_relaxed);
    snapshot.Errors = Errors_.load(std::memory_order_relaxed);
    {
        std::lock_guard<std::mutex> guard(Lock_);
        for (const auto& entry : Relays_) {
            if (entry.second) {
                snapshot.PendingQuicReceiveBytes += entry.second->PendingQuicReceiveBytes.load(std::memory_order_relaxed);
                snapshot.PendingQuicReceiveQueueDepth += entry.second->PendingQuicReceiveQueueDepth.load(std::memory_order_relaxed);
            }
        }
    }
    return snapshot;
}
```

- [ ] **Step 5: 运行测试**

Run:

```powershell
cmake --build build-win --target tcpquic_windows_relay_worker_test -j 2
.\build-win\src\Debug\tcpquic_windows_relay_worker_test.exe
```

Expected: PASS。

- [ ] **Step 6: Commit**

```bash
git add src/tunnel/windows_relay_worker.h src/tunnel/windows_relay_worker.cpp src/unittest/windows_relay_worker_test.cpp
git commit -m "test(relay): add Windows deferred receive metrics"
```

---

### Task 2: 定义 pending receive view 并让 RECEIVE 返回 PENDING

**Files:**
- Modify: `src/tunnel/windows_relay_worker.cpp`
- Modify: `src/tunnel/windows_relay_worker.h` if private declarations move
- Test: `src/unittest/windows_relay_worker_test.cpp`

- [ ] **Step 1: 写 RECEIVE pending 测试**

注册 relay 后构造两个 `QUIC_BUFFER` 的 receive event，调用 `StreamCallback`，断言返回 `QUIC_STATUS_PENDING`，且 snapshot 只记录 pending view，不记录 owned receive copy。

```cpp
{
    TqWindowsRelayWorker worker;
    assert(worker.Start());

    SOCKET fds[2]{INVALID_SOCKET, INVALID_SOCKET};
    assert(TqCreateLoopbackSocketPairForTest(fds));

    MsQuicStream stream{};
    TqRelayHandle handle{};
    TqTuningConfig tuning{};
    tuning.RelayIoSize = 64 * 1024;

    assert(worker.RegisterRelay(fds[0], &stream, nullptr, nullptr, &handle, tuning, TqCompressAlgo::None));

    uint8_t a[] = {1, 2, 3};
    uint8_t b[] = {4, 5};
    QUIC_BUFFER buffers[2]{};
    buffers[0].Buffer = a;
    buffers[0].Length = sizeof(a);
    buffers[1].Buffer = b;
    buffers[1].Length = sizeof(b);

    QUIC_STREAM_EVENT event{};
    event.Type = QUIC_STREAM_EVENT_RECEIVE;
    event.RECEIVE.BufferCount = 2;
    event.RECEIVE.Buffers = buffers;

    const QUIC_STATUS status = TqWindowsRelayWorker::StreamCallback(&stream, stream.Context, &event);
    assert(status == QUIC_STATUS_PENDING);

    const auto snapshot = worker.Snapshot();
    assert(snapshot.DeferredReceiveQueued == 1);
    assert(snapshot.DeferredReceiveBytesQueued == 5);
    assert(snapshot.PendingQuicReceiveQueueDepth == 1);
    assert(snapshot.PendingQuicReceiveBytes == 5);

    worker.StopRelay(handle.WindowsRelayId);
    worker.Stop();
    TqCloseSocket(fds[1]);
}
```

如果没有 Windows loopback socket pair helper，先在测试文件内实现：bind localhost port 0、listen、overlapped connect/accept，并返回两端 `SOCKET`。

- [ ] **Step 2: 运行测试确认红灯**

Expected: 当前实现返回 `QUIC_STATUS_SUCCESS` 且 copy payload，测试失败。

- [ ] **Step 3: 添加数据结构**

在 `windows_relay_worker.cpp` 内部新增：

```cpp
struct TqWindowsQuicReceiveSlice {
    const uint8_t* Data{nullptr};
    uint32_t Length{0};
};

struct TqWindowsPendingQuicReceive {
    MsQuicStream* Stream{nullptr};
    uint64_t RelayId{0};
    std::vector<TqWindowsQuicReceiveSlice> Slices;
    size_t SliceIndex{0};
    size_t SliceOffset{0};
    uint64_t TotalLength{0};
    uint64_t CompletedLength{0};
    uint64_t PendingCompleteBytes{0};
    bool Fin{false};
};
```

扩展 `IoOperation`：

```cpp
std::shared_ptr<TqWindowsPendingQuicReceive> ReceiveView;
```

扩展 `RelayContext`：

```cpp
std::atomic<uint64_t> PendingQuicReceiveBytes{0};
std::atomic<uint64_t> PendingQuicReceiveQueueDepth{0};
```

新增 event：

```cpp
QuicReceiveViewQueued,
```

- [ ] **Step 4: 实现 QueueDeferredQuicReceive**

新增私有方法：

```cpp
bool TqWindowsRelayWorker::QueueDeferredQuicReceive(
    const std::shared_ptr<RelayContext>& relay,
    MsQuicStream* stream,
    const QUIC_BUFFER* buffers,
    uint32_t bufferCount,
    bool fin);
```

核心逻辑：

```cpp
auto view = std::make_shared<TqWindowsPendingQuicReceive>();
view->Stream = stream;
view->RelayId = relay->Id;
view->Fin = fin;
view->Slices.reserve(bufferCount);
for (uint32_t i = 0; i < bufferCount; ++i) {
    const auto& buffer = buffers[i];
    if (buffer.Length != 0 && buffer.Buffer == nullptr) {
        Errors_.fetch_add(1, std::memory_order_relaxed);
        return false;
    }
    if (buffer.Length == 0) {
        continue;
    }
    view->Slices.push_back(TqWindowsQuicReceiveSlice{buffer.Buffer, buffer.Length});
    view->TotalLength += buffer.Length;
}
if (view->TotalLength == 0 && !fin) {
    return true;
}

auto op = std::make_unique<IoOperation>();
op->Event = TqWindowsRelayEvent::QuicReceiveViewQueued;
op->Relay = relay;
op->ReceiveView = view;

relay->PendingQuicReceiveBytes.fetch_add(view->TotalLength, std::memory_order_relaxed);
relay->PendingQuicReceiveQueueDepth.fetch_add(1, std::memory_order_relaxed);
DeferredReceiveQueued_.fetch_add(1, std::memory_order_relaxed);
DeferredReceiveBytesQueued_.fetch_add(view->TotalLength, std::memory_order_relaxed);
UpdateAtomicMax(MaxPendingQuicReceiveBytesObserved_, relay->PendingQuicReceiveBytes.load(std::memory_order_relaxed));
UpdateAtomicMax(MaxPendingQuicReceiveQueueObserved_, relay->PendingQuicReceiveQueueDepth.load(std::memory_order_relaxed));

IoOperation* raw = op.release();
if (!::PostQueuedCompletionStatus(static_cast<HANDLE>(Iocp_), 0, 0, &raw->Overlapped)) {
    // rollback accounting, delete raw, return false
}
return true;
```

- [ ] **Step 5: StreamCallback(RECEIVE) 使用 deferred view**

非压缩路径：

```cpp
if (event->Type == QUIC_STREAM_EVENT_RECEIVE) {
    const bool fin = (event->RECEIVE.Flags & QUIC_RECEIVE_FLAG_FIN) != 0;
    if (relay->Decompressor == nullptr && relay->CompressAlgo == TqCompressAlgo::None) {
        if (!callback->Worker->QueueDeferredQuicReceive(
                relay, stream, event->RECEIVE.Buffers, event->RECEIVE.BufferCount, fin)) {
            callback->Worker->CloseRelay(relay);
            return QUIC_STATUS_SUCCESS;
        }
        return QUIC_STATUS_PENDING;
    }
    // 压缩路径保留旧 copy + QUIC_STATUS_SUCCESS
}
```

- [ ] **Step 6: 运行测试**

Run:

```powershell
cmake --build build-win --target tcpquic_windows_relay_worker_test -j 2
.\build-win\src\Debug\tcpquic_windows_relay_worker_test.exe
```

Expected: PASS。

- [ ] **Step 7: Commit**

```bash
git add src/tunnel/windows_relay_worker.h src/tunnel/windows_relay_worker.cpp src/unittest/windows_relay_worker_test.cpp
git commit -m "perf(relay): queue Windows QUIC receives as pending views"
```

---

### Task 3: IOCP worker 用 borrowed QUIC buffer 写 TCP 并 complete

**Files:**
- Modify: `src/tunnel/windows_relay_worker.cpp`
- Test: `src/unittest/windows_relay_worker_test.cpp`

- [ ] **Step 1: 写 receive complete 测试**

测试应验证：

1. `StreamCallback(RECEIVE)` 返回 `QUIC_STATUS_PENDING`。
2. IOCP worker 将 borrowed payload 写到 TCP peer。
3. `StreamReceiveComplete` 被调用 5 字节。
4. pending bytes/queue 清零。

如果 `MsQuicStream` test double 当前没有记录 complete bytes，给测试 stub 增加字段或 helper。测试断言：

```cpp
assert(stream.ReceiveCompleteBytesForTest == 5);
assert(worker.Snapshot().DeferredReceiveCompleteBytes == 5);
assert(worker.Snapshot().PendingQuicReceiveBytes == 0);
```

- [ ] **Step 2: 运行测试确认红灯**

Expected: view 已入队，但 worker 尚未处理 borrowed-buffer send/complete，测试失败。

- [ ] **Step 3: Run switch 增加 QuicReceiveViewQueued**

```cpp
case TqWindowsRelayEvent::QuicReceiveViewQueued:
    HandleQuicReceiveViewQueued(std::move(op));
    break;
```

- [ ] **Step 4: 实现 PostTcpSendFromReceiveView**

新增私有方法：

```cpp
bool TqWindowsRelayWorker::PostTcpSendFromReceiveView(
    const std::shared_ptr<RelayContext>& relay,
    const std::shared_ptr<TqWindowsPendingQuicReceive>& view);
```

核心逻辑：

```cpp
while (view->SliceIndex < view->Slices.size() &&
       view->SliceOffset >= view->Slices[view->SliceIndex].Length) {
    view->SliceOffset = 0;
    ++view->SliceIndex;
}
if (view->SliceIndex >= view->Slices.size()) {
    FlushDeferredReceiveCompletion(*view, true);
    FinishReceiveView(relay, view);
    return true;
}
const auto& slice = view->Slices[view->SliceIndex];
auto op = std::make_unique<IoOperation>();
op->Event = TqWindowsRelayEvent::TcpSend;
op->Relay = relay;
op->ReceiveView = view;
op->Buffer.buf = reinterpret_cast<char*>(const_cast<uint8_t*>(slice.Data + view->SliceOffset));
op->Buffer.len = static_cast<ULONG>(slice.Length - view->SliceOffset);
DWORD sent = 0;
IoOperation* raw = op.release();
TcpSendOperationsPosted_.fetch_add(1, std::memory_order_relaxed);
const int rc = ::WSASend(relay->TcpFd, &raw->Buffer, 1, &sent, 0, &raw->Overlapped, nullptr);
if (rc != 0 && ::WSAGetLastError() != WSA_IO_PENDING) {
    delete raw;
    Errors_.fetch_add(1, std::memory_order_relaxed);
    return false;
}
TcpSendWouldBlockOrPendingCount_.fetch_add(1, std::memory_order_relaxed);
return true;
```

- [ ] **Step 5: HandleQuicReceiveViewQueued 调用 PostTcpSendFromReceiveView**

```cpp
void TqWindowsRelayWorker::HandleQuicReceiveViewQueued(std::unique_ptr<IoOperation> op) {
    auto relay = op->Relay;
    auto view = op->ReceiveView;
    if (!relay || !view || relay->Closing.load()) {
        if (view) {
            FlushDeferredReceiveCompletion(*view, true);
        }
        return;
    }
    if (!PostTcpSendFromReceiveView(relay, view)) {
        FlushDeferredReceiveCompletion(*view, true);
        CloseRelay(relay);
    }
}
```

- [ ] **Step 6: HandleTcpSend 推进 receive view**

在 `HandleTcpSend` 开头区分 `op->ReceiveView`：

```cpp
if (op->ReceiveView) {
    auto view = op->ReceiveView;
    TcpSendBytes_.fetch_add(bytes, std::memory_order_relaxed);
    AdvanceReceiveView(*view, bytes);
    if (bytes < op->Buffer.len) {
        TcpSendPartialCompletions_.fetch_add(1, std::memory_order_relaxed);
    }
    FlushDeferredReceiveCompletion(*view, false);
    if (!PostTcpSendFromReceiveView(relay, view)) {
        CloseRelay(relay);
    }
    return;
}
```

`AdvanceReceiveView` 必须只按 completion bytes 推进，不可使用 attempted len。

- [ ] **Step 7: 实现 StreamReceiveComplete flush**

```cpp
void TqWindowsRelayWorker::FlushDeferredReceiveCompletion(
    TqWindowsPendingQuicReceive& view,
    bool force) {
    const uint64_t threshold = ConfiguredCompleteBatchBytes();
    if (view.PendingCompleteBytes == 0) {
        return;
    }
    if (!force && threshold != 0 && view.PendingCompleteBytes < threshold) {
        return;
    }
    view.Stream->ReceiveComplete(static_cast<uint64_t>(view.PendingCompleteBytes));
    DeferredReceiveCompleteBytes_.fetch_add(view.PendingCompleteBytes, std::memory_order_relaxed);
    DeferredReceiveCompletes_.fetch_add(1, std::memory_order_relaxed);
    DeferredReceiveCompletionFlushes_.fetch_add(1, std::memory_order_relaxed);
    view.PendingCompleteBytes = 0;
}
```

如果 wrapper 方法名不是 `ReceiveComplete`，使用当前 `MsQuicStream` 中对等方法；计划执行时必须以实际 API 为准。

- [ ] **Step 8: FinishReceiveView 释放 pending budget**

```cpp
void TqWindowsRelayWorker::FinishReceiveView(
    const std::shared_ptr<RelayContext>& relay,
    const std::shared_ptr<TqWindowsPendingQuicReceive>& view) {
    relay->PendingQuicReceiveBytes.fetch_sub(view->TotalLength, std::memory_order_relaxed);
    relay->PendingQuicReceiveQueueDepth.fetch_sub(1, std::memory_order_relaxed);
    if (view->Fin && TqSocketValid(relay->TcpFd)) {
        (void)TqShutdownSend(relay->TcpFd);
    }
    CloseRelayIfDrained(relay);
}
```

- [ ] **Step 9: 运行测试**

Run:

```powershell
cmake --build build-win --target tcpquic_windows_relay_worker_test -j 2
.\build-win\src\Debug\tcpquic_windows_relay_worker_test.exe
```

Expected: PASS。

- [ ] **Step 10: Commit**

```bash
git add src/tunnel/windows_relay_worker.cpp src/unittest/windows_relay_worker_test.cpp
git commit -m "perf(relay): complete Windows pending receives after TCP send"
```

---

## Phase 2：Binding、budget 与 complete 聚合

### Task 4: CallbackBinding 替代 hot path weak_ptr::lock

**Files:**
- Modify: `src/tunnel/windows_relay_worker.cpp`
- Modify: `src/tunnel/windows_relay_worker.h` if private declarations change
- Test: `src/unittest/windows_relay_worker_test.cpp`

- [ ] **Step 1: 添加并发 close/callback 测试**

注册 relay 后，一个线程循环构造空 receive event 调用 `StreamCallback`，另一个线程调用 `StopRelay`/`Stop`。断言无崩溃，最终 `handle.Stop == true` 或 relay 已关闭。

- [ ] **Step 2: 新增 CallbackBinding**

```cpp
struct TqWindowsRelayWorker::CallbackBinding {
    TqWindowsRelayWorker* Worker{nullptr};
    std::atomic<RelayContext*> Relay{nullptr};
    std::atomic<uint32_t> CallbackRefs{0};
    std::atomic<bool> Closing{false};
};
```

`RegisterRelay`：

```cpp
relay->Callback = std::make_unique<CallbackBinding>();
relay->Callback->Worker = this;
relay->Callback->Relay.store(relay.get(), std::memory_order_release);
stream->Callback = StreamCallback;
stream->Context = relay->Callback.get();
```

- [ ] **Step 3: StreamCallback 使用 binding refs**

```cpp
auto* binding = static_cast<TqWindowsRelayWorker::CallbackBinding*>(context);
if (binding == nullptr || binding->Worker == nullptr || event == nullptr) {
    return QUIC_STATUS_SUCCESS;
}
if (binding->Closing.load(std::memory_order_acquire)) {
    return QUIC_STATUS_SUCCESS;
}
binding->CallbackRefs.fetch_add(1, std::memory_order_acq_rel);
auto refsGuard = TqScopeExit([&] { binding->CallbackRefs.fetch_sub(1, std::memory_order_acq_rel); });
RelayContext* relayRaw = binding->Relay.load(std::memory_order_acquire);
if (relayRaw == nullptr || relayRaw->Closing.load(std::memory_order_acquire)) {
    return QUIC_STATUS_SUCCESS;
}
```

如果 operation 需要 `shared_ptr<RelayContext>`，通过 relay id 在 worker map 中短暂获取；不要在每个 callback 用 `weak_ptr::lock()` 作为唯一定位方式。

- [ ] **Step 4: CloseRelay 关闭 binding 并 flush pending views**

`CloseRelay` 必须：

1. `Closing=true`。
2. binding `Closing=true`、`Relay=nullptr`。
3. 恢复 stream no-op callback。
4. 对所有 pending views 强制 `FlushDeferredReceiveCompletion(force=true)`。
5. 清空 pending budget。

- [ ] **Step 5: 测试 + commit**

Run:

```powershell
cmake --build build-win --target tcpquic_windows_relay_worker_test -j 2
.\build-win\src\Debug\tcpquic_windows_relay_worker_test.exe
```

Expected: PASS。

Commit:

```bash
git add src/tunnel/windows_relay_worker.h src/tunnel/windows_relay_worker.cpp src/unittest/windows_relay_worker_test.cpp
git commit -m "perf(relay): add Windows stream callback binding"
```

---

### Task 5: Pending receive budget 与 receive pause/resume

**Files:**
- Modify: `src/tunnel/windows_relay_worker.cpp`
- Modify: tuning files if adding config fields
- Test: `src/unittest/windows_relay_worker_test.cpp`

- [ ] **Step 1: 写 pending budget 测试**

设置 per-relay pending budget 为 4 字节，发送 5 字节 receive，断言触发 pause/resume 或关闭 fallback：

```cpp
const auto snapshot = worker.Snapshot();
assert(snapshot.QuicReceivePausedCount == 1 || snapshot.Errors >= 1);
```

- [ ] **Step 2: 新增 tuning 字段**

建议新增：

```cpp
uint64_t WindowsRelayMaxPendingQuicReceiveBytesPerRelay{16ull * 1024 * 1024};
uint64_t WindowsRelayQuicReceiveCompleteBatchBytes{0};
```

`0` 表示写多少 complete 多少。

- [ ] **Step 3: QueueDeferredQuicReceive 入队前检查预算**

```cpp
const uint64_t pending = relay->PendingQuicReceiveBytes.load(std::memory_order_relaxed);
if (pending + view->TotalLength > relay->MaxPendingQuicReceiveBytes) {
    SetQuicReceiveEnabled(relay.get(), false);
    QuicReceivePausedCount_.fetch_add(1, std::memory_order_relaxed);
    return false;
}
```

如果 Windows MsQuic wrapper 暂无 receive enable API，则第一版 close relay fallback：记录 `Errors_` 和 `QuicReceivePausedCount_`，关闭 relay，且不要返回 `PENDING` 后丢 view。

- [ ] **Step 4: FinishReceiveView 低水位恢复**

pending bytes 降到高水位一半以下：

```cpp
if (relay->QuicReceivePaused && relay->PendingQuicReceiveBytes.load() < relay->MaxPendingQuicReceiveBytes / 2) {
    SetQuicReceiveEnabled(relay.get(), true);
    QuicReceiveResumedCount_.fetch_add(1, std::memory_order_relaxed);
}
```

- [ ] **Step 5: 测试 + commit**

Run:

```powershell
cmake --build build-win --target tcpquic_windows_relay_worker_test tcpquic_tuning_test -j 2
.\build-win\src\Debug\tcpquic_windows_relay_worker_test.exe
.\build-win\src\Debug\tcpquic_tuning_test.exe
```

Expected: PASS。

Commit:

```bash
git add src/tunnel/windows_relay_worker.cpp src/config src/unittest/windows_relay_worker_test.cpp src/unittest/tuning_test.cpp
git commit -m "perf(relay): bound Windows pending QUIC receives"
```

---

### Task 6: StreamReceiveComplete 聚合阈值

**Files:**
- Modify: `src/tunnel/windows_relay_worker.cpp`
- Modify: tuning files
- Test: `src/unittest/windows_relay_worker_test.cpp`

- [ ] **Step 1: 写 complete 聚合测试**

配置 `WindowsRelayQuicReceiveCompleteBatchBytes = 4`，发送 5 字节，但模拟两次 TCP completion：2 字节、3 字节。

断言：

```cpp
assert(stream.ReceiveCompleteCallsForTest == 1); // 第二次达到/完成后 flush
assert(stream.ReceiveCompleteBytesForTest == 5);
```

再测试 close/unregister 强制 flush：发送 3 字节并完成 2 字节，未达阈值，StopRelay 后必须 complete 2 字节。

- [ ] **Step 2: FlushDeferredReceiveCompletion 使用阈值**

```cpp
if (!force && threshold != 0 && view.PendingCompleteBytes < threshold) {
    return;
}
```

view 完成、错误、close/unregister 必须 `force=true`。

- [ ] **Step 3: 测试 + commit**

Run:

```powershell
cmake --build build-win --target tcpquic_windows_relay_worker_test tcpquic_tuning_test -j 2
.\build-win\src\Debug\tcpquic_windows_relay_worker_test.exe
.\build-win\src\Debug\tcpquic_tuning_test.exe
```

Expected: PASS。

Commit:

```bash
git add src/tunnel/windows_relay_worker.cpp src/config src/unittest/windows_relay_worker_test.cpp src/unittest/tuning_test.cpp
git commit -m "perf(relay): batch Windows receive completions"
```

---

## Phase 3：通用 buffer pool、TCP→QUIC 分配优化与 perf 验证

### Task 7: 抽取 OS 无关 relay buffer pool

**Files:**
- Add: `src/tunnel/relay_buffer_pool.h`
- Add: `src/tunnel/relay_buffer_pool.cpp`
- Modify: `src/tunnel/linux_relay_buffer_pool.h`
- Modify: `src/tunnel/linux_relay_buffer_pool.cpp`
- Modify: `src/CMakeLists.txt`
- Test: `src/unittest/linux_relay_buffer_pool_test.cpp` 或新增 `src/unittest/relay_buffer_pool_test.cpp`

- [ ] **Step 1: 写通用 buffer pool 行为测试**

从现有 `linux_relay_buffer_pool_test.cpp` 中抽取 OS 无关用例，覆盖：

```cpp
{
    TqRelayBufferPool pool(4096, 2, 8192);
    pool.Reserve(1, 1);

    TqBufferAcquireFailure failure = TqBufferAcquireFailure::None;
    auto worker = pool.Acquire(TqBufferDomain::Worker, &failure);
    assert(worker);
    assert(failure == TqBufferAcquireFailure::None);
    assert(worker->Capacity() == 4096);

    auto callback = pool.Acquire(TqBufferDomain::Callback, &failure);
    assert(callback);
    assert(pool.PendingBytes() == 8192);

    auto denied = pool.Acquire(TqBufferDomain::Worker, &failure);
    assert(!denied);
    assert(failure == TqBufferAcquireFailure::PendingBytesLimit ||
           failure == TqBufferAcquireFailure::SlotLimit);
}
```

测试重点不是 Linux worker/ingress 名称，而是固定 chunk、RAII 归还、pending bytes、slot limit、domain free-list 是否正确。

- [ ] **Step 2: 新建通用类型**

把当前 Linux pool 中 OS 无关的类型迁移到 `relay_buffer_pool.h/.cpp`：

```cpp
enum class TqBufferDomain {
    Worker,
    Callback,
};

class TqRelayBufferSlot final { ... };
class TqBufferHandle final { ... };
using TqBufferRef = TqBufferHandle;
struct TqBufferView { ... };
class TqRelayBufferPool final { ... };
```

要求：

- 不包含 Linux/Windows 系统头。
- 不出现 epoll/readv/writev/IOCP/OVERLAPPED 语义。
- domain 名称使用 `Worker` / `Callback` 或可配置 domain，不再固化 Linux `Ingress` 命名。
- 保留 pending-bytes budget、slot limit、allocation failure 语义。
- 保留“worker 域单线程 fast path，callback 域小锁保护”的现有性能假设；如命名为通用 domain，需要在注释中说明并非 OS 约束。

- [ ] **Step 3: Linux pool 保持兼容**

为降低改动风险，`linux_relay_buffer_pool.h` 可以先变成兼容 wrapper：

```cpp
#include "relay_buffer_pool.h"

using TqLinuxRelayBufferPool = TqRelayBufferPool;
```

如果仍需要 `TqBufferDomain::Ingress` 兼容现有 Linux 调用，则提供过渡映射方案：

```cpp
constexpr TqBufferDomain TqIngressBufferDomain = TqBufferDomain::Callback;
```

或者先保留 `Ingress` 枚举值，但在通用 pool 注释中说明它代表 callback/producer domain，不代表 Linux OS 依赖。执行时优先选择最小 diff、确保 Linux relay 和测试不回归。

- [ ] **Step 4: 更新 CMake**

确保通用 pool 编译进 Linux relay、Windows relay 以及相关测试 target：

```cmake
src/tunnel/relay_buffer_pool.cpp
```

- [ ] **Step 5: 运行 Linux buffer pool 与 Windows relay 测试**

Run:

```powershell
cmake --build build-win --target tcpquic_windows_relay_worker_test -j 2
.\build-win\src\Debug\tcpquic_windows_relay_worker_test.exe
```

如当前 Windows 构建包含 Linux buffer pool 测试 target，也运行：

```powershell
cmake --build build-win --target tcpquic_linux_relay_buffer_pool_test -j 2
.\build-win\src\Debug\tcpquic_linux_relay_buffer_pool_test.exe
```

Expected: PASS。

- [ ] **Step 6: Commit**

```bash
git add src/tunnel/relay_buffer_pool.* src/tunnel/linux_relay_buffer_pool.* src/CMakeLists.txt src/unittest/*buffer_pool*_test.cpp
git commit -m "refactor(relay): extract shared relay buffer pool"
```

---

### Task 8: Windows TCP recv buffer 复用通用 pool

**Files:**
- Modify: `src/tunnel/windows_relay_worker.cpp`
- Modify: `src/tunnel/windows_relay_worker.h` if declarations move
- Modify: `src/CMakeLists.txt`
- Test: `src/unittest/windows_relay_worker_test.cpp`

- [ ] **Step 1: 写 Windows buffer reuse 测试**

注册 relay 后，通过测试 helper 触发两次 TCP recv acquire/release，断言：

```cpp
assert(worker.Snapshot().TcpRecvOperationsCreated >= 1);
assert(worker.Snapshot().TcpRecvOperationsReused >= 1);
```

如果 snapshot 后续增加通用 pool 指标，也断言：

```cpp
assert(worker.Snapshot().TcpRecvBufferPoolPendingBytes == 0);
```

- [ ] **Step 2: RelayContext 添加通用 pool**

```cpp
TqRelayBufferPool TcpRecvBuffers;
std::vector<std::unique_ptr<IoOperation>> TcpRecvOpsFree;
```

初始化建议：

```cpp
TcpRecvBuffers(tuning.RelayIoSize, tuning.WindowsRelayTcpRecvSlots, tuning.RelayInflightBytes)
```

如果 tuning 暂无 `WindowsRelayTcpRecvSlots`，先使用固定默认值 2 或复用 `WorkerSlots`，并在后续 tuning 任务中补齐。

- [ ] **Step 3: IoOperation 持有 TqBufferRef**

把 TCP recv 方向的 owned vector buffer 改为通用 pool handle：

```cpp
struct IoOperation {
    OVERLAPPED Overlapped{};
    TqWindowsRelayEvent Event{};
    std::shared_ptr<RelayContext> Relay;
    TqBufferRef BufferOwner;
    WSABUF Buffer{};
    size_t Offset{0};
};
```

`PostTcpRecv`：

```cpp
TqBufferAcquireFailure failure = TqBufferAcquireFailure::None;
auto buffer = relay->TcpRecvBuffers.Acquire(TqBufferDomain::Worker, &failure);
if (!buffer) {
    // record backpressure/allocation failure, close or retry according to existing policy
    return false;
}
op->BufferOwner = std::move(buffer);
op->Buffer.buf = reinterpret_cast<char*>(op->BufferOwner->Data());
op->Buffer.len = static_cast<ULONG>(op->BufferOwner->Capacity());
```

TCP recv completion 后：

```cpp
op->BufferOwner->SetLength(bytes);
Stream->Send(op->BufferOwner->Data(), bytes, ..., ClientContext=op);
```

SEND_COMPLETE 后，`IoOperation` 回 free-list；`TqBufferRef` reset 时自动归还 slot。

- [ ] **Step 4: 保持 deferred QUIC receive 不使用 pool**

非压缩 QUIC→TCP already uses borrowed MsQuic buffer view，不应从通用 pool 分配 payload buffer。压缩路径仍可在后续单独评估是否使用通用 pool 存放 compressed/decompressed owned buffer。

- [ ] **Step 5: 测试 + commit**

Run:

```powershell
cmake --build build-win --target tcpquic_windows_relay_worker_test tcpquic_tunnel_test -j 2
.\build-win\src\Debug\tcpquic_windows_relay_worker_test.exe
.\build-win\src\Debug\tcpquic_tunnel_test.exe
```

Expected: PASS。

Commit:

```bash
git add src/tunnel/windows_relay_worker.* src/CMakeLists.txt src/unittest/windows_relay_worker_test.cpp
git commit -m "perf(relay): reuse shared buffer pool for Windows TCP recv"
```

---

### Task 9: Windows ETW/吞吐验证

**Files:**
- Modify: `docs/superpowers/specs/2026-06-12-windows-relay-perf-hotspot-optimization-design.md` if recording results

- [ ] **Step 1: 编译 Windows relay 相关目标**

Run:

```powershell
cmake --build build-win --target tcpquic-proxy tcpquic_windows_relay_worker_test tcpquic_tunnel_test tcpquic_tuning_test tcpquic_linux_relay_buffer_pool_test -j 2
```

Expected: build succeeds。

- [ ] **Step 2: 运行 Windows 单测**

Run:

```powershell
.\build-win\src\Debug\tcpquic_windows_relay_worker_test.exe
.\build-win\src\Debug\tcpquic_tunnel_test.exe
.\build-win\src\Debug\tcpquic_tuning_test.exe
.\build-win\src\Debug\tcpquic_linux_relay_buffer_pool_test.exe
```

Expected: PASS。

- [ ] **Step 3: 采集 Windows ETW/heap profile**

Run:

```powershell
wpr -start GeneralProfile -start CPU -filemode
# 在另一个终端运行 Windows throughput 用例
wpr -stop windows-relay-always-pending.etl
```

Expected: 生成 ETL，能在 WPA 中查看 CPU、heap allocation、AFD/Winsock 相关热点。

- [ ] **Step 4: 记录结果**

在设计文档末尾追加：

```markdown
## Always-pending Windows 验证结果（YYYY-MM-DD）

- Build: <commit>
- Tests: <commands and PASS/FAIL>
- ETW: <etl path or skipped reason>
- Deferred receive: <complete bytes/calls, pending p95/p99 if available>
- Allocation observation: <relay allocation stacks>
- Decision: <complete threshold / pending budget recommendation>
```

- [ ] **Step 5: Commit**

```bash
git add docs/superpowers/specs/2026-06-12-windows-relay-perf-hotspot-optimization-design.md
git commit -m "docs: record Windows always-pending relay validation"
```

---

## 自检清单

- [ ] 非压缩 QUIC receive callback 返回 `QUIC_STATUS_PENDING`。
- [ ] 压缩 QUIC receive 保持 owned-buffer copy，不借用 MsQuic buffer 给 decompressor。
- [ ] `StreamReceiveComplete` 只按 TCP completion bytes 调用。
- [ ] partial `WSASend` 不 complete 未发送字节。
- [ ] view 完成、close、abort、unregister、error 都强制 flush 已完成字节。
- [ ] pending bytes/queue 在入队、完成、失败、关闭路径上严格配平。
- [ ] 通用 `TqRelayBufferPool` 不包含 Linux/Windows 系统调用或 IO 模型语义。
- [ ] Linux `TqLinuxRelayBufferPool` 迁移/包装后，现有 Linux buffer pool 测试保持通过。
- [ ] Windows TCP recv buffer 复用通用 pool；非压缩 QUIC→TCP deferred view 不从 pool 分配 payload buffer。
- [ ] `OVERLAPPED` operation 只在 completion 后释放或复用。
- [ ] `StreamCallback` 不在 `Closing=true` 后读取 relay。
- [ ] Windows tests 在 `build-win` 下通过。
- [ ] 不引入 Linux relay 行为回归。
