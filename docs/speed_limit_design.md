# QUIC 进程级限速与优先级调度设计

本文说明 `tcpquic-proxy` 增加 `--max-speed <kbps>` 后，如何对整个进程的 QUIC 出口做总速率限制，并在多个 peer 并发传输时按优先级调度带宽。

## 设计目标

1. `--max-speed` 限制当前进程所有 QUIC 连接的总发送速率，而不是限制单条 stream 或单条 QUIC connection。
2. 多 peer、多 path、多 QUIC connection、多 stream 共享同一个进程级带宽预算，不能因为 `--connections` 增加而绕过总限速。
3. 支持 peer 级优先级。高优先级 peer 在带宽竞争时优先获得带宽。
4. 调度必须是 work-conserving：高优先级 peer 无法填满总带宽时，普通或低优先级 peer 可以借用未使用的带宽。
5. 低优先级 peer 不能长期饿死。在高优先级流量持续占满链路时，仍应保留基本传输能力。
6. 限速实现不能阻塞 relay worker 线程，不能通过 `sleep` 卡住事件循环。
7. 保留 MsQuic 自身 BBR、pacing、flow control 行为。应用层只控制进入 QUIC 的 payload 速率。

## 基本语义

`--max-speed <kbps>` 表示当前进程所有 QUIC 出口 payload 的总速率上限。

- 未配置或配置为 `0` 表示不限速。
- client 进程配置后，限制 client 发往 server 的 QUIC 总出口。
- server 进程配置后，限制 server 发往 client 的 QUIC 总出口。
- 如果需要限制下载方向，应在实际发送下载数据的一侧配置，例如普通代理下载场景通常需要在 server 侧配置。

单位使用 kbps，按网络常用语义解释为 kilobits per second：

```text
bytes_per_second = max_speed_kbps * 1000 / 8
```

## 为什么不在 MsQuic 参数里限速

当前项目在 `TqMakeMsQuicSettings()` 中已启用 BBR 和 pacing，并关闭 MsQuic send buffering，由 relay worker 自己管理发送流水线和 backpressure。MsQuic pacing 负责拥塞控制下的平滑发送，不是用户配置的固定带宽上限；公开 `QUIC_SETTINGS` 也没有稳定的 per-process max bitrate 配置。

因此限速应放在应用层数据面：

```text
TCP read -> optional compress -> QUIC StreamSend
```

也就是在提交 `StreamSend` 前申请发送预算。预算不足时暂停对应 TCP read，等待限速器通知后再恢复。

## 总体方案

引入一个进程级共享调度器：

```text
TqConfig.MaxSpeedKbps
        |
        v
TqProcessRateScheduler
        |
        +-- Global token bucket: 控制整个进程 QUIC 总出口
        |
        +-- Peer scheduler: 按 peer priority 分配和借用 token
        |
        v
Linux / Darwin / Windows relay worker
        |
        v
TCP -> QUIC StreamSend 前 TryAcquire(peer_id, bytes)
```

relay worker 不直接从全局 token bucket 扣 token，而是通过 `TqProcessRateScheduler` 以 `peer_id` 为维度申请预算。这样可以同时满足总限速和 peer 优先级调度。

## 配置模型

### 进程级限速

CLI：

```bash
tcpquic-proxy client --max-speed 1000000 ...
tcpquic-proxy server --max-speed 1000000 ...
```

Runtime JSON 建议放在 `proto` 或顶层限速配置中，例如：

```json
{
  "proto": {
    "max_speed_kbps": 1000000
  }
}
```

### Peer 优先级

peer 配置增加：

```json
{
  "peer_id": "A",
  "quic_peer": "a.example.com:443",
  "priority": "high"
}
```

优先级建议第一版支持三档：

| priority | 默认权重 | 含义 |
|---|---:|---|
| `high` | 8 | 重要业务，竞争时优先获得带宽 |
| `normal` | 2 | 默认业务 |
| `low` | 1 | 后台或低优先级业务 |

可选增强字段：

| 字段 | 含义 |
|---|---|
| `min_speed_kbps` | peer 活跃时的保底带宽。默认 `0`，表示不显式保底 |
| `max_speed_kbps` | peer 自身封顶。默认 `0`，表示只受进程总限速约束 |

如果所有 peer 的 `min_speed_kbps` 之和超过 `--max-speed`，配置应被拒绝。

## 调度算法

推荐使用 **work-conserving weighted fair scheduling**，实现上可用加权 deficit round-robin。

每个 peer 维护：

```text
priority
weight
min_speed_kbps
max_speed_kbps
deficit_bytes
active_demand
paused_relays
```

调度流程：

1. 全局 token bucket 按 `--max-speed` 补充 token。
2. 对活跃 peer 先处理 `min_speed_kbps` 保底预算。
3. 剩余 token 按 priority weight 增加各 peer 的 deficit。
4. relay 在 `StreamSend` 前调用 `TryAcquire(peer_id, requested_bytes)`。
5. 如果 peer 预算和全局 token 足够，则返回可发送字节数。
6. 如果预算不足，则 relay 暂停该方向 TCP read，并注册下一次可发送时间。
7. 未被高优先级 peer 消费的预算进入可借用池，普通和低优先级 peer 可以继续使用。

关键要求是：分配给高优先级 peer 但没有被实际消费的额度不能浪费，必须允许低优先级 peer 借用。

## 借用语义示例

假设：

```text
--max-speed = 1000000 kbps 约 1Gbps
A = high，实际最多 300Mbps
B = normal，实际最多 300Mbps
C = low，实际最多 700Mbps
```

如果 A、B、C 都有数据，期望结果是：

```text
A 使用约 300Mbps
B 使用约 300Mbps
C 借用剩余约 400Mbps
总量约 1Gbps
```

C 不能同时跑到 700Mbps，因为 `300 + 300 + 700 = 1.3Gbps`，超过进程总限速。

其他情况：

```text
A=300Mbps, B=0Mbps, C能力=700Mbps  => C 可跑到约 700Mbps，总量约 1Gbps
A=100Mbps, B=100Mbps, C能力=700Mbps => C 可跑到约 700Mbps，总量约 900Mbps
A=800Mbps, B=100Mbps, C能力=700Mbps => C 被压到约 100Mbps，总量约 1Gbps
```

这体现了目标语义：高优先级优先保障，但高优先级填不满时，低优先级可以充分利用剩余带宽。

## Burst 与精度

限速器使用 token bucket，允许短时间 burst，但长期平均不超过配置。

推荐默认 burst：

```text
burst_bytes = max(64KB, bytes_per_second / 10)
```

也就是约 100ms 的发送额度。这样低速时不会碎片化得太严重，高速时也不会出现过大的瞬时突发。

调度 tick 建议使用 10ms 或 20ms。tick 越小，限速越平滑，但唤醒频率越高；tick 越大，CPU 开销更低，但瞬时抖动更明显。

## Relay 集成点

限速应接入所有平台的 TCP -> QUIC 方向：

- Linux：`TqLinuxRelayWorker::DrainTcpReadable()` / `SubmitTcpBatchToQuic()` / `TrySubmitQuicSendOperation()`
- Darwin：`TqDarwinRelayWorker::DrainTcpReadable()` / `SubmitTcpBatchToQuic()` / `TrySubmitQuicSendOperation()`
- Windows：`TqWindowsRelayWorker::HandleTcpRecv()` / `TrySubmitQuicSendOperation()`

推荐优先在读取 TCP 前控制本轮读取预算，避免读入大量数据后再排队等待 QUIC token：

```text
allowed = scheduler.TryAcquire(peer_id, wanted_read_bytes)
if allowed == 0:
    pause tcp read
    schedule wakeup
else:
    read at most allowed bytes
    compress if needed
    StreamSend
```

压缩开启时存在一个细节：用户配置的限速是 QUIC 出口 payload 速率，还是原始 TCP payload 速率。本文建议第一版按 **QUIC 出口 payload** 限速，即压缩后字节数计入限速。这与“限制 QUIC 协议最大传输速度”的需求一致。

如果要严格限制压缩后大小，读取 TCP 前无法准确知道压缩后字节数。可采用保守策略：

1. 先按原始读取预算控制 TCP read，避免无限读入。
2. 压缩后按实际 QUIC bytes 再扣 token。
3. 如果压缩后超过可用 token，则拆分或暂存未发送的压缩输出，等待下一次 token。

## Backpressure 与唤醒

预算不足时不能阻塞 worker 线程。应使用现有 backpressure 风格：

1. 标记 relay 因 rate limit 暂停。
2. 暂停 TCP read，例如 Linux/Darwin 取消读事件，Windows 不再 post recv。
3. scheduler 计算下一次可能有 token 的时间。
4. worker 在 timer 到期后重新 arm TCP read。

暂停原因应和现有 QUIC backlog backpressure 区分，例如使用 `quic_rate_limit`，便于 trace 和指标分析。

## 公平性策略

第一版建议做到 peer 级公平，不做 stream 级公平：

- 同一个 peer 内部仍沿用现有连接/stream 调度方式。
- 不在单个 tunnel 内做优先级，避免改动控制协议和 relay 状态过大。
- 多 peer 竞争时按 peer priority 调度。

如果后续发现单个 peer 内一个大流压住小流，再考虑 peer 内部增加 stream/relay 级 round-robin。

## 观测指标

建议新增指标：

| 指标 | 含义 |
|---|---|
| `quic_rate_limit_max_speed_kbps` | 当前进程总限速 |
| `quic_rate_limit_tokens_available` | 当前全局可用 token |
| `quic_rate_limit_pauses` | 因限速暂停 TCP read 的次数 |
| `quic_rate_limit_resume_events` | 因 token 恢复而重新 arm read 的次数 |
| `quic_rate_limit_delayed_bytes` | 因限速延迟发送的字节估算 |
| `peer_rate_limit_sent_bytes` | 每个 peer 经限速器放行的 QUIC bytes |
| `peer_rate_limit_borrowed_bytes` | peer 借用其他优先级未用额度的字节数 |

这些指标用于验证：

- 所有 peer 总发送速率是否不超过 `--max-speed`。
- 高优先级 peer 是否在竞争时获得更高份额。
- 低优先级 peer 是否在高优先级空闲或带宽不足时借用剩余额度。

## 第一版建议范围

第一版建议实现：

1. `--max-speed <kbps>` 进程级 QUIC 出口总限速。
2. peer 配置 `priority: high|normal|low`。
3. 全局 token bucket + peer 加权借用式调度。
4. relay worker 在 TCP -> QUIC 入口限速，预算不足时暂停 TCP read。
5. 基础 metrics 和 trace 原因。

暂不实现：

1. stream 级优先级。
2. 动态自动调权。
3. 跨进程或跨机器全局限速。
4. 基于 MsQuic 内部 pacing 参数的限速。

