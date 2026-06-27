# macOS relay 数据转发路径

本文记录当前 macOS/Darwin 实现中 TCP/QUIC 数据面从控制面建链到 relay worker 接管后的转发路径、线程模型、报文分发、TCP 背压、QUIC 背压和队列设计。代码入口主要在 `src/ingress/client_ingress_reactor.cpp`、`src/tunnel/tcp_tunnel.cpp`、`src/tunnel/relay.cpp`、`src/tunnel/darwin_relay_worker.cpp`。

## Phase 1-4 后的当前锁边界

Phase 1 后，QUIC receive callback 只通过 `StreamBinding` 上的原子计数预留接收预算并投递队列事件，不再在 callback 内直接修改 relay 的 pending-byte / pending-queue 状态。relay 的 `PendingQuicReceiveBytes` 和 `PendingQuicReceives` 只在 worker 事件路径中推进。

Phase 2 后，`SEND_COMPLETE` 和 QUIC terminal callback 已事件化。`SEND_COMPLETE` callback 只 claim completion 并投递 `QuicSendComplete` 事件；detached / late callback 仍通过 completion state 兜底清理。terminal callback 同步设置 close-pending marker，阻止后续 TCP->QUIC 继续推进，然后投递 worker 事件执行关闭。

Phase 3 后，register / unregister / snapshot 通过同步 control event 进入 worker，调用方等待命令完成；队列入队失败时保留唤醒重试语义，避免 command 栈对象在 worker 使用前失效。

Phase 4 后，本 Linux-only 原型采取保守边界：保留 worker-local relay lookup / known-send helper 的结构，但不在当前提交中让 `Relays`、`RetiredRelays`、`KnownSendOperations` 这类 worker-owned 容器进入无锁读写。原因是 callback fallback、test helper、stop / retire / destructor 仍会跨线程观察或修改这些容器；在没有 macOS TSAN / kqueue / MsQuic callback 验证前，正常 worker 事件路径继续通过带锁 wrapper 查找 relay，known-send local helper 也继续使用 `KnownSendMutex`。Phase 4 已把 callback SEND_COMPLETE 的 claim 迁移到每个 binding 的 `CompletionState::Mutex`，并保留同一 known-send 锁域，避免 `RelayMutex` 扩大到 send completion 跟踪。

当前仍保留的主要锁如下：

- `RelayMutex` 仍保留在外部 wrapper、stop / retire / destructor、retired binding 清理、snapshot / metrics、以及 callback 线程需要安全查找 relay 的 fallback 路径中。`PurgeQueuedEventsForStop()` 没有改为 local lookup，因为它可在 stop / destructor 路径由非 worker 线程调用；此处继续使用带锁 lookup 更保守。
- `KnownSendMutex` 仍保留在 worker local known-send helper、callback fallback、test helper、stop / drain helper 中。它保护 `KnownSendOperations` 的全部跨线程访问；后续若要移除它，必须先消除 callback / test / stop 对 worker map 的直接访问或改为生命周期安全的 worker-owned handoff。
- `CompletionState::Mutex` 仍保留在 SEND_COMPLETE callback claim、worker register / submit-mark / unregister 同步 completion state、以及 detached / late callback 清理中。它是 per-binding 锁，用于 callback 不读取 worker map 的 claim 和 detached cleanup。
- `RelayState::Mutex` 仍保留在 `ProcessTcpEvents()` 下游的 `DrainTcpReadable()`、`SubmitTcpBatchToQuic()`、`RetryPendingQuicSends()`、`ProcessQuicReceiveViewEvent()` 及 TCP write / QUIC receive 辅助函数中。虽然这些函数大多由 worker 正常数据路径调用，但同一批状态也会被 test helper、snapshot、stop / retire、callback close marker 和 late completion fallback 观察或修改；在当前 Linux 环境无法运行 Darwin 压测前，不继续移除这些 per-relay 锁。

当前环境是 Linux，无法运行 Darwin kqueue / MsQuic callback 组合测试。后续需要在 macOS 上验证：Darwin relay unit / IO tests、QUIC receive callback 高并发预算一致性、`SEND_COMPLETE` 同步/异步/迟到完成清理、terminal callback 后不再提交新的 TCP->QUIC send，以及 Phase 4 后正常数据事件中的 `RelayMutex` 热点是否下降。

## 总体结构

macOS 数据面由 `TqDarwinRelayRuntime` 管理固定数量的 `TqDarwinRelayWorker`。每个 worker 一个线程，一个 kqueue fd，并配一个跨线程 `TqDarwinRelayEventQueue`。OPEN 成功后 `TqRelayStart()` 选择 worker，注册 TCP fd 和 MsQuic stream。

线程边界如下：

- client ingress reactor：本地 listen、accept、SOCKS5/HTTP CONNECT/port-forward 握手、发起 client OPEN、写本地代理响应。
- server dial reactor：server OPEN 后处理 ACL、DNS 和 TCP connect。
- MsQuic worker：触发 stream callback。
- Darwin relay worker：kqueue 循环，处理 TCP fd read/write readiness 和 relay 事件队列。
- tunnel reaper：回收停止后的 tunnel context。

控制面 reactor 不处理 relay 数据。`TqRelayStart()` 后，TCP fd 和 QUIC stream callback 都交给 `TqDarwinRelayWorker`。

## client tcp->quic

路径是：

```text
TqClientIngressReactor listen/accept
  -> 代理握手状态机
  -> TqStartClientTunnelAsync()
  -> QUIC OPEN request
  -> OPEN response
  -> 写 SOCKS/HTTP 成功响应
  -> TqAcceptClientTunnelOpen()
  -> TqRelayStart()
  -> TqDarwinRelayWorker::RegisterRelayWithId()
  -> kqueue EVFILT_READ
  -> readv
  -> MsQuic Stream::Send()
```

ingress reactor 在握手阶段不读取应用层 early data；OPEN 成功且本地代理响应写完后才 accept tunnel。`TqTunnelContext::StartRelay()` 调用 `TqRelayStart()`，macOS 分支选择 `TqDarwinRelayRuntime::PickWorker()` 并注册 relay。

注册阶段会：

- 设置 TCP fd non-blocking。
- 设置 `SO_NOSIGPIPE`，避免 TCP 写触发 SIGPIPE。
- 在 kqueue 上添加 read filter 和 write filter，read 初始 enabled，write 初始 disabled。
- 建立 stream binding，并把 MsQuic callback 设置为 `TqDarwinRelayWorker::StreamCallback`。
- 在 `TqRelayHandle` 中记录 `DarwinWorker` 和 `DarwinRelayId`。

TCP 可读时，`ProcessKqueueEvent()` 调用 `DrainTcpReadable()`：

- 按 `ReadBatchBytes`、`ByteBudgetPerTick` 和 `ReadChunkSize` 分配多个 relay buffer。
- 用 `readv()` 批量读取。
- 非压缩模式下把读取 view 直接提交到 QUIC。
- zstd 模式下压缩后再切成 view。
- `SubmitTcpBatchToQuic()` 构造 `TqDarwinRelaySendOperation`，持有 view owner 和 `QUIC_BUFFER`。
- `TrySubmitQuicSendOperation()` 调用 MsQuic stream send。

TCP EOF 会停止 kqueue read interest，并发送 QUIC FIN。压缩模式会先 flush compressor，把压缩尾部数据带 FIN 发送。

## client quic->tcp

路径是：

```text
MsQuic QUIC_STREAM_EVENT_RECEIVE
  -> TqDarwinRelayWorker::StreamCallback()
  -> QueueDeferredQuicReceive()
  -> TqDarwinRelayEventQueue: QuicReceiveView
  -> worker wake
  -> ProcessQuicReceiveViewEvent()
  -> PendingQuicReceives / PendingTcpWrites
  -> FlushTcpWrites()
  -> sendmsg TCP
  -> ReceiveComplete()
```

macOS receive callback 保存 MsQuic buffer view 的指针和长度，封装为 `TqDarwinPendingQuicReceive`，入 `TqDarwinRelayEventQueue` 后返回 `QUIC_STATUS_PENDING`。因此 receive buffer 的归还由 worker 控制。

worker 处理 `QuicReceiveView` 时：

- 将 view 放入 `PendingQuicReceives`。
- 增加 `PendingQuicReceiveBytes`。
- 根据 pending 压力调用 `MaybePauseQuicReceive()`。
- 调用 `EnqueueQuicReceiveForTcp()` 把 view 转成 `PendingTcpWrites`。
- 调用 `FlushTcpWrites()` 尝试向 TCP fd 写出。

macOS 写 TCP 使用 `sendmsg()`。`PendingTcpWrites` 保存每个待写片段、其关联的 receive view、剩余待归还字节。写成功后推进 pending write 的 data pointer 和 length，减少 `PendingTcpWriteBytes`；当某个 receive view 的所有 pending write 完成后，设置 `PendingCompleteBytes`，从 `PendingQuicReceives` 移除，并调用 `CompleteDeferredQuicReceive()` 归还 MsQuic receive buffer。

如果 TCP 写返回 `EAGAIN/EWOULDBLOCK`，worker 将 `TcpWriteArmed=true` 并启用 kqueue write filter，等待下一次 writable 后继续 `FlushTcpWrites()`。

收到 QUIC FIN 时，worker 在相关数据全部写入 TCP 后执行 `shutdown(tcpFd, SHUT_WR)`，完成 TCP 写方向半关闭。

## server quic->tcp

server 新 QUIC stream 先进入 `TqServerIncomingStreamDispatcher`。普通 OPEN 被移交给 `TqTunnelContext`：

```text
incoming QUIC stream
  -> dispatcher 缓存控制头
  -> TqTunnelContext::TryHandleServerOpen()
  -> decode OPEN
  -> server dial reactor
  -> ACL / DNS / TCP connect
  -> send OPEN response
  -> response SEND_COMPLETE
  -> StartPendingServerRelay()
  -> TqRelayStart()
```

OPEN 后如果同一个 QUIC receive 中还有应用数据，`TqTunnelContext` 会保存到 `PendingRelayRx`。relay 启动后 `DispatchPendingRelayRx()` 构造 synthetic receive 事件交给 Darwin relay callback 处理，保持 OPEN early data 顺序。

server dial reactor 成功返回 connected target fd 后，server 发送 OPEN response。response 的 send complete 到达后才启动 relay；这保证客户端看到 OPEN 成功时，server 侧 relay 已进入可接管状态。随后 QUIC->TCP 路径与 client quic->tcp 相同。

## server tcp->quic

server 的 TCP fd 是 dial reactor 连接到目标服务的 fd。relay 启动后，Darwin worker 通过 kqueue read filter 读取目标 TCP 数据：

```text
target TCP EVFILT_READ
  -> DrainTcpReadable()
  -> readv batch
  -> optional zstd compress
  -> TqDarwinRelaySendOperation
  -> MsQuic Stream::Send()
  -> SEND_COMPLETE
```

目标 TCP EOF 被映射为 QUIC FIN。目标 TCP 硬错误会把 relay 标记 closing，并走 close/retire 流程。

## 报文分发和事件队列

macOS worker 有两个输入：

- kqueue：TCP read/write readiness，以及 worker wake。
- `TqDarwinRelayEventQueue`：MsQuic callback 或控制路径投递的 relay 事件。

`TqDarwinRelayEventQueue` 是固定容量 power-of-two ring buffer，使用 per-cell sequence 做无锁 push/pop，默认容量来自 `EventQueueCapacity`，并设置最大容量上限。事件类型包括：

- `QuicReceiveView`
- `QuicSendComplete`
- `QuicIdealSendBuffer`
- `TcpWritable`
- `StopRelay`
- `Shutdown`

Darwin 实现不在 MsQuic receive callback 中做 TCP I/O；callback 只入队并返回 pending。send complete 可直接回调 `CompleteQuicSend()`，通过 known operation 表处理 operation 生命周期，避免 stream callback 与 worker 关闭竞态。

## TCP 背压

macOS TCP 读侧背压由 QUIC send backlog 控制：

- `MaxInFlightQuicSends` 非 0 且 `InFlightQuicSends` 达上限时暂停读。
- `MaxBufferedQuicSendBytes` 非 0 且 `InFlightQuicSendBytes` 达上限时暂停读。
- 暂停时 `SetTcpReadBackpressure(true)` 设置 `TcpReadPausedByQuicBacklog=true`，并禁用 kqueue read filter。
- 恢复条件是 in-flight send 数低于上限，且 in-flight send bytes 低于 `MaxBufferedQuicSendBytes / 2`。

TCP 写侧背压由 `sendmsg()` 返回值体现。遇到 `EAGAIN/EWOULDBLOCK` 时保留 `PendingTcpWrites`，启用 kqueue write filter，后续 writable 再继续写。`TcpWriteBurstBytes` 和 `TcpWriteMaxBytes` 限制单轮 flush 的写出量，避免单条 relay 长时间占用 worker。

TCP read buffer 的分配使用 `TqRelayBufferBudget`。如果 `TqAllocateRelayBuffer()` 因 pending bytes 超限失败，`DrainTcpReadable()` 会触发读侧背压，等待已有 buffer 随 QUIC send complete 释放后再恢复。

## QUIC 背压

QUIC send 背压由 `TrySubmitQuicSendOperation()` 处理：

- send 前增加 `InFlightQuicSends` 和 `InFlightQuicSendBytes`。
- send complete 后减少计数，并释放 send operation。
- MsQuic 返回 `QUIC_STATUS_OUT_OF_MEMORY` 或 `QUIC_STATUS_BUFFER_TOO_SMALL` 时，把 operation 放入 `PendingQuicSends`，暂停 TCP read。
- 后续 send complete 或状态变化触发 `RetryPendingQuicSends()`。

QUIC receive 背压由 pending 压力控制。macOS 计算：

```text
pendingPressure = PendingQuicReceiveBytes + PendingTcpWriteBytes
```

当 pending pressure 达到 `MaxPendingQuicReceiveBytesPerRelay()` 时，调用 `ReceiveSetEnabled(false)` 暂停该 stream 的 receive。写 TCP 后 pending pressure 下降到 low watermark 时调用 `ReceiveSetEnabled(true)`。low watermark 当前由 `LowPendingQuicReceiveBytesPerRelay()` 给出，通常为上限的一半或等价的低水位配置。

## 队列和生命周期

每条 relay 的核心队列和状态：

- `PendingQuicSends`：QUIC send 资源不足时等待重试的 send operation。
- `PendingQuicReceives`：已从 MsQuic callback 接管、等待 TCP 写完成后归还的 receive view。
- `PendingTcpWrites`：QUIC->TCP 方向待写 TCP 的片段队列。
- `KnownSendOperations`：worker 级别 map，跟踪已提交/正在提交的 QUIC send operation，处理 send complete 和关闭竞态。
- `RetiredRelays` / `RetiredStreamBindings`：延迟释放关闭中的 relay 和 stream binding，等待 callback refs 和 known operations drain。

relay 完整关闭需要 TCP read/write、QUIC send/receive、pending send、pending receive、pending TCP write 都 drain。关闭期间 callback binding 会用 callback refs 防止释放中对象被迟到 callback 访问。

## 锁与同步操作

macOS relay 的同步可以分成控制面建链锁、worker 生命周期锁、worker 级数据结构锁、relay 级状态锁、callback 生命周期原子计数、无锁事件队列原子操作和统计/预算原子操作。下文先记录改造前的完整同步面和性能影响，用于解释热点来源；Phase 1-4 后的当前锁边界见文档开头。基线实现没有在 MsQuic receive callback 中做 TCP I/O，但 callback 仍会触发 relay 查找、receive view 记账和事件入队，所以 QUIC->TCP 方向并不是完全无锁热路径。

### 控制面和建链阶段

- `TqTunnelContext::Lock`：保护 OPEN 状态、`Stream`、`TcpFd`、`RelayStarted`、`PendingRelayRx`、server dial 状态等。`StartRelay()` 会先短暂加锁取出 `Stream/TcpFd`，`TqRelayStart()` 成功后再次加锁标记 `RelayStarted` 并移交 fd；`DispatchPendingRelayRx()` 也会加锁把 OPEN early data 从 `PendingRelayRx` 交换到临时 buffer。该锁属于 tunnel/连接级别，只在建链、关闭、OPEN response、server dial 回调和 early data 移交时出现，不参与 relay 接管后的每个 TCP read/write 或 QUIC receive 转发。
- `TqTunnelContext::StreamOpLock`：串行化 pre-relay 阶段对 MsQuic stream 的 `Send()`/`Shutdown()`，避免状态检查后 stream 被并发关闭。它也是连接级控制面锁，主要影响 OPEN request/response 和失败关闭，不影响 Darwin relay worker 接管后的数据转发。
- `TqClientTunnelOpenHandle::Lock` 与 `RefCount`：保护异步 client OPEN handle 状态和回调所有权。粒度是一次 client 建链请求，OPEN 完成后不在数据面使用。
- `ServerOpenDispatched`、全局 active relay 计数、`TqRelayHandle::Stop`：这些是原子变量。`ServerOpenDispatched` 防止 server OPEN 被重复派发；active relay 计数在 `TqRelayStart()`/`TqRelayStop()` 调整预算；`Stop` 表示 relay handle 生命周期状态。它们是连接级或生命周期级同步，频率低，对报文转发影响可以忽略。

### runtime 和 worker 生命周期

- `TqDarwinRelayRuntime::Mutex`：保护 `Started`、`Workers` 和 round-robin `NextWorker`。`Start()`、`Stop()`、`Snapshot()`、`PickWorker()` 会持有它。`PickWorker()` 每条 relay 注册时调用一次，因此是连接级锁；如果大量短连接同时建立，会在 worker 选择处串行化，但长连接转发期间不再触发。
- `TqDarwinRelayWorker::LifecycleMutex`：保护 worker start/stop/register/unregister 与 kqueue fd 生命周期。`RegisterRelayWithId()` 和 `UnregisterRelay()` 会持有它。粒度是 worker 生命周期和 relay 注册/注销，不在每个报文转发中使用。
- `TqDarwinRelayWorker::RelayMutex`：保护 `Relays`、`RetiredRelays`、`RetiredStreamBindings` 和 worker 级 `KnownSendOperations` map。它既用于连接级注册/关闭，也用于热路径查找和 QUIC send completion：
  - kqueue TCP 事件和 MsQuic receive callback 通过 `FindRelay()` 按 relay id 查 `Relays`，每个事件至少加锁一次。
  - TCP->QUIC 每个 send operation 注册、标记 submitted、完成注销都会操作 `KnownSendOperations`，通常每个 `readv` batch 对应一次 operation。
  - SEND_COMPLETE callback 会先用 `IsKnownSendOperation()` 查询该 map，再在 `CompleteQuicSend()` 中注销。
  这个锁是 worker 级共享锁，所有落在同一个 worker 的 relay 都竞争它。正常情况下临界区短，只做 map 查找/插入/删除；高并发小包、send complete 密集或大量短连接关闭时，它会成为跨连接共享的热锁。

### relay 数据面状态锁

- `RelayState::Mutex`：保护单条 relay 的 TCP/QUIC 状态、backpressure 标志、pending 队列、in-flight send 计数、stream/binding 指针、压缩状态标志等。它是连接级锁，但在报文转发级别频繁出现：
  - TCP->QUIC：`DrainTcpReadable()` 检查关闭状态、累计 `TcpReadBytes`、EOF 标记、backpressure 判断；`SubmitTcpBatchToQuic()` 取 relay id/binding；`TrySubmitQuicSendOperation()` 增加 `SubmittingQuicSends`、`InFlightQuicSends`、`InFlightQuicSendBytes`；send complete 后减少计数并可能恢复 TCP read。
  - QUIC->TCP：receive callback 在 `QueueDeferredQuicReceive()` 中检查 closing 并增加 `PendingQuicReceiveBytes`；worker 处理 receive view 时加入 `PendingQuicReceives`；`EnqueueQuicReceiveForTcp()` 增加 `PendingTcpWrites/PendingTcpWriteBytes`；`FlushTcpWrites()` 取 pending write 快照并在 sendmsg 成功后推进队列和归还 receive view；pause/resume QUIC receive 也通过该锁更新状态。
  - 关闭路径：`RetireRelay()`、`CloseRelay()`、`PurgeRetiredRelaysIfSafe()` 会持有该锁清理队列、关闭 fd、等待 submitting send 退出。
  该锁的主要性能影响是每条 relay 内 TCP worker 线程和 MsQuic callback 线程之间会串行化状态更新。因为 TCP I/O 只在该 relay 所属 worker 线程执行，锁竞争通常来自 QUIC callback、SEND_COMPLETE、Stop/Snapshot；如果流量是小包且 QUIC receive callback 很密，锁开销会按报文或 receive event 放大。

### stream callback 和 send completion 生命周期

- `StreamBinding` 原子字段：`Worker`、`Active`、`RetiredStream`、`CallbackRefs` 用 acquire/release 或 acq_rel 访问。`StreamCallback()` 入口通过 `shared_from_this()` 和 `CallbackRefs.fetch_add()` 固定 binding 生命周期，退出时 `fetch_sub()`；关闭路径用 `Active=false`、`Worker=nullptr`、`RetiredStream` 和 callback refs 判断能否清理 stream callback。它是 callback 生命周期级同步，每个 MsQuic callback 都会执行一次引用计数原子加减。相比 mutex，它避免 callback 入口阻塞，但在高 callback 频率下仍有 cache line 写竞争。
- `CompletionState::Mutex` 与 `KnownSendOperationCount`：`CompletionState` 是 binding 侧 send operation 表，用于 stream 已退休但 SEND_COMPLETE 迟到时仍能安全释放 operation。注册/注销 send operation 时会同时更新 worker 级 `KnownSendOperations` 和 binding 级 completion map。它的粒度是 stream binding 生命周期；在正常未退休路径上每个 QUIC send operation 也会触发一次插入和一次删除，因此属于 TCP->QUIC send batch 级同步。`KnownSendOperationCount` 让退休清理可以先用原子计数做快速判断。

### 无锁队列、buffer 预算和统计原子

- `TqDarwinRelayEventQueue`：固定容量 ring buffer，`TryPush()`/`TryPop()` 使用 `EnqueuePos`、`DequeuePos` 和 per-cell `Sequence` 的 atomic load/CAS/store。它避免了跨线程事件队列 mutex，但每个 QUIC receive view 入队和 worker 出队都会做 CAS；多 producer 同时入队时会在 `EnqueuePos` 和目标 cell sequence 上自旋竞争。入队成功后还会触发 kqueue `EVFILT_USER` wake，wake 的系统调用成本通常比 CAS 更明显。
- `TqRelayBufferBudget::PendingBufferBytes`：`TqAllocateRelayBuffer()` 用 CAS 预留 pending bytes，buffer 释放时 `fetch_sub()`。这个预算是 relay 内共享的，TCP read buffer、压缩/解压输出 buffer、QUIC->TCP pending write buffer 都会触发；它属于 buffer 分配级同步。高吞吐大 batch 时 CAS 次数随 buffer 数增长，`ReadChunkSize` 越小，预算原子操作越频繁。
- worker 统计计数：`EventsProcessed`、`Wakeups`、`TcpReadBatches`、`TcpReadBytes`、`TcpWriteBatches`、`TcpWriteBytes`、`QuicReceiveViewCount`、`QuicReceiveViewBytes`、`DeferredReceiveCompletes`、`QuicReceivePausedCount`、`QuicReceiveResumedCount`、`Errors` 等基本使用 relaxed 原子。它们不提供同步顺序，只用于观测；每个 batch 或事件仍会写共享 cache line，单 worker 高速转发时会有轻微 cache 写成本。

### 对性能的总体影响

按基线热度排序，最值得关注的是：

1. `RelayState::Mutex`：连接级锁，但被每个 TCP read batch、每个 QUIC receive view、每轮 TCP write flush 和 backpressure 变更使用，是单连接数据面的主同步成本。
2. `RelayMutex`：worker 级锁，`FindRelay()` 和 `KnownSendOperations` 让它进入 kqueue event、receive callback 和 SEND_COMPLETE 路径；大量连接共享同一 worker 时会形成跨连接竞争。
3. `CompletionState::Mutex`/`KnownSendOperations`：每个 QUIC send operation 注册和完成都需要 map 操作，粒度接近 TCP->QUIC batch。
4. `TqDarwinRelayEventQueue` 原子 CAS 与 kqueue wake：每个 QUIC receive view 都会触发，主要成本是跨线程唤醒和多 producer CAS 竞争。
5. buffer budget 和统计原子：单次成本小，但随 buffer 数、batch 数线性增加。

控制面锁如 `TqTunnelContext::Lock`、`StreamOpLock`、runtime `Mutex`、worker `LifecycleMutex` 主要影响连接建立/关闭和 snapshot，不是稳定长连接转发的主要瓶颈。

### 最高热点：`RelayState::Mutex`

从基线路径看，macOS 数据面热点最高的是 `RelayState::Mutex`。它名义上是连接级锁，不会让不同 relay 之间直接互斥；但它处在单条 relay 的双向数据面交汇点，TCP worker 线程、MsQuic receive callback、MsQuic send complete callback、stop/snapshot 路径都会访问它。只要单连接吞吐足够高，或者报文被拆成大量小 receive/send batch，这个锁就会从“连接级保护”变成“报文转发级同步”。

它热的根本原因是职责过宽：

- 它同时保护控制状态和数据状态。`Closing`、`TcpReadClosed`、`QuicReceiveClosed` 这类生命周期标志，和 `PendingTcpWrites`、`PendingQuicReceives`、`InFlightQuicSends`、`PendingQuicReceiveBytes` 这类数据面队列/计数放在同一把锁下。
- 它跨线程使用。TCP read/write 在 Darwin worker 线程执行；QUIC receive 和 SEND_COMPLETE 来自 MsQuic callback 线程；关闭和 snapshot 也可能从控制线程进入。即使每个临界区很短，线程之间仍会争用同一 cache line 和 mutex 状态。
- 它在双向转发上都出现。TCP->QUIC 的每个 `readv` batch 会更新 in-flight send；QUIC->TCP 的每个 receive view 会更新 pending receive/write；TCP 写出后又要归还 receive view 并恢复 QUIC receive。
- 它和背压判断耦合。暂停/恢复 TCP read、暂停/恢复 QUIC receive 都需要读取多个 pending/in-flight 字段，所以即使没有队列 push/pop，也会频繁短加锁。

基线影响最明显的场景有三类：

- 小包高 QPS：每个 receive view 或 read batch 携带的数据少，锁操作次数按报文数增长，单位字节同步成本升高。
- QUIC->TCP 写阻塞：`PendingTcpWrites` 堆积后，`FlushTcpWrites()` 每次 writable 都要取快照、推进队列、扫描同一个 receive 是否还有 pending write，锁内工作量随队列长度上升。
- send complete 密集：TCP->QUIC 方向 in-flight send 完成频繁，callback 会更新 `InFlightQuicSends/InFlightQuicSendBytes` 并触发 retry/resume 判断，与 worker 线程读 TCP 形成竞争。

更合适的处理思路是把 `RelayState` 改成更明确的“worker actor + callback 投递”模型：

1. worker 线程独占修改 relay 数据面队列。`PendingQuicReceives`、`PendingTcpWrites`、`PendingQuicSends`、`PendingQuicReceiveBytes`、`PendingTcpWriteBytes`、`InFlightQuicSends` 等由 worker 线程单写，MsQuic callback 不直接修改这些字段。
2. MsQuic receive callback 只做最小工作：校验 binding 仍 active、构造 receive view、入队、返回 `QUIC_STATUS_PENDING`。pending bytes 可以先记到 binding 级原子计数，或者由事件队列容量和 MsQuic receive disable 共同限制，避免 callback 入队前失控占用内存。
3. SEND_COMPLETE callback 也尽量投递 completion 事件给 worker，由 worker 统一减少 in-flight 计数、释放 operation、恢复 TCP read。只有 stream 已退休且 worker 不可用时才走 detached completion 兜底。
4. 生命周期状态拆成少量原子门闩。`Closing`、`Active`、`Worker`、`RetiredStream` 这类 callback 入口必须快速判断的字段保留为原子；复杂清理仍交给 worker 线程串行执行。
5. snapshot 改为低干扰读取。热字段可以由 worker 本地累计，snapshot 读取原子镜像或通过事件请求 worker 汇报，避免 snapshot 持有 `RelayState::Mutex` 扫描队列。

这个方向的收益是：单条 relay 的数据面状态从多线程互斥变成 worker 线程串行处理，锁竞争会转化为事件队列压力；TCP I/O、QUIC receive 归还、背压状态变化的顺序也更容易推理。代价是必须补齐三个边界：

- 内存上限：如果 receive callback 不再直接增加 `PendingQuicReceiveBytes`，就需要在 callback 入口有等价的原子预算或队列容量门限，否则 worker 处理慢时 pending receive buffer 会增长。
- 关闭竞态：close/retire 必须能处理已经入队但尚未处理的 receive、send complete、tcp writable 事件，保证 MsQuic receive buffer 最终 `ReceiveComplete()`，send operation 最终释放。
- wake 可靠性：callback 投递事件后需要保证 worker 不会因为 wake 合并或队列空/非空竞态而睡过头。

设计上按以下阶段推进，降低一次性重构风险：

1. 先把 receive callback 对 `PendingQuicReceiveBytes` 的直接修改迁移为 binding 级原子预算，worker 出队后再转入 relay 内部计数。
2. 再把 SEND_COMPLETE 改为优先投递 worker 事件，保留当前直接完成路径作为退休/停止兜底。
3. 最后把 `RelayState::Mutex` 缩小为关闭路径保护，热路径字段改为 worker 线程私有状态或原子门闩。

每一步都需要用小包双向转发、TCP 写阻塞、MsQuic 同步 SEND_COMPLETE、relay stop 与迟到 callback 这些场景做回归验证。

### 可选优化方向

- 把 relay 状态进一步 actor 化：让 worker 线程成为 `RelayState` pending 队列、backpressure 标志和 in-flight 计数的唯一写者；MsQuic receive callback 只构造 receive view 并入队，不直接增加 `PendingQuicReceiveBytes`。这样可以移除 QUIC->TCP receive callback 对 `RelayState::Mutex` 的热路径依赖，但需要用事件队列容量、每 binding 原子 pending bytes 或 MsQuic receive disabled 状态保证 callback 入队前的内存上限。
- 减少 worker 级 `RelayMutex` 热路径：kqueue `udata` 已携带 relay id，可以考虑让事件携带 `shared_ptr`/稳定 handle，或使用分片 map、RCU/epoch 表、intrusive refcount 表来降低 `FindRelay()` 的 worker 级串行化。代价是关闭路径和迟到事件的生命周期协议会更复杂。
- 优化 send operation 跟踪：当前每个 operation 同时进入 worker map 和 binding completion map。可以考虑把 operation 所属 binding 和 relay id 完全放入 operation 自身，并以 intrusive refcount/状态位处理迟到 SEND_COMPLETE，只保留退休 binding 的 operation 计数或链表，减少每个 send batch 的 map mutex 操作。这个改动要重点验证 synchronous SEND_COMPLETE、提交中失败和 stream callback 退休竞态。
- 批量化 QUIC receive 入队和 wake：如果 MsQuic 连续触发多个 receive，可以合并 wake，或在队列从空变非空时才触发 kqueue wake，降低 `EVFILT_USER` 系统调用频率。需要确保 worker 睡眠和队列非空之间没有丢 wake。
- 调整 buffer 粒度和预算策略：增大 `ReadChunkSize`/`ReadBatchBytes` 可降低每字节 buffer budget CAS 次数，但会增加单连接内存占用和尾延迟风险。更激进的方案是 per-relay worker 本地预算加全局低频 reconcile，但会削弱当前严格 pending bytes 上限。
- 降低观测计数写放大：统计可以先累积在线程本地或 worker 本地普通计数，再在 snapshot 或固定 tick 汇总为原子值。这样能减少 relaxed 原子写入，但会牺牲实时指标精度。

## 关键参数

- `ReadChunkSize`：单个 read buffer 大小，默认 128 KiB。
- `ReadBatchBytes`：单轮 TCP read 批量预算。
- `ByteBudgetPerTick`：worker 单次处理预算。
- `MaxIov`：readv/sendmsg 最大 iov 数。
- `TcpWriteMaxBytes`：单次写尝试上限。
- `TcpWriteBurstBytes`：单轮 flush burst 上限。
- `MaxPendingQuicReceiveBytesPerRelay`：QUIC receive pending 上限。
- `DeferredReceiveCompleteBatchBytes`：批量 `ReceiveComplete()` 阈值。
- `MaxInFlightQuicSends`：in-flight QUIC send operation 上限。
- `MaxBufferedQuicSendBytes`：in-flight QUIC send bytes 上限，也参与 TCP read buffer budget。
