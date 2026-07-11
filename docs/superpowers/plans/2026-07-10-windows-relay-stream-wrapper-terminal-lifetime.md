# Windows Relay Stream Wrapper Terminal Lifetime Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 消除 Windows relay 在 `QUIC_STREAM_EVENT_SHUTDOWN_COMPLETE` callback 返回后由 IOCP close、maintenance、pending receive/send 和 worker stop 路径读取失效 `MsQuicStream` wrapper 的风险，同时保证 `OVERLAPPED`、buffer、send context 和 public handle 只退役一次。

**Architecture:** relay-capable stream 从创建时使用公共 `CleanUpManual` owner 和 stable callback router；wrapper callback/context 启动前设置一次。router target 安全切换到 Windows binding。所有非 callback stream API 调用持有 owner lease；terminal callback 在 owner 发布终态、seal target 并投递不携带 stream pointer 的 terminal cleanup record，不等待 IOCP/API lease。TCP socket close/cancel 只启动 IO 取消，worker 必须继续 drain 每个已提交的 `OVERLAPPED` completion 后才能释放 operation/buffer 或关闭 IOCP。

**Design:** `docs/superpowers/specs/2026-07-10-relay-stream-wrapper-terminal-lifetime-design.md`

**System Test:** `docs/superpowers/specs/2026-07-10-relay-stream-wrapper-terminal-lifetime-system-test-design.md`

**Review Remediation:** `docs/superpowers/plans/2026-07-11-windows-relay-stream-wrapper-review-remediation.md`

**Tech Stack:** C++17, Windows IOCP/Winsock overlapped IO, MsQuic C++ wrapper, CMake Windows tests.

**Prerequisite:** 平台中立 lifetime owner、stable router、owner-level send completion registry 和 tunnel/dispatcher/speed-control target handoff 已按 Linux 计划 Task 1（或等价公共变更）实现。本计划只实现 Windows adapter、IOCP cleanup 和 Windows-specific tests，不复制公共 owner/state machine。

---

## 仓库现状与改造边界

以下均为当前实现中的真实入口，实施时不能用抽象的新类型绕过这些路径：

- `TqWindowsRelayWorker::RegisterRelay()` / `RegisterRelayLocal()` 当前接收裸 `MsQuicStream*`，在 `RegisterRelayLocal()` 内直接写 `command.Stream->Callback/Context`，并在 `MaybePostTcpRecv()` 成功前写 `Relays_` 和 `TqRelayHandle`。失败会形成半提交 registration。
- `TqWindowsRelayWorker::CloseRelay()` 和 `TryRetireRelay()` 当前检查 `relay->Stream->Context` 并写 `NoOpCallback/nullptr`；terminal IOCP operation 消费时 wrapper 可能已经被 auto-delete。
- `TqWindowsPendingQuicReceive::Stream` 是逃逸到 IOCP operation 的裸 capability；`FlushDeferredReceiveCompletion()`、`CompleteRemainingReceiveOwnership()`、`FlushBatchedDeferredReceiveCompletion()` 和 `CompleteAllPendingQuicReceives()` 都可能在 terminal 后调用 `ReceiveComplete()`。
- `TrySubmitQuicSendOperation()`、`SetQuicReceiveEnabled()` 和 receive completion helpers 是 worker-thread stream API 调用点；仅检查 `Closing`/非空 pointer 不能关闭 terminal TOCTOU，必须改为 owner lease。
- `StreamCallback()` 当前把 `SEND_COMPLETE.ClientContext` 直接 cast 为 `TqWindowsQuicSendOperation*`。handoff 前由 tunnel 发起的 send 可能在 Windows target 发布后才完成，必须改为公共 registry 的 opaque-key claim。
- `Stop()` 当前先 `PostStop()`/join worker，再调用 `CloseRelay()`/`closesocket()` 并清 `Relays_`/`RetiredCallbacks_`。`TqCloseSocket()`/`TqResetSocket()` 在 Windows 仅调用 `closesocket()`；这会请求取消 pending Winsock IO，但不代表其 `OVERLAPPED` completion 已出队。IOCP/operation/buffer 不能在 drain 前销毁。
- `CallbackBinding::Worker` 和 `RelayHint` 当前是无所有权 pointer；worker 销毁后 late callback 不能再通过它们访问 worker。router target 必须使用 callback-safe owner/ref guard，并在 stop 前切到 shutdown sink。
- `RelayContext::PublicHandle` 当前保存调用方 `TqRelayHandle*`，`TryRetireRelay()` 会异步写 `PublicHandle->Stop`；`TqRelayHandle` 又保存裸 `WindowsWorker*`，`TqRelayStop()`/`TqRelaySetTraceContext()` 可能与 runtime销毁并发。terminal sink、late IOCP或worker stop都不能依赖 tunnel context中的 handle storage继续存在。
- `GetQueuedCompletionStatus()` 返回 `FALSE` 且 `overlapped != nullptr` 表示一个失败 IO 已完成，仍必须取得并退役对应 `IoOperation`；`overlapped == nullptr` 才表示没有 operation。不能把 close/cancel error 当作“没有 completion”。

## File Structure

- Add or Modify as needed: `src/tunnel/stream_lifetime.h` / `.cpp`, `src/unittest/stream_lifetime_test.cpp`, `src/CMakeLists.txt`
  - 只补 Windows adapter 所需的公共 owner/router/registry 接口和 build source；不创建 Windows 私有 lifetime 实现。
- Modify: `src/tunnel/relay.h` / `relay.cpp`, `src/tunnel/tcp_tunnel.cpp`
  - 将 `TqRelayStart()` -> `TqWindowsRelayRuntime::RegisterRelay()` 的裸 stream 参数升级为同一 owner/router handoff；Windows public handle 仅在 commit 后发布，并通过安全 control owner连接 worker/sink。
- Modify: `src/tunnel/windows_relay_worker.h` / `.cpp`
  - 改造 `RegisterRelay*`、`RelayContext`、`CallbackBinding`、`IoOperation`、`TqWindowsPendingQuicReceive`、`StreamCallback()`、`ProcessQuicShutdownComplete()`、`CloseRelay()`、`TryRetireRelay()` 和 `Stop()`。
  - 增加 terminal cleanup record、stream disposition、TCP cancellation/drain state、callback-safe target 和 test hooks/counters。
- Modify: `src/unittest/windows_relay_worker_test.cpp`
  - 增加 wrapper terminal、real pending WSA receive、late IOCP completion、send registry、active/terminal receive 和 worker-stop tests；更新原来期望 handler 被清成 `NoOp/nullptr` 的断言。
- Modify as needed: `src/unittest/tcp_tunnel_test.cpp`, `src/unittest/client_tunnel_open_test.cpp`, `src/unittest/speed_test_test.cpp`
  - 复用公共 owner tests，并在 Windows build 覆盖同步/异步 client、server dispatcher 和 speed-control handoff。
- Modify as needed: `src/runtime/relay_metrics.h` / `.cpp`, `src/docs/thread_model_cn.md`, `docs/relay_win.md`
  - 贯通 terminal retention/router/registry、pending overlapped IO 和 stop drain 指标，更新 Windows IOCP 生命周期说明。

## Task 1: 接入公共 owner 并事务化 Windows registration

- [x] 定义 Windows registration 参数结构（名称按现有风格确定），携带 `TqStreamLifetime`/router owner、TCP fd、compressor/decompressor、tuning 和内部 completion；删除 worker 对裸 stream 的 adopt 语义。
- [x] `TqWindowsRelayRuntime::RegisterRelay()`、`TqWindowsRelayWorker::RegisterRelay()`、`RegisterRelayCommand` 和 `RegisterRelayLocal()` 传递同一 owner，不从 `owner->Stream()` 重新构造或接管 wrapper。
- [x] `RelayContext` 和 Windows route target/binding 持有 owner；删除并行的 `RelayContext::Stream` 裸 capability。所有 API helper返回持有 shared owner 的 typed lease，禁止返回可脱离 lease 使用的裸 pointer。
- [x] 删除 `RegisterRelayLocal()` 和 `RegisterRelayForTest()` 对 `stream->Callback/Context` 的写入。wrapper 启动前已经固定为公共 router；Windows registration 只 prepare/publish Windows target。
- [x] Windows target snapshot 不保存无保护的 `RelayContext*`/`TqWindowsRelayWorker*`。callback 通过 shared adapter 或 `TryEnter/Leave` guard 保证 method 返回前 target/worker 有效，detach 使用 deferred retire。
- [x] 将现有 registration 拆为 `PrepareRelay`、`PublishTarget`、`CommitRelay`（名称可调整）：prepare 可执行 `CreateIoCompletionPort()` association 并创建 `RelayContext`/binding，但不得 post `WSARecv`、schedule maintenance、写 `Relays_` 的 public-active view 或写 `TqRelayHandle`。
- [ ] prepare从入口开始用 RAII token明确接管 TCP socket，并在结果中显式返回 consumed disposition；
      调用方观察 consumed后立即置 invalid。因为已关联 IOCP 的 socket不能解绑，publish前 rollback
      也必须关闭该 socket并按同一 IOCP drain规则回收，不能退还 tunnel或让 caller重复 close。
- [ ] lease admission、shutdown reservation、`PublishTarget(expected_generation, prepared_target)` 与
      `PublishTerminalAndTakeTarget()` 使用公共 control线性化协议；terminal 后 publish/新 lease失败，
      publish前失败可 rollback，publish后 activation失败不可恢复旧 target，临界区内不调用 MsQuic。
- [x] commit 重新校验 owner phase、router generation 和 prepared token；先以 internal activating state插入 map并 post首个 `WSARecv`，全部 activation成功后才标记 active、schedule maintenance并一次性发布 `TqRelayHandle`/`RelayStarted`。
- [x] 修复当前 `RegisterRelayLocal()` 在 `MaybePostTcpRecv()` 失败后仍残留 map/handle 的行为；所有 prepare/commit failure 都有幂等 rollback。`TqRelayRegisterActive()` 若继续作为 prepare前容量 reservation，必须在每个失败分支恰好归还一次，public active metrics只统计 commit success。
- [x] 用每次 registration唯一的 shared control block（名称按公共实现确定）替换 `RelayContext::PublicHandle` 裸 pointer。worker、terminal cleanup record和shutdown sink只持有 control owner + generation，只向 control state发布 stop/result；不得保存、读取或写调用方 `TqRelayHandle*`。
- [ ] commit result把 control owner和 worker registration token返回给 `TqRelayStart()`，由调用线程一次性填充 `TqRelayHandle`。`TqRelayStop()`/`TqRelaySetTraceContext()` 先 snapshot control/token，并经 runtime-owned endpoint或受 `RuntimeWorkerLifetimeLock()`保护的 lookup投递，不能直接调用可能已析构的 `WindowsWorker*`。
- [x] handle被 reset/reuse或 `TqTunnelContext` 析构时只释放其 control owner；late terminal/IOCP写旧 control block不会命中新 registration。所有 stop/trace operation同时验证 control generation和relay generation。
- [ ] prepared target 用有界 precommit queue 接收 publish/commit 间的 copied RECEIVE；此阶段不调用 stream API、不 post TCP send/receive。terminal 或 activation failure 只释放本地 ownership/accounting并 request shutdown。
- [ ] 已启动 owner 的 registration/commit failure 通过公共方向性 shutdown
      desired/reserved/submitted ledger收敛并等待 terminal；同方向同强度只提交一次，graceful send
      可升级 abort，API failure保留 desired intent/retention。只有 `StartFailed` 或从未启动才可直接 close wrapper。
- [ ] 覆盖同步/异步 client open、server accepted dispatcher、dispatcher -> tunnel 和 dispatcher -> speed-control；`DispatchPendingRelayRx()` 不再读取/人工调用 wrapper callback，而是把 copied bytes交给 prepared target。

## Task 2: 先建立确定性 terminal 和 handoff 回归测试

- [x] 复用现有 `TestPostWorkerQueueBlockForTest()`、`TestWaitWorkerQueueBlockEnteredForTest()`、`TestReleaseWorkerQueueBlockForTest()`/等价 barrier 阻塞 worker，注册 manual owner + fixed router 的 Windows relay，再通过 router callback 分派真实 `SHUTDOWN_COMPLETE`；测试不得依赖 sleep 命中竞态。
- [x] callback 同步返回且 terminal operation 尚未处理时，断言 owner phase=`TerminalPublished`、Windows binding closing/terminal、`RelayHint=null`、router target empty，wrapper callback/context 仍是创建时 router。
- [x] terminal operation 必须持有 relay/binding 或独立 terminal cleanup record 的强 owner，并携带 relay id/generation/error/status；断言其中没有 `MsQuicStream*`。不能只投递 id 后依赖可能已从 `Relays_` 退役的 lookup。
- [x] 释放 tunnel 侧 owner，通过 `weak_ptr`/wrapper close计数证明 callback/operation/API lease 覆盖最终析构；drain terminal operation 后 wrapper 只析构一次且 active relay 最终为零。
- [ ] 直接调用固定 router callback不会经过 private `MsQuicStream::MsQuicCallback` adapter；该测试只能证明 state transition，必须与 manual owner析构计数及 ASan/Application Verifier case组合，不能单独作为“callback返回后未自动delete”的证据。
- [x] 增加 `PublishTarget` 与 terminal 的 barrier interleaving，以及 terminal 位于 prepare/publish/commit 每个边界的 tests；结果只能由旧 target或新 target完整处理一次，不能出现半 public handle/已 arm fd。
- [ ] 复用 `START_COMPLETE(success/failure)` 早于 start API返回、同步 no-callback reject、accepted
      factory handler安装边界、allocation failure emergency callback、shutdown API failure、同 intent
      重复、graceful -> abort升级和 terminal-wins公共 tests。direct start failure不等待 terminal；
      `SEND(START)` failure必须由 completion retention跨过 `START_COMPLETE(failure)`，直到随后
      `SEND_COMPLETE(Canceled=true)`才归还 record/buffer并允许 wrapper close。两类 failure都必须
      once-only取走业务 target、通知 open失败，并拒绝后续 target publish/API lease。
- [x] 增加 worker target detach与正在执行 callback并发，以及 worker对象销毁后 router收到 terminal/non-ownership event的 tests；late callback只能命中 shutdown sink，不能读旧 `Worker` pointer。
- [x] 增加 `TqTunnelContext`/栈上 `TqRelayHandle` 先销毁、terminal operation和late TCP completion后drain的 barrier test；worker只更新仍存活的 shared control block，Application Verifier下不得触碰旧handle地址。
- [x] 复用同一 handle storage立即注册新 relay，随后投递旧 generation terminal/stop/trace operation；断言旧 control completion不能置停或改写新 relay，两个 control block分别只完成一次。
- [x] 更新现有 callback/retire tests：已启动 stream 的 handler 不再变为 `MsQuicStream::NoOpCallback/nullptr`，始终保持公共 router。

## Task 3: 实现 callback-time seal 和 terminal IOCP ownership

- [x] router 的 terminal 分支调用幂等 `PublishTerminalAndTakeTarget()`；只有首次 publish 取得 Windows target snapshot，duplicate terminal 不投递第二个 operation。
- [x] router必须在普通 binding `Closing` filter之前识别并发布 `SHUTDOWN_COMPLETE`；active close已将 Windows binding置 closing时也不能吞掉最终 terminal。`SEND_COMPLETE`同样先由公共 registry处理。
- [x] 在 router临界区外从 snapshot 构造强所有权 terminal cleanup record，再 release-store binding terminal/closing、清 `RelayHint`；router target 已由公共线性化操作切为空。
- [x] 新增 dedicated terminal post helper。它接收已 sealed target/cleanup record，不套用普通 `binding->Closing` 拒绝逻辑；成功 post 后 operation 自己覆盖到 dequeue 为止的 relay/binding 生命周期。
- [x] terminal post helper 对 `QueuedWorkerOps`/terminal-pending counter 增减成对；`TryRetireRelay()` 必须把未消费 terminal record计入退役条件，不能让 id-only event在 map erase 后变 stale。
- [x] `QuicShutdownComplete` operation 只保存 cleanup record、relay id、generation、connection error/status；禁止保存 stream pointer或从 operation 中恢复 wrapper capability。
- [x] callback seal 后 RECEIVE/IDEAL/PEER_ABORT 不再创建新普通 operation；已 snapshot 的并发 callback按 target owner完成。`SEND_COMPLETE` 始终走 owner registry，不依赖当前 Windows target。
- [x] `PostQueuedCompletionStatus()` 失败时记录 `WindowsCallbackIocpPostFailedCount` 和 terminal-specific metric，并把 cleanup record交给 callback-safe shutdown sink/reaper；禁止在 MsQuic callback thread 直接调用会修改 worker-owned queue/list 的 `CloseRelay()`。
- [x] post failure fallback 必须向 captured shared control state发布 stop并保留 owner/binding最终回收路径；禁止解引用原 `TqRelayHandle*`。不能只设置 `Closing` 后遗留 `Relays_` entry，也不能调用 stream shutdown或改 callback/context。

## Task 4: 拆分 active close、terminal logical detach 与 TCP IO cancel/drain

- [x] 在 `TqRelayCloseMode`（TCP graceful/reset语义）之外增加 Windows stream disposition，例如 `ActiveShutdown`、`ActiveAbort`、`TerminalLogicalDetach`；reason 字符串不得参与控制流。
- [x] 所有 `ProcessQuicShutdownComplete()` overload 先验证 operation generation/terminal token，再使用 `TerminalLogicalDetach`；立即设置 stream-terminal state和逻辑清空 relay stream capability，不等待 pending TCP/send/receive accounting。
- [x] terminal logical detach、`CloseRelay()` 和 `TryRetireRelay()` 不得读取 `relay->Stream->Handle/Context/Callback`，也不得写 wrapper callback/context。`RetiredCallbacks_` 若保留，只保存 binding/target owner和callback refs，不保存 wrapper capability。
- [ ] active TCP hard error、admin `StopRelay()`、registration activation failure 和 worker stop 通过
      owner方向 ledger请求相应方向或双向 shutdown；调用持有 lease，相同 intent不重复提交，
      API failure不释放 terminal retention，terminal并发时 terminal wins。
- [x] QUIC `RECEIVE(FIN)` / `PEER_SEND_SHUTDOWN` 是 graceful peer EOF：先排空已接受 receive，
      再对 TCP执行 `shutdown(SD_SEND)`，反向 TCP->QUIC可继续；不得走 `ActiveShutdown` 或 terminal detach。
- [x] `PEER_SEND_ABORTED` / `PEER_RECEIVE_ABORTED` 按受影响方向停止数据面、回收对应 ownership并可
      按 relay policy请求本地 abort；`SEND_SHUTDOWN_COMPLETE`只更新 local-send ledger，canceled
      `SEND_COMPLETE`只归还该 operation，`CANCEL_ON_LOSS`同步填写稳定 error code并记录策略。
      这些事件均不能发布 terminal、解除 retention或采用 `TerminalLogicalDetach`，最终只有
      `SHUTDOWN_COMPLETE`发布 wrapper terminal。
- [x] 将 socket close集中到 worker-thread `BeginTcpCancellation()`（名称可调整）：对有效 socket先调用 `CancelIoEx(reinterpret_cast<HANDLE>(socket), nullptr)`，将 `TRUE` 和 `ERROR_NOT_FOUND` 视为正常结果；其它 Win32 error只记录诊断且仍继续按 close mode调用 `TqCloseSocket()`/`TqResetSocket()`并使 fd无效。
- [x] 明确 `CancelIoEx()`/`closesocket()` 只是 cancellation request。每个已成功提交的 `WSARecv`/`WSASend` 即使同步返回成功，也必须等对应 GQCS dequeue 后才减 `InFlightTcp*`、释放 `IoOperation`、`ReceiveView` 和 buffer；项目没有启用 skip-completion-on-success。
- [x] `WSARecv`/`WSASend` 同步失败且错误不是 `WSA_IO_PENDING` 时不会产生 completion，必须在 post site同步 rollback counter并释放 operation一次；`PostQueuedCompletionStatus()` 失败同理。
- [x] GQCS `FALSE + non-null OVERLAPPED` 仍进入该 operation 的 completion path并恰好一次 decrement；`ERROR_OPERATION_ABORTED`、`WSA_OPERATION_ABORTED`、`ERROR_NETNAME_DELETED`、`ERROR_CONNECTION_ABORTED`、`ERROR_GRACEFUL_DISCONNECT` 及已有 teardown WSA错误在 closing/terminal 后归一为 teardown downgrade，不增加 fatal reset。
- [x] cancel与真实IO完成竞争时，GQCS可能返回成功 completion而不是 `ERROR_OPERATION_ABORTED`；两者都按 operation当前 closing/terminal disposition cleanup，不能要求固定错误码，也不能重复settle counter/buffer。
- [x] active 且未发起 close时的非 teardown error保持 fatal；保留原始 `DWORD`、operation type和错误来源用于 metrics/trace，禁止把 Win32 `GetLastError()` 与 `WSAGetLastError()` 混为“无 completion”。
- [x] duplicate/stale terminal operation按 owner terminal token和 generation幂等丢弃，不能关闭后续 relay generation或重复关闭 socket/public handle。
- [x] 增加 peer send/receive abort先于或晚于 terminal operation的双向顺序测试；只有 terminal event执行 logical detach，peer event不能恢复 sealed target或重复 request shutdown。

## Task 5: 修复 pending receive 与 send completion ownership

- [x] 删除 `TqWindowsPendingQuicReceive::Stream`；view 持有 owner/binding completion capability、relay id/generation、copied buffer和 once-only ownership state。
- [x] 将 `FlushDeferredReceiveCompletion()`/`CompleteRemainingReceiveOwnership()`/`FlushBatchedDeferredReceiveCompletion()`/`CompleteAllPendingQuicReceives()` 拆为 active complete 和 terminal discard 两套 helper，禁止保留一个无条件调用 `ReceiveComplete()` 的 cleanup函数。
- [x] active complete 取得 `ReceiveComplete` lease后调用一次 API；owner terminal或 receive方向已
      abort时只释放 view、buffer、pending bytes/depth/budget，不调用
      `ReceiveComplete()`/`ReceiveSetEnabled()`。graceful FIN不能因一次瞬时 lease失败就当作 discard；
      必须由 active owner重试/排空或升级明确 abort。同一 view同时被 `PendingReceives` 和 TCP
      `IoOperation` 引用时也只能 settle一次。
- [x] `SetQuicReceiveEnabled()`、`TrySubmitQuicSendOperation()`、maintenance retry和receive drain的所有 stream API都通过 typed lease；terminal/closing 后不能仅凭缓存 pointer或先前状态检查开始新调用。
- [x] 接入公共 send completion registry：send前注册 typed record，MsQuic同步 send失败时 unregister；router只把 `ClientContext` 当 opaque key once-only claim，禁止读取 `TqWindowsQuicSendOperation::Magic`或先 cast pointer。
- [x] 每个成功提交、尚未 `SEND_COMPLETE` 的 record复用 connection/runtime级 completion-retention
      token在 owner外保活 router；callback先建立 owner guard并claim，再解除 token。尤其
      `SEND(START)` failure的 token必须跨过 `START_COMPLETE(failure)`直到 canceled send completion。
- [x] unknown/duplicate key只增加诊断并fail-safe，传入不可解引用的 sentinel key也不得崩溃、delete或影响其它 relay。
- [x] completion token在同一 owner内单调不复用，或保留 tombstone/envelope直到 owner关闭；
      allocator复用旧 operation地址时，late duplicate callback不得 claim新 record。generation字段
      不能替代 key本身的 ABA防护，增加强制地址/token复用测试。
- [x] terminal/close 不再用 `FinalizeQuicSendAccountingOnClose()` 强制把 `InFlightQuicSends`/bytes清零后等待晚到 callback；每个 submitted record在 callback claim或owner registry cleanup时归还 accounting一次，避免 late `fetch_sub` 下溢。
- [x] 覆盖 terminal operation与 queued send completion正反两种顺序，以及 tunnel send晚于 Windows target handoff；operation/buffer/accounting owner只释放一次，最终 registry/in-flight/bytes均为零。
- [x] 使用 `rtk rg -n "Stream->|stream->|ReceiveComplete|ReceiveSetEnabled|->Send\\(" src/tunnel/windows_relay_worker.cpp` 分类全部 wrapper/API访问；非 callback访问必须能追溯到有效 lease，terminal/retire路径不得出现 wrapper字段读取。

## Task 6: 重写 worker/runtime stop 顺序

- [x] `Stop()` 先在 `ControlCommandLock_` 下拒绝新 registration/control post，再在 worker线程执行 begin-stop；不得先投递无条件退出的 `StopWorker` 然后在线程外关闭 relay socket。
- [x] worker begin-stop 对每个 active owner request shutdown，将 router target切到不引用 worker的 shutdown sink，并在 worker线程关闭/cancel本地 TCP；切换后新 callback不再向即将关闭的 IOCP post。
- [x] worker继续调用 `GetQueuedCompletionStatus()`，drain begin-stop前已排队的 callback operations，以及 `closesocket`/`CancelIoEx` 触发的所有 TCP completion；退役条件包含 `InFlightTcpRecvs/Sends`、`QueuedWorkerOps`、terminal pending和operation owners均为零。
- [ ] begin-stop还必须消费或显式失败并唤醒已入队的 `RegisterRelayCommand`/`SnapshotCommand` 等 control command，保证等待栈上 command的调用线程先返回；退出后队列中不得残留 `IoOperation::Control` raw pointer。
- [x] 只有 stop barrier确认上述本地 IOCP ownership为零后才退出 thread、清 `Relays_`/maintenance queue并 `CloseHandle(Iocp_)`。不能通过清 map或关闭 IOCP来代替 completion drain。
- [ ] started wrapper的 terminal retention可在 worker退出后继续由 router/shutdown sink持有；stop timeout只能告警/dump stuck owner或IO，不得强制 delete wrapper。Windows worker本地 `OVERLAPPED` 则必须继续 drain到零。
- [x] stop结果在 relay local retirement或sink接管时向 shared control block发布一次；调用方通过 control state/helper观察。`TqRelayStop()`、`TqRelaySetTraceContext()` 与 runtime `Stop()` 并发时，worker endpoint lookup要么安全成功，要么返回 already-stopped，不能调用已销毁 worker pointer。
- [x] 增加 real socket-pair pending `WSARecv` + worker stop test：确认收到取消 completion、operation/buffer析构一次、in-flight/active relay归零后才返回。pending `WSASend` 使用可控 socket backpressure或专用 completion injection hook，不依赖概率性 sleep。
- [ ] 增加无 pending IO时 `CancelIoEx(ERROR_NOT_FOUND)`、已经入队 completion后 close、terminal前后各一个 TCP recv/send completion、以及 stop与terminal并发的 barrier tests。

## Task 7: 可观测性与验证

- [x] 在 `TqWindowsRelayWorkerSnapshot`、runtime聚合和 relay metrics中暴露：terminal seal/post/post-failure/logical-detach、terminal后抑制API、pending overlapped recv/send、cancel requested/completed、stop drain剩余量；公共 metrics暴露 retained owner count/oldest age、registry和shutdown sink数量。
- [ ] 每个 focused test结束断言 active relay、pending overlapped、pending receive bytes/depth、send registry、terminal retention和shutdown sink均归零；不得只以进程未崩溃为通过。
- [ ] 在 Windows 运行：

```powershell
rtk cmake --build build --config Release --target tcpquic_stream_lifetime_test tcpquic_windows_relay_worker_test tcpquic_windows_reactor_test tcpquic_tcp_tunnel_test tcpquic_client_tunnel_open_test tcpquic_speed_test_test tcpquic_router_runtime_test -j
rtk build/bin/Release/tcpquic_stream_lifetime_test.exe
rtk build/bin/Release/tcpquic_windows_relay_worker_test.exe
rtk build/bin/Release/tcpquic_windows_reactor_test.exe
rtk build/bin/Release/tcpquic_tcp_tunnel_test.exe
rtk build/bin/Release/tcpquic_client_tunnel_open_test.exe
rtk build/bin/Release/tcpquic_speed_test_test.exe
rtk build/bin/Release/tcpquic_router_runtime_test.exe
rtk git diff --check
```

- [ ] 使用 ASan for Windows；若当前工具链不可用，使用 Application Verifier page heap运行 owner/terminal/send-context/stop-drain cases。保护页只用于独占 page 的专用测试。
- [ ] 保留 active TCP hard-error reset、zero-byte recv/send、IOCP teardown downgrade、callback post metrics、receive backpressure和maintenance queue原有断言。
- [ ] 执行 System Test F01-F18 的 Windows矩阵、k6 short-tunnel baseline、raw TCP吞吐、30分钟 churn soak和worker-stop恢复场景；保存 config、git/binary SHA、metrics、trace、Verifier输出和fault timeline。
- [ ] 记录 lifetime lease/registry对 IOCP worker CPU、callback duration、p95/p99和吞吐的影响；满足设计文档的延迟<=5%、吞吐<=3%回归门禁。

## 验收标准

- Windows registration传递同一 manual owner/router，prepare失败、publish竞争和commit失败均无半提交 handle、已激活数据面、双删或泄漏。
- owner phase是 terminal/close唯一事实源；`SHUTDOWN_COMPLETE` callback seal target且不等待 worker/API lease，wrapper callback/context始终稳定。
- wrapper phase与方向 ledger分离；TCP/QUIC graceful EOF、peer abort、send completion和
  `CANCEL_ON_LOSS`不误发 terminal，同方向请求不重复且 graceful send可升级 abort。
- terminal IOCP operation持有完成cleanup所需的强 owner，但不携带或恢复 stream pointer；duplicate/stale event幂等。
- relay/terminal/sink不保存调用方 `TqRelayHandle*` 或裸 worker capability；handle/context提前销毁和storage复用时，control owner + generation隔离旧异步事件。
- `ProcessQuicShutdownComplete()`、terminal `CloseRelay()`、`TryRetireRelay()`、maintenance和late TCP completion均不读取 wrapper字段或开始新 stream API。
- active stream API调用全程持有 typed lease；active close仍提交一次 shutdown，terminal seal后同类路径调用零次。
- pending receive active path调用一次 `ReceiveComplete()`；terminal/abort discard调用零次且本地budget/accounting只归零一次。
- send completion通过公共 registry claim；handoff、unknown/duplicate key和terminal乱序下无 type confusion、double free或counter underflow。
- `CancelIoEx`/`closesocket` 后所有已提交 `OVERLAPPED` 都在 IOCP drain后才释放；GQCS失败 completion被归一为 teardown或active fatal，operation counter恰好归还一次。
- worker/runtime stop先切 shutdown sink并cancel/drain IO，再退出worker和关闭IOCP；worker销毁后的late callback不访问 worker，started owner不被强制提前析构。
- feature system-test release gates通过，压力测试结束 owner/registry/pending IO/receive budget全部归零。

## 2026-07-11 Windows 实现评审记录

### 标注口径与结论

- 本轮按当前 HEAD 做静态代码、测试入口和文档证据核查；`[x]` 只表示对应实现或 focused test 已落地，不表示整个 Task 或最终验收已经通过。
- 当前环境没有 Windows 编译器和运行环境，未执行 Windows 二进制、ASan/Application Verifier、F01-F18、k6、吞吐或 soak 门禁。
- 结论：Task 1-5 的主要结构已经落地，Task 6 仍存在 terminal retention 和并发 stop 阻断问题，Task 7 仅完成 metrics/单元测试侧工作；当前版本不能视为达到本计划验收标准。

### 发现的问题

#### P0：当前 HEAD 的 Windows 分支无法编译

- 公共 `TqRelayStopControl::Generation` 在 `src/tunnel/relay.h` 中是 `const uint64_t`，Windows 代码仍把它当作 atomic：`AllocateRelayStopControl()` 调用 `.store()`，`ControlGenerationMatches()`、`TqRelayStop()`、`TqRelaySetTraceContext()` 及 Windows 单测大量调用 `.load()`。
- 静态搜索在 `src/tunnel/windows_relay_worker.cpp`、`src/tunnel/relay.cpp` 和 `src/unittest/windows_relay_worker_test.cpp` 中得到 24 处 `.load()`/`.store()` 不兼容调用。这不是运行期竞态，而是确定的编译错误。
- 修复要求：使用构造时一次性生成的 immutable generation，例如 `std::make_shared<TqRelayStopControl>(TqRelayNextControlGeneration())`，所有读取直接使用 `Generation`；删除 Windows 私有的二次 `.store()` 初始化，并先恢复完整 Windows build matrix。

#### P0：主动 close 会在真实 terminal callback 前提前发布 terminal

- `TqWindowsRelayWorker::TryRetireRelay()` 在 owner phase 仍为 `Starting/Started` 时直接调用 `PublishTerminalAndTakeTarget()`。主动 `CloseRelay()` 在本地 TCP/operation accounting 已归零后会立即进入该路径，此时并没有收到 `QUIC_STREAM_EVENT_SHUTDOWN_COMPLETE`。
- `FinalizeWorkerStopOnWorkerThread()` 也会对仍为 `Starting/Started` 的 owner 强制调用 `PublishTerminalAndTakeTarget()`。
- `PublishTerminalAndTakeTarget()` 会移走 router target并调用 `ReleaseTerminalRetention()`；因此 wrapper 可能在 MsQuic 真正发出 `SHUTDOWN_COMPLETE` 前析构/close，重新引入本计划要消除的 callback 返回前后 UAF/失效 wrapper 风险。真实 terminal 随后也无法再取得 Windows target并创建 terminal cleanup record。
- 修复要求：只有公共 router 处理真实 `SHUTDOWN_COMPLETE`（以及独立的合法 start-failure 状态机）可以发布 terminal。主动 close/worker stop只能请求 shutdown、切换到 shutdown sink并保留 owner；本地 relay retirement不能推进 wrapper terminal phase。增加“主动 close 本地 accounting 先归零，但 terminal 被 barrier 阻塞”的回归测试，断言 retention 和 wrapper 一直存活到真实 terminal callback 返回。

#### P0：registration activation rollback 的 shutdown 顺序无效

- `WithdrawPublishedRelayTarget()` 通过 `PublishTerminalAndTakeTarget()` 撤回 target；随后 `RollbackPreparedRelay()` 或 `CommitRelay()` failure 才调用 `RequestRelayShutdown()`。
- owner 一旦进入 `TerminalPublished`，`TryAcquireSendApi()` 和 `RequestShutdown()` 都会拒绝调用，因此 started stream 的 activation failure 实际不会提交计划要求的 `AbortBoth`，同时 terminal retention已经被提前释放。
- 该问题覆盖 publish 后 terminal/commit failure、首个 `WSARecv` activation failure等路径；现有 fake/manual owner测试没有证明真实 MsQuic wrapper会收到 shutdown并等到 terminal。
- 修复要求：为“publish 后 activation failure”建立独立的失败 target/sink handoff；先在线性化协议中保留 active owner并通过 ledger提交 shutdown，最终仍只由真实 terminal发布 `TerminalPublished`。测试必须断言 shutdown API调用一次、owner在 terminal前仍 retained、无 public handle且 socket cancellation completion被 drain。

#### P1：stop-drain 并发与 legacy 集成用例仍有 crash、deadlock 和 hang

- `src/unittest/windows_relay_worker_test.cpp` 仍用 `#if 0` 禁用了 pending-register wake、`StopRelay` 后并发 `Snapshot()`、half-close、backpressure/fatal-send teardown、TCP recv pool和 zstd flush等用例。
- `docs/relay_win.md` 明确记录原因包括“pending-register wake crashes”“可死锁”和多条 `Stop()` hang。同步拒绝新 registration 的 gate不能替代“命令已经入队并持有栈上 raw `Control` 指针”与 stop交错的场景；单独的 injected completion gate也不能替代原真实 socket/data-path teardown。
- 修复要求：先修复并恢复这些用例，不得以粒度更小的 gate宣称等价覆盖；至少证明已入队 `RegisterRelayCommand`/`SnapshotCommand` 全部完成或显式失败、调用线程先返回、stop 后队列中没有 raw command pointer，且真实 half-close/zstd/backpressure 路径都能 drain退出。

#### P1：public handle/result 仍暴露裸 `TqWindowsRelayWorker*`

- `TqWindowsRelayRegistrationResult::Worker` 和 `TqRelayHandle::WindowsWorker` 仍保存 worker裸指针，`TqRelayStartImpl()` 与测试 helper仍会写入。当前 stop/trace 主路径虽然改为 runtime index lookup，但该 capability仍违反本计划“不能保存裸 worker capability”的验收条件，并给后续代码留下误用已析构 worker的入口。
- 修复要求：从 registration result和 public handle删除该字段，或降级为不具备解引用能力的诊断 identity；控制路径只保留 shared control、worker index、relay id及双 generation。

#### P1：release/system/performance 证据尚未完成

- `docs/relay_win.md` 已明确延期 F01-F18、k6 short-tunnel、raw TCP、30分钟 churn/worker-stop、ASan/Application Verifier及延迟/吞吐回归门禁；仓库中也没有本轮 Windows binary SHA、测试日志和 fault timeline证据。
- 在 P0 编译和生命周期问题修复前，Task 7 的 Windows build/run清单、每项 focused teardown zero、Verifier/ASan和系统性能门禁必须全部保持未完成。

### 建议修复顺序

1. 先统一 immutable control generation并恢复 Windows全量编译。
2. 删除 `TryRetireRelay()`/stop finalize/registration rollback中的人工 terminal publish，重新建立“主动 shutdown只保留 retention，真实 terminal才释放”的唯一状态迁移。
3. 修复 publish 后 activation failure的 shutdown ledger与 sink handoff，并增加真实 wrapper/barrier测试。
4. 修复并恢复所有因 crash/deadlock/hang禁用的 stop-drain集成用例，再执行 Windows unit、Verifier/ASan和 F01-F18/性能门禁。
