# Windows relay IOCP 事件化锁优化设计

## 背景

当前 Windows relay worker 已经使用 IOCP 统一处理 TCP overlapped completion 和大部分 MsQuic callback operation。这个线程模型天然适合把 relay 数据面状态收敛到 worker 线程内推进。

但现有实现仍有几类热点锁：

- `TqWindowsRelayWorker::Lock_`：保护 worker 级 `Relays_` map 和 `RetiredCallbacks_`。`StreamCallback()`、`QueueQuicSendCompleteFromCallback()`、`FindRelayById()`、`TryRetireRelay()`、`Snapshot()`、`RegisterRelay()`、`StopRelay()` 都可能持有它。
- `RelayContext::PendingReceiveLock`：保护 per-relay `PendingReceives`。QUIC receive callback 入队、worker drain、finish/close 清理都会持锁。
- `RelayContext::TcpRecvOpsLock`：保护 per-relay `TcpRecvOpsFree` freelist。TCP recv post 和 completion 回收都会持锁。
- `RelayContext::PendingQuicSendLock`：保护 per-relay `PendingQuicSendRetries`。主要影响 QUIC send backpressure、关闭和观测路径。
- `TqWindowsRelayRuntime::Lock_`：保护 runtime worker vector、start/stop、register 和 snapshot。

其中最值得优先处理的是 `TqWindowsRelayWorker::Lock_`。它是 worker 级共享锁；同一 worker 上所有 relay 的 MsQuic callback 和 send complete 都会争用这把锁。相比 per-relay 锁，它更容易在多连接、小包、高 callback 频率或高频 metrics snapshot 场景下成为横向扩展瓶颈。

Linux relay 的 `RelayLock` 事件化设计提供了方向：把 relay 容器的读写全部放到 worker 线程执行，外部线程只投递 control event 并等待同步结果。Windows 版本不需要新增独立 event queue；可以复用已有 IOCP 和 `PostQueuedCompletionStatus()`，把 TCP completion、MsQuic callback、register、stop、snapshot 都收敛为同一种 worker operation。

## 目标

1. 把 `Relays_`、`RetiredCallbacks_`、`PendingReceives`、`TcpRecvOpsFree`、`PendingQuicSendRetries` 的修改收敛到 Windows relay worker 线程。
2. 让 MsQuic callback 尽量只做必要拷贝和 IOCP 投递，不再持 `TqWindowsRelayWorker::Lock_` 查 `Relays_`。
3. 保持 `TqRelayStart()`、`TqRelayStop()`、`Snapshot()` 对上层的同步语义。
4. 保持 Windows receive callback 的 ownership 语义：返回 `QUIC_STATUS_PENDING` 后，后续必须由 worker 调用 `ReceiveComplete()` 或关闭丢弃时补齐 complete。
5. 保持现有关闭语义：late TCP completion、late MsQuic send complete、shutdown complete、retired callback binding 都不能引入 use-after-free、漏 complete 或 relay 泄漏。
6. 分阶段实施，每个阶段都能单独测试、单独压测、单独回滚。

## 非目标

- 不重写 TCP/QUIC 背压策略。
- 不改变压缩、解压和 relay buffer budget 的语义。
- 不改变上层 tunnel 的 OPEN、accept、server dial 和 reaper 生命周期模型。
- 不引入新的跨线程队列；Windows 版以 IOCP 作为唯一 worker event 入口。
- 不在第一版移除所有 atomic。`Closing`、`CallbackRefs`、in-flight 计数、pending byte/depth 观测值仍保留 atomic，作为跨线程可见状态和关闭判定。

## 当前机制

### Worker 级 relay 查找

当前 `TqWindowsRelayWorker` 持有：

```cpp
mutable std::mutex Lock_;
std::unordered_map<uint64_t, std::shared_ptr<RelayContext>> Relays_;
std::vector<std::shared_ptr<CallbackBinding>> RetiredCallbacks_;
```

典型查找路径：

```text
StreamCallback()
  -> lock worker->Lock_
  -> Relays_.find(binding->RelayId)
  -> 校验 relay->Callback.get() == binding
  -> unlock
  -> QueueDeferredQuicReceive() / PostCallbackOperation()

QueueQuicSendCompleteFromCallback()
  -> FindRelayById(operation->RelayId)
  -> lock Lock_
  -> Relays_.find()
  -> unlock
  -> PostCallbackOperation(QuicSendComplete)
```

这意味着每个 QUIC receive、ideal send buffer、peer abort、shutdown complete 和 send complete 都可能进入 worker 级共享锁。

### Per-relay 队列

`RelayContext` 中的数据面队列当前也带锁：

```cpp
std::mutex PendingReceiveLock;
std::list<std::shared_ptr<TqWindowsPendingQuicReceive>> PendingReceives;

std::mutex TcpRecvOpsLock;
std::vector<std::unique_ptr<IoOperation>> TcpRecvOpsFree;

std::mutex PendingQuicSendLock;
std::deque<std::unique_ptr<TqWindowsQuicSendOperation>> PendingQuicSendRetries;
```

其中 `PendingReceives` 是 QUIC->TCP 的核心热路径。MsQuic callback 复制 receive buffer 后直接入队，worker drain/finish 再出队。虽然它是 per-relay 锁，但 callback 线程和 worker 线程仍会竞争。

### IOCP operation

当前 IOCP operation 已经包含 relay shared pointer：

```cpp
struct IoOperation {
    OVERLAPPED Overlapped{};
    TqWindowsIocpOperationType Event;
    std::shared_ptr<RelayContext> Relay;
    uint64_t RelayId{0};
    ...
};
```

TCP `WSARecv` / `WSASend` completion 需要 `Relay` 持有生命周期，这是合理的，因为 Winsock completion 可能晚于关闭。callback operation 目前也持有 `Relay`，导致 callback 为了构造 operation 要先查 `Relays_`，这是 `Lock_` 热点的来源。

## 总体方案

Windows 版采用三阶段方案。

### 阶段一：观测和低风险拆锁

目标是确认热点并减少观测侧干扰，不改变核心数据面 ownership：

1. 增加 callback 到 IOCP post 的耗时、`FindRelayById()` 次数、worker `Lock_` 等待耗时、snapshot 耗时等指标。
2. 调整 runtime snapshot：先复制 worker 指针列表，再释放 `TqWindowsRelayRuntime::Lock_`，避免 metrics 阻塞新 relay 注册。
3. 调整 worker snapshot：先在 `Lock_` 内复制 active relay `shared_ptr` 列表，释放 `Lock_` 后读取 per-relay atomic 和队列统计。

阶段一不移除 `Lock_`，只为后续阶段提供基线和降低 admin snapshot 的干扰。

### 阶段二：MsQuic callback 纯投递 IOCP

目标是让 MsQuic callback 不再查 `Relays_`，从热路径移除 `TqWindowsRelayWorker::Lock_`。

核心变化：

```text
MsQuic callback
  -> 检查 binding Closing / CallbackRefs
  -> 对 RECEIVE 复制 MsQuic buffer 到 view
  -> 构造 callback operation，填 RelayId / Generation / payload
  -> PostQueuedCompletionStatus()
  -> 返回 QUIC_STATUS_PENDING 或 SUCCESS

worker IOCP thread
  -> 收到 callback operation
  -> worker-local 查 Relays_
  -> 校验 generation / binding
  -> 修改 PendingReceives / retry queue / relay 状态
```

阶段二后，callback operation 不再持有 `std::shared_ptr<RelayContext>`，只携带 `RelayId`、`Generation` 和 payload。`Relays_` 仍可暂时由 `Lock_` 保护，因为 register/stop/snapshot 还未事件化；但 callback 热路径不再进入这把锁。

### 阶段三：register、stop、snapshot 控制操作 IOCP 事件化

目标是让 `Relays_` 和 `RetiredCallbacks_` 只在 worker 线程读写，从结构上移除 `TqWindowsRelayWorker::Lock_`。

外部同步 API 变为：

```text
TqWindowsRelayRuntime::RegisterRelay()
  -> 选择 worker
  -> worker->RegisterRelay() 创建 RegisterCommand
  -> PostQueuedCompletionStatus(RegisterRelay)
  -> 等待 command.Done
  -> 返回 command.Result

TqWindowsRelayRuntime::StopRelay()
  -> PostQueuedCompletionStatus(CloseRelay)
  -> 需要同步语义时等待 command.Done

TqWindowsRelayRuntime::Snapshot()
  -> 对每个 worker 投递 Snapshot command
  -> 等待结果并聚合
```

worker 线程内执行：

```text
ProcessRegisterRelayCommand()
  -> CreateIoCompletionPort(tcpFd, workerIocp)
  -> 创建 RelayContext
  -> 分配 relay id / generation
  -> 插入 Relays_
  -> 安装 stream callback binding
  -> MaybePostTcpRecv()
  -> 完成 command

ProcessCloseRelayCommand()
  -> CloseRelay()
  -> TryRetireRelay()
  -> 完成 command

ProcessSnapshotCommand()
  -> worker-local 遍历 Relays_
  -> 生成 snapshot
  -> 完成 command
```

阶段三后，`Relays_`、`RetiredCallbacks_`、`PendingReceives`、`TcpRecvOpsFree`、`PendingQuicSendRetries` 都是 worker-owned 状态。外部线程不能直接读写这些结构。

## 阶段一详细设计

### 新增观测指标

建议新增 Windows worker 级指标：

- `WindowsRelayWorkerLockWaitNanos`：进入 `Lock_` 的累计等待时间。
- `WindowsRelayWorkerLockAcquireCount`：`Lock_` 获取次数。
- `WindowsRelayFindRelayByIdCount`：relay id 查找次数。
- `WindowsRelayCallbackDispatchNanos`：MsQuic callback 从进入到 IOCP post 完成的累计耗时。
- `WindowsRelaySnapshotBuildNanos`：worker snapshot 构建耗时。
- `WindowsRelaySnapshotActiveRelaysScanned`：snapshot 扫描 relay 数。

这些指标用于判断 `Lock_` 是否真是当前 workload 的主要热点，也用于对比阶段二和阶段三的收益。

### Runtime snapshot 拆锁

当前 runtime snapshot 持有 `TqWindowsRelayRuntime::Lock_` 遍历 `Workers_` 并调用 `worker->Snapshot()`。应改为：

```text
Snapshot()
  -> lock runtime Lock_
  -> copy raw worker pointer / shared handle list
  -> unlock
  -> 逐个 worker->Snapshot()
  -> 聚合
```

如果 `Workers_` 仍由 `std::unique_ptr` 持有，可以在 runtime 锁内复制裸指针，但必须保证 runtime stop 不会并发销毁 worker。更稳妥的后续结构是 `std::shared_ptr<TqWindowsRelayWorker>`。

### Worker snapshot 拆锁

当前 worker snapshot 在 `Lock_` 内遍历 active relay 并读取较多状态。应改为：

```text
Snapshot()
  -> 读取 worker 原子计数
  -> lock worker Lock_
  -> copy vector<shared_ptr<RelayContext>> relays
  -> unlock
  -> 遍历 relays 读取 atomic / per-relay 统计
```

阶段一仍可能读 per-relay queue size，因此可以暂时保留 per-relay 锁。阶段三后 snapshot 改成 worker command，队列 size 读取也不需要锁。

## 阶段二详细设计

### Relay generation

为每个 relay 增加 generation，用于识别迟到 operation：

```cpp
struct RelayContext {
    uint64_t Id{0};
    uint64_t Generation{0};
    ...
};

struct CallbackBinding {
    TqWindowsRelayWorker* Worker{nullptr};
    uint64_t RelayId{0};
    uint64_t Generation{0};
    std::atomic<uint32_t> CallbackRefs{0};
    std::atomic<bool> Closing{false};
};

struct IoOperation {
    TqWindowsIocpOperationType Event;
    uint64_t RelayId{0};
    uint64_t Generation{0};
    ...
};
```

当前 relay id 单进程内递增，理论上不会复用；generation 仍有价值，因为它可以把 stale 判断显式化，方便后续改成 slot/generation 或 id wrap 防护。

### Callback operation 不持 relay shared_ptr

新增 callback-only post helper：

```text
PostCallbackOperationById(type, binding, payload)
  -> 分配 IoOperation
  -> op.Event = type
  -> op.RelayId = binding->RelayId
  -> op.Generation = binding->Generation
  -> op payload = receive view / send op pointer / value
  -> PostQueuedCompletionStatus()
```

这个 helper 不调用 `FindRelayById()`，不持 `worker->Lock_`，也不设置 `op->Relay`。

worker 收到 operation 后：

```text
ResolveRelayForCallback(op)
  -> 在 worker 线程查 Relays_
  -> relay 不存在：stale drop
  -> relay.Generation != op.Generation：stale drop
  -> relay.Closing：按事件类型清理 payload
  -> 返回 relay*
```

阶段二期间，如果 `Relays_` 还未完全 worker-owned，worker 线程查 map 可以继续复用短锁或内部 helper；关键是 MsQuic callback 不再抢这把锁。

### RECEIVE callback

当前 receive callback 做：

```text
StreamCallback(RECEIVE)
  -> lock worker Lock_
  -> relay = Relays_[relayId]
  -> QueueDeferredQuicReceive(relay, stream, buffers, fin)
  -> PostCallbackOperation(RelayReceiveReady, relay)
  -> return QUIC_STATUS_PENDING
```

阶段二改为：

```text
StreamCallback(RECEIVE)
  -> if binding.Closing return SUCCESS
  -> copy buffers into TqWindowsPendingQuicReceive
  -> view.Stream = stream
  -> view.RelayId = binding.RelayId
  -> view.Generation = binding.Generation
  -> op.Event = RelayReceiveReady
  -> op.ReceiveView = view
  -> post IOCP
  -> if post ok return QUIC_STATUS_PENDING
  -> if post failed call ReceiveComplete(totalLength) 或返回 SUCCESS 前释放 ownership
```

worker 处理：

```text
ProcessRelayReceiveReady(op)
  -> relay = ResolveRelayForCallback(op)
  -> if relay missing/closing:
       CompleteRemainingReceiveOwnership(op.ReceiveView)
       stale drop
  -> push view into relay.PendingReceives
  -> update PendingQuicReceiveBytes / QueueDepth
  -> maybe StreamReceiveSetEnabled(FALSE)
  -> DrainRelayReceives(relay)
```

这样 `PendingReceives` 入队发生在 worker 线程，可以删除 `PendingReceiveLock` 的热路径使用。

### SEND_COMPLETE callback

当前 send complete callback 做：

```text
QueueQuicSendCompleteFromCallback(sendOp)
  -> FindRelayById(sendOp->RelayId)
  -> PostCallbackOperation(QuicSendComplete, relay, sendOp)
```

阶段二改为：

```text
StreamCallback(SEND_COMPLETE)
  -> sendOp = event ClientContext
  -> op.Event = QuicSendComplete
  -> op.RelayId = sendOp->RelayId
  -> op.Generation = sendOp->Generation
  -> op.Value = sendOp pointer
  -> post IOCP
  -> if post failed:
       safe delete sendOp
       increment post failed metric
```

worker 处理：

```text
ProcessQuicSendCompleteOperation(op)
  -> unique_ptr sendOp
  -> relay = ResolveRelayForCallback(op)
  -> if relay missing:
       stale drop, delete sendOp
  -> CompleteQuicSendAccounting(relay, sendOp)
  -> RetryPendingQuicSends(relay)
  -> MaybePostTcpRecv(relay)
```

如果 post failed 且没有 relay 指针，无法扣减 relay in-flight 计数。这个场景应被视为 fatal post failure：sendOp 释放后增加 worker 错误计数；后续 relay 依靠 timeout/close 清理。更稳妥的做法是阶段二保留一个 callback binding worker-local emergency queue，post failed 时设置 binding closing 并请求 abort；设计上必须把该路径作为异常处理测试覆盖。

### 其他 callback

以下 callback 都改成只投递 id/generation：

- `QUIC_STREAM_EVENT_IDEAL_SEND_BUFFER_SIZE` -> `QuicIdealSendBuffer`
- `QUIC_STREAM_EVENT_PEER_SEND_ABORTED` -> `QuicPeerSendAborted`
- `QUIC_STREAM_EVENT_PEER_RECEIVE_ABORTED` -> `QuicPeerReceiveAborted`
- `QUIC_STREAM_EVENT_SHUTDOWN_COMPLETE` -> `QuicShutdownComplete`

worker resolve 失败时统一增加 stale drop 计数。

### `PendingReceives` worker-only

阶段二后：

- `QueueDeferredQuicReceive()` 拆成 callback 侧 `BuildDeferredQuicReceiveView()` 和 worker 侧 `EnqueueDeferredQuicReceiveView()`。
- `DrainRelayReceives()`、`FinishReceiveView()`、`CompleteAllPendingQuicReceives()` 只由 worker 线程调用。
- `PendingQuicReceiveBytes` 和 `PendingQuicReceiveQueueDepth` 可以暂时保留 atomic，方便 snapshot 和关闭判定；队列本身不再需要 mutex。

## 阶段三详细设计

### 扩展 IOCP operation type

新增 control operation：

```cpp
enum class TqWindowsIocpOperationType : uint32_t {
    ...
    RegisterRelay,
    Snapshot,
    StopWorker,
};
```

`CloseRelay` 继续表示 relay stop/close command。`StopWorker` 用于 worker stop，不与 relay close 混用。

### Control command payload

同步 control API 使用 command 对象：

```cpp
struct TqWindowsRelayRegisterCommand {
    TqSocketHandle TcpFd{TqInvalidSocket};
    MsQuicStream* Stream{nullptr};
    ITqCompressor* Compressor{nullptr};
    ITqDecompressor* Decompressor{nullptr};
    TqRelayHandle* Handle{nullptr};
    TqTuningConfig Tuning;
    TqCompressAlgo CompressAlgo{TqCompressAlgo::None};
    bool Ok{false};
    std::mutex Mutex;
    std::condition_variable Cv;
    bool Done{false};
};

struct TqWindowsRelaySnapshotCommand {
    TqWindowsRelayWorkerSnapshot Result{};
    std::mutex Mutex;
    std::condition_variable Cv;
    bool Done{false};
};
```

`IoOperation` 新增：

```cpp
void* Control{nullptr};
```

caller 栈上创建 command 时，必须等待 `Done` 后才能返回，确保 worker 不再访问 command。若后续需要异步 stop，则改成 `shared_ptr` command。

### RegisterRelay 事件化

现有 register 在 caller 线程执行 `CreateIoCompletionPort(tcpFd, Iocp_)`、创建 relay、安装 stream callback、插入 `Relays_`、再 `MaybePostTcpRecv()`。

阶段三改为全部 worker 执行：

```text
RegisterRelay()
  -> 如果当前线程是 worker thread：直接 RegisterRelayLocal()
  -> 否则创建 RegisterCommand
  -> post RegisterRelay operation
  -> wait Done
  -> return Ok

RegisterRelayLocal(command)
  -> CreateIoCompletionPort(tcpFd, Iocp_)
  -> 创建 RelayContext
  -> relay.Id = NextRelayId_++
  -> relay.Generation = NextGeneration_++
  -> 创建 CallbackBinding
  -> stream->Callback = StreamCallback
  -> stream->Context = binding
  -> Relays_[relay.Id] = relay
  -> 写 public handle
  -> MaybePostTcpRecv(relay)
```

这样 `Relays_` 插入和第一笔 `WSARecv` 都在 worker 线程，`TcpRecvOpsFree` 从一开始就是 worker-owned。

### StopRelay 事件化

现有 `StopRelay()` 需要先持 `Lock_` 查 relay，再投递 `CloseRelay` operation。阶段三改为：

```text
StopRelay(relayId)
  -> post CloseRelay operation with RelayId / Generation
  -> worker resolve relay
  -> CloseRelayLocal()
```

如果外部需要同步确认 active relay 已移除，可以使用 `CloseRelayCommand` 等待完成；当前 `TqRelayStop()` 语义是第一次调用发起 stop、后续看到 `handle->Stop` 后清理 handle，因此第一版可以保持异步 close，但查找必须由 worker 执行。

### Snapshot 事件化

阶段三的 worker snapshot 不再直接持锁遍历：

```text
Snapshot()
  -> 如果当前线程是 worker thread：BuildSnapshotLocal()
  -> 否则 post Snapshot command
  -> wait Done
  -> return Result
```

`BuildSnapshotLocal()` 直接读取 `Relays_`、`PendingReceives`、`PendingQuicSendRetries`、`TcpRecvOpsFree` 等 worker-owned 结构，不需要 mutex。高频 admin metrics 如果影响 worker latency，可以在后续阶段增加 cached snapshot。

### Retire 和 callback binding

阶段三后 `RetiredCallbacks_` 也只在 worker 线程维护：

```text
TryRetireRelayLocal(relay)
  -> 检查 ActiveHandlers / QueuedWorkerOps / InFlight*
  -> binding.Closing = true
  -> stream callback 切 NoOp
  -> Relays_.erase(relay.Id)
  -> RetiredCallbacks_.push_back(binding)
  -> public handle Stop = true
  -> PruneRetiredCallbacksLocal()
```

`PruneRetiredCallbacksLocal()` 不再需要 `Lock_`，但仍必须检查 `CallbackRefs==0`。MsQuic 迟到 callback 进入后看到 `Closing=true` 应直接返回；receive callback 若已经返回 `PENDING`，对应 view 必须由 worker stale handling 调用 `ReceiveComplete()`。

## 线程边界和所有权

阶段三目标线程边界：

| 数据结构 | owner | 外部访问方式 |
|----------|-------|--------------|
| `Relays_` | worker thread | IOCP control/callback operation |
| `RetiredCallbacks_` | worker thread | IOCP retire/prune |
| `PendingReceives` | worker thread | receive callback 通过 operation 交付 view |
| `TcpRecvOpsFree` | worker thread | 只在 post/complete recv 本地访问 |
| `PendingQuicSendRetries` | worker thread | send backpressure 在 worker 内入队/重试 |
| callback binding `Closing` / `CallbackRefs` | MsQuic callback + worker | atomic |
| in-flight counters / pending byte metrics | worker + callback/snapshot 过渡期 | atomic，后续可局部降级 |

TCP overlapped operation 仍可持有 `std::shared_ptr<RelayContext>`，因为 Winsock completion 生命周期必须保守。callback operation 不应为了查 relay 而持有 `shared_ptr`；它只带 id/generation，由 worker resolve。

## 死锁防护

必须遵守以下规则：

1. worker 线程调用同步 register/snapshot helper 时，直接走 local helper，不 post 后等待自己。
2. command completion mutex 只用于写结果和 `notify_one()`，不能在持有 command mutex 时调用外部回调或 MsQuic API。
3. worker stop 时必须 fail 或 drain 所有未完成 control command，避免 caller 永久等待。
4. callback path 不允许等待 worker completion；callback 只能复制必要 payload 并 post IOCP。
5. close/retire 不得等待 `CallbackRefs` 归零；只能把 binding 放入 retired 列表，由后续 prune 清理。

## 测试策略

### 阶段一测试

- snapshot 拆锁后，active relay 数、pending bytes、active relay states 与现有测试一致。
- 高频 snapshot 与 register/stop 并发，不崩溃、不死锁。
- 新增 lock wait / callback dispatch 指标能在 admin snapshot 中看到，并且单调递增。

### 阶段二测试

- QUIC receive callback 不再通过 `FindRelayById()`，仍能把 view 交给 worker 并写入 TCP。
- receive callback post 失败时，不泄漏 MsQuic receive ownership。
- SEND_COMPLETE callback 不查 `Relays_`，worker 仍能扣减 `InFlightQuicSends` 和 `OutstandingQuicSendBytes`。
- relay close 后迟到 receive operation 会 stale drop，并调用 `ReceiveComplete()` 归还 ownership。
- relay close 后迟到 SEND_COMPLETE 会 stale drop，并释放 send operation。
- ideal buffer、peer abort、shutdown complete 的 id/generation stale handling 正确。
- `PendingReceiveLock` 从主 receive 入队/drain 路径移除后，现有 Windows relay worker unit tests 通过。

### 阶段三测试

- `RegisterRelay()` 返回前，TCP fd 已关联 IOCP、stream callback 已安装、public handle 已写入。
- register 失败时，TCP fd、stream callback、public handle、active relay 计数都正确回滚。
- `StopRelay()` 不再外部查 `Relays_`，关闭 operation 由 worker resolve。
- `Snapshot()` 通过 worker control command 返回一致结果。
- worker stop 时，正在等待 register/snapshot 的线程不会永久阻塞。
- `TcpRecvOpsFree`、`PendingReceives`、`PendingQuicSendRetries` 移除 mutex 后没有数据竞争。
- Windows loopback validation 在 compress off 和 zstd 下通过。

### 压测和观测

- 多连接小包 receive/send complete 压测，比较阶段前后 callback dispatch p95/p99。
- 高频 admin snapshot 压测，确认 snapshot 不再显著影响新连接注册和 callback post。
- 长连接大流量压测，确认吞吐不下降、pending receive bytes 和 outstanding quic send bytes 不异常增长。
- 随机 stop/abort/shutdown 压测，确认 stale drop、late completion、retired callback 无泄漏。

## 风险和缓解

| 风险 | 影响 | 缓解 |
|------|------|------|
| callback operation 不持 relay 后 post failed 无法扣减 in-flight | 计数泄漏或 relay 无法 retire | post failed 路径作为 fatal error 记录；优先保证 receive ownership；send complete post failed 增加专项测试和保守关闭策略 |
| receive stale drop 忘记 `ReceiveComplete()` | MsQuic receive flow control 泄漏 | 所有 stale receive operation 统一走 `CompleteRemainingReceiveOwnership()` |
| generation 校验缺失 | 迟到 operation 命中新 relay | 所有 callback/control operation 都带 relay id + generation；worker resolve 强制校验 |
| register 事件化改变时序 | `TqRelayStart()` 返回但 relay 尚未可用 | 同步等待 RegisterCommand 完成后才返回 |
| snapshot control command 阻塞 worker | metrics 影响转发延迟 | 阶段三先保持同步语义；必要时后续改 cached snapshot 和限频 |
| worker stop 时 command 未完成 | caller 永久等待 | stop path fail pending control command 并 notify |
| 移除 per-relay mutex 后仍有外部访问 | 数据竞争 | 编译期收敛 API，保留少量测试 helper 走 worker command；TSAN/并发单测覆盖 |
| callback binding 强引用 relay | relay 泄漏 | binding 不持强 `shared_ptr<RelayContext>`；使用 id/generation + atomic refs |

## 上线顺序

1. 合入阶段一观测和 snapshot 拆锁，建立基线。
2. 合入阶段二的 SEND_COMPLETE 纯投递，先处理最简单 payload。
3. 合入阶段二的 RECEIVE 纯投递和 worker 入队，移除 `PendingReceiveLock` 主路径使用。
4. 合入阶段三 register/stop/snapshot control command，收敛 `Relays_` 和 `RetiredCallbacks_` 到 worker。
5. 移除 `TcpRecvOpsLock`、`PendingQuicSendLock` 的主路径使用。
6. 根据压测结果决定是否删除剩余锁字段，或保留为测试/兼容过渡层。

## 验收标准

- MsQuic callback 热路径不再持有 `TqWindowsRelayWorker::Lock_`。
- `PendingReceives`、`TcpRecvOpsFree`、`PendingQuicSendRetries` 的主路径修改只发生在 worker 线程。
- register、stop、snapshot 不直接读写 worker relay 容器，而是通过 IOCP control command 执行。
- Windows relay worker 现有单元测试通过，并新增 late callback、stale operation、post failed、control command stop 的覆盖。
- Windows loopback validation 在 `Compress off` 和 `Compress zstd` 下通过。
- 多连接 callback 压测中，worker lock wait 时间和 callback dispatch p99 相比基线下降。
- 不引入 relay 泄漏、receive ownership 泄漏、late send complete use-after-free、worker stop 卡死。
