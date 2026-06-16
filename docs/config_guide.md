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
    // Client certificate, private key, and CA certificate used by MsQuic TLS.
    "cert": "certs/client.crt",
    "key": "certs/client.key",
    "ca": "certs/ca.crt"
  },
  "admin": {
    // Optional admin HTTP endpoint for /health, /metrics, and router APIs.
    "listen": "127.0.0.1:18080"
  },
  "proto": {
    // QUIC execution profile. Use max-throughput for bandwidth tests.
    "profile": "max-throughput",

    // Insecure lab-only switch that disables QUIC 1-RTT packet encryption.
    "disable_1rtt_encryption": false,

    // Default connection settings inherited by peers unless overridden.
    "connections": 16,
    "reconnect_interval_ms": 3000,

    // QUIC transport overrides for high-BDP/high-throughput tests.
    "fcw": 1073741824,
    "srw": 1073741824,
    "iw": 4000,
    "initrtt_ms": 1
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
    // Use custom when setting explicit override fields.
    "mode": "custom",
    "target_bandwidth_mbps": 100000,
    "target_rtt_ms": 1,
    "max_memory_mb": 4096
  },
  "relay": {
    "io_size": 1048576,
    "inflight_bytes": 1073741824,
    "linux": {
      "read_chunk_size": 1048576,
      "worker_slots": 1024,
      "tcp_write_max_bytes": 262144,
      "tcp_write_burst_bytes": 4194304
    }
  },
  "trace": {
    "enabled": false,
    "interval_sec": 10,
    "connect_on_start": false
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

      // Optional per-peer overrides.
      "proto_connections": 16,
      "proto_reconnect_interval_ms": 3000,
      "compress": "off",
      "enabled": true
    }
  ]
}
```

## Server Example

```jsonc
{
  "tls": {
    // Server certificate, private key, and CA certificate used by MsQuic TLS.
    "cert": "certs/server.crt",
    "key": "certs/server.key",
    "ca": "certs/ca.crt"
  },
  "admin": {
    // Optional admin HTTP endpoint for /health and /metrics.
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
    "disable_1rtt_encryption": false,
    "fcw": 1073741824,
    "srw": 1073741824,
    "iw": 4000,
    "initrtt_ms": 1
  },
  "compression": {
    "mode": "off",
    "level": 1
  },
  "tuning": {
    "mode": "custom",
    "target_bandwidth_mbps": 100000,
    "target_rtt_ms": 1,
    "max_memory_mb": 4096
  },
  "relay": {
    "io_size": 1048576,
    "inflight_bytes": 1073741824,
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
| `tls.cert` | client/server | TLS certificate PEM path. Required. |
| `tls.key` | client/server | TLS private key PEM path. Required. |
| `tls.ca` | client/server | CA certificate PEM path. Required. |
| `admin.listen` | client/server | Admin HTTP listen address. |
| `proto.profile` | client/server | `max-throughput` or `low-latency`. |
| `proto.disable_1rtt_encryption` | client/server | Insecure lab-only QUIC 1-RTT encryption disable switch. |
| `proto.connections` | client | Default QUIC connection count inherited by peers. |
| `proto.reconnect_interval_ms` | client | Default reconnect interval inherited by peers, 1000..60000 ms. |
| `proto.fcw` | client/server | QUIC connection flow-control window override. |
| `proto.srw` | client/server | QUIC stream receive window override. |
| `proto.iw` | client/server | QUIC initial congestion window in packets. |
| `proto.initrtt_ms` | client/server | QUIC initial RTT estimate. |
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
| `tuning.mode` | client/server | `auto`, `lan`, `wan`, or `custom`. |
| `tuning.target_bandwidth_mbps` | client/server | Target bandwidth for BDP-based tuning. |
| `tuning.target_rtt_ms` | client/server | Target RTT for BDP-based tuning. |
| `tuning.max_memory_mb` | client/server | Relay memory pool budget. |
| `relay.io_size` | client/server | Relay IO chunk size override. |
| `relay.inflight_bytes` | client/server | Target relay in-flight bytes. |
| `relay.linux.read_chunk_size` | client/server | Linux relay TCP read chunk size. |
| `relay.linux.worker_slots` | client/server | Linux relay worker buffer slots per tunnel. |
| `relay.linux.tcp_write_max_bytes` | client/server | Cap bytes per Linux relay TCP `sendmsg`; omit when not overriding. |
| `relay.linux.tcp_write_burst_bytes` | client/server | Cap bytes per TCP write flush burst; omit when not overriding. |
| `trace.enabled` | client/server | Enable trace/debug file logging. |
| `trace.interval_sec` | client/server | Periodic trace interval in seconds. |
| `trace.connect_on_start` | client | Debug option to connect protocol sessions at startup. |
| `peers[].id` | client | Stable peer id. Required. |
| `peers[].proto_peer` | client | Remote protocol endpoint. Required. |
| `peers[].socks_listen` | client | SOCKS5 listener for this peer. Required. |
| `peers[].http_listen` | client | Optional HTTP CONNECT listener for this peer. |
| `peers[].proto_connections` | client | Optional per-peer connection count override. |
| `peers[].proto_reconnect_interval_ms` | client | Optional per-peer reconnect interval override. |
| `peers[].compress` | client | Optional per-peer compression override. |
| `peers[].enabled` | client | Whether the peer starts active. |

## Notes

- Use `proto`, not `quic`, in the config schema. Internal code may still use QUIC names because the current protocol backend is QUIC.
- Client configs should use `peers` even for a single remote server.
- Use `tuning.mode: "custom"` when relying on explicit override keys such as `relay.linux.worker_slots`, `proto.fcw`, or `proto.initrtt_ms`.
- Built-in speed-test options require exactly one enabled peer.
- `proto.disable_1rtt_encryption` is insecure and should be used only in isolated lab measurements.
