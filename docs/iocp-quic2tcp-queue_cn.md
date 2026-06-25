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
