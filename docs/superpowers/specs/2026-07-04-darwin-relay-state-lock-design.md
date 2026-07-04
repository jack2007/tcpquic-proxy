# Darwin relay state/map lock 热路径出清设计

## 背景

`docs/relay_macos.md` 将 macOS relay 转发路径中热度最高的同步点列为：

1. `RelayState::Mutex`：单 relay 数据面状态锁。
2. `TqDarwinRelayWorker::RelayMutex`：单 worker relay map 生命周期锁。

send completion 锁出清和 receive callback hardening 已经完成之后，当前代码已经具备若干前置能力：

- active TCP->QUIC send completion 已迁到 `ActiveSendOperations` + operation 原子状态，正常 SEND_COMPLETE 不再走 `KnownSendMutex` / `CompletionState::Mutex`。
- RECEIVE callback 已改为 `TqDarwinQuicReceiveEnqueueResult`，并支持 `CallbackPendingQuicReceives` queue-full backpressure。
- `StreamBinding` 已持有 `std::weak_ptr<RelayState> Relay`，callback 不再需要通过 `FindRelay()` 查 worker map 才能拿到 relay owner。
- worker kqueue / receive view 主路径已经开始使用 `FindRelayLocal()`。
- 非 worker `UnregisterRelay()` 已事件化优先，只有 worker 已停止或 kqueue 不可用时才 fallback local unregister。
- `RelayMapAccessMutex` 已引入，用于隔离 worker-local map 读和 lifecycle map/deque 修改。

因此本设计不是重新执行旧计划，而是从当前代码出发，完成剩余热路径锁边界收敛：将 `RelayState::Mutex` 和 `RelayMutex` 明确限制在生命周期、stop、fallback、测试辅助和 best-effort snapshot 边界，避免稳定转发数据面再依赖这些锁。

## 目标

1. worker 线程数据面函数不持有 `RelayState::Mutex`。
2. worker 线程数据面函数不调用需要 `RelayMutex` 的 `FindRelay()`，统一使用 `FindRelayLocal()` 或事件自带 owner。
3. callback 不直接写 `RelayState` worker-only 字段；需要改变 relay 状态时只投递 worker event，或在 worker 已停止的 fallback 路径执行 lifecycle close。
4. running worker 的 `Snapshot()` 保持事件化；`SnapshotLocal()` 不逐条加 `RelayState::Mutex`，只做 worker-thread / stopped-state best-effort 读取。
5. 保留 `RelayMutex` 用于 register/unregister/retire/purge/stop/snapshot map 边界，保留 `RelayState::Mutex` 用于 lifecycle cleanup、非 worker fallback、测试辅助和 retired relay 观察。
6. 不改变 TCP/QUIC 转发语义、receive ownership、send completion 释放规则、fake FIN fail-fast 行为或 public metrics 字段。

## 非目标

- 不删除 `RelayState::Mutex` 或 `RelayMutex` 本身。
- 不重写 `ActiveSendMutex`、`KnownSendMutex`、`CompletionState::Mutex`。
- 不实现 Darwin receive sink。
- 不做平台中性 tuning/metrics 命名重构。
- 不优化 `FlushTcpWrites()` 中 pending receive 查找复杂度；该 P2 性能问题单独计划。
- 不改 Linux/Windows relay 行为。

## 当前剩余问题

### 1. callback 仍有直接 worker-only 字段写入

`StreamCallback()` 在部分 receive 失败分支中通过 `binding->Relay.lock()` 后直接写：

- `relay->Closing = true`（allocation failed）
- `relay->QuicReceivePaused = true`（budget rejected）

`QueueDeferredQuicReceive()` queue-full 分支也会 lock relay 并写 `relay->QuicReceivePaused = true`。

这些字段属于 worker-owned 数据面状态。callback 可调用 MsQuic `ReceiveSetEnabled(false)` 或投递 close 事件，但不应直接修改 relay 状态。否则 `RelayState::Mutex` 无法真正从 callback/data-plane 交界处退场。

### 2. `UpdateTcpInterest()` 仍是锁读取版本

当前同时存在：

- `UpdateTcpInterest()`：读取 `TcpReadArmed` / `TcpWriteArmed` 时加 `RelayState::Mutex`。
- `UpdateTcpInterestLocal()`：worker-thread-only，不加 relay lock。

数据面路径应全部调用 `UpdateTcpInterestLocal()`；`UpdateTcpInterest()` 仅保留给非 worker lifecycle fallback，或被审计后删除。

### 3. lifecycle cleanup 仍需要锁，但边界要文档化

`RetireRelay()`、`CloseRelay()`、`PurgeRetiredRelaysIfSafe()` 会在 stop/unregister/retired cleanup 中读取或修改 relay 字段。这些路径可以保留 `RelayState::Mutex` / `RelayMutex`，但必须被明确标记为 lifecycle-only，不能被 worker 数据面复用。

### 4. snapshot 是 best-effort 数据读取

running worker 的 `Snapshot()` 通过 event queue 在 worker 线程执行 `SnapshotLocal()`。因此 active relay 字段可以在 worker 线程无 per-relay lock 读取。worker 停止后的 `SnapshotLocal()` 是静止状态 best-effort 读取。这个契约需要写入计划并由测试覆盖并发 unregister/snapshot 不挂起。

## 设计

### RelayState 字段所有权

将 `RelayState` 字段分为三类：

**worker-only 数据面字段**，只允许 worker 线程读写：

- kqueue readiness / backpressure：`TcpReadArmed`、`TcpWriteArmed`、`TcpReadPausedByQuicBacklog`
- half-close 状态：`TcpReadClosed`、`TcpWriteClosed`、`QuicSendClosed`、`QuicReceiveClosed`、`QuicSendFinSubmitted`、`QuicSendFinCompleted`
- send accounting：`SubmittingQuicSends`、`InFlightQuicSends`、`InFlightQuicSendBytes`、`PendingQuicSends`
- receive/write queues：`PendingQuicReceives`、`PendingQuicReceiveBytes`、`PendingTcpWrites`、`PendingTcpWriteBytes`、`TcpWriteShutdownQueued`
- pause/resume：`QuicReceivePaused`
- byte counters and buffers：`TcpReadBytes`、`TcpWriteBytes`、`TcpReadBuffers`、`CompressionOutput`、`DecompressionOutput`
- data-plane close marker while active：`Closing`

**lifecycle fields**，可由 lifecycle helper 在锁边界修改：

- `TcpFd`
- `Stream`
- `PublicHandle`
- `Binding`
- retired cleanup 中观察的 `InFlightQuicSends`

**immutable-after-register fields**，注册完成后不再修改：

- `Id`
- `Compressor`、`Decompressor`、`CompressAlgo`
- `EnableQuicSends`

### callback 状态变更原则

callback 不直接写 worker-only relay 字段。

- RECEIVE enqueue success：返回 `QUIC_STATUS_PENDING`，worker 处理 `QuicReceiveView` 后维护 relay 状态。
- `EventQueueFull`：callback 只将 receive 暂存在 `StreamBinding::CallbackPendingQuicReceives`，调用 `ReceiveSetEnabled(false)`，返回 `PENDING`；worker drain 后 retry 并决定是否 resume。callback 不写 `relay->QuicReceivePaused`。
- `CallbackBudgetRejected`：callback 可以调用 `ReceiveSetEnabled(false)` 并 `ReceiveComplete(totalLength)`，但不写 `relay->QuicReceivePaused`。需要后续 resume 时，由 worker 侧 flush/retry 或下一次状态处理决定。
- `AllocationFailed`：callback 投递 close event；若 worker 已停止才走 fallback close。callback 不写 `relay->Closing`。
- abort/shutdown：callback 通过 `EnqueueRelayCloseFromCallback()` 投递 close event；队列不可用且 worker stopped 时 fallback `CloseRelay()`。

### Relay map 边界

- `FindRelay()`：只允许 public/test/fallback/lifecycle 路径使用。
- `FindRelayLocal()`：worker kqueue/event/data-plane 路径使用。
- `Relays` / `RetiredRelays` 结构性修改：继续由 `RelayMapAccessMutex` + `RelayMutex` 保护。
- worker-local map read：由 worker 线程独占执行；必要时在 `FindRelayLocal()` 内用 shared access guard 或 assert 保护，与当前 `RelayMapAccessMutex` 契约一致。

### 数据面函数边界

以下函数应保持 worker-thread-only，不持有 `RelayState::Mutex`：

- `ProcessTcpEvents()`
- `DrainTcpReadable()`
- `SubmitTcpBatchToQuic()`
- `TrySubmitQuicSendOperation()`
- `RetryPendingQuicSends()`
- `CompleteQuicSend()` 的 worker-thread active relay 分支
- `ProcessQuicReceiveViewEvent()`
- `EnqueueQuicReceiveForTcp()`
- `DiscardDeferredQuicReceive()` 的 worker relay 分支
- `FlushTcpWrites()`
- `SetTcpReadBackpressure()`
- `SetQuicReceiveEnabled()`
- `MaybePauseQuicReceive()`
- `MaybeResumeQuicReceive()`
- `FlushAllCallbackPendingQuicReceivesLocal()`
- `SnapshotLocal()` running-worker scan

### lifecycle / fallback 允许锁点

以下路径允许保留 `RelayState::Mutex` 和/或 `RelayMutex`：

- `RegisterRelayWithIdLocal()` 初始化 relay，直到后续任务移除多余初始锁。
- `UnregisterRelayLocal()` map erase + kqueue filter removal。
- `RetireRelay()` 和 `CloseRelay()` cleanup。
- `PurgeRetiredRelaysIfSafe()` retired relay 是否可释放的观察。
- `Stop()` worker 已退出后的 relay swap / retire。
- 非 worker fallback `CompleteQuicSend()` / retired binding cleanup。
- `TCPQUIC_TESTING` helper。

## 测试策略

新增或更新 `src/unittest/darwin_relay_worker_io_test.cpp`：

1. receive callback queue-full / budget-reject 不增加 locked relay lookup，也不依赖 direct relay field write 才能恢复。
2. allocation failure callback 投递 close event，最终 active relay 归零，locked lookup count 不增加。
3. kqueue TCP read/write path locked lookup count 不增加。
4. worker receive view path locked lookup count 不增加，并保持 `ReceiveComplete()` 语义。
5. snapshot 与 unregister 并发不崩溃、不挂起。

保留已有 Darwin relay tests：

- `tcpquic_darwin_relay_worker_io_test`
- `tcpquic_darwin_relay_worker_queue_test`
- `tcpquic_darwin_relay_metrics_test`
- `tcpquic_darwin_reactor_test`

## 风险

- callback 不再写 `QuicReceivePaused` 后，必须确保 worker 能在 backpressure drain 后 resume，否则可能出现 stream 长期 paused。测试需覆盖 queue-full retry/resume。
- direct unregister 重新事件化后，worker 长时间 `FlushTcpWrites()` 仍可能拖慢 unregister；本阶段通过 retry wake 和 bounded burst 保持现状，不进一步重写 flush fairness。
- `SnapshotLocal()` 无 per-relay lock 读取是 best-effort，不应被解释为严格一致快照。
- `RelayMapAccessMutex` 与 `RelayMutex` 锁顺序必须保持一致：先 map access，再 `RelayMutex`。
- `StreamBinding::Relay` 只能作为 callback 临时 owner/fallback close 入口，不能长期延长 relay 生命周期。

## 验收标准

- `StreamCallback()` RECEIVE / abort / shutdown 正常 running-worker 路径不调用 `FindRelay()`。
- callback RECEIVE 失败分支不直接写 `relay->Closing` 或 `relay->QuicReceivePaused`。
- worker kqueue/event/data-plane 路径不调用 `FindRelay()`。
- worker data-plane 函数不持有 `std::lock_guard<std::mutex> relayLock(relay->Mutex)`。
- `RelayState::Mutex` 剩余使用点仅在 lifecycle/fallback/test/retired cleanup 中。
- `RelayMutex` 剩余使用点仅在 register/unregister/retire/purge/stop/snapshot map 边界和 fallback/test 中。
- Darwin relay worker IO/queue/metrics/reactor 测试通过。
- `docs/relay_macos.md` 更新为 rank 1–2 已完成或正在实施，并保留 P0/P2/P3 后续事项。
