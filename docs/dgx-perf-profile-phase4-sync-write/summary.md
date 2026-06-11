# Phase 4 callback sync-write DGX 验证（2026-06-12）

分支：`phase4-callback-sync-write`（`TryCompleteQuicReceiveInline` + ReceiveComplete 语义修复）。

## 修复

同步 inline 写满 payload 时仅返回 `QUIC_STATUS_SUCCESS`，**不再**调用 `StreamReceiveComplete`（MsQuic 同步路径已隐式推进流控；二者并用会导致 ~12KB 后卡死）。

## 吞吐（proxy-1x1，30s，无时延）

| 方案 | 裸 TCP | proxy-1x1 | proxy / bare |
|------|--------|-----------|--------------|
| Phase 3 Rerun（文档） | 28.18 Gbps | **11.36 Gbps** | **40.3%** |
| Phase 4 always-pending（Task 10.1） | 18.52 Gbps | 5.23 Gbps | 28.2% |
| Phase 4 always-pending（同晚 #2） | 21.99 Gbps | 4.94 Gbps | 22.5% |
| **Phase 4 sync-write（修复后 v3）** | **24.63 Gbps** | **7.68 Gbps** | **31.2%** |

同轮 sync-write v3 多流：proxy-16×16 **3.40 Gbps**，proxy-4×16 **2.38 Gbps**（低于 always-pending 同晚 10.75 / 12.41 Gbps）。

## 结论

- 修复后单流 **7.68 Gbps**，高于 always-pending 同会话 **4.94 Gbps**（+55%），但仍低于 Phase 3 Rerun **11.36 Gbps**。
- 多流场景 sync-write 明显弱于 always-pending，需后续排查 callback 线程阻塞或 receive 恢复时序。
- 坏 run（未修复或 QUIC 未建连）仍可出现 ~0 Gbps；bench 以 HTTP CONNECT listening 就绪为准。

报告：`/tmp/dgx-bench-phase4-syncwrite-v3.md`；对比 pending：`/tmp/dgx-bench-phase4-pending-v2.md`。
