# Admin API 接口清单

本文档按当前代码实现整理，入口来自 `src/runtime/admin_http.cpp`、`src/runtime/router_runtime.cpp` 和 `src/runtime/server_admin.cpp`。

## 通用约定

- 公开路径前缀固定为 `/api/v1`。Admin HTTP server 会先校验该前缀，再剥离 `/api/v1` 转发给内部 handler。
- 只允许绑定 loopback：`127.0.0.1:<port>`、`localhost:<port>`、`::1:<port>`。启动参数为 `--admin-listen` 或配置 `admin.listen`。
- 默认开启 Bearer Token 鉴权。Token 文件由 `--admin-token-file` 或配置 `admin.token_file` 指定；显式 token 文件存在且 JSON 合法时会复用，缺失时会创建，不合法时启动失败。省略时使用按进程/角色生成的默认路径，并且每次启动重新生成 token JSON。
- 请求和响应均为 JSON。路径参数使用单个 path segment，若包含特殊字符需要 percent-encode；实现会拒绝空 segment、非法 `%xx`、以及包含 `/` 的 segment。
- 当前 HTTP server 白名单只接受 `/api/v1/*`。代码中仍存在少量 legacy 内部路径解析，例如 `/peers/{id}/enable`，但经公开 Admin HTTP 入口会被白名单拦截，应使用本文档中的 colon action 形式。
- 常见错误：
  - 未认证：`401 {"error":{"code":"unauthorized","message":"unauthorized"}}`
  - HTTP server 层找不到路径：`404 {"error":{"code":"not_found","message":"not found"}}`
  - handler 层找不到资源：`404 {"error":"not found"}`
  - 请求体非法：`400 {"error":"<reason>"}`
  - 冲突：`409 {"error":"<reason>"}`
  - 请求体过大：`413 {"error":{"code":"payload_too_large","message":"payload too large"}}`
  - 当前后端不支持：`503 {"error":{"code":"not_supported","message":"..."}}`

## Admin HTTP Server 接口

| Method | Path | 状态码 | 说明 |
| --- | --- | --- | --- |
| `GET` | `/api/v1/admin` | `200` | 查询 Admin HTTP server 自身状态，包括角色、监听地址、鉴权状态、token 文件路径、线程数、请求体限制和安全约束；不返回 token 内容。 |

## Client/Router 模式接口

Client 单 peer 和 multi-peer 都通过 `TqRouterRuntime::HandleAdmin()` 提供以下接口。

| Method | Path | 状态码 | 说明 |
| --- | --- | --- | --- |
| `GET` | `/api/v1/health` | `200` | 返回 client 角色、健康状态和 uptime。 |
| `GET` | `/api/v1/metrics` | `200` | 返回 client/router 总体状态和 peer 指标列表。 |
| `GET` | `/api/v1/runtime/config` | `200` | 查询完整运行时配置快照，敏感内容脱敏或省略。 |
| `PATCH` | `/api/v1/runtime/config` | `200`/`400`/`503` | 热更新运行期安全字段；启动级字段返回 `not_supported`。 |
| `GET` | `/api/v1/client/config` | `200` | 查询 client 推荐 schema 配置快照。 |
| `GET` | `/api/v1/diagnostics` | `200` | 查询 trace、diag-stats 等诊断配置状态。 |
| `PATCH` | `/api/v1/diagnostics` | `200`/`400` | 更新 trace、diag-stats 等诊断配置状态。 |
| `GET` | `/api/v1/config` | `200` | 返回当前 router 配置。 |
| `PUT` | `/api/v1/config` | `200`/`400` | 替换当前 router 配置。 |
| `GET` | `/api/v1/peers` | `200` | 返回所有 peer 运行时指标。 |
| `POST` | `/api/v1/peers` | `201`/`400`/`409` | 创建 peer。重复 `peer_id` 返回 `409`。 |
| `GET` | `/api/v1/peers/{peer_id}` | `200`/`404` | 查询单个 peer 指标。 |
| `GET` | `/api/v1/peers/{peer_id}/config` | `200`/`404` | 查询单 peer 推荐 schema 配置。 |
| `PUT` | `/api/v1/peers/{peer_id}` | `200`/`400`/`404` | 替换 peer 配置。请求体内 `peer_id` 应与路径一致。 |
| `PATCH` | `/api/v1/peers/{peer_id}` | `200`/`400`/`404` | 部分更新 peer 配置。 |
| `DELETE` | `/api/v1/peers/{peer_id}` | `200`/`400`/`404`/`409` | 删除 peer。活跃 peer 默认拒绝，需要 body 指定 `mode`。 |
| `POST` | `/api/v1/peers/{peer_id}:enable` | `200`/`400`/`404` | 启用 peer。 |
| `POST` | `/api/v1/peers/{peer_id}:disable` | `200`/`400`/`404` | 禁用 peer。 |
| `POST` | `/api/v1/peers/{peer_id}:drain` | `202`/`400`/`404` | 停止接收新流量并等待 drain。 |
| `POST` | `/api/v1/peers/{peer_id}:abort-tunnels` | `202`/`400`/`404` | 中止该 peer 下的 active tunnels。 |
| `GET` | `/api/v1/peers/{peer_id}/connections` | `200`/`404` | 列出该 peer 的 QUIC connection。 |
| `POST` | `/api/v1/peers/{peer_id}/connections` | `201`/`400`/`404` | 增加一个目标 connection。 |
| `GET` | `/api/v1/peers/{peer_id}/connections/{connection_id}` | `200`/`404` | 查询单个 connection。 |
| `DELETE` | `/api/v1/peers/{peer_id}/connections/{connection_id}` | `200`/`400`/`404`/`409` | 删除 connection。只能删除最高 slot，且不能删最后一个 connection。 |
| `POST` | `/api/v1/peers/{peer_id}/connections/{connection_id}:reconnect` | `202`/`400`/`404` | 重连指定 connection。 |
| `POST` | `/api/v1/peers/{peer_id}/connections/{connection_id}:abort-tunnels` | `202`/`400`/`404` | 中止指定 connection 下的 tunnels。 |
| `GET` | `/api/v1/tunnels` | `200` | 列出所有 tunnel。 |
| `GET` | `/api/v1/tunnels/{tunnel_id}` | `200`/`404` | 查询单个 tunnel。 |
| `DELETE` | `/api/v1/tunnels/{tunnel_id}` | `202`/`404` | 中止 tunnel，等价于 `:abort`。 |
| `POST` | `/api/v1/tunnels/{tunnel_id}:abort` | `202`/`404` | 中止 tunnel。 |
| `POST` | `/api/v1/tunnels/{tunnel_id}:drain` | `202`/`404` | drain tunnel。 |
| `GET` | `/api/v1/relay/metrics` | `200` | 返回 relay 聚合指标和平台扩展诊断字段。 |
| `GET` | `/api/v1/relay/active-relays` | `200` | 查询 active relay capability 和当前可见明细列表。 |
| `GET` | `/api/v1/relay/active-relays/{relay_id}` | `200`/`404`/`503` | 查询单个 active relay；当前平台不支持逐 relay 明细时返回 `not_supported`。 |
| `GET` | `/api/v1/relay/workers` | `200` | 返回 worker capability 和 worker 列表；包含跨平台 `aggregate`，Linux runtime 启动后还会包含 `linux-N`。 |
| `GET` | `/api/v1/relay/workers/aggregate` | `200` | 返回聚合 worker 指标。 |
| `GET` | `/api/v1/relay/workers/{worker_id}` | `200`/`404` | 查询 worker 指标。不存在的 worker 返回 `not_found`。 |
| `POST` | `/api/v1/memory/allocator:dump` | `200` | 将 allocator 统计写到日志，并返回统计 JSON。 |

### Client 请求体

`PUT /api/v1/config`：

```json
{
  "version": 1,
  "peers": [
    {
      "peer_id": "agent-a",
      "quic_peer": "127.0.0.1:14444",
      "socks_listen": "127.0.0.1:11001",
      "http_listen": "127.0.0.1:18001",
      "port_forwards": [
        {"listen": "127.0.0.1:15432", "target": "db.internal:5432"}
      ],
      "paths": [
        {"name": "default", "local": "0.0.0.0", "peer": "127.0.0.1:14444", "connections": 1}
      ],
      "quic_connections": 1,
      "compress": "off",
      "enabled": true
    }
  ]
}
```

`POST /api/v1/peers` 和 `PUT /api/v1/peers/{peer_id}` 使用单个 peer 对象，不包 `version/peers`。写接口兼容旧字段和推荐字段别名：

- `id` 等价于 `peer_id`。
- `proto_peer` 等价于 `quic_peer`。
- `proto_connections` 等价于 `quic_connections`。
- 同一请求不能同时出现同一含义的新旧字段，否则返回 `400 {"error":"conflicting peer field aliases"}`。

`PATCH /api/v1/peers/{peer_id}` 支持字段：`peer_id`/`id`、`quic_peer`/`proto_peer`、`socks_listen`、`http_listen`、`port_forwards`、`paths`、`quic_connections`/`proto_connections`、`compress`、`enabled`。未出现字段保持不变。

`PATCH /api/v1/diagnostics`：

```json
{
  "trace": true,
  "trace_interval_sec": 30,
  "trace_level": "debug",
  "diag_stats": true,
  "diag_stats_interval_sec": 5
}
```

- `trace_interval_sec` 和 `diag_stats_interval_sec` 范围为 `1..86400`。
- `trace_level` 只允许 `info`、`debug`。
- 未出现字段保持不变；未知字段返回 `400`。

`PATCH /api/v1/runtime/config`：

```json
{
  "compression": {
    "mode": "zstd",
    "level": 1
  },
  "tuning": {
    "max_memory_mb": 1024
  }
}
```

- `compression.mode` 只允许 `auto`、`zstd`、`off`。
- `compression.level` 范围为 `1..22`。
- `admin`、`tls`、`proto`、`relay`、`listen` 等启动级字段返回 `503 not_supported`。
- 未知字段返回 `400`。

`DELETE /api/v1/peers/{peer_id}` 和 `POST /api/v1/peers/{peer_id}:drain` 可使用：

```json
{
  "mode": "reject-if-active",
  "grace_seconds": 10
}
```

- `mode` 默认 `reject-if-active`；删除 active peer 时需要 `drain` 或 `abort`。
- `grace_seconds` 默认 `10`，仅 drain 类动作使用。

### Client 响应模型

`PeerConfig`：

```json
{
  "id": "agent-a",
  "proto_peer": "127.0.0.1:14444",
  "socks_listen": "127.0.0.1:11001",
  "http_listen": "",
  "port_forwards": [{"listen": "127.0.0.1:15432", "target": "db.internal:5432"}],
  "paths": [{"name": "default", "local": "0.0.0.0", "peer": "127.0.0.1:14444", "connections": 1}],
  "proto_connections": 1,
  "compress": "off",
  "enabled": true
}
```

`PeerMetrics` 在 `PeerConfig` 基础上增加：`state`、`connection_count`、`connected_connections`、`active_streams`、`total_streams`、`reconnects`、`last_error`、`last_connected_at`。

`Connection` 字段：`connection_id`、`slot_index`、`generation`、`connected`、`retry_scheduled`、`state`、`path`、`local`、`peer`、`active_tunnels`、`last_error`。

`Tunnel` 字段：`tunnel_id`、`peer_id`、`connection_id`、`state`、`target`、`role`、`ingress`、`compress`、`created_at`、`duration_ms`、`tcp_read_bytes`、`tcp_write_bytes`、`pending_bytes`、`relay_backend`、`worker_index`、`last_error`。

## Server 模式接口

Server 模式由 `RunServer()` 的 admin handler 加 `TqHandleServerAdmin()` 提供以下接口。

| Method | Path | 状态码 | 说明 |
| --- | --- | --- | --- |
| `GET` | `/api/v1/health` | `200` | 返回 server 健康/指标 JSON。 |
| `GET` | `/api/v1/metrics` | `200` | 返回 server 指标 JSON。 |
| `GET` | `/api/v1/diagnostics` | `200` | 查询 trace、diag-stats 等诊断配置状态。 |
| `PATCH` | `/api/v1/diagnostics` | `200`/`400` | 更新 trace、diag-stats 等诊断配置状态。 |
| `GET` | `/api/v1/runtime/config` | `200` | 查询 server 运行时配置快照。 |
| `PATCH` | `/api/v1/runtime/config` | `200`/`400`/`503` | 热更新运行期安全字段；启动级字段返回 `not_supported`。 |
| `GET` | `/api/v1/server` | `200` | 返回 server 指标 JSON。 |
| `GET` | `/api/v1/server/metrics` | `200` | 返回 server 指标 JSON。 |
| `GET` | `/api/v1/server/config` | `200` | 查询 server 配置快照，包括 listen、resolved listens、ACL、proto、tuning、relay、compression 等字段。 |
| `GET` | `/api/v1/server/connections` | `200` | 列出 server 侧 QUIC connection。 |
| `GET` | `/api/v1/server/connections/{connection_id}` | `200`/`404` | 查询 server 侧单个 connection。 |
| `POST` | `/api/v1/server/connections/{connection_id}:abort-tunnels` | `202`/`404` | 中止该 server connection 下的 tunnels。 |
| `GET` | `/api/v1/server/tunnels` | `200` | 列出 server role 的 tunnels。 |
| `GET` | `/api/v1/server/tunnels/{tunnel_id}` | `200`/`404` | 查询 server tunnel。 |
| `DELETE` | `/api/v1/server/tunnels/{tunnel_id}` | `202`/`404` | 中止 server tunnel，等价于 `:abort`。 |
| `POST` | `/api/v1/server/tunnels/{tunnel_id}:abort` | `202`/`404` | 中止 server tunnel。 |
| `POST` | `/api/v1/server/tunnels/{tunnel_id}:drain` | `202`/`404` | drain server tunnel。 |
| `POST` | `/api/v1/memory/allocator:dump` | `200` | 将 allocator 统计写到日志，并返回统计 JSON。 |

### Server 响应模型

`GET /api/v1/server`、`/server/metrics`、`/metrics`、`/health` 返回：

```json
{
  "role": "server",
  "status": "healthy",
  "listen": "0.0.0.0:4433",
  "resolved_listens": ["0.0.0.0:4433"],
  "uptime_seconds": 10,
  "accepted_connections": 1,
  "active_streams": 0,
  "total_streams": 1,
  "acl_denied": 0,
  "tcp_dialing": 0,
  "last_error": ""
}
```

实际响应还会包含 `TqAppendRelayMetricsJson()` 输出的 relay 诊断字段。

`ServerConnection` 字段：`connection_id`、`remote_address`、`state`、`active_streams`、`total_streams`、`active_tunnels`、`last_error`。

`ServerTunnel` 字段：`tunnel_id`、`peer_id`、`connection_id`、`state`、`target`、`role`、`duration_ms`、`active`。

`GET /api/v1/server/config` 返回 server 运行时配置快照，字段使用推荐配置 schema 命名，例如 `listen`、`resolved_listens`、`allow_targets`、`deny_targets`、`proto`、`tls`、`tuning`、`relay`、`compression`、`admin`。证书密钥等敏感字段会脱敏或不返回。

## Relay 和内存诊断响应

`GET /api/v1/relay/metrics` 的基础字段：`backend`、`active_relays`、`pending_bytes`、`tcp_read_bytes`、`tcp_write_bytes`、`errors`。Linux/Windows/macOS 平台会追加不同的 relay 诊断计数器。

`GET /api/v1/relay/active-relays` 在不支持逐 relay 明细的平台返回类似：

```json
{
  "capabilities": {
    "active_relay_detail": false,
    "worker_detail": true,
    "per_worker_active_relays": false
  },
  "relays": []
}
```

- Windows 和 Linux 后端可通过 active relay snapshot 返回逐 relay 明细。
- macOS 或不支持逐 relay 明细的平台，`GET /api/v1/relay/active-relays/{relay_id}` 返回 `503 not_supported`。
- Linux 明细额外包含 `tcp_fd`、`tcp_read_armed`、`tcp_write_armed`、`pending_quic_send_retries`、`ideal_send_bytes`、`tcp_write_eagain_count`、`epoll_out_events`、`pending_tcp_write_queue_depth`、`pending_tcp_write_bytes`、`relay_buffer_bytes_in_use`、`local_address`、`peer_address`。

`GET /api/v1/relay/workers` 返回：

```json
{
  "capabilities": {
    "active_relay_detail": false,
    "worker_detail": true,
    "per_worker_active_relays": false
  },
  "workers": [
    {
      "worker_id": "aggregate",
      "backend": "worker",
      "worker_index": 0,
      "active_relays": 0,
      "pending_bytes": 0,
      "tcp_read_bytes": 0,
      "tcp_write_bytes": 0,
      "errors": 0
    }
  ]
}
```

`GET /api/v1/relay/workers/{worker_id}` 返回 worker 指标，并包含 `relays` 数组。当前 `per_worker_active_relays:false`，因此 `relays` 为空数组。

`POST /api/v1/memory/allocator:dump` 返回：

```json
{
  "status": "dumped",
  "allocator": "mimalloc",
  "enabled": true,
  "available": true,
  "requested_current_bytes": 0,
  "requested_total_bytes": 0,
  "requested_freed_bytes": 0,
  "requested_peak_bytes": 0,
  "reserved_current_bytes": 0,
  "committed_current_bytes": 0,
  "page_committed_current_bytes": 0,
  "normal_alloc_count": 0,
  "huge_alloc_count": 0,
  "mmap_calls": 0,
  "commit_calls": 0,
  "purge_calls": 0,
  "threads_current": 0
}
```

未启用 mimalloc 时，`enabled`/`available` 为 `false`，计数字段通常为 `0`。
