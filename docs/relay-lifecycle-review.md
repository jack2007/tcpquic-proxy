# Relay 生命周期代码梳理

本文梳理当前 `relay` 关键路径中 TCP 连接、QUIC stream、QUIC connection 之间的关系。范围以生产数据通道为主：客户端本地 SOCKS5/HTTP CONNECT TCP 入口到 QUIC stream，服务端 QUIC stream 到目标 TCP 连接，以及 relay worker 中双向转发、关闭和异常处理。

## 1. 核心对象关系

- 一个业务 TCP 连接映射一条 QUIC 双向 stream。客户端入口 TCP 来自 `TqSocks5Server` / `TqHttpConnectServer`，成功解析 SOCKS5/HTTP CONNECT 请求中的目标地址字段后调用 `onTunnel(tunnel, clientFd)`；这里不是在入口侧做 DNS 解析，域名会作为 `TQ_ADDR_DOMAIN` 传到服务端 open 流程。失败时直接关闭 `clientFd`。见 `src/ingress/socks5_server.cpp:447`、`src/ingress/http_connect_server.cpp:258`。
- 客户端 runtime 在启动 tunnel 前必须先选中一个已连接的 `MsQuicConnection`，然后调用 `TqStartClientTunnel(conn, req, fd, cfg)`。单 peer 和多 peer 路径分别见 `src/main.cpp:473`、`src/main.cpp:83`。
- `TqStartClientTunnelInternal()` 创建 `TqTunnelContext`，再在选中的 QUIC connection 上创建 `MsQuicStream`，发送 open request，并等待服务端 open response。见 `src/tunnel/tcp_tunnel.cpp:1039`、`src/tunnel/tcp_tunnel.cpp:1053`、`src/tunnel/tcp_tunnel.cpp:1079`、`src/tunnel/tcp_tunnel.cpp:1087`。
- open 成功后才调用 `StartRelay()`，最终进入 `TqRelayStart()`，由平台实现接管 TCP fd 与 QUIC stream callback。见 `src/tunnel/tcp_tunnel.cpp:1104`、`src/tunnel/tcp_tunnel.cpp:344`、`src/tunnel/relay.cpp:13`。
- 服务端在 `QUIC_CONNECTION_EVENT_PEER_STREAM_STARTED` 中把新 stream 交给 `TqHandleServerIncomingStream()`。见 `src/protocol/quic_session.cpp:1264`、`src/main.cpp:626`。
- 服务端先用 `TqServerIncomingStreamDispatcher` 读取 stream 前几个字节并判断命令；如果是 `TQ_CMD_OPEN`，转交给服务端 `TqTunnelContext`，解析目标地址、ACL 检查、拨目标 TCP、发送 open response，然后启动 relay。入口见 `src/tunnel/tcp_tunnel.cpp:1166`、包装 raw stream 见 `src/tunnel/tcp_tunnel.cpp:1377`。
- 每个 `TqTunnelContext` 会按 QUIC connection 注册到 `tunnel_registry`。连接中断时，connection callback 调 `TqAbortConnectionTunnels(connection)`，再回调每个 tunnel 的 `AbortForConnectionShutdown()`。注册、注销、批量 abort 见 `src/tunnel/tunnel_registry.cpp:28`、`src/tunnel/tunnel_registry.cpp:69`、`src/tunnel/tunnel_registry.cpp:114`。

## 2. TCP 建立连接 -> 打开 stream

客户端路径：

1. SOCKS5/HTTP CONNECT accept 到本地 TCP，解析协议请求中的目标 host/port 字段并调 `onTunnel()`；这里不做 DNS 解析。见 `src/ingress/socks5_server.cpp:447`、`src/ingress/http_connect_server.cpp:258`。
2. runtime 调 `EnsureAnyConnected()`，确认至少一个 QUIC connection 已连接；随后 `PickConnection()` 用 `PickIndex.fetch_add(1) % connection_count` 做轮询选择，并跳过未连接或无效的 connection slot。该策略不按当前 stream 数、负载或 RTT 加权。见 `src/main.cpp:463`、`src/protocol/quic_session.cpp:527`。
3. `TqStartClientTunnelInternal()` 创建 client-side `TqTunnelContext` 和 `MsQuicStream`，stream callback 初始指向 `TqTunnelContext::Callback`。见 `src/tunnel/tcp_tunnel.cpp:1039`、`src/tunnel/tcp_tunnel.cpp:1053`。
4. client 在 stream 上发送 open request，等待 open response。open response 成功后调用 `FinishClientOpenAndStartRelay()`，进入 `TqRelayStart()`。见 `src/tunnel/tcp_tunnel.cpp:1079`、`src/tunnel/tcp_tunnel.cpp:1087`、`src/tunnel/tcp_tunnel.cpp:1104`。

服务端路径：

1. server connection callback 收到 `PEER_STREAM_STARTED` 后调用配置的 stream handler。见 `src/protocol/quic_session.cpp:1264`。
2. `TqHandleServerIncomingStream()` 将 raw stream 包装为 `MsQuicStream`，先挂到 dispatcher callback。见 `src/tunnel/tcp_tunnel.cpp:1377`、`src/tunnel/tcp_tunnel.cpp:1409`。
3. dispatcher 收到 `RECEIVE`，根据前缀识别 `TQ_CMD_OPEN` 后转交给 `TqTunnelContext`。见 `src/tunnel/tcp_tunnel.cpp:1204`。
4. `TqTunnelContext` 解析 open request，ACL 通过后异步拨目标 TCP；拨号成功后发送 open response，并在 send complete 后启动 relay。相关路径在 `TryHandleServerOpen()`、`FinishServerOpenAfterDial()`、`StartPendingServerRelay()` 中。

## 3. Relay 接管后的绑定关系

公共入口：

- `TqRelayStart()` 按平台选择 Windows IOCP worker 或 Linux epoll worker，并把 `tcpFd`、`MsQuicStream*`、压缩/解压对象、`TqRelayHandle` 传入 worker。见 `src/tunnel/relay.cpp:29`、`src/tunnel/relay.cpp:38`。
- `TqRelayStop()` 通过 `TqRelayHandle` 找到平台 worker 并停止 relay。Linux 调 `UnregisterRelay()`；Windows 投递 `CloseRelay`。见 `src/tunnel/relay.cpp:133`。

Linux：

- `RegisterRelayWithId()` 将 TCP fd 设为 non-blocking，注册到 epoll，创建 `StreamRelayBinding`，并把 stream callback 改为 `TqLinuxRelayWorker::StreamCallback`。见 `src/tunnel/linux_relay_worker.cpp:734`、`src/tunnel/linux_relay_worker.cpp:759`。
- `RelayState` 保存一条 relay 的全部状态：TCP fd、stream、半关闭状态、待写 TCP 队列、pending QUIC receive、未完成 QUIC send 等。

Windows：

- `RegisterRelay()` 把 TCP socket 绑定到 IOCP，创建 `RelayContext` 和 `CallbackBinding`，并把 stream callback 改为 `TqWindowsRelayWorker::StreamCallback`。见 `src/tunnel/windows_relay_worker.cpp:249`、`src/tunnel/windows_relay_worker.cpp:282`。
- Windows 用 `CloseRelayIfDrained()` 表示收到 stream 关闭/abort 后等待已排队的 QUIC->TCP 写完成，再 shutdown TCP send。见 `src/tunnel/windows_relay_worker.cpp:547`。

## 4. TCP -> QUIC：数据、shutdown、异常

### 4.1 TCP 数据 -> QUIC send

Linux 在 epoll 的 `EPOLLIN | EPOLLRDHUP | EPOLLHUP | EPOLLERR` 上进入 `DrainTcpReadable()`。`readv()` 读到数据后，可选压缩，再 `SubmitTcpBatchToQuic()` 调 `stream->Send(..., QUIC_SEND_FLAG_NONE, ...)`。见 `src/tunnel/linux_relay_worker.cpp:888`、`src/tunnel/linux_relay_worker.cpp:928`、`src/tunnel/linux_relay_worker.cpp:953`、`src/tunnel/linux_relay_worker.cpp:1194`。

Windows 在 `HandleTcpRecv()` 中处理 WSARecv 完成，读到数据后可选压缩，再 `stream->Send(..., QUIC_SEND_FLAG_NONE, ...)`。send complete 后继续投递下一次 TCP recv。见 `src/tunnel/windows_relay_worker.cpp:564`、`src/tunnel/windows_relay_worker.cpp:1309`。

### 4.2 TCP 正常 shutdown / EOF -> QUIC FIN

Linux `readv()` 返回 0 时设置 `TcpReadClosed=true`，停止读 TCP，然后 `FinishTcpToQuic()` 发送 `QUIC_SEND_FLAG_FIN`。如果有压缩器，会先 flush 结束帧。见 `src/tunnel/linux_relay_worker.cpp:970`、`src/tunnel/linux_relay_worker.cpp:1080`、`src/tunnel/linux_relay_worker.cpp:1119`。

Windows `HandleTcpRecv()` 中 `bytes == 0` 表示 TCP recv EOF；它设置 `TcpRecvClosed`，flush 压缩器，并发送带 `QUIC_SEND_FLAG_FIN` 的 stream send。之后关闭本地 TCP socket。见 `src/tunnel/windows_relay_worker.cpp:569`、`src/tunnel/windows_relay_worker.cpp:586`、`src/tunnel/windows_relay_worker.cpp:594`。

### 4.3 TCP reset / TCP 异常 -> relay 停止

Linux TCP 读侧硬错误会记录 `TcpReadHardErrors` / `LastTcpReadErrno`，进入 `FailRelayFatal()`，reset TCP、解绑 stream 并设置 relay stop。TCP 写侧硬错误会记录 `TcpWriteHard` 并 `SetRelayStop(relay, "tcp_write_hard_error")`。见 `src/tunnel/linux_relay_worker.cpp` 的 `DrainTcpReadable()` 和 `FlushTcpWrites()`。

Windows 的 IOCP 完成错误、WSARecv/WSASend 投递失败、TCP send completion 异常会记录 `TcpHardErrors` 并进入 `FailRelayFatal()`；fatal cleanup 使用 `TqResetSocket()` reset TCP、解绑 stream callback、complete pending receive，并设置 public handle stop。见 `src/tunnel/windows_relay_worker.cpp` 的 `Run()`、`RecordTcpHardErrorAndFail()` 和 `CloseRelay(..., AbortReset)`。

### 4.4 QUIC send 异常

Linux `SubmitTcpBatchToQuic()` 会区分 fatal 和资源类失败。stream/handle 无效等不可恢复状态会记录 `QuicSendFatalErrors` 并让上层停止 relay；`QUIC_STATUS_OUT_OF_MEMORY`、`QUIC_STATUS_BUFFER_TOO_SMALL` 等资源类 send 失败会记录 `QuicSendBackpressureEvents`，暂停 TCP read，并尽量保留 send operation 供后续重试。

Windows `stream->Send()` 失败也增加了分类计数：资源类状态记录 `QuicSendBackpressureEvents`，不可恢复状态记录 `QuicSendFatalErrors` 并进入 `FailRelayFatal()`。

## 5. QUIC stream -> TCP：数据、FIN、异常

### 5.1 Stream RECEIVE -> TCP write

Linux stream callback 收到 `QUIC_STREAM_EVENT_RECEIVE` 后把 MsQuic receive buffer 做成 deferred view，入队到 worker，返回 `QUIC_STATUS_PENDING`。worker 按实际写入 TCP 的进度调用 `StreamReceiveComplete()`，避免提前释放 MsQuic receive buffer。见 `src/tunnel/linux_relay_worker.cpp:2334`、`src/tunnel/linux_relay_worker.cpp:2351`、`src/tunnel/linux_relay_worker.cpp:2359`、`src/tunnel/linux_relay_worker.cpp:1586`、`src/tunnel/linux_relay_worker.cpp:1607`。

Windows 同样在 stream callback 对 `RECEIVE` 调 `QueueDeferredQuicReceive()`，返回 `QUIC_STATUS_PENDING`；后续 IOCP worker 把 view 写到 TCP，并按进度 complete receive。见 `src/tunnel/windows_relay_worker.cpp:1358`、`src/tunnel/windows_relay_worker.cpp:1369`、`src/tunnel/windows_relay_worker.cpp:708`、`src/tunnel/windows_relay_worker.cpp:1025`。

### 5.2 Stream FIN -> TCP 写半关闭

Linux receive view 带 FIN 时，待所有 pending QUIC receive 写入 TCP 后设置 `TcpWriteShutdownQueued`，最终 `shutdown(tcpFd, SHUT_WR)`，并标记 `TcpWriteClosed=true`。见 `src/tunnel/linux_relay_worker.cpp:1511`、`src/tunnel/linux_relay_worker.cpp:1610`、`src/tunnel/linux_relay_worker.cpp:1650`。

Windows FIN 且无数据时直接 `TqShutdownSend(tcpFd)`；有数据时在 `FinishReceiveView()` 完成该 view 后再 `TqShutdownSend(tcpFd)`。见 `src/tunnel/windows_relay_worker.cpp:740`、`src/tunnel/windows_relay_worker.cpp:1045`。

### 5.3 Stream reset / abort / shutdown complete -> TCP 关闭或半关闭

Linux：

- `QUIC_STREAM_EVENT_PEER_SEND_ABORTED` 和 `QUIC_STREAM_EVENT_PEER_RECEIVE_ABORTED` 都进入 `FailRelayFatal()`，reset 对应 TCP fd，解绑 stream callback，并设置 relay stop。
- `QUIC_STREAM_EVENT_SHUTDOWN_COMPLETE` 解绑 stream 并调用 `MaybeStopFullyClosedRelay()`；如果 TCP 两侧和 QUIC FIN 都已完成，则设置 handle stop。见 `src/tunnel/linux_relay_worker.cpp:2374`、`src/tunnel/linux_relay_worker.cpp:2406`、`src/tunnel/linux_relay_worker.cpp:1080`。

Windows：

- `PEER_SEND_ABORTED`、`PEER_RECEIVE_ABORTED` 进入 `FailRelayFatal()` 并 reset TCP。
- `SHUTDOWN_COMPLETE.ConnectionShutdown` 进入 `FailRelayFatal()`；非 connection shutdown 的 shutdown complete 仍走 `CloseAfterDrained`，待 pending QUIC receive / in-flight TCP send 排空后对 TCP 执行 send shutdown 并设置 handle stop。

## 6. QUIC connection 中断 -> 关闭对应 TCP

client 和 server connection callback 在以下事件中都会调用 `TqAbortConnectionTunnels(connection)`：

- `QUIC_CONNECTION_EVENT_SHUTDOWN_INITIATED_BY_TRANSPORT`
- `QUIC_CONNECTION_EVENT_SHUTDOWN_INITIATED_BY_PEER`
- `QUIC_CONNECTION_EVENT_SHUTDOWN_COMPLETE`

client 见 `src/protocol/quic_session.cpp:1001`、`src/protocol/quic_session.cpp:1020`、`src/protocol/quic_session.cpp:1037`；server 见 `src/protocol/quic_session.cpp:1278`、`src/protocol/quic_session.cpp:1290`、`src/protocol/quic_session.cpp:1301`。

`TqAbortConnectionTunnels()` 从 registry 中找出该 connection 下所有 tunnel，逐个调用 tunnel 的 abort 回调。见 `src/tunnel/tunnel_registry.cpp:114`、`src/tunnel/tunnel_registry.cpp:136`。

`TqTunnelContext::AbortForConnectionShutdown()` 的行为：

- 标记 `AbortedByConnection=true`、`SelfDeleteOnShutdown=true`。
- 立即 `CloseTcpLocked()` 关闭当前 tunnel 的 TCP fd。
- 如果 relay 已启动，调用 `TqRelayStop(&RelayHandle)`，平台 worker 负责解绑 stream callback、清理 pending receive/send。
- 如果 relay 尚未启动但 stream 仍有效，则对 stream 执行 `Shutdown(..., QUIC_STREAM_SHUTDOWN_FLAG_ABORT)`。

见 `src/tunnel/tcp_tunnel.cpp:528`、`src/tunnel/tcp_tunnel.cpp:545`、`src/tunnel/tcp_tunnel.cpp:553`、`src/tunnel/tcp_tunnel.cpp:558`。

这意味着 QUIC connection 级中断会以 connection 为粒度关闭该连接承载的所有 TCP tunnel。多 QUIC connection 场景下，registry key 是 `MsQuicConnection*`，不会主动 abort 其他 connection 上的 tunnel。

## 7. 关闭矩阵

| 场景 | Linux 行为 | Windows 行为 | 备注 |
| --- | --- | --- | --- |
| TCP 建立后打开 stream | client 创建 `MsQuicStream`，open request/response 成功后 `TqRelayStart()` | 同左 | 平台 worker 只接管 relay 阶段 |
| TCP EOF / shutdown read | `readv()==0` -> `TcpReadClosed` -> QUIC FIN | `bytes==0` -> `TcpRecvClosed` -> QUIC FIN | FIN 前会 flush zstd |
| TCP hard error | 读侧记录 `TcpReadHardErrors`，写侧记录 `TcpWriteHardErrors`，并 stop relay | IOCP/WSA/TCP send completion 硬错误记录 `TcpHardErrors` 并 fatal reset | 具体 errno/WSA code 未在文档范围内展开 |
| TCP reset | read/write hard error 路径收敛到 relay stop/fatal cleanup | IOCP/WSA 错误路径 reset TCP 并清理 relay | |
| stream RECEIVE | deferred receive view -> 写 TCP -> `ReceiveComplete` | deferred receive view -> WSASend TCP -> `ReceiveComplete` | callback 返回 `QUIC_STATUS_PENDING` |
| stream FIN | pending 写完后 `shutdown(SHUT_WR)` | view 完成后 `TqShutdownSend()` | 对应 TCP 写半关闭 |
| stream peer send/receive aborted | fatal cleanup，reset TCP | fatal cleanup，reset TCP | 跨平台语义已对齐 |
| stream shutdown complete | 解绑 stream，若 TCP/QUIC 全部收尾则 handle stop | connection shutdown fatal reset；非 connection shutdown drain 后 close condition | |
| QUIC connection transport/peer shutdown/complete | registry abort 对应 connection 的 tunnel，关闭 TCP，停止 relay/abort stream | 同左 | connection 粒度 |

## 8. Review 关注点

- Linux `PEER_SEND_ABORTED` / `PEER_RECEIVE_ABORTED` 已进入 fatal reset；仍建议补充专门单测覆盖 peer abort 后 TCP fd reset 和 callback binding retire。
- Linux TCP read hard error 已记录 errno 并 stop relay；仍建议补充 `EPOLLERR` + `readv()` 错误的定向单测，避免只依赖现有 IO 测试的间接覆盖。
- Windows stream abort/connection shutdown 已改为 fatal reset；**已在 Windows build host 验证**：`tcpquic_platform_socket_test` 与 `tcpquic_windows_relay_worker_test` 均 exit 0，覆盖 peer abort、connection shutdown、receive backpressure、QUIC send 分类、TCP 硬错误与 graceful drain 路径。
- Windows QUIC send transient backpressure 已有分类计数与 pending send retry 队列；长期资源紧张下的 retry 行为仍见第 10 节 Task 8 follow-up。
- connection 中断路径相对明确：client/server 三类 shutdown 事件都会调用 `TqAbortConnectionTunnels()`，并且 registry 只影响同一个 `MsQuicConnection*` 下注册的 tunnel。

## 9. Implemented Error Semantics

- 平台层新增 `TqResetSocket()`，用 `SO_LINGER {1,0}` 后 close/closesocket 表达 TCP reset close。Linux/Windows fatal relay cleanup 都以这个 helper 作为 TCP reset 原语。
- Linux TCP 读侧硬错误已纳入异常处理：`DrainTcpReadable()` 在非 `EINTR/EAGAIN/EWOULDBLOCK` 的 `readv()` 错误上记录 `TcpReadHardErrors` 和 `LastTcpReadErrno`，进入 fatal reset cleanup。
- Windows TCP/IOCP 硬错误已走统一 fatal helper：IOCP completion error、WSARecv/WSASend post failure、TCP send completion 异常会记录 `TcpHardErrors` 并进入 `FailRelayFatal()`，用 reset close 清理 TCP。
- Linux QUIC receive event queue 满已改为背压：`EventQueue.TryPush()` 失败时不再 `StreamShutdown(ABORT)`，而是把 receive view 保存在 per-relay callback backlog，暂停 QUIC receive，等待 owner worker drain 后再 `ReceiveComplete()`。
- Windows QUIC receive pending bytes 超限已改为背压：当前 receive view 会继续入队，relay 暂停后续 QUIC receive，而不是关闭 relay。
- stream fatal abort 语义已对齐：Linux/Windows 对 `PEER_SEND_ABORTED`、`PEER_RECEIVE_ABORTED` 进入 fatal cleanup，并 reset 对应 TCP；Windows `SHUTDOWN_COMPLETE.ConnectionShutdown` 也进入 fatal cleanup。
- stream graceful FIN/shutdown 仍走排空后 TCP send shutdown：Linux 在 pending QUIC receive 写完后 `shutdown(SHUT_WR)`；Windows 在 FIN view 完成或 `CloseRelayIfDrained()` 中 `TqShutdownSend()`，并记录 `GracefulRelayDrains`。
- QUIC send 失败已开始分类：Linux 对 stream/handle 无效、无法保留当前 TCP 数据的本地资源失败等 fatal 状态记录 `QuicSendFatalErrors` 并停止 relay；对 `stream->Send()` 返回的 `QUIC_STATUS_OUT_OF_MEMORY` / `QUIC_STATUS_BUFFER_TOO_SMALL` 等资源类状态记录 `QuicSendBackpressureEvents`，保留 send operation 并暂停 TCP read。Windows 同步增加 send failure 分类计数、fatal cleanup 分支和 pending send retry 队列。
- 不可恢复 relay 错误会写 `spdlog::error`，日志包含 reason、relay id、TCP fd/socket、pending bytes/queue 和 in-flight send 状态；新增 fatal/backpressure counters 已纳入 admin metrics JSON 输出。

## 10. Remaining Follow-ups

- Windows QUIC send retry 在 sustained resource pressure 下的行为观察（Task 8 follow-up）：确认 pending send retry 队列在长期 `QUIC_STATUS_OUT_OF_MEMORY` 下不会永久停滞，recovery 后 TCP read 能恢复。
- （可选）Linux 侧仍缺 `PEER_RECEIVE_ABORTED` 与 QUIC send 背压的定向单元测试，可与 alignment plan Task 7 对齐补充。
