# Phase 4 always-pending 分支 UDP Buffer 与优化建议

时间：2026-06-12

适用代码分支：

- 分支：`master`
- 代表 commit：`efcedd6`
- 方案：deferred receive view + always `QUIC_STATUS_PENDING` + `StreamMultiReceiveEnabled`

本文从 `docs/dgx-msquic-udp-buffer-optimization-20260612.md` 拆分而来，只讨论 always-pending 分支的优化方向。通用结论仍成立：MsQuic 没有应用可配置的 UDP socket buffer knob；Linux epoll datapath 会请求 `SO_RCVBUF = INT32_MAX`，实际值由 `net.core.rmem_max` 封顶；UDP `SO_SNDBUF` 未由 MsQuic 显式设置，发送侧依赖系统 `wmem_*`。

## 当前性能画像

sysctl 128MB 后：

| 指标 | always-pending |
|------|----------------|
| proxy-1x1 | 5.75 Gbps |
| proxy-16x16 | 15.61 Gbps |
| proxy-4x16 | 24.99 Gbps |

结论：

1. 多流最优，4x16 接近 25 Gbps。
2. 单流仍弱，约为 Phase 3 的一半。
3. 多流 perf 里 `__arch_copy_to_user` 约 22% 量级时，瓶颈已经接近 MsQuic UDP receive path。
4. relay receive copy 已基本消失，继续只压 QUIC->TCP memcpy 不是主收益点。

## 分支核心路径

always-pending 的 QUIC->TCP receive 路径是：

```text
MsQuic RECEIVE callback
  -> QueueDeferredQuicReceive()
  -> 保存 QUIC_BUFFER slice 元数据
  -> return QUIC_STATUS_PENDING

relay worker
  -> ProcessQuicReceiveViewEvent()
  -> FlushDeferredQuicReceives()
  -> sendmsg(TCP fd, borrowed QUIC buffers)
  -> StreamReceiveComplete(bytes)
```

该路径的优点：

- QUIC receive payload 不再 copy 到 relay buffer。
- 多流下多个 pending receive 可以填满 worker / MsQuic 调度空隙。
- sysctl 后大 TCP/UDP buffer 让多流吞吐更稳定。

该路径的弱点：

- 单流每次 receive 都要等待 `PENDING -> worker -> sendmsg -> StreamReceiveComplete`。
- 即使 TCP socket buffer 很大，completion cadence 仍限制单流流水线。
- 多流高吞吐时热点转向 UDP kernel copy，而不是 relay copy。

## MsQuic UDP Buffer 结论

### UDP RX

MsQuic Linux epoll datapath 创建 UDP socket 时请求：

```text
setsockopt(SOL_SOCKET, SO_RCVBUF, INT32_MAX)
```

但生效上限由：

```text
net.core.rmem_max
```

决定。因此 always-pending 多流 benchmark 必须固定 sysctl，否则不同 run 的 `__arch_copy_to_user`、packet drop、receive pacing 都不可比。

### UDP TX

MsQuic 没有设置 UDP `SO_SNDBUF`。虽然 always-pending 的主要问题在 receive completion，但 UDP TX 仍依赖：

```text
net.core.wmem_default
net.core.wmem_max
```

这些 sysctl 应随 RX 一起固化。

## 必须固化的 bench 前置条件

```bash
sudo sysctl -w net.core.rmem_max=134217728
sudo sysctl -w net.core.wmem_max=134217728
sudo sysctl -w net.core.rmem_default=4194304
sudo sysctl -w net.core.wmem_default=4194304
sudo sysctl -w 'net.ipv4.tcp_rmem=4096 1048576 134217728'
sudo sysctl -w 'net.ipv4.tcp_wmem=4096 1048576 134217728'
```

bench 脚本应 fail-fast 校验：

```text
net.core.rmem_max >= 134217728
net.core.wmem_max >= 134217728
net.ipv4.tcp_rmem max >= 134217728
net.ipv4.tcp_wmem max >= 134217728
```

## always-pending 分支优化优先级

### P0：补可观测性

必须先补指标，否则无法判断单流弱是 complete 粒度、worker 调度、TCP backpressure，还是 MsQuic receive cadence。

建议新增：

- `StreamReceiveComplete` 次数
- `StreamReceiveComplete` bytes 总量
- 每次 complete 平均 bytes
- `PendingQuicReceiveBytes` p50/p95/p99
- per-relay pending receive 队列长度
- `ReceiveSetEnabled(false/true)` 次数
- `sendmsg` 次数
- 每次 `sendmsg` bytes
- `EAGAIN` / partial write 次数
- worker 从 receive event 入队到 first sendmsg 的延迟

同时，TCP socket tuning 后应打印：

- requested `SO_RCVBUF`
- effective `SO_RCVBUF`
- requested `SO_SNDBUF`
- effective `SO_SNDBUF`

### P1：StreamReceiveComplete 聚合实验

当前逻辑是每次 `sendmsg` 成功后立即：

```text
StreamReceiveComplete(sent)
```

可测试改为按阈值聚合 complete：

| complete threshold | 目标 |
|--------------------|------|
| 128KB | 保守，验证是否减少 complete 次数 |
| 256KB | 平衡 latency 与吞吐 |
| 512KB | 更偏吞吐 |
| 1MB | 压测上限，观察 flow-control 副作用 |

风险：

- complete 太晚会压住 MsQuic receive buffer / flow control。
- 单流可能因 complete 延迟更差。
- 多流可能增加内存占用。

验收：

- proxy-1x1 高于当前 5.75 Gbps。
- proxy-4x16 不低于当前 always-pending 的 24.99 Gbps 太多。
- `ReceiveSetEnabled` pause/resume 不恶化。
- pending bytes 不出现持续堆积。

### P2：per-relay pending receive budget 调整

always-pending 对 pending receive 空间敏感。需要测试：

| per-relay budget | 预期 |
|------------------|------|
| 4MB | 当前保守基线附近 |
| 16MB | 减少 pause/resume |
| 32MB | 适合 20Gbps 单流试验 |
| 64MB | 压测 memory / flow control 上限 |

观察：

- 是否减少 `ReceiveSetEnabled(false)`。
- 是否提高单流。
- 是否损害多流公平性。
- 内存占用是否可控。

### P3：极窄 inline write fast path

always-pending 可以引入非常小的 inline 尝试，但不能变成 sync-write 分支那种“尽量写完”。

建议策略：

```text
if receive 非压缩 && TCP fd 可写:
  最多 1 次 sendmsg
  最多 64KB / 128KB
  full write 才返回 QUIC_STATUS_SUCCESS
  partial / EAGAIN / 超预算 -> queue deferred view + QUIC_STATUS_PENDING
```

目的：

- 单流短包或 TCP 空闲时绕过 pending/complete 往返。
- 保持 callback 时间可预测。
- 多流下仍以 worker pending path 为主。

必须记录：

- inline full success count
- inline partial count
- inline EAGAIN count
- inline budget exceeded count
- callback duration p50/p95/p99

### P4：UDP receive path 系统优化

当 proxy-4x16 已到 25 Gbps，`__arch_copy_to_user` 是共性热点。always-pending 分支下一步应单独调查：

- UDP_GRO 是否启用且生效
- NIC GRO/GSO/TSO/LRO/offload 状态
- RSS queue 数量
- IRQ affinity
- MsQuic worker 与 NIC IRQ 的 CPU 分布
- BBR vs CUBIC 的 CPU / throughput 差异
- 是否值得评估 MsQuic XDP / AF_XDP datapath

这部分不是 relay 层优化，不应和 pending receive 修改混在一个 commit。

## 不建议优先做的事

1. 在 tcpquic-proxy 里尝试设置 MsQuic UDP socket buffer：当前没有公开 fd / setting。
2. 修改 `DatagramReceiveEnabled` 期待改变 UDP buffer：它不是 socket buffer。
3. 大改 relay buffer pool 来解决 always-pending 多流：当前多流热点已转向 UDP copy。
4. 无预算 callback write：这会把 always-pending 变成 sync-write 的风险形态。

## 建议执行顺序

1. bench 脚本固化 sysctl 校验。
2. 加 TCP socket effective buffer 日志。
3. 加 always-pending pending/complete/sendmsg 指标。
4. 跑 `StreamReceiveComplete` 聚合矩阵。
5. 跑 per-relay pending budget 矩阵。
6. 实现极窄 inline write fast path。
7. 单独开任务调查 UDP_GRO / offload / RSS / affinity。

## 分支判断

always-pending 是当前多流主路径。它的优化目标不是替代 Phase 3 单流路径，而是在保持 4x16 / 16x16 优势的前提下，减少单流 pending completion 往返。

推荐目标：

```text
proxy-4x16: 保持接近 25 Gbps
proxy-1x1: 从 5-6 Gbps 明显上探
callback p99: 不出现长尾
UDP sysctl: bench 前强制满足
```
