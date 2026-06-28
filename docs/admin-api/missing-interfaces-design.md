# Admin API 缺口补齐设计

> 状态：设计草案  
> 日期：2026-06-28  
> 依据：[interface.md](./interface.md) 的“当前缺少的 Admin API 覆盖”章节  
> 范围：补齐配置查询、Admin 自身状态、server 配置/隧道控制、relay 明细、诊断状态接口；不在本阶段扩大 Admin 网络暴露面

## 1. 背景

当前 Admin API 已经覆盖 client/router 的 peer、connection、tunnel 控制，以及 server 的基础指标、connection 列表和 server tunnel 列表。但 `interface.md` 明确列出 7 类缺口：完整运行时配置不可查询、server 模式缺少配置查询、Admin server 自身不可查询、client peer schema 与推荐配置 schema 不一致、relay worker 只有 aggregate、server tunnel 缺少单 tunnel 控制、trace/diag-stats 无运行时接口。

这些缺口本质分为两类：

- 只读观测缺口：启动配置、Admin 状态、server 配置、relay/diagnostics 状态。
- 行为变更缺口：诊断开关、server tunnel abort/drain、peer reconnect interval 等运行时修改。

本设计优先补齐只读观测接口，降低风险；对会改变数据面行为的接口，只在已有 runtime 能安全承接时开放。

## 2. 目标和非目标

### 2.1 目标

- 提供完整、稳定、可脱敏的运行时配置查询接口。
- 为 server 模式补齐配置查询，至少覆盖 listen、ACL、proto、compression、tuning、relay 和 trace 配置。
- 提供 Admin 自身状态接口，便于运维脚本发现 listen、鉴权状态、线程数和 payload limit。
- 统一新增接口的公开 schema 使用配置文档中的命名：`proto_*`、`id`、`proto_peer`、`proto_connections`。现有 `/api/v1/config` 维持兼容。
- 提供 relay active relay 查询和 worker 明细查询；平台不支持精细 worker 时显式返回 capability。
- 为 server tunnel 补齐单项查询、abort 和 drain。
- 为 trace/diag-stats 提供状态查询；动态开关作为第二阶段，只支持能在线安全切换的字段。

### 2.2 非目标

- 不通过 Admin API 动态替换 TLS 证书、QUIC listen 地址、Admin listen 地址或 token 文件路径。
- 不把启动配置文件完整原文回显给客户端；接口返回规范化后的 runtime snapshot。
- 不在 `/api/v1/config` 上做破坏性 schema 改名；新增 canonical 查询接口承载推荐配置 schema。
- 不引入多用户 RBAC、远程 Admin TLS 或公网访问能力。
- 不为了 relay worker 明细重构所有平台 relay 后端；没有平台快照能力时返回聚合与 `capabilities`。

## 3. 方案选择

### 3.1 方案 A：全量热更新控制面

把 `TqConfig` 的所有字段都纳入 GET/PATCH，包括 TLS、proto、relay、trace、server ACL 和 Admin 自身配置。

优点：接口完整。  
缺点：热更新边界大，涉及 QUIC listener、MsQuic settings、relay worker 和证书生命周期，容易引入数据面风险。

### 3.2 方案 B：只读配置快照 + 精选安全控制

新增完整只读 snapshot；只开放已有 runtime 操作或可安全切换的控制项，例如 server tunnel abort/drain、诊断状态查询，动态诊断开关放在后续小阶段。

优点：能快速补齐运维查询能力，风险低，接口边界清晰。  
缺点：不能一次解决所有“配置可修改”诉求。

### 3.3 方案 C：仅补齐文档，不新增代码

只在文档中说明缺口与未来方向，不设计具体接口。

优点：成本最低。  
缺点：不能指导实现，也无法约束后续接口 schema。

### 3.4 推荐

采用方案 B。Admin API 现在已经能做 peer/config/tunnel 局部控制，应避免把所有启动项都伪装成可热更新。先把“可查询”和“可安全控制”拆开，能让运维脚本获得完整视图，同时不扩大数据面风险。

## 4. 新增接口设计

### 4.1 Admin 自身状态

```text
GET /api/v1/admin
```

响应：

```json
{
  "role": "client",
  "listen": "127.0.0.1:18080",
  "auth": {
    "enabled": true,
    "type": "bearer",
    "token_file": "/tmp/tcpquic-proxy-admin/client-admin-12345.json",
    "token_present": true
  },
  "http": {
    "threads": 2,
    "max_body_bytes": 1048576,
    "keep_alive": false
  },
  "security": {
    "loopback_only": true,
    "tls": false
  }
}
```

不返回 token 内容。`token_file` 是路径，不是凭据；如果后续认为路径敏感，可通过查询参数 `?redact=true` 或默认脱敏目录部分。

### 4.2 运行时完整配置快照

```text
GET /api/v1/runtime/config
GET /api/v1/runtime/config?redact=true
```

响应使用 `docs/config_guide.md` 的推荐 schema，而不是内部 `TqConfig` 字段名。

Client 示例：

```json
{
  "role": "client",
  "source": {
    "config_path": "client.json",
    "client_config_path": "",
    "loaded_from_file": true
  },
  "tls": {
    "ca": "certs/ca.crt"
  },
  "admin": {
    "listen": "127.0.0.1:18080",
    "token_file": "/tmp/tcpquic-proxy-admin/client-admin-12345.json",
    "threads": 2
  },
  "proto": {
    "profile": "max-throughput",
    "disable_1rtt_encryption": true,
    "connections": 16,
    "connection_stream_count": 1024,
    "keepalive_ms": 5000,
    "iw": 4000,
    "initrtt_ms": 100
  },
  "client": {
    "handshake_threads": 8
  },
  "compression": {
    "mode": "off",
    "level": 1
  },
  "tuning": {
    "mode": "wan"
  },
  "relay": {
    "io_size": 1048576,
    "linux": {
      "read_chunk_size": 1048576,
      "tcp_write_max_bytes": 262144,
      "tcp_write_burst_bytes": 4194304
    }
  },
  "trace": {
    "enabled": false,
    "interval_sec": 10,
    "level": "info"
  },
  "diag_stats": {
    "enabled": false,
    "interval_sec": 5
  },
  "peers": [
    {
      "id": "agent-a",
      "proto_peer": "127.0.0.1:14444",
      "socks_listen": "127.0.0.1:11001",
      "http_listen": "",
      "port_forwards": [],
      "paths": [],
      "proto_connections": 16,
      "compress": "off",
      "enabled": true
    }
  ]
}
```

Server 示例只包含 server 有效字段，并保留 `server.allow_targets`、`server.deny_targets`。

脱敏规则：

- `proxy_auth[].password` 固定返回 `"***"`。
- token 内容永不返回。
- TLS key 文件只返回路径；若 `redact=true`，返回 basename 或 `"***"`。

### 4.3 Server 配置查询

```text
GET /api/v1/server/config
```

这是 `/api/v1/runtime/config` 的 server 子集，便于 server-only 运维脚本不解析完整配置。响应包含：

- `listen`、`resolved_listens`
- `allow_targets`、`deny_targets`
- `proto.profile`、`proto.disable_1rtt_encryption`、`proto.connection_stream_count`、`proto.keepalive_ms`、`proto.iw`、`proto.initrtt_ms`
- `compression`
- `tuning`
- `relay`
- `trace`、`diag_stats`

该接口只读。ACL 热更新不在本阶段实现，因为当前 ACL 绑定在 server stream/dial 判断路径，需要单独设计并发可见性和回滚。

### 4.4 Client peer canonical schema 查询

现有 `/api/v1/config` 继续返回内部兼容 schema：`peer_id/quic_peer/quic_connections`。

新增：

```text
GET /api/v1/client/config
GET /api/v1/peers/{peer_id}/config
```

响应使用推荐配置 schema：`id/proto_peer/proto_connections/proto_reconnect_interval_ms`。当前代码没有保存 per-peer reconnect interval 到 `TqPeerConfig`，第一阶段返回全局 `proto.reconnect_interval_ms` 作为 `effective_proto_reconnect_interval_ms`，不伪造 per-peer override。

```json
{
  "id": "agent-a",
  "proto_peer": "127.0.0.1:14444",
  "socks_listen": "127.0.0.1:11001",
  "http_listen": "",
  "port_forwards": [],
  "paths": [],
  "proto_connections": 16,
  "effective_proto_reconnect_interval_ms": 3000,
  "compress": "off",
  "enabled": true
}
```

第二阶段再决定是否把 `TqPeerConfig` 扩展出 `ReconnectIntervalMs` 并支持 PATCH。

### 4.5 Relay worker 与 active relay 查询

```text
GET /api/v1/relay/workers/{worker_id}
GET /api/v1/relay/active-relays
GET /api/v1/relay/active-relays/{relay_id}
```

`worker_id=aggregate` 继续可用。平台有真实 worker 快照时返回真实 worker；没有时：

```json
{
  "error": {
    "code": "not_supported",
    "message": "per-worker relay snapshot is not supported by this backend"
  }
}
```

`active-relays` 响应：

```json
{
  "capabilities": {
    "per_worker": false,
    "active_relay_detail": true
  },
  "relays": [
    {
      "relay_id": "relay-1",
      "backend": "windows",
      "worker_index": 0,
      "active_handlers": 1,
      "pending_quic_receive_bytes": 0,
      "outstanding_quic_send_bytes": 0,
      "tcp_read_bytes": 0,
      "tcp_write_bytes": 0,
      "closing": false
    }
  ]
}
```

Linux 当前只有 aggregate metrics 时，可以先返回空数组加 capability，后续再扩展 Linux worker runtime snapshot。

### 4.6 Server tunnel 单项查询与控制

```text
GET /api/v1/server/tunnels/{tunnel_id}
POST /api/v1/server/tunnels/{tunnel_id}:abort
POST /api/v1/server/tunnels/{tunnel_id}:drain
DELETE /api/v1/server/tunnels/{tunnel_id}
```

- `DELETE` 等价于 `:abort`，返回 `202 {"status":"aborting"}`。
- `:drain` 返回 `202 {"status":"draining"}`。
- 只允许操作 `role == "server"` 的 tunnel；client role tunnel 返回 `404`，避免跨角色误操作。

### 4.7 Diagnostics 状态和有限控制

```text
GET /api/v1/diagnostics
PATCH /api/v1/diagnostics
```

第一阶段只实现 `GET`。响应：

```json
{
  "trace": {
    "enabled": false,
    "interval_sec": 10,
    "level": "info",
    "runtime_mutable": false
  },
  "diag_stats": {
    "enabled": false,
    "interval_sec": 5,
    "runtime_mutable": false
  },
  "memory_allocator": {
    "dump_endpoint": "/api/v1/memory/allocator:dump"
  }
}
```

第二阶段 `PATCH` 只允许 runtime 已支持的字段；在当前 trace/diag-stats 生命周期未改造前返回：

```json
{
  "error": {
    "code": "not_supported",
    "message": "runtime diagnostics mutation is not supported"
  }
}
```

## 5. 架构调整

### 5.1 配置快照

新增 `TqRuntimeConfigSnapshot`，由启动后的 `TqConfig` 构造，只读持有在 Admin handler 生命周期中。

```cpp
struct TqRuntimeConfigSnapshot {
    TqMode Role{};
    TqConfig Config;
    std::string BoundAdminListen;
    std::string AdminTokenFile;
};
```

序列化独立放在 `src/runtime/admin_config.cpp`，避免继续扩大 `router_runtime.cpp`。

### 5.2 Admin 状态

`TqAdminHttpServer` 当前知道 `BoundListen`、`Options`、`Auth` 和 token path，但业务 handler 不知道。新增 `TqAdminHttpServer::StatusJson()` 或在 `TqAdminHttpServerOptions` 中传入 role，再由 `ConfigureRoutes()` 对 `/api/v1/admin` 做 server 层处理。

推荐 server 层处理 `/api/v1/admin`，因为该接口描述 Admin HTTP server 自身，不属于 client/server runtime。

### 5.3 Handler 分发

`TqIsV1AdminPath()` 需要加入新增公开路径。为避免白名单和 handler 分支漂移，建议新增表驱动 helper：

```cpp
bool TqIsV1KnownAdminPath(const std::string& path);
```

短期可以继续扩展白名单，但开发计划要求补充单测覆盖每个新增 path。

### 5.4 JSON 错误格式

现有 handler 层仍返回 `{"error":"<message>"}`，HTTP server 层返回 `{"error":{"code":"<code>","message":"<message>"}}`。新增接口统一使用结构化错误：

```json
{"error":{"code":"not_found","message":"not found"}}
```

兼容接口暂不改旧响应，避免破坏测试和脚本。

## 6. 系统测试设计

### 6.1 范围和目标

核心功能目标：

- 已认证 Admin 客户端能查询完整 runtime config、server config、admin status、diagnostics、relay active relays。
- 未认证请求不能获得任何配置、路径、ACL 或 token 相关信息。
- server tunnel 单项查询和控制只影响目标 server tunnel。
- 不支持的平台能力返回明确 `not_supported`，不是 500。

非功能目标：

- 只读接口 p95 延迟在本机 loopback 下小于 20 ms，p99 小于 100 ms。
- 32 个并发 Admin 请求不会阻塞数据面线程；Admin worker threads 上限仍为 1..32。
- 配置 snapshot JSON 不包含 token 内容或明文 proxy auth 密码。
- 在 relay/backend snapshot 不可用时接口仍返回 200 capability 或 404/503 类结构化错误，不崩溃。

### 6.2 端到端功能路径

| 路径 | 入口 | 依赖 | 断言 | 观测证据 |
| --- | --- | --- | --- | --- |
| Admin status | `GET /api/v1/admin` | `TqAdminHttpServer`、`TqAdminAuth` | 返回 listen、auth enabled、threads；不返回 token | HTTP 200、JSON 字段、stderr admin start 日志 |
| Runtime config | `GET /api/v1/runtime/config` | `TqConfig` snapshot、JSON serializer | client/server 字段按 role 输出，敏感值脱敏 | 单测 JSON 字段、E2E curl |
| Server config | `GET /api/v1/server/config` | server cfg snapshot、server metrics resolved listens | server role 返回 200，client role 返回 404 | server admin test |
| Relay active relays | `GET /api/v1/relay/active-relays` | relay runtime snapshot | unsupported backend 不 500；supported backend 返回 relays | relay metrics test |
| Server tunnel control | `POST /api/v1/server/tunnels/{id}:abort` | tunnel registry | 只 abort server role tunnel，返回 202 | server admin unit + registry counter |
| Diagnostics | `GET /api/v1/diagnostics` | trace/diag config snapshot | 返回 enabled/interval/runtime_mutable | router/server admin tests |
| Unauthorized | 任意新增接口无 token | auth layer | 返回 401，handler 不执行 | admin_http_test hit counter |

### 6.3 覆盖矩阵

| 类别 | 测试 |
| --- | --- |
| 单元测试 | JSON serializer 字段、脱敏、role-specific 输出、path whitelist |
| Handler 测试 | router/server handler 对新增 path 的 200/404/503/202 |
| HTTP 集成测试 | token 鉴权、payload limit、公开 `/api/v1` path 转发 |
| 并发测试 | 32 并发 GET config/admin/diagnostics，不出现 data race 或 5xx |
| 负向测试 | 无 token、错 token、非法 path segment、client 请求 server-only endpoint |
| 兼容测试 | 现有 `/api/v1/config` schema 和状态码不变 |

### 6.4 k6 性能基线

Admin API 是 loopback 运维面，建议在 Linux 上新增可选 k6 脚本：

- 场景：
  - `readonly_mix`：70% `/runtime/config`、10% `/admin`、10% `/diagnostics`、10% `/relay/metrics`
  - `server_tunnel_control_smoke`：低频 server tunnel 查询和 abort/drain mock，不进入压力阶段
- 流量：
  - baseline：1 VU，30 秒
  - expected peak：8 VU，60 秒
  - stress：32 VU，60 秒，对齐默认最大 admin threads 上限
  - spike：0 到 32 VU，10 秒爬升，30 秒保持
- 阈值：
  - `http_req_failed < 0.01`
  - `http_req_duration{scenario:readonly_mix}.p(95) < 20`
  - `http_req_duration{scenario:readonly_mix}.p(99) < 100`
  - 进程 RSS 不因只读请求持续增长

### 6.5 异常和恢复

| 场景 | 注入故障 | 预期影响 | 检测信号 | 恢复标准 |
| --- | --- | --- | --- | --- |
| token 缺失 | 删除 token 文件但请求仍带内存 token | 已持有 token 请求仍可用；新脚本无法发现 token | curl 401/200 分流、日志 | 重启后 token 文件重建 |
| relay snapshot unsupported | 平台无 per-worker snapshot | 返回 capability 或 not_supported | HTTP 状态和 code | 不出现崩溃和 500 |
| tunnel 已结束 | 控制已不存在 tunnel | 404 not_found | handler 测试 | registry 无额外副作用 |
| 并发 config 查询 | 32 并发请求 | 不阻塞数据面，不 data race | TSAN/单测、k6 | 无 5xx，延迟达标 |
| JSON 序列化异常 | 空路径、空 vectors、默认 cfg | 返回合法 JSON | unit test | 所有字段类型稳定 |

## 7. 迁移与兼容

- 现有接口不删除、不改状态码。
- `/api/v1/config` 保持当前内部 schema；新增 `/api/v1/runtime/config` 和 `/api/v1/client/config` 承载推荐 schema。
- 新增结构化错误只用于新增接口；旧接口的 `{"error":"<message>"}` 可在后续独立兼容窗口迁移。
- 文档更新顺序：先更新 `interface.md` 新接口表，再补充示例和缺口状态。

## 8. 风险和缓解

| 风险 | 缓解 |
| --- | --- |
| 配置 snapshot 暴露敏感信息 | 默认脱敏 token、proxy auth password；提供 `redact=true` 强脱敏 |
| schema 双轨增加维护成本 | 新增 serializer 明确“public config schema”，旧 `/config` 标记兼容 |
| server tunnel 控制误伤 client tunnel | handler 检查 `role == "server"` |
| relay backend 能力不一致 | 响应带 `capabilities`，unsupported 返回结构化错误 |
| Admin status 需要访问 server 内部状态 | `/api/v1/admin` 在 `TqAdminHttpServer` 层实现，避免 handler 反向依赖 |

## 9. 开放问题

1. `token_file` 路径是否默认完整返回。本文建议完整返回，因为启动日志已经打印路径；安全环境可用 `redact=true`。
2. 是否在第一阶段支持 `PATCH /api/v1/diagnostics`。本文建议第一阶段只实现 GET，PATCH 返回 `not_supported`，等 trace/diag runtime 生命周期改造后再开放。
3. 是否将 `proto_reconnect_interval_ms` 纳入 `TqPeerConfig`。本文建议先输出 effective 值，不引入未实现的 per-peer override。
