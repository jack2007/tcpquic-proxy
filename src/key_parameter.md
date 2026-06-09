# tcpquic-proxy 大带宽关键调优参数

本文记录 `tcpquic-proxy` 在 200G 直连、100ms RTT、可选 5% loss 等大带宽 / 高 BDP 场景下，已经验证对传输速度有明显影响的关键参数与推荐组合。

## 结论速查

`tcpquic-proxy` 的大带宽提速重点不是盲目增加连接数，而是同时调大 QUIC 窗口、拥塞控制、应用层流水线和测试环境队列：

```text
BBR + 500MB 连接流控 + 512MiB Stream 接收窗口 + iw=2000 + initrtt=100ms
+ 1MB relay IO + 深 in-flight send pipeline + 单 QUIC 连接
+ 正确 netem limit/interface + 足够长的测试载荷
```

## 最有效参数

| 参数 / 方向 | 推荐值 | 作用 | 备注 |
|-------------|--------|------|------|
| 拥塞控制 | BBR | 高 RTT / 有丢包时比默认 CUBIC 更适合维持高吞吐 | 当前 WAN 优化配置固定使用 BBR |
| 连接流控窗口 | 500 MB | 提供足够连接级在途数据窗口，填充高 BDP 管道 | 1GB 试验未超过 500MB，暂不推荐继续增大 |
| Stream 接收窗口 | 512 MiB | 避免单流接收窗口限制下载吞吐 | 默认 64KB 级窗口不适合 100ms 高带宽路径 |
| 初始拥塞窗口 | 2000 packets | 降低启动阶段窗口爬升时间 | 约 2.4MB，按 1200B 包估算 |
| 初始 RTT | 100 ms | 让拥塞控制按 netem 后真实 RTT 收敛 | 直连 RTT 约 0.1ms，100ms netem 下必须显式提示 |
| SendBuffering | false | 应用自行管理发送流水线 | 与 secnetperf 当前高吞吐配置对齐 |
| Pacing | true | 保持平滑发送，避免突发拥塞 | `pacing:0` 未观察到明显收益 |
| Execution profile | MAX_THROUGHPUT | 使用 MsQuic 高吞吐执行配置 | 适合压测 / 大流量场景 |
| relay IO 大小 | 1 MB | 降低 syscall / 分片开销 | 当前 `TqRelayIoSize` |
| in-flight send | 32–64 | 保持 TCP→QUIC 转发流水线深度 | send buffer pool 后默认提升到 64 |
| Ideal send buffer | 16–64 MB+ | 保证应用层发送队列足够深 | 可随 `IDEAL_SEND_BUFFER_SIZE` 增大 |
| TCP socket buffer | 4 MB | 提高本地 TCP 后端 / 前端吞吐 | `TqTuneTcpForThroughput()` 设置 NODELAY + SNDBUF/RCVBUF |

## 是否可以按不同带宽自适应

可以做成自适应，但需要分层理解：**MsQuic 的拥塞窗口会自适应网络，当前 `tcpquic-proxy` 的若干上限参数仍是固定常量**。因此现在更准确地说是“用一组偏高的 WAN 安全上限覆盖大带宽场景”，还不是完整的“按实时带宽自动调参”。

当前代码中已经固定设置的关键值包括：

| 参数 | 当前固定值 | 自适应建议 |
|------|------------|------------|
| 拥塞控制 | BBR | 保持 BBR，让拥塞窗口随网络反馈自动变化 |
| 连接流控窗口 | 500 MB | 按 BDP 计算上限，或保留高上限避免限制高带宽 |
| Stream 接收窗口 | 512 MiB | 按 BDP 计算上限，需保持 2 的幂 |
| 初始拥塞窗口 | 2000 packets | 可按目标 BDP / RTT 档位设置，不能过大以免突发 |
| 初始 RTT | 100 ms | 可由配置 / profile 指定；连接建立后 MsQuic 会测量真实 RTT |
| relay IO | 1 MB | 可按吞吐档位选择 256KB / 1MB / 4MB |
| in-flight send | 64 | 可按 `IdealSendBufferSize` 和 IO 大小动态约束 |
| QUIC 连接数 | 默认 1 | 按业务并发数自适应，不建议单流按带宽盲目增加 |

### 推荐自适应公式

核心按 BDP 估算窗口：

```text
BDP(bytes) = bandwidth_mbps * 1_000_000 / 8 * rtt_ms / 1000
推荐连接窗口 = clamp(2 × BDP, 16MB, 500MB)
推荐 Stream 窗口 = round_up_power_of_2(clamp(2 × BDP, 1MB, 512MiB))
推荐 send buffer = clamp(2 × BDP, 16MB, 500MB)
推荐 relay in-flight bytes = min(IdealSendBufferSize, send buffer)
```

示例：

| 带宽 / RTT | BDP | 推荐窗口档位 |
|------------|-----|--------------|
| 100 Mbps / 100ms | 1.25 MB | 16–32 MB 已足够 |
| 1 Gbps / 100ms | 12.5 MB | 32–64 MB |
| 5 Gbps / 100ms | 62.5 MB | 128–256 MB |
| 10 Gbps / 100ms | 125 MB | 256–500 MB |
| 20 Gbps / 100ms | 250 MB | 500 MB |

### 哪些参数适合自动调

| 参数 | 是否适合自动调 | 原因 |
|------|----------------|------|
| `ConnFlowControlWindow` / `StreamRecvWindowDefault` | 适合 | 本质是 BDP 上限，适合按目标带宽和 RTT 设定 |
| `InitialRttMs` | 半自动 | 连接后可测量 RTT，但初始值需要 profile 或外部配置提示 |
| `InitialWindowPackets` | 半自动 | 有助于启动，但过大可能造成突发丢包，应按档位保守设置 |
| relay IO size | 适合 | 低带宽可小块降低延迟，高带宽可大块降低 syscall 开销 |
| in-flight sends | 适合 | 可由 `target_inflight_bytes / io_size` 推导 |
| compression | 适合 | 可采样压缩率和 CPU 成本后选择 off / lz4 / zstd |
| QUIC 连接数 | 不建议只按带宽自动调 | 更应按并发 TCP 隧道数、CPU、连接稳定性决定 |
| netem limit/interface | 不属于生产自适应 | 只影响测试环境，脚本应自动解析 interface 并按 BDP 设置 limit |

### 建议实现方式

推荐增加一个轻量 profile，而不是把所有参数都暴露成必填 CLI：

```text
--tuning auto|lan|wan-1g|wan-10g|custom
--target-bandwidth-mbps <n>
--target-rtt-ms <n>
```

实现思路：

1. `auto` 默认使用当前稳定 WAN 上限：BBR、500MB / 512MiB、`iw=2000`、`initrtt=100ms`。
2. 如果用户提供 `--target-bandwidth-mbps` 和 `--target-rtt-ms`，按 BDP 公式计算窗口和 send buffer。
3. 对低带宽场景降低 relay buffer pool 和窗口上限，减少每隧道内存占用。
4. 对高带宽场景保持 500MB 上限，避免再次遇到窗口不足。
5. 对压缩路径单独做采样自适应，避免在不可压缩载荷上浪费 CPU。

## 推荐 WAN 压测组合

### 100ms RTT，0% loss，tunnel_off 稳态

```bash
NETEM=1 DELAY=100ms LOSS=0% NETEM_LIMIT=1000000 \
  QUIC_CONNECTIONS=1 SIZE_MB=128 ITERATIONS=3 MODES="tunnel_off" \
  ./scripts/bench-tcpquic-proxy-dgx.sh
```

已验证结果：

| 指标 | 结果 |
|------|------|
| secnetperf 基线 | 约 1098 Mbps |
| tunnel_off 32 MB | 约 494 Mbps，短载荷，慢启动主导 |
| tunnel_off 128 MB | 约 1326–1352 Mbps，稳态，已超过 secnetperf |

### 100ms RTT，5% loss，tunnel_off

推荐仍使用：

```text
QUIC_CONNECTIONS=1
BBR
fcw=500MB
srw=512MiB
iw=2000
initrtt=100ms
NETEM_LIMIT=1000000
```

关键是 netem 必须施加在下载数据发送端的出站网卡。当前双机下载方向是：

```text
spark-1b6f -> spark-1619
```

因此应在 spark-1b6f 上对 `ip route get 169.254.250.230` 解析到的出站 dev 加 `tc netem`。本环境正确网卡是：

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
| netem 位置 | 数据发送端出站网卡 | 下载测试中发送端是 server 侧，不一定是脚本默认网卡 |
| 测试载荷 | `SIZE_MB>=128` 或 `DURATION_SEC=10` | 32MB 在 100ms 下主要测到 BBR 慢启动，不代表稳态 |
| 压测模式 | 优先 `tunnel_off` | 先验证纯隧道转发能力，再看压缩收益 |
| QUIC 连接数 | WAN 单流优先 1 | 多连接适合 LAN 摊 CPU，但高 RTT 下会分散窗口、增加收敛成本 |

## 可压缩载荷

压缩不是传输层参数，但对于可压缩内容会显著提高业务有效吞吐。

| 场景 | tunnel_off | zstd | lz4 |
|------|------------|------|-----|
| loopback 64MB 示例 | 5430.73 Mbps | 12943.82 Mbps | 7526.05 Mbps |

使用建议：

| 模式 | 适用场景 |
|------|----------|
| `--compress off` | 已压缩文件、视频、随机数据、纯网络栈基准 |
| `--compress zstd` | 文本、JSON、日志、可压缩 HTTP 响应，追求压缩率 |
| `--compress lz4` | 可压缩数据且更看重 CPU 开销 / 低延迟 |

## 不建议优先调整的项

| 参数 / 做法 | 结论 | 原因 |
|-------------|------|------|
| `encrypt:0` | WAN 下不优先 | 高 RTT 场景瓶颈主要是 BDP / 窗口，不是加密 CPU |
| `pacing:0` | 不推荐 | 未观察到明显收益，可能增加突发发送风险 |
| 1GB 流控窗口 | 暂不采用 | 500MB 已足够，1GB 未超过 500MB 且可能增加波动 |
| 盲目增加 QUIC 连接数 | 不推荐作为 WAN 单流默认 | 高 RTT 下更少连接 + 大窗口更稳定 |
| 32MB 短测作为最终结论 | 不推荐 | 会把 BBR 慢启动成本计入整段平均，低估稳态吞吐 |

## 推荐排查顺序

当大带宽下吞吐不达预期时，按以下顺序检查：

1. 确认 netem 加在数据发送端出站网卡，而不是固定写死某个 interface。
2. 确认 `tc -s qdisc show dev <iface>` 中 `limit 1000000` 生效，且没有异常队列溢出。
3. 确认 `QUIC_CONNECTIONS=1`、BBR、500MB/512MiB 窗口、`iw=2000`、`initrtt=100ms` 生效。
4. 用 `SIZE_MB=128` 或 `DURATION_SEC=10` 复测，避免 32MB 短载荷误判。
5. 先看 `tunnel_off`，再评估 `zstd` / `lz4` 的业务收益。
6. 如只在本地 loopback 慢，优先排查 relay fast path、send buffer pool、in-flight send 和 TCP socket buffer。

## 常用命令备忘

### 仅 delay，无 loss

```bash
NETEM=1 DELAY=100ms LOSS=0% NETEM_LIMIT=1000000 \
  QUIC_CONNECTIONS=1 SIZE_MB=128 ITERATIONS=3 MODES="tunnel_off" \
  ./scripts/bench-tcpquic-proxy-dgx.sh
```

### delay + 5% loss

```bash
NETEM=1 DELAY=100ms LOSS=5% NETEM_LIMIT=1000000 \
  QUIC_CONNECTIONS=1 SIZE_MB=128 ITERATIONS=2 MODES="direct tunnel_off" \
  ./scripts/bench-tcpquic-proxy-dgx.sh
```

### 与 secnetperf 对齐的持续下载

```bash
NETEM=1 DELAY=100ms DURATION_SEC=10 SIZE_MB=32 MODES="tunnel_off" \
  ./scripts/bench-tcpquic-proxy-dgx.sh
```
