# Relay 平台中性配置与 metrics 命名统一设计

## 背景

`docs/relay_macos.md` 中的 P0 问题指出：macOS/Darwin relay 已具备独立的 `TqDarwinRelayWorkerConfig`，但外部配置和部分运行时指标仍复用 `LinuxRelay*` / `linux_relay_*` 命名。当前 `TqDarwinRelayRuntime::Start()` 从 `TqTuningConfig` 的 `LinuxRelayWorkerCount`、`LinuxRelayReadChunkSize`、`LinuxRelayTcpWriteMaxBytes`、`LinuxRelayPerTunnelPendingBytes` 等字段组装 Darwin worker 配置；JSON 配置只能通过 `relay.linux.*` 覆盖这些值；admin/metrics 输出也以 `linux_relay_*` 为主，部分前端再额外探测 `darwin_relay_backend` / `windows_relay_backend`。

这在功能上可用，但会让 macOS 用户误以为必须配置 Linux 后端才能调优 Darwin relay，也会让指标消费者把平台无关的 relay 指标误判为 Linux 专属。

## 目标

1. 增加平台中性的 relay tuning 字段与 JSON 配置入口，让 Linux 和 Darwin 可共享同一组数据面调优项。
2. 保留现有 `relay.linux.*`、`--linux-relay-*`、`LinuxRelay*` 字段和 `linux_relay_*` metrics key 的兼容性，不破坏旧配置、旧脚本和 admin UI。
3. 新增平台中性的 metrics key（`relay_*`），使 `/metrics` 和 admin 后续可以优先读取中性字段。
4. 不改变 Darwin、Linux、Windows relay 的数据转发语义；本计划只做命名、解析、映射和观测层统一。

## 非目标

- 不移除旧 `LinuxRelay*` 字段、旧 CLI 参数或旧 metrics key。
- 不实现 macOS receive sink；该问题仍作为独立 P0 后续处理。
- 不重构 Darwin relay worker 热路径或锁边界。
- 不统一所有 Windows 专属 tuning；Windows 的 worker count 和 Windows-only metrics 仍保留平台专属命名。

## 当前入口

主要涉及文件：

- `src/config/tuning.h`：`TqTuningConfig` 保存 tuning 计算结果，目前平台共享数据面字段仍以 `LinuxRelay*` 命名。
- `src/config/config.h`：`TqConfig` 保存命令行/JSON 覆盖值，目前只有 `TuningOverrideLinuxRelay*`。
- `src/config/tuning.cpp`：`TqComputeTuning()` 默认值、runtime scaling 和 overrides 应用都写入 `LinuxRelay*`。
- `src/config/config.cpp`：解析 `relay.linux.*` 和 `--linux-relay-*`，并在 runtime config JSON 中序列化为 `relay.linux`。
- `src/tunnel/darwin_relay_worker.cpp`：`TqDarwinRelayRuntime::Start()` 读取 `LinuxRelay*` 组装 `TqDarwinRelayWorkerConfig`。
- `src/runtime/relay_metrics.{h,cpp}`：`TqRelayMetricsSnapshot` 中仍有 `LinuxRelay*` 统计字段，JSON 输出主要是 `linux_relay_*`。
- `src/runtime/admin_console.cpp`：admin console 的 `platformRelayBackend()` 兼容多个 backend key。
- `src/unittest/*`：配置、tuning、admin/metrics 单测需要覆盖兼容矩阵。

## 设计方案

### 1. Tuning 内部字段采用“中性主字段 + legacy alias 同步”

在 `TqTuningConfig` 中新增中性主字段：

- `RelayWorkerCount`
- `RelayMaxIov`
- `RelayReadChunkSize`
- `RelayReadBatchBytes`
- `RelayQuicRecvBatchBytes`
- `RelayTcpWriteMaxBytes`
- `RelayTcpWriteBurstBytes`
- `RelayGlobalPendingBytes`
- `RelayPerTunnelPendingBytes`
- `RelayWorkerEventBudget`
- `RelayEventQueueCapacity`
- `RelayWorkerByteBudgetPerTick`
- `RelayQuicReceiveCompleteBatchBytes`

保留现有 `LinuxRelay*` 字段作为 legacy mirror。默认值计算、runtime scaling 和新配置 override 写入中性字段；最后通过一个同步 helper 将中性字段复制到 `LinuxRelay*`。旧 `TuningOverrideLinuxRelay*` 生效时也写入同一个中性字段，再同步回 legacy mirror。

这样可以让新代码读取 `Relay*`，旧代码和旧测试在迁移过程中继续看到一致的 `LinuxRelay*` 值。

### 2. 配置入口新增 `relay.common`，保留 `relay.linux`

新增 JSON 配置：

```json
{
  "relay": {
    "common": {
      "read_chunk_size": 262144,
      "tcp_write_max_bytes": 1048576,
      "tcp_write_burst_bytes": 4194304,
      "event_queue_capacity": 8192,
      "worker_count": 4
    }
  }
}
```

`relay.common` 表示 Linux 和 Darwin 数据面共享调优项。旧 `relay.linux` 继续可用，字段含义不变。解析优先级为：

1. 默认/模式计算值。
2. legacy `relay.linux.*` / `--linux-relay-*` override。
3. 新 `relay.common.*` / `--relay-*` override。

当同一个字段同时由新旧入口指定时，新入口优先。这样既兼容旧配置，又能让迁移后的配置显式覆盖旧值。

### 3. CLI 新增中性参数，旧参数保留

新增 CLI 参数：

- `--relay-read-chunk-size`
- `--relay-tcp-write-max-bytes`
- `--relay-tcp-write-burst-bytes`
- `--relay-event-queue-capacity`
- `--relay-worker-count`

旧 `--linux-relay-*` 参数继续解析，usage 文本标注为 legacy compatibility。为了避免一次变更过大，本阶段不删除旧参数，也不强制输出 deprecation warning；只更新 usage 和文档。

### 4. Darwin/Linux 后端读取中性字段

`TqDarwinRelayRuntime::Start()` 改为读取 `Relay*` 字段组装 `TqDarwinRelayWorkerConfig`。Linux relay 后端后续也应读取 `Relay*`。若本计划实施时 Linux 后端仍有 `LinuxRelay*` 读取点，应在同一任务中逐步替换为 `Relay*`，但保留 snapshot/metrics 的 legacy 输出字段。

### 5. Metrics 增加 `relay_*`，保留 legacy keys

`TqRelayMetricsFieldsJson()` 先输出平台中性 key，例如：

- `relay_backend`
- `relay_active_relays`
- `relay_pending_bytes`
- `relay_event_queue_capacity`
- `relay_errors`
- `relay_quic_receive_view_count`
- `relay_quic_receive_view_bytes`
- `relay_deferred_receive_completes`
- `relay_last_quic_send_status`

同时继续输出现有 `linux_relay_*` key。对于平台通用统计，`relay_*` 与 `linux_relay_*` 使用同一个 `TqRelayMetricsSnapshot` 字段值。对真正平台专属的 Windows 统计，保留 `windows_relay_*`；若某个 Linux legacy 字段实际已经在 Darwin 复用，也可先添加中性 alias，但不在本阶段重命名 struct 字段。

admin console 的 backend 读取顺序调整为优先 `data.relay_backend`，然后 fallback 到 `data.linux_relay_backend`、`data.darwin_relay_backend`、`data.windows_relay_backend`、`data.backend`。

### 6. Runtime config JSON 输出策略

`RuntimeConfigJson()` 对新覆盖值输出 `relay.common`。如果只有旧 `TuningOverrideLinuxRelay*` 被设置，短期仍输出 `relay.linux`，保持 round-trip 兼容。若新旧 override 同时存在且新值生效，输出 `relay.common` 表示实际推荐配置形态。

## 错误处理和兼容性

- `relay.common` 必须是 object，否则报 `relay.common must be an object`。
- unknown key 报 `unknown relay.common key: <key>`。
- 数值校验沿用现有规则：非零 uint32；event queue capacity 使用现有 min/max；worker count 使用 `TqRelayWorkerCountMin/Max`。
- 旧配置不报错，行为保持不变。
- 新旧配置同时存在时不报冲突；新配置覆盖旧配置。

## 测试策略

1. `config_router_test` 或配置解析相关测试：覆盖 `relay.common.*` 能正确写入中性 override 并在 `TqFinalizeConfig()` 后反映到 tuning。
2. 旧 `relay.linux.*` 回归：确认旧配置仍能解析并产生相同 tuning。
3. 新旧冲突优先级：同一字段同时设置时 `relay.common` 覆盖 `relay.linux`。
4. CLI 测试：新增 `--relay-read-chunk-size` 等参数，并保留 `--linux-relay-read-chunk-size`。
5. `tuning_test`：确认默认值、high-BDP scaling、pool budget 同步到 `Relay*` 和 `LinuxRelay*`。
6. `relay_metrics` 测试：确认 JSON 同时含 `relay_backend` 和 `linux_relay_backend`，且值一致。
7. admin console 测试：确认前端优先读取 `relay_backend`，并保留 legacy fallback。

## 迁移路径

第一阶段（本计划）：新增中性入口、同步 legacy mirror、metrics alias、测试和文档。

第二阶段（后续）：将源码内大部分平台共享字段读取迁移到 `Relay*`，保留少量 legacy 字段用于旧 API/metrics 输出。

第三阶段（更长期）：评估是否将 `LinuxRelay*` struct 字段标注为 deprecated，或在主要发布版本后移除旧 JSON/CLI 入口。该阶段不属于本计划。

## 验收标准

- macOS 用户可以通过 `relay.common.*` 或 `--relay-*` 调整 Darwin relay worker 配置，不再必须写 `relay.linux.*`。
- 旧 `relay.linux.*` 和旧 `--linux-relay-*` 配置仍通过原有测试。
- metrics JSON 同时提供 `relay_*` 中性 key 和旧 key。
- admin console 优先显示 `relay_backend`，旧 key fallback 不破坏。
- Darwin relay 数据路径行为无变化，相关单元测试和目标级构建通过。
