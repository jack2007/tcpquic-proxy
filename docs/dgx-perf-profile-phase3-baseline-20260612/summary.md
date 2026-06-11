# Phase 3 event-queue baseline — DGX 吞吐（2026-06-12）

- 分支：`phase3-event-queue-baseline` @ `18f5f58`
- worktree：`.worktrees/phase3-event-queue-baseline`
- 工具：`DURATION_SEC=30 ./scripts/dgx-msquic-vs-proxy-bench.sh`

| 指标 | 值 |
|------|-----|
| 裸 TCP | 22.47 Gbps |
| proxy-1×1 | **14.09 Gbps** |
| proxy / 裸 TCP | **62.7%** |
| proxy-16×16 | 8.25 Gbps |
| proxy-4×16 | 10.18 Gbps |

完整对比见 `docs/dgx-perf-profile-phase4-comparison-20260612/summary.md`（Task 11）。
