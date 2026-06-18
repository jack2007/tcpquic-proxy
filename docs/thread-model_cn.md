# 线程模型

本文说明 `tcpquic-proxy` 当前生产实现中的线程来源和职责边界。

## 总览

代理的执行层次分成四层：

1. **监听线程** 负责 SOCKS5 和 HTTP CONNECT 的 `accept()`
2. **握手线程池** 负责单连接的代理协议握手和 tunnel 启动
3. **Server dial worker 线程** 负责 server 侧 OPEN 的 DNS 解析和 TCP connect
4. **Relay worker 线程** 负责 tunnel 建立后的数据转发

整体路径如下：

```text
accept() 线程 -> 握手线程池线程 -> server dial worker 线程 -> relay worker 线程
```

## 监听线程

`TqSocks5Server::Run()` 和 `TqHttpConnectServer::Run()` 各自运行在独立的 `std::thread` 中。
这两个线程只做三件事：

- 调用 `accept()`
- 把拿到的 `clientFd` 交给握手线程池
- 如果任务提交失败，就关闭这个 socket

相关代码：

- `src/ingress/socks5_server.cpp`
- `src/ingress/http_connect_server.cpp`

## 握手线程池

`clientFd` 不会直接进入 relay。
SOCKS5 和 HTTP CONNECT 都会把任务提交到同一个 `TqThreadPool`。

这个线程池负责执行：

- SOCKS5 greeting 和认证
- HTTP CONNECT 请求解析和认证
- `TqTuneTcpForThroughput(clientFd)`
- QUIC 连接选择
- tunnel 启动

这个池是固定大小的，不是“一条连接一个线程”。

要点：

- 单 peer 模式下，SOCKS5 和 HTTP CONNECT 共用同一个 pool 实例。
- multi-peer 模式下，每个 peer runtime 有自己的 pool。
- 同一个 peer 的 SOCKS5 / HTTP CONNECT listener 共享这个 peer 自己的 pool，不和其它 peer 共用。

相关代码：

- `src/runtime/thread_pool.cpp`
- `src/main.cpp`
- `src/ingress/socks5_server.cpp`
- `src/ingress/http_connect_server.cpp`

## Server Dial Worker 线程

server 模式下，入站 QUIC stream 收到普通 OPEN 以后，不会在 `quic_worker` 里直接做 DNS / TCP connect。
当前实现会先完成这些前置步骤：

- 解析 OPEN request
- 执行 ACL 和地址解析
- `std::thread(...).detach()` 启动一个短生命周期的 dial worker

这个 dial worker 负责：

- `TqResolveAllowedTarget()` 里同步执行的 DNS 解析
- `TqDialTcp()` 里的非阻塞 `connect()` + `poll()/WSAPoll()`
- 连接成功后的 `TqTuneTcpForThroughput(targetFd)`
- 发送 OPEN response
- 启动 relay

要点：

- 这不是一个常驻 worker pool，而是 **每条 server OPEN 一个短生命周期线程**
- DNS 解析使用同步 `getaddrinfo()`
- TCP connect 也不是走事件循环状态机，而是在线程里完成非阻塞 connect + 等待

相关代码：

- `src/tunnel/tcp_tunnel.cpp`
- `src/tunnel/tcp_dialer.cpp`

## Relay worker 线程

tunnel 建立后，数据转发进入 relay backend：

- **Linux**：固定数量的 `TqLinuxRelayWorker`
- **Windows**：固定数量的 `TqWindowsRelayWorker`

这些 worker 和 listener 线程、握手线程池是分开的。

相关代码：

- `src/docs/thread_model_cn.md`
- `src/tunnel/linux_relay_worker.cpp`
- `src/tunnel/windows_relay_worker.cpp`

## 实际理解

如果你在追踪一条连接，正常流程是：

1. 监听线程接受 TCP 客户端
2. 握手线程池完成代理协议协商
3. server 侧 OPEN 通过短生命周期 dial worker 完成 DNS / connect
4. relay worker 负责后续隧道流量

这里没有“每条连接一个独立监听线程”。
listener 线程也不负责 relay 的数据转发。
