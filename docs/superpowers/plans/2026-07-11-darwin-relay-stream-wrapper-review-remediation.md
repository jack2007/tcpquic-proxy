# Darwin Relay Stream Wrapper 评审问题修复开发计划

> **实施规则：** 按 Task 顺序执行，每个 Task 先提交可稳定失败的确定性测试，
> 再实现修复。步骤使用 checkbox（`- [ ]`）跟踪；不得使用 sleep 猜测竞争时序。

**Goal:** 修复 `docs/superpowers/plans/2026-07-10-darwin-relay-stream-wrapper-terminal-lifetime.md`
末尾 2026-07-11 评审记录中的 6 个 P1 和 3 个 P2 问题，使 Darwin relay 的
public control、prepare/publish/commit、send/receive operation、terminal、worker stop 和方向性
shutdown 共享一套无裸指针、可线性化、可确定验证的生命周期协议。

**Architecture:** 使用每次 relay start 独立的 shared `TqRelayControl` + immutable generation
代替 Darwin worker/callback 对 `TqRelayHandle*` 的异步访问；registration 改为显式
prepare token、router target publish 和 worker-thread commit 三阶段事务。被发布的 binding
所有 callback-visible identity 在 publish 前一次性初始化，之后不再修改同一
`shared_ptr`/`weak_ptr` 实例。send completion 注册使用 RAII reservation 保证提交前所有
退出路径自动 cancel；RECEIVE 资源失败只发布 `ActiveShutdown`，不伪造
terminal。worker 退出前将 route 切到不引用 worker 的 sink，退出后 purge 只执行
lifecycle-safe 收敛。

**Source Review:** `docs/superpowers/plans/2026-07-10-darwin-relay-stream-wrapper-terminal-lifetime.md`

**Design:** `docs/superpowers/specs/2026-07-10-relay-stream-wrapper-terminal-lifetime-design.md`

**System Test:** `docs/superpowers/specs/2026-07-10-relay-stream-wrapper-terminal-lifetime-system-test-design.md`

**Tech Stack:** C++17, Darwin kqueue, MsQuic C++ wrapper, shared/weak ownership, CMake, ASan,
k6, fault-injection hooks.

---

## 1. 范围、非目标与发布约束

### 1.1 本计划范围

- Darwin production relay 从 tunnel 到 kqueue worker 的 control/FD/stream owner handoff。
- terminal callback、queue-full fallback、worker stop、retired purge 与 tunnel reaper 的收敛。
- Darwin TCP -> QUIC send completion 和 QUIC -> TCP deferred receive ownership。
- peer abort、`CANCEL_ON_LOSS`、half-close 和 stop-purge 事件语义。
- 相关 public control、metrics、Admin snapshot、确定性并发测试和 Darwin 系统门禁。

### 1.2 非目标

- 不重写公共 `TqStreamLifetime` phase 状态机，除非 completion reservation 需要
  增加最小的 move-only RAII API。
- 不在本计划中迁移 Windows/Linux worker；修改 `relay.h`/`relay.cpp` 时必须
  保持两端编译和现有行为。
- 不改变网络协议 payload framing。
- 不通过 timeout 强制 close started wrapper 来掩盖未收到 terminal 的问题。

### 1.3 发布阻断条件

- callback、late event 或 retired cleanup 仍解引用 tunnel-owned `TqRelayHandle*`。
- target publish 后 callback 能看到零 relay id、空 stream owner 或未初始化 generation。
- terminal 后仍能新注册 completion key、取得 API lease 或调用 `ReceiveComplete()`。
- 非 `SHUTDOWN_COMPLETE` 路径使用 `TerminalLogicalDetach`。
- 正常 terminal 后 tunnel context、active accounting、owner、registry 或 queue 不归零。
- worker 线程退出后 purge 仍进入 worker-thread-only 方法。

## 2. 评审问题到修复任务的映射

| 评审问题 | 根因 | 修复 Task | 核心证据 |
|---|---|---|---|
| P1-1 terminal 未唤醒 reaper | worker 清 public handle 但未交接 stop/accounting | Task 1 | terminal 后 context/control/activity 归零 |
| P1-2 send retention 泄漏 | completion key 注册不是 RAII 事务 | Task 3 | barrier 命中每个 pre-submit exit |
| P1-3 publish 早于 identity | binding 未完整初始化就可达 | Task 2 | publish 入口 callback snapshot 完整 |
| P1-4 同一 weak_ptr 并发读写 | callback reset、worker lock 无同步 | Task 2 | callback-visible pointer 发布后不变 |
| P1-5 allocation failure 伪 terminal | 资源失败误用 terminal event | Task 4 | owner phase 仍 active，只请求 ActiveShutdown |
| P1-6 fallback 访问裸 handle | worker/binding 保存 tunnel storage | Task 1 | 真实释放+原地复用存储测试 |
| P2-7 commit 无最终校验 | check 与 activation 不是单一线性化点 | Task 2 | terminal-before/after-commit 对照 |
| P2-8 stop purge 断言 | worker-exited purge 复用 data-plane handler | Task 5 | 真实 worker thread 退出测试 |
| P2-9 方向/CANCEL 语义 | peer/local 方向映射错误且事件合并 | Task 5 | shutdown flags/error code 精确断言 |

## 3. 必须在实现前固化的安全不变量

### 3.1 Public control 与 accounting

- 每次 relay start 创建新的 `shared_ptr<TqRelayControl>`，control 携带 immutable
  generation；不复用上一个 relay 的 control 实例。
- `TqRelayHandle` 只由 tunnel/caller 线程发布和清理。Darwin worker、binding、callback、
  event queue 和 retired state 只持 control + generation，不保存 handle pointer。
- terminal、queue fallback 和 worker stop 通过 `SignalStop(expectedGeneration)` 幂等发布
  stop；旧 control 不能修改新 handle。
- active accounting token 只能通过 control 的 once-only release 释放一次；
  registration failure、terminal reaper、explicit stop 和 runtime stop 可以竞争。

### 3.2 Callback target 与 registration transaction

- binding 的 relay id、route generation、weak relay、weak stream owner、control generation、
  completion state 和 precommit limit 在 `PublishTarget()` 前完整初始化。
- binding 发布后，同一 `shared_ptr`/`weak_ptr` 字段不再 assign/reset；可变生命周期
  只用 atomic phase 或受单一 mutex 保护的普通字段表示。
- 引用方向固定为：owner -> router -> binding；binding -> weak relay + weak owner + shared
  control；relay -> owner + binding。不允许 binding 强持 owner 形成环。
- prepare 不安装 kqueue filter、不启动 TCP 数据面、不写 public handle。publish 成功
  是 FD 所有权不可逆转移点。
- commit 在 binding activation mutex 内同时校验 owner phase、route generation、
  terminal/activation、control generation 和 map identity，再从 `Prepared` 进入 `Active`。
- terminal callback 使用同一 activation mutex 将 `Prepared|Active -> Terminal`，不等待
  worker/API lease。赢得 commit 的一方决定 registration 成功还是 rollback。

### 3.3 Send/receive operation

- completion key 注册后必须被 move-only reservation 持有；MsQuic Send 成功提交
  或 callback 已 claim 前，任意 return 都自动 cancel registry entry。
- terminal 可与已 admitted lease 竞争，但不能留下“key 已注册、Send 未提交、
  无人 cancel”的状态。
- managed pending receive 不保存可单独使用的裸 stream capability。callback 从
  binding weak owner 取得强 owner；active complete 必须取 lease。
- allocation/budget/queue 失败是 active fatal condition，只能请求 shutdown 并做本地
  accounting cleanup；只有 owner terminal 才允许 terminal discard。

### 3.4 Stop 与方向语义

- worker 退出前，active route 切到 worker-independent shutdown sink。sink 能处理 terminal、
  fail-safe RECEIVE 和 detached send completion，但不解引用 worker。
- worker 退出后 purge 不调用任何 worker-thread-only 方法。
- `PEER_SEND_ABORTED` 先合并 local receive abort；`PEER_RECEIVE_ABORTED` 先合并
  local send abort。worker 可按 policy 升级 `AbortBoth`，但不能先提交反方向。
- `CANCEL_ON_LOSS` 同步写入固定、62-bit 范围内的公共 relay error code，
  不发布 owner terminal。

## 4. 预计文件变更

- `src/tunnel/relay.h`, `src/tunnel/relay.cpp`, `src/tunnel/tcp_tunnel.cpp`：
  shared control/generation、once-only accounting、caller-owned handle publication。
- `src/tunnel/stream_lifetime.h`, `src/tunnel/stream_lifetime.cpp`：必要时增加 move-only
  send completion reservation。
- `src/tunnel/darwin_relay_worker.h`, `src/tunnel/darwin_relay_worker.cpp`：删除
  `PublicHandle`，引入 transaction、immutable binding identity、shutdown sink 和 safe purge。
- `src/tunnel/darwin_relay_event_queue.h`：typed close reason/disposition/control generation，
  pending receive 删除裸 stream capability。
- `src/runtime/relay_metrics.h`, `src/runtime/relay_metrics.cpp`：control、rollback、reservation、
  active failure 和 stop metrics。
- `src/unittest/darwin_relay_worker_io_test.cpp`, `darwin_relay_worker_queue_test.cpp`,
  `darwin_relay_metrics_test.cpp`, `stream_lifetime_test.cpp`, `tcp_tunnel_test.cpp`,
  `tunnel_reaper_test.cpp`, `router_runtime_test.cpp`：确定性回归和 metrics 门禁。
- `src/docs/thread_model_cn.md`, `docs/relay_macos.md`, `src/CMakeLists.txt`：文档和构建接线。

**实施依赖：** `Task 1 -> Task 2 -> (Task 3, Task 4) -> Task 5 -> Task 6`。
Task 3 和 Task 4 在 Task 2 的 immutable binding/transaction 契约完成后可并行；
Task 5 必须统一收口前面产生的 control、operation 和 active-failure event。

## Task 1: 用 shared control + generation 移除 Darwin 裸 public handle

**覆盖问题：** P1-1、P1-6。

**Files:** `relay.h`, `relay.cpp`, `tcp_tunnel.cpp`, `darwin_relay_worker.h/.cpp`,
`tcp_tunnel_test.cpp`, `tunnel_reaper_test.cpp`, `darwin_relay_worker_io_test.cpp`.

- [ ] **Step 1: 先增加正常 terminal -> reaper 失败测试**
  - 使用真实 `TqTunnelContext`/reaper harness，不只检查 worker relay map。
  - 启动 managed Darwin relay，记录 context destructor、`OnComplete`、active-relay count
    和 control stop 次数。
  - 分派真正 `SHUTDOWN_COMPLETE`，terminal event 正常入队并 drain，不注入
    queue-full。
  - 断言 reaper 回收 context，destructor/`OnComplete` 各一次，active count 恢复
    基线，owner/control weak ref 最终失效。

- [ ] **Step 2: 增加真实 handle storage 释放与原地复用测试**
  - 用 placement new 在同一块 storage 构造 handle A，注册 relay A。
  - 在 callback fallback barrier 上暂停，销毁 A，在同一 storage 构造 handle B
    并注册 relay B。
  - 释放 A 的 late terminal/stop/fallback，断言只更新 control A，B 的 Backend、
    worker、relay id、Stop 和 generation 不变。
  - ASan 必须 0 UAF；测试不得保留 A 的 handle owner 伪造安全。

- [ ] **Step 3: 定义 `TqRelayControl` 契约**
  - control 含 immutable `Generation`、atomic `Stop`、once-only
    `ActiveAccountingReleased`。
  - 增加 `SignalStop(expectedGeneration)` 和 `ReleaseActiveAccountingOnce()` helper；
    generation mismatch 只记诊断，不改状态。
  - control 不强持 worker、relay、binding、tunnel context 或 stream owner。

- [ ] **Step 4: 重写 Darwin start/publication 边界**
  - `TqRelayStartImpl()` 在 registration 前创建 control/accounting token，registration
    只接收 control + generation，不接收 handle pointer。
  - worker commit 成功后，只由 caller 将 Backend/worker/relay id/control/generation
    一次性发布到 handle。
  - 发布前或发布后立即观察到 control Stop 时，收敛为 start failure/
    immediate stop，FD 和 accounting 只归还一次。

- [ ] **Step 5: 删除 Darwin worker 的 `PublicHandle`**
  - 删除 `RelayState::PublicHandle`、`ClearPublicHandle()` 及 callback/fallback 对
    `RelayHandle.Stop` 的写入。
  - terminal、active shutdown、queue fallback、worker stop 只调用 control helper。
  - `TqRelayStop()` 由 tunnel-owned handle 捕获 Backend/worker/id/control，清空 handle，
    同步 unregister，并通过 control 释放 accounting 一次。

- [ ] **Step 6: 验证 normal terminal、explicit stop、queue full 和 runtime stop 竞争**
  - 四条路径任意排列均只回收 context、FD、accounting 一次。
  - terminal event 丢失时 control 仍使 reaper 完成本地 cleanup；正常 terminal
    与 queue-full terminal 最终结果一致。

- [ ] **Step 7: 独立提交**

```bash
rtk git add src/tunnel/relay.h src/tunnel/relay.cpp src/tunnel/tcp_tunnel.cpp src/tunnel/darwin_relay_worker.h src/tunnel/darwin_relay_worker.cpp src/unittest/tcp_tunnel_test.cpp src/unittest/tunnel_reaper_test.cpp src/unittest/darwin_relay_worker_io_test.cpp
rtk git commit -m "fix(darwin): replace relay public handle callbacks with shared control"
```

## Task 2: 将 prepare/publish/commit 改为完整事务

**覆盖问题：** P1-3、P1-4、P2-7。

**Files:** `darwin_relay_worker.h/.cpp`, `darwin_relay_event_queue.h`,
`darwin_relay_worker_io_test.cpp`, `darwin_relay_worker_queue_test.cpp`.

- [ ] **Step 1: 增加 publish 入口确定性 callback 测试**
  - 在 `PublishTarget()` 完成但 registration 尚未执行下一条语句的 barrier
    上分别注入 RECEIVE、terminal 和 peer abort。
  - RECEIVE 必须看到非零 relay id、正确 generation、可 lock 的 weak owner，
    并进入有界 precommit queue。
  - terminal 必须使 registration 返回 `Ok=false, TcpFdConsumed=true`，不发布
    public handle，FD 只 close 一次。

- [ ] **Step 2: 增加 commit 最终线性化对照测试**
  - barrier A：terminal 在最终 commit check 前发布，预期 rollback。
  - barrier B：terminal 在 `Prepared -> Active` 线性化点后发布，registration
    可返回成功，但 control 立即 Stop 并走正常 terminal cleanup。
  - 两条路径均断言 filter install/delete、FD close、precommit discard/drain 和
    map publication 各一次。

- [ ] **Step 3: 一次性初始化 callback-visible identity**
  - publish 前分配 relay id，设置 immutable route generation、weak relay、weak
    stream owner、control/generation、completion state 和 precommit limit。
  - `binding->Relay`/`binding->StreamOwner` 发布后不再 reset。terminal/retire 只修改
    atomics/activation phase，weak pointer 随对象自然析构变 expired。
  - pending receive 从 binding weak owner 取得强 owner，不从 map 中的 relay 推导。

- [ ] **Step 4: 引入 prepared token 和单一 activation mutex**
  - token 拥有 FD disposition、filter-installed bit、map publication、binding、precommit
    queue 和 rollback once flag。
  - target publish 成功时 token 不可逆接管 FD；caller 立即将自己的 FD
    置 invalid。
  - terminal seal、commit 和 activation failure 通过同一 mutex/phase 从
    `Prepared` 竞争进入 `Active|Terminal|Failed`。
  - mutex 内不调用 MsQuic、kevent、close、wait 或 destructor；只产生 disposition。

- [ ] **Step 5: 实现 commit 最终校验与 rollback**
  - 先安装 inactive filter，再在 activation mutex 内校验 owner phase、route generation、
    control generation、map identity 和 terminal bit。
  - commit 赢得后启用 filter、标记 `Committed` 并 drain precommit；terminal/failed
    赢得后删 filter、清 queue、request active shutdown、close FD 一次，不恢复旧 target。
  - public result 只在 commit 成功后生成。

- [ ] **Step 6: 结构审计与独立提交**
  - 用 `rtk rg` 列出 binding 所有 shared/weak pointer 写入点，确认 publish 后零写入。
  - 增加 weak expiry/destructor 计数，证明 owner/router/binding/relay 无强引用环。

```bash
rtk git add src/tunnel/darwin_relay_worker.h src/tunnel/darwin_relay_worker.cpp src/tunnel/darwin_relay_event_queue.h src/unittest/darwin_relay_worker_io_test.cpp src/unittest/darwin_relay_worker_queue_test.cpp
rtk git commit -m "fix(darwin): make relay target publication transactional"
```

## Task 3: 使 send completion 注册具有 RAII 回滚语义

**覆盖问题：** P1-2。

**Files:** `stream_lifetime.h/.cpp`, `darwin_relay_worker.h/.cpp`,
`stream_lifetime_test.cpp`, `darwin_relay_worker_io_test.cpp`.

- [ ] **Step 1: 加入“注册后、active recheck 前 terminal”失败测试**
  - completion key 注册后触发 barrier，另一线程发布 terminal 并 seal binding。
  - 断言 Send 次数为 0，registry active count 恢复基线，cleanup/operation
    析构各一次，最后一个外部 owner 释放后 weak owner 失效。

- [ ] **Step 2: 穷举 completion registration 后的所有退出点**
  - binding null/inactive、relay closing、stream mismatch、operation state failure、registry failure、
    Send synchronous failure、callback-before-return、terminal-before-Send。
  - 每个退出点都断言 registry count、operation destructor、buffer accounting 和
    owner weak expiry。

- [ ] **Step 3: 实现 move-only completion reservation**
  - reservation 持有 owner + key + armed bit；destructor 在 armed 时调用
    `CancelSendCompletion()`。
  - 只有 Send 成功提交或 callback 已 claim 并接管 operation 时才 dismiss。
  - callback-before-return 时 cancel 可返回 false，但 cleanup/operation 只结算一次。
  - reservation 不写入 owner completion record，禁止 owner self-cycle。

- [ ] **Step 4: 改写 `TrySubmitQuicSendOperation()` 为单一 cleanup 协议**
  - lease、reservation、known-operation registration、in-flight accounting 按固定顺序建立。
  - pre-submit return 不再手写分散 cancel；operation ownership 在 unique_ptr、callback
    claim 和 worker completion 之间只能有一方持有。

- [ ] **Step 5: 可观测性与独立提交**
  - snapshot 增加 active reservation、pre-submit rollback、unknown/duplicate claim、oldest age；
    测试结束 active/oldest 为 0。

```bash
rtk git add src/tunnel/stream_lifetime.h src/tunnel/stream_lifetime.cpp src/tunnel/darwin_relay_worker.h src/tunnel/darwin_relay_worker.cpp src/unittest/stream_lifetime_test.cpp src/unittest/darwin_relay_worker_io_test.cpp
rtk git commit -m "fix(darwin): roll back unsubmitted send completion reservations"
```

## Task 4: 将 RECEIVE 资源失败收敛为 ActiveShutdown

**覆盖问题：** P1-5，并完成 pending receive 裸 stream capability 移除。

**Files:** `darwin_relay_event_queue.h`, `darwin_relay_worker.h/.cpp`,
`darwin_relay_worker_io_test.cpp`, `darwin_relay_worker_queue_test.cpp`.

- [ ] **Step 1: 增加 allocation failure 对照测试**
  - 用精确 fault hook 仅使 pending receive allocation 失败，不影响其他分配。
  - callback 后断言 owner phase 仍为 `Starting|Started`，binding terminal=false，
    `QuicShutdownComplete` event 数为 0，active-shutdown reason 为 allocation failure。
  - 断言 callback 未返回 `PENDING`、未保存 MsQuic buffer、未调用
    `ReceiveComplete()`，shutdown intent 只提交一次。
  - 随后注入真正 terminal，断言此时才执行 `TerminalLogicalDetach`。

- [ ] **Step 2: 增加 allocation failure + queue full/worker stopped 组合测试**
  - active-failure event 入队失败时，worker-independent sink/control 保留 owner
    并请求 shutdown，callback 不等待 queue。
  - worker 已退出时不访问 worker pointer，terminal 后 owner/control 归零。

- [ ] **Step 3: 引入 typed active-failure reason/disposition**
  - 新增 `QuicActiveShutdown` 或等价 typed event，携带 relay owner/control generation、
    `ReceiveAllocationFailed|ReceiveBudgetExceeded|ReceiveQueueFull` reason 和 shutdown intent。
  - `QuicShutdownComplete` 只能由真正 `SHUTDOWN_COMPLETE` callback 产生；增加
    debug invariant counter/assertion 阻止其他调用点。

- [ ] **Step 4: 删除 pending receive 的裸 stream capability**
  - 删除 `TqDarwinPendingQuicReceive::ReceiveCompleteStream`。
  - callback 内同步 `ReceiveSetEnabled(false)` 仅使用当次 callback 参数，不保存。
  - deferred active completion 只从 `receive->StreamOwner->TryAcquireApi()` 取得 stream；
    owner terminal/abort discard 不调用 stream API。

- [ ] **Step 5: 固化 callback 失败契约**
  - allocation failure 未接管 buffer：将 consumed length 设为 0，通过 owner ledger 请求
    `AbortBoth`，返回 `QUIC_STATUS_OUT_OF_MEMORY`，不先调用 `ReceiveComplete()`。
  - callback budget 拒绝与 event queue full 分开定义：已返回 `PENDING` 的 buffer
    由 pending record once-only complete/discard，未接管 buffer 不伪造 deferred complete。

- [ ] **Step 6: 独立提交**

```bash
rtk git add src/tunnel/darwin_relay_event_queue.h src/tunnel/darwin_relay_worker.h src/tunnel/darwin_relay_worker.cpp src/unittest/darwin_relay_worker_io_test.cpp src/unittest/darwin_relay_worker_queue_test.cpp
rtk git commit -m "fix(darwin): keep receive allocation failures on active shutdown path"
```

## Task 5: 分离 worker-exited purge 并修正方向性事件

**覆盖问题：** P2-8、P2-9，并完成 worker-independent shutdown sink。

**Files:** `relay.h`, `darwin_relay_event_queue.h`, `darwin_relay_worker.h/.cpp`,
`darwin_relay_worker_io_test.cpp`, `darwin_relay_worker_queue_test.cpp`.

- [ ] **Step 1: 增加真实 worker thread 退出后 purge 测试**
  - 使用 `Start()` 创建真实 kqueue thread，不使用 `StartForTest()` 代替时序。
  - 在 worker exit barrier 前排队 `QuicPeerSendShutdown`、
    `QuicSendShutdownComplete`、peer abort、terminal、receive view 和 send complete。
  - 退出后由 Stop 线程 purge，断言 0 assertion/deadlock，每个
    command/operation/accounting 收敛一次。

- [ ] **Step 2: 分离 worker-local drain 与 lifecycle-safe purge**
  - worker-local drain 可调用 data-plane handler。
  - worker-exited purge 只允许 receive local discard、typed send settle、terminal cleanup、
    control signal 和 control command completion。
  - peer half-close/ideal-send 提示不再更新 worker-owned relay 字段；Stop 主路径
    已对 active relay 请求 `AbortBoth`。

- [ ] **Step 3: worker 退出前切换 shutdown sink**
  - 禁止新 worker callback admission，只等待 endpoint active-call guard，不等待
    MsQuic terminal/API lease。
  - sink 持 control、weak owner 和 completion state；terminal 发布 Stop，RECEIVE 消费 0
    并请求 receive abort，SEND_COMPLETE 使用 typed registry 收敛。
  - sink 不持 worker、relay raw pointer、handle pointer 或 stream pointer。

- [ ] **Step 4: 修正 peer abort 方向和本地 ledger**
  - `PEER_SEND_ABORTED` -> `AbortReceive`，标记 QUIC->TCP 方向关闭并丢弃
    pending receive。
  - `PEER_RECEIVE_ABORTED` -> `AbortSend`，停止 TCP->QUIC 新 send，已提交
    send 仍由 `SEND_COMPLETE` 归还。
  - worker 升级 `AbortBoth` 时，owner ledger 只提交尚未 submitted 的另一
    方向，同方向 API call 不重复。
  - 测试记录每次 fake `StreamShutdown` flags/error code，不只断言 owner phase。

- [ ] **Step 5: 固化 `CANCEL_ON_LOSS` error code**
  - 在公共 relay header 定义 62-bit 范围内的
    `TqRelayStreamErrorCancelOnLoss`（建议保留值 `0x54510001`）。
  - callback 同步设置 `event->CANCEL_ON_LOSS.ErrorCode`，更新方向诊断，
    不设 terminal/不切 route。

- [ ] **Step 6: 独立提交**

```bash
rtk git add src/tunnel/relay.h src/tunnel/darwin_relay_event_queue.h src/tunnel/darwin_relay_worker.h src/tunnel/darwin_relay_worker.cpp src/unittest/darwin_relay_worker_io_test.cpp src/unittest/darwin_relay_worker_queue_test.cpp
rtk git commit -m "fix(darwin): separate stopped purge and directional abort handling"
```

## Task 6: 可观测性、文档与 focused 回归

**Files:** `relay_metrics.h/.cpp`, `darwin_relay_metrics_test.cpp`, `router_runtime_test.cpp`,
`thread_model_cn.md`, `docs/relay_macos.md`, `src/CMakeLists.txt` as needed.

- [ ] **Step 1: 增加发布门禁 metrics**
  - control：active、stop signaled、generation mismatch、accounting duplicate release。
  - registration：prepared、commit success、terminal-before-commit rollback、activation failure、
    precommit bytes/depth。
  - operation：send reservation active/rollback/oldest age，receive active/discard、
    active-failure by reason。
  - stop：shutdown sink active、worker-exited purge events、stop remaining/oldest age。

- [ ] **Step 2: Admin/runtime snapshot 聚合与 invariant tests**
  - multi-worker 聚合不重复计算全局 terminal retention；sum/max 语义逐项测试。
  - 每个 focused case 结束后 active control、prepared、send reservation、pending receive、
    shutdown sink 和 stop remaining 全部为 0。

- [ ] **Step 3: 更新 Darwin 线程/生命周期文档**
  - 用时序图记录 tunnel caller、control、router callback、worker、reaper 的
    ownership 和线性化点。
  - 记录 transaction FD disposition、terminal 竞争和 worker stop sink。
  - 列出禁止模式：异步裸 handle、publish 后 reset weak pointer、伪 terminal、
    无 reservation completion key。

- [ ] **Step 4: 运行 focused regression**

```bash
rtk cmake --build build --target tcpquic_stream_lifetime_test tcpquic_darwin_relay_worker_io_test tcpquic_darwin_relay_worker_queue_test tcpquic_darwin_relay_metrics_test tcpquic_darwin_reactor_test tcpquic_tcp_tunnel_test tcpquic_tunnel_reaper_test tcpquic_client_tunnel_open_test tcpquic_router_runtime_test -j$(sysctl -n hw.ncpu)
rtk build/bin/Release/tcpquic_stream_lifetime_test
rtk build/bin/Release/tcpquic_darwin_relay_worker_io_test
rtk build/bin/Release/tcpquic_darwin_relay_worker_queue_test
rtk build/bin/Release/tcpquic_darwin_relay_metrics_test
rtk build/bin/Release/tcpquic_darwin_reactor_test
rtk build/bin/Release/tcpquic_tcp_tunnel_test
rtk build/bin/Release/tcpquic_tunnel_reaper_test
rtk build/bin/Release/tcpquic_client_tunnel_open_test
rtk build/bin/Release/tcpquic_router_runtime_test
rtk git diff --check
```

- [ ] **Step 5: 运行 ASan 确定性竞争组**
  - handle storage release/reuse、publish-entry callback、terminal-before/after-commit、
    send-registration terminal、receive allocation failure、worker-exited purge。
  - macOS ASan 不使用 unsupported `detect_leaks`；用 destructor/registry/metric 证明
    逻辑泄漏为 0。

- [ ] **Step 6: 独立提交**

```bash
rtk git add src/runtime/relay_metrics.h src/runtime/relay_metrics.cpp src/unittest/darwin_relay_metrics_test.cpp src/unittest/router_runtime_test.cpp src/docs/thread_model_cn.md docs/relay_macos.md src/CMakeLists.txt
rtk git commit -m "docs(darwin): document relay control and remediation gates"
```

## 5. 系统级端到端功能图

```text
HTTP CONNECT / SOCKS5 / port-forward client
  -> TqTunnelContext + per-start TqRelayControl(generation N)
  -> TqStreamLifetime + stable router
  -> Darwin prepare token (FD inactive)
  -> immutable binding identity published
  -> commit validation + kqueue activation
  -> TCP <-> QUIC forwarding
       TCP->QUIC: API lease -> completion reservation -> Send -> typed claim
       QUIC->TCP: callback owner -> pending receive -> TCP write -> API lease ReceiveComplete
  -> graceful close / peer abort / resource fault / worker stop
  -> owner terminal publication or ActiveShutdown
  -> control Stop(generation N)
  -> tunnel reaper -> TqRelayStop -> unregister/accounting once
  -> owner/control/registry/queue/context all zero
```

| 链路 | 必测断言 | 证据 |
|---|---|---|
| ingress -> tunnel/control | 每次 start 使用新 generation，旧 control 不影响新 handle | control trace、reuse test |
| owner/router -> prepared binding | publish 时 identity/weak owner 完整，数据面未激活 | barrier snapshot |
| publish -> commit | terminal 与 commit 只有一方赢得，FD/filter 各收敛一次 | fault trace、FD counter |
| TCP -> QUIC | pre-submit return 自动 cancel reservation，late completion typed claim | registry metrics |
| QUIC -> TCP | pending receive 持 owner 不持裸 stream，terminal discard 零 API | fake API counters |
| receive allocation failure | owner 仍 active，只提交 ActiveShutdown，不伪 terminal | phase/event counters |
| peer abort/CANCEL | 方向 flags 正确，error code 稳定，不发布 terminal | shutdown call log |
| worker stop | route 先切 sink，退出后 purge 不进 worker-only handler | stop timeline |
| terminal -> reaper | normal/queue-full terminal 都 signal control，context/accounting 一次归零 | destructor + metric |

## 6. 系统测试策略与覆盖矩阵

| ID | 场景 | 层级 | 主要门禁 |
|---|---|---|---|
| R01 | normal terminal -> control -> reaper | integration | context/accounting/owner 一次归零 |
| R02 | handle storage destroy + same-address reuse | ASan/concurrency | 0 UAF，旧 generation 不命中新 relay |
| R03 | RECEIVE 紧跟 target publish | deterministic concurrency | identity/owner 完整 |
| R04 | terminal-before-commit | deterministic concurrency | rollback，`Ok=false` |
| R05 | terminal-after-commit | deterministic concurrency | commit 成功后正常 cleanup |
| R06 | send key registered -> terminal -> active recheck | deterministic concurrency | registry/owner 不泄漏 |
| R07 | Send callback-before-return + failure | state machine | operation/accounting once-only |
| R08 | pending receive allocation failure | fault injection | ActiveShutdown only |
| R09 | allocation failure + queue full/stop | resilience | callback 不等待，sink 收敛 |
| R10 | worker 真实退出后 mixed-event purge | resilience | 0 assert/deadlock |
| R11 | peer send/receive abort | directional | 第一次 shutdown flags 方向正确 |
| R12 | CANCEL_ON_LOSS | protocol callback | 固定 error code，phase 不变 |
| R13 | HTTP CONNECT/SOCKS/port-forward | E2E | payload 100%，结束 counters=0 |
| R14 | 30 分钟 churn + worker/runtime stop | system/soak | owner/control/registry 无增长 |

## 7. k6 性能基线

复用 system-test design 的 HTTP CONNECT 拓扑，额外记录 control、activation mutex、
completion reservation 对 CPU/延迟的影响。

| Scenario | 模型 | 负载 | 断言 |
|---|---|---|---|
| baseline-short | constant-arrival-rate | 20 req/s, 5m | error <0.1%, p95 <500ms, p99 <1s |
| peak-short | ramping-arrival-rate | 20 -> 100 -> 300 req/s，各5m | dropped=0，control/registry 稳定 |
| spike | constant-arrival-rate | 500 req/s, 60s | 无崩溃/死锁，受控拒绝 |
| steady-active | constant-vus | 100/1,024 VU, 10m | active control=active tunnel，结束归零 |
| churn-soak | constant-arrival-rate | 100 req/s, 30m | owner/control/registry oldest age 不得无界增长 |
| breaking-point | staged | 每级 +250 req/s | 找到受控饱和点，不 silent corruption |

**数据模型：** 80% 1 KiB、15% 64 KiB、5% 1 MiB；short-tunnel 使用
`noConnectionReuse: true`，另跑 keep-alive 长连接；QUIC connection pool 使用 1/4/16。
固定 origin、证书、proxy 配置、binary SHA256 和网络条件，修复前后在同机执行。

**回归门禁：** p95/p99 回归 <=5%，吞吐回归 <=3%，Darwin worker CPU
增幅 <=5%，每 active stream 额外内存不高于基线 +1 KiB，
`dropped_iterations == 0`。

## 8. 容量、异常条件与恢复验证

### 8.1 容量梯度

- active tunnels：100 -> 1,024 -> 4,096。
- handoff rate：100 -> 500 -> 1,000/s。
- pending sends per stream：1 -> 8 -> 32。
- worker count：1 -> CPU/2 -> CPU 数。
- 每个梯度结束后等待 terminal/reaper 收敛，再断言 control、prepared、
  send reservation、pending receive、retired binding 和 stop remaining 为 0。

### 8.2 异常与恢复矩阵

| 场景 | 触发/注入 | 用户影响 | 检测信号 | 缓解/恢复 | 验收 |
|---|---|---|---|---|---|
| terminal queue full | 填满 callback queue | 单 tunnel 关闭 | queue-full + control stop | sink -> reaper | callback 不等待，资源归零 |
| terminal-before-commit | publish 后 barrier | open 失败 | rollback reason | remove filter/close FD/request shutdown | 不发 public handle |
| commit activation failure | kevent fault hook | open 失败 | activation metric | rollback token | FD/filter/precommit 一次收敛 |
| receive allocation failure | allocator fault | 单 tunnel 中止 | active-failure reason | AbortBoth + reaper | 不伪 terminal，真 terminal 后归零 |
| send registration race | barrier 发 terminal | 单 send 未提交 | reservation rollback | RAII cancel | registry/owner 不泄漏 |
| worker thread exit | exit barrier + mixed queue | active tunnel 断开 | sink/stop remaining | safe purge + reaper | 60s 内归零，0 assert |
| handle storage reuse | placement-new same address | 无可见影响 | generation mismatch | ignore stale control | 新 relay 状态不变 |
| kqueue FD reuse | close 旧 FD 并立即复用 | 无误关新 socket | FD token trace | once-only disposition | 新 FD 仍可用 |
| QUIC network partition | drop UDP 60s | 在途 tunnel 超时 | owner oldest age | timeout/reconnect | 恢复后 30s 内新 tunnel 成功 |
| process restart | SIGKILL + supervisor | 在途 tunnel 丢失 | process health | restart/reconnect | RTO <=60s，RPO=0 |
| CPU/memory pressure | stress/limit RSS | 延迟升高、新 tunnel 受控失败 | CPU/RSS/queue | backpressure/rollback | 无死锁，恢复后归零 |

每个异常场景恢复后统一执行：Admin health、10 个新 HTTP CONNECT、一次
payload hash 校验，再检查 owner/control/registry/queue 归零。任一 stuck object age
>30s 时保存 dump/trace 并判定失败，不强制 close wrapper 掩盖。

## 9. 实施进入、退出与发布门禁

### 9.1 进入条件

- 保留当前 9 个评审问题作为基线，不通过删测试/放宽断言规避。
- common owner/router tests 和现有 Darwin focused tests 在修复分支起点通过。
- 新 fault hook 仅在 `TCPQUIC_TESTING` 编译中可用，默认关闭。

### 9.2 每个 Task 退出条件

- 新增测试在修复前稳定失败、修复后稳定通过。
- 该 Task 影响的 owner/control/operation/FD/accounting 都有 once-only 断言。
- focused tests、ASan 相关 case、`rtk git diff --check` 通过。
- 不把下一 Task 才解决的未定义生命周期留在可运行 production 路径。

### 9.3 最终发布门禁

- R01–R14 通过；Darwin focused tests 和 ASan 为 0 failure、0 UAF、0 double free、
  0 invalid handle/API call、0 payload corruption。
- normal terminal、queue-full terminal、explicit stop、worker/runtime stop 后，context destructor、
  active accounting、FD close 均恰好一次。
- soak 结束后 owner、terminal retention、control、prepared relay、send registry、
  pending receive、shutdown sink、retired state 和 stop remaining 在 30s 内归零。
- k6 p95/p99 <=5% 回归，吞吐 <=3% 回归，worker CPU <=5% 增幅，
  dropped iterations=0，成功 workload error rate <0.1%。
- 异常恢复后 10 个新 tunnel 成功，process restart RTO <=60s，RPO=0。
- `rtk rg` 证明 Darwin production 不存在 `PublicHandle`、publish 后 weak pointer
  reset、pending raw stream capability 或非 terminal `QuicShutdownComplete` 生成点。

## 10. 风险、假设与开放问题

- **Common control 影响面：** `relay.h` 是跨平台接口。首版可保留
  `TqRelayStopControl` 类型名并扩展字段，降低 Linux/Windows 编译风险；是否改名
  `TqRelayControl` 以跨平台 diff 为准，但契约不可缩减。
- **Accounting owner：** 假设可把 once token 放入 control；若 metrics API 不允许，
  创建独立 shared activity token，但不回退到裸 handle 计数。
- **kqueue filter：** 如果无法原子“安装但不启用”，先使用
  `EV_ADD|EV_DISABLE` 再在 commit 后 `EV_ENABLE`；不允许 publish 前启用数据面。
- **CANCEL_ON_LOSS error code：** 提议值 `0x54510001` 需确认未与已公开的
  error registry 冲突。若已有统一 enum，必须复用其保留值。
- **TSAN 可用性：** macOS 暂无 TSAN 时，使用 barrier + ASan + publish 后零
  pointer mutation 静态审计补足，不得以“未复现”关闭 P1-4。
- **Reaper polling 延迟：** focused test 可用 reaper wake hook 确定性验证；
  system gate 仍以实际 poll/recovery 延迟为准。

## 11. 最终检查清单

- [ ] 9 个评审问题均有失败测试、实现修复和回归证据。
- [ ] Darwin callback/late event/retired cleanup 不解引用 tunnel-owned handle 或 worker capability。
- [ ] transaction 的 identity、generation、FD 和 activation 只收敛一次。
- [ ] send completion 在所有 pre-submit exit 上自动 rollback，terminal 不泄漏 owner。
- [ ] receive allocation failure 仅走 ActiveShutdown，pending receive 不持裸 stream。
- [ ] worker-exited purge 不进 worker-only handler，shutdown sink 不引用 worker。
- [ ] peer abort 方向、`CANCEL_ON_LOSS` error code 和 terminal 分类符合设计。
- [ ] focused、ASan、E2E、k6、capacity、soak 和异常恢复门禁通过。
- [ ] owner/control/registry/queue/retired/context metrics 在测试结束 30s 内归零。
- [ ] `rtk git diff --check` 通过，文档与实现的线程/所有权说明一致。
