# Issues Addressed

本文记录对客户端 HTTP CONNECT/SOCKS5 接入、客户端 relay/QUIC 适配、服务端 TCP dial/relay 路径的代码审查结果和修复状态。日期：2026-06-22。

## 1. 异步客户端 OPEN 缺少超时收敛

- 严重度：High
- 状态：已修复。客户端 ingress reactor 对 async OPEN 使用 10 秒超时；超时后写本地代理失败响应并 cancel handle。单测使用测试钩子缩短超时。
- 影响路径：客户端 HTTP CONNECT/SOCKS5 reactor 接入 -> `TqStartClientTunnelAsync()` -> QUIC OPEN 响应等待。
- 证据：
  - `src/ingress/client_ingress_reactor.cpp:756` 将 client 标记为 `Opening`，并只保留 `Error` 事件。
  - `src/tunnel/tcp_tunnel.cpp:1624` 的异步 OPEN 创建 stream、发送 OPEN 后返回 handle，但没有启动等价于同步路径 `WaitForOpenResponse(TqOpenTimeout)` 的定时器。
  - 同步旧路径在 `src/tunnel/tcp_tunnel.cpp:1599` 还能通过 `FinishClientOpenAndStartRelay()` 等待超时后失败。
- 风险：如果服务端或 QUIC stream 没有返回 `OPEN_OK/OPEN_FAIL`，本地代理连接会长期卡在 Opening；客户端收不到 SOCKS/HTTP 失败响应，`OpenHandle` 也只能等 peer removal / reactor stop / QUIC abort 才释放。
- 建议：
  - 在 `TqClientIngressReactor::StartClientOpen()` 或 `TqClientTunnelOpenHandle` 层增加 OPEN deadline。
  - 超时后调用 `CompleteClientOpen(... TcpTimeout ...)`，同时 cancel/reject async handle，确保 SOCKS 返回 TTL expired/HTTP 返回 504。
  - 增加单测：异步 OPEN 无 completion 时，超时写失败响应并只调用一次 cancel/reject。

## 2. 客户端 ingress reactor 线程可能被每个新 tunnel 同步等待 QUIC 连接阻塞

- 严重度：High
- 状态：已修复。`TqClientPeerRuntime::StartTunnel()` 只选择已连接 QUIC connection，不再在 ingress reactor 线程调用默认 10 秒的 `EnsureAnyConnected()`。
- 影响路径：客户端 reactor 接入 -> `TqClientPeerRuntime::StartTunnel()`。
- 证据：
  - `src/ingress/client_ingress_reactor.cpp:789` 在 reactor 线程直接调用 `startTunnel(...)`。
  - `src/runtime/client_peer_runtime.cpp:245` 在 `StartTunnel()` 中调用 `Quic->EnsureAnyConnected()`，未传入更短超时。
  - `src/protocol/quic_session.cpp:628` 的 `EnsureConnected()` 会循环等待直到超时，默认声明为 10 秒。
- 风险：监听器虽会在连接状态回调中关闭，但连接刚断开、状态尚未应用或多 peer 切换时，单个入站请求可能让整个 ingress reactor 阻塞最多约 10 秒；期间其它 SOCKS/HTTP 客户端、peer removal、queued completion、delayed reconnect task 都被拖住。
- 建议：
  - reactor 热路径只做非阻塞连接选择；无可用连接应立即返回 open failure。
  - 如确实要等待连接，应把等待移出 ingress reactor 线程，并为该 open 设置独立 deadline。
  - 增加单测：`StartTunnel` 模拟阻塞时，reactor 仍能处理其它 peer/remove/stop。

## 3. OPEN 控制帧之后的同一 QUIC receive 中额外字节未移交给 relay

- 严重度：Medium
- 状态：已修复。OPEN 控制帧后的剩余 bytes 会保存在 tunnel context 中，并在 relay callback 接管后补投递；补投递 buffer 生命周期覆盖 relay。
- 影响路径：QUIC 适配层 OPEN 解析 -> relay callback 切换。
- 证据：
  - `src/tunnel/tcp_tunnel.cpp:930` 将 receive buffer 全部追加到 `OpeningRx`。
  - 客户端 `TryCompleteClientOpen()` 在 `src/tunnel/tcp_tunnel.cpp:943` 只解码前 `TQ_OPEN_RESPONSE_SIZE` 字节，未消费或转发剩余字节。
  - 服务端 `TryHandleServerOpen()` 在 `src/tunnel/tcp_tunnel.cpp:1068` 根据 `expectedLen` 解码 OPEN request，也没有处理 `OpeningRx` 中超过控制帧长度的字节。
  - relay 只在 `StartPendingServerRelay()` / `StartPendingClientRelay()` 中启动，见 `src/tunnel/tcp_tunnel.cpp:1419` 和 `src/tunnel/tcp_tunnel.cpp:1436`。
- 风险：正常实现会等 OPEN 成功后才开始传业务数据，因此该问题通常不触发；但 QUIC 层如果把 OPEN 响应和随后 relay 数据合并到同一 receive event，或对端异常地在 OPEN 后立即发送 payload，控制帧后的 payload 会留在 `OpeningRx`，不会交给新的 relay callback，造成首批数据丢失或协议卡住。
- 建议：
  - OPEN 解码后显式消费控制帧长度。
  - relay 注册时支持把剩余 `OpeningRx` 作为初始 QUIC receive view 投递，或在切换 callback 前主动转交。
  - 增加单测覆盖：`OPEN_OK + payload` 同一 receive event、`OPEN request + payload` 同一 receive event。

## 4. 旧阻塞 HTTP/SOCKS 接入路径仍可在 relay 启动后关闭已移交的 TCP fd

- 严重度：Medium
- 状态：已修复。旧阻塞 `TqHttpConnectServer` / `TqSocks5Server` class 和 `Run*Server` API 已删除，仅保留 parser/helper 函数供新 reactor/state 使用。
- 影响路径：遗留 `TqHttpConnectServer` / `TqSocks5Server` 阻塞接入。
- 证据：
  - `src/ingress/http_connect_server.cpp:258` 在 `onTunnel()` 成功后才发送 HTTP 200；如果 `TqSendHttpStatus()` 失败，`src/ingress/http_connect_server.cpp:267` 会直接 `TqCloseSocket(clientFd)`。
  - `src/ingress/socks5_server.cpp:447` 同样在 `onTunnel()` 成功后才发送 SOCKS success；失败时 `src/ingress/socks5_server.cpp:456` 直接关闭 `clientFd`。
  - 同步 tunnel 成功路径会在 `src/tunnel/tcp_tunnel.cpp:1599` 启动 relay，此时 fd 已由 tunnel/relay 持有。
- 风险：如果这些旧 server 被测试工具或未来代码重新接入，成功 OPEN 后的代理响应写失败会关闭 relay 正在使用的 fd，可能引发双重 close、fd 复用误伤或 relay 状态错乱。当前 `main` 使用 `TqClientIngressReactor`，新路径已先写本地代理响应再 `AcceptTunnel`，风险主要来自遗留代码保留。
- 建议：
  - 明确删除/隔离旧阻塞 server，或改成与 reactor 一样的顺序：本地代理成功响应写完后才启动/accept relay。
  - 如果保留旧 API，应在 `onTunnel` 成功后转移 fd 所有权，失败写响应只能 abort tunnel，不能直接 close 原 fd。

## 5. 服务端 dial reactor 缺少 tcp dialing 可观测性

- 严重度：Medium
- 状态：已修复。按当前决策不增加额外配置上限；实际上限由 peer 的 QUIC stream 数约束。新增 `TqServerDialReactor::PendingCount()`，并在 server admin `/health`、`/metrics` JSON 中暴露 `tcp_dialing`。
- 影响路径：服务端 QUIC OPEN -> `TqServerDialReactor::Submit()` -> DNS/connect pending。
- 证据：
  - `src/tunnel/tcp_tunnel.cpp:1008` 为每个 OPEN 构造 `TqServerDialRequest` 并提交到全局 server dial reactor。
  - `src/tunnel/server_dial_reactor.cpp:214` 的 `Submit()` 创建 `DialState` 并在 `src/tunnel/server_dial_reactor.cpp:229` 加入 `Pending`，未看到全局 pending 数、每连接 pending 数或 per-peer 限流检查。
- 风险：大量并发 OPEN 可以堆积 DNS 请求、非阻塞 connect fd 和内存状态；当前通过 QUIC stream 数限制并发面，并通过 `tcp_dialing` 暴露当前服务端 TCP dialing 数。
- 建议：
  - 持续观察 `tcp_dialing` 与 active streams 的比例。
  - 如后续发现 stream 数上限不足以保护 fd/DNS 资源，再补 per-peer 或全局限流策略。

## 已检查但暂未列为问题

- 新 `TqClientIngressReactor` 使用 1 字节读握手，避免 HTTP/SOCKS parser 在接管 relay 前读走首包；`TqClientIngressState::TakeBufferedData()` 目前主要由单测验证，生产路径依赖 1 字节读取规避 over-read。
- `TqServerDialReactor` 已覆盖 DNS cancel、connect cancel、completion reenter cancel 等单测；现阶段主要缺口是容量/背压策略，而不是基础 cancel 生命周期。
