# Darwin relay `RelayState::Mutex` + `RelayMutex` 热路径出清设计

## 背景

`docs/relay_macos.md` 把 macOS relay 转发路径中热度最高的前两把锁列为：

- `RelayState::Mutex`：单 relay 锁，当前覆盖 TCP/QUIC 数据面状态、半关闭状态、pending 队列、压缩缓冲和 snapshot 读取。
- `TqDarwinRelayWorker::RelayMutex`：单 worker 全局 relay map 锁，当前覆盖 `FindRelay()`、register/unregister、retire/purge、snapshot 和 callback 查找。

这两把锁应作为同一阶段处理。`RelayState::Mutex` 的核心优化是把 active relay 数据面收敛为 worker-thread-only；但如果 worker 数据面仍通过 `FindRelay()` 取 relay，就会继续打到 `RelayMutex`。反过来，如果 callback 为了避开 `RelayMutex` 直接改 `RelayState` 字段，又会保留 `RelayState::Mutex` 热点和跨线程状态写入。因此本设计目标不是一次性删除两把锁，而是把它们从稳定转发热路径中移走。

## 目标

1. worker 线程数据面路径不再持有 `RelayState::Mutex`。
2. worker 线程数据面路径不再调用需要 `RelayMutex` 的 `FindRelay()`，统一使用 `FindRelayLocal()` 或已持有的 relay shared_ptr。
3. MsQuic callback 不直接修改 `RelayState` 数据面字段，也不通过 `RelayMutex` 查 worker map；callback 只读取 `StreamBinding` 上的原子状态和稳定 relay 标识，并投递事件。
4. 运行中 `Snapshot()` 保持事件化，由 worker 线程构造快照；`SnapshotLocal()` 不再逐条加 `RelayState::Mutex`。
5. register/unregister/retire/purge 等生命周期路径继续允许使用 `RelayMutex`；非 worker direct unregister 的语义暂不重写。
6. 不改变 TCP/QUIC 转发语义、背压阈值、pending receive ownership、send complete 释放规则和 public metrics 字段。

## 非目标

- 不删除 `RelayMutex` 本身；本阶段只把它从热路径中移走。
- 不在本阶段重写 `KnownSendMutex` 或 `CompletionState::Mutex`。这两把锁属于下一组 send completion 生命周期重构。
- 不改 event queue 的 wake/coalesce 策略。
- 不实现 Darwin receive sink。
- 不调整 Linux/Windows relay 行为。

## 方案选择

推荐方案：两把锁同阶段、分步骤出清热路径。

第一步先建立 worker-only 访问边界和测试护栏；第二步把 worker kqueue/event queue 路径改为 `FindRelayLocal()`；第三步把 callback 的 relay map lookup 去掉，让 `StreamBinding` 承载 callback 所需的稳定信息；第四步移除 worker 数据面上的 `RelayState::Mutex`；最后审计剩余锁点，只允许生命周期和 fallback 路径保留。

不推荐把前四把锁一次解决。`KnownSendMutex` 和 `CompletionState::Mutex` 共同维护 send operation 生命周期，和 relay map/data-plane ownership 是另一个风险面。把四把锁一起改会同时触碰 relay lifecycle、callback ownership、send completion、late callback 和 stop/unregister fallback，问题定位成本过高。

也不推荐完全逐锁处理。`RelayState::Mutex` 和 `RelayMutex` 在 worker lookup、callback receive、abort/shutdown、snapshot 上天然耦合，硬拆会制造过渡状态和返工。

## 设计

### RelayState 线程所有权

`RelayState` 按访问来源分三类。

worker-only 字段只允许 worker 线程读写：

- `TcpReadArmed`、`TcpWriteArmed`、`TcpReadPausedByQuicBacklog`
- `TcpReadClosed`、`TcpWriteClosed`、`QuicSendClosed`、`QuicReceiveClosed`
- `QuicSendFinSubmitted`、`QuicSendFinCompleted`
- `SubmittingQuicSends`、`InFlightQuicSends`、`InFlightQuicSendBytes`
- `TcpReadBytes`、`TcpWriteBytes`
- `PendingQuicReceives`、`PendingQuicReceiveBytes`、`QuicReceivePaused`
- `PendingTcpWrites`、`PendingTcpWriteBytes`、`TcpWriteShutdownQueued`
- `PendingQuicSends`、`TcpReadBuffers`
- `CompressionOutput`、`DecompressionOutput`

lifecycle 字段可在外部线程边界读取或修改，但必须集中在 lifecycle helper 或 close/retire/unregister 路径：

- `Closing`
- `Stream`
- `PublicHandle`
- `Binding`

immutable-after-register 字段在 `RegisterRelayWithIdLocal()` 建好后不再改：

- `Id`
- `TcpFd`
- `Compressor`、`Decompressor`、`CompressAlgo`
- `EnableQuicSends`

新增 helper：

```cpp
void TqDarwinRelayWorker::AssertWorkerThreadForRelayState() const;
bool TqDarwinRelayWorker::IsRelayClosingLocal(const RelayState& relay) const;
void TqDarwinRelayWorker::MarkRelayClosingLocal(RelayState& relay) const;
MsQuicStream* TqDarwinRelayWorker::RelayStreamLocal(const RelayState& relay) const;
```

这些 helper 在 testing/debug 构建中断言 worker 线程，release 构建保持低成本。

### RelayMutex 热路径边界

`RelayMutex` 继续保护 `Relays` 和 `RetiredRelays` 的结构性修改：

- register 分配 relay id 并插入 map
- unregister 从 active map 移除 relay
- retire/purge 管理 retired relay
- stop 清空 active relay
- fallback 查询 retired relay

以下路径不得调用 `FindRelay()`：

- `ProcessTcpEvents()`
- `DrainEvents()` 中所有 worker-thread event handler
- `DrainEventsLocalOnly()` 中所有 local event handler
- `SnapshotLocal()` 运行在 worker 线程时的 active relay 遍历
- `StreamCallback()` 的 RECEIVE、peer abort、peer receive abort、shutdown complete

worker 线程路径改用 `FindRelayLocal()`。callback 路径通过 `StreamBinding` 拿到必要信息，避免 map lookup。

### StreamBinding callback 数据

`StreamBinding` 增加弱 relay 引用或等价的 callback-safe 句柄：

```cpp
std::weak_ptr<RelayState> Relay;
```

`RegisterRelayWithIdLocal()` 创建 relay 和 binding 后，在同一初始化阶段设置：

```cpp
binding->RelayId = relay->Id;
binding->Relay = relay;
relay->Binding = binding;
```

callback 使用规则：

- RECEIVE callback 不调用 `worker->FindRelay()`。
- 若只需要 relay id 和 stream ownership，使用 `binding->RelayId`。
- 若 fallback 需要 close relay，使用 `binding->Relay.lock()`；lock 失败说明 relay 已 unregister/retire，callback 返回成功或完成 receive cleanup。
- callback 不写 worker-only 字段。

`QueueDeferredQuicReceive()` 签名应改为不依赖 relay map lookup。推荐形态：

```cpp
bool QueueDeferredQuicReceive(
    const std::shared_ptr<StreamBinding>& binding,
    MsQuicStream* stream,
    const QUIC_BUFFER* buffers,
    uint32_t bufferCount,
    bool fin);
```

该函数只做 receive view 构造、callback budget 预留和事件投递。worker 处理 `QuicReceiveView` 时通过 `receive->RelayId` 使用 `FindRelayLocal()` 查 active relay；找不到则 `DiscardDeferredQuicReceive(nullptr, receive)`。

### worker 数据面

worker 线程处理 kqueue 和 event queue 时：

- `ProcessTcpEvents()` 使用 `FindRelayLocal()`。
- `QuicReceiveView` event 在 worker 线程中使用 `FindRelayLocal()`。
- send complete event 在 worker 线程中使用 operation 上的 relay id 和 `FindRelayLocal()`。
- peer abort/shutdown/stop relay event 在 worker 线程中使用 `FindRelayLocal()`。
- 数据面函数入口调用 `AssertWorkerThreadForRelayState()`。

正常路径直接读写 worker-only 字段：

- `DrainTcpReadable()`
- `SubmitTcpBatchToQuic()`
- `TrySubmitQuicSendOperation()`
- `CompleteQuicSend()`
- `RetryPendingQuicSends()`
- `ProcessQuicReceiveViewEvent()`
- `EnqueueQuicReceiveForTcp()`
- `FlushTcpWrites()`
- `MaybePauseQuicReceive()`
- `MaybeResumeQuicReceive()`

### snapshot

运行中 `Snapshot()` 已通过 event queue 让 worker 线程执行 `SnapshotLocal()`。本阶段保留这一机制。

`SnapshotLocal()` 中 active relay 遍历仍可在 `RelayMutex` 下复制或遍历 map，但不应逐条加 `RelayState::Mutex`。如果后续要进一步减少 snapshot 对 `RelayMutex` 的影响，可单独做“复制 shared_ptr 列表后释放 map 锁”的冷路径优化；这不属于本阶段热路径目标。

### unregister 和 close

非 worker `UnregisterRelay()` 直接执行 local unregister 的现状暂不重写。原因是当前注释说明 worker 可能阻塞在 `FlushTcpWrites()`，事件化 unregister 可能等待不到控制事件。

本阶段边界：

- `UnregisterRelayLocal()` 可以继续使用 `RelayMutex` 修改 map 和移除 kqueue filter。
- `RetireRelay()`、`CloseRelay()` 可以继续使用 lifecycle 同步处理 public handle、binding inactive、pending receive discard。
- worker 数据面只处理 active map 中查到的 relay；relay 从 map 删除后，后续 worker event lookup miss 即返回。

## 测试策略

新增或调整 `src/unittest/darwin_relay_worker_io_test.cpp`：

- callback shutdown/abort 只通过 worker event 关闭 relay，不直接写 worker-only 字段。
- receive callback 在 active relay 上不需要 `RelayMutex` map lookup，事件进入 worker 后仍能写 TCP 并 `ReceiveComplete()`。
- receive event 到 worker 时 relay 已 unregister，必须 complete/discard receive 且 pending bytes 归零。
- QUIC->TCP partial write 与 concurrent unregister 保持安全。
- snapshot 与 unregister/stop 并发不崩溃、不挂起。

新增或调整 `src/unittest/darwin_relay_worker_queue_test.cpp`：

- event queue 中的 receive view 在 relay 缺失时仍可被 worker drain，不泄漏 callback budget。

新增测试辅助指标，建议只在 `TCPQUIC_TESTING` 下暴露：

- `FindRelayLockedCountForTest()` 或 snapshot 字段 `RelayMapLockedLookupCount`
- worker-local lookup count 可选，用于证明路径转移

验证命令在 macOS 上执行：

```bash
rtk cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug
rtk cmake --build build --target tcpquic_darwin_relay_worker_io_test tcpquic_darwin_relay_worker_queue_test tcpquic_darwin_relay_metrics_test -j$(sysctl -n hw.ncpu)
rtk build/src/tcpquic_darwin_relay_worker_io_test
rtk build/src/tcpquic_darwin_relay_worker_queue_test
rtk build/src/tcpquic_darwin_relay_metrics_test
```

Linux 开发机不能运行 Darwin kqueue 测试时，至少执行配置和非 Darwin 单测：

```bash
rtk cmake -S . -B build
rtk cmake --build build --target tcpquic_relay_buffer_test tcpquic_relay_alloc_test -j$(nproc)
rtk build/src/tcpquic_relay_buffer_test
rtk build/src/tcpquic_relay_alloc_test
```

## 风险

- `UnregisterRelay()` 非 worker 直删仍是主要竞态风险。本阶段不重写该路径，只把数据面无锁范围限定在 active worker-local relay 上，并用并发测试守住边界。
- `StreamBinding::Relay` 弱引用如果使用不当，可能延长或缩短 relay 生命周期。callback 只能临时 lock，不能把 shared_ptr 长期保存到外部对象。
- callback 入队失败不能丢失 receive ownership。入队失败或 relay missing 必须显式 complete/discard receive。
- `SnapshotLocal()` 删除 per-relay lock 后只适合 worker 线程和 stop 后静止状态。外部运行中 snapshot 必须继续事件化。
- 压缩/解压输出缓冲变成 worker-owned 后，不能从 callback 或 close fallback 读取其内容。

## 验收标准

- 稳定数据面函数不再出现 `std::lock_guard<std::mutex> relayLock(relay->Mutex)`。
- worker-thread event/kqueue 处理路径不再调用 `FindRelay()`，改用 `FindRelayLocal()`。
- `StreamCallback()` RECEIVE、peer abort、peer receive abort、shutdown complete 不调用 `FindRelay()`。
- `RelayMutex` 只保留在 register/unregister/retire/purge/stop/snapshot map 边界和 fallback 查询。
- `SnapshotLocal()` 不再逐条加 `RelayState::Mutex`。
- Darwin relay worker IO/queue/metrics 测试通过。
- `docs/relay_macos.md` 中排名 1、2 的建议可以标记为已有实施计划；排名 3、4 作为下一组 send completion 重构处理。
