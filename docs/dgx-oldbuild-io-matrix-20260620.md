# msquic secnetperf vs tcpquic-proxy（无时延 LAN）

- 时间: 2026-06-20T22:50:00+08:00
- 本机: 169.254.250.230 | 对端: 169.254.59.196
- secnetperf: `-exec:maxtput -down:15s`（server bind 为 `ip:port`）
- 同一套 libmsquic: /home/jack/src/tcpquic-proxy/build/msquic/bin/Release
- tcpquic-proxy extra args: `none`

## 参考：裸 TCP (curl → busybox)
- **33643.40 Mbps** (33.643 Gbps)

## 对比表

| 工具 | 场景 | conns/streams 或 quic×curl | Mbps | Gbps | vs msquic 单连接 |
|------|------|---------------------------|------|------|------------------|
| tcpquic-proxy | HTTP CONNECT 单流 | quic=1 curl=1 | 23162.83 | 23.163 | n/a |
| tcpquic-proxy | 多流 | quic=2 curl=1 | 25367.10 | 25.367 | — |
| tcpquic-proxy | 多流 | quic=4 curl=4 | 53687.08 | 53.687 | — |
