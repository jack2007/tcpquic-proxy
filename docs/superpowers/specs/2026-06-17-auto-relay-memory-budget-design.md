# 自动 Relay Memory Budget 设计

> 日期：2026-06-17  
> 状态：待评审  
> 决策：删除用户侧 `--max-memory-mb` / `tuning.max_memory_mb`，relay memory budget 改为内部自动推导。

## 1. 背景

当前 tuning 参数暴露了 `--max-memory-mb <n>`，用于限制 relay pool / pending buffer 的内存规模。这个参数对普通用户不友好：用户很难根据带宽、RTT、连接数和并发 tunnel 数量手动给出合理值。

代码中它主要影响两条路径：

| 路径 | 当前作用 |
|------|----------|
| `TqApplyLinuxRelayDefaults()` | 通过 `TqGetRelayMemoryBudget()` 推导 `LinuxRelayGlobalPendingBytes`、`LinuxRelayPerTunnelPendingBytes`、`MaxPendingBufferBytesPerRelay` |
| `TqApplyRelayPoolBudget()` | 按 active relay 数量压低 `RelayDefaultIdealSend`、`RelayMaxInFlightSends`、`RelayMaxFreeSendContexts` |

当前主程序调用顺序还有行为问题：`TqFinalizeConfig(cfg)` 先计算 tuning，随后才调用 `TqSetRelayMemoryBudget(cfg.MaxMemoryMb)`。因此 Linux 默认 pending budget 在 finalize 阶段看不到用户传入的 `--max-memory-mb`，参数语义并不可靠。

## 2. 目标

1. 普通用户不再需要输入 relay 内存预算。
2. 自动预算必须覆盖高带宽高 RTT 场景，避免 10Gbps/20Gbps WAN 因 buffer 过小跑不满。
3. 自动预算必须有保守上限，避免因 BDP 过大或并发过多导致进程内存失控。
4. 保留内部 budget / backpressure 机制，删除的是用户入口，不是安全边界。
5. tuning cache 后续落地时，cache 可保存推导结果，但主配置仍只保存目标带宽等用户意图。

## 3. 非目标

- 不引入新的用户参数替代 `--max-memory-mb`。
- 不要求一次实现完整 OS memory pressure 自适应回收。
- 不改变 QUIC flow control、relay pause/resume、pending queue 的基本背压语义。
- 不修改历史 benchmark 原始日志中的旧命令记录。

## 4. 用户侧行为

删除公开参数：

```text
--max-memory-mb <n>
```

删除配置文件字段：

```json
{
  "tuning": {
    "max_memory_mb": 4096
  }
}
```

推荐最终用户侧 tuning 入口收敛为：

```text
--tuning auto
--target-bandwidth-mbps <n>
```

如果用户不提供 `target-bandwidth-mbps`，使用内置 WAN profile，不主动 probe，不写 tuning cache。后续如果实现显式测速开关，可以由测速结果提供带宽输入，但不重新暴露 memory budget。

## 5. 自动预算模型

### 5.1 基础公式

```text
BDP(bytes) = bandwidth_mbps * 1_000_000 / 8 * rtt_ms / 1000
target_pending_per_tunnel = clamp(2 * BDP, 16MiB, 512MiB)
```

`2 * BDP` 用于覆盖 TCP/QUIC relay pipeline、调度延迟和短时间 burst。上限先设为 `512MiB`，对应当前 20Gbps/100ms 级别 WAN profile 的高水位。

示例：

| 带宽 / RTT | BDP | 2BDP | per tunnel 预算 |
|------------|-----|------|-----------------|
| 1Gbps / 50ms | 6.25MiB | 12.5MiB | 16MiB |
| 10Gbps / 100ms | 125MiB | 250MiB | 250MiB |
| 20Gbps / 100ms | 250MiB | 500MiB | 500MiB |
| 100Gbps / 100ms | 1.25GiB | 2.5GiB | 512MiB，受单 tunnel 上限保护 |

### 5.2 进程级 soft limit

自动预算需要进程级上限，不让 active tunnels 简单相乘失控：

```text
system_soft_limit = min(detected_memory / 8, 8GiB)
system_soft_limit = max(system_soft_limit, 256MiB)
```

如果无法可靠读取系统内存，使用保守默认：

```text
system_soft_limit = 512MiB
```

Linux 可从 `sysconf(_SC_PHYS_PAGES)` / `sysconf(_SC_PAGE_SIZE)` 读取物理内存。Windows 可用 `GlobalMemoryStatusEx`。第一阶段可以实现跨平台 helper，失败时返回默认值。

### 5.3 active relay 分摊

当 active relay 数量增加时，单 relay 可用预算应被压低：

```text
effective_per_tunnel_budget =
    min(target_pending_per_tunnel, system_soft_limit / max(active_relays, 1))
```

下限不强行保证吞吐，只保证不低于一个可运行的最小值：

```text
effective_per_tunnel_budget = max(effective_per_tunnel_budget, 4MiB)
```

如果 active relay 很多，吞吐优先级让位于内存安全和公平性。

## 6. Tuning 字段映射

自动预算结果应写入现有内部字段：

| 字段 | 自动来源 |
|------|----------|
| `LinuxRelayGlobalPendingBytes` | `system_soft_limit / 2` 或基于 target tunnel 数量推导后 clamp |
| `LinuxRelayPerTunnelPendingBytes` | `effective_per_tunnel_budget` |
| `MaxPendingBufferBytesPerRelay` | `effective_per_tunnel_budget` 或与 worker 分摊后的较大值 |
| `RelayDefaultIdealSend` | 继续由 profile / BDP 推导，再被 active relay budget 压低 |
| `RelayMaxInFlightSends` | 继续由 `RelayDefaultIdealSend / RelayIoSize` 推导，再被 budget 压低 |
| `RelayMaxFreeSendContexts` | 与 `RelayMaxInFlightSends` 保持一致或更低 |

注意：`RelayIoSize` 是 Windows relay IO buffer size，`LinuxRelayReadChunkSize` 是 Linux relay TCP read chunk size。memory budget 不应该重新合并这两个平台字段。

## 7. 代码结构

### 7.1 删除用户配置入口

删除：

- `TqConfig::MaxMemoryMb`
- CLI help 中的 `--max-memory-mb`
- CLI parser 中的 `--max-memory-mb`
- JSON parser 中的 `tuning.max_memory_mb`
- `main.cpp` 中基于 `cfg.MaxMemoryMb` 的 `TqSetRelayMemoryBudget()` 调用和启动日志

保留但重构：

- `TqApplyRelayPoolBudget()`
- `TqSetRelayMemoryBudget()` / `TqGetRelayMemoryBudget()` 可在第一阶段保留给单元测试和内部自动预算使用；最终可以改名为 `TqSetAutoRelayMemoryBudget()`，避免误解为用户配置。

### 7.2 新增内部 helper

建议新增：

```cpp
uint64_t TqDetectSystemMemoryBytes();
uint32_t TqComputeAutoRelayMemoryBudgetMb(const TqConfig& cfg, const TqTuningConfig& tuning);
void TqApplyAutoRelayMemoryBudget(TqConfig& cfg);
```

调用顺序建议改为：

```text
TqComputeTuning(cfg, cfg.Tuning)
  -> profile / BDP defaults
  -> compute auto memory budget
  -> apply Linux relay budget defaults
  -> apply active relay pool budget as relays are registered
```

不要再出现“先 finalize，后设置 budget，导致 defaults 读不到预算”的顺序。

### 7.3 cache 对接

后续 tuning cache 可以保存以下观测值：

```json
{
  "peer": "example:4433",
  "target_bandwidth_mbps": 20000,
  "measured_rtt_ms": 100,
  "computed": {
    "relay_memory_budget_mb": 512,
    "linux_per_tunnel_pending_bytes": 524288000,
    "linux_global_pending_bytes": 1073741824
  }
}
```

cache 中保存的是运行时结果，主配置仍不保存 memory budget。cache 失效或不存在时重新基于带宽 + RTT 计算。

## 8. 运行时更新

RTT 或带宽变化后，自动 tuning 可以重新计算 memory budget。更新策略：

1. 如果新预算比旧预算更大，只影响后续 buffer acquire，不强制预分配。
2. 如果新预算比旧预算更小，不立即释放正在使用的 buffer，只降低后续 acquire 上限。
3. 当 active relay 数量变化时，调用 `TqApplyRelayPoolBudget()` 或等价逻辑重新计算 per relay 上限。
4. 将自动预算值输出到 trace / metrics，便于诊断。

建议 metrics：

```text
tcpquic_relay_auto_memory_budget_bytes
tcpquic_relay_effective_per_tunnel_budget_bytes
tcpquic_relay_pending_buffer_bytes
tcpquic_relay_budget_clamp_events_total
```

## 9. 迁移策略

### Phase 1：删除用户入口，内部默认自动预算

- 删除 `--max-memory-mb` 和 `tuning.max_memory_mb`。
- 自动预算先使用现有 WAN/LAN/profile 推导值。
- `--target-bandwidth-mbps` 存在时，用 BDP 推导 per tunnel pending。
- 无目标带宽时，沿用 WAN profile 的保守默认，不 probe，不写 cache。

### Phase 2：接入 RTT probe / tuning cache

- 首次无 cache 且用户提供目标带宽时，建立 probe 连接获取真实 RTT。
- 基于目标带宽 + RTT 计算 memory budget 和其它 tuning。
- 写入独立 tuning cache。

### Phase 3：运行时动态更新

- 持续观测 RTT / throughput / active relays。
- 超过阈值时重算预算并更新 cache。
- 对不能动态修改的 QUIC 参数，只影响后续连接。

## 10. 测试计划

单元测试：

1. CLI help 不再包含 `--max-memory-mb`。
2. CLI 传入 `--max-memory-mb` 返回 unknown option。
3. JSON `tuning.max_memory_mb` 返回 unknown key 或明确错误。
4. 1Gbps/50ms 自动预算得到至少 16MiB。
5. 20Gbps/100ms 自动预算接近 500MiB，但不超过 per tunnel 上限。
6. 100Gbps/100ms 被 per tunnel / process soft limit clamp。
7. active relay 增加时，per relay budget 被压低。

集成验证：

1. `tcpquic_config_router_test`
2. `tcpquic_tuning_test`
3. `tcpquic-proxy` 主目标编译
4. Linux relay worker 高吞吐 smoke test

## 11. 风险与缓解

| 风险 | 缓解 |
|------|------|
| 自动预算过小影响高带宽吞吐 | BDP 使用 `2 * BDP`，20Gbps/100ms 可得到约 500MiB |
| 自动预算过大导致 RSS 偏高 | process soft limit + active relay 分摊 + lazy allocation |
| 系统内存探测失败 | 使用 512MiB 保守默认 |
| 删除参数影响旧脚本 | 当前目标是减少用户输入；旧脚本应迁移到 `--target-bandwidth-mbps` 或默认 WAN profile |
| cache 中预算过期 | cache 需要记录 RTT、带宽、时间戳，变化超过阈值时重算 |

## 12. 决策摘要

`--max-memory-mb` 是内部资源预算细节，不应继续作为用户参数。删除用户入口后，程序通过带宽、RTT、active relay 数量和系统内存 soft limit 自动推导 relay memory budget。这样可以降低配置复杂度，同时保留大带宽场景所需的 BDP 级 pending buffer。
