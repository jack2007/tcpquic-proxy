# tcpquic-proxy 大带宽关键调优参数

本文记录 `tcpquic-proxy` 在 200G 直连、100ms RTT、可选 5% loss 等大带宽 / 高 BDP 场景下，对传输速度有明显影响的关键参数、当前代码实现状态与推荐组合。

## 结论速查

`tcpquic-proxy` 的大带宽提速重点不是盲目增加连接数，而是同时调大 QUIC 窗口、拥塞控制、应用层流水线和测试环境队列：

```text
BBR + 500MB 连接流控 + 512MiB Stream 接收窗口 + iw=2000 + initrtt=100ms
+ 1MiB relay IO + 64MiB ideal send + 64 in-flight send + 单 QUIC 连接
+ 正确 netem limit/interface + 足够长的测试载荷或 steady 指标口径
```

当前程序默认 `--tuning wan`，因此不显式传 tuning 参数时已经启用上述 WAN 档位。`--tuning auto` 当前等价于 WAN 档位入口，并保留给后续运行时自适应策略；`wan` / `auto` 会利用运行时 RTT 和 MsQuic ideal-send hint 做保守下调。

## 最有效参数

| 参数 / 方向 | 当前 WAN 默认值 | 作用 | 备注 |
|-------------|-----------------|------|------|
| 拥塞控制 | BBR | 高 RTT / 有丢包时比默认 CUBIC 更适合维持高吞吐 | `TqMakeMsQuicSettings()` 固定设置 `QUIC_CONGESTION_CONTROL_ALGORITHM_BBR` |
| 连接流控窗口 | 500,000,000 bytes | 提供足够连接级在途数据窗口，填充高 BDP 管道 | `ApplyWanDefaults()` 默认；1GB 试验未超过 500MB，普通 WAN 档暂不继续增大 |
| Stream 接收窗口 | 536,870,912 bytes（512MiB） | 避免单流接收窗口限制下载吞吐 | 固定使用验证后的窗口值，不再暴露 `--srw` 覆盖 |
| 初始拥塞窗口 | 2000 packets | 降低启动阶段窗口爬升时间 | 约 2.4MB，按 1200B 包估算 |
| 初始 RTT | 100 ms | 让拥塞控制按 netem 后真实 RTT 收敛 | 运行时可用实测 RTT 覆盖，前提是未显式 `--initrtt-ms` |
| SendBuffering | false | 应用自行管理发送流水线 | 与 secnetperf 高吞吐配置对齐 |
| Pacing | true | 保持平滑发送，避免突发拥塞 | `pacing:0` 未观察到明显收益 |
| StreamMultiReceive | true | 允许 MsQuic 多 receive 语义 | 当前 settings 显式启用 |
| Execution profile | MAX_THROUGHPUT | 使用 MsQuic 高吞吐执行配置 | 默认 `--quic-profile max-throughput`；也支持 `low-latency` |
| relay IO 大小 | 1MiB | 降低 syscall / 分片开销 | WAN 默认 `RelayIoSize` |
| compressed relay IO | 256KiB | 限制压缩路径块大小 | 避免压缩路径过大缓冲 |
| Ideal send buffer | 64MiB | 保证应用层发送队列足够深 | WAN 默认 `RelayDefaultIdealSend` |
| in-flight send | 64 | 保持 TCP→QUIC 转发流水线深度 | WAN 默认 `RelayMaxInFlightSends` / `RelayMaxFreeSendContexts` |
| TCP socket buffer | 4MiB | 提高本地 TCP 后端 / 前端吞吐 | `TqTuneTcpForThroughput()` 设置 NODELAY + SNDBUF/RCVBUF |
| Linux relay pending | 默认不低于 512MiB/隧道，内存预算一半为全局 pending | 约束 Linux relay 队列内存 | 以 relay pending 512MiB 作为高吞吐基线，不再由 `--relay-inflight-bytes` 推导 |

## 当前 tuning profile 实现

程序已实现轻量 tuning profile，而不是只停留在设计建议阶段：

```text
--tuning auto|lan|wan
--relay-io-size <bytes>                  # Windows relay IO buffer size
--linux-relay-read-chunk-size <bytes>    # Linux relay TCP read chunk size
--linux-relay-worker-slots <n>
--iw <packets>
--initrtt-ms <n>
```

| profile | 当前行为 | 适用场景 |
|---------|----------|----------|
| `wan` | 默认 profile；固定使用 512MiB SRW、500MB FCW、iw=2000、initrtt=100ms、1MiB relay IO、64MiB ideal send、64 in-flight | 生产默认、高 RTT / 大 BDP、DGX WAN 压测 |
| `lan` | 16MiB SRW/FCW、iw=256、initrtt=10ms、256KiB relay IO、4MiB ideal send、16 in-flight、1MiB TCP buffer | 低 RTT、本地/LAN、降低每隧道内存占用 |
| `auto` | 当前退回 WAN 默认，并保留运行时 RTT / ideal-send 观测收敛 | 需要后续接入运行时自适应策略，但不希望引入静态 BDP 输入 |

### profile 默认值对照

| 参数 | `wan` 默认 | `lan` 默认 | `auto` 当前行为 |
|------|------------|------------|----------------|
| `StreamRecvWindow` | 512MiB | 16MiB | 同 WAN |
| `ConnFlowControlWindow` | 500MB | 16MiB | 同 WAN |
| `InitialWindowPackets` | 2000 | 256 | 同 WAN，可用实验参数 `--iw` 覆盖 |
| `InitialRttMs` | 100 | 10 | 同 WAN，可用实验参数 `--initrtt-ms` 覆盖 |
| `RelayIoSize` | 1MiB | 256KiB | 同 WAN |
| `RelayDefaultIdealSend` | 64MiB | 4MiB | 同 WAN，运行时可按 MsQuic ideal-send hint 保守下调 |
| `RelayMaxInFlightSends` | 64 | 16 | 同 WAN |
| `TcpSocketBufferBytes` | 4MiB | 1MiB | 同 WAN |

## 是否可以按不同带宽自适应

可以，但当前代码只保留运行时观测层，不再暴露静态目标带宽 / RTT 参数：

1. **运行时保守下调**：`wan/auto` 会记录 RTT 和 MsQuic ideal-send hint；当实测 hint 低于当前 profile 上限时，保守降低 `InitialRttMs`、`RelayDefaultIdealSend` 和 in-flight。`lan` 不启用这层运行时调参。
2. **压缩模式自适应**：`--compress auto` 初始选择 `off`；采样压缩率后，若压缩后大小低于原始大小的 98% 阈值，则选择 `zstd`，否则保持 `off`。

### 哪些参数适合自动调

| 参数 | 当前支持状态 | 原因 |
|------|--------------|------|
| `ConnFlowControlWindow` / `StreamRecvWindowDefault` | 固定为验证后的高吞吐窗口 | 当前测试不支持继续通过命令行覆盖 |
| `InitialRttMs` | 支持运行时实测 RTT 覆盖；`--initrtt-ms` 仅作为实验/诊断参数保留 | 连接后可测量 RTT，当前实验候选值为 100ms |
| `InitialWindowPackets` | `--iw` 仅作为实验/诊断参数保留 | 当前实验候选值为 4000，但测试未证明稳定正向收益 |
| relay IO size | 支持 `--relay-io-size` 覆盖 | 高带宽大块降低 syscall 开销 |
| in-flight sends | profile 固定并受运行时 ideal-send hint 保守下调 | 保持应用层流水线深度，同时受内存预算约束 |
| TCP socket buffer | profile 固定设置 | 保护本地 TCP 侧不成为瓶颈 |
| compression | 支持 `auto` 采样选择 off / zstd | 避免在不可压缩载荷上浪费 CPU |
| QUIC 连接数 | 不按带宽自动调 | 更应按并发 TCP 隧道数、CPU、连接稳定性决定；WAN 单流优先 1 |
| netem limit/interface | 测试脚本支持 | 只影响测试环境，脚本会自动解析对端发送方向 interface |

## 推荐 WAN 压测组合

### 100ms RTT，0% loss，tunnel_off 稳态

推荐使用 steady 口径，让结果更接近 secnetperf `-down:10s`：

```bash
NETEM=1 DELAY=100ms LOSS=0% NETEM_LIMIT=1000000 \
  RATE= METRIC_MODE=steady DURATION_SEC=10 SIZE_MB=128 \
  QUIC_CONNECTIONS=1 MODES="tunnel_off" ITERATIONS=3 \
  ./scripts/bench-tcpquic-proxy-dgx.sh
```

如果需要复用历史 fixed-size 口径，可使用：

```bash
NETEM=1 DELAY=100ms LOSS=0% NETEM_LIMIT=1000000 \
  RATE= QUIC_CONNECTIONS=1 SIZE_MB=128 ITERATIONS=3 MODES="tunnel_off" \
  ./scripts/bench-tcpquic-proxy-dgx.sh
```

注意：当前脚本在 `NETEM=1` 且未设置 `RATE` 时会默认加 `rate 1gbit`；如果要测 200G 直连 + delay/loss 而不限制带宽，需要显式传 `RATE=`。

已验证历史结果：

| 指标 | 结果 |
|------|------|
| secnetperf 基线 | 约 1098 Mbps |
| tunnel_off 32MB | 约 494 Mbps，短载荷，慢启动主导 |
| tunnel_off 128MB | 约 1326–1352 Mbps，稳态，已超过 secnetperf |

### 100ms RTT，5% loss，tunnel_off

推荐仍使用：

```text
QUIC_CONNECTIONS=1
--tuning wan（默认）
BBR
fcw=500MB
srw=512MiB
iw=2000
initrtt=100ms
NETEM_LIMIT=1000000
RATE=   # 如需不限制 netem rate，显式置空
```

关键是 netem 必须施加在下载数据发送端的出站网卡。当前双机下载方向是：

```text
spark-1b6f -> spark-1619
```

脚本默认会在对端执行 `ip route get ${BIND}` 解析出站 dev；本环境解析到的正确网卡是：

```text
enP2p1s0f0np0
```

已验证正确发送端 netem 后：

| 模式 | 平均吞吐 |
|------|----------|
| TCP direct 默认 | 42.06 Mbps |
| TCP direct + BBR + 512MiB 窗口 | 约 0.4–0.5 Gbps |
| QUIC tunnel_off | 8028.41 Mbps |

## 测试环境关键参数

| 参数 | 推荐值 | 原因 |
|------|--------|------|
| netem limit | 1000000 | 默认 1000 会在高 BDP 下造成假性大量丢包 |
| netem 位置 | 数据发送端出站网卡 | 下载测试中发送端是 server 侧，不一定是脚本运行机器 |
| netem rate | 按目标选择；不限速时显式 `RATE=` | 脚本 `NETEM=1` 且未设置 `RATE` 时默认 1gbit |
| 指标口径 | 优先 `METRIC_MODE=steady DURATION_SEC=10` | 避免 32MB/短连接把 BBR 慢启动计入整段平均 |
| 固定载荷 | `SIZE_MB>=128` | fixed-size 口径下提供足够长稳态窗口 |
| 压测模式 | 优先 `tunnel_off` | 先验证纯隧道转发能力，再看压缩收益 |
| QUIC 连接数 | WAN 单流优先 1 | 多连接适合 LAN 摊 CPU，但高 RTT 下会分散窗口、增加收敛成本 |
| 预热 | 短载荷可设置 `WARMUP_MB` | 脚本在 `DURATION_SEC=0` 且 `SIZE_MB<64` 时默认每连接预热 2MB |

## 可压缩载荷

压缩不是传输层参数，但对于可压缩内容会显著提高业务有效吞吐。当前默认 `--compress auto`，初始按 `off` 运行，采样后如果压缩率明显有效（压缩后大小低于原始大小 98%）才切到 `zstd`。

| 场景 | tunnel_off | zstd |
|------|------------|------|
| loopback 64MB 示例 | 5430.73 Mbps | 12943.82 Mbps |

使用建议：

| 模式 | 适用场景 |
|------|----------|
| `--compress off` | 已压缩文件、视频、随机数据、纯网络栈基准 |
| `--compress auto` | 生产默认；自动在不可压缩 / 可压缩内容间选择 |
| `--compress zstd` | 明确知道是文本、JSON、日志、可压缩 HTTP 响应，且追求压缩率 |

## 不建议优先调整的项

| 参数 / 做法 | 结论 | 原因 |
|-------------|------|------|
| `encrypt:0` | WAN 下不优先 | 高 RTT 场景瓶颈主要是 BDP / 窗口，不是加密 CPU |
| `pacing:0` | 不推荐 | 未观察到明显收益，可能增加突发发送风险 |
| 普通 WAN 档 1GB 流控窗口 | 暂不采用 | 500MB 已足够，1GB 未超过 500MB 且可能增加波动；极限实验可用 `custom` |
| 盲目增加 QUIC 连接数 | 不推荐作为 WAN 单流默认 | 高 RTT 下更少连接 + 大窗口更稳定 |
| 32MB 短测作为最终结论 | 不推荐 | 会把 BBR 慢启动成本计入整段平均，低估稳态吞吐 |
| 忽略脚本默认 `RATE=1gbit` | 不推荐 | 会把 netem 限速误判成程序吞吐上限 |

## 推荐排查顺序

当大带宽下吞吐不达预期时，按以下顺序检查：

1. 确认 netem 加在数据发送端出站网卡；优先使用脚本自动解析，必要时显式 `IFACE=<dev>`。
2. 确认 `tc -s qdisc show dev <iface>` 中 `limit 1000000` 生效，且没有异常队列溢出。
3. 确认是否被 `RATE` 限速；如果要测无限速 delay/loss，命令中显式写 `RATE=`。
4. 确认 `QUIC_CONNECTIONS=1`、`--tuning wan`、BBR、500MB/512MiB 窗口、`iw=2000`、`initrtt=100ms` 生效；程序启动日志会打印 `tcpquic-proxy tuning: srw=... fcw=...`。
5. 用 `METRIC_MODE=steady DURATION_SEC=10` 或 `SIZE_MB>=128` 复测，避免 32MB 短载荷误判。
6. 先看 `tunnel_off`，再评估 `auto/zstd` 的业务收益。
7. 如只在本地 loopback 慢，优先排查 relay fast path、send buffer pool、in-flight send、Linux relay pending 队列和 TCP socket buffer。

## 常用命令备忘

### 仅 delay，无 loss，不限 netem rate

```bash
NETEM=1 DELAY=100ms LOSS=0% NETEM_LIMIT=1000000 RATE= \
  METRIC_MODE=steady DURATION_SEC=10 SIZE_MB=128 \
  QUIC_CONNECTIONS=1 MODES="tunnel_off" ITERATIONS=3 \
  ./scripts/bench-tcpquic-proxy-dgx.sh
```

### delay + 5% loss，不限 netem rate

```bash
NETEM=1 DELAY=100ms LOSS=5% NETEM_LIMIT=1000000 RATE= \
  METRIC_MODE=steady DURATION_SEC=10 SIZE_MB=128 \
  QUIC_CONNECTIONS=1 MODES="direct tunnel_off" ITERATIONS=2 \
  ./scripts/bench-tcpquic-proxy-dgx.sh
```

### 与 secnetperf 对齐的持续下载

```bash
NETEM=1 DELAY=100ms LOSS=0% NETEM_LIMIT=1000000 RATE= \
  METRIC_MODE=steady DURATION_SEC=10 SIZE_MB=32 MODES="tunnel_off" \
  ./scripts/bench-tcpquic-proxy-dgx.sh
```

### 20Gbps 实验参数

脚本仍保留 `TUNING_20GBPS=1` 快捷开关，会向 client/server 注入：

```text
--tuning wan
--iw 4000
--initrtt-ms 100
--relay-io-size 1048576
```

该档位用于极限实验，不作为普通 WAN 默认推荐。
