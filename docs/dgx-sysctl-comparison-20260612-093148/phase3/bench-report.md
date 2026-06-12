# msquic secnetperf vs tcpquic-proxy（无时延 LAN）

- 时间: 2026-06-12T09:31:53+08:00
- 本机: 169.254.250.230 | 对端: 169.254.59.196
- secnetperf: `-exec:maxtput -down:30s`（server bind 为 `ip:port`）
- 同一套 libmsquic: /home/jack/src/tcpquic-proxy/build/msquic/bin/Release

## 参考：裸 TCP (curl → busybox)
- **19423.69 Mbps** (19.424 Gbps)

## 对比表

| 工具 | 场景 | conns/streams 或 quic×curl | Mbps | Gbps | vs msquic 单连接 |
|------|------|---------------------------|------|------|------------------|
| secnetperf | 单连接下载 | conns=1 | 18906.54 | 18.907 | 100% |
| secnetperf | 16 连接下载 | conns=16 | 64534.62 | 64.535 | — |
| secnetperf | 单连接 16 stream | conns=1 streams=16 | 10928.95 | 10.929 | — |
| tcpquic-proxy | HTTP CONNECT 单流 | quic=1 curl=1 | 11767.86 | 11.768 | 62.2% |
| tcpquic-proxy | 多流 | quic=16 curl=16 | 6886.86 | 6.887 | — |
| tcpquic-proxy | 多流（最佳） | quic=16 curl=4 | 7177.95 | 7.178 | — |

## 说明

- **secnetperf**：msquic 官方吞吐工具，端到端纯 QUIC（无 HTTP CONNECT / 无 TCP relay）。
- **tcpquic-proxy**：在 QUIC 之上叠加 HTTP CONNECT + 对端 TCP relay → busybox。
- 若 proxy 单流仅为 msquic 单连接的几分之一，瓶颈在代理栈而非 msquic 库本身。

报告: /home/jack/src/tcpquic-proxy/docs/dgx-sysctl-comparison-20260612-093148/phase3/bench-report.md
