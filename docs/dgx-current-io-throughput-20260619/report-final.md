# msquic secnetperf vs tcpquic-proxy（无时延 LAN）

- 时间: 2026-06-19T17:40:51+08:00
- 本机: 169.254.250.230 | 对端: 169.254.59.196
- secnetperf: `-exec:maxtput -down:30s`（server bind 为 `ip:port`）
- 同一套 libmsquic: /home/jack/src/tcpquic-proxy/build/msquic/bin/Release
- tcpquic-proxy extra args: `none`

## 参考：裸 TCP (curl → busybox)
- **34577.12 Mbps** (34.577 Gbps)

## 对比表

| 工具 | 场景 | conns/streams 或 quic×curl | Mbps | Gbps | vs msquic 单连接 |
|------|------|---------------------------|------|------|------------------|
| tcpquic-proxy | HTTP CONNECT 单流 | quic=1 curl=1 | 18898.75 | 18.899 | n/a |
| tcpquic-proxy | 多流 | quic=4 curl=4 | 37181.88 | 37.182 | — |
| tcpquic-proxy | 多流 | quic=16 curl=4 | 31218.88 | 31.219 | — |

## 说明

- **secnetperf**：msquic 官方吞吐工具，端到端纯 QUIC（无 HTTP CONNECT / 无 TCP relay）。
- **tcpquic-proxy**：在 QUIC 之上叠加 HTTP CONNECT + 对端 TCP relay → busybox。
- 若 proxy 单流仅为 msquic 单连接的几分之一，瓶颈在代理栈而非 msquic 库本身。

报告: /home/jack/src/tcpquic-proxy/docs/dgx-current-io-throughput-20260619/report-final.md
