# tcpquic-proxy 参数重构建议

日期：2026-06-22

## 背景

根据 `docs/io-throughput-diag_cn.md` 的诊断结果，当前 relay 背压模型已经发生变化：

- TCP -> QUIC 方向不再以历史的 `relay-inflight-bytes` / `initial-quic-read-ahead` 计算背压阈值，而是以 MsQuic 上报的 `QUIC_STREAM_EVENT_IDEAL_SEND_BUFFER_SIZE.ByteCount` 为主要依据。
- QUIC -> TCP 方向的 receive 背压以 `relay_pending` 为上限，本轮测试中 `512MiB` 不触发 receive pause，`128MiB` 会触发 pause 并带来下载不完整风险。
- `StreamRecvWindowDefault` 和 `ConnFlowControlWindow` 当前固定为 `2GiB` 验证值。

因此，历史上用于高 BDP 手动堆队列的一系列命令行 tuning 参数，不应再作为推荐配置继续暴露给普通用户。

## 总体结论

原来的 tuning 参数不应该再作为“必须手动调大才能跑满”的主路径保留。更合理的处理方式是：

1. 保留少数仍直接影响运行行为的 profile/诊断参数。
2. 对已经失效、容易误导、或与当前背压模型冲突的参数，从代码中删除其配置影响。
3. 更新脚本和文档，不再推荐历史的 `--tuning custom --fcw --srw --relay-inflight-bytes --initial-quic-read-ahead ...` 长参数组合。

推荐的普通运行方式应尽量接近：

```bash
tcpquic-proxy ... --tuning wan
```

需要诊断时再显式增加：

```bash
--diag-stats --diag-stats-interval 1
```

高 RTT/loss 场景中，`--iw` / `--initrtt-ms` 可作为实验/诊断参数保留，但不应作为普通用户必须配置的参数。2026-06-23 的对照测试显示：

- tcpquic-proxy 当前代码默认仍是 `wan` profile，即 `iw=2000`、`initrtt=100ms`。
- MsQuic 自身默认值是 `iw=10`、`initrtt=333ms`，但 tcpquic-proxy 默认不会走这两个值。
- 从当前测试对比看，`iw=4000`、`initrtt=100ms` 的效果相对较好，尤其在 `100ms + 5% loss` 下明显优于 MsQuic 默认值组合。
- 这些参数仍建议定位为实验性质，而不是长期吞吐主控参数。

## 参数分级

### 应从代码中删除影响

#### `--fcw <bytes>` / `--srw <bytes>`

当前已基本是 no-op。

代码中 `TqComputeTuning()` 最终强制：

- `StreamRecvWindow = TqValidationFlowWindowBytes`
- `ConnFlowControlWindow = TqValidationFlowWindowBytes`

MsQuic settings 也直接使用 `TqValidationFlowWindowBytes` 设置 stream recv window 和 connection flow control window。因此命令行上的 `--fcw` / `--srw` 覆盖不会再决定实际窗口。

建议：

- 从推荐文档和测试脚本中移除。
- 从配置结构和 tuning override 逻辑中删除对应影响。
- CLI 层后续实现时可直接删除解析；如需要短期兼容，也只能接受并忽略，不能再改变实际窗口。

#### `--relay-inflight-bytes <n>`

不再适合作为用户可见的“背压阈值”参数。

当前 TCP -> QUIC 背压逻辑是：

- pause TCP read: `OutstandingQuicSendBytes >= IdealSendBufferBytes`
- resume TCP read: `OutstandingQuicSendBytes < IdealSendBufferBytes`

其中 `IdealSendBufferBytes` 来自 MsQuic stream ideal send buffer 事件。`RelayMaxInFlightSends` 不再参与 TCP read 背压判断。

`--relay-inflight-bytes` 现在仍可能影响：

- `RelayDefaultIdealSend`
- `RelayMaxInFlightSends`
- send context/free context 数量
- 部分派生预算

但这个名字已经明显误导，会让用户以为它仍是 relay 背压主阈值。

建议：

- 从 CLI、配置结构和 tuning override 逻辑中删除。
- 不再通过该参数影响 `RelayDefaultIdealSend`、`RelayMaxInFlightSends` 或 send context/free context 数量。
- 如后续确实需要内部调试 send context budget，应另建明确的 debug 参数，而不是复用 `relay-inflight` 语义。

#### `--initial-quic-read-ahead <bytes>`

不应再推荐给用户用于提高吞吐。

诊断记录中，强行将 initial read-ahead 提高到 `1.47GiB` 后：

- outstanding QUIC send bytes 确实被抬高。
- 默认 event queue 下出现 `event_queue_full_errors`。
- 扩大 event queue 后错误消失，但吞吐进一步下降。
- client RTT 上升到约 300ms。

结论是：继续扩大发送队列不是当前有效方向，反而会增加排队和 RTT。

建议：

- 从普通 CLI、配置结构和 tuning override 逻辑中删除其影响。
- 不再允许用户通过该参数强行抬高 postbuf/read-ahead。
- 默认 initial/fallback read-ahead 留在代码内部，由当前固定默认和 MsQuic ideal-send 事件驱动背压共同决定。

#### `--target-bandwidth-mbps <n>` / `--target-rtt-ms <n>`

这两个参数来自旧的 BDP 计算路径。当前高 RTT/loss 吞吐瓶颈已经收敛到 MsQuic ideal-send、bytes-in-flight、loss/recovery、pacing/ACK 节奏等运行时行为，继续通过目标带宽/RTT 推导 relay send 队列和窗口会制造误导。

建议：

- 从 CLI、配置结构和 tuning 计算路径中删除影响。
- `wan` / `lan` / `auto` profile 保留为粗粒度策略，不再依赖用户输入目标带宽/RTT 推导高 BDP 参数。
- 如后续需要链路估计，应来自 `--diag-stats` / runtime observations，而不是静态命令行 BDP hint。

#### `--tuning custom`

`custom` 的主要用途是让用户组合一系列底层覆盖参数；这些覆盖参数大多已经不适合当前模型。继续保留 `custom` 会鼓励用户回到手动堆窗口、堆队列的调参方式。

建议：

- 从 CLI 和配置枚举中删除 `custom` 模式的行为影响。
- 后续只保留 `wan`、`lan`、`auto` 三种 profile。
- 旧配置或脚本中的 `--tuning custom` 应迁移到 `--tuning wan` 或更明确的实验参数。

### 应保留

#### `--tuning wan|lan|auto`

`--tuning` 仍有价值，但只保留 `wan`、`lan`、`auto`，不再保留 `custom`。

它目前还影响：

- `InitialWindowPackets`
- `InitialRttMs`
- relay IO size / batch / worker budget
- Linux relay pending/buffer 派生值
- runtime 保守下调逻辑

建议：

- 保留 `wan` 作为默认。
- 保留 `lan` 用于低 RTT 小队列场景。
- 保留 `auto` 作为粗粒度 profile。
- 删除 `custom`，避免继续鼓励用户手动拼大量高 BDP 覆盖参数。

#### `--max-memory-mb <n>`

仍有保留价值。它控制 relay pool memory budget，并影响 active tunnel 下的资源分配。

建议：

- 保留。
- 文档中强调这是资源保护参数，不是吞吐调优优先项。

### 应保留为实验/诊断参数

#### `--iw <packets>`

仍会传给 MsQuic `InitialWindowPackets`。

2026-06-23 的 1 connection / 1 stream / 20GB download 对照中：

- `100ms + 5% loss`：default 平均 `9058.77 Mbps`，`--iw 4000 --initrtt-ms 100` 平均 `9501.37 Mbps`，差异小于 default 自身波动范围。
- `200ms + 5% loss`：default 平均 `6419.11 Mbps`，`--iw 4000 --initrtt-ms 200` 平均 `5067.06 Mbps`，显式设置组反而更低，且三轮均 curl 28 超时。
- MsQuic 默认值对照 `--iw 10 --initrtt-ms 333` 单轮结果：`100ms + 5% loss` 为 `7320.24 Mbps`，`200ms + 5% loss` 为 `6857.03 Mbps`。

这说明 `--iw` 仍是实际生效的启动期参数，但目前没有证据表明它是当前高吞吐瓶颈的长期主控项。若需要保留一个实验候选值，当前数据更支持 `--iw 4000`，而不是 MsQuic 默认的 `--iw 10`。

建议：

- 保留。
- 作为启动期实验/诊断参数，而非长期吞吐主控参数。
- 普通运行不要求用户显式设置。
- 实验场景可优先使用 `--iw 4000` 作为候选值。
- 文档说明过大可能带来突发丢包风险。

#### `--initrtt-ms <n>`

仍会传给 MsQuic `InitialRttMs`，并且高 RTT/loss 场景下可能影响启动收敛。

同一轮对照中，显式设置 `--initrtt-ms 100` / `--initrtt-ms 200` 没有带来稳定吞吐收益。补充的 MsQuic 默认值对照 `--initrtt-ms 333` 在 `100ms + 5% loss` 下明显低于 `--initrtt-ms 100` 组合；在 `200ms + 5% loss` 单轮表现尚可，但样本不足，不能据此改成推荐值。

当前结果没有测试 `--initrtt-ms 400`。因此结论应限制为：在已有 100ms/200ms + 5% loss download 样本中，`--initrtt-ms` 适合作为启动期实验参数；如果要选实验候选值，`100ms` 场景下 `--initrtt-ms 100` 更合适。

建议：

- 保留。
- 只在明确要验证 MsQuic startup 行为或启动期收敛时使用。
- 普通 DGX/netem 高吞吐测试默认不设置。
- 实验场景可优先用 `--initrtt-ms 100` 作为候选值，再按 RTT 做对照。

#### `--linux-relay-read-chunk-size <bytes>`

这不是 QUIC 背压参数，而是 Linux relay TCP read chunk 大小，仍会直接进入 worker config。

建议：

- 保留为平台 IO 形态参数。
- 不放在普通用户推荐命令里。
- benchmark/诊断脚本可继续按场景使用。

#### `--linux-relay-tcp-write-max-bytes <bytes>` / `--linux-relay-tcp-write-burst-bytes <bytes>`

这两个参数控制 Linux relay TCP write flush 行为，仍会直接进入 worker config。

建议：

- 保留为诊断/实验参数。
- 默认关闭。
- 不作为通用吞吐推荐项。

## `relay_pending` 策略

当前结论支持将 QUIC -> TCP receive 方向的 per-relay pending 上限固定为 `512MiB` 级别的内部安全阈值，而不是暴露为普通 tuning 参数。

诊断依据：

- `relay_pending=512MiB` 三轮完整样本平均吞吐约 `8886.21 Mbps`。
- 三轮 `max_pending_quic_receive_bytes` 均低于 `512MiB`。
- `quic_receive_paused/resumed` 均为 `0/0`。
- `relay_pending=128MiB` 会触发 receive pause，并且 3 次里 2 次出现 `curl exit 18`。

建议：

- 继续使用 `512MiB` 作为默认/下限。
- 如果需要开放配置，应命名为资源保护参数，而不是 tuning 参数。
- 不建议再支持通过历史 tuning profile 自动降到 `128MiB` 级别。

## 推荐重构步骤

1. 更新 CLI help：
   - 删除 `--fcw` / `--srw`。
   - 删除 `--relay-inflight-bytes`。
   - 删除 `--initial-quic-read-ahead`。
   - 删除 `--target-bandwidth-mbps` / `--target-rtt-ms`。
   - 删除 `--tuning custom`。
   - 删除 `--trace-connect-on-start`，当前 client 启动流程已经会启动 QUIC session 并等待连接，该参数只剩隐式开启 trace 的历史调试语义。

2. 更新文档和脚本：
   - 移除历史推荐命令中的 `--fcw`、`--srw`、`--relay-inflight-bytes`、`--initial-quic-read-ahead`、`--target-bandwidth-mbps`、`--target-rtt-ms`、`--tuning custom`。
   - `--iw` / `--initrtt-ms` 仅在启动期实验/诊断时显式设置；当前实验候选优先使用 `--iw 4000 --initrtt-ms 100`。
   - 保留 `--diag-stats` 用于高 RTT/loss 诊断。
   - 将 Linux relay read/write 参数移动到“诊断参数”或“平台 IO 参数”章节。

3. 后续代码清理：
   - 将 `RelayDefaultIdealSend` 从“背压阈值”语义改名为“initial/fallback/send-context budget”语义。
   - 将 `RelayMaxInFlightSends` 的文档改为 send context budget，不再描述为 TCP read 背压控制。
   - 删除已经不生效的 flow-control 覆盖项。
   - 删除目标 BDP 静态输入对 tuning 的影响。

4. 验证：
   - 默认参数下复跑 `100ms + 5% loss` 和 `200ms + 5% loss`。
   - 用 `--diag-stats` 确认：
     - TCP -> QUIC 方向 outstanding bytes 跟随 stream ideal。
     - QUIC -> TCP 方向 `quic_receive_paused/resumed` 在正常场景不触发。
     - `event_queue_full_errors`、`quic_send_failures`、`quic_send_api_failures` 仍为 0。

## 决策摘要

普通用户需要的是稳定默认 profile，不是继续暴露一组历史高 BDP 魔法参数。

保留：

- `--tuning wan|lan|auto`
- `--max-memory-mb`
- Linux relay IO/diagnostic 参数
- `--diag-stats`

实验/诊断保留：

- `--iw`，当前实验候选值 `4000`
- `--initrtt-ms`，当前实验候选值 `100`

从代码中删除影响：

- `--target-bandwidth-mbps`
- `--target-rtt-ms`
- `--tuning custom`
- `--fcw`
- `--srw`
- `--relay-inflight-bytes`
- `--initial-quic-read-ahead`
- `--trace-connect-on-start`

`--trace-connect-on-start` 同时从 JSON 配置中的 `trace.connect_on_start` 删除。后续需要启动阶段诊断时直接使用 `--trace --trace-interval <sec>`；client 启动连接不再依赖额外开关。
