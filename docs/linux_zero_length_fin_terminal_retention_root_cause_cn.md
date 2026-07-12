# Linux 零长度 FIN 导致 terminal retention 无法收敛根因分析

## 1. 结论摘要

2026-07-12，客户端进程 `636345` 中出现一条长期无法关闭的 tunnel。该对象为客户端 tunnel 56，对应服务端 tunnel 128，目标为 `api.hcaptcha.com:443`。

最终确认的根因不是服务端未关闭、网络丢失 terminal 通知，也不是 terminal scheduler 或 reaper 线程死锁，而是 Linux relay worker 在特定关闭竞态中遗漏了零长度 FIN 对应的 `StreamReceiveComplete(0)`：

1. MsQuic 向应用投递 `TotalBufferLength=0` 且带 `QUIC_RECEIVE_FLAG_FIN` 的 RECEIVE 事件。
2. Linux relay callback 返回 `QUIC_STATUS_PENDING`，表示应用接管了该 receive，后续必须调用 `StreamReceiveComplete`；即使长度为 0，该完成调用也不可省略。
3. receive view 尚未走完正常 drain 路径时，relay 因 TCP EOF、发送 FIN completion 等事件进入 graceful terminal handoff 和 `Closing` 状态。
4. 零长度 FIN view 随后进入 `CompleteAndDiscardQuicReceive()` 清理路径。
5. 该 helper 只按剩余字节数调用 `FlushDeferredReceiveCompletion()`；零长度 view 的剩余字节数为 0，因此没有调用 `StreamReceiveComplete(0)`。
6. MsQuic 永久保持 `ReceiveDataPending=1`，无法设置 `RemoteCloseFin` 和 `RemoteCloseAcked`，所以不会发布 `QUIC_STREAM_EVENT_SHUTDOWN_COMPLETE`。
7. terminal ledger 一直停在 `shutdown_submitted`，retention registry 持续强持 stream owner，reaper 的 terminal 门禁永远无法满足。

`graceful_complete` 不启动 fatal watchdog 是问题长期暴露而不自动恢复的放大因素，但不是本次事件的首要根因。

## 2. 运行环境

### 2.1 客户端

```text
PID: 636345
程序: build/bin/Release/raypx2
角色: client
Admin: 0.0.0.0:2346
对端: amazon-sgp / 52.74.45.234:8443
```

客户端异常对象：

```text
tunnel_id: 56
stream_id: 56（terminal identity）
MsQuic stream ID: 224
target: api.hcaptcha.com:443
worker: 6
relay_id: 7
```

### 2.2 服务端

```text
主机: 52.74.45.234
PID: 20482
程序: build/bin/Release/raypx2
角色: server
Admin: 0.0.0.0:2345
```

对应服务端对象：

```text
connection: srv-7 / 日志 conn=7
tunnel_id: 128
target: api.hcaptcha.com:443
worker: 0
relay_id: 63
```

## 3. 两端对象关联证据

客户端 tunnel 56 创建时间和服务端 tunnel 128 接收时间相邻，目标一致：

```text
客户端 2026-07-12 08:01:02.643 +08:00
event=stream_started role=client conn=1 tunnel=56 target=api.hcaptcha.com:443

服务端 2026-07-12 00:01:02.696 +00:00
event=stream_started role=server conn=7 tunnel=128 target=api.hcaptcha.com:443
```

两端字节计数完全镜像：

| 方向 | 客户端 tunnel 56 | 服务端 tunnel 128 |
|---|---:|---:|
| 客户端到服务端 | TCP read `2385` | TCP write `2385` |
| 服务端到客户端 | TCP write `4373` | TCP read `4373` |

服务端 relay 63 的日志也显示目标 TCP 对端为 `104.19.230.21:443`，与该 `api.hcaptcha.com` tunnel 对应。

因此可以确定客户端 tunnel 56 与服务端 tunnel 128 是同一条 QUIC stream 的两端，而不是仅凭域名猜测关联。

## 4. 关闭时间线

### 4.1 客户端

```text
08:02:25.146
event=linux_relay_tcp_read worker=6 relay=7 result=eof

08:02:25.248
event=linux_relay_stream_event worker=6 relay=7
stream_event=receive_fin
total_buffer_length=0
buffer_count=0
receive_flags=0x2
fin=1

08:02:25.248
event=linux_relay_stop_condition worker=6 relay=7
trigger=tcp_write_shutdown_complete
```

此后该 stream 没有再记录 `SEND_SHUTDOWN_COMPLETE` 或 `SHUTDOWN_COMPLETE`。

客户端 Admin retention 长期保持：

```json
{
  "stream_id": 56,
  "tunnel_id": 56,
  "terminal_phase": "shutdown_submitted",
  "shutdown_intent": "graceful_complete",
  "shutdown_status": "pending",
  "shutdown_attempt": 1,
  "last_stream_event": "none",
  "watchdog_state": "idle",
  "connection_escalated": false
}
```

同时 relay runtime 显示：

```text
active_relays=1
closing_relays=1
active_quic_send_relays=1
outstanding_quic_sends=0
outstanding_quic_send_bytes=0
pending_quic_receive_bytes=0
tcp_fd=-1
```

这说明数据面已经没有实际工作，残留对象只是在等待 terminal 事实。

### 4.2 服务端

服务端对应 stream 正常完成：

```text
00:02:25.206
worker=0 relay=63 stream_event=receive_fin
total_buffer_length=0
fin=1

00:02:25.228
worker=0 relay=63 target TCP result=eof

00:02:25.308
worker=0 relay=63 trigger=quic_send_fin_completed

00:02:25.308
worker=0 relay=63 stream_event=shutdown_complete

00:02:25.308
worker=0 relay=63 event=linux_relay_unregister

00:02:25.355
tunnel=128 event=stream_closed
```

调查时服务端状态为：

```text
terminal-retentions: 0
srv-7 active_streams: 0
srv-7 active_tunnels: 0
srv-7 state: connected
```

因此服务端已经完成 stream terminal 和本地资源释放，QUIC connection 仍健康。服务端不是 retention 的持有方。

## 5. 客户端 live MsQuic 状态

通过只读附加客户端进程，并从日志中的 `MsQuicStream*` wrapper 取得其底层 `HQUIC`，读取到 stream ID 224 的实时 flags：

```text
LocalCloseFin       = 1
LocalCloseAcked     = 1
FinAcked            = 1

ReceiveDataPending  = 1
RemoteCloseFin      = 0
RemoteCloseAcked    = 0

HandleSendShutdown  = 1
HandleShutdown      = 0
ShutdownComplete    = 0
InStreamTable       = 1
```

callback handler 和 callback context 均仍有效，因此不是 callback/context 被提前清空。

这些 flags 表明：

- 客户端发送方向已经发送 FIN，并已被对端确认。
- 接收方向仍有一个由应用接管但未完成的 receive。
- MsQuic 尚未把远端 FIN 记入完成状态。
- stream 仍保留在 connection stream table 中。
- `SHUTDOWN_COMPLETE` 的发布条件尚未成立。

这将问题从“为什么 MsQuic 没有通知”收敛为“为什么应用没有完成已经返回 PENDING 的零长度 receive”。

## 6. MsQuic 状态机依据

MsQuic 收到 FIN 时会先设置 `ReceiveDataPending` 并向应用投递 RECEIVE。应用返回 `QUIC_STATUS_PENDING` 后，需要通过 `StreamReceiveComplete` 归还该 receive。

在 receive 被完全消费后，MsQuic 才会执行以下状态迁移：

```text
ReceiveDataPending = 0
RemoteCloseFin = 1
RemoteCloseAcked = 1
QuicStreamTryCompleteShutdown()
```

相关实现位于：

- `third_party/msquic/src/core/stream_recv.c`：receive pending、FIN drain 和 `RemoteCloseFin/RemoteCloseAcked` 发布。
- `third_party/msquic/src/core/stream.c`：只有 `LocalCloseAcked && RemoteCloseAcked && !ReceiveDataPending` 时，`QuicStreamTryCompleteShutdown()` 才发布 `SHUTDOWN_COMPLETE`。

因此本次 MsQuic 行为符合其状态机约束：只要应用仍欠一个 receive completion，它就不能宣告 stream terminal。

## 7. 本地代码根因

零长度 FIN view 在构造时已被正确标记：

```cpp
view->ZeroLengthFinCompletionPending = fin && view->TotalLength == 0;
```

正常 drain 的多个分支也已经调用：

```cpp
CompleteZeroLengthFinReceive(*view);
```

但公共清理 helper `CompleteAndDiscardQuicReceive()` 仍只有：

```cpp
void TqLinuxRelayWorker::CompleteAndDiscardQuicReceive(
    TqPendingQuicReceive& view) {
    const uint64_t remaining = view.TotalLength >= view.CompletedLength
        ? view.TotalLength - view.CompletedLength
        : 0;
    if (remaining != 0) {
        view.PendingCompleteBytes += remaining;
        view.CompletedLength += remaining;
    }
    FlushDeferredReceiveCompletion(view, true);
}
```

对于 `TotalLength == 0` 的 FIN：

- `remaining == 0`；
- `PendingCompleteBytes == 0`；
- `FlushDeferredReceiveCompletion()` 不会调用 `ReceiveComplete`；
- `ZeroLengthFinCompletionPending` 没有被消费；
- view 被移除或析构后，完成义务永久丢失。

该 helper 被以下与本次竞态相关的分支调用：

1. `ProcessQuicReceiveViewEvent()` 发现 relay 不存在或已经 `Closing`。
2. relay handoff/abort 清理 `PendingQuicReceives`。
3. relay handoff/abort 清理 `CallbackPendingQuicReceives`。
4. retired relay 或 unregister 清理非 terminal pending receive。

因此已有的正常 drain 覆盖不能保证所有 ownership exit 都完成零长度 FIN。

## 8. 竞态重建

本次事故最符合以下事件顺序：

```text
MsQuic callback
  收到 TotalBufferLength=0 + FIN
  构造 ZeroLengthFinCompletionPending=true 的 receive view
  投递到 worker queue
  返回 QUIC_STATUS_PENDING

Linux worker
  处理 TCP EOF / 本地 FIN send completion / TCP SHUT_WR 完成
  满足本地 fully-closed 条件
  启动 graceful terminal handoff
  relay 进入 Closing，数据面被冻结

Linux worker
  随后处理零长度 FIN receive view，或在 handoff 中清理该 view
  进入 CompleteAndDiscardQuicReceive()
  因字节数为 0，没有调用 ReceiveComplete(0)

MsQuic
  ReceiveDataPending 永久保持为 1
  RemoteCloseAcked 无法置位
  不发布 SHUTDOWN_COMPLETE

terminal convergence
  ledger 停在 shutdown_submitted
  graceful watchdog 保持 idle
  retention registry 继续强持 owner
  reaper 等待真实 terminal，无法释放 tunnel
```

该竞态不要求 event queue full，也不要求 TCP 或 QUIC 数据仍未排空。它只要求零长度 FIN view 与 relay 进入 `Closing` 的顺序交错。

## 9. 为什么已有修复和测试没有拦住

提交 `1e370fc` 和 `2b25fcc` 已引入 `ZeroLengthFinCompletionPending` 与 `CompleteZeroLengthFinReceive()`，覆盖了：

- 正常未压缩 receive drain；
- sink receive；
- 压缩 receive；
- 正常 pending queue drain；
- 若干 FIN-only 单元测试。

但这些测试主要验证 receive view 在有效、未关闭 relay 上被正常消费。它们没有覆盖以下组合：

```text
零长度 FIN
+ callback 返回 PENDING
+ view 尚未完成
+ relay 先进入 Closing 或 terminal handoff
+ view 从 discard/cleanup ownership exit 离开
```

`CompleteAndDiscardQuicReceive()` 在引入零长度 FIN 专用完成逻辑后没有同步更新，成为覆盖缺口。

## 10. 放大因素与次要问题

### 10.1 Graceful terminal 不启动 watchdog

`TqStreamLifetime::BeginTerminalShutdown()` 只为 fatal `AbortBothImmediate` 启动 watchdog。`GracefulComplete` 被明确排除，并有测试保证推进 35 秒后仍保持 `watchdog=idle`。

这符合“不把正常 FIN half-close 当作 fatal”的原设计，但当应用遗漏 receive completion 时，没有第二条路径把永久 pending 转为可观测超时或 connection escalation。因此它是永久 retention 的放大因素，而不是遗漏 completion 的根因。

### 10.2 Retention handoff facts 与实际 fd 状态不一致

Admin retention 中曾显示：

```text
in_tunnel_registry=true
relay_active=true
tcp_valid=true
```

但 relay runtime 同时显示 `tcp_fd=-1`。这说明 retention handoff facts 没有及时反映 worker 已关闭 TCP fd 的事实，降低了诊断准确性。该问题不导致 retention，但建议独立修正或增加明确的 `tcp_fd_closed`/`data_plane_stopped` 字段。

## 11. 建议修复范围

本报告只记录根因，尚未实施以下修复。

### 11.1 首要修复

应确保 `CompleteAndDiscardQuicReceive()` 在丢弃非 terminal view 前，同时完成两类义务：

1. 正长度 receive 的剩余字节 completion；
2. `ZeroLengthFinCompletionPending` 对应的 `ReceiveComplete(0)`。

更稳妥的设计是把“完成 view 的全部 MsQuic receive ownership”集中到一个 helper，使所有正常 drain、closing、fallback 和 handoff 清理路径都调用同一出口，避免每个分支分别记住零长度 FIN 特例。

### 11.2 门禁加固

评估 graceful handoff 的 fully-closed predicate 是否还应检查：

- `CallbackPendingQuicReceives`；
- 已入 MPMC queue 但尚未挂入 relay `PendingQuicReceives` 的 receive view obligation。

即使增加门禁，公共 cleanup helper 仍必须正确完成零长度 FIN，因为 relay abort、worker stop、queue fallback 等路径依然可能清理 receive view。

### 11.3 Graceful 长期 pending 诊断

不建议直接把 fatal watchdog 原样套到正常 half-close。可以独立增加只诊断、不立即升级连接的 graceful terminal age 监控，用于暴露：

- `ReceiveDataPending` completion 欠账；
- 正常关闭长期没有 terminal；
- retention facts 与实际 worker 状态不一致。

是否允许 graceful 超时后升级连接，需要单独设计，避免破坏正常 FIN half-close 语义。

## 12. 必需回归测试

至少应增加以下确定性测试，使用 barrier/latch 或可控 event ordering，不使用 sleep 猜测竞态：

1. **零长度 FIN 在 relay Closing 后到达**
   - callback 返回 `QUIC_STATUS_PENDING`；
   - worker 处理 view 时 relay 已为 `Closing`；
   - 断言恰好调用一次 `StreamReceiveComplete(0)`。

2. **零长度 FIN 位于 PendingQuicReceives，handoff 先执行**
   - handoff 清理 pending queue；
   - 断言 completion exactly-once；
   - 断言不存在 pending receive obligation。

3. **零长度 FIN 位于 CallbackPendingQuicReceives，handoff 先执行**
   - 强制正常 event queue enqueue 失败，进入 callback fallback queue；
   - handoff 清理 fallback queue；
   - 断言调用 `ReceiveComplete(0)`。

4. **receive view 与 SHUTDOWN_COMPLETE 并发**
   - terminal 已发布时不得对失效 wrapper 再调用 stream API；
   - 非 terminal cleanup 必须完成 receive；
   - 验证两种 ownership 分支互斥且 exactly-once。

5. **完整双向 FIN 集成回归**
   - 复现客户端发送 FIN、服务端返回零长度 FIN、worker 并发进入 graceful handoff；
   - 最终必须观察真实 `SHUTDOWN_COMPLETE`；
   - terminal retention 回到 0；
   - tunnel registry 和 relay closing count 回到 0。

6. **多轮压力回归**
   - 在 delay/loss 环境批量关闭大量短连接；
   - 断言 `terminal_retained_owner_count == 0`；
   - 断言 `terminal_sink_pending == 0`；
   - 断言 exactly-once violation counters 全为 0。

## 13. 修复验收标准

修复完成后应同时满足：

- 所有返回 `QUIC_STATUS_PENDING` 的 receive 最终都有 exactly-once completion 或由真实 terminal 接管。
- 零长度 FIN 在正常、closing、fallback、handoff 和 worker stop 路径均调用一次 `ReceiveComplete(0)`。
- 客户端 MsQuic stream 最终达到：

```text
ReceiveDataPending=0
RemoteCloseFin=1
RemoteCloseAcked=1
ShutdownComplete=1
```

- 客户端收到真实 `QUIC_STREAM_EVENT_SHUTDOWN_COMPLETE`。
- tunnel、relay、terminal sink 和 retention registry 全部释放。
- 不通过直接 `StreamClose`、伪造 terminal 或重启进程掩盖问题。
- 正常单向 FIN half-close 行为不变。

## 14. 根因置信度

根因置信度为高，依据包括：

1. 两端 tunnel 通过时间、目标和双向字节数精确关联。
2. 服务端对应 stream 已正常收到 `SHUTDOWN_COMPLETE` 并释放。
3. 客户端日志明确记录零长度 FIN，并对 RECEIVE 返回 pending 语义。
4. 客户端 live MsQuic flags 明确显示唯一未收敛条件为 `ReceiveDataPending=1` 和远端关闭未确认。
5. 本地 cleanup helper 对正长度 receive 有完成逻辑，但确实遗漏零长度 FIN 专用 completion。
6. 该 helper 正好位于 Closing/handoff 竞态使用路径中。

因此无需假设服务端异常、QUIC connection 断开或 MsQuic 丢失 callback，即可完整解释所有已观察现象。
