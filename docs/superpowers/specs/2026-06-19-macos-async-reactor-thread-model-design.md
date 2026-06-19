# macOS 线程异步优化设计方案

> 日期：2026-06-19  
> 状态：设计完成 / 待实现  
> 参考：`docs/thread-model_cn.md`、`docs/superpowers/plans/2026-06-18-async-reactor-thread-model.md`、`docs/superpowers/specs/2026-06-19-windows-async-reactor-thread-model-design.md`、`docs/superpowers/plans/2026-06-18-macos-kqueue-relay.md`。

## 1. 现状

Linux 与 Windows 平台已经完成控制面异步线程优化：

- client 侧所有 peer 的 SOCKS5 / HTTP CONNECT listen、accept、握手状态机收敛到一个 `TqClientIngressReactor` 线程。
- server 侧普通 OPEN 的 ACL、DNS 和 TCP connect 收敛到一个 `TqServerDialReactor` 线程。
- DNS 使用 vendored c-ares 异步解析，由所属 reactor 驱动 c-ares socket readiness 和 timeout，不启用 c-ares 内部 event thread。
- relay worker 数据面不随控制面迁移改变，Linux、Windows 保持各自高吞吐 worker 模型。

当前代码已经具备跨平台控制面 reactor 抽象：

- `src/runtime/socket_reactor.h` 定义 `ITqSocketReactor` 和 `TqReactorEvents`。
- Linux 使用 `src/runtime/linux_reactor.h/.cpp`，底层为 `epoll` / `eventfd`。
- Windows 使用 `src/runtime/windows_reactor.h/.cpp`，底层为 `WSAEventSelect` / `WSAWaitForMultipleEvents`。
- `src/ingress/client_ingress_reactor.*` 与 `src/tunnel/server_dial_reactor.*` 的业务状态机已经基本平台无关。

macOS 当前缺口在控制面：

- Darwin CMake 分支目前只接入了 `tunnel/darwin_relay_worker.cpp` 等 relay 数据面文件。
- Darwin 尚未接入 `client_ingress_reactor.cpp`、`server_dial_reactor.cpp`、`ares_dns_resolver.cpp`、`acl_filter.cpp` 和平台控制面 reactor。
- `server_dial_reactor.cpp` 的 reactor 工厂当前只区分 Windows 与非 Windows，非 Windows 默认返回 `TqLinuxReactor`。
- `ares_dns_resolver.cpp` 当前只在 Windows 使用 `TqWindowsReactor`，其它平台默认使用 `TqLinuxReactor`。

## 2. 目标

1. macOS client 侧所有 peer 的 SOCKS5 / HTTP CONNECT listen、accept、handshake 收敛到单一 `TqClientIngressReactor` 线程。
2. macOS server 侧普通 OPEN 的 ACL、DNS、non-blocking TCP connect 收敛到单一 `TqServerDialReactor` 线程。
3. macOS DNS 使用已有 vendored c-ares wrapper，由 macOS 控制面 reactor 驱动 socket readiness 和 timeout。
4. 复用 Linux/Windows 已验证的 `TqClientIngressReactor`、`TqServerDialReactor`、`TqAresDnsResolver` 业务状态机。
5. macOS relay 数据面继续由 `TqDarwinRelayWorker` 负责，不与控制面 reactor 合并。
6. Linux/Windows 当前异步控制面行为不回退。

## 3. 非目标

- 不重写 `TqDarwinRelayWorker` 的 kqueue relay 数据面。
- 不把 client ingress、server dial、relay worker 合并到同一个 kqueue 或同一个线程。
- 不改变 SOCKS5、HTTP CONNECT、OPEN response、ACL、speed-test ephemeral authorizer 等协议语义。
- 不改变 QUIC session 的连接管理、重连、peer 选择策略。
- 不改变 MsQuic 内部 worker / datapath 线程模型。

## 4. 总体设计

### 4.1 新增 Darwin 控制面 reactor

新增 `TqDarwinReactor`，实现已有 `ITqSocketReactor`：

- 文件：`src/runtime/darwin_reactor.h`
- 文件：`src/runtime/darwin_reactor.cpp`
- 测试：`src/unittest/darwin_reactor_test.cpp`

底层机制：

- `kqueue()` 创建控制面事件队列。
- kqueue fd 设置 `FD_CLOEXEC`，并在 `Stop()` / 构造失败路径统一关闭。
- `EVFILT_USER` 作为跨线程 wake event。
- `EVFILT_READ` 映射到 `TqReactorEvents::Read`。
- `EVFILT_WRITE` 映射到 `TqReactorEvents::Write`。
- `EV_EOF` / `EV_ERROR` 映射到 `TqReactorEvents::Error`。
- `Add()` / `Modify()` / `Remove()` 维护 fd 到 `{events, handler}` 的映射，并同步增删 kqueue filters。
- `Add()` / `Modify()` 必须拒绝空事件、未知事件位、空 handler 和未启动 reactor 上的操作。
- `RunOnce(timeoutMs)` 调用 `kevent()`，`EINTR` 时重试，将平台事件转换为抽象事件后按注册 interest mask 派发 handler。
- `Wake()` 只唤醒阻塞中的 `RunOnce()`，不算一次业务 handler work；仅消费 wake event 时 `RunOnce()` 返回 `false`，保持 Linux/Windows reactor 契约一致。

`TqDarwinReactor` 只服务控制面低吞吐 socket，包括 listen fd、accepted handshake fd、c-ares fd、pending connect fd。已经建立隧道后的 TCP/QUIC relay fd 继续交给 `TqDarwinRelayWorker` 数据面。

### 4.2 kqueue filter 管理

kqueue 的 read/write 是独立 filter，因此 `Modify()` 不能只更新内存中的事件位，需要精确维护 filter 和已注册事件掩码：

- `Read` 开启：注册或保留 `EVFILT_READ`。
- `Write` 开启：注册或保留 `EVFILT_WRITE`。
- `Error` 开启但 `Read` 关闭：注册 `EVFILT_READ` 作为 EOF/error 观察 filter，但派发前用 interest mask 过滤普通 read readiness，避免 error-only 注册收到普通数据事件。
- `Read` 关闭：删除 `EVFILT_READ`。
- `Write` 关闭：删除 `EVFILT_WRITE`。
- `Remove(fd)`：删除 read/write 两个 filter，并从 handler map 中移除 fd。

派发前必须重新查 handler map，并把转换后的事件与 `(registeredEvents | Error)` 相交；如果 fd 已被 `Remove()` 或 `Stop()` 清理，或相交结果为空，则忽略迟到事件。

### 4.3 server dial reactor 平台选择

`src/tunnel/server_dial_reactor.cpp` 的 `MakeSocketReactor()` 改为三平台选择：

- Windows：`TqWindowsReactor`
- macOS：`TqDarwinReactor`
- Linux/其它 POSIX：`TqLinuxReactor`

这样 `TqServerDialReactor` 的 DNS、TCP connect、timeout、cancel 状态机不需要复制，只替换底层 readiness backend。

### 4.4 c-ares resolver 平台选择

`src/tunnel/ares_dns_resolver.cpp` 的内部 reactor 类型改为三平台选择：

- Windows：`TqWindowsReactor`
- macOS：`TqDarwinReactor`
- Linux/其它 POSIX：`TqLinuxReactor`

c-ares 生命周期保持现有约束：

- resolver channel 初始化前持有进程级 c-ares library 引用。
- channel cancel/destroy 和 reactor stop 完成后释放引用。
- `SockStateCallback()` 只更新所属 reactor 的 socket interest。
- `ares_process_fds()` 可能同步触发 callback，不能在 callback 中跨锁调用外部 completion。

### 4.5 client ingress 迁移

Darwin runtime 复用现有 `TqClientIngressReactor`：

- single-peer client runtime 持有一个 `TqClientIngressReactor`。
- multi-peer client runtime 持有一个共享 `TqClientIngressReactor`。
- 所有 SOCKS5 / HTTP CONNECT listen fd 注册到该 reactor。
- accepted client socket 设置为 non-blocking 后注册到该 reactor。
- SOCKS5 / HTTP CONNECT handshake 状态机继续由 `TqClientIngressState` 处理。
- `TqStartClientTunnelAsync()` 的 OPEN completion 必须投回 ingress reactor 线程。
- early data 约束不变：控制面不能预读握手边界后的应用层数据。

### 4.6 server dial 迁移

Darwin server runtime 复用现有 `TqServerDialReactor`：

- 普通 OPEN 提交到 dial reactor。
- literal IP 跳过 DNS，直接做 no-DNS ACL filter。
- 域名目标使用 c-ares 异步解析。
- non-blocking `connect()` 返回 `EINPROGRESS` / `EALREADY` / would-block 类错误时进入 pending connect。
- 由 `TqDarwinReactor` 监听 write/error readiness。
- connect 成功后把 connected fd 交回 tunnel 层，由 tunnel 层发送 OPEN response 并启动 relay。
- completion 继续保持锁外调用，避免回调重入 `Cancel()` / `Stop()` 时死锁。

### 4.7 CMake 接入

Darwin proxy sources 需要补齐控制面源文件：

- `ingress/client_ingress_reactor.cpp`
- `ingress/client_ingress_state.cpp`
- `tunnel/client_tunnel_open.cpp`
- `tunnel/server_dial_reactor.cpp`
- `tunnel/ares_dns_resolver.cpp`
- `acl/acl_filter.cpp`
- `runtime/darwin_reactor.cpp`

Darwin 测试需要补齐：

- `tcpquic_darwin_reactor_test`
- `tcpquic_ares_dns_resolver_test` 的 Darwin reactor backend
- `tcpquic_server_dial_reactor_test` 的 Darwin reactor backend
- `tcpquic_client_ingress_reactor_test` 的 Darwin 版本
- 视依赖稳定性启用 `tcpquic_server_dial_reactor_worker_test`

## 5. 平台兼容风险

1. **`SOCK_NONBLOCK` / `SOCK_CLOEXEC`**  
   macOS 不应假定完全支持 Linux-style socket flags。若编译期或运行期不可用，需要使用 `socket()` + `fcntl(F_SETFL, O_NONBLOCK)` + `fcntl(F_SETFD, FD_CLOEXEC)` fallback。

2. **`accept4()`**  
   macOS 不提供 Linux `accept4()`，现有 ingress 代码如果直接引用会编译失败，而不是只在运行期返回 `ENOSYS`。Darwin 分支必须使用 `accept()` + non-blocking/close-on-exec 设置替代。

3. **`MSG_NOSIGNAL`**  
   macOS 不支持 Linux `MSG_NOSIGNAL`。平台 send helper 必须通过现有 `TqSendFlags::NoSignal` 抽象屏蔽差异，例如在 `MSG_NOSIGNAL` 不存在但 `SO_NOSIGPIPE` 可用时为 socket 设置 `SO_NOSIGPIPE`，避免 peer close 后普通 `send()` 触发 `SIGPIPE`。

4. **kqueue EOF / error 语义**  
   kqueue 的 `EV_EOF`、`EV_ERROR` 和 read/write readiness 组合不完全等价于 epoll。测试应验证抽象语义，不应绑定过窄事件组合。

5. **write readiness 空转**  
   `EVFILT_WRITE` 对可写 socket 可能持续触发。server dial reactor 只能在 pending connect 或确实需要写时启用 write interest。

6. **Remove 后迟到事件**  
   `Remove()` 后可能仍观察到已返回的事件批次；派发前必须查 handler map，不存在则忽略。

7. **控制面和 relay 数据面职责混淆**  
   macOS 控制面 `TqDarwinReactor` 与数据面 `TqDarwinRelayWorker` 都使用 kqueue，但它们必须使用独立 kqueue、独立线程、独立生命周期。

## 6. 验收标准

- macOS `tcpquic-proxy` 构建成功。
- macOS client 模式不再为每个 peer 创建独立 listener thread / handshake pool。
- macOS server 普通 OPEN 不再按连接数创建 detached dial worker。
- macOS DNS 由 c-ares async resolver + `TqDarwinReactor` 驱动。
- `tcpquic_darwin_reactor_test`、`tcpquic_ares_dns_resolver_test`、`tcpquic_server_dial_reactor_test`、`tcpquic_client_ingress_reactor_test` 在 macOS 构建并通过。
- `tcpquic_darwin_reactor_test` 覆盖 wake 不算 work、invalid event rejection、error-only interest、modify、remove、callback 内 self-remove / self-modify / stop。
- macOS client ingress 构建不引用 `accept4()`，listen socket 不依赖未定义的 Linux-only socket flags。
- macOS `TqSendFlags::NoSignal` 不会在 peer 已关闭时触发 `SIGPIPE`。
- Linux/Windows reactor、ingress、dial 相关测试继续通过。
- `docs/thread-model_cn.md` 更新为 Linux / Windows / macOS 三平台控制面统一 reactor 模型。
