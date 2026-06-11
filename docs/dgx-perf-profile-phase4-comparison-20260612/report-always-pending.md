# msquic secnetperf vs tcpquic-proxy（无时延 LAN）

- 时间: 2026-06-12T01:20:01+08:00
- 本机: 169.254.250.230 | 对端: 169.254.59.196
- secnetperf: `-exec:maxtput -down:30s`（server bind 为 `ip:port`）
- 同一套 libmsquic: /home/jack/src/tcpquic-proxy/build/msquic/bin/Release

## 参考：裸 TCP (curl → busybox)
- **18874.66 Mbps** (18.875 Gbps)

## 对比表

| 工具 | 场景 | conns/streams 或 quic×curl | Mbps | Gbps | vs msquic 单连接 |
|------|------|---------------------------|------|------|------------------|
| secnetperf | 单连接下载 | conns=1 | 735.02 | 0.735 | 100% |
| secnetperf | 16 连接下载 | conns=16 | 32632.89 | 32.633 | — |
| secnetperf | 单连接 16 stream | conns=1 streams=16 | 1514.89 | 1.515 | — |
| tcpquic-proxy | HTTP CONNECT 单流 | quic=1 curl=1 | 4373.69 | 4.374 | 595.0% |
| tcpquic-proxy | 多流 | quic=16 curl=16 | 16602.18 | 16.602 | — |
| tcpquic-proxy | 多流（最佳） | quic=16 curl=4 | 23178.44 | 23.178 | — |

## 说明

- **secnetperf**：msquic 官方吞吐工具，端到端纯 QUIC（无 HTTP CONNECT / 无 TCP relay）。
- **tcpquic-proxy**：在 QUIC 之上叠加 HTTP CONNECT + 对端 TCP relay → busybox。
- 若 proxy 单流仅为 msquic 单连接的几分之一，瓶颈在代理栈而非 msquic 库本身。

报告: /tmp/dgx-phase4-always-pending-20260612-011956.md
