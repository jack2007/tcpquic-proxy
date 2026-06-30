# Client Admin Stream 指标设计

> 状态：设计方案
> 日期：2026-06-30
> 范围：client admin console overview/peers 中 `active_streams`、`total_streams` 修复；client tunnel `peer_id` / `connection_id` 元数据补齐；避免 TCP/QUIC 数据转发热路径新增锁

---

## 1. 背景

当前 client 已经运行并有浏览器代理流量。运行态接口显示 `/api/v1/tunnels` 有 active tunnel，`/api/v1/peers/{peer_id}/connections` 的 `active_tunnels` 也大于 0，但 `/api/v1/peers` 和 admin console overview/peers 展示的 `active_streams`、`total_streams` 始终为 0。

调查结果：

- admin console client overview 的 Active streams 来自 `/api/v1/peers` 的 `active_streams` 求和。
- `TqRouterRuntime::SnapshotMetrics()` 会复制 adapter 返回的 live `ActiveStreams` / `TotalStreams`。
- `TqClientPeerRuntime::SnapshotPeerMetrics()` 没有填充 `ActiveStreams` / `TotalStreams`，因此保留 `TqPeerMetrics` 默认值 0。
- server 侧已有 `TqServerConnectionStreamStarted()` / `TqServerConnectionStreamFinished()` 维护连接级 stream 计数；client 侧没有对应统计。
- client tunnel registry 中的 metadata 对 client role 没有填 `peer_id` 和 `connection_id`，导致 `/api/v1/tunnels` 无法和 peer/connection 页面关联。

本设计修复 client admin 指标，不改变转发数据模型：一个接入 TCP 连接仍对应一条 QUIC bidirectional stream。

---

## 2. 目标

### 2.1 功能目标

- client `/api/v1/peers` 返回正确的 peer 级 `active_streams`。
- client `/api/v1/peers` 返回 peer runtime 生命周期内单调递增的 `total_streams`。
- client admin overview 的 Active streams 与当前 active tunnels 一致。
- client admin peers 页面显示每个 peer 的 active/total stream。
- client `/api/v1/tunnels` 返回 client tunnel 的 `peer_id`。
- client `/api/v1/tunnels` 返回 client tunnel 所属 `conn-N` connection id。
- 修复不改变 server admin stream 计数行为。

### 2.2 性能目标

- 不在 `tcp->quic` / `quic->tcp` 主转发路径新增 mutex。
- 不在 relay worker 的 TCP read/write、QUIC receive、QUIC send completion 热循环中维护带锁计数。
- 生命周期边界允许做少量 atomic 或已有 registry metadata 更新。
- admin snapshot 查询可以使用已有 registry mutex，因为它只发生在控制面请求期间。

### 2.3 非目标

- 不实现 per-byte、per-packet 或 per-batch stream 指标。
- 不把 client connection 页面扩展为完整 stream 详情页。
- 不改变 `TqTunnelRegistry` 的 active tunnel 判定语义。
- 不修改 tunnel 调度策略，不改变 `PickConnection()` 的负载分配行为。
- 不把 `total_streams` 持久化到磁盘；进程重启后从 0 开始。

---

## 3. 设计原则

### 3.1 热路径边界

数据转发热路径包括：

- relay worker 从 TCP fd 读取数据并调用 QUIC send。
- QUIC stream receive callback 把数据推入 TCP write。
- TCP write/sendmsg 批处理。
- QUIC send complete 和 receive complete 的批量处理。

这些路径不新增 mutex，也不调用 tunnel registry snapshot/count 接口。修复只发生在：

- tunnel 创建成功后的生命周期边界。
- tunnel relay 启动后的 metadata 更新。
- admin snapshot 查询。

### 3.2 指标语义

client 上一个 TCP tunnel 对应一条 client-initiated QUIC bidirectional stream，因此：

```text
peer.active_streams = 当前 peer 所有连接 active tunnel 数之和
peer.total_streams  = 当前 peer runtime 内成功创建的 client tunnel stream 数
```

`active_streams` 使用已有 `TqCountConnectionTunnels(connection)` 派生。这个函数持有 tunnel registry mutex，但只在 admin snapshot 查询时调用，不在数据转发路径调用。

`total_streams` 使用 `std::atomic<uint64_t>` 维护。每次 `TqClientPeerRuntime::StartTunnel()` 成功拿到 `TqClientTunnelOpenHandle*` 后递增一次。若没有可用 QUIC connection 或 `TqStartClientTunnelAsync()` 返回 `nullptr`，不递增。

### 3.3 元数据语义

client tunnel metadata 在 relay 启动后更新。补齐规则：

- `peer_id` 由 `TqClientPeerRuntime` 传入 tunnel context。
- `connection_id` 使用 client connection slot id，例如 `conn-0`。
- `role` 仍为 `client`。
- `target`、`ingress`、`compress`、`relay_backend` 保持现有语义。

为了不在 metadata 更新时反查全局状态，`peer_id` 和 `connection_id` 在 tunnel 创建时作为静态上下文写入 tunnel context。后续 `UpdateRegistryMetadata()` 只复制已有字符串。

---

## 4. 组件设计

### 4.1 `QuicClientSession`

新增一个轻量选择结果类型：

```cpp
struct TqClientPickedConnection {
    MsQuicConnection* Connection{nullptr};
    std::string ConnectionId;
};
```

新增方法：

```cpp
TqClientPickedConnection PickConnectionWithId();
```

该方法与现有 `PickConnection()` 使用同一把 `State->Lock`，在一次锁内完成连接选择和 `conn-N` id 复制，避免调用方先 pick 再扫描。它只在 tunnel 创建路径调用，不在数据转发热路径调用。

现有 `PickConnection()` 保留，内部可以复用 `PickConnectionWithId().Connection`，避免破坏其它调用点。

`SnapshotConnections()` 不新增 stream 字段。它继续返回 `active_tunnels`，admin peer 指标通过这些 connection snapshot 聚合 active streams。

### 4.2 `TqClientPeerRuntime`

新增字段：

```cpp
std::atomic<uint64_t> TotalStreams{0};
```

`StartTunnel()` 改为：

1. 在 `TunnelStartMutex` 保护下调用 `Quic->PickConnectionWithId()`。
2. 若没有连接，保持现有错误日志并返回 `nullptr`。
3. 调用带 metadata 的 `TqStartClientTunnelAsync()`。
4. 若返回 handle 非空，`TotalStreams.fetch_add(1, std::memory_order_relaxed)`。

`SnapshotPeerMetrics()` 改为：

1. 填充现有 peer/config/connection/state 字段。
2. 调用 `Quic->SnapshotConnections()`。
3. 将所有 connection 的 `ActiveTunnels` 求和写入 `metrics.ActiveStreams`。
4. 读取 `TotalStreams.load(std::memory_order_relaxed)` 写入 `metrics.TotalStreams`。

这里的 `SnapshotConnections()` 会触发 registry count，但只在 admin 查询路径执行。

### 4.3 `tcp_tunnel` / `client_tunnel_open`

新增 client tunnel metadata 输入：

```cpp
struct TqClientTunnelMetadata {
    std::string PeerId;
    std::string ConnectionId;
};
```

在 `client_tunnel_open.h` 中新增带 metadata 的 overload，同时保留现有 5 参数签名，满足现有签名稳定性测试：

```cpp
TqClientTunnelOpenHandle* TqStartClientTunnelAsync(
    MsQuicConnection* conn,
    const TunnelRequest& req,
    TqSocketHandle clientTcpFd,
    const TqConfig& cfg,
    TqClientTunnelOpenComplete onComplete,
    TqClientTunnelMetadata metadata);
```

现有调用通过原 5 参数 overload 保持兼容：

```cpp
TqStartClientTunnelAsync(conn, req, fd, cfg, std::move(onComplete));
```

原 overload 的实现只负责包装：

```cpp
return TqStartClientTunnelAsync(conn, req, clientTcpFd, cfg, std::move(onComplete), {});
```

`TqTunnelContext` 增加 `PeerId` 和 `ConnectionId` 字段，client role 的 `UpdateRegistryMetadata()` 设置：

```cpp
metadata.PeerId = PeerId;
metadata.ConnectionId = ConnectionId;
```

server role 保持通过 `TqLookupServerConnectionId()` 填 `srv-N`。

### 4.4 `TqTunnelRegistry`

registry 结构已有 `TqTunnelRegistryMetadata::PeerId` 和 `ConnectionId` 字段，不需要新增字段。只需要补充测试，验证 client metadata 能进入 snapshot。

---

## 5. 数据流

### 5.1 Tunnel 创建

```text
client ingress accepts TCP
  -> TqClientPeerRuntime::StartTunnel()
     -> QuicClientSession::PickConnectionWithId()
        -> {connection, "conn-N"}
     -> TqStartClientTunnelAsync(connection, req, fd, cfg, onComplete, {peer_id, conn_id})
        -> create MsQuicStream
        -> register tunnel with connection
        -> save metadata on TqTunnelContext
     -> TotalStreams++
```

这里的锁只有现有 peer runtime mutex 和 QuicClientSession state mutex，属于连接选择路径，不属于数据转发热路径。

### 5.2 Relay 启动后 metadata 更新

```text
OPEN response ok
  -> relay starts
  -> UpdateRegistryMetadata()
     -> copy target/role/ingress/compress/backend
     -> copy PeerId/ConnectionId for client role
```

metadata 更新沿用已有 registry update 行为，发生在 relay 生命周期边界。

### 5.3 Admin snapshot

```text
GET /api/v1/peers
  -> TqRouterRuntime::SnapshotMetrics()
     -> Adapter::SnapshotPeerMetrics(peer_id)
        -> TqClientPeerRuntime::SnapshotPeerMetrics()
           -> Quic->SnapshotConnections()
              -> TqCountConnectionTunnels(connection)
           -> ActiveStreams = sum(ActiveTunnels)
           -> TotalStreams = TotalStreams.load()
```

admin 查询可以短时间持有 registry mutex。这个锁不被 relay 数据循环调用，因此不会给主转发路径增加每包/每批开销。

---

## 6. 错误处理

- `PickConnectionWithId()` 找不到 connected connection 时返回 `{nullptr, ""}`，调用方保持现有无连接日志和失败路径。
- `TqStartClientTunnelAsync()` 返回 `nullptr` 时不递增 `TotalStreams`。
- metadata 中 `peer_id` 或 `connection_id` 为空时，registry 仍允许 snapshot 输出空字符串；这保留兼容性，避免影响测试构造的匿名 tunnel。
- `TotalStreams` 是控制面统计指标，使用 relaxed atomic 即可；不用于同步生命周期状态。

---

## 7. 测试设计

### 7.1 单元测试

- `tcpquic_client_peer_runtime_test`
  - 覆盖 `SnapshotPeerMetrics()` 将 connection `active_tunnels` 聚合为 `active_streams`。
  - 覆盖 successful async tunnel start 后 `total_streams` 递增。
  - 覆盖 async tunnel start 失败不递增 `total_streams`。

- `tcpquic_quic_session_reconnect_test`
  - 覆盖 `PickConnectionWithId()` 返回 `conn-N` 与 selected slot 一致。
  - 覆盖无 connected slot 时返回空 connection。

- `tcpquic_tunnel_registry_test` 或 `tcpquic_tcp_tunnel_test`
  - 覆盖 client tunnel metadata 中 `peer_id` 和 `connection_id` 会进入 `TqSnapshotTunnels()`。

- `tcpquic_router_runtime_test`
  - 覆盖 `/api/v1/peers` 输出 live `active_streams` / `total_streams`。
  - 覆盖 `/api/v1/tunnels` 输出 client tunnel `peer_id` / `connection_id`。

### 7.2 手动验证

在运行中的 client 上：

```bash
TOKEN=$(sed -n 's/.*"token":"\([^"]*\)".*/\1/p' /run/user/1000/raypx2/client-admin-2841985.json)
curl -fsS -H "Authorization: Bearer $TOKEN" http://127.0.0.1:3080/api/v1/peers
curl -fsS -H "Authorization: Bearer $TOKEN" http://127.0.0.1:3080/api/v1/tunnels
curl -fsS -H "Authorization: Bearer $TOKEN" http://127.0.0.1:3080/api/v1/peers/primary/connections
```

预期：

- `/api/v1/peers` 中 `active_streams` 等于该 peer 下连接 `active_tunnels` 之和。
- `/api/v1/peers` 中 `total_streams` 大于等于历史成功创建 tunnel 数。
- `/api/v1/tunnels` 每条 client tunnel 包含 `peer_id:"primary"` 和 `connection_id:"conn-0"`。

---

## 8. 风险与缓解

- 风险：把 `total_streams` 计数放在过早的位置会统计未创建 stream 的失败请求。
  缓解：只在 `TqStartClientTunnelAsync()` 返回非空 handle 后递增。

- 风险：active streams 与 active tunnels 语义绑定后，未来如果出现非 tunnel client stream，会被漏计。
  缓解：当前 client admin 的 stream 目标就是代理 tunnel stream；未来新增非 tunnel control stream 时再引入 stream 类型维度。

- 风险：为了填 `connection_id` 反向扫描 slot 增加锁和复杂度。
  缓解：新增 `PickConnectionWithId()` 在选择连接时一次性返回 id，不做后续反查。

- 风险：admin snapshot 与 tunnel unregister 竞争导致瞬时 active 计数变化。
  缓解：这是控制面快照的自然语义；registry mutex 保证单次 count 内部一致。

---

## 9. 验收标准

- 运行测试目标：
  - `tcpquic_quic_session_reconnect_test`
  - `tcpquic_client_peer_runtime_test`
  - `tcpquic_tunnel_registry_test`
  - `tcpquic_router_runtime_test`
  - `tcpquic_admin_http_test`
- client 有浏览器代理流量时，admin console overview Active streams 不再为 0。
- client peers 页面 `active_streams` 与 `/api/v1/peers/{peer_id}/connections` 中 `active_tunnels` 聚合一致。
- client peers 页面 `total_streams` 在新建 tunnel 后单调增加。
- `/api/v1/tunnels` 的 client tunnel 能展示 peer 和 connection 归属。
- 代码审查确认 relay worker 数据转发循环没有新增 mutex 计数逻辑。
