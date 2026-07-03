# Admin API 待实现功能实施记录

本文档记录从 [`interface.md`](interface.md) 拆出的 Admin API 设计与开发计划的实施结果。`interface.md` 只保留当前公开接口、请求体和响应模型说明。

## 实施状态

截至 2026-07-03，原计划中的 Admin API 功能已完成第一轮落地：

1. 推荐配置 schema 查询已对齐。
   - `GET /api/v1/runtime/config` 返回完整运行时配置快照，并包含 `max_memory_mb`。
   - `GET /api/v1/client/config` 返回 client 推荐 schema 字段，包括 `socks_listen`、`http_listen`、`proto`、`speed_test`、`handshake_threads`、`compression` 和 `peers`。
   - `GET /api/v1/peers/{peer_id}/config` 返回 peer 局部推荐 schema，不泄露 proxy auth。
   - `GET /api/v1/diagnostics` 返回 `trace_level`。

2. Peer 写接口已兼容推荐字段别名。
   - `id` 等价于 `peer_id`。
   - `proto_peer` 等价于 `quic_peer`。
   - `proto_connections` 等价于 `quic_connections`。
   - 同一请求混用同义新旧字段会返回 `400 {"error":"conflicting peer field aliases"}`。

3. Diagnostics 动态开关已接入。
   - 新增 `PATCH /api/v1/diagnostics`。
   - 支持字段：`trace`、`trace_interval_sec`、`trace_level`、`diag_stats`、`diag_stats_interval_sec`。
   - `trace_interval_sec` 和 `diag_stats_interval_sec` 校验范围为 `1..86400`。
   - `trace_level` 只允许 `info`、`debug`。
   - Patch 成功后会调用 trace/diag-stats 运行时启停入口，使开关、interval 和 level 生效。

4. Relay worker capability 和 worker snapshot 已接入。
   - `GET /api/v1/relay/workers` 返回 `capabilities` 和 worker 列表。
   - `aggregate` worker 保留为跨平台兼容入口。
   - Linux runtime 已暴露 `linux-N` worker 快照；runtime 未启动时只返回 `aggregate`。
   - Worker 详情返回该 worker 计数和空 `relays` 数组；当前 `per_worker_active_relays` 仍为 `false`。

5. Runtime 配置热更新已接入。
   - 新增 `PATCH /api/v1/runtime/config`。
   - 第一阶段允许更新 `compression.mode`、`compression.level`、`tuning.max_memory_mb`。
   - `admin`、`tls`、`proto`、`relay`、`listen` 等启动级字段返回 `503 not_supported`。
   - 未知字段返回 `400`。

6. Admin Console 已对接公开接口。
   - Diagnostics 页读取 `GET /diagnostics` 并提交 `PATCH /diagnostics`。
   - Relay 页读取 `/relay/metrics` 和 `/relay/workers`，展示 capability 与 worker 列表。
   - Config 页支持提交 `PATCH /runtime/config`。
   - Console 不调用 legacy internal path。

## 当前边界

1. Server 模式的 Admin handler 仍接收 `const TqConfig&`。因此 `PATCH /api/v1/diagnostics` 和 `PATCH /api/v1/runtime/config` 会返回本次 patch 后的响应，并对 trace/diag-stats 运行时状态生效；但 server 配置快照本身尚未改为可持久保存 patch 后状态的共享 runtime config。

2. Relay worker 详情当前暴露 worker 级计数，不暴露逐 relay 明细。能力字段明确返回 `per_worker_active_relays:false`，前端应据此隐藏逐 relay 明细入口。

3. Runtime config patch 第一阶段只覆盖低风险字段。TLS、Admin listen、QUIC profile、relay backend、listen 地址等启动级字段仍需要修改配置并重启进程。

## 主要修改文件

- `src/runtime/admin_config.cpp`
- `src/runtime/admin_config.h`
- `src/runtime/admin_console.cpp`
- `src/runtime/relay_metrics.cpp`
- `src/runtime/relay_metrics.h`
- `src/runtime/router_runtime.cpp`
- `src/runtime/server_admin.cpp`
- `src/runtime/trace.cpp`
- `src/runtime/trace.h`
- `src/tunnel/linux_relay_worker.cpp`
- `src/tunnel/linux_relay_worker.h`
- `src/unittest/admin_http_test.cpp`
- `src/unittest/router_runtime_test.cpp`
- `src/unittest/server_admin_test.cpp`

## 验收命令

```bash
rtk cmake --build build --target tcpquic_router_runtime_test tcpquic_server_admin_test tcpquic_admin_http_test tcpquic_trace_network_stats_test tcpquic_config_router_test tcpquic_linux_relay_worker_io_test -j2
rtk ./build/bin/Release/tcpquic_router_runtime_test
rtk ./build/bin/Release/tcpquic_server_admin_test
rtk ./build/bin/Release/tcpquic_admin_http_test
rtk ./build/bin/Release/tcpquic_trace_network_stats_test
rtk ./build/bin/Release/tcpquic_config_router_test
rtk ./build/bin/Release/tcpquic_linux_relay_worker_io_test
```

## 后续建议

1. 将 server runtime config 从只读 `const TqConfig&` 改为共享可更新状态，使 server 模式后续 `GET /diagnostics` 和 `GET /runtime/config` 能反映最近一次 Admin patch。
2. 在 relay worker 层补逐 relay snapshot 后，再把 `per_worker_active_relays` 切换为 `true` 并填充 worker 详情中的 `relays` 数组。
