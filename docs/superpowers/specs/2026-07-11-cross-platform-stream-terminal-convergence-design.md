# 跨平台 MsQuic stream terminal 有界收敛设计

**状态：** 已批准，供 Linux、Windows、macOS 分平台开发计划使用  
**问题分析：** `docs/msquic_stream_terminal_lifecycle_fix_cn.md`  
**基础设计：** `docs/superpowers/specs/2026-07-10-relay-stream-wrapper-terminal-lifetime-design.md`  
**既有系统测试设计：** `docs/superpowers/specs/2026-07-10-relay-stream-wrapper-terminal-lifetime-system-test-design.md`

## 1. 背景与问题定义

现有跨平台 stream wrapper 设计已经完成以下基础治理：relay-capable stream 使用
`CleanUpManual`，`TqStreamLifetime` 是 `MsQuicStream` 的唯一 owner，callback 入口持有强引用
guard，业务 target 通过稳定 router 切换，非 callback stream API 通过 owner lease 调用，只有
真实 `QUIC_STREAM_EVENT_SHUTDOWN_COMPLETE` 才发布 wrapper terminal。

这些约束解决了 wrapper auto-delete、裸指针 UAF、callback/context 并发改写和 terminal 后继续
调用 stream API 的问题，但没有保证 fatal 数据面结束后 stream shutdown 一定被可靠提交并在
有界时间内收敛。当前公共 tunnel/reaper 仍可能把 relay stop 当作 tunnel 可删除条件：

```text
TCP hard error / admin stop / worker stop
  -> relay 发布 stop
  -> reaper 删除 tunnel context
  -> shutdown 返回值未参与交接判断
  -> callback target 随 tunnel/relay 失效
  -> stream owner 只剩 terminal retention
  -> SHUTDOWN_COMPLETE 未到达时永久 retained
```

该失配与平台事件机制无关。Linux 的 epoll、Windows 的 IOCP、macOS 的 kqueue 都可能发生
TCP 终止、relay 退役、tunnel 删除与 MsQuic terminal callback 不同序到达，因此三平台必须共享
同一 terminal handoff 和有界收敛契约。

## 2. 目标、成功标准与非目标

### 2.1 目标

1. 正常 TCP EOF 继续使用 QUIC FIN，保留双向 stream half-close。
2. 任意 started tunnel 准备最终释放且尚未 terminal 时，先完成独立于 tunnel/relay 的 terminal
   handoff，再提交 `ABORT | IMMEDIATE`。
3. `StreamShutdown` 的同步返回值必须被分类、记录并驱动重试或 connection escalation。
4. tunnel context 可以在 handoff 成功后释放；terminal callback、retention、最终 accounting 和
   wrapper 析构不依赖 tunnel context、relay state 或平台 worker 存活。
5. fatal stream 在可配置但有上限的时间内到达 terminal，或升级为 connection shutdown；不得
   永久占用 terminal retention registry。
6. Linux、Windows、macOS 使用同一公共状态机、错误语义、Admin 数据模型和发布门禁。
7. stop、shutdown、terminal、watchdog、connection escalation、accounting 和析构全部幂等且
   exactly-once。

### 2.2 可量化成功标准

- fatal shutdown 正常路径在提交后 5 秒内观察到本地 `SHUTDOWN_COMPLETE`；5 秒是默认 watchdog
  deadline，可通过配置在 5–30 秒范围内调整。
- shutdown 同步失败最多重试 3 次，退避为 10 ms、50 ms、250 ms；terminal 已观察或 connection
  已进入 shutdown 时停止重试。
- 第一次 watchdog 超时立即触发一次 connection shutdown/reconnect escalation；升级后 30 秒仍无
  terminal 时进入 `terminal_timeout` 发布阻断状态并保持可诊断 retention，不强制 `StreamClose`。
- 无故障回归结束后 10 秒内，`active_tunnels`、`active_relays`、terminal sink pending、watchdog
  pending 和 send completion registry 均回到测试基线。
- 30 分钟、每秒至少 100 次 tunnel churn 的 soak 中，terminal retained owner 数量不得单调增长；
  测试结束 30 秒后必须回到基线。
- 正常 half-close 不进入 fatal watchdog，反向数据传输和 FIN 行为保持不变。

### 2.3 非目标

- 不改变 tunnel wire protocol、认证、压缩或 payload framing。
- 不以 `StreamClose`、删除 retention 或杀死 worker 代替真实 terminal 收敛。
- 不把 Linux epoll、Windows IOCP、macOS kqueue 的事件循环统一成同一种实现。
- 不把所有 connection 生命周期逻辑迁入 `TqStreamLifetime`；owner 只通过窄接口请求 connection
  escalation。
- 不在本设计中处理与 terminal 收敛无关的 relay 性能重构。

## 3. 跨平台公共不变量

### 3.1 四种生命周期事实必须分离

| 事实 | 表示内容 | 不能推导出的结论 |
|---|---|---|
| TCP closed | TCP fd/socket 已终止 | QUIC stream 已 terminal |
| data-plane stopped | relay 不再接受新 I/O | tunnel 可以无条件删除 |
| terminal handoff complete | callback/retention/watchdog 已脱离 tunnel/relay | 已收到 terminal |
| terminal observed | owner 已处理 `SHUTDOWN_COMPLETE` | 对端一定已确认 RESET |

Admin inventory 中 tunnel 从 active 消失，只表示业务 tunnel 已完成 handoff 并释放；stream 的最终
状态必须从 terminal ledger 查询。

### 3.2 唯一 wrapper owner 和唯一 close 路径

- `TqStreamLifetime` 继续独占 `MsQuicStream*`。
- `StreamClose` 只允许由 `TqStreamLifetime` 析构中的 `delete MsQuicStream` 间接执行。
- terminal sink、watchdog、connection controller、relay、reaper 均不得直接 close handle。
- started owner 在 terminal 前始终由 retention registry 强持；API 失败、worker stop 和 watchdog
  timeout 均不得解除 retention。
- callback 内释放 retention 后，callback guard 仍保证 wrapper 在 callback 返回前存活。

### 3.3 Terminal 与 shutdown ledger 分离

扩展 owner 的公共 terminal phase：

```cpp
enum class TerminalPhase : uint8_t {
    Active,
    ShutdownReserved,
    ShutdownSubmitted,
    TerminalObserved,
    Closed,
};
```

该 phase 与现有 wrapper start phase、方向性 FIN/abort ledger 并存：

- wrapper start phase 继续表达 `CreatedNotStarted/Starting/Started/StartFailed/TerminalPublished/Closed`；
- `TerminalPhase` 表达最终释放 handoff 的推进情况；
- TCP EOF、QUIC FIN、peer abort 和 `SEND_SHUTDOWN_COMPLETE` 不发布 `TerminalObserved`；
- 只有 `SHUTDOWN_COMPLETE` 同时推进 wrapper phase 和 terminal phase；
- `TerminalPhase::Closed` 只在析构的一次性 stream exchange 中写入。

### 3.4 Handoff 是 tunnel 释放前的同步屏障

started tunnel 最终释放前必须按以下顺序完成：

```text
1. FreezeDataPlane
2. RetainUntilTerminal（已存在则幂等成功）
3. 创建并发布 worker-independent terminal sink
4. Active -> ShutdownReserved
5. StreamShutdown(ABORT | IMMEDIATE)
6. 分类并记录 QUIC_STATUS
7. 成功时 ShutdownReserved -> ShutdownSubmitted
8. 发布 DataPlaneStopped/HandoffComplete
9. reaper 才可释放 tunnel context、TCP 和 relay state
```

第 1–4 步与平台 registration/activation mutex 或等价 control 协议线性化；第 5 步在锁外调用
MsQuic；第 6–8 步只更新共享 ledger/control，不等待 callback。terminal callback 可以在第 5 步
返回前重入并赢得竞争，调用线程必须把它作为幂等成功处理，不能把状态退回 active。

## 4. 公共组件与接口

### 4.1 `TqTerminalShutdownResult`

```cpp
struct TqTerminalShutdownResult {
    QUIC_STATUS Status{QUIC_STATUS_SUCCESS};
    bool Submitted{false};
    bool AlreadyTerminal{false};
    bool RetryScheduled{false};
    uint32_t Attempt{0};
};
```

结果语义：

- `QUIC_STATUS_SUCCESS` 和 `QUIC_STATUS_PENDING` 都表示 shutdown operation 已成功交给 MsQuic，
  `Submitted=true`；
- 已 terminal 返回 `AlreadyTerminal=true`，不再调用 stream API；
- 同一 shutdown 已 reserved/submitted 的重复调用返回当前事实，不重复提交；
- 同步失败保留 desired intent、sink 和 retention，记录 status，并由公共 retry scheduler 决定
  `RetryScheduled`；
- 结果不得只返回 bool，调用方必须可区分提交、terminal、失败和重试。

### 4.2 `BeginTerminalShutdown()`

```cpp
TqTerminalShutdownResult TqStreamLifetime::BeginTerminalShutdown(
    uint64_t errorCode,
    std::shared_ptr<TqStreamLifetime::Target> terminalSink,
    std::shared_ptr<TqTerminalEscalation> escalation) noexcept;
```

接口契约：

1. 在 owner control mutex 内验证 phase、建立 retention、发布 sink、预留
   `ShutdownReserved` 和 API lease。
2. 锁外调用 `RequestShutdown(AbortBothImmediate, errorCode)`。
3. `AbortBothImmediate` 映射为
   `QUIC_STREAM_SHUTDOWN_FLAG_ABORT | QUIC_STREAM_SHUTDOWN_FLAG_IMMEDIATE`。
4. 成功提交后写 `ShutdownSubmitted`；失败时释放 reservation 但保留 desired intent。
5. terminal 并发到达时 terminal wins；返回值反映 `AlreadyTerminal`。
6. sink 发布后禁止恢复到 tunnel/relay target；后续业务 event 由 sink fail-safe 处理。

### 4.3 `TqTerminalSink`

terminal sink 必须独立于平台 worker、relay map 和 tunnel context，保存：

```cpp
struct TqTerminalIdentity {
    uint64_t StreamId;
    uint64_t TunnelId;
    uint64_t ConnectionId;
    TqTunnelRole Role;
    TqRelayBackend Backend;
};
```

以及 shutdown reason/error code、每次 `QUIC_STATUS`、attempt、handoff/submitted/terminal 时间戳、
最后一个 stream event、watchdog 状态、connection escalation 状态和 once-only accounting token。

sink 处理规则：

- `PEER_SEND_ABORTED`、`PEER_RECEIVE_ABORTED`：记录方向事实，不访问已释放 relay；
- `SEND_COMPLETE`：通过公共 completion registry 归还 operation；
- `SEND_SHUTDOWN_COMPLETE`：记录本地 send 方向完成；
- `SHUTDOWN_COMPLETE`：由 owner 先发布 terminal，再通知 sink 完成 accounting、取消 watchdog；
- `RECEIVE`：不返回伪成功或悬空 `PENDING`，执行 fail-safe receive disable/zero-byte accept 并保持
  shutdown intent；
- 任何路径均不得直接访问 tunnel context、TCP fd、platform worker 或 stream handle。

引用方向固定为：retention registry 强持 owner，owner router 强持 sink，sink 弱持 owner并强持独立
ledger/escalation token；禁止 `owner -> sink -> owner` 强引用环。

### 4.4 Watchdog 与 escalation

新增公共 `TqTerminalWatchdog`，按 monotonic clock 管理，不为每个 stream 创建线程。它维护弱 owner、
独立 ledger 和 connection escalation token；callback terminal 时 once-only cancel。

```cpp
class TqTerminalEscalation {
public:
    virtual ~TqTerminalEscalation() = default;
    virtual void RequestConnectionShutdown(
        uint64_t connectionId,
        uint64_t streamId,
        QUIC_STATUS streamStatus,
        uint64_t errorCode) noexcept = 0;
};
```

失败策略：

| 条件 | 动作 |
|---|---|
| shutdown `SUCCESS/PENDING` | 启动 5 秒 terminal watchdog |
| shutdown 同步失败且未 terminal | 按 10/50/250 ms 最多重试 3 次 |
| phase 不允许调用且未 terminal | 立即请求 connection shutdown |
| 5 秒无 terminal | once-only connection shutdown/reconnect escalation |
| escalation 后 30 秒仍无 terminal | 标记 `terminal_timeout`、告警并阻断发布；不直接 close stream |
| connection 已 closing | 不重复关闭，继续等待其 stream terminal callbacks |

connection escalation 必须通过 stable connection id/generation 查找当前 connection owner，不能保存裸
`MsQuicConnection*`。旧 generation 的 timeout 不得关闭复用槽位中的新 connection。

### 4.5 Reaper 门禁

公共 reaper predicate 从单一 `RelayStopped` 扩展为：

```text
DataPlaneStopped
AND TerminalHandoffComplete
AND LocalOperationOwnershipTransferredOrDrained
```

- handoff 成功提交或 owner 已 terminal 时，tunnel 可以删除而无需等待 callback；
- shutdown 同步失败但 retry/sink/escalation owner 已独立接管时，也可删除 tunnel；
- sink、retry 或 escalation 仍依赖 tunnel/relay raw pointer 时，handoff 不完整，禁止 reaper；
- reaper 不调用 stream API、不取消 watchdog、不删除 retention、不等待 MsQuic callback。

## 5. 平台适配设计

### 5.1 Linux / epoll

- `AbortRelayAndRelease()` 不再自行拼接 stop 与 shutdown；统一调用 terminal handoff。
- event queue 中的 terminal payload 只携带 relay id/generation、control 和 sink/ledger，不携带可解引用
  tunnel/stream raw pointer。
- `EPOLLERR/HUP/RDHUP` 与 terminal/reaper 竞争时，只有赢得 `ShutdownReserved` 的路径提交一次
  abort；late TCP event 只做本地 operation/accounting cleanup。
- worker stop 前把 active route 切换到 terminal sink；queue-full fallback 只能使用 sink/control。
- fd close 后验证不存在对应 `CLOSE-WAIT`，epoll stale tagged event 不能命中新 relay generation。

### 5.2 Windows / IOCP

- terminal handoff 必须在 IOCP relay close/retire 前完成，不能用 pending overlapped 数量为零代替
  wrapper terminal。
- posted callback/control/terminal operation 使用 heap-owned typed envelope 和 shared completion
  state；不得把 caller 栈地址或 tunnel-owned handle 放入 IOCP。
- GQCS `FALSE + non-null OVERLAPPED` 仍完成 operation retirement；stop 前已成功 post 的 terminal
  work 必须执行或被显式转移到 worker-independent sink。
- socket cancel、`CancelIoEx`、worker stop 和 terminal callback 可以竞争，但 shutdown、active
  accounting、socket close和operation settle各执行一次。
- connection escalation 通过 connection id/generation 投递到 runtime/connection owner，不保存
  worker或connection裸指针。

### 5.3 macOS / Darwin kqueue

- prepare/publish/commit registration transaction 保持不变；terminal handoff 与 commit 使用相同
  activation/generation 协议决定谁赢得 stream target。
- kqueue `EV_EOF/EV_ERROR`、retired relay purge、queue-full fallback 与 worker stop 均只能通过
  shared control/sink推进终止。
- route publish 后 binding identity 不再修改；sink 不强持 relay/worker，worker 退出后 purge 不调用
  worker-thread-only handler。
- retired relay 可保留 send/receive operation owner和buffer accounting，但不得保留无 lease 的裸
  stream capability。
- fd reuse 测试必须证明旧 kqueue event、旧 watchdog 和旧 connection generation不能影响新 tunnel。

### 5.4 公共代码与平台代码边界

| 公共代码 | 平台代码 |
|---|---|
| terminal phase/ledger | freeze 本平台数据面 |
| shutdown status 分类与 retry policy | 投递/执行平台 control operation |
| terminal sink 数据模型 | 排空 epoll/IOCP/kqueue ownership |
| watchdog deadline 与状态 | 连接 runtime 查找和 shutdown 调度 |
| retention detail snapshot | 平台 fd/socket/worker 明细 |
| Admin JSON 公共字段 | backend-specific diagnostic 字段 |

任何平台不得改变 `SUCCESS/PENDING` 分类、重试次数、默认 watchdog deadline、terminal 唯一事实源或
`StreamClose` 唯一路径。

## 6. 可观测性与 Admin 契约

### 6.1 Retention 明细

Admin 增加 terminal retention 明细数组，每条至少包含：

```json
{
  "stream_id": 17,
  "tunnel_id": 133,
  "connection_id": 2,
  "role": "client",
  "backend": "linux",
  "terminal_phase": "shutdown_submitted",
  "retained_age_ms": 428,
  "shutdown_intent": "abort_both_immediate",
  "shutdown_status": "pending",
  "shutdown_attempt": 1,
  "shutdown_submitted_at_ms": 1720000000000,
  "terminal_observed_at_ms": 0,
  "last_stream_event": "send_shutdown_complete",
  "in_tunnel_registry": false,
  "relay_active": false,
  "tcp_valid": false,
  "watchdog_state": "armed",
  "connection_escalated": false
}
```

快照只复制 immutable identity 和原子/受锁 ledger 值，不在 Admin 线程解引用 relay、tunnel、stream
或 connection raw pointer。支持按 backend、connection id、tunnel id和terminal phase过滤。

### 6.2 聚合指标

公共及分平台指标至少包括：

- terminal handoff started/completed/failed；
- shutdown submitted、pending、sync failure、retry；
- terminal observed；
- watchdog armed/canceled/timeout；
- connection escalation；
- terminal timeout pending；
- retained owner count/oldest age；
- terminal sink pending；
- duplicate stop/shutdown/terminal suppressed；
- accounting/destructor/stream close exactly-once violation。

告警门限：任一 `terminal_timeout` 立即告警；retained oldest age 超过 5 秒触发 warning，超过 30 秒
触发 critical；retained count 连续 5 分钟增长且 active tunnel 未同步增长时触发 leak warning。

## 7. 系统级端到端功能图

| 链路 | 输入/故障 | 必须观察的结果 |
|---|---|---|
| client TCP -> client relay -> QUIC stream | TCP FIN | client 提交一次 FIN，server 继续反向发送，不启动 fatal watchdog |
| server QUIC -> server relay -> target TCP | peer FIN | 排空后 `SHUT_WR`，不把 half-close 当 terminal |
| target TCP -> server relay | RST/ECONNRESET | freeze、handoff、`ABORT|IMMEDIATE`，server tunnel 可安全释放 |
| server MsQuic -> client MsQuic | abort传播 | client 收到 peer abort/terminal，client TCP 和 relay 收敛 |
| terminal callback -> owner -> sink | tunnel 已删除 | owner发布terminal，sink取消watchdog并完成一次accounting |
| shutdown同步失败 -> retry | OOM/invalid state注入 | 状态和retention保留，按策略重试或升级connection |
| watchdog -> connection runtime | callback丢失/worker停止 | generation-safe connection shutdown，所有stream获得terminal |
| Admin -> retention registry | active tunnel为零但retained存在 | 仍可定位stream/tunnel/connection、phase、age和最后事件 |

每条链路都必须同时断言用户可见 socket 行为、状态转换、日志、metrics、Admin 快照和资源归零。

## 8. 测试策略与覆盖矩阵

### 8.1 公共单元测试

- `BeginTerminalShutdown` 对 `SUCCESS/PENDING`、同步失败、已 terminal、重复调用的精确结果。
- `ShutdownReserved` 与 callback terminal 重入的线性化时序。
- 三次 retry 的退避、取消和 exhaustion。
- callback 内 release retention，wrapper 仅在 callback 返回后析构。
- terminal sink 不形成 owner 强引用环。
- watchdog cancel/timeout/escalation exactly-once。
- connection generation 复用隔离。
- retention detail snapshot 与并发 terminal 的一致性。

### 8.2 分平台确定性并发测试

每个平台均使用 barrier/latch，不用 sleep 猜测竞态：

1. handoff 前、shutdown down-call 中、提交后分别注入 reaper。
2. terminal callback 在 `StreamShutdown` 返回前重入。
3. TCP fatal event、admin stop、worker stop和connection shutdown四路并发。
4. callback/worker event queue 满，fallback 在 tunnel 已释放后执行。
5. terminal callback晚到，旧fd/handle storage和connection slot已复用。
6. shutdown提交成功但抑制terminal callback，验证watchdog升级而不直接close。
7. 重复abort、stop、terminal、reaper只推进一次accounting和析构。
8. 正常half-close持续反向传输，不触发fatal watchdog。

平台特有证据：Linux 检查 epoll stale tag和 `CLOSE-WAIT`；Windows 检查所有 OVERLAPPED/control
operation归零；macOS 检查 retired relay、kqueue stale event和worker-exited purge归零。

### 8.3 精确事故回归

三平台都执行：

```text
client TCP FIN
  -> client QUIC FIN
  -> server target TCP SHUT_WR
  -> target RST/ECONNRESET
  -> server ABORT | IMMEDIATE
  -> server tunnel context 在 handoff 后释放
  -> server 本地 SHUTDOWN_COMPLETE
  -> client peer abort/SHUTDOWN_COMPLETE
  -> 两端资源回到基线
```

最终断言：active tunnel/relay、terminal sink、watchdog、send completion 和 retained owner 均回到
基线；不存在对应 `CLOSE-WAIT`/未决 socket operation；没有 terminal timeout；accounting、wrapper
destructor 和 `StreamClose` 各一次，且 close 晚于 terminal callback 返回。

## 9. 性能、容量与 k6 基线

### 9.1 工作负载

使用现有 HTTP CONNECT/SOCKS5 入口和 k6/xk6 TCP 能力，三个平台采用同一 scenario：

| 场景 | 流量模型 | 目的 |
|---|---|---|
| baseline | 100 并发，5 分钟，80%正常FIN/20%RST | 建立延迟与资源基线 |
| peak | 500 并发，10 分钟，50 tunnel/s churn | 验证预期峰值 |
| spike | 30 秒内从100升至1000并发，保持2分钟 | 验证watchdog/sink调度突发 |
| stress | 每2分钟增加250并发直到首次门禁失败 | 找到容量拐点 |
| soak | 100并发、100 tunnel/s、30分钟 | 检测retention和operation增长 |

每个 tunnel payload 分布为 1 KiB 50%、64 KiB 40%、1 MiB 10%；连接池冷启动和预热各执行一轮；
真实 MsQuic 和真实 loopback/远端 TCP target，不 stub terminal callback。故障场景通过测试 hook 精确
控制 RST、shutdown status 和 callback barrier。

### 9.2 指标与门禁

- tunnel 建立 p95 不高于修改前基线的 105%，p99 不高于 110%；
- 正常数据吞吐不低于修改前同平台基线的 95%；
- fatal terminal 收敛 p95 小于 1 秒，p99 小于 5 秒；
- 非故障场景 terminal timeout 为 0，shutdown sync failure 为 0；
- dropped iterations 为 0，业务错误率低于 0.1%；
- peak/soak 结束后 30 秒 retained、sink、watchdog和platform operations回到基线；
- CPU、RSS 和 thread count 不持续增长，峰值相对旧基线增幅分别不超过 10%、10%、0 个常驻线程。

## 10. 异常条件与灾难恢复

| 触发/注入 | 用户影响 | 检测信号 | 缓解与恢复 | 验收标准 |
|---|---|---|---|---|
| StreamShutdown同步失败 | 单tunnel关闭延迟 | sync failure/retry指标和日志 | 3次重试后connection escalation | 不丢诊断，30秒内terminal或进入明确timeout |
| terminal callback被抑制 | 单tunnel retention延长 | watchdog timeout | generation-safe connection shutdown/reconnect | 其它connection不受影响，retention最终归零 |
| event/IOCP/kqueue队列满 | relay cleanup延迟 | queue-full和sink pending | worker-independent sink/fallback | 无UAF、无raw pointer、accounting一次 |
| worker退出/进程优雅停止 | 短暂中断 | worker stop、operation pending | freeze、handoff、drain或transfer | stop返回时平台operation归零 |
| connection断开 | connection内tunnel中断 | connection shutdown事件 | MsQuic为所有stream发布terminal并重连 | 旧generation watchdog不关闭新connection |
| CPU/内存压力 | callback/worker延迟 | saturation和oldest age | 限流新tunnel、扩大告警、connection escalation | 不直接强制close，恢复后retention下降 |
| 网络分区 | 对端abort传播延迟 | QUIC transport/timeout指标 | 本地IMMEDIATE terminal + connection恢复 | 本地5秒内terminal，业务可重连 |

恢复目标：单 stream 故障 RTO 5 秒、connection escalation RTO 30 秒；stream payload 不提供持久化，
因此 RPO 定义为已被上层确认的数据为 0 丢失，未确认的在途数据允许由客户端重试。任何超出 30 秒
的 retained stream 都视为发布阻断问题，而不是依靠进程重启隐藏。

## 11. 实施拆分与依赖

开发计划按平台独立推进：

1. Linux 计划负责公共 `TqStreamLifetime` terminal ledger、sink、watchdog、retention snapshot 的
   首次实现，并接入 epoll/reaper。
2. Windows 计划依赖公共接口，接入 IOCP operation ownership、worker stop和connection escalation；
   不复制公共状态机。
3. macOS 计划依赖公共接口，接入 kqueue、registration transaction、retired purge和fallback；不复制
   公共状态机。

为避免平台计划互相阻塞，公共接口在 Linux Task 1 完成后冻结。Windows/macOS 可以先编写失败测试
和 adapter，再基于冻结接口完成实现。每个平台形成独立可评审提交序列和本平台发布证据；最终合并
门禁要求三平台公共代码编译、公共单元测试通过，并完成各自事故回归和 soak。

## 12. 发布门禁

以下任一项成立均阻断发布：

- started stream 在真实 terminal 或明确 start failure 前析构/close；
- tunnel reaper 在 terminal handoff 完成前删除 context；
- `StreamShutdown` 返回值被丢弃或 `PENDING` 被当作失败；
- fatal 最终释放未使用 `ABORT | IMMEDIATE`；
- sink/watchdog/escalation 保存 tunnel、relay、worker、stream或connection裸指针；
- terminal timeout 被静默删除、强制 `StreamClose` 或只靠进程重启处理；
- 任一平台精确事故回归未证明两端资源归零；
- Admin 无法按 stream/tunnel/connection定位 retained owner；
- exactly-once violation计数非零；
- 性能、stress或soak门禁未满足。

## 13. 风险与决策

- **风险：** 公共 owner 状态增多，错误锁序可能造成 callback/down-call 死锁。  
  **决策：** control mutex 内只做状态、route、reservation和快照；所有 MsQuic/worker/connection
  调用均在锁外执行，并以确定性重入测试验证。
- **风险：** connection escalation 会扩大单 stream 故障影响。  
  **决策：** 只在三次同步失败、非法 phase 或 5 秒无 terminal 时升级，并用 connection generation
  隔离；这是避免永久 retained 的最后有界手段。
- **风险：** Linux 首先实现公共层可能引入平台偏置。  
  **决策：** 公共接口和状态语义由本设计冻结，Windows/macOS 计划只补 adapter；最终三平台
  review同时检查公共代码无 `#ifdef` 平台语义分叉。
- **风险：** Admin 明细快照增加锁竞争和内存。  
  **决策：** retention registry 只保存固定大小 identity/ledger，快照复制后锁外序列化，并通过
  peak/stress门禁限制开销。

