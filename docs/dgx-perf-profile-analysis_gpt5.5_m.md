# Phase 3 / Phase 4 DGX 性能差异分析

时间：2026-06-12

本文基于以下对比数据和代码路径整理：

- `docs/dgx-perf-profile-phase4-comparison-20260612/summary.md`
- `docs/dgx-perf-profile-analysis.md`
- `docs/finished/superpowers/plans/2026-06-11-relay-perf-hotspot-optimization.md`
- Phase 3：`18f5f58`，MPMC event queue，receive 仍 copy 到 ingress buffer
- Phase 4 always-pending：`efcedd6`，deferred receive view + always `QUIC_STATUS_PENDING` + `StreamMultiReceiveEnabled`
- Phase 4 sync-write：`115eb06`，callback 内同步 `sendmsg/writev` fast path

## 现象摘要

| 指标 | Phase 3 baseline | Phase 4 always-pending | Phase 4 sync-write |
|------|------------------|------------------------|---------------------|
| 裸 TCP | 22.47 Gbps | 18.88 Gbps | 19.28 Gbps |
| proxy-1x1 | 14.09 Gbps | 4.37 Gbps | 4.83 Gbps |
| proxy / 裸 TCP | 62.7% | 23.2% | 25.1% |
| proxy-16x16 | 8.25 Gbps | 16.60 Gbps | 2.51 Gbps |
| proxy-4x16 | 10.18 Gbps | 23.18 Gbps | 4.87 Gbps |

结论先行：

1. Phase 3 单流快，因为 MsQuic callback 很短，实际 TCP `writev` 在 relay worker 线程执行。
2. Phase 3 多流弱，因为每条 receive 仍要 copy、buffer acquire/transfer、event queue、worker flush；并发放大后 relay CPU 成本上升。
3. Phase 4 always-pending 多流强，因为 QUIC->TCP receive payload copy 消失，multi-receive 允许多个 pending receive 并行存在。
4. Phase 4 always-pending 单流弱，因为每次 receive 都走 `PENDING -> worker -> write -> StreamReceiveComplete` 的异步完成链路，单流流水线被 receive completion 往返限制。
5. Phase 4 sync-write 当前不是足够窄的 fast path；它会在 MsQuic callback 中循环写到本次 receive 完成或遇到 `EAGAIN`，多流时 callback 线程被 TCP 写工作占住。

## Phase 3 为什么单流快

Phase 3 的 QUIC receive 路径是：

```text
MsQuic callback
  -> CopyQuicReceiveBatchToEvent()
  -> copy payload 到 ingress buffer
  -> Enqueue(QuicReceive)
  -> return QUIC_STATUS_SUCCESS

relay worker
  -> DrainEvents()
  -> TransferToWorker()
  -> PendingTcpWrites
  -> FlushTcpWrites(writev)
```

这条路径保留了一次 QUIC receive payload copy，但 callback 不做 TCP 写，也不等待本地 TCP socket 可写。MsQuic worker/connection 线程可以尽快返回，继续处理后续 UDP packet、ACK、flow control 和 receive callback。

Phase 2/3 后，早期最大的 mutex 和 shared_ptr 热点已经被压下去。Phase 3 perf 记录里 `TryPush` / `TryPop` / `QueueLock` 未成为 Top 热点，说明 event queue 本身不是单流瓶颈。单流下，一次 payload copy 的成本小于保持 MsQuic receive pipeline 顺畅带来的收益。

因此 Phase 3 单流能达到 14.09 Gbps，核心不是“copy 更快”，而是：

```text
copy + 异步 worker write
<
zero-copy + 每次 receive 都 pending/complete 往返
```

## Phase 3 为什么多流下降

Phase 3 多流时，以下成本会被连接数和 stream 数放大：

- 每个 receive 都要 copy 到 ingress buffer。
- 每个 chunk 都要 buffer pool acquire/transfer/release。
- 每个 receive batch 都要入队并唤醒 worker。
- worker 还要做真实 TCP `writev`。

单流时这些成本可以被流水线隐藏；多流时，worker 和 buffer pool 操作成为聚合吞吐的 CPU 消耗。Phase 4 always-pending 在多流下消掉 receive copy，并利用 `StreamMultiReceiveEnabled` 让多个 pending receive 并存，因此 4x16 能从 Phase 3 的 10.18 Gbps 提到 23.18 Gbps。

这说明 Phase 3 的多流瓶颈更接近 relay 层 CPU 成本，而不是 MsQuic 本身。

## always-pending 为什么多流强但单流弱

always-pending 的 receive 路径是：

```text
MsQuic callback
  -> 保存 QUIC_BUFFER 的 slice 元数据，不拷贝 payload
  -> Enqueue(QuicReceiveView)
  -> return QUIC_STATUS_PENDING

relay worker
  -> FlushDeferredQuicReceives()
  -> sendmsg/writev borrowed buffer 到 TCP
  -> StreamReceiveComplete(bytes)
```

这个模型的优点是明确的：QUIC->TCP payload copy 消失。文档里的 perf 也显示 `CopyQuicReceiveBatchToEvent` / receive `memcpy` 不再进入 Top 热点。

但单流下，它引入了新的关键路径：

```text
callback 返回 PENDING
-> worker 被唤醒
-> TCP 写出
-> StreamReceiveComplete
-> MsQuic 才能推进对应 receive buffer / flow control
```

即使启用了 `StreamMultiReceiveEnabled`，单流仍然依赖这条异步 complete 链路的速度。早期实验更直接：未启用 multi-receive 时 always-pending 只有 0.052 Gbps；启用后恢复到约 5 Gbps。这证明主矛盾是 `QUIC_STATUS_PENDING` 对 receive 投递节奏和 completion 往返的影响，而不是 borrowed buffer 生命周期本身。

多流时，多个 stream/connection 能提供并行 pending receive，调度空隙可以被其他流填满，zero-copy 收益更容易显现。因此 always-pending 多流强，单流弱。

## sync-write 是否已经是混合 fast path

方向上是，但当前实现仍然太重。

`phase4-callback-sync-write` 的核心逻辑是 `TryCompleteQuicReceiveInline()`。它在 MsQuic receive callback 中构造 `iovec` 并调用 `sendmsg`，大致行为是：

```text
completed = 0
while completed < totalLength:
  build iov from receive buffers
  sent = sendmsg(tcpFd, iov)
  if sent > 0:
    completed += sent
    continue
  if EAGAIN:
    break
```

如果本次 receive 全部写完，修复后的版本返回 `QUIC_STATUS_SUCCESS`，不再额外调用 `StreamReceiveComplete`，避免 double-complete。若只写出前缀，则对已写前缀 `StreamReceiveComplete(completed)`，剩余 slice 进入 deferred view 并返回 `QUIC_STATUS_PENDING`。

这确实是“同步成功，否则 pending”的混合方向。但当前实现的问题是：它会在 callback 中尽量把本次 receive 写完。只要 TCP socket 持续接受数据，callback 就会继续循环 `sendmsg`。

这不是足够窄的 fast path，而是“优先同步写完”。

## 为什么 sync-write 多流严重回退

单流时，sync-write 省掉了 always-pending 的 worker 往返，所以能和 always-pending 同量级，某些 run 还更高。

多流时，多个 stream 的 receive callback 都可能在 MsQuic worker/connection 线程里执行 TCP 写循环。这样 MsQuic callback 线程承担了大量本应由 relay worker 执行的 I/O 工作：

- callback wall time 变长。
- 其他 stream 的 receive callback 被延后。
- ACK / flow-control / send flush 等 QUIC 栈工作被挤压。
- 多 stream 公平性变差。
- 本地 TCP 写路径的 syscall 成本进入 MsQuic callback 执行窗口。

因此 sync-write 的多流从 always-pending 的 16.60 / 23.18 Gbps 退到 2.51 / 4.87 Gbps。这不是 zero-copy 思路失败，而是 callback 内同步写的预算边界太宽。

## 更准确的下一步

下一步不是重新发明 sync-write，而是把当前 sync-write 收窄成真正的有限预算 fast path：

```text
try_inline_once_or_budgeted()
  if full write within tiny budget:
    return QUIC_STATUS_SUCCESS
  if partial / EAGAIN / budget exceeded:
    complete written prefix if needed
    queue remaining receive view
    return QUIC_STATUS_PENDING
```

建议约束：

- callback 内最多做 1 次 `sendmsg`，或极小固定次数。
- callback 内最多写固定字节预算，例如 64KB 或 128KB，具体值需要实测。
- 不为了“写完整个 receive payload”在 callback 内循环。
- callback 内不等待，不重试长循环。
- 超过预算、partial、`EAGAIN` 立即进入 pending view，由 relay worker 继续。
- 压缩路径继续走 copy/worker 路径，不纳入 zero-copy fast path。

建议新增指标：

- 每个 callback 内 `sendmsg` 次数分布。
- callback wall time p50/p95/p99。
- inline full-write bytes。
- inline partial bytes。
- fallback pending bytes。
- `EAGAIN` / partial / budget-exceeded 次数。
- `StreamReceiveComplete` 次数与字节数。
- per stream pending receive bytes。

验收标准应同时覆盖：

- proxy-1x1 不低于 Phase 3 baseline 的归一化比例，至少显著高于 always-pending。
- proxy-4x16 / proxy-16x16 不显著低于 always-pending。
- callback p99 不出现明显长尾。
- 多流下 MsQuic worker 不被 TCP write syscall 主导。

## 最终判断

Phase 3、always-pending、sync-write 三者不是简单的谁先进谁落后，而是把成本放在了不同位置：

| 方案 | callback 行为 | payload copy | TCP write 位置 | 单流 | 多流 |
|------|---------------|--------------|----------------|------|------|
| Phase 3 | copy + enqueue + SUCCESS | 有 | relay worker | 强 | 中等 |
| always-pending | view + enqueue + PENDING | 无 | relay worker | 弱 | 强 |
| sync-write | callback 内尽量写完 | 无 | MsQuic callback 优先 | 弱到中等 | 弱 |

理想方向是：

```text
Phase 3 的短 callback
+ Phase 4 的 selective zero-copy
+ sync-write 的极窄 immediate-success fast path
+ always-pending 的 worker slow path
```

也就是说，保留 worker 作为主 relay I/O 执行者，只允许 callback 做非常短、可预测、非阻塞的 inline 尝试。当前 sync-write 已经验证了方向，但它还需要被预算化和指标化，才能避免多流下把 MsQuic callback 线程变成 TCP writer。
