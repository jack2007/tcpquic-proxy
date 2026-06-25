# Windows IOCP 上 quic->tcp receive queue 现状记录

本文记录 2026-06-25 Windows relay 改造成类 Linux event queue 后，在 `quic -> tcp` 方向发现的队列调度问题、日志证据、当前修复和后续建议。

## 1. 相关队列模型

当前 Windows relay 同时存在三层“队列”概念，容易混淆：

1. **IOCP 队列**
   - OS 完成队列。
   - 承载真实 TCP overlapped I/O completion，例如 `WSARecv` / `WSASend` 完成。
   - 也承载 `PostQueuedCompletionStatus` 投递的 wake token。
   - wake token 只负责叫醒 worker，不携带 QUIC receive view。

2. **worker event queue (`EventQueue_`)**
   - 每个 `TqWindowsRelayWorker` 一个。
   - 存放用户态 `TqWindowsRelayTask`。
   - 用来把 MsQuic callback 线程上的事件转移到 relay worker 线程处理。
   - 当前承载 `QuicReceiveView`、`QuicSendComplete`、`QuicIdealSendBuffer`、peer abort、shutdown、close 等任务。

3. **relay receive 队列 (`relay->PendingReceives`)**
   - 每个 relay 一个。
   - 存放真正的 QUIC receive data view，即 `TqWindowsPendingQuicReceive`。
   - 负责保证 `quic -> tcp` 数据按顺序 drain 到 TCP。
   - 负责跟踪 MsQuic receive view 何时可以 `ReceiveComplete`。
   - 负责统计 pending receive bytes / queue depth，用于背压。

因此，QUIC receive callback 的当前路径不是“把 view 直接放进 IOCP”，而是：

```text
MsQuic receive callback
  -> 创建 TqWindowsPendingQuicReceive
  -> push 到 relay->PendingReceives
  -> 创建 TqWindowsRelayTask{QuicReceiveView, ReceiveView=view}
  -> push 到 worker EventQueue_
  -> PostQueuedCompletionStatus wake IOCP worker
```

worker 被 IOCP wake 后，会 drain `EventQueue_`，再处理其中的 `TqWindowsRelayTask`。

## 2. 当前发现的问题

这次复现中发现：同一个 `TqWindowsPendingQuicReceive` 可能被两个 `TqWindowsRelayTask::QuicReceiveView` 同时引用并重复处理。

重复的不是 `relay->PendingReceives` 里的 data view；重复的是 worker `EventQueue_` 中的 task：

```text
EventQueue_:
  task_for_view_B_1
  task_for_view_B_2

两个 task 的 ReceiveView 都指向同一个 view_B。
```

出现这种情况的原因是当前有两个唤醒来源：

1. view 初始入队时会投递一个 `QuicReceiveView` task。
2. 前一个 view 完成后，`FinishReceiveView` 会取新的队首 `nextView`，再投递一个 `QuicReceiveView` task，用于推动队列继续处理。

正常意图是：

```text
PendingReceives = [A, B]

B 的初始 task 如果先跑，但发现 B 不是队首，就返回。
A 完成后，FinishReceiveView 再投递 task 让 B 继续处理。
```

这个设计可以避免 worker 在 `B` 上阻塞等待 `A` 完成。

但存在另一种竞态：

```text
1. B 入 PendingReceives，投递 task_for_B_1。
2. A 很快完成，FinishReceiveView 又投递 task_for_B_2。
3. task_for_B_1 还没有执行，所以 EventQueue_ 里同时存在两个 B task。
4. task_for_B_1 执行，看到 B 已经是队首，提交 WSASend。
5. WSASend completion 还没有回来。
6. task_for_B_2 执行，也看到 B 是队首。
7. 旧逻辑没有判断 B 是否已经有 TCP send 在飞，于是再次提交同一个 view 的 WSASend。
```

结果是同一段 QUIC receive buffer 被重复写入 TCP，导致 relay 内部统计出现不可能状态：

```text
CompletedLength > TotalLength
AccountedLength > TotalLength
```

这会污染 TCP 字节流，后续可能表现为连接被关闭、页面部分资源加载失败，或页面卡住后刷新恢复。

## 3. 日志证据

新增 `event=relay_receive_view` 后，复现日志中可直接看到同一个 view 被重复推进。

典型证据：

```text
tunnel=2 target=cursor.com:443 view=0x1e48101ddf0 total=1642 completed=2134 accounted=2134
tunnel=6 target=cursor.com:443 view=0x1e48101d8f0 total=1742 completed=3484 accounted=3484
tunnel=14 target=cursor.com:443 view=0x1e48101df30 total=31 completed=62 accounted=62
```

`completed/accounted` 超过 `total` 不可能是正常 receive view 生命周期，说明同一个 view 的 TCP 写完成被重复计入。

同时，这些异常附近常伴随：

```text
stage=post_tcp_send_receive_view
stage=tcp_send_complete_receive_view
stage=finish_already_drained
trigger=finish_receive_view_already_drained
```

`finish_receive_view_already_drained` 本身不是根因，而是重复提交/重复完成后暴露出来的症状。

## 4. 当前修复

当前采取最小风险修复：让 `QuicReceiveView` task 处理变为幂等。

为 `TqWindowsPendingQuicReceive` 添加：

```cpp
bool TcpSendPending{false};
```

语义：

```text
TcpSendPending == true
  表示这个 receive view 已经提交过 WSASend，正在等待 TCP send completion。
```

在提交 receive view 对应的 `WSASend` 前设置：

```text
view->TcpSendPending = true
```

在 TCP send completion 回来后清除：

```text
view->TcpSendPending = false
```

在 `ProcessQuicReceiveViewTask` 中，如果发现：

```text
view 已经在 PendingReceives 中
view 是队首
view->TcpSendPending == true
```

则说明这是重复 task，直接返回，不再重复提交 `WSASend`。

修复后的预期日志：

```text
stage=process_send_pending
```

而不应该再看到同一个 view 在 TCP send completion 前重复出现：

```text
stage=post_tcp_send_receive_view
```

也不应该再出现：

```text
completed > total
accounted > total
```

## 5. Linux / macOS 是否有同类问题

目前看 Linux/macOS 没有 Windows 这次完全相同的问题路径。

### Linux

Linux 也有 `PendingQuicReceives`，但处理模型不同：

```text
QUIC receive event
  -> view push 到 relay->PendingQuicReceives
  -> FlushDeferredQuicReceives(relay)
```

`FlushDeferredQuicReceives` 自己循环处理队首 view。写不动时 arm `TcpWritable`，等 epoll 可写后继续 flush。Linux 不会在一个 view 完成后再投递一个新的 `QuicReceiveView` event 指向 next view。

所以 Linux 的重复唤醒对象是 TCP writable/flush，不是“多个 task 指向同一个 receive view”。

### macOS / Darwin

Darwin 收到 QUIC receive 后，会把 receive view 转成 `PendingTcpWrites`：

```text
QUIC receive view
  -> EnqueueQuicReceiveForTcp
  -> PendingTcpWrites
  -> kqueue writable 时 FlushTcpWrites
```

后续推进消费的是 `PendingTcpWrites`，也不是反复投递 `QuicReceiveView` task 指向同一个 receive view。

因此，这次问题是 Windows 新 event queue + overlapped `WSASend` 调度中的特有问题。

## 6. 对当前架构的观察

`TqWindowsRelayTask` 本身仍有价值：它是 MsQuic callback 线程到 relay worker 线程的跨线程调度载体。

它适合承载：

- QUIC send complete
- peer send/receive abort
- shutdown complete
- ideal send buffer 更新
- close relay / stop worker

但对 `quic -> tcp receive view` 这条路径，当前“每个 view 一个 `QuicReceiveView` task”的粒度过细，容易产生重复 task。

参考 Linux，更合理的 Windows receive 方向可能是：

```text
QUIC receive callback:
  push view -> relay->PendingReceives
  schedule relay receive drain
  wake worker

worker:
  DrainRelayReceive(relay)
  只处理队首 view
  如果 WSASend pending，则停止
  TCP send completion 回来后继续 DrainRelayReceive(relay)
```

也就是说，worker event queue 可以从：

```text
QuicReceiveView(view pointer)
```

调整为：

```text
RelayReceiveReady(relay id)
```

或者用 per-relay scheduled flag 避免重复调度：

```text
if (!relay->ReceiveDrainScheduled.exchange(true)) {
    Enqueue(RelayReceiveReady{relayId});
}
```

这样可以保证：

```text
同一个 relay 同一时间最多只有一个 receive-drain 被调度。
```

## 7. 后续建议

短期：

1. 保留 `TcpSendPending` 修复，继续复现观察。
2. 检查新日志中是否仍出现 `completed > total`。
3. 如果重复唤醒仍发生，应只看到 `stage=process_send_pending`，不应重复 `post_tcp_send_receive_view`。

中期：

1. 将 Windows `quic -> tcp` receive 调度从 per-view task 改为 per-relay drain。
2. 引入 `ReceiveDrainScheduled` 或类似状态，避免同一 relay 重复 drain task。
3. TCP `WSASend` completion 后直接驱动 `DrainRelayReceive(relay)`，而不是依赖 `FinishReceiveView` 再投递 `QuicReceiveView(nextView)`。

长期：

1. 让 Windows `quic -> tcp` 更贴近 Linux 的 `FlushDeferredQuicReceives` 模型。
2. 将 `EventQueue_` 主要用于跨线程控制事件，而不是承载每个 receive view 的细粒度数据调度。
3. 收敛 Windows/Linux/Darwin 三端 receive 生命周期语义，减少平台差异导致的竞态。

## 8. 后续观测修复计划

本节记录 2026-06-25 新版本连续多次手工验证后发现的两个次要观测问题：

1. `github.com:443` 曾出现一次 `trigger=iocp_completion_error`，错误码为 `121`。
2. `stats_active_relay` 中多次出现 `queued_quic_receives=4294967295/4294967294`。

这两个问题不属于前文的 receive view 重复 `WSASend` 根因，但会影响后续判断日志，因此需要单独修复 observability。

### 8.1 `QueuedQuicReceives` 的引用结论

`queued_quic_receives` 不是计算值，也不是 `QueuedQuicReceives - ActiveHandlers`。

修复前，日志字段来自 active snapshot：

```cpp
active.QueuedQuicReceives = relay->QueuedQuicReceives.load(std::memory_order_relaxed);
```

`ActiveHandlers` 是 worker 正在处理某个 relay completion/task 的通用计数，包括 TCP recv completion、TCP send completion、QUIC send complete、close 等，不是 QUIC receive 专用计数，因此不能用于计算 QUIC receive 队列长度。

修复前，`QueuedQuicReceives` 的增减路径是：

- `QueueDeferredQuicReceive()`：成功创建并入队一个 `TqWindowsPendingQuicReceive` 后 `fetch_add(1)`。
- `FinishReceiveView()`：从 `relay->PendingReceives` 中 erase 一个 view 后 `fetch_sub(1)`。
- `CompleteAllPendingQuicReceives()`：关闭时 swap 出 pending list，对每个 view `fetch_sub(1)`。
- `HandleQuicReceiveQueued()`：旧 IOCP receive buffer 路径中也有 `fetch_sub(1)`，但当前代码搜索不到新的投递点，疑似迁移到 event queue 后的遗留路径。

因此，`4294967295/4294967294` 更像是 `QueuedQuicReceives.fetch_sub(1)` 次数多于 `fetch_add(1)`，导致 `uint32_t` 下溢，而不是队列真的很大。

源码引用梳理后确认：`QueuedQuicReceives` 不只是日志字段，还参与过以下业务判断：

- `HasPendingRelayDrainWork()`
- `HasPendingAfterStreamShutdown()`
- `ShouldDowngradeTcpSendZeroBytes()`
- `quic_send_fin_completed` 后触发 `CloseAfterDrained` 的判断
- active relay metrics 和 hot relay scoring

但这些用途都可以用已有的 `PendingQuicReceiveQueueDepth`、`PendingQuicReceiveBytes`、`PendingReceives.empty()` 和 `CallbackPendingQuicReceives.empty()` 表达，而且 `PendingQuicReceiveQueueDepth` 已经使用饱和减法维护。因此保留 `QueuedQuicReceives` 只会形成重复状态，并带来下溢风险。

### 8.2 `QueuedQuicReceives` 的处理结果

处理策略：删除 `QueuedQuicReceives`，统一使用 `PendingQuicReceiveQueueDepth` 作为 receive view 队列深度观测和 drain 判断依据。

已做的代码调整：

1. 从 `RelayContext`、`TqWindowsRelayActiveSnapshot` 和 `TqRelayActiveSnapshot` 删除 `QueuedQuicReceives` 字段。
2. 删除 `QueueDeferredQuicReceive()`、`FinishReceiveView()`、`CompleteAllPendingQuicReceives()` 和旧 `HandleQuicReceiveQueued()` 中对 `QueuedQuicReceives` 的 add/sub。
3. 将原来依赖 `QueuedQuicReceives == 0` 的 close/drain 判断改为使用 `PendingQuicReceiveQueueDepth == 0`。
4. 从 `stats_active_relay` 日志中删除 `queued_quic_receives` 字段。
5. 从 relay metrics 聚合和 hot relay scoring 中删除 `QueuedQuicReceives`，hot score 继续基于 `PendingQuicReceiveBytes`、`PendingQuicReceiveQueueDepth`、`InFlightTcpSends` 和 `InFlightQuicSends`。

重点检查位置：

- `HasPendingRelayDrainWork()`
- `HasPendingAfterStreamShutdown()`
- `ShouldDowngradeTcpSendZeroBytes()`
- `quic_send_fin_completed` 后触发 `CloseAfterDrained` 的判断

删除后，`queued_quic_receives=4294967295/4294967294` 这类日志字段不会再出现；receive backlog 统一看 `pending_quic_receive_queue` 和 `pending_quic_receive_bytes`。

### 8.3 IOCP completion error 的处理结果

目标：`ERROR_SEM_TIMEOUT(121)` 仍然按异常结束 relay，但日志字段要能区分错误来自 TCP recv completion 还是 TCP send completion。

当前 `GetQueuedCompletionStatus()` 返回 `FALSE` 且 `overlapped != nullptr` 时，表示之前提交成功的 overlapped I/O 已完成，但完成结果是错误。当前代码会先尝试 teardown/downgrade 分流；如果错误码不属于 teardown，且 relay 未 closing，则走：

```cpp
RecordTcpHardErrorAndFail(relay, "iocp_completion_error", completionError);
```

`121` 当前不在 `IsIocpTeardownError()` 中，也不应该加入。它应保持 hard error / fatal relay reset 语义。

已做的代码调整：

1. 在 `RelayContext`、`TqWindowsRelayActiveSnapshot`、`TqRelayActiveSnapshot` 和 trace state 中新增更精确字段：

```cpp
std::atomic<uint64_t> LastTcpRecvErrno{0};
std::atomic<uint64_t> LastTcpSendErrno{0};
std::atomic<uint64_t> LastIocpCompletionErrno{0};
std::atomic<uint32_t> LastIocpOperation{0};
```

2. 在 IOCP `!ok` 分支中按 op 类型写入：
   - `TcpRecv` completion error：写 `LastTcpRecvErrno` 和 `LastIocpCompletionErrno`。
   - `TcpSend` completion error：写 `LastTcpSendErrno` 和 `LastIocpCompletionErrno`。
   - 记录 `LastIocpOperation`，用于日志区分 recv/send。
3. `RecordTcpHardErrorAndFail()` 不再通用写 `LastTcpWriteErrno`；具体 errno 字段由调用点先按 recv/send/iocp 类型写入。
4. `HandleTcpPostFailure()` 按 reason 区分：
   - `wsa_recv*` 写入 `LastTcpRecvErrno`。
   - 其他 send/write post failure 写入 `LastTcpSendErrno` 和兼容字段 `LastTcpWriteErrno`。
5. `DowngradeIocpCompletion()` 不再把 completion error 写入 `LastTcpWriteErrno`，只写 `LastIocpCompletionErrno`。
6. 更新 `stats_active_relay` 和 relay state trace 日志字段：

```text
tcp_recv_errno=<...>
tcp_send_errno=<...>
iocp_errno=<...>
iocp_op=<...>
```

7. 为兼容已有日志解析，短期保留 `tcp_write_errno`，但它现在只代表 send/write 侧错误。
8. 不修改 `IsIocpTeardownError()` 对 `121` 的判断：
   - 不把 `ERROR_SEM_TIMEOUT(121)` 加入 teardown 列表。
   - 如果 `121` 不是 stale/closing/downgrade completion，继续走 fatal。

测试 helper 中将 trigger 从通用 `iocp_completion_error` 细分为：

- `iocp_tcp_recv_completion_error`
- `iocp_tcp_send_completion_error`

最终语义仍是 hard error，不降级为 graceful teardown。

### 8.4 测试计划

新增或扩展 `src/unittest/windows_relay_worker_test.cpp`：

1. 关闭路径清理：
   - 构造 `CompleteAllPendingQuicReceives()` 对 pending list 清理的场景。
   - 验证 `PendingQuicReceiveQueueDepth` 归零。
2. 旧 `QuicReceiveQueued` 路径：
   - 后续如果继续确认无 producer，可删除 handler；当前已删除其中对重复计数的维护。
3. IOCP `121` 保持 fatal：
   - 构造 `TcpSend` completion error `121`，验证 `FatalRelayResets` / `TcpHardErrors` 增加。
   - 验证 `LastTcpSendErrno == 121`、`LastTcpWriteErrno == 121`、`LastIocpCompletionErrno == 121`、`LastTcpRecvErrno == 0`。
   - 构造 `TcpRecv` completion error `121`，验证 `LastTcpRecvErrno == 121`、`LastIocpCompletionErrno == 121`、`LastTcpSendErrno == 0`、`LastTcpWriteErrno == 0`。

### 8.5 验证命令

```powershell
cmake --build build-x64 --config Release --target tcpquic_windows_relay_worker_test -- /m:1
.\build-x64\bin\Release\tcpquic_windows_relay_worker_test.exe
```

手工运行新版 proxy 后检查日志：

- 不再出现 `queued_quic_receives` 字段。
- receive backlog 改看 `pending_quic_receive_queue`。
- 如果再出现 `121`，日志能区分是 recv completion 还是 send completion。
- `121` 仍然导致对应 relay fatal 结束。
- 不回归前文核心问题：没有 `completed/accounted > total`，没有 `finish_already_drained`。
