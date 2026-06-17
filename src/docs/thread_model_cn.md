# tcpquic-proxy 运行线程模型

本文档根据当前 `src/` 源码描述 `tcpquic-proxy` 的线程来源、回调边界、relay worker 数据路径和资源背压。重点是当前生产实现，不再描述已删除的 blocking relay / ingress-copy 旧路径。

## 1. 总体结论

`tcpquic-proxy` 是 **应用自建控制线程 + MsQuic worker + 平台 relay worker** 的混合模型：

- MsQuic registration profile 由 `--quic-profile` 选择，默认 `max-throughput`，也支持 `low-latency`。
- MsQuic 自身创建 datapath worker、QUIC worker 和 registration cleanup/close 相关线程；应用 stream callback 运行在 MsQuic QUIC worker 上。
- client 模式可以是 single-peer，也可以通过 `--client-config` 进入 multi-peer router runtime。每个 active peer 有自己的 `QuicClientSession`、handshake `TqThreadPool`、SOCKS5/HTTP listener 对象。
- client listener 只在对应 peer 至少有一个 connected QUIC connection 时打开；所有连接断开时 listener 会关闭，重连成功后再打开。
- client 本地 SOCKS5 / HTTP CONNECT 的握手和 OPEN 阶段由固定大小 `TqThreadPool` 执行，不再为每个 accepted TCP 连接无限制 detach handler。
- client `QuicClientSession` 启动后会创建 reconnect thread，按 `--quic-reconnect-interval-ms` 周期尝试补齐断开的 QUIC connection slot。
- server 模式没有 SOCKS5 / HTTP CONNECT listener；入站 QUIC stream 先经过 dispatcher，可识别普通 `OPEN` 和内置 speed-test 控制 stream。
- server 普通 OPEN 不在 MsQuic callback 内直接 DNS/TCP connect，而是创建短生命周期 dial worker 线程完成 ACL/DNS/connect，再启动 relay。
- Linux 生产 relay 由固定数量 `TqLinuxRelayWorker` 分片处理全部 relay TCP fd：`epoll` + `eventfd` + `readv` + `writev`，没有 per-tunnel TCP relay 线程。
- Linux QUIC->TCP receive callback 统一返回 `QUIC_STATUS_PENDING`。callback 只建立借用 MsQuic receive buffer 的 pending view 并入队；TCP 写出、zstd 解压和 `StreamReceiveComplete` 都在 owner relay worker 上推进。
- Windows relay 当前走 `TqWindowsRelayWorker` / IOCP：每个 worker 一个 IOCP thread，receive callback 也使用 pending view；zstd 解压在 IOCP worker 路径中处理。Windows 相关计划仍是 active plan，最终验证需要在 Windows 机器上完成。
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

`tcpquic-proxy` 没有 secnetperf 那种工具级 WorkerPool；应用层主要线程来自 listener、handshake pool、relay worker、admin、trace、speed-test 和 reconnect。

### 2.2 tcpquic-proxy 应用线程

| 线程 | client | server | 创建点 | 作用 |
|------|--------|--------|--------|------|
| 主线程 | 有 | 有 | `main()` | 初始化 socket、配置、tuning、trace、reaper，然后进入 client/server run loop |
| client reconnect thread | 有 | 无 | `QuicClientSession::StartReconnectLoop()` | 周期调用 `EnsureConnected(100ms)`，补齐断开的 connection slot |
| SOCKS5 listener thread | 有 | 无 | `TqSocks5Server::Start()` | `accept()` SOCKS5 TCP 连接，提交到 handshake pool |
| HTTP CONNECT listener thread | 有 | 无 | `TqHttpConnectServer::Start()` | `accept()` HTTP CONNECT TCP 连接，提交到 handshake pool |
| handshake pool | 有 | 无 | `TqThreadPool::Start()` | 固定 worker 数，处理 SOCKS5/HTTP 解析、OPEN、启动 relay |
| admin accept thread | 可选 | 可选 | `TqAdminHttpServer::Start()` | `/health`、`/metrics`、multi-peer `/config` 等 admin HTTP |
| admin client thread | 可选 | 可选 | `TqAdminHttpServer::Run()` | 每个 admin HTTP client 一个短生命周期 detached thread |
| server dial worker | 无 | 有 | `TryHandleServerOpen()` | 每条普通 OPEN 一个短生命周期 detached thread，做 DNS/ACL/TCP connect |
| speed-test accept/worker | speed-test client/server 时 | 有 | `TqServerSpeedTestController` / speed client runner | 内置上传/下载测试的临时 loopback TCP server 和 pump worker |
| trace periodic thread | 可选 | 可选 | `TqTraceInit()` | `--trace` 开启后周期输出统计快照 |
| tunnel reaper | 有 | 有 | `TqTunnelReaperGuard` | 全局单线程，每 100ms 检查 relay 是否停止并释放 tunnel context |
| Linux relay worker | 有 | 有 | `TqLinuxRelayRuntime::Start()` | 固定 W 个 worker，epoll/readv/writev 处理全部 relay TCP fd |
| Windows relay worker | 有 | 有 | `TqWindowsRelayRuntime::Start()` | 固定 W 个 worker，IOCP/WSARecv/WSASend 处理全部 relay TCP fd |

`TqThreadPool(0)` 会按 `std::thread::hardware_concurrency()` 自动选择 worker 数，上限 64；默认配置仍以 `--handshake-threads` 的默认值为准。

## 3. Client 模式

### 3.1 single-peer 启动路径

```text
main thread
  └─ RunSinglePeerClient(cfg)
       ├─ QuicClientSession::Start(quicCfg)
       │    ├─ MsQuicOpenVersion
       │    ├─ RegistrationOpen(profile)
       │    ├─ ConfigurationOpen/LoadCredential
       │    └─ StartReconnectLoop()
       ├─ speed-test? -> TqRunClientSpeedTest() 后退出
       ├─ warmup? -> EnsureAnyConnected() + TqRunClientWarmup()
       ├─ TqSinglePeerClientRuntime::Start()
       │    └─ handshake Pool.Start()
       ├─ SetStartTunnel(lambda)
       ├─ SetConnectionStateHandler(lambda)
       ├─ EnsureAnyConnected()
       ├─ EnableAcceptingAndApplyCurrentConnectionState()
       │    └─ connected_count > 0 时打开 SOCKS5/HTTP listener
       ├─ admin? -> TqAdminHttpServer::Start()
       └─ 主线程 sleep 保持进程存活
```

如果启动时没有任何 QUIC connection 连上，client 不直接失败；listener 先保持关闭，reconnect thread 连上后由 connection state handler 打开 listener。

### 3.2 multi-peer client

`--client-config` 启用 `TqRouterRuntime` 和 `TqMultiPeerRuntimeAdapter`。每个 peer runtime 独立拥有：

- `QuicClientSession`
- `TqThreadPool`
- `TqSocks5Server`
- 可选 `TqHttpConnectServer`
- per-peer listener mutex 和 tunnel start mutex

admin `/config` 更新会由 router runtime 调用 adapter start/stop/drain peer。单个 peer 的 QUIC 断开只关闭该 peer 的 listener，不影响其它 peer。

### 3.3 本地 SOCKS5 / HTTP CONNECT

```text
SOCKS5/HTTP listener thread
  └─ accept()
       └─ Pool.Submit(handler)

handshake pool worker
  ├─ 读取 SOCKS5 greeting/request 或 HTTP CONNECT header
  ├─ TqTuneTcpForThroughput(clientFd)
  ├─ runtime->Quic->EnsureAnyConnected()
  ├─ runtime->Quic->PickConnection()
  ├─ TqStartClientTunnel(conn, req, clientFd, cfg)
  ├─ 等待 OPEN response
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
handshake pool worker
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
            ├─ ACL 初步校验
            └─ std::thread(FinishServerOpenAfterDial).detach()

dial worker
  ├─ DNS / address iteration
  ├─ TqDialTcp(nonblocking connect + poll/select)
  ├─ TqTuneTcpForThroughput(targetFd)
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
| client reconnect thread | 每 `QuicClientSession` 1 个 | session start 到 stop |
| client listener thread | 每 active listener 1 个 | peer connected 时打开，所有 QUIC 断开时关闭 |
| server dial worker | server 普通 OPEN 期间 0 或 1 | 拨号完成即退出 |

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
C = --quic-connections
P = client-config 中 active peer 数
H = 每 peer handshake pool worker 数
W = relay worker 数
D = server 正在拨号的 OPEN 数
A = admin 并发 HTTP client 数
S = speed-test 临时 worker 数
```

### 8.1 single-peer client

```text
1 main
+ N cxplat_worker
+ N quic_worker
+ MsQuic auxiliary threads
+ 1 reconnect thread
+ 1 SOCKS5 listener
+ 0/1 HTTP CONNECT listener
+ H handshake workers
+ 0/1 admin accept thread
+ A admin client threads
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
+ P * (1 reconnect + H handshake workers + active listener threads)
+ shared admin/router thread(s)
+ shared platform relay workers
```

每个 peer 的 listener 会随 QUIC connected count 打开/关闭。

### 8.3 server

```text
1 main
+ N cxplat_worker
+ N quic_worker
+ MsQuic auxiliary threads
+ 0/1 admin accept thread
+ A admin client threads
+ 1 tunnel reaper
+ W platform relay workers
+ D short-lived dial workers
+ optional speed-test accept/worker threads
+ optional trace thread
```

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

### 10.4 server dial worker

server 每个普通 OPEN 创建短生命周期 detached dial worker。高 OPEN rate 下，DNS/connect 慢路径仍可能造成瞬时线程数上升。

### 10.5 admin 与 speed-test

admin 每个 client 一个 detached thread；speed-test 会创建临时 accept/pump worker。它们不是常规 relay 热路径，但压测时会影响线程观测。

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
| `QuicClientSession::RunReconnectLoop` | client reconnect thread |
| `TqSocks5Server::Run` | SOCKS5 listener |
| `TqHttpConnectServer::Run` | HTTP CONNECT listener |
| `TqThreadPool::WorkerLoop` | client handshake worker |
| `FinishServerOpenAfterDial` / `TqDialTcp` | server dial worker |
| `TqAdminHttpServer::Run` | admin accept thread |
| `TqAdminHttpServer::HandleClient` | admin client thread |
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
| `src/protocol/quic_session.cpp` | MsQuic API/registration/configuration、client reconnect、connection/stream callback |
| `src/ingress/socks5_server.cpp` | SOCKS5 listener 和 handler submit |
| `src/ingress/http_connect_server.cpp` | HTTP CONNECT listener 和 handler submit |
| `src/runtime/thread_pool.cpp` | handshake 固定大小线程池 |
| `src/runtime/admin_http.cpp` | admin HTTP accept/client threads |
| `src/runtime/router_runtime.cpp` | multi-peer config/admin runtime |
| `src/runtime/speed_test.cpp` | 内置 upload/download speed-test 控制和 pump worker |
| `src/runtime/trace.cpp` | 可选 trace 日志和周期 snapshot |
| `src/tunnel/tcp_tunnel.cpp` | OPEN 状态机、server incoming stream dispatcher、server dial worker、tunnel registry/reaper 集成 |
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

当前 `tcpquic-proxy` 的生产线程模型是：MsQuic 负责 QUIC/datapath worker，client 用 reconnect thread + listener thread + bounded handshake pool 管理本地入口，server 用短生命周期 dial worker 避免 callback 阻塞；relay 层由 Linux epoll worker 或 Windows IOCP worker 分片多路复用所有 TCP fd。QUIC receive callback 统一轻量 pending，实际 TCP 写出、zstd 解压和 `StreamReceiveComplete` 由 relay worker 按真实进度完成。
