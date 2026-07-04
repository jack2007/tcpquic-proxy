# Darwin receive callback hardening 设计

## 背景

`docs/relay_macos.md` 在 send completion 锁出清（rank 3–5，`docs/superpowers/plans/2026-07-04-darwin-send-completion-lock.md`）之后，将下一组工作指向 **QUIC receive callback 失败语义**（P1）和 **event queue 背压/可观测性**（P2 rank 6 部分）。

当前 Darwin `StreamCallback()` RECEIVE 路径行为：

1. fake FIN：`TqIsMsQuicFakeFinReceive()` 命中后 trace + `assert(false)` + `std::abort()` — **有意保留的 fail-fast 设计**（与 Windows 一致；Linux 另有 graceful 路径）。本阶段不修改。
2. 正常 receive：调用 `QueueDeferredQuicReceive()`；成功返回 `QUIC_STATUS_PENDING`，失败仅递增 `Errors` 并返回 `QUIC_STATUS_SUCCESS`。
3. `QueueDeferredQuicReceive()` 内部可能因 allocation 失败、空 buffer、callback receive budget 拒绝、event queue 满而失败；失败路径没有细分计数，也没有统一的 MsQuic buffer ownership 语义。

Linux relay 已在 `docs/superpowers/plans/2026-07-04-linux-relay-callback-hardening.md` 中处理 fake FIN 和 stream lookup metrics，并在 `QueueDeferredQuicReceiveFromOffset()` 的 enqueue 失败路径上 **pause receive + 暂存 pending view**。Darwin 可借鉴 enqueue 失败/backpressure 思路，但 **不照搬 fake FIN graceful 路径**。

## 目标

1. 为 `QueueDeferredQuicReceive()` 引入可观测的失败原因，替换「只记 `Errors`」的粗粒度行为。
2. 明确 RECEIVE callback 在各类失败下的 MsQuic 返回值与 `ReceiveComplete()` 调用边界，消除 buffer ownership 歧义。
3. event queue 满时优先 **pause receive + 可恢复 backpressure**，而不是 silent SUCCESS + 关 relay（当前 StreamCallback 失败路径甚至未标记 closing）。
4. 在 `TqDarwinRelayWorkerSnapshot`（及可选 admin metrics）暴露细分计数，便于区分 queue 满、budget 拒绝、enqueue 失败等。
5. （可选后续 task）降低 `FlushTcpWrites()` 对 `PendingTcpWrites` 的线性扫描成本。

## 非目标

- 不修改 fake FIN abort 行为；`docs/relay_macos.md` 已文档化为设计契约。
- 不处理 `RelayState::Mutex` / `RelayMutex` 出清（见 `docs/superpowers/plans/2026-07-04-darwin-relay-state-lock.md`）。
- 不处理平台中性 tuning/metrics 命名（P0/P2 配置项另排）。
- 不实现 macOS receive sink（P0 产品决策另排）。
- 不改 Linux/Windows relay。
- 不在本阶段实现 QUIC receive hysteresis 或 `DeferredReceiveCompleteBatchBytes` 批量归还（可后续独立 plan）。

## 当前 RECEIVE 数据流

```text
MsQuic RECEIVE callback (任意线程)
  -> StreamBinding 校验 (Active, Worker)
  -> fake FIN? -> trace + abort (设计行为，本阶段不动)
  -> QueueDeferredQuicReceive()
       -> 分配 TqDarwinPendingQuicReceive
       -> ReserveCallbackReceiveBudget(binding, bytes)
       -> EnqueueEvent(QuicReceiveView) + Wake
  -> 成功: QUIC_STATUS_PENDING
  -> 失败: Errors++, QUIC_STATUS_SUCCESS  (问题焦点)

worker 线程
  -> ProcessQuicReceiveViewEvent()
  -> PendingQuicReceives / EnqueueQuicReceiveForTcp / FlushTcpWrites
  -> CompleteDeferredQuicReceive() -> Stream::ReceiveComplete(bytes)
```

`QueueDeferredQuicReceive()` 仅在 **enqueue 失败且 budget 已预留** 时调用 `ReleaseCallbackReceiveBudget()`；其它失败发生在 budget 预留之前。callback 从未返回 `PENDING` 时，MsQuic 仍持有 receive buffer — 此时返回 `SUCCESS` 且不调 `ReceiveComplete()` 的语义需在本阶段钉死。

## 方案选择

推荐方案：**失败原因枚举 + 分路径 callback 语义 + queue-full backpressure**。

### 失败分类

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

`QueueDeferredQuicReceive()` 改为返回 `TqDarwinQuicReceiveEnqueueResult`（或等价的 `bool` + out-param reason）。每个失败类映射到独立 worker 计数器。

### Callback 返回语义（推荐）

| 结果 | callback 行为 | MsQuic 返回值 |
|------|---------------|---------------|
| `Ok` | 已入队，buffer 由 worker 稍后 `ReceiveComplete` | `QUIC_STATUS_PENDING` |
| `InvalidArgs` / `EmptyNonFin` / `NullBuffer` | 记录计数 + trace；无 buffer 可持有 | `QUIC_STATUS_SUCCESS` |
| `AllocationFailed` | 记录计数；关闭 relay（或投递 close event）；若有 payload 则 `ReceiveComplete(totalLength)` 后返回 | `QUIC_STATUS_SUCCESS` 或 `OUT_OF_MEMORY`（以实现时 MsQuic 行为验证为准） |
| `CallbackBudgetRejected` | pause receive（`ReceiveSetEnabled(false)`）；记录 `CallbackReceiveBudgetRejects`；对当前 event 调 `ReceiveComplete(totalLength)` 避免 buffer 挂起 | `QUIC_STATUS_SUCCESS` |
| `EventQueueFull` | pause receive；将 view 暂存到 binding/worker 侧 backpressure 结构（对齐 Linux `CallbackPendingQuicReceives` 思路）；记录 `EventQueueFull`；返回 `QUIC_STATUS_PENDING` 仅当 view 已被 worker 逻辑接管 — **若仅 pause 而不持有 buffer，则必须 `ReceiveComplete` 后再 SUCCESS** | 以实现验证为准；优先与 Linux「pause + 暂存 + PENDING」对齐 |

**关键原则：** 只有 worker 已明确接管 buffer 生命周期（入队成功或进入 backpressure 暂存且 callback 返回 PENDING）时，才允许不立即 `ReceiveComplete`。其它失败必须在 callback 返回前显式 `ReceiveComplete(totalLength)` 或返回 MsQuic 可识别的错误状态。

### Event queue 满 backpressure

借鉴 Linux `QuicReceiveViewBackpressureQueued`：

- enqueue 失败时 pause 该 stream 的 receive；
- 将 `TqDarwinPendingQuicReceive` 暂存到 `StreamBinding` 或 relay 级 backpressure deque；
- worker drain 事件队列后 retry 入队；
- 计数 `QuicReceiveViewBackpressureQueued` / `EventQueueFullErrors`。

Darwin 当前没有 `CallbackPendingQuicReceives` 等价结构 — 本阶段需新增最小 backpressure 容器（可挂在 `StreamBinding` 上，worker 线程 drain 时 flush）。

### 可观测性

在 `TqDarwinRelayWorkerSnapshot` 增加（命名可与 Linux 对齐以便 metrics 聚合）：

- `EventQueueFullErrors`
- `WakeFailures`（若 `Wake()` 失败尚未单独计数）
- `QuicReceiveEnqueueFailures`（总计或按 reason 拆分）
- `CallbackReceiveBudgetRejects`
- `QuicReceiveViewBackpressureQueued`

`TCPQUIC_TESTING` 下提供 `ForTest()` accessor，便于 characterization 测试（参照 send completion lock instrumentation）。

## 错误处理

- fake FIN：保持 abort，不进入本方案。
- budget 拒绝：不应 silent SUCCESS 后让 relay 继续接收；pause + 计数。
- queue 满：不应仅 `Errors++`；pause + backpressure 暂存 + worker retry。
- relay closing / worker null：现有 guard 保持，返回 `SUCCESS`。
- 所有新增路径必须维持 `ReleaseCallbackReceiveBudget()` 与 `CallbackPendingReceiveBytes/Events` 原子计数平衡。

## 测试策略

1. **characterization**：queue 满时当前行为（`Errors++`、`SUCCESS`）的可观测 baseline（Task 1 instrumentation）。
2. **budget 拒绝**：`MaxPendingQuicReceiveBytesPerRelay` 压测，断言 pause + 计数 + `ReceiveComplete` 调用（fake `ReceiveCompleteForTest`）。
3. **queue 满**：小 `EventQueueCapacity` + fill queue，触发 RECEIVE callback，断言 backpressure 计数、receive paused、进程不泄漏 buffer（`DeferredReceiveCompletes` 平衡）。
4. **成功路径回归**：现有 `QuicReceiveCallbackReturnsPending`、`QuicReceiveCallbackDefersPendingBytesUntilWorkerEvent` 等测试保持 PASS。
5. fake FIN：**不添加** graceful 测试；Darwin 保持 abort 契约。

## 范围外（本 design 自检）

- fake FIN graceful 路径
- `PendingTcpWriteRefs` 优化（implementation plan Task 6 可选）
- admin JSON 平台中性命名
- CI macOS runner（P3，可与本 plan 并行但不阻塞）

## 自检

- 目标与 `docs/relay_macos.md` P1/P2 条目一一对应（除已声明为设计行为的 fake FIN）。
- 方案没有把 queue 满 silently 降级为 SUCCESS。
- 与 send completion plan 正交，不修改 `ActiveSendOperations` / operation 状态机。
- Linux 仅作参考，不强制 API 完全一致。
