# tcpquic-proxy 高性能线程模型与批量化数据面设计

> 状态：设计建议（已评审，见 §5）  
> 日期：2026-06-09  
> 范围：面向 tcpquic-proxy 产品目标的高性能 TCP-over-QUIC 代理设计，不受当前 demo 代码结构约束

---

## 1. 设计目标

tcpquic-proxy 的目标是提供一个高性能、跨平台、可产品化的 TCP-over-QUIC 隧道代理。客户端侧提供 SOCKS5 和 HTTP CONNECT 入口，将本地 TCP 连接映射为 QUIC 双向 Stream；服务端侧从 QUIC Stream 还原 TCP 连接并拨号目标服务。

高性能设计不应只关注单连接吞吐，还要同时关注高并发、低 CPU 占用、低内存放大、稳定尾延迟和跨平台可实现性。因此数据面和线程模型应以以下目标为核心：

1. **避免 per-tunnel thread**：线程数量应接近 CPU 核数，而不是接近连接数。
2. **减少业务层数据复制**：TCP 与 QUIC 转换过程中，业务代码尽量只传递 buffer view，不复制 payload。
3. **减少动态内存分配**：数据面使用分片 buffer pool，避免每次收发分配 `std::string` / `std::vector`。
4. **批量化处理数据**：一次线程唤醒后尽量 drain 足够多的数据，批量提交给下游。
5. **合并跨线程唤醒**：避免每个 TCP/QUIC 事件都触发一次 worker wakeup。
6. **保持 backpressure**：批量读取必须受 QUIC send capacity、TCP write backlog、tunnel budget 和 worker budget 控制。
7. **保证公平性**：大流不能长期占用 worker，导致小连接饿死。
8. **保持平台原生性能**：Linux 使用 epoll/readv/writev，Windows 使用 IOCP/WSARecv/WSASend；QUIC 由 MsQuic 负责。

最终目标不是追求理论上的端到端零拷贝，而是做到：**tcpquic 自己的数据面不做不必要 payload copy，不做频繁小块分配，不做每事件一次线程唤醒。**

---

## 2. 主要挑战

### 2.1 TCP 与 QUIC 的数据模型不同

TCP 是 byte stream，没有天然消息边界；QUIC Stream 也是 byte stream，但其底层由 QUIC frame、TLS 加密、UDP packet、拥塞控制和重传机制组成。应用层不能直接控制 QUIC 的 UDP 报文边界，也不能绕过 MsQuic 自己的 send/receive buffer 生命周期。

因此 tcpquic 的优化重点应放在 TCP stream 与 QUIC stream 之间的应用层数据面，而不是直接操作 QUIC UDP packet。

### 2.2 完全零拷贝不现实

QUIC 发送路径需要 TLS 加密、frame 编码、packet 组包和重传缓存；接收路径需要解密、重排、流控和 stream reassembly。即使 TCP 侧可以使用 pooled buffer 和 scatter/gather I/O，MsQuic 内部仍可能复制或持有数据。

设计上应接受这一点，把目标定义为：

- TCP 读取直接进入 pool buffer。
- 内部 relay 只传 buffer view。
- QUIC send completion 前 buffer 不复用。
- QUIC receive buffer 尽量直接 scatter/gather 写 TCP。
- 压缩作为可选慢路径，默认不污染快速路径。

### 2.3 线程唤醒和上下文切换可能成为瓶颈

高并发代理中，性能瓶颈往往不是一次 `memcpy`，而是大量小事件导致的：

- 频繁 syscall。
- 频繁线程唤醒。
- 频繁跨线程队列投递。
- 锁竞争。
- cache locality 下降。
- 每连接线程导致调度器负担放大。

如果每收到一个 TCP readable、一个 QUIC receive callback 或一个 send completion 都完整唤醒一次业务线程，系统在高 PPS 或高并发场景下会快速退化。

### 2.4 批量化与低延迟存在取舍

一次性读取 1MB 并批量发送可以明显提升吞吐，减少 syscall 和 wakeup，但也可能提高单个小请求的排队延迟。因此批量化必须是 budget 驱动，而不是无条件等待固定大小。

推荐策略是：**最多读到 1MB；如果 socket 已读空、下游 capacity 不足或公平性预算耗尽，则立即停止并提交已有数据。**

### 2.5 Backpressure 与内存上限必须前置设计

批量化会放大瞬时内存占用。如果 TCP 侧一次读很多，但 QUIC send capacity 不足，就会形成大量待发送 buffer；反向路径同理，如果 TCP peer 接收慢，QUIC receive 数据会堆积在用户态。

因此必须同时设计：

- per-tunnel pending byte limit。
- per-worker pending byte limit。
- global memory budget。
- TCP read interest enable/disable。
- QUIC receive completion / receive flow 控制。
- send completion 和 write completion 后的 resume 逻辑。

---

## 3. 设计思路

### 3.1 线程模型：Sharded Reactor

采用 sharded reactor，而不是 per-tunnel thread。

```text
main thread
  -> config / signal / lifecycle

acceptor threads
  -> SOCKS5 listener
  -> HTTP CONNECT listener
  -> QUIC listener callback
  -> accept / classify / assign worker

worker shard 0..N-1
  -> epoll / IOCP event loop
  -> TCP socket ownership
  -> tunnel state ownership
  -> buffer pool shard
  -> pending QUIC send queue
  -> pending TCP write queue
  -> backpressure state

MsQuic internal threads
  -> QUIC connection / stream callbacks
  -> lightweight callback only
  -> enqueue event to owner worker

optional DNS pool
  -> blocking resolver fallback only

optional compression pool
  -> zstd or expensive compression only
```

每条 tunnel 创建时固定分配一个 `owner_worker`。之后该 tunnel 的状态、TCP socket、pending queue 和 buffer 生命周期都由 owner worker 管理。跨线程只传事件和 buffer reference，不直接修改 tunnel 状态。

```text
Tunnel
  owner_worker
  tcp_socket
  quic_stream
  tcp_to_quic_queue
  quic_to_tcp_queue
  buffer_budget
  state
```

### 3.2 数据模型：Buffer Pool + Buffer View

数据面不使用 `std::string` 或动态增长 `std::vector` 承载 payload。每个 worker 拥有自己的 buffer pool，常用块大小可以是 16KB、32KB、64KB。

内部只传递 view：

```cpp
struct BufferView {
    uint8_t* data;
    size_t len;
    BufferRef owner;
};
```

一个 TCP read batch 可以由多个 `BufferView` 组成，一个 QUIC receive batch 也可以由多个 view 组成。只要下游支持 scatter/gather，就不合并为连续内存。

### 3.3 批量化策略：Drain-to-Budget

一次事件唤醒后，不只处理一个事件或一个 buffer，而是处理到预算耗尽或无法继续推进。

```text
event wakeup
  -> drain TCP/QUIC input to budget
  -> accumulate buffer views
  -> batch submit downstream
  -> flush pending writes/sends
  -> release completed buffers in batch
```

停止条件：

1. socket 当前读空。
2. 达到本轮 batch byte budget。
3. 达到本轮 batch chunk/event budget。
4. QUIC/TCP 下游 capacity 不足。
5. 当前 worker fairness budget 用完。
6. tunnel 或 worker buffer budget 不足。

### 3.4 QUIC 边界：不绕过 MsQuic

UDP GSO/GRO、UDP send batching、receive coalescing 是 QUIC 实现层面的优化。tcpquic 应让 MsQuic 使用平台能力，而不是绕开 MsQuic 直接处理 UDP packet。

tcpquic 自己要做的是：

- MsQuic callback coalescing。
- stream event batching。
- QUIC send request batching。
- worker wakeup batching。
- QUIC receive buffer 到 TCP writev/WSASend 的批量转发。

---

## 4. 详细方案

### 4.1 Worker Shard 与 Tunnel 归属

worker 数量默认接近 CPU 核数：

```text
worker_count = min(logical_cpu_count, configured_upper_limit)
```

建议默认上限为 8 到 32，具体值由部署环境和压测决定。

连接分配策略：

```text
acceptor accept TCP client
  -> choose worker by round-robin / least-load / hash
  -> handoff socket to worker
  -> worker owns socket and tunnel state
```

B 侧收到 QUIC OPEN 后：

```text
MsQuic stream callback
  -> identify route target
  -> enqueue OpenEvent to selected worker
  -> worker resolve target
  -> worker nonblocking connect
  -> worker owns target TCP socket
```

acceptor 只做轻量工作，不进行完整 relay，不处理大量数据，不成为中心瓶颈。

### 4.2 TCP -> QUIC 批量读取与发送

TCP readable event 到来后，owner worker 尽量一次性 drain socket。

推荐逻辑：

```text
on_tcp_readable(tunnel):
  allowed = min(
    tcp_read_batch_limit,
    tunnel_buffer_budget_available,
    worker_buffer_budget_available,
    quic_stream_send_capacity_estimate
  )

  while read_bytes < allowed:
    readv/recv into pooled buffers
    if EAGAIN or WOULD_BLOCK:
      break
    if no buffer or downstream capacity low:
      break
    append BufferView to TcpToQuicBatch

  submit batch to MsQuic StreamSend
```

Linux 推荐：

```text
iovec[16] x 64KB = 1MB
readv(fd, iov, 16)
```

Windows 推荐：

```text
Pre-post WSARecv buffers
IOCP completions are coalesced by owner worker
Batch completed WSABUFs into QUIC send requests
```

QUIC send 提交时，一个 send request 尽量包含多个 `QUIC_BUFFER`：

```text
readv 1MB
  -> 16 x 64KB BufferView
  -> MsQuic StreamSend(buffers=16)
  -> send complete
  -> release 16 buffers to worker pool
```

如果 QUIC flow-control capacity 不足，则按 capacity 切分提交：

```text
while batch not empty and quic_capacity > 0:
  n = min(batch_remaining, quic_capacity, max_quic_send_size)
  StreamSend(n buffers)
```

### 4.3 QUIC -> TCP 批量接收与写出

MsQuic receive callback 中不要做重活，不要阻塞写 TCP。callback 只负责捕获事件，并尽量合并投递到 owner worker。

推荐路径：

```text
MsQuic receive callback
  -> create QuicRecvEvent with receive buffer views
  -> enqueue to owner worker
  -> coalesced wakeup

owner worker
  -> collect QuicRecvEvent into TcpWriteBatch
  -> if TCP writable:
       writev / WSASend scatter-gather
  -> partial write:
       keep remaining BufferViews in pending queue
```

Linux 使用：

```text
writev(fd, iov, iovcnt)
```

Windows 使用：

```text
WSASend(socket, WSABUF[], count, ...)
```

关键点是不要把 QUIC receive 的多个 buffer 合并成连续内存。如果 MsQuic receive buffer 生命周期允许延迟完成，则 owner worker 可以直接使用 receive buffer view；如果 callback 返回后 buffer 立即失效，则必须复制到 worker pool。后者无法完全避免，但仍要使用批量复制和 pool buffer，避免小块分配。

### 4.4 跨线程事件与唤醒合并

每个 worker 有一个 MPSC event queue 和一个 atomic wake flag。

```text
producer thread:
  queue.push(event)
  if wake_flag.exchange(true) == false:
    wake worker once
```

worker 被唤醒后：

```text
worker loop:
  drain event queue up to event_budget
  process TCP events
  process QUIC events
  flush pending QUIC sends
  flush pending TCP writes
  release completed buffers
  clear wake_flag
  if queue still not empty:
    self-reschedule
```

这样大量 QUIC receive callback 或 send completion 可以合并成一次 worker wakeup，避免每个事件单独唤醒线程。

### 4.5 Worker Tick 与 Ready Queue

worker 每轮 tick 应该按预算处理一批事件，而不是单事件递归推进。

```text
while running:
  events = poll/io_uring/iocp_wait(timeout)
  process_net_events(events, event_budget)
  process_quic_events(quic_event_budget)
  process_ready_tunnels(byte_budget, tunnel_budget)
  flush_pending_quic_sends()
  flush_pending_tcp_writes()
  run_timers()
```

active tunnel 使用 ready queue 管理：

```text
ready_tunnels queue
  -> pop tunnel
  -> process up to per_tunnel_budget
  -> if still ready, push back
```

这可以防止单条大流长时间占用 worker。

### 4.6 Backpressure 状态机

TCP -> QUIC：

```text
TCP readable
  -> QUIC send capacity 足够才 read
  -> tunnel/worker buffer budget 足够才 read
  -> 否则 disable TCP read interest

QUIC send complete
  -> release buffer
  -> update send backlog
  -> resume TCP read if capacity recovered
```

QUIC -> TCP：

```text
QUIC receive
  -> TCP send queue 未超限才继续接收
  -> TCP writable 才批量 writev/WSASend
  -> 否则保留 pending write queue

TCP write complete
  -> release/complete receive buffer
  -> update TCP backlog
  -> resume QUIC receive / stream flow
```

必须同时设置：

```text
per_tunnel_pending_bytes
per_worker_pending_bytes
global_pending_bytes
per_tunnel_batch_bytes
per_worker_tick_bytes
event_budget
```

### 4.7 QUIC Connection Pool 与 Worker 绑定

性能优先时，推荐每个 worker 拥有自己的 QUIC connection 子池：

```text
worker 0 -> quic conn 0, 4, 8
worker 1 -> quic conn 1, 5, 9
worker 2 -> quic conn 2, 6, 10
worker 3 -> quic conn 3, 7, 11
```

优点：

- connection/stream locality 更好。
- owner worker 与 stream 管理更清晰。
- 跨线程事件更少。
- 每 worker 可独立统计 backlog、RTT、吞吐和拥塞状态。

缺点：

- 连接管理复杂度略高。
- 连接数可能比全局池更多。

如果部署目标是低并发或连接数受限，也可以使用全局 QUIC connection pool，但 stream 创建后仍必须固定 owner worker。

### 4.8 DNS、Connect 与压缩线程

目标拨号不应使用 detached thread。

推荐：

```text
worker receives OPEN
  -> async DNS or small resolver pool
  -> nonblocking connect
  -> connect completion returns to owner worker
```

压缩默认不进入快速路径：

- 默认关闭或 auto。
- lz4 fast 可在 owner worker 内执行。
- zstd 高等级压缩放入小型 compression pool。
- compression pool 只传 buffer reference，不复制 payload。
- 如果压缩导致 worker backlog 上升，应自动降级或关闭。

### 4.9 推荐默认参数

吞吐优先：

```text
tcp_read_chunk_size = 64KB
tcp_read_batch_bytes = 1MB
quic_recv_batch_bytes = 1MB
max_iov = 16
per_tunnel_pending_bytes = 4MB ~ 16MB
per_worker_pending_bytes = 256MB ~ 1GB
worker_event_budget = 4096
worker_byte_budget_per_tick = 64MB
```

低延迟：

```text
tcp_read_chunk_size = 16KB ~ 32KB
tcp_read_batch_bytes = 128KB ~ 256KB
quic_recv_batch_bytes = 128KB ~ 256KB
max_iov = 8 ~ 16
smaller worker tick budget
more frequent flush
```

这些参数不应硬编码，应暴露为 profile：

```text
--profile throughput
--profile balanced
--profile low-latency
```

### 4.10 最终推荐架构

```text
1 main thread
+ 1~2 acceptor/control threads
+ N worker shards
+ MsQuic internal threads
+ optional small DNS pool
+ optional compression pool
```

数据面：

```text
每条 tunnel 固定 owner worker
TCP socket 归 owner worker
tunnel state 归 owner worker
buffer pool 按 worker 分片
TCP readv / WSARecv 批量收
QUIC StreamSend 批量提交 QUIC_BUFFER
QUIC receive buffers 用 writev / WSASend 批量写 TCP
跨线程事件 coalesced wakeup
每 worker tick 批量 flush
per-tunnel + per-worker budget 保证公平和内存上限
```

最终设计原则是：**一次线程唤醒尽量处理更多有效字节，但任何批量化都必须受 capacity、memory budget 和 fairness 控制。**

---

## 5. 设计评审

> 评审日期：2026-06-09  
> 评审范围：本文档（目标架构）  
> 对照基线：`src/thread_model_cn.md`、当前 `src/relay.cpp` / `src/tcp_write_queue.cpp` / `src/tcp_tunnel.cpp`  
> 关联规格：`docs/specs/2026-06-09-cross-platform-porting-summary.md`、`docs/specs/2026-06-09-windows-platform-support-design.md`

### 5.1 评审结论

**总体评价：方向正确、结构完整，可作为 Phase 5（sharded reactor + 批量化数据面）的目标架构文档。** 设计目标与 `thread_model_cn.md` §10 中「relay TCP→QUIC 改 epoll 多路复用」的未实现项一致，且比当前「每隧道 2 线程 + MsQuic 回调入队」模型在并发扩展性上更合理。

**当前不建议按本文档一次性重写数据面。** 现有实现已完成 QUIC→TCP 异步 writer、handshake 线程池、全局 reaper 等中期优化；与本文差距主要在 **per-tunnel 线程消除**、**readv/writev 批量化**、**MsQuic 回调与 owner worker 对齐**。建议分阶段落地，并在每阶段补充可测量的验收指标。

| 维度 | 评分 | 说明 |
|------|------|------|
| 目标与约束 | ✅ 清晰 | 八项设计目标可验证；「不做不必要 copy / 分配 / 唤醒」表述务实 |
| 线程模型 | ⚠️ 需补 MsQuic 亲和性 | Sharded reactor 合理，但未说明如何与 MsQuic `quic_worker` 归属对齐 |
| 数据面批量化 | ✅ 合理 | budget 驱动 drain、fairness queue、backpressure 状态机完整 |
| 与现网代码差距 | ⚠️ 大 | 当前仍 per-tunnel `TcpThread` + `TcpWriter`，QUIC→TCP 路径仍逐 chunk 拷贝 |
| 跨平台一致性 | ❌ 与 Windows v1 草案冲突 | 本文假设 IOCP/WSARecv；Windows 支持设计 v1 明确「不支持 IOCP 高性能路径」 |
| 可落地性 | ⚠️ 缺阶段与验收 | 无迁移路径、无 benchmark 基线、无与 `--max-memory-mb` 等现有参数的映射 |

### 5.2 与当前实现的对照

```text
当前（thread_model_cn.md §1）          本文目标
────────────────────────────────────────────────────────────
每隧道 TcpThread（阻塞 recv）    →    owner worker epoll/IOCP 多路复用
每隧道 TcpWriter（阻塞 send）    →    同 worker 内 writev/WSASend 批量写
MsQuic 回调 → Enqueue（拷贝）    →    回调轻量入队 + 尽量零拷贝 view
server OPEN → detached dial 线程 →    worker 内 async DNS + 非阻塞 connect
handshake 固定线程池（已实现）   →    acceptor + worker handoff（一致）
全局 TqTunnelReaper（已实现）    →    main/control 生命周期（一致）
fast path send context pool      →    worker 级 buffer pool + BufferView（可演进）
```

**线程数量影响（100 活跃隧道、N≈20）：**

| 模型 | 应用数据路径线程（约） |
|------|------------------------|
| 当前 | 2L = 200（TcpThread + TcpWriter）+ H handshake + 2 listener |
| 本文 | N worker ≈ 8–32 + 1–2 acceptor + MsQuic 内部 2N |

在高并发场景下，本文模型可显著降低 pthread 调度开销；但需避免 tcpquic worker 与 MsQuic `cxplat_worker` / `quic_worker` 叠加后总线程数仍接近 `2N + N + …` 而收益有限。

### 5.3 优点（建议保留）

1. **目标定义务实**：§2.2 明确接受 MsQuic 内部 copy，聚焦应用层 relay，避免过度承诺「端到端零拷贝」。
2. **批量化有停止条件**：§2.4、§3.3、§4.6 的 budget / capacity / fairness 三元约束，能同时兼顾吞吐与尾延迟。
3. **Backpressure 前置**：§2.5、§4.6 的 per-tunnel / per-worker / global 三级预算与 read interest 开关，与现有 `TqApplyRelayPoolBudget`、`--max-memory-mb` 思路一致，应在落地时显式对接。
4. **QUIC 边界清晰**：§3.4 不绕过 MsQuic 直接操作 UDP，符合 msquic 产品化路径。
5. **Profile 化参数**：§4.9 的 throughput / balanced / low-latency profile 与已有 `--quic-profile` 形成自然扩展点。

### 5.4 问题与修订建议

| No. | 严重级别 | 问题 | 建议 |
|-----|----------|------|------|
| 1 | **Major** | **未说明 MsQuic 回调线程与 owner worker 的映射** | 补充一节：连接/stream 创建时如何通过 MsQuic Execution API、Registration 分区或「回调仅入 MPSC 队列、禁止 touch tunnel 状态」保证 §3.1「跨线程只传事件」；明确 `quic_worker` 与 tcpquic worker 是否为 1:1 或 N:M |
| 2 | **Major** | **QUIC RECEIVE 路径缺少 `StreamReceiveComplete` 语义** | §4.3 必须写明：在 TCP 写完成前不能对 MsQuic 调用 `ReceiveComplete` 的字节数；否则流控窗口提前释放导致内存堆积。当前 `OnStreamReceive` 入队后即返回，若改为 view 直写 TCP，需 deferred complete |
| 3 | **Major** | **QUIC→TCP 零拷贝前提未验证** | §4.3 已写「callback 返回后 buffer 可能失效」——与 MsQuic 实际行为一致。当前 `TqTcpWriteQueue::Enqueue` 使用 `std::vector` 拷贝（`tcp_write_queue.cpp`）。文档应 **默认推荐 copy-into-pool 为 v1**，零拷贝 view 作为 v2 优化项，并引用 msquic `StreamReceiveComplete` 时序 |
| 4 | **Major** | **与 Windows 平台设计冲突** | `2026-06-09-windows-platform-support-design.md` v1 非目标含「不支持 IOCP 高性能路径」。本文 §1、§4.2、§4.3 多处假设 IOCP/WSARecv。**二选一**：(a) 将 IOCP 批量化标为 Windows Phase 2；(b) 更新 Windows 设计，声明高性能路径依赖 IOCP。Linux 可先 epoll/readv，Windows 先 WSAPoll + 阻塞 send 对齐现行为 |
| 5 | **Medium** | **worker tick 提到 io_uring，正文仅 epoll** | §4.5 `poll/io_uring/iocp_wait` 与 §1 Linux epoll 不一致。若 io_uring 在范围外，删除或标注「可选后续」；若在范围内，补充与 epoll 的选型条件（已有 `build-iouring` 构建） |
| 6 | **Medium** | **MPSC wake coalescing 缺少并发细节** | §4.4 经典 `wake_flag.exchange` 模式需补充：memory order、`clear wake_flag` 与「queue 非空」之间的 lost wakeup 防护（例如 drain 后再 clear，或 seqlock） |
| 7 | **Medium** | **压缩池「只传 reference 不 copy」不现实** | §4.8 zstd 通常需要连续输入/输出 buffer。改为：压缩池持有 pool slab，fast path 仍在 owner worker 内 lz4；高等级 zstd 允许 copy-in/copy-out 或专用 scratch |
| 8 | **Medium** | **per-worker QUIC 连接池与连接数权衡** | §4.7 每 worker 独立 conn 子池在 C 较小、L 很大时可能导致连接利用率低。补充：全局 pool + stream 固定 owner worker 的混合策略及切换条件 |
| 9 | **Medium** | **默认内存参数与现有 CLI 未映射** | §4.9 `per_worker_pending_bytes = 256MB~1GB` × 32 worker 可达 32GB。应对照 `--max-memory-mb`、`RelayMaxFreeSendContexts`、`TqApplyRelayPoolBudget` 给出公式或配置表 |
| 10 | **Minor** | **缺少分阶段迁移计划** | 建议增加 §6「落地阶段」：Phase A 合并 TcpWriter 进单 worker（仍 copy）；Phase B TCP→QUIC epoll 多路复用；Phase C readv/writev + StreamSend 多 buffer；Phase D receive view / deferred complete |
| 11 | **Minor** | **缺少验收与回归标准** | 引用 `scripts/test-tcpquic-proxy.sh`、100 隧道压测、线程数上限（如 L=100 时应用线程 < 64）、P99 延迟与 CPU% 对比基线 |
| 12 | **Minor** | **acceptor 与 handshake 池关系未细化** | 当前 client 为 2 listener + `TqThreadPool`。本文 acceptor thread 与 handshake 是否合并、OPEN 等待是否仍在 handshake 池，需序列图补充 |
| 13 | **Minor** | **admin / 控制面未纳入** | `TqAdminHttpServer` 等管理 HTTP 线程与 main 生命周期，建议在 §3.1 架构图中点明，避免实现时遗漏 |

### 5.5 MsQuic 集成要点（建议在正文中增补）

以下为评审认为 **必须在详细方案中显式写出** 的 MsQuic 约束，否则实现阶段易出现流控 bug 或 UAF：

1. **`QUIC_STREAM_EVENT_RECEIVE`**：回调内处理时间应极短；若使用 receive buffer 指针直写 TCP，必须在 `StreamReceiveComplete` 之前保证 buffer 不被 MsQuic 回收。
2. **`QUIC_STREAM_EVENT_SEND_COMPLETE`**：当前 fast path 已在 SEND_COMPLETE 归还 `TqRelaySendContext`（`relay.cpp`）；批量化后「多 buffer 一次 StreamSend」需约定 partial complete 语义（MsQuic 按 send operation 整体 complete）。
3. **`QUIC_STREAM_EVENT_IDEAL_SEND_BUFFER_SIZE`**：现有 relay 已用于 `IdealSendBytes` / backpressure；sharded 模型应保留 per-stream ideal send，并纳入 §4.2 `quic_stream_send_capacity_estimate`。
4. **Execution Profile**：`--quic-profile max-throughput|low-latency` 已落地；worker shard 数与 MsQuic worker 数应联合调参，避免双份 `N` 线程空转。

### 5.6 跨文档一致性检查

| 文档 | 关系 | 动作 |
|------|------|------|
| `src/thread_model_cn.md` | 当前线程模型基线 | 本文视为 §10「epoll 多路复用」的展开设计；建议在 thread_model 增加指向本文的链接 |
| `docs/plans/2026-06-06-tcpquic-thread-model.md` | Phase 1–4 已完成 | 本文是 Phase 5+；避免与已完成项重复描述 |
| `2026-06-09-windows-platform-support-design.md` | IOCP 范围冲突 | **需统一**：v1 可移植性 vs 高性能 Windows 路径的优先级 |
| `2026-06-09-cross-platform-porting-summary.md` | 平台抽象 / libuv 调研 | 若引入统一 event loop 抽象，本文 §3.1 的 epoll/IOCP 分支可收敛到平台层 |

### 5.7 建议的文档修订清单

评审人建议在下一轮修订中完成：

- [ ] 新增 **§4.x MsQuic 回调与 StreamReceiveComplete 时序**（含 ASCII 序列图）
- [ ] 新增 **§6 分阶段落地计划** 与 **§7 验收指标**
- [ ] 将 Windows IOCP 标为 **Phase 2** 或同步修订 Windows 平台设计
- [ ] 增加 **与现有 tuning / CLI 参数对照表**（`RelayIoSize`、`--max-memory-mb`、`--handshake-threads` 等）
- [ ] 在 §4.3 明确 **v1 默认 copy-into-pool**，view 零拷贝为优化路径

### 5.8 评审终态

| 项 | 结论 |
|----|------|
| 是否可作为长期目标架构 | ✅ 是 |
| 是否可直接进入编码 | ❌ 否，需先闭合 MsQuic 时序与 Windows 范围 |
| 与现实现兼容的下一步 | 优先 **TCP→QUIC epoll 多路复用**（消除 per-tunnel TcpThread），QUIC→TCP 仍用现有 `TqTcpWriteQueue` 或逐步迁入同 worker |

**评审状态：** 有条件通过 — 作为 Phase 5 设计基线采纳，按 §5.7 修订后可用于 implementation plan。
