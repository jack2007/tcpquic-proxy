# DGX 双机五步吞吐排查

- 时间: 2026-06-11T13:33:08+08:00
- 本机: 169.254.250.230 (enp1s0f0np0)
- 对端: 169.254.59.196 (enp1s0f0np0)
- HTTP 源站: busybox httpd
- 下载时长: 30s / 步
- netem: 仅对端 enp1s0f0np0, limit=1000000
- 代理调优: 20Gbps preset（单 QUIC 连接）

## 五步矩阵结果

| Step | 场景 | 模式 | netem delay | netem loss | ping RTT(ms) | ping loss | 吞吐(Mbps) | 吞吐(Gbps) | curl exit |
|------|------|------|-------------|------------|--------------|-----------|------------|------------|-----------|
| 1 | 无 netem 裸 curl TCP 基线 | direct | none | 0% | 1.155 | 0 | 19339.73 | 19.340 | 0 |
| 2 | 无 netem tcpquic-proxy | proxy | none | 0% | 1.281 | 0 | 3468.88 | 3.469 | 0 |

<!-- step2 tuning: tcpquic-proxy tuning: srw=1073741824 fcw=1073741824 iw=4000 initrtt=200ms relay_io=1048576 ideal_send=1073741824 inflight=1024 tcp_buf=67108864 -->

| 3 | netem 100ms 无丢包 | proxy | 100ms | 0% | 101.876 | 0 | 61.66 | 0.062 | 0 |

<!-- step3 tuning: tcpquic-proxy tuning: srw=1073741824 fcw=1073741824 iw=4000 initrtt=200ms relay_io=1048576 ideal_send=1073741824 inflight=1024 tcp_buf=67108864 -->

| 4 | netem 100ms + 5%丢包 | proxy | 100ms | 5% | 101.852 | 10 | 103.30 | 0.103 | 0 |

<!-- step4 tuning: tcpquic-proxy tuning: srw=1073741824 fcw=1073741824 iw=4000 initrtt=200ms relay_io=1048576 ideal_send=1073741824 inflight=1024 tcp_buf=67108864 -->

| 5 | netem 200ms + 5%丢包 | proxy | 200ms | 5% | 201.802 | 0 | 39.64 | 0.040 | 0 |

<!-- step5 tuning: tcpquic-proxy tuning: srw=1073741824 fcw=1073741824 iw=4000 initrtt=200ms relay_io=1048576 ideal_send=1073741824 inflight=1024 tcp_buf=67108864 -->

## 判定说明

- **Step1 ≥ 5Gbps**：裸 curl + busybox HTTP 链路可达极高吞吐，测试工具与 HTTP 源站不是瓶颈。
- **Step2 vs Step1**：在无 netem、RTT ~1ms 的 LAN 条件下，代理路径应接近基线；实测仅约 **1/6（19.3 → 3.5 Gbps）**，属异常，需优先排查 Step2，再解读 Step3–5。
- **Step3/4/5**：在 Step2 基线偏低的前提下，WAN 退化曲线参考价值有限；Step2 修复后应重跑全矩阵。

## Step2 异常分析（用户视角）

### 现象

| 指标 | Step1 | Step2 | 比值 |
|------|-------|-------|------|
| 吞吐 | 19.34 Gbps | 3.47 Gbps | **~5.6×** |
| ping RTT | 1.16 ms | 1.28 ms | 正常 |
| netem | 无 | 无 | 同条件 |

在无丢包、亚毫秒级 RTT 的 DGX 直连场景下，tcpquic-proxy 仅承担「HTTP CONNECT + QUIC 隧道 + 对端 relay 到 busybox」的转发，**不应**比裸 TCP 低一个数量级。Step3–5 的 ~40–100 Mbps 更可能是 Step2 管道过窄 + netem 叠加，而非单纯 BDP/丢包问题。

### 已排除或次要因素

1. **路由 / netem 未生效**：Step2 无 netem；ping RTT ~1ms 说明未走错网卡（测试前已加主机路由，见下）。
2. **HTTP 源站瓶颈**：Step1 同 busybox 已达 19Gbps，源站不是瓶颈。
3. **`inflight=1024` 误解**：日志中的 inflight 为 `RelayMaxInFlightSends`（1MiB 块数），非字节窗口；QUIC fcw/srw 已为 1GiB 量级。
4. **`--quic-initrtt-ms 200`**：在 ~1ms RTT 的 LAN 上可能让 BBR 启动偏保守，**不足以单独解释 5.6× 差距**，但 Step2 对照实验应改用 `initrtt-ms 1~10`。

### 疑似根因（代码，已定位）

`TqComputeTuning()` 中 **Custom 高 BDP 调优被后续步骤覆盖**：

```text
ApplyCustomOverrides()     → ApplyHighBdpPipelineScaling 将 LinuxRelayPerTunnelPendingBytes 提到 512MB
TqApplyLinuxRelayDefaults() → 将 PerTunnelPendingBytes 重置为最大 16MB、ByteBudgetPerTick 为 64MB
```

Linux relay worker 的 per-tunnel pending 与每 tick 字节预算被压回 WAN 默认值，**单连接 LAN 大吞吐管道在 relay 层被截断**。这与 Step2「QUIC 窗口很大但 relay 只敢缓冲 16MB」的现象一致。

### 修复（2026-06-11 后续）

- 将 `ApplyHighBdpPipelineScaling()` 移到 `TqApplyLinuxRelayDefaults()` **之后**（仅 Custom 模式）。
- 高 BDP 下同步放大 `ReadBatchBytes` / `QuicRecvBatchBytes` / `MaxIov` / `ByteBudgetPerTick`（最高 512MB/tick）。

修复后需 **重跑 Step1 + Step2** 验证；若仍远低于基线，继续分解数据路径（见下）。

### Step2 排查计划（优先级）

1. **验证 tuning 生效**：启动代理后确认日志/调试输出中 `LinuxRelayPerTunnelPendingBytes` 为 512MB 量级，而非 16MB。
2. **initrtt 对照**：Step2 分别跑 `--quic-initrtt-ms 1` 与 `200`（其余参数不变）。
3. **CPU 与 relay**：30s 下载期间观察 client/server `tcpquic-proxy` CPU；若有 admin `/metrics`，记录 relay 阻塞/队列指标。
4. **路径分解**：
   - 客户端：curl → HTTP CONNECT → QUIC 收包写 TCP
   - 服务端：QUIC 收包 → relay → TCP → busybox
5. **多连接**：若单 QUIC 仍偏低，测 `--quic-connections N` + N 路 curl（无 netem）能否逼近 Step1。

## 环境注意事项

两台 DGX 各有两条 169.254 链路（`enp1s0f0np0` 与 `enP2p1s0f0np0`），默认路由可能抢走流量，导致 netem 加在 `enp1s0f0np0` 却不生效。测试前强制主机路由：

```bash
sudo ip route replace 169.254.59.196 dev enp1s0f0np0 src 169.254.250.230
ssh jack@169.254.59.196 "sudo ip route replace 169.254.250.230 dev enp1s0f0np0 src 169.254.59.196"
```

## 相关文件

- `scripts/dgx-throughput-matrix.sh` — 五步矩阵自动化
- `scripts/dgx-interconnect-netem-common.sh` — netem 公共函数
- `docs/20gbps-paramenter.md` — 20Gbps 参数建议
- `src/config/tuning.cpp` — relay pending / BDP 调优（Step2 重点）
- `docs/dgx-perf-profile-analysis.md` — perf 热点分析与改进方案
- `scripts/dgx-perf-profile.sh` — perf 采样脚本

## 修复后复测（2026-06-11T13:41）

修复 `tuning.cpp` 中 `ApplyHighBdpPipelineScaling` 被 `TqApplyLinuxRelayDefaults` 覆盖的问题，并重新部署二进制到对端后复测 Step1+Step2（无 netem，同 20Gbps preset）：

| 轮次 | Step1 (Gbps) | Step2 (Gbps) | Step1/Step2 |
|------|--------------|--------------|-------------|
| 修复前（13:33 矩阵） | 19.340 | 3.469 | **5.6×** |
| 修复后（13:41 复测） | 21.513 | 6.059 | **3.6×** |

Step2 从 3.5 Gbps 提升到 **6.1 Gbps（约 +75%）**，但仍远低于「应接近 Step1」的预期。说明 relay pending 截断是重要因素之一，**不是唯一瓶颈**。

后续 Step2 仍建议：

- 用 `--quic-initrtt-ms 1` 对照（LAN 上 200ms 初始 RTT 可能偏保守）
- 启动日志中确认 `relay_pending=536870912`（512MB）已生效（`TqPrintTuning` 已增加该字段）
- 对端 `~/tcpquic-dgx-bin/libmsquic.so.2` 符号链接需指向实际 `.so`（本次复测曾因断链导致 server 未启动）
- 若单 QUIC 仍不足，测多连接 + 多 curl

## Step2 专项：initrtt + 多连接（2026-06-11T13:43）

无 netem，busybox httpd，`relay_pending=536870912`（512MB）已确认生效。

### Step1 基线

- **29.6 Gbps**（direct curl）

### initrtt 对照（单 curl，`quic-connections=1`）

| initrtt-ms | Gbps | vs Step1 |
|------------|------|----------|
| 1 | 6.10 | 4.85× |
| 10 | 3.49 | 8.50× |
| 200 | 6.32 | 4.68× |

结论：**单连接下 initrtt 不是主因**（1ms 与 200ms 均在 ~6Gbps）；10ms 一次偏低更像测量抖动（512MB 载荷在 20Gbps 下数百 ms 传完）。

### 多 curl × 多 QUIC（`initrtt=1`）

| parallel_curl | quic_connections | 聚合 Gbps | vs Step1 |
|---------------|------------------|-----------|----------|
| 1 | 1 | 7.10 | 4.17× |
| 4 | 4 | 9.02 | 3.28× |
| 8 | 8 | 10.96 | 2.70× |
| 16 | 16 | 12.46 | 2.38× |
| 16 | 4 | 14.27 | 2.07× |
| **4** | **16** | **25.48** | **1.16×** |

结论：**Step2 单流瓶颈主要在单 QUIC 连接 / 单 HTTP CONNECT 隧道**；4 路并行 curl + 16 条 QUIC 连接时聚合吞吐 **25.5 Gbps**，已接近裸 TCP 基线（差距约 16%）。生产或大文件场景应默认多连接或提高 `--quic-connections` 并配合多路客户端并发。

复现脚本: `scripts/dgx-step2-investigate.sh`

## msquic 基线对比（无时延）

详见 **`docs/dgx-msquic-vs-proxy-bench.md`**。要点：

- **secnetperf 单连接**（纯 msquic）：典型 **6–10 Gbps**，非 19 Gbps
- **tcpquic-proxy 单流**（修复 pending 后）：**~6 Gbps**，约为 msquic 单连接的 **60%–100%**，而非 msquic 的 1/6
- Step2 与 Step1 的 **5.6×** 差距，主因是 **QUIC 相对裸 TCP 的单连接损耗（2–3×）** + **早期 relay 16MB 截断**，而非 msquic 库只有 3.5G 能力

## perf 热点分析（2026-06-11）

详见 **`docs/dgx-perf-profile-analysis.md`**（原始数据: `docs/dgx-perf-profile-rerun/`）。

### 采样条件

- 单 QUIC + 循环 curl 下载，25s @ 999Hz
- 同期吞吐: proxy **~8.5 Gbps**，secnetperf **~7.7 Gbps**

### proxy vs secnetperf 开销对比

| 开销类别 | secnetperf | tcpquic-proxy |
|----------|------------|---------------|
| TLS AES-GCM | **~23%** | ~2%（被 relay 开销稀释） |
| Buffer 池 mutex | 无 | **~33%** |
| malloc/free | 少量 | **~7–8%** |
| msquic 哈希表 | 有 | ~6% |
| HTTP/TCP relay | 无 | readv/writev + 额外 memcpy |

### 根因（proxy 独有）

1. `TqLinuxRelayBufferPool` 全局 mutex：MsQuic 回调与 relay worker 跨线程竞争
2. `CopyQuicReceiveToEvent` 额外 memcpy（QUIC → 池 → TCP）
3. `shared_ptr` + 堆分配；`FindRelayIdByStream` / `Enqueue` 路径锁

### 改进优先级

| 优先级 | 方案 | 预期收益 |
|--------|------|----------|
| **P0** | per-relay 无锁 buffer 池 / 预分配 | 25–35% CPU |
| **P1** | QUIC receive → TCP writev 零拷贝 | 5–15% |
| **P2** | Stream 上下文缓存 relayId；SPSC 事件队列 | 3–8% |
| **P3** | msquic 公共开销调参（哈希表/拥塞控制） | 2–6% |

复现: `CASE=proxy-1x1 DURATION_SEC=25 ./scripts/dgx-perf-profile.sh`

## 历史记录

- 原始报告路径: `/tmp/dgx-throughput-matrix-20260611-133305.md`
- 修复后复测: `/tmp/dgx-step2-after-tuning-fix-20260611-134106.md`
- Step2 专项: `/tmp/dgx-step2-investigate-20260611-134354.md`
- 矩阵运行日志: `/tmp/dgx-matrix-run.log`（如存在）
