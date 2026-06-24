# tcpquic-proxy config guide

`tcpquic-proxy` uses one runtime JSON config file:

```bash
tcpquic-proxy client --config client.json
tcpquic-proxy server --config server.json
```

The config file is strict JSON. The examples below use JSONC comments for readability; remove comments before using them.

Client config uses `peers` for all destinations. A single remote server is represented as a one-element `peers` array. There is no separate multi-peer client-config file in the recommended schema.

## Client Example

```jsonc
{
  "tls": {
    // CA certificate used by the client to verify the server certificate.
    "ca": "certs/ca.crt"
  },
  "admin": {
    // Optional admin HTTP endpoint. Public Admin APIs are under /api/v1/*.
    "listen": "127.0.0.1:18080",
    // Optional token JSON path. If omitted, a pid-scoped runtime path is used.
    "token_file": "/tmp/tcpquic-proxy-admin/admin-token.json",
    // Fixed Admin HTTP worker thread pool size, range 1..32.
    "threads": 2
  },
  "proto": {
    // QUIC execution profile. Use max-throughput for bandwidth tests.
    "profile": "max-throughput",

    // Insecure lab-only switch that disables QUIC 1-RTT packet encryption.
    // Default is true; set false to enable packet encryption.
    "disable_1rtt_encryption": true,

    // Default connection settings inherited by peers unless overridden.
    "connections": 16,
    "connection_stream_count": 1024,
    "reconnect_interval_ms": 3000,
    "keepalive_ms": 5000,

    // Experimental startup overrides for high-BDP/high-throughput tests.
    "iw": 4000,
    "initrtt_ms": 100
  },
  "client": {
    // SOCKS/HTTP handshake worker threads. 0 means auto.
    "handshake_threads": 8,

    // Optional startup warmup. Requires one enabled peer when used.
    "warmup_mb": 0,
    "warmup_target": "127.0.0.1:5201",
    "warmup_path": "/"

    // Optional built-in speed test. Only one may be set:
    // "download_test": 30,
    // "download_sink_test": 30,
    // "upload_test": 30
  },
  "compression": {
    // auto probes, zstd forces compression, off disables compression.
    "mode": "off",
    "level": 1
  },
  "tuning": {
    // Supported modes: auto, lan, wan.
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
      // Stable peer id for admin/runtime state.
      "id": "dgx-b",

      // Remote tcpquic-proxy server protocol endpoint.
      "proto_peer": "10.0.0.2:4433",

      // Local proxy listeners for this peer.
      "socks_listen": "127.0.0.1:1080",
      "http_listen": "127.0.0.1:8080",
      "port_forwards": [
        {"listen": "127.0.0.1:15432", "target": "db.internal.example.com:5432"}
      ],

      // Optional per-peer overrides.
      "proto_connections": 16,
      "proto_reconnect_interval_ms": 3000,
      "compress": "off",
      "enabled": true
    }
  ]
}
```

## Local Port Forward Example

Use multiple `port_forwards` array items to configure multiple local forwards.

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

## Server Example

```jsonc
{
  "tls": {
    // Server certificate and private key presented to clients.
    "cert": "certs/server.crt",
    "key": "certs/server.key"
  },
  "admin": {
    // Optional admin HTTP endpoint. Public Admin APIs are under /api/v1/*.
    "listen": "127.0.0.1:18081"
  },
  "server": {
    // QUIC listen endpoint.
    "proto_listen": "0.0.0.0:4433",

    // Target ACL. Required in server mode.
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

## Config Keys

| Key | Mode | Purpose |
| --- | --- | --- |
| `tls.cert` | server | TLS server certificate PEM path. Required in server mode; ignored in client mode. |
| `tls.key` | server | TLS server private key PEM path. Required in server mode; ignored in client mode. |
| `tls.ca` | client | CA certificate PEM path used to verify the server certificate. Required in client mode; optional and unused in server mode. |
| `admin.listen` | client/server | Admin HTTP listen address. |
| `admin.token_file` | client/server | Admin Bearer token JSON file path. |
| `admin.threads` | client/server | Fixed Admin HTTP worker threads, range 1..32, default 2. |
| `proto.profile` | client/server | `max-throughput` or `low-latency`. |
| `proto.disable_1rtt_encryption` | client/server | Insecure lab-only QUIC 1-RTT encryption disable switch. Defaults to `true`; set `false` to enable packet encryption. |
| `proto.connections` | client | Default QUIC connection count inherited by peers. |
| `proto.connection_stream_count` | client/server | Max bidirectional streams allowed per QUIC connection, range 1..65535. |
| `proto.reconnect_interval_ms` | client | Default reconnect interval inherited by peers, 1000..60000 ms. |
| `proto.keepalive_ms` | client/server | Keepalive interval, 1000..15000 ms. Defaults to 5000. |
| `proto.iw` | client/server | Experimental QUIC initial congestion window override in packets. |
| `proto.initrtt_ms` | client/server | Experimental QUIC initial RTT estimate override. |
| `client.handshake_threads` | client | SOCKS/HTTP handshake worker threads. |
| `client.warmup_mb` | client | Startup warmup download size per enabled peer. |
| `client.warmup_target` | client | Warmup target host:port. |
| `client.warmup_path` | client | Warmup HTTP GET path. |
| `client.download_test` | client | Built-in download speed-test duration. Requires exactly one enabled peer. |
| `client.download_sink_test` | client | Built-in download sink test duration. Requires exactly one enabled peer. |
| `client.upload_test` | client | Built-in upload speed-test duration. Requires exactly one enabled peer. |
| `server.proto_listen` | server | QUIC listen endpoint. Required in server mode. |
| `server.allow_targets` | server | Required target ACL, string array or comma-separated string. |
| `server.deny_targets` | server | Optional deny ACL. |
| `compression.mode` | client/server | `auto`, `zstd`, or `off`. |
| `compression.level` | client/server | Zstd compression level. |
| `tuning.mode` | client/server | `auto`, `lan`, or `wan`. |
| `relay.io_size` | client/server | Relay IO chunk size override. |
| `relay.linux.read_chunk_size` | client/server | Linux relay TCP read chunk size. |
| `relay.linux.worker_slots` | client/server | Linux relay worker buffer slots per tunnel. |
| `relay.linux.tcp_write_max_bytes` | client/server | Cap bytes per Linux relay TCP `sendmsg`; omit when not overriding. |
| `relay.linux.tcp_write_burst_bytes` | client/server | Cap bytes per TCP write flush burst; omit when not overriding. |
| `trace.enabled` | client/server | Enable trace/debug file logging. |
| `trace.interval_sec` | client/server | Periodic trace interval in seconds. |
| `peers[].id` | client | Stable peer id. Required. |
| `peers[].proto_peer` | client | Remote protocol endpoint. Required. |
| `peers[].socks_listen` | client | Optional SOCKS5 listener for this peer; each peer requires at least one ingress among SOCKS, HTTP, or `port_forwards`. |
| `peers[].http_listen` | client | Optional HTTP CONNECT listener for this peer. |
| `peers[].port_forwards` | client | Optional local port-forward rules, default `[]`. Each item contains `listen` and `target`. |
| `peers[].proto_connections` | client | Optional per-peer connection count override. |
| `peers[].proto_reconnect_interval_ms` | client | Optional per-peer reconnect interval override. |
| `peers[].compress` | client | Optional per-peer compression override. |
| `peers[].enabled` | client | Whether the peer starts active. |

## Notes

- Use `proto`, not `quic`, in the config schema. Internal code may still use QUIC names because the current protocol backend is QUIC.
- Client configs should use `peers` even for a single remote server.
- Use `--trace` / `trace.enabled` for startup diagnostics; client startup no longer needs a separate connect-on-start switch.
- Built-in speed-test options require exactly one enabled peer.
- `proto.disable_1rtt_encryption: true` is insecure and should be used only in isolated lab measurements.
