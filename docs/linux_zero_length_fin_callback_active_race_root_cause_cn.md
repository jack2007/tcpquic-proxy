# Linux 零长度 FIN callback-active completion 竞态根因分析

## 1. 结论摘要

2026-07-14，服务端进程 `28214` 的 `srv-1` 上出现一条客户端 stream 已关闭、
但服务端 tunnel 长期无法关闭的异常对象：

```text
admin tunnel: tun-935
terminal stream_id: 1895
terminal tunnel_id: 1896
relay: worker 1 / relay 454
target: translate.googleapis.com:443
connection: srv-1 / 日志 conn=1
client_name: linux-spark-1619
```

最终确认：数据面已经完全停止，残留的不是 TCP fd、待发送数据或 worker 任务，
而是一个 MsQuic 零长度 FIN receive completion obligation。MsQuic live flags 显示
`ReceiveDataPending=1`，因此它不能设置 `RemoteCloseFin/RemoteCloseAcked`，也不会发布
真实的 `QUIC_STREAM_EVENT_SHUTDOWN_COMPLETE`。

本次事件与 2026-07-12 的
[Linux 零长度 FIN cleanup 遗漏问题](linux_zero_length_fin_terminal_retention_root_cause_cn.md)
有关，但不是同一个代码缺口：

- 旧问题：closing/discard helper 完全没有调用 `ReceiveComplete(0)`。
- 本次问题：当前 binary 已包含旧修复；worker 虽然会调用 `ReceiveComplete(0)`，
  但可能在 RECEIVE callback 尚未返回 `QUIC_STATUS_PENDING` 时调用。
- MsQuic 用“累计完成字节数”传递 callback-active 窗口内的并发 completion；
  `+0` 不改变累计值，所以零字节 completion 在该窗口内不可见。
- callback 返回 `PENDING` 后观察不到已发生的零字节 completion，最终永久保留
  `ReceiveDataPending=1`。

`graceful_complete` 不启动 fatal watchdog，使该欠账没有自动收敛路径，是问题长期
暴露的放大因素，但不是首要根因。

## 2. 调查范围与运行环境

### 2.1 进程

```text
PID: 28214
角色: server
程序: build/bin/Release/raypx2
监听: 0.0.0.0:8443
Admin: 0.0.0.0:2345
启动时间: 2026-07-13
源码 HEAD: 9b97342160454b905ae782ee7cd4a10d5996dc62
binary SHA-256: ee57ed8d519ba34fc3f236f4be599ff32f631cd68aff705f56effecae9842d16
binary Build ID: 73a144cf5557db9ac4848a4d58fe209b8f2cdcf8
```

`/proc/28214/exe` 与工作区 `build/bin/Release/raypx2` 的 SHA-256 相同，排除了
“调查源码与线上 binary 不一致”的干扰。

### 2.2 调查手段

本次使用以下只读手段：

- Admin API：connection、tunnel、relay、worker、metrics 和 terminal retention 快照；
- 服务端 trace 日志；
- `/proc`、`ss`、线程栈和 fd 快照；
- 短暂只读 GDB attach，读取 `MsQuicStream` wrapper 和底层 `QUIC_STREAM::Flags`。

调查期间没有调用 tunnel abort/drain，没有关闭 connection，没有修改运行状态，
也没有记录 Bearer token、证书私钥或业务 payload。最终确认进程仍正常运行且
`TracerPid=0`。

## 3. Admin API 现场

### 3.1 Connection 和 tunnel

`GET /api/v1/server/connections`：

```json
{
  "connection_id": "srv-1",
  "client_name": "linux-spark-1619",
  "remote_address": "203.90.233.21:56769",
  "state": "connected",
  "active_streams": 1,
  "active_tunnels": 1,
  "total_streams": 1140
}
```

`GET /api/v1/server/tunnels`：

```json
{
  "tunnel_id": "tun-935",
  "connection_id": "srv-1",
  "peer_id": "linux-spark-1619",
  "target": "translate.googleapis.com:443",
  "state": "active",
  "active": true
}
```

Admin tunnel ID、trace tunnel ID 与 terminal identity 属于不同 ID 空间，因此
`tun-935`、terminal `stream_id=1895`、terminal `tunnel_id=1896` 并不矛盾。

### 3.2 Relay 已经停止数据面活动

`GET /api/v1/relay/active-relays/454` 的关键字段：

```json
{
  "relay_id": "454",
  "worker_index": 1,
  "closing": true,
  "stop_published": true,
  "stream_detached": true,
  "tcp_fd": -1,
  "tcp_read_closed": true,
  "tcp_write_closed": true,
  "quic_send_fin_submitted": true,
  "quic_send_fin_completed": true,
  "outstanding_quic_send_bytes": 0,
  "pending_quic_receive_bytes": 0,
  "pending_quic_receive_queue_depth": 0,
  "callback_pending_quic_receive_depth": 0,
  "pending_tcp_write_bytes": 0,
  "event_queue_depth": 0,
  "active_handlers": 0
}
```

进程 fd 列表也没有该 target TCP socket。由此可以排除：

- 目标 TCP 仍未关闭；
- QUIC send 或 TCP write 仍在等待；
- relay event queue 堵塞；
- worker handler 长时间占用该 relay。

### 3.3 Terminal retention

首次采集和最终复核均只有这一条 retention。最终复核时：

```json
{
  "stream_id": 1895,
  "tunnel_id": 1896,
  "connection_id": 1,
  "connection_generation": 1,
  "role": "server",
  "backend": "linux",
  "terminal_phase": "shutdown_submitted",
  "retained_age_ms": 16073543,
  "shutdown_intent": "graceful_complete",
  "shutdown_status": "pending",
  "shutdown_attempt": 1,
  "terminal_observed_at_ms": 0,
  "last_stream_event": "none",
  "watchdog_state": "idle",
  "connection_escalated": false,
  "in_tunnel_registry": true,
  "relay_active": true,
  "tcp_valid": true
}
```

`retained_age_ms=16073543` 约为 4 小时 28 分钟。这里的 `relay_active/tcp_valid`
是 handoff facts，而 relay runtime 已显示 `tcp_fd=-1`；这再次说明 retention facts
没有实时反映数据面已停止，不应据此判断 TCP 仍存活。

## 4. 关闭时间线

服务端 trace 给出的关键顺序如下。

### 4.1 建立

```text
02:21:44.376
event=stream_started role=server conn=1 tunnel=935
target=translate.googleapis.com:443

02:21:44.536
event=linux_relay_register worker=1 relay=454 fd=68
local=172.26.10.140:54026 peer=74.125.130.95:443
stream=0x7fba1806bb90
```

### 4.2 双向关闭

```text
02:25:55.086
event=linux_relay_stream_event worker=1 relay=454
stream_event=receive_fin
absolute_offset=3684
total_buffer_length=0
buffer_count=0
receive_flags=0x2
fin=1

02:25:55.106
event=linux_relay_tcp_read worker=1 relay=454 fd=68 result=eof
tcp_write_closed=1

02:25:55.197
event=linux_relay_stop_condition worker=1 relay=454
trigger=quic_send_fin_completed
outstanding_quic_sends=0
pending_tcp_write_bytes=0
pending_quic_receive_bytes=0
```

此后 relay 454 没有出现以下任何事件：

```text
stream_event=shutdown_complete
event=linux_relay_stream_shutdown
event=linux_relay_unregister
event=stream_closed tunnel=935
```

同一 worker 上邻近的 relay 455 走过相同的 FIN/TCP EOF 路径，并立即收到
`shutdown_complete` 后正常注销。这排除了 worker 全局停止、connection 全局失活或
reaper 线程死锁。

## 5. GDB live MsQuic 状态

日志中的 wrapper 地址为：

```text
MsQuicStream*: 0x7fba1806bb90
HQUIC/QUIC_STREAM*: 0x3726710cc10
MsQuic stream ID: 3776
QUIC_STREAM::Flags: 0x824c060a07
```

按当前 MsQuic [`QUIC_STREAM_FLAGS`](../third_party/msquic/src/core/stream.h)
定义解码：

```text
Allocated=1
Initialized=1
Started=1

LocalCloseFin=1
LocalCloseAcked=1
FinAcked=1
HandleSendShutdown=1

ReceiveEnabled=1
ReceiveMultiple=1
ReceiveDataPending=1

RemoteCloseFin=0
RemoteCloseAcked=0
HandleShutdown=0
ShutdownComplete=0
InStreamTable=1
```

wrapper 的 `Handle`、callback handler 和 callback context 均仍有效，因此不是 wrapper
已释放、callback 被清空或 context 提前销毁。

这些 flags 给出唯一未满足的 terminal 条件：应用接管的 receive 仍处于 pending，
MsQuic 尚未把远端 FIN 发布为 `RemoteCloseFin/RemoteCloseAcked`。

## 6. 当前应用侧路径

Linux callback 在收到真实 FIN 后：

1. 通过 `BuildPendingQuicReceive()` 构造 view；
2. 对 `TotalLength == 0 && fin` 设置
   `ZeroLengthFinCompletionPending=true`；
3. 将 `QuicReceiveView` 入 worker queue；
4. callback 返回 `QUIC_STATUS_PENDING`。

相关实现：

- [`QueueDeferredQuicReceiveFromOffset()`](../src/tunnel/linux_relay_worker.cpp)
- [`BuildPendingQuicReceive()`](../src/tunnel/linux_relay_worker.cpp)
- [`ProcessStreamEvent()` RECEIVE 分支](../src/tunnel/linux_relay_worker.cpp)

worker 在无 payload slice 时执行：

```cpp
(void)CompleteZeroLengthFinReceive(*view);
if (view->Fin) {
    relay->TcpWriteShutdownQueued = true;
}
relay->PendingQuicReceives.pop_front();
```

`CompleteZeroLengthFinReceive()` 通过 stream owner API lease 调用：

```cpp
stream->ReceiveComplete(0);
```

提交 `320530d` 已在 `CompleteAndDiscardQuicReceive()` 入口增加同样的 completion，
当前 HEAD 包含该提交，线上 binary 也来自该 HEAD 之后。因此本次不是旧版本 helper
缺失。

## 7. MsQuic callback-active 并发协议

MsQuic 在投递 RECEIVE callback 前，把 `RecvCompletionLength` 的最高位置为
`RECEIVE_CALL_ACTIVE_FLAG`：

```c
InterlockedOr64(
    &Stream->RecvCompletionLength,
    QUIC_STREAM_RECV_COMPLETION_LENGTH_RECEIVE_CALL_ACTIVE_FLAG);
```

应用调用 `StreamReceiveComplete(BufferLength)` 时，MsQuic 执行：

```c
RecvCompletionLength = InterlockedExchangeAdd64(
    &Stream->RecvCompletionLength,
    BufferLength);

if (RecvCompletionLength & RECEIVE_CALL_ACTIVE_FLAG) {
    goto Exit;
}
```

如果 completion 与 callback 并发且 callback 尚未返回，MsQuic 不另行排队 operation；
它依赖累计字节数让 callback 返回后的路径接管 completion。

callback 返回后，MsQuic清除 active bit并读取累计值：

```c
RecvCompletionLength = InterlockedExchange64(
    &Stream->RecvCompletionLength, 0) & ~RECEIVE_CALL_ACTIVE_FLAG;

if (Status == QUIC_STATUS_PENDING) {
    FlushRecv = (RecvCompletionLength != 0);
}
```

该协议对正长度 completion 有效，但无法表达零字节 completion：

```text
callback active: RecvCompletionLength = ACTIVE_FLAG
ReceiveComplete(0): ACTIVE_FLAG + 0 = ACTIVE_FLAG
callback 返回 PENDING并清 active bit: 得到 0
FlushRecv = false
```

因此 `ReceiveComplete(0)` API 调用确实发生过，也会被现有 fake API 测试计数，但
MsQuic 状态机没有观察到这项 completion。

相关 MsQuic 实现：

- [`MsQuicStreamReceiveComplete()`](../third_party/msquic/src/core/api.c)
- [`QuicStreamRecvFlush()`](../third_party/msquic/src/core/stream_recv.c)
- [`QuicStreamReceiveComplete()`](../third_party/msquic/src/core/stream_recv.c)

## 8. 竞态重建

本次现场与以下顺序完全一致：

```text
MsQuic connection worker
  设置 RecvCompletionLength.ACTIVE_FLAG
  调用应用 RECEIVE callback（0 bytes + FIN）

raypx2 callback
  构造 ZeroLengthFinCompletionPending view
  将 view 发布到 Linux relay worker queue

Linux relay worker
  在 callback 返回前取得 view
  调用 ReceiveComplete(0)
  MsQuic 看到 callback active，不排队 operation
  由于加数为 0，未留下并发 completion 计数
  view 被视为完成并移出队列

raypx2 callback
  返回 QUIC_STATUS_PENDING

MsQuic connection worker
  清 callback active bit，读取到 completion length 0
  判定没有并发 completion
  保持 ReceiveDataPending=1

raypx2 Linux relay
  TCP EOF、local FIN ACK 全部完成
  提交 graceful terminal shutdown并detach数据面

MsQuic
  RemoteCloseFin/RemoteCloseAcked无法发布
  不产生 SHUTDOWN_COMPLETE

terminal convergence
  graceful watchdog保持idle
  retention和tunnel registry永久保留
```

应用 queue publish 与 callback return 之间没有保证“worker 必须在 callback 返回后才调用
ReceiveComplete”的 happens-before 关系，因此该竞态在当前实现中成立。

## 9. API lease 失败是同类缺口

当前 `CompleteZeroLengthFinReceive()` 在无法获取 API lease 时返回 `false`，但多个调用方
忽略返回值并直接弹出或销毁 view：

```cpp
(void)CompleteZeroLengthFinReceive(*view);
relay->PendingQuicReceives.pop_front();
```

本次 live owner 尚处于可获取 API lease 的 phase，callback-active 竞态是与现场 flags
最匹配的解释；但从 ownership 契约看，lease failure 同样可能造成零长度 obligation
提前丢失。修复时必须同时保证：

- active completion 成功前不能把 obligation 标记为 settled；
- lease 暂时失败时保留 view 并重试；
- 只有真实 terminal/receive abort/connection shutdown 后才允许 bookkeeping discard。

## 10. 为什么现有测试没有发现

提交 `320530d` 新增的 Linux 回归测试构造 fake stream 和 fake MsQuic API，然后断言：

```text
ReceiveComplete 调用次数 = 1
完成字节数 = 0
ZeroLengthFinCompletionPending = false
```

该测试能发现“完全没有调用 API”，但不能发现“API 在 callback-active 窗口内调用且被
MsQuic 的零字节并发协议吞掉”，因为 fake API 没有模拟：

- `RecvCompletionLength` active bit；
- callback 返回 `PENDING` 前后的状态转换；
- completion operation 是否真正进入 MsQuic connection worker；
- `ReceiveDataPending/RemoteCloseAcked/ShutdownComplete` 的最终状态。

因此“API call count == 1”不是零长度 FIN ownership 已在 MsQuic 内部结清的充分条件。

## 11. Graceful watchdog 的作用

[`TqStreamLifetime::BeginTerminalShutdown()`](../src/tunnel/stream_lifetime.cpp) 只为
`AbortBothImmediate` 启动 fatal watchdog，明确排除 `GracefulComplete`：

```cpp
if (result.Submitted && !gracefulComplete) {
    TqTerminalScheduler::Instance().ArmWatchdog(...);
}
```

所以本次 retention 一直为：

```text
shutdown_intent=graceful_complete
shutdown_status=pending
watchdog_state=idle
connection_escalated=false
```

不建议简单把 fatal watchdog 原样套到正常 half-close；但至少应增加 graceful terminal
超龄诊断，并明确长期 pending 后允许的受控恢复策略。

## 12. 建议修复方向

### 12.1 推荐：零长度 FIN callback 返回 SUCCESS

零长度 FIN 没有需要跨 callback 保存的 payload buffer。推荐把 FIN 控制语义与 receive
buffer ownership 分离：

1. callback 把“不携带 receive completion obligation”的 FIN 控制事件交给 worker；
2. callback 对 FIN-only RECEIVE 返回 `QUIC_STATUS_SUCCESS`；
3. MsQuic 在 callback 返回路径内同步消费 FIN，不依赖外部 `ReceiveComplete(0)`；
4. worker 继续负责 TCP `SHUT_WR`、fully-closed predicate 和 terminal handoff。

优点：

- 从根本上消除 callback-active `+0` 不可见问题；
- 不需要猜测 callback 实际返回时刻；
- 不持有任何 payload pointer；
- 不需要修改 MsQuic 内部协议。

必须验证 FIN 控制事件入队失败、binding closing、precommit 和 worker stop 时仍能推进正确的
TCP/terminal 语义。

### 12.2 备选：修改 MsQuic 表示零字节并发 completion

可以在 MsQuic 增加独立的 zero-completion bit/token，使 callback-active 窗口内的
`ReceiveComplete(0)` 可被 callback 返回路径观察。

该方案保持应用 `PENDING + ReceiveComplete(0)` 契约，但会修改第三方状态机和 ABI 内部
行为，维护成本及上游同步风险更高，不作为首选。

### 12.3 不推荐：延时或 sleep 后调用

固定延时不能建立 callback-return happens-before，负载、调度和平台变化后仍会复现，
不得作为修复。

## 13. 必需回归测试

### 13.1 MsQuic callback-active 确定性测试

使用 barrier 控制顺序：

1. callback 发布零长度 FIN view后阻塞；
2. worker 尝试 settlement；
3. callback 返回 `PENDING`；
4. 断言真实 MsQuic 最终满足：

```text
ReceiveDataPending=0
RemoteCloseFin=1
RemoteCloseAcked=1
ShutdownComplete=1
```

测试不能只断言 fake API call count。

### 13.2 FIN-only SUCCESS 路径

- callback 返回 `SUCCESS`；
- worker 收到 FIN 控制事件；
- TCP 只执行写方向 shutdown；
- 双向结束后收到真实 `SHUTDOWN_COMPLETE`；
- terminal retention、closing relay 和 tunnel registry 回到 0。

### 13.3 Lease failure

- 强制第一次 API lease 获取失败；
- obligation 继续保持 pending，不弹出 view；
- 后续重试恰好结清一次；
- terminal 之后不再调用 stream API。

### 13.4 Queue/terminal 竞态

至少覆盖：

- normal queue；
- callback fallback queue；
- precommit queue；
- relay closing；
- worker stop；
- stale generation；
- RECEIVE view 与真实 `SHUTDOWN_COMPLETE` 并发。

### 13.5 系统测试

真实 client/server、真实 MsQuic 和短连接批量关闭场景应断言：

```text
terminal_retained_owner_count=0
terminal_sink_pending=0
linux_relay_closing_relays=0
active_streams=0
active_tunnels=0
```

并在 delay/loss 与高并发下执行足够长的 soak。

### 13.6 2026-07-14 修复落地

本次已按 Linux-only 方案修改：

- `src/tunnel/linux_relay_worker.cpp` / `.h`：FIN-only RECEIVE 仍向 worker 发布有序控制
  view，但显式设置 `ZeroLengthFinCompletionPending=false`，发布成功后 callback 返回
  `QUIC_STATUS_SUCCESS`；payload-only 和 data + FIN 继续返回 `QUIC_STATUS_PENDING`。
- view 分配或发布失败时 callback 返回 `QUIC_STATUS_OUT_OF_MEMORY`，不再出现“返回
  PENDING 但没有可完成 view”的错误路径。
- `src/unittest/linux_relay_worker_io_test.cpp`：覆盖 FIN-only 返回 SUCCESS、TCP
  `SHUT_WR`、零次 `StreamReceiveComplete`，并保留普通与压缩 data + FIN 的 PENDING/
  正字节 completion 断言。
- `src/unittest/linux_terminal_convergence_test.cpp`：更新 graceful half-close 的 FIN-only
  契约，同时继续验证反向 payload、TCP EOF 和无异常 stream shutdown。

TDD RED 阶段，旧实现按预期失败并输出：

```text
expected FIN-only receive callback to return SUCCESS
```

GREEN 与回归阶段已运行 Linux relay worker I/O、Linux terminal convergence 测试和生产
`tcpquic-proxy` target 构建。此次代码修复不会改变已经运行的 PID 28214；该实例仍需按
下一节方式单独恢复或替换 binary 后重启。

## 14. 当前实例恢复方式

该 stream 已经停在 MsQuic 内部 `ReceiveDataPending=1`，应用侧 view 已经不存在，无法再安全
补发与原 obligation 对应的 `ReceiveComplete(0)`。

因此：

- 单 tunnel abort 很可能因 owner 已处于 `shutdown_submitted` 且 stream 已 detached 而不能
  恢复该 receive 状态；
- 可通过关闭/重连 `srv-1` 对应 QUIC connection 清理该 stream；
- 或替换修复后的 binary 并重启 server；
- connection 级恢复会影响该 connection 上其他 streams，执行前必须确认业务影响。

本报告只记录定位与建议，没有对 PID 28214 执行上述恢复操作。

## 15. 根因置信度

根因置信度为高，依据如下：

1. 日志明确记录 `TotalBufferLength=0 + FIN`。
2. relay、TCP、send、receive queue 和 worker handler 均已排空。
3. GDB live flags 精确显示唯一未收敛条件为 `ReceiveDataPending=1`。
4. wrapper、HQUIC、callback handler 和 context 均有效，排除提前释放。
5. 当前 binary 已包含旧 cleanup 修复，排除“旧 helper 完全不调用”的解释。
6. 当前 queue/callback 之间不存在 completion 必须晚于 callback return 的顺序保证。
7. MsQuic 源码明确表明 callback-active 并发 completion 通过字节累加传递，而零字节
   completion 不改变累加值。
8. 同 worker 的邻近 relay 正常 terminal，排除全局线程或 connection 停摆。

现场不能直接回放已经发生的纳秒级调度顺序，因此第 6～7 项属于由 live state 与源码
共同支持的逆向推断；它无需假设网络丢包、MsQuic 随机丢 callback 或 reaper 死锁，能够
完整解释全部观测事实。

## 16. 关联资料

- [Linux 零长度 FIN cleanup 遗漏根因与旧修复](linux_zero_length_fin_terminal_retention_root_cause_cn.md)
- [跨平台零长度 FIN Receive Ownership 修复设计](superpowers/specs/2026-07-12-cross-platform-zero-length-fin-receive-ownership-design.md)
- [Linux relay 实现](../src/tunnel/linux_relay_worker.cpp)
- [Stream lifetime/terminal scheduler](../src/tunnel/stream_lifetime.cpp)
- [MsQuic stream receive 状态机](../third_party/msquic/src/core/stream_recv.c)
- [MsQuic StreamReceiveComplete API](../third_party/msquic/src/core/api.c)
