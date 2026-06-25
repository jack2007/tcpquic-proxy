# Windows IOCP 上 tcp->quic 路径设计与背压建议（借鉴 Linux）

本说明聚焦当前 relay 的**TCP -> QUIC** 转发链路在 Windows 上的行为、问题和改造建议。

## 1. 现状与问题

当前实现的关键链路可概括为：

- TCP 侧提交 `WSARecv`（已绑定到 IOCP）
- IOCP 工作线程通过完成事件拿到 `WSARecv` 完成
- 在 `HandleTcpRecv` 中对接收到的数据做压缩/封包，并触发 `PostQuicSend`
- 一般在 QUIC 的 `send complete` 回调后，再次调用 `PostTcpRecv`

该模式的痛点在于：  
`send complete` 往往是在“应用层可以安全回收该缓冲区”时触发，而不是“字节已离开本地队列”。  
在本项目关闭 QUIC zero-copy 时，QUIC send buffer 复用应用内存，直到该回调到来才能释放；所以高 RTT 场景下 TCP 读触发会被强行跟随 RTT 节奏，吞吐出现明显“锯齿”/节流。

---

## 2. IOCP 语义澄清

你提到的疑问很关键：IOCP 队列里是否只能有 TCP/网络完成事件？

- **不是**。IOCP 是“完成队列”，既可接收内核异步 I/O 完成，也可接收应用通过 `PostQueuedCompletionStatus` 主动投递的自定义完成项。
- 所以可以把“是否恢复读 TCP”这种调度动作也放进 IOCP 队列，由统一工作线程按同一流程驱动，避免跨线程直接重入调用。

对比理解：

- **epoll/kqueue**：关注 fd 可读/可写事件，由你去 `read` 或 `write` 后再次 `wait`。
- **IOCP**：你以“提交 I/O 请求”为主，完成后由队列通知；队列也可承载你自定义的控制事件，因此可以把“控制性动作”（如 resume recv）也消息化。

---

## 3. 与 Linux 的对应关系

Linux 里常见思路（概述）：

- `epoll` 持续就绪可读，但若 QUIC pending 太多可**暂停 arm/read**，防止无界堆积；
- `DrainTcpReadable` 阶段按 `pending` 阈值决定是否继续读；
- 通过 `SetTcpReadBackpressure`/`ShouldPauseTcpReadForQuicBacklog` 一类逻辑做前压控流。

Windows 不能直接“取消可读就绪事件”，但可以用同构思路实现：

- 不依赖“总是有读事件就绪就处理”，而是**主动决定是否提交下一次 `WSARecv`**；
- 将“可提交下一次读”的时机和条件显式化，变成和 pending 字节相关的状态机。

---

## 4. 建议改造（推荐）

建议以“pending 字节阈值 + 自定义 IOCP 事件 resume”改造 tcp->quic 的采样节奏。

### 4.1 核心思路

1. 定义 `pending_send_bytes`（或 pending buffers）统计：记录已从 TCP 收到并提交给 QUIC 且尚未完成 send-complete 的累计量。  
2. `PostTcpRecv` 之前检查阈值：超过高水位则暂停提交新的 WSARecv。  
3. 在 QUIC 发送完成事件中减少 `pending_send_bytes`。  
4. 从高位回落到低位时，不要在 callback 直接 `WSARecv`，而是投递一个**自定义完成事件**到 IOCP。  
5. IOCP worker 看到这个事件后再调用 `PostTcpRecv`，保持单线程/统一调度语义，减少重入/竞态。

### 4.2 伪流程

1. 初始化：`PostTcpRecv` 提交一次异步读
2. `GetQueuedCompletionStatus` 收到 `TCP_RECV_COMPLETE`
3. `HandleTcpRecv`：
   - bytes > 0：`pending_send_bytes += bytes`，调用 `PostQuicSend`
   - bytes == 0 / error：按关闭路径清理
4. 若 `pending_send_bytes` 已达上限，不再继续 `PostTcpRecv`
5. `QUIC_SEND_COMPLETE` 回调：
   - `pending_send_bytes -= sent_bytes`
   - 若处于 backpressure paused 且已从高位回落到低位：`PostQueuedCompletionStatus(..., RESUME_TCP_RECV, relay_ptr)`
6. IOCP worker 收到 `RESUME_TCP_RECV`：再次调用 `PostTcpRecv`

### 4.3 与现有 Linux 设计保持一致的点

- 使用“高/低阈值”而非单点阈值，避免抖动；
- 不在 callback 里直接连环投递 I/O 请求，改为“事件触发再提交”；
- 以队列作为统一调度器，便于统一日志与状态观察。

---

## 5. 与 QUIC 语义的注意事项

- `QUIC_STREAM_EVENT_SEND_COMPLETE` 仅表示应用缓冲可回收（尤其在禁用 send-copy 时）；
- 它不等于“对端已确认（ACK）”这种更严格定义，不能直接拿它做网络发送进度控制；
- 因此这个事件更适合作为**下发下游读控制**的节拍器，而不是 TCP 读事件本身。

---

## 6. 风险与注意点

- 回调线程和 IOCP worker 的共享状态需原子/互斥保护（尤其 pending 计数与 pause flag）；
- 连接关闭时，`paused` 状态、挂起的自定义事件、未完成 send 需一次性清理；
- 若后续改为可复制 send（enable send-copy），“pending 语义”可适当放宽；
- 建议在日志里记录 `pending_send_bytes` 的高低位变化和事件驱动点，便于对比 Linux/backlog 方案。

---

## 7. 结论

本问题本质不是“IOCP 本身效率问题”，而是“发送完成语义被当作吞吐节拍器”。  
更合理的实现是引入 Linux 风格的 `pending` 背压控制并用自定义 IOCP 事件恢复读：

- 阻止高 RTT 下每次读都等待一个 send-complete；
- 提升吞吐连续性；
- 保持线程模型清晰，避免在 QUIC worker 中直接驱动 TCP I/O 的重入问题；
- 为 Windows 与 Linux 的 tcp->quic 方向提供统一的背压行为语义。

---

## 8. Windows quic->tcp 路径中的问题与应对：`PostQueuedCompletionStatus` 失败缺少可恢复 backlog

你指出的问题对应当前实现中的一个关键短板：

- Linux 在用户态有 `EventQueue`，当队列满时能落到 `CallbackPendingQuicReceives` 等后备路径；
- Windows 当前是 `OVERLAPPED` 入队 IOCP，`PostQueuedCompletionStatus` 失败会直接走错误路径回收资源，缺少“可重试的可恢复排队”层。

这会导致：

- 在短时高压时出现“入队失败即丢弃/失败关闭”的硬退化；
- 与 Linux 的“可持续摄入+可延后处理”行为不一致；
- 诊断上难以区分“系统资源短时抖动”与“真实上游异常”。

### 8.1 解决思路

核心目标：在 `PostQueuedCompletionStatus` 失败时，提供一个轻量、可靠、可重放的 fallback，不阻塞 QUIC 回调线程。

1. 增加 `Relay` 级/全局级 `PostQueuedCompletionStatus` fallback 队列（无锁 MPSC 或带锁小队列）；
2. `PostQuicReceiveViewQueued / PostTcpSend / custom resume 事件` 在投递 IOCP 前失败时，不直接 `return false`。
   - 优先入队 fallback 队列（带容量上限）；  
   - 入队成功则设置 pending 标记并等待 worker 侧扫描；
   - 入队失败才触发致命错误。
3. IOCP worker 在每次 `Run()` 循环尾部做 `DrainFallbackQueue`；
   - 一次性拉取 pending event，在 worker 上下文执行原本应由 IOCP 处理的分发逻辑（`HandleQuicReceiveViewQueued`、`RetryPendingQuicSends`、自定义唤醒事件等）。
4. 增加“退化熔断”指标：
   - `iocp_post_fallback_count`
   - `iocp_post_fallback_dropped_count`
   - `iocp_fallback_drain_batch_count`

### 8.2 实施建议（最小改造）

- 先补统一的“事件抽象”：把 `TqWindowsRelayEvent` + `IoOperation*` 转成统一 `RelayIoTask`，可在主任务队列与 IOCP 中都表示同一种语义。
- 统一在以下提交点加 fallback：
  - `QueueDeferredQuicReceive` 里的 `PostQueuedCompletionStatus(QuicReceiveViewQueued)`；
  - `PostTcpSendComplete` / send 完成重试相关的 `QuicSendRetry` 投递；
  - `CloseRelay`/`Stop` 这类控制事件。
- worker 每 1 次 `GetQueuedCompletionStatus` 处理之后，额外调用 `DrainFallbackQueue`，确保 fallback 事件最终可执行，不依赖系统事件队列容量波动。

### 8.3 兼容性和回收策略

- 每个 `IoOperation` 仍由唯一 owner 生命周期管理（成功投递到 IOCP 或入队 fallback 后在 worker 统一释放）。
- fallback 队列消费与 `Relays_` map 锁必须避免循环锁嵌套，采用独立互斥或无锁队列；
- 退避策略建议：fallback 入队成功后可在 worker 中按批处理，减少大量 wake/锁竞争。

### 8.4 预期收益

- 避免 `PostQueuedCompletionStatus` 失败导致的突发连接失败；
- 行为更接近 Linux 的 “事件入队压力可延后处理”；
- 便于后续把 quic->tcp 与 tcp->quic 的背压机制收敛到统一的“状态机+阈值+事件驱动”模型。
