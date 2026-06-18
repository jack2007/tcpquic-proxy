# Windows 线程异步改造设计方案

> 日期：2026-06-19  
> 状态：已实现 / 待 Task 10 全量验证  
> 参考：`docs/superpowers/specs/2026-06-18-thread-model-cares-design.md`、`docs/superpowers/plans/2026-06-18-async-reactor-thread-model.md`、已落地 Linux `TqClientIngressReactor` / `TqServerDialReactor` / `TqAresDnsResolver` 实现。

## 1. 实现状态

Task 1-8 已完成 Windows async reactor threading refactor 的代码迁移：

- `ITqSocketReactor` 已成为跨平台控制面 reactor 抽象；Linux 使用 `TqLinuxReactor`，Windows 使用 `TqWindowsReactor`。
- Windows client runtime 已切到 `TqClientIngressReactor`，single-peer 与 multi-peer 的 SOCKS5 / HTTP CONNECT listen、accept、握手状态机由单一 ingress reactor 线程处理。
- `TqAresDnsResolver` 已平台无关化，vendored c-ares 的 socket readiness 和 timeout 由所属 reactor 驱动，不启用 c-ares 内部 event thread。
- `TqServerDialReactor` 已平台无关化，Windows server 普通 OPEN 由单一 dial reactor 线程处理 DNS 与 non-blocking TCP connect。
- Windows relay worker 仍保持 `TqWindowsRelayWorker` IOCP data plane；控制面 reactor 不接管 relay 数据面，也不与其共享 IOCP 队列。
- Windows legacy listener thread / handshake pool / per-OPEN detached dial worker 路径已从当前 client/server runtime 主路径替换；fallback 仅作为未设置 reactor 时的兼容路径存在。

关键验证覆盖：

- Windows reactor 覆盖 wake、read readiness、write/connect readiness、remove suppress dispatch、超过 `WSA_MAXIMUM_WAIT_EVENTS` 的分片等待。
- client ingress reactor 覆盖 SOCKS5 / HTTP CONNECT handshake、early data 边界、取消和异步 OPEN 完成路径。
- c-ares resolver 覆盖 localhost resolve、cancel suppress callback、timeout selection，并在 Windows 构建中启用。
- server dial reactor 覆盖 literal IP、DNS 失败、connect refused、cancel、destructor closes pending fd，以及 Windows server OPEN 经 dial reactor 的 worker smoke 路径。
- Windows relay worker 测试保持作为 IOCP 数据面回归覆盖。

实现过程中固化的约束：

1. **c-ares global lifecycle guard**：resolver channel 初始化前必须持有进程级 c-ares library 引用；channel cancel/destroy 和 reactor stop 完成后才能释放，避免 Windows/Linux 多 resolver 生命周期交错时提前 cleanup。
2. **server dial completion 锁外调用**：`TqServerDialReactor` 在内部锁下只摘除 pending state 并收集 ready completion，释放锁后再调用 `TqServerDialComplete`，避免完成回调重入 Cancel/Stop 或 tunnel 状态时死锁。
3. **async ingress auth/lifetime guard**：Windows client ingress 的 async OPEN / auth 完成必须通过 reactor task queue 回到 ingress reactor 线程，并由 operation token / lifetime guard 抑制 Stop、RemovePeer、Close 后的迟到回调。

## 2. 背景

Linux 阶段已经把控制面线程模型改造成两个固定事件线程：

- client 侧所有 peer 的 SOCKS5 / HTTP CONNECT listen、accept、握手状态机收敛到一个 `TqClientIngressReactor` 线程。
- server 侧普通 OPEN 的 DNS 解析和 TCP connect 收敛到一个 `TqServerDialReactor` 线程。
- DNS 通过 vendored `third_party/c-ares` 异步解析，由 reactor 驱动，不启用 c-ares 内部 event thread。
- relay worker 数据面保持原平台模型。

改造前 Windows 保留旧模型：

- client 侧每个 peer 创建 `TqThreadPool`，并由 `TqSocks5Server` / `TqHttpConnectServer` 各自维护 listener thread。
- SOCKS5 / HTTP CONNECT 握手仍使用阻塞式 `recv()` / `send()`，再同步调用 `TqStartClientTunnel()` 等待 OPEN response。
- server 侧 `tcp_tunnel.cpp` 在非 Linux 路径不会使用 `TqServerDialReactor`，普通 OPEN 仍走同步解析 / dial 的 legacy 路径。
- Windows relay 数据面已经有 `TqWindowsRelayWorker`，使用 IOCP 处理 TCP overlapped I/O 和 QUIC 回调投递，本次不改变它的职责。

本设计把 Linux 已验证的“控制面单 reactor 化”迁移到 Windows，但不直接把 Linux `epoll` / `eventfd` 代码用条件编译扩展到 Windows，而是抽象出跨平台 reactor 接口，再用 Windows 实现承载 client ingress 与 server dial。

## 3. 目标

1. Windows client 侧所有 peer 的 accept + SOCKS5 / HTTP CONNECT handshake 收敛到一个 ingress reactor 线程。
2. Windows server 侧普通 OPEN 的 DNS + TCP connect 收敛到一个 dial reactor 线程。
3. Windows DNS 使用同一个 vendored c-ares wrapper，不引入系统 c-ares 依赖，也不启用 c-ares 内部线程。
4. 对外协议行为保持不变：SOCKS5、HTTP CONNECT、OPEN response、ACL、speed-test ephemeral authorizer 语义不变。
5. Windows relay 数据面 `TqWindowsRelayWorker` 保持现状，不并入控制面 reactor。
6. 保持 Linux 已落地行为不回退，Windows 改造通过可测试的小步迁移完成。

## 4. 非目标

- 不重写 `TqWindowsRelayWorker` 的 IOCP relay 数据面。
- 不改变 QUIC session 的连接管理、重连、peer 选择策略。
- 不改变控制协议 wire format。
- 不把 Windows 控制面和 relay worker 合并到同一个 IOCP 线程。
- 不在第一阶段优化 Windows speed-test 统计边界；已知 FINISH 边界竞态继续按现有决策处理。

## 5. 总体架构

### 5.1 reactor 抽象

新增轻量跨平台 reactor 接口，供控制面使用：

```cpp
namespace TqReactorEvents {
constexpr uint32_t Read = 0x1;
constexpr uint32_t Write = 0x2;
constexpr uint32_t Error = 0x4;
}

class ITqSocketReactor {
public:
    using Handler = std::function<void(TqSocketHandle fd, uint32_t events)>;

    virtual ~ITqSocketReactor() = default;
    virtual bool Start() = 0;
    virtual void Stop() = 0;
    virtual bool Add(TqSocketHandle fd, uint32_t events, Handler handler) = 0;
    virtual bool Modify(TqSocketHandle fd, uint32_t events) = 0;
    virtual bool Remove(TqSocketHandle fd) = 0;
    virtual bool Wake() = 0;
    virtual bool RunOnce(int timeoutMs) = 0;
};
```

Linux `TqLinuxReactor` 适配这个接口；Windows 新增 `TqWindowsReactor`。

### 5.2 Windows reactor 实现选择

Windows 控制面 reactor 推荐采用 `WSAEventSelect` + `WSAWaitForMultipleEvents` 分片模型，而不是直接复用 relay worker IOCP：

- 控制面 socket 数量是 listen socket、handshake socket、c-ares sockets、pending connect sockets，吞吐要求低于 relay 数据面。
- `WSAEventSelect` 可自然覆盖 accept/read/write/connect/close 事件，也适合 c-ares `sock_state_cb` 暴露的 socket readiness。
- 一个 Windows wait set 最多 `WSA_MAXIMUM_WAIT_EVENTS`，因此实现需要分片轮询，保留一个 wake event。
- relay worker 的 IOCP 已承担高吞吐 TCP relay，控制面不应与其共享队列和生命周期，避免职责耦合。

`TqWindowsReactor` 负责：

- 为每个 socket 创建 `WSAEVENT`。
- 调用 `WSAEventSelect(fd, event, FD_READ | FD_WRITE | FD_ACCEPT | FD_CONNECT | FD_CLOSE)` 注册网络事件。
- 在 `RunOnce()` 中分片调用 `WSAWaitForMultipleEvents()`。
- 对 ready event 调用 `WSAEnumNetworkEvents()`，映射为 `Read` / `Write` / `Error`。
- 用 `WSACreateEvent()` 作为 wake event，`Wake()` 调用 `WSASetEvent()`。
- `Stop()` 和 `Remove()` 必须清理 event 并解除 socket event select。

### 5.3 client ingress 迁移

现有 Linux `TqClientIngressReactor` 的业务逻辑可复用，但需要把 Linux-only socket 操作抽到平台层：

- `TqCreateNonBlockingListenSocket()`：改成跨平台 helper。
- `TqSetNonBlockingCloseOnExec()`：Linux 用 `fcntl`，Windows 用 `ioctlsocket(FIONBIO)`。
- `TqScopedFd`：改成 `TqScopedSocket`，使用 `TqSocketHandle`。
- `TqLinuxReactor` 成员替换为 `std::unique_ptr<ITqSocketReactor>` 或按平台 typedef 的 `TqPlatformReactor`。

Windows client runtime 不再创建每 peer `TqThreadPool`、`TqSocks5Server`、`TqHttpConnectServer`。它和 Linux 一样：

- 单 peer runtime 持有一个 `TqClientIngressReactor`。
- 多 peer runtime 持有一个全局 `TqClientIngressReactor`。
- `StartTunnel` 改用 `TqStartClientTunnelAsync()`。
- OPEN 完成回调必须通过 reactor task queue 回到 ingress reactor 线程，再写 SOCKS5 / HTTP CONNECT 响应。
- 握手读边界继续使用 1 字节推进，避免把 early data 预读进控制面缓冲区。

### 5.4 server dial 迁移

`TqServerDialReactor` 的核心状态机应迁移为跨平台：

- DNS：继续使用 `TqAresDnsResolver`。
- TCP connect：使用非阻塞 connect；Windows 下 `connect()` 返回 `WSAEWOULDBLOCK` / `WSAEINPROGRESS` / `WSAEINVAL` 视为 pending，由 reactor 监听 write/connect/error。
- socket error：Windows 用 `getsockopt(SOL_SOCKET, SO_ERROR)`，错误分类使用 `WSAECONNREFUSED`、`WSAETIMEDOUT`、`WSAEHOSTUNREACH`、`WSAENETUNREACH` 等。
- cancel：仍由 token 驱动，只在 dial reactor 线程内关闭 pending socket、取消 DNS、丢弃回调。

`tcp_tunnel.cpp` 中 `TqGetServerDialReactor()` 当前只在 `__linux__` 返回 reactor。迁移后应允许 Windows 设置 reactor：

- include `server_dial_reactor.h` 不再 Linux-only。
- `TqSetServerDialReactor()` 可在 Windows server runtime 初始化时调用。
- fallback legacy detached path 保留一段时间，只有未设置 reactor 时使用。

### 5.5 c-ares Windows 适配

`TqAresDnsResolver` 当前依赖 `TqLinuxReactor` 和 Unix fd。迁移后应改为：

- 内部持有平台 reactor。
- `ares_socket_t` 在 Windows 下是 Winsock socket，可转为 `TqSocketHandle` 注册。
- `SockStateCallback()` 根据 readable / writable 调用 reactor `Add` / `Modify` / `Remove`。
- `ProcessFd()` 继续调用 `ares_process_fds()`。
- `NextWaitMs()` 继续使用 `ares_timeout()`。
- 所有 c-ares channel 操作仍限定在 resolver 所属 reactor 线程。

### 5.6 生命周期和线程所有权

- `TqClientIngressReactor` 是 client handshake state 的唯一 owner。
- `TqServerDialReactor` 是 server dial state 的唯一 owner。
- QUIC 回调不得直接修改 ingress / dial state，只能投递到对应 reactor。
- `TqWindowsRelayWorker` 仍是 relay state 的 owner。控制面只在 OPEN 成功后把 socket + stream handoff 给 relay runtime。
- `Stop()` 顺序：停止接收新 peer/listen → cancel pending handshake/open/dial → drain reactor task queue → stop reactor thread → stop QUIC / relay runtime。

## 6. 分阶段落地

### Phase 0：平台 reactor 基础

- 新增 `ITqSocketReactor`。
- 让 Linux `TqLinuxReactor` 实现接口，保持现有 Linux 测试通过。
- 新增 Windows `TqWindowsReactor` 和单元测试。

### Phase 1：Windows client ingress reactor

- 抽出跨平台 listen socket / scoped socket helper。
- 让 `TqClientIngressReactor` 使用平台 reactor。
- Windows runtime 切到与 Linux 相同的 async ingress 路径。
- 保留旧 `TqSocks5Server` / `TqHttpConnectServer` 作为 fallback，直到集成测试稳定。

### Phase 2：Windows c-ares resolver

- 改造 `TqAresDnsResolver` 使用平台 reactor。
- Windows 启用 `tcpquic_ares_dns_resolver_test`。
- 确保 c-ares CMake 在 Windows 下只构建静态库，且不污染现有 target。

### Phase 3：Windows server dial reactor

- 让 `TqServerDialReactor` 编译运行于 Windows。
- Windows server runtime 创建并设置全局 dial reactor。
- 普通 OPEN 走 reactor；fallback legacy detached path 保留。
- Windows 启用 `server_dial_reactor` 单元测试和 worker smoke 测试。

### Phase 4：清理与文档

- 删除 Windows client runtime 对 per-peer handshake pool 的依赖。
- 文档更新 Windows 新线程模型。
- 验证 Windows/Linux 构建和核心测试。

## 7. 测试策略

单元测试：

- `tcpquic_windows_reactor_test`：wake、read readiness、write/connect readiness、remove 后不再派发、分片超过 64 events。
- `tcpquic_client_ingress_reactor_test`：在 Windows 下启用现有测试，覆盖 SOCKS5 / HTTP CONNECT handshake、early data 边界、cancel。
- `tcpquic_ares_dns_resolver_test`：Windows 下启用 localhost resolve、cancel suppress callback、timeout selection。
- `tcpquic_server_dial_reactor_test`：Windows 下启用 literal IP、DNS fail、connect refused、cancel、destructor closes pending fd。
- `tcpquic_windows_relay_worker_test`：保持现有 relay 数据面回归。

集成 / 手工验证：

- Windows client 单 peer SOCKS5 代理到 Linux server。
- Windows client HTTP CONNECT 代理到 Linux server。
- Windows server 接收 Linux client OPEN，domain target 由 Windows c-ares 异步解析。
- `speed_test` upload/download 在 Windows server 和 Windows client 上不出现 fatal relay reset 回归。

## 8. 风险与约束

1. `WSAEventSelect` 会把 socket 设置为 non-blocking；所有控制面 socket 必须按 non-blocking 语义处理。
2. `WSA_MAXIMUM_WAIT_EVENTS` 限制必须通过分片处理，wake event 要出现在每个 wait 轮次或以轮询方式优先检查。
3. c-ares callback 可能在 `ares_process_fds()` 内同步触发，不能在 callback 中重入获取 reactor 外部锁。
4. Windows `FD_WRITE` 不是边沿等价于 epoll write，需要避免无意义常驻 write interest 导致空转。
5. client ingress 仍必须保证 early data 不被控制面预读。
6. legacy fallback 保留期间要避免双路径同时监听同一个地址。

## 9. 验收标准

- Windows client 模式不再为每个 peer 创建 `TqThreadPool` 和独立 listener thread。
- Windows server 普通 OPEN 不再为每条连接创建 detached dial thread。
- Windows DNS 解析通过 c-ares 异步执行，dial reactor 线程驱动其 socket 和 timeout。
- Linux 当前测试目标继续通过，线程模型不回退。
- Windows `tcpquic_windows_reactor_test`、`tcpquic_client_ingress_reactor_test`、`tcpquic_ares_dns_resolver_test`、`tcpquic_server_dial_reactor_test`、`tcpquic_windows_relay_worker_test` 可构建并通过。
