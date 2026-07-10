# Linux Relay Stream Wrapper Terminal Lifetime Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 保证 Linux relay 在 `QUIC_STREAM_EVENT_SHUTDOWN_COMPLETE` callback 返回后不再解引用自动析构的 `MsQuicStream` wrapper，同时保留 pending send completion accounting 和 active TCP hard-error abort 行为。

**Architecture:** relay-capable stream 从创建时使用 `CleanUpManual`、公共 lifetime owner 和 stable callback router；wrapper callback/context 启动前设置一次。router target 从 tunnel 安全切换到 Linux binding。所有 worker stream API 持有 owner lease；terminal callback 在 owner 发布终态并 seal target，不等待 worker/API lease。

**Design:** `docs/superpowers/specs/2026-07-10-relay-stream-wrapper-terminal-lifetime-design.md`

**System Test:** `docs/superpowers/specs/2026-07-10-relay-stream-wrapper-terminal-lifetime-system-test-design.md`

**Tech Stack:** C++17, MsQuic C++ wrapper, Linux epoll, lock-free relay event queue, CMake unit tests.

**Linux I/O boundary:** 当前 relay 数据面直接管理 nonblocking `TqSocketHandle`/raw fd，
通过 `epoll_ctl`、`epoll_wait`、`readv`/`sendmsg` 驱动；仓库中没有 Asio
`posix::stream_descriptor` 或 `async_read`。本计划不得假设 Asio descriptor 会为 callback
保活 fd。需要处理的两个异步边界是 MsQuic callback -> `TqLinuxRelayEventQueue`，以及
`epoll_wait` 已返回但尚未消费的 stale TCP event。

---

## File Structure

- Add or Modify: `src/tunnel/stream_lifetime.h` / `.cpp`（名称可按项目惯例调整）
  - 提供 manual-cleanup owner、唯一 phase、terminal retention、API lease 和 stable callback router。
- Add: `src/unittest/stream_lifetime_test.cpp`
  - 独立覆盖公共 owner phase、router线性化、callback guard、send registry和析构一次语义；
    不把公共状态机只埋在平台 worker大测试中。
- Modify as needed: `src/CMakeLists.txt`
  - 注册公共 lifetime `.cpp` 和 `tcpquic_stream_lifetime_test`；如果实现为 header-only 则无需
    注册生产 `.cpp`。
- Modify: `src/tunnel/relay.h` / `relay.cpp`
  - 将 relay registration 从裸 pointer 契约升级为明确的 owner handoff、prepared token、
    fd disposition和 callback-safe public stop/control owner。
- Modify: `src/tunnel/tcp_tunnel.cpp`
  - 将 client tunnel和 server incoming dispatcher wrapper改为 manual owner/stable router。
  - 更新 `TqTunnelContext`、dispatcher target转交、pending bytes显式移交及失败回收。
- Modify: `src/runtime/speed_test.h` / `speed_test.cpp`
  - server speed-control attach接受 accepted stream的同一 owner/router target，不再改 wrapper handler。
- Modify: `src/tunnel/linux_relay_worker.h`
  - 接收 router target/binding owner，声明 terminal logical detach helper。
  - 调整 retiring state，不再保存 terminal `MsQuicStream*`。
  - 增加必要的 test hooks/counters。
- Modify: `src/tunnel/linux_relay_event_queue.h`
  - terminal event携带 relay generation/control owner而非 stream/handle裸指针；pending receive
    和 claimed send completion携带 typed owner。
- Modify: `src/tunnel/linux_relay_worker.cpp`
  - 将现有 `StreamCallback` 拆成 router target dispatch，不再安装/清除 wrapper handler。
  - terminal dispatch 在 owner 发布终态并 seal binding。
  - 重写 `ProcessQuicShutdownComplete()` cleanup 顺序。
  - 移除 terminal path 对 retired stream 的访问。
  - 审计所有 stream API call capability。
- Modify: `src/unittest/linux_relay_worker_io_test.cpp`
  - 增加 callback-time seal、失效 wrapper、pending completion 和 late epoll error 测试。
- Modify: `src/unittest/tcp_tunnel_test.cpp`, `src/unittest/client_tunnel_open_test.cpp`
  - 覆盖 manual owner 创建、pre-relay shutdown、dispatcher handoff 和 registration failure。
- Modify: `src/unittest/speed_test_test.cpp`
  - 覆盖 dispatcher -> speed-control target handoff和 terminal owner回收。
- Modify as needed: `src/tunnel/tunnel_reaper.h` / `.cpp`
  - reaper只观察 callback-safe stop/control state；不得让 terminal sink持有可能随
    `TqTunnelContext` 一起析构的裸 `TqRelayHandle*`。
- Modify as needed: `src/runtime/relay_metrics.*`
  - 仅在新增生产诊断计数时贯通 metrics；不改变已有字段语义。
- Modify: `src/docs/thread_model_cn.md`, `docs/relay_linux.md`
  - 更新 stable router、owner phase、三阶段激活和 shutdown sink线程/所有权模型。

## Task 1: 建立公共 lifetime owner、router 和 ownership handoff

- [ ] 实现 `TqStreamLifetime`（暂定名），唯一持有 `CleanUpManual` wrapper和 phase：
      `CreatedNotStarted`、`Starting`、`Started`、`StartFailed`、`TerminalPublished`、`Closed`；
      shutdown desired/reserved/submitted和双向 half-close保存在独立 ledger，不能扩张或反向
      修改 wrapper phase。start异步失败进入 `StartFailed`，不能假装等待永远不会到达的 terminal callback。
- [ ] 提供 outgoing/accepted 两个 factory：先建立 shared owner+router再打开/包装 stream，不允许运行期 raw-wrapper adopt。
- [ ] managed outgoing factory禁止 `QUIC_STREAM_START_FLAG_SHUTDOWN_ON_FAIL`；当前 phase把
      `StartFailed`视为可直接 close。若未来支持该 flag，必须先扩展为 failure后继续等待
      `SHUTDOWN_COMPLETE`的状态/retention路径，不能悄悄透传。
- [ ] outgoing含 `QUIC_SEND_FLAG_START` 的首次 Send或显式 `StreamStart` 使用
      begin/submit/complete/fail转换：API返回只表示提交结果，`QUIC_STREAM_EVENT_START_COMPLETE`
      才提交异步 start结果；允许 `START_COMPLETE`/terminal callback在发起 API返回前发生。
- [ ] `START_COMPLETE` success幂等进入 `Started`；failure发布 `StartFailed`、解除 start
      retention且不等待 `SHUTDOWN_COMPLETE`。failure必须通过
      `PublishStartFailureAndTakeTarget()` once-only关闭 route/API admission、取走 target并在锁外
      通知 open失败。direct `StreamStart` 此后可 close；若由已成功排队的
      `SEND(START)` 触发，start-send record/buffer仍由独立 completion retention保活，直到随后
      `SEND_COMPLETE(Canceled=true)` once-only归还。`StreamSend`同步失败才立即 unregister并释放。
- [ ] accepted factory在安装 handler前准备好 owner/router/retention，返回前直接进入
      `Started`；accepted stream不等待本地 `START_COMPLETE`。
- [ ] accepted owner/router allocation failure安装无分配 emergency C callback，abort后在 terminal close raw HQUIC；禁止直接 close已启动 raw stream。
- [ ] outgoing进入 `Starting`、accepted安装 handler前启用 terminal retention；只有确认未启动/无 callback或 terminal已发布的 owner才能 delete wrapper。
- [ ] router callback入口先从 retention/registry取得强 owner guard，再读取 route snapshot；
      terminal发布可以解除 retention，但 guard必须持续到 callback返回，禁止最后一个引用在
      `PublishTerminalAndTakeTarget()` 内或其临界区内析构 wrapper/router。
- [ ] 实现轻量 API lease；lease 持有 shared owner，禁止只返回无 owner 的裸 pointer。
- [ ] lease 根据 owner phase/API kind/route generation判断；lease admission、shutdown reservation、
      `PublishTarget`、`PublishStartFailureAndTakeTarget` 和
      `PublishTerminalAndTakeTarget` 使用同一个短 control临界区或经证明等价的原子协议。
      terminal 发布前 admitted 的 lease允许完成，callback不等待它；terminal后新 lease为零。
- [ ] 实现 stable callback router：wrapper callback/context 启动前设置一次，router 使用安全 route snapshot/target adapter完成 dispatcher、tunnel、relay handoff。
- [ ] router target不得与 lifetime owner形成无法在 terminal/failure 路径打破的强引用环；callback snapshot 必须保活当前 target。
- [ ] route snapshot不保存裸 `TqTunnelContext*` / relay / worker；target adapter用 shared owner或 `TryEnter/Leave` + deferred retire保证 callback期间存活。
- [ ] `PublishTarget`、`PublishStartFailureAndTakeTarget` 与 `PublishTerminalAndTakeTarget` 使用上述
      公共 control线性化协议；`StartFailed`/terminal后 publish必须失败，临界区内不调用
      MsQuic、等待 worker或触发 owner析构。
- [ ] 增加 owner级 send completion registry；router把 `ClientContext` 仅作为 opaque key once-only claim typed record，不按当前 target cast或读取未知 pointer。
- [ ] registry record不反向强持 owner形成引用环；claim后需要异步 owner时由 callback显式转交。
      terminal时允许存在已 admitted但尚未从 send API返回的 record；最后一个 in-flight send lease
      返回后由 deferred finalizer once-only reconcile，不能在 terminal callback内断言为空或提前清理。
- [ ] 每个成功提交、尚未 `SEND_COMPLETE` 的 record在 connection/runtime retention registry持有
      独立 completion-retention token，保证 callback入口的 owner/router存活；callback建立 owner
      guard并claim后才解除 token。token不得放回 owner内 record形成 self-cycle。
- [ ] completion key/envelope地址在同一 owner生命周期内不得复用，或保留 tombstone/nonce
      消除 pointer-key ABA；duplicate completion不能误 claim地址复用后的新 operation。
- [ ] registry claim成功后所有权必须进入 typed callback guard、Linux event或 shutdown sink之一；
      Linux event queue full时在 callback-safe fallback完成 accounting/释放，不能只返回
      `QUIC_STATUS_OUT_OF_MEMORY` 丢失已经 claim的 record。
- [ ] 不得用 `StreamOpLock` 包住 worker MsQuic API 再让 callback 获取同一锁；为该禁令增加注释/断言。
- [ ] owner/lease 不授权 worker修改已启动 stream 的 callback/context；handler handoff 只切换 router target。
- [ ] client tunnel和 server accepted wrapper从创建时使用 `CleanUpManual`；禁止在 terminal callback 内转换，因为 adapter 已提前缓存 `DeleteOnExit`。
- [ ] 覆盖同步/异步 client tunnel、server incoming dispatcher、dispatcher -> tunnel和 dispatcher -> speed-control target；client hello/structured-error由 dispatcher stable target完成 terminal。
- [ ] `TqTunnelContext` / dispatcher 的 pre-relay shutdown、self-delete 和 relay handoff 都使用同一 owner；任何 target都能在 owner 发布 terminal。
- [ ] 审计 `TqTunnelContext::SendBytes/Abort/Drain`、dispatcher和 server speed-control的非 callback API调用，全部通过 owner lease/phase；callback内调用也通过 owner注册 send和转换状态。
- [ ] handoff 采用 prepare/publish/commit 或等价事务：relay registration和新 target都成功后才释放旧 target，失败不能留下半提交路由。
- [ ] `PrepareRelay` 创建 inactive relay且不 arm epoll fd/提交 QUIC send；`PublishTarget` 成功后 `CommitRelay` 才激活，并重新校验 owner phase和 route generation。
- [ ] prepare返回内部 token且不写 `TqRelayHandle`/`RelayStarted`；commit成功才发布 public handle，rollback/terminal用 token清理。
- [ ] 明确 raw TCP fd disposition：prepare/publish前失败仍由 tunnel拥有；target publish成功是
      fd所有权转给 prepared token/worker的不可逆点，即使 commit/activation失败也由 token
      `EPOLL_CTL_DEL`（若已 arm）并 close。start结果必须告诉 caller fd是否 consumed，caller在
      consumed后立即置 `TqInvalidSocket`，防止 double-close或 fd-number reuse误关新连接。
- [ ] prepared target提供有界 precommit receive/pending-bytes queue；publish与commit之间不写 TCP或调用其它 stream API。
- [ ] 删除 `DispatchPendingRelayRx()` 对 wrapper callback/context 的读取和人工回调；copied pending bytes进入 precommit queue。
- [ ] publish前失败可恢复旧 target；publish后 activation失败不可回切，必须 request shutdown并清理 precommit ownership。
- [ ] 明确注册成功、注册失败、worker stop 三种情况下 owner 归属，增加析构一次/无泄漏测试。
- [ ] 将 `TqRelayHandle*` 从 callback/binding/sink生命周期契约中移除，或以 shared
      control owner + generation替代；terminal callback、late event和 worker stop只能发布
      该 control state。`TqTunnelContext`/reaper销毁后不得再读取 `Backend`、worker/id或
      `Stop`所在内存。
- [ ] public handle的 backend/worker/id/stop必须作为一个同步发布的 committed result可见；
      prepare阶段不可见，commit后 snapshot/stop不能观察到字段半更新。
- [ ] 已启动 stream 的 relay registration failure request shutdown并等待 terminal；只有 StreamStart失败/从未启动才能直接 close。
- [ ] `RequestShutdown(intent)` 使用公共方向性 desired/reserved/submitted ledger合并请求：同方向
      相同强度只提交一次，graceful send允许升级为 abort send，receive abort独立合并；API failure
      释放 reservation但保留 desired intent/retention供策略重试，terminal并发时 terminal wins。
- [ ] 定义 Linux worker stop target：先关闭 registration/post gate，拒绝新 handoff；active
      owner request shutdown；`EPOLL_CTL_DEL`后关闭本地 fd；把 router切到不引用 worker的
      shutdown sink；处理/转交已接受的 terminal、receive和claimed completion owner后再退出
      线程。不得超时强制 close started owner，也不得让 callback经 raw `Worker*`访问已析构对象。

## Task 2: 建立会暴露 UAF 的回归测试

- [ ] 先在新增 `stream_lifetime_test.cpp` 覆盖公共状态机/router，再在
      `linux_relay_worker_io_test.cpp` 覆盖真实 Linux event/epoll组合；生命周期测试必须构造
      正常的 `CleanUpManual` wrapper和 fake `StreamClose`计数，不能继续只用未执行构造函数的
      `alignas(MsQuicStream) uint8_t[]` 伪对象证明析构安全。
- [ ] 在 `linux_relay_worker_io_test.cpp` 增加 terminal router 测试。注册 relay 后，通过固定 router callback 分派 `SHUTDOWN_COMPLETE`，不要直接调用 `ProcessQuicShutdownComplete()` test helper。
- [ ] router callback 同步返回后、worker event 尚未 drain 时断言 owner phase terminal、binding closing，wrapper callback/context 仍为原 router且 target 已清空。
- [ ] callback 返回后释放 tunnel 侧 owner，通过 `weak_ptr`/析构计数确认 binding 或 API lease 仍保活 wrapper；然后 drain worker event。
- [ ] 断言 worker 能完成 logical detach、snapshot 显示 `StreamDetached`、没有 fake stream API 调用。
- [ ] 在同一失效 wrapper 状态下分派 `EPOLLERR | EPOLLHUP | EPOLLRDHUP`，断言只清理 relay/TCP，并保持 late-error metric。
- [ ] 增加 shutdown complete 前 active TCP hard error 对照，断言 fake `StreamShutdown` 仍恰好调用一次。
- [ ] 增加 API lease 与 terminal callback 交错测试：lease 释放前 wrapper 不析构，terminal 后不能取得新 lease。
- [ ] 增加 active TCP hard error取得 lease后暂停、terminal callback发布后再恢复 abort downcall
      的确定性 barrier测试；callback不得等待，wrapper在 lease释放前存活，shutdown最多提交一次。
- [ ] 先运行 focused test，确认现有实现因 terminal worker detach 读取 wrapper 而失败，或由新增结构性断言失败。
- [ ] 明确记录直接调用 router callback 不经过 private C++ wrapper adapter；该测试必须与 manual owner析构测试组合，不能单独作为验收证据。
- [ ] 增加 tunnel -> relay target handoff 与 terminal 并发测试，断言 terminal 只由一个完整 target处理。
- [ ] 分别覆盖 START提交同步失败、`START_COMPLETE`异步 success/failure及 callback早于发起
      API返回；direct start failure不等待 terminal且最终 close一次。`SEND(START)`失败覆盖
      `START_COMPLETE(failure) -> SEND_COMPLETE(Canceled=true)`及两 callback间 caller释放，
      completion retention必须保活 router并只归还一次 buffer/accounting。两类 failure都断言
      target被取走、失败通知一次且后续 publish/lease失败。另覆盖 accepted factory安装边界。
- [ ] 断言 managed factory拒绝 `QUIC_STREAM_START_FLAG_SHUTDOWN_ON_FAIL`；如果实现改为支持，测试
      必须改为 failure后保留 terminal retention并消费随后 `SHUTDOWN_COMPLETE`，不得提前 close。
- [ ] 注入 accepted owner/router/dispatcher allocation failure，断言 emergency handler只 abort/close一次且无泄漏。
- [ ] 注入 shutdown API failure、相同 intent重复请求和 terminal-wins，断言同方向同强度最多提交
      一次且 owner不早删；另测 graceful -> abort的合法单次升级。
- [ ] 覆盖方向 ledger：TCP EOF只在排空后提交一次 FIN；QUIC `RECEIVE(FIN)` /
      `PEER_SEND_SHUTDOWN`先排空 pending receive再对 TCP `SHUT_WR`；graceful send可升级 abort；
      `PEER_SEND_ABORTED` / `PEER_RECEIVE_ABORTED`、`SEND_SHUTDOWN_COMPLETE`、canceled
      `SEND_COMPLETE`和`CANCEL_ON_LOSS`均不发布 owner terminal。
- [ ] 增加 target detach与正在执行 callback并发测试，旧 target deferred retire且 callback不访问已删除 context/worker。
- [ ] 增加 tunnel send completion晚于 target切换测试，确保原 `TqTunnelSendContext` 不会被 cast成 Linux relay send operation。
- [ ] 增加 completion key duplicate/unknown/地址复用测试，以及 send-complete event queue full
      测试；typed record、buffer、outstanding sends/bytes必须各收敛一次。
- [ ] 在 prepare/publish/commit 每个边界注入 terminal/failure，断言 fd未误 arm、relay不残留且 owner只释放一次。
- [ ] 在 publish前失败和publish后 commit失败分别断言 fd disposition：前者 caller仍能使用并
      只 close一次，后者 caller已置 invalid且 token/worker只 close一次；强制复用相同 fd number
      验证不会误关新 socket。
- [ ] 在 publish/commit间注入 RECEIVE和activation failure，断言 receive ownership有界、只释放一次且旧 target不被错误恢复。
- [ ] 并发 snapshot/stop在 prepare阶段看不到 public active relay；commit后 handle字段一次性一致可见。
- [ ] 让 terminal sink/late epoll event与 tunnel reaper销毁并发，释放原
      `TqTunnelContext`/public handle后继续 drain；control owner/generation仍安全发布 stop，
      ASan下不得访问旧 `TqRelayHandle*`。
- [ ] 填满 `TqLinuxRelayEventQueue` 后触发 terminal callback，断言 callback仍先发布 owner
      terminal并返回 success，fallback完成 fd/relay/control owner回收，terminal-retained计数归零。
- [ ] 构造 `epoll_wait` 已返回的旧 relay token，随后 logical detach、close并复用 fd；消费旧
      `EPOLLERR|EPOLLHUP|EPOLLRDHUP`时 generation不匹配且不得命中新 relay或调用任何 stream API。
- [ ] 覆盖 dispatcher -> speed-control handoff，不发生 wrapper handler写入且 terminal仍发布到同一 owner。

## Task 3: 在 router terminal callback 内发布 owner 终态

- [ ] router callback 调用幂等 `PublishTerminalAndTakeTarget()`，在线性化临界区发布 owner terminal并取走唯一 route snapshot。
- [ ] 在临界区外从 snapshot取得 relay id、route/relay generation、shared control owner和
      callback-safe worker endpoint/post permit，release-store `binding->Closing=true` 并清除
      binding route capability；不得把裸 `TqRelayHandle*`、`RelayState*` 或 `Worker*` 复制进事件/sink。
- [ ] router 已为 terminal-empty；不得写 `stream->Callback` / `stream->Context`。
- [ ] shutdown event仅携带 relay id、generation、error code/status和必要的 shared logical-cleanup
      owner；禁止加入 stream/public-handle/worker裸指针。worker用 generation拒绝 stale/duplicate event。
- [ ] enqueue 失败时只发布 handle stop 或执行无 wrapper fallback，不得重新激活 binding，也不得调用 `Shutdown()`。
- [ ] enqueue失败或 worker post gate已关闭时，通过 control owner发布 stop，并由 shutdown sink
      执行/登记 logical cleanup；sink只能操作 fd disposition、relay-local owner和 accounting，
      不能访问已析构 tunnel/worker。不能因 terminal event丢失而泄漏。
- [ ] 确认 `SEND_COMPLETE` 特殊分支仍能交还已经完成的 send operation，且 terminal event 不会吞掉已排队 completion。
- [ ] send前注册、同步失败unregister；`SEND_COMPLETE` registry claim一次，unknown/duplicate key不得解引用、delete或 cast。
- [ ] SEND_COMPLETE claim后投递 Linux queue失败时，callback/sink直接执行 typed completion或
      转交持久 endpoint，仍返回 success；禁止把已 claim key重新塞回 registry或遗失 byte accounting。

## Task 4: 将 worker cleanup 改为 logical detach

- [ ] `ProcessQuicShutdownComplete(relayId, generation, ...)` 在 active和retired索引中按 generation
      定位；即使 relay已 `Closing`/unregister也要幂等完成 terminal logical detach，不能沿用
      当前 `if (relay == nullptr || relay->Closing) return` 而遗漏 terminal回收。
- [ ] 设置 `StreamShutdownComplete=true` 后立即将 relay 的 `Stream` / `StreamBinding` 逻辑清空，
      但不读取 stream字段；duplicate/stale generation只记诊断，不触碰新 relay。
- [ ] 将 sealed binding 放入 retired binding 容器；释放条件只依赖 callback refs、outstanding operation 和 lifetime owner/lease。
- [ ] 删除 terminal path 中 `DetachRelayStreamBinding(relay, stream, binding)` 形式的调用。
- [ ] 删除或重构 `RetiringStream`。若 pending sends 需要延迟 cleanup，只保留 `RetiringStreamBinding` 或独立 completion owner。
- [ ] `CompletePendingStreamDetach()` 不得从 terminal state 恢复或解引用 stream pointer。
- [ ] 普通 abort/unregister 不再从 worker 跨线程清 callback/context；已启动 stream 发起 shutdown 后保留 binding/owner，等待 terminal callback retire。
- [ ] 只有 StreamStart失败或从未启动的 stream 可以直接 close；relay registration failure 不能据此直接析构已启动 owner。
- [ ] terminal cleanup顺序固定为 logical stream detach -> `EPOLL_CTL_DEL`/close owned TCP fd ->
      不调用 stream API地 discard pending receive/write -> 通过 control owner发布 stop；claimed
      send completion可在前后到达，但 byte/accounting只结算一次。任何 trace/snapshot不得在
      stop发布后经裸 handle读取 tunnel内存。
- [ ] 将 pending receive view 中的裸 stream capability 替换为 owner/binding owner；active completion 通过 lease 调用 `ReceiveComplete()`。
- [ ] terminal cleanup 使用独立 discard helper，只释放 view/budget/accounting，不调用 `ReceiveComplete()` 或其它 stream API。

## Task 5: 审计 Linux 全部 stream API 调用点

- [ ] 使用 `rtk rg -n --fixed-strings -- '->Shutdown(' src/tunnel/linux_relay_worker.cpp` 列出 abort 点。
- [ ] 使用 `rtk rg -n "ReceiveComplete|ReceiveSetEnabled|Send\\(" src/tunnel/linux_relay_worker.cpp` 列出其它 API。
- [ ] 使用 `rtk rg -n -- '->(Handle|Callback|Context)' src/tunnel/linux_relay_worker.cpp src/tunnel/linux_relay_worker.h src/tunnel/linux_relay_event_queue.h` 列出把裸 wrapper字段当 capability或改 handler的点；完成后只允许非 terminal诊断中经有效 lease读取 `Handle`，callback/context写入必须为零。
- [ ] callback 内调用只使用 router 提供的当前 callback 参数/owner snapshot；worker 使用 binding owner lease。
- [ ] worker 内调用必须由 active relay + active binding capability 保护，并全程持有 lifetime lease。
- [ ] 所有 abort入口（TCP hard error、fake FIN、receive queue failure、admin/worker stop）统一走
      owner `RequestShutdown`/callback-safe variant合并请求，不再各自直接 `stream->Shutdown()`；
      terminal-wins时 late commit/fail不得覆盖 `TerminalPublished`。
- [ ] graceful half-close不走 terminal logical detach：TCP EOF排空后请求 QUIC FIN；QUIC FIN/
      `PEER_SEND_SHUTDOWN`排空 receive后只关闭 TCP send方向；`SEND_SHUTDOWN_COMPLETE`只更新
      local-send ledger。peer abort按受影响方向回收 ownership并可按策略升级 abort，最终仍等待
      `SHUTDOWN_COMPLETE`发布 wrapper terminal。
- [ ] lease作用域只覆盖一次 MsQuic downcall及其必要输入，不跨 `epoll_wait`、TCP read/write、
      event queue wait或整个 relay lifetime；retry每次重新取得 lease，避免 terminal owner被长时间
      非必要保留并控制 shared ownership热路径开销。
- [ ] `HasAbortableStream()` 增加/保留 binding terminal 检查；`TcpWriteClosed` 和 `StreamShutdownComplete` 继续返回 false。
- [ ] terminal worker path 和 retired cleanup 不得出现 `stream->`。
- [ ] 审计 `Callback` / `Context` 写入点；只允许 stream 启动前的 router初始化，terminal callback 也不得改写。
- [ ] epoll data携带不可误命中新 relay的 relay token/generation；`EPOLL_CTL_DEL`/close后已返回的
      event只做 stale accounting。不要为不存在的 Asio `async_read` 增加 cancel/wait逻辑。

## Task 6: pending completion 与 stop/unregister 回归

- [ ] 构造 shutdown complete 到达时至少一个 send-complete event 已入队但尚未消费的测试。
- [ ] 先处理 terminal event再处理 send completion，以及反向顺序各覆盖一次。
- [ ] 断言 operation/buffer 只释放一次，outstanding sends/bytes 最终归零。
- [ ] 覆盖 worker stop 和 `UnregisterRelay()` 与 terminal event 相邻的情况，binding 不提前释放且不访问 wrapper。
- [ ] `UnregisterRelay()` active路径先撤销数据面/epoll token并 request shutdown，再把 route交给
      shutdown sink等待 terminal；terminal已发布路径只 logical detach。两条路径都不得跨线程
      清 wrapper handler，也不得因 active/retired索引切换丢 terminal event。
- [ ] 覆盖 duplicate/stale terminal event，logical detach/retire 幂等且不重复释放 owner。
- [ ] 覆盖 started owner 的 worker stop/unregister，terminal retention 保证最后一个业务 owner释放后不会提前 `StreamClose`。
- [ ] 覆盖 worker已退出后的 terminal callback，由 shutdown sink完成 owner回收且不投递到失效 worker。
- [ ] 覆盖 stop关闭 post gate时已有 callback拿到旧 endpoint snapshot的竞态：post permit要么在
      worker退出前完成，要么失败并转交 sink，不得在 `TqLinuxRelayWorker`析构后调用 `Enqueue()`。
- [ ] 覆盖 API lease 已取得后 terminal 发布的情况，确认 callback 不等待且 wrapper 在 lease 释放后才析构。
- [ ] 保留 fake FIN、receive completion 和 queue-full 原有测试行为。
- [ ] 增加 active receive complete 一次、terminal receive discard 零次 stream API 的对照测试，并检查 pending bytes/budget 归零。
- [ ] 覆盖 terminal callback/late epoll/completion晚于 `TqTunnelContext`与 public handle释放；
      shared control owner最终 stop=true并回收，旧 handle地址不得再访问。
- [ ] 覆盖 terminal与send completion各自 queue-full fallback，以及 stop时queue中未消费 typed
      owner的 purge/转交；所有 owner、fd、buffer和 accounting最终归零。

## Task 7: 验证

- [ ] 运行：

```bash
rtk cmake --build build --target tcpquic_stream_lifetime_test tcpquic_linux_relay_worker_io_test tcpquic_linux_relay_worker_queue_test tcpquic_tcp_tunnel_test tcpquic_client_tunnel_open_test tcpquic_speed_test_test tcpquic_router_runtime_test -j$(nproc)
rtk build/bin/Release/tcpquic_stream_lifetime_test
rtk build/bin/Release/tcpquic_linux_relay_worker_io_test
rtk build/bin/Release/tcpquic_linux_relay_worker_queue_test
rtk build/bin/Release/tcpquic_tcp_tunnel_test
rtk build/bin/Release/tcpquic_client_tunnel_open_test
rtk build/bin/Release/tcpquic_speed_test_test
rtk build/bin/Release/tcpquic_router_runtime_test
rtk rg -n -- '->(Callback|Context)\s*=' src/tunnel src/runtime
rtk rg -n 'RetiringStream|TqRelayHandle\*|MsQuicStream\*' src/tunnel/linux_relay_worker.* src/tunnel/linux_relay_event_queue.h
rtk git diff --check
```

- [ ] 建立独立 ASan目录并运行 owner和 Linux relay IO test：

```bash
rtk cmake -S . -B build-asan -DCMAKE_BUILD_TYPE=RelWithDebInfo -DQUIC_ENABLE_ASAN=ON -DTCPQUIC_USE_MIMALLOC=OFF -DTCPQUIC_ENABLE_CRASHPAD=OFF
rtk cmake --build build-asan --target tcpquic_stream_lifetime_test tcpquic_linux_relay_worker_io_test tcpquic_tcp_tunnel_test tcpquic_client_tunnel_open_test -j$(nproc)
rtk env ASAN_OPTIONS=detect_leaks=1:halt_on_error=1 build-asan/bin/Release/tcpquic_stream_lifetime_test
rtk env ASAN_OPTIONS=detect_leaks=1:halt_on_error=1 build-asan/bin/Release/tcpquic_linux_relay_worker_io_test
rtk env ASAN_OPTIONS=detect_leaks=1:halt_on_error=1 build-asan/bin/Release/tcpquic_tcp_tunnel_test
rtk env ASAN_OPTIONS=detect_leaks=1:halt_on_error=1 build-asan/bin/Release/tcpquic_client_tunnel_open_test
```

- [ ] 运行现有 Linux relay 性能/压力入口或最小 microbenchmark，记录 owner lease 对 worker CPU 和吞吐的影响。
- [ ] snapshot/metrics暴露 terminal-retained owner数量、最老 age和 stop剩余量，压力测试结束为零。
- [ ] 执行 System Test F01-F18 的 Linux矩阵、k6 short-tunnel baseline和30分钟 churn soak，保存规定证据。
- [ ] 静态确认 `RetiringStream` 已移除，或只在有 owner lease的 non-terminal兼容路径使用；
      terminal event、retired cleanup和 pending receive不保存裸 wrapper，Linux callback/sink不保存
      裸 `TqRelayHandle*`/`Worker*`。
- [ ] 更新 `docs/superpowers/specs/2026-07-09-linux-relay-stream-lifetime-design.md`，注明本计划
      取代其“worker-time wrapper detach”部分，但保留 late TCP error guard 和指标。

## 验收标准

- relay 注册期间 wrapper 使用 manual cleanup，owner handoff 无双删/泄漏。
- outgoing start以 `START_COMPLETE`为异步结果；同步/异步 start failure不等待不存在的 terminal，
  accepted stream则从 factory开始受 terminal retention保护；成功排队的 START send在失败后仍由
  completion retention保活到 canceled `SEND_COMPLETE`。
- owner phase 是 terminal/close 唯一事实源，started owner 由 terminal retention 保活。
- shutdown/half-close使用独立方向 ledger；TCP/QUIC FIN、peer abort、send completion和
  `CANCEL_ON_LOSS`不误发 wrapper terminal，同方向请求不重复且 graceful send可升级 abort。
- wrapper callback/context 只初始化一次；terminal callback 发布 owner终态并清 router target，不等待 worker/API lease。
- callback 返回后 worker、late epoll error、send completion、unregister 不通过裸 pointer 解引用 wrapper。
- worker stream API 调用全程持有 lifetime lease。
- worker/retired cleanup和 terminal callback都不改写已启动 stream 的 callback/context。
- pending receive view 不保存无 owner 的 stream capability，terminal discard 不调用 stream API。
- pending send accounting 正确，binding 生命周期覆盖 callback refs/operation owner。
- handoff 前后的 send completion由 operation owner处理，prepared relay只在 target publish后激活。
- publish前后 TCP fd disposition唯一且可观察；stale epoll token/fd number reuse不命中新 relay，
  无 double-close、误关新 fd或 prepared relay提前 arm。
- terminal sink、late event和 completion只持 shared control owner/generation，不持裸
  `TqRelayHandle*`/`TqLinuxRelayWorker*`；tunnel/reaper销毁并发下无 UAF。
- terminal/send-complete queue full和 worker post gate关闭都有 once-only fallback，owner、buffer、
  fd、retention和 accounting最终归零。
- active TCP hard error 仍调用一次 stream abort；terminal seal 后调用零次。
- 现有 Linux late TCP error 指标和回归测试继续通过。
- feature system-test release gates通过。
