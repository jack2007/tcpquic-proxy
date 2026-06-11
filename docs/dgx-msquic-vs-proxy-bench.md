# msquic secnetperf vs tcpquic-proxy（无时延 LAN 对比）

- 时间: 2026-06-11
- 本机: 169.254.250.230 (`enp1s0f0np0`)
- 对端: 169.254.59.196 (`enp1s0f0np0`)
- msquic 工具: `third_party/msquic` 自带 **secnetperf**（与 tcpquic-proxy 共用同一套 `libmsquic`）
- 构建: `cmake -B build -DQUIC_BUILD_PERF=ON && cmake --build build --target secnetperf`
- 复现脚本: `scripts/dgx-msquic-vs-proxy-bench.sh`

## 测试方法

### secnetperf（msquic 官方）

```bash
# 对端 server（注意：有 -bind 时 -port 无效，必须写成 ip:port）
LD_LIBRARY_PATH=~/tcpquic-dgx-bin ~/tcpquic-dgx-bin/secnetperf \
  -exec:maxtput -bind:169.254.59.196:4434

# 本机 client
LD_LIBRARY_PATH=build/msquic/bin/Release build/msquic/bin/Release/secnetperf \
  -target:169.254.59.196 -bind:169.254.250.230 -port:4434 \
  -exec:maxtput -conns:1 -down:15s -ptput:1
```

输出示例：`Result: Download 19324116 kbps.` → **Mbps = kbps ÷ 1000**（≈ 19.3 Gbps）。

### tcpquic-proxy

同矩阵 20Gbps preset：HTTP CONNECT + QUIC + 对端 relay → busybox httpd。

## 实测数据（无时延，多轮采样）

| 层级 | 场景 | 吞吐 (Gbps) | 备注 |
|------|------|-------------|------|
| 裸 TCP | curl → busybox | **17–28** | 链路/HTTP 基线上限 |
| **msquic** | secnetperf 单连接 `-conns:1` | **0.8–10**（典型 **6–10**） | 纯 QUIC 端到端，无 HTTP/relay |
| **msquic** | secnetperf 16 连接 | **42–49** | `Result:` 汇总行 |
| **msquic** | secnetperf 1 连接 16 stream | **7–9** | 多 stream 单连接 |
| tcpquic-proxy | 单 curl + 1 QUIC | **5.8–9.4** | 与 msquic 单连接同量级 |
| tcpquic-proxy | 4 curl + 16 QUIC | **22–28** | 接近裸 TCP |

本机 loopback 对照：secnetperf 单连接约 **19 Gbps**（`Result: Download 19324116 kbps`），说明库本身在理想条件下可达极高单流吞吐。

## 结论（回答 Step2 疑问）

原先「Step2 只有 Step1 的 1/6」对比的是：

```
裸 TCP (curl)  ≈ 19 Gbps
tcpquic-proxy  ≈  3.5 Gbps   （早期矩阵，relay pending 被 16MB 截断 + 单连接）
```

若改为与 **msquic 基线** 对比（同无 netem LAN、单 QUIC 连接）：

```
msquic secnetperf 单连接  ≈  6–10 Gbps
tcpquic-proxy 单流        ≈  5.8–6.2 Gbps（修复 pending 后）
```

**单流 proxy 约为 msquic 单连接的 60%–100%**（轮次间有抖动），而不是 msquic 的 1/6。

损耗分层：

1. **裸 TCP → msquic 单连接**：约 **2–3×**（QUIC/TLS/用户态栈，secnetperf 实测）
2. **msquic 单连接 → tcpquic-proxy 单流**：约 **0–40%** 额外损耗（HTTP CONNECT + relay + 对端 TCP 读 busybox）
3. **早期矩阵 3.5 Gbps**：主要是 **relay `LinuxRelayPerTunnelPendingBytes` 被重置为 16MB** + 测量方式（单 curl 短载荷）叠加，不是 msquic 库上限

## 多连接行为

- msquic：16 连接 secnetperf 汇总 **~44 Gbps**
- proxy：4 curl + 16 QUIC **~24 Gbps**，已接近裸 TCP 单路 curl 上限

两者都说明：**单连接不是 200G 链路的满速形态**；要逼近线速需多连接/多 stream。

## 工具与坑

1. secnetperf server 必须 `-bind:IP:PORT`，单独 `-port` 在带 bind 时被忽略（默认落到 4433）
2. 解析吞吐：`Result: Download N kbps` → **Gbps = N / 1e6**
3. 勿将 curl `speed_download`（bytes/s）与 secnetperf `kbps` 混用同一换算公式
4. 对端 `libmsquic.so.2` 符号链接需指向实际 `.so` 文件

## 相关文件

- `third_party/msquic/src/perf/readme.md` — secnetperf 用法
- `docs/dgx-perf-profile-analysis.md` — perf 热点分析与改进建议
- `docs/dgx-throughput-matrix-20260611-133305.md` — 五步矩阵 + Step2 分析
- `/tmp/dgx-msquic-vs-proxy-20260611-135130.md` — 首轮完整对比（kbps 换算已修正）
