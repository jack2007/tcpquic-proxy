# Phase 4 sync-write 分支 UDP Buffer 与优化建议

时间：2026-06-12

适用代码分支：

- 分支：`phase4-callback-sync-write`
- 代表 commit：`115eb06`
- 方案：callback 内同步 `sendmsg/writev` fast path；full write 返回 `QUIC_STATUS_SUCCESS`，partial / `EAGAIN` 后进入 pending view

本文从 `docs/dgx-msquic-udp-buffer-optimization-20260612.md` 拆分而来，只讨论 sync-write 分支的优化方向。通用结论仍成立：MsQuic 没有应用可配置的 UDP socket buffer knob；Linux epoll datapath会请求 `SO_RCVBUF = INT32_MAX`，实际值由 `net.core.rmem_max` 封顶；UDP `SO_SNDBUF` 未由 MsQuic 显式设置，发送侧依赖系统 `wmem_*`。

## 当前性能画像

sysctl 128MB 后：

| 指标 | sync-write |
|------|------------|
| proxy-1x1 | 5.01 Gbps |
| proxy-16x16 | 14.41 Gbps |
| proxy-4x16 | 22.68 Gbps |

对比 sysctl 前：

| 指标 | sysctl 前 | sysctl 后 |
|------|-----------|-----------|
| proxy-4x16 | 4.87 Gbps | 22.68 Gbps |

结论：

1. TCP socket buffer 太小是 sync-write 多流崩溃的重要外因。
2. sysctl 128MB 后，sync-write 多流接近 always-pending，但仍略低。
3. 单流仍只有约 5 Gbps，没有接近 Phase 3。
4. sync-write 的主要风险仍是 MsQuic callback 内同步写过重。

## 分支核心路径

sync-write 的 QUIC->TCP receive 路径是：

```text
MsQuic RECEIVE callback
  -> TryCompleteQuicReceiveInline()
  -> build iovec from QUIC receive buffers
  -> sendmsg(TCP fd)

if full write:
  return QUIC_STATUS_SUCCESS

if partial / EAGAIN:
  StreamReceiveComplete(written_prefix)
  QueueDeferredQuicReceive(remaining)
  return QUIC_STATUS_PENDING
```

该路径的优点：

- full write 时不需要 pending/worker/complete 往返。
- sysctl 足够大时，多流不再频繁因小 TCP send buffer 退化。
- 可作为 always-pending 的 inline fast path 候选。

该路径的弱点：

- 当前实现倾向于在 callback 内尽量写完整个 receive。
- TCP socket 可持续接受数据时，callback 会承担较多 `sendmsg` 工作。
- callback 时间变长会挤压 MsQuic worker 上其他 stream 的 receive、ACK、flow-control、send flush。
- 单流 perf 显示 TCP->QUIC 方向 buffer pool / `DrainTcpReadable` 仍是热点，sync-write 没有解决反方向 relay 成本。

## MsQuic UDP Buffer 结论

### UDP RX

MsQuic Linux epoll datapath创建 UDP socket 时请求：

```text
setsockopt(SOL_SOCKET, SO_RCVBUF, INT32_MAX)
```

但生效上限由：

```text
net.core.rmem_max
```

决定。因此 sync-write 的多流结果必须在固定 sysctl 后比较。否则小 UDP/TCP buffer 会放大 `EAGAIN`、packet backlog、调度抖动，让 callback 内同步写看起来比真实情况更差。

### UDP TX

MsQuic 没有设置 UDP `SO_SNDBUF`。sync-write 分支虽然最直接受 TCP `SO_SNDBUF` 影响，但 QUIC 发送侧仍依赖：

```text
net.core.wmem_default
net.core.wmem_max
```

这些系统值也应固定。

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

sync-write 分支尤其要记录 TCP effective buffer，因为它对 TCP send buffer 最敏感。

## sync-write 分支优化优先级

### P0：补 callback 与 sendmsg 可观测性

必须先知道 callback 内到底写了多少、写了多久、失败模式是什么。

建议新增：

- callback inline `sendmsg` 次数
- callback inline write bytes
- full inline success count
- partial inline count
- inline `EAGAIN` count
- inline budget exceeded count
- callback wall time p50/p95/p99
- 每次 `sendmsg` bytes 分布
- deferred fallback bytes
- `StreamReceiveComplete(written_prefix)` 次数和 bytes

TCP socket 调优后还应打印：

- requested `SO_RCVBUF`
- effective `SO_RCVBUF`
- requested `SO_SNDBUF`
- effective `SO_SNDBUF`
- `setsockopt` 失败 errno

没有这些指标时，不能判断 sync-write 是被 TCP buffer、callback 预算、iov 上限，还是 UDP path 限制。

### P1：callback write 预算化

当前 sync-write 的方向对，但边界太宽。应改为可配置预算：

```text
inline_sendmsg_max_calls = 1 或 2
inline_write_byte_budget = 64KB / 128KB / 256KB
```

行为：

```text
if full write within budget:
  return QUIC_STATUS_SUCCESS

if partial / EAGAIN / budget exceeded:
  StreamReceiveComplete(written_prefix) if written_prefix > 0
  queue remaining receive view
  return QUIC_STATUS_PENDING
```

禁止：

- callback 内长循环 `sendmsg`
- 为写完整个 receive 持续重试
- 在 TCP backpressure 下占住 MsQuic worker

建议矩阵：

| max calls | byte budget |
|-----------|-------------|
| 1 | 64KB |
| 1 | 128KB |
| 1 | 256KB |
| 2 | 128KB |
| 2 | 256KB |

验收：

- proxy-4x16 不显著低于 22.68 Gbps。
- proxy-16x16 不显著低于 14.41 Gbps。
- proxy-1x1 不低于当前 5.01 Gbps，并尝试上探。
- callback p99 明显受控。
- `EAGAIN` / partial 不随多流爆炸。

### P2：TCP socket buffer 应用层显式 tuning

sysctl 只放开上限，应用仍需要请求足够大的 TCP `SO_SNDBUF/SO_RCVBUF`。当前默认 tuning 对 20Gbps 偏保守：

| 模式 | 当前 TCP buffer |
|------|-----------------|
| default / WAN | 4MB |
| LAN | 1MB |
| BDP tuning | 最高 clamp 到 4MB |
| Custom high-BDP | 可提升到 16MB+ |

sync-write 分支建议优先测试：

| TCP socket buffer | 目的 |
|-------------------|------|
| 16MB | 最小 high-speed baseline |
| 32MB | 20Gbps 推荐起点 |
| 64MB | 验证多流和 callback EAGAIN |

观察：

- inline `EAGAIN` 是否下降
- partial write 是否下降
- callback p99 是否下降
- proxy-4x16 是否稳定
- proxy-1x1 是否改善

如果 32MB 与 64MB 差异很小，默认可选 32MB；如果 64MB 显著降低 callback 退化，则 DGX profile 应使用 64MB。

### P3：TCP->QUIC 方向 buffer pool 热点

sysctl 后 sync-write 单流 perf 显示：

```text
AcquireWorker / ReleaseWorker 约 21%
DrainTcpReadable 约 7.5%
```

这说明单流下热点不只在 QUIC->TCP receive fast path，反方向 TCP->QUIC 的 worker buffer pool 仍重。

建议：

1. 复用 worker 内 `std::vector<iovec>` / refs scratch，减少每轮构造成本。
2. 测 `ReadChunkSize=128KB`，减少 buffer handle 数。
3. 测 `MaxIov=32/64`，减少 syscall 次数。
4. 统计 `TcpReadBytes / TcpReadBatches`、`TcpWriteBytes / TcpWriteBatches`。
5. 检查 `AcquireWorker/ReleaseWorker` 是否主要是 atomic pending bytes 计数。

测试矩阵：

| ReadChunkSize | MaxIov | ReadBatchBytes |
|---------------|--------|----------------|
| 64KB | 32 | 4MB |
| 64KB | 64 | 4MB |
| 128KB | 32 | 4MB |
| 128KB | 64 | 8MB |

### P4：fallback pending path 与 always-pending 对齐

sync-write partial / `EAGAIN` 后仍会进入 deferred receive view。该 slow path 应与 always-pending 保持一致：

- pending bytes accounting 一致
- `ReceiveSetEnabled(false/true)` 逻辑一致
- `StreamReceiveComplete` 粒度一致
- FIN 顺序一致
- worker `sendmsg` 聚合逻辑一致

否则预算化后，sync-write 的 fallback path 会成为新的不稳定来源。

### P5：UDP receive path 系统优化

sync-write 多流调优后也进入 20Gbps+ 区间，Phase 4 共性瓶颈同样会转向 UDP kernel copy。

建议单独调查：

- UDP_GRO 是否启用且生效
- NIC GRO/GSO/TSO/LRO/offload 状态
- RSS queue 数量
- IRQ affinity
- MsQuic worker 与 NIC IRQ 的 CPU 分布
- BBR vs CUBIC 的 CPU / throughput 差异
- MsQuic XDP / AF_XDP datapath 可行性

这部分不应和 callback budget 化混在一个任务里。

## 不建议优先做的事

1. 继续扩大无预算 sync-write 循环：sysctl 后多流恢复不代表 callback 无限写是正确结构。
2. 通过 `DatagramReceiveEnabled` 调 UDP buffer：它不是 socket buffer。
3. 在 tcpquic-proxy 层直接控制 MsQuic UDP fd：当前没有公开接口。
4. 在没有 callback p99 和 `EAGAIN` 指标前判断 sync-write 是否优于 always-pending。

## 建议执行顺序

1. bench 脚本固化 sysctl 校验。
2. 加 TCP socket requested/effective buffer 日志。
3. 加 sync-write callback/sendmsg 指标。
4. 跑 TCP socket buffer 16MB / 32MB / 64MB 矩阵。
5. 实现 callback write 预算化。
6. 跑 inline budget 矩阵。
7. 压 TCP->QUIC buffer pool 热点。
8. 与 always-pending 对齐 fallback pending path。
9. 单独开任务调查 UDP_GRO / offload / RSS / affinity。

## 分支判断

sync-write 在 sysctl 128MB 后不再是“多流不可用”，但它仍不适合作为无预算主路径。它更适合成为 always-pending 的受控 inline fast path。

推荐目标：

```text
proxy-4x16: 接近 always-pending，不低于 20Gbps
proxy-1x1: 不低于当前 5Gbps，并尝试改善
callback p99: 明确受预算控制
EAGAIN/partial: 可观测、可比较
TCP socket buffer: requested/effective 都记录
```
