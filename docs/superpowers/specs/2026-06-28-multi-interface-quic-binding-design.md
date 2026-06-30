# MsQuic 多网口绑定设计

> 状态：设计方案  
> 日期：2026-06-28  
> 范围：server 端多 IP 监听展开、client 端 peer 多地址和显式 path 绑定、连接槽到路径的稳定分配、配置兼容和验证方案

---

## 1. 背景

当前 tcpquic-proxy 使用 MsQuic 作为 QUIC 栈。server 侧通过 `--listen` / `server.proto_listen` 配置 QUIC 监听地址，常见配置为 `0.0.0.0:443`。client 侧通过 `--peer` / `quic_peer` 配置单个 server QUIC 地址，并通过 `--connections` / `quic_connections` 建立多条 QUIC 长连接。

在多网口环境下，现有行为存在两个问题：

- server 多网口时，通配监听可能接收来自非默认网口的流量，但响应经默认路由发出，导致五元组或源地址不匹配，连接失败。
- client 多网口时，多条 QUIC 连接默认仍由系统路由选择本地出口，无法明确表达“移动网口连移动入口、电信网口连电信入口”这类运营商同网互联路径。

本设计把“地址绑定”建模为两层：

- server 侧只保留 `listen` 配置。通配地址由程序展开为具体本机 IP 并分别启动 MsQuic listener；显式地址列表直接逐项绑定。
- client 侧支持两种模式：未配置 `paths` 时使用 `peer` 地址列表并由系统默认路由选择出口；配置 `paths` 时每条 path 显式绑定 `local -> peer`，并忽略 `peer`。

---

## 2. 目标

### 2.1 功能目标

- server 端 `listen` 支持单地址、地址列表、IPv4 通配、IPv6 通配。
- server 端 `listen=0.0.0.0:port` 自动枚举本机非本地 IPv4 地址并逐个绑定。
- server 端 `listen=[::]:port` 自动枚举本机非本地 IPv6 地址并逐个绑定。
- server 端 `listen=ip1:port,ip2:port` 只绑定用户指定地址，不再额外枚举。
- client 端 `peer` 支持 `ip:port` 列表；未配置 `paths` 时连接槽按 peer 列表 round-robin 连接，出口由系统默认路由决定。
- client 端 `paths` 支持显式 `local`、`peer`、`connections`、`name`，用于固定本地网口和 server 网口的对应关系。
- client 端配置了 `paths` 后忽略 `peer` 和全局 `connections`，总连接数等于所有 path 的 `connections` 之和。
- client 端新接入的 TCP tunnel 在同一 peer 的已连接 QUIC connection slot 之间轮询分配；单个 TCP tunnel 生命周期内绑定到一个 QUIC connection，不跨连接迁移。
- admin snapshot、trace、日志中能看到每个 QUIC connection slot 的 path 名称、本地地址和 peer 地址。

### 2.2 工程目标

- 不引入额外 `bind_addr` / `bind_addrs` 参数，避免 server 配置表面膨胀。
- 优先使用 MsQuic 已有 API：server 多 listener，client `QUIC_PARAM_CONN_LOCAL_ADDRESS` / `MsQuicConnection::SetLocalAddr()`。
- 保持旧配置兼容。单 peer、单 listen、单 connection 的行为不变。
- 把地址解析、列表解析、通配枚举和路径展开放入小而清晰的 helper，避免继续扩大 `quic_session.cpp` 的职责。
- 失败隔离到 listener 或 connection slot。某个 IP 绑定失败或某条 path 连接失败时，错误信息可定位到具体地址。

### 2.3 非目标

- 不修改系统路由表，不依赖策略路由自动创建。
- 不实现 QUIC multipath 协议扩展；这里的“多路径”是多条 QUIC connection 分布在不同本地/远端地址上。
- 不实现单个 TCP tunnel 跨多条 QUIC connection 的分片、重组或迁移。第一版只让多个并发 TCP tunnel 分摊到多条 QUIC connection。
- 第一版不做 path 自动 failover。运营商同网路径通常是强约束，自动跨运营商切换容易引入误判。
- 不支持 hostname 出现在 `paths.local`。本地绑定必须是具体 IP。

---

## 3. 配置模型

### 3.1 Server 配置

server 仍使用现有 `listen` 语义，只扩展取值：

```json
{
  "server": {
    "proto_listen": "0.0.0.0:443"
  }
}
```

通配地址展开为本机有效地址：

```text
0.0.0.0:443
  -> 36.1.1.10:443
  -> 59.1.1.10:443
```

用户也可以显式指定列表：

```json
{
  "server": {
    "proto_listen": "36.1.1.10:443,59.1.1.10:443"
  }
}
```

CLI 等价形式：

```bash
tcpquic-proxy server --listen 36.1.1.10:443,59.1.1.10:443 --cert server.crt --key server.key --allow-targets 10.0.0.0/8
```

### 3.2 Client 非 path 模式

未配置 `paths` 时，`peer` 支持地址列表：

```json
{
  "peers": [
    {
      "peer_id": "server-b",
      "quic_peer": "36.1.1.10:443,59.1.1.10:443",
      "quic_connections": 8
    }
  ]
}
```

展开规则：

```text
slot 0 -> peer[0] 36.1.1.10:443
slot 1 -> peer[1] 59.1.1.10:443
slot 2 -> peer[0] 36.1.1.10:443
slot 3 -> peer[1] 59.1.1.10:443
slot 4..7 按相同规则继续轮询 peer[0] 和 peer[1]
```

本模式不绑定本地地址。client 出口由操作系统路由决定，因此该模式保证“连多个 server 入口”，不保证“用多个 client 网口”。

### 3.3 Client path 模式

配置 `paths` 后，每条 path 显式描述本地 IP 和远端 peer：

```json
{
  "peers": [
    {
      "peer_id": "server-b",
      "paths": [
        {
          "name": "cmcc",
          "local": "10.10.1.2",
          "peer": "36.1.1.10:443",
          "connections": 4
        },
        {
          "name": "ctcc",
          "local": "10.20.1.2",
          "peer": "59.1.1.10:443",
          "connections": 4
        }
      ]
    }
  ]
}
```

展开规则：

```text
slot 0..3 -> path cmcc, local 10.10.1.2 -> peer 36.1.1.10:443
slot 4..7 -> path ctcc, local 10.20.1.2 -> peer 59.1.1.10:443
```

path 模式下：

- `paths` 非空时忽略 peer 级 `quic_peer`。
- `paths` 非空时忽略 peer 级 `quic_connections`。
- 每条 path 的 `connections` 范围为 `1..128`。
- 所有 path 的连接数总和范围为 `1..128`。
- `name` 必须非空且在同一 peer 内唯一。
- `local` 必须是本地 IP 地址，不接受端口，不接受 hostname。
- `peer` 必须是 `host:port` 或 `[ipv6]:port`，第一版建议文档要求生产配置使用具体 IP。

### 3.4 兼容字段

保留现有字段：

```json
{
  "peer_id": "primary",
  "quic_peer": "203.0.113.10:443",
  "quic_connections": 4
}
```

新的规范化结构在内存中表示为：

```cpp
struct TqQuicPathConfig {
    std::string Name;
    std::string LocalAddress;
    std::string Peer;
    uint32_t Connections{0};
};

struct TqQuicPathRuntime {
    std::string Name;
    std::string LocalAddress;
    std::string PeerHost;
    uint16_t PeerPort{0};
};
```

旧配置在 finalize 或 session start 时规范化为自动 path：

```text
quic_peer=203.0.113.10:443, quic_connections=4
  -> slot 0..3: Name=default, LocalAddress="", Peer=203.0.113.10:443
```

peer 列表模式规范化为多个自动 path，但 `LocalAddress` 为空：

```text
quic_peer=36.1.1.10:443,59.1.1.10:443, quic_connections=4
  -> slot 0: auto-peer-1, LocalAddress="", Peer=36.1.1.10:443
  -> slot 1: auto-peer-2, LocalAddress="", Peer=59.1.1.10:443
  -> slot 2: auto-peer-1, LocalAddress="", Peer=36.1.1.10:443
  -> slot 3: auto-peer-2, LocalAddress="", Peer=59.1.1.10:443
```

### 3.5 TCP tunnel 调度语义

tcpquic-proxy 的数据模型保持不变：

```text
1 个接入 TCP 连接 = 1 条 QUIC bidirectional stream
1 条 QUIC stream 属于 1 条 QUIC connection
同一个 peer 可以有多条 QUIC connection slot
```

当 SOCKS5、HTTP CONNECT 或 port forward 接入一个新的 TCP 连接时，client runtime 会为这个 tunnel 选择同一 peer 下的一条已连接 QUIC connection，并在这条 connection 上打开一条新的 bidirectional stream。该 TCP tunnel 生命周期内固定在这个 QUIC connection 上，直到 tunnel 结束或连接异常关闭。

第一版调度策略为 `round_robin_connected_slots`：

- 使用 peer 内的全局原子 `PickIndex` 递增。
- 对 connection slot 做 round-robin。
- 遇到未连接、retry 中或无效的 slot 时跳过。
- 如果没有任何 connected slot，新 TCP tunnel 拒绝打开，client 本地监听在所有连接断开时也会关闭或保持不可接受状态。

示例：

```text
paths:
  cmcc connections=4 -> slot 0..3
  ctcc connections=4 -> slot 4..7

tcp tunnel 1 -> slot 0 -> cmcc
tcp tunnel 2 -> slot 1 -> cmcc
tcp tunnel 3 -> slot 2 -> cmcc
tcp tunnel 4 -> slot 3 -> cmcc
tcp tunnel 5 -> slot 4 -> ctcc
tcp tunnel 6 -> slot 5 -> ctcc
tcp tunnel 7 -> slot 6 -> ctcc
tcp tunnel 8 -> slot 7 -> ctcc
tcp tunnel 9 -> slot 0 -> cmcc
```

因此，`connections` 在第一版同时承担两个含义：

- 为该 path 建立多少条 QUIC connection。
- 在默认 round-robin 下，该 path 获取的新 TCP tunnel 份额权重。比如 `cmcc=6`、`ctcc=2` 时，新 tunnel 长期分配比例约为 `3:1`。

该策略能让“多个并发 TCP 连接”利用同一 peer 的多条连接和多条网口路径，但不能让“单个 TCP 连接”聚合多条连接的带宽。单 TCP 流跨路径聚合需要 QUIC multipath 或应用层分片重组，不属于第一版范围。

---

## 4. 组合场景推演

### 4.1 单网口 client / 单网口 server

server：

```json
{ "server": { "proto_listen": "203.0.113.10:443" } }
```

client：

```json
{
  "peers": [
    { "peer_id": "server-b", "quic_peer": "203.0.113.10:443", "quic_connections": 4 }
  ]
}
```

结果：

```text
client 默认出口 -> 203.0.113.10:443
```

正常工作，行为与旧版本一致。

### 4.2 单网口 client / 多网口 server

server：

```json
{ "server": { "proto_listen": "0.0.0.0:443" } }
```

server 展开：

```text
36.1.1.10:443
59.1.1.10:443
```

client：

```json
{
  "peers": [
    { "peer_id": "server-b", "quic_peer": "36.1.1.10:443,59.1.1.10:443", "quic_connections": 4 }
  ]
}
```

结果：

```text
slot 0/2 -> client 默认出口 -> 36.1.1.10:443
slot 1/3 -> client 默认出口 -> 59.1.1.10:443
```

server 响应从对应 listener 的本地地址发出，避免通配监听下的默认路由错口问题。

### 4.3 多网口 client / 单网口 server，未配置 paths

server：

```json
{ "server": { "proto_listen": "203.0.113.10:443" } }
```

client：

```json
{
  "peers": [
    { "peer_id": "server-b", "quic_peer": "203.0.113.10:443", "quic_connections": 8 }
  ]
}
```

结果：

```text
slot 0..7 -> OS 默认路由出口 -> 203.0.113.10:443
```

该场景可用，但不能保证利用 client 多网口。

### 4.4 多网口 client / 单网口 server，配置 paths

client：

```json
{
  "peers": [
    {
      "peer_id": "server-b",
      "paths": [
        { "name": "client-a", "local": "10.10.1.2", "peer": "203.0.113.10:443", "connections": 4 },
        { "name": "client-b", "local": "10.20.1.2", "peer": "203.0.113.10:443", "connections": 4 }
      ]
    }
  ]
}
```

结果：

```text
10.10.1.2 -> 203.0.113.10:443
10.20.1.2 -> 203.0.113.10:443
```

如果其中一个本地 IP 没有到 server 的有效路由，该 path 的连接槽失败并重试，其他 path 不受影响。

### 4.5 多网口 client / 多网口 server，未配置 paths

server：

```json
{ "server": { "proto_listen": "0.0.0.0:443" } }
```

client：

```json
{
  "peers": [
    { "peer_id": "server-b", "quic_peer": "36.1.1.10:443,59.1.1.10:443", "quic_connections": 8 }
  ]
}
```

结果：

```text
client 默认出口 -> 36.1.1.10:443
client 默认出口 -> 59.1.1.10:443
```

该场景能连接多个 server 入口并解决 server 回包错网口问题，但 client 侧出口仍由系统路由决定。

### 4.6 多网口 client / 多网口 server，配置 paths

server：

```json
{ "server": { "proto_listen": "0.0.0.0:443" } }
```

client：

```json
{
  "peers": [
    {
      "peer_id": "server-b",
      "paths": [
        { "name": "cmcc", "local": "10.10.1.2", "peer": "36.1.1.10:443", "connections": 4 },
        { "name": "ctcc", "local": "10.20.1.2", "peer": "59.1.1.10:443", "connections": 4 },
        { "name": "cucc", "local": "10.30.1.2", "peer": "221.1.1.10:443", "connections": 4 }
      ]
    }
  ]
}
```

结果：

```text
client 移动 -> server 移动
client 电信 -> server 电信
client 联通 -> server 联通
```

这是中国三大运营商同网互联场景的推荐生产配置。

---

## 5. 实现设计

### 5.1 地址解析 helper

新增 `src/protocol/quic_address.h` 和 `src/protocol/quic_address.cpp`，集中处理：

- comma-separated endpoint list 解析。
- host:port / [ipv6]:port 解析。
- server listen 通配展开。
- 本地地址枚举和过滤。
- client path 规范化。

核心接口：

```cpp
struct TqEndpoint {
    std::string Host;
    uint16_t Port{0};
};

struct TqResolvedListen {
    std::string Text;
    QUIC_ADDR Address{};
};

bool TqParseEndpointList(const std::string& value, std::vector<TqEndpoint>& out, std::string& err);
bool TqResolveServerListenList(const std::string& value, std::vector<TqResolvedListen>& out, std::string& err);
bool TqMakeQuicAddr(const TqEndpoint& endpoint, QUIC_ADDR& out);
```

`quic_session.cpp` 里的匿名 `TqEndpoint`、`ParseEndpoint()`、`ConvertListenAddress()` 后续迁移到 helper。

### 5.2 本地地址枚举

POSIX 使用 `getifaddrs()`：

- 跳过 `ifa_addr == nullptr`。
- 跳过非 `AF_INET` / `AF_INET6`。
- 跳过 loopback、unspecified、multicast。
- IPv6 跳过 link-local，避免不同接口上同名链路本地地址无法唯一表达。
- 跳过 down interface。
- 对同一个 IP 去重。

Windows 使用 `GetAdaptersAddresses()`：

- 跳过 `IfType == IF_TYPE_SOFTWARE_LOOPBACK`。
- 跳过 `OperStatus != IfOperStatusUp`。
- 跳过 loopback、unspecified、multicast、IPv6 link-local。
- 对同一个 IP 去重。

如果通配展开后没有任何可绑定地址，server 启动失败，错误信息包含原始 listen：

```text
listen 0.0.0.0:443 expanded to no usable local addresses
```

### 5.3 Server session

`QuicServerSession` 将：

```cpp
std::unique_ptr<MsQuicListener> Listener;
```

改为：

```cpp
std::vector<std::unique_ptr<MsQuicListener>> Listeners;
std::vector<std::string> ListenerTexts;
```

启动流程：

1. `TqResolveServerListenList(cfg.QuicListen, listens, err)`。
2. 对每个 resolved listen 创建 `MsQuicListener`。
3. 任一 listener open/start 失败则 `Stop()`，避免只启动部分地址造成配置误判。
4. 所有 listener 复用同一个 `Registration`、`Configuration` 和 callback。
5. metrics 中 `Listen` 保留原始配置；新增 admin/trace 字段展示 `resolved_listens`。

### 5.4 Client session

`TqPeerConfig` 新增：

```cpp
std::vector<TqQuicPathConfig> QuicPaths;
```

`QuicClientSession` 内部新增 slot path：

```cpp
struct ClientPath {
    std::string Name;
    std::string LocalAddress;
    std::string PeerHost;
    uint16_t PeerPort{0};
};

std::vector<ClientPath> Paths;
std::vector<size_t> SlotPathIndexes;
```

`Start()` 解析配置：

- 若 `Config.QuicPaths` 非空，按 path 的 `connections` 展开 slot。
- 若 `Config.QuicPaths` 为空，按 `Config.QuicPeer` 地址列表和 `Config.QuicConnections` 展开 slot。

`StartSlot(index)` 行为：

1. 取 `path = Paths[SlotPathIndexes[index]]`。
2. 创建 `MsQuicConnection`。
3. 如果 `path.LocalAddress` 非空，构造 `QUIC_ADDR`，端口为 0，调用 `SetLocalAddr()`。
4. 调用 `Start(*Configuration, path.PeerHost.c_str(), path.PeerPort)`。

本地地址绑定失败时：

- `slot.LastError` 记录 path 名称、本地 IP 和 MsQuic 返回码，例如 `SetLocalAddr failed for path cmcc local 10.10.1.2, 0x80410000`。
- 释放新连接。
- 调度该 slot retry。

### 5.5 Snapshot 和日志

`TqConnectionSnapshot` 增加：

```cpp
std::string PathName;
std::string LocalAddress;
std::string PeerAddress;
```

admin JSON 输出：

```json
{
  "connection_id": "conn-0",
  "slot_index": 0,
  "path": "cmcc",
  "local": "10.10.1.2",
  "peer": "36.1.1.10:443",
  "state": "connected"
}
```

启动日志：

```text
tcpquic-proxy: peer server-b path cmcc local 10.10.1.2 -> 36.1.1.10:443 (4 connections)
```

### 5.6 TCP tunnel 到 connection 的选择

现有 client 接入链路为：

```text
TqClientIngressReactor::StartClientOpen
  -> TqClientPeerRuntime::StartTunnel
      -> QuicClientSession::PickConnection
          -> TqStartClientTunnelAsync
```

本设计保留该链路。`QuicClientSession::PickConnection()` 是调度入口，第一版继续使用 round-robin connected slot：

```cpp
const size_t index = PickIndex.fetch_add(1) % count;
if (slot.Connected && slot.Connection && slot.Connection->IsValid()) {
    return slot.Connection.get();
}
```

path 功能落地后，`PickConnection()` 不需要理解运营商名称，只需要在规范化后的 slot 列表上轮询。path 的权重由 slot 数体现。

为便于观测，connection snapshot 中已有 `ActiveTunnels` 和 `TotalTunnels` 字段；实现时需要确认这些字段在 tunnel start/finish 时按 connection 维度更新。admin 可以据此回答：

- 当前每条 connection 承载多少 active tunnel。
- 每条 path 下所有 connection 累计承载多少 tunnel。
- tunnel 分配是否符合 path 的连接数权重。

后续可以在不改变配置模型的情况下增加策略：

| 策略 | 含义 | 适用场景 |
|------|------|----------|
| `round_robin_connected_slots` | 在已连接 slot 中轮询 | 第一版默认，简单稳定 |
| `least_active_tunnels` | 选择 active tunnel 最少的 slot | tunnel 时长差异很大时更均衡 |
| `least_pending_bytes` | 选择发送积压最小的 slot | 需要 relay/QUIC backlog 指标 |
| `hash_by_target` | 按目标地址固定到 slot/path | 减少同一目标的路径抖动 |

第一版不增加配置项，默认策略固定为 `round_robin_connected_slots`。

---

## 6. 系统测试设计

### 6.1 功能测试矩阵

| 场景 | 配置 | 期望 |
|------|------|------|
| 单 client / 单 server | 单 listen、单 peer | 行为兼容旧版本 |
| 单 client / 多 server | server `0.0.0.0`，client peer 列表 | server 展开多个 listener，连接成功 |
| 多 client / 单 server，无 path | client peer 单地址 | 可连接，但出口由默认路由决定 |
| 多 client / 单 server，有 path | 两个 local 指向同一 peer | 每个 slot 设置本地地址 |
| 多 client / 多 server，无 path | peer 多地址 | slot round-robin 到多个 peer |
| 多 client / 多 server，有 path | cmcc/ctcc/cucc path | slot 稳定映射到指定 local/peer |
| 多个 TCP tunnel / 同一 peer 多 slot | path 连接数为 4+4 | 新 tunnel 在 connected slot 间 round-robin，长期分配约 1:1 |
| 单个长 TCP tunnel / 同一 peer 多 slot | 任意多连接配置 | 单 tunnel 固定在一个 slot，不跨 slot 聚合带宽 |

### 6.2 性能基线

扩展现有 DGX 脚本，形成：

```text
1 local -> 1 peer
1 local -> 2 peers
2 locals -> 1 peer
2 locals -> 2 peers, no paths
2 locals -> 2 peers, paths
3 locals -> 3 peers, paths
```

指标：

- 总吞吐。
- 每 path 吞吐。
- 每 path active tunnel / total tunnel 分布。
- QUIC connection connected count。
- reconnect 次数。
- p95/p99 tunnel 建立耗时。
- NIC tx/rx byte 分布。

### 6.3 故障恢复

- 删除一个 path 的本地 IP：对应 slot retry，其他 path 继续 connected。
- server 停掉一个 listener IP：对应 peer/path slot retry，其他 listener 继续服务。
- 配置中包含不存在的 server 显式 listen IP：server 启动失败，不进入部分监听状态。
- client path local IP 非本机地址：对应 slot `SetLocalAddr` 失败并 retry，admin snapshot 显示错误。

---

## 7. 风险和约束

- 通配展开改变了 `0.0.0.0:443` 的底层绑定行为。旧版本是单通配 listener，新版本是多个具体 listener；如果用户依赖“后续新 IP 自动可用”，新版本需要重启进程才能枚举新 IP。
- IPv6 link-local 地址第一版跳过，避免缺少 scope id 的歧义。
- `paths.local` 只绑定源 IP，不保证系统存在从该源 IP 到 peer 的策略路由。没有路由时连接失败并 retry，这是预期行为。
- MsQuic 不同平台 datapath 对 `QUIC_PARAM_CONN_LOCAL_ADDRESS` 的失败状态码可能不同，测试断言应检查“失败并记录路径信息”，不要固定单一状态码。

---

## 8. 发布门禁

- `tcpquic_config_router_test` 覆盖 listen 列表、peer 列表、paths 解析和非法配置。
- `tcpquic_quic_session_reconnect_test` 覆盖 slot 到 path 的稳定映射和 retry 不串 path。
- `tcpquic_tunnel_test` 至少覆盖 path 模式下端到端连接成功。
- Linux 上用两个 loopback alias 或 network namespace 完成 `local -> peer` 绑定验证。
- README / config guide 更新，明确“不配置 paths 时 client 多网口不保证被利用”。

---

## 9. DR03 client 恢复卡死修复设计

### 9.1 问题场景

DR03 测试先让 server 只监听 path-a 的 `10.201.1.2:4433`，client 同时配置 path-a 和 path-b，各 4 条连接。a-only 阶段 path-a 正常 connected，path-b 持续 retry。随后 server 重启为双 listener：`10.201.1.2:4433,10.201.2.2:4433`。期望 client 自动恢复为 8 条 connected。

当前失败不是确定性网络错误，而是偶发卡死。重复测试中同一配置可能一轮成功、一轮失败。失败现场显示 admin listener 仍在，但 `/connections` 和 `/peers` 会 timeout；port forward 的 iperf probe 也 timeout；client stop 可能需要强制 kill。

有效线程栈显示存在同步等待环：

```text
ingress reactor thread
  -> ProcessDueDelayedTasks()
  -> QuicClientSession::StartSlot()
  -> MsQuic API

MsQuic callback thread
  -> QuicClientSession::ConnectionCallback()
  -> NotifyConnectionStateChanged()
  -> TqClientPeerRuntime::ApplyConnectionState()
  -> OpenListenersLocked()
  -> TqClientIngressReactor::AddPeer()
  -> EnqueueSync()
  -> wait ingress reactor
```

admin timeout 是连带结果：admin worker 线程分别阻塞在 `SnapshotConnections()`、`SnapshotPeerMetrics()` 的锁等待上。路由、证书和 10.201 `/30` 地址规划已经通过 F01/F04/F05/F08 和 route/ping 证据排除为主因。

### 9.2 修复目标

- MsQuic callback 线程不得同步等待 ingress reactor。
- connection state 变化触发的 listener open/close 必须异步、幂等、可合并。
- `StartSlot()` 不在持有 `ConfigLock` 时调用 `SetParam`、`SetLocalAddr`、`ConnectionStart` 等 MsQuic connection API。
- 保持启动路径的同步语义：`Start()` 返回前仍按当前连接状态完成一次 listener 状态应用。
- 保持对外配置和 admin API 不变。

### 9.3 callback listener 更新模型

`TqClientPeerRuntime` 保留现有同步控制路径：

```text
Start()
  -> EnableAcceptingAndApplyCurrentConnectionState()

StopAll()/StopAccepting()
  -> DisableAccepting()
```

这些路径仍允许同步调用 `Ingress->AddPeer()` / `Ingress->RemovePeer()`，因为它们不在 MsQuic callback 线程上执行。

新增 callback 专用路径：

```text
QuicClientSession::ConnectionStateChanged
  -> TqClientPeerRuntime::ScheduleConnectionStateApply(connectedCount)
  -> Ingress->EnqueueDelayed(0ms, task)
  -> return immediately
```

`ScheduleConnectionStateApply()` 在 `ListenerMutex` 下只更新目标状态：

```text
DesiredListenersOpen = AcceptingEnabled && connectedCount > 0
PendingListenerUpdate = true
```

如果已有 pending update，不重复投递。投递到 ingress reactor 的 task 执行时再回到 peer runtime：

```text
FlushPendingListenerUpdate()
  -> lock ListenerMutex
  -> PendingListenerUpdate = false
  -> if !AcceptingEnabled or !DesiredListenersOpen: CloseListenersLocked()
  -> else OpenListenersLocked()
```

该 task 运行在 ingress reactor 线程上。由于它已经在目标线程中执行，`Ingress->AddPeer()` / `RemovePeer()` 不应再通过 `EnqueueSync()` 反向等待自身。实现需要在 ingress reactor 增加一个“已在 reactor 线程时直接执行”的同步入口，或者增加 peer runtime 专用的 async add/remove API。推荐最小改法：

- `TqClientIngressReactor` 记录 `ReactorThreadId`。
- `EnqueueSync()` 如果当前线程就是 reactor 线程，则直接执行 task 并返回结果。
- callback 仍只通过 `EnqueueDelayed(0ms)` 进入 reactor；进入后调用现有 `AddPeer()` / `RemovePeer()` 时不会自锁。

这个方案改动小，能同时保护其他未来误用 `EnqueueSync()` 的 reactor-thread 内调用。

### 9.4 幂等和错误处理

listener 状态更新必须幂等：

- 已经 open 且目标仍为 open：直接成功。
- 已经 closed 且目标仍为 closed：直接成功。
- open 失败时不把 `ListenersOpen` 置为 true，保留 `DesiredListenersOpen=true`，下次 connection state 变化或显式 apply 可再次尝试。
- callback 异步 apply 失败只写 stderr，不阻塞 MsQuic callback。
- `DisableAccepting()` 必须清理 pending 目标状态，并同步关闭 listener，避免 stop 后异步 task 重新打开 listener。

### 9.5 StartSlot 持锁范围收窄

当前 `StartSlot()` 的一段逻辑在持有 `ConfigLock` 期间完成连接对象创建、`SetParam`、`SetLocalAddr`、`ConnectionStart`。这会放大 admin snapshot 和 callback 之间的锁等待。

修复后的结构：

```text
1. 短暂持 ConfigLock/state->Lock
   - 校验 session/slot
   - 复制 Config、Configuration 指针和 path
   - 读取 generation/gate
   - 清理旧 slot 指针

2. 锁外执行可能阻塞的 MsQuic API
   - ConnectionOpen
   - TqSetDisable1RttEncryption
   - SetLocalAddr

3. 短暂持 state->Lock 发布新 connection/context 到 slot
   - 校验 generation 未变化
   - slot.Connection = newConnection
   - slot.Context = newContext

4. 锁外调用 ConnectionStart

5. 短暂持 state->Lock 记录 start 结果
   - start 失败时清理 slot 并 ScheduleStartRetry
```

约束：

- `Configuration` 在 session 生命周期内由 `unique_ptr` 持有；如果 runtime tuning 会刷新 configuration，刷新必须仍在 `ConfigLock` 内完成，随后把 `Configuration.get()` 保存为本次 start 使用的原始指针。第一版不改变 configuration 生命周期模型。
- 如果在锁外 MsQuic API 执行期间 slot generation 变化，必须关闭/丢弃刚创建的连接，并不能覆盖新 slot 状态。
- `oldConnection->Shutdown()` 继续放在锁外；移入 orphan list 时短暂持 `state->Lock`。

### 9.6 测试门禁

单元测试：

- `client_ingress_reactor_test` 增加 reactor 线程内调用 `AddPeer()` 不死锁的覆盖。
- `client_peer_runtime_test` 覆盖 connection state callback 只投递异步 listener update，不同步等待 callback 返回。
- `quic_session_reconnect_test` 增加 `StartSlot()` generation 变化时不覆盖新 slot 状态的覆盖。

系统测试：

- DR03 使用 `docs/dgx-multi-interface-quic-binding-20260628-10net/run-dr03-10net.sh` 连续至少 10 轮。
- 每轮要求 a-only 为 4 条 connected，dual recover 后 10 或 20 秒内达到 8 条 connected，`IPERF_AFTER_RECOVER_RC=0`，`CLIENT_WAIT_RC=0`。
- 失败时保留 sudo gdb 栈；修复验收要求不再出现 `EnqueueSync()` 等 ingress reactor 的 callback 等待环。
