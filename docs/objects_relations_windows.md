# tcpquic-proxy Windows 内部对象关系

本文整理当前 Windows relay 路径中，QUIC callback、Windows relay worker、IOCP、per-relay pending receive queue 之间的对象归属和数据流关系。结构参考 `docs/objects_relations.md`，但这里单独描述 Windows 实现。代码入口主要在 `src/tunnel/relay.cpp`、`src/tunnel/windows_relay_worker.cpp` 和 `src/tunnel/windows_relay_worker.h`。

## 1. 总体层级

Windows relay 路径的对象关系可以概括为：

```text
TqWindowsRelayRuntime                 进程级单例
  └─ Workers_[]                       多个 TqWindowsRelayWorker
       ├─ std::thread                 每个 worker 一个线程
       ├─ IOCP handle                 每个 worker 一个 IOCP
       ├─ Relays_ map                 该 worker 拥有的多个 RelayContext
       ├─ RetiredCallbacks_           late callback 安全保留区
       └─ metrics/counters
            └─ RelayContext
                 ├─ TcpFd             一个 relay 对应一个 TCP socket
                 ├─ MsQuicStream      一个 relay 对应一个 QUIC stream
                 ├─ CallbackBinding   callback 到 owner worker/relay 的绑定
                 ├─ TcpRecvBuffers    TCP->QUIC recv buffer pool
                 ├─ TcpSendBuffers    QUIC->TCP send buffer pool
                 ├─ PendingReceives   QUIC->TCP pending receive data queue
                 ├─ TcpRecvOpsFree    TCP recv IoOperation 复用池
                 └─ in-flight counters
```

关键点：

- `TqWindowsRelayRuntime` 是进程级单例，内部持有 `std::vector<std::unique_ptr<TqWindowsRelayWorker>> Workers_`。
- `TqWindowsRelayRuntime::Start(workerCount)` 创建多个 `TqWindowsRelayWorker`。
- 当前 `TqRelayStart()` 在 Windows 上调用 `Start(tuning.LinuxRelayWorkerCount)`，也就是复用同一个 worker count tuning 字段。
- `TqWindowsRelayRuntime::RegisterRelay()` 用 `NextWorker_ % Workers_.size()` 轮询选择 owner worker。
- 一个 relay 注册后固定归属于一个 owner worker；后续该 relay 的 TCP IOCP completion、QUIC receive callback、pending queue 都回到这个 worker。
- Windows 没有 `TqLinuxRelayEventQueue`。跨线程投递和 TCP IO 完成都通过每个 worker 自己的 IOCP handle 进入 worker 线程。

## 2. 创建与注册关系

Windows 上普通 relay 由 `TqRelayStart()` 启动：

```text
TqRelayStart()
  ├─ TqApplyRelayPoolBudget()
  ├─ TqWindowsRelayRuntime::Instance().Start(tuning.LinuxRelayWorkerCount)
  ├─ TqWindowsRelayRuntime::Instance().RegisterRelay(...)
  │    ├─ round-robin 选择 TqWindowsRelayWorker
  │    └─ worker->RegisterRelay(...)
  └─ handle 记录 WindowsWorker 和 WindowsRelayId
```

`TqWindowsRelayWorker::RegisterRelay()` 会在 owner worker 内创建 `RelayContext`：

```text
TqWindowsRelayWorker
  └─ RegisterRelay()
       ├─ CreateIoCompletionPort(tcpFd, worker.Iocp_)
       ├─ make_shared<RelayContext>(tuning)
       ├─ relay->Id = NextRelayId_++
       ├─ relay->TcpFd = tcpFd
       ├─ relay->Stream = stream
       ├─ relay->Callback = make_shared<CallbackBinding>()
       ├─ relay->Callback->Worker = this
       ├─ relay->Callback->RelayId = relay->Id
       ├─ relay->Callback->RelayHint = relay.get()
       ├─ stream->Callback = TqWindowsRelayWorker::StreamCallback
       ├─ stream->Context = relay->Callback.get()
       ├─ Relays_[relay->Id] = relay
       ├─ handle->Backend = WindowsWorker
       ├─ handle->WindowsWorker = this
       ├─ handle->WindowsRelayId = relay->Id
       └─ PostTcpRecv(relay)
```

这里的 `CallbackBinding` 是 callback 安全边界。MsQuic callback 只拿到 `stream->Context`，也就是 `CallbackBinding`，再从 binding 找到 owner worker 和 relay id。

## 3. Worker 级 IOCP queue

Windows worker 的跨线程入口不是自定义 ring queue，而是 IOCP：

```text
TqWindowsRelayWorker
  ├─ Iocp_ = CreateIoCompletionPort(INVALID_HANDLE_VALUE, ...)
  ├─ Thread_ = std::thread(&TqWindowsRelayWorker::Run, this)
  └─ Run()
       └─ GetQueuedCompletionStatus(Iocp_, ...)
            └─ OVERLAPPED* -> IoOperation*
```

`IoOperation` 是 Windows worker 的统一 completion/event 载体：

```text
IoOperation
  ├─ OVERLAPPED Overlapped
  ├─ TqWindowsRelayEvent Event
  ├─ shared_ptr<RelayContext> Relay
  ├─ TqBufferRef BufferOwner
  ├─ WSABUF WsaBuffer
  ├─ vector<uint8_t> Buffer
  ├─ shared_ptr<TqWindowsPendingQuicReceive> ReceiveView
  ├─ Offset
  └─ PostedLength
```

IOCP 中会出现两类来源：

- 真实 Windows socket IO completion：`WSARecv()` 和 `WSASend()` 完成后进入 IOCP。
- 应用主动投递的 worker event：`PostQueuedCompletionStatus()` 把 `IoOperation` 投递到同一个 IOCP。

常见 event 类型包括：

- `TcpRecv`
- `TcpSend`
- `QuicReceiveViewQueued`
- `QuicReceiveQueued`
- `CloseRelay`
- `StopWorker`

因此，Windows 上的“event queue”实际是每个 worker 一个 IOCP completion queue。所有 relay worker 不共用同一个 IOCP。

## 4. Relay 级 data queue

`RelayContext` 内部有 per-relay pending receive queue：

```text
RelayContext
  ├─ std::mutex PendingReceiveLock
  ├─ std::list<std::shared_ptr<TqWindowsPendingQuicReceive>> PendingReceives
  ├─ atomic<uint64_t> PendingQuicReceiveBytes
  ├─ atomic<uint64_t> PendingQuicReceiveQueueDepth
  └─ atomic<uint32_t> QueuedQuicReceives
```

它和 worker IOCP 不是同一种结构：

| 队列 | 归属 | 数据结构 | 用途 |
|------|------|----------|------|
| IOCP completion queue | 每个 Windows worker 一个 | Windows IOCP | TCP IO completion 和跨线程 worker event 投递 |
| `PendingReceives` | 每个 relay 一个 | `std::list<std::shared_ptr<TqWindowsPendingQuicReceive>>` | 保存 QUIC->TCP 尚未完全处理的 receive view |
| `TcpRecvOpsFree` | 每个 relay 一个 | `std::vector<std::unique_ptr<IoOperation>>` | TCP recv `IoOperation` 复用池 |

`PendingReceives` 由 `PendingReceiveLock` 保护，因为 QUIC callback 线程会把 view 放入队列，worker 线程会在完成 view 时移除它。

## 5. QUIC receive callback 到 worker 的关系

QUIC receive callback 运行在 MsQuic QUIC worker 上，不在 callback 内直接做阻塞 TCP 写。

```text
MsQuic QUIC worker
  └─ TqWindowsRelayWorker::StreamCallback(stream, context, event)
       └─ context -> CallbackBinding
            ├─ CallbackRefs++
            ├─ 通过 binding->RelayId 在 worker->Relays_ 中找 RelayContext
            └─ QUIC_STREAM_EVENT_RECEIVE
                 ├─ QueueDeferredQuicReceive(relay, stream, buffers, fin)
                 │    ├─ 构造 TqWindowsPendingQuicReceive
                 │    ├─ 保存 QUIC_BUFFER slice 指针
                 │    ├─ 更新 PendingQuicReceiveBytes / QueueDepth
                 │    ├─ relay->PendingReceives.push_back(view)
                 │    ├─ relay->QueuedQuicReceives++
                 │    └─ PostQueuedCompletionStatus(Iocp_, QuicReceiveViewQueued)
                 └─ return QUIC_STATUS_PENDING
```

`QUIC_STATUS_PENDING` 的含义和 Linux 一样：应用暂时持有 MsQuic receive buffer ownership。Windows worker 后续按真实处理进度调用 `ReceiveComplete()`。

## 6. Worker 消费 QUIC receive 的关系

owner worker 线程从 IOCP 收到 `QuicReceiveViewQueued` 后处理：

```text
TqWindowsRelayWorker::Run()
  └─ GetQueuedCompletionStatus()
       └─ op->Event == QuicReceiveViewQueued
            └─ HandleQuicReceiveViewQueued(op)
                 ├─ relay closing? -> CompletePendingQuicReceive()
                 ├─ zstd? -> PostTcpSendFromCompressedReceiveView()
                 └─ plain -> PostTcpSendFromReceiveView()
```

plain QUIC->TCP 路径：

```text
PostTcpSendFromReceiveView(relay, view)
  ├─ 找到 view 当前 SliceIndex/SliceOffset
  ├─ IoOperation.ReceiveView = view
  ├─ WSABUF 指向 MsQuic borrowed slice
  └─ WSASend(TcpFd, OVERLAPPED)

IOCP TcpSend completion
  └─ HandleTcpSend(op, bytes)
       ├─ AdvanceReceiveView(relay, view, bytes)
       ├─ FlushDeferredReceiveCompletion(view, force?)
       ├─ partial? -> 继续 WSASend 剩余部分
       └─ view complete? -> FinishReceiveView()
```

zstd QUIC->TCP 路径：

```text
PostTcpSendFromCompressedReceiveView(relay, view)
  ├─ 从 view 当前 slice 取压缩输入
  ├─ relay->TcpSendBuffers.AcquireWorker()
  ├─ DecompressInto(worker-owned buffer)
  ├─ AdvanceReceiveView(relay, view, InputConsumed)
  ├─ IoOperation.BufferOwner = 解压输出 buffer
  └─ WSASend(TcpFd, OVERLAPPED)

IOCP TcpSend completion
  └─ HandleTcpSend(op, bytes)
       ├─ partial? -> 继续发送同一个 output buffer 剩余部分
       └─ output buffer 完成后继续 PostTcpSendFromCompressedReceiveView()
```

与 Linux 一样，plain 路径不复制 payload，直接引用 MsQuic receive slices。zstd 路径必须使用 worker-owned TCP send buffer 保存解压输出。

## 7. TCP 到 QUIC 的关系

TCP socket 关联到 owner worker 的 IOCP 后，worker 通过 overlapped `WSARecv()` 推进 TCP->QUIC：

```text
TqWindowsRelayWorker::RegisterRelay()
  └─ PostTcpRecv(relay)
       ├─ relay->TcpRecvBuffers.Acquire()
       └─ WSARecv(TcpFd, OVERLAPPED)

IOCP TcpRecv completion
  └─ HandleTcpRecv(op, bytes)
       ├─ bytes == 0? -> TCP recv closed / close-after-drained
       ├─ 可选 zstd 压缩
       ├─ Stream->Send()
       └─ 等待 QUIC SEND_COMPLETE callback
```

QUIC send complete callback 不通过 `PostQueuedCompletionStatus()` 重新排队；它在 callback 中释放/复用对应 `IoOperation`，并在需要时直接调用 `PostTcpRecv(relay)` 继续投递下一次 TCP recv：

```text
MsQuic QUIC worker
  └─ SEND_COMPLETE callback
       ├─ client context -> IoOperation*
       ├─ relay->InFlightQuicSends--
       ├─ completed->BufferOwner.reset()
       ├─ completed 放回 relay->TcpRecvOpsFree
       └─ worker->PostTcpRecv(relay)
```

所以 Windows 的 TCP->QUIC 方向是 IOCP recv completion 驱动，QUIC send complete 负责释放 buffer 并续投 TCP recv。

## 8. 关闭与 late callback 安全

Windows relay 停止时，`TqRelayStop()` 会通过 handle 找回 owner worker 和 relay id：

```text
TqRelayHandle
  ├─ Backend = WindowsWorker
  ├─ WindowsWorker = owner worker
  └─ WindowsRelayId = relay id

TqRelayStop()
  ├─ TqWindowsRelayRuntime::StopRelay(handle)
  │    └─ handle->WindowsWorker->StopRelay(handle->WindowsRelayId)
  ├─ handle->Backend = None
  ├─ handle->WindowsWorker = nullptr
  ├─ handle->WindowsRelayId = 0
  ├─ handle->Stop = true
  └─ TqRelayUnregisterActive()
```

`TqWindowsRelayWorker::StopRelay()` 不直接在调用线程清理 relay，而是投递 `CloseRelay` 到 owner worker 的 IOCP：

```text
StopRelay(relayId)
  ├─ Relays_[relayId] -> shared_ptr<RelayContext>
  ├─ op->Event = CloseRelay
  ├─ op->Relay = relay
  └─ PostQueuedCompletionStatus(Iocp_, op)
```

`CloseRelay()` 会做实际清理：

- `Closing.exchange(true)` 防止重复 close。
- 关闭 TCP socket。
- 标记 `CallbackBinding::Closing = true`。
- `CallbackBinding::RelayHint = nullptr`。
- `CompleteAllPendingQuicReceives()` 释放仍 pending 的 MsQuic receive ownership。
- 如果 stream 仍指向当前 callback binding，则把 stream callback/context 改回 no-op/null。
- 从 `Relays_` 移除 relay。
- 把 callback binding 放入 `RetiredCallbacks_`，等待 callback refs 归零后再清理。
- 设置 public handle 的 `Stop = true`。

`CallbackRefs` 和 `RetiredCallbacks_` 的作用是避免 late callback 访问已释放的 relay 或 binding。

## 9. 关系速查

```text
Runtime : Worker = 1 : N
Worker  : IOCP handle = 1 : 1
Worker  : Thread = 1 : 1
Worker  : RelayContext = 1 : N
Worker  : RetiredCallbacks_ = 1 : 1
RelayContext : TCP socket = 1 : 1
RelayContext : MsQuicStream = 1 : 1
RelayContext : CallbackBinding = 1 : 1 shared_ptr
RelayContext : PendingReceives = 1 : 1 std::list
RelayContext : TcpRecvBuffers = 1 : 1
RelayContext : TcpSendBuffers = 1 : 1
RelayContext : TcpRecvOpsFree = 1 : 1 std::vector
```

最容易混淆的点：

- Windows 没有 `TqLinuxRelayEventQueue`，worker 级事件和 TCP IO completion 都走 IOCP。
- 每个 `TqWindowsRelayWorker` 有自己的 IOCP，不是所有 worker 共用一个 IOCP。
- `PendingReceives` 是 relay 级 data queue，不是 IOCP queue。
- receive callback 会先把 view 放进 `relay->PendingReceives`，再投递 `QuicReceiveViewQueued` 到 owner worker 的 IOCP。
- `HandleQuicReceiveViewQueued()` 才开始把 pending receive view 转成 TCP `WSASend()`。
- plain QUIC->TCP 的 `WSASend()` buffer 指向 MsQuic receive slice；zstd 路径的 `WSASend()` buffer 指向 worker-owned `TcpSendBuffers`。
- TCP->QUIC 的 SEND_COMPLETE callback 当前直接续投 `PostTcpRecv()`，不是再投递一个 IOCP worker event。
