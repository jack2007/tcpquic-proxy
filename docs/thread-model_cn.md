# 线程模型

本文描述 `tcpquic-proxy` 当前生产实现中的线程来源和职责边界。重点是
2026-06-18 async reactor thread model 合并后的状态：Linux 控制面已经迁移到
reactor 模型；Windows 仍保留旧 listener / handshake pool / per-OPEN dial worker
模型。Relay 数据面线程模型未在本次迁移中改变。

## 总览

当前线程模型按平台分为两条路径：

- **Linux client**：本地 SOCKS5 / HTTP CONNECT 的 `accept()`、代理握手和
  client OPEN 发起由 `TqClientIngressReactor` 处理。single-peer 进程内一个
  ingress reactor；multi-peer 进程内也只有一个共享 ingress reactor，所有 peer 的
  listen fd 都注册到这一个 reactor。
- **Linux server**：普通 OPEN 的 ACL、DNS 解析和 TCP connect 由一个
  `TqServerDialReactor` 线程处理。DNS 使用 `third_party/c-ares` 异步解析。
- **Windows**：仍使用旧模型，client 侧有 SOCKS5 / HTTP CONNECT listener thread
  和 handshake `TqThreadPool`，server 侧每个普通 OPEN 使用短生命周期 dial worker。
- **Relay worker**：Linux / Windows 均保持固定 worker 模型；控制面 reactor 不接管
  已建立隧道后的 TCP/QUIC 数据转发。

Linux 控制面路径如下：

```text
client:
  TqClientIngressReactor worker thread
    -> accept SOCKS5 / HTTP CONNECT
    -> 非阻塞握手状态机
    -> TqStartClientTunnelAsync()
    -> OPEN 完成事件回投 ingress reactor
    -> 写 SOCKS5 reply / HTTP 200
    -> relay worker 接管 socket

server:
  MsQuic stream callback 收到普通 OPEN
    -> TqServerDialReactor::Submit()
    -> ACL / c-ares async DNS / non-blocking TCP connect
    -> OPEN response
    -> relay worker 接管 socket
```

Windows 当前路径仍是：

```text
client listener thread -> handshake pool worker -> 同步 client OPEN -> relay worker
server MsQuic callback -> per-OPEN dial worker -> relay worker
```

## Linux Client Ingress Reactor

Linux client 侧的入口线程由 `TqClientIngressReactor::Start()` 创建。这个线程内部使用
`TqLinuxReactor`，也就是 `epoll` + `eventfd` 封装，统一管理 listen socket、accepted
client socket 和跨线程投递任务。

它负责：

- 为所有 active peer 注册 SOCKS5 listen fd 和可选 HTTP CONNECT listen fd。
- 在 listen fd 可读时执行非阻塞 `accept4()`。
- 将 accepted client socket 设置为 non-blocking，并注册到同一个 reactor。
- 推进 SOCKS5 greeting、SOCKS5 CONNECT request、HTTP CONNECT request 的状态机。
- 握手完成后调用 `TqStartClientTunnelAsync()` 发起 QUIC OPEN。
- 把 OPEN 完成回调通过 `EnqueueAsync()` 投回 ingress reactor 线程。
- 根据 OPEN 结果写回 SOCKS5 reply 或 HTTP response。
- OPEN 成功后调用 `TqAcceptClientTunnelOpen()`，让 relay 接管 client fd。
- 在客户端提前关闭、OPEN 失败、peer 删除、进程停止时取消 pending open 并清理 fd。

握手解析由 `TqClientIngressState` 完成。实现要求握手阶段只读取协议边界内的数据：
如果客户端在 SOCKS5 / HTTP CONNECT 握手包后紧跟应用层 early data，ingress reactor
不能把这些数据预读进自己的缓冲区；这些数据必须留在 TCP 接收队列中，等 relay worker
接管后再转发。

相关代码：

- `src/ingress/client_ingress_reactor.cpp`
- `src/ingress/client_ingress_reactor.h`
- `src/ingress/client_ingress_state.cpp`
- `src/ingress/client_ingress_state.h`
- `src/runtime/linux_reactor.cpp`
- `src/tunnel/client_tunnel_open.cpp`
- `src/main.cpp`

## Single-Peer 与 Multi-Peer

Linux single-peer client 的 runtime 持有一个 `TqClientIngressReactor` 成员。QUIC 至少
有一个 connected connection 时，runtime 会把 primary peer 的 SOCKS5 / HTTP listen
fd 注册到该 reactor；所有 QUIC connection 断开时会移除 listen fd，listener 保持关闭，
直到 reconnect 成功后重新注册。

Linux multi-peer client 的 runtime adapter 持有一个共享 `TqClientIngressReactor`。
每个 peer 启动自己的 `QuicClientSession`，但不再创建自己的 listener thread 或
handshake pool。peer connected count 大于 0 时，adapter 将该 peer 的 listen fd 注册
到共享 reactor；peer drain、删除或断连时，移除该 peer 的 listen fd 和还在握手 /
OPEN 阶段的 client socket。

`--handshake-threads` 和 JSON `client.handshake_threads` 仍保留给非 Linux 路径使用。
在 Linux ingress reactor 路径中，它们不会创建 client 握手线程池。

## Linux Server Dial Reactor

Linux server 侧在 `RunServer()` 中创建一个 `TqServerDialReactor`，并通过
`TqSetServerDialReactor()` 暴露给普通 OPEN 处理路径。入站 QUIC stream 仍先在 MsQuic
callback 中进入 `TqHandleServerIncomingStream()`；当识别为普通 OPEN 后，不在 MsQuic
callback 内做 DNS 或 TCP connect，而是提交给 dial reactor。

`TqServerDialReactor` 负责：

- 接收普通 OPEN 的 dial request。
- 对 literal IP 目标跳过 DNS，直接进入已解析地址过滤。
- 对域名目标调用 `TqAresDnsResolver`，通过 c-ares 异步解析 A/AAAA 地址。
- 对解析结果执行 no-DNS ACL 过滤；speed-test 的受控 loopback 授权可绕过普通 ACL。
- 对候选地址逐个创建 non-blocking TCP socket 并发起 `connect()`。
- 用 `epoll` 监听 connect fd 的 writable/error 事件，并用 deadline 处理 connect timeout。
- 将失败映射成既有 OPEN 错误语义，例如 `DnsFailed`、`AclDenied`、`TcpRefused`、
  `TcpTimeout`、`Internal`。
- connect 成功后把 connected fd 交回 tunnel 层，由 tunnel 层发送 OPEN response 并启动
  relay。

`TqServerDialReactor` 本身创建一个 worker thread。它内部还有一个 `TqLinuxReactor`
用于 TCP connect fd，DNS resolver 内部也用一个 `TqLinuxReactor` 驱动 c-ares socket；
这些都由同一个 dial reactor worker 循环推进，不额外引入 DNS 线程池。

相关代码：

- `src/tunnel/server_dial_reactor.cpp`
- `src/tunnel/server_dial_reactor.h`
- `src/tunnel/ares_dns_resolver.cpp`
- `src/tunnel/ares_dns_resolver.h`
- `src/acl/acl_filter.cpp`
- `src/tunnel/tcp_tunnel.cpp`
- `src/main.cpp`

## Server Fallback 与 Windows 边界

Linux server 当前生产启动路径会创建 `TqServerDialReactor`。代码中仍保留兼容 fallback：
如果全局 server dial reactor 不存在，普通 OPEN 会回到旧的 per-OPEN dial worker 路径。
这主要用于非 Linux 路径或特殊测试场景。

Windows 当前未迁移到新的控制面 reactor：

- client 侧 SOCKS5 和 HTTP CONNECT 各自有 listener thread。
- listener `accept()` 后把 client fd 提交给 handshake `TqThreadPool`。
- handshake worker 使用旧同步 client OPEN 路径。
- server 普通 OPEN 使用短生命周期 dial worker；DNS 仍走旧同步解析路径。

因此不要把 Linux 的“一个 ingress reactor / 一个 dial reactor”结论外推到 Windows。

## Relay Worker 线程

隧道建立后，TCP/QUIC 数据转发进入 relay backend。Relay worker 线程模型没有变化：

- **Linux**：固定数量 `TqLinuxRelayWorker`，使用 epoll/eventfd/readv/writev 分片处理
  relay fd。
- **Windows**：固定数量 `TqWindowsRelayWorker`，使用 IOCP 处理 relay I/O。

控制面 reactor 只覆盖低流量的入口握手、OPEN、DNS 和 TCP connect。relay worker 仍是
高吞吐数据面，不与 client ingress reactor 或 server dial reactor 合并。

相关代码：

- `src/tunnel/linux_relay_worker.cpp`
- `src/tunnel/windows_relay_worker.cpp`
- `src/tunnel/relay.cpp`

## 其它应用线程

除了控制面 reactor 和 relay worker，进程中还可能有这些线程：

- **MsQuic 内部线程**：由 MsQuic 创建，负责 QUIC datapath、worker 和 cleanup 等内部工作；
  应用 stream callback 运行在 MsQuic worker 上。
- **client reconnect thread**：每个 `QuicClientSession` 启动一个 reconnect loop，用于补齐
  断开的 QUIC connection slot。
- **admin HTTP 线程**：启用 `--admin-listen` 时，admin server 有 accept 线程，并为 admin
  client 使用短生命周期处理线程。
- **trace periodic thread**：启用 `--trace` 后周期输出统计快照。
- **tunnel reaper thread**：进程级 `TqTunnelReaper` 单线程统一回收停止后的 tunnel context。
- **speed-test 临时线程**：内置 upload/download speed-test 会创建临时 loopback TCP server
  和 pump worker；它不代表常规代理流量路径。

这些线程没有随本次 Linux 控制面迁移而消失，观察线程数时需要和 ingress/dial/relay
线程分开看。

## 线程数量估算

常规 Linux client：

```text
1 main thread
+ MsQuic internal threads
+ P * 1 client reconnect thread
+ 1 TqClientIngressReactor worker thread
+ R relay worker threads
+ 1 tunnel reaper thread
+ optional admin / trace / speed-test threads
```

其中 `P` 是 active peer 数。multi-peer 下 `P` 会影响 `QuicClientSession` 和 reconnect
thread 数量，但不会再生成 `P * (SOCKS5 listener + HTTP listener + handshake pool)`。

常规 Linux server：

```text
1 main thread
+ MsQuic internal threads
+ 1 TqServerDialReactor worker thread
+ R relay worker threads
+ 1 tunnel reaper thread
+ optional admin / trace / speed-test threads
```

普通 OPEN 数量不会再线性增加 dial worker 线程数。pending DNS 和 pending TCP connect
以 reactor state 的形式挂在 `TqServerDialReactor` 内部。

常规 Windows client / server 仍按旧模型估算：

```text
Windows client:
  per peer reconnect thread
  + SOCKS5 listener thread
  + optional HTTP CONNECT listener thread
  + handshake pool workers
  + relay workers

Windows server:
  per ordinary OPEN short-lived dial worker
  + relay workers
```

## 调试时的判断边界

排查 Linux 上的一条普通代理连接时，可以按下面的边界理解：

1. client ingress reactor 接受本地 TCP 连接并完成 SOCKS5 / HTTP CONNECT 握手。
2. ingress reactor 发起异步 client OPEN，但 OPEN response 由 MsQuic callback 推进后回投
   ingress reactor。
3. OPEN 成功后 ingress reactor 写回代理协议响应，并把 client fd 移交给 relay。
4. server 收到普通 OPEN 后，server dial reactor 完成 ACL、DNS 和 TCP connect。
5. server connect 成功后，tunnel 层发送 OPEN response 并启动 relay。
6. 后续数据面完全由 relay worker 和 MsQuic worker 处理。

排查 Windows 上的一条连接时，仍应按旧模型理解：listener thread 接受连接，handshake
pool worker 处理代理握手和同步 OPEN，server 侧 per-OPEN dial worker 处理 DNS/connect，
relay worker 处理后续数据面。
