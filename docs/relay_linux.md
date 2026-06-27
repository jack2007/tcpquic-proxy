# Linux relay 数据转发路径

本文记录当前 Linux 实现中 TCP/QUIC 数据面从控制面建链到 relay worker 接管后的转发路径、线程模型、报文分发、TCP 背压、QUIC 背压和队列设计。代码入口主要在 `src/ingress/client_ingress_reactor.cpp`、`src/tunnel/tcp_tunnel.cpp`、`src/tunnel/relay.cpp`、`src/tunnel/linux_relay_worker.cpp`。

## 总体结构

Linux 数据面由 `TqLinuxRelayRuntime` 管理一组固定 `TqLinuxRelayWorker`。每条 tunnel 在 OPEN 成功后调用 `TqRelayStart()`，运行时选择一个 worker，把 TCP fd、MsQuic stream、压缩器/解压器和 `TqRelayHandle` 注册到该 worker。

线程边界如下：

- client ingress 控制面线程：监听、accept、SOCKS5/HTTP CONNECT/port-forward 握手、发起 client OPEN、写代理协议响应。
- server dial 控制面线程：server 收到 OPEN 后做 ACL、DNS、非阻塞 TCP connect。
- MsQuic worker 线程：触发 stream callback，包括 OPEN 阶段的控制报文、relay 阶段的 QUIC receive/send complete/abort/shutdown 事件。
- Linux relay worker 线程：每个 worker 一个 epoll/eventfd 循环，负责 TCP fd readiness、worker 事件队列、TCP/QUIC 数据转发。
- tunnel reaper 线程：回收已停止的 tunnel context。

控制面只负责建立 tunnel。`TqRelayStart()` 成功后，TCP fd 的读写和 QUIC stream 数据事件都由 `TqLinuxRelayWorker` 处理。

## client tcp->quic

路径是：

```text
listen socket
  -> TqClientIngressReactor accept
  -> SOCKS5 / HTTP CONNECT / port-forward 握手
  -> TqStartClientTunnelAsync()
  -> client OPEN request over QUIC
  -> server OPEN response
  -> ingress 写 SOCKS/HTTP 成功响应
  -> TqAcceptClientTunnelOpen()
  -> TqRelayStart()
  -> TqLinuxRelayWorker::RegisterRelayWithId()
  -> epoll TCP readable
  -> readv
  -> Stream::Send()
```

`TqClientIngressReactor` 在握手阶段只读取代理协议边界内的数据，避免把 early data 预读进 ingress buffer。OPEN 成功后，ingress 先把 SOCKS5 reply 或 HTTP 200 写回本地客户端，然后调用 `TqAcceptClientTunnelOpen()`。accept 后 `TqTunnelContext::StartRelay()` 调用 `TqRelayStart()`，并把 TCP fd 所有权交给 relay；此后 ingress 不再操作该 fd。

Linux 注册 relay 时会：

- 设置 TCP fd non-blocking。
- 配置 keepalive / TCP user timeout。
- 将 TCP fd 注册进 worker epoll，初始关注 `EPOLLIN | EPOLLRDHUP | EPOLLERR`。
- 把 MsQuic stream callback 替换为 `TqLinuxRelayWorker::StreamCallback`。
- 在 `TqRelayHandle` 中记录 `LinuxWorker` 和 `LinuxRelayId`。

TCP 可读时，worker 执行 `DrainTcpReadable()`：

- 按 `ReadBatchBytes` 和 `ByteBudgetPerTick` 计算本轮读预算。
- 每次分配多个 `TqRelayBuffer`，构造 `iovec`，用 `readv()` 批量读取。
- 非压缩模式下直接把读取结果作为 `TqBufferView` 提交 QUIC。
- zstd 模式下先经 compressor 生成压缩输出，再重新切成 `TqBufferView`。
- 调用 `SubmitTcpBatchToQuic()` 构造 `TqLinuxRelaySendOperation`，其中持有 buffer owner 和 `QUIC_BUFFER` 数组。
- 调用 MsQuic `Stream::Send()`；send 成功后 buffer 生命周期由 send operation 持有，直到 `SEND_COMPLETE`。

TCP EOF 时，worker 关闭 TCP 读方向，停止关注 TCP readable，并发送 QUIC FIN。若启用压缩，会先 flush compressor，把尾部压缩数据和 FIN 一起提交。

## client quic->tcp

client 侧从 QUIC 收到数据时，MsQuic 在线程内调用 `TqLinuxRelayWorker::StreamCallback()`。relay 阶段的 RECEIVE 不在 callback 内直接写 TCP，而是走 deferred receive：

```text
MsQuic QUIC_STREAM_EVENT_RECEIVE
  -> StreamCallback
  -> QueueDeferredQuicReceive()
  -> TqLinuxRelayEventQueue: QuicReceiveView
  -> worker eventfd wake
  -> ProcessQuicReceiveViewEvent()
  -> PendingQuicReceives
  -> FlushDeferredQuicReceives()
  -> writev TCP
  -> stream->ReceiveComplete()
```

callback 把 MsQuic 提供的 `QUIC_BUFFER` 切片保存为 `TqPendingQuicReceive`，入 `TqLinuxRelayEventQueue` 后返回 `QUIC_STATUS_PENDING`。这表示 QUIC receive buffer 的所有权暂时由应用持有，直到数据实际写入 TCP 后再调用 `ReceiveComplete()` 归还。

worker 线程处理 `QuicReceiveView` 事件时：

- 将 view 放入 relay 的 `PendingQuicReceives`。
- 增加 `PendingQuicReceiveBytes`。
- 如超过 QUIC receive pending 上限，调用 `ReceiveSetEnabled(false)` 暂停 MsQuic 对该 stream 的继续 receive 回调。
- 调用 `FlushDeferredQuicReceives()` 尝试写 TCP。

写 TCP 使用 `writev()`，单轮受 `TcpWriteMaxBytes`、`TcpWriteBurstBytes` 和 `MaxIov` 限制。写成功后推进 view 的 slice offset 和 completed length，并把已写入字节累计到 `PendingCompleteBytes`；达到 `DeferredReceiveCompleteBatchBytes` 或 view 完成时调用 `ReceiveComplete()`。如果 TCP 写返回 `EAGAIN/EWOULDBLOCK`，worker 打开 EPOLLOUT，等待后续 writable 再继续。

如果 QUIC receive 带 FIN，worker 会等该 view 全部写入 TCP 后执行 `shutdown(tcpFd, SHUT_WR)`，形成 TCP 写方向半关闭。

## server quic->tcp

server 侧新 QUIC stream 先由 `TqServerIncomingStreamDispatcher` 解析控制流类型。普通 OPEN 流被移交给 `TqTunnelContext`：

```text
server MsQuic incoming stream
  -> dispatcher 收 OPEN bytes
  -> TqTunnelContext::TryHandleServerOpen()
  -> decode TqOpenRequest
  -> TqServerDialReactor::Submit()
  -> ACL / c-ares DNS / non-blocking TCP connect
  -> Send OPEN response
  -> Send complete 后 StartPendingServerRelay()
  -> TqRelayStart()
```

server OPEN 可能和首批数据粘在同一个 QUIC receive 中。`TryHandleServerOpen()` 会把 OPEN 头部之后的多余 bytes 保存到 `PendingRelayRx`。relay 启动后 `DispatchPendingRelayRx()` 构造一个 synthetic `QUIC_STREAM_EVENT_RECEIVE` 投给新的 relay stream callback，确保 OPEN 后的 early data 不丢。

DNS 和 TCP connect 由 `TqServerDialReactor` 处理。literal IP 目标跳过 DNS，域名目标走 c-ares 异步解析；解析结果经过 ACL 后逐个非阻塞 connect。成功后把 connected fd 返回给 tunnel context。server 发送 OPEN response 成功后，等待该响应的 MsQuic send complete，再启动 relay，避免客户端在收到 OPEN response 前数据面状态不一致。

relay 启动后，server 的 QUIC->TCP 路径与 client quic->tcp 相同：MsQuic receive callback 入 `TqLinuxRelayEventQueue`，worker 将 `PendingQuicReceives` 写入目标 TCP fd，并按写入进度调用 `ReceiveComplete()`。

## server tcp->quic

server 侧 TCP fd 是 dial reactor 建好的目标连接 fd。OPEN response send complete 后，`StartPendingServerRelay()` 调用 `StartRelay()`，Linux worker 开始监听目标 TCP fd readable。

后续路径与 client tcp->quic 相同：

```text
target TCP readable
  -> epoll
  -> DrainTcpReadable()
  -> readv batch
  -> optional zstd compress
  -> TqLinuxRelaySendOperation
  -> MsQuic Stream::Send()
  -> SEND_COMPLETE
```

目标 TCP EOF 会被映射为 QUIC FIN。目标 TCP 硬错误会触发 fatal relay reset，并关闭/abort 对应 stream 和 TCP fd。

## 报文分发和事件队列

Linux worker 使用两个事件来源：

- epoll：TCP readable/writable、rdhup/error、eventfd wake。
- `TqLinuxRelayEventQueue`：跨线程事件队列，主要由 MsQuic callback 或外部控制路径写入。

`TqLinuxRelayEventQueue` 是固定容量、power-of-two ring buffer，使用 per-cell sequence 的 MPSC/MPMC 风格无锁 push/pop。容量来自 `EventQueueCapacity`，默认配置为 4096。事件类型包括：

- `QuicReceiveView`
- `QuicSendComplete`
- `QuicIdealSendBuffer`
- `QuicPeerSendAborted`
- `QuicPeerReceiveAborted`
- `QuicShutdownComplete`
- `RegisterRelay`
- `UnregisterRelay`
- `Snapshot`
- `TcpWritable`
- `Shutdown`

其中 `RegisterRelay`、`UnregisterRelay`、`Snapshot` 是同步控制事件，用于保持外部 API 的同步语义，同时把 relay 容器读写收敛到 worker 线程内。测试代码还会使用 `TakeCapturedQuicBytesForTest`、`EnqueueQuicReceiveForTest`、`FlushTcpWritableForTest`、`RelayIndexesConsistentForTest`、`DispatchTcpEventsForTest` 等 test-only 控制事件。

MsQuic receive callback 正常把 view 入 event queue。如果 queue 满，Linux 实现会把 view 临时放进 relay 的 `CallbackPendingQuicReceives`，同时暂停 QUIC receive，并 wake worker；worker 后续调用 `DrainCallbackPendingQuicReceives()` 把这些 callback pending view 转移到正常 pending 队列。

## TCP 背压

Linux 有两类 TCP 背压：

1. TCP 读侧背压，也就是暂停 tcp->quic 的 `readv()`。
2. TCP 写侧自然背压，也就是 quic->tcp 写目标 TCP 时遇到 `EAGAIN`。

TCP 读侧背压由 QUIC send backlog 驱动。`ShouldPauseTcpReadForQuicBacklog()` 比较 `OutstandingQuicSendBytes` 和当前 relay 的 ideal send buffer。ideal send buffer 来自 MsQuic `QUIC_STREAM_EVENT_IDEAL_SEND_BUFFER_SIZE`，没有事件时使用 fallback。达到阈值后：

- 设置 `TcpReadPausedByQuicBacklog=true`。
- 通过 `ArmTcpReadable(false)` 修改 epoll interest，暂停继续读 TCP。
- send complete 降低 outstanding 后，如低于阈值则恢复 readable。

TCP 写侧背压由内核 send buffer 体现。`writev()` 返回 `EAGAIN/EWOULDBLOCK` 时，当前 view 保留在 `PendingQuicReceives`，worker 启用 EPOLLOUT，后续 writable 再从保存的 slice offset 继续写。

buffer 分配也会形成读侧背压。`TqAllocateRelayBuffer()` 受 `MaxPendingBufferBytes` 限制，如果 pending bytes 超限，读取路径会停下并取消 readable，等已提交 buffer 随 QUIC send complete 或 TCP write complete 释放后再恢复。

## QUIC 背压

QUIC send 背压主要由 MsQuic `Stream::Send()` 返回资源类错误和 outstanding bytes 共同处理：

- send 成功时增加 `OutstandingQuicSends` 和 `OutstandingQuicSendBytes`。
- `SEND_COMPLETE` 事件经 event queue 回到 worker 后减少 outstanding，并释放 send operation 持有的 buffer。
- 如果 `Stream::Send()` 返回 backpressure 类状态，operation 被放进 `PendingQuicSendRetries`，同时暂停 TCP read。
- 后续 send complete 或 ideal send buffer 事件会触发 retry。

QUIC receive 背压由 `PendingQuicReceiveBytes` 控制：

- `PendingQuicReceiveBytes >= MaxPendingQuicReceiveBytesPerRelay()` 时暂停 `ReceiveSetEnabled(false)`。
- 写入 TCP 后减少 pending bytes。
- Linux 当前 low watermark 等于 high watermark，即 pending bytes 低于上限后恢复 receive。

因为 receive callback 返回 `QUIC_STATUS_PENDING`，未完成的 QUIC receive view 不会被 MsQuic 复用。真正完成点是 TCP 写入后调用 `ReceiveComplete()`。

## 队列和生命周期

每条 relay 主要队列如下：

- `PendingQuicSendRetries`：QUIC send 资源不足时等待重试的 send operation。
- `PendingQuicReceives`：QUIC->TCP 方向等待写 TCP 的 receive view。
- `CallbackPendingQuicReceives`：event queue 满时，callback 线程临时挂起的 receive view。
- `PendingTcpWrites`：Linux 代码保留的 TCP 写 pending 队列，主要用于部分路径和 metrics；常规 QUIC receive view 直接保留在 `PendingQuicReceives`。
- relay buffer budget：`PendingBufferBytes` 限制 worker/relay 持有的 TCP read buffer 和压缩输出 buffer。

关闭条件要求两边方向都 drain 完成：TCP read closed、TCP write closed、QUIC FIN submitted/completed、outstanding QUIC sends 为 0、pending receive/write 队列为空。满足后 worker 调用停止逻辑，`TqTunnelReaper` 最终回收 tunnel context。

## 锁和同步开销

Linux relay 的目标是让每条 relay 的状态尽量只由所属 `TqLinuxRelayWorker` 线程修改。常规 TCP `readv/writev`、QUIC send retry、pending receive 推进、TCP/QUIC 背压状态切换都不使用每连接互斥锁；跨线程入口通过 stream binding 原子状态、无锁 event queue 和少量生命周期锁交给 worker 串行处理。

当前流程中的同步点如下：

| 同步点 | 代码位置 | 触发路径 | 粒度和性能影响 | 更合适的处理办法 |
| --- | --- | --- | --- | --- |
| `TqLinuxRelayRuntime::Lock` | `src/tunnel/linux_relay_worker.h` / `.cpp` | `Start()`、`Stop()`、`PickWorker()`、`Snapshot()` | runtime 级互斥锁。`TqRelayStart()` 每建一条 relay 会进入 `Start()` 和 `PickWorker()`，属于建链/拆链级别，不在报文转发热路径。高并发短连接下可能让建链线程在 worker 选择处串行化；`Snapshot()` 还会在持有 runtime 锁时遍历 worker 快照，可能和建链互相影响。 | worker 初始化可以用 `std::call_once` 或启动期一次性完成；`NextWorker` 可改为 atomic round-robin，`PickWorker()` 无锁；metrics snapshot 可复制 worker 指针后释放 runtime 锁，再逐个 snapshot。 |
| relay 容器 / `TqLinuxRelayWorker::RelayLock`（已移除） | `Relays` / `RetiredRelays` / `RelaysById` / `RetiredRelaysById` / `NextRelayId` | `RegisterRelayWithId()`、`UnregisterRelay()`、`FindRelayById()`、`FindRelayByFd()`、`Snapshot()`、`PurgeRetiredRelaysIfIdle()` | Stage 1 先增加 `RelaysById` / `RetiredRelaysById`，把 relay id 查找从线性扫描降为 O(1)，但仍保留容器锁。Stage 2 将 register、unregister、snapshot 事件化，并让 `Relays`、`RetiredRelays`、`RelaysById`、`RetiredRelaysById` 只归 worker 线程读写，因此 `RelayLock` 已移除。当前热路径剩余成本是跨线程 event queue 入队，以及 worker 内本地 map 查找。 | 如果本地 map 查找仍在压测中显著占热，可进一步改成 slot/generation id，或让 epoll data 直接携带 slot 指针并由 generation 校验生命周期。 |
| `TqLinuxRelayWorker::ControlLock` | `src/tunnel/linux_relay_worker.h` / `.cpp` | `RegisterRelayWithId()`、`UnregisterRelay()`、`Snapshot()`、`Stop()`、test-only 控制 helper | `RelayLock` 移除后仍保留的 lifecycle/control 锁。它串行化 register、unregister、snapshot 等同步控制命令提交和 `Stop()`，保护 `WorkerThreadId` / running-state 检查，以及栈上同步 command 在等待 worker 完成前的生命周期。它不是每 relay 数据面或容器读写锁，不保护 worker-owned relay 容器，也不会被 worker 侧 local helper / completion 路径持有。 | 当前定位合理：只覆盖控制面命令提交和停止边界，不进入报文转发热路径。若控制面提交频率成为瓶颈，可进一步拆分 stop 状态和 command lifetime 同步，但不应重新引入跨线程容器共享。 |
| `RelayState::CallbackPendingQuicReceiveLock` | `CallbackPendingQuicReceives` | `QueueDeferredQuicReceive()` 在 event queue 满时写入；worker 的 `DrainCallbackPendingQuicReceives()`、abort/unregister 清理 | 连接级互斥锁，但只在 event queue 满的降级路径使用。正常 QUIC receive 只进无锁 event queue，不持该锁；当 queue 满时，callback 线程会拿该锁追加 pending view，并暂停 QUIC receive。此时性能主要已经受队列容量/worker drain 能力限制，该锁会进一步增加 callback 延迟。 | 首选通过容量、worker 数、event budget 避免进入该路径。若压测显示频繁触发，可改为每 relay lock-free overflow queue，或让 overflow 只记录一个“需要 drain”标记并把 view 放进预分配 MPSC 队列。 |
| `TqLinuxRelayWorker::RetiredBindingLock` | `RetiredStreamBindings` | `DetachRelayStreamBinding()` | 生命周期级互斥锁。只在 stream binding 解除和延迟释放时使用，通常发生在关闭、fatal reset 或最后一个 QUIC send 完成后，不参与正常报文转发。 | 当前成本可接受。若关闭风暴中有竞争，可把 retired binding 回收移动到 worker 线程私有队列，或用 epoch/RCU 风格延迟释放。 |
| `TqLinuxRelayEventQueue` 的 `EnqueuePos` / `DequeuePos` / per-cell `Sequence` 原子 CAS | `src/tunnel/linux_relay_event_queue.h` | MsQuic callback 入队 `QuicReceiveView`、`QuicSendComplete`、abort/shutdown/ideal buffer；worker `DrainEvents()` 出队 | 报文/事件级同步。它是固定容量 ring buffer，没有互斥锁；每次 push/pop 有 CAS 和 acquire/release 原子操作。相比 mutex 更适合 MsQuic callback 到 worker 的跨线程转交，但多个 MsQuic callback 线程同时向同一 worker 入队时仍会在 `EnqueuePos` cache line 上竞争。 | 如果 `MultipleEventProducerThreadsObserved` 或 event queue full 指标升高，可按 producer/stream 分片队列，或者让每个 worker 有多个 MPSC shard 后由 worker 轮询；如果能保证单 producer，则可替换为 SPSC ring 降低 CAS 成本。 |
| `WakeArmed` 原子和 `eventfd` wake | `Wake()` / `DrainEvents()` | 每次跨线程 enqueue 后唤醒 worker | 事件级同步。`WakeArmed.exchange(true)` 合并 wake，避免每个事件都写 `eventfd`；成本是一条原子 RMW，收益是减少系统调用和 epoll wake storm。 | 当前方式合理。可进一步只在队列从空变非空时写 `eventfd`，但需要更精确的队列空状态同步，复杂度会上升。 |
| `StreamRelayBinding` 的 `Relay` / `Closing` / `CallbackRefs` 原子 | `StreamCallback()` / `OnStreamEventWithBinding()` / `DetachRelayStreamBinding()` | MsQuic stream callback 进入 relay 后校验生命周期 | QUIC callback 级同步。RECEIVE、ideal buffer、shutdown/abort 事件会 `fetch_add/fetch_sub` callback ref，并 acquire-load relay 指针和 closing 标志；SEND_COMPLETE 当前先直接入 event queue，不走 callback ref。该设计避免 callback 线程持 `RelayLock`，也避免注销时 use-after-free，开销通常小于互斥查找。 | 当前设计方向正确。若小包 callback 极多，可评估让 MsQuic callback 只做最少原子校验和批量入队，或用 generation id 降低 ref 计数频率；不能简单移除，除非能从 MsQuic callback 串行性和对象生命周期上证明安全。 |
| `TqRelayHandle::Stop` 原子 | `TqRelayStart()` / `TqRelayStop()` / `SetRelayStop()` | 控制面停止 relay，worker 标记停止条件 | 连接生命周期级同步，不在每个报文上读写。成本低，主要用于跨线程观察停止状态。 | 保持 atomic bool 即可。 |
| 全局 active relay 计数 | `TqRelayRegisterActive()` / `TqRelayUnregisterActive()` | `TqRelayStart()`、`TqRelayStop()` | 建链/拆链级原子计数，用于根据活跃 relay 数调整 pool budget。不是转发热路径；短连接风暴时可能成为轻微共享 cache line 热点。 | 如果建链压力很高，可改为分 worker/per-thread 计数并周期聚合；现阶段不应优先优化。 |
| relay buffer budget 原子 | `TqRelayBufferBudget::PendingBufferBytes` / `AllocateCount` | TCP read buffer、压缩输出 buffer、QUIC receive TCP buffer 分配和释放 | buffer 分配级同步。每分配一个 `TqRelayBuffer` 都会 CAS 增加 pending bytes，释放时 fetch_sub。由于读路径按 `ReadChunkSize` 和 `readv` 批量分配，粒度是 buffer/chunk 而不是每字节；在高吞吐下，真正成本通常与内存分配一起出现。 | 可用 per-worker/per-relay buffer pool 和本地 credit cache 减少全局分配与 CAS；如果确认所有释放都回到 worker 线程，也可把部分计数改为 worker 私有非原子，并把跨线程释放转成 worker 事件。 |
| 统计和指标原子 | `TcpReadBytes`、`TcpWriteBytes`、`QuicReceiveViewCount`、错误计数等 | read/write batch、receive view、send complete、错误路径、snapshot | 多数是批次/事件级 `fetch_add/load`，用于 metrics 和 trace，不保护业务状态。它们不改变转发语义，但在小包、高事件率或频繁 snapshot 时可能产生 cache line 写压力；当前不少 `fetch_add()` 使用默认内存序，成本可能高于纯计数所需。 | 对只用于统计的计数优先使用 `memory_order_relaxed`；worker 线程独占更新的计数可改成普通字段，snapshot 在 worker 线程上下文读取；高频指标可做采样或按线程分片聚合。 |

因此，从转发性能角度看，当前最需要关注的不是传统大锁，而是：

1. event queue 入队和 worker 本地 relay id map 查找是否在每个 TCP/QUIC 事件上放大。
2. event queue 的 CAS 竞争和 queue full 后的 callback pending 降级路径。
3. buffer 分配预算 CAS、内存分配和高频统计原子在小包场景中的累计成本。

实现备注：当前 Linux relay 查找优化分两阶段完成：先在保留锁的前提下增加 relay id map，再把容器读写收敛到 worker event loop 中，移除跨线程容器共享。

如果要继续改进，建议先确认 worker 本地 map 查找是否仍是热点；若是，再评估 slot/generation id 或 epoll data slot 指针。之后再根据 metrics 判断是否需要队列分片、buffer pool 或统计计数降频。

## 关键参数

- `ReadChunkSize`：单个 relay buffer 大小，默认 128 KiB。
- `ReadBatchBytes`：单次 TCP read drain 的批量预算，默认 1 MiB。
- `ByteBudgetPerTick`：worker 单 tick 字节预算。
- `MaxIov`：`readv/writev` 最大 iov 数。
- `TcpWriteMaxBytes`：单次 TCP write 尝试最大字节数，0 表示不额外限制。
- `TcpWriteBurstBytes`：单轮 TCP write burst 限制。
- `MaxPendingBufferBytes`：relay buffer 总 pending 上限。
- `MaxPendingQuicReceiveBytesPerRelay`：QUIC receive pending 上限，0 时回退到 `MaxPendingBufferBytes`。
- `DeferredReceiveCompleteBatchBytes`：批量归还 MsQuic receive buffer 的阈值。
- `MaxInFlightQuicSends` / `MaxBufferedQuicSendBytes`：QUIC send 侧背压上限。
