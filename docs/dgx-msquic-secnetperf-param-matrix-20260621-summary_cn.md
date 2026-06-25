# DGX MsQuic secnetperf 参数矩阵总结 - 2026-06-21

## 测试条件

所有测试均使用相同基础条件：

- 1 条 QUIC connection，1 条 stream，download 方向
- 拥塞控制算法：BBR
- 发送端 netem：`netem delay {100,200}ms loss 5% limit 5000000`
- 先启动 server，并确认输出 `Started!` 后，再设置 netem
- 参数矩阵单轮测试时长：20s

本轮为了精确隔离参数影响，新增了运行时控制参数：

- `-postbuf:<bytes>`：secnetperf 每条 stream 维持的应用层 outstanding 目标值。
- `-fixedpostbuf:1`：禁止 `IDEAL_SEND_BUFFER_SIZE` 事件继续增大应用层 outstanding。
- `MSQUIC_MAX_IDEAL_SEND_BUFFER=<bytes>`：限制 MsQuic core connection 级 ideal send buffer 上限。

## 关键矩阵结果

| 参数组合 | 100ms + 5% loss | 200ms + 5% loss | 含义 |
| --- | ---: | ---: | --- |
| `postbuf=128MiB, corecap=128MiB` | 3.580 Gbps | 1.790 Gbps | 基线 |
| `postbuf=128MiB, corecap=512MiB, fixedpostbuf=1` | 3.604 Gbps | 1.811 Gbps | 只扩大 core cap，应用队列固定 |
| `postbuf=128MiB, corecap=512MiB` | 7.165 Gbps | 5.816 Gbps | core IDEAL 事件把应用队列抬高 |
| `postbuf=256MiB, corecap=128MiB` | 6.974 Gbps | 3.341 Gbps | 只扩大应用 outstanding |
| `postbuf=384MiB, corecap=128MiB` | 8.450 Gbps | 4.821 Gbps | 只扩大应用 outstanding |
| `postbuf=512MiB, corecap=128MiB` | 8.651 Gbps | 6.538 Gbps | 只扩大应用 outstanding |
| `postbuf=512MiB, corecap=256MiB` | 8.505 Gbps | 6.847 Gbps | 应用 outstanding 扩大 + 中等 core cap |
| `postbuf=512MiB, corecap=512MiB` | 8.522 Gbps | 6.685 Gbps | 应用 outstanding 扩大 + 较高 core cap |
| `postbuf=1024MiB, corecap=512MiB` | 8.738 Gbps | 6.938 Gbps | 更深应用队列 |
| `postbuf=1024MiB, corecap=1024MiB` | 8.527 Gbps | 7.203 Gbps | 更深应用队列 + 较高 core cap |

## 结论

这轮参数矩阵已经能比较精确地锁定：吞吐提升的直接原因是**有效应用层 outstanding/post queue 变大**，不是单独扩大 MsQuic core 的 ideal send buffer cap。

证据如下：

- `postbuf=128MiB, corecap=512MiB, fixedpostbuf=1` 基本仍停留在基线：`3.604 / 1.811 Gbps`。
- `postbuf=512MiB, corecap=128MiB` 明显提升到：`8.651 / 6.538 Gbps`。
- 当 `postbuf=512MiB` 已经显式增大后，`corecap=256MiB/512MiB` 相比 `corecap=128MiB` 没有带来稳定的大幅提升。
- `postbuf=128MiB, corecap=512MiB` 之所以提升，是因为应用没有固定 postbuf，MsQuic 的 `IDEAL_SEND_BUFFER_SIZE` 事件把应用 outstanding 目标值继续抬高了。日志显示实际 app outstanding 已经超过 128MiB。

因此，真正带来提升的是有效应用层 outstanding bytes：

- 可以通过 `-postbuf` 直接提高。
- 也可以通过提高 core ideal cap 间接提高，但前提是应用会响应 `IDEAL_SEND_BUFFER_SIZE` 事件，并且没有使用 `-fixedpostbuf` 固定队列。

## `IDEAL_SEND_BUFFER_SIZE`、`postbuf`、`corecap` 的关系

这三个概念都和“应用层能持续给 MsQuic 提交多少待发送数据”有关，但它们处在不同层级。

### `postbuf`

`postbuf` 是 secnetperf 应用层参数，表示每条 stream 愿意同时提交给 MsQuic 的未完成发送数据上限。

更准确地说，`postbuf` 控制的是：

```text
应用已经调用 MsQuic->StreamSend() 提交给 MsQuic，
但还没有收到 QUIC_STREAM_EVENT_SEND_COMPLETE 的数据大小总和上限。
```

这些数据可能处在几种状态：

- 已进入 MsQuic stream send request 队列，但还没有第一次 packetize。
- 已经 packetize 并发送出去，但还没有被 ACK。
- 已经判定丢包，正在等待或正在进行 recovery retransmission。
- 部分数据已 ACK，但整个 send request 还没有 complete。

因此，`postbuf` 可以近似理解为应用层允许的“已提交、未完成”的发送窗口。它不是一个单独 malloc 出来的连续内存池。

在当前 secnetperf download 测试里，server 侧逻辑类似：

```cpp
while (OutstandingBytes < IdealSendBuffer) {
    MsQuic->StreamSend(...);
    OutstandingBytes += IoSize;
}
```

这里的 `IdealSendBuffer` 就是 `postbuf` 控制的应用层 outstanding 目标。

默认 `-sendbuf:0` 时，MsQuic 通常不会把 payload 全量拷贝到内部 send buffer，而是持有应用传入的 buffer 引用和 send request 元数据。因此：

- `postbuf=512MiB` 不等于应用层或 MsQuic 立即分配 512MiB payload 内存。
- 但它会增加 MsQuic 内部 send request 队列深度、元数据数量、buffer 引用生命周期，以及数据排队时间。
- 如果启用 `-sendbuf:1`，MsQuic 更可能拷贝 payload 到内部 buffer，内存占用含义会变化。

### `IDEAL_SEND_BUFFER_SIZE`

`IDEAL_SEND_BUFFER_SIZE` 是 MsQuic 通过 stream callback 通知应用层的事件：

```text
QUIC_STREAM_EVENT_IDEAL_SEND_BUFFER_SIZE
```

它不是 QUIC 协议字段，而是 MsQuic API/实现层给应用的提示，含义是：

```text
MsQuic 认为当前 stream 可以/应该保持多少应用层 outstanding send bytes，
以避免发送路径缺数据。
```

secnetperf 默认会响应这个事件。如果没有设置 `-fixedpostbuf:1`，当事件里的 `ByteCount` 大于当前应用层 `IdealSendBuffer` 时，secnetperf 会把自己的 outstanding 目标抬高：

```cpp
if (Context->IdealSendBuffer < Event->IDEAL_SEND_BUFFER_SIZE.ByteCount) {
    Context->IdealSendBuffer = Event->IDEAL_SEND_BUFFER_SIZE.ByteCount;
}
```

所以 `IDEAL_SEND_BUFFER_SIZE` 不直接发送数据，也不直接扩大拥塞窗口。它的作用是提示应用层可以增加 `StreamSend()` 提交深度。应用是否照做，取决于应用自己的逻辑。

它的计算并不是简单的：

```text
IDEAL_SEND_BUFFER_SIZE = min(BDP, corecap)
```

MsQuic 当前的实际计算链路更接近：

```text
实际 BytesInFlight 增长
  -> 拥塞控制记录 BytesInFlightMax
  -> SendBuffer 根据 BytesInFlightMax 阶梯式增长 Connection->SendBuffer.IdealBytes
  -> 再映射成每条 stream 的 QUIC_STREAM_EVENT_IDEAL_SEND_BUFFER_SIZE.ByteCount
  -> 应用层收到事件后决定是否增加 postbuf
```

核心逻辑在 `send_buffer.c` 中：

```c
NewIdealBytes =
    QuicGetNextIdealBytes(
        QuicCongestionControlGetBytesInFlightMax(&Connection->CongestionControl));
```

`QuicGetNextIdealBytes()` 不是线性返回 `BytesInFlightMax`，而是从默认值 `128KiB` 开始，按 1.5 倍阶梯增长，直到达到 `corecap`：

```c
Threshold = 128KiB;

while (Threshold <= BytesInFlightMax) {
    NextThreshold = Threshold + Threshold / 2;
    if (NextThreshold > corecap) {
        Threshold = corecap;
        break;
    }
    Threshold = NextThreshold;
}
```

然后再映射成 stream 级别的事件值，逻辑近似为：

```text
IDEAL_SEND_BUFFER_SIZE.ByteCount =
    min(Connection->SendBuffer.IdealBytes,
        QuicGetNextIdealBytes(Stream->SendWindow))
```

因此它还会受到 stream 发送窗口估计值 `Stream->SendWindow` 的影响。

对于 BBR 场景：

- BBR 维护当前 `BytesInFlight`。
- 当当前 `BytesInFlight` 超过历史最大值 `BytesInFlightMax` 时，会触发 `QuicSendBufferConnectionAdjust()`。
- `BytesInFlightMax` 受 BBR 的 congestion window、pacing、bandwidth estimate、RTT、loss recovery 等因素影响。
- 因此 `IDEAL_SEND_BUFFER_SIZE` 与 BDP 有间接关系，但不是直接读取 `bandwidth * RTT` 得到。

更准确的关系是：

```text
BDP / 拥塞控制状态
  -> 影响 cwnd、pacing 和 bytes_in_flight
  -> bytes_in_flight 的历史最大值影响 IdealBytes
  -> corecap 限制 IdealBytes 最大能增长到多少
  -> IdealBytes 进一步转换成 IDEAL_SEND_BUFFER_SIZE 事件通知应用
```

还有一个重要实现细节：当前 MsQuic 注释中明确写到：

```c
// Currently, IdealBytes only grows and never shrinks.
```

也就是说，`Connection->SendBuffer.IdealBytes` 当前只增长，不会根据后续带宽下降或拥塞状态主动缩小。

### `QUIC_STREAM_EVENT_IDEAL_SEND_BUFFER_SIZE` 回调字段

MsQuic stream callback 的接口签名是：

```c
QUIC_STATUS
(QUIC_API QUIC_STREAM_CALLBACK)(
    _In_ HQUIC Stream,
    _In_opt_ void* Context,
    _Inout_ QUIC_STREAM_EVENT* Event
);
```

当 `Event->Type == QUIC_STREAM_EVENT_IDEAL_SEND_BUFFER_SIZE` 时，事件 payload 只有一个直接字段：

```c
struct {
    uint64_t ByteCount;
} IDEAL_SEND_BUFFER_SIZE;
```

因此，测试日志中的字段对应关系是：

```text
byte_count        = Event->IDEAL_SEND_BUFFER_SIZE.ByteCount
current_ideal     = secnetperf 当前保存的应用层 IdealSendBuffer
outstanding_bytes = secnetperf 当前已经 StreamSend、但还没有 SEND_COMPLETE 的字节数
```

其中，只有 `byte_count` 是 MsQuic 事件回调接口直接给出的参数。`current_ideal` 和 `outstanding_bytes` 是 secnetperf 为了观察应用层队列状态额外打印的上下文。

server 侧当前日志打印逻辑为：

```cpp
WriteOutput(
    "IdealSendBufferEvent[server]: byte_count=%llu current_ideal=%llu outstanding_bytes=%llu\n",
    (unsigned long long)Event->IDEAL_SEND_BUFFER_SIZE.ByteCount,
    (unsigned long long)Context->IdealSendBuffer,
    (unsigned long long)Context->OutstandingBytes);
```

### 100ms 无丢包场景中的 `IDEAL_SEND_BUFFER_SIZE` 实测变化

为了观察 `IDEAL_SEND_BUFFER_SIZE` 的实际增长过程，额外执行了一轮：

- 2*DGX，1 条 QUIC connection，1 条 stream，download 方向
- 发送端 netem：`netem delay 100ms limit 5000000`
- 无显式丢包配置，qdisc 统计 `dropped 0`
- 测试参数：`postbuf=128MiB`，`MSQUIC_MAX_IDEAL_SEND_BUFFER=1GiB`
- 测试结果：`Download 15603774 kbps`，约 `15.60 Gbps`
- 日志目录：`docs/dgx-msquic-secnetperf-idealbuf-100ms-noloss-20260621-144444`

本轮 server 侧 `QUIC_STREAM_EVENT_IDEAL_SEND_BUFFER_SIZE` 事件变化如下：

| 事件 | `byte_count` | `byte_count` MiB | `current_ideal` MiB | `outstanding_bytes` MiB |
| ---: | ---: | ---: | ---: | ---: |
| 1 | 5,038,848 | 4.8 | 128.0 | 128.0 |
| 2 | 7,558,272 | 7.2 | 128.0 | 128.0 |
| 3 | 11,337,408 | 10.8 | 128.0 | 128.0 |
| 4 | 17,006,112 | 16.2 | 128.0 | 128.0 |
| 5 | 25,509,168 | 24.3 | 128.0 | 128.0 |
| 6 | 38,263,752 | 36.5 | 128.0 | 128.0 |
| 7 | 57,395,628 | 54.7 | 128.0 | 128.0 |
| 8 | 86,093,442 | 82.1 | 128.0 | 128.0 |
| 9 | 129,140,163 | 123.2 | 128.0 | 128.0 |
| 10 | 193,710,244 | 184.7 | 128.0 | 128.0 |
| 11 | 290,565,366 | 277.1 | 184.7 | 184.8 |
| 12 | 435,848,049 | 415.7 | 277.1 | 277.1 |
| 13 | 653,772,073 | 623.5 | 415.7 | 415.7 |

这个表可以看到两个现象：

- `byte_count` 先低于 secnetperf 初始 `postbuf=128MiB`，因此应用层 `current_ideal` 没有变化。
- 当 `byte_count` 超过当前 `current_ideal` 后，secnetperf 响应该事件，把应用层 outstanding 目标逐步从 `128MiB` 抬高到约 `623.5MiB`。

注意表中的 `current_ideal` 是事件处理前打印的值。事件处理逻辑随后会在 `byte_count` 更大时更新 `current_ideal`，并继续调用 `StreamSend()` 补足 outstanding。因此事件 11 之后能看到 `outstanding_bytes` 随着上一轮事件的建议值逐步增长。

### `corecap`

`corecap` 是本轮测试中给 MsQuic core 增加的运行时上限参数，对应：

```text
MSQUIC_MAX_IDEAL_SEND_BUFFER
```

它限制的是 MsQuic core 内部 connection 级 ideal send buffer 的增长上限，也就是：

```text
Connection->SendBuffer.IdealBytes 的最大值
```

原始代码中对应宏为：

```c
QUIC_MAX_IDEAL_SEND_BUFFER_SIZE
```

`corecap` 不是 QUIC 协议参数，也不是：

- congestion window
- connection flow window
- stream recv window
- 真实发送队列长度
- 启动时立即分配的内存大小

它只是限制 MsQuic 最多可以向应用建议多大的 ideal send buffer。

### 三者的链路关系

三者的关系可以理解为：

```text
corecap
  -> 限制 MsQuic core 的 Connection->SendBuffer.IdealBytes 最大值
  -> 影响 MsQuic 发出的 QUIC_STREAM_EVENT_IDEAL_SEND_BUFFER_SIZE.ByteCount
  -> 如果应用响应该事件，会提高应用层 postbuf / IdealSendBuffer
  -> 应用调用更多 MsQuic->StreamSend()
  -> MsQuic stream send queue 中 outstanding send bytes 增加
  -> 丢包场景下有更多数据可用于填充 BDP 和重传周期
```

但如果应用不响应 `IDEAL_SEND_BUFFER_SIZE`，或者使用 `-fixedpostbuf:1` 固定应用层 outstanding，那么 `corecap` 再大也不会自动增加应用已经提交给 MsQuic 的数据量。

这也是矩阵结果中的关键现象：

- `postbuf=128MiB, corecap=512MiB, fixedpostbuf=1`：吞吐仍是基线，说明只扩大 corecap 不够。
- `postbuf=128MiB, corecap=512MiB`：吞吐提升，是因为 secnetperf 响应 `IDEAL_SEND_BUFFER_SIZE`，实际 outstanding 被抬高。
- `postbuf=512MiB, corecap=128MiB`：吞吐已经明显提升，说明直接扩大应用层 outstanding 是主要因素。
- `postbuf=512MiB, corecap=512MiB`：相比 `postbuf=512MiB, corecap=128MiB` 没有稳定大幅提升，说明当应用层 outstanding 已经足够大时，corecap 不是主要瓶颈。

## 实用判断

当前矩阵里性价比最高的区域是：

- `postbuf=512MiB`
- `corecap=128MiB` 已经接近 100ms 场景最佳，并且 200ms 场景也有明显提升。
- 在 `postbuf` 已经显式足够大时，继续提高 `corecap` 不是主要提升手段。
- `postbuf` 提高到 1GiB 只给 200ms 场景带来小幅提升，对 100ms 场景没有稳定收益，并且会增加排队等待时间。

## Flow control window 说明

当前 secnetperf 测试中，flow control 相关参数固定为：

```c
PERF_DEFAULT_CONN_FLOW_CONTROL      = 0x80000000  // 2GiB
PERF_DEFAULT_STREAM_RECV_WINDOW     = 0x80000000  // 2GiB
PERF_DEFAULT_INITIAL_WINDOW_PACKETS = 8000
PERF_DEFAULT_INITIAL_RTT_MS         = 500
```

也就是：

- connection flow window：2GiB
- stream recv window：2GiB
- initial congestion window：8000 packets
- initial RTT：500ms

`connection flow window` 和 `stream recv window` 是 QUIC flow control credit 的上限，不是启动时立即分配的内存。它们表示接收端允许发送端在 connection 或 stream 维度最多推进多少未被应用消费/窗口更新的数据量。

因此，即使把这两个 window 设置为 2GiB，如果实际链路带宽较小、IO 吞吐不高，比如只有 100Mbps，并且应用层及时消费数据，也不会因为这个 cap 本身立刻分配 2GiB 内存。实际内存占用取决于：

- 对端实际发送了多少数据
- 数据是否乱序到达
- 应用层是否及时读取并完成 receive buffer
- MsQuic 是否需要暂存未被应用消费的数据
- 丢包、重排序导致的洞有多大
- 当前连接数和 stream 数量

但是，大 window 仍然意味着系统允许缓存更多接收数据。如果对端高速发送、应用消费慢、或者乱序/积压严重，实际内存占用可能随之增加。所以它是“允许占用变大”的 cap，不是“立即占用”。

从当前矩阵日志看，这两个参数不是当前吞吐瓶颈。server 端 stream stats 中 flow control 相关阻塞时间均为 0：

```text
blocked_conn_flow_us=0
blocked_stream_id_flow_us=0
blocked_stream_flow_us=0
```

这说明当前测试过程中，发送没有被 connection flow control 或 stream flow control 阻塞。当前主要影响吞吐的是有效应用层 outstanding/post queue 大小，以及数据进入队列后的 packetization、recovery 和 scheduling。

## 后续方向

目前所有矩阵测试中，发送队列仍然一直是满的。因此剩余瓶颈不在“应用没有把数据放入队列”，而是在数据进入队列之后：

- packetization
- recovery retransmission
- send scheduling
- congestion/pacing 与 recovery window 的交互

后续应继续重点观察 recovery window 是否长期打开、recovery bytes 与 new bytes 的分时比例，以及单 stream 下 recovery 优先级是否持续压制新数据发送。
