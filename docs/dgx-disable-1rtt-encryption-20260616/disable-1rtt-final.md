# msquic secnetperf vs tcpquic-proxy（无时延 LAN）

- 时间: 2026-06-16T09:58:11+08:00
- 本机: 169.254.250.230 | 对端: 169.254.59.196
- secnetperf: `-exec:maxtput -down:30s`（server bind 为 `ip:port`）
- 同一套 libmsquic: /home/jack/src/tcpquic-proxy/build/msquic/bin/Release
- tcpquic-proxy extra args: `--quic-disable-1rtt-encryption`

## 参考：裸 TCP (curl → busybox)
- **37111.24 Mbps** (37.111 Gbps)

## 对比表

| 工具 | 场景 | conns/streams 或 quic×curl | Mbps | Gbps | vs msquic 单连接 |
|------|------|---------------------------|------|------|------------------|
| secnetperf | 单连接下载 | conns=1 | 19615.82 | 19.616 | 100% |
| secnetperf | 16 连接下载 | conns=16 | 82772.36 | 82.772 | — |
| secnetperf | 单连接 16 stream | conns=1 streams=16 | 17693.45 | 17.693 | — |
| tcpquic-proxy | HTTP CONNECT 单流 | quic=1 curl=1 | 21309.59 | 21.310 | 108.6% |
| tcpquic-proxy | 多流 | quic=16 curl=16 | 72326.34 | 72.326 | — |
| tcpquic-proxy | 多流（最佳） | quic=16 curl=4 | 82853.02 | 82.853 | — |

## 说明

- **secnetperf**：msquic 官方吞吐工具，端到端纯 QUIC（无 HTTP CONNECT / 无 TCP relay）。
- **tcpquic-proxy**：在 QUIC 之上叠加 HTTP CONNECT + 对端 TCP relay → busybox。
- 若 proxy 单流仅为 msquic 单连接的几分之一，瓶颈在代理栈而非 msquic 库本身。

报告: /home/jack/src/tcpquic-proxy/docs/dgx-disable-1rtt-encryption-20260616/disable-1rtt-final.md
