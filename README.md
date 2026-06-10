# tcpquic-proxy

独立仓库实现的 **TCP-over-QUIC 隧道代理**。在 A、B 两节点各运行一个进程，预先建立 QUIC 长连接；本地应用通过 **SOCKS5** 或 **HTTP CONNECT** 接入代理，每个 TCP 连接映射为一条 QUIC 双向 Stream，由 B 侧按动态目标发起 TCP 拨号。

底层 QUIC 栈来自 [msquic](https://github.com/microsoft/msquic)，压缩库来自 [lz4](https://github.com/lz4/lz4) 与 [zstd](https://github.com/facebook/zstd)，TLS 来自 msquic 内嵌的 [quictls](https://github.com/quictls/openssl)；均以 Git 子模块 vendored（详见下方 [依赖](#依赖)）。

**典型场景：**

- 穿越防火墙 / NAT，将 TCP 流量封装在 QUIC/UDP 上
- 高延迟、丢包环境下利用 QUIC + BBR 改善传输
- 可选 zstd / lz4 流式压缩降低有效字节数

详细设计见 [`docs/specs/2026-06-06-tcpquic-proxy-design.md`](docs/specs/2026-06-06-tcpquic-proxy-design.md)。

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
| [lz4](https://github.com/lz4/lz4) | `third_party/lz4` | 流式压缩 |
| [zstd](https://github.com/facebook/zstd) | `third_party/zstd` | 流式压缩 |

当前 pin：msquic `v2.5.2-24-g0efce2bc9`、lz4 `v1.10.0`、zstd `v1.5.7`。

**不需要** 安装 `libzstd-dev`、`liblz4-dev`、`libssl-dev` 等系统库；`git submodule update --init --recursive` 会拉齐上述源码并由 CMake 一并构建。

### 构建工具

| 工具 | 说明 |
|------|------|
| CMake ≥ 3.16 | 构建系统 |
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

`tcpquic-proxy` 二进制依赖 Linux/POSIX 标准接口（socket、pthread 等），**不依赖** 系统安装的 libzstd / liblz4 / libssl。构建产物与 vendored 静态库、`libmsquic.so` 同目录部署即可（CMake 已设置 `$ORIGIN` RPATH）。

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
| `QUIC_USE_SYSTEM_LIBCRYPTO` | `OFF` | 设为 `ON` 时 msquic 使用系统 `libcrypto`，而非 vendored quictls |

## 构建

本项目统一使用仓库根目录下的 `build/` 作为本地 CMake 构建目录。不要使用旧的
`build-gcc10/`、`build-gcc10-cxx17/`、`build-gcc10-fixed/` 等临时目录；源码或
`src/CMakeLists.txt` 更新后，应重新配置 `build/`。

```bash
git submodule update --init --recursive
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release \
  -DLZ4_SOURCE_DIR="$PWD/third_party/lz4" \
  -DCMAKE_C_COMPILER=/usr/bin/gcc10-gcc \
  -DCMAKE_CXX_COMPILER=/usr/bin/gcc10-g++
cmake --build build --target tcpquic-proxy -j$(nproc)
```

若 CMake 缓存中 `LZ4_SOURCE_DIR` 不正确，请删除 `build/` 后重新配置，或始终传入 `-DLZ4_SOURCE_DIR`。

产物：`build/bin/Release/tcpquic-proxy`

### Windows 10/11 x64

Install Visual Studio 2022 Build Tools with the MSVC C++ workload, CMake, Ninja, Git, OpenSSL, Python, and curl.

```powershell
git submodule update --init --recursive
cmake -S . -B build-x64 -A x64 -DLZ4_SOURCE_DIR="$PWD/third_party/lz4"
cmake --build build-x64 --config Release --target tcpquic-proxy
```

Windows uses msquic Schannel by default, but Schannel-backed QUIC requires OS TLS 1.3 support. Per `third_party/msquic/docs/Platforms.md`, this means Windows Server 2022, Windows 11, or the latest Windows Insider Preview builds; ordinary Windows 10 releases do not provide Schannel TLS 1.3 cipher suites by default and can fail `ConfigurationLoadCredential` / `AcquireCredentialsHandleW` with `0x80090331` (`SEC_E_ALGORITHM_MISMATCH`). On Windows 10, prefer `-DTCPQUIC_WINDOWS_TLS=quictls` with Strawberry Perl, which restores Linux-style PEM CA loading. For Schannel-capable systems, PEM leaf certificates must include `serverAuth` and `clientAuth` EKU, and the CA in `--quic-ca` must be imported into the CurrentUser `Root` store (the loopback script does this automatically).

Run the Windows loopback validation:

```powershell
.\scripts\test-tcpquic-proxy-windows.ps1 -Compress off
.\scripts\test-tcpquic-proxy-windows.ps1 -Compress zstd
.\scripts\test-tcpquic-proxy-windows.ps1 -Compress lz4
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
  tcpquic_linux_relay_buffer_pool_test \
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

覆盖 happy path、ACL/DNS/refused 负路径、zstd/lz4 端到端、`--quic-connections 4` 连接池。

### 并发隧道压测

```bash
# 默认 100 条并发 HTTP CONNECT 隧道
./scripts/test-tcpquic-concurrent.sh

# 128 隧道 + 8 条 QUIC 连接 + lz4
TUNNELS=128 QUIC_CONNECTIONS=8 COMPRESS=lz4 ./scripts/test-tcpquic-concurrent.sh
```

### 双机冒烟（DGX / 直连链路）

在对端可 SSH 的前提下，于本机构建二进制后运行：

```bash
./scripts/test-tcpquic-proxy-dgx.sh
TUNNELS=100 COMPRESS=lz4 ./scripts/test-tcpquic-proxy-dgx.sh
```

默认对端 `jack@169.254.59.196`，本机出口 `169.254.250.230`；可通过 `PEER`、`TARGET`、`BIND` 覆盖。2026-06-10 Linux 验证记录见 [`docs/linux-verification-2026-06-10.md`](docs/linux-verification-2026-06-10.md)。

### 性能基线

```bash
# 对比裸 TCP、隧道 off/zstd/lz4
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
| `--compress` | 双方 | `auto` | `auto` / `zstd` / `lz4` / `off` |
| `--compress-level` | 双方 | `1` | zstd 压缩等级 |
| `--allow-targets` | server | （必填） | 逗号分隔 CIDR 白名单 |
| `--deny-targets` | server | 空 | 逗号分隔 CIDR 黑名单 |

- 非法 CIDR 在启动时直接报错（不会静默忽略）。
- 不支持 `--compress-min-size`（压缩在 OPEN 阶段协商，per-stream 流式）。

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
3. **数据阶段：** 可选 zstd / lz4 流式压缩；TCP FIN 对应 Stream FIN。

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

```
src/
├── main.cpp                  # client / server 入口
├── config.*                  # CLI 解析
├── control_protocol.*        # OPEN 帧编解码
├── acl.*                     # CIDR ACL + DNS 候选过滤
├── compress.*                # zstd / lz4 流式压缩/解压
├── quic_session.*            # QUIC 会话（mTLS、BBR 参数）
├── tcp_tunnel.*              # 隧道状态机（OPEN + relay）
├── tcp_dialer.*              # B 侧非阻塞 TCP 拨号
├── socks5_server.*           # SOCKS5 CONNECT
├── http_connect_server.*     # HTTP CONNECT
├── relay.*                   # 生产 relay 入口，Linux 选择 worker 后端
├── linux_relay_worker.*      # Linux epoll/readv/writev relay worker（含压缩隧道）
├── linux_relay_buffer_pool.* # Linux relay 池化缓冲
├── relay_blocking_demo.*     # demo/legacy blocking relay（非生产链接）
├── router_runtime.*          # 连接池与路由
├── server_metrics.*          # 运行时指标与 admin HTTP 序列化
├── admin_http.*              # admin/metrics HTTP 端点
├── thread_pool.*             # 工作线程池
├── tuning.*                  # 自适应调参
├── tunnel_reaper.*           # 隧道延迟回收
├── tcp_write_queue.*         # demo/legacy 写队列与独立单测
└── unittest/                 # 单元测试

third_party/
├── msquic/                   # msquic 子模块（QUIC 栈；内含 submodules/quictls）
├── lz4/                      # lz4 子模块（压缩）
└── zstd/                     # zstd 子模块（压缩）
scripts/                      # 集成测试与性能脚本
docs/                         # 设计规格与实现计划
```

## v1 能力与限制

**已实现：**

- 单 QUIC 长连接或连接池（`--quic-connections`）、1 TCP = 1 Stream
- SOCKS5 CONNECT（NO AUTH）、HTTP CONNECT 隧道
- 动态目标（B 侧 DNS + TCP 拨号）
- mTLS、CIDR ACL、zstd / lz4 流式压缩

**未实现（规格预留）：**

- SOCKS / HTTP 用户凭证认证
- QUIC 断连后在途隧道透明迁移（断连会关闭在途流，QUIC 自动重连）
- UDP 代理、普通 HTTP 代理（非 CONNECT）

## 相关文档

| 文档 | 说明 |
|------|------|
| [`docs/specs/2026-06-06-tcpquic-proxy-design.md`](docs/specs/2026-06-06-tcpquic-proxy-design.md) | 设计规格 |
| [`docs/specs/2026-06-09-tcpquic-proxy-repo-restructure-design.md`](docs/specs/2026-06-09-tcpquic-proxy-repo-restructure-design.md) | 独立仓库重构设计 |
| [`docs/plans/2026-06-06-tcpquic-proxy.md`](docs/plans/2026-06-06-tcpquic-proxy.md) | 实现计划 |
| [`docs/plans/2026-06-06-tcpquic-thread-model.md`](docs/plans/2026-06-06-tcpquic-thread-model.md) | 线程模型 |
| [`docs/plans/2026-06-06-tcpquic-adaptive-tuning.md`](docs/plans/2026-06-06-tcpquic-adaptive-tuning.md) | 自适应调参 |
| [`docs/plans/2026-06-07-tcpquic-remaining-work.md`](docs/plans/2026-06-07-tcpquic-remaining-work.md) | 剩余工作 |
| [`docs/plans/2026-06-09-tcpquic-proxy-repo-restructure.md`](docs/plans/2026-06-09-tcpquic-proxy-repo-restructure.md) | 独立仓库迁移计划 |
| [`docs/tcpquic_next_steps.md`](docs/tcpquic_next_steps.md) | 后续步骤 |
