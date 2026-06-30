# Windows IOCP receive FIN 乱序处理怀疑与验证方案

## 背景

2026-06-30 的内置 `--download-test 100` 中，server 为 Linux，本机为 server，Windows 为 client。Windows 侧下载方向在测试中途出现无速度。

Windows client 日志显示，最后一次 download 方向测试在 `2026-06-30 18:49:57` 建立 tunnel：

```text
event=stream_started role=client conn=1 tunnel=1 target=127.0.0.1:33729 compress=off
event=relay_started tunnel=1 backend=windows worker=0 relay_id=1
```

约 14 秒后出现大量同类日志：

```text
[2026-06-30 18:50:11.731] [info] event=relay_stop_condition worker=0 relay=1
outstanding_quic_sends=0 outstanding_quic_send_bytes=0
pending_tcp_write_queue=1193 pending_tcp_write_bytes=0
pending_quic_receive_bytes=5541973
tcp_read_bytes=0 tcp_write_bytes=1566673990
tcp_write_errno=0 tcp_recv_errno=0 tcp_send_errno=0 iocp_errno=0
tcp_read_closed=0 tcp_write_closed=1
quic_send_fin_submitted=0 quic_send_fin_completed=0
trigger=quic_receive_after_tcp_write_closed
```

这里的 `pending_tcp_write_queue` 在 Windows relay 状态中实际来自 `PendingQuicReceiveQueueDepth`，表示等待写入本地 TCP socket 的 QUIC receive view 队列深度。该状态说明：relay 已经关闭本地 TCP 写方向，但仍有约 1193 个 receive view、约 5 MiB 数据等待写入。

## 当前怀疑

怀疑点不是 QUIC stream 协议层的 FIN 乱序。QUIC stream FIN 本身应该遵守 stream 字节流顺序。

更精确的怀疑是：Windows relay 在把 MsQuic receive callback 异步转发到 IOCP worker 时，没有建立 relay-local 的有序 receive 队列或序号校验。FIN receive view 可能在本项目的 worker 处理层面先于部分早到的数据 receive view 被处理完成，从而提前设置 `TcpWriteClosed=true`，导致后续数据 view 被 `quic_receive_after_tcp_write_closed` 路径丢弃。

## 相关代码路径

MsQuic 配置开启了 multi receive：

```cpp
settings.SetStreamMultiReceiveEnabled(true);
```

位置：`src/protocol/quic_session.cpp`

MsQuic 文档说明 multi receive 模式下，在前一个 receive 还没有 `StreamReceiveComplete` 时，应用仍可能继续收到新的 `QUIC_STREAM_EVENT_RECEIVE`。因此应用必须自己维护 pending receive 的处理纪律。

Windows stream callback 中，每个 receive event 会构造成一个 `TqWindowsPendingQuicReceive`，然后作为 IOCP payload 单独投递：

```cpp
if (event->Type == QUIC_STREAM_EVENT_RECEIVE) {
    const bool fin = (event->RECEIVE.Flags & QUIC_RECEIVE_FLAG_FIN) != 0;
    auto view = worker->BuildDeferredQuicReceiveView(..., fin, 0);
    if (worker->PostCallbackOperationById(
            TqWindowsIocpOperationType::RelayReceiveReady,
            *binding,
            0,
            0,
            view)) {
        return QUIC_STATUS_PENDING;
    }
}
```

位置：`src/tunnel/windows_relay_worker.cpp`

真正进入 relay 的 `PendingReceives` 是 worker 从 IOCP 取出 `RelayReceiveReady` 后：

```cpp
case TqWindowsIocpOperationType::RelayReceiveReady:
    if (op->ReceiveView &&
        !EnqueueDeferredQuicReceiveView(relay, op->ReceiveView)) {
        ...
    }
    DrainRelayReceives(relay);
    break;
```

`EnqueueDeferredQuicReceiveView()` 中只是 `push_back`：

```cpp
relay->PendingReceives.push_back(view);
```

因此当前顺序依赖两个外部事实：

- MsQuic 对同一 stream 是否绝对串行调用 callback。
- `PostQueuedCompletionStatus()` 到 `GetQueuedCompletionStatus()` 是否在当前多 producer 情况下保持同一 stream receive event 的投递顺序。

代码本身没有显式的 `ReceiveSeq`、`AbsoluteOffset` 或 FIN barrier 来校验和恢复顺序。

FIN view 完成时，当前代码立即关闭本地 TCP 写方向：

```cpp
if (view->Fin) {
    relay->CloseAfterDrained.store(true, std::memory_order_release);
    if (!relay->TcpWriteClosed.exchange(true, std::memory_order_acq_rel) &&
        TqSocketValid(relay->TcpFd)) {
        (void)TqShutdownSend(relay->TcpFd);
        GracefulRelayDrains_.fetch_add(1, std::memory_order_relaxed);
    }
}
```

位置：`FinishReceiveView()`。

如果 FIN view 在 worker 处理层面越过了早到但未 drain 的数据 view，就会出现日志中的状态：`tcp_write_closed=1` 但 `pending_tcp_write_queue` 和 `pending_quic_receive_bytes` 仍非零。

## 为什么现有日志还不足以最终确认

当前日志能证明 Windows relay 出现了不合理状态：

```text
tcp_write_closed=1
pending_tcp_write_queue=1193
pending_quic_receive_bytes=5541973
trigger=quic_receive_after_tcp_write_closed
```

但它还不能直接证明 FIN receive view 是否真的越过了较早的数据 view。缺少以下信息：

- receive event 创建顺序。
- receive event 入 IOCP 顺序。
- receive event 从 IOCP 出队顺序。
- receive view 入 `PendingReceives` 顺序。
- `finish_fin` 时队列里是否仍存在更早序号的数据 view。
- 触发 `quic_receive_after_tcp_write_closed` 的 view 序号是否小于或大于 FIN view 序号。

## 建议增加的验证日志

建议给 `TqWindowsPendingQuicReceive` 增加 relay-local 单调序号，例如：

```cpp
uint64_t ReceiveSeq{0};
```

在 `RelayContext` 增加：

```cpp
std::atomic<uint64_t> NextReceiveSeq{1};
std::atomic<uint64_t> FinReceiveSeq{0};
```

在 `BuildDeferredQuicReceiveView()` 或 receive callback 构造 view 后分配：

```cpp
view->ReceiveSeq = binding/relay 对应的 NextReceiveSeq.fetch_add(1);
```

需要记录以下日志点。

### callback 收到 RECEIVE

在 `QUIC_STREAM_EVENT_RECEIVE` callback 中记录：

```text
event=windows_receive_callback relay=<id> seq=<seq> fin=<0|1>
buffer_count=<n> total_length=<bytes> flags=<flags>
```

目的：确认 MsQuic 交付给应用 callback 的 receive/FIN 顺序。

### callback 投递 IOCP

在 `PostCallbackOperationById(RelayReceiveReady, ...)` 前后记录：

```text
event=windows_receive_post_iocp relay=<id> seq=<seq> fin=<0|1> post_ok=<0|1>
```

目的：确认应用投递到 IOCP 的顺序和是否有失败 fallback。

### worker 从 IOCP 处理 RelayReceiveReady

在 worker 处理 `RelayReceiveReady` 时记录：

```text
event=windows_receive_iocp_dequeue relay=<id> seq=<seq> fin=<0|1>
```

目的：确认 IOCP 出队顺序是否与 callback/post 顺序一致。

### 入 PendingReceives 队列

在 `EnqueueDeferredQuicReceiveView()` 中记录：

```text
event=windows_receive_queue relay=<id> seq=<seq> fin=<0|1>
queue_depth=<depth> pending_bytes=<bytes>
```

目的：确认 relay 自己的 pending 队列顺序。

### FIN 完成时

在 `FinishReceiveView()` 的 `view->Fin` 分支记录：

```text
event=windows_receive_finish_fin relay=<id> seq=<seq>
queue_depth=<depth> pending_bytes=<bytes>
inflight_tcp_sends=<n> tcp_write_closed=<0|1>
```

目的：验证 FIN 完成时是否仍有 pending 数据或 in-flight TCP send。

### 数据在 TCP 写关闭后被处理

在 `PostTcpSendFromReceiveView()` 的 `TcpWriteClosed` 分支记录：

```text
event=windows_receive_after_tcp_write_closed relay=<id> seq=<seq> fin=<0|1>
fin_seq=<seq> queue_depth=<depth> pending_bytes=<bytes>
```

判定标准：

- 如果看到 `finish_fin seq=N` 后，又出现 `windows_receive_after_tcp_write_closed seq<M` 且 `M < N`，可以直接证明 worker 处理层面 FIN 越过了前面的数据 view。
- 如果 `M > N`，说明 FIN 没有越过已有 view，但 FIN 后仍有新数据 view 被处理，需要继续查 MsQuic multi receive 完成计数或应用过早 `StreamReceiveComplete` 的路径。
- 如果 `finish_fin` 时 `queue_depth>0` 或 `inflight_tcp_sends>0`，即使没有 seq 反转，也说明当前 FIN 分支立即 `shutdown(SD_SEND)` 的行为过早，至少应该延迟到 drain 完成。

## 临时判定逻辑

验证前先不要把根因写死为 “MsQuic FIN 乱序”。当前更稳妥的判定是：

1. Windows relay 侧确实出现 `tcp_write_closed=1` 且仍有 pending receive 的异常状态。
2. 当前 Windows relay 没有显式保证 multi receive 下的 per-stream receive view 处理顺序。
3. FIN 分支会立即关闭本地 TCP 写方向，这个动作对异步 receive queue 很敏感。
4. 增加 `ReceiveSeq` 日志后，才能确认是 IOCP 出队乱序、FIN 关闭过早，还是其他路径提前设置了 `TcpWriteClosed`。

## 可能修复方向

如果验证确认 FIN view 先于早到数据 view 被处理，应考虑：

- QUIC receive callback 不再把 receive view 作为 IOCP payload 直接投递。
- 改为在 relay-local 的有序队列中入队 receive view，并只用 IOCP wake worker。
- 对每个 receive view 记录 `ReceiveSeq`，worker 按 seq drain。
- FIN 只设置 `CloseAfterDrained=true`，不要立即 `TcpWriteClosed=true` / `shutdown(SD_SEND)`。
- `CloseRelayIfDrained()` 在 `PendingReceives`、`PendingQuicReceiveBytes`、`InFlightTcpSends`、callback pending receive 全部清零后统一关闭 TCP 写方向。

该方向与既有设计文档 `docs/superpowers/specs/2026-06-25-windows-linux-style-relay-design.md` 一致：Windows quic->tcp 数据/receive view 不应通过 `PostQueuedCompletionStatus` 承载业务 payload，而应使用可观测、可限流、可按 relay drain 的用户态队列。
