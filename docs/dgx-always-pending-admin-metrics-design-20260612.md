# Phase 4 always-pending Admin Metrics 设计

时间：2026-06-12

适用分支：`master` / Phase 4 always-pending

## 目标

为 DGX bench 同时提供 client/server 两端的 admin `/metrics` 可观测性，重点覆盖 always-pending relay path 的 pending receive、`sendmsg`、`StreamReceiveComplete` 聚合、inline QUIC receive fast path 和 backpressure 指标。

## 接口

single-peer client 与 server 均支持：

```text
GET /health
GET /metrics
```

client JSON 基础字段：

| 字段 | 含义 |
|------|------|
| `role` | 固定为 `client` |
| `status` | 无错误且至少 1 条 QUIC connection connected 时为 `healthy`，否则 `degraded` |
| `quic_peer` | client 当前 QUIC peer |
| `socks_listen` | SOCKS listen 地址 |
| `http_listen` | HTTP CONNECT listen 地址 |
| `connection_count` | client 配置/启动的 QUIC connection 数 |
| `connected_connections` | 当前 connected QUIC connection 数 |
| `uptime_seconds` | 进程启动后的秒数 |
| `last_error` | 最近错误，当前 single-peer client 预留为空 |

server JSON 继续保留：

| 字段 | 含义 |
|------|------|
| `role` | 固定为 `server` |
| `listen` | QUIC listen 地址 |
| `accepted_connections` | 接受过的 QUIC connection 数 |
| `active_streams` | 当前活跃 stream 数 |
| `total_streams` | 累计 stream 数 |
| `acl_denied` | ACL 拒绝数 |
| `last_error` | 最近 server 错误 |

## 共用 relay metrics

client/server JSON 都追加同一组 `linux_relay_*` 字段，由 `runtime/relay_metrics.cpp` 从 `TqLinuxRelayRuntime::Snapshot()` 读取。

基础 worker 指标：

| 字段 | 用途 |
|------|------|
| `linux_relay_backend` | Linux 为 `worker`，非 Linux 为 `unsupported` |
| `linux_relay_wakeups` | worker wakeup 写次数 |
| `linux_relay_events_processed` | worker 已处理事件数 |
| `linux_relay_pending_events` | event queue 当前估算长度 |
| `linux_relay_pending_bytes` | relay pool、TCP write queue、pending QUIC receive 的当前 bytes |
| `linux_relay_tcp_read_bytes` | TCP->QUIC 方向累计 read bytes |
| `linux_relay_tcp_write_bytes` | QUIC->TCP 方向累计 write bytes |
| `linux_relay_max_tcp_read_iov_used` | TCP read path 最大 iov 数 |
| `linux_relay_max_tcp_write_iov_used` | TCP write path 最大 iov 数 |
| `linux_relay_read_disabled_count` | TCP read backpressure 触发次数 |
| `linux_relay_errors` | relay worker 错误数 |

always-pending 关键指标：

| 字段 | 用途 |
|------|------|
| `linux_relay_tcp_write_sendmsg_calls` | QUIC->TCP `sendmsg` 调用次数 |
| `linux_relay_max_tcp_write_sendmsg_bytes` | 单次 `sendmsg` 最大写入 bytes |
| `linux_relay_tcp_write_eagain_count` | TCP 写 `EAGAIN` 次数 |
| `linux_relay_tcp_write_partial_count` | TCP partial write 次数 |
| `linux_relay_deferred_receive_complete_bytes` | 已向 MsQuic complete 的 receive bytes |
| `linux_relay_deferred_receive_completes` | `StreamReceiveComplete` 调用次数 |
| `linux_relay_deferred_receive_completion_flushes` | completion batch flush 次数 |
| `linux_relay_max_pending_quic_receive_bytes` | 单 relay pending QUIC receive bytes 高水位 |
| `linux_relay_max_pending_quic_receive_queue` | 单 relay pending QUIC receive view 队列高水位 |
| `linux_relay_quic_receive_paused_count` | `ReceiveSetEnabled(false)` 次数 |
| `linux_relay_quic_receive_resumed_count` | `ReceiveSetEnabled(true)` 次数 |

inline receive fast path 指标：

| 字段 | 用途 |
|------|------|
| `linux_relay_inline_quic_receive_attempts` | callback 内 inline `sendmsg` 尝试次数 |
| `linux_relay_inline_quic_receive_full_writes` | inline 一次完整写完次数 |
| `linux_relay_inline_quic_receive_partial_writes` | inline partial 次数 |
| `linux_relay_inline_quic_receive_eagain_count` | inline `EAGAIN` 次数 |
| `linux_relay_inline_quic_receive_budget_exceeded` | receive 超过 inline budget 次数 |
| `linux_relay_inline_quic_receive_bytes` | inline 已写 bytes |
| `linux_relay_max_inline_quic_receive_bytes` | 单次 inline 最大写入 bytes |

压缩相关指标：

| 字段 | 用途 |
|------|------|
| `linux_relay_compressed_tcp_bytes` | TCP->QUIC 压缩前/后路径累计压缩 bytes |
| `linux_relay_decompressed_tcp_bytes` | QUIC->TCP 解压累计 bytes |

## DGX 使用方式

bench 脚本可在 client/server 两端采集：

```bash
curl -s http://127.0.0.1:<admin-port>/metrics
```

建议每个 case 至少采集：

1. bench 前 baseline `/metrics`
2. bench 后 `/metrics`
3. 两者差值，用于计算每秒 `sendmsg`、每次 complete bytes、inline full ratio、EAGAIN/partial 频率

重点派生指标：

| 派生指标 | 计算 |
|----------|------|
| average complete bytes | delta `deferred_receive_complete_bytes` / delta `deferred_receive_completes` |
| sendmsg average bytes | delta `tcp_write_bytes` / delta `tcp_write_sendmsg_calls` |
| inline full ratio | delta `inline_quic_receive_full_writes` / delta `inline_quic_receive_attempts` |
| receive pause pressure | delta `quic_receive_paused_count` 与 pending high-water |
| TCP write pressure | delta `tcp_write_eagain_count`、delta `tcp_write_partial_count` |

