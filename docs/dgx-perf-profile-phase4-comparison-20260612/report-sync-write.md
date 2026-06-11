# msquic secnetperf vs tcpquic-proxy（无时延 LAN）

- 时间: 2026-06-12T01:23:30+08:00
- 本机: 169.254.250.230 | 对端: 169.254.59.196
- secnetperf: `-exec:maxtput -down:30s`（server bind 为 `ip:port`）
- 同一套 libmsquic: /home/jack/src/tcpquic-proxy/build/msquic/bin/Release

## 参考：裸 TCP (curl → busybox)
- **19278.62 Mbps** (19.279 Gbps)

## 对比表

| 工具 | 场景 | conns/streams 或 quic×curl | Mbps | Gbps | vs msquic 单连接 |
|------|------|---------------------------|------|------|------------------|
| secnetperf | 单连接下载 | conns=1 | 759.96 | 0.760 | 100% |
| secnetperf | 16 连接下载 | conns=16 | 45148.64 | 45.149 | — |
| secnetperf | 单连接 16 stream | conns=1 streams=16 | 1033.12 | 1.033 | — |
| tcpquic-proxy | HTTP CONNECT 单流 | quic=1 curl=1 | 4834.41 | 4.834 | 636.1% |
| tcpquic-proxy | 多流 | quic=16 curl=16 | 2512.46 | 2.512 | — |
| tcpquic-proxy | 多流（最佳） | quic=16 curl=4 | 4870.62 | 4.871 | — |

## 说明

- **secnetperf**：msquic 官方吞吐工具，端到端纯 QUIC（无 HTTP CONNECT / 无 TCP relay）。
- **tcpquic-proxy**：在 QUIC 之上叠加 HTTP CONNECT + 对端 TCP relay → busybox。
- 若 proxy 单流仅为 msquic 单连接的几分之一，瓶颈在代理栈而非 msquic 库本身。

报告: /tmp/dgx-phase4-sync-write-20260612-012325.md
