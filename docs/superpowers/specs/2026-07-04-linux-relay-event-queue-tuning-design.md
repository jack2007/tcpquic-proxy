# Linux relay event queue 配置和观测设计

## 背景

`docs/relay_linux.md` 原剩余问题中，`3.2 Event queue capacity 未暴露配置` 和 `3.8 运行时缺少锁/队列等待时间指标` 指向同一条排障链路：Linux relay 的 MsQuic callback、send complete、ideal send、控制事件都要进入 owner worker 的 `TqLinuxRelayEventQueue`。本阶段已补齐队列容量配置、queue capacity、CAS retry 和多 producer 观测；ControlLock/Runtime lock/command wait 时间指标仍留给后续阶段。

本阶段不处理 `3.3 QUIC receive 背压没有 hysteresis`。receive high/low watermark、`ReceiveSetEnabled(false/true)` 抖动和 per-profile hysteresis 留给后续独立设计，避免把背压策略调整和队列配置/观测混在一起。

## 目标

1. 增加 Linux relay event queue 容量生产配置入口。
2. 让 CLI 和 JSON 都能覆盖该容量。
3. runtime 启动 Linux relay worker 时把 tuning 中的容量传入 `TqLinuxRelayWorkerConfig::EventQueueCapacity`。
4. 明确容量 normalize 行为和上限，避免用户误配成过小、非 power-of-two 或过大。
5. 在 metrics/admin JSON 中暴露最小必要队列观测字段，帮助判断是否需要调大队列或继续做 queue shard。
6. 更新文档，让排障流程能从 “queue full 现象” 走到 “配置和观测判断”。

## 非目标

- 不修改 QUIC receive hysteresis。
- 不改变 `TqLinuxRelayEventQueue` 的 MPMC ring buffer 语义。
- 不实现 per-worker 多 shard queue 或 per-stream queue。
- 不重构 `ControlLock`、`Runtime::Lock`、同步 command wait。
- 不新增 lock wait nanos 第一版全量指标；本阶段只补齐 queue 竞争相关观测。

## 配置设计

新增字段：

```cpp
struct TqConfig {
    uint32_t TuningOverrideLinuxRelayEventQueueCapacity{0};
};

struct TqTuningConfig {
    uint32_t LinuxRelayEventQueueCapacity{4096};
};
```

配置入口：

- CLI：`--linux-relay-event-queue-capacity <events>`
- JSON：`relay.linux.event_queue_capacity`

取值规则：

- 必须为非零整数。
- 最小值为 2。
- 最大值为 `1048576`。
- 非 power-of-two 输入按现有队列行为 normalize 到下一个 power-of-two。
- normalize 后的值不得超过最大值；例如输入 `1048577` 应拒绝，输入 `65535` 可生效为 `65536`。

默认值：

- `auto` / `wan` / `lan` 均保持 4096。先给用户显式调参能力，不在本阶段改变默认行为。

生效路径：

```text
CLI / cfg.json
  -> TqConfig::TuningOverrideLinuxRelayEventQueueCapacity
  -> TqComputeTuning()
  -> TqTuningConfig::LinuxRelayEventQueueCapacity
  -> TqLinuxRelayRuntime::Start()
  -> TqLinuxRelayWorkerConfig::EventQueueCapacity
  -> TqLinuxRelayEventQueue(capacity)
```

## 观测设计

新增 worker snapshot 字段：

```cpp
uint64_t EventQueueCapacity{0};
uint64_t EventQueuePushCasRetries{0};
uint64_t EventQueuePopCasRetries{0};
uint64_t EventProducerThreadsObserved{0};
bool MultipleEventProducerThreadsObserved{false};
```

其中 `EventProducerThreadsObserved` 和 `MultipleEventProducerThreadsObserved` 在 worker snapshot 中已有，当前缺口是 runtime/metrics/admin JSON 未完整暴露。`EventQueueCapacity`、`EventQueuePushCasRetries`、`EventQueuePopCasRetries` 需要从 `TqLinuxRelayEventQueue` 暴露只读计数。

新增 `TqLinuxRelayEventQueue` 只读统计：

```cpp
struct TqLinuxRelayEventQueueStats {
    uint64_t PushCasRetries{0};
    uint64_t PopCasRetries{0};
};
```

统计规则：

- `TryPush()` 中 `compare_exchange_weak()` 失败时累计 `PushCasRetries`。
- `TryPop()` 中 `compare_exchange_weak()` 失败时累计 `PopCasRetries`。
- 计数使用 `memory_order_relaxed`，只用于观测。
- 不把 `TryPush()` 返回 false 单独记入 queue 统计；现有 `EventQueueFullErrors`、`QuicReceiveViewEnqueueFailures` 等业务指标继续表达 full/failure 结果。

已新增 metrics/admin JSON 字段：

| 字段 | 来源 | 含义 |
| --- | --- | --- |
| `linux_relay_event_queue_capacity` | worker snapshot max/统一容量 | 当前 worker event queue 容量。 |
| `linux_relay_event_queue_push_cas_retries` | queue stats 聚合 | producer push 侧 CAS 重试次数。 |
| `linux_relay_event_queue_pop_cas_retries` | queue stats 聚合 | worker pop 侧 CAS 重试次数。 |
| `linux_relay_event_producer_threads_observed` | worker snapshot 聚合 max | 已观测到的 producer 线程数量下界。 |
| `linux_relay_multiple_event_producer_threads_observed` | worker snapshot OR | 是否观测到多个 producer 线程。 |

worker 详情 JSON 也包含 `event_queue_capacity`，便于对照 `event_queue_depth`。

## 排障语义

推荐解释：

- `linux_relay_event_queue_full_errors > 0`：队列容量或 worker drain 能力不足，已经进入 full 结果路径。
- `linux_relay_quic_receive_view_backpressure_queued > 0`：QUIC receive view 曾因队列压力进入 callback pending 降级路径。
- `linux_relay_multiple_event_producer_threads_observed = true` 且 `push_cas_retries` 持续增长：存在多 producer 竞争，调大容量只能缓解 burst，不一定解决 CAS 竞争；后续可考虑 queue shard。
- `event_queue_depth` 长时间接近 `event_queue_capacity`：优先调大 `relay.linux.event_queue_capacity`，同时检查 worker event budget 和 CPU 饱和。

## 测试策略

单元测试覆盖：

- CLI 能解析 `--linux-relay-event-queue-capacity` 并写入 tuning。
- JSON 能解析 `relay.linux.event_queue_capacity`。
- 非法值 0、超过最大值、未知 key 行为符合现有 config 错误风格。
- Linux worker snapshot 能返回 normalized capacity。
- 多 producer queue test 中 CAS retry 计数和多 producer 字段不会破坏现有队列语义。
- metrics JSON 包含新增 Linux 字段。

验证命令：

```bash
rtk cmake --build build --target tcpquic_tuning_test tcpquic_linux_relay_worker_queue_test tcpquic_router_runtime_test -j$(nproc)
rtk build/bin/Release/tcpquic_tuning_test
rtk build/bin/Release/tcpquic_linux_relay_worker_queue_test
rtk build/bin/Release/tcpquic_router_runtime_test
```

## 风险和约束

- 容量越大，worker queue 内存占用越高。上限必须固定，不能让配置直接驱动任意大分配。
- CAS retry 计数是竞争信号，不等于业务错误；文档应避免把它解释成失败数。
- `EventProducerThreadsObserved` 是下界，不是精确线程集合大小；当前实现只区分“至少一个”和“至少两个”。
- 如果 queue full 仍持续出现，下一步应处理 event budget、worker CPU、queue shard 或控制面 wait，而不是继续无限增大容量。
