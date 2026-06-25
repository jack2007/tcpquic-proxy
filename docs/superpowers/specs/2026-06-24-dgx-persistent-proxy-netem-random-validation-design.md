# DGX Persistent Proxy Netem Random Validation Design

## 目标

本设计用于验证 `tcpquic-proxy` 在 DGX 直连链路上的长期运行稳定性和 IO 吞吐表现。测试重点不是只得到一张顺序递增的吞吐矩阵，而是在 `tcpquic-proxy client/server` 尽量不重启的条件下，通过随机切换 netem 时延/丢包组合，记录吞吐、暴露异常，并在异常发生时保留现场、复现规律。

本设计参考 `docs/superpowers/plans/2026-06-23-dgx-10loss-io-throughput-validation-cn.md`，但修正测试拓扑和执行策略：

- `tcpquic-proxy client` 固定运行在本机 DGX。
- `tcpquic-proxy server` 固定运行在对端 DGX。
- 测试开始时启动 proxy，测试结束时关闭；除非 proxy 已无法继续服务，否则异常发生后不主动重启。
- 每轮测试只通过 netem 增加、替换或删除直连网卡的时延/丢包规则。
- 场景顺序随机化，避免固定升序 RTT 掩盖长期运行状态漂移。

## 固定环境

默认沿用当前 DGX 双机环境：

```text
本机 DGX 直连 IP: 169.254.250.230
对端 DGX 直连 IP: 169.254.59.196
对端 DGX 管理 SSH: jack@172.16.10.81
直连网卡: enp1s0f0np0
QUIC UDP 端口: 4433
HTTP CONNECT 代理端口: 18080
SOCKS5 代理端口: 19080
iperf3 端口: 16001
```

所有远端控制命令必须通过 `jack@172.16.10.81` 执行。`172.16.10.81` 是独立管理网口，不承载本次 netem。禁止使用 `jack@169.254.59.196` 作为控制通道，因为 download 场景会在对端直连网卡上施加 netem，upload 场景会在本机直连网卡上施加 netem。

## 固定 Proxy 拓扑

整场测试只使用一种 proxy 拓扑：

```text
本机 iperf3 client
  -> 本机 tcpquic-proxy client, HTTP CONNECT 127.0.0.1:18080
  -> QUIC over DGX 直连链路
  -> 对端 tcpquic-proxy server
  -> 对端 iperf3 server, 169.254.59.196:16001
```

download 和 upload 都通过本机 HTTP CONNECT 代理发起：

- download: 本机 `iperf3 -c 169.254.59.196 --reverse --proxy http://127.0.0.1:18080`，QUIC 主数据方向为对端 server 到本机 client。
- upload: 本机 `iperf3 -c 169.254.59.196 --proxy http://127.0.0.1:18080`，QUIC 主数据方向为本机 client 到对端 server。

因此两种方向不再切换 proxy 角色，也不再为 upload 启动“对端 client、本机 server”的反向部署。

## Netem 规则

netem 只配置在承载 QUIC 主数据发送方向的直连网卡 egress 上：

- download: 发送端是对端 DGX，netem 加在对端 `enp1s0f0np0`。
- upload: 发送端是本机 DGX，netem 加在本机 `enp1s0f0np0`。

每个正常测试轮次的 netem 生命周期：

1. 采集本机和对端当前 qdisc。
2. 删除本机和对端直连网卡上的残留 netem。
3. 在当前方向的发送端直连网卡上应用本轮规则。
4. 运行 iperf3 并持续采样 qdisc。
5. 采集本轮结束 qdisc。
6. 如果本轮正常，删除 netem 并进入下一轮。
7. 如果本轮异常，保留当前 netem 和 proxy 进程，冻结现场并暂停主矩阵。

默认 netem 命令形式：

```bash
sudo -n tc qdisc replace dev enp1s0f0np0 root netem delay <delay>ms loss <loss>% limit 5000000
```

`limit 5000000` 沿用历史测试，以降低 netem 队列上限成为额外瓶颈的概率。

## 测试矩阵和随机顺序

默认时延集合沿用参考文档：

```text
10,20,30,40,50,60,70,80,90,150,300,500 ms
```

默认丢包集合为：

```text
10%
```

计划必须支持通过环境变量扩展丢包集合，例如：

```text
LOSSES="0 1 5 10"
```

每个场景由 `(direction, delay_ms, loss_percent)` 唯一确定。执行前生成随机顺序文件 `scenario-order.csv`，并记录随机种子。随机化范围覆盖 download 和 upload 两个方向，而不是先跑完一个方向再跑另一个方向。

默认单轮时长：

```text
iperf3 duration: 30s
iperf3 hard timeout: 120s
```

500ms 或高丢包场景可能需要更长连接建立时间。若历史或预跑显示 120s 不足，允许把 `IPERF_TIMEOUT_SEC` 调整到 `180`，但必须写入 `run.env` 和 `orchestrator.log`。

## Proxy 生命周期

proxy 是本测试的被测对象，生命周期必须尽量覆盖整场测试：

1. 前置清理后启动本机 `tcpquic-proxy client`。
2. 通过管理网 SSH 启动对端 `tcpquic-proxy server`。
3. 等待本机 HTTP CONNECT 监听、对端 QUIC server 监听和 QUIC client connected。
4. 运行 baseline 和随机矩阵。
5. 正常测试结束后统一停止 proxy。

异常发生时，默认不重启 proxy。只有满足以下条件之一，才允许关闭或重启：

- proxy 进程已经退出，无法继续保留运行现场。
- proxy 仍运行但 HTTP CONNECT 或 QUIC 会话完全不可用，后续复现无法开始。
- 已完成现场冻结，且明确需要重启来验证“重启后问题消失或复现”这一规则。

任何重启都必须在结果目录记录：

```text
restart_reason
restart_time
old_pid
new_pid
restart_before_artifacts
```

## 日志和现场保留

常驻 proxy 日志必须作为整场连续日志保存：

```text
proxy/local-client.stderr.log
proxy/remote-server.stderr.log
```

每个测试轮次额外记录该轮开始和结束时的日志字节 offset，并抽取轮次切片：

```text
local-proxy.offsets
remote-proxy.offsets
local-proxy.case.log
remote-proxy.case.log
```

这样既能保留整场运行上下文，也能方便逐轮分析。

每个轮次目录必须包含：

```text
iperf.stdout.json
iperf.stderr.txt
iperf.rc
run.env
run.log
local-qdisc-before.txt
local-qdisc-after.txt
local-qdisc-samples.txt
remote-qdisc-before.txt
remote-qdisc-after.txt
remote-qdisc-samples.txt
netem-apply.stdout.txt
netem-apply.stderr.txt
netem-active-qdisc.txt
netem-verify.stderr.txt
local-proxy.offsets
remote-proxy.offsets
local-proxy.case.log
remote-proxy.case.log
```

异常目录额外包含：

```text
anomaly.env
anomaly-notes.md
proxy-pids.txt
local-ss-tcp.txt
local-ss-udp.txt
remote-ss.txt
local-ip-link.txt
remote-ip-link.txt
local-proc-status.txt
remote-proc-status.txt
local-proxy-ps.txt
remote-proxy-ps.txt
local-proxy.full-at-freeze.log
remote-proxy.full-at-freeze.log
```

## 异常判定

硬异常：

- `iperf.rc != 0`
- iperf3 JSON 缺失或无法解析
- `received_mbps == 0`
- iperf3 达到 hard timeout
- netem 应用失败或 qdisc 与预期不一致
- 本机 proxy client 或对端 proxy server 进程退出
- HTTP CONNECT 代理无法建立连接

软异常：

- 当前 `received_mbps` 明显低于历史同场景结果。
- 当前 `received_mbps` 明显低于相邻时延同方向结果。
- 同一方向低时延场景吞吐出现非预期塌陷，例如 `delay <= 150ms` 且低于历史中位数的 50%。
- proxy diag 中出现非零 `event_queue_full_errors`、`quic_send_failures`、`quic_send_api_failures`、`quic_send_fatal_errors`、`tcp_hard_errors`，并伴随吞吐下降或 iperf 异常。

历史参考优先使用：

```text
docs/dgx-iperf3-10loss-io-matrix-20260623-235738/summary.csv
```

该历史结果不是验收阈值，而是异常发现的参考基线。因为新计划保留 proxy 常驻运行，结果可能受到连接长期状态、上一个随机场景、BBR 状态和 netem 切换顺序影响。

## 异常复现策略

一旦发现硬异常或强软异常，主矩阵必须暂停。暂停后按以下顺序操作：

1. 不删除当前 netem。
2. 不停止 proxy。
3. 冻结 full proxy log、qdisc、socket、进程状态。
4. 在同一 proxy 进程、同一 netem 规则下重复同场景 3 次。
5. 删除并重新应用同一 netem 规则，再重复同场景 3 次。
6. 若问题仍存在，测试相邻 delay 或相同 delay 的另一个方向，判断是否和方向、delay、loss、前序场景有关。
7. 只有完成上述现场记录后，才允许重启 proxy 做“重启是否清除问题”的对照。

复现结论至少要回答：

- 是否只在 download 或 upload 出现。
- 是否只在特定 delay/loss 出现。
- 是否依赖前一个随机场景。
- 是否依赖 netem 删除再重建。
- 是否依赖 proxy 长时间运行或特定 proxy 连接状态。
- 重启 proxy 后是否消失。

## 汇总报告

最终 `summary.csv` 至少包含：

```text
order_index,seed,direction,delay_ms,loss,netem_side,iperf_rc,sent_mbps,received_mbps,retransmits,jitter_ms,netem_limit,qdisc_drop_delta,qdisc_backlog_max_bytes,qdisc_backlog_max_packets,srtt_avg_ms,srtt_max_ms,bbr_bw_avg_mbps,bytes_in_flight_max,lost_retransmittable_bytes_max,event_queue_full_errors_max,proxy_restarted,anomaly,notes
```

最终 `summary.md` 必须先列完整随机执行顺序，再按方向和 delay/loss 汇总吞吐，最后列异常和复现结论。

报告中必须区分：

- 性能结果：吞吐、重传、qdisc、BBR/RTT。
- 稳定性结果：连接失败、proxy 错误、timeout。
- 测试过程影响：随机顺序、前序场景、是否重启、是否保留 netem。

## 非目标

本设计不修改 `tcpquic-proxy` 代码，不修改 MsQuic 参数，不启用 trace 文件日志。除非异常复现需要额外诊断，否则只使用 `--diag-stats` 低开销日志。
