# Windows IOCP Relay 问题调查（GPT-5.5）

> 日期：2026-06-25  
> 平台：Windows 10 x64  
> 日志：`build-x64\bin\Release\log\client.log`  
> 场景：浏览器通过本项目 proxy 访问 `https://cursor.com/dashboard`  
> 本地参数来自终端：`tcpquic-proxy.exe client --socks-listen 0.0.0.0:11080 --peer 52.74.45.234:8443 --compress off --trace --trace-interval 30 --ca cert\ca.crt`

## 1. 结论

本次 `client.log` 里没有看到 QUIC 连接失败、OPEN 失败或旧版 Windows relay 的 fatal 风暴。相反，日志显示：

- QUIC client 已连接到 `52.74.45.234:8443`。
- `cursor.com:443` 的多条 tunnel 均 `open_result ok=1`。
- 全程 `open_fail=0`，统计中 `fatal_relay_resets=0`、`tcp_hard_errors=0`、`quic_send_failures=0`。
- 问题主要表现为 **部分已成功打开的 Windows relay/tunnel 长时间不退休**：页面加载后 `active_relays` 长时间维持在 19-20，多个 `stats_active_tunnel` 持续输出，浏览器侧高概率表现为 dashboard 资源请求卡住或等待超时。

因此，本次问题的主线不是“连接 cursor.com 失败”，而是 **Windows IOCP relay 的关闭/半关闭/空闲状态没有及时推进到 `relay_unregister -> relay_stopping -> stream_closed`**。

## 2. 复现环境

运行参数：

```powershell
.\build-x64\bin\Release\tcpquic-proxy.exe client `
  --socks-listen 0.0.0.0:11080 `
  --peer 52.74.45.234:8443 `
  --compress off `
  --trace `
  --trace-interval 30 `
  --ca cert\ca.crt
```

启动日志显示当前 relay backend 为 `windows-iocp (8 workers)`，压缩关闭，因此本次问题与 zstd 压缩路径无直接关系。

## 3. 日志事实

### 3.1 QUIC 与 OPEN 阶段正常

日志开头显示：

```text
event=quic_connected role=client conn=1 slot=1 local=192.168.1.113:61399 peer=52.74.45.234:8443
```

`cursor.com:443` tunnel 的 OPEN 成功：

```text
14:14:55.768 event=stream_started tunnel=4 target=cursor.com:443
14:14:55.904 event=open_result tunnel=4 ok=1 error=ok
14:14:55.905 event=relay_started tunnel=4 backend=windows worker=3 relay_id=1
```

后续 `tunnel=5`、`15`、`16`、`17`、`18`、`20`、`21` 等 `cursor.com:443` 请求也都出现 `open_result ok=1`。

### 3.2 部分 cursor tunnel 能正常关闭

例如 `tunnel=4`：

```text
trigger=tcp_read_closed
trigger=quic_send_fin_completed
trigger=close_after_drained
event=relay_unregister worker=3 relay=1
event=relay_stopping tunnel=4 target=cursor.com:443 reason=reaper
event=stream_closed tunnel=4 target=cursor.com:443 reason=ok
```

这说明当前 Windows relay 的一部分 FIN/drain 修复已经生效；不是所有 cursor 连接都会卡死。

### 3.3 异常特征：active relay 长时间不降

`stats_relay` 的变化：

```text
14:15:14 active_relays=11 pending_bytes=11534336
14:15:45 active_relays=10 pending_bytes=10485760
14:16:15 active_relays=20 pending_bytes=20971520
14:16:45 active_relays=20 pending_bytes=19922944
14:17:16 active_relays=19 pending_bytes=18874368
14:17:46 active_relays=19 pending_bytes=18874368
14:18:16 active_relays=19 pending_bytes=17825792
```

同时 QUIC 统计显示传输逐渐进入低流量/应用受限状态：

```text
14:16:15 stream_rx=15013794 bbr_bw_mbps=1.09 app_limited=0
14:17:16 stream_rx=15020425 bbr_bw_mbps=0.01 app_limited=1
14:18:16 stream_rx=15021058 bbr_bw_mbps=0.03 app_limited=1
```

也就是说，在 14:16 之后，QUIC 总体不再大量传输，但 relay/tunnel 数量没有同步释放。

### 3.4 两个 cursor tunnel 长时间滞留

`tunnel=5`：

```text
14:14:56.629 stream_started target=cursor.com:443
14:14:56.787 open_result ok=1
14:14:56.788 relay_started worker=4 relay_id=1
14:15:14.966 stats_active_tunnel tunnel=5 age=18.3s
14:16:15.530 stats_active_tunnel tunnel=5 age=78.9s
14:17:16.006 stats_active_tunnel tunnel=5 age=139.4s
14:18:16.551 stats_active_tunnel tunnel=5 age=199.9s
```

`tunnel=21`：

```text
14:16:02.009 stream_started target=cursor.com:443
14:16:02.185 open_result ok=1
14:16:02.185 relay_started worker=4 relay_id=3
14:16:15.530 stats_active_tunnel tunnel=21 age=13.5s
14:17:16.007 stats_active_tunnel tunnel=21 age=74.0s
14:18:16.552 stats_active_tunnel tunnel=21 age=134.5s
```

这两条连接没有在日志中看到对应的 `relay_stop_condition`、`relay_unregister`、`relay_stopping`、`stream_closed`。它们是本次 dashboard 高概率异常的核心证据。

### 3.5 仍有 TCP teardown，但已被降级

日志中仍可看到 Windows 本地 TCP teardown，例如：

```text
trigger=wsa_recv_post_failed tcp_write_errno=10053
trigger=wsa_send_receive_view_failed tcp_write_errno=10053
trigger=iocp_tcp_send_teardown tcp_write_errno=64
```

这些连接大多最终走 `relay_unregister` 和 `stream_closed`，统计中 `tcp_hard_errors=0`，说明当前代码已经把这类本地 teardown 视为 graceful drain，而不是 fatal reset。这是较旧日志相比的明显改善。

## 4. 代码对应关系

### 4.1 relay 生命周期由 worker 退休后发布 Stop

`TqTunnelReaper` 每 100ms 轮询 `TqTunnelRelayStopped(ctx)`，只有 `RelayHandle.Stop == true` 才会调用 `TqReapTunnelContext` 删除 tunnel context。

Windows 路径中，`TqRelayStop` 第一次调用只会把 close 请求投递给 `TqWindowsRelayRuntime::StopRelay(handle)`；真正的 `Stop=true` 由 `TqWindowsRelayWorker::TryRetireRelay` 发布。

`TryRetireRelay` 的退休条件包括：

```text
Closing == true
ActiveHandlers == 0
QueuedWorkerOps == 0
InFlightTcpRecvs == 0
InFlightTcpSends == 0
InFlightQuicSends == 0
```

因此，只要某条 relay 没进入 `Closing`，或某个 in-flight/queued 计数没有归零，reaper 就不会释放 tunnel。

### 4.2 正常关闭路径依赖明确事件推进

当前正常路径大致是：

1. TCP 读到 EOF，`HandleTcpReadClosed` 置 `TcpRecvClosed=true`。
2. 发送 QUIC FIN，`PostQuicSend(..., QUIC_SEND_FLAG_FIN)`。
3. MsQuic 回调 `SEND_COMPLETE`，置 `QuicSendFinCompleted=true`。
4. 若满足 drain 条件，置 `CloseAfterDrained=true`。
5. `CloseRelayIfDrained` 置 `TcpWriteClosed=true`，必要时 `TqShutdownSend`，最后调用 `CloseRelay(..., "close_after_drained")`。
6. `CloseRelay` 清理 pending receive/send accounting，调用 `TryRetireRelay`。
7. `TryRetireRelay` 发布 `PublicHandle->Stop=true`。
8. reaper 输出 `relay_stopping` 和 `stream_closed`。

能正常关闭的 `cursor.com` tunnel 符合这条链路。

### 4.3 长时间滞留 tunnel 的缺口

`tunnel=5` 和 `tunnel=21` 的日志只到 `relay_started`，随后只出现在 `stats_active_tunnel` 中。没有任何 stop condition，说明代码没有收到或没有记录能推进关闭的事件：

- 没有 TCP EOF：未见 `tcp_read_closed`。
- 没有 QUIC FIN 完成：未见 `quic_send_fin_completed`。
- 没有 peer shutdown：未见 `stream_shutdown`。
- 没有 post failure：未见 `wsa_recv_post_failed` 或 `wsa_send_receive_view_failed`。
- 没有最终退休：未见 `relay_unregister`。

这有两种可能：

1. **正常但不友好的 keep-alive/HTTP2 长连接**：浏览器保留连接，relay 长时间 active，短期内不应关闭。
2. **异常挂起**：某些 dashboard 资源请求已经没有有效数据流，但 Windows relay 缺少 idle/first-byte/双向静默超时，导致 tunnel 无限等待。

结合用户观察“高概率出现问题”，第二种可能更需要优先验证。

## 5. 根因判断

### 高置信结论

本次日志不支持以下根因：

- 不是 QUIC peer 连接失败。
- 不是 cursor.com OPEN 失败。
- 不是压缩路径问题，因为 `--compress off`。
- 不是旧版 fatal reset 风暴，因为 `fatal_relay_resets=0`、`tcp_hard_errors=0`。

本次日志支持的根因方向是：

> Windows IOCP relay 对“已打开但长时间无关闭信号”的 tunnel 缺少足够的收敛机制或可观测性，导致 dashboard 场景下多个 cursor.com 连接长期占用 relay；浏览器等待关键资源或长连接状态时，高概率表现为页面加载异常。

### 中置信假设

`tunnel=5` 和 `tunnel=21` 可能卡在首次或后续 `WSARecv`/QUIC RECEIVE 等待状态。因为 `stats_active_tunnel` 目前只输出 tunnel 层状态，不输出该 relay 的 `tcp_read_bytes`、`tcp_write_bytes`、`InFlightTcpRecvs`、`InFlightTcpSends`、`QueuedQuicReceives`，所以不能仅凭当前日志区分“正常 idle keep-alive”和“零 IO 僵尸”。

### 已修正的诊断前置风险

实现 P0 诊断时同步修正了一个会影响观测可信度的 Windows relay 生命周期风险：`CloseRelay` 不再 detach stream callback，也不再清零已提交给 MsQuic 的 `InFlightQuicSends`；callback detach、`relay_unregister` 和 `PublicHandle->Stop=true` 只在 `TryRetireRelay` 确认全部 in-flight/queued/handler 计数归零后发生。

这保证后续 `stats_active_relay` 里看到的 `inflight_quic_sends`、`stop_published`、`stream_detached` 更接近真实生命周期状态，不会因为关闭路径提前吞掉 `SEND_COMPLETE` 而误判。

## 6. 建议后续验证

### P0：增强 active tunnel/relay 诊断

已实现：`stats_active_tunnel` 会输出 `backend`、`worker`、`relay_id`；Windows 下新增 `stats_active_relay` 输出以下字段：

- `worker`
- `relay_id`
- `tcp_read_bytes`
- `tcp_write_bytes`
- `tcp_read_closed`
- `tcp_write_closed`
- `close_after_drained`
- `inflight_tcp_recvs`
- `inflight_tcp_sends`
- `queued_quic_receives`
- `pending_quic_receive_bytes`
- `inflight_quic_sends`
- `quic_send_fin_submitted`
- `quic_send_fin_completed`
- `last_tcp_errno`

有了这些字段，就能判断 `tunnel=5` / `tunnel=21` 是正常 HTTP keep-alive，还是某个 IOCP 计数或半关闭状态卡住。

### P0：增加 relay idle/first-byte 超时策略

已实现诊断型超时日志，暂不默认强制关闭 relay：

- relay started 后 30 秒内双向 `tcp_read_bytes == 0 && tcp_write_bytes == 0`，输出 `event=relay_first_byte_timeout`。
- relay 已有字节后 60 秒无任何字节增长，输出 `event=relay_idle_timeout`。
- 可配置强制关闭超过阈值的完全 idle tunnel 仍建议作为下一步实验参数，不在本轮默认启用。

### P1：复核 QUIC peer shutdown / TCP EOF 传播

对卡住 tunnel，重点确认：

- 浏览器侧 TCP 是否仍保持 ESTABLISHED。
- server 侧是否已向 client 发送 QUIC FIN 或 shutdown。
- client `StreamCallback` 是否收到了 `QUIC_STREAM_EVENT_PEER_SEND_SHUTDOWN` / `SHUTDOWN_COMPLETE`。
- `PEER_SEND_SHUTDOWN` 当前只返回 success，不主动推进 `CloseAfterDrained`；如果对端只发 shutdown 而没有 receive FIN view，可能缺少关闭推进。

### P1：修复或补充测试

已有计划中的 Windows relay 单测仍应覆盖：

- TCP half-close 后继续接收 QUIC 数据并写回浏览器。
- `wsa_recv_post_failed` / `wsa_send_receive_view_failed` 伴随 pending receive 时能正确完成 receive ownership。
- `CloseRelay` 后 late callback/stale IOCP completion 不新增 active relay 泄漏。

## 7. 与现有计划的关系

`docs/superpowers/specs/2026-06-25-windows-iocp-refactor-design.md` 中的 Phase 0-2 已经覆盖了旧问题：fatal 降级、Stop 退休语义、QUIC send accounting、late callback 防护。本次日志显示这些方向有效，但 dashboard 仍暴露了另一个问题：

> 已成功建立的长连接在 Windows IOCP relay 中缺少足够的“长期静默”观测与收敛策略。

因此，不建议回退到“所有 teardown 都 fatal reset”的旧行为；应先补齐 per-relay stuck 诊断，再根据 `tunnel=5` / `tunnel=21` 的真实状态决定是否引入 idle timeout、peer shutdown 推进，或修复某个 in-flight 计数不归零的问题。
