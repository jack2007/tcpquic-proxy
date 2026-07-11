# Darwin fully-closed relay 收敛设计

**日期：** 2026-07-11  
**状态：** 设计已确认；实现计划见 `docs/superpowers/plans/2026-07-11-darwin-fully-closed-convergence.md`  
**问题文档：** `docs/2026-07-11-darwin-http-connect-zombie-relay.md`（H1 已证据闭环）  
**对照：** Linux `MaybeStopFullyClosedRelay` / `SetRelayStop`（`src/tunnel/linux_relay_worker.cpp`）

## 1. 背景

HTTP CONNECT 客户端在浏览器/连接关闭后，Darwin relay 已完成双向半关闭数据面（TCP EOF → QUIC FIN、peer FIN → `SHUT_WR`），但缺少 Linux 的 fully-closed → `SignalStop` 收敛。现场与诊断复现均证实 **H1**：`FullyClosedPredicateReady` 成立且长期无 `relay_stopping`。

诊断阶段已落地（只观测、不 stop）：

- `FullyClosedPredicateReady` / `TraceHalfClose` / half-close metrics / `stats_darwin_half_close`
- 复现证据见问题文档 §9.6

本设计在 §8 评审约束下实施收敛修复。

## 2. 目标

1. 双向半关闭且本地队列排空后，worker 幂等调用 `StopControl->SignalStop(ControlGeneration)`，由既有 reaper / unregister / FD close 收敛。
2. 收敛判定与 `SignalStop` **仅在 worker 线程**执行；callback / send-completion fallback 不得扫描 worker-owned 队列。
3. `PEER_SEND_SHUTDOWN`、`SEND_SHUTDOWN_COMPLETE`、convergence wake 在 event queue 满时 **不可静默丢失**（sticky + Wake）。
4. 谓词覆盖 binding callback pending receive（§8.5），避免未写完 TCP 就 stop。
5. 保留并增强现有 half-close 可观测性：stop 成功后 `blockers` / metrics 不再长期出现 `predicate_ready_no_signal_stop`。

## 3. 非目标

- 不改 Linux / Windows relay。
- 不把谓词升级为强制 `SEND_SHUTDOWN_COMPLETE`（见 §4 产品语义 B）。
- 不用 timeout 强关已 started stream wrapper 掩盖未收到 terminal。
- 不修 tunnel admin `tcp_*_bytes=0`（次要，另开）。
- 不在本设计引入独立 terminal 控制环或无限扩大主 event queue。

## 4. 产品语义（已锁定）

**语义 B — 兼容 Linux 当前行为：**

- 收敛谓词使用 `QuicSendFinSubmitted && QuicSendFinCompleted`（FIN 的 `SEND_COMPLETE` / buffer released）。
- **不**要求 `QuicSendShutdownCompleteObserved` 为真才 stop。
- 文档与日志必须明确：这是「本地队列排空后可 abortive retire」，与严格优雅关闭（FIN 已被对端确认）不同。
- `SEND_SHUTDOWN_COMPLETE` 仍更新观测标志并触发收敛检查，但不作为谓词必要条件。
- `SEND_COMPLETE.Canceled=true` 的 FIN **不得**记为成功的 `QuicSendFinCompleted`（与 Linux 对齐时一并核对；若 Darwin 当前未区分，实现计划中补上）。

## 5. 方案选择

| 方案 | 摘要 | 结论 |
|------|------|------|
| A. Binding sticky bits + Wake | enqueue 失败置 sticky，worker 消费后收敛 | **采用** |
| B. 独立 terminal 控制环 | 另建控制队列 | 过重，拒绝 |
| C. 仅加大主队列 / 重试 | 持续满仍丢 | 拒绝 |

## 6. 架构

```text
任意线程 callback
  → TryPush(half-close / shutdown event) + Wake
  → 失败: 置对应 sticky atomic + Wake
  → 禁止: 扫描 Pending* / 调用 MaybePublishFullyClosedStopLocal

worker 线程
  → DrainEvents / DrainFallbacks
  → ConsumeHalfCloseStickies(relay)   // 合并进 RelayState
  → 状态转移处理（EOF / FIN / peer FIN / SHUT_WR / drain）
  → MaybePublishFullyClosedStopLocal(relay, trigger)
       → assert IsWorkerThread()
       → FullyClosedPredicateReady(relay)  // 可复用并扩展
       → StopControl->SignalStop(ControlGeneration)  // 幂等
       → TraceHalfClose(..., "signal_stop") 或等价 stop 日志
```

### 6.1 Sticky 字段（建议挂在 `StreamBinding`）

| Sticky | 含义 | worker 消费动作 |
|--------|------|-----------------|
| `PeerSendShutdownSticky` | peer FIN 入队失败 | 等价 `ProcessPeerSendShutdown` |
| `SendShutdownCompleteSticky` | send shutdown complete 入队失败 | 等价 `ProcessSendShutdownComplete` |
| `ConvergenceCheckSticky` | off-worker 仅需再检查收敛 | 仅触发 `MaybePublishFullyClosedStopLocal` |

规则：

- sticky 用 `atomic` / `exchange`，可重复置位；消费幂等。
- sticky 与正常 event 可乱序、可重复；不得要求恰好一次交付。
- `EnqueueRelayCloseFromCallback` 失败路径必须置 sticky（至少 peer send shutdown）；现有「忽略失败」改为可观测计数 + sticky。

### 6.2 收敛 helper

新增 worker-owned：

```text
MaybePublishFullyClosedStopLocal(RelayState& relay, const char* trigger)
```

行为对齐 Linux `MaybeStopFullyClosedRelay` + `SetRelayStop` 的 stop 侧：

- `Closing` 或 `StopControl == nullptr` → return
- `!FullyClosedPredicateReady(relay)` → return（可继续 `TraceHalfClose`）
- `StopControl->SignalStop(ControlGeneration)`；成功且首次 stop 时打 trace（trigger 固定前缀便于 `rg`，如 `fully_closed_stop`）

**不**在本 helper 内直接 `UnregisterRelay` / `close(fd)`；生命周期仍走 reaper。

### 6.3 谓词

以现有 `FullyClosedPredicateReady` 为基线（诊断阶段已含 callback pending atomics），实现时确认：

- `!Closing`
- `TcpReadClosed && TcpWriteClosed`
- `QuicSendFinSubmitted && QuicSendFinCompleted`
- `InFlightQuicSends == 0`，`PendingQuicSends` / `PendingTcpWrites` / `PendingQuicReceives` 空
- `PendingQuicReceiveBytes == 0`，`PendingTcpWriteBytes == 0`
- `!TcpWriteShutdownQueued`（queued 尚未 `SHUT_WR` 完成）
- binding：`CallbackPendingReceiveEvents/Bytes == 0`
- 无未消费的 half-close sticky（或 Consume 后再判）

`TraceHalfClose` 在 stop 成功后不应再报 `predicate_ready_no_signal_stop`；可改为 `signal_stop` / `already_stopped`。

## 7. 触发矩阵

以下状态转移后必须调用 `MaybePublishFullyClosedStopLocal`（worker 路径）：

1. TCP EOF 成功提交 FIN 后（含 `EnableQuicSends=false` 同步完成测试分支）
2. worker 处理 FIN `SEND_COMPLETE`（`QuicSendFinCompleted=true`）后
3. `ProcessSendShutdownComplete` 后
4. `FlushTcpWrites` 完成 `SHUT_WR`、`TcpWriteClosed=true` 后
5. pending TCP write / QUIC receive / callback pending receive 排空到空后
6. `ConsumeHalfCloseStickies` 合并状态后
7. TCP read 背压解除并可能补读 EOF 后

Off-worker `CompleteQuicSend`（FIN buffer released）：只更新允许在 fallback 锁下更新的标志（或置 `ConvergenceCheckSticky`），**不**调用收敛 helper；由 worker 消费 sticky 后检查。

## 8. 测试门槛

确定性单测（`src/unittest/darwin_relay_worker_io_test.cpp` 为主）至少覆盖：

| 场景 | 关键断言 |
|------|----------|
| EOF → FIN complete → peer FIN | 无 MsQuic `SHUTDOWN_COMPLETE` 也 `SignalStop` |
| peer FIN → SHUT_WR → EOF → FIN complete | 反向顺序同样 stop |
| queue 满丢 peer FIN enqueue | sticky 仍使 `TcpWriteClosed` 并 stop |
| callback pending receive 非空 | 不得 stop；排空后 stop |
| in-flight / pending send 未完 | 不得 stop |
| 重复 sticky / 重复 peer FIN | stop 与 accounting 幂等 |
| generation mismatch | 不错误 stop；诊断计数增加 |

手工回归：

- HTTP CONNECT 短连接关闭后：`fully_closed_predicate_ready_relays` 不长期等于 `active_relays`
- `relay_control_stop_signaled` 上升；`active_relays` / ingress FD 数秒内归零
- 日志出现 `fully_closed_stop`（或等价），不再长期 `predicate_ready_no_signal_stop`

系统级 soak（发布建议，可与实现并行排期）：并发 CONNECT + 小 event queue；关闭后 1s 内 active 下降 ≥99%，5s 归零；payload 字节一致。

## 9. 文件边界

| 路径 | 职责 |
|------|------|
| `src/tunnel/darwin_relay_worker.h` | sticky 字段、helper 声明 |
| `src/tunnel/darwin_relay_worker.cpp` | sticky 消费、收敛 helper、触发点、enqueue 失败路径 |
| `src/unittest/darwin_relay_worker_io_test.cpp` | 确定性收敛 / sticky 测试 |
| `docs/2026-07-11-darwin-http-connect-zombie-relay.md` | 状态改为修复中/已修；链到本设计 |
| `docs/relay_macos.md` | P0 条目更新 |

可选：metrics 增加 sticky 消费 / fully-closed stop 计数（若现有 half-close 字段不足）。

## 10. 成功标准

- H1 复现场景下，半关闭完成后及时 `SignalStop`，tunnel/relay/FD 收敛。
- 单测矩阵通过；手工回归无长期 zombie。
- 文档明确语义 B 与 sticky 交付契约。

## 11. 决策记录

| 决策 | 选择 |
|------|------|
| FIN 完成语义 | B：`QuicSendFinCompleted`（兼容 Linux） |
| Event queue 满 | 同计划必做 sticky + Wake（方案 A） |
| 收敛执行线程 | 仅 worker |
| Stop 后生命周期 | 既有 reaper；helper 不直接 close FD |
