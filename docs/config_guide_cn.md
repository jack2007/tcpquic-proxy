# tcpquic-proxy 配置文件指南

`tcpquic-proxy` 使用一个运行配置文件启动：

```bash
tcpquic-proxy client --config client.json
tcpquic-proxy server --config server.json
```

配置文件必须是严格 JSON。下面示例为了说明方便使用 JSONC 注释，实际使用前需要删除注释。

client 统一使用 `peers` 配置所有对端。只有一个远端 server 时，也配置一个元素的 `peers` 数组。不再需要单独的 multi-peer `client_config` 文件。

## Client 示例

```jsonc
{
  "tls": {
    // client 用该 CA 证书验证 server 证书。
    "ca": "certs/ca.crt"
  },
  "admin": {
    // 可选 admin HTTP 端口。公开 Admin API 位于 /api/v1/*。
    "listen": "127.0.0.1:18080",
    // 可选 token JSON 路径；省略时使用包含 pid 的运行时默认路径。
    "token_file": "/tmp/tcpquic-proxy-admin/admin-token.json",
    // 固定 Admin HTTP worker 线程池大小，范围 1..32。
    "threads": 2
  },
  "proto": {
    // 协议执行 profile。吞吐测试使用 max-throughput。
    "profile": "max-throughput",

    // 不安全的实验室开关：禁用 QUIC 1-RTT 包加密。
    // 默认值为 true；设为 false 可启用包加密。
    "disable_1rtt_encryption": true,

    // 默认连接设置。peer 未覆盖时继承这些值。
    "connections": 16,
    "connection_stream_count": 1024,
    "reconnect_interval_ms": 3000,
    "keepalive_ms": 5000,

    // 高 BDP / 高吞吐测试使用的启动期实验 override。
    "iw": 4000,
    "initrtt_ms": 100
  },
  "client": {
    // SOCKS/HTTP 握手 worker 线程数。0 表示自动。
    "handshake_threads": 8,

    // 可选启动预热。使用时要求只有一个 enabled peer。
    "warmup_mb": 0,
    "warmup_target": "127.0.0.1:5201",
    "warmup_path": "/"

    // 可选内置测速。三者只能配置一个：
    // "download_test": 30,
    // "download_sink_test": 30,
    // "upload_test": 30
  },
  "compression": {
    // auto 表示自适应探测，zstd 表示强制压缩，off 表示关闭压缩。
    "mode": "off",
    "level": 1
  },
  "tuning": {
    // 支持 auto、lan、wan。
    "mode": "wan"
  },
  "relay": {
    "io_size": 1048576,
    "linux": {
      "read_chunk_size": 1048576,
      "worker_slots": 1024,
      "tcp_write_max_bytes": 262144,
      "tcp_write_burst_bytes": 4194304
    }
  },
  "trace": {
    "enabled": false,
    "interval_sec": 10
  },
  "peers": [
    {
      // 稳定 peer id，用于 admin/runtime 状态。
      "id": "dgx-b",

      // 远端 tcpquic-proxy server 协议端点。
      // 未配置 paths 时可写逗号分隔列表；连接槽会按 proto_connections round-robin 到多个端点。
      "proto_peer": "10.0.0.2:4433",

      // 该 peer 的本地代理监听地址。
      "socks_listen": "127.0.0.1:1080",
      "http_listen": "127.0.0.1:8080",
      "port_forwards": [
        {"listen": "127.0.0.1:15432", "target": "db.internal.example.com:5432"}
      ],

      // 可选 peer 级 override。
      "proto_connections": 16,
      "proto_reconnect_interval_ms": 3000,
      "compress": "off",
      "enabled": true
    }
  ]
}
```

## 多网口 paths 示例

未配置 `paths` 时，client 只会按 `proto_peer` 地址列表分配远端端点，本地出口由系统路由决定。需要显式固定本地地址到远端地址时，在 peer 下配置 `paths`：

```jsonc
{
  "tls": {
    "ca": "certs/ca.crt"
  },
  "peers": [
    {
      "id": "dgx-b",
      "socks_listen": "127.0.0.1:1080",
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

配置 `paths` 后，peer 级 `proto_peer` 和 `proto_connections` 不参与连接槽分配；总 QUIC connection 数为所有 path 的 `connections` 之和。

## 本地端口转发示例

配置多个本地转发时，添加多个 `port_forwards` 数组元素。

```json
{
  "id": "db",
  "proto_peer": "proxy-b.example.com:443",
  "socks_listen": "",
  "http_listen": "",
  "port_forwards": [
    {"listen": "127.0.0.1:15432", "target": "db.internal.example.com:5432"}
  ]
}
```

## Server 示例

```jsonc
{
  "tls": {
    // server 向 client 出示的证书和私钥。
    "cert": "certs/server.crt",
    "key": "certs/server.key"
  },
  "admin": {
    // 可选 admin HTTP 端口。公开 Admin API 位于 /api/v1/*。
    "listen": "127.0.0.1:18081"
  },
  "server": {
    // 协议监听端点。可写逗号分隔列表；0.0.0.0 会展开为本机非本地 IPv4 地址。
    "proto_listen": "0.0.0.0:4433",

    // server 目标访问 ACL。server 模式必填。
    "allow_targets": ["127.0.0.1/32", "10.0.0.0/8"],
    "deny_targets": ["169.254.0.0/16"]
  },
  "proto": {
    "profile": "max-throughput",
    "disable_1rtt_encryption": true,
    "iw": 4000,
    "initrtt_ms": 100
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
      "worker_slots": 1024,
      "tcp_write_max_bytes": 262144,
      "tcp_write_burst_bytes": 4194304
    }
  },
  "trace": {
    "enabled": false,
    "interval_sec": 10
  }
}
```

## 配置项说明

| 配置项 | 模式 | 作用 |
| --- | --- | --- |
| `tls.cert` | server | TLS server 证书 PEM 路径。server 模式必填；client 模式忽略。 |
| `tls.key` | server | TLS server 私钥 PEM 路径。server 模式必填；client 模式忽略。 |
| `tls.ca` | client | 用于验证 server 证书的 CA 证书 PEM 路径。client 模式必填；server 模式可选且不使用。 |
| `admin.listen` | client/server | admin HTTP 监听地址。 |
| `admin.token_file` | client/server | Admin Bearer token JSON 文件路径。 |
| `admin.threads` | client/server | 固定 Admin HTTP worker 线程数，范围 1..32，默认 2。 |
| `proto.profile` | client/server | `max-throughput` 或 `low-latency`。 |
| `proto.disable_1rtt_encryption` | client/server | 不安全的 QUIC 1-RTT 加密禁用开关，默认 `true`；设为 `false` 可启用包加密。 |
| `proto.connections` | client | 默认协议连接数，peer 未覆盖时继承。 |
| `proto.connection_stream_count` | client/server | 每条 QUIC 连接允许的双向 stream 上限，范围 1..65535。 |
| `proto.reconnect_interval_ms` | client | 默认重连间隔，peer 未覆盖时继承，范围 1000..60000 ms。 |
| `proto.keepalive_ms` | client/server | keepalive 心跳间隔，范围 1000..15000 ms，默认 5000。 |
| `proto.iw` | client/server | 实验性质的 QUIC 初始拥塞窗口 override，单位 packets。 |
| `proto.initrtt_ms` | client/server | 实验性质的 QUIC 初始 RTT 估计 override。 |
| `client.handshake_threads` | client | SOCKS/HTTP 握手 worker 线程数。 |
| `client.warmup_mb` | client | 启动预热下载大小。 |
| `client.warmup_target` | client | 预热目标 host:port。 |
| `client.warmup_path` | client | 预热 HTTP GET 路径。 |
| `client.download_test` | client | 内置下载测速秒数。要求只有一个 enabled peer。 |
| `client.download_sink_test` | client | 内置 download sink 测速秒数。要求只有一个 enabled peer。 |
| `client.upload_test` | client | 内置上传测速秒数。要求只有一个 enabled peer。 |
| `server.proto_listen` | server | 协议监听端点。server 模式必填。支持逗号分隔 `ip:port` 列表；`0.0.0.0:port` 会展开为本机非本地 IPv4 地址并分别启动 listener。 |
| `server.allow_targets` | server | server 必填访问 ACL，支持数组或逗号分隔字符串。 |
| `server.deny_targets` | server | 可选 deny ACL。 |
| `compression.mode` | client/server | `auto`、`zstd` 或 `off`。 |
| `compression.level` | client/server | Zstd 压缩级别。 |
| `tuning.mode` | client/server | `auto`、`lan` 或 `wan`。 |
| `relay.io_size` | client/server | relay IO chunk 大小 override。 |
| `relay.linux.read_chunk_size` | client/server | Linux relay TCP read chunk 大小。 |
| `relay.linux.worker_slots` | client/server | 每 tunnel 的 Linux relay worker buffer slot 数。 |
| `relay.linux.tcp_write_max_bytes` | client/server | 每次 Linux relay TCP `sendmsg` 字节上限；不需要 override 时省略。 |
| `relay.linux.tcp_write_burst_bytes` | client/server | 每轮 TCP write flush 字节上限；不需要 override 时省略。 |
| `trace.enabled` | client/server | 开启 trace/debug 文件日志。 |
| `trace.interval_sec` | client/server | 周期性 trace 间隔，单位秒。 |
| `peers[].id` | client | 稳定 peer id。必填。 |
| `peers[].proto_peer` | client | 远端协议端点。未配置 `paths` 时必填；支持逗号分隔列表，连接槽按 `proto_connections` round-robin 到多个端点，出口由系统路由决定。 |
| `peers[].socks_listen` | client | 该 peer 的可选 SOCKS5 监听地址；每个 peer 至少需要 SOCKS、HTTP 或 `port_forwards` 中的一种入口。 |
| `peers[].http_listen` | client | 该 peer 的可选 HTTP CONNECT 监听地址。 |
| `peers[].port_forwards` | client | 可选本地端口转发列表，默认 `[]`。每项包含 `listen` 和 `target`。 |
| `peers[].proto_connections` | client | 可选 peer 级连接数 override，范围 1..128。配置 `paths` 后忽略该字段。 |
| `peers[].proto_reconnect_interval_ms` | client | 可选 peer 级重连间隔 override。 |
| `peers[].compress` | client | 可选 peer 级压缩模式 override。 |
| `peers[].enabled` | client | 是否启动该 peer。 |
| `peers[].paths` | client | 可选多网口显式路径数组。非空时忽略 peer 级 `proto_peer` 和 `proto_connections`。 |
| `peers[].paths[].name` | client | path 名称。必填，单个 peer 内必须唯一，用于日志、trace 和 admin 状态。 |
| `peers[].paths[].local` | client | 本地源 IP。必填，必须是具体 IP，不支持 hostname。程序只绑定源地址，不创建策略路由。 |
| `peers[].paths[].peer` | client | 远端 server 协议端点 `host:port`。必填。 |
| `peers[].paths[].connections` | client | 该 path 展开的 QUIC connection 数，单条 path 范围为 1..128；同一 peer 下所有 paths 的 `connections` 总和范围也必须是 1..128。总和也是该 peer 的总连接数。 |

## Admin 观测

Admin connection JSON 会包含每个 QUIC connection slot 的 path 信息。未配置 `paths` 时这些字段为空或表示默认 peer；配置 `paths` 后可通过 `path`、`local`、`peer` 判断 slot 绑定到哪条本地到远端路径。

```json
{
  "connection_id": "conn-0",
  "slot_index": 0,
  "generation": 1,
  "connected": true,
  "retry_scheduled": false,
  "state": "connected",
  "path": "cmcc",
  "local": "10.10.1.2",
  "peer": "36.1.1.10:443",
  "active_tunnels": 0,
  "last_error": ""
}
```

## 注意事项

- 配置 schema 使用 `proto`，不是 `quic`。当前内部实现仍可能使用 QUIC 命名，因为现有协议后端是 QUIC。
- client 无论单对端还是多对端，都应使用 `peers` 数组。
- TCP tunnel 调度粒度是一个 TCP 连接。一个 TCP 连接绑定到一条 QUIC connection，生命周期内不跨 slot；多个并发 TCP 连接会在同一 peer 的已连接 slot 间 round-robin，`connections` 也是默认分配权重。
- 启动阶段诊断使用 `--trace` / `trace.enabled`；client 启动连接不再需要单独的 connect-on-start 开关。
- 内置 speed-test 配置要求只有一个 enabled peer。
- `proto.disable_1rtt_encryption: true` 不安全，只应用于隔离实验室测试。
