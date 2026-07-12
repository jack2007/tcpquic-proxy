# Linux 零长度 FIN Receive Ownership Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 修复 Linux/epoll 在零长度 FIN 与 closing/terminal handoff 竞争时遗漏 `StreamReceiveComplete(0)`、导致 stream terminal retention 永久不收敛的问题。

**Architecture:** 保留 Linux worker 现有 deferred receive batching，并为每个 `TqPendingQuicReceive` 增加独立于字节数的 completion state。所有 active cleanup 出口统一结清零长度 FIN obligation；只有真实 terminal/receive abort 后才 bookkeeping discard。graceful fully-closed 门禁额外检查 callback-pending receive，作为防御层避免 handoff 超前。

**Tech Stack:** C++17、MsQuic、Linux epoll、`std::shared_ptr`、原子状态、CMake、现有自包含测试、ASan、k6/xk6 TCP。

## Global Constraints

- 文档、日志说明默认使用中文；代码标识符和 metrics key 使用英文 snake_case/现有 C++ 风格。
- callback 返回 `QUIC_STATUS_PENDING` 后，零长度 FIN 必须恰好调用一次 `StreamReceiveComplete(0)`。
- 正长度 receive 保持现有分批 completion，不重复完成同一 batch。
- relay `Closing`、TCP fd 关闭、handoff started 不是 terminal discard 依据。
- 真实 `SHUTDOWN_COMPLETE`、receive abort 或 connection shutdown 后不得调用 stream receive API。
- stream API 只能通过 `TqStreamLifetime` API lease；不新增裸 stream pointer ownership。
- 正常 TCP EOF 仍只提交一次 QUIC FIN；单向 FIN 仍只执行 half-close。
- 不修改 wire protocol、不新增每 stream 线程、不伪造 terminal、不直接 `StreamClose`。
- 确定性竞态测试使用 test hook/barrier，不用 sleep 猜测顺序。
- 保留工作区既有改动；每次提交只暂存本 Task 列出的文件。

---

## File Structure

- Modify: `src/tunnel/linux_relay_event_queue.h` — Linux receive view completion state。
- Modify: `src/tunnel/linux_relay_worker.h` — settlement helper/test hook/snapshot counter 声明。
- Modify: `src/tunnel/linux_relay_worker.cpp` — active completion、terminal discard、cleanup 与 handoff 门禁。
- Modify: `src/unittest/linux_relay_worker_io_test.cpp` — 正常、closing、fallback、terminal race 测试。
- Modify: `src/unittest/linux_terminal_convergence_test.cpp` — 双向 FIN 到真实 terminal/reaper release 集成测试。
- Modify: `src/runtime/relay_metrics.h`、`src/runtime/relay_metrics.cpp` — Linux obligation counters 与 JSON。
- Modify: `scripts/run-linux-terminal-convergence.sh` — drain 后 release gates。

### Task 1: 用失败测试锁定 active cleanup 的零长度 FIN 欠账

**Files:**
- Modify: `src/unittest/linux_relay_worker_io_test.cpp`
- Modify: `src/tunnel/linux_relay_worker.h`

**Interfaces:**
- Consumes: `TqLinuxRelayWorker::DispatchStreamEventForTest()`、`BeginGracefulTerminalHandoffForTest()`、fake `StreamReceiveComplete`。
- Produces: `ZeroLengthReceiveCompleteCallsForTest()` test accessor，供后续竞态测试读取 exactly-once 次数。

- [ ] **Step 1: 让 fake API 记录零长度调用次数**

```cpp
std::atomic<uint64_t> g_receiveCompleteCalls{0};
std::atomic<uint64_t> g_receiveCompleteBytes{0};

void QUIC_API FakeStreamReceiveComplete(HQUIC, uint64_t bytes) {
    g_receiveCompleteCalls.fetch_add(1, std::memory_order_relaxed);
    g_receiveCompleteBytes.fetch_add(bytes, std::memory_order_relaxed);
}
```

- [ ] **Step 2: 增加正常 FIN-only 失败断言**

在现有 FIN-only fixture 中，在 dispatch 前清零 counters；worker drain 后增加：

```cpp
if (g_receiveCompleteCalls.load(std::memory_order_relaxed) != 1 ||
    g_receiveCompleteBytes.load(std::memory_order_relaxed) != 0) {
    return 2256;
}
```

- [ ] **Step 3: 增加 closing-before-view-drain 失败测试**

使用现有 worker test hook 先投递零长度 FIN view，再在处理 view 前调用 `BeginGracefulTerminalHandoffForTest(relayId)`；drain 后断言：

```cpp
CHECK(g_receiveCompleteCalls.load() == 1);
CHECK(g_receiveCompleteBytes.load() == 0);
CHECK(worker.Snapshot().PendingQuicReceiveQueue == 0);
```

- [ ] **Step 4: 运行测试确认失败**

Run: `rtk cmake --build build --target tcpquic_linux_relay_worker_io_test -j$(nproc)`

Run: `rtk ./build/bin/Release/tcpquic_linux_relay_worker_io_test`

Expected: FAIL；closing cleanup 场景中 `g_receiveCompleteCalls == 0`。

- [ ] **Step 5: 提交测试**

```bash
git add src/unittest/linux_relay_worker_io_test.cpp src/tunnel/linux_relay_worker.h
git commit -m "test(linux): reproduce zero-length FIN cleanup debt"
```

### Task 2: 将 completion obligation 与字节 accounting 分离

**Files:**
- Modify: `src/tunnel/linux_relay_event_queue.h`
- Modify: `src/tunnel/linux_relay_worker.h`
- Modify: `src/tunnel/linux_relay_worker.cpp`
- Test: `src/unittest/linux_relay_worker_io_test.cpp`

**Interfaces:**
- Consumes: `TqStreamLifetime::TryAcquireApi()`、`TqStreamLifetime::GetPhase()`。
- Produces: `TqLinuxReceiveCompletionState`、`CompleteActiveReceiveOwnership()`、`DiscardTerminalReceiveOwnership()`。

- [ ] **Step 1: 在 receive view 中定义最终状态**

```cpp
enum class TqLinuxReceiveCompletionState : uint8_t {
    NotRequired,
    Pending,
    Dispatching,
    ActiveCompleted,
    TerminalDiscarded,
};

struct TqPendingQuicReceive {
    std::atomic<TqLinuxReceiveCompletionState> CompletionState{
        TqLinuxReceiveCompletionState::NotRequired};
    bool ZeroLengthFinCompletionPending{false};
};
```

- [ ] **Step 2: view 构造时创建 obligation**

在 `BuildPendingQuicReceive()` 完成 view 校验后设置：

```cpp
view->ZeroLengthFinCompletionPending = fin && view->TotalLength == 0;
view->CompletionState.store(
    TqLinuxReceiveCompletionState::Pending,
    std::memory_order_release);
```

- [ ] **Step 3: 实现 active settlement helper**

```cpp
bool TqLinuxRelayWorker::CompleteActiveReceiveOwnership(
    TqPendingQuicReceive& view) noexcept {
    auto expected = TqLinuxReceiveCompletionState::Pending;
    if (!view.CompletionState.compare_exchange_strong(
            expected, TqLinuxReceiveCompletionState::Dispatching,
            std::memory_order_acq_rel, std::memory_order_acquire)) {
        return expected == TqLinuxReceiveCompletionState::ActiveCompleted;
    }
    auto lease = view.StreamOwner != nullptr
        ? view.StreamOwner->TryAcquireApi()
        : TqStreamLifetime::ApiLease{};
    MsQuicStream* stream = lease ? lease.Stream() : view.Stream;
    if (stream == nullptr || stream->Handle == nullptr) {
        view.CompletionState.store(
            TqLinuxReceiveCompletionState::Pending,
            std::memory_order_release);
        return false;
    }
    FlushDeferredReceiveCompletion(view, true);
    if (view.ZeroLengthFinCompletionPending) {
        stream->ReceiveComplete(0);
        view.ZeroLengthFinCompletionPending = false;
        DeferredReceiveCompletes.fetch_add(1, std::memory_order_relaxed);
        DeferredReceiveCompletionFlushes.fetch_add(1, std::memory_order_relaxed);
    }
    view.CompletionState.store(
        TqLinuxReceiveCompletionState::ActiveCompleted,
        std::memory_order_release);
    return true;
}
```

生产 managed path 不得使用 `view.Stream` fallback；实现时仅为没有 owner 的 inert unit-test registration 保留现有测试路径。

- [ ] **Step 4: 实现 terminal discard helper**

```cpp
bool TqLinuxRelayWorker::DiscardTerminalReceiveOwnership(
    TqPendingQuicReceive& view) noexcept {
    auto expected = TqLinuxReceiveCompletionState::Pending;
    if (!view.CompletionState.compare_exchange_strong(
            expected, TqLinuxReceiveCompletionState::TerminalDiscarded,
            std::memory_order_acq_rel, std::memory_order_acquire)) {
        return expected == TqLinuxReceiveCompletionState::TerminalDiscarded;
    }
    view.ZeroLengthFinCompletionPending = false;
    view.PendingCompleteBytes = 0;
    view.CompletedLength = view.TotalLength;
    view.Stream = nullptr;
    view.StreamOwner.reset();
    return true;
}
```

将正常 drain 中现有的所有 `CompleteZeroLengthFinReceive(*view)` 调用替换为 `CompleteActiveReceiveOwnership(*view)`；正长度 view 只有在最后一个 batch 已 flush 且 `CompletedLength == TotalLength` 时发布 `ActiveCompleted`。不得让正常路径在 API 已完成后仍停留在 `Pending`。

- [ ] **Step 5: 运行 focused test**

Run: `rtk cmake --build build --target tcpquic_linux_relay_worker_io_test -j$(nproc)`

Run: `rtk ./build/bin/Release/tcpquic_linux_relay_worker_io_test`

Expected: PASS；正常零长度 FIN 与 closing cleanup 均调用一次 `ReceiveComplete(0)`。

- [ ] **Step 6: 提交状态模型**

```bash
git add src/tunnel/linux_relay_event_queue.h src/tunnel/linux_relay_worker.h src/tunnel/linux_relay_worker.cpp src/unittest/linux_relay_worker_io_test.cpp
git commit -m "fix(linux): settle zero-length FIN receive ownership"
```

### Task 3: 收口所有 cleanup/fallback ownership exits

**Files:**
- Modify: `src/tunnel/linux_relay_worker.cpp`
- Test: `src/unittest/linux_relay_worker_io_test.cpp`

**Interfaces:**
- Consumes: Task 2 settlement helpers。
- Produces: 所有非 terminal cleanup active-complete、所有真实 terminal cleanup discard 的统一行为。

- [ ] **Step 1: 改写公共 cleanup helper**

```cpp
void TqLinuxRelayWorker::CompleteAndDiscardQuicReceive(
    TqPendingQuicReceive& view) {
    const uint64_t remaining = view.TotalLength >= view.CompletedLength
        ? view.TotalLength - view.CompletedLength
        : 0;
    view.PendingCompleteBytes += remaining;
    view.CompletedLength += remaining;
    (void)CompleteActiveReceiveOwnership(view);
}
```

- [ ] **Step 2: terminal helper 只做 terminal discard**

```cpp
void TqLinuxRelayWorker::DiscardTerminalQuicReceive(
    TqPendingQuicReceive& view) {
    (void)DiscardTerminalReceiveOwnership(view);
}
```

- [ ] **Step 3: 审计并替换四类出口**

逐个验证以下调用点均使用正确 helper：`AbortRelayAndRelease()` 的两组 pending queues、`ProcessQuicReceiveViewEvent()` 的 relay-missing/Closing 分支、retired relay purge、worker stop drain。owner phase 为 `TerminalPublished` 才进入 terminal discard，否则 active-complete。

- [ ] **Step 4: 增加 callback fallback 与 terminal race 测试**

分别强制 event queue full 和先 dispatch `SHUTDOWN_COMPLETE`；断言：

```cpp
CHECK(activeCase.ReceiveCompleteCalls == 1);
CHECK(terminalCase.ReceiveCompleteCalls == 0);
CHECK(terminalCase.CompletionState ==
      TqLinuxReceiveCompletionState::TerminalDiscarded);
```

- [ ] **Step 5: 运行 Linux relay tests**

Run: `rtk cmake --build build --target tcpquic_linux_relay_worker_io_test tcpquic_linux_terminal_convergence_test -j$(nproc)`

Run: `rtk ./build/bin/Release/tcpquic_linux_relay_worker_io_test`

Run: `rtk ./build/bin/Release/tcpquic_linux_terminal_convergence_test`

Expected: 全部 PASS，duplicate/violation counters 为 0。

- [ ] **Step 6: 提交 cleanup 收口**

```bash
git add src/tunnel/linux_relay_worker.cpp src/unittest/linux_relay_worker_io_test.cpp
git commit -m "fix(linux): close receive ownership cleanup gaps"
```

### Task 4: 加固 graceful handoff 门禁与端到端回归

**Files:**
- Modify: `src/tunnel/linux_relay_worker.cpp`
- Modify: `src/unittest/linux_terminal_convergence_test.cpp`

**Interfaces:**
- Consumes: pending receive queues、callback pending queue、terminal handoff facts。
- Produces: `HasPendingReceiveOwnership()`，阻止 handoff 超越尚未结清的 receive view。

- [ ] **Step 1: 写 handoff 超前失败测试**

通过 barrier 将零长度 FIN view 保留在 callback pending queue，同时完成本地 TCP/QUIC 两方向；断言 handoff 尚未开始。释放 barrier 后断言 completion、真实 terminal 和 release ready。

- [ ] **Step 2: 实现 pending ownership 门禁**

```cpp
bool TqLinuxRelayWorker::HasPendingReceiveOwnership(RelayState* relay) const {
    if (relay == nullptr || !relay->PendingQuicReceives.empty()) return true;
    std::lock_guard<std::mutex> guard(relay->CallbackPendingQuicReceiveLock);
    return !relay->CallbackPendingQuicReceives.empty();
}
```

在 `MaybeStopFullyClosedRelay()` 中、调用 `BeginTerminalHandoff()` 前增加该门禁。

- [ ] **Step 3: 运行集成测试**

Run: `rtk cmake --build build --target tcpquic_linux_terminal_convergence_test -j$(nproc)`

Run: `rtk ./build/bin/Release/tcpquic_linux_terminal_convergence_test`

Expected: PASS；release facts 只有在 receive obligation 为 0 后满足。

- [ ] **Step 4: 提交门禁**

```bash
git add src/tunnel/linux_relay_worker.cpp src/unittest/linux_terminal_convergence_test.cpp
git commit -m "fix(linux): gate graceful handoff on receive ownership"
```

### Task 5: 指标、ASan 与系统发布门禁

**Files:**
- Modify: `src/tunnel/linux_relay_worker.h`
- Modify: `src/tunnel/linux_relay_worker.cpp`
- Modify: `src/runtime/relay_metrics.h`
- Modify: `src/runtime/relay_metrics.cpp`
- Modify: `scripts/run-linux-terminal-convergence.sh`

**Interfaces:**
- Consumes: completion state transitions。
- Produces: Linux required/completed/discarded/zero-length/lease-retry/pending/violation counters 和脚本 gate。

- [ ] **Step 1: 在 snapshot/JSON 中加入平台 counters**

字段使用：

```text
linux_relay_receive_completion_required
linux_relay_receive_completion_active_completed
linux_relay_receive_completion_terminal_discarded
linux_relay_receive_completion_zero_length
linux_relay_receive_completion_lease_retry
linux_relay_receive_completion_pending
linux_relay_receive_completion_exactly_once_violation
```

- [ ] **Step 2: 更新 runner drain gate**

在 after snapshot 检查：

```bash
jq -e '
  .terminal_retained_owner_count == 0 and
  .terminal_sink_pending == 0 and
  .linux_relay_receive_completion_pending == 0 and
  .linux_relay_receive_completion_exactly_once_violation == 0
' "$OUT/metrics/client-after-relay.json"
```

- [ ] **Step 3: 运行 focused 与全量 Linux tests**

Run: `rtk cmake --build build -j$(nproc)`

Run: `rtk ctest --test-dir build --output-on-failure -R 'linux_relay_worker_io|linux_terminal_convergence|stream_lifetime|tcp_tunnel|tunnel_reaper'`

Expected: 100% tests passed。

- [ ] **Step 4: 运行 ASan focused tests**

Run: `rtk cmake --build build-terminal-asan --target tcpquic_linux_relay_worker_io_test tcpquic_linux_terminal_convergence_test -j$(nproc)`

Run: `rtk ./build-terminal-asan/bin/Release/tcpquic_linux_relay_worker_io_test`

Run: `rtk ./build-terminal-asan/bin/Release/tcpquic_linux_terminal_convergence_test`

Expected: PASS，无 ASan/LSan 报告。

- [ ] **Step 5: 运行系统 smoke 与 soak**

Run: `rtk env SCENARIO=baseline scripts/run-linux-terminal-convergence.sh`

Run: `rtk env SCENARIO=soak SOAK_SECONDS=1800 scripts/run-linux-terminal-convergence.sh`

Expected: drain 后 retention、sink pending、receive obligation、violation 全为 0；吞吐下降不超过 1%，p95/p99 回归不超过 5%。

- [ ] **Step 6: 提交指标和门禁**

```bash
git add src/tunnel/linux_relay_worker.h src/tunnel/linux_relay_worker.cpp src/runtime/relay_metrics.h src/runtime/relay_metrics.cpp scripts/run-linux-terminal-convergence.sh
git commit -m "test(linux): gate zero-length FIN ownership release"
```
