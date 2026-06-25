# Windows IOCP callback 队列重设计

## 背景

Windows relay 当前对 MsQuic callback 工作同时存在三层调度概念：

1. IOCP completion queue：用于真实 TCP overlapped completion 和 wake token。
2. `TqWindowsRelayEventQueue`：MsQuic callback 线程通过它投递 `TqWindowsRelayTask`。
3. relay 内部队列，例如 `relay->PendingReceives`：用于保存 QUIC receive view 的所有权和顺序。

当前 `quic -> tcp` receive 路径已经把 payload view 存入 `relay->PendingReceives`，随后又向 worker event queue 投递 `QuicReceiveView(view*)`。`FinishReceiveView()` 还可能为下一个 view 再投递一个 task。这会产生重复的 view 粒度 work item，因此目前需要 `TcpSendPending` 这类临时幂等保护。

目标设计是移除 worker event queue 作为 callback 事件传输通道。所有来自 MsQuic callback 的工作都直接投递到 worker 的 IOCP 队列；receive drain 按 relay 粒度调度，而不是按 receive view 粒度调度。

## 目标

- Windows 上只使用 IOCP 作为跨线程 worker dispatch 队列。
- 所有 callback 事件进入同一个队列，以保留同一 worker 上 callback 事件之间的相对顺序。
- 用 `RelayReceiveReady(relayId)` 顺序标记和 worker 内部 `RelayReceiveDrain(relayId)` continuation，替代 `QuicReceiveView(view*)` callback 调度。
- `relay->PendingReceives` 是唯一拥有 QUIC receive view 的队列。
- wake 通知不携带 QUIC receive view 指针。
- TCP overlapped completion 处理逻辑保持不变，除非它需要调度 receive-drain continuation。
- shutdown、abort、send-complete、ideal-send-buffer 事件和 receive-drain 事件通过同一个 IOCP 队列排序。

## 非目标

- 不改变 Linux 或 Darwin relay 行为。
- 不改变 QUIC receive 所有权语义：receive view 被接收并排队后，callback 仍返回 `QUIC_STATUS_PENDING`。
- 不在 MsQuic callback 线程上 inline 执行 TCP write。
- 不改变压缩语义。
- 在新的 invariant 有测试覆盖之前，不移除 receive-view 防御性状态检查。

## 架构方案

为每类 callback 事件增加对应的 posted worker operation：

- `RelayReceiveReady`
- `QuicSendComplete`
- `QuicIdealSendBuffer`
- `QuicPeerSendAborted`
- `QuicPeerReceiveAborted`
- `QuicShutdownComplete`
- `RelayReceiveDrain`，只用于 TCP send completion 或 finish 后的 worker 内部 continuation
- 现有 `CloseRelay` 和 `StopWorker` 仍作为 IOCP operation 保留

posted operation 由 `IoOperation` 表示，`Event` 字段标识 operation 类型，`Relay` 或 `RelayId` 标识 relay。receive-drain operation 只标识 relay，不携带 `TqWindowsPendingQuicReceive`。

`TqWindowsRelayEventQueue` 和 `TqWindowsRelayTask` 不再参与生产路径的 callback dispatch。单元测试可以迁移到 IOCP posted operation，也可以保留小范围 test-only helper，但生产路径不应再向 `EventQueue_` 投递 callback 工作。

## Receive 路径

Callback 路径：

```text
MsQuic RECEIVE callback
  -> 把 QUIC buffers 复制到 TqWindowsPendingQuicReceive::OwnedBuffer
  -> push shared_ptr 到 relay->PendingReceives
  -> 更新 pending bytes / queue depth 指标和 receive 背压
  -> PostCallbackOperation(RelayReceiveReady, relay)
  -> return QUIC_STATUS_PENDING
```

Worker 路径：

```text
IOCP RelayReceiveReady(relayId)
  -> DrainRelayReceives(relay)
       -> 检查 relay->PendingReceives.front()
       -> 没有 view: 返回
       -> closing/write closed: 按现有逻辑 complete 或 close
       -> view->TcpSendPending: 返回
       -> 为队首 view 最多提交一个 WSASend
```

TCP send completion 路径：

```text
IOCP TcpSend completion
  -> 清除 view->TcpSendPending
  -> 按完成字节推进 receive view
  -> 按 batching 规则 flush ReceiveComplete
  -> 如果 view 完成: FinishReceiveView(relay, view)
  -> 如果可能还有 receive 工作: ScheduleRelayReceiveDrain(relay)
```

`FinishReceiveView()` 不再为 `nextView` enqueue task。它只负责从 `PendingReceives` 删除已完成 view、更新指标、处理 FIN、按需恢复 QUIC receive，并在队列仍非空时调度 relay receive drain。

## 调度 invariant

receive 相关 IOCP post 分为两类：

1. `RelayReceiveReady`：callback 顺序标记。每个接受了 receive 所有权的 RECEIVE callback 都投递一个 marker。它不能合并，因为合并可能让后到的 receive 越过已经投递到 IOCP 的 ideal-buffer、abort、shutdown 或 send-complete callback 事件。
2. `RelayReceiveDrain`：worker 内部 continuation。它不代表独立的 MsQuic callback 事件，因此可以合并。

只为 continuation post 增加 `RelayContext::ReceiveDrainQueued`。

```text
ScheduleRelayReceiveDrain(relay):
  if relay is null or closing: return false
  if relay->ReceiveDrainQueued.exchange(true) == true: return true
  PostQueuedCompletionStatus(IOCP, RelayReceiveDrain(relay))
```

如果 post 失败，清除 `ReceiveDrainQueued`，更新 wake/post failure 指标；如果该失败意味着已经接收的 receive 所有权无法继续服务，则 fail relay。

IOCP worker 处理 `RelayReceiveDrain` 时，先清除 `ReceiveDrainQueued`，再执行 drain。如果 worker 侧状态在 drain 过程中产生更多 receive 工作，`ScheduleRelayReceiveDrain()` 可以再投递一个 continuation。`DrainRelayReceives()` 仍必须在提交 send 前检查 `TcpSendPending`，因为当前队首 view 可能已经有一个 overlapped TCP send 在飞。`RelayReceiveReady` 不使用 `ReceiveDrainQueued`；重复 ready marker 是安全的，因为 drain 是状态驱动的，并且由队首 view gate 保护。

## Callback 事件顺序

所有 MsQuic callback event handler 都按 callback 顺序投递 IOCP operation：

- receive callback 在 view push 到 `PendingReceives` 后投递 `RelayReceiveReady`
- send complete 投递 `QuicSendComplete`
- ideal send buffer 投递 `QuicIdealSendBuffer`
- peer abort 投递对应 abort operation
- shutdown complete 投递 `QuicShutdownComplete`

任何 callback 事件都不再使用 `EventQueue_`。这样 callback-originated ordering 只依赖一个队列。`RelayReceiveReady` 有意设计为每个 callback event 投递一次，而不是合并；否则它可能跳过另一个已经投递到 IOCP 的 callback event。IOCP 不会和真实 TCP completion 做严格串行化，因此 TCP completion 与 callback 事件交错时，worker handler 仍必须保持状态驱动和幂等。

## 错误处理

- 如果 posted callback operation 找不到对应 relay，丢弃 stale operation，并在适当位置更新 stale/drop 指标。
- 如果 view 已经排队后 receive-drain post 失败，需要 close/fail relay，并 complete pending receive ownership，避免泄漏 MsQuic receive buffer。
- 现有 TCP completion error 的 teardown downgrade 行为保持不变。
- close 路径必须 complete 所有 pending receive view，并且只在 outstanding posted worker operation 可安全丢弃之后 retire callback binding。

## 指标和诊断

重命名或重新解释 event queue 指标，使其描述 IOCP-posted callback work，而不是 `EventQueue_` depth：

- callback IOCP post count
- callback IOCP post failure count
- receive-ready callback post count
- receive-drain continuation scheduled count
- receive-drain continuation coalesced count
- stale posted callback drop count

保留 receive-view drain 边界附近的 trace stage：

- `queue_receive`
- `post_receive_ready`
- `schedule_receive_drain`
- `drain_receive_front`
- `drain_receive_send_pending`
- `post_tcp_send_receive_view`
- `tcp_send_complete_receive_view`
- `finish_remove`

旧的 duplicate-task trace `process_send_pending` 应变成 invariant guard，而不再是预期修复信号。

## 测试

单元测试需要覆盖：

- worker drain 前已经排队两个 receive view：只为队首 view 提交一个 TCP send。
- view A 完成后可以 schedule/drain view B，但不会 enqueue 指向 B 的专用 task。
- 重复 callback receive event 会投递重复 `RelayReceiveReady` marker，但 marker 不携带 receive view 指针。
- 重复 worker 内部 `ScheduleRelayReceiveDrain()` 在 continuation 已排队时会合并。
- receive-drain operation 只携带 relay 身份，不携带 receive view 指针。
- receive、ideal-send-buffer、abort、shutdown、send-complete 通过 IOCP 保持 callback event 顺序。
- relay close 后 stale posted operation 会被忽略，不访问已经释放的 callback state。
- receive ownership 已被接受后，如果 queue/post 失败，能够安全 complete 或 close。
- compressed receive 路径仍然只在生成的 TCP bytes 被接受后推进 compressed input ownership。

回归验证应包含 Windows relay worker tests，以及一次 Windows integration run，用于覆盖过去能复现 `CompletedLength > TotalLength` 或 `AccountedLength > TotalLength` 的场景。

## 迁移步骤

1. 增加 IOCP posted callback operation helper，以及 relay-id/payload 字段。
2. 实现 `ScheduleRelayReceiveDrain()` 和 `DrainRelayReceives()`。
3. 把 RECEIVE callback dispatch 从 `EnqueueEvent(QuicReceiveView)` 改为 `PostCallbackOperation(RelayReceiveReady)`。
4. 修改 `FinishReceiveView()` 和 `HandleTcpSend()`，改为调度 relay receive drain，不再 enqueue view task。
5. 把 send-complete、ideal-send-buffer、peer-abort、shutdown-complete callback event 从 `EventQueue_` 迁移到 IOCP posted operation。
6. 移除生产路径对 `DrainEvents()`、`ProcessRelayTask()`、`TqWindowsRelayEventQueue`、`TqWindowsRelayTask` 的使用。
7. 更新指标、snapshot、测试和文档，使其描述 IOCP-posted callback operation。

## 实现注意事项

- `IoOperation` 当前存储 `std::shared_ptr<RelayContext> Relay`；posted callback operation 可以用它让 relay state 存活到 worker 处理或丢弃 operation。实现时需要和 close/retirement 规则平衡，避免已停止 relay 在可见集合里保留过久。
- Send-complete operation 拥有 `TqWindowsQuicSendOperation*`；IOCP posted op 必须保持唯一删除路径。
- `TcpSendPending` 在迁移期间应保留，作为防止同一个 receive view 重复提交 WSASend 的硬保护。
