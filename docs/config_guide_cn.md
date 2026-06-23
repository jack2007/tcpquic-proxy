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
    // 可选 admin HTTP 端口，用于 /health、/metrics 和 router 管理接口。
    "listen": "127.0.0.1:18080"
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
    // 可选 admin HTTP 端口，用于 /health 和 /metrics。
    "listen": "127.0.0.1:18081"
  },
  "server": {
    // 协议监听端点。
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
| `server.proto_listen` | server | 协议监听端点。server 模式必填。 |
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
| `peers[].proto_peer` | client | 远端协议端点。必填。 |
| `peers[].socks_listen` | client | 该 peer 的可选 SOCKS5 监听地址；每个 peer 至少需要 SOCKS、HTTP 或 `port_forwards` 中的一种入口。 |
| `peers[].http_listen` | client | 该 peer 的可选 HTTP CONNECT 监听地址。 |
| `peers[].port_forwards` | client peer | `[]` | 本地端口转发列表，每项包含 `listen` 和 `target` |
| `peers[].proto_connections` | client | 可选 peer 级连接数 override。 |
| `peers[].proto_reconnect_interval_ms` | client | 可选 peer 级重连间隔 override。 |
| `peers[].compress` | client | 可选 peer 级压缩模式 override。 |
| `peers[].enabled` | client | 是否启动该 peer。 |

## 注意事项

- 配置 schema 使用 `proto`，不是 `quic`。当前内部实现仍可能使用 QUIC 命名，因为现有协议后端是 QUIC。
- client 无论单对端还是多对端，都应使用 `peers` 数组。
- 启动阶段诊断使用 `--trace` / `trace.enabled`；client 启动连接不再需要单独的 connect-on-start 开关。
- 内置 speed-test 配置要求只有一个 enabled peer。
- `proto.disable_1rtt_encryption: true` 不安全，只应用于隔离实验室测试。
