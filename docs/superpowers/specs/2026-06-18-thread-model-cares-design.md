# 线程模型重构设计

> 日期：2026-06-18  
> 状态：待评审  
> 决策：将 client 侧 accept + handshake 收敛到单一事件线程；将 server 侧普通 OPEN 的 DNS 解析与 TCP connect 收敛到单一 dial 线程；DNS 通过 c-ares 做异步解析，不再使用同步 `getaddrinfo()`。

## 1. 背景

当前实现的线程模型是分层的，但对握手与 dial 这类低吞吐控制路径来说，线程切分过细：

- client 侧 SOCKS5 / HTTP CONNECT 各自有 listener thread。
- 每个 peer 还会持有自己的 handshake pool。
- server 侧普通 OPEN 会为每条连接启动短生命周期 dial thread。

这些路径的流量很小，主要成本不是吞吐，而是状态管理和阻塞点控制。现在的问题不是“线程不够”，而是：

1. listener / handshake / dial 的职责边界比较碎。
2. 线程数量随 peer 数增长。
3. 握手和 dial 阶段仍然混有同步 I/O：
   - client 侧握手阶段当前仍依赖阻塞式 `recv()` / `send()`
   - server 侧 DNS 解析当前使用同步 `getaddrinfo()`
   - server 侧 TCP connect 当前虽然是非阻塞 connect + poll，但仍被包在每条连接一个线程里

本设计希望把这些控制面路径改造成真正的异步状态机。

## 2. 目标

1. client 侧所有 peer 的 `accept()` 和握手处理收敛到 **一个线程**。
2. server 侧普通 OPEN 的 DNS 解析和 TCP connect 收敛到 **一个线程**。
3. DNS 解析改为异步实现，不再阻塞 dial 线程。
4. relay worker 维持现有 Linux / Windows 数据面模型，不在本次重构中改造。
5. 对外行为不变：SOCKS5、HTTP CONNECT、server OPEN 的协议语义保持一致。

## 3. 非目标

- 不重写 relay backend。
- 不改变 QUIC connection 管理策略。
- 不改变 trace / metrics 的字段语义，最多补充少量状态追踪。
- 不引入额外的 DNS 专用线程池。
- 不把 client 侧握手再拆回“每连接线程”。

## 4. 总体方案

### 4.1 client 侧：单一 ingress reactor

client 侧引入一个进程级的 ingress reactor 线程，负责：

- 所有 peer 的 SOCKS5 listen socket
- 所有 peer 的 HTTP CONNECT listen socket
- 所有 accepted client socket
- SOCKS5 / HTTP CONNECT 的握手状态推进
- 关闭、超时、半包、写回响应
- 把已完成的 tunnel 交给 relay 层

这个 reactor 线程是控制面入口。它不是“每 peer 一份”，而是 **所有 peer 共用一份**。

### 4.2 server 侧：单一 dial reactor

server 侧引入一个进程级的 dial reactor 线程，负责：

- 接收普通 OPEN 的 dial 请求
- 使用 c-ares 异步解析域名
- 执行 TCP connect
- 发送 OPEN response
- 在 connect 成功后启动 relay

这里不再为每条 OPEN 创建短生命周期线程。

### 4.3 relay 维持现状

relay worker 仍然使用现有平台模型：

- Linux：固定数量 `TqLinuxRelayWorker`
- Windows：固定数量 `TqWindowsRelayWorker`

本设计只改控制面，不改数据面。

## 5. client 侧设计

### 5.1 线程模型

client 侧只有一个 ingress reactor 线程。它替代以下现有结构：

- `TqSocks5Server::Run()` 的独立 listener thread
- `TqHttpConnectServer::Run()` 的独立 listener thread
- 每个 peer 的 handshake `TqThreadPool`

所有 peer 的 listen socket 都注册到同一个 reactor。peer 只是配置和状态归属，不再对应独立的握手线程池。

### 5.2 状态机

每个 accepted client 连接维护一个轻量状态对象，例如 `ClientIngressState`。状态推进是非阻塞的：

- `ACCEPTED`
- `READ_GREETING`
- `READ_REQUEST`
- `AUTH_CHECK`
- `WAIT_TUNNEL_START`
- `WAIT_OPEN_RESPONSE`
- `READY_FOR_RELAY`
- `CLOSED`

SOCKS5 和 HTTP CONNECT 可以共享同一个状态机框架，只是在解析和响应格式上分支。

### 5.3 I/O 模型

client socket 改成 non-blocking。reactor 负责：

- 可读事件：尽量读入缓冲区，解析状态机
- 可写事件：尽量刷出待发送响应
- 超时事件：关闭长时间未完成握手的连接

握手阶段不再依赖阻塞 `recv()` / `send()`。

### 5.4 与 QUIC 的交互

client 侧握手完成后，需要发起 tunnel / OPEN。这个动作可能触发后续 QUIC 回调。设计上要求：

- client ingress reactor 是握手状态的唯一 owner
- 来自其他线程的完成事件必须通过队列或唤醒机制回到 reactor
- 不允许其他线程直接修改握手状态对象

这能避免现在那种“握手线程里阻塞等待 QUIC 完成”的模型。

### 5.5 peer 配置变化

多 peer 模式下，配置变更会动态注册或注销 listen socket：

- 新 peer：向 ingress reactor 注册 socks/http listen fd
- 删除 peer：从 ingress reactor 注销 listen fd，并关闭该 peer 相关的活跃握手状态

peer 自身不再携带 listener thread 和 handshake pool。

## 6. server 侧设计

### 6.1 线程模型

server 侧只有一个 dial reactor 线程负责所有普通 OPEN 的 dial 流程。

当前 `TryHandleServerOpen()` 里直接 `std::thread(...).detach()` 的做法会被移除，改成把请求投递给 dial reactor。

### 6.2 状态机

每条普通 OPEN 维护一个 `ServerDialState`，状态推进如下：

- `OPEN_RECEIVED`
- `ACL_CHECKED`
- `DNS_PENDING`
- `DNS_DONE`
- `CONNECT_PENDING`
- `CONNECTED`
- `RESP_SENT`
- `RELAY_STARTED`
- `FAILED`

对于 literal IP 地址，`DNS_PENDING` 可以直接跳过。

### 6.3 c-ares 接入方式

DNS 异步解析使用 c-ares，但不启用它的内部 event thread。原因是本设计要求 server dial 仍然是 **一个线程所有权**，不能再引入额外的 DNS 工作线程。

建议的接入方式：

1. 进程启动时调用 `ares_library_init()`
2. dial reactor 线程创建并持有一个 `ares_channel`
3. 通过 `ares_init_options()` 配置 socket state callback
4. reactor 用自己的 poll / epoll / IOCP 监视 c-ares 暴露的 socket 状态
5. 用 `ares_process_fd()` 和 `ares_timeout()` 驱动 c-ares 查询推进

这样 c-ares 负责 DNS 协议状态，线程调度仍然由我们自己的 dial reactor 控制。

官方文档层面的依据是：

- `ares_getaddrinfo()` 提供异步地址解析接口
- `ARES_OPT_SOCK_STATE_CB` 适合和 poll/epoll/kqueue 之类事件循环集成
- `ares_process_fd()` 可以处理 I/O 事件和超时
- `ares_timeout()` 可以提供下一次超时轮询时间

### 6.4 TCP connect

TCP connect 继续走非阻塞 socket，但不再由每条 OPEN 启一个线程。
dial reactor 负责：

- 创建 non-blocking socket
- 对解析出的地址逐个尝试 connect
- 用事件驱动或定时器推进连接完成
- 在成功时发送 OPEN response

如果某个地址失败，继续尝试下一地址。
如果所有地址失败，返回 `TcpRefused` / `TcpTimeout` / `Internal` 等既有错误语义。

### 6.5 ACL 与 DNS 的顺序

需要把当前同步的 `TqResolveAllowedTarget()` 拆成可挂起的步骤：

1. 先做与字符串直接相关的校验和 ACL 前置检查
2. 如果是域名，发起 c-ares 异步解析
3. 解析完成后再做地址集合过滤
4. 通过后发起 TCP connect

这一步很关键，因为原实现把 DNS 解析和 ACL 过滤绑在一个同步函数里，无法直接迁移到状态机。

## 7. 数据流

### 7.1 client 侧

```text
listen fd ready
  -> ingress reactor accept
  -> connection state machine parse greeting / header
  -> optional auth / parse target
  -> request tunnel start
  -> wait for open completion event
  -> hand off to relay
```

### 7.2 server 侧

```text
QUIC stream receive
  -> parse OPEN
  -> enqueue dial state
  -> dial reactor
      -> ACL precheck
      -> c-ares async DNS
      -> TCP connect
      -> send OPEN response
      -> start relay
```

## 8. 关闭与超时

### 8.1 client 侧

需要统一处理以下关闭场景：

- 客户端提前断开
- 握手超时
- auth 失败
- 请求格式错误
- tunnel start 失败

关闭必须由 ingress reactor 统一回收状态对象，避免跨线程重复关闭 socket。

### 8.2 server 侧

需要处理：

- DNS 超时
- DNS 失败
- connect 超时
- connect refused
- 进程退出时取消所有未完成的 dial 状态

c-ares 关闭时要先取消未完成请求，再销毁 channel，最后调用 `ares_library_cleanup()`。

## 9. 错误映射

建议保持现有 open error 语义不变：

| 场景 | 结果 |
|------|------|
| DNS 找不到 | `DnsFailed` |
| connect refused | `TcpRefused` |
| connect timeout | `TcpTimeout` |
| 其它内部错误 | `Internal` |
| client 握手协议错误 | 现有 SOCKS5 / HTTP 响应码 |

客户端和服务端都不应暴露 c-ares 细节错误码，只保留现有面向协议的错误语义。

## 10. 代码结构建议

建议新增或重构为以下模块：

### client ingress

- `ingress/client_ingress_reactor.h/.cpp`
- `ingress/client_ingress_state.h`

职责：

- 多 peer listen fd 注册
- accepted socket 状态机
- handshake 完成与 tunnel 提交

### server dial

- `tunnel/server_dial_reactor.h/.cpp`
- `tunnel/server_dial_state.h`
- `tunnel/ares_dns_resolver.h/.cpp`

职责：

- 普通 OPEN dial 状态机
- c-ares wrapper
- TCP connect 推进

### 兼容层

现有 `TqSocks5Server`、`TqHttpConnectServer`、`TryHandleServerOpen()`、`TqDialTcp()` 需要逐步降级成兼容包装，最终只保留协议解析和状态迁移逻辑。

## 11. 迁移策略

### Phase 1：引入状态机骨架

- 新增 client ingress reactor
- 新增 server dial reactor
- 保持现有 relay 不变
- client / server 先支持少量路径跑通

### Phase 2：切 client 侧握手

- 移除 per-peer handshake pool
- listener thread 合并到 ingress reactor
- 把 SOCKS5 / HTTP CONNECT 握手改成非阻塞状态机

### Phase 3：切 server 侧 dial

- 移除 per-open detached dial thread
- 接入 c-ares 异步 DNS
- 把 TCP connect 纳入 dial reactor 状态机

### Phase 4：清理旧路径

- 删除旧 listener thread / handshake pool 代码
- 删除同步 DNS 代码路径
- 更新线程模型文档和测试

## 12. 测试策略

### 单元测试

- SOCKS5 状态机：半包、错误包、认证失败、成功路径
- HTTP CONNECT 状态机：头部拆包、认证失败、成功路径
- server dial 状态机：DNS 成功 / 失败 / 超时 / connect refused
- c-ares wrapper：通过 mock resolver 验证 callback 和超时推进

### 集成测试

- 单 peer client：多个 SOCKS5 / HTTP CONNECT 连接共用一个 ingress 线程
- multi-peer client：多个 peer 的 listener 同时工作，但共享同一 ingress reactor
- server OPEN：域名和 IP 两种目标都能完成 dial
- shutdown：销毁 reactor 时不泄漏 state，不重复关闭 socket

### 回归测试

- 慢客户端不应阻塞其它连接
- 慢 DNS 不应阻塞 server 侧其它 dial 状态
- 大量并发连接下线程数应保持稳定，不再随 peer / connection 线性膨胀

## 13. 风险

1. 状态机复杂度明显上升，尤其是半包、关闭和超时。
2. c-ares 集成如果处理不当，容易出现 fd 生命周期和超时处理遗漏。
3. 某些原本同步函数必须拆分，否则很难真正做到单线程异步。
4. 需要非常严格地定义“谁拥有 socket / state”，否则容易出现重复关闭或竞态。

## 14. 结论

这个方案把 control plane 收敛成两个单线程 reactor：

- client 侧一个 ingress 线程
- server 侧一个 dial 线程

DNS 用 c-ares 做异步解析，relay 保持现有 worker 模型不变。这样能满足当前目标：线程数收敛、职责清晰、控制面完全异步化，同时不去碰已经稳定的数据面。
