# Linux relay tunnel 异常关闭回收设计

## 背景

DGX 200Gbps 直连、`netem delay 90ms loss 10% limit 5000000`、download/reverse iperf3 顺序复用同一个 `tcpquic-proxy` 连接时，复现到 iperf3 在 30 秒数据阶段结束后卡在 `TEST_END -> DISPLAY_RESULTS`。现场显示远端 `tcpquic-proxy` 保留约 `444-448 MB` pending buffer，`out_flow_blocked=0x10`、`bytes_in_flight=0`，远端 iperf server 到 proxy 的本机 TCP 处于 `CLOSING/FIN-WAIT-2` 并仍有 `notsent`/`Recv-Q`。

这说明旧 tunnel 在关闭/排空路径上没有及时释放，继续占用同一个 QUIC connection 上的 stream/flow-control/buffer 状态，进而影响后续 tunnel。

## 目标

同一个 QUIC connection 上的每个 tunnel 必须独立收敛。任一 tunnel 的 TCP/QUIC 一侧已经明确不可继续完成语义时，proxy 必须及时 reset 当前 tunnel、释放 pending buffer 和 stream binding，不能等待无限期优雅 drain，也不能影响同一 connection 上的新 stream。

## 非目标

- 不改变 iperf3 的统计逻辑。
- 不关闭整个 QUIC connection 来处理单个 tunnel 的异常。
- 不把“pending bytes 大于某阈值”单独作为 reset 条件。
- 不依赖模糊的“长期无进展”判定作为第一版修复核心。

## 术语

用户提出的“linger 探测”在 Linux TCP API 上应拆成两类：

- `SO_LINGER`: 控制 `close()` 行为，是否等待未发送数据，或用 RST 关闭。
- `SO_KEEPALIVE` + `TCP_KEEPIDLE/TCP_KEEPINTVL/TCP_KEEPCNT`: TCP keepalive 探测，用于发现静默失效的对端。
- `TCP_USER_TIMEOUT`: 当已有数据未被 ACK 超过指定时间时，让 TCP 连接失败，更贴近“写侧无法推进”。

本设计采用 TCP keepalive/user-timeout 作为辅助异常信号，但 tunnel 回收以 relay 状态机为主。

## 状态模型

每个 Linux relay tunnel 维护以下语义状态：

```text
Open
ClosingGraceful
ClosingAbort
Closed
```

现有字段可继续使用：

```text
TcpReadClosed
TcpWriteClosed
TcpWriteShutdownQueued
QuicSendFinSubmitted
QuicSendFinCompleted
OutstandingQuicSends
OutstandingQuicSendBytes
PendingTcpWrites
PendingQuicReceives
PendingQuicReceiveBytes
StreamBinding
```

新增或显式化字段：

```text
QuicReceiveFinSeen
QuicStreamShutdownComplete
TcpPeerWriteFailed
TcpSocketError
```

`ClosingGraceful` 只允许有限 drain，不再读入新的同向 TCP 数据。`ClosingAbort` 立即释放资源。

## 正常优雅关闭

正常路径保持现有语义：

```text
TCP read EOF
  -> TcpReadClosed = true
  -> Submit QUIC FIN
  -> QuicSendFinCompleted = true

QUIC receive FIN
  -> QuicReceiveFinSeen = true
  -> drain receive data to TCP
  -> shutdown(TcpFd, SHUT_WR)
  -> TcpWriteClosed = true

双向都关闭且 pending/outstanding 全空
  -> SetRelayStop
  -> reaper 释放 tunnel
```

## 异常 reset 条件

以下条件必须 reset 当前 tunnel，不等待优雅 drain：

1. TCP 明确硬错误

```text
read/write 返回 ECONNRESET、EPIPE、ENOTCONN、EBADF、EIO、ETIMEDOUT 等硬错误
epoll EPOLLERR 且 getsockopt(SO_ERROR) 非 0
```

2. QUIC 明确 abort/error

```text
QUIC_STREAM_EVENT_PEER_SEND_ABORTED
QUIC_STREAM_EVENT_PEER_RECEIVE_ABORTED
QUIC send API fatal error
```

3. QUIC stream 已 shutdown complete，但 relay 仍有 pending/outstanding

```text
QUIC_STREAM_EVENT_SHUTDOWN_COMPLETE
且 PendingTcpWrites 非空
或 PendingQuicReceives 非空
或 PendingQuicReceiveBytes != 0
或 OutstandingQuicSends != 0
或 QuicSendFinSubmitted != QuicSendFinCompleted
```

stream 已经 shutdown complete 后，当前 tunnel 不再可能继续正常完成 QUIC 侧传输。此时继续持有 pending 数据只会污染同一个 connection 上的后续 stream。

4. TCP 写侧被内核判定失效，且仍有待写数据

```text
TCP_USER_TIMEOUT 触发后的 ETIMEDOUT
keepalive failure 后的 ETIMEDOUT/ECONNRESET
EPOLLHUP/EPOLLERR 后 SO_ERROR 非 0
并且 PendingTcpWrites/PendingQuicReceives 非空
```

5. TCP peer 已 half-close，但 TCP->QUIC 方向因 QUIC backlog 被暂停

```text
events 包含 EPOLLRDHUP 或 EPOLLHUP
且 TcpReadPausedByQuicBacklog == true
且 OutstandingQuicSendBytes > 0 或 PendingQuicSendRetries 非空
```

这是本次 DGX 现场最接近的状态：proxy 为了 QUIC 发送背压暂停读取本地 TCP，而 TCP peer 已经进入关闭路径。此时继续等待会保留旧 tunnel 的 TCP fd、stream 和 pending buffer；应 reset 当前 tunnel，释放它占用的共享 QUIC connection 状态。

6. 状态冲突

```text
stream 已 detach/shutdown 后仍要求提交新的 QUIC send
TcpFd 已关闭后仍要求写 TCP
relay Closing 后仍收到新的 receive view 入队
```

这些属于实现内部状态不一致，按 fatal relay reset 处理。

## 不应立即 reset 的条件

以下条件不能单独触发 reset：

```text
TCP read EOF，但 TCP 写侧仍可能向对端发送数据
QUIC receive FIN，但 TCP 写侧仍能继续 drain
write 返回 EAGAIN/EWOULDBLOCK
存在 pending bytes，但没有任何明确关闭/错误信号
high RTT/high loss 导致短时间 send complete 变慢
```

这些是正常 half-close 或背压，应继续优雅 drain。`ECONNRESET`、`ETIMEDOUT` 和 `SO_ERROR != 0` 不属于此类；它们说明 TCP socket 已不可继续承载 tunnel 语义，必须 reset 当前 tunnel。
`EPOLLRDHUP` 在普通 half-close 中不单独触发 reset；只有它与 `TcpReadPausedByQuicBacklog` 和未完成 QUIC send backlog 同时出现时，才说明该 tunnel 已无法按优雅路径继续收敛。

## TCP keepalive/user-timeout 配置

在 Linux relay 注册 TCP fd 后设置：

```text
SO_KEEPALIVE = 1
TCP_KEEPIDLE = config.TcpKeepaliveIdleSec
TCP_KEEPINTVL = config.TcpKeepaliveIntervalSec
TCP_KEEPCNT = config.TcpKeepaliveCount
TCP_USER_TIMEOUT = config.TcpUserTimeoutMs
```

建议默认先使用保守配置，DGX/压测 profile 可使用激进配置：

```text
默认: idle=10s, interval=2s, count=3, user_timeout=10000ms
DGX 调试: idle=1s, interval=1s, count=3, user_timeout=3000ms
```

keepalive/user-timeout 只作为 TCP 错误信号来源，不替代 relay 状态机。

第一版实现只在 `TqLinuxRelayWorkerConfig` 中加入 Linux relay worker 内部默认值，不新增 CLI/config 文件参数。后续如需要区分默认 profile 和 DGX 调试 profile，再单独把这些字段接入外部配置。

## 回收动作

新增一个单 tunnel abort/reclaim helper，语义等价于“当前 tunnel 不可恢复”：

```text
SetRelayStop(relay, trigger)
relay->Closing = true
epoll_ctl DEL TcpFd
TqResetSocket(TcpFd)
abort 当前 QUIC stream
detach stream callback/context
retire StreamBinding
force FlushDeferredReceiveCompletion
clear PendingQuicReceives
clear PendingTcpWrites
clear CallbackPendingQuicReceives
PendingQuicReceiveBytes = 0
```

该 helper 不关闭 QUIC connection，不影响其它 relay。

## 测试策略

单元测试覆盖：

- `SHUTDOWN_COMPLETE` 且 pending 不为空时必须 reset relay。
- TCP write hard error 且 pending 不为空时必须 reset relay。
- TCP `SO_LINGER {on,0}` 触发 RST 后，read 侧 `ECONNRESET` 必须 reset relay，不能当作 graceful EOF。
- `EPOLLRDHUP` 到达时如果 TCP read 已因 QUIC send backlog 暂停，必须 reset 当前 relay。
- TCP read EOF 后，如果反向 pending 能正常 drain，仍允许优雅关闭。
- `PEER_SEND_ABORTED`/`PEER_RECEIVE_ABORTED` 继续立即 reset。
- reset 后 `PendingBytes == 0`、`ActiveRelays` 不继续增长、`Handle.Stop == true`。

DGX 验收复现：

```bash
rtk env ACCEPTANCE_MODE=1 SEQUENTIAL=1 DELAYS='10 20 30 40 50 60 70 80 90 150 300 500' ATTEMPTS=1 scripts/debug-dgx-iperf3-download-issue.sh
```

修复前可在顺序复用 proxy 时复现 `iperf_rc_124` 或低 receiver 吞吐；修复后不应再出现 `TEST_END` 后数百秒等待、旧 tunnel pending buffer 残留影响后续 stream 的现象。
