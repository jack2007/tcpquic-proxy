# tcpquic-proxy Admin API 重构设计

> 状态：设计草案  
> 日期：2026-06-23  
> 范围：重建 tcpquic-proxy Admin API，覆盖现有 client/server/multi-peer 管理能力，并补齐 peer、connection、tunnel/relay 的查询与控制模型

---

## 1. 背景

当前 Admin HTTP API 由 `TqAdminHttpServer` 基于 `cpp-httplib` 提供，公开路径统一位于 `/api/v1/*`，并要求 Bearer token。旧的 `/health`、`/metrics`、`/config` 和 `/peers/{peer_id}/enable` 形态已经从公开 API 移除。当前能力按角色划分：

- client：`/api/v1/health`、`/api/v1/metrics`、`/api/v1/config`、`/api/v1/peers/*`、`/api/v1/tunnels/*`、`/api/v1/relay/*`。single-peer client 也表现为 `peers` 中只有一个 `primary` peer。
- server：`/api/v1/health`、`/api/v1/metrics`、`/api/v1/server/*`。

当前 API 没有鉴权或 TLS，依赖 loopback 绑定限制访问面。下一阶段需要把 Admin API 从临时运维接口升级为稳定控制面：

1. 启动成功后生成本机授权 token 文件，本地服务读取 token 文件后才能访问 Admin API。
2. 覆盖项目已有功能配置，并补齐客户端 peer、connection、tunnel/relay 等资源的增删改查与状态查询。
3. HTTP server 采用 `cpp-httplib` 作为基础实现，替换当前自研 HTTP parser/server。

`cpp-httplib` 是 C++11 单头文件 HTTP/HTTPS server/client 库，使用阻塞 socket I/O，仅支持 HTTP/1.1。它适合简化当前 Admin HTTP 代码，但不能把控制面变成异步非阻塞模型。设计中需要显式配置固定大小 thread pool，避免把当前每连接 detached thread 模型换成另一个无边界并发模型。

---

## 2. 设计目标

### 2.1 功能目标

- 保留现有 Admin API 能力，并提供兼容迁移路径。
- 提供 token 鉴权，默认只允许持有本机 token 文件的进程访问。
- 提供清晰的 REST 资源模型：
  - process/system
  - config
  - peers
  - connections
  - tunnels
  - relay metrics
  - server metrics
  - ACL
- 支持 multi-peer client 的 peer 增删改查、启停、drain、abort tunnels。
- 支持 connection 状态查询和连接级控制。
- 支持 tunnel/relay 状态查询和 tunnel 级 drain/abort。
- 支持 server 侧健康、指标、连接、stream/tunnel 观测。

### 2.2 工程目标

- HTTP server 层只处理路由、鉴权、JSON 和状态码，不直接修改 runtime 细节。
- 管理操作集中在 `TqAdminService` / runtime adapter 中，便于单元测试。
- 配置 JSON 序列化/解析尽量复用现有 config/router config 结构，避免多套 schema 漂移。
- Admin 控制面不能阻塞数据面热路径。
- 所有变更操作必须有明确的生命周期语义：apply、drain、abort、rollback。
- 默认保持 loopback 绑定限制；token 是额外门禁，不替代网络暴露控制。

### 2.3 非目标

- 不在本阶段实现公网安全 Admin 面。Admin 仍默认只绑定 loopback。
- 不在本阶段引入 OAuth/JWT、多用户 RBAC 或远程身份系统。
- 不把 `cpp-httplib` 当成异步框架使用。
- 不支持通过 Admin API 主动创建 data-plane tunnel；tunnel 由实际入口连接自然产生，Admin 只查询和终止。

---

## 3. 总体架构

```text
Admin HTTP client
  -> httplib::Server
      -> TqAdminAuth
      -> TqAdminHttpRoutes
          -> TqAdminService
              -> TqRouterRuntime
              -> TqClientRuntimeManager
              -> QuicClientSession
              -> tunnel registry / relay runtime
              -> server metrics / server connection registry
```

### 3.1 模块划分

| 模块 | 职责 |
|------|------|
| `TqAdminHttpServer` | 封装 `httplib::Server` 生命周期、listen/stop、路由注册 |
| `TqAdminAuth` | token 生成、token 文件写入、请求鉴权、constant-time compare |
| `TqAdminService` | 管理面业务入口，提供纯 C++ 方法，便于测试 |
| `TqAdminJson` | JSON 响应、错误格式、资源序列化 |
| `TqAdminRouterAdapter` | client multi-peer peer/config 操作适配 |
| `TqConnectionRegistry` | QUIC connection snapshot 与控制操作 |
| `TqTunnelRegistry` | tunnel snapshot 与 tunnel abort/drain |
| `TqRelaySnapshot` | relay backend 聚合与 per-tunnel relay 状态 |

### 3.2 HTTP 与业务解耦

HTTP handler 只做薄适配：

```cpp
server.Get(R"(/api/v1/peers)", [&](const httplib::Request& req, httplib::Response& res) {
    if (!Auth.Authorize(req, res)) {
        return;
    }
    auto result = Service.ListPeers();
    WriteJson(res, result);
});
```

文档中的 `{peer_id}`、`{connection_id}`、`{tunnel_id}` 是逻辑路径变量。`cpp-httplib` 实现时应使用正则路由和 `req.matches` 提取变量，或在 `TqAdminHttpRoutes` 内封装统一 matcher。不要把 `{peer_id}` 字面量直接传给 `httplib::Server::Get/Post`。

`TqAdminService` 返回结构化结果：

```cpp
struct TqAdminResult {
    int Status{200};
    std::string BodyJson;
};
```

HTTP 层不直接持有 `QuicClientSession*`、relay worker 或 tunnel context。

---

## 4. Token 授权

### 4.1 Token 文件

tcpquic 启动 Admin 时先生成内存 token，再启动 `httplib::Server`，最后在获取实际 bound listen address 后原子写入 token 文件。这样即使 token 文件写入前已有连接进入，服务端也已经要求内存 token；本地调用方必须等 token 文件出现后才能访问。

推荐默认路径：

| 平台 | 默认路径 |
|------|----------|
| Linux/macOS | `$XDG_RUNTIME_DIR/<binary-name>/admin-${pid}.json` |
| Linux/macOS fallback | `/tmp/<binary-name>-$UID/admin-${pid}.json` |
| Windows | `%LOCALAPPDATA%\<binary-name>\admin-${pid}.json` |

允许通过参数覆盖：

```text
--admin-token-file <path>
```

默认路径必须包含 pid，避免同一用户启动多个 tcpquic 实例时互相覆盖 token 文件。生产环境或由 supervisor 启动时，推荐显式指定 `--admin-token-file`，让本地 sidecar/运维脚本通过约定路径读取。进程日志应打印 token 文件路径，但不能打印 token 内容。

`<binary-name>` 使用进程启动时 `argv[0]` 的 basename；例如直接运行 `raypx2` 时为 `raypx2`，如果用其它文件名或软链接名启动，则跟随实际启动名。

token 文件格式：

```json
{
  "version": 1,
  "token_type": "Bearer",
  "token": "hex-or-base64url-random-token",
  "listen": "127.0.0.1:19091",
  "pid": 12345,
  "created_at_unix": 1782200000
}
```

### 4.2 生成规则

- token 使用 OS CSPRNG 生成，至少 32 bytes 随机熵。
- 编码使用 hex 或 base64url，不包含空白字符。
- POSIX 目录权限 `0700`，文件权限 `0600`。
- 写入流程为 `open tmp -> write -> fsync -> chmod -> rename`。
- 进程退出时默认删除 token 文件，但删除前必须读取并确认文件中的 token 或 pid 匹配当前进程，避免误删后启动的新实例 token 文件。
- 异常退出留下的旧 token 文件在下次启动时覆盖；默认 pid 路径会自然隔离不同实例。
- token 文件写入失败时 Admin 启动失败并退出，不能退化为无鉴权或“只监听但无 token 文件”的状态。

### 4.3 请求鉴权

所有 `/api/v1/*` 请求必须包含：

```http
Authorization: Bearer <token>
```

错误行为：

| 条件 | HTTP |
|------|------|
| 缺少 token | `401 Unauthorized` |
| token 错误 | `401 Unauthorized` |
| token 类型不是 Bearer | `401 Unauthorized` |
| 已鉴权但资源不存在 | `404 Not Found` |
| 已鉴权但请求非法 | `400 Bad Request` |
| runtime 操作冲突 | `409 Conflict` |
| runtime 暂不可用 | `503 Service Unavailable` |

响应体统一：

```json
{
  "error": {
    "code": "unauthorized",
    "message": "unauthorized"
  }
}
```

token 比较必须使用 constant-time compare，避免按前缀长度泄露信息。

### 4.4 公开 API 路径

公开 Admin API 只支持 `/api/v1/*`。旧路径：

```text
/health
/metrics
/config
/peers/{peer_id}/enable
/peers/{peer_id}/disable
```

不再转发到 handler，也不提供旧路径免鉴权开关。新脚本必须读取 token 文件，并使用 `Authorization: Bearer <token>` 访问 `/api/v1/*`。

---

## 5. 资源模型

### 5.1 Process

```text
GET /api/v1/health
GET /api/v1/metrics
GET /api/v1/process
GET /api/v1/version
```

`GET /api/v1/health`：

```json
{
  "role": "client",
  "status": "healthy",
  "uptime_seconds": 123,
  "admin": {
    "listen": "127.0.0.1:19091",
    "auth": "token"
  }
}
```

`GET /api/v1/metrics` 是兼容性聚合入口，按进程角色返回当前最接近旧 `/metrics` 的结构：

- single-peer client：返回 primary peer 的 client metrics。
- multi-peer client：返回 router metrics，包含 peers 数组。
- server：返回 server metrics。

新代码应优先使用更具体的资源接口，例如 `/api/v1/peers`、`/api/v1/server/metrics`、`/api/v1/relay/metrics`。

### 5.2 Config

```text
GET /api/v1/config
PUT /api/v1/config
GET /api/v1/config/client
GET /api/v1/config/server
GET /api/v1/config/quic
GET /api/v1/config/relay
GET /api/v1/config/acl
```

`PUT /api/v1/config` 是整体替换。它必须先完整 parse 和 validate，成功后再 apply。失败时 runtime config 不变。

分段 config 接口在第一版应先做只读，等完整 config serializer/parser 抽取完成后再开放分段写操作。原因是当前 router Admin parser 只覆盖 `version` 和 `peers`，完整进程配置还包含 TLS、QUIC、ACL、relay tuning、trace 等字段；如果每段配置各自维护 parser，后续很容易与启动配置漂移。

### 5.3 Peers

client multi-peer 的主资源。单 peer client 可以把 primary peer 映射为只读或有限可改的 `primary` peer。

```text
GET    /api/v1/peers
POST   /api/v1/peers
GET    /api/v1/peers/{peer_id}
PUT    /api/v1/peers/{peer_id}
PATCH  /api/v1/peers/{peer_id}
DELETE /api/v1/peers/{peer_id}
POST   /api/v1/peers/{peer_id}:enable
POST   /api/v1/peers/{peer_id}:disable
POST   /api/v1/peers/{peer_id}:drain
POST   /api/v1/peers/{peer_id}:abort-tunnels
```

Peer schema：

```json
{
  "peer_id": "agent-b",
  "quic_peer": "127.0.0.1:14444",
  "socks_listen": "127.0.0.1:11001",
  "http_listen": "127.0.0.1:18001",
  "port_forwards": [
    {
      "listen": "127.0.0.1:15432",
      "target": "db.internal.example.com:5432"
    }
  ],
  "quic_connections": 4,
  "compress": "off",
  "enabled": true
}
```

Peer status schema：

```json
{
  "peer_id": "agent-b",
  "enabled": true,
  "state": "healthy",
  "quic_peer": "127.0.0.1:14444",
  "listeners": {
    "socks": "open",
    "http": "open",
    "port_forwards": [
      {
        "listen": "127.0.0.1:15432",
        "target": "db.internal.example.com:5432",
        "state": "open"
      }
    ]
  },
  "connection_count": 4,
  "connected_connections": 4,
  "active_tunnels": 12,
  "total_tunnels": 100,
  "reconnects": 2,
  "last_error": "",
  "last_connected_at": ""
}
```

`active_tunnels`、`total_tunnels` 依赖 Phase 4 tunnel registry snapshot。在 Phase 2 交付 peer CRUD 时可以先省略，或返回 `null`，不能用现有聚合 relay metrics 伪造 peer 级 tunnel 数。

### 5.4 Connections

Connection 是 peer 下的 QUIC connection slot。当前代码只暴露连接总数和已连接数；完整 connection 管理需要给 `QuicClientSession` 增加 slot snapshot 和受控 slot 操作。

```text
GET    /api/v1/peers/{peer_id}/connections
GET    /api/v1/peers/{peer_id}/connections/{connection_id}
POST   /api/v1/peers/{peer_id}/connections
DELETE /api/v1/peers/{peer_id}/connections/{connection_id}
POST   /api/v1/peers/{peer_id}/connections/{connection_id}:reconnect
POST   /api/v1/peers/{peer_id}/connections/{connection_id}:abort-tunnels
```

Connection API 要区分两类对象：

- desired connection slots：由 peer config 的 `quic_connections` 管理，是控制面可增减的目标连接数。
- live QUIC connection：MsQuic 运行时对象，会因为 reconnect、shutdown complete 等事件变化。

第一版 connection 写操作只允许改变 desired slot 数，不能任意删除中间 slot。任意删除中间 live connection 会破坏当前 `Config.QuicConnections` 作为连续 slot 数量的假设，也会让 round-robin pick、reconnect 和 config roundtrip 语义变复杂。

Connection schema：

```json
{
  "connection_id": "agent-b-0",
  "peer_id": "agent-b",
  "slot_index": 0,
  "state": "connected",
  "quic_peer": "127.0.0.1:14444",
  "connected": true,
  "retry_scheduled": false,
  "active_tunnels": 3,
  "total_tunnels": 20,
  "last_connected_at": "",
  "last_disconnected_at": "",
  "last_error": ""
}
```

`POST /connections` 语义：

- 增加一个 connection slot。
- 启动新 QUIC connection。
- 成功后更新 peer config 中 `quic_connections`。

`DELETE /connections/{connection_id}` 语义：

- 只允许删除最高序号 slot，或改用 `PATCH /api/v1/peers/{peer_id}` 调低 `quic_connections`。
- 对非最高序号 slot 返回 `409 Conflict`，提示先使用 `:reconnect` 或 `:abort-tunnels`。
- 删除最高序号 slot 时停止该 slot 接受新 tunnel，并按 body 指定 drain/abort 现有 tunnels：

```json
{
  "mode": "drain",
  "grace_seconds": 10
}
```

### 5.5 Tunnels

Tunnel 是一次 TCP-over-QUIC 业务连接，对应 SOCKS5/HTTP CONNECT/port forward 入口到远端 TCP target 的数据通道。Admin 不主动创建 tunnel，只查询和终止。

```text
GET    /api/v1/tunnels
GET    /api/v1/tunnels/{tunnel_id}
DELETE /api/v1/tunnels/{tunnel_id}
POST   /api/v1/tunnels/{tunnel_id}:drain
POST   /api/v1/tunnels/{tunnel_id}:abort
```

过滤参数：

```text
GET /api/v1/tunnels?peer_id=agent-b
GET /api/v1/tunnels?connection_id=agent-b-0
GET /api/v1/tunnels?state=active
GET /api/v1/tunnels?limit=100
```

Tunnel schema：

```json
{
  "tunnel_id": 42,
  "peer_id": "agent-b",
  "connection_id": "agent-b-0",
  "role": "client",
  "ingress": "http_connect",
  "target": "example.com:443",
  "state": "active",
  "compress": "off",
  "created_at_unix": 1782200000,
  "duration_ms": 1200,
  "tcp": {
    "local_address": "127.0.0.1:51234",
    "peer_address": "127.0.0.1:18001",
    "read_closed": false,
    "write_closed": false
  },
  "relay": {
    "backend": "linux_worker",
    "worker_index": 2,
    "pending_quic_receive_bytes": 0,
    "pending_tcp_write_bytes": 0,
    "outstanding_quic_sends": 1,
    "outstanding_quic_send_bytes": 65536,
    "tcp_read_armed": true,
    "tcp_write_armed": false,
    "quic_receive_paused": false
  },
  "counters": {
    "tcp_read_bytes": 1048576,
    "tcp_write_bytes": 1048576,
    "quic_send_bytes": 1048576,
    "quic_receive_bytes": 1048576
  },
  "last_error": ""
}
```

### 5.6 Relay Metrics

Relay metrics 保留现有聚合指标，并逐步补 per-tunnel snapshot。

```text
GET /api/v1/relay/metrics
GET /api/v1/relay/workers
GET /api/v1/relay/workers/{worker_id}
```

`GET /api/v1/relay/metrics` 返回当前 `TqSnapshotRelayMetrics()` 的结构化 JSON。字段命名可以继续兼容现有 `linux_relay_*`，同时新增平台中性字段：

```json
{
  "backend": "worker",
  "active_relays": 12,
  "pending_events": 0,
  "pending_bytes": 0,
  "tcp_read_bytes": 123,
  "tcp_write_bytes": 456,
  "errors": 0,
  "platform": {
    "linux_relay_wakeups": 10
  }
}
```

### 5.7 Server

Server 模式下没有 peer CRUD，但需要暴露 server 侧观测资源：

```text
GET /api/v1/server
GET /api/v1/server/metrics
GET /api/v1/server/connections
GET /api/v1/server/connections/{connection_id}
GET /api/v1/server/tunnels
POST /api/v1/server/connections/{connection_id}:abort-tunnels
```

现有 server metrics 已包含：

- listen
- accepted_connections
- active_streams
- total_streams
- acl_denied
- tcp_dialing
- relay metrics
- last_error

---

## 6. Runtime 能力差距

### 6.1 已可直接复用

| 能力 | 当前支持 |
|------|----------|
| client health/metrics | `TqClientMetricsJson` |
| server health/metrics | `TqServerMetricsJson` |
| router config GET/PUT | `TqRouterRuntime::ConfigJson` / `ApplyConfig` |
| peer enable/disable | `TqRouterRuntime::HandleAdmin` 中通过 config apply 完成 |
| peer start/stop/drain | `TqClientRuntimeManager` |
| abort peer tunnels | `TqClientRuntimeManager::AbortPeerTunnels` |
| relay aggregate metrics | `TqSnapshotRelayMetrics()` |

### 6.2 需要新增

| 能力 | 新增点 |
|------|--------|
| peer CRUD 原子 API | `TqRouterRuntime::{Create,Update,Patch,Delete}Peer` |
| connection list | `QuicClientSession::SnapshotConnections()` |
| connection slot 管理 | `QuicClientSession::{SetDesiredConnectionCount,ReconnectConnection,StopHighestConnection}` |
| connection stable id | slot id + generation 或 UUID |
| tunnel list | 扩展 `tunnel_registry` 记录 metadata 和 counters |
| tunnel abort/drain by id | `TqAbortTunnel(tunnel_id)` / `TqDrainTunnel(tunnel_id)` |
| per-relay snapshot | Linux/Darwin/Windows relay worker 暴露 relay state snapshot |
| server connection list | server connection registry 保存 conn_id/state/stream count |
| JSON config 完整读写 | 抽取 config serializer/parser，避免 Admin 自写局部 parser |

---

## 7. 状态模型

### 7.1 Peer State

```text
disabled
starting
connecting
healthy
degraded
down
draining
deleted
```

### 7.2 Connection State

```text
starting
connected
reconnecting
draining
closing
closed
failed
```

### 7.3 Tunnel State

```text
opening
active
draining
closing
closed
failed
aborted
```

---

## 8. HTTP 约定

### 8.1 响应格式

资源查询默认直接返回资源对象，不额外包一层 `data`。这样可以最大限度复用当前 `/health`、`/metrics`、`/config` JSON，并降低迁移成本。

```json
{
  "role": "client",
  "status": "healthy"
}
```

列表接口返回 `data + meta`，便于后续分页和 cursor：

```json
{
  "data": [],
  "meta": {
    "count": 0,
    "limit": 100,
    "next_cursor": ""
  }
}
```

错误响应统一使用 `error` envelope：

```json
{
  "error": {
    "code": "invalid_request",
    "message": "peer_id is required",
    "details": {}
  }
}
```

### 8.2 状态码

| HTTP | 含义 |
|------|------|
| 200 | 查询或同步操作成功 |
| 201 | 创建成功 |
| 202 | 异步 drain/reconnect 已接受 |
| 204 | 删除成功且无响应体 |
| 400 | 请求 JSON 或参数非法 |
| 401 | 未授权 |
| 404 | 资源不存在 |
| 409 | 状态冲突，例如重复 peer_id、删除 active resource 未指定 drain/abort |
| 413 | body 过大 |
| 500 | 内部错误 |
| 503 | runtime 暂不可用 |

### 8.3 幂等性

- `PUT /peers/{id}` 幂等。
- `DELETE /peers/{id}` 对不存在资源返回 `404`。
- `POST /peers/{id}:enable` 对已启用 peer 返回 `200` 和当前状态。
- `POST /peers/{id}:disable` 对已禁用 peer 返回 `200` 和当前状态。
- `POST :abort` 可以幂等；若资源已关闭，返回 `200` 或 `404` 需在实现中统一。推荐保留短时间 tombstone，返回 `200`。

---

## 9. cpp-httplib 集成

### 9.1 依赖方式

推荐 vendored 单头文件：

```text
third_party/cpp-httplib/httplib.h
```

CMake 增加 interface target：

```cmake
add_library(cpp_httplib INTERFACE)
target_include_directories(cpp_httplib INTERFACE
    ${CMAKE_SOURCE_DIR}/third_party/cpp-httplib)
```

Admin server target link `cpp_httplib`。

### 9.2 Threading

`cpp-httplib` 默认是阻塞 I/O server。需要显式设置 thread pool，避免无限 per-client detached thread：

```cpp
server.new_task_queue = [] {
    return new httplib::ThreadPool(4);
};
```

线程数建议：

```text
--admin-threads <n>
默认：2
范围：1..32
```

Admin API 是控制面，不应按 CPU 核数自动开很大线程池。

### 9.3 Server 参数

建议：

- read timeout：5s
- write timeout：5s
- keep-alive：关闭或限制很低
- max body：默认 1MiB，配置更新可放宽到 4MiB
- access log：默认关闭，避免 token 或敏感配置泄露

---

## 10. 配置变更语义

### 10.1 Peer Create

1. validate peer JSON。
2. 检查 `peer_id` 唯一。
3. 加入 router config。
4. 若 `enabled=true`，启动 peer runtime。
5. runtime 启动失败时：
   - 默认保留配置但 peer state 为 `down`，返回 `202` 或 `200` with degraded state。
   - 若请求指定 `strict=true`，失败则回滚并返回 `503`。

### 10.2 Peer Update

如果 data-plane 字段变更：

- `quic_peer`
- `socks_listen`
- `http_listen`
- `port_forwards`
- `quic_connections`
- `compress`

则执行：

1. stop accepting。
2. abort 或 drain old tunnels。
3. drain old runtime。
4. start new runtime。

仅 `enabled` 变化时走 enable/disable。

### 10.3 Peer Delete

默认不允许直接删除有 active tunnels 的 peer：

```json
{
  "mode": "drain",
  "grace_seconds": 10
}
```

支持：

- `mode=reject-if-active`
- `mode=drain`
- `mode=abort`

### 10.4 Connection Delete

删除 connection slot 需要调整 peer `quic_connections`。第一版只允许删除最高序号 slot；中间 slot 只能 reconnect 或 abort tunnels。若后续需要任意 slot 删除，必须先把 config 模型从单个 `quic_connections` 数量升级为显式 connection slot 列表。

---

## 11. 安全注意事项

- token 文件不能写入 world-readable 目录。
- token 不写日志，不出现在 URL query。
- 错误响应不回显 token。
- 默认保持 loopback 限制。
- 非 loopback 绑定必须显式开关，例如 `--admin-allow-non-loopback`，并强制打印高危警告。
- 后续如需远程 Admin，应优先支持 HTTPS + mTLS，而不是只依赖 bearer token。

---

## 12. 兼容与迁移

Phase 1 曾规划保留旧 API；当前实现已完成迁移，公开 API 只保留 v1 形态：

| 已移除旧接口 | v1 替代接口 |
|--------|--------|
| `GET /health` | `GET /api/v1/health` |
| `GET /metrics` | `GET /api/v1/metrics` |
| `GET /config` | `GET /api/v1/config` |
| `PUT /config` | `PUT /api/v1/config` |
| `POST /peers/{id}/enable` | `POST /api/v1/peers/{id}:enable` |
| `POST /peers/{id}/disable` | `POST /api/v1/peers/{id}:disable` |

旧路径直接返回 `404`；未带 token 访问 v1 路径返回 `401`。

---

## 13. 验收标准

- 未带 token 访问 `/api/v1/*` 返回 `401`。
- 正确 token 可访问所有已授权接口。
- token 文件权限符合平台要求。
- 旧 `/health`、`/metrics` 返回 `404`。
- peer CRUD 能正确触发 runtime start/stop/drain。
- connection list 能显示每个 QUIC slot 的状态。
- relay metrics 与 `/api/v1/metrics` 中 relay 字段一致。
- tunnel list 能显示 active tunnel，按 id abort 后 tunnel 关闭。
- Admin HTTP 并发不再无限创建 detached thread，而受 `--admin-threads` 限制。
