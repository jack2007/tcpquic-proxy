# Linux Relay 热点性能优化设计

> 基于 `docs/dgx-perf-profile-analysis.md`（DGX 200G LAN、proxy-1x1、perf 25s 采样）  
> 日期：2026-06-11  
> 状态：待评审

## 1. 背景与问题

在 DGX 无时延场景下，`tcpquic-proxy` 单连接吞吐约 **8.5 Gbps**，与 `secnetperf` 基线（~7.7 Gbps）绝对值接近，但 **proxy 额外消耗约 40% CPU 在 relay 层**。perf 显示：

| 热点 | Client | Server | proxy 独有 |
|------|--------|--------|-----------|
| mutex 原子（`swp`/`cas`） | ~33% | ~33% | 是 |
| `TqLinuxRelayBufferPool::Acquire/Release` | ~8% | ~8% | 是 |
| `malloc`/`free` + `shared_ptr` deleter | ~7% | ~7% | 是 |
| TLS AES-GCM | ~2% | ~0.8% | 否（被 relay 开销稀释） |
| `CxPlatHashtableEnumerateNext` | — | ~6% | 否 |

**结论**：差距主要来自 relay 层，而非 MsQuic QUIC 栈本身。优化 relay 后，下一瓶颈将回到 TLS 与 UDP 收包（与 secnetperf 对齐）。

### 1.1 代码审查补充（perf 报告未单独量化）

当前 QUIC→TCP 生产路径存在 **二次拷贝 + 双倍 buffer 池操作**：

```text
MsQuic 回调线程 (OnStreamEvent)
  -> CopyQuicReceiveToEvent
       -> FindRelayById (RelayLock)
       -> Pool.Acquire (Pool.Lock) + memcpy #1
       -> Enqueue (QueueLock)
  -> return

Relay worker 线程 (DrainEvents)
  -> ProcessQuicReceiveEvent
       -> FindRelayById (RelayLock)  # 再次查找
       -> EnqueueQuicReceive
            -> Pool.Acquire (Pool.Lock) + memcpy #2
            -> PendingTcpWrites
  -> FlushTcpWrites -> writev
```

TCP→QUIC 路径（`DrainTcpReadable`）仅在 worker 线程访问 `Pool`，但 MsQuic `SEND_COMPLETE` 回调经 `Enqueue` 归还 buffer 时仍可能与 worker 竞争同一把 `Pool.Lock`。

---

## 2. 目标与非目标

### 2.1 目标

| 指标 | 基线 | 目标（分阶段） |
|------|------|----------------|
| 单连接 DGX 吞吐（proxy-1x1，compress off） | ~8.5 Gbps | Phase 1 后 ≥9 Gbps；Phase 2 后接近 secnetperf 同 CPU 预算 |
| relay 层 CPU（mutex + pool + malloc） | ~40% | Phase 1 后 ≤25%；Phase 2 后 ≤10% |
| `TqLinuxRelayBufferPool::Acquire` perf 占比 | ~7% | Phase 2 后 <1%（worker 路径零锁） |
| 功能回归 | — | 现有 unit test + DGX bench 无退化 |

### 2.2 非目标

- 本设计 **不** 修改 MsQuic 内部（`CxPlatHashtableEnumerateNext` 等）——留作 Phase 4 调参项。
- 本设计 **不** 在 Phase 1–2 实现 QUIC receive buffer 零拷贝（deferred `StreamReceiveComplete`）——需专项生命周期验证（见 §6.4）。
- 本设计 **不** 改动 Windows relay（IOCP 路径独立演进）。
- 压缩开启路径保持正确性优先；优化以 `compress off` 高速场景为主，压缩路径允许保留额外拷贝。

---

## 3. 根因分析

### 3.1 Buffer 池全局互斥锁

```44:70:src/tunnel/linux_relay_buffer_pool.cpp
TqBufferRef TqLinuxRelayBufferPool::Acquire() {
    std::lock_guard<std::mutex> guard(Lock);
    // ...
    return TqBufferRef(buffer, [this](TqRelayBuffer* released) {
        Release(released);
    });
}
```

- 每个 relay 拥有独立 `TqLinuxRelayBufferPool`，但 **MsQuic 回调线程** 与 **relay worker 线程** 同时调用 `Acquire/Release`。
- `shared_ptr` 自定义 deleter 在任意线程释放时再次抢 `Pool.Lock`。
- `DrainTcpReadable` 一次 `readv` 批次最多 `MaxIov`（32）次 `Acquire`，与回调并发时锁竞争剧烈。

### 3.2 事件队列与 relay 查找锁

- `Enqueue` / `DrainEvents`：`std::deque` + `QueueLock`，每个 QUIC receive chunk 一次加锁。
- `FindRelayIdByStream` / `FindRelayById`：线性扫描 `Relays` + `RelayLock`，在 MsQuic 回调热路径执行。
- `Stream->Context` 当前仅存 `TqLinuxRelayWorker*`，无法 O(1) 定位 `RelayState`。

### 3.3 堆分配

- 池耗尽时 `new TqRelayBuffer`；`shared_ptr` 控制块额外分配。
- `TqLinuxRelayEvent` 按 chunk 入队，高吞吐时事件数 = 数据量 / ReadChunkSize（1MB chunk → 每 GB 约 1024 次事件）。

---

## 4. 方案对比

### 方案 A：渐进式修补（推荐）

分三阶段，每阶段可独立合并、可独立验证：

| 阶段 | 内容 | 风险 | 预期收益 |
|------|------|------|----------|
| **Phase 1** | 消除 QUIC→TCP 二次拷贝；合并 receive 事件；stream 上下文 O(1) 查找 | 低 | 10–15% CPU |
| **Phase 2** | per-relay 预分配 + worker 无锁池 + 去 shared_ptr；SPSC 事件环 | 中 | 再回收 20–30% CPU |
| **Phase 3** | 回调侧 ingress 槽位（双缓冲域），彻底分离跨线程池访问 | 中 | 收尾 3–5% |

**优点**：与现有 `2026-06-09-high-performance-threading-and-batching-design.md` v1 copy-into-pool 策略一致；每步可回滚。  
**缺点**：Phase 2 前回调路径仍需一次拷贝。

### 方案 B：激进零拷贝（deferred receive view）

MsQuic 回调仅入队 `QUIC_BUFFER` 视图，worker `writev` 完成后调用 `StreamReceiveComplete`。

**优点**：理论上消除 QUIC→TCP 拷贝。  
**缺点**：
- `QUIC_STREAM_EVENT_RECEIVE` buffer 在 callback 返回后可能失效（需 msquic 版本专项测试）。
- 必须处理 TCP 部分写、背压、FIN 与流控窗口时序。
- 与 `docs/tcpquic_next_steps.md` Phase E 前置条件一致——**不作为本设计默认路径**。

### 方案 C：架构重构（MsQuic worker 直写 TCP）

收包在 MsQuic 线程直接 `writev`，绕过 relay worker 队列。

**优点**：消除跨线程队列与双线程池竞争。  
**缺点**：违反当前线程模型（`src/docs/thread_model_cn.md`：MsQuic 回调不做 socket I/O）；解压、背压、epoll 集成复杂；回归风险高。

**推荐：方案 A**，方案 B 作为 Phase 4 可选项，方案 C 不采纳。

---

## 5. 详细设计（方案 A）

### 5.1 Phase 1：快速路径修复（P0-lite + P1-lite）

#### 5.1.1 消除 QUIC→TCP 二次拷贝

**变更**：`ProcessQuicReceiveEvent` 直接将 `event.Buffer` 移入 `PendingTcpWrites`，不再调用 `EnqueueQuicReceive` 重新 `Acquire` + `memcpy`。

```cpp
// 伪代码 — ProcessQuicReceiveEvent
if (event.Buffer && event.Length > 0) {
    relay->PendingTcpWrites.push_back(
        TqBufferView{event.Buffer->Data(), event.Length, event.Buffer});
}
if (event.Fin) relay->TcpWriteShutdownQueued = true;
FlushTcpWrites(relay);
```

- **压缩路径例外**：若 `relay->Decompressor != nullptr`，仍走 `EnqueueQuicReceive`（需连续内存解压）。
- **测试路径**：`compress off` 的 DGX bench 与现有 `linux_relay_worker_io_test` 无压缩用例直接受益。

#### 5.1.2 合并 receive 事件（减少 Enqueue 次数）

**变更**：`CopyQuicReceiveToEvent` 对单次 `QUIC_STREAM_EVENT_RECEIVE` 的所有 `QUIC_BUFFER` 合并为 **一个** `TqLinuxRelayEvent`：

```cpp
struct TqLinuxRelayEvent {
    // ...
    std::vector<TqBufferRef> Buffers;  // 新增：多 buffer 批次
    size_t TotalLength;
};
```

- 一次 RECEIVE 回调 → 一次 `Enqueue` → 一次 `FindRelayById`（Phase 1.3 后为零次）。
- 预期将 `QueueLock` 竞争降低 1/MaxChunk 倍。

#### 5.1.3 Stream 上下文 O(1) 定位 Relay

**新增**：

```cpp
struct TqStreamRelayBinding {
    TqLinuxRelayWorker* Worker;
    RelayState* Relay;  // 生命周期由 Relays deque 的 shared_ptr 保证
};
```

- `RegisterRelayWithId`：`binding = new TqStreamRelayBinding{this, relay.get()}`；`Stream->Context = binding`。
- `OnStreamEvent`：`auto* b = static_cast<TqStreamRelayBinding*>(stream->Context)`；直接使用 `b->Relay->Id`。
- `UnregisterRelay` / stream 替换：释放 binding，恢复 `NoOpCallback`。
- **删除** 热路径上的 `FindRelayIdByStream`。

#### 5.1.4 SendComplete 携带 RelayId

`SEND_COMPLETE` 事件在入队时从 `TqLinuxRelaySendOperation` 读取 `RelayId`（已有），`CompleteQuicSend` 避免 `FindRelayById`——改为 operation 内嵌 `RelayState*` 或仅递减 `OutstandingQuicSends` 通过 operation 已持有的 relay 指针。

### 5.2 Phase 2：Buffer 池无锁化（P0 核心）

#### 5.2.1 数据结构

```cpp
class TqRelayBufferSlot {
    uint8_t* Data;
    size_t Capacity;
    size_t Length;
    std::atomic<uint32_t> RefCount;  // 替代 shared_ptr
};

class TqLinuxRelayBufferPool {
    // worker 线程专用（无锁）
    std::vector<TqRelayBufferSlot*> WorkerFree;
    size_t WorkerAllocated;

    // 统计与上限（可用 atomic 或仅 worker 写）
    uint64_t PendingBytes;
    uint64_t MaxBytes;
    size_t ChunkBytes;
    size_t MaxSlots;
};
```

**规则**：
- `AcquireWorker()` / `ReleaseWorker()`：**仅** relay worker 线程调用，无 mutex。
- 预分配：`RegisterRelayWithId` 时 `ReserveSlots(MaxIov * 4)`，避免热路径 `new`。
- `TqBufferView::Owner` 改为 `TqRelayBufferSlot*` + 显式 `Retain/Release`，或 `TqBufferHandle` 轻量 RAII。

#### 5.2.2 回调线程 ingress 策略

MsQuic 回调仍需拷贝（v1 策略），但 **不与 worker 共享 free-list**：

```text
Option 2a（推荐）：回调使用 relay 预分配的 IngressSlots 环形缓冲
  - Ingress 与 Worker 各有一半 slot（或 1/3 : 2/3）
  - 回调 AcquireIngress()：无锁，仅回调线程写 head
  - worker DrainEvents 消费后 ReleaseWorker()

Option 2b（备选）：thread_local 批量缓存
  - 回调每 RECEIVE 批量向全局池借 N 个（一次加锁）
  - 实现简单但仍有锁，收益小于 2a
```

**推荐 2a**：ingress ring 与 worker free-list 分离，彻底消除跨线程 mutex。

#### 5.2.3 PendingBytes 记账

- Worker 路径：`PendingBytes` 仅 worker 更新（单线程）。
- Ingress 路径：使用独立 `IngressPendingBytes` 计数，移交时一次性转入 worker 账本。
- `DrainTcpReadable` 背压检查读 worker 侧计数，包含已移交但未释放的 buffer。

### 5.3 Phase 3：SPSC 事件环（P2）

替换 `std::deque<TqLinuxRelayEvent> + QueueLock`：

```cpp
class TqSpscEventRing {
    std::vector<TqLinuxRelayEvent> Slots;
    alignas(64) std::atomic<uint32_t> Head{0};
    alignas(64) std::atomic<uint32_t> Tail{0};
};
```

- **单生产者**：MsQuic 回调线程（每 stream 绑定一个 worker，回调仅入该 worker 队列）。
- **单消费者**：relay worker `DrainEvents`。
- 队列满时：回调返回 `QUIC_STATUS_OUT_OF_MEMORY` 或暂停 `StreamReceiveSetEnabled`（需与 MsQuic 流控对齐）——**必须实现背压，禁止丢事件**。

`Wake()` 逻辑保持不变（eventfd + `WakeArmed` 合并唤醒）。

### 5.4 Phase 4（可选）：Deferred Receive View

前置条件（来自 `docs/tcpquic_next_steps.md` Phase E）：

1. 单元测试：在 callback 返回后探测 `QUIC_BUFFER` 指针是否仍可读（msquic 版本相关）。
2. 集成测试：高吞吐下无 UAF、无 QUIC 流控异常。
3. 实现 `StreamReceiveComplete` 与 TCP 部分写的字节级对应。

通过后，ingress 拷贝可替换为 view 直挂 `PendingTcpWrites`，TCP 写完成后 complete 对应字节数。

---

## 6. 数据流（优化后）

### 6.1 QUIC → TCP（compress off，Phase 2 后）

```text
MsQuic quic_worker
  -> OnStreamEvent(RECEIVE)
  -> binding = stream->Context  // O(1)
  -> 从 IngressRing 取 slot
  -> memcpy(msquic_buf -> slot)  // 单次拷贝
  -> SpscRing.push(QuicReceive{batch of slots})
  -> Wake()
  -> return

relay worker
  -> DrainEvents
  -> ProcessQuicReceiveEvent
  -> move slots -> PendingTcpWrites  // 无二次拷贝
  -> FlushTcpWrites -> writev
  -> ReleaseWorker(slots)
```

### 6.2 TCP → QUIC（Phase 2 后）

```text
relay worker (epoll EPOLLIN)
  -> DrainTcpReadable
  -> AcquireWorker() x N  // 无锁
  -> readv -> SubmitTcpBatchToQuic
  -> SEND_COMPLETE 回调
  -> CompleteQuicSend -> ReleaseWorker via operation->Views
```

---

## 7. 测试与验证

### 7.1 单元测试

| 测试 | 文件 | 验证点 |
|------|------|--------|
| 无二次拷贝 | `linux_relay_worker_io_test.cpp` | receive 后 `Pool.PendingBytes` 不翻倍；`FreeCount` 符合预期 |
| 合并事件 | 新增 | 多 buffer RECEIVE 仅 1 次 queue depth 增加 |
| Stream binding | 新增 | 注销后 callback 不再访问已释放 relay |
| 无锁池 | `linux_relay_buffer_pool_test.cpp` | 预分配、Retain/Release、上限拒绝 |
| SPSC 背压 | 新增 | 环满时生产者阻塞/错误，不丢事件 |

### 7.2 性能验证

```bash
# 功能回归
make test

# DGX 吞吐（与 perf 基线同参数）
CASE=proxy-1x1 ./scripts/dgx-throughput-matrix.sh

# perf 对比（优化前基线已存档于 docs/dgx-perf-profile-rerun/）
CASE=proxy-1x1 DURATION_SEC=25 ./scripts/dgx-perf-profile.sh
```

**验收标准**（Phase 2 完成）：
- `TqLinuxRelayBufferPool::Acquire` 在 client/server perf top 中 **退出 Top 10**。
- mutex 原子符号（`__aarch64_swp4_rel` + `__aarch64_cas4_acq`）合计 **<10%**。
- 吞吐 ≥ 9 Gbps 或同等 CPU 下超过优化前 ≥15%。

---

## 8. 风险与缓解

| 风险 | 缓解 |
|------|------|
| Ingress ring 与 worker ring 槽位耗尽 | 按 `MaxIov * 4` 预分配；背压时禁用 stream receive |
| 去掉 shared_ptr 后 UAF | `TqBufferHandle` RAII + relay 注销时 drain 所有 pending |
| SPSC 环大小不足 | 默认 `EventBudget * 2`；高吞吐可调 `LinuxRelayWorkerEventBudget` |
| 压缩路径回归 | 压缩仍走独立路径；专项测试保留 |
| Windows 平台漂移 | Linux 改动限定在 `linux_relay_*`；Windows 不改 |

---

## 9. 实施顺序与里程碑

```text
M1 (Phase 1, ~1-2 天)
  ├─ 消除二次拷贝
  ├─ 合并 receive 事件
  ├─ StreamRelayBinding
  └─ DGX bench 验证

M2 (Phase 2, ~2-3 天)
  ├─ TqRelayBufferSlot + 无锁 worker 池
  ├─ Ingress ring 分离
  ├─ 去除 shared_ptr deleter
  └─ perf 复测

M3 (Phase 3, ~1-2 天)
  ├─ SPSC 事件环
  └─ 背压与流控联调

M4 (Phase 4, 可选)
  └─ MsQuic receive 生命周期验证 + deferred complete
```

---

## 10. 参考

- `docs/dgx-perf-profile-analysis.md` — perf 热点原始数据
- `docs/specs/2026-06-09-high-performance-threading-and-batching-design.md` — v1/v2 copy 策略
- `docs/tcpquic_next_steps.md` — Phase E 前置条件
- `src/tunnel/linux_relay_buffer_pool.cpp`
- `src/tunnel/linux_relay_worker.cpp`
- `src/docs/thread_model_cn.md`
