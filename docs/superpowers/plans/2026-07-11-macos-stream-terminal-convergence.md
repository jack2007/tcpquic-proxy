# macOS Stream Terminal Convergence Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 在不改变正常 FIN half-close 的前提下，把 Darwin kqueue relay 的 fatal TCP 结束、注册事务、queue-full fallback、worker stop、retired purge、tunnel reaper 与公共 stream terminal handoff/watchdog/connection escalation 接成有界、可观测、exactly-once 的收敛链路。

**Architecture:** Linux 公共层冻结 `TqStreamLifetime::BeginTerminalShutdown()`、`TqTerminalSink`、`TqTerminalWatchdog`、retention ledger 与 `TqTerminalEscalation` 后，Darwin 只实现平台 adapter，不复制 terminal phase 或 retry 状态机。Darwin activation mutex 与 worker lifecycle gate 共同线性化 prepare/publish/commit、fatal handoff 和 Stop；所有 kqueue late event、queue-full fallback、retired purge 都只携带 shared control/sink/ledger 与 generation，worker 退出后不再进入 worker-thread-only handler。

**Tech Stack:** C++17, Darwin kqueue, MsQuic C++ wrapper, CMake, deterministic barrier/fault injection, AddressSanitizer, k6, JSON Admin API.

## Global Constraints

- 文档和新增测试说明使用中文；生产符号沿用仓库现有英文命名。
- 本计划以 `docs/superpowers/specs/2026-07-11-cross-platform-stream-terminal-convergence-design.md` 为唯一跨平台语义源；不在 Darwin 复制 `TerminalPhase`、shutdown retry、watchdog scheduler、retention registry 或 sink ledger。
- 公共层依赖必须先冻结：`TqTerminalShutdownResult`、`TqStreamLifetime::BeginTerminalShutdown(uint64_t, std::shared_ptr<TqStreamLifetime::Target>, std::shared_ptr<TqTerminalEscalation>) noexcept`、`TqTerminalLedger`、`TqTerminalSink::Create(std::weak_ptr<TqStreamLifetime>, std::shared_ptr<TqTerminalLedger>) noexcept`、`TqTerminalWatchdog`、`TqTerminalEscalation` 和 retention detail snapshot。
- `QUIC_STATUS_SUCCESS` 与 `QUIC_STATUS_PENDING` 都表示 shutdown 已提交；同步失败最多重试 3 次，退避固定为 10 ms、50 ms、250 ms。
- fatal shutdown 使用 `QUIC_STREAM_SHUTDOWN_FLAG_ABORT | QUIC_STREAM_SHUTDOWN_FLAG_IMMEDIATE`；默认 watchdog deadline 为 5 秒，可配置范围为 5–30 秒。
- watchdog 首次超时只执行一次 generation-safe connection shutdown/reconnect escalation；升级后 30 秒仍无 terminal 时标记 `terminal_timeout` 并阻断发布，不直接 `StreamClose`。
- 正常 TCP EOF 继续提交一次 QUIC FIN；正常 QUIC FIN/`PEER_SEND_SHUTDOWN` 继续排空后对 TCP 执行 `SHUT_WR`；两者都不得建立 fatal sink、启动 watchdog 或升级 connection。
- started stream 只有真实 `QUIC_STREAM_EVENT_SHUTDOWN_COMPLETE` 才能发布 terminal；`SEND_SHUTDOWN_COMPLETE`、peer abort、TCP close、relay stop、worker stop 均不是 terminal 事实源。
- `StreamClose` 仍只由 `TqStreamLifetime` 析构中的唯一 RAII 路径间接执行；sink、watchdog、worker、reaper 和 connection controller 均不得直接 close stream handle。
- callback、event queue、retired state 与 sink 不得保存可解引用的 `TqRelayHandle*`、`MsQuicStream*`、`MsQuicConnection*`、tunnel context 或 worker raw pointer。
- prepare/publish/commit 保持 FD disposition：publish 前失败仍由 caller 拥有 TCP fd；publish 成功后 token 不可逆接管；commit/rollback/terminal 只能 close fd 和删除 kqueue filter 一次。
- target publish 后 binding identity（relay id、route generation、control generation、weak owner、completion state、terminal context）不再修改。
- reaper 门禁固定为 `DataPlaneStopped && TerminalHandoffComplete && LocalOperationOwnershipTransferredOrDrained`；reaper 不调用 stream API、不取消 watchdog、不删除 retention。
- 所有竞态测试使用 barrier/latch/fault hook，不使用 sleep 猜测顺序；所有 fault hook 只在 `TCPQUIC_TESTING` 或 `TQ_UNIT_TESTING` 编译中存在。
- Darwin target 只在 `CMAKE_SYSTEM_NAME STREQUAL "Darwin"` 下构建；当前真实 target 名为 `tcpquic_darwin_relay_worker_io_test`、`tcpquic_darwin_relay_worker_queue_test`、`tcpquic_darwin_relay_metrics_test`、`tcpquic_darwin_reactor_test`，公共相关 target 为 `tcpquic_stream_lifetime_test`、`tcpquic_tcp_tunnel_test`、`tcpquic_tunnel_reaper_test`、`tcpquic_client_tunnel_open_test`、`tcpquic_router_runtime_test`。
- 性能门禁：tunnel 建立 p95 不高于旧基线 105%，p99 不高于 110%，吞吐不低于 95%，CPU/RSS 峰值增幅各不超过 10%，不增加常驻线程。
- soak 固定为 100 并发、至少 100 tunnel/s、30 分钟；结束 30 秒后 active tunnel/relay、terminal sink、watchdog、send completion、retained owner、retired relay 全部回到基线。

---

## File Structure

- Modify: `src/tunnel/darwin_relay_worker.h`
  - 扩展 registration 的公共 terminal context，声明 Darwin fatal handoff disposition、worker lifecycle gate、测试 barrier 与本地 snapshot 字段。
- Modify: `src/tunnel/darwin_relay_worker.cpp`
  - 接入公共 handoff；线性化 registration/Stop；让 `EV_EOF/EV_ERROR`、queue-full、worker stop、retired purge 使用独立 sink/control；保护 FIN half-close。
- Modify: `src/tunnel/darwin_relay_event_queue.h`
  - terminal/control event 只携带 shared ownership 与 generation，不携带 stream/tunnel/worker capability。
- Modify: `src/tunnel/relay.h`, `src/tunnel/relay.cpp`
  - 将公共 terminal identity/escalation context 传入 Darwin registration，并在成功 commit 后保持 caller-only handle publication。
- Modify: `src/tunnel/tcp_tunnel.cpp`
  - 为 Darwin start 构造已冻结的 terminal identity/escalation；用 handoff-complete predicate 驱动 reaper；不改变 graceful FIN 路径。
- Modify: `src/tunnel/tunnel_reaper.h`, `src/tunnel/tunnel_reaper.cpp`
  - 消费公共 handoff predicate，不以 relay `Stop` 单独推导 tunnel 可删除。
- Modify: `src/runtime/relay_metrics.h`, `src/runtime/relay_metrics.cpp`
  - 聚合 Darwin handoff/sink/watchdog/escalation/timeout/duplicate suppression 与 retained detail；修复 precommit snapshot 并发读取。
- Modify: `src/runtime/admin_http.cpp`, `src/runtime/router_runtime.cpp`
  - 暴露公共 retention detail 查询和过滤，Darwin backend 值为 `darwin`，worker backend 保持 `kqueue`。
- Modify: `src/unittest/darwin_relay_worker_io_test.cpp`
  - 覆盖 kqueue fatal、正常 half-close、transaction/Stop、queue-full、late completion、retired purge、fd reuse 与精确事故时序。
- Modify: `src/unittest/darwin_relay_worker_queue_test.cpp`
  - 覆盖 typed terminal event、worker-exited purge 和 queue capacity/fallback ownership。
- Modify: `src/unittest/darwin_relay_metrics_test.cpp`
  - 覆盖 snapshot/聚合/Admin JSON 与并发 precommit 读取。
- Modify: `src/unittest/tcp_tunnel_test.cpp`, `src/unittest/tunnel_reaper_test.cpp`
  - 覆盖 tunnel handoff gate、generation-safe escalation 和 terminal 后删除。
- Modify: `src/unittest/router_runtime_test.cpp`, `src/unittest/admin_http_test.cpp`
  - 覆盖 retention detail endpoint、过滤和告警字段。
- Modify: `src/CMakeLists.txt`
  - 把新增 Darwin test source/definition 接入现有真实 target，不创建重复 target。
- Create: `tests/k6/macos-terminal-convergence.js`
  - 统一 baseline/peak/spike/stress/soak workload 与资源归零检查。
- Create: `scripts/run-macos-terminal-convergence.sh`
  - 固定采集 build SHA、macOS 版本、Admin 前后快照、k6 summary 与失败证据。
- Modify: `docs/relay_macos.md`, `docs/test/macos-performance-stress-test-design_cn.md`
  - 记录最终 handoff/Stop/purge 语义与可复现命令；只在实现和证据完成后更新。

### Task 1: 固化公共依赖并把 terminal context 传入 Darwin registration

**Files:**
- Modify: `src/tunnel/darwin_relay_worker.h`
- Modify: `src/tunnel/relay.h`
- Modify: `src/tunnel/relay.cpp`
- Modify: `src/tunnel/tcp_tunnel.cpp`
- Test: `src/unittest/darwin_relay_worker_io_test.cpp`
- Test: `src/unittest/tcp_tunnel_test.cpp`

**Interfaces:**
- Consumes: `TqTerminalShutdownResult TqStreamLifetime::BeginTerminalShutdown(uint64_t errorCode, std::shared_ptr<TqStreamLifetime::Target> terminalSink, std::shared_ptr<TqTerminalEscalation> escalation) noexcept`; `void TqStreamLifetime::BindTerminalIdentity(TqTerminalIdentity identity, uint32_t watchdogSeconds = 5) noexcept`; `std::shared_ptr<TqTerminalLedger> TqStreamLifetime::TerminalLedger() const noexcept`; `std::shared_ptr<TqTerminalSink> TqTerminalSink::Create(std::weak_ptr<TqStreamLifetime> owner, std::shared_ptr<TqTerminalLedger> ledger) noexcept`; `TqTerminalIdentity`, `TqTerminalLedger` and `TqTerminalEscalation` from the frozen common layer.
- Produces: `TqRelayTerminalContext`; extended `TqRelayStartManaged(..., const TqRelayTerminalContext& terminalContext, ...)`; `TqDarwinRelayRegistration::TerminalLedger`, `TqDarwinRelayRegistration::TerminalSink`, `TqDarwinRelayRegistration::TerminalEscalation`; immutable copies in `RelayState`/`StreamBinding`; existing `TqDarwinRelayRegistrationResult{bool Ok, bool TcpFdConsumed, uint64_t RelayId}` remains unchanged.

- [ ] **Step 1: 写 registration identity 的失败测试**

```cpp
void ManagedRegistrationPublishesFrozenTerminalContext() {
    TqDarwinRelayWorker worker(MakeManagedConfig());
    CHECK(worker.StartForTest());
    auto owner = TqStreamLifetime::CreateForTest(TqStreamLifetime::Phase::Started);
    auto escalation = std::make_shared<RecordingTerminalEscalation>();
    const TqTerminalIdentity identity{
        .StreamId = 17,
        .TunnelId = 133,
        .ConnectionId = 2,
        .ConnectionGeneration = 9,
        .Role = TqTunnelRole::ClientOpen,
        .Backend = TqRelayBackendType::DarwinWorker,
    };
    owner->BindTerminalIdentity(identity, 5);
    auto ledger = owner->TerminalLedger();
    CHECK(ledger != nullptr);
    auto sink = TqTerminalSink::Create(owner, ledger);
    ManagedRelayHarness relay(worker, owner, ledger, sink, escalation);
    CHECK(relay.Result.Ok);
    const auto snapshot = worker.BindingTerminalContextForTest(relay.Result.RelayId);
    CHECK(snapshot.Ledger == ledger);
    CHECK(snapshot.Ledger == owner->TerminalLedger());
    CHECK(snapshot.Ledger->Identity().StreamId == 17);
    CHECK(snapshot.Ledger->Identity().TunnelId == 133);
    CHECK(snapshot.Ledger->Identity().ConnectionId == 2);
    CHECK(snapshot.Ledger->Identity().ConnectionGeneration == 9);
    CHECK(snapshot.Ledger->Identity().Role == TqTunnelRole::ClientOpen);
    CHECK(snapshot.Ledger->Identity().Backend == TqRelayBackendType::DarwinWorker);
    CHECK(snapshot.Sink == sink);
    CHECK(snapshot.Escalation == escalation);
    worker.Stop();
}
```

- [ ] **Step 2: 运行测试并确认缺少字段而失败**

Run: `rtk cmake --build build --target tcpquic_darwin_relay_worker_io_test -j$(sysctl -n hw.ncpu)`

Expected: FAIL，编译器报告 `TqDarwinRelayRegistration` 没有 `TerminalLedger`/`TerminalSink`/`TerminalEscalation` 或 worker 没有 `BindingTerminalContextForTest()`。

- [ ] **Step 3: 增加只读 terminal context 并沿 start 边界传递**

```cpp
struct TqRelayTerminalContext {
    std::shared_ptr<TqTerminalLedger> Ledger;
    std::shared_ptr<TqTerminalSink> Sink;
    std::shared_ptr<TqTerminalEscalation> Escalation;
};

struct TqDarwinRelayRegistration {
    TqSocketHandle TcpFd{TqInvalidSocket};
    MsQuicStream* Stream{nullptr};
    std::shared_ptr<TqStreamLifetime> StreamOwner;
    std::shared_ptr<TqRelayStopControl> Control;
    uint64_t ControlGeneration{0};
    std::shared_ptr<TqTerminalLedger> TerminalLedger;
    std::shared_ptr<TqTerminalSink> TerminalSink;
    std::shared_ptr<TqTerminalEscalation> TerminalEscalation;
    ITqCompressor* Compressor{nullptr};
    ITqDecompressor* Decompressor{nullptr};
    TqCompressAlgo CompressAlgo{TqCompressAlgo::None};
    bool EnableQuicSends{true};
};

bool TqRelayStartManaged(
    TqSocketHandle tcpFd,
    MsQuicStream* stream,
    std::shared_ptr<TqStreamLifetime> streamOwner,
    ITqCompressor* compressor,
    ITqDecompressor* decompressor,
    TqRelayHandle* handle,
    const TqTuningConfig& tuning,
    const TqRelayTerminalContext& terminalContext,
    TqCompressAlgo compressAlgo = TqCompressAlgo::None,
    bool* tcpFdConsumed = nullptr);

struct TqDarwinTerminalContextSnapshot {
    std::shared_ptr<TqTerminalLedger> Ledger;
    std::shared_ptr<TqTerminalSink> Sink;
    std::shared_ptr<TqTerminalEscalation> Escalation;
};
```

在 `RegisterRelayWithIdLocal()` 的 `PublishTarget()` 前一次性初始化：

```cpp
relay->TerminalLedger = registration.TerminalLedger;
relay->TerminalSink = registration.TerminalSink;
relay->TerminalEscalation = registration.TerminalEscalation;
binding->TerminalLedger = registration.TerminalLedger;
binding->TerminalSink = registration.TerminalSink;
binding->TerminalEscalation = registration.TerminalEscalation;
```

`TqRelayStartManaged()` 的 Darwin 分支必须把 caller 创建的同一实例直接传入，不重新创建 sink/watchdog：

```cpp
if (terminalContext.Ledger == nullptr || terminalContext.Sink == nullptr ||
    terminalContext.Escalation == nullptr || streamOwner == nullptr ||
    terminalContext.Ledger != streamOwner->TerminalLedger()) {
    return false;
}
registration.TerminalLedger = terminalContext.Ledger;
registration.TerminalSink = terminalContext.Sink;
registration.TerminalEscalation = terminalContext.Escalation;
```

- [ ] **Step 4: 运行 focused tests**

Run: `rtk cmake --build build --target tcpquic_stream_lifetime_test tcpquic_darwin_relay_worker_io_test tcpquic_tcp_tunnel_test -j$(sysctl -n hw.ncpu) && rtk build/bin/Release/tcpquic_stream_lifetime_test && rtk build/bin/Release/tcpquic_darwin_relay_worker_io_test && rtk build/bin/Release/tcpquic_tcp_tunnel_test`

Expected: 三个可执行文件均退出 0；identity 与 shared instance 精确相等；Linux/Windows 条件编译不受 Darwin 字段影响。

- [ ] **Step 5: 提交**

```bash
rtk git add src/tunnel/darwin_relay_worker.h src/tunnel/relay.h src/tunnel/relay.cpp src/tunnel/tcp_tunnel.cpp src/unittest/darwin_relay_worker_io_test.cpp src/unittest/tcp_tunnel_test.cpp
rtk git commit -m "feat(darwin): pass terminal handoff context into relay registration"
```

### Task 2: 接入 kqueue fatal handoff并保护正常 FIN half-close

**Files:**
- Modify: `src/tunnel/darwin_relay_worker.h`
- Modify: `src/tunnel/darwin_relay_worker.cpp`
- Test: `src/unittest/darwin_relay_worker_io_test.cpp`

**Interfaces:**
- Consumes: Task 1 immutable terminal context; common `BeginTerminalShutdown()` result semantics.
- Produces: `TqDarwinFatalReason`; `TqDarwinRelayWorker::BeginFatalTerminalHandoff(const std::shared_ptr<RelayState>&, TqDarwinFatalReason, uint64_t) -> TqTerminalShutdownResult`; `RelayState::TerminalHandoffComplete` and `RelayState::DataPlaneStopped` atomics.

- [ ] **Step 1: 写 fatal 与 half-close 对照失败测试**

```cpp
void KqueueErrorUsesImmediateHandoffButTcpEofKeepsHalfClose() {
    ManagedRelayHarness fatal;
    fatal.Worker.InvokeTcpEventForTest(
        fatal.RelayId(), EVFILT_READ, EV_ERROR, ECONNRESET);
    CHECK(fatal.ShutdownCalls.size() == 1);
    CHECK(fatal.ShutdownCalls[0].Flags ==
        (QUIC_STREAM_SHUTDOWN_FLAG_ABORT | QUIC_STREAM_SHUTDOWN_FLAG_IMMEDIATE));
    CHECK(fatal.Worker.TerminalHandoffCompleteForTest(fatal.RelayId()));
    CHECK(fatal.TerminalLedger().WatchdogState == TqTerminalWatchdogState::Armed);

    ManagedRelayHarness graceful;
    graceful.Worker.InvokeTcpEventForTest(
        graceful.RelayId(), EVFILT_READ, EV_EOF, 0);
    CHECK(graceful.SendCalls.size() == 1);
    CHECK((graceful.SendCalls[0].Flags & QUIC_SEND_FLAG_FIN) != 0);
    CHECK(graceful.ShutdownCalls.empty());
    CHECK(graceful.TerminalLedger().WatchdogState == TqTerminalWatchdogState::Idle);
    CHECK(graceful.Worker.BindingActiveForTest(graceful.RelayId()));
}
```

- [ ] **Step 2: 运行测试并确认 fatal 仍走 `AbortBoth` 而失败**

Run: `rtk cmake --build build --target tcpquic_darwin_relay_worker_io_test -j$(sysctl -n hw.ncpu)`

Expected: FAIL，fatal flags 缺少 `IMMEDIATE` 或 `TerminalHandoffCompleteForTest()` 尚未定义；graceful 分支应继续通过 FIN 断言。

- [ ] **Step 3: 实现统一 Darwin fatal adapter**

```cpp
enum class TqDarwinFatalReason : uint8_t {
    TcpReset,
    TcpError,
    AdminStop,
    WorkerStop,
    RegistrationRollback,
    ReceiveResourceFailure,
};

TqTerminalShutdownResult TqDarwinRelayWorker::BeginFatalTerminalHandoff(
    const std::shared_ptr<RelayState>& relay,
    TqDarwinFatalReason reason,
    uint64_t errorCode) {
    if (relay == nullptr || relay->StreamOwner == nullptr ||
        relay->TerminalLedger == nullptr ||
        relay->TerminalSink == nullptr || relay->TerminalEscalation == nullptr) {
        Errors.fetch_add(1, std::memory_order_relaxed);
        return TqTerminalShutdownResult{QUIC_STATUS_INVALID_STATE, false, false, false, 0};
    }
    if (relay->StreamOwner->TerminalLedger() != relay->TerminalLedger) {
        Errors.fetch_add(1, std::memory_order_relaxed);
        return TqTerminalShutdownResult{QUIC_STATUS_INVALID_STATE, false, false, false, 0};
    }
    relay->FreezeReason.store(static_cast<uint8_t>(reason), std::memory_order_release);
    relay->DataPlaneStopped.store(true, std::memory_order_release);
    const auto result = relay->StreamOwner->BeginTerminalShutdown(
        errorCode,
        relay->TerminalSink,
        relay->TerminalEscalation);
    const bool transferred = result.Submitted || result.AlreadyTerminal ||
        result.RetryScheduled;
    relay->TerminalHandoffComplete.store(transferred, std::memory_order_release);
    if (!transferred && relay->StopControl != nullptr) {
        (void)relay->StopControl->SignalStop(relay->ControlGeneration);
    }
    return result;
}
```

`ProcessTcpEvents()` 只把 hard error 送入该 helper；纯 EOF 继续原有 FIN：

```cpp
if ((flags & EV_ERROR) != 0 || (data != 0 && (flags & EV_EOF) != 0)) {
    (void)BeginFatalTerminalHandoff(
        relay,
        data == ECONNRESET ? TqDarwinFatalReason::TcpReset
                           : TqDarwinFatalReason::TcpError,
        static_cast<uint64_t>(data));
    CloseRelay(relay, TqDarwinRelayCloseDisposition::ActiveShutdown);
    return;
}
if ((flags & EV_EOF) != 0) {
    DrainTcpReadable(relay);
    SubmitTcpBatchToQuic(relay, {}, true);
    return;
}
```

- [ ] **Step 4: 覆盖 `SUCCESS/PENDING`、同步失败和 terminal 重入**

```cpp
for (const QUIC_STATUS status : {
         QUIC_STATUS_SUCCESS,
         QUIC_STATUS_PENDING,
         QUIC_STATUS_OUT_OF_MEMORY}) {
    ManagedRelayHarness relay(status);
    relay.InjectTcpReset();
    const auto ledger = relay.TerminalLedger();
    if (status == QUIC_STATUS_OUT_OF_MEMORY) {
        CHECK(ledger.ShutdownAttempt == 1);
        CHECK(ledger.RetryScheduled);
    } else {
        CHECK(ledger.ShutdownSubmitted);
        CHECK(ledger.WatchdogState == TqTerminalWatchdogState::Armed);
    }
}
```

- [ ] **Step 5: 运行 focused tests**

Run: `rtk cmake --build build --target tcpquic_darwin_relay_worker_io_test tcpquic_stream_lifetime_test -j$(sysctl -n hw.ncpu) && rtk build/bin/Release/tcpquic_darwin_relay_worker_io_test && rtk build/bin/Release/tcpquic_stream_lifetime_test`

Expected: 退出 0；正常 EOF shutdown API 次数为 0、FIN 一次；fatal `SUCCESS/PENDING` 均 handoff complete，失败进入公共 retry。

- [ ] **Step 6: 提交**

```bash
rtk git add src/tunnel/darwin_relay_worker.h src/tunnel/darwin_relay_worker.cpp src/unittest/darwin_relay_worker_io_test.cpp
rtk git commit -m "fix(darwin): hand off fatal kqueue errors before relay retirement"
```

### Task 3: 线性化 prepare/publish/commit 与 worker Stop

**Files:**
- Modify: `src/tunnel/darwin_relay_worker.h`
- Modify: `src/tunnel/darwin_relay_worker.cpp`
- Test: `src/unittest/darwin_relay_worker_io_test.cpp`

**Interfaces:**
- Consumes: existing `PreparedRelayToken`, `StreamBinding::ActivationMutex`, `PublishTarget(expectedGeneration, target, &publishedGeneration)` and Task 1 terminal context.
- Produces: `enum class TqDarwinWorkerLifecycle : uint8_t { Accepting, Stopping, Stopped }`; `LifecycleState`; `TryAdmitPreparedRelay()`; successful Stop handoff or deterministic transaction rollback.

- [ ] **Step 1: 写四个 Stop barrier 失败测试**

```cpp
for (uint32_t iteration = 0; iteration != 1000; ++iteration) {
    for (const auto point : {
             StopRacePoint::BeforeMapPublication,
             StopRacePoint::AfterMapPublication,
             StopRacePoint::AfterOwnerPublish,
             StopRacePoint::AfterCommit}) {
        StopRegistrationRaceHarness race(point);
        race.Run();
        CHECK(race.FilterInstalls() == race.FilterDeletes());
        CHECK(race.FdCloseCount() == 1);
        CHECK(race.ActiveAccountingReleaseCount() == 1);
        CHECK(race.OwnerRouteIsTerminalSink() || race.OwnerIsTerminal());
        if (point == StopRacePoint::AfterCommit) {
            CHECK(race.RegistrationResult().Ok);
            CHECK(race.ControlStopped());
        } else {
            CHECK(!race.RegistrationResult().Ok);
            CHECK(race.RegistrationResult().TcpFdConsumed == race.OwnerWasPublished());
        }
    }
}
```

- [ ] **Step 2: 运行测试并复现 P1-11**

Run: `rtk cmake --build build --target tcpquic_darwin_relay_worker_io_test -j$(sysctl -n hw.ncpu) && rtk build/bin/Release/tcpquic_darwin_relay_worker_io_test`

Expected: FAIL；至少 `AfterMapPublication` 或 `AfterOwnerPublish` 能在 Stop 快照之后继续 commit，或 sink publish 返回值被忽略。

- [ ] **Step 3: 增加 worker lifecycle gate**

```cpp
enum class TqDarwinWorkerLifecycle : uint8_t {
    Accepting,
    Stopping,
    Stopped,
};

std::atomic<TqDarwinWorkerLifecycle> LifecycleState{
    TqDarwinWorkerLifecycle::Stopped};

bool TqDarwinRelayWorker::TryAdmitPreparedRelay() const noexcept {
    return LifecycleState.load(std::memory_order_acquire) ==
        TqDarwinWorkerLifecycle::Accepting;
}
```

`Start()` 在 kqueue/thread 成功后发布 `Accepting`；`Stop()` 先在 `LifecycleMutex` 下发布 `Stopping`，再扫描 prepared/active bindings；`TryCommitPreparedActivation()` 在同一 activation 临界区复核：

```cpp
if (LifecycleState.load(std::memory_order_acquire) !=
        TqDarwinWorkerLifecycle::Accepting) {
    binding->ActivationState.store(
        StreamBinding::Activation::Failed,
        std::memory_order_release);
    return PreparedCommitDisposition::RollbackFailed;
}
```

- [ ] **Step 4: 检查 sink publish 结果并回滚未交接 binding**

```cpp
const bool published = relay->StreamOwner->PublishTarget(
    binding->RouteGeneration,
    relay->TerminalSink,
    &publishedGeneration);
if (!published) {
    const auto ownerPhase = relay->StreamOwner->GetPhase();
    if (ownerPhase != TqStreamLifetime::Phase::TerminalPublished &&
        ownerPhase != TqStreamLifetime::Phase::Closed) {
        RollbackPreparedRelay(token, TqStreamLifetime::ShutdownIntent::AbortBoth);
        return false;
    }
}
return true;
```

- [ ] **Step 5: 运行 1000 次确定性 race case**

Run: `rtk build/bin/Release/tcpquic_darwin_relay_worker_io_test`

Expected: 1000 次退出 0；无成功 registration 留下 worker binding route；FD/filter/accounting 每次各收敛一次。

- [ ] **Step 6: 提交**

```bash
rtk git add src/tunnel/darwin_relay_worker.h src/tunnel/darwin_relay_worker.cpp src/unittest/darwin_relay_worker_io_test.cpp
rtk git commit -m "fix(darwin): linearize relay registration with worker stop"
```

### Task 4: 让 queue-full fallback 与 late SEND_COMPLETE 脱离 worker

**Files:**
- Modify: `src/tunnel/darwin_relay_event_queue.h`
- Modify: `src/tunnel/darwin_relay_worker.h`
- Modify: `src/tunnel/darwin_relay_worker.cpp`
- Test: `src/unittest/darwin_relay_worker_queue_test.cpp`
- Test: `src/unittest/darwin_relay_worker_io_test.cpp`

**Interfaces:**
- Consumes: common sink `TqStreamLifetime::Target`; current `CompletionState`; current owner completion registry preserving submit-time `RouteSnapshot::TargetOwner`.
- Produces: `TqDarwinTerminalEnvelope` shared payload; non-target helper `TqDarwinDetachedCompletion::Settle()`; queue fallback publishes the common `TqTerminalSink` and never re-enters worker data plane.

- [ ] **Step 1: 写真实 owner registry late completion 失败测试**

```cpp
void StopThenOwnerDispatchesLateSendCompleteExactlyOnce() {
    ManagedRelayHarness relay;
    relay.SubmitOneSendThroughOwnerRegistry();
    CHECK(relay.Worker.KnownSendOperationCountForTest() == 1);
    relay.Worker.Stop();
    relay.DispatchSendCompleteFromOwner();
    CHECK(relay.Worker.KnownSendOperationCountForTest() == 0);
    CHECK(relay.Worker.InFlightQuicSendCountFromRelayForTest(relay.RelayOwner) == 0);
    CHECK(relay.Worker.RetiredStreamBindingCountForTest() == 0);
    CHECK(relay.Worker.SendOperationDestructorCountForTest() == 1);
    CHECK(TqStreamLifetime::SnapshotSendCompletions().ActiveCount == 0);
}
```

- [ ] **Step 2: 运行测试并复现 P1-10**

Run: `rtk cmake --build build --target tcpquic_darwin_relay_worker_io_test -j$(sysctl -n hw.ncpu) && rtk build/bin/Release/tcpquic_darwin_relay_worker_io_test`

Expected: FAIL；旧 submit-time binding 因 endpoint closed 拒绝 callback，known operation 或 retired binding 非零。

- [ ] **Step 3: 定义 worker-independent terminal envelope**

```cpp
struct TqDarwinTerminalEnvelope {
    uint64_t RelayId{0};
    uint64_t RouteGeneration{0};
    uint64_t ControlGeneration{0};
    std::shared_ptr<TqRelayStopControl> Control;
    std::shared_ptr<TqTerminalLedger> Ledger;
    std::shared_ptr<TqTerminalSink> Sink;
    std::shared_ptr<void> CompletionOwner;
};

struct TqDarwinRelayEvent {
    TqDarwinRelayEventType Type{TqDarwinRelayEventType::Shutdown};
    uint64_t Value{0};
    uint64_t RelayId{0};
    std::shared_ptr<TqDarwinTerminalEnvelope> Terminal;
    std::shared_ptr<void> RelayOwner;
    std::shared_ptr<void> ControlOwner;
    std::shared_ptr<TqDarwinPendingQuicReceive> ReceiveView;
};
```

生产 terminal/control event 删除 `void* Relay` 和异步栈 `Control` 使用；register/unregister 若仍同步等待，使用 heap-owned command envelope 后再删除这两个 raw 字段。

- [ ] **Step 4: endpoint admission 失败时只做 typed detached settlement**

```cpp
QUIC_STATUS StreamBinding::OnStreamEvent(
    MsQuicStream* stream,
    QUIC_STREAM_EVENT* event,
    uint64_t generation) noexcept {
    auto admission = Endpoint != nullptr ? Endpoint->TryEnter() : CallbackEndpoint::Guard{};
    if (!admission) {
        if (event != nullptr && event->Type == QUIC_STREAM_EVENT_SEND_COMPLETE) {
            return TqDarwinDetachedCompletion::Settle(
                Completions,
                event->SEND_COMPLETE.ClientContext);
        }
        if (event != nullptr && event->Type == QUIC_STREAM_EVENT_RECEIVE) {
            event->RECEIVE.TotalBufferLength = 0;
        }
        return QUIC_STATUS_SUCCESS;
    }
    return admission.Worker()->OnStreamEventWithBinding(stream, event, this);
}
```

`TqDarwinDetachedCompletion::Settle()` 不是 callback target，只能 claim typed registry、归还 buffer/accounting 并 delete operation，不访问 worker/relay data-plane handler；terminal/RECEIVE 继续由 Task 1 传入的公共 `TqTerminalSink` 处理，不创建 Darwin 私有 sink 状态机。

- [ ] **Step 5: 覆盖 terminal/receive/active-failure queue full**

```cpp
for (const auto type : {
         TqDarwinRelayEventType::QuicShutdownComplete,
         TqDarwinRelayEventType::QuicReceiveView,
         TqDarwinRelayEventType::QuicActiveShutdown}) {
    QueueFullFallbackHarness h(type);
    h.FillQueue();
    h.Dispatch();
    CHECK(h.CallbackWaitCount() == 0);
    CHECK(h.RawCapabilityDereferenceCount() == 0);
    CHECK(h.ControlStopCount() <= 1);
    CHECK(h.PendingOwnershipAfterTerminal() == 0);
}
```

- [ ] **Step 6: 运行 queue 与 IO tests**

Run: `rtk cmake --build build --target tcpquic_darwin_relay_worker_queue_test tcpquic_darwin_relay_worker_io_test -j$(sysctl -n hw.ncpu) && rtk build/bin/Release/tcpquic_darwin_relay_worker_queue_test && rtk build/bin/Release/tcpquic_darwin_relay_worker_io_test`

Expected: 退出 0；queue-full callback 不等待；stop 后 late send completion 使 operation、registry、retired binding 全部为 0。

- [ ] **Step 7: 提交**

```bash
rtk git add src/tunnel/darwin_relay_event_queue.h src/tunnel/darwin_relay_worker.h src/tunnel/darwin_relay_worker.cpp src/unittest/darwin_relay_worker_queue_test.cpp src/unittest/darwin_relay_worker_io_test.cpp
rtk git commit -m "fix(darwin): settle terminal fallback without worker admission"
```

### Task 5: 收紧 worker stop、retired purge 与 tunnel reaper 门禁

**Files:**
- Modify: `src/tunnel/darwin_relay_worker.cpp`
- Modify: `src/tunnel/tunnel_reaper.h`
- Modify: `src/tunnel/tunnel_reaper.cpp`
- Modify: `src/tunnel/tcp_tunnel.cpp`
- Test: `src/unittest/darwin_relay_worker_io_test.cpp`
- Test: `src/unittest/tunnel_reaper_test.cpp`

**Interfaces:**
- Consumes: Tasks 2–4 handoff facts; common handoff ledger snapshot.
- Produces: `TqTunnelReleaseFacts{DataPlaneStopped, TerminalHandoffComplete, LocalOperationOwnershipTransferredOrDrained}`; worker stop order `freeze -> sink publish -> handoff -> join -> lifecycle-safe purge`.

- [ ] **Step 1: 写 reaper 三事实 truth-table 失败测试**

```cpp
void TerminalHandoffGatesTunnelRelease() {
    for (unsigned mask = 0; mask != 8; ++mask) {
        TqTunnelReleaseFacts facts{
            .DataPlaneStopped = (mask & 1) != 0,
            .TerminalHandoffComplete = (mask & 2) != 0,
            .LocalOperationOwnershipTransferredOrDrained = (mask & 4) != 0,
        };
        CHECK(TqCanReleaseTunnel(facts) == (mask == 7));
    }
}
```

- [ ] **Step 2: 写 worker 退出后 mixed purge 测试**

```cpp
void RealWorkerExitPurgesLifecycleOnly() {
    RealKqueueWorkerHarness h;
    h.QueueMixedEventsBeforeExit();
    h.Stop();
    CHECK(h.WorkerThreadOnlyHandlerCallsAfterExit() == 0);
    CHECK(h.PendingReceiveOwners() == 0);
    CHECK(h.PendingSendOwners() == 0);
    CHECK(h.TerminalSinkPending() == 0);
    CHECK(h.RetiredRelayCount() == 0);
    CHECK(h.FdCloseCount() == 1);
}
```

- [ ] **Step 3: 运行并确认旧 Stop/reaper 条件失败**

Run: `rtk cmake --build build --target tcpquic_tunnel_reaper_test tcpquic_darwin_relay_worker_io_test -j$(sysctl -n hw.ncpu) && rtk build/bin/Release/tcpquic_tunnel_reaper_test && rtk build/bin/Release/tcpquic_darwin_relay_worker_io_test`

Expected: FAIL；旧 reaper 仅看 stop，或 purge 仍调用 `CloseRelay()`/`CompleteQuicSend()` worker handler。

- [ ] **Step 4: 实现公共 predicate 的 Darwin 消费**

```cpp
struct TqTunnelReleaseFacts {
    bool DataPlaneStopped{false};
    bool TerminalHandoffComplete{false};
    bool LocalOperationOwnershipTransferredOrDrained{false};
};

constexpr bool TqCanReleaseTunnel(const TqTunnelReleaseFacts& facts) noexcept {
    return facts.DataPlaneStopped &&
        facts.TerminalHandoffComplete &&
        facts.LocalOperationOwnershipTransferredOrDrained;
}
```

`TqTunnelContext` 从 relay control/terminal ledger 复制 facts；reaper 只消费值快照。同步 shutdown 失败但公共 retry/sink/escalation 已独立接管时，`TerminalHandoffComplete=true`。

- [ ] **Step 5: 重排 Stop 并限制 retired purge**

```cpp
void TqDarwinRelayWorker::Stop() {
    std::unordered_map<uint64_t, std::shared_ptr<RelayState>> relays;
    {
        std::lock_guard<std::mutex> lifecycle(LifecycleMutex);
        LifecycleState.store(TqDarwinWorkerLifecycle::Stopping,
            std::memory_order_release);
        InstallShutdownSinksForStop();
        if (StreamCallbackEndpoint != nullptr) {
            StreamCallbackEndpoint->CloseAndWait();
        }
        {
            std::shared_lock<std::shared_mutex> access(RelayMapAccessMutex);
            relays = Relays;
        }
        for (const auto& entry : relays) {
            (void)BeginFatalTerminalHandoff(
                entry.second, TqDarwinFatalReason::WorkerStop, 0);
        }
        Running.store(false, std::memory_order_release);
        (void)Wake();
    }
    if (Thread.joinable()) {
        Thread.join();
    }
    DetachActiveSendOperationsForStop();
    PurgeQueuedEventsForStop();
    PurgeRetiredRelaysIfSafe();
    LifecycleState.store(TqDarwinWorkerLifecycle::Stopped,
        std::memory_order_release);
}
```

`PurgeRetiredRelaysIfSafe()` 的删除条件必须完整：

```cpp
const bool purgeable = relay->InFlightQuicSends == 0 &&
    relay->PendingQuicReceives.empty() &&
    relay->Binding->CallbackRefs.load(std::memory_order_acquire) == 0 &&
    relay->Binding->Completions->KnownSendOperationCount.load(
        std::memory_order_acquire) == 0 &&
    relay->LocalOperationOwnershipTransferredOrDrained.load(
        std::memory_order_acquire);
```

不得把 `TerminalObserved` 作为 tunnel context 删除前置条件；handoff 完成即可删除 tunnel，owner/sink/watchdog 独立等待 callback。

- [ ] **Step 6: 运行 tests**

Run: `rtk cmake --build build --target tcpquic_darwin_relay_worker_io_test tcpquic_tunnel_reaper_test tcpquic_tcp_tunnel_test -j$(sysctl -n hw.ncpu) && rtk build/bin/Release/tcpquic_darwin_relay_worker_io_test && rtk build/bin/Release/tcpquic_tunnel_reaper_test && rtk build/bin/Release/tcpquic_tcp_tunnel_test`

Expected: 退出 0；reaper truth table 仅 `111` 为 true；Stop 后 worker-only handler 调用为 0，retired/send/receive/sink 全归零。

- [ ] **Step 7: 提交**

```bash
rtk git add src/tunnel/darwin_relay_worker.cpp src/tunnel/tunnel_reaper.h src/tunnel/tunnel_reaper.cpp src/tunnel/tcp_tunnel.cpp src/unittest/darwin_relay_worker_io_test.cpp src/unittest/tunnel_reaper_test.cpp
rtk git commit -m "fix(darwin): gate tunnel reaping on terminal ownership transfer"
```

### Task 6: 接通 generation-safe connection escalation

**Files:**
- Modify: `src/tunnel/tcp_tunnel.cpp`
- Modify: `src/tunnel/darwin_relay_worker.h`
- Modify: `src/tunnel/darwin_relay_worker.cpp`
- Test: `src/unittest/tcp_tunnel_test.cpp`
- Test: `src/unittest/darwin_relay_worker_io_test.cpp`

**Interfaces:**
- Consumes: frozen `TqTerminalEscalation::RequestConnectionShutdown(uint64_t connectionId, uint64_t streamId, QUIC_STATUS streamStatus, uint64_t errorCode) noexcept`; common runtime escalation token carrying stable connection id and generation.
- Produces: Darwin propagation of the exact same escalation token from tunnel to sink/watchdog; no Darwin connection pointer cache.

- [ ] **Step 1: 写旧 generation 隔离失败测试**

```cpp
void OldWatchdogCannotShutdownReusedConnectionSlot() {
    FakeConnectionRuntime runtime;
    auto oldEscalation = runtime.MakeEscalation(/*id=*/2, /*generation=*/7);
    auto newConnection = runtime.ReplaceSlot(/*id=*/2, /*generation=*/8);
    oldEscalation->RequestConnectionShutdown(2, 17, QUIC_STATUS_TIMEOUT, 0x54510002);
    CHECK(runtime.ShutdownCount(2, 7) == 0);
    CHECK(runtime.GenerationMismatchCount() == 1);
    CHECK(newConnection->IsOpen());
}
```

- [ ] **Step 2: 写 Darwin watchdog propagation 测试**

```cpp
void DarwinHandoffEscalatesOnceAfterWatchdogDeadline() {
    ManualClock clock;
    ManagedRelayHarness relay(clock, /*connectionId=*/2, /*generation=*/9);
    relay.InjectTcpReset();
    clock.Advance(std::chrono::seconds(5));
    relay.RunWatchdogDue();
    relay.RunWatchdogDue();
    CHECK(relay.EscalationCalls().size() == 1);
    CHECK(relay.EscalationCalls()[0].ConnectionId == 2);
    CHECK(relay.EscalationCalls()[0].StreamId == 17);
    CHECK(relay.StreamCloseCalls() == 0);
}
```

- [ ] **Step 3: 运行并确认 token/generation 未传递而失败**

Run: `rtk cmake --build build --target tcpquic_tcp_tunnel_test tcpquic_darwin_relay_worker_io_test -j$(sysctl -n hw.ncpu)`

Expected: FAIL；Darwin registration/sink 缺少 connection generation，或 watchdog 无法定位同一 escalation token。

- [ ] **Step 4: 在 tunnel 创建一次 escalation token并只传 shared interface**

```cpp
TqRelayTerminalContext terminalContext{
    .Ledger = nullptr,
    .Sink = nullptr,
    .Escalation = nullptr,
};
const TqTerminalIdentity identity{
    .StreamId = streamId,
    .TunnelId = TraceTunnelId,
    .ConnectionId = connectionId,
    .ConnectionGeneration = connectionGeneration,
    .Role = role,
    .Backend = TqRelayBackendType::DarwinWorker,
};
streamOwner->BindTerminalIdentity(identity, tuning.TerminalWatchdogSeconds);
terminalContext.Ledger = streamOwner->TerminalLedger();
terminalContext.Escalation = connectionRuntime->MakeTerminalEscalation(
    connectionId,
    connectionGeneration);
terminalContext.Sink = TqTerminalSink::Create(
    streamOwner,
    terminalContext.Ledger);
if (terminalContext.Ledger == nullptr || terminalContext.Sink == nullptr ||
    terminalContext.Escalation == nullptr) {
    return false;
}
```

Darwin worker 不得调用 connection runtime、不保存 `MsQuicConnection*`；它只把该 shared token 传给 `BeginTerminalShutdown()`。

- [ ] **Step 5: 覆盖 retry exhaustion、connection closing、30 秒 timeout**

```cpp
CHECK(RunShutdownFailures({
    QUIC_STATUS_OUT_OF_MEMORY,
    QUIC_STATUS_OUT_OF_MEMORY,
    QUIC_STATUS_OUT_OF_MEMORY}).RetryDelays ==
    std::vector<std::chrono::milliseconds>{10ms, 50ms, 250ms});
CHECK(runtime.EscalationCount() == 1);
clock.Advance(30s);
CHECK(ledger.TerminalTimeout);
CHECK(ledger.ReleaseBlocked);
CHECK(streamCloseCount == 0);
```

- [ ] **Step 6: 运行 tests**

Run: `rtk cmake --build build --target tcpquic_stream_lifetime_test tcpquic_tcp_tunnel_test tcpquic_darwin_relay_worker_io_test -j$(sysctl -n hw.ncpu) && rtk build/bin/Release/tcpquic_stream_lifetime_test && rtk build/bin/Release/tcpquic_tcp_tunnel_test && rtk build/bin/Release/tcpquic_darwin_relay_worker_io_test`

Expected: 退出 0；同一 watchdog 最多升级一次；旧 generation mismatch 不关闭新 connection；30 秒 timeout 保留诊断且 close 次数为 0。

- [ ] **Step 7: 提交**

```bash
rtk git add src/tunnel/tcp_tunnel.cpp src/tunnel/darwin_relay_worker.h src/tunnel/darwin_relay_worker.cpp src/unittest/tcp_tunnel_test.cpp src/unittest/darwin_relay_worker_io_test.cpp
rtk git commit -m "feat(darwin): propagate generation-safe terminal escalation"
```

### Task 7: 完成 Darwin Admin/metrics 并修复 precommit snapshot 数据竞争

**Files:**
- Modify: `src/tunnel/darwin_relay_worker.h`
- Modify: `src/tunnel/darwin_relay_worker.cpp`
- Modify: `src/runtime/relay_metrics.h`
- Modify: `src/runtime/relay_metrics.cpp`
- Modify: `src/runtime/admin_http.cpp`
- Modify: `src/runtime/router_runtime.cpp`
- Test: `src/unittest/darwin_relay_metrics_test.cpp`
- Test: `src/unittest/router_runtime_test.cpp`
- Test: `src/unittest/admin_http_test.cpp`

**Interfaces:**
- Consumes: common `TqTerminalRetentionDetailSnapshot` and common filter fields `backend`, `connection_id`, `tunnel_id`, `terminal_phase`.
- Produces: Darwin worker counters `TerminalHandoffStarted/Completed/Failed`, `TerminalSinkPending`, `WatchdogArmed/Canceled/Timeout`, `ConnectionEscalation`, `TerminalTimeoutPending`, `DuplicateTerminalSuppressed`, `AccountingExactlyOnceViolation`, `DestructorExactlyOnceViolation`, `StreamCloseExactlyOnceViolation`; `/api/v1/relay/terminal-retentions` JSON.

- [ ] **Step 1: 写 precommit snapshot 并发失败测试**

```cpp
void SnapshotDuringPublishEntryReceiveIsConsistent() {
    PublishReceiveSnapshotBarrier barrier;
    barrier.StartReceiveUnderActivationMutex();
    auto future = std::async(std::launch::async, [&] {
        return barrier.Worker().Snapshot();
    });
    barrier.ReleaseReceive();
    const auto snapshot = future.get();
    CHECK(snapshot.PrecommitDepth == 0 || snapshot.PrecommitDepth == 1);
    CHECK(snapshot.PrecommitBytes == 0 || snapshot.PrecommitBytes == 8);
    CHECK((snapshot.PrecommitDepth == 0) == (snapshot.PrecommitBytes == 0));
}
```

- [ ] **Step 2: 写 JSON contract 失败测试**

```cpp
const std::string json = TqRelayMetricsToJson(metrics);
CheckContains(json, "\"darwin_terminal_handoff_completed\":3");
CheckContains(json, "\"darwin_terminal_sink_pending\":0");
CheckContains(json, "\"darwin_terminal_watchdog_timeout\":1");
CheckContains(json, "\"darwin_connection_escalation\":1");
CheckContains(json, "\"darwin_terminal_timeout_pending\":1");
CheckContains(json, "\"terminal_phase\":\"shutdown_submitted\"");
CheckContains(json, "\"backend\":\"darwin\"");
```

- [ ] **Step 3: 运行并确认 P2-12 与缺失字段**

Run: `rtk cmake --build build --target tcpquic_darwin_relay_metrics_test tcpquic_router_runtime_test tcpquic_admin_http_test -j$(sysctl -n hw.ncpu)`

Expected: FAIL；缺少 Darwin terminal 字段或并发 snapshot 在未锁 `ActivationMutex` 时触发 TSAN/断言。

- [ ] **Step 4: 在固定锁顺序下复制 precommit 值**

```cpp
uint64_t precommitBytes = 0;
uint64_t precommitDepth = 0;
if (relay->Binding != nullptr) {
    std::lock_guard<std::mutex> activation(relay->Binding->ActivationMutex);
    precommitBytes = relay->Binding->PrecommitPendingBytes;
    precommitDepth = relay->Binding->PrecommitReceives.size();
}
snapshot.PrecommitBytes += precommitBytes;
snapshot.PrecommitDepth += precommitDepth;
```

锁顺序固定为先复制 `Relays` 的 `shared_ptr` 列表并释放 `RelayMutex`，再逐 binding 获取 `ActivationMutex`；禁止持 `RelayMutex` 等 callback activation，以免反向锁序。

- [ ] **Step 5: 增加字段并保持 sum/max 语义**

```cpp
struct TqDarwinRelayWorkerSnapshot {
    uint64_t TerminalHandoffStarted{0};
    uint64_t TerminalHandoffCompleted{0};
    uint64_t TerminalHandoffFailed{0};
    uint64_t TerminalSinkPending{0};
    uint64_t WatchdogArmed{0};
    uint64_t WatchdogCanceled{0};
    uint64_t WatchdogTimeout{0};
    uint64_t ConnectionEscalation{0};
    uint64_t TerminalTimeoutPending{0};
    uint64_t DuplicateTerminalSuppressed{0};
    uint64_t AccountingExactlyOnceViolation{0};
    uint64_t DestructorExactlyOnceViolation{0};
    uint64_t StreamCloseExactlyOnceViolation{0};
};
```

事件 counters 求和；全局 retained count/oldest age、sink/watchdog/timeout pending gauges 取 max，避免多 worker 重复计算公共 registry。
Admin health 以 retained oldest age `>5000 ms` 标记 `warning`、`>30000 ms` 标记 `critical`；任一 `TerminalTimeoutPending` 或 exactly-once violation 立即标记 `release_blocked=true`。

- [ ] **Step 6: 实现 retention detail endpoint 与过滤**

```json
{
  "stream_id": 17,
  "tunnel_id": 133,
  "connection_id": 2,
  "role": "client",
  "backend": "darwin",
  "terminal_phase": "shutdown_submitted",
  "retained_age_ms": 428,
  "shutdown_intent": "abort_both_immediate",
  "shutdown_status": "pending",
  "shutdown_attempt": 1,
  "watchdog_state": "armed",
  "connection_escalated": false
}
```

支持 `?backend=darwin&connection_id=2&tunnel_id=133&terminal_phase=shutdown_submitted`；Admin 线程只复制 identity/ledger，不解引用 relay/tunnel/stream/connection。

- [ ] **Step 7: 运行 metrics/Admin tests**

Run: `rtk cmake --build build --target tcpquic_darwin_relay_metrics_test tcpquic_router_runtime_test tcpquic_admin_http_test -j$(sysctl -n hw.ncpu) && rtk build/bin/Release/tcpquic_darwin_relay_metrics_test && rtk build/bin/Release/tcpquic_router_runtime_test && rtk build/bin/Release/tcpquic_admin_http_test`

Expected: 退出 0；filter 精确返回一条 Darwin retention；并发 snapshot 字段成对一致；aggregate 不重复计算公共 gauges。

- [ ] **Step 8: 提交**

```bash
rtk git add src/tunnel/darwin_relay_worker.h src/tunnel/darwin_relay_worker.cpp src/runtime/relay_metrics.h src/runtime/relay_metrics.cpp src/runtime/admin_http.cpp src/runtime/router_runtime.cpp src/unittest/darwin_relay_metrics_test.cpp src/unittest/router_runtime_test.cpp src/unittest/admin_http_test.cpp
rtk git commit -m "feat(darwin): expose terminal convergence diagnostics"
```

### Task 8: 固化精确事故回归、fault injection 与 Darwin 发布门禁

**Files:**
- Modify: `src/tunnel/darwin_relay_worker.h`
- Modify: `src/unittest/darwin_relay_worker_io_test.cpp`
- Modify: `src/unittest/tcp_tunnel_test.cpp`
- Modify: `src/CMakeLists.txt`
- Create: `tests/k6/macos-terminal-convergence.js`
- Create: `scripts/run-macos-terminal-convergence.sh`
- Modify: `docs/test/macos-performance-stress-test-design_cn.md`
- Modify: `docs/relay_macos.md`

**Interfaces:**
- Consumes: Tasks 1–7 complete Darwin path and existing test-only hooks.
- Produces: named deterministic cases `target-rst-after-client-fin`, `shutdown-status-matrix`, `terminal-reentrant-downcall`, `queue-full-after-tunnel-release`, `fd-connection-generation-reuse`; `summary.json`, before/after Admin snapshots, process resource samples, ASan logs and a reproducible Darwin release evidence directory.

- [ ] **Step 1: 增加精确事故回归**

```cpp
void ClientFinThenTargetResetConvergesBothSides() {
    CrossProxyTerminalHarness h;
    h.ClientTcpFin();
    CHECK(h.ClientQuicFinCount() == 1);
    h.ServerReceivesPeerFin();
    CHECK(h.TargetShutdownWriteCount() == 1);
    h.TargetReset(ECONNRESET);
    CHECK(h.ServerAbortImmediateCount() == 1);
    h.ReleaseServerTunnelAfterHandoff();
    h.DeliverServerShutdownComplete();
    h.DeliverClientPeerAbortAndShutdownComplete();
    CHECK(h.ActiveTunnels() == 0);
    CHECK(h.ActiveRelays() == 0);
    CHECK(h.TerminalSinkPending() == 0);
    CHECK(h.WatchdogPending() == 0);
    CHECK(h.SendCompletionPending() == 0);
    CHECK(h.RetainedOwners() == h.RetentionBaseline());
    CHECK(h.StreamCloseCountEachSide() == 1);
    CHECK(h.CloseOccurredAfterCallbackReturn());
}
```

- [ ] **Step 2: 增加完整故障矩阵**

```cpp
const std::vector<FaultCase> cases{
    {"shutdown-success", QUIC_STATUS_SUCCESS, false, false},
    {"shutdown-pending", QUIC_STATUS_PENDING, false, false},
    {"shutdown-sync-failure", QUIC_STATUS_OUT_OF_MEMORY, false, false},
    {"terminal-reentrant-downcall", QUIC_STATUS_PENDING, true, false},
    {"queue-full-after-tunnel-release", QUIC_STATUS_PENDING, false, true},
};
for (const auto& test : cases) {
    RunTerminalFaultCase(test).AssertExactlyOnceAndBaseline();
}
```

- [ ] **Step 3: 增加 fd/watchdog/connection generation reuse 测试**

```cpp
void StaleKqueueWatchdogAndConnectionGenerationCannotHitNewTunnel() {
    ReuseHarness h;
    const auto old = h.Create(/*fd=*/41, /*relayGen=*/7, /*connGen=*/11);
    h.Retire(old);
    const auto fresh = h.CreateReusingFdAndSlot(41, 8, 12);
    h.DeliverOldKqueueEvent(old);
    h.FireOldWatchdog(old);
    CHECK(fresh.Active());
    CHECK(fresh.FdCloseCount() == 0);
    CHECK(fresh.ConnectionShutdownCount() == 0);
    CHECK(h.GenerationMismatchCount() == 2);
}
```

- [ ] **Step 4: 用 production linkage guard 排除 test hook symbol**

```cpp
const std::vector<std::string_view> forbiddenDarwinTestingSymbols{
    "AfterTerminalReserveHookForTest",
    "BeforeTerminalDowncallHookForTest",
    "SuppressTerminalCallbackForTest",
    "BeforeWorkerExitHookForTest",
};
for (const auto symbol : forbiddenDarwinTestingSymbols) {
    CHECK(productionSymbols.find(symbol) == productionSymbols.end());
}
```

- [ ] **Step 5: 运行全量 focused regression**

Run: `rtk cmake --build build --target tcpquic_stream_lifetime_test tcpquic_darwin_relay_worker_io_test tcpquic_darwin_relay_worker_queue_test tcpquic_darwin_relay_metrics_test tcpquic_darwin_reactor_test tcpquic_tcp_tunnel_test tcpquic_tunnel_reaper_test tcpquic_client_tunnel_open_test tcpquic_router_runtime_test tcpquic_admin_http_test tcpquic_production_linkage_guard_test -j$(sysctl -n hw.ncpu)`

Run: `for t in tcpquic_stream_lifetime_test tcpquic_darwin_relay_worker_io_test tcpquic_darwin_relay_worker_queue_test tcpquic_darwin_relay_metrics_test tcpquic_darwin_reactor_test tcpquic_tcp_tunnel_test tcpquic_tunnel_reaper_test tcpquic_client_tunnel_open_test tcpquic_router_runtime_test tcpquic_admin_http_test tcpquic_production_linkage_guard_test; do rtk build/bin/Release/$t || exit 1; done`

Expected: 所有 target 退出 0；事故回归两端归零；无 terminal timeout；FIN 对照仍能反向传输；reuse case 不影响新 tunnel。

- [ ] **Step 6: 创建 k6 workload**

```javascript
import http from 'k6/http';
import { check } from 'k6';
import { Counter, Trend } from 'k6/metrics';

const errors = new Counter('terminal_workload_errors');
const requestDuration = new Trend('terminal_request_duration', true);

const profiles = {
  baseline: { executor: 'constant-vus', vus: 100, duration: '5m' },
  peak: { executor: 'constant-arrival-rate', rate: 50, timeUnit: '1s', duration: '10m', preAllocatedVUs: 100, maxVUs: 500 },
  spike: { executor: 'ramping-vus', stages: [{ duration: '30s', target: 1000 }, { duration: '2m', target: 1000 }, { duration: '30s', target: 0 }] },
  stress: {
    executor: 'ramping-vus',
    startVUs: 100,
    stages: Array.from({ length: 16 }, (_, index) => ({
      duration: '2m',
      target: 350 + index * 250,
    })),
    gracefulRampDown: '30s',
  },
  soak: { executor: 'constant-arrival-rate', rate: 100, timeUnit: '1s', duration: '30m', preAllocatedVUs: 100, maxVUs: 500 },
};

export const options = {
  scenarios: { terminal: profiles[__ENV.PROFILE || 'baseline'] },
  thresholds: {
    terminal_workload_errors: [{ threshold: 'count==0', abortOnFail: true, delayAbortEval: '30s' }],
    http_req_failed: [{ threshold: 'rate<0.001', abortOnFail: true, delayAbortEval: '30s' }],
    dropped_iterations: [{ threshold: 'count==0', abortOnFail: true, delayAbortEval: '30s' }],
  },
};

export default function () {
  const size = __ITER % 10 === 0 ? 1048576 : (__ITER % 2 === 0 ? 65536 : 1024);
  const response = http.post(`${__ENV.ORIGIN_URL}/terminal-churn?size=${size}&rst=${__ITER % 5 === 0 ? 1 : 0}`, 'x'.repeat(size), {
    timeout: '10s',
    tags: { profile: __ENV.PROFILE || 'baseline' },
  });
  requestDuration.add(response.timings.duration);
  if (!check(response, { 'status accepted': (r) => r.status === 200 || r.status === 204 })) errors.add(1);
}
```

通过环境变量 `HTTPS_PROXY` 指向本机 tcpquic HTTP CONNECT ingress；origin fixture 根据 `rst=1` 在读完请求后发送 RST，其余请求正常 FIN。

- [ ] **Step 7: 创建证据采集脚本**

```bash
#!/usr/bin/env bash
set -euo pipefail

profile="${1:-baseline}"
stamp="$(date -u +%Y%m%d-%H%M%S)"
out="artifacts/macos-terminal-convergence/${stamp}-${profile}"
mkdir -p "$out"
sw_vers >"$out/sw_vers.txt"
uname -a >"$out/uname.txt"
git rev-parse HEAD >"$out/git-sha.txt"
curl -fsS -H "Authorization: Bearer ${ADMIN_TOKEN}" "${ADMIN_URL}/api/v1/relay/metrics" >"$out/metrics-before.json"
curl -fsS -H "Authorization: Bearer ${ADMIN_TOKEN}" "${ADMIN_URL}/api/v1/relay/terminal-retentions?backend=darwin" >"$out/retentions-before.json"
PROFILE="$profile" HTTPS_PROXY="$PROXY_URL" ORIGIN_URL="$ORIGIN_URL" \
  k6 run --summary-export "$out/summary.json" tests/k6/macos-terminal-convergence.js 2>&1 | tee "$out/k6.log"
sleep 30
curl -fsS -H "Authorization: Bearer ${ADMIN_TOKEN}" "${ADMIN_URL}/api/v1/relay/metrics" >"$out/metrics-after.json"
curl -fsS -H "Authorization: Bearer ${ADMIN_TOKEN}" "${ADMIN_URL}/api/v1/relay/terminal-retentions?backend=darwin" >"$out/retentions-after.json"
jq -e '.data | length == 0' "$out/retentions-after.json"
jq -e '.terminal_timeout_pending == 0 and .terminal_sink_pending == 0 and .terminal_watchdog_pending == 0' "$out/metrics-after.json"
```

Run: `rtk chmod +x scripts/run-macos-terminal-convergence.sh`

Expected: `rtk proxy test -x scripts/run-macos-terminal-convergence.sh` 退出 0；git 记录 executable mode，后续 `rtk scripts/run-macos-terminal-convergence.sh <profile>` 可直接执行。

- [ ] **Step 8: 配置并运行 macOS ASan**

Run: `rtk cmake -S . -B build-asan-darwin -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo -DCMAKE_CXX_FLAGS="-fsanitize=address -fno-omit-frame-pointer" -DCMAKE_EXE_LINKER_FLAGS="-fsanitize=address"`

Run: `rtk cmake --build build-asan-darwin --target tcpquic_darwin_relay_worker_io_test tcpquic_darwin_relay_worker_queue_test tcpquic_darwin_relay_metrics_test tcpquic_tcp_tunnel_test -j$(sysctl -n hw.ncpu)`

Run: `ASAN_OPTIONS=abort_on_error=1:detect_leaks=0 rtk build-asan-darwin/bin/Release/tcpquic_darwin_relay_worker_io_test && ASAN_OPTIONS=abort_on_error=1:detect_leaks=0 rtk build-asan-darwin/bin/Release/tcpquic_darwin_relay_worker_queue_test && ASAN_OPTIONS=abort_on_error=1:detect_leaks=0 rtk build-asan-darwin/bin/Release/tcpquic_darwin_relay_metrics_test && ASAN_OPTIONS=abort_on_error=1:detect_leaks=0 rtk build-asan-darwin/bin/Release/tcpquic_tcp_tunnel_test`

Expected: 全部退出 0；ASan 无 UAF/double-free/heap-buffer-overflow；逻辑泄漏由 registry/metrics 归零证明，不启用 macOS 不稳定的 leak detector。

- [ ] **Step 9: 运行 baseline、peak、spike、stress 和 soak**

Run: `rtk scripts/run-macos-terminal-convergence.sh baseline`

Run: `rtk scripts/run-macos-terminal-convergence.sh peak`

Run: `rtk scripts/run-macos-terminal-convergence.sh spike`

Run: `rtk scripts/run-macos-terminal-convergence.sh stress`

Run: `rtk scripts/run-macos-terminal-convergence.sh soak`

Expected: baseline/peak/spike/soak 的 error rate `<0.1%`、dropped iterations `0`；stress 从 100 VU 起每 2 分钟增加 250 VU，并在首个 threshold 失败时立即停止并保留该级证据，若 4100 VU 前无失败则完整结束；fatal terminal p95 `<1s`、p99 `<5s`；结束 30 秒后 retained/sink/watchdog/send/retired 全回基线；30 分钟 retained count 不单调增长。

- [ ] **Step 10: 运行 capacity 梯度并比较旧基线**

Run: `for vus in 100 1024 4096; do ACTIVE_TUNNELS=$vus rtk scripts/run-macos-terminal-convergence.sh peak; done`

Run: `for workers in 1 $(($(sysctl -n hw.ncpu)/2)) $(sysctl -n hw.ncpu); do RELAY_WORKERS=$workers rtk scripts/run-macos-terminal-convergence.sh baseline; done`

Expected: p95 回归 `<=5%`、p99 回归 `<=10%`、吞吐回归 `<=5%`、CPU/RSS 增幅各 `<=10%`、常驻线程增量 `0`；每个梯度结束后资源归零。

- [ ] **Step 11: 更新文档并执行静态发布扫描**

Run: `rtk rg -n "PublicHandle|RetiredStream|ReceiveCompleteStream|MsQuicConnection\*|MsQuicStream\*" src/tunnel/darwin_relay_worker.cpp src/tunnel/darwin_relay_worker.h src/tunnel/darwin_relay_event_queue.h`

Expected: 仅允许 callback 参数、初始化兼容入口和明确测试代码中的 `MsQuicStream*`；不存在异步 `PublicHandle`、`RetiredStream`、pending receive raw capability 或 connection raw pointer。

Run: `rtk git diff --check`

Expected: 无输出且退出 0。

- [ ] **Step 12: 提交**

```bash
rtk git add src/tunnel/darwin_relay_worker.h src/unittest/darwin_relay_worker_io_test.cpp src/unittest/tcp_tunnel_test.cpp src/CMakeLists.txt tests/k6/macos-terminal-convergence.js scripts/run-macos-terminal-convergence.sh docs/test/macos-performance-stress-test-design_cn.md docs/relay_macos.md
rtk git commit -m "test(darwin): add terminal convergence release gates"
```

## Final Verification

- [ ] `rtk cmake --build build --target tcpquic_stream_lifetime_test tcpquic_darwin_relay_worker_io_test tcpquic_darwin_relay_worker_queue_test tcpquic_darwin_relay_metrics_test tcpquic_darwin_reactor_test tcpquic_tcp_tunnel_test tcpquic_tunnel_reaper_test tcpquic_client_tunnel_open_test tcpquic_router_runtime_test tcpquic_admin_http_test tcpquic_production_linkage_guard_test -j$(sysctl -n hw.ncpu)` 全部成功。
- [ ] 精确事故回归证明 client FIN 后 target RST 会触发 server `ABORT|IMMEDIATE`，两端 terminal、accounting、析构、`StreamClose` 各一次且全部资源回到基线。
- [ ] 正常 FIN half-close 对照证明反向数据继续传输，fatal watchdog/escalation 次数均为 0。
- [ ] transaction/Stop 四个 barrier、queue-full、late completion、worker-exited purge、fd reuse、旧 connection generation 全部确定性通过。
- [ ] Admin 可按 backend/connection/tunnel/phase 定位 retained owner；任一 `terminal_timeout`、exactly-once violation 或 oldest age 超过 30 秒都阻断发布。
- [ ] ASan、baseline、peak、spike、独立 stress、capacity、30 分钟 soak 均满足 Global Constraints 的量化门禁；capacity 梯度证据不得替代 stress 结果。
- [ ] `rtk git diff --check` 无输出；`rtk git status --short` 只包含本计划授权的实现文件和证据文件。
