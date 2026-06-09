# tcpquic-proxy 运行线程模型

本文档参考 `docs/thread_model_cn.md` 的 msquic 线程模型说明，并结合 `src/tools/tcpquic-proxy/` 当前源码，描述 `tcpquic-proxy` 在 client / server 两种模式下的应用线程、MsQuic worker、隧道 relay 线程以及回调线程边界。

## 1. 总体结论

`tcpquic-proxy` 是 **应用自建线程 + MsQuic MAX_THROUGHPUT worker + 每隧道 relay 线程** 的混合模型：

- MsQuic 协议执行使用 `QUIC_EXECUTION_PROFILE_TYPE_MAX_THROUGHPUT`，因此会创建独立 `quic_worker` 线程。
- UDP datapath 仍由 msquic 库级 `cxplat_worker` 处理。
- client 模式额外创建 2 个监听线程：SOCKS5 listener 和 HTTP CONNECT listener。
- 每个 accepted 本地 TCP 连接由固定大小的 **handshake 线程池**（`TqThreadPool`，默认 8 worker，`--handshake-threads` 可配）处理 SOCKS5 / HTTP CONNECT 握手和 OPEN 阶段，不再为每个连接 detached 一个 handler 线程。
- 每条已建立隧道会创建 1 个 `TqTunnelRelay::TcpThread`，负责 **TCP → QUIC Stream** 方向的阻塞 `recv()` 与 `Stream->Send()`。
- **QUIC Stream → TCP** 方向在 MsQuic stream 回调线程上只做入队：`OnStreamReceive()` 将数据写入 `TqTcpWriteQueue`，由独立的 **TcpWriter 线程** 阻塞 `send()` 到 TCP fd，避免慢 TCP 写阻塞 `quic_worker`。
- 隧道清理由进程级 **全局 reaper**（`TqTunnelReaper`，单线程轮询）统一回收 context，不再为每条隧道创建 cleanup watcher。
- server 模式没有 SOCKS5 / HTTP CONNECT 监听线程，但每条入站 QUIC stream 会创建拨号线程和 relay 线程。
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
| relay TCP thread | 有 | 有 | `TqTunnelRelay::Start()` | 每条隧道一个线程，负责 TCP→QUIC 方向 |
| QUIC→TCP writer | 有 | 有 | `TqTcpWriteQueue::Start()` | 每条隧道一个 writer 线程，从队列阻塞写 TCP fd |
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
            ├─ 设置 StreamCallback = TqTunnelRelay::StreamCallback
            ├─ fast path 预分配 send context pool
            └─ std::thread(&TqTunnelRelay::TcpToStreamLoop)
```

数据方向分为两条线程路径：

| 方向 | 线程 | 路径 |
|------|------|------|
| 本地 TCP → QUIC | relay `TcpThread` | `recv(tunnelFd)` → 可选压缩 → `Stream->Send()` |
| QUIC → 本地 TCP | relay `TcpWriter` 线程 | `QUIC_STREAM_EVENT_RECEIVE` → 入队 `TqTcpWriteQueue` → `send(tunnelFd)` |

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
| 目标 TCP → QUIC | relay `TcpThread` | `recv(targetFd)` → 可选压缩 → `Stream->Send()` |
| QUIC → 目标 TCP | relay `TcpWriter` 线程 | `QUIC_STREAM_EVENT_RECEIVE` → 入队 `TqTcpWriteQueue` → `send(targetFd)` |

## 5. 每条隧道的线程与对象生命周期

一条完整隧道大致包含：

| 对象 / 线程 | 数量 | 生命周期 |
|-------------|------|----------|
| `TqTunnelContext` | 1 | OPEN 阶段创建并 `Register` 到 reaper，relay 结束后由全局 reaper 删除 |
| `MsQuicStream` wrapper | 1 | QUIC stream 生命周期 |
| relay `TcpThread` | 1 | `TqRelayStart()` 创建，`TqRelayStop()` join |
| relay `TcpWriter` | 1 | `TqTcpWriteQueue` writer 线程，`TqRelayStop()` 时 join |
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
  │    ├─ DetachStreamCallback()
  │    ├─ Stop TcpWriter 并 join
  │    ├─ join TcpThread
  │    └─ FlushRuntimeSample / FlushCompressionSample
  ├─ CloseTcp()
  └─ delete TqTunnelContext（reaper Unregister）
```

### 5.2 回调解绑

`TqRelayStop()` 会调用 `DetachStreamCallback()`，将 stream callback 替换成 `MsQuicStream::NoOpCallback`，避免 relay 对象释放后 MsQuic 后续事件访问旧 context。

## 6. 数据路径与线程切换

### 6.1 TCP → QUIC 发送路径

```text
relay TcpThread
  ├─ recv(TcpFd, ...)
  ├─ fast path: 直接 recv 到 TqRelaySendContext::Data
  ├─ SendContextToStream
  │    └─ Stream->Send(...)
  └─ FillSendPipeline
       └─ MSG_DONTWAIT 继续 pump，直到 in-flight / ideal send 达上限
```

`Stream->Send()` 的真实组包与 UDP `sendmsg()` 是否在当前线程完成，取决于 msquic 的 worker 归属：

- 如果当前线程不是连接所属 `quic_worker`，MsQuic 会把 API send 操作投递到连接 worker。
- 后续 `QUIC_STREAM_EVENT_SEND_COMPLETE` 在 `quic_worker` 回调线程触发。
- relay 在 SEND_COMPLETE 中减少 `OutstandingBytes` / `InFlightSends` 并唤醒 `TcpThread`。

### 6.2 QUIC → TCP 接收路径

```text
MsQuic quic_worker thread
  └─ QUIC_STREAM_EVENT_RECEIVE
       └─ TqTunnelRelay::OnStreamReceive
            ├─ 可选 Decompress
            └─ TcpWriter->Enqueue(...)
                 └─ 唤醒 relay TcpWriter 线程

relay TcpWriter thread
  └─ TqTcpWriteQueue::WriterLoop
       └─ TqWriteAll(TcpFd) → send(TcpFd, ..., MSG_NOSIGNAL)
```

优化后 QUIC 回调只做入队，**阻塞 TCP send 发生在独立 TcpWriter 线程**，不再占用 `quic_worker`。

## 7. 线程数量估算

设：

```text
N = CxPlatProcCount()，通常约等于逻辑 CPU 数
C = --quic-connections
L = 当前活跃隧道数
H = handshake 线程池 worker 数（默认 8，`--handshake-threads` 或 0=auto）
A = 正在握手队列中等待的 accepted TCP 数（≤ H 并发执行）
D = server 正在拨号的 OPEN 请求数
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
+ L relay TcpThread
+ L TcpWriter 线程
```

即：

```text
client_threads ≈ 1 + 2N + 2 + 2 + H + 1 + 2L
```

如果 N=20、L=100、H=8，则约：

```text
1 + 40 + 2 + 2 + 8 + 1 + 200 = 254 线程
```

相较优化前（每隧道 relay + cleanup watcher ≈ 2L 隧道线程），**去掉 L 个 cleanup watcher，净减少约 L−1 个线程**（100 隧道时约少 99 个）。

### 7.2 Server 模式

稳定状态粗略估算：

```text
1 主线程
+ N cxplat_worker
+ N quic_worker
+ 2 MsQuic 辅助线程左右
+ 1 tunnel reaper
+ L relay TcpThread
+ L TcpWriter 线程
+ D dial worker
```

即：

```text
server_threads ≈ 1 + 2N + 2 + 1 + 2L + D
```

如果 N=20、L=100、D≈0，则约：

```text
1 + 40 + 2 + 1 + 200 = 244 线程
```

实际线程数会随 Registration close worker、系统库线程、短生命周期 handler/dial 线程略有波动。

## 8. 与 secnetperf 线程模型的主要差异

| 项目 | secnetperf | tcpquic-proxy |
|------|------------|---------------|
| 执行 profile | CLI `-exec:` 可选 | CLI `--quic-profile` 可选 max-throughput / low-latency |
| 工具级 WorkerPool | 有，通常多一套 `cxplat_worker` | 无 |
| 应用发送线程 | `Perf Worker -threads:N` | 每隧道 1 个 relay `TcpThread` |
| 应用接收处理 | perf 回调较轻，统计为主 | QUIC 回调入队，blocking `send()` 在 TcpWriter 线程 |
| 连接/stream 数 | 压测参数控制 | 1 TCP 连接 = 1 QUIC 双向 stream |
| 并发模型 | 少量 perf worker 驱动大量 stream | 大量 TCP 隧道会线性增加应用线程 |

## 9. 性能风险点

### 9.1 QUIC 回调与 TCP 写路径（已优化）

`OnStreamReceive()` 在 MsQuic `quic_worker` 上只做解压与 `TcpWriter->Enqueue()`，阻塞 `send()` 已移到 `TqTcpWriteQueue` writer 线程。慢 TCP 后端不再直接占用 `quic_worker`。

剩余风险：入队队列满或 writer 线程阻塞时，仍可能通过背压间接影响接收侧；高并发需关注 `TqTcpWriteQueue` 的 `maxChunks` / `maxBytes` 上限。

### 9.2 每隧道两个数据路径线程

每条活跃隧道有：

```text
relay TcpThread（TCP→QUIC）+ TcpWriter 线程（QUIC→TCP）
```

100 并发时数据路径约 200 个线程。相较优化前，**不再额外增加 per-tunnel cleanup watcher**；清理由全局单线程 reaper 承担。

### 9.3 handshake 线程池容量

client 的 SOCKS5 / HTTP listener 将 accept 的 fd 提交到 `TqThreadPool`。默认 8 worker 时，突发 100 并发握手会在队列中排队；压测脚本默认 `--handshake-threads 32` 以保证 100 隧道回归稳定。生产环境应按峰值并发调大 `--handshake-threads`。

### 9.4 relay fast path 内存

fast path 会按 tuning 为每条 relay 预分配 send context pool，并按 active relays / memory budget 缩放。高并发时必须关注 `--max-memory-mb` 和 `RelayMaxFreeSendContexts`，避免 64 × 1MiB × 隧道数造成内存压力。

## 10. 建议优化方向

| 优先级 | 方向 | 状态 | 说明 |
|--------|------|------|------|
| 高 | 保持 `MAX_THROUGHPUT` 作为 WAN / 高吞吐默认 | ✅ 默认 | 能把 datapath 与 QUIC 协议线程分离 |
| 高 | 继续避免在 OPEN 回调中做 DNS / TCP connect | ✅ 已有 | server 已派发 dial worker |
| 中 | QUIC→TCP 改异步 writer | ✅ DONE | `TqTcpWriteQueue` + per-tunnel TcpWriter 线程 |
| 中 | cleanup watcher 合并为集中 reaper | ✅ DONE | `TqTunnelReaper` 全局单线程 |
| 中 | accept handler 改线程池 | ✅ DONE | `TqThreadPool` handshake pool |
| 中 | 按 profile 支持 LOW_LATENCY / MAX_THROUGHPUT 切换 | ✅ DONE | `--quic-profile max-throughput\|low-latency` |
| 低 | relay TCP→QUIC 改 epoll 多路复用 | ✅ Linux fast path | Linux 无压缩 relay 已走 owner worker + epoll |
| 低 | relay QUIC→TCP 改 worker writev | ✅ Linux fast path | 池化缓冲 + writev，替代 per-tunnel TcpWriter |

## Linux Phase 5 relay worker status

Linux fast-path relays now use owner workers with `epoll`, `eventfd`, `readv`, pooled buffers, batched `StreamSend`, and worker-owned `writev` for QUIC receive data. Compressed relays keep the previous blocking relay implementation because compression output still uses separate contiguous buffers and requires a later compression-pool design.

Windows remains on the existing path in this phase. IOCP, WSARecv, and WSASend are not part of this Linux implementation.

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
| `TqTunnelRelay::TcpToStreamLoop` | 每隧道 TCP→QUIC relay 线程 |
| `TqTcpWriteQueue::WriterLoop` | 每隧道 QUIC→TCP writer 线程 |
| `TqTunnelReaper::ReaperLoop` | 全局 tunnel 清理 reaper |
| `TqTunnelRelay::OnStreamReceive` → `Enqueue` | QUIC worker 回调（仅入队，不阻塞 send） |

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
| `tcp_write_queue.cpp` | QUIC→TCP 异步写队列与 writer 线程 |
| `relay.cpp` | 每隧道 TCP→QUIC 线程、QUIC→TCP 入队、send buffer pool |
| `tcp_dialer.cpp` | server 侧非阻塞 TCP connect、socket buffer 调优 |
| `tuning.cpp` | 自适应 tuning、relay memory budget、runtime samples |

## 13. 一句话总结

`tcpquic-proxy` 线程模型优化（阶段 1–4）已完成：MsQuic profile 可切换（默认 `MAX_THROUGHPUT`），每隧道 relay 线程负责阻塞 TCP 读，QUIC→TCP 经 `TqTcpWriteQueue` 异步写出，handshake 走固定线程池，隧道清理由全局 `TqTunnelReaper` 单线程回收（无 per-tunnel cleanup watcher）。100 隧道并发回归已通过。下一步可选方向是 relay TCP→QUIC 的 epoll 多路复用（未纳入本轮范围）。
