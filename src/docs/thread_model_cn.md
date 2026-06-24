# tcpquic-proxy 运行线程模型

本文档根据当前 `src/` 源码描述 `tcpquic-proxy` 的线程来源、回调边界、relay worker 数据路径和资源背压。重点是当前生产实现，不再描述已删除的 blocking relay / ingress-copy 旧路径。

## 1. 总体结论

`tcpquic-proxy` 是 **应用自建控制线程 + MsQuic worker + 平台 relay worker** 的混合模型：

- MsQuic registration profile 由 `--quic-profile` 选择，默认 `max-throughput`，也支持 `low-latency`。
- MsQuic 自身创建 datapath worker、QUIC worker 和 registration cleanup/close 相关线程；应用 stream callback 运行在 MsQuic QUIC worker 上。
- client 模式可以是 single-peer，也可以通过 `--client-config` 进入 multi-peer router runtime。每个 active peer 有自己的 `QuicClientSession`，本地 SOCKS5 / HTTP CONNECT 入口由共享的 `TqClientIngressReactor` 管理。
- client ingress listen 只在对应 peer 至少有一个 connected QUIC connection 时打开；所有连接断开或从未连接成功时，该 peer 的 listener 保持关闭。
- client 本地 SOCKS5 / HTTP CONNECT 的 listen、accept、握手状态机和 OPEN 完成回投由 `TqClientIngressReactor` 执行，不再为每个 peer 创建 listener 线程和握手线程池。
- client `QuicClientSession` 不创建独立重连线程。异步断开由 MsQuic connection callback 在 `SHUTDOWN_COMPLETE` 后重建对应 slot；同步 `ConnectionOpen` / `ConnectionStart` 失败由注入的 `DelayedTaskScheduler` 投递到 `TqClientIngressReactor` 延迟任务队列重试。
- server 模式没有 SOCKS5 / HTTP CONNECT listener；入站 QUIC stream 先经过 dispatcher，可识别普通 `OPEN` 和内置 speed-test 控制 stream。
- server 普通 OPEN 不在 MsQuic callback 内直接 DNS/TCP connect，而是提交给进程级 `TqServerDialReactor`，由该 reactor 线程完成 ACL、c-ares DNS 和 non-blocking TCP connect。
- Linux 生产 relay 由固定数量 `TqLinuxRelayWorker` 分片处理全部 relay TCP fd：`epoll` + `eventfd` + `readv` + `writev`，没有 per-tunnel TCP relay 线程。
- Linux QUIC->TCP receive callback 统一返回 `QUIC_STATUS_PENDING`。callback 只建立借用 MsQuic receive buffer 的 pending view 并入队；TCP 写出、zstd 解压和 `StreamReceiveComplete` 都在 owner relay worker 上推进。
- Windows relay 当前走 `TqWindowsRelayWorker` / IOCP：每个 worker 一个 IOCP thread，receive callback 也使用 pending view；zstd 解压在 IOCP worker 路径中处理。
- Linux / Windows / macOS 使用同一套 client reconnect、client ingress gating 和 server dial reactor 机制；平台差异只在底层 reactor/relay 后端，功能完成后三个平台分别验证即可。
- 隧道清理由进程级 `TqTunnelReaper` 单线程统一回收，不再为每条隧道创建 cleanup watcher。
- 旧 `TqBlockingDemoRelay` 和 `TqTcpWriteQueue` 已删除，不参与生产或测试链接。

## 2. 线程来源

### 2.1 MsQuic 内部线程

`quic_session.cpp` 根据配置调用 `RegistrationOpen`：

```cpp
TqToMsQuicProfile(cfg.QuicProfile)
// max-throughput -> QUIC_EXECUTION_PROFILE_TYPE_MAX_THROUGHPUT
// low-latency    -> QUIC_EXECUTION_PROFILE_LOW_LATENCY
```

常见 MsQuic 线程：

| 线程 | 数量 | 来源 | 作用 |
|------|------|------|------|
| `cxplat_worker` | 约 CPU 数 | MsQuic datapath WorkerPool | UDP socket、epoll/IOCP datapath、packet 收发 |
| `quic_worker` | 约 CPU 数 | MsQuic registration QuicWorkerPool | QUIC 连接、stream、定时器、应用 callback |
| `RegistrationCloseWorker` | 每 registration 约 1 个 | MsQuic | 异步关闭 registration |
| `RegistrationCleanupWorker` | 全局 1 个 | MsQuic | 清理已关闭 registration |

`tcpquic-proxy` 没有 secnetperf 那种工具级 WorkerPool；应用层主要线程来自 client ingress reactor、server dial reactor、relay worker、admin、trace、speed-test 和 tunnel reaper。

### 2.2 tcpquic-proxy 应用线程

| 线程 | client | server | 创建点 | 作用 |
|------|--------|--------|--------|------|
| 主线程 | 有 | 有 | `main()` | 初始化 socket、配置、tuning、trace、reaper，然后进入 client/server run loop |
| client ingress reactor | 有 | 无 | `TqClientIngressReactor::Start()` | 共享线程处理 SOCKS5/HTTP listen、accept、握手状态机、OPEN 完成回投和同步 start 失败 delayed retry |
| admin listen thread | 可选 | 可选 | `TqAdminHttpServer::Start()` | `cpp-httplib` listen loop，接收 `/api/v1/*` 和兼容旧 admin HTTP |
| admin worker pool | 可选 | 可选 | `TqAdminHttpServer::Start()` | 固定大小 worker pool，默认 2，受 `--admin-threads` 限制 |
| server dial reactor | 无 | 有 | `TqServerDialReactor::Start()` | 单线程处理普通 OPEN 的 ACL、c-ares DNS 和 non-blocking TCP connect |
| speed-test accept/worker | speed-test client/server 时 | 有 | `TqServerSpeedTestController` / speed client runner | 内置上传/下载测试的临时 loopback TCP server 和 pump worker；client 数据连接通过 `cfg.HttpListen` 的 HTTP CONNECT ingress 路径 |
| trace periodic thread | 可选 | 可选 | `TqTraceInit()` | `--trace` 开启后周期输出统计快照 |
| tunnel reaper | 有 | 有 | `TqTunnelReaperGuard` | 全局单线程，每 100ms 检查 relay 是否停止并释放 tunnel context |
| Linux relay worker | 有 | 有 | `TqLinuxRelayRuntime::Start()` | 固定 W 个 worker，epoll/readv/writev 处理全部 relay TCP fd |
| Windows relay worker | 有 | 有 | `TqWindowsRelayRuntime::Start()` | 固定 W 个 worker，IOCP/WSARecv/WSASend 处理全部 relay TCP fd |

`--handshake-threads` 和 JSON `client.handshake_threads` 作为兼容配置保留，但当前跨平台 ingress reactor 路径不会创建 client 握手线程池。

## 3. Client 模式

### 3.1 single-peer 启动路径

```text
main thread
  └─ RunSinglePeerClient(cfg)
       ├─ TqSinglePeerClientRuntime::Start()
       │    └─ TqClientIngressReactor::Start()
       ├─ SetStartTunnel(lambda)
       ├─ SetDelayedTaskScheduler(lambda -> ingress.EnqueueDelayed)
       ├─ SetConnectionStateHandler(lambda)
       ├─ QuicClientSession::Start(quicCfg)
       │    ├─ MsQuicOpenVersion
       │    ├─ RegistrationOpen(profile)
       │    ├─ ConfigurationOpen/LoadCredential
       │    └─ StartAllSlots()
       ├─ EnableAcceptingAndApplyCurrentConnectionState()
       │    └─ connected_count > 0 时打开 SOCKS5/HTTP listener
       ├─ speed-test? -> 通过 cfg.HttpListen 的 HTTP CONNECT ingress 建立数据连接，控制面使用 speed control stream
       ├─ admin? -> TqAdminHttpServer::Start()
       └─ 主线程 sleep 保持进程存活
```

如果启动时没有任何 QUIC connection 连上，client 不直接失败；listener 先保持关闭。异步断开后 MsQuic callback 在 `SHUTDOWN_COMPLETE` 后立即重建 slot；同步 start 失败由 ingress reactor delayed task 重试。连接成功后 connection state handler 打开 listener。

### 3.2 multi-peer client

`--client-config` 启用 `TqRouterRuntime` 和 `TqMultiPeerRuntimeAdapter`。每个 peer runtime 独立拥有：

- `QuicClientSession`
- peer config 和 state handler
- per-peer tunnel start mutex

adapter 持有一个共享 `TqClientIngressReactor`。每个 peer 的 SOCKS5 / HTTP CONNECT listen fd 按 connected count 注册到这个共享 reactor。它不是每个 peer 或每条 connection 一个重连线程，也不是每个 peer 一个 listener 线程。

admin `/api/v1/config` 更新会由 router runtime 调用 adapter start/stop/drain peer。单个 peer 的 QUIC 断开只关闭该 peer 的 listener，不影响其它 peer。

### 3.3 本地 SOCKS5 / HTTP CONNECT

```text
TqClientIngressReactor worker
  ├─ accept() SOCKS5 / HTTP CONNECT client fd
  ├─ non-blocking 读取 SOCKS5 greeting/request 或 HTTP CONNECT header
  ├─ TqTuneTcpForThroughput(clientFd)
  ├─ runtime->Quic->PickConnection()
  ├─ TqStartClientTunnelAsync(conn, req, clientFd, cfg)
  ├─ OPEN completion 由 EnqueueAsync() 回投本 reactor
  └─ 成功后返回 SOCKS5 reply 或 HTTP 200
```

当前 HTTP/SOCKS handler 直接把 accepted `clientFd` 交给 tunnel/relay，不再为 relay 复制一个额外 tunnel fd。

OPEN response 由 MsQuic stream callback 处理：

```text
quic_worker
  └─ QUIC_STREAM_EVENT_RECEIVE
       └─ TqTunnelContext::OnReceive()
            └─ TryCompleteClientOpen()
                 └─ condition_variable.notify_all()
```

### 3.4 Client 数据转发

OPEN 成功后：

```text
TqClientIngressReactor worker
  └─ StartRelay()
       └─ TqRelayStart()
            ├─ Linux: TqLinuxRelayRuntime::PickWorker()
            ├─ Windows: TqWindowsRelayRuntime::RegisterRelay()
            └─ stream callback 切换到平台 relay worker callback
```

Linux 数据路径：

| 方向 | 线程 | 路径 |
|------|------|------|
| 本地 TCP -> QUIC | Linux relay worker | `epoll(EPOLLIN)` -> `readv(TcpFd)` -> 可选 zstd 压缩 -> 多缓冲 `Stream->Send()` |
| QUIC -> 本地 TCP | MsQuic callback + Linux relay worker | `RECEIVE` callback 建 pending view 并返回 `PENDING` -> worker `writev()`；zstd 时 worker 先 bounded 解压到 worker buffer 再写 TCP |

## 4. Server 模式

### 4.1 启动路径

```text
main thread
  └─ RunServer(cfg)
       ├─ TqServerMetrics
       ├─ TqServerSpeedTestController
       ├─ QuicServerSession::SetConnectionHandler()
       ├─ QuicServerSession::SetPeerStreamHandler()
       ├─ QuicServerSession::Start(cfg)
       │    ├─ RegistrationOpen(profile)
       │    ├─ ConfigurationOpen/LoadCredential
       │    ├─ ListenerOpen
       │    └─ ListenerStart
       ├─ admin? -> TqAdminHttpServer::Start()
       └─ quic.Run()
```

server 只有 QUIC listener，没有本地 TCP listener。入站 stream 由 MsQuic callback 触发。

### 4.2 入站 stream dispatcher

```text
quic_worker
  └─ PEER_STREAM_STARTED
       └─ TqHandleServerIncomingStream(conn, stream, acl, cfg, speed, ...)
            ├─ 读取首个控制帧
            ├─ TQ_CMD_OPEN        -> 普通 tunnel OPEN
            └─ TQ_CMD_SPEED_START -> 内置 speed-test control stream
```

普通 OPEN 的 server 侧拨号：

```text
quic_worker
  └─ TqTunnelContext::OnReceive()
       └─ TryHandleServerOpen()
            ├─ 解析 OPEN request
            └─ TqServerDialReactor::Submit()

TqServerDialReactor worker
  ├─ ACL / speed-test ephemeral authorizer
  ├─ c-ares async DNS / literal address handling
  ├─ non-blocking TCP connect + timeout
  ├─ completion 回到 tunnel context
  ├─ SendOpenResponse
  └─ StartRelay()
```

server relay 数据路径与 client 对称：

| 方向 | 线程 | 路径 |
|------|------|------|
| 目标 TCP -> QUIC | 平台 relay worker | `readv`/`WSARecv` -> 可选 zstd 压缩 -> `StreamSend` |
| QUIC -> 目标 TCP | MsQuic callback + 平台 relay worker | pending receive view -> worker `writev`/`WSASend`；zstd 时 worker 侧解压 |

## 5. Linux relay worker 细节

### 5.1 注册与线程

`TqRelayStart()` 在 Linux 上：

```text
TqApplyRelayPoolBudget(tuning, activeRelays)
TqLinuxRelayRuntime::Start(tuning)
TqLinuxRelayRuntime::PickWorker()
worker->RegisterRelayWithId(registration)
```

`TqLinuxRelayRuntime` 按 `LinuxRelayWorkerCount` 创建 worker。每个 worker 有：

- 1 个 `std::thread` 执行 `TqLinuxRelayWorker::Run()`
- 1 个 epoll fd
- 1 个 eventfd 用于跨线程唤醒
- bounded `TqLinuxRelayEventQueue`
- 多个 relay state，每个 relay 一个 TCP fd、stream binding、buffer pool 和 pending receive 队列

### 5.2 TCP -> QUIC

```text
Linux relay worker
  ├─ epoll_wait()
  ├─ EPOLLIN -> DrainTcpReadable()
  ├─ AcquireWorker() 获取 worker buffer
  ├─ readv(TcpFd, iov)
  ├─ zstd? -> BuildTcpToQuicViews() 压缩输出到 worker buffer
  ├─ Stream->Send()
  └─ SEND_COMPLETE callback -> eventfd 唤醒 worker -> CompleteQuicSend() 释放 buffer
```

如果 worker buffer pool slot / budget 用尽，worker 会临时 `ArmTcpReadable(relay, false)` 停读 TCP fd。这样不会丢 TCP 数据；数据留在内核 socket receive buffer，等 QUIC send complete 释放 buffer 后再 re-arm。

相关可配置项：

| 参数 | 默认 | 含义 |
|------|------|------|
| `--linux-relay-read-chunk-size <bytes>` | 128KB | TCP read 单个 worker buffer chunk 大小 |
| `--linux-relay-worker-slots <n>` | 128 | 每 relay worker buffer slot 数 |
| `LinuxRelayPerTunnelPendingBytes` | tuning 计算，默认 4MB 起 | 每 relay QUIC pending receive 背压上限 |
| `LinuxRelayQuicReceiveCompleteBatchBytes` | 0 | `StreamReceiveComplete` 聚合阈值；0 表示写多少 complete 多少 |

### 5.3 QUIC -> TCP

当前 Linux `RECEIVE` callback：

```text
quic_worker
  └─ TqLinuxRelayWorker::OnStreamEventWithBinding(RECEIVE)
       ├─ QueueDeferredQuicReceive()
       │    └─ 保存 MsQuic QUIC_BUFFER slice 指针和 FIN 标志
       ├─ Enqueue(QuicReceiveView)
       └─ return QUIC_STATUS_PENDING
```

worker 侧：

```text
Linux relay worker
  └─ ProcessQuicReceiveViewEvent()
       ├─ relay->PendingQuicReceives.push_back(view)
       ├─ MaybePauseQuicReceive()  // pending bytes 超阈值时 ReceiveSetEnabled(false)
       └─ FlushDeferredQuicReceives()
            ├─ plain: writev(TcpFd, MsQuic borrowed slices)
            ├─ zstd: DrainCompressedQuicReceiveView()
            │    ├─ DecompressInto(worker buffer)
            │    ├─ PendingTcpWrites.push_back(worker-owned output)
            │    └─ FlushTcpWrites()
            ├─ 按 TCP 实际写出/压缩输入消费进度累计 PendingCompleteBytes
            └─ FlushDeferredReceiveCompletion() -> StreamReceiveComplete(bytes)
```

关键语义：

- callback 不做 inline TCP write。
- plain QUIC->TCP 不拷贝 payload，worker `writev()` 直接引用 MsQuic receive slices。
- zstd QUIC->TCP 必须产生 worker-owned 解压输出 buffer；输出可跨多个 worker buffer chunk。
- `StreamReceiveComplete` 按已写出 TCP bytes（plain）或已消费压缩输入 bytes（zstd）推进，不提前释放。
- `EAGAIN` / partial write 会保留 view offset，并 arm `EPOLLOUT` 后续继续。
- `PendingQuicReceiveBytes` 超上限时调用 `ReceiveSetEnabled(false)`，低于一半阈值后 resume。

### 5.4 关闭与回调安全

`RegisterRelayWithId()` 将 stream callback 切到 `TqLinuxRelayWorker::StreamCallback`，并用 `StreamRelayBinding` 记录 worker/relay/refcount。`UnregisterRelay()` 会标记 closing、解绑 relay 指针，并把 binding 放入 retired list，避免 late callback 访问已释放 relay。

`TqRelayStop()` 对 Linux worker：

```text
handle->Stop = true
worker->UnregisterRelay(relayId)
TqRelayUnregisterActive()
```

## 6. Windows relay worker 当前状态

Windows 生产 relay 使用 `TqWindowsRelayRuntime` 和 `TqWindowsRelayWorker`：

- 每个 worker 一个 IOCP handle 和一个 `std::thread`。
- TCP recv/send 使用 `WSARecv` / `WSASend` completion。
- QUIC receive callback 队列化 `TqWindowsPendingQuicReceive` 并返回 pending。
- plain path 从 MsQuic receive slices post TCP send。
- zstd path 在 worker 中 `DecompressInto()` 到 TCP send buffer pool 后 post TCP send。
- snapshot/admin metrics 已包含 pending receive、deferred complete、TCP send、zstd decompress、buffer pool 等字段。

Windows 相关计划仍是 active plan：源码已有统一 pending/zstd/metrics 的实现基础，但 Windows 构建、unit、loopback 和双机验证需要在 Windows 机器上继续闭环。

## 7. 每条隧道对象生命周期

| 对象 / 线程 | 数量 | 生命周期 |
|-------------|------|----------|
| `TqTunnelContext` | 1 | OPEN 阶段创建，注册到 `TqTunnelReaper`，relay 停止后由 reaper 释放 |
| `MsQuicStream` wrapper | 1 | QUIC stream 生命周期 |
| relay registration | 1 | `TqRelayStart()` 注册到 Linux/Windows runtime，`TqRelayStop()` 注销 |
| platform relay worker | W | 进程级 runtime 管理，多个隧道共享 |
| tunnel reaper | 全局 1 | 每 100ms 扫描 stopped relay context |
| client ingress reactor | single-peer 1 个；multi-peer 每个 runtime adapter 1 个共享 reactor | runtime start 到 stop，peer connected 时注册 listen fd，全断开时移除 |
| server dial reactor | 进程级 1 个 | server start 到 stop |

正常关闭：

```text
TCP EOF / QUIC FIN / abort
  ├─ relay 标记 Stop 或 TcpWriteShutdownQueued
  ├─ worker flush pending receive completion
  ├─ TqRelayStop() 注销 relay
  ├─ stream callback 解绑/retire binding
  ├─ CloseTcp()
  └─ TqTunnelReaper 删除 TqTunnelContext
```

QUIC connection shutdown 时，`QuicClientSession` / server callback 会调用 `TqAbortConnectionTunnels(connection)`，只 abort 属于该 connection 的 tunnel。

## 8. 线程数量估算

设：

```text
N = 逻辑 CPU 数 / MsQuic worker 数量近似
P = client-config 中 active peer 数
W = relay worker 数
A = admin HTTP worker pool 大小，默认 2
S = speed-test 临时 worker 数
```

### 8.1 single-peer client

```text
1 main
+ N cxplat_worker
+ N quic_worker
+ MsQuic auxiliary threads
+ 1 TqClientIngressReactor worker
+ 0/1 admin listen thread
+ A admin worker pool threads
+ 1 tunnel reaper
+ W platform relay workers
+ optional trace thread
+ optional speed-test workers
```

relay 线程数随 `W` 增长，不随活跃隧道数 `L` 线性增长。

### 8.2 multi-peer client

multi-peer 大致为：

```text
single process common threads
+ 1 shared TqClientIngressReactor worker per client runtime adapter
+ P * QuicClientSession state, but no per-peer reconnect/listener/handshake threads
+ shared admin/router thread(s)
+ shared platform relay workers
```

每个 peer 的 listener fd 会随 QUIC connected count 注册/移除；fd 数量随 peer 变化，线程数量不随 peer 线性增长。

### 8.3 server

```text
1 main
+ N cxplat_worker
+ N quic_worker
+ MsQuic auxiliary threads
+ 0/1 admin listen thread
+ A admin worker pool threads
+ 1 tunnel reaper
+ W platform relay workers
+ 1 TqServerDialReactor worker
+ optional speed-test accept/worker threads
+ optional trace thread
```

普通 OPEN 数量只增加 `TqServerDialReactor` 内部 pending state，不再增加短生命周期拨号线程。

## 9. 与 secnetperf 的差异

| 项目 | secnetperf | tcpquic-proxy |
|------|------------|---------------|
| QUIC profile | CLI `-exec:` | `--quic-profile max-throughput|low-latency` |
| 工具级 WorkerPool | 有 | 无 |
| 应用发送线程 | perf worker threads | relay worker 分片，epoll/IOCP 多路复用 TCP fd |
| 应用接收处理 | 统计为主 | pending receive view + worker TCP write/zstd decompress |
| 连接/stream 模型 | 压测参数控制 | 每 TCP 连接映射一个 QUIC bidirectional stream |
| 背压 | perf 内部统计路径 | TCP buffer、worker buffer pool、pending receive bytes、QUIC flow control 多层背压 |

## 10. 性能风险点

### 10.1 QUIC callback

当前主线避免 callback 内同步 TCP write 和 payload copy。callback 的主要风险变为：

- pending view 分配失败；
- event queue 满；
- callback 绑定/late callback 生命周期 bug；
- pending receive bytes 太高导致 flow-control 周转慢。

这些已通过 `linux_relay_quic_receive_view_*`、pending queue/bytes、deferred complete 指标暴露。

### 10.2 relay worker buffer pool

TCP->QUIC 和 zstd QUIC->TCP 都需要 worker-owned buffer。slot limit / pending budget 用尽时：

- TCP->QUIC：worker 停读 TCP fd，等待 QUIC send complete 释放 buffer 后 re-arm。
- zstd QUIC->TCP：解压输出拿不到 worker buffer 时保留 pending receive view，等待 TCP/QUIC 侧进展后继续。

这些场景不应直接导致数据丢失，但会造成吞吐下降和 backpressure。

### 10.3 QUIC receive pending 与 flow control

`QUIC_STATUS_PENDING` 意味着 MsQuic receive buffer ownership 暂留给应用。`StreamReceiveComplete` 越晚，QUIC receive flow-control 窗口越晚释放。当前策略是按真实处理进度 complete，不做提前 complete。

### 10.4 server dial reactor

server 普通 OPEN 通过 `TqServerDialReactor` 处理。高 OPEN rate 下，DNS/connect 慢路径会增加 reactor 内部 pending state 和 socket 数量，但不会线性增加应用线程数。

### 10.5 admin 与 speed-test

admin 使用 `cpp-httplib` 阻塞 HTTP/1.1 server 和固定大小 worker pool，不再为每个 client 创建 detached thread。speed-test 会创建临时 accept/pump worker。它们不是常规 relay 热路径，但压测时会影响线程观测。

## 11. 调试线程角色

Linux 下 `/proc/<pid>/task/*/comm` 常显示相同进程名，建议结合 stack：

```bash
gdb -p <pid>
thread apply all bt
```

典型栈特征：

| 栈特征 | 角色 |
|--------|------|
| `CxPlatWorkerThread` | MsQuic datapath worker |
| `QuicWorkerThread` | MsQuic QUIC worker / 应用 callback |
| `TqClientIngressReactor::Run` | client ingress reactor，处理 SOCKS5/HTTP accept、握手、OPEN completion 和 delayed retry |
| `TqServerDialReactor::Run` | server dial reactor，处理 ACL、c-ares DNS 和 non-blocking TCP connect |
| `TqAdminHttpServer::Run` | admin listen thread |
| `httplib::ThreadPool` worker | admin worker pool |
| `StatsThreadMain` / `DumpPeriodicStats` trace snapshot 栈 | trace periodic thread |
| `TqRunAcceptLoop` / `TqRunUploadWorker` / `TqRunDownloadWorker` | speed-test server temporary workers |
| `TqRunUploadPump` / `TqRunDownloadPump` | speed-test client pump workers |
| `TqTunnelReaper::ReaperLoop` | tunnel reaper |
| `TqLinuxRelayWorker::Run` | Linux relay worker |
| `TqLinuxRelayWorker::DrainTcpReadable` | Linux TCP->QUIC read path |
| `TqLinuxRelayWorker::FlushDeferredQuicReceives` | Linux QUIC->TCP pending view drain |
| `TqLinuxRelayWorker::DrainCompressedQuicReceiveView` | Linux zstd receive decompression |
| `TqWindowsRelayWorker::Run` | Windows IOCP relay worker |

## 12. 关键源文件索引

| 文件 | 说明 |
|------|------|
| `src/main.cpp` | client/server 入口、single/multi-peer runtime、admin、trace、reaper guard |
| `src/protocol/quic_session.cpp` | MsQuic API/registration/configuration、callback-driven client reconnect、connection/stream callback |
| `src/ingress/client_ingress_reactor.cpp` | client SOCKS5/HTTP listen、accept、handshake、OPEN completion 回投和 delayed retry |
| `src/ingress/client_ingress_state.cpp` | SOCKS5 / HTTP CONNECT 握手状态机 |
| `src/runtime/admin_http.cpp` | admin HTTP cpp-httplib listen thread、固定 worker pool、token/v1/legacy route gate |
| `src/runtime/router_runtime.cpp` | multi-peer config/admin runtime |
| `src/runtime/speed_test.cpp` | 内置 upload/download speed-test 控制、HTTP CONNECT ingress data path 和 pump worker |
| `src/runtime/trace.cpp` | 可选 trace 日志和周期 snapshot |
| `src/tunnel/tcp_tunnel.cpp` | OPEN 状态机、server incoming stream dispatcher、server dial reactor 提交、tunnel registry/reaper 集成 |
| `src/tunnel/server_dial_reactor.cpp` | server 普通 OPEN 的 ACL、c-ares DNS、non-blocking TCP connect |
| `src/tunnel/ares_dns_resolver.cpp` | c-ares async DNS resolver 封装 |
| `src/tunnel/tunnel_registry.cpp` | connection -> tunnel abort registry |
| `src/tunnel/tunnel_reaper.cpp` | 全局 tunnel context reaper |
| `src/tunnel/relay.cpp` | 平台 relay runtime 选择和 active relay 计数 |
| `src/tunnel/linux_relay_worker.cpp` | Linux 生产 relay：epoll/readv/writev、pending receive、zstd worker 解压 |
| `src/tunnel/relay_buffer.cpp` | OS 无关 relay 按需 buffer：mimalloc/std malloc 分配、pending bytes 预算 |
| `src/tunnel/relay_alloc.cpp` | relay buffer allocator 封装 |
| `src/tunnel/windows_relay_worker.cpp` | Windows 生产 relay：IOCP/WSARecv/WSASend、pending receive、zstd worker 解压 |
| `src/tunnel/tcp_dialer.cpp` | server 非阻塞 TCP connect 和 socket buffer tuning |
| `src/config/tuning.cpp` | tuning、relay memory budget、runtime samples |
| `src/protocol/compress.cpp` | zstd streaming compress/decompress |

## 13. 一句话总结

当前 `tcpquic-proxy` 的生产线程模型是：MsQuic 负责 QUIC/datapath worker；client 用共享 `TqClientIngressReactor` 管理本地 SOCKS5 / HTTP CONNECT 入口、OPEN completion 和同步 start 失败 delayed retry，异步断开由 MsQuic callback 在 `SHUTDOWN_COMPLETE` 后重建 slot；server 用 `TqServerDialReactor` 处理普通 OPEN 的 ACL、DNS 和 TCP connect；relay 层由 Linux epoll worker、Windows IOCP worker 或 macOS kqueue worker 分片多路复用所有 TCP fd。QUIC receive callback 统一轻量 pending，实际 TCP 写出、zstd 解压和 `StreamReceiveComplete` 由 relay worker 按真实进度完成。
