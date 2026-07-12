# 跨平台零长度 FIN Receive Ownership 修复设计

## 1. 范围与目标

### 1.1 背景

Linux 线上实例已经确认：MsQuic 投递 `TotalBufferLength == 0` 且携带 `QUIC_RECEIVE_FLAG_FIN` 的 RECEIVE 事件后，relay callback 返回 `QUIC_STATUS_PENDING`，但 closing/handoff 清理路径没有执行 `StreamReceiveComplete(0)`。MsQuic 因此永久保持 `ReceiveDataPending=1`，无法发布真实 `QUIC_STREAM_EVENT_SHUTDOWN_COMPLETE`，最终造成 terminal retention 和 tunnel 无界滞留。

代码审计表明 Windows 和 macOS 也把“待完成字节数为 0”错误等同于“receive ownership 已结清”：

- Linux 已有零长度 FIN 专用标记，但公共 discard/cleanup 出口没有消费该标记。
- Windows 在 `completeBytes == 0` 时直接返回成功，并以 `CompletedLength >= TotalLength` 将零长度 view 标记为 settled。
- macOS 的 deferred completion 和 callback completion helper 都在长度为 0 时直接返回。

### 1.2 核心目标

建立三个平台一致的 receive ownership 契约：

1. callback 一旦返回 `QUIC_STATUS_PENDING`，即创建一项必须 exactly-once 结清的 receive completion obligation。
2. obligation 是否存在独立于 payload 字节数；`BytesToComplete == 0` 仍是有效义务。
3. active stream 上必须通过 `TqStreamLifetime` API lease 调用一次 `StreamReceiveComplete(BytesToComplete)`。
4. 只有真实 terminal、receive abort 或 connection shutdown 已使 stream API 不再合法时，才允许 bookkeeping-only terminal discard。
5. normal drain、closing、terminal handoff、precommit、queue fallback、worker stop 和 stale generation 都必须进入明确的 ownership exit。
6. 保持正常 TCP/QUIC FIN half-close 语义，不通过伪造 terminal、直接 `StreamClose` 或进程重启掩盖欠账。

### 1.3 用户可见成功标准

- 正常双向 FIN 最终观察到真实 `QUIC_STREAM_EVENT_SHUTDOWN_COMPLETE`。
- workload 结束后 tunnel registry、active/closing relay、terminal sink 和 terminal retention 全部回到 0。
- delay/loss、queue full、worker stop 和 connection reconnect 下不出现永久 stuck stream。
- 不引入 payload 截断、重复 `ReceiveComplete`、terminal 后 stream API 调用或正常 half-close 回归。

### 1.4 非目标

- 不修改 tunnel wire protocol、认证、压缩或 payload framing。
- 不把 graceful terminal 直接改为 fatal watchdog 或无条件 connection escalation。
- 不新增每 stream 线程。
- 不重构 MsQuic 内部状态机。
- 不把三个平台 worker 合并为同一个实现类。
- 不以 release/close wrapper 代替真实 receive ownership settlement。

## 2. 功能与非功能目标

### 2.1 功能目标

| ID | 目标 | 可验证结果 |
|---|---|---|
| F1 | 零长度 FIN active completion | callback 返回 `PENDING` 后恰好调用一次 `ReceiveComplete(0)` |
| F2 | 正长度 receive completion 保持不变 | 完成字节总数等于已接管字节总数，无重复或遗漏 |
| F3 | closing/handoff 结清 | view 在任一 cleanup 出口离开前 obligation 已 active-complete 或 terminal-discard |
| F4 | terminal 安全 | `SHUTDOWN_COMPLETE` 发布后不再调用 stream receive API |
| F5 | retry-safe lease | API lease 暂时不可用时保留 obligation，后续可重试，不提前 settled |
| F6 | generation-safe | stale relay/binding 不访问新 generation，不丢失旧 view 的独立 ownership settlement |
| F7 | normal FIN 保持 | 单向 FIN 只 half-close；双向完成后再进入 terminal handoff |
| F8 | exactly-once accounting | receive completion、terminal accounting、retention release 均无重复计数 |

### 2.2 非功能目标

| 维度 | 目标与门禁 |
|---|---|
| 正确性 | 所有 deterministic race tests 中 completion obligation 最终状态必须为 `ActiveCompleted` 或 `TerminalDiscarded`，不得停在 pending/dispatching |
| 性能 | baseline/peak 下吞吐相对修复前下降不超过 1%，p95/p99 tunnel completion latency 回归不超过 5% |
| 容量 | 单连接至少验证 1,000 并发 streams；多连接聚合至少验证 2,000 并发 streams |
| 可靠性 | 30 分钟 soak 后 `terminal_retained_owner_count=0`、`terminal_sink_pending=0`、platform pending receive count=0 |
| 可用性 | 单 stream 失败不得阻塞同 connection 其他 streams；必要的 connection reconnect 在 10 秒内恢复入口可用性 |
| 可观测性 | completion required/completed/discarded/duplicate/lease-failure 均有可关联 counter；retention 可关联 platform、connection、tunnel、stream |
| 安全性 | 不新增网络入口、权限或敏感信息；Admin 仍使用既有 Bearer 鉴权 |
| 恢复 | proxy 无持久业务数据，RPO=0；进程级故障 RTO 取现有 supervisor 重启与 QUIC reconnect 的 10 秒上限 |

## 3. 统一 Ownership 契约

### 3.1 基本原则

receive view 必须分别表示两个维度：

```text
BytesToComplete
CompletionObligationState
```

禁止再通过以下条件推断 ownership 已结清：

```text
BytesToComplete == 0
CompletedLength >= TotalLength
PendingCompleteBytes == 0
PendingReceiveBytes == 0
```

这些条件只能说明没有 payload 字节待推进，不能说明 callback 返回 `PENDING` 后的 MsQuic completion obligation 已结清。

### 3.2 逻辑状态机

平台内部可使用不同类型名，但必须实现以下等价状态：

```text
NotRequired
    callback 未返回 PENDING；应用没有 receive completion ownership

Pending
    callback 已返回 PENDING；尚未结清

ActiveDispatching
    已 exactly-once claim 当前 completion batch，正在持 API lease 调用 ReceiveComplete

ActiveCompleted
    ReceiveComplete(BytesToComplete) 已调用一次

TerminalDiscarded
    真实 terminal/abort/connection shutdown 后不再允许 stream API，仅完成 bookkeeping
```

状态转换：

```text
NotRequired -> Pending
Pending -> ActiveDispatching -> ActiveCompleted
Pending -> ActiveDispatching -> Pending    // 正长度 view 仍有未消费字节
Pending -> TerminalDiscarded
ActiveDispatching -> Pending               // API lease 获取失败且尚未调用 API
```

禁止转换：

```text
ActiveCompleted -> 任意其他状态
TerminalDiscarded -> 任意其他状态
ActiveCompleted <-> TerminalDiscarded
```

### 3.3 Active completion

active completion 必须按以下顺序执行：

1. 验证 obligation 为 `Pending`。
2. CAS claim 为 `ActiveDispatching`。
3. 计算本次尚未 dispatch 的 completion batch；正长度 view 可沿用既有 batching，零长度 FIN 的 batch 固定为 0 且由独立 obligation 标记为有效。
4. 通过 `TqStreamLifetime::TryAcquireReceiveApi()` 或平台现有等价 API lease 获取 wrapper。
5. lease 失败且 owner 尚未 terminal：回滚到 `Pending`，保留 view 供 worker maintenance/retry。
6. lease 成功：调用 `stream->ReceiveComplete(batchBytes)`；零长度 FIN 时参数为 0。
7. 调用返回后将该 batch 标记为已 dispatch；正长度 view 仍有字节时回到 `Pending`，整个 view 的所有字节及零长度 FIN obligation 均结清时发布 `ActiveCompleted`。

MsQuic `StreamReceiveComplete` 是无返回值 API，因此每个 batch 一旦发起调用，不得回滚或重试同一 batch。正长度 view 允许多个互不重叠的 completion batch；零长度 FIN 只允许一个 0-byte batch。

### 3.4 Terminal discard

terminal discard 仅允许满足以下任一事实：

- `TqStreamLifetime::Phase::TerminalPublished`；
- 已观察真实 `QUIC_STREAM_EVENT_SHUTDOWN_COMPLETE`；
- receive direction 已被 abort；
- connection shutdown 已使 stream API lease 永久失效。

terminal discard 必须 CAS `Pending -> TerminalDiscarded`，释放平台 pending bytes/queue depth/callback budget，但不得调用 `ReceiveComplete`。

不能仅以 relay `Closing`、TCP fd 关闭、worker stop 或 terminal handoff 已启动作为 terminal discard 依据；这些状态仍可能要求 active completion 才能推动 MsQuic 发布 terminal。

### 3.5 共享与平台边界

不新增跨平台公共 worker 基类，也不强制三个 pending receive struct 使用同一个 C++ 实现。共享内容限于：

- 本设计冻结的状态语义和 invariants；
- 公共 metrics 命名与 release gates；
- 必要时在 `quic_receive_guard.h` 增加无平台依赖的状态枚举或小型 CAS helper。

ownership settlement、API lease、queue accounting 和 worker wake/retry 保留在各平台 worker 内部。

## 4. 平台设计

### 4.1 Linux/epoll

现有 `TqPendingQuicReceive::ZeroLengthFinCompletionPending` 已表达部分 obligation，但与正长度 completion 分离，且 `CompleteAndDiscardQuicReceive()` 没有消费零长度义务。

Linux 修复应：

- 将“零长度 FIN 特例”提升为明确 completion obligation，而不是只在正常 drain 分支逐个调用 helper。
- 让 `CompleteAndDiscardQuicReceive()`、`AbortRelayAndRelease()`、`ProcessQuicReceiveViewEvent()` closing/stale 分支、callback fallback 和 worker stop 清理都调用同一个非 terminal settlement 出口。
- terminal 已发布时继续使用 `DiscardTerminalQuicReceive()`，但必须 exactly-once claim terminal discard。
- graceful fully-closed predicate 纳入 callback-pending 或已投递 receive obligation，避免 handoff 超前；该门禁是防御层，不能替代 cleanup helper 的正确性。
- 保留现有 `CompleteZeroLengthFinReceive()` 测试 hook/metrics，或将其迁移到统一 helper 后保持指标兼容。

### 4.2 Windows/IOCP

Windows 已有 `OwnershipSettled`，但其含义被 `CompletedLength >= TotalLength` 错误驱动；`ActiveReceiveCompleteViaOwner(0)` 直接返回成功，没有调用 MsQuic。

Windows 修复应：

- 将 `OwnershipSettled` 扩展为可区分 pending/dispatching/completed/discarded 的状态，而不是单 bool。
- `BuildDeferredQuicReceiveView()` 在 callback 最终选择返回 `PENDING` 时创建 obligation；构造 view 本身不等于 ownership 已转移，IOCP post 失败并返回 `SUCCESS` 时不得留下 obligation。
- `ActiveReceiveCompleteViaOwner()` 必须允许显式 obligation 调用 `ReceiveComplete(0)`；普通无 obligation 的零字节调用仍应被拒绝，防止伪 completion。
- `ActiveCompleteRemainingReceiveOwnership()` 不再使用 `CompletedLength >= TotalLength` 作为零长度 view 的 settled 条件。
- `FlushPendingQuicReceivesOnClose()`、IOCP orphan cleanup、callback post fallback、stop drain 和 terminal seal 必须根据 owner phase选择 active completion 或 terminal discard。
- 保留 IOCP in-flight ownership drain，completion obligation settlement 不得绕过 `WorkerIocpOwnershipIsZero()` 发布条件。

### 4.3 macOS/kqueue

macOS 的 `CompleteDeferredQuicReceive()` 与 `CompleteMsQuicReceiveFromCallback()` 都以字节数 0 为无操作；正常零长度 FIN 即使没有 closing 竞态也可能遗漏 completion。

macOS 修复应：

- `TqDarwinPendingQuicReceive` 增加独立 completion obligation/state；不能继续仅以 `PendingCompleteBytes` 表示义务。
- `EnqueueQuicReceiveForTcp()` 对无 slices 的 FIN view 必须 active-complete `ReceiveComplete(0)`，再推进 TCP `SHUT_WR`。
- `DiscardDeferredQuicReceive()` 对非 terminal owner 必须 active-complete，即使 bytes 为 0；terminal owner 才 bookkeeping discard。
- `CompleteMsQuicReceiveFromCallback()` 接受显式 `completionRequired` 或由上层保证只在 obligation claim 后调用，允许 `totalLength == 0`。
- precommit、event-queue-full callback queue、binding retire、worker stop 和 terminal seal 都使用同一 settlement 状态。
- 保留“不得在 relay mutex 内调用 ReceiveComplete”和 lease-only 约束。

## 5. 系统级端到端功能图

| 链路阶段 | 输入/状态 | 行为 | 关键断言 | 可观测证据 |
|---|---|---|---|---|
| Client ingress | SOCKS/HTTP/TCP port-forward 建立 tunnel | 创建 QUIC stream 和 relay | stream/tunnel identity 唯一 | `stream_started`、tunnel Admin |
| TCP→QUIC | 本地 TCP EOF | 提交一次 QUIC FIN | 不创建 fatal watchdog，不重复 FIN | FIN submitted/completed counters |
| QUIC peer | 对端发送零长度 FIN | MsQuic 投递 RECEIVE FIN | real FIN 与 `UINT64_MAX` fake FIN 分离 | receive event trace |
| Stream callback | RECEIVE FIN，长度 0 | 构造 view，返回 `PENDING` | obligation=`Pending`，bytes=0 | required counter +1 |
| Platform queue | epoll event/IOCP/kqueue event | 转移 view ownership | enqueue/fallback/precommit exactly-once | queue depth、generation |
| Active drain | 无 payload slices | 调用 `ReceiveComplete(0)` | API 调用恰好一次 | completed counter +1 |
| TCP half-close | receive FIN 已完成 | TCP `SHUT_WR` | 只关闭写方向 | graceful drain trace |
| Concurrent close | relay 先进入 Closing | cleanup 仍完成 active obligation | Closing 不等于 terminal discard | closing-race test trace |
| Terminal | MsQuic 两方向确认 | 发布真实 `SHUTDOWN_COMPLETE` | terminal 唯一事实源 | terminal ledger event |
| Reaper | 三门禁满足 | 释放 tunnel/relay/owner | retention 回到 0 | Admin metrics/retentions |
| Connection recovery | connection 被故障注入关闭 | generation-safe reconnect | 旧 view 不访问新 generation | reconnect + stale-drop counters |

认证、授权和 Admin API 不在数据面 receive ownership 路径内，但系统验证读取 Admin 时必须继续使用 Bearer token；未认证请求必须返回 401。

## 6. 测试策略与覆盖矩阵

### 6.1 分层策略

| 层级 | 目标 |
|---|---|
| helper 单元测试 | 0-byte obligation 状态转换、CAS exactly-once、lease failure rollback、terminal discard |
| worker focused test | normal、closing、precommit、queue fallback、worker stop、stale generation |
| stream lifetime test | API lease 与 terminal callback 竞争，terminal 后禁止 down-call |
| tunnel/reaper 集成测试 | 双向 FIN 到真实 terminal 和 retention release |
| 跨进程系统测试 | client/server 真实 MsQuic、真实 TCP target、delay/loss/connection recovery |
| 性能与 soak | 大量并发 streams 下无 retention、无性能回归 |

### 6.2 必测场景

| 场景 | Linux | Windows | macOS | 预期 |
|---|---:|---:|---:|---|
| 正常零长度 FIN | 必须 | 必须 | 必须 | `ReceiveComplete(0)` 一次 |
| FIN + payload | 必须 | 必须 | 必须 | 总字节 completion 一次且 FIN terminal |
| relay Closing 先于 view drain | 必须 | 必须 | 必须 | active completion，不丢 obligation |
| terminal 先于 cleanup | 必须 | 必须 | 必须 | terminal discard，不调用 API |
| lease 暂时失败 | 必须 | 必须 | 必须 | 保持 pending，maintenance 后完成 |
| queue full/fallback | 必须 | IOCP post failure | kqueue event full | 不丢 view/obligation |
| precommit race | 必须 | 必须 | 必须 | activation/rollback exactly-once |
| worker stop | 必须 | 必须 | 必须 | active completion 或合法 terminal discard |
| stale generation | 必须 | 必须 | 必须 | 旧 obligation 独立结清，不触碰新 relay |
| connection shutdown | 必须 | 必须 | 必须 | terminal discard，retention release |

所有确定性竞态测试使用 barrier/latch、fake event ordering 或 test hook，不使用 sleep 推测事件顺序。仅等待真实 OS completion 的既有 IOCP/kqueue 测试可以使用带硬 deadline 的 condition polling。

## 7. k6 性能基线

### 7.1 Workload 模型

复用并扩展 `docs/test/k6/linux-terminal-convergence.js` 的 tunnel workload，使 runner 可连接任意平台 client：

| Scenario | 并发 streams | 持续时间 | 流量模型 |
|---|---:|---:|---|
| baseline | 100 | 5 分钟 | 每秒稳定建立/双向传输/双向 FIN |
| expected_peak | 1,000 | 10 分钟 | 80% 小流、15% 256 KiB、5% 4 MiB |
| spike | 2,000 | 2 分钟 | 30 秒内从 100 升至 2,000，再回落 |
| stress | 从 1,000 每级增加 500 | 每级 3 分钟 | 探索首次违反门禁的 breaking point |
| soak | 500 | 30 分钟 | 持续 churn，周期性注入 5% loss/200 ms delay |

### 7.2 数据与环境

- 每个 iteration 创建独立 tunnel，目标 fixture 在读完请求后发送响应并正常 FIN。
- 50% fixture 返回 payload 后零长度 FIN，50% FIN 与最后一段 payload 同事件到达。
- client/server 使用同一提交和 Release 构建；平台 worker 数、buffer tuning 和 QUIC connection 数固定记录。
- baseline 使用无 netem 网络；loss 场景使用 200 ms RTT、5% packet loss，参数与既有 DGX matrix 对齐。
- 不 stub MsQuic；DNS/公网第三方目标替换为受控本地/远端 fixture，消除第三方抖动。

### 7.3 指标与阈值

| 指标 | 门禁 |
|---|---|
| tunnel completion success rate | ≥ 99.99% |
| p50/p95/p99 completion latency | 相对修复前 baseline 回归 ≤ 5% |
| throughput | 相对修复前下降 ≤ 1% |
| dropped iterations | 0（stress breaking-point 阶段除外） |
| terminal retained owners | workload drain 后 30 秒内为 0 |
| terminal sink pending | workload drain 后 30 秒内为 0 |
| pending receive obligations | workload drain 后 30 秒内为 0 |
| duplicate completion/violation | 始终为 0 |
| worker queue full | baseline/peak 为 0；故障注入场景必须恢复为 0 |
| CPU/RSS | 相对 baseline 增幅 ≤ 5%，且无持续单调增长 |

## 8. 容量与可扩展性验证

- 单 connection 逐步提高到 1,000 并发 bidirectional streams，验证 stream table、worker queue 和 retention registry 释放速度。
- 使用 2–8 个 QUIC connections 聚合到 2,000 streams，验证 generation registry 和 worker sharding。
- 分别测试零字节 FIN 占比 1%、50%、100%，确认 completion obligation 数量不会改变 payload buffer capacity。
- queue capacity 使用默认值、最小支持值和故障注入 full 三档。
- 每个平台记录 worker CPU、queue depth、pending receive count、completion dispatch rate 和 retention oldest age。
- breaking point 只用于建立容量边界，不作为发布通过；baseline 和 expected peak 必须满足全部 release gates。

## 9. 异常条件与恢复设计

| 场景 | 注入故障 | 用户影响 | 检测信号 | 缓解与恢复 | 验收标准 |
|---|---|---|---|---|---|
| API lease 暂时失败 | test hook 拒绝 1–3 次 receive lease | 单 stream terminal 延迟 | lease failure + pending obligation | maintenance 重试 | lease 恢复后一次 completion，30 秒内 retention=0 |
| worker queue full | 强制 event/IOCP/kqueue post 失败 | 短暂 backpressure | queue-full/fallback counter | intrusive/binding fallback drain | obligation 不丢、queue 恢复、无 fatal reset |
| TCP reset | target 在 FIN 前 RST | 当前 tunnel 失败 | TCP errno/fatal handoff | abort immediate + watchdog | 5 秒内 escalation 或真实 terminal |
| QUIC packet loss | 5% loss、200 ms RTT | completion 延迟 | QUIC/terminal age | MsQuic 重传 | 无永久 pending，drain 后 30 秒 retention=0 |
| connection shutdown | 关闭承载 connection | 该 connection streams 中断 | connection callback + generation | generation-safe reconnect | 10 秒内新入口可用，旧 obligation terminal-discard |
| worker stop | stop 时仍有零长度 obligation | 当前 stream closing | stop-drain counters | stop drain active completion；终态后 discard | worker 退出前 ownership 为 0 |
| process crash | SIGKILL 测试实例 | 全部 tunnel 中断 | supervisor/process alert | supervisor 重启、client reconnect | RPO=0，10 秒内恢复新连接 |
| CPU/memory pressure | 限制 CPU、降低 buffer budget | latency 上升/backpressure | CPU/RSS/alloc failure | backpressure、bounded queues | 无 UAF、无 duplicate、恢复后 retention=0 |
| stale generation | relay id 重用并投递旧 completion | 不应影响新 tunnel | generation mismatch counter | 丢弃旧路由操作，独立结清旧 obligation | 新 relay 数据完整，旧 ownership 为终态 |

本功能无数据库、缓存、消息队列或持久业务数据，灾难恢复不涉及备份恢复或数据回放；RPO 固定为 0。进程 crash 会中断内存中的 tunnels，这是既有产品边界，验收关注自动重启和新连接恢复，不承诺恢复旧 tunnel payload。

## 10. 可观测性与测试证据

### 10.1 公共指标

新增或统一聚合以下 snake_case metrics：

```text
relay_receive_completion_required
relay_receive_completion_active_completed
relay_receive_completion_terminal_discarded
relay_receive_completion_zero_length
relay_receive_completion_lease_retry
relay_receive_completion_duplicate_suppressed
relay_receive_completion_pending
relay_receive_completion_exactly_once_violation
```

平台指标保留 `linux_`、`windows_`、`darwin_` 前缀明细，公共聚合指标用于 release gates。

### 10.2 日志关联

发生 pending age warning、duplicate suppression 或 lease retry 时，日志至少包含：

```text
platform
connection_id
connection_generation
tunnel_id
stream_id
relay_id
worker_index
completion_bytes
completion_state
owner_phase
```

不记录 payload、Bearer token、证书私钥或认证内容。

### 10.3 证据包

每个平台系统验证输出：

- client/server 完整命令与版本；
- workload 参数和环境信息；
- before/peak/after relay metrics；
- terminal retention snapshots；
- process/thread/socket snapshots；
- failure injection timeline；
- k6 summary 与 thresholds；
- focused tests、全量 tests、ASan/平台 sanitizer 结果。

## 11. 进入、退出与发布门禁

### 11.1 进入条件

- 根因报告已确认三个平台的静态代码缺口。
- 每个平台能够构造真实 absolute offset 的零长度 FIN，而不是 `UINT64_MAX` fake FIN。
- fake MsQuic API 能分别统计 `ReceiveComplete(0)` 调用次数和字节数。

### 11.2 退出条件

- 三个平台 focused zero-FIN tests 全部通过。
- closing/handoff、queue fallback、lease failure、terminal race 全部有确定性回归。
- 所有受影响平台全量测试通过。
- Linux ASan、Windows ASan/Application Verifier 等价检查、macOS ASan 无 UAF/leak/double completion。
- 系统 workload drain 后所有 pending/retention/violation 门禁为 0。
- k6 baseline/peak/soak 满足第 7 节阈值。

### 11.3 发布阻断条件

出现任一项即阻断发布：

- `relay_receive_completion_pending != 0` 且 workload 已 drain 30 秒；
- `relay_receive_completion_exactly_once_violation != 0`；
- terminal 后发生 receive API down-call；
- terminal retention 或 terminal sink pending 非 0；
- 正常 FIN 被降级为 abort/reset；
- payload 字节校验失败；
- stale generation 影响新 relay；
- baseline/peak 性能超过允许回归。

## 12. 风险、假设与已决策项

### 12.1 风险

- CAS claim 后 API lease 失败若不能回滚，会产生新的永久 dispatching 状态。
- terminal callback 与 active completion 并发可能导致 terminal 后 API 调用，必须以 owner phase和 exactly-once 状态双重约束。
- 仅修正常 drain 会再次遗漏 stop/fallback/precommit 等 ownership exit。
- 指标若只统计字节数，零长度 obligation 仍不可见，必须独立计数事件数。
- Windows/macOS 当前测试可能把“queue 清零”误当作“MsQuic ownership 已完成”，需要改写断言而不是只新增测试。

### 12.2 假设

- MsQuic 对 callback 返回 `QUIC_STATUS_PENDING` 的零长度 FIN 要求显式 `StreamReceiveComplete(0)`；Linux live flags 已验证这一行为。
- `StreamReceiveComplete` 调用本身无同步失败返回，因此 exactly-once claim 必须在调用前完成。
- 真实 `SHUTDOWN_COMPLETE` 仍是 terminal 唯一事实源。
- 平台 worker 的现有 API lease 和 generation control 可复用，不新增裸 stream pointer ownership。

### 12.3 已决策项

- 采用共享语义、平台独立实现，不引入跨平台 worker 基类。
- completion obligation 与 byte accounting 分离。
- graceful watchdog 改造不属于本修复范围。
- 三个平台分别编写和执行独立 implementation plan。
- Linux 先作为已复现平台验证修复模型；Windows 和 macOS 计划可独立执行，不依赖 Linux 提交顺序。
