# tcpquic-proxy

独立仓库实现的 **TCP-over-QUIC 隧道代理**。在 A、B 两节点各运行一个进程，预先建立 QUIC 长连接；本地应用通过 **SOCKS5** 或 **HTTP CONNECT** 接入代理，每个 TCP 连接映射为一条 QUIC 双向 Stream，由 B 侧按动态目标发起 TCP 拨号。

底层 QUIC 栈来自 [msquic](https://github.com/microsoft/msquic)，压缩库来自 [zstd](https://github.com/facebook/zstd)，TLS 来自 msquic 内嵌的 [quictls](https://github.com/quictls/openssl)；均以 Git 子模块 vendored（详见下方 [依赖](#依赖)）。

**典型场景：**

- 穿越防火墙 / NAT，将 TCP 流量封装在 QUIC/UDP 上
- 高延迟、丢包环境下利用 QUIC + BBR 改善传输
- 可选 zstd 流式压缩降低有效字节数

详细设计见 [`docs/finished/specs/2026-06-06-tcpquic-proxy-design.md`](docs/finished/specs/2026-06-06-tcpquic-proxy-design.md)。

## 架构

```
[A 节点] TCP 应用（curl、浏览器等）
    ↓  SOCKS5 :1080 / HTTP CONNECT :8080
  tcpquic-proxy client
    ↓  QUIC/UDP 长连接（mTLS，ALPN: tcpquic-tunnel/1）
  tcpquic-proxy server
    ↓  TCP connect（DNS + CIDR ACL 校验后）
[B 侧网络] 目标 TCP 服务
```

**映射关系：** 1 个 TCP 连接 = 1 个 QUIC 双向 Stream。

| 模式 | 部署 | 职责 |
|------|------|------|
| `client` | A 节点 | SOCKS5 / HTTP CONNECT 入口、压缩、QUIC 客户端、长连接维护 |
| `server` | B 节点 | QUIC 监听、mTLS、ACL、解压、TCP 拨号 |

## 依赖

### Vendored 库（Git 子模块，无需系统 dev 包）

| 组件 | 路径 | 用途 |
|------|------|------|
| [msquic](https://github.com/microsoft/msquic) | `third_party/msquic` | QUIC 栈 |
| [quictls](https://github.com/quictls/openssl) | `third_party/msquic/submodules/quictls` | TLS/mTLS（msquic 子模块，**首次构建从源码编译**） |
| [zstd](https://github.com/facebook/zstd) | `third_party/zstd` | 流式压缩 |

当前 pin：msquic `v2.5.2-24-g0efce2bc9`、zstd `v1.5.7`。

**不需要** 安装 `libzstd-dev`、`libssl-dev` 等系统库；`git submodule update --init --recursive` 会拉齐上述源码并由 CMake 一并构建。

### 构建工具

| 工具 | 说明 |
|------|------|
| CMake ≥ 3.20 | 构建系统（msquic 静态库集成要求） |
| C/C++ 编译器 | GCC 10+ 或 clang（需要完整 C++17 `<filesystem>` 支持） |
| `perl`、`make` | msquic 编译 quictls 时使用（OpenSSL `Configure` + `make install_dev`） |
| Git | 初始化子模块 |

Debian/Ubuntu 示例（仅工具链，**不含** vendored 库对应的 `-dev` 包）：

```bash
sudo apt install git cmake build-essential perl
```

Amazon Linux 2 / RHEL 系环境如果默认 `g++` 仍是 GCC 7，应安装并显式使用
GCC 10 工具链，例如 `/usr/bin/gcc10-gcc` 与 `/usr/bin/gcc10-g++`。

### 运行时

`tcpquic-proxy` 二进制依赖 Linux/POSIX 标准接口（socket、pthread、libnuma 等），**不依赖** 系统安装的 libzstd / libssl / libmsquic。默认将 msquic（含 quictls）、zstd、spdlog **静态链入** 主程序，部署时只需拷贝单个 `tcpquic-proxy` 可执行文件。

### 测试与脚本（不链接进二进制）

运行 `scripts/` 下集成测试、压测脚本时，宿主机还需：

| 命令 | 用途 |
|------|------|
| `openssl` | 生成测试用 mTLS 证书 |
| `curl` | HTTP CONNECT / SOCKS5 端到端验证 |
| `python3` | 临时 HTTP 服务、辅助计算 |

### 可选 CMake 开关

| 开关 | 默认 | 说明 |
|------|------|------|
| `TCPQUIC_MSQUIC_SHARED` | `OFF` | 设为 `ON` 时构建 `libmsquic.so` 动态库（旧行为） |
| `QUIC_USE_SYSTEM_LIBCRYPTO` | `OFF` | 设为 `ON` 时 msquic 使用系统 `libcrypto`，而非 vendored quictls |

本项目所有平台统一使用 msquic 的 `quictls` TLS 后端；不支持 Windows Schannel 构建。

## 构建

本项目统一使用仓库根目录下的 `build/` 作为本地 CMake 构建目录。不要使用旧的
`build-gcc10/`、`build-gcc10-cxx17/`、`build-gcc10-fixed/` 等临时目录；源码或
`src/CMakeLists.txt` 更新后，应重新配置 `build/`。

```bash
git submodule update --init --recursive
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release \  -DCMAKE_C_COMPILER=/usr/bin/gcc10-gcc \
  -DCMAKE_CXX_COMPILER=/usr/bin/gcc10-g++
cmake --build build --target tcpquic-proxy -j$(nproc)
```


产物：`build/bin/Release/tcpquic-proxy`

### Windows 10/11 x64

Install Visual Studio 2022 Build Tools with the MSVC C++ workload, CMake, Git, Strawberry Perl, OpenSSL, Python, and curl.

本项目在 Windows 上同样只支持 msquic `quictls` TLS 后端，不支持 Schannel 构建。CMake 会强制设置 `QUIC_TLS_LIB=quictls`，并在 Windows 上使用 `no-asm` 避免额外 NASM 依赖。证书使用 PEM 文件路径，与 Linux/macOS 构建保持一致。

```powershell
git submodule update --init --recursive
cmake -S . -B build-x64 -A x64
cmake --build build-x64 --config Release --target tcpquic-proxy
```

Run the Windows loopback validation:

```powershell
.\scripts\test-tcpquic-proxy-windows.ps1 -Compress off
.\scripts\test-tcpquic-proxy-windows.ps1 -Compress zstd
```

### wdtq 集成

若通过 [wdtq](https://github.com/) 等工具启动代理，可显式指定二进制路径：

```bash
export TCPQUIC_PROXY_BIN=/path/to/build/bin/Release/tcpquic-proxy
```

### 单元测试

```bash
cmake --build build --target \
  tcpquic_acl_test \
  tcpquic_admin_http_test \
  tcpquic_blocking_relay_demo_test \
  tcpquic_compress_test \
  tcpquic_config_router_test \
  tcpquic_control_test \
  tcpquic_http_connect_test \
  tcpquic_relay_buffer_test \
  tcpquic_linux_relay_worker_io_test \
  tcpquic_linux_relay_worker_queue_test \
  tcpquic_relay_backend_selection_test \
  tcpquic_router_runtime_test \
  tcpquic_socks5_test \
  tcpquic_tcp_write_queue_test \
  tcpquic_thread_pool_test \
  tcpquic_tuning_test \
  tcpquic_tunnel_reaper_test \
  tcpquic_tunnel_test \
  tcpquic_production_linkage_guard_test \
  -j$(nproc)

for t in build/bin/Release/tcpquic_*_test; do
  "$t"
done
```

### 集成测试

```bash
./scripts/test-tcpquic-proxy.sh
```

覆盖 happy path、ACL/DNS/refused 负路径、zstd 端到端、`--quic-connections 4` 连接池。

### 内置吞吐测试

内置 speed test 通过已认证的 QUIC 控制流在 server 侧临时启动 loopback TCP 测试端口，
client 再用正常 TCP-over-QUIC tunnel 连接该端口，因此不需要额外 curl、iperf 或 HTTP server。

```bash
# server
./build/bin/Release/tcpquic-proxy server \
  --quic-listen 127.0.0.1:14443 \
  --quic-cert cert/server/server.crt \
  --quic-key cert/server/server.key \
  --quic-ca cert/ca.crt \
  --allow-targets 10.0.0.0/8

# client download / upload
./build/bin/Release/tcpquic-proxy client \
  --quic-peer 127.0.0.1:14443 \
  --quic-cert cert/client/client.crt \
  --quic-key cert/client/client.key \
  --quic-ca cert/ca.crt \
  --quic-connections 1 \
  --compress off \
  --download-test 10

./build/bin/Release/tcpquic-proxy client \
  --quic-peer 127.0.0.1:14443 \
  --quic-cert cert/client/client.crt \
  --quic-key cert/client/client.key \
  --quic-ca cert/ca.crt \
  --quic-connections 1 \
  --compress off \
  --upload-test 10
```

输出中的 upload 吞吐以 server bytes 为主，download 吞吐以 client local bytes 为主。
speed test 的默认 `--compress auto` 会在测试 tunnel 内按 `off` 处理；如需验证压缩路径，
显式传入 `--compress zstd`。详细说明和 caveat 见
[`docs/finished/built-in-speed-test-20260612.md`](docs/finished/built-in-speed-test-20260612.md)。

### 并发隧道压测

```bash
# 默认 100 条并发 HTTP CONNECT 隧道
./scripts/test-tcpquic-concurrent.sh

# 128 隧道 + 8 条 QUIC 连接 + zstd
TUNNELS=128 QUIC_CONNECTIONS=8 COMPRESS=zstd ./scripts/test-tcpquic-concurrent.sh
```

### 双机冒烟（DGX / 直连链路）

在对端可 SSH 的前提下，于本机构建二进制后运行：

```bash
./scripts/test-tcpquic-proxy-dgx.sh
TUNNELS=100 COMPRESS=zstd ./scripts/test-tcpquic-proxy-dgx.sh
```

默认对端 `jack@169.254.59.196`，本机出口 `169.254.250.230`；可通过 `PEER`、`TARGET`、`BIND` 覆盖。2026-06-10 Linux 验证记录见 [`docs/finished/linux-verification-2026-06-10.md`](docs/finished/linux-verification-2026-06-10.md)。

### 性能基线

```bash
# 对比裸 TCP、隧道 off/zstd
./scripts/bench-tcpquic-proxy.sh

# 双机 DGX（spark-1619 ↔ spark-1b6f，可选对端 netem）
./scripts/bench-tcpquic-proxy-dgx.sh
NETEM=1 DELAY=100ms LOSS=5% ./scripts/bench-tcpquic-proxy-dgx.sh

# 本机 loopback + netem
NETEM=1 NETEM_DELAY=100ms NETEM_LOSS=5% ./scripts/bench-tcpquic-proxy.sh
```

## 快速开始

双方需 mTLS 证书（同一 CA 签发 server / client 证书）。以下为最小示例。

**B 节点（server）：**

```bash
./build/bin/Release/tcpquic-proxy server \
  --quic-listen 0.0.0.0:443 \
  --quic-cert /path/server.crt \
  --quic-key  /path/server.key \
  --quic-ca   /path/ca.crt \
  --allow-targets 10.0.0.0/8,192.168.0.0/16 \
  --deny-targets 127.0.0.0/8 \
  --compress auto
```

**A 节点（client）：**

```bash
./build/bin/Release/tcpquic-proxy client \
  --quic-peer proxy-b.example.com:443 \
  --quic-cert /path/client.crt \
  --quic-key  /path/client.key \
  --quic-ca   /path/ca.crt \
  --socks-listen 127.0.0.1:1080 \
  --http-listen 127.0.0.1:8080 \
  --compress auto
```

**应用侧：**

```bash
# HTTP CONNECT
curl -x http://127.0.0.1:8080 --proxytunnel http://目标主机:端口/

# SOCKS5（推荐 hostname 解析走代理）
curl --socks5-hostname 127.0.0.1:1080 http://目标主机:端口/

# 环境变量
export ALL_PROXY=socks5://127.0.0.1:1080
export HTTPS_PROXY=http://127.0.0.1:8080
```

## CLI 参数

```
Usage: tcpquic-proxy client|server [options]
```

| 参数 | 模式 | 默认值 | 说明 |
|------|------|--------|------|
| `--socks-listen` | client | `127.0.0.1:1080` | SOCKS5 监听地址 |
| `--http-listen` | client | `127.0.0.1:8080` | HTTP CONNECT 监听地址 |
| `--quic-peer` | client | （必填） | B 节点 QUIC 地址 `host:port` |
| `--quic-listen` | server | （必填） | QUIC 监听 `host:port` |
| `--quic-cert` | 双方 | （必填） | 本端证书 PEM |
| `--quic-key` | 双方 | （必填） | 本端私钥 PEM |
| `--quic-ca` | 双方 | （必填） | CA / 对端校验用证书 PEM |
| `--quic-connections` | client | `1` | QUIC 连接池大小（最大 128） |
| `--quic-reconnect-interval-ms` | client | `3000` | QUIC slot reconnect interval, range `1000..60000` |
| `--download-test` | client | 空 | 内置端到端 download 吞吐测试秒数 |
| `--upload-test` | client | 空 | 内置端到端 upload 吞吐测试秒数 |
| `--compress` | 双方 | `auto` | `auto` / `zstd` / `off` |
| `--compress-level` | 双方 | `1` | zstd 压缩等级 |
| `--allow-targets` | server | （必填） | 逗号分隔 CIDR 白名单 |
| `--deny-targets` | server | 空 | 逗号分隔 CIDR 黑名单 |
| `--admin-listen` | 双方 | 空 | Admin HTTP 监听地址；当前只允许 loopback：`127.0.0.1`、`localhost`、`::1` |

- Client SOCKS5 / HTTP CONNECT listeners are open only while the peer has at least one connected QUIC connection. If all QUIC connections for a peer drop, that peer's local listeners close and reopen after reconnect.
- Client 侧每条 QUIC connection 默认使用独立的本地 UDP 临时端口，而不是所有连接共用一个 UDP 端口，也不是按 peer/server 共用一个端口。例如 `--quic-connections 4` 连接同一个 server 时，通常会形成 4 个不同的 `client_ip:client_udp_port -> server_ip:server_udp_port` 五元组。这样符合多连接设计目标：服务端网卡 RSS/多队列通常会按五元组哈希分发报文；如果同一源 IP + 源 UDP 端口承载所有连接，报文容易落到同一个网卡队列。客户端每条连接使用不同随机 UDP 端口，可以让多条 QUIC 连接更容易分散到不同队列，从而提升整体 I/O 吞吐能力。
- `--download-test` 和 `--upload-test` 只允许 client 单 peer 模式使用，不能与 `--warmup-mb` 或 router `--client-config` 同时使用。
- 非法 CIDR 在启动时直接报错（不会静默忽略）。
- 不支持 `--compress-min-size`（压缩在 OPEN 阶段协商，per-stream 流式）。

## Admin HTTP API

Admin HTTP 通过 `--admin-listen host:port` 开启；不配置该参数时不会启动。当前实现只允许绑定 loopback 地址，适合本机运维脚本或 SSH tunnel 访问。

### Client 多 peer 模式

| 方法 | 路径 | 能力 |
|------|------|------|
| `GET` | `/health` | 返回 client runtime 健康状态：`role`、`status`、`uptime_seconds` |
| `GET` | `/metrics` | 返回每个 peer 的运行指标 |
| `GET` | `/config` | 返回当前 router 配置 |
| `PUT` | `/config` | 用请求 body 中的 JSON 替换当前 router 配置 |
| `POST` | `/peers/{peer_id}/enable` | 启用指定 peer |
| `POST` | `/peers/{peer_id}/disable` | 禁用指定 peer |

`GET /metrics` 的 peer 字段包括：

```json
{
  "peer_id": "agent-b",
  "enabled": true,
  "quic_peer": "127.0.0.1:14444",
  "socks_listen": "127.0.0.1:11001",
  "http_listen": "127.0.0.1:18001",
  "state": "connected",
  "connection_count": 4,
  "connected_connections": 4,
  "active_streams": 0,
  "total_streams": 10,
  "reconnects": 0,
  "last_error": "",
  "last_connected_at": ""
}
```

`PUT /config` 与 peer enable/disable 会触发运行时变更。peer 被禁用、删除或 data-plane 参数变化时，client 会停止对应 listener、主动中断该 peer 的 tunnel，并 drain 旧 peer runtime。当前没有单独的 `DELETE /peers/{peer_id}` 或新增 peer 接口；新增、删除 peer 通过 `PUT /config` 整体替换配置完成。

### Server 模式

| 方法 | 路径 | 能力 |
|------|------|------|
| `GET` | `/health` | 返回 server 健康状态和指标 |
| `GET` | `/metrics` | 返回 server 指标 |

server metrics 字段包括：

```json
{
  "role": "server",
  "status": "healthy",
  "listen": "0.0.0.0:443",
  "uptime_seconds": 123,
  "accepted_connections": 1,
  "active_streams": 0,
  "total_streams": 10,
  "acl_denied": 0,
  "linux_relay_wakeups": 0,
  "linux_relay_events_processed": 0,
  "linux_relay_pending_events": 0,
  "linux_relay_pending_bytes": 0,
  "linux_relay_tcp_read_bytes": 0,
  "linux_relay_tcp_write_bytes": 0,
  "linux_relay_read_disabled_count": 0,
  "linux_relay_backend": "worker",
  "linux_relay_compressed_tcp_bytes": 0,
  "linux_relay_decompressed_tcp_bytes": 0,
  "linux_relay_errors": 0,
  "last_error": ""
}
```

Admin API 当前没有鉴权或 TLS，依赖 loopback 绑定限制访问面。

## 安全与 ACL

- **mTLS：** server 要求客户端证书；client 校验 server 证书。
- **ALPN：** `tcpquic-tunnel/1`
- **本地绑定：** SOCKS / HTTP 默认仅 `127.0.0.1`。
- **ACL（server 侧）：**
  - `--allow-targets` 为空则拒绝所有拨号。
  - `--deny-targets` 优先于 allow。
  - IP 字面量直接匹配 CIDR。
  - 域名由 B 侧 DNS 解析，对所有 A/AAAA 候选逐一校验；任一命中 deny 或无 allow 则拒绝。

## 隧道协议（概要）

每个 QUIC Stream 先完成控制握手，再进入数据转发。

1. **OPEN（A→B）：** magic `TQ`，携带目标地址、端口、压缩 flags。
2. **OPEN_OK / OPEN_FAIL（B→A）：** 固定 9 字节响应。
3. **数据阶段：** 可选 zstd 流式压缩；TCP FIN 对应 Stream FIN。

### 错误映射

| error_code | 含义 | SOCKS5 REP | HTTP CONNECT |
|------------|------|------------|--------------|
| `0x00` | 成功 | `0x00` | `200` |
| `0x01` | ACL 拒绝 | `0x02` | `403` |
| `0x02` | DNS 失败 | `0x04` | `502` |
| `0x03` | TCP 超时 | `0x05` | `504` |
| `0x04` | TCP 拒绝 | `0x05` | `502` |
| `0x05` | 内部错误 | `0x01` | `500` |

HTTP CONNECT 在隧道建立**成功之后**才返回 `200 Connection Established`；失败时按上表返回对应 HTTP 状态码。

## 源码结构

```text
src/
├── main.cpp                  # client / server 入口
├── acl/                      # CIDR ACL + DNS 候选过滤
├── config/                   # CLI 解析与调优参数
├── protocol/                 # OPEN 帧、压缩、QUIC 会话封装
├── platform/                 # POSIX/Windows socket 抽象与 Windows QUIC 凭据
├── ingress/                  # SOCKS5 / HTTP CONNECT 本地入口
├── tunnel/                   # TCP ↔ QUIC Stream 隧道与平台 relay worker
├── runtime/                  # admin、metrics、router runtime、线程池、warmup
├── docs/                     # 与源码实现紧密相关的说明
└── unittest/                 # 单元测试
```

## v1 能力与限制

**已实现：**

- 单 QUIC 长连接或连接池（`--quic-connections`）、1 TCP = 1 Stream
- SOCKS5 CONNECT（NO AUTH）、HTTP CONNECT 隧道
- 动态目标（B 侧 DNS + TCP 拨号）
- mTLS、CIDR ACL、zstd 流式压缩

**未实现（规格预留）：**

- SOCKS / HTTP 用户凭证认证
- QUIC 断连后在途隧道透明迁移（断连会关闭在途流，QUIC 自动重连）
- UDP 代理、普通 HTTP 代理（非 CONNECT）

## 相关文档

| 文档 | 说明 |
|------|------|
| [`docs/finished/specs/2026-06-06-tcpquic-proxy-design.md`](docs/finished/specs/2026-06-06-tcpquic-proxy-design.md) | 设计规格 |
| [`docs/finished/specs/2026-06-09-tcpquic-proxy-repo-restructure-design.md`](docs/finished/specs/2026-06-09-tcpquic-proxy-repo-restructure-design.md) | 独立仓库重构设计 |
| [`docs/finished/built-in-speed-test-20260612.md`](docs/finished/built-in-speed-test-20260612.md) | 内置 speed test 用法与结果解读 |
| [`docs/finished/plans/2026-06-06-tcpquic-proxy.md`](docs/finished/plans/2026-06-06-tcpquic-proxy.md) | 实现计划 |
| [`docs/finished/plans/2026-06-06-tcpquic-thread-model.md`](docs/finished/plans/2026-06-06-tcpquic-thread-model.md) | 线程模型 |
| [`docs/finished/plans/2026-06-06-tcpquic-adaptive-tuning.md`](docs/finished/plans/2026-06-06-tcpquic-adaptive-tuning.md) | 自适应调参 |
| [`docs/finished/plans/2026-06-07-tcpquic-remaining-work.md`](docs/finished/plans/2026-06-07-tcpquic-remaining-work.md) | 剩余工作 |
| [`docs/finished/plans/2026-06-09-tcpquic-proxy-repo-restructure.md`](docs/finished/plans/2026-06-09-tcpquic-proxy-repo-restructure.md) | 独立仓库迁移计划 |
| [`docs/tcpquic_next_steps.md`](docs/tcpquic_next_steps.md) | 后续步骤 |
