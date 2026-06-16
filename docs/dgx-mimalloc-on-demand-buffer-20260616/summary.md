# DGX 2×机 proxy-1×1 对比（mimalloc + 按需 buffer）

- Date: 2026-06-16
- Binary: `build/bin/Release/tcpquic-proxy`（Phase 1：mimalloc 静态链入 + 按需 relay buffer，无 worker_slots Reserve）
- Topology: client `169.254.250.230` → server `169.254.59.196`（spark 200G 直连，无 netem）
- Script: `./scripts/dgx-msquic-vs-proxy-bench.sh`
- Duration: 30s / case
- Tuning: `--tuning custom` 高 BDP（fcw/srw=1GiB, iw=4000, initrtt=1ms, relay-io=1MiB, compress off）
- Scenario: **proxy-1×1** = `--quic-connections 1` + 单路 curl HTTP CONNECT 下载

## 本次 1×1 结果（4 次测量）

| Run | Gbps | Mbps |
|-----|------|------|
| bench report（含全表） | 16.989 | 16988.79 |
| rerun #1 | 16.400 | 16399.87 |
| rerun #2 | 18.146 | 18146.44 |
| rerun #3 | 17.449 | 17449.22 |
| **Average** | **17.246** | **17246.08** |

完整报告（含 secnetperf / 多流对照）: [bench-1x1-report.md](./bench-1x1-report.md)

## 与历史 1×1 基线对比

| 来源 | 日期 | 说明 | 1×1 Gbps | vs 本次 avg |
|------|------|------|----------|-------------|
| `docs/dgx-disable-1rtt-encryption-20260616/default-final.md` | 2026-06-16 | 改前（默认加密，旧 buffer pool） | **16.862** | **+2.3%** |
| `docs/dgx-linux-unified-pending-zstd-bench-20260614-1x1` tunnel_off | 2026-06-14 | 3-run avg（17.238 / 14.258 / 17.664） | **16.387** | **+5.2%** |
| `docs/dgx-single-stream-max-probe-20260614` | 2026-06-14 | worker_slots 128→1024 后 | 15.557 | +10.9% |
| 本次 mimalloc on-demand | 2026-06-16 | 4-run avg | **17.246** | — |

## 同次运行环境参考（2026-06-16 18:32）

| 场景 | Gbps |
|------|------|
| direct TCP curl | 36.477 |
| secnetperf 1 conn | 18.551 |
| **tcpquic-proxy 1×1** | **16.989** |
| tcpquic-proxy 16×16 | 53.013 |
| tcpquic-proxy 4×16 | 70.288 |

对比 2026-06-16 上午同脚本基线（`default-final.md`）：secnetperf 1 conn 16.396→18.551 Gbps（环境波动），proxy 1×1 16.862→16.989 Gbps（+0.8%）。

## 结论

1. **吞吐未回归**：按需 buffer + mimalloc 后，1×1 四次平均 **17.25 Gbps**，与改前 **16.86 Gbps** 同量级，波动范围内略高（+2.3%）。
2. **仍接近 msquic 单连接上限**：本次 proxy 1×1 / secnetperf 1 conn ≈ **91.6%**（16.989 / 18.551），与历史比例一致。
3. **单次 run 方差 ~±5%**（16.4–18.1 Gbps），评估时应以多次平均为准。
4. **RSS / buffer_bytes_in_use** 未在本脚本中采样；若需验证 idle relay 零 Reserve 收益，需单独跑带 `/metrics` 的多 relay 场景。

## 复现命令

```bash
cmake --build build --target tcpquic-proxy -j$(nproc)
REPORT=docs/dgx-mimalloc-on-demand-buffer-20260616/bench-1x1-report.md \
  DURATION_SEC=30 ./scripts/dgx-msquic-vs-proxy-bench.sh
```
