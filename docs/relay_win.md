# Windows relay 数据转发路径

本文记录当前 Windows 实现中 TCP/QUIC 数据面从控制面建链到 relay worker 接管后的转发路径、线程模型、报文分发、TCP 背压、QUIC 背压和队列设计。代码入口主要在 `src/ingress/client_ingress_reactor.cpp`、`src/tunnel/tcp_tunnel.cpp`、`src/tunnel/relay.cpp`、`src/tunnel/windows_relay_worker.cpp`。

## 总体结构

Windows 数据面由 `TqWindowsRelayRuntime` 管理一组固定 `TqWindowsRelayWorker`。每个 worker 拥有一个 IOCP handle 和一个 worker 线程。OPEN 成功后 `TqRelayStart()` 调用 Windows runtime，把 TCP socket 绑定到 IOCP，并把 MsQuic stream callback 切换成 relay callback。

线程边界如下：

- client ingress reactor：本地 listen、accept、代理握手、发起 client OPEN、写 SOCKS/HTTP 响应。
- server dial reactor：server OPEN 后执行 ACL、DNS 和非阻塞 TCP connect。
- MsQuic worker：触发 stream callback，包括 receive、send complete、ideal send buffer、abort/shutdown。
- Windows relay worker：IOCP 循环，处理 WSARecv/WSASend completion 和由 callback 投递的 worker operation。
- tunnel reaper：回收停止后的 tunnel context。

Windows relay 不使用 epoll/kqueue，也没有独立的 relay ring queue；跨线程分发统一落到 IOCP，通过 `PostQueuedCompletionStatus()` 投递 callback operation。

## client tcp->quic

路径是：

```text
TqClientIngressReactor listen/accept
  -> SOCKS5 / HTTP CONNECT / port-forward 握手
  -> TqStartClientTunnelAsync()
  -> QUIC OPEN request
  -> OPEN response
  -> ingress 写本地成功响应
  -> TqAcceptClientTunnelOpen()
  -> TqRelayStart()
  -> TqWindowsRelayWorker::RegisterRelay()
  -> WSARecv overlapped
  -> IOCP TcpRecv completion
  -> MsQuic Stream::Send()
```

ingress reactor 只处理代理握手和 OPEN 编排。OPEN 成功后，ingress 写回本地代理协议成功响应；写完后调用 `TqAcceptClientTunnelOpen()`，由 `TqTunnelContext::StartRelay()` 进入 Windows relay。

注册 relay 时：

- 调用 `CreateIoCompletionPort(tcpFd, workerIocp, ...)` 把 TCP socket 关联到 worker IOCP。
- 创建 `RelayContext`，保存 TCP fd、MsQuic stream、压缩器、解压器、tuning、public handle。
- 创建 callback binding，并设置 `stream->Callback = TqWindowsRelayWorker::StreamCallback`。
- 更新 `TqRelayHandle` 为 `WindowsWorker`，记录 worker index 和 relay id。
- 调用 `MaybePostTcpRecv()` 发起第一笔 overlapped `WSARecv()`。

TCP 收包不是 readiness 模式，而是 posted receive 模式。`PostTcpRecv()` 分配 `TqRelayBuffer`，投递一个 overlapped `WSARecv()`。IOCP 收到 `TcpRecv` completion 后进入 `HandleTcpRecv()`：

- `bytes == 0` 表示 TCP 读 EOF，进入 `HandleTcpReadClosed()` 并发送 QUIC FIN。
- 非压缩模式下，receive buffer owner 直接挂到 `TqWindowsQuicSendOperation`，构造 `QUIC_BUFFER`。
- zstd 模式下先压缩到 `OwnedBytes`，再构造 `QUIC_BUFFER`。
- 调用 `TrySubmitQuicSendOperation()` 提交 MsQuic send。
- 如果没有被 QUIC backlog 暂停，继续 `MaybePostTcpRecv()` 投递下一笔 TCP receive。

## client quic->tcp

路径是：

```text
MsQuic QUIC_STREAM_EVENT_RECEIVE
  -> TqWindowsRelayWorker::StreamCallback()
  -> QueueDeferredQuicReceive()
  -> PostQueuedCompletionStatus(RelayReceiveReady)
  -> worker DrainRelayReceives()
  -> WSASend overlapped
  -> IOCP TcpSend completion
  -> AdvanceReceiveView()
  -> ReceiveComplete()
```

Windows receive callback 会把 MsQuic buffer 内容复制到 `TqWindowsPendingQuicReceive::OwnedBuffer`。这是 Windows 与 Linux/macOS 的一个重要差异：Windows 不长期持有 MsQuic buffer 指针，而是在 callback 中拷贝出稳定内存，然后返回 `QUIC_STATUS_PENDING`。真正归还 receive ownership 仍由后续 `ReceiveComplete()` 完成。

`QueueDeferredQuicReceive()` 做以下事情：

- 计算 buffer 总长度。
- 复制 MsQuic buffers 到 `OwnedBuffer`，并建立 slices。
- 增加 `PendingQuicReceiveBytes` 和 `PendingQuicReceiveQueueDepth`。
- 将 view 放入 `PendingReceives`。
- 如 pending bytes 达到 `WindowsRelayMaxPendingQuicReceiveBytesPerRelay`，调用 `StreamReceiveSetEnabled(FALSE)` 暂停 QUIC receive。

callback 随后投递 `RelayReceiveReady` 到 IOCP。worker 收到后调用 `DrainRelayReceives()`，只处理队首 view；如果该 view 已有 `TcpSendPending`，则等待已有 WSASend completion。

非压缩模式下，`PostTcpSendFromReceiveView()` 针对当前 slice 投递 overlapped `WSASend()`。TCP send completion 到达 `HandleTcpSend()` 后：

- 检查完成字节数。
- 推进 slice offset 和 completed length。
- 累计 `PendingCompleteBytes`，按 batch 阈值或 view 完成调用 `ReceiveComplete()`。
- view 未完成则继续投递下一段 WSASend。
- view 完成则从 `PendingReceives` 移除，减少 pending bytes/depth，并调度下一次 drain。

zstd 解压模式走 `PostTcpSendFromCompressedReceiveView()`：先用 decompressor 把 receive view 解压到 relay buffer，再对解压输出投递 WSASend；输入消费后同样推进 receive view 并可批量 `ReceiveComplete()`。

QUIC FIN 到达后，如果没有 payload，Windows 会直接设置 `CloseAfterDrained` 并 shutdown TCP send；如果 FIN 跟随数据，则等数据全部写入 TCP 后再半关闭。

## server quic->tcp

server 新 QUIC stream 先进入 `TqServerIncomingStreamDispatcher`。普通 OPEN 被交给 `TqTunnelContext`：

```text
incoming QUIC stream
  -> dispatcher 判断 TQ_CMD_OPEN
  -> TqTunnelContext::TryHandleServerOpen()
  -> decode TqOpenRequest
  -> TqServerDialReactor::Submit()
  -> ACL / c-ares DNS / non-blocking TCP connect
  -> send OPEN response
  -> response SEND_COMPLETE
  -> StartPendingServerRelay()
  -> TqRelayStart()
```

server 解析 OPEN 时，如果同一个 MsQuic receive 中 OPEN 后已经带了应用数据，会保存到 `PendingRelayRx`。relay 启动后 `DispatchPendingRelayRx()` 用 synthetic receive 事件交给 Windows relay callback，因此 OPEN early data 会走正常 quic->tcp 队列。

dial reactor 成功后，server 保存 target TCP fd，发送 OPEN response。只有 response 的 MsQuic `SEND_COMPLETE` 到达后，`StartPendingServerRelay()` 才调用 `StartRelay()`，避免数据面早于 OPEN 响应可见。

relay 启动后，server quic->tcp 与 client quic->tcp 相同：MsQuic receive callback 入 `PendingReceives`，IOCP worker 逐段 WSASend 到目标 TCP。

## server tcp->quic

server 的 TCP fd 来自 dial reactor 成功连接目标服务。Windows relay 注册后立即投递 WSARecv：

```text
target TCP WSARecv completion
  -> IOCP TcpRecv
  -> HandleTcpRecv()
  -> optional zstd compress
  -> TqWindowsQuicSendOperation
  -> MsQuic Stream::Send()
  -> QUIC SEND_COMPLETE
```

目标 TCP EOF 被映射为 QUIC FIN。目标 TCP receive/send 的硬错误通过 `HandleTcpPostFailure()`、`RecordTcpHardErrorAndFail()` 等路径关闭 relay。

## 报文分发和 IOCP operation

Windows worker 的所有异步事件最终都通过 IOCP 串行处理。`TqWindowsIocpOperationType` 包含：

- `TcpRecv`
- `TcpSend`
- `QuicSendComplete`
- `QuicSendRetry`
- `RelayReceiveReady`
- `RelayReceiveDrain`
- `QuicIdealSendBuffer`
- `QuicPeerSendAborted`
- `QuicPeerReceiveAborted`
- `QuicShutdownComplete`
- `CloseRelay`
- `StopWorker`

TCP I/O completion 由 Winsock 投递到 IOCP。MsQuic callback 不直接操作复杂 relay 状态，而是通过 `PostCallbackOperation()` 或 `QueueQuicSendCompleteFromCallback()` 投递 IOCP operation。这样大多数 relay 状态推进在 worker 线程内完成，减少跨线程状态竞争。

receive drain 使用 `ReceiveDrainQueued` 做合并：如果已有 drain operation 在队列中，新的调度只增加 coalesced 计数，不重复投递。

## TCP 背压

Windows TCP 读侧背压由 `MaybePostTcpRecv()` 控制。它只在满足条件时投递新的 WSARecv：

- relay 未 closing。
- TCP read 未关闭。
- 没有已 posted 的 TCP recv。
- QUIC send backlog 未达到暂停阈值。

暂停阈值来自 `CurrentRelayIdealSendBytes()`。`ShouldPauseTcpReadForQuicBacklog()` 在 `OutstandingQuicSendBytes >= idealSendBytes` 时暂停。恢复条件更保守：`OutstandingQuicSendBytes < idealSendBytes / 2`。暂停时设置 `TcpReadPausedByQuicBacklog=true`，不会投递新的 WSARecv；已经 in-flight 的 WSARecv 仍会正常完成。

TCP 写侧背压由 overlapped `WSASend()` 表示：

- 投递成功但返回 `WSA_IO_PENDING` 是正常异步 pending。
- completion 字节数小于 posted length 时，非压缩路径推进部分 offset 后继续投递剩余部分。
- receive view 有 `TcpSendPending` 时，`DrainRelayReceives()` 不会处理后续 view，保持每条 relay 的 QUIC->TCP 写顺序。

relay buffer 分配也会限制 TCP read。`RelayContext` 继承 `TqRelayBufferBudget`，`MaxPendingBufferBytes` 至少覆盖 QUIC receive pending 上限和 in-flight send buffer 需求；分配失败会停止本次 post 并走错误/关闭路径。

## QUIC 背压

QUIC send 背压由 `TrySubmitQuicSendOperation()` 处理：

- send 前增加 `InFlightQuicSends`。
- MsQuic send 成功后增加 `OutstandingQuicSendBytes`，并更新 max observed。
- `SEND_COMPLETE` 由 callback 投递 `QuicSendComplete` 到 IOCP，worker 再减少 outstanding/in-flight 并释放 operation。
- 如果 MsQuic 返回 backpressure 类状态，operation 放入 `PendingQuicSendRetries`，并设置 TCP read backpressure。
- 后续 `RetryPendingQuicSends()` 逐个重试，队列清空后尝试恢复 TCP recv。

QUIC receive 背压由 `PendingQuicReceiveBytes` 控制：

- 达到 `WindowsRelayMaxPendingQuicReceiveBytesPerRelay` 时调用 `StreamReceiveSetEnabled(FALSE)`。
- `HandleTcpSend()` 推进 receive view 后减少 pending bytes。
- pending bytes 降到上限一半以下时 `MaybeResumeQuicReceive()` 调用 `StreamReceiveSetEnabled(TRUE)`。
- 上限为 0 时表示不按该字节阈值暂停，resume 会直接启用 receive。

Windows callback 虽然复制了 receive buffer，但仍返回 `QUIC_STATUS_PENDING`，所以必须在 view 完成或丢弃时调用 `ReceiveComplete()`，否则 MsQuic 的 receive flow control 不会释放。

## 队列和生命周期

每条 Windows relay 的主要队列和状态：

- `PendingReceives`：QUIC->TCP 方向待写 TCP 的 receive view。
- `CallbackPendingQuicReceives`：保留字段，用于 callback pending 统计/兼容；主要路径使用 `PendingReceives`。
- `PendingQuicSendRetries`：QUIC send backpressure 时等待重试的 operation。
- `TcpRecvOpsFree`：可复用的 overlapped TCP receive operation。
- `ReceiveDrainQueued`：避免重复投递 receive drain。
- `InFlightTcpRecvs` / `InFlightTcpSends` / `InFlightQuicSends`：关闭和 drain 判断使用。
- `CloseAfterDrained`：收到 FIN 或 send teardown 后，等待 pending 数据 drain 再半关闭/关闭。

关闭流程会等待或处理迟到 completion。`RelayContext`、callback binding 和 retired callback 列表用于防止 stream callback、IOCP completion 和 StopRelay 之间的生命周期竞态。完成条件包括：TCP recv/write 方向关闭、QUIC FIN send complete、pending receives 清空、in-flight TCP send/recv 和 QUIC send 归零。

## 锁操作和性能影响

Windows relay 的主要并发策略是：TCP completion、callback operation 和 relay 维护操作最终进入同一个 worker IOCP，由 worker 线程推进大部分状态；只有跨线程入口、连接注册/回收、少数 per-relay 队列和观测路径使用互斥锁。下面按流程中的锁粒度整理。

| 锁 | 位置 | 保护对象 | 粒度 | 是否在转发热路径 | 性能影响 |
|----|------|----------|------|------------------|----------|
| `TqClientTunnelOpenHandle::Lock` | `tcp_tunnel.cpp` client OPEN handle | OPEN 状态、`Context`、完成回调 | 单次 client OPEN | 否 | 只在 OPEN 成功、accept/reject/cancel 和超时清理时使用。属于连接建立控制面锁，不参与 relay 后的报文转发。 |
| `TqTunnelContext::Lock` | `tcp_tunnel.cpp` | tunnel 生命周期、`TcpFd`/`Stream`、`RelayStarted`、pending relay flags、`PendingRelayRx` | 单 tunnel / 单连接 | 建链和 early data 一次性路径上使用，relay 启动后基本退出数据面 | 保护控制面状态，`StartRelay()` 前后各短暂持锁；server/client OPEN early data 只在保存或 swap `PendingRelayRx` 时持锁。对持续吞吐影响很低。 |
| `TqTunnelContext::StreamOpLock` | `tcp_tunnel.cpp` | pre-relay 阶段 MsQuic stream `Send()`/`Shutdown()` 顺序 | 单 tunnel / 单 stream | 否 | 只串行化 OPEN、失败响应、abort/shutdown 等控制操作；relay 接管后 QUIC data send 由 Windows worker 走 `Stream::Send()`，不再通过该锁。 |
| `TqServerDialReactor::Lock` | `server_dial_reactor.cpp` | server OPEN 的 DNS/connect 任务队列和回调状态 | dial reactor 级 | 否 | 只影响 server 侧 OPEN 到目标 TCP connect 的控制面排队，不参与 quic->tcp 或 tcp->quic 转发。高连接建立速率下可能影响建链延迟，但不影响已接管 relay 的吞吐。 |
| `TqTunnelReaper::Mutex` | `tunnel_reaper.cpp` | 待回收 tunnel context 列表 | reaper 全局 | 否 | `StartRelay()` 注册一次，relay stop 后回收一次；不是报文级锁。 |
| `TqWindowsRelayRuntime::Lock_` | `windows_relay_worker.cpp` runtime | worker vector、start/stop、worker 选择 | Windows relay runtime 全局 | 否，连接注册/metrics 路径 | `Start()`/`RegisterRelay()`/`Snapshot()` 持锁。连接建立时会短暂串行选择 worker；已建立连接的报文转发不经过该锁。admin snapshot 持有 runtime 锁并逐个取 worker snapshot，可能与新 relay 注册短暂互斥。 |
| `TqWindowsRelayWorker::Lock_` | `windows_relay_worker.cpp` worker | `Relays_` map、`RetiredCallbacks_` | 单 worker 级，覆盖该 worker 上所有 relay | 部分在 callback/completion 热路径 | `RegisterRelay()`、`StopRelay()`、`FindRelayById()`、`StreamCallback()` 查 relay、`QueueQuicSendCompleteFromCallback()`、`TryRetireRelay()`、`Snapshot()` 都会使用。锁内主要是 map 查找/插入/删除和 retired callback 列表维护，临界区短；但同一 worker 上大量 stream 同时触发 MsQuic callback 或 SEND_COMPLETE 时，会形成跨连接共享锁竞争。 |
| `RelayContext::PendingReceiveLock` | `windows_relay_worker.cpp` per relay | `PendingReceives` receive view 链表 | 单 relay / 单连接 | 是，quic->tcp receive view 级 | `QueueDeferredQuicReceive()` 在 MsQuic callback 中入队，`DrainRelayReceives()` 取队首，`FinishReceiveView()`/`CompleteAllPendingQuicReceives()` 删除或清空。锁内只做 push/front/erase/swap，不做拷贝、解压或 `WSASend()`，但每个 QUIC receive view 至少会触发入队和出队锁操作。慢 TCP 写导致队列变长时，查找/erase 当前 view 的成本也会增加。 |
| `RelayContext::TcpRecvOpsLock` | `windows_relay_worker.cpp` per relay | `TcpRecvOpsFree` overlapped recv operation freelist | 单 relay / 单连接 | 是，tcp->quic recv completion/post 级 | `PostTcpRecv()` 取 freelist，`HandleTcpRecv()`/`HandleTcpReadClosed()` 归还 operation。正常转发阶段大多由同一个 worker 线程访问，实际竞争通常很低；但它仍是每个 TCP receive completion 的显式 mutex 操作。 |
| `RelayContext::PendingQuicSendLock` | `windows_relay_worker.cpp` per relay | `PendingQuicSendRetries` | 单 relay / 单连接 | 只在 QUIC send backpressure、关闭和观测时热 | 普通 `Stream::Send()` 成功路径不入队，不持该锁；MsQuic 返回资源类 backpressure 时才把 send operation 放入 retry 队列，`RetryPendingQuicSends()` 再取出重试。高丢包/低窗口或内存压力下会变成 tcp->quic 方向的连接级瓶颈，但正常吞吐路径影响低。 |
| `RelayContext::CallbackPendingQuicReceiveLock` | `windows_relay_worker.cpp` per relay | `CallbackPendingQuicReceives` 兼容/统计字段 | 单 relay / 单连接 | 否 | 主要在 snapshot、drain 判断和测试辅助路径读取；当前主路径使用 `PendingReceives`，因此对转发基本无影响。 |
| `LastPostedCallbackLock_` | `windows_relay_worker.cpp` unit test | 最近一次 callback post 记录 | worker 测试状态 | 否 | 仅 `TQ_UNIT_TESTING` 下存在，不影响生产性能。 |

除上表的 mutex 外，Windows relay 还大量使用 atomic 状态，例如 `Closing`、`TcpRecvPosted`、`InFlightTcpRecvs`、`InFlightTcpSends`、`InFlightQuicSends`、`PendingQuicReceiveBytes`、`PendingQuicReceiveQueueDepth`、`OutstandingQuicSendBytes`、`ReceiveDrainQueued`、callback binding 的 `CallbackRefs`/`Closing`/`RelayHint`。这些不是锁，但在高并发 callback 和 worker completion 下会产生缓存一致性开销。当前实现把大对象队列操作控制在短锁内，把计数和关闭判定放到 atomic 上，是偏保守、易验证的设计。

### 对当前转发路径的影响

- `tcp->quic` 普通成功路径的显式锁主要是 `TcpRecvOpsLock`：每次 `WSARecv` completion 后归还 `IoOperation`，下一次 `PostTcpRecv()` 再取出；QUIC send 成功时不需要 `PendingQuicSendLock`。因此单连接吞吐更多受 `WSARecv`/`Stream::Send()`、压缩、buffer budget 和 MsQuic send backlog 影响，而不是 mutex 竞争。
- `tcp->quic` backpressure 路径会使用 `PendingQuicSendLock`。这时 TCP read 已被 `TcpReadPausedByQuicBacklog` 或 `quic_send_resource` 暂停，锁竞争不是根因，根因通常是 QUIC send 窗口、MsQuic buffer/resource 或网络拥塞。
- `quic->tcp` 路径的显式热锁是 `PendingReceiveLock`。MsQuic callback 拷贝 receive buffer 后持锁入 `PendingReceives`，worker drain/finish 持锁取队首和删除 view。单连接内该锁保护有序队列；多连接之间不共享。但同一 relay 的 callback 线程和 worker 线程可能竞争，尤其是 TCP 写慢、pending receive queue 较深时。
- 跨连接共享的主要锁是 `TqWindowsRelayWorker::Lock_`。每个 MsQuic callback 需要通过 binding relay id 到 `Relays_` map 取 `shared_ptr`，SEND_COMPLETE 也会查找 relay。临界区很短，但它是 worker 内所有 relay 共享的锁；当 `RelayWorkers` 较少、单 worker 承载大量连接且 callback 频率很高时，它比 per-relay 锁更可能表现为横向扩展瓶颈。
- `TqWindowsRelayRuntime::Lock_`、`TqTunnelContext::Lock`、`TqClientTunnelOpenHandle::Lock`、`TqServerDialReactor::Lock` 属于建链/关闭/观测路径。它们会影响连接建立速率和 admin snapshot 抖动，不应直接影响 relay 接管后的稳定转发吞吐。

### 最高热点锁：`TqWindowsRelayWorker::Lock_`

从锁粒度和调用频率看，Windows 平台最值得优先关注的是 `TqWindowsRelayWorker::Lock_`。它保护单个 worker 的 `Relays_` map 和 `RetiredCallbacks_`，本身临界区很短，但它是 worker 级共享锁：只要多个 relay 被分到同一个 worker，它们的 MsQuic callback、send complete、stop、snapshot 和 retire 都会争用这把锁。相比之下，`PendingReceiveLock` 和 `TcpRecvOpsLock` 都是 per-relay 锁，主要影响单连接内部；`Lock_` 才会把一个连接的 callback 频率放大成整个 worker 的横向扩展问题。

`Lock_` 当前出现在这些关键路径：

- `StreamCallback()`：除 `SEND_COMPLETE` 之外的 receive、ideal send buffer、peer abort、shutdown complete 等事件，都会先根据 `binding->RelayId` 到 `Relays_` 查找 relay，并校验 `Callback` 与 `RelayHint`。
- `QueueQuicSendCompleteFromCallback()` / `FindRelayById()`：`SEND_COMPLETE` callback 也要根据 operation relay id 查 `Relays_`，然后再把 `QuicSendComplete` 投递到 IOCP。
- worker 维护路径：`DrainPerRelayMaintenance()` 需要复制 `Relays_` 快照；`TryRetireRelay()` 关闭时从 `Relays_` 删除 relay，并把 callback binding 放到 `RetiredCallbacks_`。
- 观测路径：`Snapshot()` 持锁遍历 active relay，生成 admin metrics。
- 连接控制路径：`RegisterRelay()` 插入 map，`StopRelay()` 查找 relay 并投递关闭 operation。

这把锁的性能风险不在于单次持锁时间长，而在于“每个 callback 都要进同一个 worker map”。当单 worker 承载很多活跃 stream 时，QUIC receive 和 send complete 都可能由 MsQuic worker 线程并发触发；这些线程会在 relay worker 的 `Lock_` 前排队。排队期间 callback 还没有把 operation 投递到 IOCP，worker 线程也就无法及时推进状态。表现上可能是 callback 到 IOCP post 的延迟升高、`WindowsCallbackIocpPostCount` 增长但 worker drain 滞后、CPU profile 出现 mutex wait，或者提高 `RelayWorkers` 后吞吐/尾延迟明显改善。

这个热点的判断需要注意边界：

- 单连接大流量时，`Lock_` 不一定是最大热点，因为一个 relay 的 receive view 处理更可能受 `PendingReceiveLock`、TCP 写、压缩或 MsQuic send backlog 影响。
- 多连接高并发、小包或高 callback 频率场景下，`Lock_` 更容易成为最高热点，因为它跨连接共享。
- admin snapshot 如果频率很高，也会放大 `Lock_` 持有时间，因为 snapshot 会在锁内遍历 worker 上所有 active relay；这属于观测侧干扰。

#### 方案一：先降低观测和配置侧干扰

这是风险最低的处理办法，适合先做：

- runtime snapshot 先复制 worker 指针列表，再释放 `TqWindowsRelayRuntime::Lock_`，避免 admin metrics 阻塞新 relay 注册。
- worker snapshot 避免在 `Lock_` 内做过多 per-relay 计算；先复制 `shared_ptr<RelayContext>` 列表，释放 `Lock_` 后逐个读取 atomic 和 per-relay 状态。
- 对 active relay 明细提供采样或开关，避免高频 admin 请求每次遍历所有 relay。

该方案不移除 callback 热路径上的 `Lock_`，但可以减少非数据面路径制造的锁等待。它的正确性风险低，适合作为第一步。

#### 方案二：callback binding 直接 pin relay，减少 map 查找

这是针对 `Lock_` 热点的核心方案。当前 callback binding 保存 `RelayId`、`RelayHint` 原始指针、`CallbackRefs` 和 `Closing`，但为了安全拿到 `shared_ptr<RelayContext>`，callback 仍然进入 `Relays_` map 查找。可以把 binding 改成能直接取得安全引用：

- 在 `CallbackBinding` 中保存 `std::weak_ptr<RelayContext>` 或自定义 intrusive refcount handle。
- `StreamCallback()` 先检查 `Closing`，再尝试从 binding pin 出 relay；成功后直接检查 relay 状态并投递 IOCP，不再进入 worker `Relays_` map。
- `TryRetireRelay()` 关闭时先设置 `binding->Closing=true`、清空 `RelayHint` 或 weak handle，再等待 `CallbackRefs==0` 后清理 retired binding。
- `Relays_` map 仍保留给注册、StopRelay、snapshot 和 worker maintenance 使用，但不再服务每个 callback。

这个方案的收益最大：receive、ideal buffer、abort、shutdown complete 和 send complete 都可以少一次 worker 级 map 锁竞争。主要风险是生命周期：不能让迟到 callback 访问已经析构的 `RelayContext`，也不能因为 binding 强引用 relay 导致 relay 永不回收。实践上更推荐 `weak_ptr` 或“binding refcount + relay generation”组合，而不是在 binding 中长期持有强 `shared_ptr`。

#### 方案三：把 callback 变成纯投递路径

进一步的做法是让 MsQuic callback 不直接获取 relay 对象，而只使用 callback binding 中的不可变字段投递 IOCP operation：

- binding 保存 worker 指针、relay id、generation 和 closing flag。
- callback 不查 `Relays_`，直接构造 operation，operation 中只带 relay id/generation 和必要 payload。
- worker 线程收到 operation 后，在自己的线程内查 `Relays_`，校验 generation，再推进状态；如果 relay 已关闭，就按 stale operation 丢弃。

这个方案能把 `Lock_` 从 MsQuic callback 线程移到 worker 线程，减少跨线程锁竞争和 callback 阻塞。但它不是完全无锁：worker 仍要查 map，只是查找发生在单 worker 线程内，竞争显著降低。代价是 operation payload 需要更完整，例如 receive view、send complete 指针、ideal buffer byte count 等都要能安全跨线程携带；关闭和 stale drop 统计也要更严格。

#### 方案四：分片或读写化 `Relays_`

如果不想大改 callback 生命周期，可以考虑降低 `Lock_` 的冲突范围：

- 每个 worker 内把 `Relays_` 按 relay id 分片，例如 16 或 64 个 shard，每个 shard 一把 mutex。
- callback 查找只锁对应 shard；snapshot 遍历所有 shard。
- 或者使用 `std::shared_mutex`，callback 查找走 shared lock，注册/删除走 exclusive lock。

分片比 `shared_mutex` 更可控，因为 callback 频率高时 shared lock 仍可能让删除和 shutdown 等写路径等待；而分片能减少无关 relay 之间的互斥。缺点是实现复杂度中等，snapshot 和 retire 要遍历多个 shard，死锁顺序必须固定。这个方案能缓解竞争，但不能消除每个 callback 都查 map 的结构性成本。

#### 建议落地顺序

推荐按“可观测、低风险、再去锁”的顺序推进：

1. 先加观测：记录 `StreamCallback()` 从进入到 `PostCallbackOperation()` 的耗时、`FindRelayById()` 次数、worker `Lock_` 等待时间、snapshot 耗时。没有这些数据时，不应直接上生命周期重构。
2. 先拆 snapshot：把 admin 观测从 `Lock_` 的长临界区里移出去，避免误判数据面锁热点。
3. 再做 callback binding pin relay：这是最直接降低最高热点锁的方案，但要配套迟到 callback、retired binding 和 relay 回收测试。
4. 如果仍有竞争，再考虑 sharded `Relays_` 或纯投递 callback。前者是缓解，后者是更彻底的架构调整。

需要覆盖的测试点包括：receive callback 与 `StopRelay()` 并发、SEND_COMPLETE 晚于 relay retire、shutdown complete 晚到、relay id 复用防护、snapshot 与 register/retire 并发、callback pin 失败时不泄漏 MsQuic receive ownership。

### 可选优化方向

当前实现的锁设计总体合理：数据面状态推进被 IOCP worker 串行化，锁的临界区短，且大部分锁是 per-relay 级别。除非 profiling 显示 mutex wait 或 worker `Lock_` 竞争明显，否则不建议为了去锁增加生命周期复杂度。更合适的优化方向按优先级如下：

1. 降低 `TqWindowsRelayWorker::Lock_` 在 callback 热路径上的使用。可以让 callback binding 持有可安全 pin 住生命周期的 relay 引用，例如 intrusive refcount / hazard pointer / generation token，使 `StreamCallback()` 和 SEND_COMPLETE 不必每次进入 worker map 查找。收益是减少同 worker 多连接共享锁竞争；代价是回收和迟到 callback 的正确性更难验证。
2. 将 `PendingReceives` 改成 worker 独占队列。MsQuic callback 仍负责复制 buffer，但通过 IOCP operation 携带 receive view 交给 worker，由 worker 入队和 drain；或者使用 per-relay MPSC 队列加 worker 单消费者。这样可以移除 `PendingReceiveLock` 的 callback/worker 竞争。代价是 operation 数量和所有权状态更多，`ReceiveComplete()`、FIN、关闭丢弃路径需要重新梳理。
3. 去掉或弱化 `TcpRecvOpsLock`。如果把首次 `WSARecv` 也改成投递 worker operation 后由 worker 发起，则 `TcpRecvOpsFree` 可成为 worker-only freelist，正常路径不需要 mutex。收益是减少每次 TCP recv 的锁开销；代价是注册后多一次 IOCP hop，且需要保证 stop/register 竞态仍正确。
4. 将 `PendingQuicSendRetries` 变成 worker-only 队列。当前 retry 入队和重试基本都在 worker 线程，锁主要服务关闭、trace/snapshot 和防御性访问。可以用 atomic retry depth 给 snapshot 使用，队列只在 worker 内修改。收益主要在 backpressure 场景；普通成功路径收益很小。
5. 调整 snapshot 锁策略。`TqWindowsRelayRuntime::Snapshot()` 当前持 runtime 锁遍历 worker，worker snapshot 又持 worker lock 扫 active relays。可以先复制 worker 指针列表再释放 runtime lock，减少 admin metrics 与新连接注册互斥。该优化影响观测路径，不改变数据面。
6. 如果目标是吞吐而不是锁等待，优先调大或校准 `RelayWorkers`、`RelayIoSize`、QUIC flow control/ideal send buffer 和 pending receive 上限。现有锁多数是短临界区；在没有 mutex contention 证据前，网络窗口、buffer budget、压缩和 IOCP/MsQuic 调度通常更可能是瓶颈。

## 关键参数

- `RelayIoSize`：单次 WSARecv buffer 大小，也是部分解压输出 buffer 大小。
- `WindowsRelayMaxPendingQuicReceiveBytesPerRelay`：Windows QUIC receive pending 上限。
- `WindowsRelayQuicReceiveCompleteBatchBytes`：批量 `ReceiveComplete()` 阈值。
- `RelayMaxInFlightSends`：用于计算 pending buffer budget。
- `MaxBufferedQuicSendBytes` / ideal send buffer：控制 TCP read 暂停阈值。
- `RelayWorkers`：Windows relay worker 数量，由 runtime 启动时创建固定 worker 池。
