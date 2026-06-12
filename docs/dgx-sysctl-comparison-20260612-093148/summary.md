# Phase 3 vs Phase 4 — sysctl 调优后 DGX 吞吐 + perf（20260612-093148）

- bench 时长: 30s | perf 采样: 25s @ 999Hz（proxy-1×1 + proxy-4×16）
- 脚本: `./scripts/dgx-phase-sysctl-comparison.sh`
- 节点: 169.254.250.230 ↔ 169.254.59.196（`enp1s0f0np0`，无时延 LAN）

## 内核 socket 参数

两台 DGX 均已设置：

```
net.core.rmem_max / wmem_max = 134217728
net.core.rmem_default / wmem_default = 4194304
net.ipv4.tcp_rmem / tcp_wmem = 4096 1048576 134217728
```

## 吞吐对比

| 指标 | Phase 3 baseline | Phase 4 always-pending | Phase 4 sync-write |
|------|------------------|------------------------|---------------------|
| 裸 TCP | 19.424 Gbps | 21.237 Gbps | **27.933 Gbps** |
| proxy-1×1 | **11.768 Gbps** | 5.747 Gbps | 5.010 Gbps |
| proxy / 裸 TCP | **60.6%** | 27.1% | 17.9% |
| proxy-16×16 | 6.887 Gbps | **15.610 Gbps** | 14.407 Gbps |
| proxy-4×16 | 7.178 Gbps | **24.988 Gbps** | 22.678 Gbps |

原始 bench 报告：

- Phase 3: `phase3/bench-report.md`
- Phase 4 A: `phase4-always-pending/bench-report.md`
- Phase 4 B: `phase4-sync-write/bench-report.md`

## 与 Task 11（sysctl 调优前，2026-06-12）对比

| 指标 | Phase 3（前→后） | always-pending（前→后） | sync-write（前→后） |
|------|------------------|-------------------------|---------------------|
| proxy-1×1 | 14.09 → **11.77** Gbps | 4.37 → **5.75** Gbps | 4.83 → **5.01** Gbps |
| proxy-4×16 | 10.18 → **7.18** Gbps | 23.18 → **24.99** Gbps | 4.87 → **22.68** Gbps |

**要点：**

1. **单流排序不变**：Phase 3 仍约为 Phase 4 的 **2×**（11.8 vs ~5 Gbps）。
2. **sync-write 多流大幅回升**：proxy-4×16 从 **4.9 → 22.7 Gbps**（≈4.7×），说明先前多流回退中 **TCP 发送窗口 / socket buffer 过小** 是重要因素；更大的 `tcp_wmem`/`wmem_max` 让 callback 内 `writev` 更少遇到 `EAGAIN`，MsQuic 线程阻塞减轻。
3. **Phase 3 多流略降**（10.2 → 7.2 Gbps）：可能与裸 TCP 基线波动（22.5 → 19.4 Gbps）及三轮测试顺序有关；相对 Phase 4 多流优势缩小。
4. **always-pending 多流稳定领先**（~25 Gbps），与 Task 11 结论一致。

## Perf 热点（proxy-1×1，client）

| 热点 | Phase 3 | always-pending | sync-write |
|------|---------|----------------|------------|
| `__arch_copy_to_user`（UDP recv） | **21.5%** | 8.6% | — |
| AES-GCM 解密 | 4.6% | **16.6%** | — |
| `AcquireWorker` / `ReleaseWorker` | — | ~3% / ~3% | **11.1% / 10.0%** |
| `DrainTcpReadable` | — | — | **7.5%** |

- **Phase 3 client**：MsQuic UDP 收包 + 内核 copy 占主导；relay 符号未进 client top（relay 工作在对端 server）。
- **always-pending**：加密 + UDP copy 为主；relay 池操作已很低（~3%），零拷贝 receive 路径 CPU 开销小。
- **sync-write**：client 侧 **buffer 池 Acquire/Release + DrainTcpReadable** 合计 ~30%，说明单流时大量 TCP→QUIC 方向工作仍在 worker 线程，且池操作仍是热点；与 always-pending 的 receive 侧零拷贝形成对比。

## Perf 热点（proxy-4×16，client）

| 热点 | Phase 3 | always-pending | sync-write |
|------|---------|----------------|------------|
| `__arch_copy_to_user` | 10.7% | **22.1%** | **23.7%** |
| AES-GCM | 11.5% | — | — |
| `ProcessQuicReceiveEvent` / relay | 可见（多连接注册） | 低 | 低 |

多流高吞吐时，**MsQuic UDP 收包路径的内核 copy** 成为 Phase 4 两方案的共性瓶颈（~22–24%）。Phase 3 采样窗口内 CPU 利用率较低（仅 1K samples），热点分布参考性弱于 Phase 4。

## Perf 热点（proxy-1×1，server）

Phase 3 server：`DrainTcpReadable` **11.1%** + `AcquireWorker` **8.4%** + `readv` 系统调用链 ~10% — TCP relay 读路径仍是 server 侧主成本。

## 目录结构

```
phase3/
  bench-report.md
  perf-proxy-1x1/   client.top.txt server.top.txt
  perf-proxy-4x16/
phase4-always-pending/
  ...
phase4-sync-write/
  ...
```

## 结论与建议

1. **sysctl 调优对 sync-write 多流影响最大**，应作为 DGX bench 的标准前置步骤并写入 `/etc/sysctl.d/` 持久化。
2. **架构选择不变**：单流优先 Phase 3 路径；多流优先 always-pending；sync-write 在调优后可作为 **大 buffer 下的 fast path** 候选，但仍需限制 callback 内写预算。
3. **下一优化方向**：降低 MsQuic UDP `recvmmsg` 内核 copy（GSO/GRO、XDP 或 MsQuic 收包路径）；sync-write 单流继续压 buffer 池热点；always-pending 单流减少 `PENDING → complete` 往返。
