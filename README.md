# tcpquic-proxy

独立仓库实现的 **TCP-over-QUIC 隧道代理**。在 A、B 两节点各运行一个进程，预先建立 QUIC 长连接；本地应用通过 **SOCKS5** 或 **HTTP CONNECT** 接入代理，每个 TCP 连接映射为一条 QUIC 双向 Stream，由 B 侧按动态目标发起 TCP 拨号。

底层 QUIC 栈来自 [msquic](https://github.com/microsoft/msquic)，压缩库来自 [zstd](https://github.com/facebook/zstd)，异步 DNS 解析来自 [c-ares](https://github.com/c-ares/c-ares)，TLS 来自 msquic 内嵌的 [quictls](https://github.com/quictls/openssl)；均以 Git 子模块形式 vendored（详见下方 [依赖](#依赖)）。

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
    ↓  QUIC/UDP 长连接（client 验证 server 证书，ALPN: tcpquic-tunnel/1）
  tcpquic-proxy server
    ↓  TCP connect（DNS + CIDR ACL 校验后）
[B 侧网络] 目标 TCP 服务
```

**映射关系：** 1 个 TCP 连接 = 1 个 QUIC 双向 Stream。

| 模式 | 部署 | 职责 |
|------|------|------|
| `client` | A 节点 | SOCKS5 / HTTP CONNECT 入口、压缩、QUIC 客户端、长连接维护 |
| `server` | B 节点 | QUIC 监听、服务端证书、ACL、解压、TCP 拨号 |

## 依赖

### Vendored 库（Git 子模块，无需系统 dev 包）

| 组件 | 路径 | 用途 |
|------|------|------|
| [msquic](https://github.com/microsoft/msquic) | `third_party/msquic` | QUIC 栈 |
| [quictls](https://github.com/quictls/openssl) | `third_party/msquic/submodules/quictls` | TLS（msquic 子模块，**首次构建从源码编译**） |
| [zstd](https://github.com/facebook/zstd) | `third_party/zstd` | 流式压缩 |
| [c-ares](https://github.com/c-ares/c-ares) | `third_party/c-ares` | 异步 DNS 解析 |

当前 pin：msquic `v2.5.2-24-g0efce2bc9`、zstd `v1.5.7`、c-ares `v1.34.6`。

**不需要** 安装 `libzstd-dev`、`libssl-dev`、`libc-ares-dev` 等系统库；`git submodule update --init --recursive` 会拉齐子模块源码，c-ares 也会由 CMake 一并构建。

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

`tcpquic-proxy` 二进制依赖 Linux/POSIX 标准接口（socket、pthread、libnuma 等），**不依赖** 系统安装的 libzstd / libssl / libmsquic / libc-ares。默认将 msquic（含 quictls）、zstd、c-ares、spdlog **静态链入** 主程序，部署时只需拷贝单个 `tcpquic-proxy` 可执行文件。

### 测试与脚本（不链接进二进制）

运行 `scripts/` 下集成测试、压测脚本时，宿主机还需：

| 命令 | 用途 |
|------|------|
| `openssl` | 生成测试用 CA 和 server 证书 |
| `curl` | HTTP CONNECT / SOCKS5 端到端验证 |
| `python3` | 临时 HTTP 服务、辅助计算 |

### 可选 CMake 开关

| 开关 | 默认 | 说明 |
|------|------|------|
| `TCPQUIC_MSQUIC_SHARED` | `OFF` | 设为 `ON` 时构建 `libmsquic.so` 动态库（旧行为） |
| `QUIC_USE_SYSTEM_LIBCRYPTO` | `OFF`（强制） | 固定使用 vendored quictls crypto，不使用系统 `libcrypto` |

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

覆盖 happy path、ACL/DNS/refused 负路径、zstd 端到端、`--connections 4` 连接池。

### 内置吞吐测试

内置 speed test 通过已认证的 QUIC 控制流在 server 侧临时启动 loopback TCP 测试端口，
client 再用正常 TCP-over-QUIC tunnel 连接该端口，因此不需要额外 curl、iperf 或 HTTP server。

```bash
# server
./build/bin/Release/tcpquic-proxy server \
  --listen 127.0.0.1:14443 \
  --cert cert/server/server.crt \
  --key cert/server/server.key \
  --allow-targets 10.0.0.0/8

# client download / upload
./build/bin/Release/tcpquic-proxy client \
  --peer 127.0.0.1:14443 \
  --ca cert/ca.crt \
  --connections 1 \
  --compress off \
  --download-test 10

./build/bin/Release/tcpquic-proxy client \
  --peer 127.0.0.1:14443 \
  --ca cert/ca.crt \
  --connections 1 \
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

默认使用客户端单向验证服务端证书：server 配置 `server.crt/server.key`，client 只配置签发 server 证书的 `ca.crt`。

**B 节点（server）：**

```bash
./build/bin/Release/tcpquic-proxy server \
  --listen 0.0.0.0:443 \
  --cert /path/server.crt \
  --key  /path/server.key \
  --allow-targets 10.0.0.0/8,192.168.0.0/16 \
  --deny-targets 127.0.0.0/8 \
  --compress auto
```

**A 节点（client）：**

```bash
./build/bin/Release/tcpquic-proxy client \
  --peer proxy-b.example.com:443 \
  --ca   /path/ca.crt \
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

### 本地端口转发

```bash
./build/bin/Release/tcpquic-proxy client \
  --peer proxy-b.example.com:443 \
  --ca cert/ca.crt \
  --forward 127.0.0.1:15432=db.internal.example.com:5432

psql -h 127.0.0.1 -p 15432
```

## CLI 参数

```
Usage: tcpquic-proxy client|server [options]
```

| 参数 | 模式 | 默认值 | 说明 |
|------|------|--------|------|
| `-h` / `--help` / `--usage` | 双方 | - | 展示命令行帮助并退出 |
| `--socks-listen` | client | `127.0.0.1:1080` | SOCKS5 监听地址 |
| `--http-listen` | client | `127.0.0.1:8080` | HTTP CONNECT 监听地址 |
| `--peer` | client | （必填） | B 节点 QUIC 地址 `host:port` |
| `--forward` | client | `空` | 本地端口转发，格式 `local=target`，可重复配置 |
| `--listen` | server | （必填） | QUIC 监听 `host:port` |
| `--cert` | server | （必填） | 服务端证书 PEM；client 模式忽略 |
| `--key` | server | （必填） | 服务端私钥 PEM；client 模式忽略 |
| `--ca` | client | （必填） | 用于验证服务端证书的 CA PEM；server 模式可选且不使用 |
| `--connections` | client | `1` | QUIC 连接池大小（最大 128） |
| `--connection-stream-count` | client/server | `1024` | 每条 QUIC 连接允许的双向 stream 上限（范围 `1..65535`） |
| `--reconnect-interval-ms` | client | `3000` | QUIC slot reconnect interval, range `1000..60000` |
| `--keepalive-ms` | client/server | `5000` | QUIC keepalive interval, range `1000..15000` |
| `--download-test` | client | 空 | 内置端到端 download 吞吐测试秒数 |
| `--upload-test` | client | 空 | 内置端到端 upload 吞吐测试秒数 |
| `--compress` | 双方 | `auto` | `auto` / `zstd` / `off` |
| `--compress-level` | 双方 | `1` | zstd 压缩等级 |
| `--allow-targets` | server | （必填） | 逗号分隔 CIDR 白名单 |
| `--deny-targets` | server | 空 | 逗号分隔 CIDR 黑名单 |
| `--admin-listen` | 双方 | 空 | Admin HTTP 监听地址；当前只允许 loopback：`127.0.0.1`、`localhost`、`::1` |
| `--admin-token-file` | 双方 | pid 默认路径 | Admin Bearer token JSON 文件路径 |
| `--admin-threads` | 双方 | `2` | Admin HTTP 固定 worker 线程数，范围 `1..32` |
| `--admin-allow-unauthenticated-legacy` | 双方 | 关闭 | 仅允许旧路径在 loopback 上跳过 token；`/api/v1/*` 仍要求 token |

- Client SOCKS5 / HTTP CONNECT listeners are open only while the peer has at least one connected QUIC connection. If all QUIC connections for a peer drop, that peer's local listeners close and reopen after reconnect.
- Client 侧每条 QUIC connection 默认使用独立的本地 UDP 临时端口，而不是所有连接共用一个 UDP 端口，也不是按 peer/server 共用一个端口。例如 `--connections 4` 连接同一个 server 时，通常会形成 4 个不同的 `client_ip:client_udp_port -> server_ip:server_udp_port` 五元组。这样符合多连接设计目标：服务端网卡 RSS/多队列通常会按五元组哈希分发报文；如果同一源 IP + 源 UDP 端口承载所有连接，报文容易落到同一个网卡队列。客户端每条连接使用不同随机 UDP 端口，可以让多条 QUIC 连接更容易分散到不同队列，从而提升整体 I/O 吞吐能力。
- `--download-test` 和 `--upload-test` 只允许 client 单 peer 模式使用，不能与 `--warmup-mb` 或 router `--client-config` 同时使用。
- 非法 CIDR 在启动时直接报错（不会静默忽略）。
- 不支持 `--compress-min-size`（压缩在 OPEN 阶段协商，per-stream 流式）。

### 多网口 QUIC 绑定

server 端仍只使用 `--listen` / `server.proto_listen` 配置 QUIC 监听地址。配置 `0.0.0.0:443` 时，程序会枚举本机非本地 IPv4 地址，并对每个地址分别启动 MsQuic listener；显式配置 `ip:port` 列表时，只绑定列表中的指定地址。

```bash
tcpquic-proxy server --listen 0.0.0.0:443 --cert server.crt --key server.key --allow-targets 10.0.0.0/8
tcpquic-proxy server --listen 36.1.1.10:443,59.1.1.10:443 --cert server.crt --key server.key --allow-targets 10.0.0.0/8
```

client 未配置 `paths` 时，`--peer` / router `quic_peer` 支持地址列表。连接槽按 `--connections` / `quic_connections` 数量创建，并 round-robin 分配到多个 peer 地址；本地出口由系统路由决定，程序不会强制选择本地网口。

```bash
tcpquic-proxy client --peer 36.1.1.10:443,59.1.1.10:443 --connections 8 --ca ca.crt
```

client 配置 `paths` 时，每条 path 显式描述 `local -> peer`。配置后会忽略 peer 级 `quic_peer` 和 `quic_connections`，总 QUIC connection 数等于所有 path 的 `connections` 之和。

```json
{
  "version": 1,
  "peers": [
    {
      "peer_id": "server-b",
      "paths": [
        { "name": "cmcc", "local": "10.10.1.2", "peer": "36.1.1.10:443", "connections": 4 },
        { "name": "ctcc", "local": "10.20.1.2", "peer": "59.1.1.10:443", "connections": 4 }
      ],
      "socks_listen": "127.0.0.1:1080"
    }
  ]
}
```

TCP tunnel 调度粒度是一个 TCP 连接。一个 TCP 连接会绑定到一条 QUIC connection，生命周期内不跨 slot 迁移；多个并发 TCP 连接会在同一 peer 的已连接 slot 间 round-robin 分摊。因此，`connections` 同时决定 path 上的 QUIC 连接数，也是默认 tunnel 分配权重。

## Admin HTTP API

Admin HTTP 通过 `--admin-listen host:port` 开启；不配置该参数时不会启动。当前实现只允许绑定 loopback 地址，适合本机运维脚本或 SSH tunnel 访问。

启动 Admin 时会生成 Bearer token 并写入 token JSON 文件。可用 `--admin-token-file <path>` 指定路径；未指定时使用包含 pid 的运行时默认路径。POSIX 下 token 文件父目录必须由当前用户拥有并会被限制为 `0700`，token 文件为 `0600`；不要把 token 文件直接写到 `/tmp/admin.json` 这类共享目录下。日志只打印 token 文件路径，不打印 token 内容。

```bash
tcpquic-proxy ... \
  --admin-listen 127.0.0.1:18080 \
  --admin-token-file "$XDG_RUNTIME_DIR/tcpquic-proxy/admin-token.json"

TOKEN_FILE="$XDG_RUNTIME_DIR/tcpquic-proxy/admin-token.json"
TOKEN=$(jq -r .token "$TOKEN_FILE")
curl -H "Authorization: Bearer ${TOKEN}" \
  http://127.0.0.1:18080/api/v1/health
```

新路径 `/api/v1/*` 覆盖 client peer、connection、tunnel/relay 和 server 侧状态控制。旧路径 `/health`、`/metrics`、`/config` 和 `/peers/{peer_id}/enable|disable` 已进入 deprecated 兼容期，默认同样要求 Bearer token。只有显式开启 `--admin-allow-unauthenticated-legacy` 时，旧路径才允许无 token 访问；该开关不影响 `/api/v1/*`。

### Client 多 peer 模式

| 方法 | 路径 | 能力 |
|------|------|------|
| `GET` | `/api/v1/health` | 返回 client runtime 健康状态：`role`、`status`、`uptime_seconds` |
| `GET` | `/api/v1/metrics` | 返回每个 peer 的运行指标 |
| `GET` | `/api/v1/config` | 返回当前 router 配置 |
| `PUT` | `/api/v1/config` | 用请求 body 中的 JSON 替换当前 router 配置 |
| `GET` | `/api/v1/peers` | 返回 peer 列表和状态 |
| `POST` | `/api/v1/peers` | 创建 peer |
| `GET` | `/api/v1/peers/{peer_id}` | 返回指定 peer 状态 |
| `PUT` | `/api/v1/peers/{peer_id}` | 整体替换指定 peer |
| `PATCH` | `/api/v1/peers/{peer_id}` | 局部更新指定 peer |
| `DELETE` | `/api/v1/peers/{peer_id}` | 删除 peer；enabled peer 需要 body 指定 `{"mode":"drain"}` 或 `{"mode":"abort"}` |
| `POST` | `/api/v1/peers/{peer_id}:enable` | 启用指定 peer |
| `POST` | `/api/v1/peers/{peer_id}:disable` | 禁用指定 peer |
| `POST` | `/api/v1/peers/{peer_id}:drain` | drain 指定 peer |
| `POST` | `/api/v1/peers/{peer_id}:abort-tunnels` | 中断指定 peer 的 tunnel |
| `GET` | `/api/v1/peers/{peer_id}/connections` | 列出 peer 的 QUIC connection slots |
| `GET` | `/api/v1/peers/{peer_id}/connections/{connection_id}` | 返回单条 connection 状态 |
| `POST` | `/api/v1/peers/{peer_id}/connections` | 增加一个最高序号 connection slot |
| `DELETE` | `/api/v1/peers/{peer_id}/connections/{connection_id}` | 删除最高序号 connection slot；删除中间 slot 返回 `409` |
| `POST` | `/api/v1/peers/{peer_id}/connections/{connection_id}:reconnect` | 重连单条 connection |
| `POST` | `/api/v1/peers/{peer_id}/connections/{connection_id}:abort-tunnels` | 中断单条 connection 上的 tunnel |
| `GET` | `/api/v1/tunnels` | 列出当前活跃 tunnel |
| `GET` | `/api/v1/tunnels/{tunnel_id}` | 返回单条 tunnel 状态 |
| `DELETE` | `/api/v1/tunnels/{tunnel_id}` | abort 指定 tunnel |
| `POST` | `/api/v1/tunnels/{tunnel_id}:drain` | graceful drain 指定 tunnel |
| `POST` | `/api/v1/tunnels/{tunnel_id}:abort` | abort 指定 tunnel |
| `GET` | `/api/v1/relay/metrics` | 返回 relay 聚合指标 |
| `GET` | `/api/v1/relay/workers` | 返回 relay worker snapshot；当前包含 aggregate worker |
| `GET` | `/api/v1/relay/workers/{worker_id}` | 返回指定 relay worker snapshot |
| `GET` | `/health` | deprecated 兼容路径：返回 client runtime 健康状态 |
| `GET` | `/metrics` | deprecated 兼容路径：返回每个 peer 的运行指标 |
| `GET` | `/config` | deprecated 兼容路径：返回当前 router 配置 |
| `PUT` | `/config` | deprecated 兼容路径：替换当前 router 配置 |
| `POST` | `/peers/{peer_id}/enable` | deprecated 兼容路径：启用指定 peer |
| `POST` | `/peers/{peer_id}/disable` | deprecated 兼容路径：禁用指定 peer |

`GET /config` 和 `PUT /config` 使用相同的 peer JSON 字段，例如：

```json
{
  "peers": [
    {
      "peer_id": "db",
      "quic_peer": "proxy-b.example.com:443",
      "socks_listen": "",
      "http_listen": "",
      "port_forwards": [
        {"listen": "127.0.0.1:15432", "target": "db.internal.example.com:5432"}
      ],
      "enabled": true
    }
  ]
}
```

`GET /metrics` 的 peer 字段包括：

```json
{
  "peer_id": "agent-b",
  "enabled": true,
  "quic_peer": "127.0.0.1:14444",
  "socks_listen": "127.0.0.1:11001",
  "http_listen": "127.0.0.1:18001",
  "port_forwards": [
    {"listen": "127.0.0.1:15432", "target": "db.internal.example.com:5432"}
  ],
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

`PUT /config`、peer CRUD 和 peer enable/disable 会触发运行时变更。peer 被禁用、删除或 data-plane 参数变化时，client 会停止对应 listener、主动中断该 peer 的 tunnel，并 drain 旧 peer runtime。新增脚本应优先使用 `/api/v1/peers*`；整体 `PUT /config` 仍保留用于批量替换。

### Server 模式

| 方法 | 路径 | 能力 |
|------|------|------|
| `GET` | `/health` | 返回 server 健康状态和指标 |
| `GET` | `/metrics` | 返回 server 指标 |
| `GET` | `/api/v1/server` | 返回 server 状态和指标 |
| `GET` | `/api/v1/server/metrics` | 返回 server 指标 |
| `GET` | `/api/v1/server/connections` | 列出 server QUIC connections |
| `GET` | `/api/v1/server/connections/{connection_id}` | 返回指定 server connection 状态 |
| `GET` | `/api/v1/server/tunnels` | 列出 server 侧 tunnel |
| `POST` | `/api/v1/server/connections/{connection_id}:abort-tunnels` | 中断指定 server connection 上的 tunnel |

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

Admin API 当前有本机 Bearer token 鉴权但没有 TLS；仍依赖 loopback 绑定限制访问面。旧路径已进入兼容期，建议新脚本使用 `/api/v1/*`。

## 安全与 ACL

- **TLS：** client 使用 `--ca` 验证 server 证书；server 不要求客户端证书。
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

- 单 QUIC 长连接或连接池（`--connections`）、1 TCP = 1 Stream
- SOCKS5 CONNECT（NO AUTH）、HTTP CONNECT 隧道
- 动态目标（B 侧 DNS + TCP 拨号）
- 客户端验证服务端证书、CIDR ACL、zstd 流式压缩

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
