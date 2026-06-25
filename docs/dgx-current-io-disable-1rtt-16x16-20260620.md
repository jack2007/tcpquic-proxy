# msquic secnetperf vs tcpquic-proxy（无时延 LAN）

- 时间: 2026-06-20T22:54:52+08:00
- 本机: 169.254.250.230 | 对端: 169.254.59.196
- secnetperf: `-exec:maxtput -down:15s`（server bind 为 `ip:port`）
- 同一套 libmsquic: /home/jack/src/tcpquic-proxy/build/msquic/bin/Release
- tcpquic-proxy extra args: `--quic-disable-1rtt-encryption`

## 参考：裸 TCP (curl → busybox)
- **35231.71 Mbps** (35.232 Gbps)

## 对比表

| 工具 | 场景 | conns/streams 或 quic×curl | Mbps | Gbps | vs msquic 单连接 |
|------|------|---------------------------|------|------|------------------|
