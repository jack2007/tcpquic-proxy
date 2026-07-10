# Darwin Relay Stream Wrapper Terminal Lifetime Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 消除 Darwin relay 在 terminal callback 返回后由 kqueue worker、retired relay 或 send completion cleanup 读取自动析构 `MsQuicStream` wrapper 的风险。

**Architecture:** relay-capable stream 使用公共 manual owner和 stable callback router；wrapper callback/context 启动前设置一次。router target 安全切换到 Darwin binding。kqueue stream API 调用持有 owner lease；terminal callback 在 owner发布终态并 seal target，不等待 worker/API lease；retire 不再依赖 `RetiredStream`。

**Design:** `docs/superpowers/specs/2026-07-10-relay-stream-wrapper-terminal-lifetime-design.md`

**System Test:** `docs/superpowers/specs/2026-07-10-relay-stream-wrapper-terminal-lifetime-system-test-design.md`

**Tech Stack:** C++17, Darwin kqueue, MsQuic C++ wrapper, shared/weak binding ownership, CMake Darwin relay tests.

**Prerequisite:** 平台中立 lifetime owner、stable router 与三个 tunnel target handoff 路径已经按 Linux
计划 Task 1（或等价的独立公共变更）实现；本计划只接入 Darwin，不复制公共 owner。

---

## File Structure

- Add or Modify: `src/tunnel/stream_lifetime.h` / `.cpp`
  - 复用平台中立 manual owner/phase/terminal retention/router/API lease，不创建 Darwin 私有副本。
- Modify as needed: `src/unittest/stream_lifetime_test.cpp`, `src/CMakeLists.txt`
  - 复用公共 owner phase、方向 ledger、router/lease线性化和析构一次测试，不创建 Darwin 私有状态机。
- Modify: `src/tunnel/relay.h` / `relay.cpp`, `src/tunnel/tcp_tunnel.cpp`
  - 复用 client tunnel、server dispatcher及其 tunnel/speed-control target的 manual owner handoff。
- Modify: `src/tunnel/darwin_relay_worker.h`
  - 增加 terminal close disposition/helper 和测试接口。
- Modify: `src/tunnel/darwin_relay_worker.cpp`
  - 将现有 `StreamCallback` 改为 router target dispatch，不安装或撤销 wrapper handler。
  - router callback 内发布 owner terminal并 seal target。
  - terminal retire 不读取 stream。
  - 从 terminal send-completion cleanup 移除 `RetiredStream` 依赖。
- Modify: `src/tunnel/darwin_relay_event_queue.h`
  - 如需增加 terminal 标志，只保存逻辑 owner/payload，不保存 stream pointer。
- Modify: `src/unittest/darwin_relay_worker_io_test.cpp`
  - 增加 wrapper 失效、pending send、late kqueue event 和 fallback close 测试。
- Modify: `src/unittest/darwin_relay_worker_queue_test.cpp`
  - 覆盖 terminal event queue full/stop purge ownership（如适合）。
- Modify as needed: `src/runtime/relay_metrics.*`, `src/docs/thread_model_cn.md`, `docs/relay_macos.md`
  - 输出 retention/router/registry指标并更新 Darwin kqueue生命周期说明。

## Task 1: 接入公共 lifetime owner

- [ ] Darwin registration 接收 owner，`RelayState` / `StreamBinding` 持有 owner，不再以裸 pointer 表达所有权。
- [ ] `RelayState` / `StreamBinding` 持有 owner和 route target adapter；所有非 callback API 通过 lease。
- [ ] owner phase只包含 `CreatedNotStarted`、`Starting`、`Started`、`StartFailed`、
      `TerminalPublished`、`Closed`；shutdown/half-close使用独立方向 ledger。started owner由 terminal
      retention保活，start failure和terminal各自通过 once-only token解除 retention。
- [ ] `TryAcquireStreamApi()`、shutdown reservation、target publish 与
      `PublishTerminalAndTakeTarget()` 使用公共 owner中同一 control线性化协议；terminal发布不等待
      已有 lease，但发布后不能再成功取得 lease，禁止仅做一次 phase load后延迟调用 API。
- [ ] 复用公共 outgoing/accepted factory 和 `Starting` 状态；不得在 Darwin registration 中重新包装 raw stream。
- [ ] terminal callback 不等待 lease，不获取 worker 调用 MsQuic 时可能持有的锁。
- [ ] owner/lease 不授权 kqueue worker 或 retired cleanup 跨线程写已启动 stream 的 callback/context。
- [ ] 明确 register failure、queue fallback、worker stop 和 retired purge 的 owner 回收路径。
- [ ] client tunnel和 server accepted wrapper从创建时使用 manual cleanup；禁止在 terminal callback 内转换，因为 adapter 已提前缓存 `DeleteOnExit`。
- [ ] 验证同步/异步 client open 与 server incoming dispatcher 均传递同一公共 owner，而不是重新 adopt 裸 pointer。
- [ ] Darwin registration 只发布 router target；采用 prepare/publish/commit 或等价事务避免 registration failure 的半提交路由。
- [ ] Darwin target adapter不保存无保护的 `RelayState*` / worker pointer；callback snapshot使用安全 owner/ref guard和deferred retire。
- [ ] 设计强引用方向并加析构测试，禁止形成 `owner -> router -> target -> binding/owner` 环；target detach 后只允许已有 callback guard完成。
- [ ] target publish 与 terminal take共享公共线性化协议；terminal 后 Darwin target publish必须失败。
- [ ] Darwin relay prepare保持 kqueue/TCP数据面 inactive；target publish成功后 commit才安装filter/启动数据面，并重新校验 owner phase/generation。
- [ ] prepare只返回内部 token，不写 public handle/`RelayStarted`；commit后一次性发布，rollback/terminal按 token清理。
- [ ] prepared target用有界 precommit queue接收 copied bytes和publish/commit间 RECEIVE；不安装filter或写 TCP。
- [ ] 明确 Darwin TCP fd disposition：prepare/publish前失败仍由 tunnel拥有；target publish成功是
      fd转给 prepared token/worker的不可逆点，caller立即置 invalid；commit/activation失败由 token
      删除可能安装的 kqueue filter并只 close一次，禁止 fd-number reuse误关新 socket。
- [ ] publish前失败可 rollback，publish后 activation失败 request shutdown并清理 queue，不恢复旧 target。
- [ ] 将现有 operation lookup接到公共 owner级 registry语义；router把 context仅作为 opaque key
      claim typed record，不按当前 Darwin target cast。terminal时允许已 admitted send lease对应的
      record暂存，最后一个 lease返回后 deferred reconcile；禁止 callback内强制清空或形成 owner环。
- [ ] 复用 connection/runtime级 completion retention：每个成功提交但未 `SEND_COMPLETE` 的
      record在 owner外保活 router，callback建立 owner guard并claim后解除；不能把 token放进
      owner内 record形成 self-cycle。
- [ ] 已启动 stream registration failure request shutdown并等待 terminal；仅 StreamStart失败/从未启动可直接 close。
- [ ] 复用公共方向性 shutdown desired/reserved/submitted ledger：同方向同强度只提交一次，
      graceful send可升级 abort，receive abort独立合并；API failure保留 desired intent/retention，
      terminal并发时 terminal wins。
- [ ] worker stop在 kqueue thread退出前把 route切到不引用 worker的 shutdown sink；sink/owner registry等待 terminal，不以 timeout强制 close。
- [ ] 用 shared `TqRelayControl`（名称可调整）+ generation替代 `RelayState::PublicHandle` 异步裸
      pointer；terminal sink、stop fallback和late event不得解引用已析构 tunnel context中的
      `TqRelayHandle*`，也不得通过已失效 raw worker pointer投递。

## Task 2: 增加 terminal wrapper 失效测试

- [ ] 注册 relay 后通过固定 router callback 分派 `SHUTDOWN_COMPLETE`。
- [ ] router callback 返回后、terminal event 尚未 drain 时断言 owner terminal、`binding->Active=false`、wrapper handler仍为 router且 target 已清空。
- [ ] callback 返回后释放 tunnel 侧 owner，通过 weak owner/析构计数确认 binding/lease 正确保活 wrapper。
- [ ] 驱动 event queue/kqueue worker执行 `CloseRelay()`、`RetireRelay()` 和 retired purge，断言不访问 wrapper。
- [ ] terminal event 前后注入 `EV_ERROR` 或 TCP read/write failure，断言只执行一次本地 close。
- [ ] 构造 known send operation 尚未由 worker消费的情况，确认 completion state 能在无 stream 下完成。
- [ ] 增加 lease/terminal 交错测试，确认 callback 不等待、terminal 后无新 lease、最后 lease 释放后只析构一次。
- [ ] 直接调用 router callback 的测试必须与 manual cleanup/owner析构计数组合；它本身不经过 private wrapper adapter。
- [ ] 增加 tunnel -> Darwin target handoff 与 terminal 并发测试，terminal只能由一个完整 target处理。
- [ ] 复用 `START_COMPLETE(success/failure)` 早于 start API返回、同步 no-callback reject、accepted
      factory handler安装边界测试。direct start failure不等待 terminal；`SEND(START)` failure在
      `START_COMPLETE(failure)` 后仍由 completion retention保活，直到随后
      `SEND_COMPLETE(Canceled=true)`再归还 record/buffer/accounting并释放 wrapper。两类 failure
      都要取走业务 target、通知失败一次，并拒绝后续 target publish/API lease。
- [ ] 在 Darwin注入 accepted owner/router allocation failure，验证 emergency C callback在 terminal后 close一次。
- [ ] 复用 shutdown API failure、同 intent重复、graceful -> abort升级、双方向请求和
      terminal-wins测试；同方向同强度最多提交一次。
- [ ] 增加 target detach与 callback并发，以及 kqueue worker退出后 terminal sink回收测试。
- [ ] 增加 tunnel send completion晚于 Darwin target切换测试，以及 prepare/publish/commit边界 terminal注入。
- [ ] 增加 public handle storage释放/立即复用、旧 control generation late terminal/stop事件和
      kqueue fd-number reuse测试，验证不访问旧 handle、不命中新 relay且 fd只 close一次。

## Task 3: 完成 callback-time terminal seal

- [ ] 在 shutdown/peer-abort callback 分支区分真正 `SHUTDOWN_COMPLETE` 与非 terminal peer abort；只有 shutdown complete 调用 `PublishTerminalAndTakeTarget()`、seal binding并采用 wrapper terminal 语义。
- [ ] peer send/receive abort 由当前 route snapshot完整处理，只更新相应半关闭状态并通过 owner发起一次 active shutdown；不得发布 terminal、清空 router target、设置 `Terminal=true` 或直接 retire owner，最终等待 `SHUTDOWN_COMPLETE`。
- [ ] router callback 使用 `PublishTerminalAndTakeTarget()` 原子发布 owner terminal并取走唯一 Darwin target snapshot。
- [ ] release-store `binding->Active=false`，必要时增加 `binding->Terminal=true`，避免普通 unregister 与 auto-delete terminal 混淆。
- [ ] router target切为空 terminal target；不得修改 `stream->Callback` / `stream->Context`。
- [ ] `EnqueueRelayCloseFromCallback()` 继续携带 `shared_ptr<RelayState>` owner，但不得携带 stream。
- [ ] 删除 `EnqueueRelayCloseFromCallback()` 在 MsQuic callback线程上的 sleep/yield重试循环；terminal
      post只做一次或严格有界的 nonblocking `TryPush`。queue full、worker stopped立即转交
      terminal disposition的 shutdown sink，不得把 stream保存到 retired state。
- [ ] fallback 必须保留 owner/binding 回收路径，不能在 callback 中同步等待 worker close，也不能直接进入会获取 `RelayMapAccessMutex` / `RelayMutex` / `relay->Mutex` 的 worker cleanup；post失败交给不引用 worker的 deferred shutdown sink或等价 control owner。

## Task 4: 重写 terminal retire

- [ ] 用显式 close disposition替换 `CloseRelay()` / `RetireRelay()` 的 `retainedCallbackRefs` 数值参数，至少区分 `ActiveShutdown` 与 `TerminalLogicalDetach`；各调用点必须按事件语义选择，禁止靠“当前 callback ref恰好为 1”推断 wrapper有效性。
- [ ] terminal retire 直接清空 `relay->Binding` 和 `relay->Stream`，禁止读取 `Stream->Context`。
- [ ] terminal path 不设置 `binding->RetiredStream`。
- [ ] `RetiredRelays` / `RetiredStreamBindings` 继续延长 relay/binding 生命周期，释放条件覆盖
      callback refs、route target guards、known/claimed/queued send records和pending receive owners；
      wrapper owner另由 shared ref保证 API lease退出前不析构。
- [ ] `ClearRetiredStreamCallbackIfSafe()` 不得由 terminal completion path 调用；逐项迁移调用者，能够删除 `RetiredStream` 时直接删除。
- [ ] 彻底删除 `RetiredStream` 和跨线程 `ClearRetiredStreamCallbackIfSafe()`；stable router不再需要该兼容路径。
- [ ] 删除 retire/fallback 中等待 `CallbackRefs` 的 yield loop；terminal callback和 worker cleanup都不等待 callback ref，retired owner按 ref/operation gate异步回收。
- [ ] 普通 unregister、TCP hard error和 worker stop采用 `ActiveShutdown`：先停本地数据面，通过 owner lease发起一次 shutdown，并保留 router target或安全 shutdown sink直到 terminal；不以 `CallbackRefs==0` 瞬时值证明 wrapper handler可写。
- [ ] `QuicShutdownComplete` 采用 `TerminalLogicalDetach`；`QuicPeerSendAborted` / `QuicPeerReceiveAborted` 只能触发 `ActiveShutdown`，`DrainEvents()` 与 `PurgeQueuedEventsForStop()` 不得再把三类事件合并为同一个 close分支。
- [ ] 已启动 stream 不得 abort 后立即析构 owner；只有 StreamStart失败或从未启动才可直接 close。
- [ ] terminal logical detach/retire幂等，duplicate/stale cleanup不重复加入 retired容器或释放 owner。

## Task 5: send completion 与 callback 并发审计

- [ ] `KnownSendOperationInfo::BindingOwner` 和 `CompletionState` 足以完成 terminal 后 send accounting，不得回读 stream。
- [ ] tunnel send与 Darwin relay send都通过类型安全 operation owner完成；router先把 `ClientContext` 当作 `void*` opaque key查公共 registry，claim成功后才取得 typed record，unknown/duplicate key不得盲目 cast或解引用。
- [ ] 同一 owner内 completion key不得在 late/duplicate callback仍可能到达时复用；使用单调 token、
      tombstone或不复用 envelope消除 ABA，并测试旧 key不能 claim地址复用后的新 operation。
- [ ] `CompleteDetachedQuicSend()` 只 unregister operation、更新计数并 delete operation。
- [ ] `EnqueueQuicSendCompleteFromCallback()` 同样不得在 callback线程等待 queue腾空；claim后的
      typed record在 post失败时由 callback-safe completion/sink once-only结算，不能重新注册或遗失。
- [ ] callback refs guard 覆盖 callback-time seal；retired binding 在 refs 清零前不释放。
- [ ] router在 generic `Active=false` filter前分类 RECEIVE。target失效但 owner尚非 terminal时，
      fail-safe sink把 `event->RECEIVE.TotalBufferLength=0` 后同步返回 success，使后续 receive停用，
      再在 control锁外通过方向 ledger请求 receive abort；不得静默按原长度返回 success、返回
      `PENDING`或保存 callback buffer。owner已 terminal的事故性 late RECEIVE只记诊断且不开始新 API。
- [ ] `SetQuicReceiveEnabled()`、retry send、flush callback-pending receive 在 terminal binding 后不执行 stream API。
- [ ] `TqDarwinPendingQuicReceive` 不保存裸 `Stream` capability；为 receive data/budget owner与 stream lifetime owner使用独立字段，释放 callback budget时不得顺带丢失后续 active completion所需的 owner。
- [ ] `CompleteDeferredQuicReceive()` 显式区分 active complete与 terminal discard并以 once-only状态竞争：active path取得 lease后调用一次 `ReceiveComplete()`，terminal/abort path只释放 slice/budget/accounting，不调用 stream API。
- [ ] graceful FIN前已接受的 pending receive必须经 active lease排空后再对 TCP `SHUT_WR`；瞬时
      lease失败不能直接 discard，只有 receive abort或 wrapper terminal可走本地 discard。
- [ ] 所有非 callback wrapper 访问除状态检查外还必须持有 lifetime lease。
- [ ] 审计 `Callback` / `Context` 写入点；只允许启动前 router初始化，terminal callback 也不得写入。

## Task 6: 回归普通 close 和 peer abort

- [ ] peer send/receive aborted 不一定意味着 wrapper 已自动删除，不能错误套用 shutdown-complete 的“wrapper 已失效”事实。
- [ ] 为 peer abort event保留方向性 half-close信息并走 `ActiveShutdown`；所有 stream API 调用仍要求 owner phase、binding/generation与 lease同时有效。
- [ ] TCP EOF只在 pending TCP->QUIC数据排空后提交一次 FIN；QUIC `RECEIVE(FIN)` /
      `PEER_SEND_SHUTDOWN`排空 receive后只关闭 TCP send方向；`SEND_SHUTDOWN_COMPLETE`、canceled
      `SEND_COMPLETE`和`CANCEL_ON_LOSS`只更新方向/operation状态，均不得发布 terminal。
- [ ] 覆盖 unregister、worker stop、queue purge 和 callback fallback，确保锁顺序与现有 state-lock 设计一致。
- [ ] late kqueue event 找不到 active relay时应直接丢弃，不从 retired relay 恢复 stream capability。
- [ ] duplicate/stale terminal event 必须幂等丢弃，不重复 retire owner；event 自带 `RelayOwner` 必须与 active map entry 一致。
- [ ] 覆盖 peer abort先入队、terminal后入队以及反向消费顺序，验证只有 terminal event执行 `TerminalLogicalDetach`，active-shutdown事件不会恢复已 seal target或重复发起 shutdown。
- [ ] worker stop/queue purge 测试证明 terminal retention保留 started owner，直到 terminal router callback 或连接 teardown完成。
- [ ] snapshot/metrics暴露 terminal-retained owner数量、最老 age和 stop剩余量，压力测试结束为零。

## Task 7: 验证

- [ ] 在 macOS 运行：

```bash
rtk cmake --build build --target tcpquic_stream_lifetime_test tcpquic_darwin_relay_worker_io_test tcpquic_darwin_relay_worker_queue_test tcpquic_darwin_relay_metrics_test tcpquic_darwin_reactor_test tcpquic_tcp_tunnel_test tcpquic_client_tunnel_open_test tcpquic_speed_test_test tcpquic_router_runtime_test -j$(sysctl -n hw.ncpu)
rtk build/bin/Release/tcpquic_stream_lifetime_test
rtk build/bin/Release/tcpquic_darwin_relay_worker_io_test
rtk build/bin/Release/tcpquic_darwin_relay_worker_queue_test
rtk build/bin/Release/tcpquic_darwin_relay_metrics_test
rtk build/bin/Release/tcpquic_darwin_reactor_test
rtk build/bin/Release/tcpquic_tcp_tunnel_test
rtk build/bin/Release/tcpquic_client_tunnel_open_test
rtk build/bin/Release/tcpquic_speed_test_test
rtk build/bin/Release/tcpquic_router_runtime_test
rtk git diff --check
```

- [ ] 在 ASan 构建运行 owner handoff、terminal lifetime 和 pending send completion cases。
- [ ] 运行 Darwin relay 性能/压力入口，记录 lifetime lease 对 kqueue worker CPU 和吞吐的影响。
- [ ] 使用 `rtk rg -n "RetiredStream|ClearRetiredStreamCallbackIfSafe|relay->Stream->|receive->Stream|stream->(Callback|Context)" src/tunnel/darwin_relay_worker.cpp` 确认 Darwin worker不再读写 wrapper handler、terminal path不再通过 relay/receive裸指针访问 wrapper；逐项审计剩余 stream API调用均为 callback参数内调用或有效 lease调用。
- [ ] 保持现有 callback queue-full backpressure、state/map lock 和 send completion 测试通过。
- [ ] 增加 active `ReceiveComplete` 一次与 terminal receive discard 零次 stream API 的对照测试。
- [ ] 执行 System Test Darwin矩阵、ASan、k6 baseline和worker-stop恢复场景。

## 验收标准

- relay 使用 manual cleanup，registration handoff 无双删/泄漏。
- owner phase 是 terminal/close 唯一事实源，started owner不被 worker stop提前析构。
- start success/failure由 `START_COMPLETE` 或明确 no-callback同步拒绝推进；方向 ledger独立于
  wrapper phase，FIN、peer abort、send completion和`CANCEL_ON_LOSS`不误发 terminal。
- terminal callback 发布 owner终态、seal target且不等待 worker/API lease；wrapper handler保持稳定。
- terminal event、fallback close、retire、purge、send completion 不读取 wrapper。
- pending send operation 仅依赖 completion/binding owner并只释放一次。
- 非 callback stream API 调用全程持有 lifetime lease。
- wrapper callback/context 只初始化一次；kqueue worker/terminal/retired cleanup均不改写。
- pending receive terminal discard 不调用 stream API，receive budget/accounting 正确归零。
- late kqueue event 不能恢复 terminal stream capability。
- peer abort、普通 unregister、worker stop 的现有语义和锁边界不回归。
- peer abort只发起 active shutdown，只有 `SHUTDOWN_COMPLETE` 发布 terminal并执行 terminal logical detach。
- terminal callback/fallback/retire不自旋等待 callback ref，也不进入可能与 worker API路径互锁的 cleanup。
- terminal sink、late kqueue event和completion只持 control owner/generation，不持裸
  `TqRelayHandle*`/worker capability；handle storage和fd number复用不命中新 relay。
- prepared relay只在 target publish后激活，handoff前后的 send completion由 operation owner处理。
- feature system-test release gates通过。
