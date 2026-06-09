# tcpquic-proxy 高性能线程模型与批量化数据面设计

> 状态：设计建议  
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
