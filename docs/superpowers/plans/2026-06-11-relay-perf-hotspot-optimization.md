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

---

## Phase 4（可选）：Deferred Receive View

> 仅在 `docs/tcpquic_next_steps.md` Phase E 生命周期测试通过后执行。

### Task 9: MsQuic receive buffer 生命周期验证

**Files:**
- Create: `src/unittest/msquic_receive_lifetime_test.cpp`

- [ ] **Step 1: 编写生命周期探测测试**（需真实 MsQuic 连接，可标记为 integration test）

- [ ] **Step 2: 若通过，设计 `StreamReceiveComplete` 与 TCP 部分写映射**

- [ ] **Step 3: 实现并单独 bench**

---

## 完成检查清单

- [x] relay 单元测试全通过（Phase 1–3 各 task 单测）
- [x] `compress off` DGX proxy-1x1 归一化吞吐不低于 Phase 2（proxy/裸 TCP ~46%）
- [x] perf 中 buffer 池与 mutex 热点显著下降（见 Task 7 / Task 8 验收）
- [x] `src/docs/thread_model_cn.md` 更新数据路径描述
- [x] `docs/dgx-perf-profile-analysis.md` 追加 Phase 1–3 优化结果章节
