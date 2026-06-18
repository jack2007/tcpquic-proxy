# 线程模型

本文说明 `tcpquic-proxy` 当前生产实现中的线程来源和职责边界。当前已迁移的是
Linux 控制面；Windows 仍保留旧线程模型，后续单独迁移。Relay 数据面线程模型不在
本轮迁移范围内，保持现状。

## 总览

当前线程模型按平台分为两条路径：

- **Linux client**：所有 peer 的 SOCKS5 / HTTP CONNECT `accept()` 和握手由一个
  `TqClientIngressReactor` 线程处理。
- **Linux server**：普通 OPEN 的 DNS 解析和 TCP connect 由一个
  `TqServerDialReactor` 线程处理，DNS 通过 vendored c-ares 异步解析。
- **Windows**：仍使用旧的 listener thread、handshake pool、per-OPEN dial worker
  模型。
- **Relay worker**：Linux / Windows 均保持既有固定 worker 模型，不随本次控制面迁移改变。

Linux 控制面路径如下：

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

Windows 当前路径仍是：

```text
listener thread -> handshake pool worker -> per-OPEN dial worker -> relay worker
```

## Linux Client Ingress Reactor

Linux client 模式下，进程使用一个 `TqClientIngressReactor` 线程统一处理本地入口。
它覆盖 single-peer 和 multi-peer 两种运行方式：所有 peer 的 SOCKS5 listen fd 和
HTTP CONNECT listen fd 都注册到同一个 reactor，而不是每个 peer 各自启动 listener
线程和 handshake pool。

这个线程负责：

- 监听所有 peer 的 SOCKS5 / HTTP CONNECT socket。
- `accept()` 新的本地 TCP 客户端连接。
- 推进 SOCKS5 greeting、认证、CONNECT request 解析。
- 推进 HTTP CONNECT request 解析和认证。
- 在握手完成后发起异步 client OPEN。
- 在 OPEN 完成后写回 SOCKS5 reply / HTTP 200，并把 socket 交给 relay。
- 处理握手失败、客户端提前关闭、peer 删除和 reactor 停止时的清理。

握手 socket 使用非阻塞状态机推进。实现会避免读过 SOCKS5 / HTTP CONNECT 握手边界；
如果客户端在握手包后紧跟 early data，这些数据必须留在 TCP 接收队列中，等 relay
接管后再转发，避免 ingress reactor 抢读应用数据。

相关代码：

- `src/ingress/client_ingress_reactor.cpp`
- `src/ingress/client_ingress_reactor.h`
- `src/ingress/client_ingress_state.h`
- `src/main.cpp`

## 非 Linux Client 路径

非 Linux client 路径当前没有迁移到 `TqClientIngressReactor`。尤其是 Windows 仍使用
旧模型：

- SOCKS5 和 HTTP CONNECT listener 分别运行在 listener thread 中。
- listener 接受 TCP 连接后提交到 bounded handshake `TqThreadPool`。
- handshake worker 完成 SOCKS5 / HTTP CONNECT 协议协商和 tunnel 启动。

因此不要把 Linux ingress reactor 的线程数量结论外推到 Windows 或其它未迁移平台。

相关代码：

- `src/ingress/socks5_server.cpp`
- `src/ingress/http_connect_server.cpp`
- `src/runtime/thread_pool.cpp`

## Linux Server Dial Reactor

Linux server 模式下，普通 OPEN 不再为每条连接启动短生命周期 dial thread。入站 QUIC
stream 收到普通 OPEN 后，会把 DNS 和 TCP connect 流程提交给一个
`TqServerDialReactor` 线程。

这个线程负责：

- 接收普通 OPEN 的 dial 请求。
- 执行 ACL 前置检查。
- 对域名目标使用 vendored c-ares 异步解析。
- 对 literal IP 或解析结果执行已解析地址过滤。
- 使用 non-blocking socket 推进 TCP connect。
- 连接成功后发送 OPEN response 并启动 relay。
- 失败时映射为既有 OPEN 错误语义，例如 `DnsFailed`、`TcpRefused`、`TcpTimeout`。

c-ares 作为源码依赖 vendored 在 `third_party/c-ares`，不要求系统安装 c-ares dev 包。
reactor 线程持有并驱动 resolver，不引入 DNS 专用线程池。

server dial reactor 还支持 speed-test 使用的 ephemeral authorizer：正常 ACL 不允许的
loopback 目标，如果处于受控的临时授权范围内，可以被精确授权给当前测试会话使用。

相关代码：

- `src/tunnel/server_dial_reactor.cpp`
- `src/tunnel/server_dial_reactor.h`
- `src/tunnel/ares_dns_resolver.cpp`
- `src/tunnel/ares_dns_resolver.h`
- `src/tunnel/tcp_tunnel.cpp`

## Server Fallback 与 Windows 边界

`TqServerDialReactor` 是 Linux server 的当前生产路径，但代码仍保留兼容 fallback：如果
server dial reactor 不存在，普通 OPEN 会回到旧的 per-OPEN dial worker 路径。

Windows 当前仍使用旧 server dial 模型：

- 每条普通 OPEN 启动一个短生命周期 dial worker。
- DNS 解析仍走旧同步路径。
- TCP connect 在线程内完成。

Windows 的 ingress / dial reactor 需要单独设计和迁移，不能假设已经具备 Linux 的线程
收敛特性。

## Relay Worker 线程

tunnel 建立后，数据转发进入 relay backend。Relay worker 线程模型没有变化：

- **Linux**：固定数量的 `TqLinuxRelayWorker`。
- **Windows**：固定数量的 `TqWindowsRelayWorker`。

这些 worker 和 client ingress reactor、server dial reactor 是分开的。控制面 reactor 只负责
握手、OPEN、DNS 和 TCP connect；隧道建立后的 TCP/QUIC 数据转发仍由 relay worker
负责。

相关代码：

- `src/tunnel/linux_relay_worker.cpp`
- `src/tunnel/windows_relay_worker.cpp`

## 实际理解

追踪 Linux 上的一条连接时，可以按下面的边界理解：

1. client 侧 ingress reactor 接受本地 SOCKS5 / HTTP CONNECT 连接并完成握手。
2. client 侧异步发起 OPEN，OPEN 完成后把 TCP socket 交给 relay worker。
3. server 侧收到普通 OPEN 后，server dial reactor 完成 ACL、DNS 和 TCP connect。
4. server 侧 connect 成功后启动 relay worker 转发数据。

追踪 Windows 上的一条连接时，仍应按旧模型理解：

1. listener thread 接受本地 SOCKS5 / HTTP CONNECT 连接。
2. handshake pool worker 完成代理协议握手和 tunnel 启动。
3. server 侧普通 OPEN 通过 per-OPEN dial worker 完成 DNS / connect。
4. relay worker 负责后续隧道流量。

这里没有“所有平台都已迁移到 reactor”的结论；当前完成的是 Linux 控制面迁移。
