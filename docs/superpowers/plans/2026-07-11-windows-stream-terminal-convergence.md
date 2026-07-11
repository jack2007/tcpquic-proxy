# Windows Stream Terminal 有界收敛 Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 在不破坏正常 FIN half-close 的前提下，把 Windows IOCP relay 的 fatal close、worker stop、terminal callback、connection escalation 与公共 stream terminal 有界收敛协议接通，并以事故回归、故障注入、Verifier、k6 和 soak 证明所有 ownership 有界归零。

**Architecture:** Linux 公共层计划冻结 `TqStreamLifetime::BeginTerminalShutdown()`、`TqTerminalSink`、`TqTerminalWatchdog`、retention snapshot 和 retry/escalation 语义；Windows 只实现 adapter，不复制公共 phase、retry 或 watchdog 状态机。Windows relay 在本地 close/retire 前冻结数据面并把 typed IOCP/OVERLAPPED ownership 转移或排空，再以 stable connection identity/generation 提供 escalation；真实 `SHUTDOWN_COMPLETE` 仍是唯一 terminal 事实，正常 TCP EOF 仍只提交 QUIC FIN。

**Tech Stack:** C++17, MsQuic C++ wrapper, Windows IOCP/Winsock overlapped I/O, CMake/MSVC, Application Verifier/Windows ASan, PowerShell, k6/xk6 TCP.

## Global Constraints

- 依赖 Linux 公共层 Task 1 冻结并合入后再实现 Windows adapter；不得在 `windows_relay_worker.*` 复制 `TerminalPhase`、retry scheduler、watchdog timer wheel、retention registry 或 sink ledger。
- 冻结依赖的公共签名必须保持为 `TqTerminalShutdownResult TqStreamLifetime::BeginTerminalShutdown(uint64_t errorCode, std::shared_ptr<TqStreamLifetime::Target> terminalSink, std::shared_ptr<TqTerminalEscalation> escalation) noexcept`。
- 公共 backend 类型冻结为 `using TqRelayBackend = TqRelayBackendType`，Windows 值只能使用 `TqRelayBackendType::WindowsWorker`；role 只能使用 `TqTunnelRole::ClientOpen` 或 `TqTunnelRole::ServerOpen`。
- terminal sink 必须以 `TqTerminalSink::Create(std::weak_ptr<TqStreamLifetime>, std::shared_ptr<TqTerminalLedger>) noexcept` 创建并消费 registration 携带的同一个 ledger；Windows 不创建第二份 ledger 或私有 sink factory。
- `QUIC_STATUS_SUCCESS` 与 `QUIC_STATUS_PENDING` 均表示提交成功；同步失败最多重试 3 次，退避固定为 10 ms、50 ms、250 ms。
- fatal watchdog 默认 5 秒，配置范围固定为 5–30 秒；首次超时只升级一次 connection shutdown，升级后 30 秒仍未 terminal 则保留 retention 并进入 `terminal_timeout` 发布阻断状态。
- fatal 最终释放必须使用 `QUIC_STREAM_SHUTDOWN_FLAG_ABORT | QUIC_STREAM_SHUTDOWN_FLAG_IMMEDIATE`；不得用 `StreamClose`、删除 retention、清 worker 或进程重启伪造 terminal。
- 只有真实 `QUIC_STREAM_EVENT_SHUTDOWN_COMPLETE` 推进 wrapper terminal；TCP close、relay stop、IOCP drain、`SEND_SHUTDOWN_COMPLETE`、peer abort 和 FIN 均不得发布 terminal。
- 正常 TCP EOF 继续使用 `QUIC_SEND_FLAG_FIN`，允许 QUIC->TCP 反向数据继续传输；正常 half-close 不创建 terminal sink、不 arm fatal watchdog、不升级 connection。
- callback、sink、watchdog、IOCP envelope 与 escalation token 不得保存 tunnel、relay、worker、stream wrapper、public handle 或 connection 的可解引用裸指针。
- 所有成功提交的 `WSARecv`/`WSASend` 必须等 GQCS dequeue 后才 settle；`GetQueuedCompletionStatus()` 返回 `FALSE` 且 `OVERLAPPED != nullptr` 仍是必须消费的完成。
- stop、shutdown、terminal、socket close、operation settle、active accounting、watchdog cancel 和 connection escalation 必须 exactly-once；duplicate/stale generation 只能增加 suppressed 诊断。
- 所有并发测试用 barrier/latch 指定线性化点，不用固定 sleep 命中竞态；fault hook 仅在 `TQ_UNIT_TESTING` 或专用系统测试构建中启用。
- `tcpquic_windows_relay_worker_test` 当前是自有 `main()` 的单 executable，不支持 gtest filter/`--case`；每个新增 `bool Test...()` 必须在同一 Task 把唯一失败码接入 `main()`，验证命令运行整个 executable。
- 文档与提交说明使用中文；代码标识符、Admin JSON 字段和 metrics 名称使用本计划给出的精确英文拼写。
- 不修改 tunnel wire protocol、认证、压缩、payload framing 或三平台公共语义。

---

## Frozen Public Interfaces

Windows 每个 Task 都可以直接消费以下 Linux 公共层产物；若实际合入签名不同，先在公共层 review 中统一，不能在 Windows 添加兼容分支：

```cpp
struct TqTerminalShutdownResult {
    QUIC_STATUS Status{QUIC_STATUS_SUCCESS};
    bool Submitted{false};
    bool AlreadyTerminal{false};
    bool RetryScheduled{false};
    uint32_t Attempt{0};
};

struct TqTerminalIdentity {
    uint64_t StreamId{0};
    uint64_t TunnelId{0};
    uint64_t ConnectionId{0};
    TqTunnelRole Role{};
    TqRelayBackend Backend{TqRelayBackendType::WindowsWorker};
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

TqTerminalShutdownResult TqStreamLifetime::BeginTerminalShutdown(
    uint64_t errorCode,
    std::shared_ptr<TqStreamLifetime::Target> terminalSink,
    std::shared_ptr<TqTerminalEscalation> escalation) noexcept;
```

## File Structure

- Modify: `src/tunnel/windows_relay_worker.h`
  - 定义 typed command/terminal envelope、Windows handoff adapter、local operation ownership snapshot、fault barriers 和不含裸 capability 的 registration result。
- Modify: `src/tunnel/windows_relay_worker.cpp`
  - 接入公共 `BeginTerminalShutdown()`；修复 GQCS/CancelIoEx/stop drain；分离 local retire 与 terminal；保留 FIN half-close。
- Modify: `src/tunnel/relay.h`, `src/tunnel/relay.cpp`
  - 删除 Windows public handle 中的裸 worker capability，以 runtime-owned worker index + relay/control generation 投递 stop/trace。
- Modify: `src/tunnel/tcp_tunnel.cpp`
  - 在 Windows registration 中提供 immutable terminal identity、sink 与 generation-safe escalation；把 handoff complete 纳入 tunnel reaper 门禁。
- Modify: `src/protocol/quic_session.h`, `src/protocol/quic_session.cpp`
  - 为 client slot 和 server connection registry 提供 stable connection shutdown endpoint；lookup 同时校验 connection id 与 generation，锁外调用 MsQuic shutdown。
- Modify: `src/tunnel/tunnel_reaper.h`, `src/tunnel/tunnel_reaper.cpp`
  - Windows tunnel 仅在 data-plane stopped、terminal handoff complete、local IO ownership transferred/drained 三项同时满足时可删除。
- Modify: `src/runtime/relay_metrics.h`, `src/runtime/relay_metrics.cpp`, `src/runtime/admin_http.cpp`
  - 暴露公共 terminal retention detail/filter 与 Windows IOCP backend 字段；聚合值遵循 count 求和、oldest age/violation 求最大值。
- Modify: `src/unittest/windows_relay_worker_test.cpp`
  - 添加 deterministic handoff、terminal reentry、worker stop、typed command、OVERLAPPED、half-close 与故障注入用例；恢复本范围内禁用的真实 Winsock 用例。
- Modify: `src/unittest/tcp_tunnel_test.cpp`, `src/unittest/quic_session_reconnect_test.cpp`, `src/unittest/admin_http_test.cpp`
  - 分别验证 reaper 门禁、connection generation 隔离和 Admin JSON/filter。
- Inspect only: `src/CMakeLists.txt`
  - 已核对并继续使用实际 target `tcpquic_windows_relay_worker_test`、`tcpquic_stream_lifetime_test`、`tcpquic_tunnel_test`、`tcpquic_quic_session_reconnect_test`、`tcpquic_admin_http_test`；现有 source list 已包含本计划修改的 `.cpp`，不创建同义 target。
- Create: `tests/windows/terminal_incident_target.py`
  - 可控 target：先读到 client FIN，再对 server socket 施加 RST，输出带 tunnel correlation id 的事件序列。
- Create: `tests/windows/terminal_incident_regression.ps1`
  - 启动 client/server/target，执行精确事故时序，轮询 Admin/metrics 并保存 trace、socket 与资源归零证据。
- Create: `tests/k6/windows-terminal-convergence.js`
  - baseline、peak、spike、stress、soak 共用 workload 与阈值；payload 分布严格为 1 KiB 50%、64 KiB 40%、1 MiB 10%。
- Modify: `scripts/test-tcpquic-proxy-windows.ps1`
  - 把事故回归、Verifier 和 k6 入口接到现有 Windows 测试脚本，不改变原 HTTP CONNECT/SOCKS5 smoke 行为。
- Modify: `docs/relay_win.md`, `src/docs/thread_model_cn.md`
  - 记录 terminal handoff、IOCP drain、connection escalation 与发布门禁；实现提交中更新，不把文档当状态机来源。

### Task 1: 固化公共依赖并建立 Windows 编译与 ownership 失败基线

**Files:**
- Modify: `src/tunnel/windows_relay_worker.h`
- Modify: `src/tunnel/windows_relay_worker.cpp`
- Modify: `src/tunnel/relay.h`
- Modify: `src/tunnel/relay.cpp`
- Test: `src/unittest/windows_relay_worker_test.cpp`
- Test: `src/unittest/stream_lifetime_test.cpp`

**Interfaces:**
- Consumes: Frozen Public Interfaces 中的 `TqTerminalShutdownResult`、`TqTerminalEscalation`、`BeginTerminalShutdown()` 和公共 retention snapshot。
- Produces: immutable `TqRelayStopControl::Generation` 的 Windows 合法读取；不含 `Worker` 的 `TqWindowsRelayRegistrationResult`；后续 Task 使用的 `TqWindowsCommandBase` 与 `TqWindowsCommandEnvelope<Result>`。

- [ ] **Step 1: 写入编译期公共接口与裸 capability 失败测试**

```cpp
static_assert(std::is_same_v<
    decltype(std::declval<TqStreamLifetime&>().BeginTerminalShutdown(
        uint64_t{},
        std::shared_ptr<TqStreamLifetime::Target>{},
        std::shared_ptr<TqTerminalEscalation>{})),
    TqTerminalShutdownResult>);
static_assert(std::is_const_v<
    std::remove_reference_t<decltype(std::declval<TqRelayStopControl>().Generation)>>);
static_assert(!std::is_pointer_v<
    decltype(std::declval<TqWindowsRelayRegistrationResult>().WorkerIndex)>);
template<class T, class = void> struct HasWorkerMember : std::false_type {};
template<class T> struct HasWorkerMember<T, std::void_t<decltype(std::declval<T>().Worker)>>
    : std::true_type {};
static_assert(!HasWorkerMember<TqWindowsRelayRegistrationResult>::value);
```

- [ ] **Step 2: 运行 MSVC 编译并确认当前失败原因**

Run:

```powershell
rtk cmake --build build-x64 --config Debug --target tcpquic_stream_lifetime_test tcpquic_windows_relay_worker_test -j
rtk rg -n "Generation\.(load|store)" src/tunnel/windows_relay_worker.cpp src/tunnel/relay.cpp
```

Expected: build 在 `const uint64_t Generation` 的 `.load()`/`.store()` 使用处失败；`rg` 列出 Windows production 调用点，测试尚未通过。

- [ ] **Step 3: 最小修复 generation，并定义 heap-owned command completion**

```cpp
enum class TqWindowsCommandState : uint8_t {
    Pending,
    Completed,
    Stopped,
    PostFailed,
};

class TqWindowsCommandBase {
public:
    virtual ~TqWindowsCommandBase() = default;
    virtual uint64_t Id() const noexcept = 0;
    virtual void CompleteStopped() noexcept = 0;
    virtual void CompletePostFailed(DWORD error) noexcept = 0;
};

template<class Result>
struct TqWindowsCommandEnvelope final : TqWindowsCommandBase {
    uint64_t CommandId{0};
    std::mutex Mutex;
    std::condition_variable Cv;
    TqWindowsCommandState State{TqWindowsCommandState::Pending};
    Result Value{};
    DWORD PostError{ERROR_SUCCESS};

    uint64_t Id() const noexcept override { return CommandId; }
    void CompleteStopped() noexcept override {
        (void)Complete(TqWindowsCommandState::Stopped);
    }
    void CompletePostFailed(DWORD error) noexcept override {
        PostError = error;
        (void)Complete(TqWindowsCommandState::PostFailed);
    }

    bool Complete(TqWindowsCommandState state, Result value = {}) noexcept {
        std::lock_guard<std::mutex> guard(Mutex);
        if (State != TqWindowsCommandState::Pending) return false;
        Value = std::move(value);
        State = state;
        Cv.notify_all();
        return true;
    }
};

struct TqWindowsRelayRegistrationResult {
    bool Ok{false};
    bool TcpFdConsumed{false};
    uint64_t RelayId{0};
    uint64_t RelayGeneration{0};
    std::shared_ptr<TqRelayStopControl> StopControl;
    uint32_t WorkerIndex{0};
};

std::shared_ptr<TqRelayStopControl> AllocateRelayStopControl() {
    return std::make_shared<TqRelayStopControl>(TqRelayNextControlGeneration());
}
```

把所有 Windows `control->Generation.load(...)` 改为 `control->Generation`，删除 Windows 私有 generation allocator 与 `.store()` 初始化。`IoOperation` 暂时增加 `std::shared_ptr<void> CommandOwner`，后续 Task 4 改为 typed variant；不得再新增栈地址。

- [ ] **Step 4: 运行 focused 测试并确认通过**

Run:

```powershell
rtk cmake --build build-x64 --config Debug --target tcpquic_stream_lifetime_test tcpquic_windows_relay_worker_test -j
rtk build-x64/bin/Debug/tcpquic_stream_lifetime_test.exe
rtk build-x64/bin/Debug/tcpquic_windows_relay_worker_test.exe
rtk rg -n "Generation\.(load|store)" src/tunnel/windows_relay_worker.cpp src/tunnel/relay.cpp
```

Expected: 两个 executable exit code 0；最后一条 `rg` 无输出。

- [ ] **Step 5: 提交公共依赖与 Windows 编译基线**

```bash
rtk git add src/tunnel/windows_relay_worker.h src/tunnel/windows_relay_worker.cpp src/tunnel/relay.h src/tunnel/relay.cpp src/unittest/windows_relay_worker_test.cpp src/unittest/stream_lifetime_test.cpp
rtk git commit -m "fix(windows): align terminal adapter prerequisites"
```

### Task 2: 先写 fatal handoff 与正常 FIN half-close 的确定性测试

**Files:**
- Modify: `src/tunnel/windows_relay_worker.h`
- Test: `src/unittest/windows_relay_worker_test.cpp`
- Test: `src/unittest/tcp_tunnel_test.cpp`

**Interfaces:**
- Consumes: `TqTerminalShutdownResult`；现有 `TestPostWorkerQueueBlockForTest()` barrier 和真实 socket pair helper。
- Produces: `TqWindowsTerminalHandoffSnapshot`、`TestBlockTerminalDowncallForTest()`、`TestReleaseTerminalDowncallForTest()`，为 Task 3 实现提供精确断言。

- [ ] **Step 1: 增加只读 handoff 测试快照**

```cpp
struct TqWindowsTerminalHandoffSnapshot {
    bool DataPlaneStopped{false};
    bool HandoffComplete{false};
    bool LocalOwnershipDrained{false};
    uint64_t BeginCalls{0};
    uint64_t FatalWatchdogArmed{0};
    uint64_t PendingOverlapped{0};
    uint64_t PendingCommands{0};
};
```

- [ ] **Step 2: 写 fatal close 与 terminal down-call 重入失败测试**

```cpp
bool TestWindowsFatalCloseHandoffBeforeRetire() {
    WindowsManagedRelayFixture f;
    f.BlockBeginTerminalShutdownAfterReservation();
    f.InjectTcpCompletionError(WSAECONNRESET);
    f.WaitUntilTerminalShutdownReserved();
    const auto during = f.WorkerSnapshot();
    if (!during.DataPlaneStopped || during.HandoffComplete) return false;
    if (f.TunnelCanReap() || f.StreamCloseCount() != 0) return false;
    f.DispatchShutdownCompleteReentrant();
    f.ReleaseBeginTerminalShutdown();
    return f.BeginTerminalResult().AlreadyTerminal &&
           f.BeginTerminalCallCount() == 1 &&
           f.WaitForAllOwnershipZero();
}
```

- [ ] **Step 3: 写正常 half-close 保护测试**

```cpp
bool TestWindowsTcpFinKeepsReverseDirectionAndSkipsFatalWatchdog() {
    WindowsRealSocketRelayFixture f;
    f.ShutdownClientSend();
    if (!f.WaitForQuicFinSubmittedExactlyOnce()) return false;
    f.SendReversePayload("reverse-after-fin");
    if (f.ReadClientPayload() != "reverse-after-fin") return false;
    const auto s = f.TerminalHandoffSnapshot();
    return s.BeginCalls == 0 && s.FatalWatchdogArmed == 0 &&
           !s.HandoffComplete && f.FatalResetCount() == 0;
}

// 接入当前自有 main()；失败码在本文件内保持唯一。
if (!TestWindowsFatalCloseHandoffBeforeRetire()) return 620;
if (!TestWindowsTcpFinKeepsReverseDirectionAndSkipsFatalWatchdog()) return 621;
```

- [ ] **Step 4: 运行并确认两个测试在现实现上失败**

Run:

```powershell
rtk cmake --build build-x64 --config Debug --target tcpquic_windows_relay_worker_test tcpquic_tunnel_test -j
rtk build-x64/bin/Debug/tcpquic_windows_relay_worker_test.exe
```

Expected: fatal case 因 `FinalizeWorkerStopOnWorkerThread()` 人工 terminal 或 reaper 过早通过而失败；half-close case 必须保持通过，作为后续每个 Task 的不回归门禁。

- [ ] **Step 5: 仅提交失败测试**

```bash
rtk git add src/tunnel/windows_relay_worker.h src/unittest/windows_relay_worker_test.cpp src/unittest/tcp_tunnel_test.cpp
rtk git commit -m "test(windows): expose fatal terminal handoff gap"
```

### Task 3: 接入公共 terminal handoff，并分离 local retire 与真实 terminal

**Files:**
- Modify: `src/tunnel/windows_relay_worker.h`
- Modify: `src/tunnel/windows_relay_worker.cpp`
- Modify: `src/tunnel/tcp_tunnel.cpp`
- Modify: `src/tunnel/tunnel_reaper.h`
- Modify: `src/tunnel/tunnel_reaper.cpp`
- Test: `src/unittest/windows_relay_worker_test.cpp`
- Test: `src/unittest/tcp_tunnel_test.cpp`

**Interfaces:**
- Consumes: registration 中同一个 `std::shared_ptr<TqTerminalLedger>`、公共 `TqTerminalSink::Create(std::weak_ptr<TqStreamLifetime>, std::shared_ptr<TqTerminalLedger>) noexcept`、`BeginTerminalShutdown()` 和 `TqTerminalShutdownResult`。
- Produces: `TqWindowsTerminalHandoffState::FreezeAndBegin()`；reaper 消费的三个布尔事实；后续 IOCP Task 消费的 independent sink/control owner。

- [ ] **Step 1: 定义 Windows handoff state，不复制公共 ledger**

```cpp
struct TqWindowsTerminalHandoffState final {
    std::atomic<bool> DataPlaneStopped{false};
    std::atomic<bool> HandoffComplete{false};
    std::atomic<bool> LocalOperationOwnershipTransferredOrDrained{false};
    std::shared_ptr<TqTerminalLedger> TerminalLedger;
    std::shared_ptr<TqTerminalSink> TerminalSink;
    std::shared_ptr<TqTerminalEscalation> Escalation;
    TqTerminalIdentity Identity{};
};

// 在现有 TqRelayStopControl 中增加一个 Windows release-facts owner；
// 它不拥有公共 terminal ledger，也不反向持有 tunnel/relay/worker。
std::shared_ptr<TqWindowsTerminalHandoffState> WindowsTerminalHandoff;

bool TqWindowsRelayCanReleaseTunnel(
    const TqWindowsTerminalHandoffState& state) noexcept {
    return state.DataPlaneStopped.load(std::memory_order_acquire) &&
           state.HandoffComplete.load(std::memory_order_acquire) &&
           state.LocalOperationOwnershipTransferredOrDrained.load(std::memory_order_acquire);
}
```

registration prepare 时只创建一次公共 sink，并把同一个 ledger/sink 放入 handoff state：

```cpp
relay->TerminalHandoff->TerminalLedger = registration.TerminalLedger;
relay->TerminalHandoff->TerminalSink = TqTerminalSink::Create(
    std::weak_ptr<TqStreamLifetime>{registration.StreamOwner},
    relay->TerminalHandoff->TerminalLedger);
if (relay->TerminalHandoff->TerminalLedger == nullptr ||
    relay->TerminalHandoff->TerminalSink == nullptr) {
    return false;
}
```

- [ ] **Step 2: 用统一 helper 替换 fatal/admin/worker-stop 的拼接 shutdown**

```cpp
TqTerminalShutdownResult TqWindowsRelayWorker::BeginFatalTerminalHandoff(
    const std::shared_ptr<RelayContext>& relay,
    uint64_t errorCode) noexcept {
    relay->Closing.store(true, std::memory_order_release);
    relay->TerminalHandoff->DataPlaneStopped.store(true, std::memory_order_release);
    SwitchRelayTargetToTerminalSink(relay);
    auto result = relay->StreamOwner->BeginTerminalShutdown(
        errorCode,
        relay->TerminalHandoff->TerminalSink,
        relay->TerminalHandoff->Escalation);
    relay->TerminalHandoff->HandoffComplete.store(
        result.Submitted || result.AlreadyTerminal || result.RetryScheduled,
        std::memory_order_release);
    return result;
}
```

调用点必须覆盖 active TCP hard error、admin stop、publish 后 activation failure、worker stop；调用 `BeginTerminalShutdown()` 时不得持 `Lock_`、`ControlCommandLock_`、binding precommit lock 或 tunnel lock。

- [ ] **Step 3: 删除人工 terminal publication并保留真实 callback 路径**

```cpp
void TqWindowsRelayWorker::FinalizeWorkerStopOnWorkerThread() {
    DrainTerminalShutdownSink();
    DrainMaintenanceQueueOnStop();
    assert(WorkerIocpOwnershipIsZero());
    // 不调用 PublishTerminalAndTakeTarget()，不清公共 retention。
    Relays_.clear();
    RetiredCallbacks_.clear();
    StreamCallbackEndpoint_->CloseAndWait();
    ::CloseHandle(static_cast<HANDLE>(Iocp_));
    Iocp_ = nullptr;
}
```

`ApplyTerminalLogicalDetach()` 只在真实 terminal operation/sink notification 后设置 `StreamShutdownComplete`；active close 不 reset `StreamOwner`。

- [ ] **Step 4: 把三事实 predicate 接入 tunnel reaper**

```cpp
bool TqTunnelContext::CanReap() const noexcept {
    if (RelayHandle.Backend != TqRelayBackendType::WindowsWorker) {
        return ExistingPlatformPredicate();
    }
    const auto control = std::atomic_load(&RelayHandle.Control);
    const auto facts = control ? std::atomic_load(&control->WindowsTerminalHandoff) : nullptr;
    return control && facts && control->Stop.load(std::memory_order_acquire) &&
           facts->HandoffComplete.load(std::memory_order_acquire) &&
           facts->LocalOperationOwnershipTransferredOrDrained.load(std::memory_order_acquire);
}
```

在 `TqRelayStopControl` 中增加的两个事实只镜像 handoff 结果，不实现公共 terminal phase；terminal callback 无需等待 tunnel reaper。

- [ ] **Step 5: 运行 fatal、reentry、activation failure、half-close 测试**

Run:

```powershell
rtk cmake --build build-x64 --config Debug --target tcpquic_windows_relay_worker_test tcpquic_tunnel_test -j
rtk build-x64/bin/Debug/tcpquic_windows_relay_worker_test.exe
rtk build-x64/bin/Debug/tcpquic_tunnel_test.exe
```

Expected: 全部 exit code 0；fatal shutdown flags 恰为 `ABORT|IMMEDIATE`；half-close 的 `BeginCalls` 和 watchdog armed 均为 0。

- [ ] **Step 6: 提交 Windows terminal handoff adapter**

```bash
rtk git add src/tunnel/windows_relay_worker.h src/tunnel/windows_relay_worker.cpp src/tunnel/tcp_tunnel.cpp src/tunnel/tunnel_reaper.h src/tunnel/tunnel_reaper.cpp src/unittest/windows_relay_worker_test.cpp src/unittest/tcp_tunnel_test.cpp
rtk git commit -m "feat(windows): hand off fatal streams before relay retirement"
```

### Task 4: 把 control command 与 OVERLAPPED 统一为 typed IOCP ownership 并完成 stop drain

**Files:**
- Modify: `src/tunnel/windows_relay_worker.h`
- Modify: `src/tunnel/windows_relay_worker.cpp`
- Test: `src/unittest/windows_relay_worker_test.cpp`
- Test: `src/unittest/windows_reactor_test.cpp`

**Interfaces:**
- Consumes: Task 1 的 `TqWindowsCommandEnvelope<Result>`；Task 3 的 independent handoff state。
- Produces: `TqWindowsIocpEnvelope` typed payload；`WorkerIocpOwnershipIsZero()` 的完整门禁；Task 6 metrics 消费的 pending counts/oldest age。

- [ ] **Step 1: 写 GQCS failure、command-stop 与 pending Winsock 失败测试**

```cpp
bool TestWindowsFalseGqcsWithOverlappedStillSettles() {
    WindowsRealSocketRelayFixture f;
    f.PostPendingRecv();
    f.CancelSocketIo();
    const auto c = f.DequeueOneCompletion();
    if (c.Ok || c.Overlapped == nullptr) return false;
    f.DispatchCompletion(c);
    return f.PendingRecvCount() == 0 && f.OperationDestroyCount() == 1;
}

bool TestWindowsPostedRegisterCommandCompletesDuringStop() {
    WindowsManagedRelayFixture f;
    auto command = f.PostRegisterAndBlockWorker();
    auto stop = f.BeginStopAsync();
    f.ReleaseWorker();
    return command->Wait() == TqWindowsCommandState::Stopped &&
           stop.Wait() && f.PendingCommandCount() == 0;
}

if (!TestWindowsFalseGqcsWithOverlappedStillSettles()) return 630;
if (!TestWindowsPostedRegisterCommandCompletesDuringStop()) return 631;
```

- [ ] **Step 2: 用 typed variant 替换 `IoOperation::Control`**

```cpp
using TqWindowsIocpPayload = std::variant<
    std::monostate,
    std::shared_ptr<TqWindowsCommandEnvelope<TqWindowsRelayRegistrationResult>>,
    std::shared_ptr<TqWindowsCommandEnvelope<TqWindowsRelayWorkerSnapshot>>,
    std::shared_ptr<TerminalCleanupRecord>,
    std::shared_ptr<TqWindowsPendingQuicReceive>>;

struct TqWindowsRelayWorker::IoOperation {
    OVERLAPPED Overlapped{};
    TqWindowsIocpOperationType Event{TqWindowsIocpOperationType::TcpRecv};
    TqWindowsIocpPayload Payload;
    std::shared_ptr<RelayContext> Relay;
    uint64_t RelayId{0};
    uint64_t Generation{0};
};
```

删除 `void* Control`、栈上 `RegisterRelayCommand`、栈上 `SnapshotCommand`；post failure 将 shared envelope 完成为 `PostFailed`。

- [ ] **Step 3: 线性化 stop admission 并登记全部已 post envelope**

```cpp
enum class TqWindowsCommandAdmission : uint8_t {
    Accepting,
    Stopping,
    Stopped,
};

bool TqWindowsRelayWorker::PostCommand(
    std::unique_ptr<IoOperation> op,
    const std::shared_ptr<TqWindowsCommandBase>& command) {
    std::lock_guard<std::mutex> guard(ControlCommandLock_);
    if (CommandAdmission_ != TqWindowsCommandAdmission::Accepting || Iocp_ == nullptr) {
        command->CompleteStopped();
        return false;
    }
    PendingCommands_.emplace(command->Id(), command);
    if (!::PostQueuedCompletionStatus(Iocp_, 0, 0, &op->Overlapped)) {
        PendingCommands_.erase(command->Id());
        command->CompletePostFailed(::GetLastError());
        return false;
    }
    op.release();
    return true;
}
```

stop 在同一锁下把 admission 置为 `Stopping` 并 post marker；marker 前的 command 必须执行或显式完成为 `Stopped`。

- [ ] **Step 4: 修复 GQCS dispatch 与 socket cancel/drain**

```cpp
BOOL ok = ::GetQueuedCompletionStatus(Iocp_, &bytes, &key, &overlapped, waitMs);
const DWORD error = ok ? ERROR_SUCCESS : ::GetLastError();
if (overlapped == nullptr) {
    HandleNoOperationWake(ok, error);
    continue;
}
std::unique_ptr<IoOperation> op(reinterpret_cast<IoOperation*>(overlapped));
DispatchCompletedOperation(*op, bytes, error); // ok==FALSE 也执行 settle
```

`CancelIoEx(..., nullptr)` 的 `TRUE` 与 `ERROR_NOT_FOUND` 均继续 closesocket；已提交 recv/send 只有在此 dispatch 后 decrement 并释放 buffer。

- [ ] **Step 5: 扩展 stop 归零 predicate**

```cpp
return PendingCommands_.empty() &&
       PendingOverlappedTcpRecvs_ == 0 &&
       PendingOverlappedTcpSends_ == 0 &&
       PendingCallbackOps_ == 0 &&
       TerminalOperationPendingCount_ == 0 &&
       TerminalShutdownSinkPendingCount_ == 0 &&
       MaintenanceQueue_.empty();
```

stop 前成功 post 的 terminal work 必须被 worker 执行，或把 shared record 转交 worker-independent common sink；只有完成转移才计为本地 ownership drained。

- [ ] **Step 6: 运行真实 Winsock 与 stop 并发矩阵**

Run:

```powershell
rtk cmake --build build-x64 --config Debug --target tcpquic_windows_relay_worker_test tcpquic_windows_reactor_test -j
rtk build-x64/bin/Debug/tcpquic_windows_relay_worker_test.exe
rtk build-x64/bin/Debug/tcpquic_windows_reactor_test.exe
```

Expected: 全部 exit code 0；stop 返回时 command、recv/send OVERLAPPED、callback op、terminal op、maintenance 均为 0；success 与 `ERROR_OPERATION_ABORTED` 两种竞争结果都只 settle 一次。

- [ ] **Step 7: 提交 typed IOCP ownership**

```bash
rtk git add src/tunnel/windows_relay_worker.h src/tunnel/windows_relay_worker.cpp src/unittest/windows_relay_worker_test.cpp src/unittest/windows_reactor_test.cpp
rtk git commit -m "fix(windows): drain typed iocp ownership through worker stop"
```

### Task 5: 实现 generation-safe connection escalation 与旧 generation 隔离

**Files:**
- Modify: `src/protocol/quic_session.h`
- Modify: `src/protocol/quic_session.cpp`
- Modify: `src/tunnel/tcp_tunnel.cpp`
- Modify: `src/tunnel/windows_relay_worker.h`
- Test: `src/unittest/quic_session_reconnect_test.cpp`
- Test: `src/unittest/windows_relay_worker_test.cpp`

**Interfaces:**
- Consumes: 公共 `TqTerminalEscalation::RequestConnectionShutdown()`；client `ConnectionSlot::Generation` 与 server connection registry。
- Produces: `TqConnectionShutdownToken`、`TqConnectionShutdownEndpoint`、`TqWindowsTerminalEscalation`；不暴露 `MsQuicConnection*`。

- [ ] **Step 1: 定义 stable connection token 与 endpoint**

```cpp
struct TqConnectionShutdownToken {
    uint64_t ConnectionId{0};
    uint64_t Generation{0};
};

class TqConnectionShutdownEndpoint {
public:
    virtual ~TqConnectionShutdownEndpoint() = default;
    virtual bool RequestShutdown(
        TqConnectionShutdownToken token,
        QUIC_STATUS streamStatus,
        uint64_t errorCode) noexcept = 0;
};

class TqWindowsTerminalEscalation final : public TqTerminalEscalation {
public:
    TqWindowsTerminalEscalation(
        std::weak_ptr<TqConnectionShutdownEndpoint> endpoint,
        TqConnectionShutdownToken token) noexcept;
    void RequestConnectionShutdown(
        uint64_t connectionId,
        uint64_t streamId,
        QUIC_STATUS streamStatus,
        uint64_t errorCode) noexcept override;
private:
    std::weak_ptr<TqConnectionShutdownEndpoint> Endpoint_;
    TqConnectionShutdownToken Token_;
    std::atomic<bool> Requested_{false};
};
```

- [ ] **Step 2: 写 slot reuse 与 once-only escalation 失败测试**

```cpp
bool TestOldTerminalWatchdogCannotShutdownReusedSlot() {
    FakeConnectionShutdownEndpoint endpoint;
    auto old = endpoint.TokenForSlot(0);       // generation 7
    endpoint.ReconnectSlot(0);                 // generation 8
    TqWindowsTerminalEscalation escalation(endpoint.Weak(), old);
    escalation.RequestConnectionShutdown(old.ConnectionId, 17, QUIC_STATUS_ABORTED, 42);
    escalation.RequestConnectionShutdown(old.ConnectionId, 17, QUIC_STATUS_ABORTED, 42);
    return endpoint.ShutdownCalls() == 0 && endpoint.GenerationMismatchCount() == 1;
}

if (!TestOldTerminalWatchdogCannotShutdownReusedSlot()) return 640;
```

- [ ] **Step 3: client/server lookup 在锁内校验、锁外 down-call**

```cpp
bool ClientConnectionShutdownEndpoint::RequestShutdown(
    TqConnectionShutdownToken token,
    QUIC_STATUS,
    uint64_t errorCode) noexcept {
    MsQuicConnection* connection = nullptr;
    {
        std::lock_guard<std::mutex> guard(State_->Lock);
        if (token.ConnectionId >= State_->Slots.size()) return false;
        auto& slot = State_->Slots[token.ConnectionId];
        if (slot.Generation != token.Generation || !slot.Connection) return false;
        connection = slot.Connection.get();
        slot.RetryScheduled = true;
    }
    connection->Shutdown(errorCode, QUIC_CONNECTION_SHUTDOWN_FLAG_NONE);
    return true;
}
```

server registry 同样为每条 record 增加 immutable generation；endpoint 在锁内取得安全 owner/guard，锁外调用 connection shutdown。禁止把 `connection` 存入 escalation object。

- [ ] **Step 4: tunnel 创建 terminal identity/escalation，并一路传入 Windows registration**

```cpp
registration.TerminalIdentity = TqTerminalIdentity{
    StreamId(), TraceTunnelId, stableConnectionId,
    Role,
    TqRelayBackendType::WindowsWorker};
registration.TerminalEscalation = std::make_shared<TqWindowsTerminalEscalation>(
    connectionEndpoint, TqConnectionShutdownToken{stableConnectionId, connectionGeneration});
```

- [ ] **Step 5: 运行 generation 与 watchdog escalation 测试**

Run:

```powershell
rtk cmake --build build-x64 --config Debug --target tcpquic_quic_session_reconnect_test tcpquic_windows_relay_worker_test tcpquic_tunnel_test -j
rtk build-x64/bin/Debug/tcpquic_quic_session_reconnect_test.exe
rtk build-x64/bin/Debug/tcpquic_windows_relay_worker_test.exe
```

Expected: 全部 exit code 0；旧 generation 不调用新 connection；同一 watchdog 只升级一次；connection 已 closing 时不重复 down-call。

- [ ] **Step 6: 提交 connection escalation adapter**

```bash
rtk git add src/protocol/quic_session.h src/protocol/quic_session.cpp src/tunnel/tcp_tunnel.cpp src/tunnel/windows_relay_worker.h src/unittest/quic_session_reconnect_test.cpp src/unittest/windows_relay_worker_test.cpp
rtk git commit -m "feat(windows): escalate terminal timeout by connection generation"
```

### Task 6: 补齐 Windows Admin retention 明细、metrics 与过滤

**Files:**
- Modify: `src/runtime/relay_metrics.h`
- Modify: `src/runtime/relay_metrics.cpp`
- Modify: `src/runtime/admin_http.cpp`
- Test: `src/unittest/admin_http_test.cpp`
- Test: `src/unittest/windows_relay_worker_test.cpp`

**Interfaces:**
- Consumes: 公共 `TqTerminalRetentionDetail` snapshot；Task 4 Windows IOCP ownership snapshot。
- Produces: `GET /api/v1/relay/terminal-retentions`；backend/connection_id/tunnel_id/terminal_phase filters；Windows backend diagnostic fields。

- [ ] **Step 1: 写 Admin JSON 精确契约测试**

```cpp
const auto response = admin.Get(
    "/api/v1/relay/terminal-retentions?backend=windows&connection_id=2"
    "&tunnel_id=133&terminal_phase=shutdown_submitted");
const auto body = nlohmann::json::parse(response.Body);
const auto& item = body.at("terminal_retentions").at(0);
CHECK(item.at("stream_id") == 17);
CHECK(item.at("backend") == "windows");
CHECK(item.at("shutdown_intent") == "abort_both_immediate");
CHECK(item.at("watchdog_state") == "armed");
CHECK(item.at("pending_overlapped") == 2);
CHECK(item.at("pending_commands") == 1);

if (!TestWindowsTerminalMetricsExactlyOnce()) return 650;
```

- [ ] **Step 2: 增加 Windows 聚合字段**

```cpp
struct TqWindowsTerminalMetrics {
    uint64_t HandoffStarted{0};
    uint64_t HandoffCompleted{0};
    uint64_t HandoffFailed{0};
    uint64_t ShutdownSubmitted{0};
    uint64_t ShutdownPending{0};
    uint64_t ShutdownSyncFailure{0};
    uint64_t ShutdownRetry{0};
    uint64_t WatchdogArmed{0};
    uint64_t WatchdogCanceled{0};
    uint64_t WatchdogTimeout{0};
    uint64_t ConnectionEscalation{0};
    uint64_t TerminalTimeoutPending{0};
    uint64_t DuplicateSuppressed{0};
    uint64_t ExactlyOnceViolation{0};
    uint64_t PendingOverlapped{0};
    uint64_t PendingCommands{0};
};
```

- [ ] **Step 3: 快照复制后锁外过滤与序列化**

```cpp
auto details = TqStreamLifetime::SnapshotTerminalRetentionDetails();
std::erase_if(details, [&](const auto& d) {
    return (filter.Backend && d.Identity.Backend != *filter.Backend) ||
           (filter.ConnectionId && d.Identity.ConnectionId != *filter.ConnectionId) ||
           (filter.TunnelId && d.Identity.TunnelId != *filter.TunnelId) ||
           (filter.TerminalPhase && d.TerminalPhase != *filter.TerminalPhase);
});
return SerializeTerminalRetentionDetails(details);
```

Admin 线程不得解引用 relay/tunnel/stream/connection；multi-worker count 求和，oldest age 与 exactly-once violation 求最大值。

- [ ] **Step 4: 运行 Admin 与 metrics 测试**

Run:

```powershell
rtk cmake --build build-x64 --config Debug --target tcpquic_admin_http_test tcpquic_windows_relay_worker_test -j
rtk build-x64/bin/Debug/tcpquic_admin_http_test.exe
rtk build-x64/bin/Debug/tcpquic_windows_relay_worker_test.exe
```

Expected: exit code 0；filter 仅返回匹配项；concurrent terminal snapshot JSON 可解析；`exactly_once_violation == 0`。

- [ ] **Step 5: 提交 Admin 与 metrics**

```bash
rtk git add src/runtime/relay_metrics.h src/runtime/relay_metrics.cpp src/runtime/admin_http.cpp src/unittest/admin_http_test.cpp src/unittest/windows_relay_worker_test.cpp
rtk git commit -m "feat(admin): expose windows terminal convergence state"
```

### Task 7: 增加精确事故回归与公共失败策略故障注入

**Files:**
- Create: `tests/windows/terminal_incident_target.py`
- Create: `tests/windows/terminal_incident_regression.ps1`
- Modify: `src/unittest/windows_relay_worker_test.cpp`
- Modify: `scripts/test-tcpquic-proxy-windows.ps1`

**Interfaces:**
- Consumes: Admin endpoint、公共 test clock/shutdown status hook、Windows terminal down-call/worker barrier。
- Produces: 事故证据目录，包含 timeline、client/server metrics、retention detail、socket snapshot、trace 与 process exit status。

- [ ] **Step 1: 添加 shutdown status、terminal suppression 与 queue-full hooks**

```cpp
struct TqWindowsTerminalFaultHooks {
    std::deque<QUIC_STATUS> ShutdownStatuses;
    bool SuppressShutdownComplete{false};
    bool FailNextTerminalPost{false};
    bool HoldWorkerStop{false};
};
```

测试注入序列 `{QUIC_STATUS_OUT_OF_MEMORY, QUIC_STATUS_OUT_OF_MEMORY, QUIC_STATUS_OUT_OF_MEMORY}` 时，公共 fake clock 必须观察 10/50/250 ms 三次 retry，随后 exactly-once connection escalation；`PENDING` 必须 arm watchdog，不进入 sync failure。

- [ ] **Step 2: 写精确 retry/watchdog/terminal-wins 测试**

```cpp
bool TestWindowsTerminalFailurePolicyUsesCommonScheduler() {
    WindowsManagedRelayFixture f;
    f.SetShutdownStatuses({QUIC_STATUS_OUT_OF_MEMORY,
                           QUIC_STATUS_OUT_OF_MEMORY,
                           QUIC_STATUS_OUT_OF_MEMORY});
    f.InjectTcpCompletionError(WSAECONNRESET);
    f.AdvanceCommonClock(10ms);
    f.AdvanceCommonClock(50ms);
    f.AdvanceCommonClock(250ms);
    return f.ShutdownAttempts() == 3 &&
           f.RetryDelays() == std::vector{10ms, 50ms, 250ms} &&
           f.ConnectionEscalations() == 1 && f.StreamCloseCount() == 0;
}

if (!TestWindowsTerminalFailurePolicyUsesCommonScheduler()) return 660;
if (!TestWindowsTerminalPostFallbackAfterWorkerStop()) return 661;
```

- [ ] **Step 3: 实现先 FIN 后 RST 的可控 target**

```python
conn, _ = listener.accept()
while conn.recv(65536):
    pass  # 收到 proxy 转发的 client FIN
conn.setsockopt(socket.SOL_SOCKET, socket.SO_LINGER, struct.pack("ii", 1, 0))
timeline.write("target_rst_after_client_fin\n")
conn.close()
```

- [ ] **Step 4: PowerShell 回归严格断言两端归零**

```powershell
$deadline = (Get-Date).AddSeconds(10)
do {
  $client = Invoke-RestMethod "$ClientAdmin/api/v1/relay/metrics"
  $server = Invoke-RestMethod "$ServerAdmin/api/v1/relay/metrics"
  $zero = $client.active_relays -eq 0 -and $server.active_relays -eq 0 -and
          $client.terminal_sink_pending -eq 0 -and $server.terminal_sink_pending -eq 0 -and
          $client.watchdog_pending -eq 0 -and $server.watchdog_pending -eq 0 -and
          $client.send_completion_registry_count -eq 0 -and
          $server.send_completion_registry_count -eq 0
  if ($zero) { break }
  Start-Sleep -Milliseconds 100
} while ((Get-Date) -lt $deadline)
if (-not $zero) { throw "terminal resources did not return to baseline" }
if ($client.terminal_timeout_pending -ne 0 -or $server.terminal_timeout_pending -ne 0) {
  throw "unexpected terminal timeout"
}
```

脚本还必须校验 timeline 顺序为 client TCP FIN -> client QUIC FIN -> server target `SHUT_WR` -> target RST -> server `ABORT|IMMEDIATE` -> server local terminal -> client peer abort/terminal；`Get-NetTCPConnection` 不存在相关 `CloseWait`；destructor/StreamClose 各一次且时间晚于 callback return。

- [ ] **Step 5: 运行 focused 故障与事故回归**

Run:

```powershell
rtk build-x64/bin/Debug/tcpquic_windows_relay_worker_test.exe
rtk powershell -ExecutionPolicy Bypass -File tests/windows/terminal_incident_regression.ps1 -Bin build-x64/bin/Debug/tcpquic-proxy.exe -EvidenceDir artifacts/windows-terminal-incident
```

Expected: 三条命令 exit code 0；事故结束 10 秒内 active tunnel/relay、sink、watchdog、send registry、retention 和 pending OVERLAPPED 回到基线；无 `CLOSE-WAIT`、timeout 或 exactly-once violation。

- [ ] **Step 6: 提交故障注入与事故回归**

```bash
rtk git add tests/windows/terminal_incident_target.py tests/windows/terminal_incident_regression.ps1 src/unittest/windows_relay_worker_test.cpp scripts/test-tcpquic-proxy-windows.ps1
rtk git commit -m "test(windows): reproduce fin then target reset convergence"
```

### Task 8: 完成 Windows 构建、Verifier、k6、soak 与文档发布门禁

**Files:**
- Create: `tests/k6/windows-terminal-convergence.js`
- Modify: `scripts/test-tcpquic-proxy-windows.ps1`
- Modify: `docs/relay_win.md`
- Modify: `src/docs/thread_model_cn.md`

**Interfaces:**
- Consumes: Tasks 1–7 的 tests、Admin fields、fault hooks 和事故脚本。
- Produces: 可重复执行的 Release/Debug、Verifier、性能、容量与 soak 证据；不改变 production API。

- [ ] **Step 1: 写 k6 workload 与硬门限**

```javascript
const scenarioName = __ENV.SCENARIO || 'baseline';
const scenarios = {
  baseline: { executor: 'constant-vus', vus: 100, duration: '5m' },
  peak: { executor: 'constant-arrival-rate', rate: 50, timeUnit: '1s', duration: '10m', preAllocatedVUs: 500 },
  spike: { executor: 'ramping-vus', stages: [{ duration: '30s', target: 1000 }, { duration: '2m', target: 1000 }] },
  stress: { executor: 'ramping-vus', stages: [
    { duration: '2m', target: 250 }, { duration: '2m', target: 500 },
    { duration: '2m', target: 750 }, { duration: '2m', target: 1000 },
    { duration: '2m', target: 1250 },
  ] },
  soak: { executor: 'constant-arrival-rate', rate: 100, timeUnit: '1s', duration: '30m', preAllocatedVUs: 500 },
};
if (!scenarios[scenarioName]) throw new Error(`unknown SCENARIO=${scenarioName}`);

export const options = {
  scenarios: { [scenarioName]: scenarios[scenarioName] },
  thresholds: {
    checks: ['rate>0.999'],
    dropped_iterations: ['count==0'],
    tunnel_connect_ms: ['p(95)<500', 'p(99)<1000'],
    fatal_terminal_ms: ['p(95)<1000', 'p(99)<5000'],
  },
};
```

payload selector 严格按 50/40/10 选择 1 KiB/64 KiB/1 MiB；80% 正常 FIN、20% RST；冷启动和预热各跑一轮。stress 另用每 2 分钟增加 250 并发的命令参数运行直到首次门禁失败。

- [ ] **Step 2: 清除 Windows relay 测试中本范围的禁用用例**

Run:

```powershell
rtk rg -n "#if 0|pending-register wake crashes|half-close teardown hangs|backpressure/fatal-send teardown hangs|tcp-recv-pool Stop hangs|zstd flush socket relay Stop hangs" src/unittest/windows_relay_worker_test.cpp
```

Expected: 无输出；pending register、StopRelay+Snapshot、half-close、backpressure/fatal-send、TCP recv pool、zstd flush 和 `CancelIoEx(ERROR_NOT_FOUND)` 都已恢复并使用 Task 4 drain 语义。

- [ ] **Step 3: 运行真实 Windows focused 与全量构建矩阵**

Run:

```powershell
rtk cmake --build build-x64 --config Debug --target tcpquic_stream_lifetime_test tcpquic_windows_relay_worker_test tcpquic_windows_reactor_test tcpquic_tunnel_test tcpquic_quic_session_reconnect_test tcpquic_admin_http_test tcpquic_client_tunnel_open_test tcpquic_speed_test_test tcpquic_router_runtime_test -j
rtk cmake --build build-x64 --config Release --target tcpquic-proxy tcpquic_stream_lifetime_test tcpquic_windows_relay_worker_test tcpquic_windows_reactor_test tcpquic_tunnel_test tcpquic_quic_session_reconnect_test tcpquic_admin_http_test tcpquic_client_tunnel_open_test tcpquic_speed_test_test tcpquic_router_runtime_test -j
rtk build-x64/bin/Debug/tcpquic_stream_lifetime_test.exe
rtk build-x64/bin/Debug/tcpquic_windows_relay_worker_test.exe
rtk build-x64/bin/Debug/tcpquic_windows_reactor_test.exe
rtk build-x64/bin/Debug/tcpquic_tunnel_test.exe
rtk build-x64/bin/Debug/tcpquic_quic_session_reconnect_test.exe
rtk build-x64/bin/Debug/tcpquic_admin_http_test.exe
rtk build-x64/bin/Debug/tcpquic_client_tunnel_open_test.exe
rtk build-x64/bin/Debug/tcpquic_speed_test_test.exe
rtk build-x64/bin/Debug/tcpquic_router_runtime_test.exe
rtk build-x64/bin/Release/tcpquic_stream_lifetime_test.exe
rtk build-x64/bin/Release/tcpquic_windows_relay_worker_test.exe
rtk build-x64/bin/Release/tcpquic_windows_reactor_test.exe
rtk build-x64/bin/Release/tcpquic_tunnel_test.exe
rtk build-x64/bin/Release/tcpquic_quic_session_reconnect_test.exe
rtk build-x64/bin/Release/tcpquic_admin_http_test.exe
rtk build-x64/bin/Release/tcpquic_client_tunnel_open_test.exe
rtk build-x64/bin/Release/tcpquic_speed_test_test.exe
rtk build-x64/bin/Release/tcpquic_router_runtime_test.exe
```

Expected: build 与 18 个 executable 全部 exit code 0；没有 compile/link failure、hang 或被跳过的 Windows terminal case。

- [ ] **Step 4: 运行 Application Verifier 与 Windows ASan**

Run:

```powershell
appverif.exe /verify tcpquic_windows_relay_worker_test.exe /tests Heaps Handles Locks Exceptions
1..100 | ForEach-Object { rtk build-x64/bin/Debug/tcpquic_windows_relay_worker_test.exe }
appverif.exe /verify tcpquic-proxy.exe /tests Heaps Handles Locks Exceptions
rtk powershell -ExecutionPolicy Bypass -File tests/windows/terminal_incident_regression.ps1 -Bin build-x64/bin/Debug/tcpquic-proxy.exe -EvidenceDir artifacts/windows-terminal-verifier
```

Expected: 0 UAF、0 double free、0 invalid handle、0 lock misuse、0 payload corruption；逻辑 ownership 由 Admin/metrics 归零证明。

- [ ] **Step 5: 运行 k6 baseline/peak/spike/stress/soak**

Run:

```powershell
k6 run -e SCENARIO=baseline -e PROXY_ADDR=127.0.0.1:18081 tests/k6/windows-terminal-convergence.js
k6 run -e SCENARIO=peak -e PROXY_ADDR=127.0.0.1:18081 tests/k6/windows-terminal-convergence.js
k6 run -e SCENARIO=spike -e PROXY_ADDR=127.0.0.1:18081 tests/k6/windows-terminal-convergence.js
k6 run -e SCENARIO=stress -e PROXY_ADDR=127.0.0.1:18081 tests/k6/windows-terminal-convergence.js
k6 run -e SCENARIO=soak -e PROXY_ADDR=127.0.0.1:18081 tests/k6/windows-terminal-convergence.js
```

Expected: tunnel 建立 p95 ≤旧基线 105%、p99 ≤110%，吞吐 ≥95%，fatal terminal p95 <1 秒/p99 <5 秒，错误率 <0.1%，dropped iterations=0；CPU/RSS 峰值增幅各≤10%，常驻线程增幅=0；30 分钟 100 tunnel/s soak 中 retention 不单调增长，结束 30 秒后 retained/sink/watchdog/command/OVERLAPPED 全回基线。

- [ ] **Step 6: 更新 Windows 生命周期与线程模型文档**

```markdown
Windows fatal close 顺序：FreezeDataPlane -> publish common terminal sink ->
BeginTerminalShutdown(ABORT|IMMEDIATE) -> classify status -> transfer/drain IOCP ownership ->
publish handoff complete -> tunnel reaper；SHUTDOWN_COMPLETE 独立到达并取消 watchdog。

Windows normal FIN 顺序：TCP EOF -> QUIC_SEND_FLAG_FIN -> 等待 SEND_COMPLETE ->
保留 QUIC->TCP 反向数据 -> peer FIN 后 shutdown(SD_SEND)；不进入 fatal handoff。
```

文档同时列出 Admin 查询示例、5/30 秒告警门限、generation mismatch 诊断和完整验证命令。

- [ ] **Step 7: 最终静态门禁与独立提交**

Run:

```bash
rtk rg -n "PublishTerminalAndTakeTarget" src/tunnel/windows_relay_worker.cpp
rtk rg -n "MsQuicStream\*|MsQuicConnection\*|TqWindowsRelayWorker\*|TqRelayHandle\*" src/tunnel/windows_relay_worker.h
rtk git diff --check
```

Expected: 第一条无 production 调用；第二条只保留 callback 参数/明确 test helper，不出现在 sink、watchdog、escalation、terminal/command envelope；`git diff --check` 无输出。

```bash
rtk git add tests/k6/windows-terminal-convergence.js scripts/test-tcpquic-proxy-windows.ps1 docs/relay_win.md src/docs/thread_model_cn.md
rtk git commit -m "docs(windows): enforce terminal convergence release gates"
```

## Release Evidence Checklist

- [ ] 正常 FIN half-close 反向 payload hash 100%，fatal watchdog armed 为 0。
- [ ] target RST 精确事故链两端在 10 秒内 active tunnel/relay、sink、watchdog、send registry 回基线。
- [ ] stop 返回时 command、TCP recv/send OVERLAPPED、callback op、terminal op、maintenance 全为 0。
- [ ] `SUCCESS/PENDING` 分类正确，10/50/250 ms 三次 retry 与 5 秒 escalation 均 exactly-once。
- [ ] escalation 后 30 秒无 terminal 保留 retention 并发布 `terminal_timeout`，没有 `StreamClose` 强制回收。
- [ ] 旧 connection/relay/control generation 不影响复用槽位或 handle storage。
- [ ] Admin 能按 backend、connection id、tunnel id、terminal phase 定位 retained owner。
- [ ] exactly-once violation、terminal timeout（无故障场景）、shutdown sync failure（无故障场景）均为 0。
- [ ] Debug/Release、Application Verifier、事故回归、k6 五场景与 30 分钟 soak 证据完整。
