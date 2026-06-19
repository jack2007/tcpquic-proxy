# DGX 2机 IO 吞吐验证

- Date: 2026-06-19
- Topology: client `169.254.250.230` -> server `169.254.59.196`
- Mode: 200G direct, no netem, HTTP CONNECT -> busybox httpd
- Binary: `build-regression/bin/Release/tcpquic-proxy`
- Final report: `docs/dgx-current-io-throughput-20260619/report-final2.md`

## 当前结果

| 场景 | 当前 Gbps | 备注 |
|------|-----------|------|
| direct TCP curl | 34.586 | busybox HTTP 源站基线 |
| proxy 1x1 | 20.489 | quic=1 curl=1 |
| proxy 4x4 | 26.982 | quic=4 curl=4 |
| proxy 16x4 | 26.465 | quic=16 curl=4 |

补充复测观察到短样本波动：

| 报告 | 场景 | Gbps |
|------|------|------|
| `report-final.md` | proxy 1x1 | 18.899 |
| `report-final.md` | proxy 4x4 | 37.182 |
| `report-final.md` | proxy 16x4 | 31.219 |
| `report-16x4-percurl.md` | proxy 16x4 | 30.753 |

## 历史基线对比

| 场景 | 当前 Gbps | 历史基线 | 差异 |
|------|-----------|----------|------|
| proxy 1x1 | 20.489 | 16.989 (`docs/dgx-mimalloc-on-demand-buffer-20260616/bench-1x1-report.md`) | +20.6% |
| proxy 1x1 | 20.489 | 16.862 (`docs/dgx-disable-1rtt-encryption-20260616/default-final.md`) | +21.5% |
| proxy 4x4 | 26.982 | 未找到精确历史 4x4 基线 | n/a |
| proxy 16x4 | 26.465 | 55.040 (`docs/dgx-disable-1rtt-encryption-20260616/default-final.md`) | -51.9% |
| proxy 16x4 | 26.465 | 70.288 (`docs/dgx-mimalloc-on-demand-buffer-20260616/bench-1x1-report.md`) | -62.3% |
| proxy 16x4 | 26.465 | 28.571 (`docs/dgx-phase4-comparison-20260612-120356/phase4-always-pending/bench-report.md`) | -7.4% |

## 已处理的问题

本次最初验证中，`proxy-16x4` 无法启动到吞吐阶段。根因有两个：

1. MsQuic `ConnectionStart` 返回 `QUIC_STATUS_PENDING` 表示异步启动已接受，当前代码误按失败处理，导致 slot 被 reset/retry。
2. `StartSlot()` 持有 `ClientSharedState::Lock` 调用 `MsQuicConnection` 构造。多连接启动时早到的 CONNECTED 回调也需要同一把锁，16 路场景可卡在后续 slot 的 `ConnectionOpen`。

修复：

- 将 `QUIC_STATUS_PENDING` 视为 `ConnectionStart` accepted，并新增 reconnect 单测覆盖。
- 将 `MsQuicConnection` 创建移到 session 状态锁外，只在安装 slot 状态时短暂持锁。
- connection-state callback 使用回调传入的 `connectedCount` 应用 ingress 开关，避免回调路径里重入查询 session 状态。
- bench 脚本支持 `PROXY_CASES`，支持跳过 secnetperf，支持等待连接池建满，并打印多 curl 每路吞吐。

## 结论

- 1x1 当前结果高于 2026-06-16 默认历史基线。
- 4x4 无精确历史基线，本次同轮为 26.982 Gbps，另一次复测到 37.182 Gbps。
- 16x4 功能性启动问题已修复，但当前吞吐仍低于 2026-06-16 的高点基线。短样本每路 curl 分布不均，后续若继续追 16x4 高点，应使用更长/更大 payload 的稳定流量源并结合 per-connection/CPU 采样定位。
