# Phase 3 vs Phase 4 双方案 DGX 吞吐对比（2026-06-12）

同一晚连续三轮 `DURATION_SEC=30` 的 `dgx-msquic-vs-proxy-bench.sh`，节点 169.254.250.230 ↔ 169.254.59.196（`enp1s0f0np0`，无时延 LAN），共用 `build/msquic/bin/Release` 的 libmsquic / secnetperf。

| 代码基线 | 分支 / commit | 说明 |
|----------|---------------|------|
| **Phase 3** | `phase3-event-queue-baseline` @ `18f5f58` | MPMC 事件队列，receive 仍 copy 到 ingress buffer |
| **Phase 4 A** | `master` @ `efcedd6` | deferred receive view + always `QUIC_STATUS_PENDING` + `StreamMultiReceiveEnabled` |
| **Phase 4 B** | `phase4-callback-sync-write` @ `115eb06` | callback 内 `writev` fast path；全量写出返回 `QUIC_STATUS_SUCCESS`（不 double-complete） |

## 吞吐对比表

| 指标 | Phase 3 baseline | Phase 4 always-pending | Phase 4 sync-write |
|------|------------------|------------------------|---------------------|
| 裸 TCP | **22.47 Gbps** | 18.88 Gbps | 19.28 Gbps |
| proxy-1×1 | **14.09 Gbps** | 4.37 Gbps | 4.83 Gbps |
| proxy / 裸 TCP | **62.7%** | 23.2% | 25.1% |
| proxy-16×16 | 8.25 Gbps | **16.60 Gbps** | 2.51 Gbps |
| proxy-4×16 | 10.18 Gbps | **23.18 Gbps** | 4.87 Gbps |

原始报告：

- Phase 3：`docs/dgx-perf-profile-phase3-baseline-20260612/report.md`
- Phase 4 A：`docs/dgx-perf-profile-phase4-comparison-20260612/report-always-pending.md`
- Phase 4 B：`docs/dgx-perf-profile-phase4-comparison-20260612/report-sync-write.md`

## 结论

1. **单流**：Phase 3 专用基线仍明显领先两种 Phase 4（~14 Gbps vs ~4–5 Gbps），归一化比值 Phase 3 **63%** vs Phase 4 **23–25%**。与早前 Phase 3 Rerun（11.36 Gbps）相比，本轮裸 TCP 与 proxy-1×1 均更高，说明链路状态较好；但 **相对排序不变**——Phase 4 单流回退是结构性的，不能仅用裸 TCP 波动解释。
2. **多流**：always-pending 在本轮 **大幅领先** Phase 3（16×16 **16.6** vs **8.2** Gbps；4×16 **23.2** vs **10.2** Gbps），与 Phase 4 零拷贝 + multi-receive 的设计一致。
3. **sync-write**：修复 double-complete 后功能正常，单流与 always-pending 同量级（4.83 vs 4.37 Gbps），但 **多流严重回退**（2.5–4.9 Gbps），推测 callback 内同步 `writev` 在 MsQuic 线程上阻塞，限制并发 receive/relay。不宜作为纯替换 always-pending 的方案；更适合作为 **单流 fast path + partial/EAGAIN 才 pending** 的混合设计中的一条路径。
4. **推荐方向**：保留 Phase 3 单流路径优势的同时，选择性引入 Phase 4 零拷贝；多流场景优先 always-pending / multi-receive 模型，sync-write 仅用于 TCP 可立即收完的快速路径，并需进一步排查多流 callback 阻塞。
