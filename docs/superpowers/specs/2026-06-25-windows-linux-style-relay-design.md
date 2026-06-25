# Windows Linux-Style Relay Redesign

> 日期：2026-06-25  
> 范围：`src/tunnel/windows_relay_worker.{h,cpp}`、新增 Windows relay event queue、`src/unittest/windows_relay_worker_test.cpp`  
> 参考：`docs/iocp-tcp2quic.md`、`docs/windows/iocp-bug-cm25.md`、`docs/windows/iocp-bug-gpt55.md`、`docs/superpowers/specs/2026-06-25-windows-iocp-refactor-design.md`

---

## 1. 背景

Windows relay 当前基于 IOCP worker：

```text
MsQuic callback thread
  -> PostQueuedCompletionStatus(QuicReceiveViewQueued / QuicSendRetry / CloseRelay)
  -> sometimes directly PostTcpRecv on SEND_COMPLETE

IOCP worker
  -> GetQueuedCompletionStatus
  -> dispatch TCP completion and posted user events
```

这个设计把 IOCP 同时用作两类队列：

1. Windows kernel overlapped I/O completion queue
2. relay 自己的用户态调度队列

问题是第二类队列不可观测。`PostQueuedCompletionStatus` 成功后，QUIC callback 线程看不到队列长度，也看不到消费进度；失败时当前路径只能回滚或 fatal。对于 `quic->tcp`，这意味着 QUIC receive 只能把数据扔进一个不可查询 backlog。对于 `tcp->quic`，当前 `SEND_COMPLETE` 回调又直接参与 `PostTcpRecv`，导致 TCP read 被 MsQuic send-complete 节拍绑定。

Linux/macOS 没有这个问题：它们有显式的用户态 event queue/backlog，worker 能 drain、计数、限流、恢复。Windows 端网页代理长期不稳定，而 Linux/macOS 正常，说明 Windows 现在这套 proactive IOCP 调度结构需要向 Linux 靠拢。

本设计 supersede 早先 `2026-06-25-windows-iocp-refactor-design.md` 中“不引入 eventqueue”的结论。早先结论对当时的 teardown accounting 修复成立，但不足以解决 dashboard/cursor.com 场景暴露出的结构性问题。

---

## 2. 目标与非目标

### 目标

- 建立 Windows 自己的 bounded user-space event queue。
- `quic->tcp` 数据/receive view 不再通过 `PostQueuedCompletionStatus` 承载。
- `tcp->quic` 的 `WSARecv` 由 worker 状态机决定，不再放在 QUIC `SEND_COMPLETE` callback 中。
- QUIC callback 线程只做轻量入队、计数和必要的 receive pause，不直接驱动 TCP I/O。
- IOCP 只处理真实 `WSARecv` / `WSASend` overlapped completion，以及最小 wake/stop 信号。
- Windows 的 backlog、pause/resume、drain、teardown 语义尽量对齐 Linux relay。

### 非目标

- 不把 Windows 改成 epoll/kqueue readiness 模型。Windows 仍使用 overlapped I/O。
- 不改 tunnel protocol、compressor/decompressor 格式、MsQuic stream API。
- 不要求第一阶段彻底拆小 `windows_relay_worker.cpp`；拆分仅限新 queue 和 op 类型，避免过大重构。
- 不默认引入强制 idle timeout。idle 诊断保留，是否关闭长 idle tunnel 另行决策。

---

## 3. 设计原则

### 3.1 IOCP 边界

IOCP 应只表达“内核异步 I/O 完成”：

- `TcpRecv`
- `TcpSend`
- worker stop/wake sentinel

以下事件不再用 IOCP payload 承载：

- `QuicReceiveViewQueued`
- `QuicSendComplete`
- `QuicSendRetry`
- `CloseRelay`
- `ResumeTcpRecv`
- QUIC peer abort/shutdown event

这些都是 relay 自己的调度语义，应进入用户态 event queue。

### 3.2 Callback 边界

MsQuic callback 可以来自任意线程。它应满足：

- 不调用 `WSARecv`
- 不调用 `WSASend`
- 不直接 `CloseRelay`
- 不直接 `FailRelayFatal`
- 不长时间持有 relay lock
- 不依赖 IOCP 队列长度

callback 只允许：

- 查找 relay/binding
- 构造轻量 event 或 pending receive view
- `TryPush` 到 event queue
- 队列满时进入 callback fallback backlog
- 必要时 `StreamReceiveSetEnabled(false)`
- 唤醒 worker

### 3.3 Worker 边界

Windows relay worker 是状态机 owner。它负责：

- drain event queue
- 处理 TCP IOCP completion
- 提交/重试 QUIC send
- 决定是否提交下一次 `WSARecv`
- 推进 `quic->tcp` receive view drain
- 统一处理 teardown/late callback
- 发布 `RelayHandle.Stop`

---

## 4. 新增 Windows Event Queue

新增文件：

```text
src/tunnel/windows_relay_event_queue.h
```

结构可直接借鉴 `src/tunnel/linux_relay_event_queue.h` 的 bounded ring：

```cpp
enum class TqWindowsRelayEventType {
    Wake,
    QuicReceiveView,
    QuicSendComplete,
    QuicIdealSendBuffer,
    QuicSendRetry,
    ResumeTcpRecv,
    CloseRelay,
    QuicPeerSendAborted,
    QuicPeerReceiveAborted,
    QuicShutdownComplete,
    StopWorker,
};

struct TqWindowsRelayEvent {
    TqWindowsRelayEventType Type{TqWindowsRelayEventType::Wake};
    uint64_t RelayId{0};
    uint64_t Value{0};
    uint32_t Status{0};
    bool Fin{false};
    std::shared_ptr<TqWindowsPendingQuicReceive> ReceiveView;
    TqWindowsQuicSendOperation* SendOperation{nullptr};
};
```

队列接口：

```cpp
class TqWindowsRelayEventQueue final {
public:
    explicit TqWindowsRelayEventQueue(size_t capacity);
    bool TryPush(TqWindowsRelayEvent&& event);
    bool TryPop(TqWindowsRelayEvent& event);
    size_t SizeApprox() const;
    size_t Capacity() const;
};
```

容量来自 tuning：

```cpp
uint32_t WindowsRelayEventQueueCapacity{8192};
uint32_t WindowsRelayWorkerEventBudget{4096};
```

`SizeApprox()` 是设计关键：它让 Windows 端也能看到用户态事件 backlog，解决 IOCP 队列长度不可查的问题。

---

## 5. Worker 主循环

新的 worker loop：

```text
while !stopping:
  DrainEvents(event_budget)
  DrainPerRelayMaintenance()
  Wait/Drain one IOCP completion
  DrainEvents(event_budget)
  DrainPerRelayMaintenance()
```

推荐实现：

1. `DrainEvents(size_t budget)` 从 `EventQueue` pop 事件并分发。
2. `DrainPerRelayMaintenance()` 遍历 active relay：
   - `RetryPendingQuicSends`
   - `DrainCallbackPendingQuicReceives`
   - `MaybePostTcpRecv`
   - `CloseRelayIfDrained`
   - `TryRetireRelay`
3. `GetQueuedCompletionStatus` 使用短 timeout 或 wake-only sentinel，避免 event queue 已有数据但没有 TCP completion 时 worker 睡死。

IOCP wake 只作为提示，不承载业务 payload：

```cpp
void Wake() {
    if (WakeArmed.exchange(true)) {
        return;
    }
    PostQueuedCompletionStatus(Iocp_, 0, 0, nullptr);
}
```

即使 wake post 失败，事件仍在用户态队列里，不得丢弃。worker 下次 timeout 或 TCP completion 后仍能 drain。

---

## 6. `quic->tcp` 路径

### 6.1 新流程

```text
MsQuic RECEIVE callback
  -> build TqWindowsPendingQuicReceive
  -> EventQueue.TryPush(QuicReceiveView)
     success:
       increment queued/depth/bytes metrics
       Wake()
       return QUIC_STATUS_PENDING
     full:
       push to relay.CallbackPendingQuicReceives
       pause StreamReceiveSetEnabled(false)
       Wake()
       return QUIC_STATUS_PENDING

Windows worker
  -> ProcessQuicReceiveViewEvent
  -> enqueue view in relay.PendingReceives if it is not already front-owned
  -> PostTcpSendFromReceiveView / PostTcpSendFromCompressedReceiveView
  -> WSASend completion advances view
  -> ReceiveComplete in batches
  -> low water resumes QUIC receive
```

### 6.2 Fallback backlog

队列满不是 fatal。队列满表示 worker 赶不上 callback 生产速度，此时正确行为是：

- 记录 `WindowsEventQueueFullCount`
- 将 receive view 放入 `CallbackPendingQuicReceives`
- 设置 `QuicReceivePaused=true`
- 调用 `StreamReceiveSetEnabled(false)`
- worker 后续 drain fallback，成功转入 event queue 或直接处理

这与 Linux `CallbackPendingQuicReceives` 一致。

### 6.3 Pending byte accounting

`PendingQuicReceiveBytes` 表示“QUIC receive 已交给应用但还未 ReceiveComplete 的 bytes”。它不应等同于 event queue depth。两者都需要：

- `EventQueue.SizeApprox()`：用户态调度 backlog
- `PendingQuicReceiveBytes`：MsQuic receive ownership backlog
- `PendingQuicReceiveQueueDepth`：relay 内 receive view backlog
- `CallbackPendingQuicReceiveDepth`：callback fallback backlog

---

## 7. `tcp->quic` 路径

### 7.1 当前问题

当前 `QUIC_STREAM_EVENT_SEND_COMPLETE` 中存在直接 `PostTcpRecv` 逻辑。其结果是：

- TCP read 节奏被 QUIC send-complete 绑定。
- send-complete 表示应用 buffer 可回收，不等于 peer ACK，也不等于网络真实发送进度。
- RTT 高或 MsQuic 内部排队时，`WSARecv` 被人为节流。
- callback 线程直接驱动 TCP I/O，形成重入和生命周期复杂度。

### 7.2 新流程

```text
RegisterRelay
  -> worker MaybePostTcpRecv

IOCP TcpRecv completion
  -> HandleTcpRecv
  -> build/compress TqWindowsQuicSendOperation
  -> TrySubmitQuicSendOperation
  -> update OutstandingQuicSendBytes
  -> MaybePostTcpRecv if below high water

MsQuic SEND_COMPLETE callback
  -> EventQueue.TryPush(QuicSendComplete)
  -> Wake()

worker ProcessQuicSendComplete
  -> decrement OutstandingQuicSends/OutstandingQuicSendBytes
  -> free send operation
  -> RetryPendingQuicSends
  -> if below low water, MaybePostTcpRecv
```

### 7.3 Backpressure

新增 relay 状态：

```cpp
std::atomic<bool> TcpReadPausedByQuicBacklog{false};
std::atomic<bool> TcpRecvPosted{false};
std::atomic<uint64_t> OutstandingQuicSendBytes{0};
std::atomic<uint64_t> IdealSendBufferBytes{0};
```

高低水位：

```text
pause threshold = IdealSendBufferBytes if available
                = tuning.WindowsRelayMaxBufferedQuicSendBytes otherwise
resume threshold = pause threshold / 2
```

规则：

- `OutstandingQuicSendBytes >= pause threshold`：不提交新的 `WSARecv`
- `OutstandingQuicSendBytes < resume threshold`：恢复 `WSARecv`
- QUIC send 返回 backpressure status：暂停 TCP read，send op 进入 retry backlog
- `QUIC_IDEAL_SEND_BUFFER_SIZE` 事件更新 `IdealSendBufferBytes` 后可触发 resume

与 Linux 不同的是 Windows 不能取消已提交的 `WSARecv`。因此设计约束是：

- 每个 relay 默认只允许一个 in-flight `WSARecv`
- pause 只阻止提交下一次 recv
- 已完成的 recv 数据仍必须提交 QUIC 或进入 pending send retry，不能丢弃

---

## 8. Operation Ownership

当前 `IoOperation` 同时是 overlapped op、QUIC send context、posted worker op。新设计拆分：

```cpp
struct TqWindowsTcpIoOperation {
    OVERLAPPED Overlapped{};
    TqWindowsTcpIoType Type;
    std::shared_ptr<RelayContext> Relay;
    TqBufferRef BufferOwner;
    WSABUF WsaBuffer{};
    std::shared_ptr<TqWindowsPendingQuicReceive> ReceiveView;
    uint64_t PostedLength{0};
    size_t Offset{0};
};

struct TqWindowsQuicSendOperation {
    static constexpr uint64_t MagicValue = 0x54515753454e4431ull;
    uint64_t Magic{MagicValue};
    uint64_t RelayId{0};
    uint64_t TotalBytes{0};
    bool Fin{false};
    QUIC_SEND_FLAGS Flags{QUIC_SEND_FLAG_NONE};
    std::vector<TqBufferRef> Buffers;
    std::vector<QUIC_BUFFER> QuicBuffers;
};
```

Ownership rules：

- `TqWindowsTcpIoOperation` 由 IOCP completion 释放/复用。
- `TqWindowsQuicSendOperation` 由 MsQuic `ClientContext` 持有，`QuicSendComplete` event 回到 worker 后释放。
- close 时不能假装 in-flight send 已完成；只能进入 retired relay 状态，等待 send-complete 或 explicit abort completion。
- late send-complete 用 magic guard 识别，避免 UAF。

---

## 9. Teardown 语义

所有 QUIC stream terminal events 都通过 event queue 回 worker：

- `PEER_SEND_ABORTED`
- `PEER_RECEIVE_ABORTED`
- `PEER_SEND_SHUTDOWN`
- `SHUTDOWN_COMPLETE`

worker 根据 relay 状态处理：

- pending/in-flight 均为 0：late teardown 降级为 graceful close。
- 仍有 `PendingQuicReceives` / `InFlightTcpSends`：设置 `CloseAfterDrained`，等待 TCP write drain。
- `wsa_recv_post_failed(10053)` 且已有 quic->tcp pending/in-flight：标记 `TcpRecvClosed=true`，设置 `CloseAfterDrained`，不要立即 close socket。
- `wsa_send_*_failed(10053)`：完成 pending receive ownership，设置 `TcpWriteClosed=true`，再按 drain/close 判断。

`CloseRelay` 仍是最终资源释放入口，但不应成为所有 soft teardown 的第一步。

---

## 10. 可观测性

新增 snapshot/trace 字段：

- `WindowsEventQueueDepth`
- `WindowsEventQueueCapacity`
- `WindowsEventQueueFullCount`
- `WindowsEventQueueWakeCount`
- `WindowsEventQueueWakeFailedCount`
- `CallbackPendingQuicReceiveDepth`
- `TcpReadPausedByQuicBacklog`
- `OutstandingQuicSendBytes`
- `MaxOutstandingQuicSendBytes`
- `ResumeTcpRecvEvents`
- `QuicSendCompleteEvents`
- `LateTeardownDowngradedCount`

`stats_active_relay` 应输出：

- worker/relay id
- TCP read/write bytes
- in-flight TCP recv/send
- queued receive bytes/depth
- callback pending receive depth
- outstanding QUIC send bytes/count
- TCP read paused flag
- close/drain flags
- event queue depth

这些字段用于判断 cursor.com/dashboard 卡住时，是正常 keep-alive、event queue 堆积、QUIC receive backlog 未 drain，还是 TCP read 被高水位暂停。

---

## 11. 风险

### 风险 A：一次性重写过大

缓解：按 plan 分阶段迁移。先加 event queue 和测试，再迁移 `quic->tcp`，最后改 `tcp->quic`。

### 风险 B：callback fallback ownership 出错

缓解：所有 `QUIC_STATUS_PENDING` receive view 必须最终 `ReceiveComplete`。测试覆盖队列满、close、late callback、TCP send failure。

### 风险 C：TCP read 暂停后不恢复

缓解：恢复点必须包括 `QuicSendComplete`、`QuicIdealSendBuffer`、`QuicSendRetry` drain、worker maintenance。

### 风险 D：wake-only IOCP 失败导致 worker 睡眠

缓解：GQCS 使用有限 timeout；wake 失败只记指标，不丢事件。

---

## 12. 验收标准

### 单测

- event queue push/pop/capacity/full。
- quic->tcp receive view 通过 event queue drain。
- event queue full 时进入 callback fallback，并暂停 QUIC receive。
- fallback drain 后恢复 QUIC receive。
- `SEND_COMPLETE` callback 不直接调用 `PostTcpRecv`。
- `OutstandingQuicSendBytes` 高水位暂停 TCP read，低水位恢复。
- `wsa_recv_post_failed(10053)` + pending TCP send 不立即硬 close。
- late `PEER_RECEIVE_ABORTED` 在 pending/in-flight 为 0 时降级。

### 手工验证

运行：

```powershell
.\build-x64\bin\Release\tcpquic-proxy.exe client `
  --socks-listen 0.0.0.0:11080 `
  --peer 52.74.45.234:8443 `
  --compress off `
  --trace --trace-interval 30 `
  --ca cert\ca.crt
```

访问 `cursor.com/dashboard`，检查：

- `fatal_relay_resets=0`
- `tcp_hard_errors=0`
- `windows_event_queue_depth` 不长期保持高位
- `callback_pending_quic_receive_depth` 能回落
- `active_relays` 随页面关闭回落
- 大包 tunnel 更多出现 `close_after_drained` 而非 abrupt `relay_stopping`
- 无中途资源截断、dashboard 可正常加载

---

## 13. Spec Self-Review

- Placeholder scan：无未完成标记或未定义后续步骤。
- Scope check：本设计只覆盖 Windows relay 队列、背压、teardown，不包含 idle timeout 策略和协议变更。
- Consistency check：IOCP payload 只保留 TCP I/O completion；用户态事件统一进入 `TqWindowsRelayEventQueue`。
- Ambiguity check：`SEND_COMPLETE` 不再直接 `PostTcpRecv`，恢复 TCP read 的唯一入口是 worker `MaybePostTcpRecv`。
