# tcpquic-proxy

基于 [msquic](https://github.com/microsoft/msquic) 的 **TCP-over-QUIC 隧道代理**。在 A、B 两节点各运行一个进程，预先建立 QUIC 长连接；本地应用通过 **SOCKS5** 或 **HTTP CONNECT** 接入代理，每个 TCP 连接映射为一条 QUIC 双向 Stream，由 B 侧按动态目标发起 TCP 拨号。

**典型场景：**

- 穿越防火墙 / NAT，将 TCP 流量封装在 QUIC/UDP 上
- 高延迟、丢包环境下利用 QUIC + BBR 改善传输
- 可选 zstd 流式压缩降低有效字节数

详细设计见 [`docs/finished/specs/2026-06-06-tcpquic-proxy-design.md`](../docs/finished/specs/2026-06-06-tcpquic-proxy-design.md)。

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

## 构建

**依赖：** 见仓库根目录 [`README.md`](../README.md#依赖)。msquic / quictls / zstd 均已 vendored，无需系统 `libzstd-dev` 等 dev 包。

```bash
git submodule update --init --recursive
mkdir -p build && cd build
cmake .. -DCMAKE_BUILD_TYPE=Release
cmake --build . --target tcpquic-proxy -j$(nproc)
```

产物：`build/bin/Release/tcpquic-proxy`

### 单元测试

```bash
cmake --build . --target tcpquic_control_test tcpquic_acl_test \
  tcpquic_compress_test tcpquic_tunnel_test \
  tcpquic_socks5_test tcpquic_http_connect_test -j$(nproc)
./bin/Release/tcpquic_*_test
```

### 集成测试

```bash
./scripts/test-tcpquic-proxy.sh
```

覆盖 happy path、ACL/DNS/refused 负路径、zstd 端到端、`--quic-connections 4` 连接池。

### 并发隧道压测

```bash
# 默认 100 条并发 HTTP CONNECT 隧道
./scripts/test-tcpquic-concurrent.sh

# 128 隧道 + 8 条 QUIC 连接 + zstd
TUNNELS=128 QUIC_CONNECTIONS=8 COMPRESS=zstd ./scripts/test-tcpquic-concurrent.sh
```

### 性能基线（Task 14）

```bash
# 对比裸 TCP、隧道 off/zstd（结果写入 research_progress.md）
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
tcpquic-proxy server \
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
tcpquic-proxy client \
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
| `--compress` | 双方 | `auto` | `auto` / `zstd` / `off` |
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

早期实现参考了 msquic forwarder tool 的 Stream relay 模式；当前代码已作为独立仓库维护。

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
| [`docs/finished/specs/2026-06-06-tcpquic-proxy-design.md`](../docs/finished/specs/2026-06-06-tcpquic-proxy-design.md) | 设计规格 |
| [`docs/finished/plans/2026-06-06-tcpquic-proxy.md`](../docs/finished/plans/2026-06-06-tcpquic-proxy.md) | 实现计划 |
| [`docs/tcpquic_next_steps.md`](../docs/tcpquic_next_steps.md) | 后续工作 |
