# tcpquic-proxy 内置测速工具设计方案

> 日期：2026-06-11  
> 状态：设计中

## 背景

`tcpquic-proxy` 当前只提供 TCP-over-QUIC 连接转换代理。速度测试需要额外准备 `curl` 一类客户端，并在 server 侧手工启动 Python HTTP server。这个流程不利于双机链路验证，也不方便在不同压缩、连接数、tuning 参数下重复跑端到端 I/O 测试。

本设计在 `tcpquic-proxy` 内置测速工具。client 通过命令行参数发起上传或下载测试；server 不增加命令行开关，而是由 client 通过 QUIC 控制指令请求 server 临时启动一个真实绑定的本机 TCP server。测试数据仍然通过现有 tunnel 数据面转发，覆盖完整路径：

```text
client 内置 TCP client
  -> client 侧 TCP socket
  -> TqStartClientTunnel / QUIC stream
  -> server 侧 TqHandleServerPeerStream
  -> server 侧真实 loopback TCP connect
  -> server 内置 TCP test server
```

## 目标

1. `tcpquic-proxy client --download-test <sec>` 发起下载测速，持续 `<sec>` 秒。
2. `tcpquic-proxy client --upload-test <sec>` 发起上传测速，持续 `<sec>` 秒。
3. server 默认具备测速能力，不通过 server 命令行参数启停；只有收到已认证 client 的控制指令时才临时启动测试 TCP server。
4. server 侧测试服务必须真正绑定本机 TCP 监听端口，client 再通过现有 tunnel OPEN 到该端口，保证测试覆盖端到端网络 I/O、relay、TCP 栈和 QUIC 数据面。
5. 测试完成后 client 打印本端统计和 server 回传统计，特别是 upload 场景要显示 server 实际接收字节数。
6. 设计保持跨平台：Linux 与 Windows 共享主要逻辑，socket 操作通过 `platform_socket.*` 和少量平台条件编译完成。

## 非目标

- 不替代 `iperf3` 的全部能力；本工具只提供项目内置的基础上传/下载吞吐测试。
- 不支持同时在一个进程内跑多个不同 client-config peer 的测速；首版仅支持单 peer client 模式。
- 不新增 server 侧持久测速端口或配置文件字段。
- 不引入 HTTP 协议作为测速数据格式；测试数据使用简单二进制流，降低解析开销。
- 不绕过现有 tunnel 数据面，也不在 server 侧直接把 QUIC stream 接到测试处理器。

## 方案对比

| 方案 | 描述 | 优点 | 缺点 | 结论 |
|------|------|------|------|------|
| A. 控制 stream + 临时 loopback TCP server | client 通过新控制指令请求 server 启动临时 TCP server，server 回传 loopback 端口，client 再用普通 tunnel OPEN 到该端口 | 控制面和数据面边界清晰；完整经过真实 TCP listen/connect；不污染普通 OPEN | 需要新增控制协议和测试 runtime | 采用 |
| B. 特殊目标地址触发 | client OPEN 到保留域名或端口，server 在普通 OPEN 路径识别测速 | 初期改动少 | 控制语义混入普通目标；与 ACL/DNS/trace 纠缠；结果回传困难 | 不采用 |
| C. 直接 QUIC stream 测速 | client/server 在 QUIC stream 上直接收发测试数据 | 实现简单，吞吐上限更直接 | 不覆盖 TCP listen/connect 与 relay，不满足端到端 I/O 要求 | 不采用 |

## 用户体验

### 命令行

下载测速：

```bash
tcpquic-proxy client \
  --quic-peer <server:443> \
  --quic-cert client.crt --quic-key client.key --quic-ca ca.crt \
  --download-test 10
```

上传测速：

```bash
tcpquic-proxy client \
  --quic-peer <server:443> \
  --quic-cert client.crt --quic-key client.key --quic-ca ca.crt \
  --upload-test 10
```

### 参数规则

| 参数 | 模式 | 默认 | 说明 |
|------|------|------|------|
| `--download-test <sec>` | client only | 0 | 下载测速持续秒数，`1..86400` |
| `--upload-test <sec>` | client only | 0 | 上传测速持续秒数，`1..86400` |

校验规则：

- `--upload-test` 与 `--download-test` 互斥。
- 仅单 peer client 模式支持测速；与 `--client-config` 互斥。
- 测速激活时不启动 SOCKS5 / HTTP CONNECT 监听；client 连接 QUIC、执行测速、打印结果后退出。
- 测速仍沿用 `--quic-connections`、`--compress`、`--compress-level`、`--tuning`、`--max-memory-mb` 等现有数据面参数。
- `--warmup-mb` 可继续保留原行为，但与测速同时使用时不建议首版支持；首版将其判为互斥，避免测试结果混入预热流量和外部 HTTP server 依赖。

### 输出

示例：

```text
tcpquic-proxy: speed test download duration=10s quic_connections=4 compress=off
tcpquic-proxy: speed test local bytes=12884901888 elapsed=10.002s throughput=10.31 Gbps
tcpquic-proxy: speed test server bytes=12884901888 elapsed=10.001s throughput=10.31 Gbps
```

upload 场景：

```text
tcpquic-proxy: speed test upload duration=10s quic_connections=4 compress=lz4
tcpquic-proxy: speed test local bytes=9663676416 elapsed=10.004s throughput=7.73 Gbps
tcpquic-proxy: speed test server bytes=9663600128 elapsed=10.006s throughput=7.73 Gbps
```

如果 client/server 字节数不一致，client 仍打印双方结果，并额外输出 warning。

## 总体架构

```text
src/main.cpp
  RunSinglePeerClient()
    if SpeedTest.Enabled:
      EnsureAnyConnected()
      TqRunClientSpeedTest(quic, cfg)
      exit

src/runtime/speed_test.*
  client runner
  server controller
  temporary loopback TCP server
  result aggregation

src/protocol/control_protocol.*
  existing OPEN protocol
  new speed-test control messages

src/protocol/quic_session.*
  existing peer stream callback
  server dispatches control streams vs tunnel streams
```

核心分工：

- **控制面**：QUIC bidirectional stream 上发送测速控制消息，用于创建测试 session、回传端口、结束并返回 server 统计。
- **数据面**：仍使用现有 `TqStartClientTunnel()` 创建普通 tunnel，目标地址是 server 回传的 loopback TCP 地址和端口。
- **server 测试 runtime**：收到控制请求后绑定 `127.0.0.1:0`，启动 accept 线程；测试结束或控制 stream 关闭后停止监听并回收 session。
- **client 测试 runner**：建立控制 stream，请求 server 准备，拿到 loopback 端口后创建本地 socket pair，把一端交给 tunnel，另一端作为内置 TCP client 读写测速数据。

## 控制协议

### 命令分配

现有命令：

```cpp
TQ_CMD_OPEN      = 0x01
TQ_CMD_OPEN_OK   = 0x02
TQ_CMD_OPEN_FAIL = 0x03
```

新增命令：

```cpp
TQ_CMD_SPEED_START   = 0x10
TQ_CMD_SPEED_READY   = 0x11
TQ_CMD_SPEED_FINISH  = 0x12
TQ_CMD_SPEED_RESULT  = 0x13
TQ_CMD_SPEED_ERROR   = 0x14
```

所有控制消息继续使用 `TQ_MAGIC_0`、`TQ_MAGIC_1`、`TQ_VERSION` 头，便于和现有控制协议保持一致。

### SPEED_START

client -> server。请求 server 启动临时 TCP test server。

```text
magic[2] version cmd
session_id:u32
direction:u8       # 1=download, 2=upload
duration_sec:u32
parallel:u16       # 首版等于 quic_connections，可扩展
flags:u8           # reserved, must be 0
```

server 校验：

- `duration_sec` 在 `1..86400`。
- `direction` 只能是 download/upload。
- `parallel` 在 `1..128`。
- flags 必须为 0。

### SPEED_READY

server -> client。表示 TCP test server 已绑定并开始监听。

```text
magic[2] version cmd
session_id:u32
addr_type:u8       # TQ_ADDR_IPV4 or TQ_ADDR_IPV6
port:u16
```

首版 server 默认绑定 IPv4 loopback `127.0.0.1:0`。如果后续需要 IPv6，可扩展为优先 IPv4、失败再尝试 IPv6。

### SPEED_FINISH

client -> server。client 本地测试循环结束，请求 server 停止 session 并返回统计。

```text
magic[2] version cmd
session_id:u32
client_bytes:u64
client_elapsed_us:u64
```

server 收到后停止 accept、关闭活跃测试 TCP 连接的发送/接收方向，等待短暂 drain，然后生成结果。

### SPEED_RESULT

server -> client。server 端统计结果。

```text
magic[2] version cmd
session_id:u32
server_bytes:u64
server_elapsed_us:u64
accepted_connections:u32
closed_connections:u32
status:u8          # 0=ok, non-zero=completed with warning
```

### SPEED_ERROR

server -> client 或 client 本地解码失败时使用。

```text
magic[2] version cmd
session_id:u32
error:u8
message_len:u16
message:bytes
```

错误码：

```cpp
enum class TqSpeedError : uint8_t {
    Ok = 0,
    InvalidRequest = 1,
    BindFailed = 2,
    ListenFailed = 3,
    Internal = 4,
    Timeout = 5,
    Unsupported = 6,
};
```

## 控制 stream 识别

`QuicServerSession` 当前把所有 peer stream 交给 `TqHandleServerPeerStream()`，后者默认等待 OPEN 请求。新增逻辑需要在 server 收到 stream 首包后判断命令：

- 如果 cmd 是 `TQ_CMD_OPEN`，沿用 `TqHandleServerPeerStream()`。
- 如果 cmd 是 `TQ_CMD_SPEED_START`，交给 `TqHandleServerSpeedControlStream()`。
- 其他命令关闭 stream，并尽量发送 `SPEED_ERROR`。

为了避免在两个 handler 中重复读首包，设计上新增一个轻量 dispatch context：

```cpp
void TqHandleServerIncomingStream(
    MsQuicConnection* conn,
    HQUIC rawStream,
    const TqAcl& acl,
    const TqConfig& cfg,
    TqServerSpeedTestController* speed,
    TqTunnelCompletionFn onComplete,
    TqTunnelAclDeniedFn onAclDenied);
```

该函数只负责收集足够的 header 字节并按 cmd 分派。普通 OPEN 分派时要把已收到的字节交给 tunnel context，避免丢失首包。实现上可以为 `TqTunnelContext` 增加 `FeedInitialBytes()`，或让 incoming stream dispatcher 在判断为 OPEN 后创建 tunnel context 并立即调用现有 receive 处理逻辑。

## Server 侧测试服务

### 生命周期

每个 SPEED_START 创建一个 `TqSpeedTestSession`：

```cpp
struct TqSpeedTestSession {
    uint32_t SessionId;
    TqSpeedDirection Direction;
    uint32_t DurationSec;
    uint16_t Parallel;
    TqSocketHandle ListenFd;
    uint16_t ListenPort;
    std::thread AcceptThread;
    std::vector<std::thread> WorkerThreads;
    std::atomic<bool> StopRequested;
    std::atomic<uint64_t> Bytes;
    std::atomic<uint32_t> Accepted;
    std::atomic<uint32_t> Closed;
    std::chrono::steady_clock::time_point Start;
    std::chrono::steady_clock::time_point End;
};
```

`TqServerSpeedTestController` 持有 session map：

- key：`session_id`。
- 创建：`StartSession()`
- 查询 ready：`ListenAddress()`
- 停止：`FinishSession()`
- 清理：控制 stream 关闭、测试超时、server 进程退出。

### 绑定地址

server 只绑定 loopback：

- 首选 `127.0.0.1:0`。
- 监听 backlog 至少 `parallel`，最低 16。
- 绑定成功后通过 `getsockname()` 获取实际端口。
- 不绑定 `0.0.0.0`，避免暴露测速端口到外部网络。

因为数据面 tunnel 的 server 侧 TCP dial 发生在同一台机器，client OPEN 到 `127.0.0.1:<port>` 即可命中这个临时 server。

### Download 行为

download 表示 server 发送、client 接收：

- accept 每个 TCP 连接后，worker 持续发送固定大小 buffer，直到 `StopRequested`、发送失败或对端关闭。
- buffer 使用可复用的伪随机或递增模式，默认 64 KiB，避免过小 syscall 开销。
- `Bytes` 统计成功写入 socket 的字节数。
- 测试 duration 由 client 计时，server 也可设置一个 `duration_sec + 30s` 兜底超时，防止控制 stream 丢失导致 session 泄漏。

### Upload 行为

upload 表示 client 发送、server 接收：

- accept 每个 TCP 连接后，worker 持续 `recv()` 并丢弃。
- `Bytes` 统计成功收到的字节数。
- 收到 FIN 或 StopRequested 后退出。

### 并发连接数

首版每条 QUIC connection 建一条测试 tunnel：

```text
parallel = cfg.QuicConnections
```

client 对每个 tunnel 创建一个 socket pair：

- 一端传给 `TqStartClientTunnel()`。
- 另一端由 client 测试 worker 读或写。

这样可以覆盖现有 connection pool 的 round-robin 选择与多 stream/多 connection 行为。后续可扩展 `--test-connections <n>`，但首版不增加新参数。

## Client 侧测试执行器

### 流程

```text
TqRunClientSpeedTest(quic, cfg):
  EnsureAnyConnected()
  Pick control connection
  Open control stream
  Send SPEED_START(session_id, direction, duration, parallel)
  Wait SPEED_READY(port)
  For i in 1..parallel:
    create socket pair
    PickConnection()
    TqStartClientTunnel(conn, req=127.0.0.1:port, fd=pair[0], cfg)
    start worker on pair[1]
  Sleep / deadline loop until duration expires
  stop workers, close pump sockets
  Send SPEED_FINISH(client_bytes, elapsed)
  Wait SPEED_RESULT
  print local + server results
```

### Download worker

- 循环 `recv()`，累加字节。
- 到 deadline 后关闭本地 pump socket，让 tunnel 和 server worker 自然退出。
- 统计第一个 worker 开始时间到所有 workers 结束时间；同时全局 runner 使用统一 deadline 输出主 elapsed。

### Upload worker

- 循环 `send()` 固定 buffer。
- 到 deadline 后调用 `TqShutdownSend()` 或关闭 socket，通知 server 侧 recv 结束。
- 累加本地成功发送字节数。

### 结果计算

吞吐：

```text
Gbps = bytes * 8 / elapsed_seconds / 1e9
MiB/s = bytes / elapsed_seconds / 1024 / 1024
```

client 输出：

- direction
- duration
- quic_connections
- compress mode
- local bytes / elapsed / Gbps / MiB/s
- server bytes / elapsed / Gbps / MiB/s
- accepted/closed connection count

## 配置结构

`TqConfig` 新增：

```cpp
enum class TqSpeedTestMode {
    None,
    Download,
    Upload,
};

TqSpeedTestMode SpeedTestMode{TqSpeedTestMode::None};
uint32_t SpeedTestDurationSec{0};
```

`TqPrintUsage()` 增加：

```text
  --download-test <sec>       Client: built-in end-to-end download speed test
  --upload-test <sec>         Client: built-in end-to-end upload speed test
```

校验放在 `TqParseArgs()`：

- 参数值必须能解析为 uint32 且大于 0。
- 两个测速参数互斥。
- server 模式禁止测速参数。
- `--client-config` 模式禁止测速参数。
- `--warmup-mb > 0` 与测速互斥。

## 与 ACL 的关系

临时 TCP server 绑定在 server 本机 loopback；client OPEN 目标是 `127.0.0.1:<port>`。现有 server 仍会对普通 OPEN 执行 ACL。

为避免用户必须显式 `--allow-targets 127.0.0.1/32` 才能测速，设计采用以下规则：

- 对于由 SPEED_START 创建的 session，controller 记录允许的 `(addr, port)`。
- OPEN 来自同一个 QUIC connection 且目标为这个精确 loopback `(addr, port)` 时，server tunnel 层允许绕过普通 ACL。
- 只绕过该 session 的临时端口，不开放任意 loopback 访问。
- session 结束后立即撤销该临时允许项。

这仍然保持普通代理目标的 ACL 语义不变，同时避免内置测速被外部目标 ACL 配置阻断。

实现上可新增一个 `TqEphemeralTargetAuthorizer` 接口，由 `TqHandleServerPeerStream()` 在 ACL 前查询：

```cpp
class TqEphemeralTargetAuthorizer {
public:
    virtual bool IsAllowedEphemeralTarget(MsQuicConnection* conn, const std::string& host, uint16_t port) const = 0;
};
```

首版只允许发起 SPEED_START 的同一条 QUIC connection 访问 `127.0.0.1:<session_port>`。

## 错误处理

| 场景 | 行为 |
|------|------|
| server 不支持新控制命令 | client 等待 READY 超时，打印明确错误 |
| SPEED_START 解码失败 | server 返回 SPEED_ERROR InvalidRequest 后关闭 stream |
| server bind/listen 失败 | 返回 SPEED_ERROR BindFailed/ListenFailed |
| tunnel OPEN 到临时端口失败 | client 发送 SPEED_FINISH，停止已打开 tunnels，返回失败 |
| worker send/recv 单连接失败 | 记录该 worker 结束；其他 worker 继续到 deadline |
| 控制 stream 断开 | server 停止对应 session |
| client 中断退出 | server 兜底超时清理 session |
| server 结果超时 | client 打印本地结果和 warning，进程返回非 0 |

## 线程模型

client：

- 主线程负责控制 stream、deadline、结果打印。
- 每条测试 tunnel 一个 worker 线程读/写 pump socket。
- `TqStartClientTunnel()` 仍在主流程里逐条建立，避免并发 OPEN 失败时清理复杂化；必要时后续再并行化 tunnel 建立。

server：

- QUIC stream callback 只做轻量分派。
- speed control handler 负责解析控制消息。
- 每个 session 一个 accept 线程。
- 每个 accepted TCP 连接一个 worker 线程。

首版线程数与 `--quic-connections` 成正比，最大 128，符合现有连接数上限。

## 文件变更规划

| 文件 | 变更 |
|------|------|
| `src/config/config.h` | 新增测速模式和 duration 字段 |
| `src/config/config.cpp` | 解析和校验 `--upload-test` / `--download-test` |
| `src/protocol/control_protocol.h` | 新增测速命令、结构体、错误码、encode/decode 声明 |
| `src/protocol/control_protocol.cpp` | 实现测速控制消息编码/解码 |
| `src/runtime/speed_test.h` | client runner、server controller API |
| `src/runtime/speed_test.cpp` | 测速 runtime 实现 |
| `src/protocol/quic_session.*` | 复用现有 `PickConnection()` 返回的 `MsQuicConnection` 打开 client-initiated control stream；server 侧 incoming stream 仍通过 existing peer stream callback 进入 dispatcher |
| `src/tunnel/tcp_tunnel.*` | 支持 incoming stream dispatcher、临时目标 ACL 绕过 |
| `src/main.cpp` | client 测速模式分支；server 创建 speed controller 并注入 stream handler |
| `src/CMakeLists.txt` | 加入 `runtime/speed_test.cpp` 和单元测试目标 |
| `src/unittest/control_protocol_test.cpp` | 覆盖测速控制消息编解码 |
| `src/unittest/speed_test_test.cpp` | 覆盖统计、loopback listener、错误路径 |
| `README.md` | 添加内置测速用法 |

## 测试计划

### 单元测试

1. 控制协议：
   - SPEED_START/READY/FINISH/RESULT/ERROR round trip。
   - 非法 direction、duration、parallel、flags 被拒绝。
   - message_len 越界被拒绝。
2. 配置解析：
   - `--download-test 10` / `--upload-test 10` 成功。
   - 两者互斥。
   - server 模式禁止。
   - `--client-config` 禁止。
   - `--warmup-mb` 与测速互斥。
3. server runtime：
   - 绑定 loopback port。
   - upload 模式能接收并统计本地 TCP client 发送字节。
   - download 模式能发送并统计字节。
   - Finish 后端口关闭、session 清理。
4. 结果计算：
   - bytes + elapsed 转换 Gbps / MiB/s 正确。

### 集成测试

新增脚本或扩展 `scripts/test-tcpquic-proxy.sh`：

```bash
tcpquic-proxy server ... --allow-targets 0.0.0.0/0
tcpquic-proxy client ... --download-test 2
tcpquic-proxy client ... --upload-test 2
```

覆盖：

- `--compress off`
- `--compress lz4`
- `--quic-connections 4`
- server 侧没有外部 HTTP server 的情况下测试仍可运行

### 手工性能验证

双机运行：

```bash
tcpquic-proxy client ... --quic-connections 8 --compress off --download-test 10
tcpquic-proxy client ... --quic-connections 8 --compress off --upload-test 10
tcpquic-proxy client ... --quic-connections 8 --compress lz4 --download-test 10
```

记录 local/server bytes 是否接近、吞吐是否稳定、server session 是否在结束后清理。

## 兼容性与迁移

- 不改变普通 SOCKS5 / HTTP CONNECT 代理行为。
- 不改变现有 OPEN / OPEN_OK / OPEN_FAIL wire format。
- 新控制命令只在测速模式使用；旧 server 不支持时 client 会失败并提示，而不会误走普通代理路径。
- server 默认支持测速控制面，但只有 mTLS 认证成功后的 QUIC client 能触发。

## 后续扩展

- `--test-connections <n>`：独立于 `--quic-connections` 控制并发测试 tunnels。
- `--test-json`：输出机器可读 JSON。
- `--test-bidirectional <sec>`：同时上传和下载。
- IPv6 loopback 支持：`[::1]:0`。
- 测试进度周期输出：每秒打印瞬时吞吐。
- admin API 暴露最近测速 session 结果。
