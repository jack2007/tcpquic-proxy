# 线程模型

本文说明 `tcpquic-proxy` 当前生产实现中的线程来源和职责边界。控制面已经收敛为跨平台 socket reactor 模型：client 侧本地入口由 ingress reactor 驱动，server 侧普通 OPEN 的 DNS / TCP connect 由 dial reactor 驱动。Relay 数据面线程模型不随本次控制面迁移改变，继续保持各平台既有 worker 模型。

## 总览

当前线程模型按职责划分为两层：

- **Client ingress 控制面**：所有 peer 的 SOCKS5 / HTTP CONNECT `listen`、`accept` 和握手状态机由一个 `TqClientIngressReactor` 线程处理。Linux 底层使用 `TqLinuxReactor`，Windows 底层使用 `TqWindowsReactor`。
- **Server dial 控制面**：普通 OPEN 的 ACL、DNS 解析和 TCP connect 由一个 `TqServerDialReactor` 线程处理。DNS 使用 vendored c-ares 异步解析，并由同一个 reactor 线程驱动 c-ares sockets 和 timeout。
- **Relay worker 数据面**：Linux / Windows 均保持既有固定 worker 模型；Windows relay 仍由 `TqWindowsRelayWorker` 使用 IOCP 和 overlapped I/O 处理高吞吐 TCP/QUIC 转发。

控制面路径如下：

```text
client:
  ingress reactor 线程
    -> accept SOCKS5 / HTTP CONNECT
    -> 非阻塞握手状态机
    -> async client OPEN
    -> relay worker

server:
  QUIC stream OPEN
    -> server dial reactor 线程
    -> ACL / c-ares async DNS / non-blocking TCP connect
    -> relay worker
```

Windows 旧路径已经被替换：client 不再为每个 peer 创建独立 listener thread 和 bounded handshake `TqThreadPool`；server 普通 OPEN 不再为每条连接创建 detached dial worker。旧 fallback 只作为未配置 reactor 时的兼容路径存在，不是当前 Windows runtime 的主路径。

## Client Ingress Reactor

Client 模式下，进程使用一个 `TqClientIngressReactor` 线程统一处理本地入口。它覆盖 single-peer 和 multi-peer 两种运行方式：所有 peer 的 SOCKS5 listen socket 和 HTTP CONNECT listen socket 都注册到同一个 ingress reactor，而不是每个 peer 各自启动 listener 线程和 handshake pool。

这个线程负责：

- 监听所有 peer 的 SOCKS5 / HTTP CONNECT socket。
- `accept()` 新的本地 TCP 客户端连接。
- 推进 SOCKS5 greeting、认证、CONNECT request 解析。
- 推进 HTTP CONNECT request 解析和认证。
- 在握手完成后发起异步 client OPEN。
- 在 OPEN 完成后写回 SOCKS5 reply / HTTP 200，并把 socket 交给 relay。
- 处理握手失败、客户端提前关闭、peer 删除和 reactor 停止时的清理。

握手 socket 使用非阻塞状态机推进。实现会避免读过 SOCKS5 / HTTP CONNECT 握手边界；如果客户端在握手包后紧跟 early data，这些数据必须留在 TCP 接收队列中，等 relay 接管后再转发，避免 ingress reactor 抢读应用数据。

Windows client ingress 与 Linux 使用同一套业务状态机，但由 `TqWindowsReactor` 通过 Winsock event 驱动 listen / handshake socket。OPEN 完成回调不会直接跨线程修改 ingress state，而是投递回 ingress reactor 线程；异步认证和 OPEN lifetime 都由 guard/token 保护，避免 Stop、RemovePeer 或关闭连接后的迟到回调触达已释放状态。

相关代码：

- `src/ingress/client_ingress_reactor.cpp`
- `src/ingress/client_ingress_reactor.h`
- `src/ingress/client_ingress_state.h`
- `src/runtime/listen_socket.cpp`
- `src/runtime/listen_socket.h`
- `src/runtime/scoped_socket.h`
- `src/main.cpp`

## Server Dial Reactor

Server 模式下，普通 OPEN 不再为每条连接启动短生命周期 dial thread。入站 QUIC stream 收到普通 OPEN 后，会把 DNS 和 TCP connect 流程提交给一个 `TqServerDialReactor` 线程。

这个线程负责：

- 接收普通 OPEN 的 dial 请求。
- 执行 ACL 前置检查。
- 对域名目标使用 vendored c-ares 异步解析。
- 对 literal IP 或解析结果执行已解析地址过滤。
- 使用 non-blocking socket 推进 TCP connect。
- 连接成功后发送 OPEN response 并启动 relay。
- 失败时映射为既有 OPEN 错误语义，例如 `DnsFailed`、`TcpRefused`、`TcpTimeout`。

c-ares 作为 Git 子模块 vendored 在 `third_party/c-ares`，不要求系统安装 c-ares dev 包。`TqAresDnsResolver` 不启用 c-ares 内部 event thread；resolver channel、socket readiness 和 timeout 都由所属 reactor 线程驱动。c-ares 全局初始化/清理由进程级 lifecycle guard 管理，确保 channel 生命周期位于 library init/cleanup 范围内。

server dial reactor 还支持 speed-test 使用的 ephemeral authorizer：正常 ACL 不允许的 loopback 目标，如果处于受控的临时授权范围内，可以被精确授权给当前测试会话使用。

完成回调在锁外调用：dial reactor 先在内部锁保护下摘除 pending state，再把 ready completion 移到本地队列并释放锁后调用 `TqServerDialComplete`，避免回调重入时和 Cancel/Stop 互相死锁。

相关代码：

- `src/tunnel/server_dial_reactor.cpp`
- `src/tunnel/server_dial_reactor.h`
- `src/tunnel/ares_dns_resolver.cpp`
- `src/tunnel/ares_dns_resolver.h`
- `src/tunnel/tcp_tunnel.cpp`
- `src/main.cpp`

## 平台 Reactor 边界

控制面通过 `ITqSocketReactor` 抽象 socket readiness 和 wake/run-once 语义：

- **Linux**：`TqLinuxReactor` 使用 epoll / eventfd。
- **Windows**：`TqWindowsReactor` 使用 `WSAEventSelect` / `WSAWaitForMultipleEvents`，并处理 `WSA_MAXIMUM_WAIT_EVENTS` 分片限制。

控制面 reactor 只负责 listen、accept、handshake、OPEN、DNS 和 TCP connect。它不接管已经建立隧道后的 TCP/QUIC 数据转发，也不和 Windows relay worker 共享 IOCP 队列。

## Relay Worker 线程

tunnel 建立后，数据转发进入 relay backend。Relay worker 线程模型没有变化：

- **Linux**：固定数量的 `TqLinuxRelayWorker`。
- **Windows**：固定数量的 `TqWindowsRelayWorker`。

这些 worker 和 client ingress reactor、server dial reactor 是分开的。Windows relay 数据面仍由 `TqWindowsRelayWorker` worker 组处理，使用 IOCP 和 overlapped I/O；控制面 reactor 不接管 relay 数据面。

相关代码：

- `src/tunnel/linux_relay_worker.cpp`
- `src/tunnel/windows_relay_worker.cpp`

## 实际理解

追踪一条连接时，可以按下面的边界理解：

1. client 侧 ingress reactor 接受本地 SOCKS5 / HTTP CONNECT 连接并完成握手。
2. client 侧异步发起 OPEN，OPEN 完成后把 TCP socket 交给 relay worker。
3. server 侧收到普通 OPEN 后，server dial reactor 完成 ACL、DNS 和 TCP connect。
4. server 侧 connect 成功后启动 relay worker 转发数据。

Windows 上的旧 listener thread / handshake pool / per-OPEN dial worker 路径已经不再是主路径；当前 Windows client 和 server 控制面与 Linux 一样走跨平台 reactor 模型。
