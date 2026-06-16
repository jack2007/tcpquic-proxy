# msquic secnetperf vs tcpquic-proxy（无时延 LAN）

- 时间: 2026-06-16T09:55:27+08:00
- 本机: 169.254.250.230 | 对端: 169.254.59.196
- secnetperf: `-exec:maxtput -down:30s`（server bind 为 `ip:port`）
- 同一套 libmsquic: /home/jack/src/tcpquic-proxy/build/msquic/bin/Release
- tcpquic-proxy extra args: `none`

## 参考：裸 TCP (curl → busybox)
- **33694.38 Mbps** (33.694 Gbps)

## 对比表

| 工具 | 场景 | conns/streams 或 quic×curl | Mbps | Gbps | vs msquic 单连接 |
|------|------|---------------------------|------|------|------------------|
| secnetperf | 单连接下载 | conns=1 | 16396.19 | 16.396 | 100% |
| secnetperf | 16 连接下载 | conns=16 | 74533.57 | 74.534 | — |
| secnetperf | 单连接 16 stream | conns=1 streams=16 | 14388.69 | 14.389 | — |
| tcpquic-proxy | HTTP CONNECT 单流 | quic=1 curl=1 | 16862.33 | 16.862 | 102.8% |
| tcpquic-proxy | 多流 | quic=16 curl=16 | 48718.37 | 48.718 | — |
| tcpquic-proxy | 多流（最佳） | quic=16 curl=4 | 55040.10 | 55.040 | — |

## 说明

- **secnetperf**：msquic 官方吞吐工具，端到端纯 QUIC（无 HTTP CONNECT / 无 TCP relay）。
- **tcpquic-proxy**：在 QUIC 之上叠加 HTTP CONNECT + 对端 TCP relay → busybox。
- 若 proxy 单流仅为 msquic 单连接的几分之一，瓶颈在代理栈而非 msquic 库本身。

报告: /home/jack/src/tcpquic-proxy/docs/dgx-disable-1rtt-encryption-20260616/default-final.md
