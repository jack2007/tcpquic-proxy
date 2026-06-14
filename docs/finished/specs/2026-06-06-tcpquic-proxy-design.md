# TCP-over-QUIC 代理设计规格

> 状态：已批准（2026-06-06）  
> 基于项目：msquic  
> 目标工具：`src/tools/tcpquic-proxy/`

## 1. 背景与目标

### 1.1 问题陈述

两个网络节点 A、B 之间，现有应用通过传统 TCP C/S 模式通信。希望在 A、B 各部署一个代理进程，预先建立 QUIC 连接，将应用侧的 TCP 流量封装到 QUIC Stream 中传输，实现：

1. **网络穿透（场景 A）**：TCP 流量经 QUIC/UDP 隧道穿越受限网络、NAT 或防火墙
2. **差网友优化（场景 B）**：利用 QUIC + BBR + 大窗口等手段，在高延迟、丢包环境下改善传输体验
3. **带宽优化**：可选 zstd/lz4 压缩，降低有效传输字节数

### 1.2 非目标（v1）

- 不支持 UDP 代理（仅 TCP）
- 不支持 HTTP 普通代理（`GET http://...`），仅 HTTP CONNECT 隧道
- 不支持 SOCKS5 用户名/密码认证（协议预留，v2 实现）
- 不支持 QUIC 连接池（配置项预留，v1 固定单连接）
- 不支持在途 TCP 隧道的透明 QUIC 迁移（断连后关闭在途流，自动重连 QUIC）

## 2. 架构概览

### 2.1 拓扑

```
[A 节点]
  TCP 应用
    ↓ (配置 SOCKS5 / HTTP CONNECT 代理)
  ProxyA (tcpquic-proxy client)
    ↓ QUIC/UDP 长连接（mTLS）
  ProxyB (tcpquic-proxy server)
    ↓ TCP connect（动态目标）
  [B 侧网络] 任意 TCP 服务

映射关系：1 个 TCP 连接 = 1 个 QUIC 双向 Stream
```

### 2.2 进程模式

| 模式 | 部署位置 | 职责 |
|------|----------|------|
| `client` | A 节点 | SOCKS5 + HTTP CONNECT 入口、压缩、QUIC 客户端、长连接维护 |
| `server` | B 节点 | QUIC 监听、mTLS 校验、ACL、解压、TCP 拨号 |

### 2.3 模块划分

**ProxyA (client)**

```
FrontEnd
  ├─ Socks5Listener
  └─ HttpConnectListener
TunnelCore
  ├─ QuicSession       (长连接 + 重连)
  ├─ StreamTable       (tcp_fd ↔ HQUIC stream)
  ├─ ControlCodec      (OPEN/OPEN_OK/OPEN_FAIL)
  ├─ Compressor        (zstd/lz4/none)
  └─ RelayEngine       (背压 + 双向转发)
```

**ProxyB (server)**

```
QuicListener + mTLS
TunnelCore
  ├─ AclEngine         (CIDR 白/黑名单)
  ├─ TcpDialer         (B 侧 DNS + TCP connect)
  ├─ Decompressor
  └─ RelayEngine
```

### 2.4 QUIC 连接模型

- **v1**：默认 `--quic-connections 1`（单条长连接）
- **预留**：`--quic-connections N`（N>1 时 v2 启用连接池，OPEN 响应 `conn_id` 字段路由）
- 启动时建立 QUIC 连接；运行期新 TCP 隧道仅新建 Stream，不重复握手

### 2.5 与现有 msquic 组件的关系

| 现有组件 | 复用方式 |
|----------|----------|
| `src/tools/forwarder/forwarder.cpp` | Stream 双向 relay、`ForwardedSend`、背压模式 |
| `src/tools/sample/sample.c` | Stream 创建/收发 API 参考 |
| BBR/窗口调参经验 | `QUIC_SETTINGS` 默认值 |

## 3. 前端协议（应用 → ProxyA）

### 3.1 SOCKS5

- 监听：默认 `127.0.0.1:1080`（可配置）
- v1 支持：NO AUTH + CONNECT
- 不支持：BIND、UDP ASSOCIATE、USERNAME/PASSWORD（v2 预留）

### 3.2 HTTP CONNECT

- 监听：默认 `127.0.0.1:8080`（可配置）
- v1 支持：`CONNECT host:port HTTP/1.1` → `200 Connection Established`
- 失败映射：见 §4.3 error_code 表

### 3.3 统一内部表示

```c
typedef struct TunnelRequest {
    uint8_t  AddrType;    // 0x01=IPv4, 0x02=IPv6, 0x03=DOMAIN
    char     Host[256];
    uint16_t Port;
    uint8_t  CompressFlags;
} TunnelRequest;
```

两种前端协议解析后均生成 `TunnelRequest`，交由同一 `TunnelCore` 处理。

## 4. 隧道控制协议（QUIC Stream 上）

### 4.1 设计原则

- 每个 Stream 先控制握手，再进入数据阶段
- 所有多字节整数：**网络字节序（大端）**
- Magic：`0x54 0x51`（"TQ"）

### 4.2 OPEN 请求（A → B，Stream 首包）

| 偏移 | 长度 | 字段 | 说明 |
|------|------|------|------|
| 0 | 2 | magic | `0x54 0x51` |
| 2 | 1 | version | `0x01` |
| 3 | 1 | cmd | `0x01` = OPEN |
| 4 | 1 | flags | 见 §4.4 |
| 5 | 1 | addr_type | `0x01` IPv4 / `0x02` IPv6 / `0x03` DOMAIN |
| 6 | 2 | port | uint16 |
| 8 | 2 | addr_len | IPv4=4, IPv6=16, DOMAIN=域名长度 |
| 10 | N | addr | 地址字节或域名字符串（无 NUL） |
| 10+N | 1 | auth_present | v1 固定 `0` |

### 4.3 OPEN 响应（B → A）

响应帧固定 9 字节，`OK` 与 `FAIL` 使用同一布局，便于一次性读取和解码。

| 偏移 | 长度 | 字段 | 说明 |
|------|------|------|------|
| 0 | 2 | magic | `0x54 0x51` |
| 2 | 1 | version | `0x01` |
| 3 | 1 | cmd | `0x02` OK / `0x03` FAIL |
| 4 | 1 | error_code | OK 固定 `0x00`；FAIL 见下表 |
| 5 | 4 | conn_id | v1 固定 `0` |

**error_code 映射：**

| 值 | 含义 | SOCKS5 | HTTP CONNECT |
|----|------|--------|--------------|
| `0x00` | 成功 | `0x00` | `200` |
| `0x01` | ACL 拒绝 | `0x02` | `403` |
| `0x02` | DNS 失败 | `0x04` | `502` |
| `0x03` | TCP 超时 | `0x05` | `504` |
| `0x04` | TCP 拒绝 | `0x05` | `502` |
| `0x05` | 内部错误 | `0x01` | `500` |

### 4.4 flags 位定义

| 位 | 含义 |
|----|------|
| `0x01` | 启用压缩 |
| `0x02` | 算法：0=zstd，1=lz4（需同时置 `0x01`） |
| `0x04` | DNS 由 B 侧解析（DOMAIN 时必须） |
| `0x08–0x80` | 保留 |

### 4.5 数据阶段

- `OPEN_OK` 之后 Stream 上仅为载荷字节
- 压缩启用时：zstd/lz4 流式压缩帧
- TCP FIN → Stream FIN；TCP RST/全关 → Stream abort/shutdown

### 4.6 超时

| 阶段 | 默认值 |
|------|--------|
| OPEN 等待响应 | 10s |
| B 侧 TCP connect | 10s |
| QUIC Idle | 60s（> KeepAlive 15s） |

## 5. 安全模型

### 5.1 v1（必做）

1. **mTLS**：ProxyB 要求客户端证书；ProxyA 校验服务端证书
2. **ALPN**：`tcpquic-tunnel/1`
3. **B 侧 ACL**：CIDR 白名单 `--allow-targets`；可选 `--deny-targets`
   - deny 优先于 allow
   - IP 字面量目标按规范化后的地址直接校验
   - DOMAIN 目标必须由 B 侧解析；对解析得到的全部 A/AAAA 候选地址逐一执行 ACL，存在 deny 或不存在 allow 时拒绝；TCP 拨号只能使用已通过 ACL 的地址
   - DNS 解析失败返回 `OPEN_FAIL(0x02)`，ACL 拒绝返回 `OPEN_FAIL(0x01)`
4. **本地绑定**：SOCKS/HTTP 默认仅 `127.0.0.1`

### 5.2 v2（协议预留）

- OPEN 帧 `auth_present` / `auth_type` / `auth_payload`
- SOCKS5 USERNAME/PASSWORD、HTTP Proxy-Authorization
- 按凭证的 per-user ACL

## 6. 压缩策略

### 6.1 配置

```bash
--compress auto|zstd|lz4|off   # 默认 auto
--compress-level 1             # zstd 等级 1-3
```

### 6.2 决策流程

1. 全局 `off` → OPEN flags 不带 COMPRESS
2. `auto` 默认选择 zstd；显式 `zstd`/`lz4` 按配置选择算法
3. 压缩是否启用在 OPEN 请求阶段确定；进入数据阶段后不得在同一 Stream 内切换压缩状态
4. per-stream 独立压缩状态；使用流式 API
5. Stream 结束时 flush 压缩器

### 6.3 v1 不做

- 跨 Stream 共享字典
- 内容类型嗅探
- 基于数据长度的压缩动态启停
- 压缩后更大回退

## 7. QUIC 参数建议

```yaml
alpn: tcpquic-tunnel/1
congestion_control: bbr
keep_alive_ms: 15000
idle_timeout_ms: 60000
peer_bidi_stream_count: 1000
stream_recv_window: 16777216      # 16MB，按压测调整
conn_flow_control_window: 67108864
server_resumption: enabled
```

## 8. 错误处理

| 事件 | v1 行为 |
|------|---------|
| QUIC 连接断开 | 自动重连；在途 TCP 隧道全部关闭 |
| OPEN 超时 | 向应用返回连接失败 |
| ACL 拒绝 | OPEN_FAIL(0x01) + 日志 |
| 压缩错误 | 关闭该 Stream + 日志 |

## 9. CLI 接口

### 9.1 Client

```bash
tcpquic-proxy client \
  --socks-listen 127.0.0.1:1080 \
  --http-listen 127.0.0.1:8080 \
  --quic-peer proxy-b.example.com:443 \
  --quic-cert /path/client.pem \
  --quic-key  /path/client.key \
  --quic-ca   /path/ca.pem \
  --quic-connections 1 \
  --compress auto
```

### 9.2 Server

```bash
tcpquic-proxy server \
  --quic-listen 0.0.0.0:443 \
  --quic-cert /path/server.pem \
  --quic-key  /path/server.key \
  --quic-ca   /path/ca.pem \
  --allow-targets 10.0.0.0/8,192.168.0.0/16 \
  --deny-targets 127.0.0.0/8 \
  --compress auto
```

## 10. 代码结构

```
src/tools/tcpquic-proxy/
├── CMakeLists.txt
├── main.cpp
├── config.h / config.cpp
├── control_protocol.h / control_protocol.cpp
├── acl.h / acl.cpp
├── compress.h / compress.cpp
├── relay.h / relay.cpp
├── quic_session.h / quic_session.cpp
├── tcp_tunnel.h / tcp_tunnel.cpp
├── socks5_server.h / socks5_server.cpp
├── http_connect_server.h / http_connect_server.cpp
└── tcp_dialer.h / tcp_dialer.cpp
```

**依赖：** msquic（必需）、libzstd（必需）、liblz4（可选 CMake 开关）

## 11. 测试与验收

### 11.1 测试层级

| 层级 | 内容 |
|------|------|
| 单元测试 | OPEN 帧编解码、ACL CIDR、压缩往返 |
| 集成测试 | curl 经 HTTP CONNECT；redis-cli/nc 经 SOCKS5 |
| 故障测试 | ACL 拒绝、目标不可达、QUIC 断连重连 |
| 性能测试 | 裸 TCP vs 隧道 vs 隧道+压缩（netem 场景） |

### 11.2 v1 验收标准

1. SOCKS5 + HTTP CONNECT 均可穿透到 B 侧动态目标
2. mTLS + ACL 生效
3. zstd 对文本流量有可测压缩收益
4. 100ms+5% 丢包下隧道稳定工作
5. 单 QUIC 连接支持 ≥100 并发 TCP 隧道

## 12. 版本路线图

| 版本 | 内容 |
|------|------|
| v1 | 单 QUIC 连接、SOCKS5、HTTP CONNECT、mTLS、ACL、zstd |
| v1.1 | lz4、YAML 配置、更细日志 |
| v2 | 连接池、用户凭证、per-user ACL、可压缩性检测 |
