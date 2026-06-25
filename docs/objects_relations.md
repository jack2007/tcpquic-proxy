# tcpquic-proxy 内部对象关系

本文整理当前 Linux 生产 relay 路径中，QUIC callback、relay worker、event queue、per-relay data queue 之间的对象归属和数据流关系。代码入口主要在 `src/tunnel/relay.cpp`、`src/tunnel/linux_relay_worker.cpp`、`src/tunnel/linux_relay_worker.h` 和 `src/tunnel/linux_relay_event_queue.h`。

## 1. 总体层级

Linux relay 路径的对象关系可以概括为：

```text
TqLinuxRelayRuntime                  进程级单例
  └─ Workers[]                       多个 TqLinuxRelayWorker
       ├─ std::thread                每个 worker 一个线程
       ├─ epoll fd                   每个 worker 一个 epoll
       ├─ eventfd                    每个 worker 一个跨线程唤醒 fd
       ├─ TqLinuxRelayEventQueue     每个 worker 一个 event queue
       └─ Relays[]                   该 worker 拥有的多个 RelayState
            ├─ TcpFd                 一个 relay 对应一个本地 TCP fd
            ├─ MsQuicStream          一个 relay 对应一个 QUIC stream
            ├─ StreamRelayBinding    callback 到 owner worker/relay 的绑定
            ├─ TqLinuxRelayBufferPool
            ├─ PendingQuicReceives   QUIC->TCP pending receive data queue
            └─ PendingTcpWrites      QUIC->TCP zstd 解压后的 TCP write queue
```

关键点：

- `TqLinuxRelayRuntime` 是进程级单例，内部持有 `std::vector<std::unique_ptr<TqLinuxRelayWorker>> Workers`。
- `TqLinuxRelayRuntime::Start()` 按 `LinuxRelayWorkerCount` 创建多个 `TqLinuxRelayWorker`。
- `TqLinuxRelayRuntime::PickWorker()` 用 round-robin 给新 relay 选择一个 owner worker。
- 一个 relay 注册后固定归属于一个 owner worker；后续该 relay 的 TCP fd、QUIC receive callback、pending queue 都回到这个 worker。
- `TqLinuxRelayEventQueue` 不是全局共享队列，而是 `TqLinuxRelayWorker` 的成员，所以是每个 worker 一个。

## 2. 创建与注册关系

Linux 上普通 relay 由 `TqRelayStart()` 启动：

```text
TqRelayStart()
  ├─ TqApplyRelayPoolBudget()
  ├─ TqLinuxRelayRuntime::Instance().Start(tuning)
  ├─ TqLinuxRelayRuntime::Instance().PickWorker()
  ├─ worker->RegisterRelayWithId(registration)
  └─ handle 记录 LinuxWorker 和 LinuxRelayId
```

`RegisterRelayWithId()` 会在 owner worker 内创建 `RelayState`：

```text
TqLinuxRelayWorker
  └─ RegisterRelayWithId()
       ├─ make_shared<RelayState>()
       ├─ relay->Id = NextRelayId++
       ├─ Relays.push_back(relay)
       ├─ epoll_ctl(EPOLL_CTL_ADD, TcpFd)
       └─ 如果有 QUIC stream:
            ├─ new StreamRelayBinding
            ├─ binding->Worker = this
            ├─ binding->Relay = relay.get()
            ├─ relay->StreamBinding = binding
            ├─ stream->Callback = TqLinuxRelayWorker::StreamCallback
            └─ stream->Context = binding
```

这里的 `StreamRelayBinding` 是 callback 安全边界。MsQuic callback 只拿到 `stream->Context`，也就是 `StreamRelayBinding`，再从 binding 找到 owner worker 和 relay。

## 3. Worker 级 event queue

`TqLinuxRelayEventQueue` 定义在 `src/tunnel/linux_relay_event_queue.h`，是一个 bounded ring queue：

```text
TqLinuxRelayEventQueue
  ├─ Cell[] Slots
  ├─ atomic<size_t> EnqueuePos
  ├─ atomic<size_t> DequeuePos
  ├─ TryPush(TqLinuxRelayEvent&&)
  └─ TryPop(TqLinuxRelayEvent&)
```

它的归属关系是：

```text
TqLinuxRelayWorker
  └─ TqLinuxRelayEventQueue EventQueue
```

因此：

- 每个 `TqLinuxRelayWorker` 有一个独立 `EventQueue`。
- 所有 relay worker 不共用同一个 `EventQueue`。
- 属于某个 worker 的 relay，其 QUIC callback 事件会进入该 worker 自己的 `EventQueue`。
- worker 线程在 `Run()` 中被 `eventfd` 或 epoll 事件唤醒，然后通过 `DrainEvents()` 消费自己的 `EventQueue`。

event queue 承载的是跨线程控制/通知事件，不直接代表 per-relay 的长期数据 backlog。常见事件包括：

- `QuicReceiveView`
- `QuicSendComplete`
- `TcpWritable`
- `QuicIdealSendBuffer`
- `Shutdown`

## 4. Relay 级 data queue

`RelayState` 内部还有 per-relay 队列：

```text
RelayState
  ├─ std::deque<TqBufferView> PendingTcpWrites
  └─ std::deque<std::shared_ptr<TqPendingQuicReceive>> PendingQuicReceives
```

这两类队列和 `TqLinuxRelayEventQueue` 不是同一种结构：

| 队列 | 归属 | 数据结构 | 用途 |
|------|------|----------|------|
| `EventQueue` | 每个 worker 一个 | `TqLinuxRelayEventQueue` ring queue | 跨线程投递 worker 事件 |
| `PendingQuicReceives` | 每个 relay 一个 | `std::deque<std::shared_ptr<TqPendingQuicReceive>>` | 保存 QUIC->TCP 尚未完全处理的 receive view |
| `PendingTcpWrites` | 每个 relay 一个 | `std::deque<TqBufferView>` | 保存 zstd QUIC->TCP 解压后尚未写完的 TCP buffer |

`EventQueue` 是 callback 线程到 owner worker 线程的入口队列。`PendingQuicReceives` 和 `PendingTcpWrites` 是 owner worker 线程内部按 relay 维护的数据 backlog。

## 5. QUIC receive callback 到 worker 的关系

QUIC receive callback 运行在 MsQuic QUIC worker 上，不在 callback 内直接写 TCP。

```text
MsQuic QUIC worker
  └─ TqLinuxRelayWorker::StreamCallback(stream, context, event)
       └─ context -> StreamRelayBinding
            └─ binding->Worker->OnStreamEventWithBinding(...)
                 └─ QUIC_STREAM_EVENT_RECEIVE
                      ├─ QueueDeferredQuicReceive()
                      │    ├─ 构造 TqPendingQuicReceive
                      │    ├─ 保存 QUIC_BUFFER slice 指针
                      │    └─ 封装成 TqLinuxRelayEventType::QuicReceiveView
                      ├─ Enqueue(event)
                      │    ├─ ownerWorker.EventQueue.TryPush(event)
                      │    └─ Wake() 写 eventfd 唤醒 owner worker
                      └─ return QUIC_STATUS_PENDING
```

`QUIC_STATUS_PENDING` 的含义是应用暂时持有 MsQuic receive buffer ownership。后续只有当 owner worker 确认数据已经按真实进度处理后，才调用 `StreamReceiveComplete()` 释放对应 receive buffer。

## 6. Worker 消费 QUIC receive 的关系

owner worker 线程被 eventfd 唤醒后，消费自己的 event queue：

```text
TqLinuxRelayWorker::Run()
  └─ DrainEvents()
       └─ EventQueue.TryPop(event)
            └─ event.Type == QuicReceiveView
                 └─ ProcessQuicReceiveViewEvent(event)
                      ├─ FindRelayById(event.RelayId)
                      ├─ relay->PendingQuicReceiveBytes += view remaining bytes
                      ├─ relay->PendingQuicReceives.push_back(view)
                      ├─ MaybePauseQuicReceive()
                      └─ FlushDeferredQuicReceives(relay)
```

`FlushDeferredQuicReceives()` 是真正推进 QUIC->TCP 的地方：

```text
relay->PendingQuicReceives
  └─ front view
       ├─ plain:
       │    ├─ writev(TcpFd, MsQuic borrowed slices)
       │    ├─ 更新 SliceIndex/SliceOffset/CompletedLength
       │    └─ FlushDeferredReceiveCompletion()
       └─ zstd:
            ├─ DrainCompressedQuicReceiveView()
            ├─ DecompressInto(worker-owned buffer)
            ├─ relay->PendingTcpWrites.push_back(output)
            ├─ FlushTcpWrites()
            └─ 按压缩输入消费进度 StreamReceiveComplete()
```

plain 路径中，worker 直接引用 MsQuic receive slices 做 `writev()`，不复制 payload。zstd 路径中，worker 必须把解压输出写入 worker-owned buffer，再通过 `PendingTcpWrites` 写 TCP。

## 7. TCP 到 QUIC 的关系

TCP fd 由 owner worker 的 epoll 管理：

```text
Linux relay worker thread
  └─ epoll_wait()
       └─ TCP fd EPOLLIN
            └─ DrainTcpReadable(relay)
                 ├─ relay->Pool.AcquireWorker()
                 ├─ readv(TcpFd)
                 ├─ 可选 zstd 压缩
                 ├─ Stream->Send()
                 └─ 等待 QUIC SEND_COMPLETE callback
```

QUIC send complete callback 也会回到同一个 worker 的 event queue：

```text
MsQuic QUIC worker
  └─ SEND_COMPLETE callback
       ├─ event.Type = QuicSendComplete
       └─ ownerWorker.EventQueue.TryPush(event)

owner worker thread
  └─ DrainEvents()
       └─ CompleteQuicSend()
            └─ 释放 worker buffer / 推进后续 TCP read
```

因此，TCP->QUIC 和 QUIC->TCP 都以 relay 的 owner worker 为执行核心；MsQuic callback 只负责快速投递事件。

## 8. 关闭与 late callback 安全

Linux relay 停止时，`TqRelayStop()` 会通过 handle 找回 owner worker 和 relay id：

```text
TqRelayHandle
  ├─ Backend = LinuxWorker
  ├─ LinuxWorker = owner worker
  └─ LinuxRelayId = relay id

TqRelayStop()
  ├─ handle->Stop = true
  ├─ handle->LinuxWorker->UnregisterRelay(handle->LinuxRelayId)
  └─ TqRelayUnregisterActive()
```

`UnregisterRelay()` 会把 relay 从 active list 中移除，并处理 pending receive ownership：

- 标记 relay closing。
- 从 epoll 删除 TCP fd。
- 清理 `PendingTcpWrites`。
- 对 `PendingQuicReceives` 中仍持有的 MsQuic receive buffer 调用 completion。
- 解绑 `StreamRelayBinding::Relay`。
- 将 binding 放入 retired list，等待 callback ref 归零后再清理，避免 late callback 访问已释放 relay。

## 9. 关系速查

```text
Runtime : Worker = 1 : N
Worker  : EventQueue = 1 : 1
Worker  : Thread = 1 : 1
Worker  : epoll fd = 1 : 1
Worker  : eventfd = 1 : 1
Worker  : RelayState = 1 : N
RelayState : TCP fd = 1 : 0/1       sink relay 可没有 TCP fd
RelayState : MsQuicStream = 1 : 1
RelayState : StreamRelayBinding = 1 : 0/1
RelayState : PendingQuicReceives = 1 : 1 std::deque
RelayState : PendingTcpWrites = 1 : 1 std::deque
```

最容易混淆的点：

- `TqLinuxRelayEventQueue` 是 worker 级队列，不是 relay 级队列。
- `PendingQuicReceives` 是 relay 级 data queue，不是跨线程 event queue。
- receive callback 不直接消费 `PendingQuicReceives`；它先投递 `QuicReceiveView` event。
- `ProcessQuicReceiveViewEvent()` 才把 event 中的 receive view 转移到 `relay->PendingQuicReceives`。
- 所有对 TCP fd 的实际读写都在 owner relay worker 线程推进。
