# QUIC 客户端固定间隔单飞重连实施计划

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 修复 QUIC 客户端历史重试任务复活问题，使每个 slot 保持固定 3000ms 静默期且任意时刻最多一个有效重试，并证明故障 peer 不影响共享 ingress reactor 上的健康 peer。

**Architecture:** 使用 session 级不可复用 token、`NumericConnectionId + Generation` 和 slot index 组成 `RetryTicket`。所有自动重试通过票据预约；延迟任务只在 `StartSlot` 接管槽位的同一个锁内临界区完成最终票据校验和消费。`SHUTDOWN_COMPLETE` 的身份校验、当前连接清理和下一轮票据预约合并为一个原子状态转换，外部 scheduler 始终在锁外调用。

**Tech Stack:** C++17、MsQuic、CMake/CTest、现有 `QuicClientSession`/`TqClientIngressReactor`、Bash、k6 JavaScript、Linux `pidstat`。

## Global Constraints

- 保持固定 `std::chrono::milliseconds(3000)` 静默期，不增加退避、随机抖动或最大重试次数。
- `retry_scheduled` 必须由 `ActiveRetryToken != 0` 派生，不保留第二个可变事实源。
- scheduler、MsQuic API 和 connection close 等外部调用不得在 `ClientSharedState::Lock` 内执行。
- 手动 reconnect、slot 删除、session stop、新连接发布必须能使尚未接管 slot 的旧票据永久失效。
- 不修改 QUIC 协议、路径选择、业务流格式或 SOCKS/HTTP 监听行为。
- 文档和新增测试说明使用中文；代码标识符及现有日志风格保持英文。
- 不操作开发机上已有 proxy client；系统测试必须使用动态端口、独立临时目录和仅清理自身 PID 的 trap。

## 文件与职责

- `src/protocol/quic_session.h`：定义重试票据、诊断快照、slot active token、测试钩子和条件化启动接口。
- `src/protocol/quic_session.cpp`：实现票据分配、提交、精确回滚、条件化 `StartSlot`、shutdown 原子预约和诊断计数。
- `src/unittest/quic_session_reconnect_test.cpp`：覆盖历史任务、并发抢占、shutdown、ABA、stop、多 slot 和 scheduler 拒绝。
- `src/ingress/client_ingress_reactor.h/.cpp`：提供延迟队列深度和 reactor timeout overshoot 诊断。
- `src/unittest/client_ingress_reactor_test.cpp`：覆盖 delayed queue depth、最大值和排空。
- `src/runtime/router_runtime.h/.cpp`、`src/runtime/client_peer_runtime.h/.cpp`、`src/main.cpp`：把 retry/ingress 诊断添加到现有管理快照。
- `src/unittest/router_runtime_test.cpp`：验证新增 JSON 字段向后兼容。
- `scripts/run-quic-client-retry-single-flight-test.sh`：双 peer 故障隔离、恢复、CPU/RSS/日志增长验收。
- `tests/k6/quic_retry_reconnect.js`：管理 reconnect API 的 baseline、peak、spike、soak 和 breaking-point 场景。

---

### Task 1: 引入唯一重试票据并阻止历史任务复活

**Files:**
- Modify: `src/protocol/quic_session.h:58-70,202-325`
- Modify: `src/protocol/quic_session.cpp:1158,1524-1557,1782-1906,2058-2382`
- Test: `src/unittest/quic_session_reconnect_test.cpp:439-566,1245-1265`

**Interfaces:**
- Produces: `TqClientRetryDiagnostics`、`RetryTicket`、`RetrySubmission`、`ConnectionSlot::ActiveRetryToken`、`ClientSharedState::NextRetryToken`。
- Produces: `ReserveRetryLocked(...)`、`SubmitRetry(...)`、`InvalidateRetryLocked(...)`。
- Produces: `ScheduleStartRetry(size_t,uint64_t,uint64_t)`，生产调用必须提供预期 `NumericConnectionId + Generation`。

- [ ] **Step 1: 写出历史任务不能借用新 token 的失败测试**

在 `quic_session_reconnect_test.cpp` 增加并注册：

```cpp
static int TestStaleRetryCannotBorrowReplacementToken() {
    QuicClientSession session;
    std::vector<std::function<void()>> tasks;
    std::atomic<int> starts{0};
    session.MarkReconnectStartedForTest(1);
    session.SetReconnectTestHooks({[&](size_t index) {
        if (index != 0) return false;
        starts.fetch_add(1, std::memory_order_relaxed);
        return true;
    }});
    session.SetDelayedTaskScheduler(
        [&](std::chrono::milliseconds delay, std::function<void()> task) {
            if (delay != std::chrono::milliseconds(3000)) return false;
            tasks.push_back(std::move(task));
            return true;
        });

    session.ScheduleStartRetryForTest(0);
    if (tasks.size() != 1) return 2001;
    std::string err;
    if (!session.ReconnectConnection("conn-0", err)) return 2002;
    if (starts.load(std::memory_order_relaxed) != 1) return 2003;
    session.ScheduleStartRetryForTest(0);
    if (tasks.size() != 2) return 2004;

    tasks[0]();
    if (starts.load(std::memory_order_relaxed) != 1) return 2005;
    tasks[1]();
    if (starts.load(std::memory_order_relaxed) != 2) return 2006;
    const auto diag = session.SnapshotRetryDiagnostics();
    session.Stop();
    return diag.StaleDroppedTotal == 1 && diag.ExecutedTotal == 1 ? 0 : 2007;
}
```

同一步增加 token 回绕测试。新增测试入口 `SetNextRetryTokenForTest(uint64_t)`，把 next token 设为 `UINT64_MAX` 后调用 `ScheduleStartRetryForTest(0)`；断言 scheduler 未收到任务、`ScheduleFailedTotal==1`、connection snapshot 的 `LastError` 包含 `retry token exhausted`。

- [ ] **Step 2: 构建并确认 RED**

Run: `rtk cmake --build build --target tcpquic_quic_session_reconnect_test -j2 && rtk proxy build/bin/Release/tcpquic_quic_session_reconnect_test`

Expected: 构建先因 `SnapshotRetryDiagnostics` 尚不存在失败；只补齐声明和返回零值的测试接缝后重新运行，测试必须以 `2005` 失败，证明旧任务借用了第二轮布尔状态。

- [ ] **Step 3: 添加票据和单一 active token 状态**

在 `quic_session.h` 添加：

```cpp
struct TqClientRetryDiagnostics {
    uint64_t ScheduledTotal{0};
    uint64_t ExecutedTotal{0};
    uint64_t StaleDroppedTotal{0};
    uint64_t ScheduleFailedTotal{0};
};
```

在 `QuicClientSession` public 区添加：

```cpp
TqClientRetryDiagnostics SnapshotRetryDiagnostics() const;
```

把 `ConnectionSlot::RetryScheduled` 替换为：

```cpp
uint64_t ActiveRetryToken{0};
```

在 `ClientSharedState` 添加：

```cpp
uint64_t NextRetryToken{1};
TqClientRetryDiagnostics RetryDiagnostics;
```

在 private 区添加：

```cpp
struct RetryTicket {
    size_t SlotIndex{0};
    uint64_t NumericConnectionId{0};
    uint64_t Generation{0};
    uint64_t Token{0};
};

struct RetrySubmission {
    RetryTicket Ticket;
    DelayedTaskScheduler Scheduler;
    std::weak_ptr<ClientSharedState> WeakState;
    std::shared_ptr<ClientSessionGate> Gate;
};

static void InvalidateRetryLocked(ConnectionSlot& slot) noexcept;
static bool RetryTicketMatchesLocked(
    const ClientSharedState& state,
    const RetryTicket& ticket) noexcept;
std::optional<RetrySubmission> ReserveRetryLocked(
    const std::shared_ptr<ClientSharedState>& state,
    size_t index,
    uint64_t expectedNumericConnectionId,
    uint64_t expectedGeneration);
void ScheduleStartRetry(
    size_t index,
    uint64_t expectedNumericConnectionId,
    uint64_t expectedGeneration);
void SubmitRetry(RetrySubmission submission);
```

同时添加 `#include <optional>`。

- [ ] **Step 4: 实现预约、提交、精确回滚和快照**

按以下语义实现；`ReserveRetryLocked` 的调用方已经持有 `state->Lock`：

```cpp
void QuicClientSession::InvalidateRetryLocked(ConnectionSlot& slot) noexcept {
    slot.ActiveRetryToken = 0;
}

bool QuicClientSession::RetryTicketMatchesLocked(
    const ClientSharedState& state,
    const RetryTicket& ticket) noexcept {
    if (!state.Started || state.Stopping || ticket.SlotIndex >= state.Slots.size()) return false;
    const auto& slot = state.Slots[ticket.SlotIndex];
    return ticket.Token != 0 &&
        slot.ActiveRetryToken == ticket.Token &&
        slot.NumericConnectionId == ticket.NumericConnectionId &&
        slot.Generation == ticket.Generation;
}
```

`ReserveRetryLocked` 必须：验证身份；在 `ActiveRetryToken != 0` 时返回空表示合并；拒绝 `NextRetryToken == 0` 或 `UINT64_MAX` 的分配；写入 `retry token exhausted` 到 slot `LastError` 并增加失败计数；正常时写入 token并复制 scheduler/gate/weak state。`SubmitRetry` 固定提交 3000ms；失败回滚时只有 `RetryTicketMatchesLocked` 为真才清零，并增加 `ScheduleFailedTotal`、写入 `retry scheduler unavailable`。scheduler 接受后增加 `ScheduledTotal`。

schedule、execute、stale-drop 和 schedule-failed 使用 `TqTraceLogLine` 写 trace-only 结构化行，包含 slot、`NumericConnectionId`、`Generation`，不额外写默认 stderr。connection attempt 时间序列继续使用现有 `event=quic_connecting`。

把 `SnapshotConnections()` 中的字段改为：

```cpp
snapshot.RetryScheduled = slot.ActiveRetryToken != 0;
```

把所有直接写 `RetryScheduled=false` 的路径改为 `InvalidateRetryLocked(slot)`。

`StartSlot()` 第一次接管 slot 时同时捕获 `numericConnectionId` 和 `generation`；所有本地配置、ConnectionOpen、SetLocalAddr 和 ConnectionStart 失败路径调用 `ScheduleStartRetry(index, numericConnectionId, generation)`。该入口持锁重新验证身份后才预约，禁止失败路径在手动 reconnect 抢占后为新实例创建任务。`ScheduleStartRetryForTest(index)` 作为测试包装，可以先读取当前身份再调用三参数生产入口。

- [ ] **Step 5: 让到期任务按完整票据校验**

先保留现有 `StartSlot(index)` 调用，但任务执行前使用完整票据：

```cpp
if (!submission.Scheduler(TqClientStartRetryDelay,
        [weakState = submission.WeakState,
         gate = submission.Gate,
         ticket = submission.Ticket]() {
            auto state = weakState.lock();
            if (!state) return;
            {
                std::lock_guard<std::mutex> guard(state->Lock);
                if (!RetryTicketMatchesLocked(*state, ticket)) {
                    ++state->RetryDiagnostics.StaleDroppedTotal;
                    return;
                }
                state->Slots[ticket.SlotIndex].ActiveRetryToken = 0;
                ++state->RetryDiagnostics.ExecutedTotal;
            }
            auto* session = AcquireLiveSession(gate);
            if (!session) return;
            (void)session->StartSlot(ticket.SlotIndex);
            ReleaseLiveSession(gate);
        })) {
    // 精确票据回滚，不能清除替代 token。
}
```

这一步只关闭“排队历史任务复活”；Task 2 将关闭校验后到 `StartSlot()` 之间的在途竞态。

- [ ] **Step 6: 运行目标测试并确认 GREEN**

Run: `rtk cmake --build build --target tcpquic_quic_session_reconnect_test -j2 && rtk proxy build/bin/Release/tcpquic_quic_session_reconnect_test`

Expected: exit 0；现有固定延迟、stop、duplicate coalescing 和 scheduler rejection 测试继续通过。

- [ ] **Step 7: 提交 Task 1**

```bash
rtk git add src/protocol/quic_session.h src/protocol/quic_session.cpp src/unittest/quic_session_reconnect_test.cpp
rtk git commit -m "fix: identify QUIC retry tasks with unique tickets"
```

---

### Task 2: 把票据校验移动到 StartSlot 接管线性化点

**Files:**
- Modify: `src/protocol/quic_session.h:202-325`
- Modify: `src/protocol/quic_session.cpp:2058-2333`
- Test: `src/unittest/quic_session_reconnect_test.cpp`

**Interfaces:**
- Consumes: Task 1 的 `RetryTicket` 和 `RetryTicketMatchesLocked`。
- Produces: `StartSlot(size_t, const RetryTicket*)`，`nullptr` 表示初始/手动启动。
- Produces: `ReconnectTestHooks::BeforeRetryStartClaim`，仅在测试构建中使用。

- [ ] **Step 1: 先增加可重复的在途抢占失败测试**

增加 `BeforeRetryStartClaim` 测试钩子，并用条件变量暂停任务：

```cpp
static int TestManualReconnectSupersedesInFlightRetry() {
    QuicClientSession session;
    std::function<void()> retry;
    std::mutex lock;
    std::condition_variable cv;
    bool retryAtClaim = false;
    bool releaseRetry = false;
    std::atomic<int> starts{0};

    session.MarkReconnectStartedForTest(1);
    QuicClientSession::ReconnectTestHooks hooks;
    hooks.StartSlotOverride = [&](size_t) { starts.fetch_add(1); return true; };
    hooks.BeforeRetryStartClaim = [&] {
        std::unique_lock<std::mutex> guard(lock);
        retryAtClaim = true;
        cv.notify_all();
        cv.wait(guard, [&] { return releaseRetry; });
    };
    session.SetReconnectTestHooks(std::move(hooks));
    session.SetDelayedTaskScheduler([&](auto, std::function<void()> task) {
        retry = std::move(task);
        return true;
    });
    session.ScheduleStartRetryForTest(0);

    std::thread worker([&] { retry(); });
    {
        std::unique_lock<std::mutex> guard(lock);
        cv.wait(guard, [&] { return retryAtClaim; });
    }
    std::string err;
    if (!session.ReconnectConnection("conn-0", err)) return 2101;
    {
        std::lock_guard<std::mutex> guard(lock);
        releaseRetry = true;
    }
    cv.notify_all();
    worker.join();
    const auto diag = session.SnapshotRetryDiagnostics();
    session.Stop();
    return starts.load() == 1 && diag.StaleDroppedTotal == 1 ? 0 : 2102;
}
```

- [ ] **Step 2: 确认 RED**

Run: `rtk cmake --build build --target tcpquic_quic_session_reconnect_test -j2 && rtk proxy build/bin/Release/tcpquic_quic_session_reconnect_test`

Expected: 在只把钩子放到旧任务“校验并清 token”之后、无条件 `StartSlot()` 之前时以 `2102` 失败，`starts==2`。

- [ ] **Step 3: 实现条件化 StartSlot**

修改签名：

```cpp
bool StartSlot(size_t index, const RetryTicket* expectedRetry = nullptr);
```

延迟任务不得提前清除 token；它只获取 live session 并调用：

```cpp
(void)session->StartSlot(ticket.SlotIndex, &ticket);
```

在 `StartSlot` 已有的 `ConfigLock + state->Lock` 临界区、移动 `slot.Connection` 之前执行：

```cpp
auto& slot = state->Slots[index];
if (expectedRetry != nullptr) {
    if (!RetryTicketMatchesLocked(*state, *expectedRetry)) {
        ++state->RetryDiagnostics.StaleDroppedTotal;
        return false;
    }
    slot.ActiveRetryToken = 0;
    ++state->RetryDiagnostics.ExecutedTotal;
} else {
    InvalidateRetryLocked(slot);
}
```

测试钩子在延迟任务调用 `StartSlot(..., &ticket)` 之前运行，不能放在持锁区内。这样手动 reconnect 在钩子期间完成后，最终临界区校验失败且无副作用。

- [ ] **Step 4: 增加 slot 删除重建 ABA 测试**

新增测试：创建两个 slot；为 `conn-1` 保存旧任务；删除 `conn-1`；重新扩容到两个 slot；为新 `conn-1` 预约任务；执行旧任务后 `StartSlotOverride` 计数不变，执行新任务后只增加一次，同时旧任务增加 stale counter。

- [ ] **Step 5: 运行目标测试并确认 GREEN**

Run: `rtk cmake --build build --target tcpquic_quic_session_reconnect_test -j2 && rtk proxy build/bin/Release/tcpquic_quic_session_reconnect_test`

Expected: exit 0；并发测试不挂起，ABA 测试证明旧 index 任务不能匹配新 `NumericConnectionId`。

- [ ] **Step 6: 提交 Task 2**

```bash
rtk git add src/protocol/quic_session.h src/protocol/quic_session.cpp src/unittest/quic_session_reconnect_test.cpp
rtk git commit -m "fix: linearize QUIC retry slot claims"
```

---

### Task 3: 原子处理 SHUTDOWN_COMPLETE 身份和重试预约

**Files:**
- Modify: `src/protocol/quic_session.h:202-325`
- Modify: `src/protocol/quic_session.cpp:2384-2396,2583-2637`
- Test: `src/unittest/quic_session_reconnect_test.cpp`

**Interfaces:**
- Consumes: `ReserveRetryLocked`、`RetrySubmission`。
- Produces: `CompleteSlotShutdownLocked(...)` 返回 `std::optional<RetrySubmission>` 和当前 connection owner。
- Replaces: 旧 `RestartSlotAfterShutdownComplete(state,index,generation)` 的分离检查/调度语义。

- [ ] **Step 1: 写 shutdown 检查后被手动 reconnect 抢占的失败测试**

把现有 `RestartSlotAfterShutdownCompleteForTest` 替换为调用生产 shutdown 状态转换的测试包装。测试钩子 `BeforeShutdownRetryReservation` 在进入原子临界区前暂停：保存旧 `NumericConnectionId/Generation`，启动线程处理旧 shutdown，钩子暂停时执行手动 reconnect，再恢复线程。断言 scheduler 未收到任务且当前连接 generation 未被旧 shutdown 改写。

测试包装签名固定为：

```cpp
bool CompleteSlotShutdownForTest(
    size_t index,
    uint64_t expectedNumericConnectionId,
    uint64_t expectedGeneration,
    bool useCurrentConnectionIdentity);
```

`useCurrentConnectionIdentity=false` 构造 orphan 身份；包装必须调用同一个生产 helper，不能直接调用 `ScheduleStartRetry`。

- [ ] **Step 2: 确认 RED**

Run: `rtk cmake --build build --target tcpquic_quic_session_reconnect_test -j2 && rtk proxy build/bin/Release/tcpquic_quic_session_reconnect_test`

Expected: 旧“检查 generation、解锁、再 ScheduleStartRetry(index)”语义会提交一个错误任务，测试失败。

- [ ] **Step 3: 实现 shutdown 原子状态转换**

定义结果：

```cpp
struct ShutdownCompleteResult {
    std::shared_ptr<MsQuicConnection> CompletedSlotConnection;
    std::optional<RetrySubmission> Retry;
    ConnectionStateNotification Notification;
    bool WasCurrent{false};
};
```

`CompleteSlotShutdownLocked` 在调用方持有 `state->Lock` 时：

```cpp
if (index >= state->Slots.size()) return result;
auto& slot = state->Slots[index];
if (slot.Context != context || slot.Connection.get() != connection ||
    slot.NumericConnectionId != numericConnectionId ||
    slot.Generation != generation) {
    return result;
}
result.WasCurrent = true;
slot.Context = nullptr;
slot.Connected = false;
slot.Closing = true;
result.CompletedSlotConnection = std::move(slot.Connection);
result.Retry = ReserveRetryLocked(
    state, index, numericConnectionId, generation);
```

真实 callback 在同一 lock scope 内调用该 helper，锁外依次执行 notification、`connection->Close()`、owner reset、orphan drop、context delete 和 `SubmitRetry(std::move(*result.Retry))`。只有 `WasCurrent` 才提交 retry；orphan callback 只释放自己的资源。

- [ ] **Step 4: 增加重复 shutdown 和 orphan 测试**

分别验证：当前 shutdown 只调度一次；第二次相同身份不调度；旧 connection/context 身份不清除当前 active token；手动 reconnect 后旧 generation 不调度。

- [ ] **Step 5: 运行目标测试并确认 GREEN**

Run: `rtk cmake --build build --target tcpquic_quic_session_reconnect_test -j2 && rtk proxy build/bin/Release/tcpquic_quic_session_reconnect_test`

Expected: exit 0；所有 shutdown、历史任务、并发和 slot 控制测试通过。

- [ ] **Step 6: 提交 Task 3**

```bash
rtk git add src/protocol/quic_session.h src/protocol/quic_session.cpp src/unittest/quic_session_reconnect_test.cpp
rtk git commit -m "fix: reserve QUIC retries during shutdown completion"
```

---

### Task 4: 暴露重试诊断并验证管理 JSON 兼容性

**Files:**
- Modify: `src/runtime/router_runtime.h:13-30`
- Modify: `src/runtime/client_peer_runtime.cpp:95-114`
- Modify: `src/runtime/router_runtime.cpp:329-342,1203-1234`
- Test: `src/unittest/router_runtime_test.cpp`

**Interfaces:**
- Consumes: `QuicClientSession::SnapshotRetryDiagnostics()`。
- Produces: `TqPeerMetrics::{RetryScheduledTotal,RetryExecutedTotal,RetryStaleDroppedTotal,RetryScheduleFailedTotal}`。

- [ ] **Step 1: 先扩展 router JSON 失败断言**

在现有 peer/metrics JSON 测试中加入：

```cpp
if (json.find("\"retry_scheduled_total\":11") == std::string::npos) return 2401;
if (json.find("\"retry_executed_total\":7") == std::string::npos) return 2402;
if (json.find("\"retry_stale_dropped_total\":3") == std::string::npos) return 2403;
if (json.find("\"retry_schedule_failed_total\":1") == std::string::npos) return 2404;
```

测试 adapter 返回带上述固定值的 `TqPeerMetrics`，并继续断言已有字段未消失。

- [ ] **Step 2: 构建并确认 RED**

Run: `rtk cmake --build build --target tcpquic_router_runtime_test -j2 && rtk proxy build/bin/Release/tcpquic_router_runtime_test`

Expected: 因 `TqPeerMetrics` 尚无字段而构建失败；补齐字段但不序列化后以 `2401` 失败。

- [ ] **Step 3: 添加 peer metrics 字段和聚合**

在 `TqPeerMetrics` 添加：

```cpp
uint64_t RetryScheduledTotal{0};
uint64_t RetryExecutedTotal{0};
uint64_t RetryStaleDroppedTotal{0};
uint64_t RetryScheduleFailedTotal{0};
```

`TqClientPeerRuntime::SnapshotPeerMetrics()` 读取一次 retry snapshot 并复制四个字段。`TqRouterRuntime::SnapshotMetrics()` 从 live metrics 复制四个字段。所有 peer JSON 对象以新增键输出这些字段；不输出内部 token。

- [ ] **Step 4: 运行 router 和 reconnect 测试**

Run: `rtk cmake --build build --target tcpquic_router_runtime_test tcpquic_quic_session_reconnect_test -j2 && rtk proxy build/bin/Release/tcpquic_router_runtime_test && rtk proxy build/bin/Release/tcpquic_quic_session_reconnect_test`

Expected: 两个进程均 exit 0。

- [ ] **Step 5: 提交 Task 4**

```bash
rtk git add src/runtime/router_runtime.h src/runtime/router_runtime.cpp src/runtime/client_peer_runtime.cpp src/unittest/router_runtime_test.cpp
rtk git commit -m "feat: expose QUIC retry diagnostics"
```

---

### Task 5: 增加 ingress 延迟队列容量诊断

**Files:**
- Modify: `src/ingress/client_ingress_reactor.h:65-70,110-207`
- Modify: `src/ingress/client_ingress_reactor.cpp:474-489,539-588,639-706`
- Modify: `src/runtime/client_peer_runtime.h:128-155`
- Modify: `src/runtime/client_peer_runtime.cpp:540-610`
- Modify: `src/runtime/router_runtime.h:32-80`
- Modify: `src/runtime/router_runtime.cpp:1203-1234,1522-1533`
- Modify: `src/main.cpp:20-75`
- Test: `src/unittest/client_ingress_reactor_test.cpp:1201-1345`
- Test: `src/unittest/router_runtime_test.cpp`

**Interfaces:**
- Produces: `TqClientIngressDiagnostics SnapshotDiagnostics() const`。
- Produces: `TqClientRuntimeManager::SnapshotIngressDiagnostics()` 和 adapter 同名虚函数。
- Produces: queue depth 当前值/最大值、timeout overshoot 样本数、p95/p99/最大微秒数。

- [ ] **Step 1: 写 queue depth 和排空失败测试**

```cpp
int TestDelayedTaskDiagnosticsTrackDepthAndDrain() {
    TqClientIngressReactor reactor;
    if (!reactor.Start()) return 2501;
    std::atomic<int> calls{0};
    for (int i = 0; i < 32; ++i) {
        if (!reactor.EnqueueDelayed(std::chrono::milliseconds(100),
                [&] { calls.fetch_add(1); })) return 2502;
    }
    auto queued = reactor.SnapshotDiagnostics();
    if (queued.DelayedTaskQueueDepth != 32 ||
        queued.MaxDelayedTaskQueueDepth < 32) return 2503;
    for (int i = 0; i < 100 && calls.load() != 32; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    auto drained = reactor.SnapshotDiagnostics();
    reactor.Stop();
    if (calls.load() != 32 || drained.DelayedTaskQueueDepth != 0) return 2504;
    if (drained.ReactorTimeoutOvershootP99Micros <
        drained.ReactorTimeoutOvershootP95Micros) return 2505;
    return 0;
}
```

- [ ] **Step 2: 构建并确认 RED**

Run: `rtk cmake --build build --target tcpquic_client_ingress_reactor_test -j2 && rtk proxy build/bin/Release/tcpquic_client_ingress_reactor_test`

Expected: 因 `SnapshotDiagnostics` 不存在构建失败。

- [ ] **Step 3: 实现最小诊断结构**

在 header 添加：

```cpp
struct TqClientIngressDiagnostics {
    uint64_t DelayedTaskQueueDepth{0};
    uint64_t MaxDelayedTaskQueueDepth{0};
    uint64_t ReactorTimeoutOvershootSamples{0};
    uint64_t ReactorTimeoutOvershootP95Micros{0};
    uint64_t ReactorTimeoutOvershootP99Micros{0};
    uint64_t ReactorTimeoutOvershootMaxMicros{0};
};
```

reactor 成员增加最大 queue depth 和 timeout overshoot 直方图。固定桶上界为 `100,250,500,1000,2500,5000,10000,25000,50000,100000,250000,UINT64_MAX` 微秒；`SnapshotDiagnostics()` 按累计样本计算 p95/p99。`EnqueueDelayed` push 后更新最大值；`ProcessDueDelayedTasks` erase 后当前值自然由 `DelayedTasks.size()` 得出。`Run()` 在 `Reactor.RunOnce(timeoutMs)` 前后使用 steady clock 计算 `max(0, elapsed_us-timeoutMs*1000)` 并落桶。`SnapshotDiagnostics()` 在 `LifecycleMutex`、`Mutex` 的既有锁顺序下复制值，不能反转锁顺序。

- [ ] **Step 4: 运行 ingress 测试并确认 GREEN**

Run: `rtk cmake --build build --target tcpquic_client_ingress_reactor_test -j2 && rtk proxy build/bin/Release/tcpquic_client_ingress_reactor_test`

Expected: exit 0；原有 delayed due、stop/drop 和 reactor-thread 测试继续通过。

- [ ] **Step 5: 先写管理 metrics 贯通失败测试**

让 router 测试 adapter 返回：

```cpp
TqClientIngressDiagnostics SnapshotIngressDiagnostics() override {
    return TqClientIngressDiagnostics{17, 41, 1000, 250, 500, 2500};
}
```

并断言 `/metrics` JSON 包含：

```cpp
if (json.find("\"ingress_delayed_task_queue_depth\":17") == std::string::npos) return 2511;
if (json.find("\"ingress_delayed_task_queue_depth_max\":41") == std::string::npos) return 2512;
if (json.find("\"ingress_reactor_timeout_overshoot_p95_us\":250") == std::string::npos) return 2513;
if (json.find("\"ingress_reactor_timeout_overshoot_p99_us\":500") == std::string::npos) return 2514;
```

Run: `rtk cmake --build build --target tcpquic_router_runtime_test -j2 && rtk proxy build/bin/Release/tcpquic_router_runtime_test`

Expected: adapter 和 metrics 字段尚不存在，构建失败。

- [ ] **Step 6: 贯通 manager、adapter 和 router metrics**

在 `TqClientRuntimeManager` 增加：

```cpp
TqClientIngressDiagnostics SnapshotIngressDiagnostics() const;
```

在 `TqPeerRuntimeAdapter` 增加带默认空值的虚函数，并在 `main.cpp` adapter override 后转发 manager。`TqRouterMetrics` 增加：

```cpp
TqClientIngressDiagnostics Ingress;
```

`SnapshotMetrics()` 从 adapter 复制一次共享 ingress 诊断；`MetricsJson()` 按 Step 5 的键名输出 queue depth、max、samples、p95、p99 和 max。字段位于 metrics 顶层，不复制到每个 peer。

- [ ] **Step 7: 运行 ingress/router 测试并确认 GREEN**

Run: `rtk cmake --build build --target tcpquic_client_ingress_reactor_test tcpquic_router_runtime_test -j2 && rtk proxy build/bin/Release/tcpquic_client_ingress_reactor_test && rtk proxy build/bin/Release/tcpquic_router_runtime_test`

Expected: 两个进程均 exit 0，新增 JSON 字段与既有 peer 字段共存。

- [ ] **Step 8: 提交 Task 5**

```bash
rtk git add src/ingress/client_ingress_reactor.h src/ingress/client_ingress_reactor.cpp \
  src/runtime/client_peer_runtime.h src/runtime/client_peer_runtime.cpp \
  src/runtime/router_runtime.h src/runtime/router_runtime.cpp src/main.cpp \
  src/unittest/client_ingress_reactor_test.cpp src/unittest/router_runtime_test.cpp
rtk git commit -m "feat: measure client ingress delayed task depth"
```

---

### Task 6: 添加双 peer 故障隔离和 k6 管理面压力工具

**Files:**
- Create: `scripts/run-quic-client-retry-single-flight-test.sh`
- Create: `tests/k6/quic_retry_reconnect.js`
- Create: `tests/scripts/test_quic_retry_single_flight_script.py`
- Modify: `docs/superpowers/specs/2026-07-12-quic-client-retry-single-flight-design.md`（仅在脚本实测需要校准命令时更新，不改变门槛）

**Interfaces:**
- Consumes: Release `raypx2`、admin `/api/v1/peers`、connection reconnect API、retry metrics。
- Produces: `docs/test/quic-client-retry-single-flight-<timestamp>/` 证据目录。

- [ ] **Step 1: 先写脚本静态测试**

新增 `tests/scripts/test_quic_retry_single_flight_script.py`，断言脚本：使用 `mktemp -d`；记录所有子 PID；trap 只 kill PID 数组；配置两个不同 peer 和不同监听端口；包含 600 秒 soak、201 次上限、2900ms 下限、健康探测和恢复检查；支持 `HOLD_FOR_K6_SECONDS`/`ENV_OUT`；不使用 `pkill`/`killall`。

Run: `rtk pytest -q tests/scripts/test_quic_retry_single_flight_script.py`

Expected: 脚本不存在，测试失败。

- [ ] **Step 2: 实现双 peer 脚本**

脚本必须采用以下安全骨架：

```bash
#!/usr/bin/env bash
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BIN="${BIN:-$ROOT/build/bin/Release/raypx2}"
RESULT_ROOT="${RESULT_ROOT:-$ROOT/docs/test/quic-client-retry-single-flight-$(date +%Y%m%d-%H%M%S)}"
TMP="$(mktemp -d)"
PIDS=()
cleanup() {
  for pid in "${PIDS[@]:-}"; do
    kill "$pid" 2>/dev/null || true
  done
  for pid in "${PIDS[@]:-}"; do
    wait "$pid" 2>/dev/null || true
  done
  rm -rf "$TMP"
}
trap cleanup EXIT INT TERM
alloc_port() {
  python3 - <<'PY'
import socket
s=socket.socket(); s.bind(('127.0.0.1',0)); print(s.getsockname()[1]); s.close()
PY
}
```

动态分配 admin、failed QUIC、healthy QUIC、healthy SOCKS、failed SOCKS 和目标 HTTP 端口。写入：

```bash
cat >"$TMP/client.json" <<JSON
{
  "admin":{"listen":"127.0.0.1:$admin_port","threads":2,"token_file":"$TMP/admin-token.json"},
  "client":{"client_name":"retry-single-flight-test"},
  "peers":[
    {"id":"failed","enabled":true,"proto_peer":"127.0.0.1:$failed_quic_port","proto_connections":1,"socks_listen":"127.0.0.1:$failed_socks_port"},
    {"id":"healthy","enabled":true,"proto_peer":"127.0.0.1:$healthy_quic_port","proto_connections":1,"socks_listen":"127.0.0.1:$healthy_socks_port"}
  ],
  "proto":{"connections":1,"keepalive_ms":5000,"disable_1rtt_encryption":true},
  "tls":{"ca":"$ROOT/cert/ca.crt"},
  "trace":{"enabled":true,"level":"info","interval_sec":30}
}
JSON
```

用下列命令启动目标、健康 server 和 client；每个后台命令后立即 `PIDS+=("$!")`：

```bash
mkdir -p "$TMP/http" "$RESULT_ROOT"
printf 'ok\n' >"$TMP/http/health.txt"
python3 -m http.server "$target_http_port" --bind 127.0.0.1 --directory "$TMP/http" \
  >"$RESULT_ROOT/http.log" 2>&1 & PIDS+=("$!")
"$BIN" server --listen "127.0.0.1:$healthy_quic_port" \
  --cert "$ROOT/cert/server/server.crt" --key "$ROOT/cert/server/server.key" \
  >"$RESULT_ROOT/healthy-server.log" 2>&1 & PIDS+=("$!")
"$BIN" client --ca "$ROOT/cert/ca.crt" --config "$TMP/client.json" \
  --admin-listen "127.0.0.1:$admin_port" --admin-token-file "$TMP/admin-token.json" \
  >"$RESULT_ROOT/client.log" 2>&1 & client_pid=$!; PIDS+=("$client_pid")
```

通过 token 文件读取 Bearer token 并轮询 admin。脚本支持 `SOAK_SECONDS`（默认 600）、`RECOVERY_TIMEOUT_MS`（默认 3500）和 `HOLD_FOR_K6_SECONDS`（默认 0）。当 hold 大于 0 时，把 `TOKEN`、`BASE_URL`、`CLIENT_PID` 和 `RESULT_ROOT` 写入调用方指定的 `ENV_OUT` 文件，并在完成 smoke 后保持进程存活相应秒数。

稳定阶段每秒执行：

```bash
curl --fail --silent --show-error \
  --socks5-hostname "127.0.0.1:$healthy_socks_port" \
  "http://127.0.0.1:$target_http_port/health.txt" >/dev/null
```

600 秒后解析 failed peer 的 `event=quic_connecting` 时间戳，断言次数不超过 201、任意相邻间隔不小于 2900ms；健康探测失败数必须为 0。随后用与健康 server 相同的证书参数在 failed QUIC 端口启动第二个测试 server，并断言 3500ms 内 peer connected。每 10 秒采集 `pidstat`、`ps` RSS、日志字节和 `/api/v1/metrics`，从 metrics 读取 retry counters、delayed queue depth 及 reactor p95/p99。

- [ ] **Step 3: 实现 k6 reconnect 场景**

`tests/k6/quic_retry_reconnect.js` 使用 `constant-arrival-rate`，必须要求 `ADMIN_TOKEN`、`BASE_URL`、`PEER_ID`，POST：

```javascript
const res = http.post(
  `${BASE_URL}/peers/${encodeURIComponent(PEER_ID)}/connections/conn-0:reconnect`,
  null,
  { headers: { Authorization: `Bearer ${ADMIN_TOKEN}` } },
);
check(res, { 'reconnect accepted': (r) => r.status === 202 });
```

支持 `baseline|peak|spike|soak|breaking_point`：默认 rate/duration 分别为 `1/60s`、`5/120s`、`20/30s`、`1/10m`、`50/60s`。非 breaking-point 阈值：`http_req_failed rate==0`、`checks rate==1`、`dropped_iterations count==0`、`http_req_duration p(95)<200`、`p(99)<500`。

- [ ] **Step 4: 运行静态测试、shell 语法和短时 smoke**

Run:

```bash
rtk pytest -q tests/scripts/test_quic_retry_single_flight_script.py
rtk proxy bash -n scripts/run-quic-client-retry-single-flight-test.sh
rtk proxy env SOAK_SECONDS=20 RECOVERY_TIMEOUT_MS=3500 scripts/run-quic-client-retry-single-flight-test.sh
```

Expected: pytest 和 bash syntax exit 0；smoke 仅终止自身 PIDs，健康探测零失败，结果目录包含 client/server logs、admin JSON 和资源采样。短时 smoke 使用 `ceil(SOAK_SECONDS/3)+1` 动态尝试上限。

- [ ] **Step 5: 提交 Task 6**

```bash
rtk git add scripts/run-quic-client-retry-single-flight-test.sh tests/k6/quic_retry_reconnect.js tests/scripts/test_quic_retry_single_flight_script.py docs/superpowers/specs/2026-07-12-quic-client-retry-single-flight-design.md
rtk git commit -m "test: cover QUIC retry isolation and admin pressure"
```

---

### Task 7: 完整验证与发布证据

**Files:**
- Verify only; failures只修改对应任务已列出的文件。

**Interfaces:**
- Consumes: Tasks 1-6 的全部实现。
- Produces: 构建、单元测试、双 peer soak、k6 和 sanitizer 证据。

- [ ] **Step 1: 运行格式和差异检查**

Run: `rtk git diff --check d0c1eaf..HEAD`

Expected: 无 whitespace error。

- [ ] **Step 2: 构建所有直接相关目标**

Run:

```bash
rtk cmake --build build --target \
  tcpquic_quic_session_reconnect_test \
  tcpquic_client_ingress_reactor_test \
  tcpquic_router_runtime_test \
  tcpquic-proxy -j2
```

Expected: exit 0，无新增编译 warning。

- [ ] **Step 3: 运行直接相关测试**

Run:

```bash
rtk proxy build/bin/Release/tcpquic_quic_session_reconnect_test
rtk proxy build/bin/Release/tcpquic_client_ingress_reactor_test
rtk proxy build/bin/Release/tcpquic_router_runtime_test
rtk pytest -q tests/scripts/test_quic_retry_single_flight_script.py
```

Expected: 四项均 exit 0。

- [ ] **Step 4: 运行 CTest 回归**

Run: `rtk ctest --test-dir build --output-on-failure`

Expected: 0 failed tests。

- [ ] **Step 5: 运行完整 10 分钟双 peer 验收**

Run: `rtk proxy scripts/run-quic-client-retry-single-flight-test.sh`

Expected: 600 秒窗口尝试数 ≤201、最小间隔 ≥2900ms、健康探测 100%、恢复 ≤3500ms、CPU/RSS/日志增长门槛通过。

- [ ] **Step 6: 运行 k6 baseline、peak、spike/排空**

先让 Task 6 脚本以短 smoke + hold 模式创建隔离 client，再从环境文件读取连接信息：

```bash
env_file="$(mktemp)"
rtk proxy env SOAK_SECONDS=20 HOLD_FOR_K6_SECONDS=900 ENV_OUT="$env_file" \
  scripts/run-quic-client-retry-single-flight-test.sh & harness_pid=$!
for _ in $(seq 1 100); do
  test -s "$env_file" && break
  sleep 0.1
done
test -s "$env_file"
source "$env_file"
rtk proxy k6 run -e SCENARIO=baseline -e ADMIN_TOKEN="$TOKEN" -e BASE_URL="$BASE_URL" -e PEER_ID=failed tests/k6/quic_retry_reconnect.js
rtk proxy k6 run -e SCENARIO=peak -e ADMIN_TOKEN="$TOKEN" -e BASE_URL="$BASE_URL" -e PEER_ID=failed tests/k6/quic_retry_reconnect.js
rtk proxy k6 run -e SCENARIO=spike -e ADMIN_TOKEN="$TOKEN" -e BASE_URL="$BASE_URL" -e PEER_ID=failed tests/k6/quic_retry_reconnect.js
kill "$harness_pid" 2>/dev/null || true
wait "$harness_pid" 2>/dev/null || true
rm -f "$env_file"
```

Expected: 所有非 breaking-point threshold 通过；停止负载 5 秒后 delayed queue depth 回到 baseline，10 秒内 CPU 回到 baseline 的 1.5 倍以内。

- [ ] **Step 7: 运行 sanitizer 目标**

Run:

```bash
rtk cmake --build build-asan --target tcpquic_quic_session_reconnect_test -j2
rtk proxy env ASAN_OPTIONS=detect_leaks=1 build-asan/bin/Release/tcpquic_quic_session_reconnect_test
```

若仓库存在已配置的 TSan build，另运行对应 reconnect test；没有 TSan build 时，在最终报告明确记录“未配置 TSan”，不得声称 TSan 通过。

Expected: ASan exit 0，无 leak/UAF；TSan 如执行则无 data race。

- [ ] **Step 8: 最终规格核对**

逐项对照设计文档第 2、4、5、6、7、8、9、10、12、13、14、15 节，把每条要求映射到测试输出或结果文件。发现缺口时返回对应 Task 补测试，不以人工代码审阅替代证据。

- [ ] **Step 9: 提交验证文档（仅当产生跟踪文档）**

若完整 soak 生成需要纳入版本库的摘要，只提交 `docs/test/quic-client-retry-single-flight-<timestamp>/summary.md` 和小型 JSON/CSV 指标，不提交大日志或二进制：

```bash
rtk git add docs/test/quic-client-retry-single-flight-*/summary.md \
  docs/test/quic-client-retry-single-flight-*/*.json \
  docs/test/quic-client-retry-single-flight-*/*.csv
rtk git commit -m "test: record QUIC retry single-flight validation"
```
