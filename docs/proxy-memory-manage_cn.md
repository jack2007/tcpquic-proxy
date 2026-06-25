# tcpquic-proxy Linux Relay 内存释放汇总（`tcp->quic` / `quic->tcp`）

> 说明范围：基于当前 `src/tunnel/linux_relay_worker.cpp` 实现。  
> 重点是“路径上的内存何时可回收”，不是“TCP/QUIC 网络层何时真正送达”。

## 一、`tcp -> quic`（TCP recv / QUIC send）

### 正常路径（成功）

1. `DrainTcpReadable()` 使用 `TqAllocateRelayBuffer()` 从 relay buffer 池分配 `TqBufferRef`。
   - 读取后切成 `std::vector<TqBufferView>`，`TqBufferView` 里 `Owner` 持有 `TqBufferRef`，负责后续释放。
   - 无压缩场景 `BuildTcpToQuicViews()` 直接 `output = std::move(input)`，属于零拷贝沿用已读内存。
   - 压缩场景会生成新的 relay buffer，并 `memcpy` 压缩结果（非零拷贝）。
2. `SubmitTcpBatchToQuic()` 将 `TqBufferView` 整包进 `TqLinuxRelaySendOperation`，并调用 `TqLinuxRelayStreamSend(...)`。
3. 发送提交成功后 `OutstandingQuicSends/Bytes` 计数加一，内存暂留在 `TqLinuxRelaySendOperation` 内。
4. 收到 `QUIC_STREAM_EVENT_SEND_COMPLETE` 时：
   - 回调里只入队 `QuicSendComplete` 事件；
   - 工作线程执行 `CompleteQuicSend(context)`。
5. `CompleteQuicSend()`：
   - `delete operation;`（`operation` 的 `Views` 被析构，随即释放 `TqBufferRef` 持有的 buffer）
   - 回减 `OutstandingQuicSends/Bytes`
   - 如果 FIN 已提交则置 `QuicSendFinCompleted`。

结论：`tcp->quic` 方向的内存回收触发于 `SEND_COMPLETE` 事件驱动的 `CompleteQuicSend`，而不是“对端 ACK 了每个包”的直接语义。

### 异常路径

#### 1) 提交阶段失败/构建失败
- `TqAllocateRelayBuffer` 失败、`TqLinuxRelayStreamSend` 失败、压缩失败等会在对应分支 `SetRelayStop`/`return false`，并且未成功入队到 msquic 的 `Send` 会立即释放本次 `sendViews`（`TqBufferView` 生命周期结束或 `operation` 析构）。

#### 2) 对端异常或流关闭
- `ProcessQuicPeerSendAborted` / `ProcessQuicPeerReceiveAborted` 会走 `FailRelayFatal(...)`。
- `FailRelayFatal` -> `AbortRelayAndRelease(...)`：
  - 关闭 TCP socket、标记 relay closing；
  - `PendingQuicSendRetries` 清空；
  - 若非 closing 且 `abortStream=true`，调用 `Stream->Shutdown(...ABORT)`；
  - 未展示的 `outstanding send operation` 由 msquic 的 stream 回调收敛（按 msquic 关闭语义）触发 `SEND_COMPLETE` 侧回收；因此不会因网络中断永久悬挂未完成 send。
- `ProcessQuicShutdownComplete` 若还有未决后续（`HasPendingAfterStreamShutdown` 为 true）同样会进入 `AbortRelayAndRelease(..., false)` 做清理。

#### 3) relay 生命周期退出
- `UnregisterRelay()` 与 `AbortRelayAndRelease()` 都会将 `PendingTcpWrites / PendingQuicSendRetries / PendingQuicReceives` 等队列清空；后续的 `TqBufferView`/`TqBufferRef` 通过析构释放 buffer。

## 二、`quic -> tcp`（QUIC receive callback / TCP write）

### 正常路径（成功）

1. `OnStreamEventWithBinding()` 在 `QUIC_STREAM_EVENT_RECEIVE` 中返回 `QUIC_STATUS_PENDING`，并把事件中的 buffer 封装到 `QueueDeferredQuicReceive*()`：
   - 非压缩：`TqPendingQuicReceive` 保存 `const uint8_t*` 切片引用（`TqQuicReceiveSlice`）。
   - 事件入队到 worker，随后 `ProcessQuicReceiveViewEvent()` 把 `ReceiveView` 放入 `PendingQuicReceives`。
2. `FlushDeferredQuicReceives()` 取出 `PendingQuicReceives`，把 slice 映射为 `iovec`，`sendmsg` 写 TCP。
3. 每次成功写入 `sent` 字节时，更新 view 的 `CompletedLength` / `PendingCompleteBytes`，并更新队列统计。
4. `FlushDeferredReceiveCompletion()` 满足阈值后调用 `CompleteDeferredQuicReceive()` -> `stream->ReceiveComplete(bytes)`。
5. `view` 全部消费后出队，FIN 触发 `TcpWriteShutdownQueued`。

结论：`quic->tcp` 方向的内存回收是通过 `MsQuicStream::ReceiveComplete(bytes)` 通知 msquic 已消费，不是依赖 TCP 成功到达远端。

### 异常路径

#### 1) TCP 写失败/错误
- `sent < 0` 且非 `EAGAIN` 时（包括 `EPOLLOUT` 触发后的 `tcp_write_hard_error`、`tcp_socket_error`）：
  - `AbortRelayAndRelease(relay, ..., true)`
  - 清空 `PendingQuicReceives` 和 `CallbackPendingQuicReceives`，对每个条目执行 `CompleteAndDiscardQuicReceive()`。
  - `CompleteAndDiscardQuicReceive()` 会把 `view` 剩余未完成字节补齐到 `PendingCompleteBytes` 后 `FlushDeferredReceiveCompletion(..., true)`，最终触发 `ReceiveComplete(remaining)`，防止 msquic 侧长时间挂着未确认数据。

#### 2) relay/stream 异常移除
- `ProcessQuicPeerSendAborted`、`ProcessQuicPeerReceiveAborted` 等都走 `FailRelayFatal`，其清理逻辑同上。
- `ProcessQuicReceiveViewEvent()` 收到已经 closing 的 `relay` 时，会直接 `CompleteAndDiscardQuicReceive(*event.ReceiveView)`，不等待 TCP 可写。
- `AbandonOrphanedEventBuffers()` 在 orphan event 到达时 `abandon()` 掉 `TqBufferRef`，防止 orphan 缓存泄露。

## 三、`tcp->quic` 与 `quic->tcp` 的本质差异

- 两条路径都不是“等到对端确认后才释放”；
- 它们都通过 msquic 的**协议层回调语义**释放：  
  - `tcp->quic` 用 `SendComplete` 回收 `TqBufferView`；  
  - `quic->tcp` 用 `ReceiveComplete(bytes)` 回收接收引用。
- 异常场景下不会靠业务重试队列“永久持有”：
  - `tcp->quic` 依赖 msquic 在关闭/abort/shutdown 上清空未完成 send 的回调；
  - `quic->tcp` 在关闭前调用补齐 `ReceiveComplete` 的兜底路径。
