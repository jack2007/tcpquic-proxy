# Phase 4 sync-write 分支 UDP Buffer 与优化建议

时间：2026-06-12

适用代码分支：

- 分支：`phase4-callback-sync-write`
- 基线 commit：`115eb06`
- 本次实现：当前 worktree 未提交改动（P0/P1/P2/P4 部分落地）
- 方案：callback 内受预算控制的同步 `sendmsg/writev` fast path；full write within budget 返回 `QUIC_STATUS_SUCCESS`；budget exceeded 不再先写前缀，直接整段进入 pending view；partial / `EAGAIN` fallback 的前缀 complete 延后到 worker 事件处理阶段

本文从 `docs/dgx-msquic-udp-buffer-optimization-20260612.md` 拆分而来，只讨论 sync-write 分支的优化方向。通用结论仍成立：MsQuic 没有应用可配置的 UDP socket buffer knob；Linux epoll datapath会请求 `SO_RCVBUF = INT32_MAX`，实际值由 `net.core.rmem_max` 封顶；UDP `SO_SNDBUF` 未由 MsQuic 显式设置，发送侧依赖系统 `wmem_*`。

## 本次代码更新状态

已在 `phase4-callback-sync-write` worktree 落地的代码项：

1. bench 前置条件 fail-fast：`scripts/dgx-msquic-vs-proxy-bench.sh` 与 `scripts/bench-tcpquic-proxy-dgx.sh` 会校验本机和对端 `net.core.rmem_max`、`net.core.wmem_max`、`net.ipv4.tcp_rmem` max、`net.ipv4.tcp_wmem` max。
2. TCP socket buffer requested/effective 记录：`TqTuneTcpForThroughput()` 设置 `SO_RCVBUF`/`SO_SNDBUF` 后读取 effective 值，并打印 requested、effective、errno。
3. sync-write inline 指标：worker snapshot 与 admin metrics 暴露 inline `sendmsg` calls、inline write bytes、full success、partial、`EAGAIN`、budget exceeded、fallback bytes、callback count/avg/max usec。
4. callback write 预算化：默认 `inline_sendmsg_max_calls=1`、`inline_write_byte_budget=128KB`；新增 CLI `--inline-sendmsg-max-calls` 与 `--inline-write-byte-budget` 用于矩阵测试。
5. fallback path：budget exceeded、partial、`EAGAIN` 统一进入现有 `QueueDeferredQuicReceive()` view，由 worker path 继续 `sendmsg`、`StreamReceiveComplete()` 与 pause/resume accounting。
6. fallback pause/resume 修复：不再每次 deferred receive 都无条件 `ReceiveSetEnabled(false)`；改为达到 `MaxPendingQuicReceiveBytesPerRelay` / relay pending 阈值后才暂停，避免单连接 receive pipeline 被串行化。

仍未由代码更新本身证明的事项：

- 更长时间 DGX bench 和预算矩阵仍建议继续跑；本次已完成 10s smoke bench 与 10s perf。
- callback p50/p95/p99 目前未实现直方图分位，只记录 count/avg/max usec；如需 p99，应增加 histogram 或 trace 后处理。
- P3 buffer pool 热点与 P5 UDP_GRO/offload/RSS/affinity 仍是后续性能实验，不应和本次 callback budget 化混为一个结论。

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

本次修复后 10s DGX smoke bench（spark-1619 -> spark-1b6f，512MB payload，sysctl 128MB）：

| 指标 | sync-write fixed |
|------|------------------|
| direct TCP | 33.680 Gbps |
| secnetperf 1conn | 12.767 Gbps |
| secnetperf 1conn 16stream | 16.129 Gbps |
| proxy-1x1 | 11.229 Gbps |
| proxy-16x16 | 17.081 Gbps |
| proxy-4x16 | 29.553 Gbps |

复测说明：

- 修复前同一 WIP 默认 `inline_calls=1` / `inline_bytes=128KB` 的 proxy-1x1 约 0.088-0.089 Gbps。
- 单变量实验把 inline budget 放大到 16MB 后，1x1 立即恢复，证明故障触发条件是 budget fallback path。
- 修复后默认 128KB budget 下，1x1 不再卡尾；10s perf 吞吐 `speed_download=900306734` B/s，client 热点回到 UDP `__arch_copy_to_user`、AES-GCM 和本地 TCP `__arch_copy_from_user`，不再是 perf 样本极少的 idle 状态。

### 1x1 inline byte budget 单轮矩阵（2026-06-12）

测试条件：

- 拓扑：spark-1619 client -> spark-1b6f server
- 模式：`tunnel_off`，`QUIC_CONNECTIONS=1`，`DURATION_SEC=10`
- payload：实际下载 `1,572,864,208` bytes
- 两端均开启 admin metrics：`--admin-listen 127.0.0.1:19091`
- 两端均设置同一个 `--inline-write-byte-budget`
- metrics 文件：`/tmp/dgx-client-admin-metrics/{client,server}-<budget>-tunnel_off-1.json`

client QUIC receive -> TCP write 指标：

| inline budget | 吞吐 | inline bytes | inline % | defer bytes | defer % | callbacks | full | budget exceeded | partial/EAGAIN | avg/max |
|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| 64KB | 5518.64 Mbps | 67,308,951 | 4.28% | 1,505,555,257 | 95.72% | 12,207 | 9,842 | 2,365 | 0/0 | 12us / 971us |
| 128KB | 7340.00 Mbps | 188,385,996 | 11.98% | 1,384,478,212 | 88.02% | 12,987 | 10,440 | 2,547 | 0/0 | 10us / 678us |
| 192KB | 7400.77 Mbps | 381,786,579 | 24.27% | 1,191,077,629 | 75.73% | 14,245 | 12,514 | 1,731 | 0/0 | 7us / 740us |
| 256KB | 9552.45 Mbps | 380,418,999 | 24.19% | 1,192,445,209 | 75.81% | 13,296 | 11,217 | 2,079 | 0/0 | 16us / 940us |

client receive callback `totalLength` 分布：

| budget | <=16K | <=64K | <=128K | <=256K | <=1M | >1M |
|---:|---:|---:|---:|---:|---:|---:|
| 64KB | 8,971 | 871 | 330 | 364 | 1,203 | 468 |
| 128KB | 8,160 | 1,736 | 544 | 619 | 1,707 | 221 |
| 192KB | 7,474 | 4,422 | 363 | 463 | 1,176 | 347 |
| 256KB | 7,853 | 1,758 | 964 | 642 | 1,978 | 101 |

server TCP -> QUIC 指标：

| budget | tcp_read_bytes | tcp_write_bytes | inline write | partial/EAGAIN/budget |
|---:|---:|---:|---:|---:|
| 64KB | 1,572,864,208 | 106 | 106 | 0/0/0 |
| 128KB | 1,572,864,208 | 106 | 106 | 0/0/0 |
| 192KB | 1,572,864,208 | 106 | 106 | 0/0/0 |
| 256KB | 1,572,864,208 | 106 | 106 | 0/0/0 |

server QUIC send batch bytes 分布：

| budget | <=16K | <=64K | <=128K | <=256K | <=1M | >1M |
|---:|---:|---:|---:|---:|---:|---:|
| 64KB | 4 | 42 | 125 | 174 | 1,491 | 0 |
| 128KB | 11 | 24 | 87 | 175 | 1,474 | 0 |
| 192KB | 3 | 17 | 3 | 2 | 1,505 | 0 |
| 256KB | 13 | 21 | 68 | 74 | 1,520 | 0 |

说明与限制：

- QUIC stream 是字节流，server `Stream->Send()` 次数和 client RECEIVE callback 次数不是 1:1；send batch 分布与 receive callback `totalLength` 分布需要分别观察。
- client inline budget 不直接改变 server TCP 读入总字节数；它改变 client receive consume / `StreamReceiveComplete()` 节奏，间接影响 QUIC flow-control、send complete 与后续 receive slice 形态。
- 本表是单轮结果，DGX 单流吞吐存在波动；默认值决策应对 128KB / 192KB / 256KB 各跑 3 次取中位数。

## 分支核心路径

sync-write 的 QUIC->TCP receive 路径是：

```text
MsQuic RECEIVE callback
  -> TryCompleteQuicReceiveInline()
  -> build iovec from QUIC receive buffers
  -> sendmsg(TCP fd)

if full write:
  return QUIC_STATUS_SUCCESS

if budget exceeded before write:
  QueueDeferredQuicReceive(full receive)
  return QUIC_STATUS_PENDING

if partial / EAGAIN after prefix write:
  QueueDeferredQuicReceive(remaining, completed_prefix)
  worker later calls StreamReceiveComplete(completed_prefix)
  return QUIC_STATUS_PENDING
```

该路径的优点：

- full write 时不需要 pending/worker/complete 往返。
- sysctl 足够大时，多流不再频繁因小 TCP send buffer 退化。
- 可作为 always-pending 的 inline fast path 候选。

该路径的弱点：

- 早期实现倾向于在 callback 内尽量写完整个 receive；当前实现已加 `InlineSendmsgMaxCalls` 与 `InlineWriteByteBudget` 边界。
- TCP socket 可持续接受数据时，callback 仍会承担 inline `sendmsg` 工作，但现在最多按配置预算执行。
- callback 时间变长会挤压 MsQuic worker 上其他 stream 的 receive、ACK、flow-control、send flush。
- 单流 perf 显示 TCP->QUIC 方向 buffer pool / `DrainTcpReadable` 仍是热点，sync-write 没有解决反方向 relay 成本。
- 当前还存在一个待修顺序风险：若 callback N 超预算后仅 enqueue deferred receive，callback N+1 又在 N 的 deferred view 被 worker 写入 TCP 前 inline full write 成功，则 N+1 的 bytes 可能先于 N 写入同一个 TCP fd。现有 FIFO 只保证 deferred view 之间的顺序，不能保证“前一个 deferred event”早于“后一个 inline write”。

建议修复：

```text
relay has deferred receive queued / pending / flushing
  -> later RECEIVE callbacks must not inline
  -> queue full receive view and return PENDING
```

也就是增加 per-relay deferred ordering barrier，例如 `OutstandingDeferredReceives` / `QuicReceiveOrderingBarrier`。`QueueDeferredQuicReceive()` 成功 enqueue 前同步置位或递增；worker 完整写完对应 view 并 `pop_front()` 后清除或递减。`TryCompleteQuicReceiveInline()` 入口看到 barrier 时直接走 full deferred fallback。该修复需要单测覆盖“第一个 callback 超预算 deferred，第二个 callback 本可 inline”的场景，验证 TCP 输出顺序仍是 `first + second`。

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

bench 脚本已 fail-fast 校验：

```text
net.core.rmem_max >= 134217728
net.core.wmem_max >= 134217728
net.ipv4.tcp_rmem max >= 134217728
net.ipv4.tcp_wmem max >= 134217728
```

sync-write 分支现在会记录 TCP requested/effective buffer，因为它对 TCP send buffer 最敏感。

## sync-write 分支优化优先级

### P0：补 callback 与 sendmsg 可观测性

必须先知道 callback 内到底写了多少、写了多久、失败模式是什么。

当前已新增/仍建议补充：

- [已实现] callback inline `sendmsg` 次数
- [已实现] callback inline write bytes
- [已实现] full inline success count
- [已实现] partial inline count
- [已实现] inline `EAGAIN` count
- [已实现] inline budget exceeded count
- [部分实现] callback wall time count/avg/max usec；p50/p95/p99 需 histogram 或 trace 后处理
- [已实现] TCP->QUIC send batch bytes bucket 分布（admin metrics `linux_relay_quic_send_bytes_buckets`）
- [已实现] QUIC receive callback `totalLength` bucket 分布（admin metrics `linux_relay_quic_receive_bytes_buckets`）
- [已实现] deferred fallback bytes
- [已实现] `StreamReceiveComplete(written_prefix)` 次数和 bytes（沿用 deferred complete counters）

TCP socket 调优后已打印：

- [已实现] requested `SO_RCVBUF`
- [已实现] effective `SO_RCVBUF`
- [已实现] requested `SO_SNDBUF`
- [已实现] effective `SO_SNDBUF`
- [已实现] `setsockopt` 失败 errno

缺少 callback 分位数时，仍不能完全判断 sync-write 的 p99 尾延迟；send/receive bytes 分布已由 bucket metrics 覆盖。

### P1：callback write 预算化

当前 sync-write 已改为可配置预算，默认值为 `1` 次 `sendmsg` 与 `128KB` inline byte budget：

```text
inline_sendmsg_max_calls = 1（默认，可通过 `--inline-sendmsg-max-calls` 覆盖为 1 或 2）
inline_write_byte_budget = 128KB（默认，可通过 `--inline-write-byte-budget` 覆盖为 64KB / 128KB / 192KB / 256KB）
```

当前行为：

```text
if full write within budget:
  return QUIC_STATUS_SUCCESS

if budget exceeded before write:
  queue full receive view
  return QUIC_STATUS_PENDING

if partial / EAGAIN after prefix write:
  queue remaining receive view with completed_prefix
  worker completes completed_prefix after callback returns
  return QUIC_STATUS_PENDING
```

当前实现约束：

- 默认禁止 callback 内长循环 `sendmsg`（默认 max calls=1）
- 不为写完整个 receive 持续重试，超预算直接 pending fallback，且不先 inline 写前缀
- TCP backpressure 下通过 `EAGAIN`/partial/budget exceeded 退到 worker path
- deferred receive pause 按 pending 阈值触发，不再每个 fallback 都暂停 QUIC receive

当前未完成的约束：

- deferred ordering barrier 尚未实现。当前代码隐含依赖“同一 stream 返回 `QUIC_STATUS_PENDING` 后，MsQuic 不会在 `ReceiveComplete()` 前继续交付后续 RECEIVE callback”；在启用 `StreamMultiReceiveEnabled` 后，这不应作为唯一顺序保证。
- 在 barrier 实现前，sync-write fast path 不应作为最终合并版本；预算矩阵结果只能用于性能画像和定位，不代表顺序语义已闭环。

仍建议跑的验证矩阵：

| max calls | byte budget |
|-----------|-------------|
| 1 | 64KB |
| 1 | 128KB |
| 1 | 192KB |
| 1 | 256KB |
| 2 | 128KB |
| 2 | 192KB |
| 2 | 256KB |

验收仍需 DGX bench 验证：

- proxy-4x16 不显著低于 22.68 Gbps。
- proxy-16x16 不显著低于 14.41 Gbps。
- proxy-1x1 不低于当前 5.01 Gbps，并尝试上探。
- callback avg/max 明显受控；p99 需 histogram 或 trace 后处理。
- `EAGAIN` / partial 不随多流爆炸。

### P2：TCP socket buffer 应用层显式 tuning

sysctl 只放开上限，应用仍需要请求足够大的 TCP `SO_SNDBUF/SO_RCVBUF`。当前默认 tuning 对 20Gbps 偏保守：

| 模式 | 当前 TCP buffer |
|------|-----------------|
| default / WAN | 4MB |
| LAN | 1MB |
| BDP tuning | 最高 clamp 到 4MB |
| Custom high-BDP | 可提升到 16MB+ |

当前代码已支持 requested/effective 日志；sync-write 分支仍建议优先测试：

| TCP socket buffer | 目的 |
|-------------------|------|
| 16MB | 最小 high-speed baseline |
| 32MB | 20Gbps 推荐起点 |
| 64MB | 验证多流和 callback EAGAIN |

观察 admin metrics 与 stderr 日志：

- inline `EAGAIN` 是否下降
- partial write 是否下降
- callback avg/max 是否下降；p99 需 histogram 或 trace 后处理
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

本次 88Mbps 根因就在这里：budget fallback 后每个 deferred receive 都无条件暂停 QUIC receive，单连接 pipeline 被串行化；同时早期路径会在 callback 返回前 complete 已写前缀。当前修复为：

- budget exceeded 在写 TCP 前整段 deferred。
- partial fallback 的 prefix complete 延后到 worker 事件处理阶段。
- pause/resume 改为按 pending byte 阈值触发。

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
4. 在没有 DGX bench、callback p99 后处理和 `EAGAIN`/partial/budget 指标对比前判断 sync-write 是否优于 always-pending。

## 建议执行顺序

1. [DONE] bench 脚本固化 sysctl 校验。
2. [DONE] 加 TCP socket requested/effective buffer 日志。
3. [DONE] 加 sync-write callback/sendmsg 指标。
4. [TODO] 跑 TCP socket buffer 16MB / 32MB / 64MB 矩阵。
5. [DONE] 实现 callback write 预算化。
6. [PARTIAL] 默认 128KB budget 已通过 10s DGX smoke bench；完整 inline budget 矩阵仍需跑。
7. [TODO] 压 TCP->QUIC buffer pool 热点。
8. [DONE] fallback 已统一走 deferred receive view，并修复 budget fallback pause/resume 串行化问题；仍可继续做更长时间对比。
9. [TODO] 单独开任务调查 UDP_GRO / offload / RSS / affinity。

## 分支判断

sync-write 在 sysctl 128MB 后不再是“多流不可用”，但它仍不适合作为无预算主路径。它更适合成为 always-pending 的受控 inline fast path。

推荐目标：

```text
proxy-4x16: 接近 always-pending，不低于 20Gbps
proxy-1x1: 不低于当前 5Gbps，并尝试改善
callback latency: 当前记录 avg/max usec；p99 需 histogram/trace 后处理
EAGAIN/partial/budget-exceeded: 可观测、可比较
TCP socket buffer: requested/effective 都记录
```
