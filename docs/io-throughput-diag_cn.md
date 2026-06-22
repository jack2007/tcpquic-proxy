# tcpquic-proxy IO 吞吐诊断记录

日期：2026-06-21

## 背景

测试目标是分析 2*DGX、1 条 QUIC connection、每条连接 1 个 stream、HTTP CONNECT download 压测时，在 netem 高时延和丢包场景下 tcpquic-proxy IO 吞吐低于 secnetperf 的原因。

关键测试条件：

- 数据方向：download，server 侧从 TCP 源读取并通过 QUIC 发送，client 侧从 QUIC 接收并写回 curl。
- netem：发送端 egress，`limit 5000000`。
- 重点场景：`100ms + 5% loss`、`200ms + 5% loss`。
- QUIC 参数：BBR、`srw=2GiB`、`fcw=2GiB`。
- 当前探索分支：`proxy-ideal-send-buffer-20260621`。

## 已完成的关键修改

### relay backpressure

已将 TCP read 背压从原来的 `relay-inflight-bytes` / `initial-quic-read-ahead` 计算，调整为以 `QUIC_STREAM_EVENT_IDEAL_SEND_BUFFER_SIZE.ByteCount` 为主要依据：

- pause TCP read：`OutstandingQuicSendBytes >= IdealSendBufferBytes`
- resume TCP read：`OutstandingQuicSendBytes < IdealSendBufferBytes`
- `RelayMaxInFlightSends` 不再参与 TCP read 背压判断。
- `relay_pending` 的 QUIC receive pause/resume 逻辑调整为：
  - `>= relay_pending` 停止 MsQuic receive
  - `< relay_pending` 恢复 MsQuic receive

### 窗口和 cap

当前主要验证组合：

- `StreamRecvWindowDefault = 2GiB`
- `ConnFlowControlWindow = 2GiB`
- `QUIC_MAX_IDEAL_SEND_BUFFER_SIZE = 2GiB`
- relay send buffer cap = `512MiB`
- 默认初始 read-ahead = `128MiB`

### 低开销诊断

新增 `--diag-stats`：

- 不启用 spdlog trace 文件。
- 每隔 `--diag-stats-interval` 秒向 stderr 输出一行聚合状态。
- 输出包括 relay 侧：
  - `outstanding_quic_send_bytes`
  - `ideal_send_threshold_bytes`
  - `tcp_read_disabled_relays`
  - `pending_bytes`
  - `event_queue_full_errors`
  - `quic_send_failures`
  - `quic_send_api_failures`
  - `quic_send_fatal_errors`
  - `last_quic_send_status`
- 输出包括 QUIC net stats 最新样本：
  - `bbr_bw_mbps`
  - `bytes_in_flight`
  - `bytes_in_flight_max`
  - `posted`
  - `ideal`
  - `srtt`
  - `cwnd`
  - `bbr_state`
  - `bbr_recovery_state`
  - `recovery_window`
  - `app_limited`

最初 client 侧可以拿到 net stats，但 server 侧没有拿到 `NETWORK_STATISTICS` 样本。原因已定位并修复：server connection callback 的 `QUIC_CONNECTION_EVENT_NETWORK_STATISTICS` 分支仍然只判断 `TqTraceEnabled()`，没有纳入 `TqDiagStatsEnabled()`，导致 `--diag-stats` 开启时 server 侧收到事件也不会保存样本。

## 测试结果摘要

### 1GiB/2GiB 与 2GiB/4GiB cap 对比

`200ms + 5% loss, limit 5000000, initrtt=200ms`

| 组合 | trace | 吞吐 |
| --- | --- | --- |
| corecap=1GiB, relaycap=2GiB | no trace | 5066.46 Mbps |
| corecap=1GiB, relaycap=2GiB | no trace rerun | 4960.03 Mbps |
| corecap=2GiB, relaycap=4GiB | no trace | 7285.96 Mbps |
| corecap=2GiB, relaycap=4GiB | no trace rerun | 5641.88 Mbps |
| corecap=2GiB, relaycap=4GiB | no trace natural latest | 5271.98 Mbps |

说明：

- 2GiB/4GiB 在部分轮次明显优于 1GiB/2GiB，但存在较大波动。
- 不能只用 cap 解释所有吞吐差异。
- trace 文件日志会显著降低吞吐，不能把 trace 下数据直接当作真实性能。

### 100ms 场景

`100ms + 5% loss, limit 5000000`

| 组合 | trace | 吞吐 |
| --- | --- | --- |
| corecap=1GiB, relaycap=2GiB | no trace | 7094.88 Mbps |
| corecap=2GiB, relaycap=4GiB | no trace | 8657.88 Mbps |
| corecap=2GiB, relaycap=4GiB | no trace rerun | 5524.25 Mbps |

说明：

- 早期曾跑到接近 secnetperf 的 8.6Gbps。
- 后续复测波动较大，因此需要依赖低开销运行时状态判断瓶颈，而不是只看单次吞吐。

### relay_pending 512MiB/128MiB 对照

为验证 QUIC -> TCP receive 方向的 `relay_pending` 上限是否会影响 IO 吞吐，使用独立二进制对比：

- `tcpquic-proxy-relaypending512`：`relay_pending=536870912`
- `tcpquic-proxy-relaypending128`：`relay_pending=134217728`

共同测试条件：

- `100ms + 5% loss, limit 5000000`
- `tunnel_off`
- `QUIC_CONNECTIONS=1`
- `SIZE_MB=20000`
- `DURATION_SEC=30`
- `WARMUP_MB=0`
- `--initrtt-ms 100 --diag-stats --diag-stats-interval 1`
- 每轮启动前清理远端残留 `tcpquic-proxy` 进程和旧 netem qdisc。

512MiB 结果：

| run | 吞吐 | max_pending_quic_receive_bytes | pause/resume | curl exit |
|---|---:|---:|---:|---:|
| 1 | 8762.51 Mbps | 310088355 bytes | 0 / 0 | 0 |
| 2 | 9200.47 Mbps | 405487546 bytes | 0 / 0 | 0 |
| 3 | 8695.65 Mbps | 306718791 bytes | 0 / 0 | 0 |

512MiB 三个有完整日志且 curl 正常结束的样本平均吞吐为 `8886.21 Mbps`。三轮 `max_pending_quic_receive_bytes` 均低于 512MiB，`quic_receive_paused/resumed` 均为 `0/0`，说明该上限在当前 100ms/5% download 场景下没有实际介入 receive 背压。

128MiB 结果：

| run | 吞吐 | max_pending_quic_receive_bytes | pause/resume | curl exit |
|---|---:|---:|---:|---:|
| 1 | 8358.46 Mbps | 363397871 bytes | 14 / 14 | 0 |
| 2 | 6197.94 Mbps | 触发 receive pause | 至少 9 / 9 | 18 |
| 3 | 6476.43 Mbps | 425565168 bytes | 9 / 9 | 18 |

128MiB 已经明确触发 QUIC receive pause，并且 3 次里 2 次出现 `curl exit 18`。第三轮 client 日志中还出现多条 `linux relay unrecoverable error reason=tcp_write_hard_error`。

结论：

- `512MiB` 对当前实测的 100ms/5% download 吞吐没有可见负面影响，且不会触发 receive pause。
- `128MiB` 太小，会实际介入 receive 背压，并在本场景下带来下载不完整的稳定性风险。
- 当前默认 `relay_pending` 固定为 `512MiB`，比 4GiB 明显降低 QUIC -> TCP receive 方向的最坏 pending 内存上限，同时保留本轮测试中的吞吐表现。

### 200ms 自然 ideal/postbuf 诊断

`200ms + 5% loss, limit 5000000, corecap=2GiB, relaycap=4GiB, initial_read_ahead=128MiB`

| 模式 | 吞吐 | 结果目录 |
| --- | --- | --- |
| no trace | 5271.98 Mbps | `docs/dgx-proxy-corecap2g-relaycap4g-notrace-natural-200ms-20260621-191409` |
| diag-stats | 5681.89 Mbps | `docs/dgx-proxy-diagstats-corecap2g-relaycap4g-200ms-errors-20260621-190720` |

发送端 relay 侧观察：

- `outstanding_quic_send_bytes` 基本贴着 `ideal_send_threshold_bytes`。
- ideal 从约 `290MiB` 增长到 `653MiB`，再到约 `980MiB`。
- `tcp_read_disabled_relays=1` 时，说明 TCP read 被 ideal/postbuf 背压暂停。
- `event_queue_full_errors=0`
- `quic_send_failures=0`
- `quic_send_api_failures=0`
- `quic_send_fatal_errors=0`

结论：

- 应用层并不是产生不了数据。
- relay 内部也没有明显 send API 失败。
- 当前直接限制是 `QUIC_STREAM_EVENT_IDEAL_SEND_BUFFER_SIZE` 驱动的 postbuf 阈值停在约 `980MiB`。

### 强制提高 initial read-ahead 到 1.47GiB

为了验证“980MiB 是否太小”，将 `--initial-quic-read-ahead` 设置为 `1470987163`，即 MsQuic ideal 计算的下一个 1.5x 台阶。

`200ms + 5% loss, limit 5000000`

| 额外修改 | 吞吐 | 结果目录 |
| --- | --- | --- |
| default event queue | 4264.14 Mbps | `docs/dgx-proxy-diagstats-readahead1470m-200ms-20260621-190922` |
| event queue capacity 提高到 65536 | 3627.59 Mbps | `docs/dgx-proxy-diagstats-readahead1470m-queue65536-200ms-20260621-191159` |

观察：

- `outstanding_quic_send_bytes` 确实被抬高到约 `1.47GiB`。
- 默认 event queue 下出现 `event_queue_full_errors=268`。
- 将 event queue capacity 提高到 65536 后，`event_queue_full_errors=0`，但吞吐进一步下降。
- client 侧 RTT 观察上升到 300ms 左右。

结论：

- event queue full 是强行提高 postbuf 后引入的新副作用，不是自然 980MiB 场景的主要瓶颈。
- 单纯把 postbuf 从 980MiB 抬到 1.47GiB 不会提高吞吐，反而会增加排队和 RTT，降低吞吐。
- “继续扩大发送队列”不是当前有效方向。

## server 侧 BBR net stats 修复后观察

修复点：

- `src/protocol/quic_session.cpp`
- server callback 中 `QUIC_CONNECTION_EVENT_NETWORK_STATISTICS` 的条件改为：
  - `TqTraceEnabled() || TqDiagStatsEnabled()`

复测场景：

- `200ms + 5% loss, limit 5000000`
- `corecap=2GiB, relaycap=4GiB`
- `initial_read_ahead=128MiB`
- `--diag-stats --diag-stats-interval 5`

结果：

- 吞吐：`4053.10 Mbps`
- 结果目录：`docs/dgx-proxy-diagstats-server-bbr-200ms-20260621-205502`

server 侧关键样本：

| posted/relay outstanding | stream ideal | BBR bw | bytes_in_flight | srtt | cwnd | bbr_state | recovery_state | app_limited |
| --- | --- | --- | --- | --- | --- | --- | --- | --- |
| 293202047 | 290565366 | 7630.50 Mbps | 82980679 | 231.221ms | 503872935 | 2 | 2 | 0 |
| 436138167 | 435848049 | 10456.88 Mbps | 101302999 | 211.258ms | 626882230 | 2 | 2 | 0 |
| 654596196 | 653772073 | 6569.44 Mbps | 126075328 | 261.646ms | 419861007 | 2 | 2 | 0 |
| 654559498 | 653772073 | 11256.34 Mbps | 267306696 | 247.444ms | 635225398 | 2 | 2 | 0 |
| 656556742 | 653772073 | 10016.50 Mbps | 0 | 223.049ms | 5888 | 3 | 2 | 1 |
| 654208763 | 653772073 | 9755.75 Mbps | 73835377 | 329.326ms | 610588869 | 2 | 2 | 0 |

状态含义：

- `bbr_state=2`：`BBR_STATE_PROBE_BW`
- `bbr_state=3`：`BBR_STATE_PROBE_RTT`
- `bbr_recovery_state=2`：`RECOVERY_STATE_GROWTH`

新的直接证据：

- 发送端 BBR 带宽估计并不低，多个样本在 `6.5Gbps ~ 11.2Gbps`。
- 发送端应用/relay 已经把 posted/outstanding 填到 ideal 附近。
- 实际 `bytes_in_flight` 明显低于 posted/ideal，最高样本约 `267MiB`。
- 出现过一次 `PROBE_RTT`，`cwnd=5888`，`app_limited=1`。
- `srtt` 可上升到 `329ms`，明显高于 netem 基础 RTT。

因此，当前问题不能简单归因为“BBR 带宽估计低”或“应用没有填满发送队列”。更准确的描述是：

- 应用层已经把 MsQuic send buffer/postbuf 填满。
- BBR 的估计带宽较高，但实际在途数据没有持续跟到 posted/ideal。
- `IDEAL_SEND_BUFFER_SIZE` 增长依赖 MsQuic 的 `BytesInFlightMax`，而不是直接依赖 BBR bandwidth estimate。
- 在当前样本中 `bytes_in_flight` 没有持续突破下一个 ideal 台阶，因此 stream ideal 停在 `653MiB` 附近。

## 增加 BytesInFlightMax 后的直接证据

为了确认 `IDEAL_SEND_BUFFER_SIZE` 为什么停在 `980MiB`，已给 MsQuic `QUIC_CONNECTION_EVENT_NETWORK_STATISTICS` 增加 `BytesInFlightMax` 字段，并在 tcpquic-proxy 的 `--diag-stats` 中输出 `bytes_in_flight_max`。

复测场景：

- `200ms + 5% loss, limit 5000000`
- `corecap=2GiB, relaycap=4GiB`
- 原始 BBR ProbeRTT 行为
- 结果目录：`docs/dgx-proxy-diagstats-bifmax-200ms-20260621-205915`
- 吞吐：`5415.44 Mbps`
- netem qdisc dropped：`11346`

server 侧关键样本：

| outstanding/postbuf | ideal | bytes_in_flight_max | bytes_in_flight | BBR bw | srtt | cwnd | event_queue_full |
| --- | --- | --- | --- | --- | --- | --- | --- |
| 980832929 | 980658109 | 694638343 | 134328832 | 10664.41 Mbps | 305.336ms | 786370398 | 0 |
| 980697913 | 980658109 | 694638343 | 9520896 | 14905.27 Mbps | 590.559ms | 849154017 | 0 |
| 981152310 | 980658109 | 694638343 | 204148736 | 11705.98 Mbps | 248.556ms | 694830726 | 0 |
| 984086300 | 980658109 | 731937280 | 140232579 | 16902.52 Mbps | 201.303ms | 956488733 | 1071 |
| 981068807 | 980658109 | 731937280 | 224493108 | 13847.44 Mbps | 224.896ms | 786940487 | 1071 |
| 904155700 | 980658109 | 731937280 | 91452416 | 14446.14 Mbps | 285.529ms | 826471634 | 1071 |

结论：

- `ideal=980658109` 与 `bytes_in_flight_max=694638343/731937280` 匹配 MsQuic `QuicGetNextIdealBytes(BytesInFlightMax)` 的增长路径。
- 要让 ideal 从 `980MiB` 跨到下一个约 `1.47GiB` 台阶，`BytesInFlightMax` 必须先超过当前 `980MiB` 台阶；本轮最高只到约 `732MiB`。
- 因此 `980MiB` 停住的直接原因不是 corecap 或 relaycap 已到上限，而是 MsQuic 观察到的历史最大在途数据没有继续增长。
- BBR bandwidth estimate 可达到 `10Gbps ~ 16Gbps`，但实际 `bytes_in_flight` 经常只有几十到两百 MiB，并没有持续接近 `cwnd` 或 `ideal`。
- 本轮后半段出现 `event_queue_full_errors=1071`，但它发生在 ideal 已经停在 `980MiB` 之后；后续单独扩大 event queue 的反证显示它不是根因。

## event queue capacity 反证

为了排除 event queue full 对自然场景的影响，临时把 event queue capacity 提高到 `65536` 后复测。

复测场景：

- `200ms + 5% loss, limit 5000000`
- 结果目录：`docs/dgx-proxy-diagstats-queue65536-natural-200ms-20260621-210133`
- 吞吐：`4290.21 Mbps`
- netem qdisc dropped：`11346`

观察：

- `event_queue_full_errors=0`
- ideal 主要停在 `653772073`
- `bytes_in_flight_max` 最高约 `598989540`
- 出现一次 `bbr_state=3`，即 `PROBE_RTT`，当时 `bytes_in_flight=0`、`cwnd=5888`、`app_limited=1`

结论：

- 扩大 event queue 可以消除 event queue full 计数，但吞吐没有改善。
- event queue full 是高 postbuf 或高突发条件下的副作用，不是当前 200ms+5% 场景吞吐低的主因。
- 该临时修改已回退，不进入正式代码。

## ProbeRTT 验证

为了判断 ProbeRTT 是否是吞吐下降的直接原因，临时把 MsQuic BBR 的 MinRTT 过期时间从 `10s` 改为 `60s`，让 30 秒测试窗口内尽量不进入 ProbeRTT。

复测场景：

- `200ms + 5% loss, limit 5000000`
- 结果目录：`docs/dgx-proxy-diagstats-bbr-probertt60-200ms-20260621-210358`
- 吞吐：`4440.53 Mbps`
- netem qdisc dropped：`11280`

server 侧关键样本：

| outstanding/postbuf | ideal | bytes_in_flight_max | bytes_in_flight | BBR bw | srtt | cwnd | bbr_state |
| --- | --- | --- | --- | --- | --- | --- | --- |
| 656323932 | 653772073 | 548803039 | 235939705 | 11570.36 Mbps | 461.774ms | 713150777 | 2 |
| 981014217 | 980658109 | 752419328 | 192979878 | 16668.45 Mbps | 269.316ms | 1025291626 | 2 |
| 981066266 | 980658109 | 752419328 | 145468928 | 10887.45 Mbps | 277.779ms | 611111765 | 2 |
| 980773005 | 980658109 | 752419328 | 435609462 | 11133.08 Mbps | 233.146ms | 661139528 | 2 |
| 984328909 | 980658109 | 800526592 | 1748736 | 13750.06 Mbps | 495.955ms | 409505066 | 2 |

结论：

- 采样中没有再看到 `bbr_state=3`，说明该实验基本屏蔽了 ProbeRTT。
- 吞吐没有提升，`BytesInFlightMax` 仍然没有超过 `980MiB`，ideal 仍停在 `980MiB`。
- ProbeRTT 会造成瞬时 cwnd 降到极低并引入抖动，但它不是当前吞吐低的单独根因。
- 该临时修改已回退，保留原始 MsQuic BBR 行为。

## MsQuic send flush 调度上限诊断

为了继续定位“postbuf 已满，但 `bytes_in_flight` 没有持续跟上”的原因，给 MsQuic 的 `QUIC_CONNECTION_EVENT_NETWORK_STATISTICS` 增加了 send flush 诊断字段，并通过 tcpquic-proxy `--diag-stats` 输出：

- `flush_count`
- `flush_pacing_delayed`
- `flush_cc_blocked`
- `flush_scheduling`
- `flush_amp_blocked`
- `flush_no_work`
- `flush_last_allowance`
- `flush_last_path_allowance`
- `flush_last_result`
- `flush_last_datagrams`
- `out_flow_blocked`

这些字段用于区分：

- 是否被 pacing timer 延迟；
- 是否被 congestion control/cwnd/recovery 硬限制；
- 是否被 anti-amplification 限制；
- 是否只是没有待发送数据；
- 是否每次 flush 都打满 `QUIC_MAX_DATAGRAMS_PER_SEND` 后被 scheduling 限制重新排队。

### `QUIC_MAX_DATAGRAMS_PER_SEND` 的逻辑意义

`QUIC_MAX_DATAGRAMS_PER_SEND` 是 MsQuic core 里的编译期宏，定义在 `third_party/msquic/src/core/quicdef.h`。它不是 QUIC 协议里的 flow window，也不是 BBR/cwnd 参数，而是 MsQuic send path 的批量/调度粒度参数。

可以近似理解为：一次 `QuicSendFlush()` 最多连续构造并提交多少个 UDP datagram。它不是网络层一次 syscall 精确发送多少个 UDP 包的绝对值，因为 MsQuic 还会受 USO buffer、当前 packet builder 状态、pacing、cwnd、anti-amplification、可发送 stream 数据等因素影响；实际观测值可能略高于宏值。例如设置为 `240` 时，测试中 `flush_last_datagrams` 约为 `244`。

关键路径：

- `QuicSendFlush()` 从 connection/stream send queue 中取数据；
- `QuicPacketBuilder` 构造 QUIC packet 和 UDP datagram；
- `Builder.TotalCountDatagrams` 记录当前 flush 已经创建的 datagram 数；
- 当 `Builder.TotalCountDatagrams >= QUIC_MAX_DATAGRAMS_PER_SEND` 时，本次 flush 会停止，剩余数据需要依赖后续 send flush 重新调度后继续发送。

因此，这个参数的直接含义不是“发送窗口大小”，而是“每次被调度起来后，发送路径最多连续推进多少个 UDP datagram”。它影响的是调度批量，而不是允许未确认数据总量。未确认数据总量仍然受 postbuf、stream/connection flow control、congestion control、pacing 和 send buffer 等因素共同影响。

40 和 240 在高 BDP 场景下差异明显，原因是每次 flush 结束后，后续数据不是在同一个简单 `for` 循环里无成本继续发送，而是要重新经过 MsQuic 的连接操作队列、worker 调度、packet builder 初始化/清理、状态检查、send path 再进入等流程。

按 MTU 约 `1470B` 粗略估算：

- `40` 个 datagram 约 `58.8KiB`
- `240` 个 datagram 约 `352.8KiB`

如果目标吞吐是 `10Gbps`，发送速率约 `1.25GB/s`：

- `40` 个 datagram 的数据量只够约 `47us`
- `240` 个 datagram 的数据量约 `282us`

在 100Gbps 网卡、100ms/200ms RTT、5% loss、BBR、高 postbuf 的场景里，发送端需要持续把大量新数据推入网络，才能维持足够的 `bytes_in_flight`。如果每次 flush 只推进几十 KiB，就会产生极高的 flush 调度频率。调度、锁、状态检查、worker queue、packet builder 反复初始化/清理这些开销和间隙会变成实际吞吐瓶颈。

本轮诊断数据正好验证了这一点：

- batch=40 时，`flush_count=340638`，`flush_scheduling=339236`，`flush_last_datagrams=44`，吞吐 `3546.69 Mbps`。
- batch=240 时，`flush_count` 降到约 `21727`，`flush_last_datagrams=244`，200ms+5% 吞吐提升到 `7269.02/8227.46 Mbps`，100ms+5% 提升到 `9580.54 Mbps`。

所以当前问题不是“应用层没有数据”，也不是“大部分时间 BBR/cwnd 不让发”，而是默认 batch=40 使得 send flush 刚发送一点数据就因为 batch 上限停下来，然后反复重新调度。把 batch 提高到 240 后，一次 flush 可以连续推进更多数据，调度次数大幅降低，`bytes_in_flight` 更容易持续抬升。

这个值也不能无限增大。batch=480 的反证显示吞吐反而下降，RTT 多次膨胀到 `370ms ~ 441ms`。可能原因包括：

- 单次突发太大，造成更明显的队列排队和 RTT 膨胀；
- pacing 粒度变粗，短时间 burst 更明显；
- packet builder 的 `TotalCountDatagrams` 计数路径和 USO batching 行为被改变；
- 单次 flush 占用 worker 时间过长，影响 ACK、loss recovery、timer 等其他连接事件处理。

因此，`QUIC_MAX_DATAGRAMS_PER_SEND` 需要找一个批量和排队之间的平衡点。当前 2*DGX、1x1 download、100ms/200ms+5% loss 场景里，默认 `40` 明显太小，`240` 是目前测到的较好候选值。

### MsQuic pacing 与 qdisc 的关系

MsQuic 的 pacing 功能是内置在 MsQuic 用户态协议栈内部的，不是 Linux qdisc/netem 里的功能。

在发送路径中，`QuicSendFlush()` 会调用 `QuicPacketBuilderHasAllowance(&Builder)` 判断当前 packet builder 是否还有发送 allowance。如果 allowance 不足，但 congestion control 认为连接后续仍然可以发送，MsQuic 会：

- 给 connection 添加 `QUIC_FLOW_BLOCKED_PACING`；
- 设置 `QUIC_CONN_TIMER_PACING`；
- 本次 flush 返回 `QUIC_SEND_DELAYED_PACING`；
- 等 MsQuic 自己的 pacing timer 到期后，再重新调度下一次 send flush。

也就是说，MsQuic pacing 决定的是“用户态 QUIC 协议栈什么时候继续构造并提交 UDP datagram”。

Linux qdisc/netem 是另一层机制。qdisc 处理的是已经从应用/协议栈交给内核的 UDP 包，决定这些包什么时候真正从内核队列发往网卡，或者按 netem 配置注入 delay/loss/limit/rate。当前测试中的 `netem delay/loss/limit` 属于 qdisc 层网络仿真，不是 MsQuic pacing。

两者关系可以理解为：

- MsQuic pacing：包进入内核前，在用户态控制 QUIC packet/UDP datagram 的构造和提交节奏；
- qdisc/netem：包进入内核后，在内核网络队列中控制排队、延迟、丢包、限速等行为。

因此，`QUIC_MAX_DATAGRAMS_PER_SEND` 增大后是否产生更大 burst，首先受 MsQuic 内部 pacing allowance 约束；如果 MsQuic 已经把一批 UDP datagram 提交给内核，后续还会继续受 qdisc/netem 的排队、延迟和丢包影响。

本轮测试中 `flush_pacing_delayed` 只有个位数，说明默认 batch=40 的主要瓶颈不是 MsQuic pacing timer 频繁阻塞，而是单次 flush datagram batch 太小导致大量 `flush_scheduling`。但 batch 继续增大到 480 后 RTT 膨胀，说明即使 MsQuic 内部 pacing 没有表现为大量 delayed 计数，过大的单次提交批量仍可能在 qdisc/网卡队列侧形成更明显的排队。

### 原始 batch=40 诊断

复测场景：

- `200ms + 5% loss, limit 5000000`
- `QUIC_MAX_DATAGRAMS_PER_SEND=40`
- 结果目录：`docs/dgx-proxy-diagstats-sendflush-200ms-20260621-212809`
- 吞吐：`3546.69 Mbps`
- curl exit：`28`
- netem qdisc dropped：`11502`

server 侧末尾关键样本：

| ideal | bytes_in_flight_max | bytes_in_flight | cwnd | BBR bw | flush_count | flush_scheduling | flush_pacing_delayed | flush_cc_blocked | flush_last_datagrams |
| --- | --- | --- | --- | --- | --- | --- | --- | --- | --- |
| 653772073 | 572467734 | 22801280 | 34851072 | 8987.68 Mbps | 340638 | 339236 | 3 | 216 | 44 |

结论：

- `flush_scheduling` 几乎等于 `flush_count`，说明绝大多数 flush 都是打满单次发送 batch 后重新排队。
- `flush_pacing_delayed=3`，不是 pacing timer 频繁阻塞。
- `flush_cc_blocked=216`，相对 `flush_count=340638` 很小，说明大部分时候不是 cwnd/recovery 直接返回不能发。
- `flush_last_datagrams=44` 与 `QUIC_MAX_DATAGRAMS_PER_SEND=40` 同量级，直接指向 MsQuic 单次 flush datagram batch 上限。

### batch=160 单变量验证

将 `QUIC_MAX_DATAGRAMS_PER_SEND` 从 `40` 临时提高到 `160` 后复测。

结果：

- 结果目录：`docs/dgx-proxy-diagstats-sendflush-batch160-200ms-20260621-213033`
- 吞吐：`4082.98 Mbps`
- curl exit：`28`
- netem qdisc dropped：`11280`

server 侧末尾关键样本：

| ideal | bytes_in_flight_max | bytes_in_flight | cwnd | BBR bw | flush_count | flush_scheduling | flush_pacing_delayed | flush_cc_blocked | flush_last_datagrams |
| --- | --- | --- | --- | --- | --- | --- | --- | --- | --- |
| 980658109 | 885508096 | 4404224 | 843119194 | 14311.67 Mbps | 101458 | 100404 | 3 | 351 | 176 |

结论：

- 单次 flush datagrams 从约 `44` 提高到约 `176`。
- `flush_count` 从 `340638` 降到 `101458`。
- 吞吐从 `3546.69 Mbps` 提高到 `4082.98 Mbps`。
- `BytesInFlightMax` 从约 `572MiB` 提高到约 `885MiB`，ideal 能升到 `980MiB`。
- 但仍有大量 `flush_scheduling`，说明 batch=160 仍可能偏小。

### batch=240 单变量验证

考虑 `QUIC_PACKET_BUILDER.TotalCountDatagrams` 当前是 `uint8_t`，先将 `QUIC_MAX_DATAGRAMS_PER_SEND` 提高到 `240`，避免超过 255。

复测结果：

| 场景 | 吞吐 | curl exit | netem dropped | 结果目录 |
| --- | --- | --- | --- | --- |
| 200ms+5% batch=240 run1 | 7269.02 Mbps | 0 | 11280 | `docs/dgx-proxy-diagstats-sendflush-batch240-200ms-20260621-213243` |
| 200ms+5% batch=240 run2 | 8227.46 Mbps | 0 | 11498 | `docs/dgx-proxy-diagstats-sendflush-batch240-repeat-200ms-20260621-213439` |
| 100ms+5% batch=240 | 9580.54 Mbps | 0 | 11412 | `docs/dgx-proxy-diagstats-sendflush-batch240-u8-100ms-20260621-214842` |

run2 server 侧关键样本：

| ideal | bytes_in_flight_max | bytes_in_flight | cwnd | BBR bw | flush_count | flush_scheduling | flush_pacing_delayed | flush_cc_blocked | flush_last_datagrams |
| --- | --- | --- | --- | --- | --- | --- | --- | --- | --- |
| 980658109 | 823201755 | 150980960 | 1167238420 | 18668.16 Mbps | 3207 | 2377 | 5 | 30 | 244 |
| 980658109 | 823201755 | 297155584 | 1077881218 | 20427.79 Mbps | 9735 | 8711 | 5 | 33 | 244 |
| 980658109 | 823201755 | 447156800 | 999889856 | 24030.88 Mbps | 16319 | 15267 | 5 | 61 | 244 |

结论：

- batch=240 后，单次 flush datagrams 提高到约 `244`。
- `flush_count` 从 batch=40 的 `340638` 降到约 `21727`，数量级下降。
- 200ms+5% 场景吞吐从 `3.5Gbps ~ 5.6Gbps` 区间提升到 `7.3Gbps ~ 8.2Gbps`，并且 curl 正常完成。
- `flush_pacing_delayed` 仍然只有个位数，说明主要提升不是来自绕过 pacing。
- `flush_cc_blocked` 仍远小于 `flush_scheduling`，说明主要瓶颈不是 BBR 判断不能发，而是 MsQuic 单次 flush 调度批量太小导致频繁重新调度。
- 100ms+5% 场景也从早期波动较大的 `5.5Gbps ~ 8.6Gbps` 提升到 `9.58Gbps`，说明 batch=240 不是只对 200ms 场景有效。
- 当前 100ms/200ms+5% 的直接性能瓶颈已基本定位为 `QUIC_MAX_DATAGRAMS_PER_SEND` 太小，导致高 BDP/高 postbuf 场景下 send flush 调度开销过高，实际在途数据难以持续抬升。

### batch=200 和 batch=160 补充验证

补充结果：

| 场景 | 吞吐 | curl exit | netem dropped | 结果目录 |
| --- | --- | --- | --- | --- |
| 100ms+5% batch=160 | 7246.99 Mbps | 0 | 11478 | `docs/dgx-proxy-diagstats-sendflush-batch160-100ms-20260621-214159` |
| 200ms+5% batch=160 | 4082.98 Mbps | 28 | 11280 | `docs/dgx-proxy-diagstats-sendflush-batch160-200ms-20260621-213033` |
| 200ms+5% batch=200 | 5421.46 Mbps | 28 | 11280 | `docs/dgx-proxy-diagstats-sendflush-batch200-200ms-20260621-214328` |

说明：

- batch=160 相比默认 40 有提升，但 200ms 场景仍然 timeout。
- batch=200 继续提升到 `5421.46 Mbps`，但仍低于 batch=240，且仍 timeout。
- 目前已测值中，`240` 是最优候选点；`160/200` 可以证明吞吐提升与 batch 增大有明确相关性，但还没达到稳定完成 30 秒/20GB 下载的程度。

### batch=220/250/260 邻近验证

为了继续精确收敛 `QUIC_MAX_DATAGRAMS_PER_SEND` 的候选值，在保持其他参数不变的情况下，补充测试了 `220/250/260`。

固定条件：

- `200ms + 5% loss`
- `limit 5000000`
- 2*DGX 1x1 download
- `--diag-stats --diag-stats-interval 5`
- `QUIC_PACKET_BUILDER.TotalCountDatagrams` 保持原有 `uint8_t`

结果：

| batch | 吞吐 | curl exit | netem dropped | 关键观察 | 结果目录 |
| --- | --- | --- | --- | --- | --- |
| 220 | 7478.73 Mbps | 0 | 11424 | `flush_last_datagrams=220`，`flush_scheduling=94479`，`flush_pacing_delayed=6` | `docs/dgx-proxy-diagstats-sendflush-batch220-200ms-20260621-continue` |
| 250 | 4969.42 Mbps | 28 | 11280 | `flush_last_datagrams=252`，`srtt` 最高观察到 `468.063ms` | `docs/dgx-proxy-diagstats-sendflush-batch250-200ms-20260621-continue` |
| 260 | 2747.90 Mbps | 28 | 11214 | `flush_scheduling=0`，`srtt` 最后膨胀到 `1592.685ms`，`event_queue_full_errors=1287` | `docs/dgx-proxy-diagstats-sendflush-batch260-200ms-20260621-continue` |

结论：

- `220` 可以正常完成，吞吐 `7478.73 Mbps`，低于 batch=240 的最好复测 `8227.46 Mbps`，但高于 batch=160/200。
- `250` 已经开始明显退化，虽然仍能构造约 `252` 个 datagram，但吞吐下降到 `4969.42 Mbps`，且 curl timeout。
- `260` 退化更严重，吞吐只有 `2747.90 Mbps`；虽然 `flush_scheduling=0` 表示已经不再被 batch scheduling 限制，但 RTT 和 event queue 副作用显著。
- 这组邻近测试说明：性能提升不是简单随 batch 单调增加。batch 从 40 提高到 240 可以降低调度开销；继续提高到 250/260 后，过大突发、排队、事件队列压力或计数边界副作用开始主导。
- 在当前 `uint8_t TotalCountDatagrams` 约束下，`240` 比 `250/260` 更稳妥，也比 `220` 更快；当前最优候选仍是固定 `240`。

### batch=480 反证

为了确认 batch=240 是否还偏小，将 `QUIC_PACKET_BUILDER.TotalCountDatagrams` 改为 `uint16_t`，并把 `QUIC_MAX_DATAGRAMS_PER_SEND` 提高到 `480` 后复测。

结果：

- 结果目录：`docs/dgx-proxy-diagstats-sendflush-batch480-200ms-20260621-213732`
- 吞吐：`4150.07 Mbps`
- curl exit：`28`
- netem qdisc dropped：`11214`

server 侧关键样本：

| ideal | bytes_in_flight_max | bytes_in_flight | cwnd | BBR bw | srtt | flush_count | flush_scheduling | flush_pacing_delayed | flush_cc_blocked | flush_last_datagrams |
| --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- |
| 980658109 | 900633296 | 537121024 | 881188085 | 14783.81 Mbps | 371.941ms | 20985 | 18142 | 11 | 235 | 484 |
| 980658109 | 900633296 | 89703680 | 767408412 | 13217.07 Mbps | 441.199ms | 26826 | 23954 | 11 | 242 | 484 |
| 980658109 | 900633296 | 156234668 | 822844372 | 13753.06 Mbps | 374.587ms | 40662 | 37695 | 14 | 254 | 484 |

结论：

- batch=480 可以把单次 flush datagrams 提高到约 `484`，但吞吐反而下降到 `4150.07 Mbps`。
- RTT 多次膨胀到 `370ms ~ 441ms`，明显高于 batch=240 的高吞吐复测。
- `flush_scheduling` 仍然很多，说明继续扩大 batch 并没有消除调度限制，反而可能造成更大的突发和排队。
- 当前更合理的候选值是 batch=240，而不是继续无约束增大。

### uint16 和 dynamic batch 反证

在 batch=480 实验中曾把 `QUIC_PACKET_BUILDER.TotalCountDatagrams` 从 `uint8_t` 改为 `uint16_t`。随后把常量回到 `240`，但保留 `uint16_t` 复测 100ms+5%：

| 场景 | 吞吐 | curl exit | netem dropped | 结果目录 |
| --- | --- | --- | --- | --- |
| 100ms+5% batch=240 + `uint16_t` | 4673.76 Mbps | 28 | 11412 | `docs/dgx-proxy-diagstats-sendflush-batch240-100ms-20260621-214010` |

该轮 `flush_last_datagrams` 变为约 `264`，`srtt` 多次抬高到 `150ms ~ 228ms`，吞吐明显低于 `batch=240 + uint8_t` 的 `9580.54 Mbps`。这说明只把计数类型放宽，也会改变实际突发行为，并且可能引入更严重排队。

也尝试过 RTT-based dynamic batch：

- 低 RTT 使用 `160`
- `srtt >= 150ms` 使用 `240`

复测结果：

| 场景 | 吞吐 | curl exit | netem dropped | 结果目录 |
| --- | --- | --- | --- | --- |
| 200ms+5% dynamic batch | 3132.17 Mbps | 28 | 11498 | `docs/dgx-proxy-diagstats-sendflush-dynamic-200ms-20260621-214603` |

dynamic batch 结果低于默认 batch=40 的部分复测，也低于 batch=160/200/240。该方案已回退。

当前代码状态：

- `QUIC_MAX_DATAGRAMS_PER_SEND=240`
- `QUIC_PACKET_BUILDER.TotalCountDatagrams` 保持 MsQuic 原有的 `uint8_t`
- 未保留 RTT-based dynamic batch

注意：batch=240 下观测到 `flush_last_datagrams` 约 `244`，距离 `uint8_t` 上限 255 仍有余量，但余量不大。后续如果还要把常量继续上调，必须同步审查 `TotalCountDatagrams` 以及 packet builder 中附加 datagram 的计数路径，避免 8-bit 溢出或更大突发导致 RTT 膨胀。

## upload 方向验证

前面的主要验证集中在 download 方向，即本机 curl 通过本机 tcpquic-proxy client，经 QUIC 到对端 tcpquic-proxy server，再从对端 HTTP/TCP 源读取数据返回本机。虽然 upload 和 download 都是 TCP-over-QUIC tunnel，但数据路径不同：

- download：对端 TCP 源 -> 对端 proxy server -> QUIC -> 本机 proxy client -> 本机 curl
- upload：对端 iperf3 client -> 对端 proxy client -> QUIC -> 本机 proxy server -> 本机 iperf3 server

因此需要单独验证 upload 方向，确认 tcpquic-proxy client 侧作为 QUIC 发送端时，`QUIC_MAX_DATAGRAMS_PER_SEND=240` 和新的 backpressure/diag 逻辑是否同样有效。

### upload 测试方法

测试部署：

- 本机启动 `iperf3 -s`，作为最终 TCP 测速服务器。
- 本机启动 `tcpquic-proxy server`。
- 对端 DGX 启动 `tcpquic-proxy client`。
- 对端 DGX 启动 `iperf3 -c`。
- netem 只加在对端 DGX egress 网卡 `enp1s0f0np0`，参数为 `delay/loss/limit 5000000`。

对端 `iperf3 -c` 明确使用 iperf3 的 HTTP CONNECT 代理功能，不是直连。实际命令形式：

```bash
iperf3 -c 169.254.250.230 \
  -B 169.254.59.196 \
  -p <iperf_port> \
  -t 30 \
  --json \
  --connect-timeout 5000 \
  --proxy http://127.0.0.1:<http_port>
```

其中 `--proxy http://127.0.0.1:<http_port>` 指向对端 `tcpquic-proxy client` 的 HTTP CONNECT 监听端口。

实际数据路径：

```text
对端 iperf3 -c
  -> iperf3 内置 HTTP CONNECT proxy
  -> 对端 tcpquic-proxy client
  -> QUIC
  -> 本机 tcpquic-proxy server
  -> 本机 iperf3 -s
```

注意：反向部署后，本机成为 QUIC server。原有 DGX 测试证书主要用于“对端做 server”的方向，本机做 server 时证书 SAN 需要包含 `169.254.250.230`。本轮 upload 测试重新生成了测试证书，server 证书包含：

- `IP:169.254.250.230`
- `IP:169.254.59.196`
- `DNS:localhost`

修正证书后，对端 proxy client 能正常监听 HTTP CONNECT/SOCKS/admin，并且 admin `/health` 显示 `connected_connections=1`。

### upload 测试结果

固定条件：

- 主线代码：`125b385`
- MsQuic 子模块：`8e11fb0`
- `QUIC_MAX_DATAGRAMS_PER_SEND=240`
- 1 条 QUIC connection
- 1 条 HTTP CONNECT / TCP stream
- iperf3 测试时长：30s
- netem limit：`5000000`
- proxy 参数：`--diag-stats --diag-stats-interval 5`

结果：

| 场景 | iperf rc | iperf sum_sent | iperf sum_received | qdisc dropped | 结果目录 |
| --- | --- | --- | --- | --- | --- |
| 100ms + 5% loss | 0 | 11094.21 Mbps | 10840.62 Mbps | 46116 | `docs/dgx-mainline-upload-iperf3-100ms-5loss-fixedcert-20260622` |
| 200ms + 5% loss | 0 | 7832.33 Mbps | 7578.67 Mbps | 33340 | `docs/dgx-mainline-upload-iperf3-200ms-5loss-fixedcert-20260622` |

发送端是对端 `tcpquic-proxy client`。其 diag 关键样本：

| 场景 | tcp_read_bytes | outstanding_quic_send_bytes | ideal_send_threshold_bytes | bytes_in_flight_max | BBR bw | srtt | cwnd | flush_last_datagrams | event_queue_full_errors |
| --- | --- | --- | --- | --- | --- | --- | --- | --- | --- |
| 100ms + 5% loss | 41605479273 | 0 | 0 | 496382579 | 10728.64 Mbps | 101.493ms | 275209745 | 1 | 0 |
| 200ms + 5% loss | 29372793508 | 0 | 0 | 874434867 | 22917.12 Mbps | 201.444ms | 150512384 | 1 | 0 |

传输中间的活跃样本显示：

- 100ms：`flush_last_datagrams=244`，`flush_scheduling` 持续增长，`event_queue_full_errors=0`
- 200ms：`flush_last_datagrams=244`，`flush_scheduling` 持续增长，`event_queue_full_errors=0`

结论：

- upload 方向已验证使用 iperf3 的 HTTP CONNECT 代理功能，不是 iperf3 直连。
- upload 100ms+5% 达到约 `11.09Gbps` 发送速率，200ms+5% 达到约 `7.83Gbps` 发送速率，均正常完成。
- 对端 proxy client 作为 QUIC 发送端时，`flush_last_datagrams=244`，说明主线 `QUIC_MAX_DATAGRAMS_PER_SEND=240` 生效。
- upload 测试没有观察到 `event_queue_full_errors`。
- 与最近主线 download 复测相比，upload 方向本轮表现更稳定；但 qdisc dropped 计数高于 download curl 测试，口径不同，不能直接把 iperf3 upload 与 curl download 做完全等价比较。

## 本次优化分析结论

本次 IO 吞吐探索的核心结论是：先解除 relay/MsQuic send buffer 入队侧的限制，再通过低开销 diag 证明应用层已经能把数据填到 MsQuic ideal/postbuf 附近，最终定位到 MsQuic send flush 单次 datagram batch 太小。真正带来明确吞吐提升的调整如下。

### 1. `QUIC_MAX_DATAGRAMS_PER_SEND: 40 -> 240`

这是本轮收益最大、证据最直接的参数。

`QUIC_MAX_DATAGRAMS_PER_SEND` 控制 MsQuic 一次 `QuicSendFlush()` 最多连续构造并提交多少个 UDP datagram。它不是发送窗口、flow control window 或 BBR/cwnd 参数，而是 MsQuic send path 的批量/调度粒度参数。

默认 `40` 时，200ms+5% 场景观察到：

- `flush_count=340638`
- `flush_scheduling=339236`
- `flush_last_datagrams=44`
- 吞吐 `3546.69 Mbps`，curl timeout

这说明绝大多数 flush 都不是被 pacing 或 BBR/cwnd 直接挡住，而是每次构造约 44 个 datagram 后就因为 batch 上限停止，然后反复重新调度。

单变量和邻近值验证：

| batch | 200ms+5% 结果 | 结论 |
| --- | --- | --- |
| 40 | 3546.69 Mbps，timeout | 默认值明显太小 |
| 160 | 4082.98 Mbps，timeout | 有提升但仍不足 |
| 200 | 5421.46 Mbps，timeout | 继续提升但仍不足 |
| 220 | 7478.73 Mbps，完成 | 接近有效区间 |
| 240 | 7269.02 / 8227.46 Mbps，完成 | 当前已测最优候选 |
| 250 | 4969.42 Mbps，timeout | 开始退化 |
| 260 | 2747.90 Mbps，timeout | 明显退化，RTT/队列副作用显著 |
| 480 | 4150.07 Mbps，timeout | 过大 batch 反而降低吞吐 |

因此，batch 不是越大越好。`40 -> 240` 的收益来自减少 send flush 重新调度次数，让一次 flush 能连续推进更多 UDP datagram；继续提高到 `250+` 后，过大突发、排队、RTT 膨胀、event queue 压力或计数边界副作用开始主导。

当前建议值：

- 固定 `QUIC_MAX_DATAGRAMS_PER_SEND=240`
- 保持 `QUIC_PACKET_BUILDER.TotalCountDatagrams` 为 MsQuic 原有 `uint8_t`
- 不采用 `250+`、`480` 或 RTT-based dynamic batch

### 2. `QUIC_MAX_IDEAL_SEND_BUFFER_SIZE: 1GiB -> 2GiB`

这个参数是必要的上限解除项，但不是单独决定最终吞吐的参数。

它允许 `QUIC_STREAM_EVENT_IDEAL_SEND_BUFFER_SIZE.ByteCount` 继续向更高台阶增长，避免 MsQuic ideal send buffer cap 过早卡住。配合 relay 使用 ideal send buffer 做背压后，发送端可以维持更大的 MsQuic posted buffer。

但测试也证明，单纯继续强行抬高 postbuf/read-ahead 不会自动提升吞吐：

- 将 initial read-ahead 强行抬到约 `1.47GiB` 后，吞吐下降。
- RTT 上升，出现新的排队副作用。
- event queue full 只是高 postbuf/高突发下的副作用，不是自然场景的根因。

因此，该参数的正确理解是“解除上限，让 MsQuic 能按实际在途数据增长 ideal”，而不是“越大吞吐越高”。

### 3. relay backpressure 改为基于 `QUIC_STREAM_EVENT_IDEAL_SEND_BUFFER_SIZE.ByteCount`

有效逻辑：

- pause TCP read：`OutstandingQuicSendBytes >= IdealSendBufferBytes`
- resume TCP read：`OutstandingQuicSendBytes < IdealSendBufferBytes`
- 去掉旧的 `relay-inflight-bytes` / `initial-quic-read-ahead` 主导计算
- 去掉 `RelayMaxInFlightSends` 对 TCP read 背压的限制

这个调整解决的是“应用层是否能按 MsQuic 当前建议持续填充 send buffer”的问题。后续 diag 证明：

- 发送端 relay 可以把 `outstanding_quic_send_bytes` 填到 `ideal_send_threshold_bytes` 附近。
- 应用层不是产生不了数据。
- relay 内部没有明显 `QuicSend` API 失败。

这个改动本身不是最终最大吞吐提升来源，但它是定位根因的必要前置条件。没有这个调整，问题会停留在 relay 入队不足/背压过早触发，无法进一步证明瓶颈在 MsQuic send flush batch。

### 4. relay receive resume 条件改为 `< relay_pending`

之前逻辑：

- `>= relay_pending` pause MsQuic receive
- `< relay_pending / 2` resume MsQuic receive

当前逻辑：

- `>= relay_pending` pause MsQuic receive
- `< relay_pending` resume MsQuic receive

这个调整减少了过强 hysteresis，避免 receive 侧暂停后必须等待队列下降到一半才恢复。该调整后，100ms/200ms 丢包场景的吞吐表现更符合“若内部窗口/缓冲不是瓶颈，则相同丢包率下不同 RTT 不应导致过度悬殊”的预期。

它是有效的稳定性/背压行为修正，但收益不如 `QUIC_MAX_DATAGRAMS_PER_SEND=240` 决定性。

### 5. 窗口和 relay buffer cap 的有效组合

当前有效组合：

- connection flow window：`2GiB`
- stream recv window：`2GiB`
- ideal send buffer cap：`2GiB`
- relay send buffer cap：`4GiB`

这些参数的作用是避免 flow control、stream recv window、connection flow window、relay buffer cap 成为瓶颈。测试中 `corecap=1GiB/relaycap=2GiB` 到 `corecap=2GiB/relaycap=4GiB` 有过提升，但波动较大，不能把最终吞吐提升完全归因于它们。

更准确的结论是：

- 这些参数是“解除上限”的基础配置。
- 最终把 100ms/200ms+5% 吞吐推上去的直接参数是 `QUIC_MAX_DATAGRAMS_PER_SEND=240`。
- 如果窗口/cap 太小，会挡住发送队列增长；但窗口/cap 足够大以后，继续扩大不一定提升吞吐。

### 未带来实际提升的方向

以下方向经过验证后不作为最终优化路径：

| 调整 | 结果 |
| --- | --- |
| 强行把 initial read-ahead/postbuf 提到约 `1.47GiB` | 吞吐下降，RTT 抬高 |
| event queue capacity 提到 `65536` | 可消除 event queue full，但吞吐没有提升 |
| BBR ProbeRTT 10s 改 60s | 没有提升，已回退 |
| `TotalCountDatagrams` 改 `uint16_t` | 100ms 场景明显退化 |
| RTT-based dynamic batch | 结果更差，已回退 |
| batch=250/260/480 | 均退化，说明 batch 不是越大越好 |

### 后续同类问题的分析方法建议

后续遇到类似“窗口和缓冲看起来足够大，但丢包/高 RTT 下吞吐仍低”的问题，不建议直接继续扩大窗口。推荐按以下顺序排查：

1. 先确认发送端应用层是否能持续填满 MsQuic ideal/postbuf：看 `outstanding_quic_send_bytes` 与 `ideal_send_threshold_bytes`。
2. 确认是否存在 relay 内部失败：看 `event_queue_full_errors`、`quic_send_failures`、`quic_send_api_failures`、`quic_send_fatal_errors`。
3. 确认 MsQuic 实际在途数据是否增长：看 `bytes_in_flight`、`bytes_in_flight_max`。
4. 区分是 pacing/BBR 阻塞还是 send flush 调度粒度限制：看 `flush_pacing_delayed`、`flush_cc_blocked`、`flush_scheduling`、`flush_last_datagrams`。
5. 如果 `flush_scheduling` 远高于 pacing/CC 阻塞，且 `flush_last_datagrams` 接近 batch 上限，应重点检查 send flush batch，而不是继续扩大 postbuf。
6. 调大 batch 后必须同时观察 `srtt`、`event_queue_full_errors` 和 qdisc/队列行为，避免把短期突发误判为有效吞吐提升。

本轮最终结论：**relay 背压跟随 MsQuic ideal send buffer、窗口/cap 放到不挡路以后，真正限制 2*DGX 1x1 高 RTT + 5% loss IO 吞吐的直接因素是 MsQuic 默认 `QUIC_MAX_DATAGRAMS_PER_SEND=40` 太小；当前已测最优候选为固定 `240`。**

## 当前核心判断

目前可以排除或降低优先级的方向：

- TCP 源端产生数据不足：不成立。发送端 relay 可以持续把 outstanding 填到 ideal。
- relay 内部 QuicSend API 失败：自然场景未观察到。
- event queue capacity：自然场景未触发；强行 1.47GiB 才触发，扩大后吞吐仍下降。
- 单纯继续扩大 postbuf：已验证会降低吞吐。
- ProbeRTT 单独解释：不成立。屏蔽 30 秒测试窗口内的 ProbeRTT 后吞吐未提高。
- pacing timer 单独解释：不成立。send flush 诊断中 `flush_pacing_delayed` 只有个位数。
- BBR/cwnd/recovery 直接阻塞单独解释：优先级下降。`flush_cc_blocked` 远小于 `flush_scheduling`。

当前最重要的问题已经从“server 侧拿不到 BBR stats”推进为“为什么 BBR 估计带宽较高时，实际 `bytes_in_flight` 和 `BytesInFlightMax` 没有持续跟上 posted/ideal”。下一步需要判断 `~653MiB/~980MiB` ideal 阈值停住到底是：

- 已确认一个直接原因：MsQuic `QUIC_MAX_DATAGRAMS_PER_SEND=40` 单次 flush batch 太小，导致大量 scheduling limited。
- batch=160/200/240 呈现逐步改善，batch=240 后 200ms+5% 达到 `7.27Gbps/8.23Gbps`，100ms+5% 达到 `9.58Gbps`。
- 邻近值验证中，batch=220 可正常完成但低于 batch=240；batch=250/260 已经明显退化，说明最优区间在 220 到 250 之间，当前已测最优点是 240。
- batch=480、`uint16_t` 放宽、RTT-based dynamic batch 都出现吞吐下降，说明继续无约束扩大 batch 会引入更大突发、排队和 RTT 膨胀。
- 当前候选值是固定 batch=240，并保持原有 `uint8_t` 计数类型。

## 2026-06-22 主线重复验证和未决问题

用户确认后，`230/235/245` 邻近值验证任务关闭。后续固定采用 `QUIC_MAX_DATAGRAMS_PER_SEND=240`，不再继续在邻近点消耗测试时间。已有 `220/240/250/260/480` 数据足够说明：`240` 是当前主线已测的最优候选点，继续增大 batch 会带来 RTT 膨胀、队列堆积或 event queue 副作用。

### 200ms+5% download 五轮重复验证

固定主线 batch=240，使用 curl download 方式重新跑 2*DGX 1x1、`200ms + 5% loss`、`limit 5000000`、`--diag-stats-interval 1`，结果目录：

```text
docs/dgx-mainline-download-200ms-5loss-repeat5-diag1s-20260622
```

五轮结果：

| 轮次 | 吞吐 | curl exit |
|---:|---:|---:|
| 1 | 5043.03 Mbps | 28 |
| 2 | 6975.04 Mbps | 0 |
| 3 | 3972.34 Mbps | 28 |
| 4 | 5075.07 Mbps | 28 |
| 5 | 4866.89 Mbps | 28 |

该组平均约 `5186.47 Mbps`。这说明 batch=240 虽然已经带来明确提升，但 `200ms + 5% loss` download 仍存在明显波动，部分轮次无法在 30s 窗口内完成 20GB 载荷。

该组 qdisc 采样中，多次观察到远端发送端 netem backlog 堆到数百 MB、数千到上万 packet，例如约 `438MB/6581p`、`635MB/9530p`、`799MB/11991p`。最后一轮 server diag 观测到：

- `srtt` 约 `201.481ms ~ 572.603ms`，平均约 `300.586ms`。
- `bbr_bw_mbps` 最高约 `21007.57 Mbps`，平均约 `13095.55 Mbps`。
- `cwnd` 最高约 `1196619440`。
- `bytes_in_flight` 最高约 `694183424`。
- `bytes_in_flight_max` 最高约 `862449216`。
- `ideal_send_threshold_bytes` 约 `980658109`。
- `event_queue_full_errors=0`。
- `flush_pacing_delayed=7`、`flush_cc_blocked=73`，没有显示 pacing/CC 直接阻塞占主导。

这个现象把未决问题收敛为：200ms 场景下 BBR 估计带宽并不低，应用层 post/ideal 也不小，但实际 `bytes_in_flight_max` 没有稳定跨过约 `980MiB` 的 ideal/postbuf 区间，同时 qdisc backlog 和 `srtt` 会明显膨胀。后续需要解释的是这组膨胀和波动的触发条件，而不是继续扩大 relay/postbuf。

### iperf3 reverse download 对照

为了避免 curl download 工具链影响判断，又用 iperf3 的 HTTP CONNECT proxy 功能跑同方向 download。拓扑为：

```text
远端 iperf3 -s
  -> 远端 tcpquic-proxy server
  -> QUIC download
  -> 本机 tcpquic-proxy client
  -> 本机 iperf3 -c --proxy http://127.0.0.1:<port> --reverse
```

该拓扑保持 QUIC 数据方向为远端发送、本机接收，与 download 一致。测试结果：

| 场景 | iperf3 rc | sent | received | 结果目录 |
|---|---:|---:|---:|---|
| 100ms + 5% loss | 0 | 10981.19 Mbps | 10828.88 Mbps | `docs/dgx-mainline-download-iperf3-reverse-100ms-5loss-rerun-20260622` |
| 200ms + 5% loss | 0 | 6488.54 Mbps | 6260.09 Mbps | `docs/dgx-mainline-download-iperf3-reverse-200ms-5loss-rerun-20260622` |

100ms 场景 server diag：

- `srtt` 约 `100.72ms ~ 144.82ms`，平均约 `104.12ms`。
- `bbr_bw_mbps` 最高约 `28922.12 Mbps`，平均约 `17093.35 Mbps`。
- `bytes_in_flight_max` 最高约 `465912105`。
- `ideal_send_threshold_bytes` 最高约 `1307544146`。
- qdisc backlog 最高约 `362.76MB/5712p`。

200ms 场景 server diag：

- `srtt` 约 `200.75ms ~ 651.73ms`，平均约 `236.91ms`。
- `bbr_bw_mbps` 最高约 `26965.42 Mbps`，平均约 `11943.69 Mbps`。
- `bytes_in_flight_max` 最高约 `837250048`。
- `ideal_send_threshold_bytes` 最高约 `1961316218`。
- qdisc backlog 最高约 `670.48MB/10556p`。

这组 iperf3 reverse 结果支持两个判断：

- 100ms 与 200ms 的差异不是 curl 独有现象；换成 iperf3 proxy reverse 后，200ms 仍显著低于 100ms。
- 200ms 场景的典型特征仍然是 qdisc backlog 和 RTT 膨胀更明显，`bytes_in_flight_max` 没有稳定追上 ideal/postbuf 上限。工具链差异会影响绝对吞吐，但不会改变当前未决问题的方向。

### BBR recovery 时间序列补充判断

对 iperf3 reverse 的 server 侧 1s diag 做时间序列抽样后，发现 `bbr_recovery_state=2` 在 100ms 和 200ms 活跃阶段都长期存在：

- 100ms：活跃样本 31 个，其中 29 个为 `recovery_state=2`，平均 `srtt` 约 `105.07ms`，最大 `bytes_in_flight_max` 约 `444MiB`。
- 200ms：活跃样本 30 个，其中 29 个为 `recovery_state=2`，平均 `srtt` 约 `258.48ms`，最大 `bytes_in_flight_max` 约 `798MiB`。

因此，`recovery_state=2` 本身不是区分 100ms 高吞吐和 200ms 低吞吐的充分条件。更准确的表述是：两种场景都在丢包恢复背景下运行，但 200ms 场景更容易伴随更大的 qdisc backlog、RTT 尖峰和 `bytes_in_flight` 回落。

200ms 时间序列中的典型现象：

- `srtt` 多次从约 `200ms` 抬升到 `357ms`、`386ms`、`618ms`、`652ms`。
- `posted` 多数时间维持在约 `895MiB ~ 938MiB`，说明应用层/relay 仍在持续给 MsQuic 足够数据。
- `bytes_in_flight` 会在 RTT 尖峰附近明显回落，例如低到几十 MiB，但 `bytes_in_flight_max` 仍维持在约 `686MiB ~ 798MiB` 区间。
- `flush_scheduling` 持续增长，`flush_cc_blocked` 也增长但数量级更小；这说明 batch=240 后调度粒度仍是重要观测项，但不再能单独解释 200ms 的全部波动。

下一步如果继续深挖 200ms download 波动，应该把 `bbr_recovery_state` 从“是否进入 recovery”改成“recovery 状态下的具体行为”来分析，包括 `recovery_window`、`pacing_gain` 周期、RTT 尖峰时 `bytes_in_flight` 为什么掉下去，以及 qdisc backlog 是先因还是后果。

### 步骤 1：高 RTT 样本拆分

对上面的 iperf3 reverse 样本继续拆分高 RTT/低 RTT 区间：

| 场景 | 样本分组 | 样本数 | 平均 srtt | 平均 bytes_in_flight | 平均 posted | 平均 BBR bw | 最大 bytes_in_flight_max |
|---|---|---:|---:|---:|---:|---:|---:|
| 100ms + 5% | `srtt <= 120ms` | 29 | 102.74ms | 182.8 MiB | 555.4 MiB | 19589.5 Mbps | 444.3 MiB |
| 100ms + 5% | `srtt > 120ms` | 2 | 135.18ms | 118.5 MiB | 416.2 MiB | 21101.2 Mbps | 373.7 MiB |
| 200ms + 5% | `srtt <= 300ms` | 26 | 218.67ms | 253.6 MiB | 701.2 MiB | 13852.0 Mbps | 798.5 MiB |
| 200ms + 5% | `srtt > 300ms` | 4 | 503.21ms | 111.0 MiB | 925.9 MiB | 19893.8 Mbps | 798.5 MiB |

200ms 高 RTT 样本的关键特征是：

- `posted` 仍然约 `925.9MiB`，说明应用层/relay 给 MsQuic 的数据并不缺。
- `BBR bw` 平均仍约 `19.89Gbps`，并不是带宽估计突然降低到实际 `6Gbps`。
- `bytes_in_flight` 平均只有约 `111MiB`，比 200ms 低 RTT 样本的 `253.6MiB` 还低。
- `bytes_in_flight_max` 保持约 `798.5MiB`，说明历史上能打到较高在途，但高 RTT 尖峰时当前在途会掉下去。

这一步的中间结论是：200ms 低吞吐不是因为应用层没有填队列，也不是因为 BBR 带宽估计直接降到低值；更像是 RTT/backlog/recovery 状态下，MsQuic 当前可持续发送的在途数据被周期性打断，导致 `bytes_in_flight` 从高位回落。下一步需要补采 `ss -tinmp` 和 qdisc 时间序列，确认远端 UDP 发送侧是否出现 socket 队列、qdisc backlog 或 pacing 层排队。

### 步骤 2：200ms 低吞吐样本的 qdisc/ss 补采

继续固定 batch=240，补跑一轮 2*DGX 1x1 download、`200ms + 5% loss`、`limit 5000000`，同时采样远端发送侧：

- qdisc：`tc -s qdisc show dev enp1s0f0np0`
- UDP 4433 socket：`ss -u -a -n -m -p`
- 远端 HTTP 源到 proxy 的 TCP socket：`ss -t -i -n -m -p`
- server diag：`--diag-stats --diag-stats-interval 1`

结果目录：

```text
docs/dgx-mainline-download-200ms-5loss-ss-qdisc-diag-20260622
```

本轮 curl 结果：

| 指标 | 值 |
|---|---:|
| `speed_download` | 537715577 B/s |
| 折算吞吐 | 4301.72 Mbps |
| `time_total` | 30.001045s |
| `size_download` | 16132029242 bytes |
| curl exit | 28 |

这是一个典型的 200ms 低吞吐/timeout 样本。

远端 qdisc 采样：

| 指标 | 值 |
|---|---:|
| qdisc drop delta | 14265 |
| qdisc backlog max | 569.55 MB |
| qdisc backlog max packets | 8967 |
| 测试结束 qdisc dropped | 28313 |

server diag：

| 指标 | 值 |
|---|---:|
| `srtt` | 200.967ms ~ 804.630ms，平均 302.766ms |
| `bbr_bw_mbps` | 89.52 ~ 21881.78 Mbps，平均 12626.38 Mbps |
| `bytes_in_flight` | 2944 ~ 694151040 bytes，平均 219994795 bytes |
| `bytes_in_flight_max` | 2454000 ~ 857571695 bytes |
| `posted` | 122681113 ~ 984113601 bytes，平均 758290662 bytes |
| `ideal_send_threshold_bytes` | 134217728 ~ 980658109 bytes |
| `flush_scheduling` | 4 ~ 22144 |
| `flush_cc_blocked` | 5 ~ 202 |
| `flush_pacing_delayed` | 6 ~ 16 |

高 RTT 样本中继续出现“posted 高、当前在途低”的特征：

| 样本 | srtt | BBR bw | bytes_in_flight | posted | bytes_in_flight_max | recovery | recovery_window |
|---:|---:|---:|---:|---:|---:|---:|---:|
| 11 | 805ms | 14575 Mbps | 37 MiB | 776 MiB | 799 MiB | 2 | 1861 MiB |
| 12 | 697ms | 14575 Mbps | 55 MiB | 880 MiB | 799 MiB | 2 | 2218 MiB |
| 20 | 695ms | 15584 Mbps | 69 MiB | 937 MiB | 818 MiB | 2 | 4027 MiB |
| 26 | 697ms | 12531 Mbps | 128 MiB | 883 MiB | 818 MiB | 2 | 4044 MiB |
| 33 | 309ms | 12376 Mbps | 10 MiB | 851 MiB | 818 MiB | 2 | 1004 MiB |

`ss` 观察：

- UDP 4433 socket 的 `Recv-Q/Send-Q` 显示为 `0/0`，`skmem` 中 `w0,o0,bl0`，没有看到 UDP socket 发送队列自身明显堆积。
- 远端 HTTP 源到 proxy 的本机 TCP 连接有 `notsent` 和接收队列堆积，例如 HTTP sender 侧 `notsent` 约 `31MB`，proxy 接收侧 `Recv-Q` 约 `34MB`。这更像是 QUIC 发送节奏降低后，proxy 对上游 TCP 读取被反压造成的结果；它说明应用/TCP 源可以供给数据，但不是当前低吞吐的首要根因。

这一步的中间结论是：200ms 低吞吐时，瓶颈不表现为 UDP socket send queue 堆积，也不表现为应用层数据不足；更直接的外部症状是远端 netem qdisc backlog 堆到数百 MB，同时 MsQuic diag 中 `bytes_in_flight` 在高 RTT 尖峰时从高位掉到几十 MiB。下一步需要用 100ms 同采样方式做对照，确认差异是否主要集中在 qdisc backlog/RTT 尖峰强度，而不是 socket 队列。

### 步骤 3：100ms 同采样对照

使用与步骤 2 相同的采样方式，只把 netem delay 从 `200ms` 改成 `100ms`，继续保持 `5% loss`、`limit 5000000`、batch=240。

结果目录：

```text
docs/dgx-mainline-download-100ms-5loss-ss-qdisc-diag-20260622
```

curl 结果：

| 场景 | speed_download | 折算吞吐 | time_total | size_download | curl exit |
|---|---:|---:|---:|---:|---:|
| 100ms + 5% | 1089389321 B/s | 8715.11 Mbps | 19.712729s | 21474836480 bytes | 0 |
| 200ms + 5% | 537715577 B/s | 4301.72 Mbps | 30.001045s | 16132029242 bytes | 28 |

qdisc 对比：

| 场景 | qdisc samples | drop delta | backlog max | backlog max packets |
|---|---:|---:|---:|---:|
| 100ms + 5% | 10 | 23591 | 216.83 MB | 3416 |
| 200ms + 5% | 4 | 14265 | 569.55 MB | 8967 |

server diag 对比：

| 场景 | active diag | srtt | BBR bw avg | bytes_in_flight avg | bytes_in_flight_max max | posted avg | ideal threshold max |
|---|---:|---:|---:|---:|---:|---:|---:|
| 100ms + 5% | 20 | 100.93ms ~ 224.67ms，平均 122.38ms | 17640.07 Mbps | 185451022 bytes | 470733824 bytes | 527900993 bytes | 653772073 bytes |
| 200ms + 5% | 34 | 200.97ms ~ 804.63ms，平均 302.77ms | 12626.38 Mbps | 219994795 bytes | 857571695 bytes | 758290662 bytes | 980658109 bytes |

高 RTT 样本对比：

| 场景 | 高 RTT 条件 | 样本数 | 高 RTT 平均 bytes_in_flight | 高 RTT 平均 posted |
|---|---|---:|---:|---:|
| 100ms + 5% | `srtt > 150ms` | 3 | 75.49 MiB | 569.01 MiB |
| 200ms + 5% | `srtt > 300ms` | 9 | 131.65 MiB | 879.73 MiB |

100ms 也会出现短暂高 RTT 样本，例如 `srtt=224ms/225ms/207ms`，但持续时间短，整体仍能在 19.7s 内完成 20GB。200ms 则高 RTT 样本更多，峰值达到 `804.63ms`，且测试结束时仍 timeout。

关于 qdisc backlog 的解释需要更精确：

- 这里的 qdisc 是 netem root qdisc，backlog 包含 netem 为模拟 delay 而持有的报文，不等价于真实物理网卡排队。
- 但在相同 `5% loss`、相同 `limit 5000000`、相同应用/QUIC 参数下，200ms 样本的 backlog 峰值显著高于 100ms，且伴随更高的 `srtt` 尖峰和 timeout。
- 因此 backlog 不能单独作为“网卡拥塞”的证据，但可以作为 netem 延迟队列和发送突发强度的外部观测指标。

这一步的中间结论是：100ms/200ms 差异不在 UDP socket 队列，也不在应用层供给；差异更集中在 netem 延迟队列更深、RTT 尖峰更强、以及高 RTT 期间 `bytes_in_flight` 回落更频繁。下一步要继续区分：这是 BBR 在高 RTT/loss 下的恢复和 pacing 行为，还是 MsQuic send flush batch=240 的突发形态在 200ms netem 下放大了队列和 RTT。

### 步骤 4：MsQuic 发送路径代码解释

结合 `third_party/msquic/src/core/send.c`、`packet_builder.c`、`bbr.c`、`loss_detection.c` 的代码，当前诊断计数含义如下。

`QUIC_MAX_DATAGRAMS_PER_SEND` 的位置：

- `packet_builder.c` 在新建 datagram/UDP payload 前检查 `Builder->TotalCountDatagrams >= QUIC_MAX_DATAGRAMS_PER_SEND`，达到上限后返回失败。
- `send.c::QuicSendFlush()` 的主循环条件是 `Builder.SendData != NULL || Builder.TotalCountDatagrams < QUIC_MAX_DATAGRAMS_PER_SEND`。
- 如果循环退出时 `Result` 仍是 `QUIC_SEND_INCOMPLETE`，说明还有数据可发，但本轮 flush 被调度粒度限制截断；随后设置 `QUIC_FLOW_BLOCKED_SCHEDULING` 并通过 `QuicSendQueueFlush(..., REASON_SCHEDULING)` 重新排队。
- 我们的 `flush_scheduling` 计数正是这个分支。

pacing/CC 的位置：

- `QuicPacketBuilderHasAllowance(&Builder)` 失败时，说明当前 packet builder 的发送 allowance 用完。
- 如果此时 `QuicCongestionControlCanSend()` 仍为 true，说明拥塞控制还允许发送，但当前 pacing chunk 用完，于是设置 `QUIC_FLOW_BLOCKED_PACING` 和 pacing timer；我们的 `flush_pacing_delayed` 对应这个分支。
- 如果 `QuicCongestionControlCanSend()` 为 false，则当前拥塞控制不允许继续发新数据；我们的 `flush_cc_blocked` 对应这个分支。

BBR `CanSend` 的核心判断：

```text
BbrCongestionControlCanSend:
  CongestionWindow = BbrCongestionControlGetCongestionWindow()
  return BytesInFlight < CongestionWindow || Exemptions > 0

BbrCongestionControlGetCongestionWindow:
  ProbeRTT: MinCongestionWindow
  InRecovery: min(CongestionWindow, RecoveryWindow)
  Otherwise: CongestionWindow
```

因此，在 BBR recovery 中，真正能否继续发新数据取决于：

```text
BytesInFlight < min(CongestionWindow, RecoveryWindow)
```

loss detection 的关系：

- `loss_detection.c` 对已发送包做 FACK/RACK 判断。
- FACK：包号落后 `LargestAck` 超过 packet reorder threshold。
- RACK：包号小于 `LargestAck`，并且 `SentTime + TimeReorderThreshold <= now`。
- `TimeReorderThreshold` 基于 `max(SmoothedRtt, LatestRttSample)` 计算。
- 一旦识别出 lost retransmittable bytes，会调用 `QuicCongestionControlOnDataLost()`，BBR 中会减少 `BytesInFlight`，更新 `RecoveryWindow`，并 `QuicSendQueueFlush(..., REASON_LOSS)` 触发重发/继续发送。

结合当前数据，能确认的边界是：

- `flush_scheduling` 仍然是最大计数项，说明 batch=240 后仍有大量 flush 是因为单轮 batch 用完而重新排队。
- 但 200ms 低吞吐样本里 `flush_pacing_delayed` 和 `flush_cc_blocked` 仍显著小于 `flush_scheduling`，不能简单说“拥塞窗口直接挡住了绝大多数发送”。
- 高 RTT 样本里 `posted` 仍高、BBR bw 仍高，但当前 `bytes_in_flight` 会掉到几十 MiB；从代码逻辑看，这可能来自 lost packet 被确认后 `BytesInFlight` 扣减、recovery window 更新、pacing allowance 分片，以及 batch 重新调度之间的组合效应。
- 当前证据还不能证明 MsQuic loss detection/retransmit 有 bug；只能说明在 200ms+5% 下，loss/recovery/RTT 尖峰会把当前在途数据周期性打低，而 batch=240 的突发/重新调度会影响这种状态的放大或恢复速度。

这一步的中间结论是：下一轮如果继续定位根因，应该补充“loss 事件粒度”的诊断，例如每秒 lost bytes、lost packet count、retransmit bytes、`LargestAck` 推进、FACK/RACK 触发次数。仅靠当前 1s BBR stats 还无法判断 `bytes_in_flight` 回落是合理 loss accounting，还是过早/过多 loss detection 导致。

### 步骤 5：增加 loss detection 诊断字段

为了继续定位 200ms 高 RTT 时 `bytes_in_flight` 回落的原因，本轮在诊断路径中新增 loss detection 计数。该修改只增加统计字段和日志输出，不改变发送、拥塞控制、丢包检测或重传行为。

新增 MsQuic 内部统计字段：

- `LossDetectionEventCount`：检测到 retransmittable lost bytes 的 loss batch 次数。
- `LossDetectionFackPacketCount`：通过 packet threshold/FACK 判定为 lost 的 packet 数。
- `LossDetectionRackPacketCount`：通过 time threshold/RACK 判定为 lost 的 packet 数。
- `LostRetransmittableBytes`：累计被标记 lost 的 retransmittable bytes。
- `LastLostRetransmittableBytes`：最近一次 loss batch 的 lost retransmittable bytes。

修改位置：

- `third_party/msquic/src/core/connection.h`：在 `Connection->Stats.SendDiag` 增加 loss 统计字段。
- `third_party/msquic/src/inc/msquic.h`：在 `QUIC_CONNECTION_EVENT_NETWORK_STATISTICS` 增加对应事件字段。
- `third_party/msquic/src/core/loss_detection.c`：在 FACK/RACK 分支和 `LostRetransmittableBytes > 0` 分支累计统计。
- `third_party/msquic/src/core/bbr.c`：把 `SendDiag` 中的 loss 字段带到 `NETWORK_STATISTICS` 事件。
- `src/runtime/trace.h` / `src/runtime/trace.cpp`：在 `TqTraceNetworkStats` 和 `net_stats` 文本中输出 loss 字段。
- `src/protocol/quic_session.cpp`：client/server 两侧 network stats 回调透传新字段。
- `src/unittest/trace_network_stats_test.cpp`：增加格式化输出断言。

新增输出字段：

```text
loss_events=<n>
loss_fack_packets=<n>
loss_rack_packets=<n>
lost_retransmittable_bytes=<n>
loss_last_bytes=<n>
```

验证：

```text
cmake --build build-plan --target tcpquic-proxy -j4
cmake --build build-plan --target tcpquic_trace_network_stats_test -j4
./build-plan/bin/Release/tcpquic_trace_network_stats_test
```

结果：`tcpquic-proxy` 构建通过，`tcpquic_trace_network_stats_test` 运行返回 0。

### 步骤 6：200ms lossdiag 首轮结果

使用新增 loss diag 字段后，重新跑一轮 2*DGX 1x1 download、`200ms + 5% loss`、`limit 5000000`。本轮不采 `ss`，只采 qdisc 和 1s `diag_stats`，避免采样开销影响吞吐。

结果目录：

```text
docs/dgx-mainline-download-200ms-5loss-lossdiag-20260622
```

curl 结果：

| 指标 | 值 |
|---|---:|
| `speed_download` | 757350587 B/s |
| 折算吞吐 | 6058.80 Mbps |
| `time_total` | 28.355212s |
| `size_download` | 21474836480 bytes |
| curl exit | 0 |

server diag 摘要：

| 指标 | 值 |
|---|---:|
| active diag samples | 28 |
| `srtt` | 200.58ms ~ 328.94ms，平均 214.55ms |
| `bbr_bw_mbps` | 0.28 ~ 27219.99 Mbps，平均 14578.77 Mbps |
| `bytes_in_flight` | 0 ~ 578700608 bytes，平均 184915797 bytes |
| `bytes_in_flight_max` | 1220000 ~ 862304012 bytes |
| `posted` | 130119750 ~ 983707700 bytes，平均 528518235 bytes |
| `ideal_send_threshold_bytes` | 134217728 ~ 980658109 bytes |
| `flush_scheduling` | 0 ~ 20170 |
| `flush_cc_blocked` | 2 ~ 418 |
| `flush_pacing_delayed` | 5 ~ 45 |

新增 loss diag 摘要：

| 指标 | 值 |
|---|---:|
| `loss_events` | 1 ~ 20732 |
| `loss_fack_packets` | 0 ~ 1285722 |
| `loss_rack_packets` | 1 ~ 34 |
| `lost_retransmittable_bytes` | 18 ~ 1892400554 bytes |
| `loss_last_bytes` | 18 ~ 15102720 bytes |

本轮新增字段给出的关键信息：

- 200ms 场景下，loss detection 主要由 FACK/packet-threshold 驱动，`loss_fack_packets=1285722`，而 `loss_rack_packets=34` 很少。
- 累计 `lost_retransmittable_bytes` 约 `1.89GB`，约为本轮 20GB 下载量的 `8.8%`。该值包含 QUIC 层按 retransmittable bytes 统计的丢失数据，不应直接等同于 qdisc 的 UDP skb dropped 计数。
- 高 RTT 样本里，loss delta 很大。例如：
  - `srtt=329ms` 时，前后 1s 内 `loss_fack_packets` 增量约 `136477`，`lost_retransmittable_bytes` 增量约 `191.6MiB`。
  - `srtt=307ms` 时，前后 1s 内 `loss_fack_packets` 增量约 `137191`，`lost_retransmittable_bytes` 增量约 `192.6MiB`。
- 同时这些高 RTT 样本仍然显示 `BBR bw` 很高，分别约 `22.7Gbps` 和 `27.1Gbps`，说明“带宽估计低”不是直接解释。

qdisc 摘要：

| 指标 | 值 |
|---|---:|
| qdisc samples | 5 |
| drop delta | 24776 |
| backlog max | 406.15 MB |
| backlog max packets | 6400 |

这一步的中间结论是：`bytes_in_flight` 回落和高 RTT 样本附近确实伴随大批量 FACK loss detection。当前更具体的未决问题变成：这些 FACK loss 是 5% 丢包 + 大 batch/high BDP 下合理产生的结果，还是 200ms 场景中 ACK 压缩、packet threshold、burst 形态共同导致的过度 loss 判断。下一步需要跑 100ms lossdiag 对照，比较相同丢包率下 FACK/RACK/lost bytes 的比例。

### 步骤 7：100ms/200ms lossdiag 对照

继续使用新增 loss diag 字段，补跑同口径 `100ms + 5% loss` 对照。

结果目录：

```text
docs/dgx-mainline-download-100ms-5loss-lossdiag-20260622
docs/dgx-mainline-download-200ms-5loss-lossdiag-20260622
```

吞吐对比：

| 场景 | 吞吐 | time_total | size_download | curl exit |
|---|---:|---:|---:|---:|
| 100ms + 5% | 9699.43 Mbps | 17.712251s | 21474836480 bytes | 0 |
| 200ms + 5% | 6058.80 Mbps | 28.355212s | 21474836480 bytes | 0 |

server diag 对比：

| 场景 | active diag | srtt max/avg | BBR bw avg | bytes_in_flight avg | bytes_in_flight_max max | posted avg | ideal max |
|---|---:|---:|---:|---:|---:|---:|---:|
| 100ms + 5% | 17 | 122.04ms / 102.72ms | 19665.88 Mbps | 173410175 bytes | 381483520 bytes | 356241142 bytes | 435848049 bytes |
| 200ms + 5% | 28 | 328.94ms / 214.55ms | 14578.77 Mbps | 184915797 bytes | 862304012 bytes | 528518235 bytes | 980658109 bytes |

loss diag 对比：

| 场景 | loss_events | loss_fack_packets | loss_rack_packets | lost_retransmittable_bytes | lost/download |
|---|---:|---:|---:|---:|---:|
| 100ms + 5% | 21093 | 1119566 | 18 | 1647915046 bytes | 7.67% |
| 200ms + 5% | 20732 | 1285722 | 34 | 1892400554 bytes | 8.81% |

qdisc 对比：

| 场景 | qdisc samples | drop delta | backlog max | backlog max packets |
|---|---:|---:|---:|---:|
| 100ms + 5% | 7 | 23736 | 234.55 MB | 3694 |
| 200ms + 5% | 5 | 24776 | 406.15 MB | 6400 |

这组对照修正了上一步的判断：

- FACK/packet-threshold loss detection 不是 200ms 场景独有。100ms 高吞吐样本也有 `loss_fack_packets=1119566` 和约 `1.65GB` lost retransmittable bytes。
- RACK/time-threshold loss 很少，100ms 为 `18`，200ms 为 `34`。因此当前瓶颈优先级不在 RACK time threshold 过早触发。
- 200ms 的 lost bytes 比例更高一些，但不是数量级差异：`8.81%` vs `7.67%`。
- 真正拉开吞吐的是同类 FACK/loss 背景下，200ms 的 qdisc backlog、srtt 和 ideal/postbuf 都更高，且高 RTT 样本出现 `bytes_in_flight` 明显回落。

当前更收敛的解释是：

```text
5% loss 下，两种 RTT 都会触发大量 FACK loss；
100ms 场景可以较快恢复，RTT 尖峰不明显，吞吐接近 9.7Gbps；
200ms 场景的更大 BDP/更深 netem 延迟队列把同类 loss/recovery 放大，
导致 qdisc backlog 更高、srtt 尖峰更明显、当前 bytes_in_flight 周期性掉低，
最终吞吐下降到约 6Gbps。
```

所以，下一步不应再把重点放在“是否存在 FACK loss”这个二值问题上，而应观察 FACK loss 后的恢复速度：`bytes_in_flight` 从低位恢复到 `bytes_in_flight_max/ideal` 需要多久，恢复期间是否被 batch 重新调度、pacing allowance 或 ACK 节奏限制。

## 下一步计划

已关闭：

1. `230/235/245` 邻近值验证：用户已决定固定采用 `240`，任务关闭。
2. ProbeRTT 单独解释：已验证不成立。
3. 单纯继续扩大窗口、postbuf、relay 队列：已验证不是正确方向，过大反而降低吞吐。
4. iperf3 proxy receive path 作为根因：历史 `gost`/minimal sink 验证和本轮 iperf3 reverse 对照都不支持该方向作为主要根因。
5. RACK/time-threshold 过早触发作为主要根因：当前 lossdiag 中 `loss_rack_packets` 很少，100ms 为 `18`，200ms 为 `34`，优先级下降。
6. “是否存在 FACK loss”作为二值问题：已确认 100ms/200ms 都有大量 FACK loss，不能单独解释 200ms 低吞吐。

仍需继续验证：

1. FACK loss 后的恢复速度。需要观察 `bytes_in_flight` 从几十/百 MiB 恢复到 `bytes_in_flight_max/ideal` 需要多久，以及恢复期间 `flush_scheduling`、`flush_pacing_delayed`、`flush_cc_blocked` 的增量。
2. ACK 节奏和 packet-threshold/FACK 的关系。当前 200ms 下 FACK loss 比例略高，且高 RTT 样本附近出现单秒约 `190MiB` lost bytes 增量；需要判断是否有 ACK 压缩或 burst 形态导致 packet-threshold 更集中触发。
3. qdisc backlog 的形成机制。netem backlog 包含延迟队列，不等价于物理网卡拥塞；但 200ms backlog 峰值高于 100ms，需要继续判断它是正常 BDP 表现，还是 batch=240 突发被 200ms netem 放大。
4. `bytes_in_flight_max` 为什么没有稳定跨过约 `980MiB`。目前看不是应用层供给不足，也不是 RACK 主导，更可能是 FACK loss/recovery、pacing allowance、batch 重新调度和 ACK 节奏共同限制恢复速度。
5. download 与 upload 的差异。upload 已验证可达到约 `10.84Gbps`/`7.58Gbps`，download 在 200ms 下仍波动更大；后续需要用同一 lossdiag 采样粒度比较 upload 发送端。
6. 是否需要把 `QUIC_MAX_DATAGRAMS_PER_SEND=240` 做成 tcpquic-proxy 构建/运行配置。当前主线先固定 240；如果后续需要跨环境部署，再评估可配置化。
