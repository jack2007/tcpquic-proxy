# 线程模型

本文说明 `tcpquic-proxy` 当前生产实现中的线程来源和职责边界。控制面已经收敛为跨平台 socket reactor 模型：client 侧本地入口由 ingress reactor 驱动，server 侧普通 OPEN 的 ACL、DNS 和 TCP connect 由 dial reactor 驱动。Relay 数据面线程模型不随本次控制面迁移改变，继续保持各平台既有 worker 模型。

## 总览

当前线程模型按职责划分为两层：

- **Client ingress 控制面**：所有 peer 的 SOCKS5 / HTTP CONNECT `listen`、`accept` 和握手状态机由一个 `TqClientIngressReactor` 线程处理。Linux 底层使用 `TqLinuxReactor`，Windows 底层使用 `TqWindowsReactor`，macOS 底层使用 `TqDarwinReactor`。
- **Server dial 控制面**：普通 OPEN 的 ACL、DNS 解析和 TCP connect 由一个 `TqServerDialReactor` 线程处理。DNS 使用 vendored c-ares 异步解析，并由 reactor 线程驱动 c-ares sockets 和 timeout。
- **Relay worker 数据面**：Linux / Windows / macOS 均保持既有固定 worker 模型；Windows relay 仍由 `TqWindowsRelayWorker` 使用 IOCP 和 overlapped I/O 处理高吞吐 TCP/QUIC 转发，macOS relay 仍由 `TqDarwinRelayWorker` 使用 kqueue 处理 relay fd readiness 和跨线程唤醒。

控制面路径如下：

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

Windows / macOS 旧路径已经被替换：client 不再为每个 peer 创建独立 listener thread 和 bounded handshake `TqThreadPool`；server 普通 OPEN 不再为每条连接创建 detached dial worker。旧 fallback 只作为未配置 reactor 时的兼容路径存在，不是当前 Windows / macOS runtime 的主路径。

## Client Ingress Reactor

Client 模式下，进程使用一个 `TqClientIngressReactor` 线程统一处理本地入口。它覆盖 single-peer 和 multi-peer 两种运行方式：所有 peer 的 SOCKS5 listen socket 和 HTTP CONNECT listen socket 都注册到同一个 ingress reactor，而不是每个 peer 各自启动 listener 线程和 handshake pool。

它负责：

- 为所有 active peer 注册 SOCKS5 listen fd 和可选 HTTP CONNECT listen fd。
- 在 listen fd 可读时执行非阻塞 `accept()` / `accept4()`。
- 将 accepted client socket 设置为 non-blocking，并注册到同一个 reactor。
- 推进 SOCKS5 greeting、SOCKS5 CONNECT request、HTTP CONNECT request 的状态机。
- 握手完成后调用 `TqStartClientTunnelAsync()` 发起 QUIC OPEN。
- 把 OPEN 完成回调通过 `EnqueueAsync()` 投回 ingress reactor 线程。
- 根据 OPEN 结果写回 SOCKS5 reply 或 HTTP response。
- OPEN 成功后调用 `TqAcceptClientTunnelOpen()`，让 relay 接管 client fd。
- 在客户端提前关闭、OPEN 失败、peer 删除、进程停止时取消 pending open 并清理 fd。

握手解析由 `TqClientIngressState` 完成。实现要求握手阶段只读取协议边界内的数据：如果客户端在 SOCKS5 / HTTP CONNECT 握手包后紧跟应用层 early data，ingress reactor 不能把这些数据预读进自己的缓冲区；这些数据必须留在 TCP 接收队列中，等 relay worker 接管后再转发。

Windows 和 macOS client ingress 与 Linux 使用同一套业务状态机。Windows 由 `TqWindowsReactor` 通过 Winsock event 驱动 listen / handshake socket；macOS 由 `TqDarwinReactor` 通过 kqueue 驱动 listen / handshake socket。OPEN 完成回调不会直接跨线程修改 ingress state，而是投递回 ingress reactor 线程；认证状态和 OPEN lifetime 都由 guard/token 保护，避免 Stop、RemovePeer 或关闭连接后的迟到回调触达已释放状态。

相关代码：

- `src/ingress/client_ingress_reactor.cpp`
- `src/ingress/client_ingress_reactor.h`
- `src/ingress/client_ingress_state.cpp`
- `src/ingress/client_ingress_state.h`
- `src/runtime/listen_socket.cpp`
- `src/runtime/listen_socket.h`
- `src/runtime/scoped_socket.h`
- `src/tunnel/client_tunnel_open.cpp`
- `src/main.cpp`

## Single-Peer 与 Multi-Peer

single-peer client 的 runtime 持有一个 `TqClientIngressReactor` 成员。QUIC 至少有一个 connected connection 时，runtime 会把 primary peer 的 SOCKS5 / HTTP listen fd 注册到该 reactor；所有 QUIC connection 断开时会移除 listen fd，listener 保持关闭，直到 reconnect 成功后重新注册。

multi-peer client 的 runtime adapter 持有一个共享 `TqClientIngressReactor`。每个 peer 启动自己的 `QuicClientSession`，但不再创建自己的 listener thread 或 handshake pool。peer connected count 大于 0 时，adapter 将该 peer 的 listen fd 注册到共享 reactor；peer drain、删除或断连时，移除该 peer 的 listen fd 和还在握手 / OPEN 阶段的 client socket。

`--handshake-threads` 和 JSON `client.handshake_threads` 仍作为兼容配置保留；在当前跨平台 ingress reactor 路径中，它们不会创建 client 握手线程池。

## Server Dial Reactor

Server 模式下，普通 OPEN 不再为每条连接启动短生命周期 dial thread。入站 QUIC stream 收到普通 OPEN 后，会把 DNS 和 TCP connect 流程提交给一个 `TqServerDialReactor` 线程。

`TqServerDialReactor` 负责：

- 接收普通 OPEN 的 dial request。
- 对 literal IP 目标跳过 DNS，直接进入已解析地址过滤。
- 对域名目标调用 `TqAresDnsResolver`，通过 c-ares 异步解析 A/AAAA 地址。
- 对解析结果执行 no-DNS ACL 过滤；speed-test 的受控 loopback 授权可绕过普通 ACL。
- 对候选地址逐个创建 non-blocking TCP socket 并发起 `connect()`。
- 用平台 reactor 监听 connect fd 的 writable/error 事件，并用 deadline 处理 connect timeout。
- 将失败映射成既有 OPEN 错误语义，例如 `DnsFailed`、`AclDenied`、`TcpRefused`、`TcpTimeout`、`Internal`。
- connect 成功后把 connected fd 交回 tunnel 层，由 tunnel 层发送 OPEN response 并启动 relay。

c-ares 作为 Git 子模块 vendored 在 `third_party/c-ares`，不要求系统安装 c-ares dev 包。`TqAresDnsResolver` 不启用 c-ares 内部 event thread；resolver channel、socket readiness 和 timeout 都由所属 reactor 线程驱动。c-ares 全局初始化/清理由进程级 lifecycle guard 管理，确保 channel 生命周期位于 library init/cleanup 范围内。

server dial reactor 还支持 speed-test 使用的 ephemeral authorizer：正常 ACL 不允许的 loopback 目标，如果处于受控的临时授权范围内，可以被精确授权给当前测试会话使用。

完成回调在锁外调用：dial reactor 先在内部锁保护下摘除 pending state，再把 ready completion 移到本地队列并释放锁后调用 `TqServerDialComplete`，避免回调重入时和 Cancel/Stop 互相死锁。

相关代码：

- `src/tunnel/server_dial_reactor.cpp`
- `src/tunnel/server_dial_reactor.h`
- `src/tunnel/ares_dns_resolver.cpp`
- `src/tunnel/ares_dns_resolver.h`
- `src/acl/acl_filter.cpp`
- `src/tunnel/tcp_tunnel.cpp`
- `src/main.cpp`

## 平台 Reactor 边界

控制面通过 `ITqSocketReactor` 抽象 socket readiness 和 wake/run-once 语义：

- **Linux**：`TqLinuxReactor` 使用 epoll / eventfd。
- **Windows**：`TqWindowsReactor` 使用 `WSAEventSelect` / `WSAWaitForMultipleEvents`，并处理 `WSA_MAXIMUM_WAIT_EVENTS` 分片限制。
- **macOS**：`TqDarwinReactor` 使用 `kqueue` / `EVFILT_USER`。

控制面 reactor 只负责 listen、accept、handshake、OPEN、DNS 和 TCP connect。它不接管已经建立隧道后的 TCP/QUIC 数据转发，也不和平台 relay worker 共享数据面 IOCP 队列或 kqueue。

## Server Fallback

当前生产启动路径会创建 `TqServerDialReactor`。代码中仍保留兼容 fallback：如果全局 server dial reactor 不存在，普通 OPEN 会回到旧的 per-OPEN dial worker 路径。这主要用于特殊测试或异常配置场景；在当前 Linux / Windows / macOS server runtime 中，普通 OPEN 主路径都是 server dial reactor。

## Relay Worker 线程

隧道建立后，TCP/QUIC 数据转发进入 relay backend。Relay worker 线程模型没有变化：

- **Linux**：固定数量 `TqLinuxRelayWorker`，使用 epoll/eventfd/readv/writev 分片处理 relay fd。
- **Windows**：固定数量 `TqWindowsRelayWorker`，使用 IOCP 处理 relay I/O。
- **macOS**：固定数量 `TqDarwinRelayWorker`，使用 kqueue 处理 relay fd readiness 和跨线程唤醒。

控制面 reactor 只覆盖低流量的入口握手、OPEN、DNS 和 TCP connect。relay worker 仍是高吞吐数据面，不与 client ingress reactor 或 server dial reactor 合并。Windows relay 数据面仍由 `TqWindowsRelayWorker` worker 组处理，使用 IOCP 和 overlapped I/O；macOS relay 数据面仍由 `TqDarwinRelayWorker` worker 组处理，使用独立 kqueue；控制面 reactor 不接管 relay 数据面。

相关代码：

- `src/tunnel/linux_relay_worker.cpp`
- `src/tunnel/windows_relay_worker.cpp`
- `src/tunnel/darwin_relay_worker.cpp`
- `src/tunnel/darwin_relay_worker.h`
- `src/tunnel/relay.cpp`

## 其它应用线程

除了控制面 reactor 和 relay worker，进程中还可能有这些线程：

- **MsQuic 内部线程**：由 MsQuic 创建，负责 QUIC datapath、worker 和 cleanup 等内部工作；应用 stream callback 运行在 MsQuic worker 上。
- **client reconnect thread**：每个 `QuicClientSession` 启动一个 reconnect loop，用于补齐断开的 QUIC connection slot。
- **admin HTTP 线程**：启用 `--admin-listen` 时，admin server 有 accept 线程，并为 admin client 使用短生命周期处理线程。
- **trace periodic thread**：启用 `--trace` 后周期输出统计快照。
- **tunnel reaper thread**：进程级 `TqTunnelReaper` 单线程统一回收停止后的 tunnel context。
- **speed-test 临时线程**：内置 upload/download speed-test 会创建临时 loopback TCP server 和 pump worker；它不代表常规代理流量路径。

这些线程没有随控制面迁移而消失，观察线程数时需要和 ingress/dial/relay 线程分开看。

## 线程数量估算

常规 client：

```text
1 main thread
+ MsQuic internal threads
+ P * 1 client reconnect thread
+ 1 TqClientIngressReactor worker thread
+ R relay worker threads
+ 1 tunnel reaper thread
+ optional admin / trace / speed-test threads
```

其中 `P` 是 active peer 数。multi-peer 下 `P` 会影响 `QuicClientSession` 和 reconnect thread 数量，但不会再生成 `P * (SOCKS5 listener + HTTP listener + handshake pool)`。

常规 server：

```text
1 main thread
+ MsQuic internal threads
+ 1 TqServerDialReactor worker thread
+ R relay worker threads
+ 1 tunnel reaper thread
+ optional admin / trace / speed-test threads
```

普通 OPEN 数量不会再线性增加 dial worker 线程数。pending DNS 和 pending TCP connect 以 reactor state 的形式挂在 `TqServerDialReactor` 内部。

## 调试时的判断边界

排查一条普通代理连接时，可以按下面的边界理解：

1. client ingress reactor 接受本地 TCP 连接并完成 SOCKS5 / HTTP CONNECT 握手。
2. ingress reactor 发起异步 client OPEN，但 OPEN response 由 MsQuic callback 推进后回投 ingress reactor。
3. OPEN 成功后 ingress reactor 写回代理协议响应，并把 client fd 移交给 relay。
4. server 收到普通 OPEN 后，server dial reactor 完成 ACL、DNS 和 TCP connect。
5. server connect 成功后，tunnel 层发送 OPEN response 并启动 relay。
6. 后续数据面完全由 relay worker 和 MsQuic worker 处理。

Linux / Windows / macOS 上的旧 listener thread / handshake pool / per-OPEN dial worker 路径均不再是主路径；当前三平台 client 和 server 控制面都走跨平台 reactor 模型。
