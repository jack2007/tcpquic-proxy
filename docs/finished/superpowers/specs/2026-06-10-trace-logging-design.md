# tcpquic-proxy 调试日志（Trace）设计方案

> 日期：2026-06-10  
> 状态：已实现（待联调验证）

## 背景

`tcpquic-proxy` 在手工调试 client/server 连接时，现有 `stderr` 输出仅覆盖启动信息与少量错误，无法回答以下问题：

- QUIC 连接何时发起、何时成功、为何断开？
- 每条 QUIC 连接上的 RTT、丢包、拥塞、字节量等运行时指标如何？
- SOCKS5 / HTTP CONNECT 代理端 TCP 连接何时建立/关闭，当前活跃数是多少？
- 隧道 stream（OPEN / OPEN_OK / OPEN_FAIL）与目标 TCP 拨号各处于什么状态？
- 连接建立后，各层协议报文（应用层控制消息）累计数量如何变化？

本次设计引入 **事件驱动 + 可配置周期快照** 的 Trace 模块，使用 **spdlog** 输出到文件，专门服务于问题调试，不影响默认（未开启 trace）路径的性能。

## 目标

1. **QUIC 连接生命周期**：记录 connecting / incoming / connected / shutdown / disconnected，并在 connected/disconnected 时附带 `QUIC_STATISTICS_V2` 详细指标。
2. **应用层隧道协议**：记录 stream 创建、OPEN 结果、relay 启动/关闭，以及 OPEN/OPEN_OK/OPEN_FAIL 收发计数（全局 + 按 QUIC 连接）。
3. **代理端 TCP（client 侧）**：SOCKS5 / HTTP CONNECT 的 accept、tunnel 成败、close，以及当前活跃连接数。
4. **目标端 TCP（server 侧）**：dialing / connected / failed / closed，以及当前活跃目标 TCP 数。
5. **周期快照**：保留每 N 秒（默认 10，CLI 可配）的全局计数 + 每条活跃 QUIC 连接的指标 dump。
6. **日志基础设施**：spdlog 写文件；client → `<exe>/log/client.log`，server → `<exe>/log/server.log`；关闭 trace 时零开销（早返回）。

## 非目标

- 不替代现有 `stderr` 启动/致命错误输出（trace 是可选调试通道）。
- 不对 relay 数据面每个 read/write 打日志（性能影响过大）。
- 不记录 MsQuic 底层每一帧 QUIC 包类型（MsQuic 未直接暴露按 Initial/Handshake/1-RTT 分类的计数 API；以 `QUIC_STATISTICS_V2` 聚合指标 + 应用层 OPEN 计数代替）。
- 不做日志轮转/远程采集/结构化 JSON 输出（后续可扩展）。

## 方案对比

| 方案 | 描述 | 优点 | 缺点 | 结论 |
|------|------|------|------|------|
| A. 直接 `fprintf` | 在各模块散落打印 | 实现快 | 难统一格式、线程安全差、无法文件 sink | ❌ 用户明确要求 spdlog |
| B. 中央 Trace 模块（推荐） | `runtime/trace.*` 统一 API，各层 hook 事件 | 格式一致、可开关、易扩展 | 需在各模块插入 hook | ✅ 采用 |
| C. MsQuic 回调全量日志 | 在 MsQuic 每个 event 打印 | QUIC 层极细 | 日志爆炸、性能差、仍缺 TCP/代理层 | ❌ 仅作 B 的补充 |

**推荐方案 B**：事件只在状态变迁时触发；周期线程仅做快照，不做高频轮询业务逻辑。

## 总体架构

```text
┌─────────────────────────────────────────────────────────────┐
│                        main.cpp                              │
│  --trace / --trace-interval / --trace-connect-on-start       │
│  TqTraceInit(mode, interval)  →  TqTraceShutdown()           │
└──────────────────────────┬──────────────────────────────────┘
                           │
┌──────────────────────────▼──────────────────────────────────┐
│                   runtime/trace.cpp                          │
│  • spdlog file sink (client.log / server.log)                │
│  • 全局 atomic 计数器 (TqGlobalCounters)                      │
│  • 每连接/每隧道状态 map (g_quicConnsById, g_tunnels)         │
│  • StatsThreadMain → DumpPeriodicStats()                     │
└─▲──────────▲──────────▲──────────▲──────────▲───────────────┘
  │          │          │          │          │
  │          │          │          │          └── ingress (SOCKS5/HTTP)
  │          │          │          └── tunnel/tcp_tunnel (stream/relay/target)
  │          │          └── protocol/quic_session (QUIC lifecycle)
  │          └── platform/exe_path (日志目录定位)
  └── config (CLI 开关)
```

### 线程与性能

- **热路径**：所有 `TqTrace*` 入口第一行检查 `TqTraceEnabled()`，未开启时直接返回（无锁、无 I/O）。
- **计数器**：全局计数使用 `std::atomic`；per-conn 计数在 `g_stateMu` 下更新，仅在 trace 开启时执行。
- **写日志**：`g_logMu` 保护 spdlog 写入；`flush_on(info)` 保证调试时日志及时落盘。
- **周期线程**：独立 `std::thread`，按秒 sleep，仅在 `--trace` 且 `interval > 0` 时启动；shutdown 时 join 并 dump 最后一轮。

## CLI 配置

| 参数 | 类型 | 默认 | 说明 |
|------|------|------|------|
| `--trace` | flag | off | 启用 trace（事件 + 周期快照） |
| `--trace-interval <sec>` | uint32 | 10 | 周期快照间隔；隐含开启 trace |
| `--trace-connect-on-start` | flag | off | client：启动时立即建 QUIC（便于调试）；隐含开启 trace |

配置字段（`TqConfig`）：

```cpp
bool Trace{false};
uint32_t TraceIntervalSec{10};
bool TraceConnectOnStart{false};
```

## 日志输出

### 路径

```
<可执行文件所在目录>/log/client.log   # TqMode::Client
<可执行文件所在目录>/log/server.log   # TqMode::Server
```

由 `platform/exe_path.cpp::TqGetExecutableDirectory()` 解析；目录不存在时自动 `create_directories`。

### 格式

```
[YYYY-MM-DD HH:MM:SS.mmm] [info] event=<name> key=value ... global: quic_conns=... streams=...
```

- 主行：`event=` 标识事件类型，键值对便于 grep。
- 每条事件末尾附带 `TqTraceGlobalSnapshot()`，便于关联当前全局状态。
- QUIC 指标块以缩进子行输出（`quic_metrics:` / `quic_final_metrics:`）。

### spdlog 集成

- Submodule：`third_party/spdlog`
- CMake：`SPDLOG_BUILD_SHARED OFF`，链接 `spdlog::spdlog`
- Sink：`basic_file_sink_mt`，truncate=false（追加写）

## 事件清单

### 1. QUIC 连接层（`quic_session.cpp`）

| event | 触发时机 | 关键字段 |
|-------|----------|----------|
| `trace_started` | `TqTraceInit` | role, log, interval |
| `trace_stopped` | `TqTraceShutdown` | — |
| `quic_connecting` | client `Connection->Start()` 前 | role, slot, peer |
| `quic_incoming` | server `NEW_CONNECTION` | role=server |
| `quic_connected` | `CONNECTED` 回调 | conn, slot, local, peer + quic_metrics |
| `quic_shutdown_transport` | 传输层发起关闭 | conn, status, code |
| `quic_shutdown_peer` | 对端发起关闭 | conn, code |
| `quic_disconnected` | `SHUTDOWN_COMPLETE` | conn, lifetime, streams_opened + quic_final_metrics |

**connId 分配**：

- Server：MsQuic 回调中的连接序号（现有逻辑）。
- Client：`g_clientTraceConnIds` 映射 `HQUIC → uint32_t`，connected 注册、shutdown_complete 注销；`TqLookupClientTraceConnId()` 供 tunnel 层查询。

### 2. 应用层隧道 / Stream（`tcp_tunnel.cpp`）

| event | 触发时机 | 关键字段 |
|-------|----------|----------|
| `stream_started` | 新建 tunnel context | conn, tunnel, target, compress |
| `open_result` | 收到/发出 OPEN_OK 或 OPEN_FAIL | tunnel, ok, error, conn_id_field |
| `relay_started` | 双向 relay 开始 | tunnel |
| `stream_closed` | tunnel context 销毁 | tunnel, target, relay, duration, reason |

**OPEN 报文计数**（不在每次 send/recv 打日志，仅 atomic++）：

| 计数器 | 递增点 |
|--------|--------|
| `open_tx` / `open_rx` | client SendOpenRequest / server TryHandleServerOpen |
| `open_ok_tx/rx`, `open_fail_tx/rx` | CompleteOpen / OPEN 响应路径（trace 模块内按 conn 聚合） |

### 3. 代理端 TCP（`socks5_server.cpp` / `http_connect_server.cpp`）

| event | 触发时机 | 关键字段 |
|-------|----------|----------|
| `proxy_tcp_accepted` | accept 成功 | proto, fd, peer |
| `proxy_tunnel_ok` | tunnel 建立成功 | proto, target, tunnel |
| `proxy_tunnel_fail` | tunnel 建立失败 | proto, target, error |
| `proxy_tcp_closed` | 代理 TCP 关闭 | proto, fd |

`proto` ∈ `socks5` | `http`；对应 `socksActive` / `httpActive` 全局计数。

### 4. 目标端 TCP（server，`tcp_tunnel.cpp`）

| event | 触发时机 | 关键字段 |
|-------|----------|----------|
| `target_tcp_dialing` | 开始 connect 目标 | tunnel, target |
| `target_tcp_connected` | connect 成功 | tunnel, fd |
| `target_tcp_failed` | connect 失败 | tunnel, error |
| `target_tcp_closed` | 目标 TCP 关闭 | tunnel |

### 5. 周期快照

| event | 触发时机 | 内容 |
|-------|----------|------|
| `stats_tick` | 每 interval 秒 | 全局 snapshot 一行 |
| `stats_quic` | 同上，每条活跃 conn | conn 元数据 + OPEN 计数 + quic_metrics 块 |

Shutdown 时额外 dump 一轮 `stats_tick` + 各 conn 的 `stats_quic`。

## 全局 Snapshot 字段

```
global: quic_conns=N streams=N relays=N socks=N http=N target_tcp=N
        open_ok=N open_fail=N
        open_tx=N open_rx=N open_ok_tx=N open_ok_rx=N open_fail_tx=N open_fail_rx=N
```

## QUIC 详细指标（`QUIC_STATISTICS_V2`）

周期 dump 与 connected/disconnected 事件共用 `FormatQuicStatsLines()`：

```
rtt: cur=...ms min=...ms max=...ms var=...ms
bytes: tx=... rx=... stream_tx=... stream_rx=...
pkts: tx=... rx=... lost=... spurious_lost=... reordered=... decrypt_fail=...
congestion_events=... cwnd=... mtu=...
```

> **说明**：MsQuic 统计的是聚合包/字节/RTT/拥塞，不是 RFC9000 帧类型明细。应用层 OPEN/OPEN_OK/OPEN_FAIL 计数单独统计，作为「连接上控制报文类型数量」的代理指标。

## Hook 点与文件归属

| 模块 | 文件 | 职责 |
|------|------|------|
| 入口 | `main.cpp` | `TqTraceInit` / `TqTraceGuard`；`TraceConnectOnStart` |
| QUIC | `quic_session.cpp` | 连接生命周期 + client connId 映射 |
| 隧道 | `tcp_tunnel.cpp` | stream/relay/target TCP + OPEN 计数 |
| SOCKS5 | `socks5_server.cpp` | 代理 TCP 事件 |
| HTTP | `http_connect_server.cpp` | 代理 TCP 事件（与 SOCKS5 对称） |
| 配置 | `config.cpp` | CLI 解析 |
| 核心 | `runtime/trace.cpp` | 全部实现 |

### 数据结构传递

- `TunnelRequest::IngressTraceProto`：1=socks5, 2=http，client 建 tunnel 时传入。
- `TqTunnelStartResult::TraceTunnelId`：返回 tunnel id 供 ingress 打 `proxy_tunnel_ok`。
- `TqTunnelContext`：RAII 析构调用 `EmitTraceClosed()`，保证 stream 关闭事件不遗漏。

## 使用示例

**Server：**

```bash
sudo ./tcpquic-proxy server \
  --quic-listen 0.0.0.0:443 \
  --quic-cert ... --quic-key ... --quic-ca ... \
  --allow-targets 0.0.0.0/0 \
  --compress auto \
  --trace --trace-interval 10
# 日志: ./log/server.log
```

**Client：**

```bash
./tcpquic-proxy client \
  --quic-peer 172.16.10.80:443 \
  --quic-cert ... --quic-key ... --quic-ca ... \
  --socks-listen 127.0.0.1:1080 \
  --http-listen 127.0.0.1:8080 \
  --compress auto \
  --trace --trace-interval 10 --trace-connect-on-start
# 日志: ./log/client.log
```

**典型 grep：**

```bash
grep 'event=quic_connected' log/client.log
grep 'event=proxy_tunnel_fail' log/client.log
grep 'event=stats_tick' log/server.log
grep 'open_fail' log/server.log
```

## 实现状态（2026-06-10）

| 项 | 状态 |
|----|------|
| spdlog submodule + CMake | ✅ |
| `runtime/trace.cpp` 核心 | ✅ |
| `config` CLI 参数 | ✅ |
| `quic_session.cpp` hook | ✅ |
| `tcp_tunnel.cpp` hook | ✅ |
| `socks5_server.cpp` hook | ✅ |
| `http_connect_server.cpp` hook | ✅ |
| `main.cpp` 调用 `TqTraceInit` | ✅ |
| 编译验证 | ✅ Release 构建通过 |
| 端到端联调 | ✅ 本机 127.0.0.1 冒烟通过（SOCKS5 + HTTP CONNECT `--proxytunnel`） |

## 测试计划

1. **编译**：Linux Release 构建通过，spdlog 正确链接。
2. **关闭 trace**：无 `--trace` 时无 log 文件生成，proxy 功能与基线一致。
3. **Client 连接**：`quic_connecting` → `quic_connected` 顺序正确；`log/client.log` 路径正确。
4. **Server 入站**：`quic_incoming` → `quic_connected`；周期 `stats_tick` 每 10s 出现。
5. **SOCKS5 隧道**：`proxy_tcp_accepted` → `stream_started` → `open_result ok=1` → `relay_started` → 关闭时 `stream_closed` / `proxy_tcp_closed`。
6. **失败路径**：ACL 拒绝 / TCP refused 时 `open_result ok=0` 与 `proxy_tunnel_fail` 错误码正确。
7. **Shutdown**：进程退出时 `trace_stopped` 与最终 stats dump 写入。

## 后续扩展（可选）

- `--trace-log-dir` 自定义日志目录
- spdlog rotating_file_sink 按大小轮转
- 将 `event=` 行改为 JSON 便于 ELK/Loki
- Admin HTTP 暴露当前 global snapshot（只读）

## 参考

- MsQuic：`QUIC_STATISTICS_V2`、`QUIC_CONNECTION_EVENT_*`
- 应用协议：`src/protocol/control_protocol.h`（TQ_CMD_OPEN / OPEN_OK / OPEN_FAIL）
- 现有实现：`src/runtime/trace.h`、`src/runtime/trace.cpp`
