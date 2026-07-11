# Linux Stream Terminal 有界收敛 Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 在 Linux/epoll 首次落地跨平台公共 stream terminal ledger、独立 sink、重试/watchdog、generation-safe connection escalation、retention/Admin 明细和 reaper handoff，使 fatal stream 有界收敛，同时完整保留正常 FIN half-close。

**Architecture:** `TqStreamLifetime` 继续唯一拥有 `CleanUpManual` wrapper，并新增独立于既有 start phase 和方向性 shutdown ledger 的 terminal phase；fatal tunnel 先冻结数据面、把 stable router 切到 worker-independent `TqTerminalSink`，再通过 `BeginTerminalShutdown()` 在锁外提交 `ABORT | IMMEDIATE`。公共单线程 `TqTerminalScheduler` 管理 10/50/250 ms 重试、5 秒 watchdog 和 escalation 后 30 秒 timeout；Linux relay/reaper 只依赖 shared control/ledger，连接升级通过 id+generation registry 定位当前 connection，绝不保存 connection、worker、relay、tunnel 或 stream 裸指针。

**Tech Stack:** C++17、MsQuic C++ wrapper、Linux epoll、`std::shared_ptr`/`std::weak_ptr`、`std::mutex`/`std::condition_variable`、monotonic `steady_clock`、CMake、现有自包含 C++ test executables、nlohmann/json、ASan、k6/xk6 TCP。

## Global Constraints

- 文档和新增用户可见诊断默认使用中文；代码标识符、JSON key、metrics key 保持英文 snake_case/现有 C++ 风格。
- 正常 TCP EOF 只提交一次 QUIC FIN，QUIC FIN/`PEER_SEND_SHUTDOWN` 排空后只 `SHUT_WR`；half-close 不创建 terminal sink、不 arm fatal watchdog、不触发 connection escalation。
- fatal 最终释放一律使用 `QUIC_STREAM_SHUTDOWN_FLAG_ABORT | QUIC_STREAM_SHUTDOWN_FLAG_IMMEDIATE`；`SUCCESS` 与 `PENDING` 都视为提交成功。
- shutdown 同步失败最多重试 3 次，退避固定为 10 ms、50 ms、250 ms；terminal 已观察或 connection 已 closing 时取消后续重试。
- watchdog 默认 5 秒，可配置范围严格为 5–30 秒；第一次超时只执行一次 generation-safe connection shutdown/reconnect escalation；升级后 30 秒仍无 terminal 标记 `terminal_timeout` 并阻断发布，不得直接 `StreamClose`。
- `SHUTDOWN_COMPLETE` 是 `TerminalObserved` 唯一事实源；TCP closed、relay stopped、peer abort、`SEND_SHUTDOWN_COMPLETE` 均不得发布 terminal。
- `TqStreamLifetime` 继续独占 `MsQuicStream*`；`StreamClose` 只能由其析构中的一次性 `delete MsQuicStream` 间接发生。
- 每个 owner 只能由 `BindTerminalIdentity()` 创建一个 `TerminalLedger_`；所有平台 handoff 从 `owner->TerminalLedger()` 取同一对象并调用唯一 `TqTerminalSink::Create(owner, ledger)`，不得另建 ledger/sink factory。
- started owner 在真实 terminal 前必须由 terminal retention registry 强持；API failure、worker stop、watchdog timeout 和 connection escalation 均不得删除 retention。
- callback/context 启动前设置一次，之后只切 stable router target；sink、scheduler、reaper、connection controller 不得保存 tunnel、relay、worker、stream 或 connection 裸指针。
- owner control mutex 内只允许 phase、route、reservation 和 snapshot 操作；MsQuic、worker 与 connection down-call 必须在锁外执行，terminal callback 不等待 API lease。
- reaper 门禁固定为 `DataPlaneStopped && TerminalHandoffComplete && LocalOperationOwnershipTransferredOrDrained`；reaper 不调用 stream API、不取消 watchdog、不释放 terminal retention。
- stop、handoff、shutdown、retry、watchdog cancel/timeout、connection escalation、terminal accounting、wrapper destruction 和 stream close 均幂等且 exactly-once；violation counter 非零阻断发布。
- 公共接口不得出现 Linux 条件编译语义分叉；Linux 只提供 freeze、epoll ownership drain 和 connection runtime adapter。
- 不改变 tunnel wire protocol、认证、压缩或 payload framing，不新增每 stream 线程，不以进程重启掩盖 stuck retention。
- 每个确定性竞态测试使用 barrier/latch 或可控 fake clock，不以 sleep 猜测时序。
- 实施时保留工作区现有改动；每次提交只暂存本 Task 列出的文件。

---

## File Structure

- Create: `src/tunnel/terminal_convergence.h` — 公共 terminal identity/phase/ledger/sink/scheduler/escalation/retention snapshot 的唯一类型定义。
- Create: `src/tunnel/terminal_convergence.cpp` — ledger、sink、单线程 scheduler、retention detail registry、聚合 metrics 与测试 fake-clock 驱动实现。
- Create: `src/unittest/terminal_convergence_test.cpp` — 不依赖平台 worker 的 ledger、sink、retry/watchdog、timeout、无强环与 snapshot 单元测试。
- Create: `src/unittest/linux_terminal_convergence_test.cpp` — epoll stale generation、fatal/reaper 竞争、queue-full fallback、half-close 对照和事故时序的 Linux 集成测试。
- Create: `docs/test/k6/linux-terminal-convergence.js` — baseline/peak/spike/stress/soak 统一 workload 与 thresholds。
- Create: `scripts/run-linux-terminal-convergence.sh` — 启动 target RST fixture、两端 proxy、故障注入、Admin/ss/metrics 取证和 k6 场景编排。
- Modify: `src/tunnel/stream_lifetime.h` — 冻结 `AbortBothImmediate`、`TqTerminalShutdownResult`、`BeginTerminalShutdown()` 与 terminal ledger 接口。
- Modify: `src/tunnel/stream_lifetime.cpp` — terminal reservation/down-call/terminal-wins、sink dispatch、retention detail 和唯一 close 次序。
- Modify: `src/tunnel/relay.h` — 公共 backend alias 与 handoff/reaper shared control facts。
- Modify: `src/tunnel/linux_relay_worker.h` — Linux terminal handoff API、snapshot counters 和 test hooks。
- Modify: `src/tunnel/linux_relay_worker.cpp` — fatal/worker stop/queue-full 统一 handoff，epoll generation 隔离和 local operation transfer。
- Modify: `src/tunnel/tcp_tunnel.h` — 公共 `TqTunnelRole` 与三事实 reaper predicate 声明。
- Modify: `src/tunnel/tcp_tunnel.cpp` — identity 绑定、tunnel handoff、reaper 门禁和正常 FIN 保护。
- Modify: `src/tunnel/tunnel_reaper.cpp` — 从单一 relay stop 改为公共 handoff predicate。
- Modify: `src/protocol/quic_session.h` — client/server generation-safe terminal escalation 注册与请求接口。
- Modify: `src/protocol/quic_session.cpp` — slot/accepted connection controller registry、旧 generation 拒绝和 once-only shutdown。
- Modify: `src/config/config.h`、`src/config/config.cpp`、`src/config/tuning.h`、`src/config/tuning.cpp` — 5–30 秒 watchdog 配置与默认值/序列化。
- Modify: `src/runtime/relay_metrics.h`、`src/runtime/relay_metrics.cpp` — 公共 terminal 聚合 metrics 和 retention JSON serializer。
- Modify: `src/runtime/server_admin.cpp`、`src/runtime/router_runtime.cpp` — `/relay/terminal-retentions` 列表/过滤接口。
- Modify: `src/unittest/stream_lifetime_test.cpp` — owner terminal phase、immediate flags、down-call 重入与 destructor 次序回归。
- Modify: `src/unittest/linux_relay_worker_io_test.cpp` — fatal handoff、late epoll、queue full、worker stop、正常 FIN 对照。
- Modify: `src/unittest/tcp_tunnel_test.cpp` — tunnel identity、handoff/reaper 三门禁和 target RST 事故回归。
- Modify: `src/unittest/tunnel_reaper_test.cpp` — predicate 三事实组合测试。
- Modify: `src/unittest/quic_session_reconnect_test.cpp` — connection slot generation 复用隔离。
- Modify: `src/unittest/server_admin_test.cpp`、`src/unittest/router_runtime_test.cpp` — retention JSON schema 与过滤测试。
- Modify: `src/unittest/tuning_test.cpp`、`src/unittest/config_router_test.cpp` — watchdog 默认值、边界和 round-trip。
- Modify: `src/CMakeLists.txt` — 新公共源、新 focused targets 和所有受影响 test source list。

## Frozen Public Interfaces

Linux Task 1 合并后冻结以下跨平台契约；Windows/macOS 只能调用，不能另建同义 ledger、sink factory 或平台私有 terminal phase：

```cpp
// TqStreamLifetime public members
static std::shared_ptr<TqStreamLifetime> OpenOutgoing(
    const MsQuicConnection& connection,
    QUIC_STREAM_OPEN_FLAGS flags,
    std::shared_ptr<TqStreamLifetime::Target> initialTarget,
    TqTerminalIdentity identity,
    uint32_t watchdogSeconds) noexcept;
static std::shared_ptr<TqStreamLifetime> AdoptAccepted(
    HQUIC rawStream,
    std::shared_ptr<TqStreamLifetime::Target> initialTarget,
    TqTerminalIdentity identity,
    uint32_t watchdogSeconds) noexcept;
void BindTerminalIdentity(
    TqTerminalIdentity identity,
    uint32_t watchdogSeconds = 5) noexcept;
std::shared_ptr<TqTerminalLedger> TerminalLedger() const noexcept;
TqTerminalShutdownResult BeginTerminalShutdown(
    uint64_t errorCode,
    std::shared_ptr<TqStreamLifetime::Target> terminalSink,
    std::shared_ptr<TqTerminalEscalation> escalation) noexcept;

class TqTerminalSink final : public TqStreamLifetime::Target {
public:
    static std::shared_ptr<TqTerminalSink> Create(
        std::weak_ptr<TqStreamLifetime> owner,
        std::shared_ptr<TqTerminalLedger> ledger) noexcept;
};
```

`BindTerminalIdentity()` 是 owner 的 ledger 唯一创建点：第一次调用创建并保存唯一 `TerminalLedger_`；重复传入完全相同 identity/deadline 时幂等返回；不同 identity/deadline 只增加 `ExactlyOnceViolation` 并保留原对象。所有 production owner factory 必须在 wrapper callback 可见且 `RetainUntilTerminal()` 之前完成 bind；为此 production `OpenOutgoing`/`AdoptAccepted` 增加 identity/deadline 参数并把 tunnel id、connection id/generation、role、backend 的分配提前到 factory 调用前。`TerminalLedger()` 只返回这一个 shared object；任何 platform handoff 必须执行 `auto ledger = owner->TerminalLedger();` 后调用唯一 factory `TqTerminalSink::Create(owner, ledger)`，禁止 `std::make_shared<TqTerminalLedger>` 或其它方式创建第二 ledger。

### Task 1: 冻结公共 terminal 类型与 `TqStreamLifetime` 接口

**Files:**
- Create: `src/tunnel/terminal_convergence.h`
- Modify: `src/tunnel/relay.h`
- Modify: `src/tunnel/tcp_tunnel.h`
- Modify: `src/tunnel/stream_lifetime.h`
- Test: `src/unittest/stream_lifetime_test.cpp`
- Modify: `src/CMakeLists.txt`

**Interfaces:**
- Consumes: 现有 `TqStreamLifetime::Target::OnStreamEvent(MsQuicStream*, QUIC_STREAM_EVENT*, uint64_t) noexcept`、`TqRelayBackendType`、`QUIC_STATUS`。
- Produces: `TerminalPhase`、`TqTerminalIdentity`、`TqTerminalLedger`、`TqTerminalEscalation`、`TqTerminalShutdownResult`、`ShutdownIntent::AbortBothImmediate`、`TqStreamLifetime::BindTerminalIdentity(TqTerminalIdentity, uint32_t) noexcept` 和 `TqStreamLifetime::BeginTerminalShutdown(uint64_t, std::shared_ptr<Target>, std::shared_ptr<TqTerminalEscalation>) noexcept`；后续所有 Task 必须逐字使用这些签名。

- [ ] **Step 1: 写公共接口编译失败测试**

在 `stream_lifetime_test.cpp` 增加以下完整测试，并在现有 `main()` 调用：

```cpp
void TestTerminalPublicInterfaceDefaults() {
    TqTerminalIdentity identity{};
    identity.StreamId = 17;
    identity.TunnelId = 133;
    identity.ConnectionId = 2;
    identity.ConnectionGeneration = 7;
    identity.Role = TqTunnelRole::ClientOpen;
    identity.Backend = TqRelayBackendType::LinuxWorker;
    auto owner = TqStreamLifetime::CreateForTest(
        TqStreamLifetime::Phase::CreatedNotStarted);
    owner->BindTerminalIdentity(identity, 5);
    auto ledger = owner->TerminalLedger();
    CHECK(ledger != nullptr);
    const auto snapshot = ledger->Snapshot(std::chrono::steady_clock::now());
    CHECK(snapshot.Identity.StreamId == 17);
    CHECK(snapshot.Phase == TerminalPhase::Active);
    CHECK(snapshot.ShutdownAttempt == 0);
    CHECK(snapshot.Watchdog == TqTerminalWatchdogState::Idle);
    CHECK(snapshot.LastStreamEvent == TqTerminalEvent::None);
    CHECK(!snapshot.ConnectionEscalated);
}

void TestBindTerminalIdentityCreatesExactlyOneLedger() {
    auto owner = TqStreamLifetime::CreateForTest(
        TqStreamLifetime::Phase::CreatedNotStarted);
    const TqTerminalIdentity identity{
        17, 133, 2, 7, TqTunnelRole::ClientOpen,
        TqRelayBackendType::LinuxWorker};
    owner->BindTerminalIdentity(identity, 5);
    const auto first = owner->TerminalLedger();
    owner->BindTerminalIdentity(identity, 5);
    CHECK(first != nullptr);
    CHECK(owner->TerminalLedger().get() == first.get());
}
```

- [ ] **Step 2: 运行测试确认因类型不存在而失败**

Run: `rtk cmake --build build --target tcpquic_stream_lifetime_test -j$(nproc)`

Expected: 编译失败，首个错误包含 `TqTerminalIdentity was not declared` 或 `TerminalPhase was not declared`。

- [ ] **Step 3: 创建完整公共头并公开现有 role/backend 类型**

将 `tcp_tunnel.cpp` 中现有 `TqTunnelRole` 枚举原样移到 `tcp_tunnel.h`；在 `relay.h` 保留 `TqRelayBackendType` 并增加 `using TqRelayBackend = TqRelayBackendType;`。创建 `terminal_convergence.h`，内容必须包含以下完整公共表面（字段名和类型不得在后续 Task 改写）：

```cpp
#pragma once

#include "relay.h"
#include "tcp_tunnel.h"
#include <msquic.h>

#include <chrono>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

class TqStreamLifetime;

enum class TerminalPhase : uint8_t {
    Active,
    ShutdownReserved,
    ShutdownSubmitted,
    TerminalObserved,
    Closed,
};

enum class TqTerminalWatchdogState : uint8_t {
    Idle,
    Armed,
    Canceled,
    Escalated,
    TerminalTimeout,
};

enum class TqTerminalEvent : uint8_t {
    None,
    StartComplete,
    ReceiveAfterHandoff,
    SendComplete,
    PeerSendAborted,
    PeerReceiveAborted,
    SendShutdownComplete,
    ShutdownComplete,
    CancelOnLoss,
    IdealSendBufferSize,
};

struct TqTerminalIdentity {
    uint64_t StreamId{0};
    uint64_t TunnelId{0};
    uint64_t ConnectionId{0};
    uint64_t ConnectionGeneration{0};
    TqTunnelRole Role{TqTunnelRole::ClientOpen};
    TqRelayBackend Backend{TqRelayBackendType::None};
};

struct TqTerminalLedgerSnapshot {
    TqTerminalIdentity Identity{};
    TerminalPhase Phase{TerminalPhase::Active};
    uint64_t RetainedAgeMs{0};
    uint64_t ErrorCode{0};
    QUIC_STATUS ShutdownStatus{QUIC_STATUS_SUCCESS};
    uint32_t ShutdownAttempt{0};
    uint64_t ShutdownSubmittedAtMs{0};
    uint64_t TerminalObservedAtMs{0};
    TqTerminalEvent LastStreamEvent{TqTerminalEvent::None};
    bool InTunnelRegistry{true};
    bool RelayActive{true};
    bool TcpValid{true};
    TqTerminalWatchdogState Watchdog{TqTerminalWatchdogState::Idle};
    bool ConnectionEscalated{false};
    bool AccountingCompleted{false};
};

class TqTerminalLedger final {
public:
    explicit TqTerminalLedger(TqTerminalIdentity identity) noexcept;
    TqTerminalLedgerSnapshot Snapshot(std::chrono::steady_clock::time_point now) const;
    const TqTerminalIdentity& Identity() const noexcept;
    void RecordEvent(TqTerminalEvent event) noexcept;
    void RecordShutdown(QUIC_STATUS status, uint32_t attempt, bool submitted) noexcept;
    void MarkHandoffFacts(bool inTunnelRegistry, bool relayActive, bool tcpValid) noexcept;
    bool CompleteAccountingOnce() noexcept;
private:
    friend class TqStreamLifetime;
    friend class TqTerminalScheduler;
    mutable std::mutex Mutex_;
    TqTerminalLedgerSnapshot State_{};
    std::chrono::steady_clock::time_point RetainedSince_{std::chrono::steady_clock::now()};
};

class TqTerminalEscalation {
public:
    virtual ~TqTerminalEscalation() = default;
    virtual void RequestConnectionShutdown(
        uint64_t connectionId,
        uint64_t streamId,
        QUIC_STATUS streamStatus,
        uint64_t errorCode) noexcept = 0;
};

struct TqTerminalShutdownResult {
    QUIC_STATUS Status{QUIC_STATUS_SUCCESS};
    bool Submitted{false};
    bool AlreadyTerminal{false};
    bool RetryScheduled{false};
    uint32_t Attempt{0};
};

struct TqTerminalRetentionFilter {
    TqRelayBackend Backend{TqRelayBackendType::None};
    uint64_t ConnectionId{0};
    uint64_t TunnelId{0};
    bool HasPhase{false};
    TerminalPhase Phase{TerminalPhase::Active};
};

std::vector<TqTerminalLedgerSnapshot> TqSnapshotTerminalRetentions(
    const TqTerminalRetentionFilter& filter = {});
const char* TqTerminalPhaseName(TerminalPhase phase) noexcept;
const char* TqTerminalWatchdogStateName(TqTerminalWatchdogState state) noexcept;
const char* TqTerminalEventName(TqTerminalEvent event) noexcept;
void TqRecordTerminalExactlyOnceViolation() noexcept;
uint64_t TqTerminalExactlyOnceViolationCount() noexcept;
```

并在 `stream_lifetime.h` 增加：

```cpp
#include "terminal_convergence.h"

// ShutdownIntent 最后一项
AbortBothImmediate,

static std::shared_ptr<TqStreamLifetime> OpenOutgoing(
    const MsQuicConnection& connection,
    QUIC_STREAM_OPEN_FLAGS flags,
    std::shared_ptr<TqStreamLifetime::Target> initialTarget,
    TqTerminalIdentity identity,
    uint32_t watchdogSeconds) noexcept;
static std::shared_ptr<TqStreamLifetime> AdoptAccepted(
    HQUIC rawStream,
    std::shared_ptr<TqStreamLifetime::Target> initialTarget,
    TqTerminalIdentity identity,
    uint32_t watchdogSeconds) noexcept;
void BindTerminalIdentity(
    TqTerminalIdentity identity,
    uint32_t watchdogSeconds = 5) noexcept;
std::shared_ptr<TqTerminalLedger> TerminalLedger() const noexcept;
TerminalPhase GetTerminalPhase() const noexcept;
TqTerminalShutdownResult BeginTerminalShutdown(
    uint64_t errorCode,
    std::shared_ptr<TqStreamLifetime::Target> terminalSink,
    std::shared_ptr<TqTerminalEscalation> escalation) noexcept;
```

owner private 字段一次定义为：

```cpp
std::shared_ptr<TqTerminalLedger> TerminalLedger_;
TerminalPhase TerminalPhase_{TerminalPhase::Active};
std::shared_ptr<TqTerminalEscalation> TerminalEscalation_;
uint64_t TerminalErrorCode_{0};
uint32_t TerminalShutdownAttempt_{0};
bool TerminalRetryOwned_{false};
uint32_t TerminalWatchdogSeconds_{5};
std::shared_ptr<Target> TerminalSink_;
```

从 Task 1 起，`stream_lifetime.cpp` 的 registry record 使用最终定义，不允许后续 Task 改字段布局：

```cpp
struct TerminalRetention {
    std::shared_ptr<TqStreamLifetime> Owner;
    std::shared_ptr<TqTerminalLedger> Ledger;
    std::chrono::steady_clock::time_point Since;
};
```

Task 1 同时把现有 `RetainUntilTerminal()` 的唯一插入表达式改为最终 initializer：

```cpp
g_terminalRetentions.emplace(this, TerminalRetention{
    shared_from_this(), TerminalLedger_, std::chrono::steady_clock::now()});
```

调用该表达式前必须验证 `TerminalLedger_ != nullptr`；因此 factory 执行顺序固定为 create owner → `BindTerminalIdentity` → verify `TerminalLedger()` → retain → install callback/start。不存在仅含 owner+Since 的过渡 record。

Task 2 新增 private `RetainUntilTerminalLocked()`，只允许调用方已持有 `ControlMutex_`；它再短暂取得 `g_terminalRetentionLock` 并幂等插入 registry，且插入的 `Ledger` 必须与 `TerminalLedger_` pointer-equal。terminal publication 先释放 `ControlMutex_` 再从 registry erase，因此不存在反向嵌套锁序。

- [ ] **Step 4: 将新源加入真实 target source list 并验证接口测试通过**

在 `src/CMakeLists.txt` 的 `TCPQUIC_PROXY_SOURCES`、`tcpquic_stream_lifetime_test`、Linux relay 两个 test、`TCPQUIC_TUNNEL_TEST_SOURCES`、`tcpquic_quic_session_reconnect_test`、`tcpquic_router_runtime_test` 和 `tcpquic_server_admin_test` 中加入 `tunnel/terminal_convergence.cpp`。本 Task 实现构造/snapshot/三个 enum name helper，并在 `stream_lifetime.cpp` 实现唯一 bind：

```cpp
namespace {
bool SameTerminalIdentity(
    const TqTerminalIdentity& left,
    const TqTerminalIdentity& right) noexcept {
    return left.StreamId == right.StreamId &&
           left.TunnelId == right.TunnelId &&
           left.ConnectionId == right.ConnectionId &&
           left.ConnectionGeneration == right.ConnectionGeneration &&
           left.Role == right.Role && left.Backend == right.Backend;
}
}

void TqStreamLifetime::BindTerminalIdentity(
    TqTerminalIdentity identity,
    uint32_t watchdogSeconds) noexcept {
    std::lock_guard<std::mutex> guard(ControlMutex_);
    if (TerminalLedger_ != nullptr) {
        const auto bound = TerminalLedger_->Identity();
        if (!SameTerminalIdentity(bound, identity) ||
            TerminalWatchdogSeconds_ != watchdogSeconds) {
            TqRecordTerminalExactlyOnceViolation();
        }
        return;
    }
    if (watchdogSeconds < 5 || watchdogSeconds > 30) {
        TqRecordTerminalExactlyOnceViolation();
        return;
    }
    try {
        TerminalLedger_ = std::make_shared<TqTerminalLedger>(identity);
        TerminalWatchdogSeconds_ = watchdogSeconds;
    } catch (...) {
        TerminalLedger_.reset();
    }
}

std::shared_ptr<TqTerminalLedger>
TqStreamLifetime::TerminalLedger() const noexcept {
    std::lock_guard<std::mutex> guard(ControlMutex_);
    return TerminalLedger_;
}
```

`terminal_convergence.cpp` 用单个 atomic 实现 `TqRecordTerminalExactlyOnceViolation()`/`TqTerminalExactlyOnceViolationCount()`。`TqTerminalEventName()` 必须用 exhaustive switch 返回 `none/start_complete/receive_after_handoff/send_complete/peer_send_aborted/peer_receive_aborted/send_shutdown_complete/shutdown_complete/cancel_on_loss/ideal_send_buffer_size`；default 只返回 `unknown`。Task 3 的 public metrics snapshot 调用 count helper，不得另设第二 violation counter。本 Task 不启动 scheduler thread。

同一步更新 `OpenOutgoing`/`AdoptAccepted` 的所有生产与测试调用点，传入完整 identity/deadline；不保留会绕过 bind 的旧 overload。测试 helper 需要 process-unique id 时使用现有 test counter 明确生成非零值。

Run: `rtk cmake --build build --target tcpquic_stream_lifetime_test -j$(nproc) && rtk build/bin/Release/tcpquic_stream_lifetime_test`

Expected: 构建成功，进程退出码 0，`TestTerminalPublicInterfaceDefaults` 与 `TestBindTerminalIdentityCreatesExactlyOneLedger` 通过。

- [ ] **Step 5: 提交公共接口冻结点**

```bash
git add src/tunnel/terminal_convergence.h src/tunnel/terminal_convergence.cpp src/tunnel/relay.h src/tunnel/tcp_tunnel.h src/tunnel/tcp_tunnel.cpp src/tunnel/stream_lifetime.h src/unittest/stream_lifetime_test.cpp src/CMakeLists.txt
git commit -m "feat: freeze terminal convergence interfaces"
```

### Task 2: 实现 terminal ledger、`AbortBothImmediate` 与线性化 handoff

**Files:**
- Modify: `src/tunnel/terminal_convergence.cpp`
- Modify: `src/tunnel/stream_lifetime.h`
- Modify: `src/tunnel/stream_lifetime.cpp`
- Test: `src/unittest/stream_lifetime_test.cpp`

**Interfaces:**
- Consumes: Task 1 的 `TqTerminalLedger`、`TqTerminalShutdownResult` 与 exact `TqStreamLifetime::BeginTerminalShutdown(uint64_t, std::shared_ptr<Target>, std::shared_ptr<TqTerminalEscalation>) noexcept`。
- Produces: terminal phase 状态转换、`AbortBothImmediate -> ABORT|IMMEDIATE`、terminal-wins 和使用 Task 1 唯一 ledger 的 retention 注册；Task 3 scheduler 与 Task 5 Linux handoff 依赖这些行为。

- [ ] **Step 1: 写 flags、`PENDING`、重复调用、失败和重入的失败测试**

在测试 fake wrapper 的 shutdown hook 中捕获 flags，并增加：

```cpp
void TestBeginTerminalShutdownPendingIsSubmittedOnce() {
    auto target = std::make_shared<CountingTarget>();
    auto sink = std::make_shared<CountingTarget>();
    auto owner = MakeDetachedStartedOwner(target);
    QUIC_STREAM_SHUTDOWN_FLAGS seen = QUIC_STREAM_SHUTDOWN_FLAG_NONE;
    uint32_t calls = 0;
    owner->SetShutdownHookForTest([&](uint64_t, QUIC_STREAM_SHUTDOWN_FLAGS flags) {
        ++calls;
        seen = flags;
        return QUIC_STATUS_PENDING;
    });
    owner->BindTerminalIdentity({17, 133, 2, 7, TqTunnelRole::ClientOpen,
                                 TqRelayBackendType::LinuxWorker});
    const auto first = owner->BeginTerminalShutdown(91, sink, nullptr);
    const auto duplicate = owner->BeginTerminalShutdown(91, sink, nullptr);
    CHECK(first.Status == QUIC_STATUS_PENDING);
    CHECK(first.Submitted && first.Attempt == 1);
    CHECK(duplicate.Submitted && duplicate.Attempt == 1);
    CHECK(calls == 1);
    CHECK((seen & QUIC_STREAM_SHUTDOWN_FLAG_ABORT) != 0);
    CHECK((seen & QUIC_STREAM_SHUTDOWN_FLAG_IMMEDIATE) != 0);
    CHECK(owner->GetTerminalPhase() == TerminalPhase::ShutdownSubmitted);
}

void TestTerminalReentryWinsShutdownReturn() {
    auto sink = std::make_shared<CountingTarget>();
    auto owner = MakeDetachedStartedOwner(std::make_shared<CountingTarget>());
    owner->BindTerminalIdentity({18, 134, 2, 7, TqTunnelRole::ClientOpen,
                                 TqRelayBackendType::LinuxWorker}, 5);
    owner->SetShutdownHookForTest([&](uint64_t, QUIC_STREAM_SHUTDOWN_FLAGS) {
        QUIC_STREAM_EVENT event{};
        event.Type = QUIC_STREAM_EVENT_SHUTDOWN_COMPLETE;
        CHECK(owner->DispatchForTest(&event) == QUIC_STATUS_SUCCESS);
        return QUIC_STATUS_INVALID_STATE;
    });
    const auto result = owner->BeginTerminalShutdown(5, sink, nullptr);
    CHECK(result.AlreadyTerminal);
    CHECK(!result.RetryScheduled);
    CHECK(owner->GetTerminalPhase() == TerminalPhase::TerminalObserved);
}
```

- [ ] **Step 2: 运行测试确认缺少 hook/实现而失败**

Run: `rtk cmake --build build --target tcpquic_stream_lifetime_test -j$(nproc)`

Expected: 编译或断言失败，指出 `SetShutdownHookForTest`/`BeginTerminalShutdown` 尚未实现或 flags 不含 `IMMEDIATE`。

- [ ] **Step 3: 实现一次 reservation、锁外 down-call 和 terminal-wins**

在 test-only 区域加入 exact hook：

```cpp
using ShutdownHookForTest = std::function<QUIC_STATUS(
    uint64_t, QUIC_STREAM_SHUTDOWN_FLAGS)>;
void SetShutdownHookForTest(ShutdownHookForTest hook) noexcept;
```

同时在 private test-only 字段加入 `ShutdownHookForTest ShutdownHookForTest_;`，并声明/实现：

```cpp
void TqStreamLifetime::RetainUntilTerminalLocked() {
    std::lock_guard<std::mutex> registryGuard(g_terminalRetentionLock);
    if (TerminalRetained_) return;
    if (TerminalLedger_ == nullptr) {
        TqRecordTerminalExactlyOnceViolation();
        return;
    }
    g_terminalRetentions.emplace(this, TerminalRetention{
        shared_from_this(), TerminalLedger_, std::chrono::steady_clock::now()});
    TerminalRetained_ = true;
}
```

沿用 Task 1 的 bind 契约：`BindTerminalIdentity()` 已在 `ControlMutex_` 下执行一次性 `TerminalLedger_ = std::make_shared<TqTerminalLedger>(identity)`；production factory 若 bind 后仍得到 null ledger 必须在 callback 安装/start 前失败并释放未启动 wrapper。`RetainUntilTerminalLocked()` 遇到 null 只允许测试记录 invariant violation，production 不得继续 start。

`BeginTerminalShutdown()` 必须按以下完整控制流实现；`CallTerminalShutdown()` 只封装 test hook 或 `stream->Shutdown(errorCode, flags)`：

```cpp
TqTerminalShutdownResult TqStreamLifetime::BeginTerminalShutdown(
    uint64_t errorCode,
    std::shared_ptr<Target> terminalSink,
    std::shared_ptr<TqTerminalEscalation> escalation) noexcept {
    TqTerminalShutdownResult result{};
    std::shared_ptr<TqStreamLifetime> lease;
    MsQuicStream* stream = nullptr;
    {
        std::lock_guard<std::mutex> guard(ControlMutex_);
        if (TerminalPhase_ == TerminalPhase::TerminalObserved ||
            Phase_ == Phase::TerminalPublished) {
            result.AlreadyTerminal = true;
            return result;
        }
        if (TerminalPhase_ == TerminalPhase::ShutdownSubmitted) {
            result.Submitted = true;
            result.Attempt = TerminalShutdownAttempt_;
            return result;
        }
        if (TerminalPhase_ == TerminalPhase::ShutdownReserved) {
            result.Attempt = TerminalShutdownAttempt_;
            return result;
        }
        if (Stream_ == nullptr || TerminalLedger_ == nullptr ||
            (Phase_ != Phase::Starting && Phase_ != Phase::Started) ||
            terminalSink == nullptr) {
            result.Status = QUIC_STATUS_INVALID_STATE;
            return result;
        }
        Target_ = std::move(terminalSink);
        TerminalSink_ = Target_;
        ++RouteGeneration_;
        TerminalEscalation_ = std::move(escalation);
        TerminalErrorCode_ = errorCode;
        TerminalPhase_ = TerminalPhase::ShutdownReserved;
        ++TerminalShutdownAttempt_;
        result.Attempt = TerminalShutdownAttempt_;
        lease = shared_from_this();
        stream = Stream_;
    }

    const auto flags = static_cast<QUIC_STREAM_SHUTDOWN_FLAGS>(
        QUIC_STREAM_SHUTDOWN_FLAG_ABORT | QUIC_STREAM_SHUTDOWN_FLAG_IMMEDIATE);
    result.Status = CallTerminalShutdown(stream, errorCode, flags);

    {
        std::lock_guard<std::mutex> guard(ControlMutex_);
        if (TerminalPhase_ == TerminalPhase::TerminalObserved ||
            Phase_ == Phase::TerminalPublished) {
            result.AlreadyTerminal = true;
            return result;
        }
        if (QUIC_SUCCEEDED(result.Status)) {
            TerminalPhase_ = TerminalPhase::ShutdownSubmitted;
            result.Submitted = true;
        } else if (TerminalPhase_ == TerminalPhase::ShutdownReserved) {
            TerminalPhase_ = TerminalPhase::Active;
            TerminalRetryOwned_ = true;
        }
    }
    TerminalLedger_->RecordShutdown(result.Status, result.Attempt, result.Submitted);
    return result;
}
```

同时扩展旧 `RequestShutdown()` 的 switch：

```cpp
case ShutdownIntent::AbortBothImmediate:
    DesiredSendShutdown_ = 2;
    DesiredReceiveAbort_ = true;
    immediateRequest = true;
    break;
```

并仅在该 intent 合并的 flags 上 OR `QUIC_STREAM_SHUTDOWN_FLAG_IMMEDIATE`。普通 `AbortBoth` 语义保持兼容，fatal 最终释放路径由后续 Task 全部迁到新接口。

- [ ] **Step 4: 在真实 terminal callback 先推进两套 phase，再通知 sink**

在 `Dispatch()` 的 `SHUTDOWN_COMPLETE` 分支中，调用 `PublishTerminalAndTakeTarget()` 时在同一 `ControlMutex_` 临界区把 `TerminalPhase_` 置为 `TerminalObserved`，记录 event；临界区外先从 retention registry erase，再调用 taken sink target。析构临界区用：

```cpp
stream = std::exchange(Stream_, nullptr);
TerminalPhase_ = TerminalPhase::Closed;
Phase_ = Phase::Closed;
```

增加测试断言 callback guard 返回前 `TestDetachedOwnerDestroyCountForTest()==0`，guard 释放后恰为 1。

- [ ] **Step 5: 运行 owner 全量测试**

Run: `rtk cmake --build build --target tcpquic_stream_lifetime_test -j$(nproc) && rtk build/bin/Release/tcpquic_stream_lifetime_test`

Expected: 退出码 0；`PENDING` 被认作 submitted，重复调用 down-call 恰为 1，terminal reentry 返回 `AlreadyTerminal=true`，正常 `GracefulSend` 测试仍通过。

- [ ] **Step 6: 提交 terminal owner 状态机**

```bash
git add src/tunnel/terminal_convergence.cpp src/tunnel/stream_lifetime.h src/tunnel/stream_lifetime.cpp src/unittest/stream_lifetime_test.cpp
git commit -m "feat: add stream terminal shutdown ledger"
```

### Task 3: 实现 worker-independent terminal sink 与 retention 明细

**Files:**
- Modify: `src/tunnel/terminal_convergence.h`
- Modify: `src/tunnel/terminal_convergence.cpp`
- Modify: `src/tunnel/stream_lifetime.h`
- Modify: `src/tunnel/stream_lifetime.cpp`
- Create: `src/unittest/terminal_convergence_test.cpp`
- Modify: `src/CMakeLists.txt`

**Interfaces:**
- Consumes: Task 2 owner terminal publication和现有 `Target` event ABI。
- Produces: `TqTerminalSink::Create(std::weak_ptr<TqStreamLifetime>, std::shared_ptr<TqTerminalLedger>) noexcept`、sink pending/once-only accounting、固定大小 retention snapshot；Task 4 scheduler、Task 7 Admin 和 Task 9 系统门禁依赖这些数据。

- [ ] **Step 1: 写 sink 无强环、事件和 snapshot 失败测试**

创建 `terminal_convergence_test.cpp`，测试主体如下：

```cpp
class CountingEscalation final : public TqTerminalEscalation {
public:
    void RequestConnectionShutdown(
        uint64_t, uint64_t, QUIC_STATUS, uint64_t) noexcept override { ++Calls; }
    std::atomic<uint32_t> Calls{0};
};

void TestTerminalSinkDoesNotOwnOwnerAndAccountsOnce() {
    auto owner = TqStreamLifetime::CreateForTest(
        TqStreamLifetime::Phase::CreatedNotStarted);
    owner->BindTerminalIdentity(TqTerminalIdentity{
        17, 133, 2, 7, TqTunnelRole::ClientOpen,
        TqRelayBackendType::LinuxWorker}, 5);
    auto ledger = owner->TerminalLedger();
    std::weak_ptr<TqStreamLifetime> weak = owner;
    auto sink = TqTerminalSink::Create(owner, ledger);
    owner.reset();
    CHECK(weak.expired());
    QUIC_STREAM_EVENT sendDone{};
    sendDone.Type = QUIC_STREAM_EVENT_SEND_SHUTDOWN_COMPLETE;
    CHECK(sink->OnStreamEvent(nullptr, &sendDone, 3) == QUIC_STATUS_SUCCESS);
    CHECK(ledger->Snapshot(std::chrono::steady_clock::now()).LastStreamEvent ==
          TqTerminalEvent::SendShutdownComplete);
    QUIC_STREAM_EVENT terminal{};
    terminal.Type = QUIC_STREAM_EVENT_SHUTDOWN_COMPLETE;
    CHECK(sink->OnStreamEvent(nullptr, &terminal, 3) == QUIC_STATUS_SUCCESS);
    CHECK(sink->OnStreamEvent(nullptr, &terminal, 3) == QUIC_STATUS_SUCCESS);
    CHECK(ledger->Snapshot(std::chrono::steady_clock::now()).AccountingCompleted);
    CHECK(TqTerminalMetricsSnapshot().DuplicateTerminalSuppressed == 1);
}

void TestIdentityRebindKeepsOriginalLedger() {
    auto owner = TqStreamLifetime::CreateForTest(
        TqStreamLifetime::Phase::CreatedNotStarted);
    TqTerminalIdentity identity{
        17, 133, 2, 7, TqTunnelRole::ClientOpen,
        TqRelayBackendType::LinuxWorker};
    owner->BindTerminalIdentity(identity, 5);
    const auto original = owner->TerminalLedger();
    identity.TunnelId = 134;
    owner->BindTerminalIdentity(identity, 5);
    CHECK(owner->TerminalLedger().get() == original.get());
    CHECK(TqTerminalMetricsSnapshot().ExactlyOnceViolation == 1);
}
```

- [ ] **Step 2: 运行确认 target/metrics 不存在**

Run: `rtk cmake --build build --target tcpquic_terminal_convergence_test -j$(nproc)`

Expected: CMake 报 unknown target，或编译报 `TqTerminalSink`/`TqTerminalMetricsSnapshot` 未声明。

- [ ] **Step 3: 定义并实现 sink 完整 event policy**

在 `stream_lifetime.h` 的 `TqStreamLifetime` 完整定义之后增加唯一 sink factory（不得放进只 forward-declare owner 的 `terminal_convergence.h`，以免循环 include；签名必须与 Frozen Public Interfaces 逐字一致）：

```cpp
class TqTerminalSink final : public TqStreamLifetime::Target {
public:
    static std::shared_ptr<TqTerminalSink> Create(
        std::weak_ptr<TqStreamLifetime> owner,
        std::shared_ptr<TqTerminalLedger> ledger) noexcept;
    QUIC_STATUS OnStreamEvent(
        MsQuicStream*, QUIC_STREAM_EVENT*, uint64_t) noexcept override;
private:
    TqTerminalSink(
        std::weak_ptr<TqStreamLifetime> owner,
        std::shared_ptr<TqTerminalLedger> ledger) noexcept;
    std::weak_ptr<TqStreamLifetime> Owner_;
    std::shared_ptr<TqTerminalLedger> Ledger_;
};

struct TqTerminalMetrics {
    uint64_t HandoffStarted{0};
    uint64_t HandoffCompleted{0};
    uint64_t HandoffFailed{0};
    uint64_t ShutdownSubmitted{0};
    uint64_t ShutdownPending{0};
    uint64_t ShutdownSyncFailure{0};
    uint64_t ShutdownRetry{0};
    uint64_t TerminalObserved{0};
    uint64_t WatchdogArmed{0};
    uint64_t WatchdogCanceled{0};
    uint64_t WatchdogTimeout{0};
    uint64_t ConnectionEscalation{0};
    uint64_t TerminalTimeoutPending{0};
    uint64_t TerminalSinkPending{0};
    uint64_t DuplicateStopSuppressed{0};
    uint64_t DuplicateShutdownSuppressed{0};
    uint64_t DuplicateTerminalSuppressed{0};
    uint64_t ExactlyOnceViolation{0};
};
TqTerminalMetrics TqTerminalMetricsSnapshot() noexcept;
```

`Create()` 先 lock weak owner；owner 不存在、ledger 为空或 `owner->TerminalLedger().get()!=ledger.get()` 时返回 null 并增加 `ExactlyOnceViolation`，绝不接纳外部/平台新建的 ledger。成功时 sink 只保存 weak owner 和传入的同一个 shared ledger。
`TqStreamLifetime::ResetLifecycleRegistriesForTest()` 同时把 Task 1 的唯一 exactly-once atomic 清零，保证 mismatch 测试断言从 0 开始；production 无 reset 入口。

实现规则必须逐 event 穷尽，且所有记录调用 `Ledger_->RecordEvent(TqTerminalEvent::X)`：`PEER_SEND_ABORTED`/`PEER_RECEIVE_ABORTED` 分别记录 enum，不访问 relay；`SEND_COMPLETE` 继续由 owner `Dispatch()` 在 target 分派前通过现有 completion registry once-only claim，绝不交给 sink 猜测 context 类型；`SEND_SHUTDOWN_COMPLETE` 只记录 `SendShutdownComplete`；`SHUTDOWN_COMPLETE` 记录 `ShutdownComplete` 并 complete accounting/cancel scheduler；`RECEIVE` 记录 `ReceiveAfterHandoff`，仅使用本次 callback 的 `stream` 参数调用 `ReceiveSetEnabled(false)`，把 `event->RECEIVE.TotalBufferLength` 置 0 后返回 success，且不保存 stream 参数、不返回悬空 `PENDING`。Admin 之外禁止把 enum materialize 为 `std::string`。

- [ ] **Step 4: 用 Task 1 最终 retention record 实现固定大小 snapshot**

不得重定义或迁移 `TerminalRetention`。pointer equality 在 Task 2 插入临界区检查一次；`TqSnapshotTerminalRetentions(filter)` 在 registry lock 内从 Task 1 的 final record 只复制 `shared_ptr<TqTerminalLedger>` 数组，锁外逐项复制固定大小 `TqTerminalLedgerSnapshot` 并 filter。Admin 线程不读取 owner、stream、relay、tunnel 或 connection。backend `None`、id 0 表示该维度不筛选；`LastStreamEvent` 保持 enum，直到 Task 7 JSON serializer 才调用 `TqTerminalEventName()`。

- [ ] **Step 5: 增加 focused target 并运行**

在 CMake 创建：

```cmake
add_tcpquic_executable(tcpquic_terminal_convergence_test
    unittest/terminal_convergence_test.cpp
    tunnel/terminal_convergence.cpp
    tunnel/stream_lifetime.cpp)
tcpquic_target_include_dirs(tcpquic_terminal_convergence_test)
target_link_libraries(tcpquic_terminal_convergence_test PRIVATE Threads::Threads)
target_compile_definitions(tcpquic_terminal_convergence_test PRIVATE TQ_UNIT_TESTING=1)
tcpquic_link_mimalloc(tcpquic_terminal_convergence_test)
```

Run: `rtk cmake --build build --target tcpquic_terminal_convergence_test tcpquic_stream_lifetime_test -j$(nproc) && rtk build/bin/Release/tcpquic_terminal_convergence_test && rtk build/bin/Release/tcpquic_stream_lifetime_test`

Expected: 两个进程退出码均为 0；weak owner 过期、sink accounting 一次、retention filter 返回指定 identity。

- [ ] **Step 6: 提交 sink 与 retention detail**

```bash
git add src/tunnel/terminal_convergence.h src/tunnel/terminal_convergence.cpp src/tunnel/stream_lifetime.h src/tunnel/stream_lifetime.cpp src/unittest/terminal_convergence_test.cpp src/CMakeLists.txt
git commit -m "feat: add terminal sink and retention details"
```

### Task 4: 实现公共 retry/watchdog/terminal-timeout scheduler

**Files:**
- Modify: `src/tunnel/terminal_convergence.h`
- Modify: `src/tunnel/terminal_convergence.cpp`
- Modify: `src/tunnel/stream_lifetime.h`
- Modify: `src/tunnel/stream_lifetime.cpp`
- Test: `src/unittest/terminal_convergence_test.cpp`
- Modify: `src/CMakeLists.txt`

**Interfaces:**
- Consumes: Task 2 的 retry-owned owner state、Task 3 ledger/sink、`TqTerminalEscalation`。
- Produces: 单线程 `TqTerminalScheduler::{Start,Stop,ScheduleRetry,ArmWatchdog,Cancel,AdvanceForTest}`，固定 retry/deadline policy；Task 5 Linux handoff 调用，Task 6 connection controller 接收 escalation。

- [ ] **Step 1: 用 fake clock 写 10/50/250 ms、5 秒、30 秒失败测试**

```cpp
void TestRetryBackoffAndWatchdogBoundaries() {
    TqTerminalScheduler::ResetForTest();
    auto escalation = std::make_shared<CountingEscalation>();
    auto harness = MakeTerminalHarness({QUIC_STATUS_OUT_OF_MEMORY,
                                        QUIC_STATUS_OUT_OF_MEMORY,
                                        QUIC_STATUS_OUT_OF_MEMORY,
                                        QUIC_STATUS_OUT_OF_MEMORY});
    harness.Owner->BeginTerminalShutdown(44, harness.Sink, escalation);
    CHECK(harness.ShutdownCalls == 1);
    TqTerminalScheduler::AdvanceForTest(std::chrono::milliseconds(9));
    CHECK(harness.ShutdownCalls == 1);
    TqTerminalScheduler::AdvanceForTest(std::chrono::milliseconds(1));
    CHECK(harness.ShutdownCalls == 2);
    TqTerminalScheduler::AdvanceForTest(std::chrono::milliseconds(50));
    CHECK(harness.ShutdownCalls == 3);
    TqTerminalScheduler::AdvanceForTest(std::chrono::milliseconds(250));
    CHECK(harness.ShutdownCalls == 4);
    CHECK(escalation->Calls == 1);
    TqTerminalScheduler::AdvanceForTest(std::chrono::seconds(30));
    CHECK(harness.Ledger->Snapshot(TqTerminalScheduler::NowForTest()).Watchdog ==
          TqTerminalWatchdogState::TerminalTimeout);
}

void TestTerminalCancelsRetryAndWatchdog() {
    auto harness = MakeTerminalHarness({QUIC_STATUS_PENDING});
    harness.Owner->BeginTerminalShutdown(9, harness.Sink, harness.Escalation);
    harness.PublishTerminal();
    TqTerminalScheduler::AdvanceForTest(std::chrono::seconds(40));
    CHECK(harness.Escalation->Calls == 0);
    CHECK(TqTerminalMetricsSnapshot().TerminalTimeoutPending == 0);
}
```

- [ ] **Step 2: 运行确认 scheduler 行为尚不存在**

Run: `rtk cmake --build build --target tcpquic_terminal_convergence_test -j$(nproc)`

Expected: 编译失败，指出 `TqTerminalScheduler` 未声明。

- [ ] **Step 3: 定义 scheduler exact API 和 task ownership**

```cpp
class TqTerminalScheduler final {
public:
    static TqTerminalScheduler& Instance();
    void Start();
    void Stop();
    bool ScheduleRetry(
        std::weak_ptr<TqStreamLifetime> owner,
        std::shared_ptr<TqTerminalLedger> ledger,
        std::shared_ptr<TqTerminalEscalation> escalation,
        uint64_t errorCode,
        uint32_t completedAttempt) noexcept;
    void ArmWatchdog(
        std::weak_ptr<TqStreamLifetime> owner,
        std::shared_ptr<TqTerminalLedger> ledger,
        std::shared_ptr<TqTerminalEscalation> escalation,
        uint64_t errorCode,
        std::chrono::seconds deadline) noexcept;
    void Cancel(uint64_t streamId) noexcept;
#if defined(TQ_UNIT_TESTING)
    static void ResetForTest();
    static void AdvanceForTest(std::chrono::milliseconds delta);
    static std::chrono::steady_clock::time_point NowForTest();
#endif
};
```

在 owner private 区域增加 `friend class TqTerminalScheduler;` 和 exact helper：

```cpp
TqTerminalShutdownResult RetryTerminalShutdown() noexcept {
    std::shared_ptr<Target> sink;
    std::shared_ptr<TqTerminalEscalation> escalation;
    uint64_t errorCode = 0;
    {
        std::lock_guard<std::mutex> guard(ControlMutex_);
        sink = TerminalSink_;
        escalation = TerminalEscalation_;
        errorCode = TerminalErrorCode_;
    }
    return BeginTerminalShutdown(errorCode, std::move(sink), std::move(escalation));
}
```

内部只允许一个 scheduler thread、一个 mutex/CV 和按 due time 排序的 min-heap；task 只持 weak owner、shared ledger/escalation 和 immutable ids，sink 由 owner 的 `TerminalSink_` 保活，scheduler 不建立 `owner -> task -> owner` 强环。到期 retry 先 `owner.lock()`，成功后只调用 `RetryTerminalShutdown()`。初始 down-call 记 attempt 1；其失败后严格在 10 ms、50 ms、250 ms 执行三次 retry（attempt 2、3、4），attempt 4 仍同步失败才立即 escalation。提交成功 arm watchdog；5 秒 task CAS `Armed->Escalated` 后调用一次 escalation；再排 30 秒 timeout task。terminal callback 的 `Cancel(streamId)` 必须先把 ledger 标为 Canceled，再从索引删除所有未执行 task。

- [ ] **Step 4: 把 scheduler 结果接回 `BeginTerminalShutdown()`**

失败且 attempt≤3 时排下一次 retry；attempt 4 失败时直接 escalation：

```cpp
result.RetryScheduled = TqTerminalScheduler::Instance().ScheduleRetry(
    weak_from_this(), TerminalLedger_, TerminalEscalation_,
    TerminalErrorCode_, result.Attempt);
```

成功时：

```cpp
TqTerminalScheduler::Instance().ArmWatchdog(
    weak_from_this(), TerminalLedger_, TerminalEscalation_, TerminalErrorCode_,
    std::chrono::seconds(TerminalWatchdogSeconds_));
```

terminal callback 调 `Cancel(identity.StreamId)`；phase illegal 且未 terminal 时立即调用 escalation，不排 retry。所有 escalation 调用在 owner/scheduler lock 外。

同一步更新 `src/CMakeLists.txt`：凡直接编译 `terminal_convergence.cpp` 且未链接生产 `tcpquic-proxy` 的 test target，必须同时编译 `stream_lifetime.cpp`；特别为 `tcpquic_server_admin_test` 补上两者，避免 scheduler 对 owner helper 的未解析符号。

- [ ] **Step 5: 运行确定性 scheduler 测试**

Run: `rtk cmake --build build --target tcpquic_terminal_convergence_test tcpquic_stream_lifetime_test -j$(nproc) && rtk build/bin/Release/tcpquic_terminal_convergence_test && rtk build/bin/Release/tcpquic_stream_lifetime_test`

Expected: 退出码 0；retry 调用时刻严格为 10/50/250 ms，总 down-call 4 次；最后一次仍失败后 escalation 一次；terminal 后 40 秒无 escalation/timeout。

- [ ] **Step 6: 提交有界 scheduler**

```bash
git add src/tunnel/terminal_convergence.h src/tunnel/terminal_convergence.cpp src/tunnel/stream_lifetime.h src/tunnel/stream_lifetime.cpp src/unittest/terminal_convergence_test.cpp src/CMakeLists.txt
git commit -m "feat: bound terminal retry and watchdog"
```

### Task 5: 接入 Linux fatal handoff、epoll ownership 和 reaper 三门禁

**Files:**
- Modify: `src/tunnel/relay.h`
- Modify: `src/tunnel/linux_relay_worker.h`
- Modify: `src/tunnel/linux_relay_worker.cpp`
- Modify: `src/tunnel/tcp_tunnel.h`
- Modify: `src/tunnel/tcp_tunnel.cpp`
- Modify: `src/tunnel/tunnel_reaper.cpp`
- Test: `src/unittest/linux_relay_worker_io_test.cpp`
- Test: `src/unittest/tcp_tunnel_test.cpp`
- Test: `src/unittest/tunnel_reaper_test.cpp`

**Interfaces:**
- Consumes: Task 2 `BeginTerminalShutdown()`、Task 3 sink、Task 4 scheduler。
- Produces: `TqTerminalHandoffControl` 三事实、`TqLinuxRelayWorker::BeginTerminalHandoff(RelayState*, const char*, uint64_t) noexcept` 和 `TqTunnelTerminalReleaseReady(const TqTunnelContext*)`；Task 7 Admin 读取 facts，Task 8 事故测试复用 test hook。

- [ ] **Step 1: 写 reaper 三事实 truth-table 失败测试**

在 `tunnel_reaper_test.cpp` 的 fake context predicate 改为：

```cpp
CHECK(!TqTerminalReleaseReady({false, true, true}));
CHECK(!TqTerminalReleaseReady({true, false, true}));
CHECK(!TqTerminalReleaseReady({true, true, false}));
CHECK(TqTerminalReleaseReady({true, true, true}));
```

在 Linux IO test 增加 barrier：shutdown hook 阻塞期间调用 reaper predicate，断言 false；hook 返回 `PENDING` 后 handoff complete=true，可删除 tunnel；terminal callback晚到仍由 sink收敛。

- [ ] **Step 2: 运行确认当前 relay stop 会过早通过**

Run: `rtk cmake --build build --target tcpquic_tunnel_reaper_test tcpquic_linux_relay_worker_io_test tcpquic_tcp_tunnel_test -j$(nproc)`

Expected: 新 truth-table 编译失败或 down-call barrier 中 predicate 错误返回 true。

- [ ] **Step 3: 扩展 shared control，禁止 late payload 保存 tunnel handle**

在 `relay.h` 增加：

```cpp
class TqTerminalLedger;

struct TqTerminalHandoffFacts {
    bool DataPlaneStopped{false};
    bool TerminalHandoffComplete{false};
    bool LocalOperationOwnershipTransferredOrDrained{false};
};

inline bool TqTerminalReleaseReady(const TqTerminalHandoffFacts& facts) noexcept {
    return facts.DataPlaneStopped && facts.TerminalHandoffComplete &&
           facts.LocalOperationOwnershipTransferredOrDrained;
}

struct TqTerminalHandoffControl {
    const uint64_t Generation;
    std::atomic<bool> DataPlaneStopped{false};
    std::atomic<bool> TerminalHandoffComplete{false};
    std::atomic<bool> LocalOperationOwnershipTransferredOrDrained{false};
    std::shared_ptr<TqTerminalLedger> Ledger;
    explicit TqTerminalHandoffControl(
        uint64_t generation,
        std::shared_ptr<TqTerminalLedger> ledger) noexcept
        : Generation(generation), Ledger(std::move(ledger)) {}
    TqTerminalHandoffFacts Snapshot() const noexcept {
        return {DataPlaneStopped.load(std::memory_order_acquire),
                TerminalHandoffComplete.load(std::memory_order_acquire),
                LocalOperationOwnershipTransferredOrDrained.load(std::memory_order_acquire)};
    }
};
```

`TqRelayStopControl` 新增 `std::shared_ptr<TqTerminalHandoffControl> TerminalHandoff;`；event/fallback 只携带 binding/control shared owner 与 generation。

- [ ] **Step 4: 用统一 Linux handoff 替换 fatal stop+shutdown 拼接**

声明：

```cpp
TqTerminalShutdownResult BeginTerminalHandoff(
    RelayState* relay,
    const char* reason,
    uint64_t errorCode) noexcept;
```

实现顺序必须为：worker/control lock 内设置 `Closing`、取消 epoll admission、从 owner 取得唯一 ledger、用唯一 factory 创建 sink/control 并发布 sink；锁外调用 owner `BeginTerminalShutdown()`；然后关闭 fd、discard/transfer pending receive，claimed send 继续由 typed completion owner；最后 release-store 三事实。结果为 submitted/already-terminal/retry-owned/已 escalation 接管时才设置 handoff complete。`AbortRelayAndRelease(RelayState*, const char*, bool)` 中 `abortStream=true` 的分支改为只调用此函数，不再调用 `RequestRelayShutdown(AbortBoth)`；worker stop、admin abort、fake FIN、TCP `EPOLLERR/HUP/RDHUP` fatal 分支也统一调用。Linux worker、fallback 与 reaper 文件中禁止出现 `std::make_shared<TqTerminalLedger>`。

核心调用必须是：

```cpp
auto ledger = relay->StreamOwner->TerminalLedger();
if (ledger == nullptr) {
    return TqTerminalShutdownResult{QUIC_STATUS_INVALID_STATE, false, false, false, 0};
}
auto sink = TqTerminalSink::Create(relay->StreamOwner, ledger);
if (sink == nullptr) {
    return TqTerminalShutdownResult{QUIC_STATUS_OUT_OF_MEMORY, false, false, false, 0};
}
const auto result = relay->StreamOwner->BeginTerminalShutdown(
    errorCode, sink, relay->TerminalEscalation);
```

同一个 `ledger` 同时传给 `TqTerminalHandoffControl(relay->ControlGeneration, ledger)`；handoff control、sink、retention record 三者的 ledger pointer 必须相等。

正常 `MaybeStopFullyClosedRelay()`、TCP EOF 的 `QUIC_SEND_FLAG_FIN`、peer FIN 的 delayed `SHUT_WR` 不调用 handoff。

- [ ] **Step 5: 把 reaper predicate 改为 shared facts**

`TqTunnelRelayStopped()` 保留兼容诊断；新增并让 reaper 使用：

```cpp
bool TqTunnelTerminalReleaseReady(const TqTunnelContext* ctx) {
    if (ctx == nullptr) return true;
    const auto control = ctx->RelayHandle.Control;
    if (control == nullptr || control->TerminalHandoff == nullptr) return false;
    return TqTerminalReleaseReady(control->TerminalHandoff->Snapshot());
}
```

`TqReapTunnelContext()` 只 stop/close/delete tunnel，不调用 owner API。terminal callback可晚于 tunnel delete；sink不能访问 `ctx`。

- [ ] **Step 6: 加固 epoll stale generation 和 queue-full fallback**

epoll tag/event 必须带 relay id+control generation；`FindRelayById` 后先比较 generation。handoff 后 late `EPOLLERR|EPOLLHUP|EPOLLRDHUP` 只增加 stale/late cleanup counter，不再取得 stream lease。terminal/fallback queue 满时复用现有 `TerminalFallbackHead`，payload 仅携带 binding/control/ledger，drain 只完成 local operation ownership fact。

- [ ] **Step 7: 运行 Linux/reaper/half-close focused tests**

Run: `rtk cmake --build build --target tcpquic_tunnel_reaper_test tcpquic_linux_relay_worker_io_test tcpquic_linux_relay_worker_queue_test tcpquic_tcp_tunnel_test -j$(nproc) && rtk build/bin/Release/tcpquic_tunnel_reaper_test && rtk build/bin/Release/tcpquic_linux_relay_worker_io_test && rtk build/bin/Release/tcpquic_linux_relay_worker_queue_test && rtk build/bin/Release/tcpquic_tcp_tunnel_test`

Expected: 四个进程退出码 0；down-call 未完成前 reaper=false，`PENDING` 后 reaper=true；late epoll不调用 shutdown；正常 FIN 后反向 payload仍可通过且 watchdog pending 为 0。

- [ ] **Step 8: 提交 Linux handoff/reaper**

```bash
git add src/tunnel/relay.h src/tunnel/linux_relay_worker.h src/tunnel/linux_relay_worker.cpp src/tunnel/tcp_tunnel.h src/tunnel/tcp_tunnel.cpp src/tunnel/tunnel_reaper.cpp src/unittest/linux_relay_worker_io_test.cpp src/unittest/tcp_tunnel_test.cpp src/unittest/tunnel_reaper_test.cpp
git commit -m "fix: gate linux reaper on terminal handoff"
```

### Task 6: 实现 generation-safe connection escalation

**Files:**
- Modify: `src/protocol/quic_session.h`
- Modify: `src/protocol/quic_session.cpp`
- Modify: `src/tunnel/tcp_tunnel.cpp`
- Test: `src/unittest/quic_session_reconnect_test.cpp`
- Test: `src/unittest/tcp_tunnel_test.cpp`

**Interfaces:**
- Consumes: Task 1 `TqTerminalEscalation` 和 identity `ConnectionId/ConnectionGeneration`，Task 4 watchdog callback。
- Produces: `QuicClientSession::MakeTerminalEscalation(uint32_t, uint64_t, uint64_t) noexcept`、`TqMakeServerTerminalEscalation(TqTerminalConnectionKey) noexcept`；Linux tunnel identity 绑定使用真实 slot/accepted generation。

- [ ] **Step 1: 写 slot reuse 隔离失败测试**

```cpp
void TestTerminalEscalationRejectsOldGeneration() {
    QuicClientSession session;
    session.MarkReconnectStartedForTest(1);
    session.MarkSlotConnectedForTest(0, FakeConnection(1));
    const auto before = session.SnapshotConnections().front();
    auto escalation = session.MakeTerminalEscalation(
        before.SlotIndex, before.NumericConnectionId, before.Generation);
    std::string err;
    CHECK(session.ReconnectConnection(before.ConnectionId, err));
    escalation->RequestConnectionShutdown(1, 17, QUIC_STATUS_INVALID_STATE, 91);
    CHECK(session.ConnectionShutdownCallsForTest(0) == 0);
    CHECK(session.TerminalEscalationGenerationMismatchForTest() == 1);
}
```

- [ ] **Step 2: 运行确认缺少 generation adapter**

Run: `rtk cmake --build build --target tcpquic_quic_session_reconnect_test -j$(nproc)`

Expected: 编译失败，指出 `MakeTerminalEscalation` 或 `NumericConnectionId` 未声明。

- [ ] **Step 3: 定义稳定 connection key 与工厂**

在 `quic_session.h` 增加：

```cpp
struct TqTerminalConnectionKey {
    uint64_t ConnectionId{0};
    uint64_t Generation{0};
};

class TqTerminalConnectionController {
public:
    virtual ~TqTerminalConnectionController() = default;
    virtual bool RequestShutdown(
        uint32_t slotIndex,
        TqTerminalConnectionKey key,
        QUIC_STATUS streamStatus,
        uint64_t errorCode) noexcept = 0;
};

// QuicClientSession public method
std::shared_ptr<TqTerminalEscalation> MakeTerminalEscalation(
    uint32_t slotIndex,
    uint64_t numericConnectionId,
    uint64_t generation) noexcept;

std::shared_ptr<TqTerminalEscalation> TqMakeServerTerminalEscalation(
    TqTerminalConnectionKey key) noexcept;
```

`ClientSharedState` 与每个 server connection record 各强持一个 `shared_ptr<TqTerminalConnectionController>`；escalation adapter 只弱持 controller、slot index 和 immutable key。controller 调用时在 session/registry lock 内比较 slot index、`NumericConnectionId`、`Generation` 和 closing 状态，并创建一次性 connection shutdown operation；operation 由 connection owner 队列执行 `Shutdown(0, SILENT)`。旧 generation、已 closing 和重复请求分别计数并返回 false。任何 controller/escalation/task 字段都不得是 `MsQuicConnection*`，只有 connection owner 执行队列的当前 callback/down-call 栈可以使用 wrapper 参数。

- [ ] **Step 4: 将 identity 在 tunnel/stream handoff 前绑定**

client `TqClientPickedConnection` 与 `TqConnectionSnapshot` 增加 `SlotIndex`、`NumericConnectionId`、`Generation`（snapshot 已有 slot/generation，只补 numeric id）；server `TqServerConnectionSnapshot/record` 增加 `NumericConnectionId`、`Generation`。把 tunnel id/stream id 分配提前到 outgoing/accepted owner factory 之前；factory 创建 owner 后、安装 callback/调用 `BeginStart()`/`RetainUntilTerminal()` 之前调用：

```cpp
StreamOwner->BindTerminalIdentity(TqTerminalIdentity{
    streamId, TraceTunnelId, NumericConnectionId, ConnectionGeneration,
    Role, selectedBackend}, Config.Tuning.TerminalWatchdogSeconds);
```

`selectedBackend` 来自已完成的平台 backend 选择结果，Linux 为 `TqRelayBackendType::LinuxWorker`；不得等到 `StartRelay()` 或 terminal handoff 才 bind。若真实 MsQuic stream id 尚不可查询，使用 factory 调用前分配的非零 process-unique id，并在 ledger 保持 immutable，不能在 handoff 后修改。随后 production factory 立刻检查 `StreamOwner->TerminalLedger()!=nullptr`；失败则不安装 callback、不 start wrapper。

- [ ] **Step 5: 运行 connection 与 tunnel tests**

Run: `rtk cmake --build build --target tcpquic_quic_session_reconnect_test tcpquic_tcp_tunnel_test -j$(nproc) && rtk build/bin/Release/tcpquic_quic_session_reconnect_test && rtk build/bin/Release/tcpquic_tcp_tunnel_test`

Expected: 退出码 0；旧 generation escalation 不 shutdown 新连接；相同 generation 并发 timeout 只 shutdown 一次；connection closing 时记录 suppressed 并继续等待 terminal。

- [ ] **Step 6: 提交 generation-safe escalation**

```bash
git add src/protocol/quic_session.h src/protocol/quic_session.cpp src/tunnel/tcp_tunnel.cpp src/unittest/quic_session_reconnect_test.cpp src/unittest/tcp_tunnel_test.cpp
git commit -m "feat: add generation safe terminal escalation"
```

### Task 7: 增加 watchdog 配置、公共 metrics 与 Admin retention API

**Files:**
- Modify: `src/config/config.h`
- Modify: `src/config/config.cpp`
- Modify: `src/config/tuning.h`
- Modify: `src/config/tuning.cpp`
- Modify: `src/runtime/relay_metrics.h`
- Modify: `src/runtime/relay_metrics.cpp`
- Modify: `src/runtime/server_admin.cpp`
- Modify: `src/runtime/router_runtime.cpp`
- Test: `src/unittest/tuning_test.cpp`
- Test: `src/unittest/config_router_test.cpp`
- Test: `src/unittest/server_admin_test.cpp`
- Test: `src/unittest/router_runtime_test.cpp`

**Interfaces:**
- Consumes: Task 3 `TqSnapshotTerminalRetentions(filter)`/metrics，Task 4 scheduler deadline。
- Produces: `TqTuningConfig::TerminalWatchdogSeconds`、`GET /relay/terminal-retentions` schema/filter 和发布门禁 metrics。

- [ ] **Step 1: 写配置边界与 Admin JSON 失败测试**

配置测试必须断言默认 5、显式 5/30 成功、4/31 拒绝。Admin 测试插入一条 ledger 后请求：

```cpp
TqHttpRequest req{"GET",
    "/relay/terminal-retentions?backend=linux&connection_id=2&tunnel_id=133&terminal_phase=shutdown_submitted",
    {}, {}};
const auto response = Handle(req);
CHECK(response.find("\"stream_id\":17") != std::string::npos);
CHECK(response.find("\"shutdown_intent\":\"abort_both_immediate\"") != std::string::npos);
CHECK(response.find("\"watchdog_state\":\"armed\"") != std::string::npos);
```

- [ ] **Step 2: 运行确认 config key 和 endpoint 不存在**

Run: `rtk cmake --build build --target tcpquic_tuning_test tcpquic_config_router_test tcpquic_server_admin_test tcpquic_router_runtime_test -j$(nproc)`

Expected: 新 config 断言失败，Admin 返回 404。

- [ ] **Step 3: 增加精确配置字段和校验**

在 `TqTuningConfig` 增加：

```cpp
uint32_t TerminalWatchdogSeconds{5};
```

在 `TqConfig` 增加 override `uint32_t TuningOverrideTerminalWatchdogSeconds{0};`，JSON path 固定为 `tuning.terminal_watchdog_seconds`。parser 只接受 `[5,30]`；serialize 仅 override 非零时输出；`TqComputeTuning` 默认 5 并应用 override。将值传入 owner/tunnel handoff，禁止 Linux worker 自设不同 deadline。

- [ ] **Step 4: 扩展 neutral relay metrics**

把 Task 3 的所有字段逐一加入 `TqRelayMetricsSnapshot` 和 `TqAppendNeutralRelayMetricsJson()`；另加入 `terminal_retained_owner_count`、`terminal_retained_oldest_age_ms`、`terminal_timeout_pending`、`terminal_sink_pending`。`ExactlyOnceViolation != 0` 和 `TerminalTimeoutPending != 0` 由测试 runner 判为失败；oldest>5000 ms warning，>30000 ms critical 的日志由 scheduler 只发一次。

- [ ] **Step 5: 实现列表 endpoint 与严格 filter parser**

client/router 与 server admin 都处理 `GET /relay/terminal-retentions`。返回：

```json
{"retentions":[{"stream_id":17,"tunnel_id":133,"connection_id":2,"connection_generation":7,"role":"client","backend":"linux","terminal_phase":"shutdown_submitted","retained_age_ms":428,"shutdown_intent":"abort_both_immediate","shutdown_status":"pending","shutdown_attempt":1,"shutdown_submitted_at_ms":1720000000000,"terminal_observed_at_ms":0,"last_stream_event":"send_shutdown_complete","in_tunnel_registry":false,"relay_active":false,"tcp_valid":false,"watchdog_state":"armed","connection_escalated":false}],"count":1,"oldest_age_ms":428}
```

允许且只允许 `backend=linux|windows|darwin`、十进制 `connection_id`、十进制 `tunnel_id`、`terminal_phase=active|shutdown_reserved|shutdown_submitted|terminal_observed|closed`；非法值返回 400 `invalid_filter`。snapshot 后锁外 JSON serialize；`last_stream_event` 的值只能由 `TqTerminalEventName(snapshot.LastStreamEvent)` 生成，ledger/snapshot 内不得缓存对应字符串。

- [ ] **Step 6: 运行配置/Admin/metrics 测试**

Run: `rtk cmake --build build --target tcpquic_tuning_test tcpquic_config_router_test tcpquic_server_admin_test tcpquic_router_runtime_test -j$(nproc) && rtk build/bin/Release/tcpquic_tuning_test && rtk build/bin/Release/tcpquic_config_router_test && rtk build/bin/Release/tcpquic_server_admin_test && rtk build/bin/Release/tcpquic_router_runtime_test`

Expected: 四个进程退出码 0；边界 5/30 round-trip；两端 Admin schema 相同且 filter 只返回 matching row。

- [ ] **Step 7: 提交可配置和可观测性**

```bash
git add src/config/config.h src/config/config.cpp src/config/tuning.h src/config/tuning.cpp src/runtime/relay_metrics.h src/runtime/relay_metrics.cpp src/runtime/server_admin.cpp src/runtime/router_runtime.cpp src/unittest/tuning_test.cpp src/unittest/config_router_test.cpp src/unittest/server_admin_test.cpp src/unittest/router_runtime_test.cpp
git commit -m "feat: expose terminal convergence diagnostics"
```

### Task 8: 增加 Linux 确定性事故回归与故障注入 target

**Files:**
- Create: `src/unittest/linux_terminal_convergence_test.cpp`
- Modify: `src/tunnel/linux_relay_worker.h`
- Modify: `src/tunnel/linux_relay_worker.cpp`
- Modify: `src/unittest/linux_relay_worker_io_test.cpp`
- Modify: `src/CMakeLists.txt`

**Interfaces:**
- Consumes: Task 4 fake scheduler、Task 5 handoff facts、Task 6 escalation hooks。
- Produces: `tcpquic_linux_terminal_convergence_test`，覆盖设计 §8.2 的 Linux 8 个确定性竞态和精确事故时序。

- [ ] **Step 1: 先建立八场景 test table 并确认 target 不存在**

```cpp
const TestCase cases[] = {
    {"reaper_before_during_after_downcall", TestReaperAtAllHandoffBoundaries},
    {"terminal_reenters_shutdown", TestTerminalReentersShutdown},
    {"four_way_fatal_race", TestTcpAdminWorkerConnectionRace},
    {"queue_full_after_tunnel_release", TestQueueFullSinkAfterTunnelRelease},
    {"late_terminal_fd_and_slot_reuse", TestLateTerminalFdAndGenerationReuse},
    {"watchdog_escalates_without_close", TestSuppressedTerminalEscalatesOnlyConnection},
    {"duplicates_are_exactly_once", TestDuplicateAbortStopTerminalReaper},
    {"half_close_keeps_reverse_flow", TestGracefulHalfCloseReverseFlow},
};
```

Run: `rtk cmake --build build --target tcpquic_linux_terminal_convergence_test -j$(nproc)`

Expected: CMake 报 `No rule to make target tcpquic_linux_terminal_convergence_test`。

- [ ] **Step 2: 增加只在 `TQ_UNIT_TESTING` 生效的 precise hooks**

`TqLinuxRelayWorkerConfig` 增加：

```cpp
void (*BeforeTerminalReserveForTest)(uint64_t relayId){nullptr};
void (*InsideTerminalDowncallForTest)(uint64_t relayId){nullptr};
void (*AfterTerminalSubmitForTest)(uint64_t relayId){nullptr};
bool ForceTerminalQueueFullForTest{false};
bool SuppressTerminalCallbackForTest{false};
```

hook 只能停在无 worker/control mutex 的边界；production build 不保留分支。shutdown status 使用 Task 2 owner hook，不在 worker 伪造 MsQuic flags。

- [ ] **Step 3: 实现精确事故测试**

事故 test 必须按 barrier 驱动：client TCP FIN→client FIN send completion→server peer FIN→target socketpair `shutdown(SHUT_WR)`→target `SO_LINGER{1,0}` close 产生 RST/`ECONNRESET`→server `ABORT|IMMEDIATE`→handoff facts true 后删除 tunnel→server local terminal→client peer abort/terminal。最后断言：

```cpp
CHECK(activeTunnels == baseline.ActiveTunnels);
CHECK(activeRelays == baseline.ActiveRelays);
CHECK(TqTerminalMetricsSnapshot().TerminalSinkPending == baseline.TerminalSinkPending);
CHECK(TqTerminalMetricsSnapshot().TerminalTimeoutPending == 0);
CHECK(TqStreamLifetime::SnapshotSendCompletions().ActiveCount == baseline.SendCompletions);
CHECK(TqStreamLifetime::SnapshotTerminalRetentions().OwnerCount == baseline.Retentions);
CHECK(closeWaitCount == 0);
CHECK(TqTerminalMetricsSnapshot().ExactlyOnceViolation == 0);
```

- [ ] **Step 4: 创建 CMake target 并运行全部八场景**

```cmake
add_executable(tcpquic_linux_terminal_convergence_test
    unittest/linux_terminal_convergence_test.cpp
    unittest/trace_proxy_stub.cpp
    tunnel/linux_relay_worker.cpp
    tunnel/terminal_convergence.cpp
    tunnel/stream_lifetime.cpp
    tunnel/relay_buffer.cpp
    tunnel/relay_alloc.cpp
    protocol/compress.cpp
    config/tuning.cpp
    ${TCPQUIC_PLATFORM_SOURCES})
tcpquic_target_include_dirs(tcpquic_linux_terminal_convergence_test)
target_link_libraries(tcpquic_linux_terminal_convergence_test PRIVATE Threads::Threads ${TCPQUIC_TEST_LIBS} spdlog::spdlog)
target_compile_definitions(tcpquic_linux_terminal_convergence_test PRIVATE ${TCPQUIC_TEST_DEFS} TQ_UNIT_TESTING=1)
tcpquic_link_mimalloc(tcpquic_linux_terminal_convergence_test)
```

Run: `rtk cmake --build build --target tcpquic_linux_terminal_convergence_test tcpquic_linux_relay_worker_io_test -j$(nproc) && rtk build/bin/Release/tcpquic_linux_terminal_convergence_test && rtk build/bin/Release/tcpquic_linux_relay_worker_io_test`

Expected: 两个进程退出码 0；八个 case 名逐项打印 `PASS`；half-close case 的 shutdown flags 只有 graceful/FIN，不含 `ABORT`/`IMMEDIATE`。

- [ ] **Step 5: 提交 Linux 事故/故障注入证据**

```bash
git add src/unittest/linux_terminal_convergence_test.cpp src/tunnel/linux_relay_worker.h src/tunnel/linux_relay_worker.cpp src/unittest/linux_relay_worker_io_test.cpp src/CMakeLists.txt
git commit -m "test: cover linux terminal convergence races"
```

### Task 9: 增加真实双端事故/k6 runner 并完成 Linux 发布验证

**Files:**
- Create: `scripts/run-linux-terminal-convergence.sh`
- Create: `docs/test/k6/linux-terminal-convergence.js`
- Modify: `src/CMakeLists.txt`（仅当遗漏测试 source/target dependency 时修正）
- Test: 本计划列出的全部 focused targets 与系统 runner

**Interfaces:**
- Consumes: Task 7 Admin/metrics endpoint、Task 8 fault hooks 的 test-build CLI/environment wiring。
- Produces: 可重复的 `incident`、`baseline`、`peak`、`spike`、`stress`、`soak` 命令、完整证据目录与 Linux 发布结论；不新增生产接口。

- [ ] **Step 1: 写 runner 自检并确认脚本尚不存在**

Run: `rtk bash scripts/run-linux-terminal-convergence.sh --self-test`

Expected: `No such file or directory`。

- [ ] **Step 2: 创建 k6 场景脚本**

脚本固定 payload mix 1 KiB 50%、64 KiB 40%、1 MiB 10%，80% FIN/20% RST；使用环境变量 `SCENARIO` 选择：baseline 100 并发 5 分钟、peak 500 并发 10 分钟且 50 tunnel/s、spike 100→1000 并发 30 秒 ramp 后保持 2 分钟、stress 每 2 分钟 +250 并发、soak 100 并发/100 tunnel/s/30 分钟。thresholds 完整为：

```javascript
export const options = {
  thresholds: {
    checks: ['rate>0.999'],
    http_req_failed: ['rate<0.001'],
    dropped_iterations: ['count==0'],
    http_req_duration: ['p(95)<500', 'p(99)<1000'],
  },
};
```

`handleSummary()` 必须输出 `summary.json`，包含 terminal convergence 自定义 Trend/Counter：fatal terminal latency、RST count、FIN reverse-flow checks。

- [ ] **Step 3: 创建 runner 的完整阶段和发布判定**

shell 必须 `set -euo pipefail`，支持：

```text
--self-test
--scenario incident|baseline|peak|spike|stress|soak
--client-config PATH --server-config PATH --out DIR
```

runner 启动带 token 的 client/server Admin、loopback FIN/RST target，保存 git SHA、binary SHA256、uname、config、两端日志、每 5 秒 metrics/Admin retention、`ss -tanp` before/after、k6 raw/summary。退出时轮询最多 30 秒，要求 active_tunnels/active_relays/terminal_sink_pending/watchdog pending/send completion/retained owner 回 baseline；用 `jq -e` 强制 `terminal_timeout_pending==0`、exactly-once violation==0。incident 额外要求 10 秒内归零且对应端口 `CLOSE-WAIT` 为 0。

- [ ] **Step 4: 运行自检和短 incident smoke**

Run: `rtk bash scripts/run-linux-terminal-convergence.sh --self-test`

Expected: 输出 `linux terminal convergence runner self-test passed`，退出码 0。

Run: `rtk bash scripts/run-linux-terminal-convergence.sh --scenario incident --client-config cfg-client.json --server-config cfg-server.json --out docs/test/linux-terminal-incident-smoke`

Expected: `summary/result.json` 中 `passed=true`，`close_wait=0`，所有资源 delta 为 0；若本机缺 k6，incident 仍运行，只有性能 scenario 明确失败并提示安装命令。

- [ ] **Step 5: 提交系统 workload**

```bash
git add scripts/run-linux-terminal-convergence.sh docs/test/k6/linux-terminal-convergence.js
git commit -m "test: add linux terminal convergence workload"
```

- [ ] **Step 6: 构建真实 target 名称对应的全量 focused suite**

Run:

```bash
rtk cmake --build build --target tcpquic_terminal_convergence_test tcpquic_stream_lifetime_test tcpquic_linux_terminal_convergence_test tcpquic_linux_relay_worker_io_test tcpquic_linux_relay_worker_queue_test tcpquic_tunnel_reaper_test tcpquic_tcp_tunnel_test tcpquic_quic_session_reconnect_test tcpquic_tuning_test tcpquic_config_router_test tcpquic_server_admin_test tcpquic_router_runtime_test tcpquic-proxy -j$(nproc)
```

Expected: 所有 target 构建成功；注意实际 tunnel executable target 为 `tcpquic_tunnel_test`，`tcpquic_tcp_tunnel_test` 是仓库已有兼容 custom target，运行文件仍为 `build/bin/Release/tcpquic_tcp_tunnel_test`。

- [ ] **Step 7: 运行 focused suite**

Run:

```bash
rtk build/bin/Release/tcpquic_terminal_convergence_test
rtk build/bin/Release/tcpquic_stream_lifetime_test
rtk build/bin/Release/tcpquic_linux_terminal_convergence_test
rtk build/bin/Release/tcpquic_linux_relay_worker_io_test
rtk build/bin/Release/tcpquic_linux_relay_worker_queue_test
rtk build/bin/Release/tcpquic_tunnel_reaper_test
rtk build/bin/Release/tcpquic_tcp_tunnel_test
rtk build/bin/Release/tcpquic_quic_session_reconnect_test
rtk build/bin/Release/tcpquic_tuning_test
rtk build/bin/Release/tcpquic_config_router_test
rtk build/bin/Release/tcpquic_server_admin_test
rtk build/bin/Release/tcpquic_router_runtime_test
```

Expected: 每个进程退出码 0，无 `CHECK failed`、hang 或 core；测试结束 retention/sink/watchdog/send registry 均为基线。

- [ ] **Step 8: 静态发布门禁审计**

Run:

```bash
rtk rg -n "AbortBothImmediate|QUIC_STREAM_SHUTDOWN_FLAG_IMMEDIATE|BeginTerminalShutdown" src/tunnel src/protocol
rtk rg -n -- "->Shutdown\(" src/tunnel/linux_relay_worker.cpp src/tunnel/tcp_tunnel.cpp
rtk rg -n "MsQuicStream\*|MsQuicConnection\*|TqTunnelContext\*|TqLinuxRelayWorker\*|RelayState\*" src/tunnel/terminal_convergence.*
rtk rg -n "QUIC_SEND_FLAG_FIN|MaybeStopFullyClosedRelay|SHUT_WR" src/tunnel/linux_relay_worker.cpp src/tunnel/tcp_tunnel.cpp
rtk git diff --check
```

Expected: fatal 最终释放只经 `BeginTerminalShutdown`；terminal convergence 公共文件的 raw pointer 命中仅允许 callback ABI 参数/forward declaration，不得作为字段；FIN/`SHUT_WR` 路径仍存在；`git diff --check` 无输出。

- [ ] **Step 9: 独立 ASan 构建与事故回归**

Run:

```bash
rtk cmake -S . -B build-terminal-asan -DCMAKE_BUILD_TYPE=RelWithDebInfo -DQUIC_ENABLE_ASAN=ON -DTCPQUIC_USE_MIMALLOC=OFF -DTCPQUIC_ENABLE_CRASHPAD=OFF
rtk cmake --build build-terminal-asan --target tcpquic_terminal_convergence_test tcpquic_stream_lifetime_test tcpquic_linux_terminal_convergence_test tcpquic_linux_relay_worker_io_test tcpquic_tcp_tunnel_test -j$(nproc)
rtk env ASAN_OPTIONS=detect_leaks=1:halt_on_error=1 build-terminal-asan/bin/Release/tcpquic_terminal_convergence_test
rtk env ASAN_OPTIONS=detect_leaks=1:halt_on_error=1 build-terminal-asan/bin/Release/tcpquic_stream_lifetime_test
rtk env ASAN_OPTIONS=detect_leaks=1:halt_on_error=1 build-terminal-asan/bin/Release/tcpquic_linux_terminal_convergence_test
rtk env ASAN_OPTIONS=detect_leaks=1:halt_on_error=1 build-terminal-asan/bin/Release/tcpquic_linux_relay_worker_io_test
rtk env ASAN_OPTIONS=detect_leaks=1:halt_on_error=1 build-terminal-asan/bin/Release/tcpquic_tcp_tunnel_test
```

Expected: 全部退出码 0；ASan 0 UAF、0 double free、0 leak、0 invalid handle access。

- [ ] **Step 10: 运行 incident、baseline/peak/spike/stress 和 soak**

Run:

```bash
rtk bash scripts/run-linux-terminal-convergence.sh --scenario incident --client-config cfg-client.json --server-config cfg-server.json --out docs/test/linux-terminal-incident
rtk bash scripts/run-linux-terminal-convergence.sh --scenario baseline --client-config cfg-client.json --server-config cfg-server.json --out docs/test/linux-terminal-baseline
rtk bash scripts/run-linux-terminal-convergence.sh --scenario peak --client-config cfg-client.json --server-config cfg-server.json --out docs/test/linux-terminal-peak
rtk bash scripts/run-linux-terminal-convergence.sh --scenario spike --client-config cfg-client.json --server-config cfg-server.json --out docs/test/linux-terminal-spike
rtk bash scripts/run-linux-terminal-convergence.sh --scenario stress --client-config cfg-client.json --server-config cfg-server.json --out docs/test/linux-terminal-stress
rtk bash scripts/run-linux-terminal-convergence.sh --scenario soak --client-config cfg-client.json --server-config cfg-server.json --out docs/test/linux-terminal-soak
```

Expected: incident 10 秒内资源归零且无 `CLOSE-WAIT`；fatal convergence p95<1 秒、p99<5 秒；p95 建立延迟≤旧 baseline 105%、p99≤110%、吞吐≥95%；业务错误率<0.1%、dropped iterations=0；CPU/RSS 峰值增幅各≤10%、无新增常驻线程；soak 30 分钟且至少 100 tunnel/s，retained count 不单调增长并在结束 30 秒内回 baseline；terminal timeout 和 exactly-once violation 全程为 0。

- [ ] **Step 11: 最终提交验证所需的 CMake 修正（若无修正则跳过提交，不制造空提交）**

```bash
git add src/CMakeLists.txt
git diff --cached --quiet || git commit -m "build: wire terminal convergence verification"
```

## 依赖与执行顺序

- Task 1 是公共接口冻结点；Windows/macOS adapter 计划只能依赖该接口，不得复制 terminal state machine。
- Task 2 依赖 Task 1；Task 3 依赖 Task 2；Task 4 依赖 Task 2–3。
- Task 5 依赖 Task 2–4，是 Linux tunnel/reaper 安全释放的首次平台接入。
- Task 6 可在 Task 5 后实现，但 Task 4 的 timeout 测试先使用 fake escalation；真实发布必须同时包含 Task 6。
- Task 7 依赖 Task 3–6 的稳定 snapshot/metrics 字段。
- Task 8 依赖 Task 4–6；Task 9 依赖全部前置 Task，并生成系统/性能/soak 发布证据。

## 验收标准

- `TqStreamLifetime` 同时维护既有 start phase、方向性 FIN/abort ledger 和独立 `TerminalPhase`，三者不互相冒充。
- `BeginTerminalShutdown()` 在 tunnel/relay 可释放前完成 retention、sink、reservation 和 `ABORT|IMMEDIATE` handoff；`PENDING` 是成功，失败按 10/50/250 ms 重试。
- sink、retry/watchdog 与 escalation 在 tunnel、relay、worker 销毁后仍安全，不形成 owner 强引用环，不保存任何业务/平台裸指针。
- 5 秒 watchdog 只 escalation 一次，旧 connection generation 不能关闭新 slot；30 秒 timeout 留存可诊断 owner并阻断发布，不直接 close stream。
- reaper 只在三个 handoff facts 都为 true 时删除 context；terminal callback 可在 tunnel 删除后到达并完成一次 accounting/cancel/retention release。
- 正常 FIN half-close 反向数据、send completion 和最终 FIN 行为与改造前一致，fatal watchdog pending 恒为 0。
- Linux stale epoll tag/fd reuse、queue-full fallback、worker stop、四路 fatal race 和 target RST 事故回归全部确定性通过。
- Admin 可按 backend/connection/tunnel/phase 定位 retained owner；聚合 metrics 覆盖 handoff、shutdown/retry、watchdog、escalation、timeout、sink、suppressed duplicate 和 exactly-once violation。
- focused、ASan、incident、性能、stress 和 30 分钟 soak 全部满足 Task 9 门禁。
