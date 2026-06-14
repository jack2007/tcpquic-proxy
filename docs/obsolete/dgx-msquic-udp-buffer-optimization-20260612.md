# DGX Phase 4 MsQuic UDP Buffer 与后续优化建议

时间：2026-06-12

本文基于 sysctl 128MB buffer 后的 DGX Phase 3 / Phase 4 对比，以及 MsQuic Linux datapath 代码调查整理。重点是厘清：MsQuic UDP socket buffer 是否可配置、sysctl 为什么会显著影响 Phase 4、多流 `__arch_copy_to_user` 热点下一步应从哪里优化。

## 结论摘要

1. MsQuic 没有面向应用的 UDP `SO_RCVBUF` / `SO_SNDBUF` setting 或 env。
2. Linux epoll datapath 创建 UDP socket 时会硬编码请求 `SO_RCVBUF = INT32_MAX`，但实际生效值仍受 `net.core.rmem_max` 限制。
3. MsQuic 没有设置 UDP `SO_SNDBUF`，发送侧主要依赖系统 `wmem_default` / `wmem_max`。
4. `DatagramReceiveEnabled`、`StreamRecvBufferDefault`、`SendBufferingEnabled` 等都是 QUIC/TLS/stream 层 setting，不是内核 UDP socket buffer。
5. Phase 4 多流 perf 中 `__arch_copy_to_user` 达到 22-24% 时，瓶颈已经靠近 MsQuic UDP 收包路径；继续只优化 relay 层 copy 的收益会下降。
6. DGX bench 应把 `rmem_max/wmem_max >= 128MB` 固化为前置条件；否则 MsQuic 虽请求大 UDP RX buffer，内核仍会截断。

## 已确认的 MsQuic 行为

### Linux UDP RX Buffer

MsQuic Linux epoll datapath 在创建 UDP socket 时会增加 receive buffer：

```text
third_party/msquic/src/platform/datapath_epoll.c
  setsockopt(SocketFd, SOL_SOCKET, SO_RCVBUF, INT32_MAX)
```

含义：

- 应用无法通过 `QUIC_SETTINGS`、环境变量或 `CXPLAT_UDP_CONFIG` 调整这个值。
- MsQuic 已经请求了最大值。
- Linux 内核会用 `net.core.rmem_max` 封顶。
- `getsockopt(SO_RCVBUF)` 通常会看到内核 bookkeeping 后的值，不应简单按用户传入值判断。

因此，sysctl 不是可选微调，而是让 MsQuic 的 `SO_RCVBUF = INT32_MAX` 真正拿到大 buffer 的必要条件。

### Linux UDP TX Buffer

调查结果显示 MsQuic 没有设置 UDP `SO_SNDBUF`。这意味着 UDP 发送侧更依赖：

- `net.core.wmem_default`
- `net.core.wmem_max`
- 内核 UDP / qdisc / NIC 队列调度

Phase 4 sync-write 在 sysctl 后多流显著恢复，主要解释仍在 TCP socket buffer；但 UDP TX buffer 也应纳入 DGX 标准 sysctl，因为 MsQuic 没有内部 knob 弥补系统默认值偏小的问题。

### 不相关但容易混淆的 Settings

| Setting | 实际含义 | 是否 UDP socket buffer |
|---------|----------|------------------------|
| `StreamRecvBufferDefault` | QUIC stream 层接收缓冲 | 否 |
| `StreamMultiReceiveEnabled` | 允许多个 pending receive | 否 |
| `DatagramReceiveEnabled` | QUIC Datagram 扩展 | 否 |
| `SendBufferingEnabled` | MsQuic 是否 copy 应用 send buffer | 否 |
| `TlsClientMaxSendBuffer` / `TlsServerMaxSendBuffer` | TLS 层缓冲 | 否 |

tcpquic-proxy 当前的 `TqSetSocketBuffer()` 只覆盖 TCP socket，不覆盖 MsQuic 内部 UDP socket。

## 与 Phase 4 性能现象的对应关系

sysctl 128MB 后的关键变化：

| 指标 | always-pending | sync-write |
|------|----------------|------------|
| proxy-1x1 | 5.75 Gbps | 5.01 Gbps |
| proxy-16x16 | 15.61 Gbps | 14.41 Gbps |
| proxy-4x16 | 24.99 Gbps | 22.68 Gbps |

sync-write 多流从调优前的 4.87 Gbps 提升到 22.68 Gbps，说明之前的多流崩溃并不只来自 callback 内同步写本身，还包含 TCP socket send buffer 太小导致的频繁 `EAGAIN` / partial write。

但单流排序没有改变：Phase 3 仍约为 Phase 4 的 2 倍。这说明 Phase 4 单流主要瓶颈仍是 receive completion cadence：

```text
MsQuic callback
  -> QUIC_STATUS_PENDING
  -> relay worker sendmsg
  -> StreamReceiveComplete
  -> MsQuic 继续推进 receive
```

多流下，always-pending 能用多个 stream / connection 填满 completion 往返空隙；单流下这个往返直接限制流水线。

多流 Phase 4 perf 中 `__arch_copy_to_user` 达到 22-24%，表示当前热点已经转向：

```text
NIC / kernel UDP receive
  -> recvmmsg / UDP_GRO
  -> copy_to_user
  -> MsQuic packet processing
```

这类开销不在 tcpquic-proxy relay 层，不能靠减少 relay memcpy 直接消除。

## 标准 sysctl 前置条件

DGX 20Gbps+ bench 建议固定：

```bash
sudo sysctl -w net.core.rmem_max=134217728
sudo sysctl -w net.core.wmem_max=134217728
sudo sysctl -w net.core.rmem_default=4194304
sudo sysctl -w net.core.wmem_default=4194304
sudo sysctl -w 'net.ipv4.tcp_rmem=4096 1048576 134217728'
sudo sysctl -w 'net.ipv4.tcp_wmem=4096 1048576 134217728'
```

bench 脚本应在启动前校验：

```text
net.core.rmem_max >= 134217728
net.core.wmem_max >= 134217728
net.ipv4.tcp_rmem max >= 134217728
net.ipv4.tcp_wmem max >= 134217728
```

如果条件不满足，应明确 warning 或 fail fast。否则不同 run 的 Phase 4 结果不可比。

## 后续优化建议

### P0：增加可观测性

这是下一步最应该先做的部分。

1. TCP socket 调优后记录实际值：
   - `getsockopt(SO_RCVBUF)`
   - `getsockopt(SO_SNDBUF)`
   - requested vs effective
   - `setsockopt` 失败 errno
2. bench 脚本记录 sysctl 快照：
   - `net.core.rmem_max`
   - `net.core.wmem_max`
   - `net.core.rmem_default`
   - `net.core.wmem_default`
   - `net.ipv4.tcp_rmem`
   - `net.ipv4.tcp_wmem`
3. Phase 4 relay 增加写路径指标：
   - `sendmsg` 次数
   - 每次 `sendmsg` bytes 分布
   - `EAGAIN` 次数
   - partial write 次数
   - `MaxTcpWriteIovUsed`
   - `StreamReceiveComplete` 次数与平均 bytes
   - callback inline write 次数和 bytes
   - callback wall time p50/p95/p99

没有这些指标时，很难区分是 socket buffer 不够、iov 太小、callback 过长，还是 MsQuic UDP 路径饱和。

### P1：固化 TCP Buffer Tuning

当前 `TqTuneTcpForThroughput()` 会设置 TCP `SO_RCVBUF` / `SO_SNDBUF`，但默认 tuning 对 20Gbps 偏保守：

| 模式 | 当前 TCP buffer |
|------|-----------------|
| default / WAN | 4MB |
| LAN | 1MB |
| BDP tuning | 最高 clamp 到 4MB |
| Custom high-BDP | 可提升到 16MB+ |

建议：

1. 为 DGX / 20Gbps 场景增加明确 profile，默认 TCP socket buffer 为 32MB 或 64MB。
2. `ApplyBdpTuning()` 的 TCP buffer 上限不应固定为 4MB。
3. 启动日志输出 `tcp_buf_requested` 和 `tcp_buf_effective`。
4. 若 effective 明显小于 requested，应提示检查 `net.core.rmem_max/wmem_max`。

### P2：Phase 4 always-pending 单流优化

always-pending 多流最优，单流弱。优化目标不是再减少 relay copy，而是减少 pending completion 往返。

可测试方向：

1. `StreamReceiveComplete` 聚合：
   - 当前每次 `sendmsg` 成功后立即 complete 对应 bytes。
   - 可测试按 256KB / 512KB / 1MB 聚合 complete。
   - 风险是过度延迟 complete 会压住 MsQuic flow control。
2. 增大 per-relay pending receive budget：
   - 减少 `ReceiveSetEnabled(false/true)` 抖动。
   - 需要观察 pending bytes 与内存占用。
3. 极窄 inline write：
   - callback 内最多 1 次 `sendmsg`。
   - full write 才返回 `QUIC_STATUS_SUCCESS`。
   - partial / `EAGAIN` / 超预算立即进入 pending view。

验收标准：

- proxy-1x1 明显高于当前 5-6 Gbps。
- proxy-4x16 / proxy-16x16 不明显低于 always-pending 当前水平。
- callback p99 不出现长尾。
- `StreamReceiveComplete` 平均 bytes 上升，但 pause/resume 不恶化。

### P3：Phase 4 sync-write 预算化

sysctl 后 sync-write 多流恢复，说明它可以保留为候选 fast path。但不能让 callback 无限制写完整个 receive。

建议实现为可配置预算：

```text
inline_sendmsg_max_calls = 1 或 2
inline_write_byte_budget = 64KB / 128KB / 256KB
```

行为：

```text
if full write within budget:
  return QUIC_STATUS_SUCCESS
else:
  StreamReceiveComplete(written_prefix) if needed
  queue remaining receive view
  return QUIC_STATUS_PENDING
```

需要避免：

- callback 内长循环 `sendmsg`
- 为写完整个 receive 持续重试
- 在 TCP backpressure 下占住 MsQuic worker

调参矩阵：

| max calls | byte budget |
|-----------|-------------|
| 1 | 64KB |
| 1 | 128KB |
| 1 | 256KB |
| 2 | 128KB |
| 2 | 256KB |

每组至少跑：

- proxy-1x1 bench + perf
- proxy-4x16 bench + perf
- `EAGAIN` / partial / callback p99 指标

### P4：TCP 应用层 batch 参数

当前 TCP 读写已经使用聚合接口：

- TCP read：`readv`
- TCP write：`sendmsg` with `iovec`
- TCP->QUIC：一次 `Stream::Send()` 提交多个 `QUIC_BUFFER`

现有默认：

| 参数 | 默认 | high-BDP |
|------|------|----------|
| `LinuxRelayReadChunkSize` | 64KB | 64KB |
| `LinuxRelayMaxIov` | 16 | 32 |
| 单次 readv 理论上限 | 1MB | 2MB |
| `LinuxRelayReadBatchBytes` | 1MB | 4MB |

建议测试：

| ReadChunkSize | MaxIov | ReadBatchBytes |
|---------------|--------|----------------|
| 64KB | 32 | 4MB |
| 64KB | 64 | 4MB |
| 128KB | 32 | 4MB |
| 128KB | 64 | 8MB |

观察：

- `TcpReadBatches`
- `TcpReadBytes / TcpReadBatches`
- `TcpWriteBytes / TcpWriteBatches`
- `MaxTcpReadIovUsed`
- `MaxTcpWriteIovUsed`
- buffer pool `AcquireWorker` / `ReleaseWorker`

如果 iov 经常打满，增大 `MaxIov` 可能有效；如果每次 syscall bytes 已经很大，继续增大 batch 可能只会增加延迟和内存占用。

### P5：MsQuic UDP 收包路径

当 Phase 4 多流达到 20Gbps+ 后，`__arch_copy_to_user` 是共性瓶颈。可讨论方向：

1. 确认 UDP_GRO 是否启用且实际生效。
2. 确认 NIC GRO/GSO/TSO/LRO/offload 配置。
3. 检查 RSS 队列、IRQ affinity、MsQuic worker CPU 亲和性。
4. 比较 BBR / CUBIC 对 CPU 与吞吐的影响。
5. 评估 MsQuic XDP / AF_XDP datapath，但这属于更大工程，不应和 relay fast path 混在一个任务里做。

这部分优化的目标不是减少 tcpquic-proxy relay 开销，而是减少或绕过内核 UDP 到用户态的 copy 成本。

## 建议的执行顺序

1. 固化 sysctl 并在 bench 脚本 fail-fast 校验。
2. 增加 TCP socket requested/effective buffer 日志。
3. 增加 Phase 4 `sendmsg` / `EAGAIN` / partial / complete / callback duration 指标。
4. 跑 TCP buffer 应用层调参矩阵：16MB / 32MB / 64MB。
5. 跑 relay batch 参数矩阵：`MaxIov`、`ReadChunkSize`、`ReadBatchBytes`。
6. always-pending 测 `StreamReceiveComplete` 聚合。
7. sync-write 改为有限预算 inline fast path。
8. 另起任务调查 UDP_GRO / offload / RSS / affinity / XDP。

## 当前架构判断

基于 sysctl 后结果，推荐方向仍是：

```text
多流主路径：always-pending
单流优化：Phase 3 短 callback 思路 + Phase 4 selective zero-copy
sync-write：保留为预算化 inline fast path，不能作为无预算主路径
系统前置：128MB socket sysctl 必须固化
```

Phase 4 的瓶颈已经分层：

| 层级 | 现象 | 优化方式 |
|------|------|----------|
| TCP socket buffer | sync-write 多流对 sysctl 极敏感 | sysctl + app requested/effective buffer |
| QUIC receive completion | always-pending 单流弱 | complete batching / narrow inline fast path |
| relay worker buffer pool | sync-write 单流 `Acquire/Release` 高 | buffer handle / scratch reuse / batch tuning |
| UDP kernel copy | Phase 4 多流 `__arch_copy_to_user` 高 | GRO/GSO/offload/RSS/affinity/XDP |

这几个问题不应混成一个“大优化”。下一步应先补可观测性，再按层分别做小矩阵实验。
