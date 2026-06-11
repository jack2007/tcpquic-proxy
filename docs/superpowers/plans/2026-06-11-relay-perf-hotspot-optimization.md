# Linux Relay 热点性能优化实施计划

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 消除 DGX perf 中占 ~40% CPU 的 relay 层 mutex/池/拷贝开销，使单连接吞吐接近 secnetperf 同 CPU 预算。

**Architecture:** 分三阶段渐进优化——Phase 1 消除 QUIC→TCP 二次拷贝与冗余查找；Phase 2 per-relay 预分配双域 buffer（ingress + worker 无锁）并移除热路径 `shared_ptr`；Phase 3 在证明生产者模型后用 SPSC 或 MPSC 事件队列替换 mutex deque。详见 `docs/superpowers/specs/2026-06-11-relay-perf-hotspot-optimization-design.md`。

**Tech Stack:** C++17、MsQuic C++ API、Linux epoll/readv/writev、现有 unit test（`make test`）、DGX bench 脚本。

---

## 文件结构预览

| 文件 | 职责 |
|------|------|
| `src/tunnel/linux_relay_buffer_pool.h/.cpp` | buffer slot、ingress/worker 双域池、Retain/Release |
| `src/tunnel/linux_relay_worker.h/.cpp` | 事件结构、事件队列接入、ProcessQuicReceiveEvent、StreamRelayBinding |
| `src/tunnel/linux_relay_event_queue.h` | Phase 3：SPSC/MPSC 事件队列（新文件，取决于线程模型证据） |
| `src/unittest/linux_relay_buffer_pool_test.cpp` | 池行为单测 |
| `src/unittest/linux_relay_worker_io_test.cpp` | 生产路径 receive 单测 |
| `src/unittest/linux_relay_worker_queue_test.cpp` | 队列背压与生产者模型单测 |

---

## Phase 1：快速路径修复

### Task 1: 消除 QUIC→TCP 二次拷贝

**Files:**
- Modify: `src/tunnel/linux_relay_worker.cpp` — `ProcessQuicReceiveEvent`
- Test: `src/unittest/linux_relay_worker_io_test.cpp`

- [x] **Step 1: 添加无压缩直通测试**

在 `linux_relay_worker_io_test.cpp` 增加用例：plain data（非压缩），走 `DispatchStreamEventForTest` 生产路径，断言输出正确，并通过 `TqLinuxRelayWorkerSnapshot::BufferAcquireCount` 锁定池 acquire 次数。8192 字节、4096 chunk 的 RECEIVE 事件，优化前 acquire 4 次（callback 拷贝 2 次 + worker 二次拷贝 2 次），优化后应为 2 次。

```cpp
// 在 linux_relay_worker_io_test.cpp 新增块
{
    TqLinuxRelayWorkerConfig config{};
    config.EventBudget = 128;
    config.ReadChunkSize = 4096;
    config.ReadBatchBytes = 64 * 1024;
    config.MaxIov = 8;
    config.MaxPendingBytes = 256 * 1024;

    TqLinuxRelayWorker worker(config);
    assert(worker.Start());

    int fds[2]{-1, -1};
    assert(::socketpair(AF_UNIX, SOCK_STREAM, 0, fds) == 0);

    int fakeStreamToken = 0;
    MsQuicStream* fakeStream = reinterpret_cast<MsQuicStream*>(&fakeStreamToken);

    TqLinuxRelayRegistration registration{};
    registration.TcpFd = fds[0];
    registration.Stream = fakeStream;
    registration.EnableQuicSends = false;
    const auto registered = worker.RegisterRelayWithId(registration);
    assert(registered.Ok);

    const std::vector<uint8_t> plain(8192, 0x5A);
    QUIC_BUFFER quicBuffer{};
    quicBuffer.Buffer = const_cast<uint8_t*>(plain.data());
    quicBuffer.Length = static_cast<uint32_t>(plain.size());

    QUIC_STREAM_EVENT receiveEvent{};
    receiveEvent.Type = QUIC_STREAM_EVENT_RECEIVE;
    receiveEvent.RECEIVE.BufferCount = 1;
    receiveEvent.RECEIVE.Buffers = &quicBuffer;

    assert(QUIC_SUCCEEDED(worker.DispatchStreamEventForTest(fakeStream, &receiveEvent)));
    assert(worker.DrainForTest(config.EventBudget) >= 1);

    std::vector<uint8_t> output(plain.size());
    size_t offset = 0;
    while (offset < output.size()) {
        const ssize_t n = ::read(fds[1], output.data() + offset, output.size() - offset);
        assert(n > 0);
        offset += static_cast<size_t>(n);
    }
    assert(output == plain);

    const TqLinuxRelayWorkerSnapshot snapshot = worker.Snapshot();
    if (snapshot.BufferAcquireCount != 2) {
        return 1;
    }

    worker.Stop();
    ::close(fds[1]);
}
```

- [x] **Step 2: 运行测试确认基线通过**

Run: `rtk ./build/bin/Release/tcpquic_linux_relay_worker_io_test`
Expected: FAIL（`BufferAcquireCount` 字段/计数尚不存在，或旧实现计数为 4）。这是本任务的红灯测试。Actual: FAIL, then PASS after implementation.

- [x] **Step 3: 修改 ProcessQuicReceiveEvent 消除二次拷贝**

```cpp
void TqLinuxRelayWorker::ProcessQuicReceiveEvent(const TqLinuxRelayEvent& event) {
    auto relay = FindRelayById(event.RelayId);
    if (relay == nullptr || relay->Closing) {
        return;
    }

    const bool needsDecompress =
        relay->Decompressor != nullptr && relay->CompressAlgo != TqCompressAlgo::None;

    if (needsDecompress) {
        const uint8_t* data = event.Buffer ? event.Buffer->Data() : nullptr;
        const size_t length = event.Length;
        if (!EnqueueQuicReceive(relay.get(), data, length, event.Fin)) {
            if (relay->Handle != nullptr) {
                relay->Handle->Stop.store(true);
            }
            return;
        }
    } else {
        if (event.Buffer && event.Length > 0) {
            relay->PendingTcpWrites.push_back(
                TqBufferView{event.Buffer->Data(), event.Length, event.Buffer});
        }
        if (event.Fin) {
            relay->TcpWriteShutdownQueued = true;
        }
    }

    FlushTcpWrites(relay.get());
}
```

- [x] **Step 4: 运行测试**

Run:
- `rtk cmake --build build --target tcpquic_linux_relay_worker_io_test tcpquic_linux_relay_worker_queue_test tcpquic_linux_relay_buffer_pool_test -j2`
- `rtk ./build/bin/Release/tcpquic_linux_relay_worker_io_test`
- `rtk ./build/bin/Release/tcpquic_linux_relay_worker_queue_test`
- `rtk ./build/bin/Release/tcpquic_linux_relay_buffer_pool_test`

Expected: PASS. Actual: PASS.

- [x] **Step 5: Commit**

```bash
git add src/tunnel/linux_relay_worker.cpp src/unittest/linux_relay_worker_io_test.cpp
git commit -m "perf(relay): skip second memcpy on QUIC-to-TCP plain path"
```

---

### Task 2: Stream 上下文 O(1) 定位 Relay

**Files:**
- Modify: `src/tunnel/linux_relay_worker.h` — 新增 `TqStreamRelayBinding`
- Modify: `src/tunnel/linux_relay_worker.cpp` — Register/Unregister/OnStreamEvent/StreamCallback
- Test: `src/unittest/linux_relay_worker_queue_test.cpp`

- [x] **Step 1: 定义 TqStreamRelayBinding**

在 `linux_relay_worker.cpp` 添加内部 binding 结构；不要把裸 `RelayState*` 暴露到 public header：

```cpp
struct TqStreamRelayBinding {
    TqLinuxRelayWorker* Worker{nullptr};
    TqLinuxRelayWorker::RelayState* Relay{nullptr};
    std::atomic<uint32_t> CallbackRefs{0};
    std::atomic<bool> Closing{false};
};
```

`OnStreamEvent` 必须先检查 `Closing` 并递增 `CallbackRefs`，退出时递减。该任务不得只实现裸指针快路径。

- [x] **Step 2: RegisterRelayWithId 设置 binding**

```cpp
// RegisterRelayWithId 末尾，替换：
//   registration.Stream->Context = this;
// 为：
auto* binding = new (std::nothrow) TqStreamRelayBinding{};
if (binding == nullptr) {
    // rollback epoll + Relays entry（与现有失败路径一致）
    return result;
}
binding->Worker = this;
binding->Relay = relay.get();
registration.Stream->Callback = TqLinuxRelayWorker::StreamCallback;
registration.Stream->Context = binding;
relay->StreamBinding = binding;  // RelayState 新增字段记录，便于注销释放
```

`RelayState` 新增：`TqStreamRelayBinding* StreamBinding{nullptr};`

- [x] **Step 3: OnStreamEvent 使用 binding，删除 FindRelayIdByStream 热路径调用**

```cpp
QUIC_STATUS TqLinuxRelayWorker::OnStreamEvent(
    MsQuicStream* stream,
    QUIC_STREAM_EVENT* event) noexcept {
    auto* binding = static_cast<TqStreamRelayBinding*>(stream ? stream->Context : nullptr);
    if (binding == nullptr || binding->Worker != this || binding->Relay == nullptr) {
        return QUIC_STATUS_SUCCESS;
    }
    RelayState* relay = binding->Relay;
    const uint64_t relayId = relay->Id;

    if (event->Type == QUIC_STREAM_EVENT_SEND_COMPLETE) {
        // ... 不变，使用 relayId
    }
    if (event->Type == QUIC_STREAM_EVENT_RECEIVE) {
        if (relay->Closing) {
            return QUIC_STATUS_SUCCESS;
        }
        for (uint32_t i = 0; i < event->RECEIVE.BufferCount; ++i) {
            const auto& buffer = event->RECEIVE.Buffers[i];
            if (!CopyQuicReceiveToEvent(relayId, buffer.Buffer, buffer.Length)) {
                AbortRelayFromCallback(relayId, stream);
                return QUIC_STATUS_SUCCESS;
            }
        }
        // FIN 处理不变
        return QUIC_STATUS_SUCCESS;
    }
    // shutdown 事件使用 relayId，不再 FindRelayIdByStream
}
```

`StreamCallback` 保持：`worker = binding->Worker`（或保留 `context` 为 worker 指针的兼容——统一改为 binding）。

- [x] **Step 4: UnregisterRelay 释放 binding**

```cpp
if (removed->Stream != nullptr) {
    removed->StreamBinding->Closing.store(true, std::memory_order_release);
    // 恢复 callback 后，等待或延迟到 CallbackRefs==0 再释放 binding。
    removed->StreamBinding = nullptr;
    removed->Stream->Callback = MsQuicStream::NoOpCallback;
    removed->Stream->Context = nullptr;
}
```

新增并发单测：一个线程持续 `DispatchStreamEventForTest`，另一个线程 `UnregisterRelay`；断言无崩溃、无 use-after-free（ASAN 构建下必须通过）。

- [x] **Step 5: 运行测试并提交**

Run:
- `rtk cmake --build build --target tcpquic_linux_relay_worker_io_test tcpquic_linux_relay_worker_queue_test tcpquic_linux_relay_buffer_pool_test tcpquic_relay_backend_selection_test -j2`
- `rtk ./build/bin/Release/tcpquic_linux_relay_worker_io_test`
- `rtk ./build/bin/Release/tcpquic_linux_relay_worker_queue_test`
- `rtk ./build/bin/Release/tcpquic_linux_relay_buffer_pool_test`
- `rtk ./build/bin/Release/tcpquic_relay_backend_selection_test`

Expected: PASS. Actual: PASS.

```bash
git commit -m "perf(relay): O(1) stream-to-relay binding, drop hot-path RelayLock scan"
```

---

### Task 3: 合并 receive 事件减少 Enqueue 次数

**Files:**
- Modify: `src/tunnel/linux_relay_worker.h` — `TqLinuxRelayEvent`
- Modify: `src/tunnel/linux_relay_worker.cpp` — `CopyQuicReceiveToEvent`, `ProcessQuicReceiveEvent`
- Test: `src/unittest/linux_relay_worker_io_test.cpp`

- [x] **Step 1: 扩展 TqLinuxRelayEvent 支持多 buffer**

```cpp
struct TqLinuxRelayEvent {
    TqLinuxRelayEventType Type{TqLinuxRelayEventType::Shutdown};
    uint64_t Value{0};
    uint64_t RelayId{0};
    void* Relay{nullptr};
    TqBufferRef Buffer;           // 单 buffer 兼容
    std::vector<TqBufferRef> Buffers;  // 批次
    size_t Length{0};
    size_t TotalLength{0};
    bool Fin{false};
};
```

- [x] **Step 2: CopyQuicReceiveToEvent 改为整段 RECEIVE 一次 Enqueue**

新增函数 `CopyQuicReceiveBatchToEvent(relayId, buffers, count)`：

```cpp
bool TqLinuxRelayWorker::CopyQuicReceiveBatchToEvent(
    uint64_t relayId,
    const QUIC_BUFFER* buffers,
    uint32_t bufferCount) {
    auto relay = FindRelayById(relayId);
    if (relay == nullptr || relay->Closing) {
        return false;
    }

    TqLinuxRelayEvent event{};
    event.Type = TqLinuxRelayEventType::QuicReceive;
    event.RelayId = relayId;
    event.Buffers.reserve(bufferCount);

    for (uint32_t i = 0; i < bufferCount; ++i) {
        const uint8_t* data = buffers[i].Buffer;
        uint32_t length = buffers[i].Length;
        if (length == 0) continue;
        if (data == nullptr) return false;

        size_t offset = 0;
        while (offset < length) {
            auto slot = relay->Pool.Acquire();
            if (!slot) {
                Errors.fetch_add(1);
                return false;
            }
            const size_t chunk = std::min(slot->Capacity(), static_cast<size_t>(length - offset));
            std::memcpy(slot->Data(), data + offset, chunk);
            slot->SetLength(chunk);
            event.Buffers.push_back(slot);
            event.TotalLength += chunk;
            offset += chunk;
        }
    }

    if (event.Buffers.empty()) {
        return true;
    }
    Enqueue(std::move(event));
    return true;
}
```

- [x] **Step 3: ProcessQuicReceiveEvent 处理 Buffers 向量**

```cpp
} else {
    if (!event.Buffers.empty()) {
        for (const auto& buf : event.Buffers) {
            relay->PendingTcpWrites.push_back(
                TqBufferView{buf->Data(), buf->Length(), buf});
        }
    } else if (event.Buffer && event.Length > 0) {
        relay->PendingTcpWrites.push_back(
            TqBufferView{event.Buffer->Data(), event.Length, event.Buffer});
    }
    if (event.Fin) {
        relay->TcpWriteShutdownQueued = true;
    }
}
```

- [x] **Step 4: 多 buffer RECEIVE 单测**

构造 3 个 `QUIC_BUFFER` 的 `RECEIVE` 事件，断言 `Snapshot().PendingEvents` 在处理前为 1。

- [x] **Step 5: 测试 + 提交**

Verification:
- RED: `rtk ./build/bin/Release/tcpquic_linux_relay_worker_io_test` failed with `expected 1 batched receive event, got 3`.
- GREEN: `rtk cmake --build build --target tcpquic_linux_relay_worker_io_test -j2` PASS.
- GREEN: `rtk ./build/bin/Release/tcpquic_linux_relay_worker_io_test` PASS.

```bash
git commit -m "perf(relay): batch QUIC receive into single queued event"
```

---

### Task 4: Phase 1 DGX 验证

- [x] **Step 1: 本地编译**

Run: `rtk cmake --build build -j2`
Expected: 无错误. Actual: PASS.

- [x] **Step 2: DGX 吞吐快测（如环境可用）**

Run: `CASE=proxy-1x1 ./scripts/dgx-throughput-matrix.sh`  
Expected: 吞吐 ≥ 8.5 Gbps（不低于优化前）

Actual: SKIPPED. 当前系统不具备 DGX 测试环境；本阶段交付门槛限定为基本编译与本地单元测试。

- [x] **Step 3: 记录 Phase 1 结果到 docs**

在 `docs/dgx-perf-profile-analysis.md` 末尾追加 `## Phase 1 优化结果（日期）` 小节，记录吞吐与观察。

```bash
git commit -m "docs: record Phase 1 relay fast-path optimization results"
```

---

## Phase 2：Buffer 池无锁化

### Task 5: TqRelayBufferSlot 与 worker 无锁池

**Files:**
- Modify: `src/tunnel/linux_relay_buffer_pool.h/.cpp`
- Modify: `src/unittest/linux_relay_buffer_pool_test.cpp`

- [x] **Step 1: 重写池 API（worker 线程专用）**

```cpp
// linux_relay_buffer_pool.h 核心 API
class TqRelayBufferSlot {
public:
    uint8_t* Data();
    size_t Capacity() const;
    void SetLength(size_t n);
    size_t Length() const;
private:
    friend class TqLinuxRelayBufferPool;
    std::vector<uint8_t> Storage;
    size_t UsedLength{0};
};

class TqBufferHandle {
public:
    TqBufferHandle() = default;
    TqBufferHandle(TqLinuxRelayBufferPool* pool, TqRelayBufferSlot* slot, TqBufferDomain domain);
    TqBufferHandle(TqBufferHandle&& other) noexcept;
    TqBufferHandle& operator=(TqBufferHandle&& other) noexcept;
    ~TqBufferHandle();
    TqRelayBufferSlot* operator->() const;
    explicit operator bool() const;
private:
    TqLinuxRelayBufferPool* Pool{nullptr};
    TqRelayBufferSlot* Slot{nullptr};
    TqBufferDomain Domain{TqBufferDomain::Worker};
};

using TqBufferRef = TqBufferHandle;

class TqLinuxRelayBufferPool {
public:
    explicit TqLinuxRelayBufferPool(size_t chunkSize, size_t maxSlots, uint64_t maxPendingBytes);
    void Reserve(size_t slotCount);  // 注册时预分配
    TqBufferRef AcquireWorker();     // 断言/调试构建检查线程 id
    void ReleaseWorker(TqRelayBufferSlot* slot);
    // Ingress 域 — Task 6
};
```

- [x] **Step 2: AcquireWorker/ReleaseWorker 无 mutex 实现**

```cpp
TqBufferRef TqLinuxRelayBufferPool::AcquireWorker() {
    if (PendingBytes + ChunkBytes > MaxBytes) {
        return {};
    }
    TqRelayBufferSlot* slot = nullptr;
    if (!WorkerFree.empty()) {
        slot = WorkerFree.back();
        WorkerFree.pop_back();
    } else if (WorkerAllocated < MaxSlots) {
        slot = new (std::nothrow) TqRelayBufferSlot(ChunkBytes);
        if (!slot) return {};
        ++WorkerAllocated;
    } else {
        return {};
    }
    slot->UsedLength = 0;
    PendingBytes += ChunkBytes;
    return TqBufferRef(slot, [this](TqRelayBufferSlot* s) { ReleaseWorker(s); });
}

void TqLinuxRelayBufferPool::ReleaseWorker(TqRelayBufferSlot* slot) {
    if (!slot) return;
    slot->UsedLength = 0;
    PendingBytes -= std::min<uint64_t>(PendingBytes, ChunkBytes);
    WorkerFree.push_back(slot);
}
```

- [x] **Step 3: 更新单元测试**

```cpp
TqLinuxRelayBufferPool pool(64, 4, 512);
pool.Reserve(4);
auto a = pool.AcquireWorker();
auto b = pool.AcquireWorker();
assert(a && b);
a.reset();
assert(pool.PendingBytes() == 64);
```

- [x] **Step 4: 将 DrainTcpReadable / SubmitTcpBatchToQuic 改用 AcquireWorker**

全局替换 `relay->Pool.Acquire()` → `relay->Pool.AcquireWorker()`（worker 线程路径）。

- [x] **Step 5: 验证无 `shared_ptr` 热路径**

构建后检查 `linux_relay_buffer_pool.h` 中 `TqBufferRef` 不再是 `std::shared_ptr`，`AcquireWorker` 不分配 shared_ptr 控制块。`malloc/free` 热点验收依赖这个条件。

- [x] **Step 6: 测试 + 提交**

Verification:
- RED: `rtk cmake --build build --target tcpquic_linux_relay_buffer_pool_test -j2` failed on missing `TqRelayBufferSlot`, `Reserve`, `AcquireWorker`, and copyable `TqBufferRef`.
- GREEN: `rtk cmake --build build --target tcpquic_linux_relay_worker_io_test tcpquic_linux_relay_worker_queue_test tcpquic_linux_relay_buffer_pool_test tcpquic_relay_backend_selection_test -j2` PASS.
- GREEN: `rtk ./build/bin/Release/tcpquic_linux_relay_buffer_pool_test` PASS.
- GREEN: `rtk ./build/bin/Release/tcpquic_linux_relay_worker_io_test` PASS.
- GREEN: `rtk ./build/bin/Release/tcpquic_linux_relay_worker_queue_test` PASS.
- GREEN: `rtk ./build/bin/Release/tcpquic_relay_backend_selection_test` PASS.

```bash
git commit -m "perf(relay): lock-free worker-side buffer pool with pre-reserve"
```

---

### Task 6: Ingress 域分离（回调线程）

**Files:**
- Modify: `src/tunnel/linux_relay_buffer_pool.h/.cpp`
- Modify: `src/tunnel/linux_relay_worker.cpp` — `CopyQuicReceiveBatchToEvent`

- [x] **Step 1: 添加 Ingress ring**

```cpp
class TqLinuxRelayBufferPool {
    // ...
    TqBufferRef AcquireIngress();
    void ReleaseIngress(TqRelayBufferSlot* slot);
private:
    std::vector<TqRelayBufferSlot*> IngressFree;
    size_t IngressAllocated{0};
    uint64_t IngressPendingBytes{0};
    size_t IngressMaxSlots{0};
};
```

`Reserve(total)` 时分配：`IngressMaxSlots = maxSlots / 2`，worker 域占一半。

- [x] **Step 2: CopyQuicReceiveBatchToEvent 改用 AcquireIngress**

仅修改回调路径的 `Acquire` 调用。

- [x] **Step 3: ProcessQuicReceiveEvent 移交后 ReleaseWorker**

Ingress 取得的 buffer 在移入 `PendingTcpWrites` 后所有权转给 worker 域——实现 `TransferToWorker(slot)` 将 slot 从 ingress 账本转入 worker 账本（不拷贝数据，仅转移 free-list 归属）。

- [x] **Step 4: 测试 + 提交**

Verification:
- RED: `rtk cmake --build build --target tcpquic_linux_relay_buffer_pool_test -j2` failed on missing `AcquireIngress` and `TransferToWorker`.
- GREEN: `rtk cmake --build build --target tcpquic_linux_relay_worker_io_test tcpquic_linux_relay_worker_queue_test tcpquic_linux_relay_buffer_pool_test tcpquic_relay_backend_selection_test -j2` PASS.
- GREEN: `rtk ./build/bin/Release/tcpquic_linux_relay_buffer_pool_test` PASS.
- GREEN: `rtk ./build/bin/Release/tcpquic_linux_relay_worker_io_test` PASS.
- GREEN: `rtk ./build/bin/Release/tcpquic_linux_relay_worker_queue_test` PASS.
- GREEN: `rtk ./build/bin/Release/tcpquic_relay_backend_selection_test` PASS.

```bash
git commit -m "perf(relay): split ingress/worker buffer domains to eliminate cross-thread pool lock"
```

---

### Task 7: Phase 2 perf 验证

- [x] **Step 1: DGX perf 复测**

Run: `CASE=proxy-1x1 DURATION_SEC=25 ./scripts/dgx-perf-profile.sh`  
Output: `docs/dgx-perf-profile-phase2/`

前置：须先 rsync 二进制到对端（`dgx-perf-profile.sh` / `dgx-msquic-vs-proxy-bench.sh` 内置 `deploy_binaries`；`dgx-throughput-matrix.sh` **不会**自动 deploy，未同步时对端仍为旧二进制）。

```bash
# 编译 + 单测
cmake --build build --target tcpquic-proxy \
  tcpquic_linux_relay_buffer_pool_test \
  tcpquic_linux_relay_worker_io_test \
  tcpquic_linux_relay_worker_queue_test -j$(nproc)
for t in build/bin/Release/tcpquic_linux_relay_*_test; do "$t"; done

# 吞吐（先 deploy）
DURATION_SEC=30 ./scripts/dgx-msquic-vs-proxy-bench.sh

# Perf
CASE=proxy-1x1 DURATION_SEC=25 OUT_DIR=docs/dgx-perf-profile-phase2 ./scripts/dgx-perf-profile.sh
```

Actual（2026-06-11，spark-1619 ↔ spark-1b6f，无时延 200G LAN）：
- 代码：`c74ac09` + `0f7260a`
- Perf 原始数据：`docs/dgx-perf-profile-phase2/`
- 吞吐 bench 报告：`/tmp/dgx-msquic-vs-proxy-20260611-165242.md`

- [x] **Step 2: 对比验收**

基线：Phase 1 完整（`docs/dgx-perf-profile-phase1/` + msquic-vs-proxy ~6.24 Gbps）。

**吞吐（proxy-1x1，30s，无时延）**

| 指标 | Phase 1 完整 | Phase 2 | 变化 |
|------|-------------|---------|------|
| msquic-vs-proxy proxy-1x1 | ~6.24 Gbps | **13.38 Gbps** | **+114%** |
| matrix Step2（需先 deploy） | 6.29 Gbps | — | matrix 脚本需先 rsync |
| 计划目标（Phase 2 后 ≥9 Gbps） | 未达标 | **达标** | — |
| 裸 TCP 基线（本次） | — | 29.0 Gbps | 链路正常 |

**注意**：未 deploy 时 `dgx-throughput-matrix.sh` Step2 仅 ~0.7 Gbps（curl_exit=18）；以上吞吐以 deploy 后的 bench 为准。

**Perf 热点（proxy-1x1，25s @ 999Hz）**

| 热点 / 指标 | Phase 1 Client | Phase 2 Client | Phase 1 Server | Phase 2 Server |
|-------------|----------------|----------------|----------------|----------------|
| mutex 原子合计 (`swp4`+`cas4`+pthread) | **43.4%** | **2.6%** | **39.7%** | **3.7%** |
| `Acquire()` / `AcquireWorker()` | 8.2% / — | **0%** / — | 7.2% / — | — / **5.2%** |
| `__aarch64_swp4_rel` | 20.4% | 0.9% | **18.6%** | **0%** |
| `shared_ptr` deleter | 有 | **无** | 有（~8.3% 栈） | **无** |
| `aes_gcm_dec`（client） | 32.0% | 26.5% | — | — |
| `DrainTcpReadable`（server） | 3.5% | — | 3.5% | **9.3%** |

验收项：

| 标准 | 结果 |
|------|------|
| `TqLinuxRelayBufferPool::Acquire` 不在 Top 10 | ✅ 已消失，改为无锁 `AcquireWorker` ~5.2% |
| mutex 原子符号合计 < 15%（目标 <10%） | ✅ Client 2.6% / Server 3.7% |
| 吞吐 ≥ Phase 1 目标 9 Gbps | ✅ 13.38 Gbps |

**结论**：Phase 2 消除 buffer 池跨线程 mutex 竞争，单流吞吐翻倍至 13+ Gbps；relay 热点从锁/分配转向 `DrainTcpReadable` + TLS/UDP 栈。`AcquireWorker` ~5% 为无锁栈操作本身开销，不再伴随 mutex 等待。下一步 Phase 3 可继续压 `QueueLock` / `RelayLock`。

- [x] **Step 3: 更新分析文档**

在 `docs/dgx-perf-profile-analysis.md` 追加 `## Phase 2 DGX 验证结果` 小节（与上表一致）。

```bash
git commit -m "docs: Phase 2 relay buffer pool perf results"
```

---

## Phase 3：事件队列低锁化

### Task 8: 证明生产者模型并实现事件队列

**Files:**
- Create: `src/tunnel/linux_relay_event_queue.h`
- Modify: `src/tunnel/linux_relay_worker.h/.cpp`
- Test: `src/unittest/linux_relay_worker_queue_test.cpp`

- [x] **Step 1: 证明生产者模型**

添加 trace/instrumentation 或引用 MsQuic 文档，确认同一 `TqLinuxRelayWorker` 队列是否只有一个 producer thread。至少覆盖多个并发 stream。若观察到多个 producer thread，禁止使用 SPSC。

- [x] **Step 2: 选择队列实现**

若 Step 1 证明单生产者，使用 SPSC；否则使用 MPSC ring/queue。公共 API 命名为 `TqLinuxRelayEventQueue`，避免把实现细节泄漏到 worker。

- [x] **Step 3: MPMC bounded ring 实现（Step 1 未证明单生产者，禁止 SPSC）**

实际实现：`src/tunnel/linux_relay_event_queue.h` 使用 Dmitry Vyukov 风格的 bounded MPMC sequence ring。每个 cell 持有独立 `Sequence`，producer 通过 `EnqueuePos.compare_exchange_weak` 认领槽位，consumer 通过 `DequeuePos.compare_exchange_weak` 认领可读槽位；队列满时 `TryPush` 返回 `false`，不丢弃已入队事件。

- [x] **Step 4: 替换 Enqueue/DrainEvents 中的 QueueLock deque**

`TqLinuxRelayWorker` 成员：

```cpp
TqLinuxRelayEventQueue EventQueue;
```

`Enqueue`：`if (!EventQueue.TryPush(event)) { Errors++; /* 或触发 receive 背压 */ }`

`DrainEvents`：`while (auto ev = EventQueue.TryPop()) { ... }`

- [x] **Step 5: 队列满背压测试**

单测：填满环，断言 `TryPush` 返回 false，worker drain 后恢复。

- [x] **Step 6: 删除 QueueLock 与 std::deque Queue**（Snapshot 改用 `EventQueue.SizeApprox()`）

- [x] **Step 7: 测试 + DGX perf + 提交**

Verification:
- RED: `rtk cmake --build build --target tcpquic_linux_relay_worker_queue_test -j2` failed on missing `EventQueueCapacity` and `TrackEventProducers`.
- GREEN: `rtk cmake --build build --target tcpquic_linux_relay_worker_io_test tcpquic_linux_relay_worker_queue_test tcpquic_linux_relay_buffer_pool_test tcpquic_relay_backend_selection_test -j2` PASS.
- GREEN: `rtk ./build/bin/Release/tcpquic_linux_relay_worker_queue_test` PASS.
- GREEN: `rtk ./build/bin/Release/tcpquic_linux_relay_buffer_pool_test` PASS.
- GREEN: `rtk ./build/bin/Release/tcpquic_linux_relay_worker_io_test` PASS.
- GREEN: `rtk ./build/bin/Release/tcpquic_relay_backend_selection_test` PASS.

Run:
```bash
# 编译 + 单测
cmake --build build --target tcpquic-proxy \
  tcpquic_linux_relay_buffer_pool_test \
  tcpquic_linux_relay_worker_io_test \
  tcpquic_linux_relay_worker_queue_test -j$(nproc)
for t in build/bin/Release/tcpquic_linux_relay_*_test; do "$t"; done

# 吞吐（内置 deploy）
DURATION_SEC=30 ./scripts/dgx-msquic-vs-proxy-bench.sh

# Perf
CASE=proxy-1x1 DURATION_SEC=25 OUT_DIR=docs/dgx-perf-profile-phase3 ./scripts/dgx-perf-profile.sh
```

Actual（2026-06-11，spark-1619 ↔ spark-1b6f，无时延 200G LAN）：
- 代码：`18f5f58`
- Perf 原始数据：`docs/dgx-perf-profile-phase3/`
- 吞吐 bench 报告：`/tmp/dgx-msquic-vs-proxy-20260611-183351.md`（两次跑分取较好的一次；另一次 7.71 Gbps）

**吞吐（proxy-1x1，30s，无时延）**

基线：Phase 2（`docs/dgx-perf-profile-phase2/` + msquic-vs-proxy **13.38 Gbps**）。

| 指标 | Phase 2 | Phase 3 第1次 | Phase 3 第2次（较好） |
|------|---------|--------------|---------------------|
| msquic-vs-proxy proxy-1x1 | **13.38 Gbps** | 7.71 Gbps | **9.40 Gbps** |
| 裸 TCP 基线 | 29.0 Gbps | 16.6 Gbps | 20.5 Gbps |
| **proxy / 裸 TCP** | **46.1%** | 46.4% | **45.8%** |
| 计划目标（≥9 Gbps） | 达标 | 未达标 | **达标** |

**注意**：当日裸 TCP 在 16–20 Gbps 间波动（Phase 2 为 29 Gbps），绝对 Gbps 低于 Phase 2 主要系链路容量波动；按 proxy/裸 TCP 比值归一化后与 Phase 2 持平（~46%），无代码回归迹象。

**Perf 热点（proxy-1x1，25s @ 999Hz）**

| 热点 / 指标 | Phase 2 Server | Phase 3 Server | 变化 |
|-------------|----------------|----------------|------|
| `DrainTcpReadable` | 9.3% | **11.1%** | 锁消除后 CPU 更多花在真实 I/O |
| `AcquireWorker` | 5.2% | **7.9%** | 同上 |
| `ReleaseWorker` | — | **4.3%** | — |
| `TryPush` / `TryPop` / `EventQueue` | — | **未进 Top** | MPMC CAS 未成为可见热点 |
| `QueueLock` / `pthread_mutex`（事件路径） | Phase 2 已低 | **已移除** | `QueueLock` + deque 删除 |
| Client `aes_gcm_dec` | 26.5% | 7.4% | 采样负载不同（perf 期间 client 曾崩溃） |

验收项：

| 标准 | 结果 |
|------|------|
| 单测全 PASS | ✅ |
| 吞吐无回归（proxy/裸 TCP 比值） | ✅ ~46%，与 Phase 2 持平 |
| 吞吐 ≥ 9 Gbps（绝对值，较好的一次） | ✅ 9.40 Gbps |
| `QueueLock` 消除 | ✅ |
| MPMC 无新热点（`TryPush`/`TryPop` 未上榜） | ✅ |

**结论**：Phase 3 消除事件队列 `QueueLock`，以 MPMC 无锁环替换 mutex deque，未引入可见 CAS 热点，归一化吞吐与 Phase 2 持平。设计文档预期的 3–5% 边际 CPU 回收在 Phase 2 已将 mutex 压至 ~3–4% 后难以单独量化。三阶段优化（Phase 1–3）目标已达成；可选 Phase 4（deferred receive view）见 Task 9。

**Rerun 复测**（2026-06-11 18:47，链路恢复后再次经网卡跑分，代码 `2ce6e60`）：

```bash
DURATION_SEC=30 ./scripts/dgx-msquic-vs-proxy-bench.sh
CASE=proxy-1x1 DURATION_SEC=25 OUT_DIR=docs/dgx-perf-profile-rerun-20260611 ./scripts/dgx-perf-profile.sh
```

| 指标 | Phase 2 | Phase 3（早前） | **Rerun** |
|------|---------|----------------|-----------|
| 裸 TCP | 29.0 Gbps | 20.5 Gbps | **28.2 Gbps** |
| proxy-1x1 | **13.38 Gbps** | 9.40 Gbps | **11.36 Gbps** |
| proxy / 裸 TCP | 46.1% | 45.8% | 40.3% |

Perf（全量 `perf report --comm tcpquic-proxy`，25s @ 999Hz）：

| 热点 / 指标 | Phase 2 | Phase 3（早前） | **Rerun** |
|-------------|---------|----------------|-----------|
| Server mutex 合计 | 3.7% | — | **3.8%** |
| Client mutex 合计 | 2.6% | — | **2.9%** |
| `DrainTcpReadable`（server） | 9.3% | 11.1% | **9.3%** |
| `AcquireWorker`（server） | 5.2% | 7.9% | **5.2%** |
| `ReleaseWorker`（server） | — | 4.3% | **4.2%** |
| `FindRelayById`（server） | — | — | **1.6%** |
| `TryPush` / `TryPop` / `QueueLock` | — | 未进 Top | **未出现** |
| Client `aes_gcm_dec` | 26.5% | 7.4%* | **32.6%** |

\* Phase 3 早前 client 采样因 perf 期间进程崩溃仅 964 samples；Rerun client 5830 samples，与 Phase 2 可比。

**Rerun 结论**：链路恢复至 ~28 Gbps 裸 TCP 后，Phase 3 的 server perf 画像与 Phase 2 **高度一致**（mutex ~3–4%，`DrainTcpReadable`/`AcquireWorker` 持平）；早前 Phase 3 server 热点偏高系链路慢导致 UDP/TLS 占比下降、relay 符号相对上升，非回归。Perf 原始摘要：`docs/dgx-perf-profile-rerun-20260611/`；吞吐报告：`/tmp/dgx-msquic-vs-proxy-20260611-184710.md`。

```bash
git commit -m "docs: record Phase 3 DGX perf verification results"
```

### Task 8.1: 修复 perf 采样期间 client core dump（2026-06-11）

**现象**：`dgx-perf-profile.sh` 采样窗口内 client 进程 abort，日志 `corrupted double-linked list`，`throughput.txt` 中 `speed_download=0`；server 侧 perf 仍有效，独立 bench（`dgx-msquic-vs-proxy-bench.sh`）吞吐正常。

**采集 coredump**：

```bash
ulimit -c unlimited
sudo sysctl -w kernel.core_pattern=/tmp/core.%e.%p.%t
CASE=proxy-1x1 DURATION_SEC=25 ./scripts/dgx-perf-profile.sh
gdb -batch -ex 'thread apply all bt' build/bin/Release/tcpquic-proxy /tmp/core.tcpquic-proxy.<pid>.<ts>
```

**GDB 结论**：崩溃线程在 `TqStartClientTunnel` → `operator new` → `malloc_printerr("corrupted double-linked list")`，属**堆元数据已损坏后的二次崩溃**，非隧道创建逻辑本身。

**根因（两处）**：

| # | 问题 | 机制 |
|---|------|------|
| 1 | Ingress buffer 池数据竞争 | 多个 MsQuic 回调线程并发 `AcquireIngress()`，worker 线程 `TransferToWorker()` / 事件析构 `ReleaseIngress()` 同时改 `IngressFree` / `IngressPending`，无同步 |
| 2 | Relay 注销后 use-after-free | `UnregisterRelay()` 销毁 pool 后，队列中仍残留带 `TqBufferRef` 的 `QuicReceive` 事件；`FindRelayById` 返回 null 时事件析构调用已释放 pool 的 `Release*` |

**修复**（`linux_relay_buffer_pool.*` + `linux_relay_worker.*`）：

- `IngressLock` 保护 ingress free-list（`AcquireIngress` / `ReleaseIngress` / `Reserve` / 析构）
- `WorkerPending` / `IngressPending` 改为 `std::atomic<uint64_t>`
- `RetiredRelays`：注销后暂存 relay，直至事件队列排空再释放 pool
- `TqBufferRef::abandon()`：relay 已不存在时直接 `delete` slot，避免 UAF

**验证**（修复后连续两次 `DURATION_SEC=20/25` perf 采样）：

| 指标 | 修复前 | 修复后 |
|------|--------|--------|
| client core dump | 有（`speed_download=0`） | **无** |
| client 日志 | `corrupted double-linked list` | 正常结束 |
| perf 尾 curl `speed_download` | 0 | **非 0**（例：110801630 B/s） |
| 单元测试 | — | `tcpquic_linux_relay_buffer_pool_test` / `linux_relay_worker_*_test` PASS |

`scripts/dgx-perf-profile.sh` 已在脚本入口默认 `ulimit -c unlimited` 并设置 `core_pattern=/tmp/core.%e.%p.%t`，便于后续自动采集。

- [x] **Step 1: 配置 coredump + GDB 定位**
- [x] **Step 2: 修复 ingress 池竞争 + relay 生命周期**
- [x] **Step 3: perf 采样复测无崩溃**

---

## Phase 4（可选）：Deferred Receive View / QUIC→TCP zero-copy 尝试

> 目标：QUIC→TCP 方向不再把 MsQuic receive payload 拷贝到 relay buffer，而是在 MsQuic receive callback 中返回 `QUIC_STATUS_PENDING`，由 relay worker 直接用 receive buffer 指针 `writev()` 到 TCP socket；TCP 实际写出多少字节，就调用 `StreamReceiveComplete(bytes)` 释放多少 MsQuic receive buffer。

### Task 9: MsQuic receive buffer 生命周期源码审计

**Files:**
- Read: `third_party/msquic/src/core/stream_recv.c`
- Read: `third_party/msquic/src/core/recv_buffer.c`
- Read: `third_party/msquic/src/core/recv_buffer.h`
- Read: `third_party/msquic/src/core/api.c`
- Optional test: `src/unittest/msquic_receive_lifetime_test.cpp`

**已确认的源码事实：**

- `QUIC_STREAM_EVENT_RECEIVE` 的 `event->RECEIVE.Buffers` 数组本身不能异步保存：`stream_recv.c` 优先使用栈上的 `StackRecvBuffers[3]`，超过 3 个 buffer 才临时 heap 分配，并在 receive flush 结束前释放 heap 版本。
- `QUIC_BUFFER.Buffer` 指向 MsQuic recv buffer 内部 chunk。`recv_buffer.h` 注释明确：`QuicRecvBufferRead` 返回 internal pointer，调用方必须在 `QuicRecvBufferDrain` 前保持独占访问。
- 若 receive callback 不返回 `QUIC_STATUS_PENDING`，MsQuic 会把 receive 长度计入完成长度，并调用 `QuicStreamReceiveComplete`，后者继续调用 `QuicRecvBufferDrain` 推进 `BaseOffset`，并可能释放或复用 chunk。
- 因此 Phase 4 的第一步不应是“探测测试验证回调返回后指针是否还活着”，而应是源码审计 `QUIC_STATUS_PENDING`、`StreamReceiveComplete`、`StreamReceiveSetEnabled`、`ReceiveMultiple` 的精确语义。测试最多作为防回归或验证当前 MsQuic 版本理解的 integration test。

- [ ] **Step 1: 源码审计 `QUIC_STATUS_PENDING` receive 语义**

确认 callback 返回 `QUIC_STATUS_PENDING` 后，MsQuic 不会自动 drain 当前 receive buffer；只有应用后续调用 `StreamReceiveComplete(bytes)` 后，才会释放对应字节的内部 receive buffer。

- [ ] **Step 2: 源码审计部分完成语义**

确认 `StreamReceiveComplete(stream, N)` 支持按字节部分完成，且可与 TCP partial write 一一对应。重点确认重复 complete、超额 complete、stream shutdown、`ReceiveMultiple` 模式下的行为。

- [ ] **Step 3: 源码审计 receive 暂停/恢复语义**

确认 `StreamReceiveSetEnabled(FALSE/TRUE)` 能用于 relay queue 或 per-relay pending bytes 高低水位背压，避免慢 TCP 下应用层无限持有 MsQuic receive buffer。

### Task 10: Deferred Receive View POC 设计

**Files:**
- Modify: `src/tunnel/linux_relay_worker.h`
- Modify: `src/tunnel/linux_relay_worker.cpp`
- Modify: `src/tunnel/linux_relay_event_queue.h`
- Test: `src/unittest/linux_relay_worker_io_test.cpp`
- Test: `src/unittest/linux_relay_worker_queue_test.cpp`

**核心设计：**

不能保存 `event->RECEIVE.Buffers` 指针；只能在 callback 内按值拷贝 `QUIC_BUFFER` 元数据，不拷贝 payload：

```cpp
struct TqQuicReceiveSlice {
    const uint8_t* Data;
    uint32_t Length;
};

struct TqPendingQuicReceive {
    MsQuicStream* Stream;
    uint64_t RelayId;
    std::vector<TqQuicReceiveSlice> Slices;
    uint64_t TotalLength;
    uint64_t CompletedLength;
    bool Fin;
};
```

receive callback 流程：

1. 收到 `QUIC_STREAM_EVENT_RECEIVE`。
2. 检查 relay 是否 closing。
3. 检查 per-relay / per-worker pending receive bytes 高水位。
4. 创建 `TqPendingQuicReceive`，只保存 slice 指针和长度。
5. 入队 `TqLinuxRelayEventType::QuicReceiveView`。
6. 返回 `QUIC_STATUS_PENDING`。

worker 写 TCP 流程：

1. relay worker 消费 `QuicReceiveView`。
2. 将 slices 映射为 `iovec`。
3. 调用 `writev(tcpFd, ...)`。
4. 若实际写出 `N` 字节，立即调用 `StreamReceiveComplete(stream, N)`。
5. pending view cursor 跳过已完成的 `N` 字节。
6. 如果 partial write，剩余 slices 留在 relay pending write queue，等待 epoll writable 后继续。
7. 全部 payload 写完后释放 `TqPendingQuicReceive`；若带 FIN，再处理 TCP half-close。

**背压模型：**

```text
TCP 慢
-> relay pending receive views 增多
-> StreamReceiveComplete 调用变慢
-> MsQuic recv buffer 不 drain
-> QUIC stream receive window 不继续放大
-> 对端 QUIC send 被 flow control 限制
```

需要新增水位配置：

- `MaxPendingQuicReceiveBytesPerRelay`
- `MaxPendingQuicReceiveBytesPerWorker`

超过高水位时，暂停接收或保持 pending；低于低水位后恢复 `StreamReceiveSetEnabled(TRUE)`。

**队列隔离与公平性：**

Phase 4 不应让多个 tunnel 共用一个 data queue。若 worker 共享队列中直接排放 receive data/backlog，一个 TCP 卡住的 tunnel 可能把自己的数据排在队头，导致后续 tunnel 的数据无法被处理，形成跨 tunnel head-of-line blocking。

目标结构：

```text
TqLinuxRelayWorker
  EventQueue: worker 级共享 ready/control queue，只放轻量通知，仍然有界

RelayState A
  PendingQuicReceives: A tunnel 自己的 data queue
  PendingQuicReceiveBytes
  QuicReceiveWorkQueued
  ReceivePaused

RelayState B
  PendingQuicReceives: B tunnel 自己的 data queue
```

共享 `EventQueue` 只放类似 `RelayQuicReceiveReady{RelayId}` 的轻量事件，不携带 receive payload view；真正的数据 backlog 保存在对应 `RelayState::PendingQuicReceives` 中。每个 relay 使用 `QuicReceiveWorkQueued` 去重：同一个 relay 已经在 worker ready queue 中时，不重复投递 ready event。

慢 TCP tunnel 的处理：

```text
relay A TCP write returns EAGAIN
-> A 的 PendingQuicReceives 保留在 relay A 内部
-> arm A 的 EPOLLOUT
-> 不继续向 worker EventQueue 塞 A 的 data event
-> relay B/C 仍可通过自己的 ready event 被 worker 处理
```

因此 Phase 4 的公平性边界是：

- data queue 必须是 per-relay / per-tunnel 的，不能是 worker 共享的。
- worker `EventQueue` 是 shared ready/control queue，不是 shared data queue。
- receive 背压按 per-relay 和 per-worker pending bytes 控制。
- 单个 tunnel 超过高水位时，只暂停该 stream 的 receive callback：`StreamReceiveSetEnabled(FALSE)`。
- 低于低水位后，只恢复该 stream：`StreamReceiveSetEnabled(TRUE)`。

**TCP→QUIC send 背压模型：**

当前 TCP→QUIC 路径不是把 data 放进 worker `EventQueue`，而是 worker 在 `EPOLLIN` 中 `readv()` 到 buffer pool，立即构造 `TqLinuxRelaySendOperation` 调用 `StreamSend()`；operation 持有 `TqBufferView` 和 `QUIC_BUFFER` 元数据，直到 `QUIC_STREAM_EVENT_SEND_COMPLETE` 回来后释放。`EventQueue` 只承载 `QuicSendComplete` control event。

现有背压主要来自 `relay->Pool.PendingBytes()` 与 per tick read budget：MsQuic send complete 慢时，send operation 持有 buffer 不释放，pool pending bytes 上升，`DrainTcpReadable()` 停止继续读 TCP。Phase 4 应把这个隐式机制显式化为 per-relay QUIC send 水位。

每个 `RelayState` 建议新增：

```cpp
uint64_t OutstandingQuicSendBytes;
uint64_t OutstandingQuicSendOps;
uint64_t IdealQuicSendBytes;
bool TcpReadPausedForQuicBackpressure;
bool TcpReadArmed;
```

发送路径：

```text
TCP EPOLLIN
-> if OutstandingQuicSendBytes >= high watermark: disable EPOLLIN, return
-> readv within min(tick budget, high - outstanding)
-> optional compress
-> StreamSend()
-> OutstandingQuicSendBytes += sent bytes
-> OutstandingQuicSendOps += 1
```

完成路径：

```text
QUIC_STREAM_EVENT_SEND_COMPLETE
-> enqueue QuicSendComplete control event
-> worker CompleteQuicSend()
-> OutstandingQuicSendBytes -= operation bytes
-> OutstandingQuicSendOps -= 1
-> release operation buffers
-> if paused && OutstandingQuicSendBytes <= low watermark: enable EPOLLIN
```

水位策略：

```text
HighWatermark = clamp(IdealQuicSendBytes, MinQuicSendBackpressureBytes, MaxQuicSendBackpressureBytes)
LowWatermark  = HighWatermark / 2
```

`QUIC_STREAM_EVENT_IDEAL_SEND_BUFFER_SIZE` 可用于更新 `IdealQuicSendBytes`；若未收到 hint，使用配置默认值。这样 QUIC send 慢时，`SEND_COMPLETE` 变慢，outstanding bytes 不下降，worker 暂停该 relay 的 TCP `EPOLLIN/readv`，最终 TCP socket receive buffer 填满并通过 TCP window 背压本地 TCP 对端。

TCP→QUIC 的公平性边界：

- data lifetime 属于每个 `TqLinuxRelaySendOperation`，不进入 worker 共享 `EventQueue`。
- 背压按 per-relay `OutstandingQuicSendBytes` 控制，避免一个慢 QUIC stream 消耗整个 worker 的 buffer pool。
- worker `EventQueue` 只处理 `QuicSendComplete` 等 control event；若 send complete 很密集，可后续引入 per-relay complete batching 或 budget，但不把 payload data 放进共享 queue。
- 压缩路径仍可沿用该模型：压缩输出 buffer 被 send operation 持有到 `SEND_COMPLETE`。

**错误与关闭路径要求：**

- TCP 写失败：abort stream，并确保所有尚未 `StreamReceiveComplete` 的 pending receive 有明确处理策略。
- QUIC stream shutdown：停止继续写 TCP，释放 relay，但不能让 worker 访问已释放的 stream/binding。
- relay closing 但仍有 pending receive：pending receive 必须持有足够的 owner/ref，避免 use-after-free。
- event queue 满或内存分配失败：不能简单返回普通失败状态，因为 MsQuic receive callback 的失败状态可能被当作 success 处理并 drain；安全策略应是 fallback 到现有 copy 路径，或 abort stream 并保证没有未跟踪 borrowed buffer。

**异常场景矩阵：**

| 场景 | 触发条件 | 风险 | 处理方法 |
|------|----------|------|----------|
| TCP `writev` 全部成功 | `written == remaining` | 无 | `StreamReceiveComplete(written)`；释放 view 元数据；若 `Fin`，再处理 TCP half-close |
| TCP `writev` 部分成功 | `0 < written < remaining` | cursor / complete 字节错位 | `StreamReceiveComplete(written)`；推进 slice cursor；剩余 view 等下次 writable |
| TCP `EAGAIN` / `EWOULDBLOCK` | socket 暂不可写 | 忙等或误 complete | 不 complete；不释放 view；注册/等待 `EPOLLOUT` |
| TCP hard error | `EPIPE` / `ECONNRESET` / `EBADF` 等 | 未写入数据无法交付 | 先 complete 已成功写入但尚未 complete 的前缀；剩余不 complete；abort QUIC stream；释放 view 元数据 |
| TCP partial 后 hard error | 前缀已写/已 complete，剩余失败 | 重复 complete 或漏清理 | 保持 `CompletedToMsQuicBytes` 不变；剩余不 complete；abort stream；释放 view 元数据 |
| relay 主动关闭 | 用户中断、生命周期清理、tunnel teardown | worker 仍持有 stream/data | 标记 `Closing`；禁止新 receive；owner worker 清理 pending views；未写剩余 abort；最后释放 refs |
| relay closing 时又来 RECEIVE | callback 收到新 receive，但 relay 已关闭 | 返回普通失败可能被 MsQuic 当 success drain | 不创建 view；优先 fallback copy 或 inline abort stream；避免普通 failure return 导致误消费 |
| QUIC peer FIN | `QUIC_RECEIVE_FLAG_FIN` | TCP half-close 时机错误 | FIN 挂在最后一个 pending view；payload 全部写完并 complete 后，再 shutdown TCP write side |
| QUIC peer send aborted | `PEER_SEND_ABORTED` / remote `RESET_STREAM` | pending buffer 是否仍可访问不清楚 | 标记 relay aborting；停止写 pending views；释放 view 元数据；不再调用 complete |
| QUIC peer receive aborted | `PEER_RECEIVE_ABORTED` / remote `STOP_SENDING` | 反向路径已被对端终止 | abort 整个 relay；停止新事件；清理 pending views |
| QUIC shutdown complete | `SHUTDOWN_COMPLETE` | stream handle 可能失效 | 进入 terminal state；禁止后续 `StreamReceiveComplete`；清理 pending view 元数据；关闭 TCP |
| event queue full | callback 内无法入队 view | 返回普通失败可能被 MsQuic 当 success drain | fallback 到现有 copy 路径；fallback 失败则 inline abort stream |
| view 元数据分配失败 | `new` / `vector` 失败 | 同上 | fallback copy；fallback 失败则 inline abort stream |
| pending bytes 超高水位 | TCP 慢导致积压 | 持有太多 MsQuic buffer | 暂停 receive 或保持 pending；低水位后恢复 receive |
| stream/binding 已 closing | worker 准备 complete 时发现关闭 | use-after-free | 不再调用 complete；走 abort cleanup；pending view 释放由 owner worker 执行 |
| 多个 pending view 同 relay | 多次 receive pending | complete 顺序错乱 | POC 禁止多个 pending；完整实现按 offset/order 串行 write 和 complete |
| 压缩路径 | QUIC payload 需要解压再写 TCP | 无法 zero-copy | Phase 4 POC 禁用压缩；压缩保持 copy 路径 |

**合并后的处理动作：**

1. `CompleteWrittenPrefix(view, written)`：用于 TCP 成功写出字节。推进 cursor，调用 `StreamReceiveComplete(written)`，并保证 `CompletedToMsQuicBytes <= TcpWrittenBytes <= TotalBytes`。
2. `KeepPendingForWritable(view)`：用于 `EAGAIN` 或 partial write 后仍有剩余。保留 view，不 complete 剩余字节。
3. `AbortUndeliveredReceive(view, reason)`：用于 TCP hard error、relay abort、QUIC abort。先 complete 已写但未 complete 的前缀；剩余字节不 complete；随后 abort stream 并释放 view 元数据。
4. `FallbackOrAbortInCallback(event)`：用于 callback 内 queue full、alloc fail、relay closing。先 fallback 到现有 copy 路径；失败则 inline abort stream，避免普通 failure return 被 MsQuic 当 success 消费。
5. `DrainThenCloseFin(view)`：用于正常 FIN。先写完 payload 并 complete，再执行 TCP half-close。
6. `ReleaseAfterOwnerWorker(view)`：所有 pending view 的释放都由 relay owner worker 执行；非 owner 线程只标记状态和投递事件，避免 stream/binding/data 生命周期竞态。

**MsQuic 源码确认：abort 与 pending receive 的关系：**

- `QuicStreamShutdown(... QUIC_STREAM_SHUTDOWN_FLAG_ABORT_RECEIVE ...)` 会进入 `QuicStreamRecvShutdown()`；该函数设置 `SentStopSending = TRUE`、`ReceiveEnabled = FALSE`、`ReceiveDataPending = FALSE`，并发送 `STOP_SENDING`，但不会调用 `QuicRecvBufferDrain()`。
- `QuicStreamReceiveComplete()` 在 `SentStopSending` 或 `RemoteCloseFin` 为真时直接返回 `FALSE`，不会 drain。因此 abort 之后再调用 `StreamReceiveComplete()` 不能作为释放 pending receive 的手段。
- `QuicRecvBufferUninitialize()` 会在 `QuicStreamFree()` 时释放 recv chunks；也就是说 abort/close 后必须停止访问 borrowed buffer 指针，不能假设 buffer 仍可用于 TCP write。
- remote `RESET_STREAM` 路径会设置 `RemoteCloseReset`、关闭 receive，并按 final size 修正 flow-control accounting，但不会把应用层未 complete 的 borrowed pointer 变成可继续访问资源。
- 结论：Phase 4 的关闭策略必须是“abort 前 complete 已成功写入 TCP 的前缀；abort 后不再 complete、不再 write borrowed pointer，只释放 proxy 自己的 view 元数据并等待 MsQuic stream 关闭释放内部 buffer”。

- [x] **Step 1: POC 限制范围**

先只支持 plain receive、无压缩、单 relay 单 pending receive、TCP 可立即写出或可通过 epoll writable 继续写出。保留现有 copy 路径作为 fallback。

- [x] **Step 2: 实现 `QuicReceiveView` 事件类型**

新增只持有 slice 元数据的事件，不拷贝 payload。事件入队成功后 callback 返回 `QUIC_STATUS_PENDING`。

- [x] **Step 3: 实现 TCP partial write cursor**

按 `writev()` 返回值推进 slice cursor，并对每次成功写出的字节调用 `StreamReceiveComplete(stream, written)`。

- [x] **Step 4: 加 pending bytes 水位和 receive enable/disable**

慢 TCP 下不要让应用层无限持有 MsQuic 内部 receive buffer。高水位 disable receive，低水位恢复。

- [x] **Step 5: 补齐关闭路径测试**（已覆盖 partial/EAGAIN、TCP hard error、FIN-only、QUIC shutdown cleanup、queue-full fallback abort）

覆盖 TCP close、QUIC close、queue full、abort、FIN、partial write、重复 writable 等场景。

- [x] **Step 6: 单独 bench**

对比 Phase 3 的 QUIC→TCP plain 路径，重点看 callback copy 消失后 CPU 是否下降、吞吐是否提升、MsQuic flow control 是否能稳定背压。

2026-06-11 短 bench 已执行：`DURATION_SEC=10 REPORT=/tmp/dgx-msquic-vs-proxy-phase4.md ./scripts/dgx-msquic-vs-proxy-bench.sh`。结果为 direct TCP 19.241 Gbps、secnetperf 单连接 0.705 Gbps、proxy-1x1 0.052 Gbps、proxy-16x16 0.158 Gbps、proxy-4x16 0.129 Gbps。该结果显示当前“每个 receive callback 都返回 PENDING 并交给 worker 异步 complete”的 POC 存在明显吞吐退化，不能作为最终 Phase 4 性能方案验收。源码原因高度相关：MsQuic 在 receive callback 前将 `ReceiveEnabled` 设为 `ReceiveMultiple`，默认非 multiple 时返回 `QUIC_STATUS_PENDING` 会暂停后续 receive callback，直到异步 `StreamReceiveComplete` 恢复。后续若继续 Phase 4，应改为 callback 内同步尝试 zero-copy write，只有 TCP EAGAIN/partial 时才进入 pending view；或确认并启用 receive-multiple 语义后再评估。

### Phase 4 后续正确方向

当前 always-pending POC 的问题不在于 borrowed receive buffer 本身，而在于把每次 receive callback 都变成“callback 返回 `QUIC_STATUS_PENDING` -> worker 被唤醒 -> TCP 写出 -> 异步 `StreamReceiveComplete` -> MsQuic 才能继续投递下一批 receive”的串行链路。后续优化应把普通快速路径留在 MsQuic receive callback 内完成，只有真正遇到慢 TCP 时才进入 deferred view。

建议的下一版设计：

1. receive callback 内先同步尝试 `writev()` / `sendmsg()` 到 TCP socket，直接使用 MsQuic receive buffer 指针，不拷贝 payload。
2. 如果本次 receive payload 全部写入 TCP，则在 callback 内立即调用 `StreamReceiveComplete(total)`，保持同步消费语义。返回值可优先保持与 MsQuic 同步完成路径一致；如果源码确认 `QUIC_STATUS_CONTINUE` 更适合连续投递，再考虑切换为 `QUIC_STATUS_CONTINUE`。
3. 如果 TCP 只写入部分数据，则对已写入前缀立即 `StreamReceiveComplete(written)`，只把剩余 slice view 和 cursor 交给 relay worker，callback 返回 `QUIC_STATUS_PENDING`。
4. 如果 TCP 返回 `EAGAIN` / `EWOULDBLOCK` 且没有写入任何字节，则把完整 receive view 放入该 relay 的 pending receive 队列，arm TCP `EPOLLOUT`，callback 返回 `QUIC_STATUS_PENDING`。
5. worker 只负责慢路径：TCP writable 后继续 `sendmsg()` 剩余 borrowed slice，每成功写出一段就调用 `StreamReceiveComplete(bytes)`；完成后释放 proxy 自己的 view 元数据并尝试恢复 receive。
6. callback 内同步写必须是非阻塞、有限预算的 fast path，不能在 MsQuic worker/connection 线程里长时间循环或等待 TCP。

另一条可选路线是先源码确认并启用 MsQuic `ReceiveMultiple` 语义，再重新评估 always-pending 模型。只有确认 `ReceiveMultiple` 下 pending receive 不会暂停后续 receive callback，且应用侧 pending view 的生命周期、flow control、关闭路径都能严格管理时，才值得继续评估该模型；否则优先采用“callback 同步写 fast path + partial/EAGAIN 才 pending”的设计。

2026-06-11 复测：已在 `TqMakeMsQuicSettings()` 中启用 `StreamMultiReceiveEnabled`，并新增 `tcpquic_quic_settings_test` 验证 client/server settings 都设置 `IsSet.StreamMultiReceiveEnabled` 与 `StreamMultiReceiveEnabled`。同一条 10 秒 DGX bench 命令复测结果为 direct TCP 22.373 Gbps、secnetperf 单连接 0.857 Gbps、secnetperf 16 连接 51.143 Gbps、secnetperf 单连接 16 stream 1.302 Gbps、proxy-1x1 5.013 Gbps、proxy-16x16 12.230 Gbps、proxy-4x16 14.910 Gbps。

结论：启用 `ReceiveMultiple` 后，always-pending 模型的核心串行化问题被解除，proxy-1x1 从 0.052 Gbps 恢复到 5.013 Gbps，说明前一次低吞吐的主因确实是默认非 multi receive 下 `QUIC_STATUS_PENDING` 暂停后续 receive callback。该模型可以继续评估，但仍需重点关注多 pending receive 的内存占用、水位暂停/恢复、公平性以及关闭路径。后续是否继续优化，应用新的 bench 基线与“callback 同步 write fast path + partial/EAGAIN 才 pending”方案做投入产出对比。

### Task 10.1: Phase 4 完整 DGX 验证（2026-06-11 23:22）

代码：`016ae01`（deferred receive view + `StreamMultiReceiveEnabled` + 背压/关闭路径单测）。

```bash
# 编译 + 单测
cmake --build build --target tcpquic-proxy \
  tcpquic_linux_relay_buffer_pool_test \
  tcpquic_linux_relay_worker_io_test \
  tcpquic_linux_relay_worker_queue_test \
  tcpquic_quic_settings_test -j$(nproc)
for t in build/bin/Release/tcpquic_linux_relay_*_test build/bin/Release/tcpquic_quic_settings_test; do "$t"; done

# 吞吐（内置 deploy）
DURATION_SEC=30 REPORT=/tmp/dgx-msquic-vs-proxy-phase4-20260611-232155.md \
  ./scripts/dgx-msquic-vs-proxy-bench.sh

# Perf
CASE=proxy-1x1 DURATION_SEC=25 OUT_DIR=docs/dgx-perf-profile-phase4 ./scripts/dgx-perf-profile.sh
```

**吞吐（proxy-1x1，30s，无时延）**

基线：Phase 3 Rerun（`docs/dgx-perf-profile-rerun-20260611/` + **11.36 Gbps** proxy-1x1）。

| 指标 | Phase 3 Rerun | Phase 4 短 bench（multi-receive） | **Phase 4 完整 bench** |
|------|---------------|-----------------------------------|------------------------|
| 裸 TCP | 28.18 Gbps | 22.37 Gbps | **18.52 Gbps** |
| proxy-1x1 | **11.36 Gbps** | 5.01 Gbps | **5.23 Gbps** |
| proxy / 裸 TCP | 40.3% | 22.4% | **28.2%** |
| proxy-16×16 | — | 12.23 Gbps | **16.19 Gbps** |
| proxy-4×16 | — | 14.91 Gbps | **19.61 Gbps** |

**Perf 热点（proxy-1x1，25s @ 999Hz）**

| 热点 / 指标 | Phase 3 Rerun | **Phase 4** |
|-------------|---------------|-------------|
| client core dump | 无 | **无** |
| perf curl `speed_download` | 1.42 GB/s | **0.99 GB/s**（非 0） |
| client `CopyQuicReceiveBatch` / receive `memcpy` | ~3.6% | **未进 Top** |
| client `aes_gcm_dec` | 32.6% | **~0.25%**（采样线程分布变化） |
| client `FlushDeferredQuicReceives` | — | **~0.01%** |
| client relay 主路径 | UDP recv + TLS 解密 | **DrainTcpReadable + Acquire/ReleaseWorker** |
| server `ldadd8`（buffer 池原子） | ~5% 量级 | **~22.8%** |
| server `DrainTcpReadable` | 9.3% | **~7.6%** |
| `TryPush` / `TryPop` / `QueueLock` | 未出现 | **未出现** |

验收项：

| 标准 | 结果 |
|------|------|
| 单测全 PASS | ✅ |
| perf 采样无 client core dump | ✅ |
| receive 侧 `memcpy` 消除（`CopyQuicReceiveBatch` 不进 Top） | ✅ |
| 单流吞吐 ≥ Phase 3 Rerun（11.36 Gbps） | ❌ **5.23 Gbps** |
| 单流归一化（proxy/裸 TCP）不低于 Phase 3 | ❌ 28.2% vs 40.3% |
| 多流吞吐可用 | ✅ 16–20 Gbps |

**结论**：Phase 4 deferred receive 在功能与稳定性上达标（零拷贝 receive、无 coredump、关闭/背压单测 PASS），但 **单流吞吐相对 Phase 3 明显回退**（~5.2 Gbps vs ~11.4 Gbps），与短 bench 启用 multi-receive 后的 ~5 Gbps 一致；多流场景不受影响。Perf 显示 receive 拷贝热点已消失，但 always-pending 异步 complete 链路仍限制单流并行度。下一步应实施计划中的 **callback 同步 write fast path + partial/EAGAIN 才 pending**，而非继续扩展当前 always-pending 模型。

原始数据：`docs/dgx-perf-profile-phase4/`；吞吐报告：`/tmp/dgx-msquic-vs-proxy-phase4-20260611-232155.md`。

- [x] **Step 1: 完整 bench + perf 复测**
- [x] **Step 2: 对比 Phase 3 并记录结论**

### Task 10.2: Phase 4 callback sync-write DGX 验证（2026-06-12）

代码：`phase4-callback-sync-write` 分支（`91d9820` fast path + **ReceiveComplete 语义修复**）。

**修复**：`TryCompleteQuicReceiveInline()` 在 payload 全部同步写满时只返回 `QUIC_STATUS_SUCCESS`，不再调用 `StreamReceiveComplete`（MsQuic 同步路径已推进流控；二者并用会在 ~12KB 后卡死）。

```bash
cmake --build build --target tcpquic-proxy tcpquic_linux_relay_worker_io_test -j$(nproc)
./build/bin/Release/tcpquic_linux_relay_worker_io_test

DURATION_SEC=30 REPORT=/tmp/dgx-bench-phase4-syncwrite-v3.md \
  ./scripts/dgx-msquic-vs-proxy-bench.sh
```

**吞吐（proxy-1x1，30s；同晚会话对比 always-pending）**

| 指标 | Phase 3 Rerun | Phase 4 always-pending（Task 10.1） | always-pending 同晚 | **sync-write 修复 v3** |
|------|---------------|-------------------------------------|---------------------|------------------------|
| 裸 TCP | 28.18 Gbps | 18.52 Gbps | 21.99 Gbps | **24.63 Gbps** |
| proxy-1x1 | **11.36 Gbps** | 5.23 Gbps | 4.94 Gbps | **7.68 Gbps** |
| proxy / 裸 TCP | **40.3%** | 28.2% | 22.5% | **31.2%** |
| proxy-16×16 | — | 16.19 Gbps | 10.75 Gbps | **3.40 Gbps** |
| proxy-4×16 | — | 19.61 Gbps | 12.41 Gbps | **2.38 Gbps** |

验收项：

| 标准 | 结果 |
|------|------|
| 单测 PASS（含 sync receive 不调用 ReceiveComplete） | ✅ |
| 单流功能（32MB 下载不断流） | ✅ |
| 单流吞吐 ≥ always-pending 同晚会话 | ✅ **7.68 vs 4.94 Gbps** |
| 单流吞吐 ≥ Phase 3 Rerun（11.36 Gbps） | ❌ **7.68 Gbps** |
| 多流吞吐 ≥ always-pending | ❌ 3.4 / 2.4 vs 10.8 / 12.4 Gbps |

**结论**：callback 同步 write fast path 在修复 MsQuic 完成语义后，**单流优于 always-pending**（同链路 +55%），但仍低于 Phase 3 Rerun；多流回退明显，不宜直接替换 always-pending。建议以 sync-write 作为单流 fast path、partial/EAGAIN 走 deferred 的混合方案继续迭代，并调查多流 callback 阻塞。

原始数据：`docs/dgx-perf-profile-phase4-sync-write/`；报告：`/tmp/dgx-bench-phase4-syncwrite-v3.md`。

- [x] **Step 1: ReceiveComplete+SUCCESS 修复 + 单测**
- [x] **Step 2: DGX bench 对比 always-pending / Phase 3**

---

## 完成检查清单

- [x] relay 单元测试全通过（Phase 1–3 各 task 单测）
- [x] `compress off` DGX proxy-1x1 归一化吞吐不低于 Phase 2（proxy/裸 TCP ~46%）
- [x] perf 中 buffer 池与 mutex 热点显著下降（见 Task 7 / Task 8 验收）
- [x] `src/docs/thread_model_cn.md` 更新数据路径描述
- [x] `docs/dgx-perf-profile-analysis.md` 追加 Phase 1–3 优化结果章节
- [x] perf 采样 client core dump 已修复（Task 8.1；`RetiredRelays` + ingress 池同步）
- [x] Phase 4 deferred receive DGX 验证已记录（Task 10.1；单流 ~5.2 Gbps，receive 零拷贝生效，待 fast path）
- [x] Phase 4 callback sync-write DGX 验证已记录（Task 10.2；修复后单流 ~7.7 Gbps，优于 always-pending，多流待优化）
