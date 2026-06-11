# tcpquic-proxy 运行线程模型

本文档参考 `docs/thread_model_cn.md` 的 msquic 线程模型说明，并结合当前 `src/` 源码，描述 `tcpquic-proxy` 在 client / server 两种模式下的应用线程、MsQuic worker、隧道 relay 线程以及回调线程边界。

## 1. 总体结论

`tcpquic-proxy` 是 **应用自建线程 + MsQuic MAX_THROUGHPUT worker + Linux relay worker 分片** 的混合模型：

- MsQuic 协议执行使用 `QUIC_EXECUTION_PROFILE_TYPE_MAX_THROUGHPUT`，因此会创建独立 `quic_worker` 线程。
- UDP datapath 仍由 msquic 库级 `cxplat_worker` 处理。
- client 模式额外创建 2 个监听线程：SOCKS5 listener 和 HTTP CONNECT listener。
- 每个 accepted 本地 TCP 连接由固定大小的 **handshake 线程池**（`TqThreadPool`，默认 8 worker，`--handshake-threads` 可配）处理 SOCKS5 / HTTP CONNECT 握手和 OPEN 阶段，不再为每个连接 detached 一个 handler 线程。
- Linux 生产路径不再为每条隧道创建 relay TCP 线程。所有 relay TCP fd 由 `TqLinuxRelayWorker` 分片持有，通过 `epoll`、`readv`、`writev` 处理。旧 `TqBlockingDemoRelay` 仅用于 demo/legacy 对比，不属于生产线程模型。
- **QUIC Stream → TCP** 方向：MsQuic stream 回调将 receive 数据拷贝到 ingress 池化缓冲，并通过 bounded `TqLinuxRelayEventQueue` 投递给 owner worker；owner worker 消费事件后转移到 worker 缓冲域并通过 `writev` 写出，避免慢 TCP 写阻塞 `quic_worker`。
- 隧道清理由进程级 **全局 reaper**（`TqTunnelReaper`，单线程轮询）统一回收 context，不再为每条隧道创建 cleanup watcher。
- server 模式没有 SOCKS5 / HTTP CONNECT 监听线程，但每条入站 QUIC stream 会创建拨号线程；relay 注册到 Linux worker 分片。
- 同一 QUIC connection / stream 的 MsQuic 回调仍遵守 msquic 串行语义：不会对同一连接并行回调应用。

## 2. 线程来源

### 2.1 MsQuic 内部线程

`tcpquic-proxy` 在 `quic_session.cpp` 中按 `--quic-profile` 选择 Registration profile（默认 `max-throughput`，可选 `low-latency`）：

```cpp
TqToMsQuicProfile(cfg.QuicProfile)
// max-throughput → QUIC_EXECUTION_PROFILE_TYPE_MAX_THROUGHPUT
// low-latency    → QUIC_EXECUTION_PROFILE_LOW_LATENCY
```

默认 `max-throughput` 与 `docs/thread_model_cn.md` 中的 maxtput 模式一致：

| 线程 | 数量 | 来源 | 作用 |
|------|------|------|------|
| `cxplat_worker` | 约 CPU 数 | MsQuic 库级 WorkerPool | epoll / UDP datapath 收发 |
| `quic_worker` | 约 CPU 数 | Registration 级 QuicWorkerPool | QUIC 连接、stream、定时器、应用回调 |
| `RegistrationCloseWorker` | 每 Registration 约 1 个 | MsQuic | 异步关闭 Registration |
| `RegistrationCleanupWorker` | 全局 1 个 | MsQuic | 清理已关闭 Registration |

与 `secnetperf` 不同，`tcpquic-proxy` 本身没有 perf 工具级 WorkerPool，因此正常情况下不会额外创建一套工具级 `cxplat_worker`。

### 2.2 tcpquic-proxy 应用线程

| 线程 | client | server | 创建点 | 作用 |
|------|--------|--------|--------|------|
| 主线程 | 有 | 有 | `main()` | 初始化配置、启动 client/server、阻塞等待 |
| SOCKS5 listener | 有 | 无 | `RunClient()` | 阻塞 `accept()` SOCKS5 TCP 连接 |
| HTTP CONNECT listener | 有 | 无 | `RunClient()` | 阻塞 `accept()` HTTP CONNECT TCP 连接 |
| handshake 线程池 | 有 | 无 | `RunClient()` → `TqThreadPool` | 固定 worker 数（默认 8），处理 SOCKS5 / HTTP CONNECT 握手与 OPEN |
| server dial worker | 无 | 有 | `TryHandleServerOpen()` | 每条入站 QUIC stream 一个 detached 线程，DNS/ACL 后非阻塞 connect 目标 TCP |
| Linux relay worker | 有 | 有 | `TqLinuxRelayRuntime::Start()` | 固定数量 worker 分片，epoll 多路复用所有 relay TCP fd |
| relay TCP fd 注册 | 有 | 有 | `TqRelayStart()` → `RegisterRelayWithId()` | 每条隧道注册到某个 worker，无 per-tunnel TCP 读线程 |
| tunnel reaper | 有 | 有 | `TqTunnelReaper::Instance()` | 全局单线程，轮询已停止隧道并释放 context |

## 3. Client 模式线程模型

### 3.1 启动阶段

```text
main thread
  └─ RunClient(cfg)
       ├─ QuicClientSession::Start(cfg)
       │    ├─ MsQuicOpenVersion
       │    ├─ RegistrationOpen(MAX_THROUGHPUT)
       │    └─ ConfigurationOpen/LoadCredential
       ├─ quic.EnsureConnected()
       │    └─ ConnectionStart，等待 CONNECTED 回调
       ├─ TqThreadPool handshakePool(cfg.HandshakeThreads)
       ├─ std::thread(RunSocks5Server, ..., &handshakePool)
       ├─ std::thread(RunHttpConnectServer, ..., &handshakePool)
       └─ join 两个 listener 线程
```

启动后主线程主要阻塞在 `join()`，实际工作由两个 TCP listener、MsQuic worker 和每隧道 relay 线程承担。

### 3.2 本地应用建立 SOCKS5 / HTTP CONNECT

```text
SOCKS5 listener thread / HTTP listener thread
  └─ accept()
       └─ handshakePool->Submit(handler)
            ├─ 读取 SOCKS5 或 HTTP CONNECT 请求
            ├─ dup(clientFd) 作为 tunnelFd
            ├─ TqTuneTcpForThroughput(tunnelFd)
            ├─ quic.EnsureConnected()
            ├─ quic.PickConnection()
            └─ TqStartClientTunnel(conn, req, tunnelFd, cfg)
```

handler 线程会等待 OPEN 响应：

```text
handler thread
  ├─ StreamOpen + SendOpenRequest
  ├─ WaitForOpenResponse(condition_variable)
  ├─ 成功后向本地应用返回 SOCKS5 reply 或 HTTP 200
  └─ 关闭原始 clientFd，relay 使用 dup 出来的 tunnelFd
```

OPEN 响应由 MsQuic stream 回调线程处理：

```text
quic_worker thread
  └─ QUIC_STREAM_EVENT_RECEIVE
       └─ TqTunnelContext::OnReceive
            └─ TryCompleteClientOpen
                 └─ CompleteOpen + StateChanged.notify_all()
```

### 3.3 Client 数据转发

成功 OPEN 后启动 relay：

```text
handler thread
  └─ StartRelay()
       └─ TqRelayStart()
            ├─ TqLinuxRelayRuntime::PickWorker()
            ├─ worker->RegisterRelayWithId(registration)
            └─ StreamCallback = TqLinuxRelayWorker::StreamCallback
```

数据方向分为两条线程路径：

| 方向 | 线程 | 路径 |
|------|------|------|
| 本地 TCP → QUIC | Linux relay worker | `epoll` 就绪 → `readv(tunnelFd)` → 可选压缩 → 多缓冲 `Stream->Send()` |
| QUIC → 本地 TCP | MsQuic callback + Linux relay worker | `QUIC_STREAM_EVENT_RECEIVE` → 拷贝入 ingress 池 → bounded event queue → worker 可选解压 → `writev(tunnelFd)` |

## 4. Server 模式线程模型

### 4.1 启动阶段

```text
main thread
  └─ RunServer(cfg)
       ├─ QuicServerSession::Start(cfg)
       │    ├─ MsQuicOpenVersion
       │    ├─ RegistrationOpen(MAX_THROUGHPUT)
       │    ├─ ConfigurationOpen/LoadCredential
       │    ├─ ListenerOpen
       │    └─ ListenerStart
       └─ quic.Run()
            ├─ interactive: getchar()
            └─ non-interactive: condition_variable wait
```

server 没有本地 TCP accept listener。它只监听 QUIC，入站 stream 由 MsQuic worker 回调驱动。

### 4.2 新连接与新 stream

```text
quic_worker thread
  └─ ListenerCallback(NEW_CONNECTION)
       ├─ new MsQuicConnection
       └─ connection->SetConfiguration()
```

```text
quic_worker thread
  └─ ConnectionCallback(PEER_STREAM_STARTED)
       └─ TqHandleServerPeerStream(conn, rawStream, acl, cfg)
            ├─ new TqTunnelContext(ServerOpen)
            └─ 设置 TqTunnelContext::Callback
```

server 收到 OPEN request 后，不在 MsQuic 回调里直接拨号，而是派发 detached 线程：

```text
quic_worker thread
  └─ TqTunnelContext::OnReceive
       └─ TryHandleServerOpen()
            ├─ 解析 OPEN request
            ├─ DNS / ACL 过滤
            └─ std::thread(FinishServerOpenAfterDial).detach()
```

### 4.3 Server 目标 TCP 拨号与 relay

```text
dial worker thread
  └─ FinishServerOpenAfterDial
       ├─ TqDialTcp(addrs, timeout)
       │    └─ 非阻塞 connect + poll/select 等待
       ├─ TqTuneTcpForThroughput(targetFd)
       ├─ SendOpenResponse
       └─ StartRelay(req.Flags)
```

relay 成功后，server 的数据路径与 client 对称：

| 方向 | 线程 | 路径 |
|------|------|------|
| 目标 TCP → QUIC | Linux relay worker | `epoll` 就绪 → `readv(targetFd)` → 可选压缩 → 多缓冲 `Stream->Send()` |
| QUIC → 目标 TCP | MsQuic callback + Linux relay worker | `QUIC_STREAM_EVENT_RECEIVE` → 拷贝入 ingress 池 → bounded event queue → worker 可选解压 → `writev(targetFd)` |

## 5. 每条隧道的线程与对象生命周期

一条完整隧道大致包含：

| 对象 / 线程 | 数量 | 生命周期 |
|-------------|------|----------|
| `TqTunnelContext` | 1 | OPEN 阶段创建并 `Register` 到 reaper，relay 结束后由全局 reaper 删除 |
| `MsQuicStream` wrapper | 1 | QUIC stream 生命周期 |
| Linux relay 注册 | 1 | `TqRelayStart()` 注册到 worker，`TqRelayStop()` 注销 |
| `TqLinuxRelayWorker` 分片 | W（tuning 决定） | 进程级 runtime，`TqLinuxRelayRuntime` 管理 |
| tunnel reaper | 全局 1 | `TqTunnelReaper::ReaperLoop()` 轮询所有已注册 context |
| server dial worker | server 侧 0 或 1 | OPEN request 后创建，拨号完成即退出 |
| handshake pool worker | client 侧固定 H | 复用处理本地 CONNECT/SOCKS 握手，任务完成后归还池 |

### 5.1 正常关闭

```text
TCP recv 返回 0 或 QUIC FIN/abort
  ├─ relay 设置 RelayHandle.Stop = true
  ├─ shutdown(TcpFd)
  ├─ SendCv.notify_all()
  ├─ 全局 reaper 观察到 Stop
  ├─ TqRelayStop()
  │    ├─ worker->UnregisterRelay(relayId)
  │    ├─ DetachStreamCallback()
  │    └─ FlushRuntimeSample / FlushCompressionSample
  ├─ CloseTcp()
  └─ delete TqTunnelContext（reaper Unregister）
```

### 5.2 回调解绑

`TqRelayStop()` 会调用 `DetachStreamCallback()`，将 stream callback 替换成 `MsQuicStream::NoOpCallback`，避免 relay 对象释放后 MsQuic 后续事件访问旧 context。

## 6. 数据路径与线程切换

### 6.1 TCP → QUIC 发送路径

```text
Linux relay worker
  ├─ epoll 就绪 → readv(TcpFd, pooled buffers)
  ├─ 可选压缩 → 多缓冲 TqBufferView
  ├─ Stream->Send(...)
  └─ SEND_COMPLETE → 释放池化缓冲并继续 pump
```

`Stream->Send()` 的真实组包与 UDP `sendmsg()` 是否在当前线程完成，取决于 msquic 的 worker 归属：

- 如果当前线程不是连接所属 `quic_worker`，MsQuic 会把 API send 操作投递到连接 worker。
- 后续 `QUIC_STREAM_EVENT_SEND_COMPLETE` 在 `quic_worker` 回调线程触发。
- relay worker 在 SEND_COMPLETE 中释放缓冲并继续处理同 relay 的 TCP 可读事件。

### 6.2 QUIC → TCP 接收路径

```text
MsQuic quic_worker thread
  └─ QUIC_STREAM_EVENT_RECEIVE
       └─ TqLinuxRelayWorker::OnStreamEvent
            ├─ 拷贝 receive buffer 到 worker 池
            ├─ 可选 Decompress
            └─ 入队 QUIC receive 事件 → 唤醒 owner worker

Linux relay worker
  └─ ProcessQuicReceiveEvent
       └─ writev(TcpFd, pooled views)
```

优化后 QUIC 回调只做拷贝与入队，**阻塞 TCP writev 发生在 owner relay worker 线程**，不再占用 `quic_worker`，也不再为每条隧道创建 TcpWriter 线程。

## 7. 线程数量估算

设：

```text
N = CxPlatProcCount()，通常约等于逻辑 CPU 数
C = --quic-connections
L = 当前活跃隧道数
H = handshake 线程池 worker 数（默认 8，`--handshake-threads` 或 0=auto）
A = 正在握手队列中等待的 accepted TCP 数（≤ H 并发执行）
D = server 正在拨号的 OPEN 请求数
W = Linux relay worker 分片数（tuning 决定，通常 ≪ L）
```

### 7.1 Client 模式

稳定状态粗略估算：

```text
1 主线程
+ N cxplat_worker
+ N quic_worker
+ 2 MsQuic 辅助线程左右
+ 2 listener 线程
+ H handshake pool worker
+ 1 tunnel reaper
+ W Linux relay worker
```

即：

```text
client_threads ≈ 1 + 2N + 2 + 2 + H + 1 + W
```

如果 N=20、L=100、H=8、W=4，则约：

```text
1 + 40 + 2 + 2 + 8 + 1 + 4 = 58 线程
```

相较 per-tunnel blocking relay（≈ 2L 隧道线程 + cleanup watcher），**Linux worker 分片将 relay 侧线程从 O(L) 降为 O(W)**。

### 7.2 Server 模式

稳定状态粗略估算：

```text
1 主线程
+ N cxplat_worker
+ N quic_worker
+ 2 MsQuic 辅助线程左右
+ 1 tunnel reaper
+ W Linux relay worker
+ D dial worker
```

即：

```text
server_threads ≈ 1 + 2N + 2 + 1 + W + D
```

如果 N=20、L=100、W=4、D≈0，则约：

```text
1 + 40 + 2 + 1 + 4 = 48 线程
```

实际线程数会随 Registration close worker、系统库线程、短生命周期 handler/dial 线程略有波动。

## 8. 与 secnetperf 线程模型的主要差异

| 项目 | secnetperf | tcpquic-proxy |
|------|------------|---------------|
| 执行 profile | CLI `-exec:` 可选 | CLI `--quic-profile` 可选 max-throughput / low-latency |
| 工具级 WorkerPool | 有，通常多一套 `cxplat_worker` | 无 |
| 应用发送线程 | `Perf Worker -threads:N` | W 个 Linux relay worker 分片 epoll/readv 多路复用 |
| 应用接收处理 | perf 回调较轻，统计为主 | QUIC 回调拷贝入池，blocking `writev()` 在 owner relay worker |
| 连接/stream 数 | 压测参数控制 | 1 TCP 连接 = 1 QUIC 双向 stream |
| 并发模型 | 少量 perf worker 驱动大量 stream | relay 侧线程数随 W 而非 L 增长 |

## 9. 性能风险点

### 9.1 QUIC 回调与 TCP 写路径（已优化）

MsQuic `quic_worker` 上的 stream 回调只做 receive 拷贝与事件入队；阻塞 `writev()` 在 owner `TqLinuxRelayWorker` 线程执行。慢 TCP 后端不再直接占用 `quic_worker`。

剩余风险：worker 侧 TCP 写缓冲积压或池化缓冲耗尽时，仍可能通过背压间接影响接收侧；高并发需关注 `TqLinuxRelayBufferPool` 预算与 worker fairness tick。

### 9.2 relay worker 分片与公平性

Linux 生产路径不再为每条隧道创建 relay 线程。所有 relay TCP fd 由固定数量 worker 分片 epoll 多路复用；单 worker 上 relay 过多时需关注 fairness budget 与 wake coalescing。

### 9.3 handshake 线程池容量

client 的 SOCKS5 / HTTP listener 将 accept 的 fd 提交到 `TqThreadPool`。默认 8 worker 时，突发 100 并发握手会在队列中排队；压测脚本默认 `--handshake-threads 32` 以保证 100 隧道回归稳定。生产环境应按峰值并发调大 `--handshake-threads`。

### 9.4 relay worker 内存

relay worker 按 tuning 为每个 worker 维护池化缓冲，并按 active relays / memory budget 缩放。高并发时必须关注 `--max-memory-mb` 和 worker pool 上限，避免缓冲池随隧道数线性膨胀。

## 10. 建议优化方向

| 优先级 | 方向 | 状态 | 说明 |
|--------|------|------|------|
| 高 | 保持 `MAX_THROUGHPUT` 作为 WAN / 高吞吐默认 | ✅ 默认 | 能把 datapath 与 QUIC 协议线程分离 |
| 高 | 继续避免在 OPEN 回调中做 DNS / TCP connect | ✅ 已有 | server 已派发 dial worker |
| 中 | QUIC→TCP 改 worker writev | ✅ DONE | `TqLinuxRelayWorker` copy-into-pool + writev |
| 中 | cleanup watcher 合并为集中 reaper | ✅ DONE | `TqTunnelReaper` 全局单线程 |
| 中 | accept handler 改线程池 | ✅ DONE | `TqThreadPool` handshake pool |
| 中 | 按 profile 支持 LOW_LATENCY / MAX_THROUGHPUT 切换 | ✅ DONE | `--quic-profile max-throughput\|low-latency` |
| 低 | relay TCP→QUIC 改 epoll 多路复用 | ✅ DONE | Linux 生产路径：`TqLinuxRelayWorker` + epoll + readv |
| 低 | relay QUIC→TCP 改 worker writev | ✅ DONE | 池化缓冲 + writev，无 per-tunnel TcpWriter |
| 低 | 压缩隧道走 worker inline 压缩 | ✅ DONE | 含 zstd/lz4 的生产 relay 均注册到 worker |

## Linux relay worker 生产状态

Linux 生产 relay 仅走 `TqLinuxRelayWorker`：owner worker 通过 `epoll`、`eventfd`、`readv`、池化缓冲、多缓冲 `StreamSend` 与 worker 侧 `writev` 处理全部隧道（含压缩）。`TqRelayStop()` 只注销 worker relay，不删除 blocking demo 对象。

旧 blocking relay 已重命名为 `TqBlockingDemoRelay`，仅存在于 demo/test target，不参与 `tcpquic-proxy` 链接。

Windows 仍沿用本阶段之前的实现路径；IOCP、WSAPoll、WSARecv、WSASend 不在 Linux worker 方案范围内。

## 11. 调试线程角色

Linux 下 `/proc/<pid>/task/*/comm` 往往都显示进程名，建议用以下方式识别：

```bash
gdb -p <pid>
thread apply all bt
```

典型栈特征：

| 栈特征 | 角色 |
|--------|------|
| `CxPlatWorkerThread` → `epoll_wait` | MsQuic datapath `cxplat_worker` |
| `QuicWorkerThread` → `QuicWorkerLoop` | MsQuic QUIC 协议 worker |
| `RunSocks5Server` → `accept` | SOCKS5 listener |
| `RunHttpConnectServer` → `accept` | HTTP CONNECT listener |
| `TqThreadPool::WorkerLoop` | handshake 线程池 worker |
| `TqHandleSocks5Client` / `TqHandleHttpConnectClient` | 本地 TCP 握手（在线程池中执行） |
| `FinishServerOpenAfterDial` / `TqDialTcp` | server 目标 TCP 拨号线程 |
| `TqLinuxRelayWorker::Run` → `epoll_wait` | Linux relay worker 分片 |
| `TqLinuxRelayWorker::DrainTcpReadable` | worker 侧 TCP→QUIC readv |
| `TqLinuxRelayWorker::FlushTcpWrites` | worker 侧 QUIC→TCP writev |
| `TqTunnelReaper::ReaperLoop` | 全局 tunnel 清理 reaper |
| `TqLinuxRelayWorker::OnStreamEvent` | QUIC worker 回调（拷贝入池，不阻塞 writev） |
| `TqBlockingDemoRelay::TcpToStreamLoop` | demo/legacy：每隧道 blocking relay（非 Linux 生产） |
| `TqTcpWriteQueue::WriterLoop` | demo/legacy：每隧道 QUIC→TCP writer（非 Linux 生产） |

## 12. 关键源文件索引

| 文件 | 说明 |
|------|------|
| `main.cpp` | client/server 入口、handshake pool、reaper guard、listener 线程 |
| `quic_session.cpp` | MsQuic Registration/Configuration、profile 选择、连接/stream 回调 |
| `thread_pool.cpp` | handshake 固定大小线程池 |
| `socks5_server.cpp` | SOCKS5 accept loop、提交 handshake pool |
| `http_connect_server.cpp` | HTTP CONNECT accept loop、提交 handshake pool |
| `tcp_tunnel.cpp` | OPEN 状态机、server dial worker、reaper 注册 |
| `tunnel_reaper.cpp` | 全局 tunnel 清理 reaper |
| `linux_relay_worker.cpp` | Linux 生产 relay：epoll/readv/writev、压缩/解压 |
| `linux_relay_buffer_pool.cpp` | worker 池化缓冲 |
| `relay.cpp` | 生产 `TqRelayStart()` / `TqRelayStop()`，Linux worker 选择 |
| `relay_blocking_demo.cpp` | demo/legacy `TqBlockingDemoRelay`（非生产链接） |
| `tcp_write_queue.cpp` | demo/legacy QUIC→TCP 写队列（非 Linux 生产） |
| `tcp_dialer.cpp` | server 侧非阻塞 TCP connect、socket buffer 调优 |
| `tuning.cpp` | 自适应 tuning、relay memory budget、runtime samples |

## 13. 一句话总结

`tcpquic-proxy` Linux 生产线程模型：MsQuic profile 可切换（默认 `MAX_THROUGHPUT`），relay 由固定数量 `TqLinuxRelayWorker` 分片通过 epoll/readv/writev 多路复用全部隧道（含压缩），handshake 走固定线程池，隧道清理由全局 `TqTunnelReaper` 单线程回收。旧 `TqBlockingDemoRelay` / `TqTcpWriteQueue` 仅保留在 demo/legacy target 供对比，不参与生产链接。
