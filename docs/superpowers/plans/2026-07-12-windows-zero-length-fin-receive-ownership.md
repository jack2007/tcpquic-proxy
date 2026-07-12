# Windows 零长度 FIN Receive Ownership Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 修复 Windows/IOCP 将零长度 FIN 的 `0 bytes` 错当成 ownership 已结清、callback 返回 `QUIC_STATUS_PENDING` 后未调用 `StreamReceiveComplete(0)` 的问题。

**Architecture:** 保留 IOCP receive view、partial TCP send 和 batched completion。用独立 completion state 表示 callback 是否已经把 ownership 转移给应用；只有 IOCP post 成功并返回 `PENDING` 才创建 obligation。active cleanup 通过 receive API lease 完成零长度 FIN，terminal seal 后只做 bookkeeping discard。

**Tech Stack:** C++17、MsQuic、Windows IOCP/Winsock、原子状态、CMake/MSVC、现有 Windows worker tests、Application Verifier/ASan、k6 TCP workload。

## Global Constraints

- callback 返回 `QUIC_STATUS_PENDING` 后，真实零长度 FIN 必须恰好调用一次 `StreamReceiveComplete(0)`。
- IOCP post 失败且 callback 返回 `QUIC_STATUS_SUCCESS` 时不得调用或保留 receive completion obligation。
- 正长度 partial/batched completion 的字节范围不得重叠或重复。
- `CompletedLength >= TotalLength` 不能单独证明零长度 view ownership 已结清。
- relay `Closing` 和 socket cancel 不是 terminal discard 事实。
- 真实 terminal/receive abort/connection shutdown 后不得调用 stream API。
- 不绕过 IOCP in-flight ownership drain，不保存新增裸 stream pointer。
- 正常 FIN half-close、payload framing 和 wire protocol 保持不变。
- 竞态测试使用 IOCP test hook/barrier 或明确 completion packet，不使用无条件 sleep。
- 保留工作区既有改动；每次提交只暂存本 Task 文件。

---

## File Structure

- Modify: `src/tunnel/windows_relay_worker.cpp` — view state、active/terminal settlement、IOCP cleanup。
- Modify: `src/tunnel/windows_relay_worker.h` — test hooks、snapshot counters。
- Modify: `src/unittest/windows_relay_worker_test.cpp` — FIN-only、post failure、closing、terminal、stop drain 测试。
- Modify: `src/runtime/relay_metrics.h`、`src/runtime/relay_metrics.cpp` — Windows completion metrics。
- Modify: `scripts/run-windows-terminal-convergence.ps1` — 新建或扩展 Windows 系统 runner 与 release gates。

### Task 1: 修正现有 FIN-only 测试的错误成功条件

**Files:**
- Modify: `src/unittest/windows_relay_worker_test.cpp`

**Interfaces:**
- Consumes: `FakeStreamReceiveComplete`、`TqWindowsRelayDispatchTestStreamEvent()`、worker snapshot。
- Produces: 对 normal zero-FIN 和 callback-post-failure disposition 的失败测试。

- [ ] **Step 1: 在现有零长度 FIN 测试中清零 fake counters**

```cpp
g_StreamReceiveCompleteCalls = 0;
g_StreamReceiveCompleteBytes = 0;
```

- [ ] **Step 2: normal IOCP post 成功场景断言 0-byte API 调用**

在现有 `BufferCount=0`/FIN 和 `Length=0` buffer/FIN 两个 fixture drain 后增加：

```cpp
if (g_StreamReceiveCompleteCalls != 1 ||
    g_StreamReceiveCompleteBytes != 0) {
    return 6096;
}
```

- [ ] **Step 3: IOCP post failure + callback SUCCESS 场景断言不调用 API**

强制 `PostCallbackOperationById(RelayReceiveReady)` 失败，断言：

```cpp
CHECK(status == QUIC_STATUS_SUCCESS);
CHECK(g_StreamReceiveCompleteCalls == 0);
CHECK(worker.Snapshot().PendingQuicReceiveQueueDepth == 0);
```

- [ ] **Step 4: 运行测试确认失败**

Run: `rtk cmake --build build --config Release --target tcpquic_windows_relay_worker_test -j 2`

Run: `rtk .\build\bin\Release\tcpquic_windows_relay_worker_test.exe`

Expected: FAIL；normal FIN-only 场景的 `g_StreamReceiveCompleteCalls == 0`。

- [ ] **Step 5: 提交测试**

```bash
git add src/unittest/windows_relay_worker_test.cpp
git commit -m "test(windows): expose zero-length FIN ownership debt"
```

### Task 2: 为 Windows receive view 建立独立 completion state

**Files:**
- Modify: `src/tunnel/windows_relay_worker.cpp`
- Modify: `src/tunnel/windows_relay_worker.h`
- Test: `src/unittest/windows_relay_worker_test.cpp`

**Interfaces:**
- Consumes: `TqStreamLifetime::TryAcquireReceiveApi()`、`ShouldDiscardReceiveView()`。
- Produces: `TqWindowsReceiveCompletionState`、`TryBeginActiveCompletion()`、`FinishActiveCompletion()`、`TryTerminalDiscard()`。

- [ ] **Step 1: 替换 bool OwnershipSettled**

```cpp
enum class TqWindowsReceiveCompletionState : uint8_t {
    NotRequired,
    Pending,
    Dispatching,
    ActiveCompleted,
    TerminalDiscarded,
};

struct TqWindowsPendingQuicReceive {
    std::atomic<TqWindowsReceiveCompletionState> CompletionState{
        TqWindowsReceiveCompletionState::NotRequired};
    bool ZeroLengthFinCompletionPending{false};
};
```

- [ ] **Step 2: IOCP publish 前 reservation，post 失败时回滚**

`BuildDeferredQuicReceiveView()` 设置：

```cpp
view->ZeroLengthFinCompletionPending = fin && view->TotalLength == 0;
```

调用 `PostCallbackOperationById()` 前设置，防止 worker 在线程间先消费 operation：

```cpp
view->CompletionState.store(
    TqWindowsReceiveCompletionState::Pending,
    std::memory_order_release);
```

post 失败分支在 operation 从未发布的事实下执行：

```cpp
view->CompletionState.store(
    TqWindowsReceiveCompletionState::NotRequired,
    std::memory_order_release);
return QUIC_STATUS_SUCCESS;
```

同时删除该分支中的 `ActiveCompleteRemainingReceiveOwnership()` 调用。post 成功后 reservation 不再回滚，callback 必须返回 `QUIC_STATUS_PENDING`。

- [ ] **Step 3: 让 receive API helper 接受显式 0-byte completion**

```cpp
bool ActiveReceiveCompleteViaOwner(
    const std::shared_ptr<TqStreamLifetime>& owner,
    uint64_t completeBytes) {
    if (!owner) return false;
    auto lease = owner->TryAcquireReceiveApi();
    MsQuicStream* stream = lease ? lease.Stream() : nullptr;
    if (stream == nullptr || stream->Handle == nullptr) return false;
    stream->ReceiveComplete(completeBytes);
    return true;
}
```

- [ ] **Step 4: 增加状态测试 hook**

在 `TQ_UNIT_TESTING` 下公开最后一个 receive view 的 state snapshot，使测试能够断言 `Pending`、`ActiveCompleted` 或 `TerminalDiscarded`，不返回内部裸指针。

- [ ] **Step 5: 构建并运行测试**

Run: `rtk cmake --build build --config Release --target tcpquic_windows_relay_worker_test -j 2`

Run: `rtk .\build\bin\Release\tcpquic_windows_relay_worker_test.exe`

Expected: normal zero-FIN 仍失败于 settlement helper；post-failure 场景 PASS 且 API calls=0。

- [ ] **Step 6: 提交状态模型**

```bash
git add src/tunnel/windows_relay_worker.cpp src/tunnel/windows_relay_worker.h src/unittest/windows_relay_worker_test.cpp
git commit -m "refactor(windows): model receive completion obligation"
```

### Task 3: 修复 active settlement 和 closing cleanup

**Files:**
- Modify: `src/tunnel/windows_relay_worker.cpp`
- Test: `src/unittest/windows_relay_worker_test.cpp`

**Interfaces:**
- Consumes: Task 2 completion state。
- Produces: `ActiveCompleteRemainingReceiveOwnership()` 对零长度 FIN 的 exactly-once API down-call。

- [ ] **Step 1: 在 final settlement 中处理零长度 FIN**

```cpp
void TqWindowsRelayWorker::ActiveCompleteRemainingReceiveOwnership(
    const std::shared_ptr<RelayContext>& relay,
    TqWindowsPendingQuicReceive& view) {
    if (ShouldDiscardReceiveView(view.StreamOwner)) {
        DiscardRemainingReceiveOwnership(relay, view);
        return;
    }
    auto expected = TqWindowsReceiveCompletionState::Pending;
    if (!view.CompletionState.compare_exchange_strong(
            expected, TqWindowsReceiveCompletionState::Dispatching,
            std::memory_order_acq_rel, std::memory_order_acquire)) return;

    const uint64_t remaining = view.TotalLength -
        std::min(view.CompletedLength, view.TotalLength);
    const bool zeroFin = view.ZeroLengthFinCompletionPending;
    if ((remaining != 0 || zeroFin) &&
        !ActiveReceiveCompleteViaOwner(view.StreamOwner, remaining)) {
        view.CompletionState.store(
            TqWindowsReceiveCompletionState::Pending,
            std::memory_order_release);
        return;
    }
    view.ZeroLengthFinCompletionPending = false;
    view.Drained = true;
    view.CompletedLength = view.TotalLength;
    view.PendingCompleteBytes = 0;
    view.CompletionState.store(
        TqWindowsReceiveCompletionState::ActiveCompleted,
        std::memory_order_release);
}
```

正长度已由 `ActiveFlushDeferredReceiveCompletion()` 完成全部 batch 时，`remaining==0 && !zeroFin` 只发布最终 state，不额外调用 0-byte API。

- [ ] **Step 2: terminal discard 使用独立 CAS**

```cpp
void TqWindowsRelayWorker::DiscardRemainingReceiveOwnership(
    const std::shared_ptr<RelayContext>& relay,
    TqWindowsPendingQuicReceive& view) {
    auto expected = TqWindowsReceiveCompletionState::Pending;
    if (!view.CompletionState.compare_exchange_strong(
            expected, TqWindowsReceiveCompletionState::TerminalDiscarded,
            std::memory_order_acq_rel, std::memory_order_acquire)) return;
    view.ZeroLengthFinCompletionPending = false;
    ReleasePendingReceiveAccounting(relay, view);
}
```

- [ ] **Step 3: closing cleanup 不再自动 discard active owner**

`FlushPendingQuicReceivesOnClose()` 继续根据 `ShouldDiscardReceiveView(owner)` 分流：active owner 调用 final settlement；真实 terminal/aborted owner 调用 terminal discard。

- [ ] **Step 4: 增加 Closing-before-IOCP-drain 竞态测试**

用 barrier 暂停 `RelayReceiveReady` 消费，先调用 graceful close，再释放 completion；断言：

```cpp
CHECK(g_StreamReceiveCompleteCalls == 1);
CHECK(g_StreamReceiveCompleteBytes == 0);
CHECK(state == TqWindowsReceiveCompletionState::ActiveCompleted);
```

- [ ] **Step 5: 运行测试**

Run: `rtk cmake --build build --config Release --target tcpquic_windows_relay_worker_test -j 2`

Run: `rtk .\build\bin\Release\tcpquic_windows_relay_worker_test.exe`

Expected: PASS，normal 与 closing zero-FIN 都调用一次 API。

- [ ] **Step 6: 提交 active settlement**

```bash
git add src/tunnel/windows_relay_worker.cpp src/unittest/windows_relay_worker_test.cpp
git commit -m "fix(windows): complete zero-length FIN ownership"
```

### Task 4: 覆盖 terminal、orphan IOCP、stop drain 和 lease retry

**Files:**
- Modify: `src/tunnel/windows_relay_worker.cpp`
- Modify: `src/unittest/windows_relay_worker_test.cpp`

**Interfaces:**
- Consumes: terminal seal、IOCP orphan cleanup、`DenyReceiveApiLeasesForTest()`。
- Produces: 所有 Windows ownership exits 的确定性覆盖。

- [ ] **Step 1: 增加 terminal-wins 测试**

投递 zero-FIN view 后先 dispatch `SHUTDOWN_COMPLETE`，再 drain IOCP；断言 API calls=0、state=`TerminalDiscarded`、queue depth=0。

- [ ] **Step 2: 增加 lease retry 测试**

拒绝前两次 receive lease；第一次 maintenance 后断言 state=`Pending` 且 API calls=0；恢复 lease 后断言 API calls=1、state=`ActiveCompleted`。

- [ ] **Step 3: 增加 orphan completion 和 stop drain 测试**

分别使 relay generation 失配和 worker stop 时保留 zero-FIN IOCP operation；旧 view 必须独立 active-complete或在真实 terminal 后 discard，新 relay counters 不变化。

- [ ] **Step 4: 运行完整 Windows worker test**

Run: `rtk cmake --build build --config Release --target tcpquic_windows_relay_worker_test -j 2`

Run: `rtk .\build\bin\Release\tcpquic_windows_relay_worker_test.exe`

Expected: PASS；`WorkerIocpOwnershipIsZero()` 最终为 true，无 duplicate settlement。

- [ ] **Step 5: 提交竞态覆盖**

```bash
git add src/tunnel/windows_relay_worker.cpp src/unittest/windows_relay_worker_test.cpp
git commit -m "test(windows): cover zero-FIN terminal and IOCP races"
```

### Task 5: Windows 指标、系统 runner 与发布验证

**Files:**
- Modify: `src/tunnel/windows_relay_worker.h`
- Modify: `src/tunnel/windows_relay_worker.cpp`
- Modify: `src/runtime/relay_metrics.h`
- Modify: `src/runtime/relay_metrics.cpp`
- Create: `scripts/run-windows-terminal-convergence.ps1`

**Interfaces:**
- Consumes: completion state transitions、Admin relay metrics/retentions。
- Produces: Windows obligation metrics、baseline/soak evidence package。

- [ ] **Step 1: 增加 snapshot 和 JSON counters**

```text
windows_relay_receive_completion_required
windows_relay_receive_completion_active_completed
windows_relay_receive_completion_terminal_discarded
windows_relay_receive_completion_zero_length
windows_relay_receive_completion_lease_retry
windows_relay_receive_completion_pending
windows_relay_receive_completion_exactly_once_violation
```

- [ ] **Step 2: 创建 Windows runner**

脚本启动受控 FIN fixture、server/client，读取 Bearer token，运行 baseline/soak，并在 drain 后断言：

```powershell
if ($relay.terminal_retained_owner_count -ne 0 -or
    $relay.terminal_sink_pending -ne 0 -or
    $relay.windows_relay_receive_completion_pending -ne 0 -or
    $relay.windows_relay_receive_completion_exactly_once_violation -ne 0) {
    throw "Windows zero-FIN release gate failed"
}
```

- [ ] **Step 3: 运行 Windows focused 和全量 tests**

Run: `rtk cmake --build build --config Release -j 2`

Run: `rtk ctest --test-dir build -C Release --output-on-failure -R "windows_relay_worker|stream_lifetime|tcp_tunnel|tunnel_reaper"`

Expected: 100% tests passed。

- [ ] **Step 4: 运行 Windows 系统 smoke/soak**

Run: `rtk powershell -ExecutionPolicy Bypass -File scripts/run-windows-terminal-convergence.ps1 -Scenario baseline`

Run: `rtk powershell -ExecutionPolicy Bypass -File scripts/run-windows-terminal-convergence.ps1 -Scenario soak -SoakSeconds 1800`

Expected: drain 后所有 ownership/retention/violation gate 为 0；吞吐下降不超过 1%，p95/p99 回归不超过 5%。

- [ ] **Step 5: 提交指标和 runner**

```bash
git add src/tunnel/windows_relay_worker.h src/tunnel/windows_relay_worker.cpp src/runtime/relay_metrics.h src/runtime/relay_metrics.cpp scripts/run-windows-terminal-convergence.ps1
git commit -m "test(windows): gate zero-length FIN ownership release"
```
