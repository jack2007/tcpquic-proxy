# DGX perf 采样摘要（proxy-1x1）

- 时间: 2026-06-11T14:43+08:00
- 场景: tcpquic-proxy 单 QUIC + 循环 curl 下载
- 采样: 25s @ 999Hz，`perf record -g --call-graph dwarf`
- 本机 client: 169.254.250.230 | 对端 server: 169.254.59.196
- **完整分析**: [dgx-perf-profile-analysis.md](../dgx-perf-profile-analysis.md)

## 吞吐

| 角色 | 数值 |
|------|------|
| proxy client | ~8.5 Gbps（`speed_download=105880337` B/s） |
| secnetperf 对照 | ~7.7 Gbps（`773156 kbps`） |

## Client 热点 Top（leaf，212168 样本）

| 占比 | 符号 |
|------|------|
| 19.0% | `__aarch64_swp4_rel`（mutex） |
| 13.7% | `__aarch64_cas4_acq`（mutex） |
| 7.0% | `TqLinuxRelayBufferPool::Acquire()` |
| 3.9% | `_int_malloc` |
| 3.2% | `TqLinuxRelayWorker::DrainTcpReadable` |
| 2.1% | `aes_gcm_dec_256_kernel` |

## Server 热点 Top（leaf，~196K 样本）

| 占比 | 符号 |
|------|------|
| 19.2% | `__aarch64_swp4_rel`（mutex） |
| 13.7% | `__aarch64_cas4_acq`（mutex） |
| 6.9% | `TqLinuxRelayBufferPool::Acquire()` |
| 6.0% | `CxPlatHashtableEnumerateNext`（msquic） |
| 5.2% | `_int_malloc` |
| 3.3% | `DrainTcpReadable` |
| 0.75% | `aes_gcm_enc_256_kernel` |

## 结论（一句话）

proxy 相对 secnetperf 多消耗约 **40% CPU 在 relay buffer 池 mutex + malloc**；优先做 buffer 池无锁化/预分配。

## 原始文件

| 文件 | 说明 |
|------|------|
| `client.perf.data` | 本机 client 采样（~400MB） |
| `server.perf.data` | 对端 server 采样（~3.2GB） |
| `client.top.txt` | client `perf report` 摘录 |
| `server.top.txt` | server `perf report` 摘录 |
| `throughput.txt` | curl 速度参考 |

复现: `CASE=proxy-1x1 DURATION_SEC=25 ./scripts/dgx-perf-profile.sh`
