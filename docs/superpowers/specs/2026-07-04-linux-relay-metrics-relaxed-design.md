# Linux relay metrics relaxed atomics 设计

`docs/relay_linux.md` 的剩余问题中，`3.7 metrics 原子内存序偏保守` 是当前最适合继续推进的项目：它对应同步热点排名 5，范围清晰，收益直接落在 Linux relay 高频数据面计数和 snapshot 路径上。`3.3 QUIC receive 背压没有 hysteresis` 本轮不处理；queue shard、per-relay callback pending top N、stream/fd lookup map 继续依赖生产指标信号后再排期。

## 背景

`TqLinuxRelayWorker` 内大量计数器使用 `std::atomic<uint64_t>`，但很多 `fetch_add()`、`load()`、`store()` 没有显式内存序，因此默认使用 `memory_order_seq_cst`。这些字段多数只用于 metrics、admin snapshot 或错误排障，不参与 worker 生命周期、callback binding、command 完成等跨线程同步协议。

在小包、高 callback/event 频率场景下，默认 `seq_cst` 会给纯统计写入增加不必要的全局顺序约束。Linux relay 的核心状态已经由 owner worker 串行处理；metrics 需要的是最终可观测和单调累加，不需要用统计计数建立 happens-before。

## 目标

1. 把 Linux relay 纯统计原子的更新和 snapshot 读取统一改为 `std::memory_order_relaxed`。
2. 明确区分 metrics 原子和同步原子，避免误改生命周期、binding、wake、command 等同步字段。
3. 保持 admin metrics 字段、计数语义和测试可观测行为不变。
4. 先做低风险内存序收敛，不在本阶段把原子字段改为普通整数。

## 非目标

- 不实现 `3.3 QUIC receive` hysteresis。
- 不新增 queue shard、stream/fd lookup map 或 per-relay callback pending top N。
- 不改 metrics JSON 字段名。
- 不调整 relay buffer budget CAS、event queue ring buffer CAS 或 callback binding 生命周期协议。
- 不把 worker-owned metrics 字段批量改成普通整数；该优化需要单独审计所有读写线程和 snapshot 入口。

## 分类原则

### 可改为 relaxed 的字段

满足下面条件的字段应使用 `memory_order_relaxed`：

- 只用于计数、最大值观测、错误码记录或 admin snapshot。
- 读者不依赖该字段的值来判断其它内存写入是否可见。
- 字段值不参与对象生命周期、队列 ownership、callback 引用计数、stop/closing 协议。

典型类别：

- worker event 计数：`EventsProcessed`、`WakeupWrites`。
- TCP/QUIC 数据面计数：`TcpReadBytes`、`TcpWriteBytes`、`QuicSendOperations`、receive view size/slice histogram。
- 错误和背压计数：`Errors`、`EventQueueFullErrors`、`QuicSendFatalErrors`、`TcpWriteEagainCount`、`FatalRelayResets`、`FakeFinReceiveCount`。
- 诊断状态：`LastTcpWriteErrno`、`LastTcpReadErrno`、`LastQuicSendStatus`。
- worker/runtime wait metrics：已有部分已使用 relaxed，本轮保持一致。
- `UpdateAtomicMax()` 内的最大值 CAS：该 helper 已经使用 relaxed，符合目标语义。

### 必须保留同步内存序的字段

下面字段不应在本阶段改为 relaxed 或普通字段：

- `Running`：控制 worker start/stop、线程入口和控制面命令边界。
- `WakeArmed`：配合 event queue wake 合并和 eventfd 通知。
- `StreamRelayBinding::{Relay, Handle, Closing, CallbackRefs}`：callback 线程和 worker detach 之间的生命周期协议。
- `TqRelayHandle::Stop`：relay 关闭、callback 降级和控制面可见状态。
- `FirstEventProducerHash`、`EventProducerThreadCount`、`MultipleEventProducerThreadsObserved`：用于跨 producer 观测，当前 acquire/release 语义保留，避免和本轮纯 metrics 收敛混在一起。
- event queue ring buffer 内部原子：这是 MPMC 队列正确性的一部分，不属于本计划范围。

## 推荐方案

采用 **显式 relaxed 第一阶段**：

1. 在 `src/tunnel/linux_relay_worker.cpp` 中把纯 metrics 的 `fetch_add()`、`load()`、`store()` 改为带 `std::memory_order_relaxed`。
2. `SnapshotLocal()` 中除同步观测字段外，metrics snapshot load 均显式使用 relaxed。
3. 保留所有字段类型和 public snapshot 结构不变，避免 ABI/API 和测试大范围连锁变化。
4. 通过代码审阅和 targeted search 确认 `Running`、`WakeArmed`、binding、handle stop、event queue 原子没有被误改。

该方案是低风险机械优化：它不改变计数器的可见性目标，只去掉纯统计字段不需要的全局顺序约束。后续如果生产压测仍显示 metrics cache line 压力明显，再另行设计 worker-local 普通计数或分层聚合。

## 测试策略

1. 构建并运行 Linux relay worker IO 测试，覆盖 TCP/QUIC 数据面计数、fake FIN、snapshot 和 worker lifecycle。
2. 构建并运行 router runtime 测试，确认 admin metrics JSON 字段仍存在。
3. 使用 `rtk git diff --check` 检查文档和代码格式。
4. 使用 targeted `rg` 检查关键同步字段仍保留 acquire/release/acq_rel，纯 metrics 热点不再使用默认 `seq_cst`。

## 风险和缓解

- 风险：误把生命周期字段改成 relaxed，导致 callback detach 或 worker stop 可见性退化。
  - 缓解：计划中使用明确 denylist，并在实现后 grep `Running`、`WakeArmed`、`CallbackRefs`、`Relay.load/store`、`Handle.load/store`。
- 风险：`Last*` 诊断字段读取到稍旧值。
  - 缓解：这些字段只用于错误日志和 metrics，不承担同步；relaxed 允许读到当前原子修改序中的某个值，满足诊断语义。
- 风险：一次性改普通整数引入数据竞争。
  - 缓解：本阶段不改字段类型，只改内存序。

## 自检

- 本设计只覆盖 `docs/relay_linux.md` 3.7。
- 未改变用户明确暂不处理的 `3.3`。
- 已把同步字段和 metrics 字段分开，避免把性能优化变成生命周期重写。
