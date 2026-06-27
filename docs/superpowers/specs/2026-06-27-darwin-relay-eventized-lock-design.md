# macOS relay 热点锁事件化设计

## 背景

当前 macOS/Darwin relay worker 已经使用固定 worker 线程、kqueue fd 和 `TqDarwinRelayEventQueue` 处理 TCP readiness 与跨线程 relay 事件。这个模型天然适合把 relay 数据面状态收敛到 worker 线程内推进。

但现有实现仍有几类热点同步：

- `RelayState::Mutex`：保护单条 relay 的 pending 队列、backpressure 标志、in-flight send 计数、stream/binding 指针和关闭状态。它是连接级锁，但 TCP read/write、QUIC receive callback、SEND_COMPLETE callback、snapshot 和 stop 都会访问，实际进入报文转发热路径。
- `TqDarwinRelayWorker::RelayMutex`：保护 worker 级 `Relays`、`RetiredRelays`、`RetiredStreamBindings` 和 `KnownSendOperations`。macOS active relay 查找已经是 `unordered_map`，不存在 Linux 旧实现的线性扫描问题，但每个 kqueue TCP event、QUIC receive callback、send complete 和 snapshot 仍可能进入这把 worker 级共享锁。
- `CompletionState::Mutex`：保护 binding 侧 send operation 表，用于 stream 退休后的迟到 SEND_COMPLETE 兜底。当前每个 QUIC send operation 注册/注销都会更新 worker map 和 binding map。
- `TqDarwinRelayEventQueue`：无锁 ring buffer，使用 atomic CAS 和 per-cell sequence。它不是 mutex，但如果更多 callback 路径事件化，队列容量、CAS 竞争和 kqueue wake 频率会成为新的边界。

Linux relay `RelayLock` 事件化设计提供了方向：把 relay 容器和热状态的读写全部放到 worker 线程执行，外部线程只投递事件并等待同步结果。macOS 与 Linux 的差异是：macOS active map 已经 O(1)，所以无需先做索引阶段；macOS 的优先目标是移除 `RelayState::Mutex` 和 `RelayMutex` 对数据面热路径的影响。

## 目标

1. 把 macOS relay 热数据结构的写入收敛到 Darwin relay worker 线程：pending receive/write/send 队列、pending bytes、in-flight send、TCP/QUIC backpressure、active/retired relay 容器和 send operation tracking。
2. 让 MsQuic callback 尽量只做 binding 原子检查、构造事件、入队和 wake，不再修改 `RelayState` 队列/计数，不再查 `Relays` map。
3. 保持 `TqRelayStart()`、`TqRelayStop()`、`TqDarwinRelayWorker::Snapshot()` 的同步语义；API 返回时应能观察到与当前实现等价的注册、注销或 snapshot 结果。
4. 保持 MsQuic receive ownership 语义：callback 返回 `QUIC_STATUS_PENDING` 后，后续必须由 worker `ReceiveComplete()`，关闭丢弃时也必须补齐 complete。
5. 保持 late SEND_COMPLETE、late receive event、stream abort/shutdown、retired binding 和 worker stop 的安全语义，不引入 use-after-free、operation 泄漏、receive buffer 泄漏或 relay 泄漏。
6. 分 4 个阶段实施，每个阶段都能单独测试、单独上线、单独回滚。

## 非目标

- 不重写 TCP/QUIC 背压策略，只改变背压状态的线程所有权。
- 不改变压缩、解压、relay buffer budget 和 MsQuic receive pending 语义。
- 不改变上层 tunnel OPEN、server dial、client ingress 和 reaper 生命周期模型。
- 不在第一版移除所有 atomic。`StreamBinding::Active`、`Worker`、`CallbackRefs`、`RetiredStream`、callback 入口预算和 event queue CAS 仍然是必要跨线程同步。
- 不把 `TqDarwinRelayEventQueue` 改成多 shard；如果 4 阶段后 event queue 成为瓶颈，再另开设计。

## 当前机制

### QUIC receive path

当前 receive callback 路径：

```text
MsQuic callback
  -> binding CallbackRefs++
  -> worker->FindRelay(binding->RelayId)       // RelayMutex
  -> QueueDeferredQuicReceive()
      -> relay->Mutex 检查 Closing、读取 Id
      -> 复制 buffer view 元数据
      -> relay->Mutex 增加 PendingQuicReceiveBytes
      -> EventQueue.TryPush(QuicReceiveView)
      -> Wake()
  -> return QUIC_STATUS_PENDING
```

worker 出队后再：

```text
ProcessQuicReceiveViewEvent()
  -> FindRelay(receive->RelayId)               // RelayMutex
  -> relay->Mutex push PendingQuicReceives
  -> MaybePauseQuicReceive()
  -> EnqueueQuicReceiveForTcp()
  -> FlushTcpWrites()
  -> CompleteDeferredQuicReceive()
```

问题是 callback 已经在 worker 之外修改了 `RelayState` pending bytes，并查了 `Relays` map。worker 线程和 MsQuic callback 线程会争用 `RelayState::Mutex` 和 `RelayMutex`。

### QUIC send complete path

当前 SEND_COMPLETE callback 路径：

```text
MsQuic callback
  -> IsKnownSendOperation(operation)           // RelayMutex
  -> CompleteQuicSend(operation)
      -> UnregisterKnownSendOperation()        // RelayMutex + CompletionState::Mutex
      -> FindRelay() / FindRetiredRelay()      // RelayMutex
      -> relay->Mutex 减少 InFlightQuicSends/InFlightQuicSendBytes
      -> RetryPendingQuicSends()
      -> SetTcpReadBackpressure(false)
```

这条路径在 callback 线程直接推进数据面状态。同步 SEND_COMPLETE、密集小 send、或者 stream 关闭时，callback 会与 worker TCP read/write 竞争。

### control path

`RegisterRelayWithId()`、`UnregisterRelay()` 和 `Snapshot()` 直接从调用线程操作 worker 状态：

```text
RegisterRelayWithId()
  -> LifecycleMutex
  -> 设置 fd non-blocking / SO_NOSIGPIPE
  -> relay->Mutex 初始化 TcpReadArmed
  -> RelayMutex 插入 Relays / 分配 id
  -> kevent EV_ADD
  -> 安装 stream callback

UnregisterRelay()
  -> LifecycleMutex + RelayMutex 移除 Relays
  -> RemoveTcpFilters()
  -> RetireRelay()

Snapshot()
  -> RelayMutex 遍历 Relays
  -> 每个 relay->Mutex 读取队列和计数
```

这些操作不是每包执行，但它们让 relay 容器和 relay 状态保持跨线程共享，阻止热路径完全 worker-local 化。

## 总体方案

macOS 采用 4 阶段方案。

### 阶段一：QUIC receive callback 纯投递

目标是先移除 `RelayState::Mutex` 在 QUIC receive callback 入口的热路径使用。

receive callback 改为：

```text
StreamCallback(RECEIVE)
  -> binding 原子检查 Active/Worker
  -> binding 级 pending receive budget 原子 reserve
  -> 构造 TqDarwinPendingQuicReceive
  -> EventQueue.TryPush(QuicReceiveView)
  -> Wake()
  -> return QUIC_STATUS_PENDING
```

worker 线程处理：

```text
ProcessQuicReceiveViewEvent()
  -> worker-local FindRelayLocal(receive->RelayId)
  -> 把 reserved bytes 从 binding budget 转入 relay PendingQuicReceiveBytes
  -> push PendingQuicReceives
  -> MaybePauseQuicReceive()
  -> EnqueueQuicReceiveForTcp()
  -> FlushTcpWrites()
```

新增 binding 级原子预算是阶段一的关键。callback 不再直接加 `relay->PendingQuicReceiveBytes`，但仍必须在返回 pending 前有内存上限。预算可以放在 `StreamBinding`：

```cpp
std::atomic<uint64_t> CallbackPendingReceiveBytes{0};
std::atomic<uint64_t> CallbackPendingReceiveEvents{0};
```

入队成功后，worker 在处理或丢弃 receive 时扣减该预算。入队失败时 callback 不返回 pending，直接返回 success 并不保存 MsQuic buffer；如果已经 reserve，必须先回滚 reserve。

### 阶段二：SEND_COMPLETE 和 QUIC control callback 事件化

目标是让 MsQuic callback 不再直接调用 `CompleteQuicSend()`、`CloseRelay()` 或 `FindRelay()`。

SEND_COMPLETE callback 改为：

```text
StreamCallback(SEND_COMPLETE)
  -> binding 原子检查 Worker
  -> EventQueue.TryPush(QuicSendComplete)
  -> Wake()
  -> return QUIC_STATUS_SUCCESS
```

worker 线程处理：

```text
ProcessQuicSendCompleteEvent()
  -> worker-local KnownSendOperations erase
  -> worker-local FindRelayLocal / FindRetiredRelayLocal
  -> 更新 InFlightQuicSends/InFlightQuicSendBytes
  -> RetryPendingQuicSends()
  -> MaybeResumeTcpRead()
  -> PurgeRetiredRelaysIfSafeLocal()
```

peer abort、peer receive abort、shutdown complete、ideal send buffer 也投递事件，由 worker 线程执行 close/retry。这样 callback 线程只消耗 binding 原子和 event queue，不再持 `RelayMutex` 或 `RelayState::Mutex`。

阶段二仍保留 detached completion 兜底：如果 worker 已经为空、event queue 不可用或 binding 已退休且 operation 不在 worker-local known table，callback 可以通过 `CompletionState` 释放 operation。这个兜底只用于关闭异常路径，不作为正常热路径。

### 阶段三：register、unregister、snapshot 控制操作事件化

目标是让 active/retired relay 容器只在 worker 线程读写。

新增 control command：

```cpp
struct RegisterRelayCommand {
    TqDarwinRelayRegistration Registration;
    TqDarwinRelayRegistrationResult Result;
    std::mutex Mutex;
    std::condition_variable Cv;
    bool Done{false};
};

struct UnregisterRelayCommand {
    uint64_t RelayId{0};
    std::mutex Mutex;
    std::condition_variable Cv;
    bool Done{false};
};

struct SnapshotCommand {
    TqDarwinRelayWorkerSnapshot Result;
    std::mutex Mutex;
    std::condition_variable Cv;
    bool Done{false};
};
```

外部 API 保持同步：

```text
RegisterRelayWithId()
  -> 如果当前是 worker 线程，直接 RegisterRelayWithIdLocal()
  -> 否则 enqueue RegisterRelay control event
  -> Wake()
  -> 等待 Done
  -> 返回 Result

UnregisterRelay()
  -> 如果当前是 worker 线程，直接 UnregisterRelayLocal()
  -> 否则 enqueue UnregisterRelay control event
  -> Wake()
  -> 等待 Done

Snapshot()
  -> 如果当前是 worker 线程，直接 SnapshotLocal()
  -> 否则 enqueue Snapshot control event
  -> Wake()
  -> 等待 Result
```

worker local helper 负责：

- 分配 relay id。
- 插入/移除 `Relays`。
- 注册/删除 kqueue read/write filters。
- 安装/清理 stream callback。
- active 到 retired 的移动和 retired purge。
- 构建 snapshot。

`RegisterRelayWithId()` 返回前必须保证 TCP fd 已注册到 kqueue、stream callback 已安装、public handle 已填好。`UnregisterRelay()` 返回前必须保证 active relay 已移除、fd 已关闭或已进入 retired 清理路径。

### 阶段四：收缩热点锁并切换 worker-local 状态

目标是把阶段一到三留下的过渡锁收缩到冷路径或删除。

预期结果：

- `Relays`、`RetiredRelays`、`RetiredStreamBindings` 和 worker-local known send operation 表只在 worker 线程访问，热路径不再需要 `RelayMutex`。
- `RelayState` 数据面字段只在 worker 线程写入，热路径不再需要 `RelayState::Mutex`。
- `RelayState::Mutex` 若保留，只用于 stop/destructor 的过渡保护或测试 helper；最终可以在后续清理中删除。
- `CompletionState::Mutex` 只保留 detached completion 兜底。正常 SEND_COMPLETE 由 worker 事件处理，不再每个 send batch 进入 completion map。
- snapshot 使用 worker-local 构建或 cached snapshot，避免 metrics 线程扫描 relay 队列时阻塞转发。

阶段四后，macOS relay 数据面同步边界变成：

```text
MsQuic callback thread
  -> StreamBinding atomics
  -> EventQueue CAS
  -> kqueue wake

Darwin relay worker thread
  -> worker-local relay map / relay state / pending queues / backpressure
```

这不是完全无同步，但热点数据结构不再由多线程加锁访问。

## 线程与生命周期规则

1. worker 线程是 relay 数据面的唯一写者。
2. callback 线程不能直接访问 `RelayState` pending 队列、pending bytes、in-flight send 和 backpressure 字段。
3. callback 线程可以访问 `StreamBinding` 原子字段和 callback-local command/event payload。
4. control command 的生命周期必须覆盖 worker 访问。同步 command 可以由 caller 栈上创建，worker signal `Done` 后不再访问；异步 command 必须使用 `shared_ptr`。
5. 如果 public API 在 worker 线程内被调用，必须走 local helper，不能 enqueue 后等待自己处理。
6. worker stop 必须 fail 或 drain 所有未完成 control command，避免调用方永久等待。
7. receive callback 返回 pending 后，无论 relay 正常写出、关闭、丢弃还是 worker stop，都必须最终 `ReceiveComplete()`。
8. send operation 提交后，无论 SEND_COMPLETE 正常到达、stream retired、worker stop 还是 callback 迟到，都必须最终释放 operation。

## 事件类型变化

扩展 `TqDarwinRelayEventType`：

```cpp
QuicSendComplete,
QuicIdealSendBuffer,
QuicPeerSendAborted,
QuicPeerReceiveAborted,
QuicShutdownComplete,
RegisterRelay,
UnregisterRelay,
Snapshot,
StopWorker,
```

`Shutdown` 当前表示 worker shutdown；设计中建议明确改名或新增 `StopWorker`，避免和 relay unregister 语义混淆。为了降低改动，第一版可以保留 `Shutdown`，但新增 control event 时必须在注释中说明它是 worker stop。

`TqDarwinRelayEvent` 增加 control pointer：

```cpp
void* Control{nullptr};
```

如果未来 control event 需要跨异步生命周期，可以把 `Control` 改为 `std::shared_ptr<void>`，但第一版同步 command 用栈对象加等待即可。

## 失败处理

### receive callback 入队失败

如果 receive callback 在 reserve budget 后入队失败：

1. 回滚 binding receive budget。
2. 增加 `Errors`。
3. 返回 `QUIC_STATUS_SUCCESS`，让 MsQuic 不进入 pending ownership。
4. 投递失败时若 worker 仍可用，可尝试投递轻量 close event；投递 close 也失败时，只标记 binding inactive，并依赖后续 stop/shutdown 清理。

### send complete 入队失败

SEND_COMPLETE callback 不能丢 operation。入队失败时：

1. 尝试 detached completion 兜底。
2. 如果 detached completion 不命中，并且 worker 仍可用，记录 `Errors` 并直接执行当前保守路径或同步调用 local completion helper。
3. 不允许静默泄漏 operation。

### control command 入队失败

- register 入队失败：同步返回 `{Ok=false}`，不得修改 handle。
- unregister 入队失败：保守设置 handle stop，由调用方后续 stop/reaper 再次尝试；同时记录 `Errors`。
- snapshot 入队失败：返回空 snapshot，并记录 `Errors`。

### worker stop

worker stop 时必须：

- 唤醒 worker。
- drain 或 fail control command。
- 对 active relay 执行 local unregister/retire。
- 对 pending receive 执行 discard complete。
- 等待 known operations drain 或转 detached completion。
- 清理 retired stream callback。

## 测试策略

### 阶段一测试

- receive callback 返回 pending 后，worker 处理 receive view 并完成 `ReceiveComplete()`。
- receive callback 入队失败时不返回 pending，binding budget 回滚。
- pending receive bytes 由 worker 出队后进入 relay snapshot。
- QUIC receive backpressure pause/resume 与当前行为一致。
- TCP 写阻塞时 pending receive 不丢失，后续 writable 能完成 receive。

### 阶段二测试

- synchronous SEND_COMPLETE 发生在 `Stream->Send()` 返回前时，operation 不泄漏，in-flight 计数正确归零。
- normal SEND_COMPLETE callback 只投递事件，worker 处理后恢复 TCP read。
- ideal send buffer、peer abort、shutdown complete 由 worker 处理，不在 callback 中直接 close relay。
- retired stream 的 late SEND_COMPLETE 能通过 detached completion 或 worker retired lookup 释放 operation。
- event queue 满时 send complete 不泄漏。

### 阶段三测试

- `RegisterRelayWithId()` 返回前 active relay 可在 snapshot 中观察到，fd filters 和 stream callback 已安装。
- `UnregisterRelay()` 返回前 active relay 已移除，fd 关闭，handle 清空。
- `Snapshot()` 通过 control event 返回一致数据。
- 从 worker 线程内部触发 unregister/snapshot 不死锁。
- worker stop 时 pending register/unregister/snapshot command 不永久等待。

### 阶段四测试

- 代码扫描确认数据面路径不再持 `RelayState::Mutex` 和 `RelayMutex`。
- 高并发 receive/send complete 测试通过。
- metrics snapshot 高频运行时不破坏转发。
- relay close、late callback、retired purge、ReceiveComplete、send operation release 全部通过现有 IO 测试。

## 验收标准

- 阶段一后，MsQuic receive callback 不再修改 `RelayState` pending 队列或 pending bytes。
- 阶段二后，正常 SEND_COMPLETE、ideal buffer、peer abort、shutdown complete 不再直接在 callback 线程修改 relay 状态。
- 阶段三后，register、unregister、snapshot 对 relay 容器的读写都在 worker 线程执行。
- 阶段四后，macOS relay 数据面热路径不再依赖 `RelayState::Mutex` 或 `RelayMutex`。
- 所有阶段后，`tcpquic_darwin_relay_worker_queue_test`、`tcpquic_darwin_relay_worker_io_test`、`tcpquic_darwin_relay_metrics_test` 通过。
- 不引入 receive buffer 泄漏、send operation 泄漏、relay 泄漏、late callback use-after-free 或 worker stop deadlock。

## 上线顺序

1. 合入阶段一，压测 QUIC->TCP receive 小包场景，观察 callback CPU 和 `PendingQuicReceiveBytes` 行为。
2. 合入阶段二，压测 TCP->QUIC 小包和同步 SEND_COMPLETE，观察 send complete CPU 和 in-flight 计数。
3. 合入阶段三，压测短连接 register/unregister/snapshot 并发，观察 control command 延迟。
4. 合入阶段四，运行全量 relay 测试和长时间 soak，确认锁热点从 CPU profile 中消失或显著下降。
