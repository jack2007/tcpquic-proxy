# 线程模型重构设计

> 日期：2026-06-18
> 状态：已实现 / Linux 阶段完成
> 决策：Linux client 侧 accept + handshake 已收敛到单一事件线程；Linux server 侧普通 OPEN 的 DNS 解析与 TCP connect 已收敛到单一 dial 线程；DNS 通过 vendored c-ares 做异步解析，不再在 Linux reactor 路径使用同步 `getaddrinfo()`。Windows 当前仍保留旧 listener / handshake pool / per-OPEN dial worker 模型。

## 0. 实现状态与边界

本文最初作为重构设计文档编写。当前 Linux 阶段已经落地，实际实现边界如下：

- Linux client 模式下，所有 peer 的 SOCKS5 / HTTP CONNECT listen socket、`accept()` 和握手状态机由一个 `TqClientIngressReactor` 线程处理。
- Linux server 模式下，普通 OPEN 的 DNS 解析和 TCP connect 由一个 `TqServerDialReactor` 线程处理。
- DNS 使用 vendored `third_party/c-ares`，由 CMake 随源码构建；不依赖系统 c-ares dev 包。
- client ingress reactor 在握手期按 SOCKS5 / HTTP CONNECT 协议边界读取，避免把握手后紧跟的 early data 预读进 reactor 缓冲区；应用数据必须留给 relay worker。
- server dial reactor 支持 speed-test 使用的 ephemeral authorizer，可以在受控范围内精确授权 loopback 目标，即使普通 ACL 不允许该 loopback 地址。
- Windows 尚未迁移，仍使用旧 listener thread、handshake pool 和 per-OPEN dial worker 模型。Windows reactor 仍是后续阶段。
- Relay worker 线程模型保持不变。

## 1. 背景

重构前的线程模型是分层的，但对握手与 dial 这类低吞吐控制路径来说，线程切分过细：

- client 侧 SOCKS5 / HTTP CONNECT 各自有 listener thread。
- 每个 peer 还会持有自己的 handshake pool。
- server 侧普通 OPEN 会为每条连接启动短生命周期 dial thread。

这些路径的流量很小，主要成本不是吞吐，而是状态管理和阻塞点控制。现在的问题不是“线程不够”，而是：

1. listener / handshake / dial 的职责边界比较碎。
2. 线程数量随 peer 数增长。
3. 握手和 dial 阶段仍然混有同步 I/O：
   - client 侧握手阶段依赖阻塞式 `recv()` / `send()`
   - server 侧 DNS 解析使用同步 `getaddrinfo()`
   - server 侧 TCP connect 虽然是非阻塞 connect + poll，但仍被包在每条连接一个线程里

Linux 阶段已经把这些控制面路径改造成异步状态机。Windows 仍保留上述旧模型。

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

Linux client 侧只有一个 ingress reactor 线程。它替代以下旧结构：

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

握手阶段不再依赖阻塞 `recv()` / `send()`。实现还必须维持握手期读取边界：SOCKS5 / HTTP CONNECT 握手后紧跟的 early data 不能被 reactor 预读，必须留在 TCP socket 中等待 relay 接管。

### 5.4 与 QUIC 的交互

client 侧握手完成后，需要发起 tunnel / OPEN。这个动作可能触发后续 QUIC 回调。设计上要求：

- client ingress reactor 是握手状态的唯一 owner
- 来自其他线程的完成事件必须通过队列或唤醒机制回到 reactor
- 不允许其他线程直接修改握手状态对象

这能避免现在那种“握手线程里阻塞等待 QUIC 完成”的模型。

当前 `TqStartClientTunnel()` 会同步等待 OPEN response，这个接口不能直接在 ingress reactor 中调用。重构必须新增异步 tunnel open API，例如：

```cpp
struct TqClientTunnelOpenHandle;

using TqClientTunnelOpenComplete =
    std::function<void(TqClientTunnelOpenHandle*, TqTunnelStartResult)>;

TqClientTunnelOpenHandle* TqStartClientTunnelAsync(
    MsQuicConnection* conn,
    const TunnelRequest& req,
    TqSocketHandle clientTcpFd,
    const TqConfig& cfg,
    TqClientTunnelOpenComplete onComplete);

void TqCancelClientTunnelOpen(TqClientTunnelOpenHandle* handle);
```

异步 API 的要求：

- `TqStartClientTunnelAsync()` 只创建 stream、发送 OPEN request，然后立即返回。
- OPEN response 由 MsQuic stream callback 处理后投递完成事件。
- 完成事件不能直接操作 `ClientIngressState`，必须通过 ingress reactor 的队列回到 reactor 线程。
- SOCKS5 reply / HTTP 200 必须在 reactor 收到完成事件后写回。
- OPEN 失败、客户端提前关闭、peer 断连、进程退出都要通过 `TqCancelClientTunnelOpen()` 取消 pending open。
- 旧的同步 `TqStartClientTunnel()` 第一阶段可以保留给现有调用方，但新 ingress reactor 不允许调用它。

### 5.5 peer 配置变化

多 peer 模式下，配置变更会动态注册或注销 listen socket：

- 新 peer：向 ingress reactor 注册 socks/http listen fd
- 删除 peer：从 ingress reactor 注销 listen fd，并关闭该 peer 相关的活跃握手状态

Linux peer 自身不再携带 listener thread 和 handshake pool。Windows peer 当前仍保留旧结构。

## 6. server 侧设计

### 6.1 线程模型

Linux server 侧只有一个 dial reactor 线程负责所有普通 OPEN 的 dial 流程。

旧的 `TryHandleServerOpen()` 里直接 `std::thread(...).detach()` 的做法在 Linux reactor 路径中已被替换为把请求投递给 dial reactor；reactor 不存在时仍保留旧 fallback。

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

### 6.3 生命周期与取消

server dial state 不能裸捕获 `TqTunnelContext*` 后跨线程长期使用。每条 pending OPEN 必须有明确的生命周期所有权，建议采用：

- `TqTunnelContext` 持有一个 cancellable dial token。
- `ServerDialState` 持有 tunnel 的弱引用或受控 handle，不直接拥有裸指针。
- dial reactor 是 `ServerDialState` 的唯一 owner。
- QUIC stream shutdown、connection abort、tunnel context 析构、进程退出时，通过 token 向 dial reactor 投递 cancel。
- cancel 只在 dial reactor 线程内真正执行，负责取消 c-ares 查询、关闭 pending connect socket、丢弃未发送的 OPEN response。
- dial 完成回调回 tunnel 前必须检查 token 是否仍然有效。

这替代当前 `PendingServerOpen` + detached thread 的隐式生命周期模型，避免 DNS / connect 完成后访问已释放的 tunnel context。

### 6.4 c-ares 接入方式

DNS 异步解析使用 c-ares，但不启用它的内部 event thread。原因是本设计要求 server dial 仍然是 **一个线程所有权**，不能再引入额外的 DNS 工作线程。

建议的接入方式：

1. 进程启动时调用 `ares_library_init()`
2. dial reactor 线程创建并持有一个 `ares_channel`
3. 通过 `ares_init_options()` 配置 socket state callback
4. reactor 用自己的 poll / epoll / IOCP 监视 c-ares 暴露的 socket 状态
5. 优先用 `ares_process_fds()` 和 `ares_timeout()` 驱动 c-ares 查询推进
6. 如果使用的 c-ares 版本低于 1.34.0，再 fallback 到 `ares_process_fd()`

这样 c-ares 负责 DNS 协议状态，线程调度仍然由我们自己的 dial reactor 控制。

线程与回调约束：

- 所有 `ares_channel` 操作都必须在 dial reactor 线程执行，包括 `ares_getaddrinfo()`、`ares_process_fds()` / `ares_process_fd()`、`ares_cancel()`、`ares_destroy()`。
- c-ares callback 只做最小工作：记录结果、标记 state ready、唤醒 reactor；不做 ACL 过滤、TCP connect、复杂日志或跨线程释放。
- callback 中不反向获取 tunnel / reactor 的外部锁，避免和 c-ares channel lock 形成锁顺序问题。
- 关闭时先 cancel pending query，再让 reactor drain callback，最后 destroy channel。

官方文档层面的依据是：

- `ares_getaddrinfo()` 提供异步地址解析接口
- `ARES_OPT_SOCK_STATE_CB` 适合和 poll/epoll/kqueue 之类事件循环集成
- `ares_process_fds()` 可以一次处理多个 fd 事件和超时，优先使用
- `ares_process_fd()` 是单 fd 兼容路径
- `ares_timeout()` 可以提供下一次超时轮询时间

实际依赖：

- c-ares 已 vendored 到 `third_party/c-ares`，由 CMake 随项目源码构建。
- 当前 vendored 版本满足 `ares_process_fds()` 使用要求。
- 构建不要求安装系统 c-ares dev 包。
- `ares_getaddrinfo()` 要求 c-ares 1.16.0 及以上。

### 6.5 TCP connect

TCP connect 继续走非阻塞 socket，但不再由每条 OPEN 启一个线程。
dial reactor 负责：

- 创建 non-blocking socket
- 对解析出的地址逐个尝试 connect
- 用事件驱动或定时器推进连接完成
- 在成功时发送 OPEN response

如果某个地址失败，继续尝试下一地址。
如果所有地址失败，返回 `TcpRefused` / `TcpTimeout` / `Internal` 等既有错误语义。

### 6.6 ACL 与 DNS 的顺序

需要把当前同步的 `TqResolveAllowedTarget()` 拆成可挂起的步骤：

1. 先做与字符串直接相关的校验和 ACL 前置检查
2. 如果是域名，发起 c-ares 异步解析
3. 解析完成后再做地址集合过滤
4. 通过后发起 TCP connect

这一步很关键，因为原实现把 DNS 解析和 ACL 过滤绑在一个同步函数里，无法直接迁移到状态机。

必须新增只过滤已解析地址的 ACL helper，例如：

```cpp
bool TqAclFilterResolvedAddresses(
    const TqAcl& acl,
    const std::vector<sockaddr_storage>& candidates,
    uint16_t port,
    std::vector<sockaddr_storage>& outAddrs);
```

要求：

- helper 不允许调用 `getaddrinfo()` 或任何 DNS API。
- literal IP 路径可以继续直接构造 `sockaddr_storage`，然后走同一个过滤 helper。
- 域名路径必须只使用 c-ares 返回的地址集合。
- 旧 `TqAclResolveAndFilter()` 可以暂时保留给未迁移代码，但 server dial reactor 不能调用它。

### 6.7 Windows 事件循环边界

Linux 第一版使用 `epoll` + wake fd 驱动 ingress / dial reactor。

Windows 第一版需要单独确定事件机制，不能只写成“IOCP”后留给实现猜测。建议分两阶段：

1. Phase 1 先实现 Linux reactor，Windows 保持旧线程模型。
2. Phase 2 为 Windows 增加 `WSAPoll` 或 `WSAEventSelect` 版本的 ingress / dial reactor。

如果后续选择 IOCP，需要明确使用 `AcceptEx`、overlapped connect、c-ares socket callback 到 IOCP 的桥接方式。这个复杂度高于 Linux，不放进第一阶段。当前实现状态是 Linux 已迁移，Windows 未迁移。

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

## 9. Backpressure 与资源上限

单线程 reactor 必须有硬上限，避免慢客户端、慢 DNS 或连接风暴耗尽 fd / 内存。

建议第一版上限：

| 资源 | 建议上限 | 超限行为 |
|------|----------|----------|
| 全局 pending client handshake | 4096 | 新 accept 立即关闭 |
| 单 peer pending client handshake | 512 | 该 peer 新 accept 立即关闭 |
| client handshake header buffer | 16 KiB | SOCKS5/HTTP 协议错误响应后关闭 |
| client handshake 超时 | 10s | 关闭 client socket |
| 全局 pending server dial | 4096 | 发送 `Internal` 或 `TcpTimeout` |
| pending DNS query | 4096 | 拒绝新的域名 OPEN |
| pending connect socket | 4096 | 拒绝新的 connect |
| DNS 超时 | 5s | `DnsFailed` |
| TCP connect 超时 | 10s | `TcpTimeout` |

这些值可以先作为内部常量，后续再决定是否纳入 tuning。超限必须输出 trace / metrics，便于确认是否需要调大。

## 10. 错误映射

建议保持现有 open error 语义不变：

| 场景 | 结果 |
|------|------|
| DNS 找不到 | `DnsFailed` |
| connect refused | `TcpRefused` |
| connect timeout | `TcpTimeout` |
| 其它内部错误 | `Internal` |
| client 握手协议错误 | 现有 SOCKS5 / HTTP 响应码 |

客户端和服务端都不应暴露 c-ares 细节错误码，只保留现有面向协议的错误语义。

## 11. 第三方依赖接入

c-ares 作为新增第三方依赖接入：

- 已 vendoring 到 `third_party/c-ares`，保持可重复构建。
- CMake 中增加静态库构建，并链接到主程序和相关测试。
- 保留 c-ares 原始 license 文件。
- Linux / Windows 都使用同一套 wrapper 接口，平台差异只落在 reactor fd 监听层。
- CI / 本地构建需要显式覆盖“无系统 c-ares 也能构建”的场景。

如果后续改为 FetchContent，也必须保证版本固定，不接受构建时漂移到未知版本。

## 12. 代码结构建议

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
- `acl/acl_filter.h/.cpp` 或在现有 `acl.cpp` 中新增 no-DNS filtering helper

职责：

- 普通 OPEN dial 状态机
- c-ares wrapper
- TCP connect 推进
- 只过滤已解析地址的 ACL helper

### 兼容层

现有 `TqSocks5Server`、`TqHttpConnectServer`、`TryHandleServerOpen()`、`TqDialTcp()` 需要逐步降级成兼容包装，最终只保留协议解析和状态迁移逻辑。

## 13. 迁移策略

### Phase 1：引入状态机骨架（已完成）

- 新增 client ingress reactor
- 新增 server dial reactor
- 新增 `TqStartClientTunnelAsync()`
- 新增 no-DNS ACL filtering helper
- vendoring c-ares，但先可通过 mock resolver 跑通状态机测试
- 保持现有 relay 不变
- client / server 先支持少量路径跑通

### Phase 2：切 Linux client 侧握手（已完成）

- 移除 per-peer handshake pool
- listener thread 合并到 ingress reactor
- 把 SOCKS5 / HTTP CONNECT 握手改成非阻塞状态机
- OPEN response 通过 async completion 回到 ingress reactor

### Phase 3：切 Linux server 侧 dial（已完成）

- 移除 per-open detached dial thread
- 接入 c-ares 异步 DNS
- 把 TCP connect 纳入 dial reactor 状态机
- 明确 cancellation token，覆盖 stream shutdown / connection abort

### Phase 4：清理旧路径（Linux 已切换，兼容 fallback 保留）

- Linux runtime 不再走旧 listener thread / handshake pool 入口
- Linux server reactor 路径不再走同步 DNS
- 更新线程模型文档和测试

### Phase 5：Windows reactor

- 在 Linux 路径稳定后，为 Windows 增加单线程 ingress / dial reactor。
- 选择 `WSAPoll` / `WSAEventSelect` / IOCP 之一，并补充专门设计。
- Windows 未迁移前继续使用旧线程模型，不阻塞 Linux 侧重构。

## 14. 测试策略

### 单元测试

- SOCKS5 状态机：半包、错误包、认证失败、成功路径
- HTTP CONNECT 状态机：头部拆包、认证失败、成功路径
- client async open：OPEN success / fail / cancel 都回到 ingress reactor
- server dial 状态机：DNS 成功 / 失败 / 超时 / connect refused
- c-ares wrapper：通过 mock resolver 验证 callback 和超时推进
- no-DNS ACL helper：输入已解析地址时不触发 DNS

### 集成测试

- 单 peer client：多个 SOCKS5 / HTTP CONNECT 连接共用一个 ingress 线程
- multi-peer client：多个 peer 的 listener 同时工作，但共享同一 ingress reactor
- server OPEN：域名和 IP 两种目标都能完成 dial
- shutdown：销毁 reactor 时不泄漏 state，不重复关闭 socket

### 回归测试

- 慢客户端不应阻塞其它连接
- 慢 DNS 不应阻塞 server 侧其它 dial 状态
- 大量并发连接下线程数应保持稳定，不再随 peer / connection 线性膨胀
- c-ares callback 触发期间不发生锁反转或跨线程释放
- stream shutdown / connection abort 后 pending dial 不访问已释放 tunnel

## 15. 风险

1. 状态机复杂度明显上升，尤其是半包、关闭和超时。
2. c-ares 集成如果处理不当，容易出现 fd 生命周期和超时处理遗漏。
3. 某些原本同步函数必须拆分，否则很难真正做到单线程异步。
4. 需要非常严格地定义“谁拥有 socket / state”，否则容易出现重复关闭或竞态。
5. Windows reactor 和 Linux reactor 的平台差异较大，不能假设一次设计覆盖所有细节。
6. `TqStartClientTunnel()` 同步语义迁移不彻底时，会重新把 ingress reactor 卡住。
7. 握手期 early data 处理必须严格停在协议边界，否则 relay 接管后会丢失或乱序处理应用数据。

## 16. 结论

Linux 阶段已经把 control plane 收敛成两个单线程 reactor：

- Linux client 侧一个 ingress 线程
- Linux server 侧一个 dial 线程

DNS 用 vendored c-ares 做异步解析，relay 保持现有 worker 模型不变。这样满足当前 Linux 阶段目标：线程数收敛、职责清晰、控制面异步化，同时不去碰已经稳定的数据面。Windows 仍使用旧模型，后续单独迁移。
