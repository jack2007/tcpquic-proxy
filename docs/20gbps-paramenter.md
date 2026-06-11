# 20Gbps Single-Connection Parameter Guide

本文整理 `tcpquic-proxy` 在单 QUIC connection、单 QUIC stream 目标最大 20Gbps 时的推荐参数。假设网络条件如下：

```text
target throughput: 20Gbps
RTT: 200ms
packet loss: 5%
QUIC topology: 1 connection, 1 stream
```

## BDP 估算

```text
20Gbps = 2.5GB/s
BDP = 2.5GB/s * 0.2s = 500MB
recommended window = 2 * BDP = 1000MB ~= 1GiB
```

因此，单连接单流要接近 20Gbps，连接级和 stream 级 flow control 至少不能低于 500MB。考虑到 5% loss、调度抖动、应用层 relay pipeline 和 ACK/重传带来的波动，建议使用约 1GiB 作为窗口和发送流水线目标。

## 推荐 CLI

```bash
--tuning custom \
--quic-fcw 1073741824 \
--quic-srw 1073741824 \
--quic-iw 4000 \
--quic-initrtt-ms 200 \
--relay-io-size 1048576 \
--relay-inflight-bytes 1073741824 \
--quic-connections 1
```

用于验证单连接单流上限时，`--quic-connections` 应固定为 `1`。如果目标是整机 200Gbps，应使用多条 QUIC connection 分摊吞吐。

## 参数说明

| 参数 | 推荐值 | 作用 |
|------|--------|------|
| Congestion Control | BBR | 当前代码固定使用 BBR。5% loss 下应避免 loss-based 拥塞控制因丢包过度降速。 |
| `--quic-fcw` | `1073741824` | QUIC connection flow control window。所有 stream 共享该连接级窗口；1GiB 约等于 2x BDP。 |
| `--quic-srw` | `1073741824` | QUIC stream receive window。单流传输时必须足够大，否则单连接吞吐会被 stream flow control 卡住。 |
| `--quic-iw` | `4000` | 初始拥塞窗口，按 1200B 包估算约 4.8MB。它主要降低 200ms RTT 下的启动爬坡时间，不决定稳态上限。 |
| `--quic-initrtt-ms` | `200` | 让连接初期 RTT 估计贴近真实路径，避免拥塞控制启动阶段使用过低 RTT。 |
| Pacing | enabled | 当前代码已开启。5% loss 场景应保持 pacing，避免 burst 进一步放大丢包。 |
| SendBuffering | disabled | 当前代码关闭 MsQuic send buffering，由 relay 层管理发送流水线。 |
| `--relay-io-size` | `1048576` | 1MiB relay IO block，降低高吞吐场景的 syscall、buffer 和分片开销。 |
| `--relay-inflight-bytes` | `1073741824` | 应用层发送流水线目标。QUIC 窗口足够大时，还需要 relay 持续投递数据才能填满连接。 |
| `--quic-connections` | `1` | 单连接单流验证场景固定为 1。生产侧应按总吞吐目标增加连接数。 |

## 当前默认值的限制

当前 WAN 默认/自动调优配置主要面向 10Gbps 到 20Gbps 级别高 BDP 场景，但默认上限偏保守：

| 项目 | 当前默认/自动上限 | 对 20Gbps / 200ms 的影响 |
|------|-------------------|---------------------------|
| `ConnFlowControlWindow` | 约 500MB | 只接近 1x BDP，缺少余量。 |
| `StreamRecvWindowDefault` | 512MiB | 对单流刚好覆盖 BDP，5% loss 下建议提高到 1GiB。 |
| relay ideal send buffer | 默认 64MiB，BDP 自动模式最高约 500MB | 对 20Gbps / 200ms 单流可能不足，建议显式提高。 |

因此，20Gbps / 200ms / 5% loss 的单连接单流测试应使用 `--tuning custom` 并显式覆盖窗口和 relay pipeline。

## 与多 stream 的关系

QUIC 拥塞控制是 connection 级的，同一条 connection 内的多个 stream 共享同一个 congestion window。多 stream 主要提供逻辑并行、不同 TCP tunnel 的隔离和调度能力；在发送数据量足够且单 stream 没有被 flow control 或应用层 pipeline 卡住时，单连接单流与单连接多流的连接级吞吐上限应当接近。

因此，对于“单连接最大 20Gbps”的目标，关键不是增加同一连接内的 stream 数量，而是确保以下三类窗口/流水线都能覆盖 BDP：

1. QUIC congestion window 由 BBR 根据网络反馈增长和维持。
2. QUIC connection / stream flow control window 不低于 BDP，建议约 2x BDP。
3. relay 和 TCP socket pipeline 能持续向 QUIC 投递足够数据。

## 200Gbps 总吞吐建议

如果整机目标是 200Gbps，不建议让单条 QUIC connection 承担全部吞吐。更现实的设计是按每条 connection 10Gbps 到 20Gbps 规划，通过 `--quic-connections` 横向扩展。

多连接可以带来以下收益：

- 每条 QUIC connection 拥有独立拥塞控制状态，降低单连接恢复和调度压力。
- 客户端每条 QUIC connection 默认使用独立本地 UDP 临时端口，服务端网卡 RSS/多队列更容易按五元组分散报文。
- 多连接可以更好地分摊 CPU、加密、UDP 收发和丢包恢复负载。

例如整机目标 200Gbps 时，可以先按 10 条连接、每条 20Gbps 估算，再根据 CPU、NIC 队列、实际丢包和 tunnel 分布结果调整连接数。
