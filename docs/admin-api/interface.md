# Admin API 接口清单

本文档按当前代码实现整理，入口来自 `src/runtime/admin_http.cpp`、`src/runtime/router_runtime.cpp`、`src/runtime/server_admin.cpp`、`src/runtime/admin_config.cpp`、`src/runtime/relay_metrics.cpp` 和 `src/runtime/admin_memory.cpp`。

## 通用约定

- 公开 API 路径前缀固定为 `/api/v1`。Admin HTTP server 会先校验此前缀，再剥离 `/api/v1` 后转发给内部 handler。
- Admin console 静态页面不带 `/api/v1`：`GET /` 和 `GET /console` 重定向到 `/console/`，`GET /console/`、`/console/style.css`、`/console/app.js` 返回静态资源；这些静态资源不走 Bearer Token 鉴权。
- API 请求默认开启 Bearer Token 鉴权。Token 文件由 `--admin-token-file` 或配置 `admin.token_file` 指定；显式 token 文件存在且 JSON 合法时会复用，缺失时会创建，不合法时启动失败。省略时使用按进程/角色生成的默认路径，并且每次启动重新生成 token JSON。
- `GET /api/v1/admin` 也需要 Bearer Token；未认证时不会返回 admin 状态。
- 请求和响应均为 JSON。路径参数使用单个 path segment，若包含特殊字符需要 percent-encode；实现会拒绝空 segment、非法 `%xx`、以及包含 `/` 的 segment。
- 当前 HTTP server 只放行白名单中的 `/api/v1/*` 路径。代码中仍存在少量 legacy 内部路径解析，例如 `/peers/{id}/enable`，但经公开 Admin HTTP 入口会被白名单拦截，应使用本文档中的 colon action 形式。
- Admin HTTP server 参数校验：`admin.listen` 必须是 `host:port`，端口可为 `0` 以请求系统分配；`admin.threads` 范围为 `1..32`；请求体上限默认 `65536` 字节，响应 `GET /api/v1/admin` 中的 `http.max_body_bytes` 会展示实际值。
- 当前实现只在状态响应中报告 `security.loopback_only`，启动时不强制拒绝非 loopback 监听地址；生产使用仍应绑定 loopback。
- HTTP keep-alive 被关闭，响应均设置 `Connection: close`。
- 常见错误：
  - 未认证：`401 {"error":{"code":"unauthorized","message":"unauthorized"}}`
  - HTTP server 层找不到路径：`404 {"error":{"code":"not_found","message":"not found"}}`
  - handler 层找不到资源：`404 {"error":"not found"}`，部分 relay 明细路径使用结构化 `not_found`。
  - 请求体非法：`400 {"error":"<reason>"}`
  - HTTP framing 或 httplib 层坏请求：`400 {"error":{"code":"bad_request","message":"bad request"}}`
  - 冲突：`409 {"error":"<reason>"}`
  - 请求体过大：`413 {"error":{"code":"payload_too_large","message":"payload too large"}}`
  - 当前后端或运行态不支持：`503 {"error":{"code":"not_supported","message":"..."}}`；部分文件写回失败返回简单错误 `503 {"error":"<reason>"}`。

## Admin HTTP Server 接口

| Method | Path | 状态码 | 说明 |
| --- | --- | --- | --- |
| `GET` | `/api/v1/admin` | `200`/`401` | 查询 Admin HTTP server 自身状态，包括角色、监听地址、鉴权状态、token 文件路径、线程数、请求体限制和安全约束；不返回 token 内容。 |

`GET /api/v1/admin` 返回示例：

```json
{
  "role": "client",
  "listen": "127.0.0.1:18080",
  "auth": {
    "enabled": true,
    "type": "bearer",
    "token_file": "...",
    "token_present": true
  },
  "http": {
    "threads": 2,
    "max_body_bytes": 65536,
    "keep_alive": false
  },
  "security": {
    "loopback_only": true,
    "tls": false
  }
}
```

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
| `PATCH` | `/api/v1/diagnostics` | `200`/`400`/`503` | 更新 trace、diag-stats 等诊断配置状态；应用运行态失败时返回 `503 not_supported`。 |
| `GET` | `/api/v1/config` | `200` | 返回当前 router 配置。 |
| `PUT` | `/api/v1/config` | `200`/`400` | 替换当前 router 配置，并按配置写回 client 配置文件。 |
| `GET` | `/api/v1/peers` | `200` | 返回所有 peer 运行时指标。 |
| `POST` | `/api/v1/peers` | `201`/`200`/`400`/`409` | 创建 peer。重复 `peer_id` 返回 `409`；通常返回新建 peer 指标，极端情况下返回当前配置。 |
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
| `POST` | `/api/v1/peers/{peer_id}/connections` | `201`/`400`/`404` | 增加一个目标 connection，响应返回该 peer 的 connection 列表。 |
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
| `GET` | `/api/v1/relay/workers/aggregate` | `200`/`404`/`503` | 返回聚合 worker 指标；当前后端不支持 worker 明细时返回 `not_supported`。 |
| `GET` | `/api/v1/relay/workers/{worker_id}` | `200`/`404`/`503` | 查询 worker 指标。不存在的 worker 返回 `not_found`；当前后端不支持 worker 明细时返回 `not_supported`。 |
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
- `tuning.max_memory_mb` 必须是非负整数且不超过 `UINT32_MAX`。
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

`GET /api/v1/health`：

```json
{
  "role": "client",
  "status": "healthy",
  "uptime_seconds": 10
}
```

`GET /api/v1/metrics`：

```json
{
  "role": "client",
  "status": "healthy",
  "uptime_seconds": 10,
  "peer_count": 1,
  "peers": []
}
```

`GET /api/v1/config` 返回 router 配置对象，peer 字段使用写入 schema：`peer_id`、`quic_peer`、`socks_listen`、`http_listen`、`port_forwards`、`paths`、`compress`、`enabled`，`quic_connections` 仅在非 0 时输出。

`GET /api/v1/runtime/config` 返回完整运行时快照，字段包括 `role`、`client_name`、`config_path`、`client_config_path`、`admin`、`socks_listen`、`http_listen`、`quic`、`tls`、`port_forwards`、`router`、`compress`、`compress_level`、`tuning_mode`、`max_memory_mb`。`tls.key` 在 `redact=false` 的当前调用中不会脱敏。

`GET /api/v1/client/config` 返回推荐 client schema，字段包括：`role`、`client_name`、`socks_listen`、`http_listen`、`proto`、`proxy_auth`、`speed_test`、`handshake_threads`、`compression`、`peers`。其中 peer 使用推荐字段 `id`、`proto_peer`、`proto_connections`。

`GET /api/v1/diagnostics` 返回：

```json
{
  "role": "client",
  "tuning_mode": "wan",
  "max_memory_mb": 0,
  "trace": false,
  "trace_interval_sec": 30,
  "trace_level": "info",
  "diag_stats": false,
  "diag_stats_interval_sec": 5
}
```

`PeerConfig`（推荐 schema，用于 `client/config` 或 `peers/{peer_id}/config`）：

```json
{
  "id": "agent-a",
  "proto_peer": "127.0.0.1:14444",
  "socks_listen": "127.0.0.1:11001",
  "http_listen": "",
  "port_forwards": [{"listen": "127.0.0.1:15432", "target_host": "db.internal", "target_port": 5432}],
  "paths": [{"name": "default", "local": "0.0.0.0", "peer": "127.0.0.1:14444", "connections": 1}],
  "proto_connections": 1,
  "compress": "off",
  "enabled": true
}
```

`PeerMetrics` 字段：`peer_id`、`enabled`、`quic_peer`、`client_name`、`socks_listen`、`http_listen`、`port_forwards`、`paths`、`state`、`connection_count`、`connected_connections`、`active_streams`、`total_streams`、`reconnects`、`last_error`、`last_connected_at`。

`Connection` 字段：`connection_id`、`slot_index`、`generation`、`connected`、`retry_scheduled`、`state`、`path`、`local`、`peer`、`active_tunnels`、`last_error`。

`Tunnel` 字段：`tunnel_id`、`peer_id`、`connection_id`、`state`、`target`、`role`、`ingress`、`compress`、`created_at`、`duration_ms`、`tcp_read_bytes`、`tcp_write_bytes`、`pending_bytes`、`relay_backend`、`worker_index`、`last_error`。

成功动作类响应通常为：`{"status":"draining"}`、`{"status":"aborting"}` 或 `{"status":"reconnecting"}`。

## Server 模式接口

Server 模式由 `RunServer()` 的 admin handler 加 `TqHandleServerAdmin()` 提供以下接口。

| Method | Path | 状态码 | 说明 |
| --- | --- | --- | --- |
| `GET` | `/api/v1/health` | `200` | 返回 server 健康/指标 JSON；由 server 启动代码映射到 `TqServerMetricsJson()`。 |
| `GET` | `/api/v1/metrics` | `200` | 返回 server 指标 JSON；由 server 启动代码映射到 `TqServerMetricsJson()`。 |
| `GET` | `/api/v1/diagnostics` | `200` | 查询 trace、diag-stats 等诊断配置状态。 |
| `PATCH` | `/api/v1/diagnostics` | `200`/`400`/`503` | 更新 trace、diag-stats 等诊断配置状态；应用运行态失败时返回 `503 not_supported`。 |
| `GET` | `/api/v1/runtime/config` | `200` | 查询 server 运行时配置快照。 |
| `PATCH` | `/api/v1/runtime/config` | `200`/`400`/`503` | 解析并返回热更新后的运行期安全字段快照；启动级字段返回 `not_supported`。当前 server 分支不会提交到 `runtimeConfigState`。 |
| `GET` | `/api/v1/server` | `200` | 返回 server 指标 JSON。 |
| `GET` | `/api/v1/server/metrics` | `200` | 返回 server 指标 JSON。 |
| `GET` | `/api/v1/server/config` | `200` | 查询 server 配置快照，包括 listen、resolved listens、ACL、quic、tls、tuning 等字段。 |
| `PATCH` | `/api/v1/server/config` | `200`/`400`/`503` | 热更新 server ACL，并写回 server JSON 配置文件。首版只支持 `allow_targets` 和 `deny_targets`。 |
| `GET` | `/api/v1/server/connections` | `200` | 列出 server 侧 QUIC connection。 |
| `GET` | `/api/v1/server/connections/{connection_id}` | `200`/`404` | 查询 server 侧单个 connection。 |
| `POST` | `/api/v1/server/connections/{connection_id}:abort-tunnels` | `202`/`404` | 中止该 server connection 下的 tunnels。 |
| `GET` | `/api/v1/server/tunnels` | `200` | 列出 server role 的 tunnels。 |
| `GET` | `/api/v1/server/tunnels/{tunnel_id}` | `200`/`404` | 查询 server tunnel。 |
| `DELETE` | `/api/v1/server/tunnels/{tunnel_id}` | `202`/`404` | 中止 server tunnel，等价于 `:abort`。 |
| `POST` | `/api/v1/server/tunnels/{tunnel_id}:abort` | `202`/`404` | 中止 server tunnel。 |
| `POST` | `/api/v1/server/tunnels/{tunnel_id}:drain` | `202`/`404` | drain server tunnel。 |
| `GET` | `/api/v1/relay/metrics` | `200` | 返回 relay 聚合指标和平台扩展诊断字段。 |
| `GET` | `/api/v1/relay/active-relays` | `200` | 查询 active relay capability 和当前可见明细列表。 |
| `GET` | `/api/v1/relay/active-relays/{relay_id}` | `200`/`404`/`503` | 查询单个 active relay；当前平台不支持逐 relay 明细时返回 `not_supported`。 |
| `GET` | `/api/v1/relay/workers` | `200` | 返回 worker capability 和 worker 列表。 |
| `GET` | `/api/v1/relay/workers/aggregate` | `200`/`404`/`503` | 返回聚合 worker 指标。 |
| `GET` | `/api/v1/relay/workers/{worker_id}` | `200`/`404`/`503` | 查询 worker 指标。 |
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

`ServerConnection` 字段：`connection_id`、`client_name`、`remote_address`、`state`、`active_streams`、`total_streams`、`active_tunnels`、`last_error`、`encryption`。

`ServerTunnel` 字段：`tunnel_id`、`peer_id`、`connection_id`、`state`、`target`、`role`、`duration_ms`、`active`。

`GET /api/v1/server/config` 返回 server 运行时配置快照，字段包括 `role`、`config_path`、`listen`、`resolved_listens`、`allow_targets`、`deny_targets`、`quic`、`tls`、`tuning_mode`。当前调用使用 `redact=false`，因此 `tls.key` 不会被脱敏。

`GET /api/v1/runtime/config` 在 server 模式下返回同样的 server 运行时配置快照；`PATCH /api/v1/runtime/config` 返回 `TqRuntimeConfigJson()` 风格的完整运行时快照。

`PATCH /api/v1/server/config`：

```json
{
  "allow_targets": ["127.0.0.1/32", "10.0.0.0/8"],
  "deny_targets": ["169.254.0.0/16"]
}
```

- `allow_targets` 和 `deny_targets` 支持 JSON array，也兼容逗号分隔字符串；未出现字段保持当前运行态值。
- 成功后会用 `nlohmann::json` 更新 `--config` 指向的 server JSON 文件中的 `server.allow_targets` / `server.deny_targets`，再更新运行态 ACL，随后提交 runtime config state。
- 进程没有可写回的 `config_path`，或当前 handler 没有配置文件 store 时，返回 `503 not_supported`。
- 非法 CIDR、非法 JSON、未知字段返回 `400`，旧运行态 ACL 保持不变。
- `listen`、`resolved_listens`、`tls`、`quic`、`proto`、`relay`、`admin` 等启动级字段返回 `503 not_supported`。
- PATCH 空 `allow_targets: []` 表示拒绝所有普通新建 tunnel；启动配置省略 `allow_targets` 时仍按启动默认值补 `0.0.0.0/0`。

成功动作类响应通常为：`{"status":"draining"}` 或 `{"status":"aborting"}`。

## Relay 和内存诊断响应

`GET /api/v1/relay/metrics` 的基础字段：`backend`、`active_relays`、`pending_bytes`、`tcp_read_bytes`、`tcp_write_bytes`、`errors`。响应还会合并 `TqRelayMetricsFieldsJson()` 输出字段，其中跨平台聚合字段包括 `relay_*` 前缀字段和 `relay_backend`。`relay_snapshot_complete` 为 `false` 时，数值是有界采样得到的 partial/fallback，不应作为完整健康快照解释。

常见跨平台 relay 字段包括：

- `relay_compressed_tcp_bytes`、`relay_decompressed_tcp_bytes`：压缩/解压后的 TCP 字节计数。
- `relay_zstd_decompress_input_bytes`、`relay_zstd_decompress_output_bytes`、`relay_zstd_decompress_calls`、`relay_zstd_decompress_need_input`、`relay_zstd_decompress_need_output`、`relay_zstd_decompress_failures`：zstd 解压统计。
- `relay_quic_receive_view_count`、`relay_quic_receive_view_bytes`、`relay_max_quic_receive_view_bytes`、`relay_max_quic_receive_view_slices` 及按大小/分片数分桶字段：QUIC receive view 观测统计。
- `relay_quic_receive_paused_count`、`relay_quic_receive_resumed_count`：QUIC receive pause/resume 计数。
- `relay_errors`、`relay_event_queue_full_errors`、`relay_event_queue_capacity`、`relay_snapshot_complete`、`relay_last_quic_send_status`、`relay_backend`。

Linux relay 额外包含大量 `linux_relay_*` 字段，常用字段包括：

- `linux_relay_wakeups`、`linux_relay_events_processed`、`linux_relay_pending_events`：Linux worker 事件循环计数。
- `linux_relay_pending_bytes`、`linux_relay_buffer_bytes_in_use`：pending 字节和 relay buffer 占用。
- `linux_relay_active_relays`、`linux_relay_active_tcp_relays`、`linux_relay_active_sink_relays`、`linux_relay_active_quic_send_relays`：active relay 分类计数。
- `linux_relay_current_pending_quic_receive_bytes`、`linux_relay_current_pending_quic_receive_queue`：当前 QUIC receive backlog。
- `linux_relay_tcp_read_armed_relays`、`linux_relay_tcp_read_disabled_relays`、`linux_relay_tcp_write_armed_relays`、`linux_relay_closing_relays`、`linux_relay_tcp_read_closed_relays`、`linux_relay_tcp_write_shutdown_queued_relays`：relay 状态计数。
- `linux_relay_outstanding_quic_sends`、`linux_relay_outstanding_quic_send_bytes`、`linux_relay_max_buffered_quic_send_bytes`：QUIC send backlog。
- `linux_relay_pending_tcp_write_queue`、`linux_relay_pending_tcp_write_bytes`、`linux_relay_tcp_write_eagain_count`、`linux_relay_tcp_write_partial_count`、`linux_relay_tcp_write_burst_stops`：TCP write backlog 和异常计数。
- `linux_relay_max_worker_pending_bytes`、`linux_relay_max_worker_active_relays`、`linux_relay_max_relay_pending_quic_receive_bytes`、`linux_relay_max_relay_pending_quic_receive_queue`、`linux_relay_max_relay_tcp_write_eagain_count`：峰值观测。
- `linux_relay_hot_relay_id`、`linux_relay_hot_relay_worker_index`、`linux_relay_hot_relay_tcp_fd`、`linux_relay_hot_relay_pending_quic_receive_bytes`、`linux_relay_hot_relay_pending_quic_receive_queue`、`linux_relay_hot_relay_tcp_write_bytes`、`linux_relay_hot_relay_tcp_read_bytes`、`linux_relay_hot_relay_outstanding_quic_sends`、`linux_relay_hot_relay_outstanding_quic_send_bytes`、`linux_relay_hot_relay_pending_quic_send_retries`、`linux_relay_hot_relay_ideal_send_bytes`、`linux_relay_hot_relay_tcp_write_eagain_count`、`linux_relay_hot_relay_epollout_events`、`linux_relay_hot_relay_tcp_read_armed`、`linux_relay_hot_relay_tcp_write_armed`、`linux_relay_hot_relay_local`、`linux_relay_hot_relay_peer`：热点 relay 观测。
- `linux_relay_tcp_read_batches`、`linux_relay_tcp_read_bytes`、`linux_relay_quic_send_operations`、`linux_relay_tcp_write_batches`、`linux_relay_tcp_write_bytes`、`linux_relay_max_tcp_read_iov_used`、`linux_relay_max_tcp_write_iov_used`、`linux_relay_tcp_write_sendmsg_calls`、`linux_relay_tcp_write_attempt_bytes`、`linux_relay_max_tcp_write_attempt_bytes`、`linux_relay_max_tcp_write_sendmsg_bytes` 及 send/return 字节分桶字段：I/O 批处理和 sendmsg 统计。
- `linux_relay_read_disabled_count`、`linux_relay_fake_fin_receive_count`、`linux_relay_stream_lookup_scan_count`：异常或 fallback 路径计数。
- `linux_relay_backend`、`linux_relay_compressed_tcp_bytes`、`linux_relay_decompressed_tcp_bytes` 以及 `linux_relay_zstd_*`、`linux_relay_deferred_receive_*`、`linux_relay_quic_receive_*`、`linux_relay_errors`：Linux 后端压缩、receive 和错误统计。
- `linux_relay_event_queue_capacity`、`linux_relay_event_queue_push_cas_retries`、`linux_relay_event_queue_pop_cas_retries`、`linux_relay_event_producer_threads_observed`、`linux_relay_multiple_event_producer_threads_observed`：event queue 和 producer 观测。
- `linux_relay_control_lock_wait_nanos`、`linux_relay_control_lock_acquire_count`、`linux_relay_control_command_wait_nanos`、`linux_relay_control_command_wait_count`、`linux_relay_control_command_timeouts`、`linux_relay_control_command_enqueue_failures`：控制路径等待和失败计数。
- `linux_relay_snapshot_command_wait_nanos`、`linux_relay_snapshot_command_wait_count`、`linux_relay_snapshot_command_timeouts`、`linux_relay_runtime_lock_wait_nanos`、`linux_relay_runtime_lock_acquire_count`、`linux_relay_runtime_snapshot_inflight_max`：snapshot/runtime lock 诊断。

Windows 和 Darwin/macOS 后端也会通过 `relay_metrics.cpp` 输出平台相关字段；前端和调用方应优先按 capability 与字段存在性处理，不应假设所有平台字段都存在。

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
- active relay 明细基础字段包括：`relay_id`、`relay_id_numeric`、`worker_index`、`backend`、`active_handlers`、`queued_worker_ops`、`in_flight_tcp_recvs`、`in_flight_tcp_sends`、`in_flight_quic_sends`、`pending_quic_receive_bytes`、`pending_quic_receive_queue_depth`、`callback_pending_quic_receive_depth`、`outstanding_quic_send_bytes`、`max_outstanding_quic_send_bytes`、`event_queue_depth`、`tcp_read_bytes`、`tcp_write_bytes`、`last_tcp_write_errno`、`last_tcp_recv_errno`、`last_tcp_send_errno`、`last_iocp_completion_errno`、`last_iocp_operation`、`closing`、`tcp_read_closed`、`tcp_read_paused_by_quic_backlog`、`tcp_write_closed`、`close_after_drained`、`quic_send_fin_submitted`、`quic_send_fin_completed`、`stop_published`、`stream_detached`。
- Linux 明细额外包含 `tcp_fd`、`tcp_read_armed`、`tcp_write_armed`、`pending_quic_send_retries`、`ideal_send_bytes`、`tcp_write_eagain_count`、`epoll_out_events`、`pending_tcp_write_queue_depth`、`pending_tcp_write_bytes`、`relay_buffer_bytes_in_use`、`local_address`、`peer_address`。

`GET /api/v1/relay/workers` 返回：

```json
{
  "capabilities": {
    "active_relay_detail": false,
    "worker_detail": true,
    "per_worker_active_relays": false
  },
  "snapshot_complete": true,
  "workers": [
    {
      "worker_id": "aggregate",
      "backend": "worker",
      "worker_index": 0,
      "active_relays": 0,
      "pending_bytes": 0,
      "tcp_read_bytes": 0,
      "tcp_write_bytes": 0,
      "errors": 0,
      "event_queue_capacity": 4096,
      "snapshot_complete": true
    }
  ]
}
```

`GET /api/v1/relay/workers` 使用一次 Runtime worker snapshot：首行固定为 `aggregate`，其余行按平台 worker slot 顺序排列；`aggregate` 由同批 worker 行归并，而不是第二次采样。运行中每一行和顶层均含 `snapshot_complete`。Stopped runtime 返回 complete 的 aggregate 空集；runtime lock 或 worker snapshot 超时导致 identity/数值不完整时，list 仍返回 200，但顶层 `snapshot_complete:false`，调用方不得把其中数值当作完整快照。

`GET /api/v1/relay/workers/{worker_id}` 返回 worker 指标，并包含 `relays` 数组。Linux worker 对象包含 `event_queue_capacity`，用于和 queue depth/pending events 对照。当前 `per_worker_active_relays:false`，因此 `relays` 为空数组。目标行和整体快照完整时返回 200；identity 已完整但 worker 不存在时返回 `404 not_found`；无法取得 identity 或目标 worker snapshot 不完整时返回 `503 snapshot_unavailable`。

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
