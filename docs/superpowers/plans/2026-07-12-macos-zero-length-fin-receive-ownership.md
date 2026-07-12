# macOS 零长度 FIN Receive Ownership Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 修复 macOS/kqueue 在真实零长度 FIN callback 返回 `QUIC_STATUS_PENDING` 后，normal drain 和 discard 路径都因 `0 bytes` early-return 而遗漏 `StreamReceiveComplete(0)` 的问题。

**Architecture:** 在 `TqDarwinPendingQuicReceive` 中把 completion obligation 与 `PendingCompleteBytes` 分离。保留 lease-only、callback budget、precommit 和 kqueue fallback 架构；active view 即使 payload 为 0 也通过 owner API lease完成，terminal view 仅 bookkeeping discard，所有 MsQuic down-call 保持在 relay mutex 外。

**Tech Stack:** C++17、MsQuic、macOS kqueue、原子状态、CMake/Clang、现有 Darwin relay tests、ASan、k6 TCP workload、Network Link Conditioner/dummynet 等价网络故障注入。

## Global Constraints

- callback 返回 `QUIC_STATUS_PENDING` 后，真实零长度 FIN 必须恰好调用一次 `StreamReceiveComplete(0)`。
- `PendingCompleteBytes == 0` 不代表 receive ownership 已完成。
- callback 返回 `QUIC_STATUS_SUCCESS` 的 budget-reject/invalid-input 分支不得调用 `ReceiveComplete(0)`。
- MsQuic receive API 只通过 `TqStreamLifetime` lease；禁止 bare `MsQuicStream*` fallback。
- `ReceiveComplete` 不得在 relay mutex、activation mutex 或 callback-budget mutex 内调用。
- binding `Closing/Retired` 不等于真实 terminal；非 terminal cleanup 仍需 active completion。
- terminal/receive abort/connection shutdown 后只 bookkeeping discard。
- 保持 kqueue event fallback、precommit transaction 和正常 FIN half-close。
- 不修改 wire protocol、不伪造 terminal、不直接 `StreamClose`。
- 所有确定性竞态测试使用 hook/barrier，不以 sleep 判断先后。
- 保留工作区既有改动；每次提交只暂存本 Task 文件。

---

## File Structure

- Modify: `src/tunnel/darwin_relay_event_queue.h` — Darwin completion state。
- Modify: `src/tunnel/darwin_relay_worker.h` — settlement helpers、test accessor、snapshot counters。
- Modify: `src/tunnel/darwin_relay_worker.cpp` — normal/closing/precommit/fallback/terminal settlement。
- Modify: `src/unittest/darwin_relay_worker_io_test.cpp` — zero-FIN 与竞态回归。
- Modify: `src/unittest/darwin_relay_worker_queue_test.cpp` — queue-full ownership 回归。
- Modify: `src/runtime/relay_metrics.h`、`src/runtime/relay_metrics.cpp` — Darwin completion metrics。
- Create: `scripts/run-macos-terminal-convergence.sh` — 系统 smoke/soak 和 release gates。

### Task 1: 让现有 FIN-only 测试验证真实 MsQuic completion

**Files:**
- Modify: `src/unittest/darwin_relay_worker_io_test.cpp`

**Interfaces:**
- Consumes: `FakeReceiveComplete()`、`ResetFakeReceiveComplete()`、`TqDarwinRelayWorker::StreamCallback()`。
- Produces: normal zero-FIN、zero-buffer FIN 和 callback-success disposition 的失败测试。

- [ ] **Step 1: 修改现有 `QuicReceiveCallbackRejectsOversizedReceiveAfterPendingFinOnlyEvent`**

在 worker drain FIN-only event 后增加：

```cpp
CHECK(g_receiveCompleteCalls.load(std::memory_order_acquire) == 1);
CHECK(g_receiveCompleteBytes.load(std::memory_order_acquire) == 0);
```

- [ ] **Step 2: 增加 BufferCount=1、Length=0 的 FIN 测试**

```cpp
uint8_t byte = 0;
QUIC_BUFFER buffer{0, &byte};
QUIC_STREAM_EVENT event{};
event.Type = QUIC_STREAM_EVENT_RECEIVE;
event.RECEIVE.Buffers = &buffer;
event.RECEIVE.BufferCount = 1;
event.RECEIVE.TotalBufferLength = 0;
event.RECEIVE.AbsoluteOffset = 42;
event.RECEIVE.Flags = QUIC_RECEIVE_FLAG_FIN;
CHECK(TqDarwinRelayWorker::StreamCallback(
          stream, stream->Context, &event) == QUIC_STATUS_PENDING);
```

drain 后断言 calls=1、bytes=0、TCP peer 观察到 EOF。

- [ ] **Step 3: 增加 callback 返回 SUCCESS 的对照**

强制 `CallbackBudgetRejected`，callback 返回 `SUCCESS`；断言 fake completion calls=0，因为 MsQuic 会按 callback disposition 自动消费 receive。

- [ ] **Step 4: 运行测试确认失败**

Run: `rtk cmake --build build --target tcpquic_darwin_relay_worker_io_test -j$(sysctl -n hw.logicalcpu)`

Run: `rtk ./build/bin/Release/tcpquic_darwin_relay_worker_io_test`

Expected: FAIL；pending FIN-only 场景 calls=0。

- [ ] **Step 5: 提交测试**

```bash
git add src/unittest/darwin_relay_worker_io_test.cpp
git commit -m "test(macos): expose zero-length FIN completion debt"
```

### Task 2: 为 Darwin view 增加可重试 completion state

**Files:**
- Modify: `src/tunnel/darwin_relay_event_queue.h`
- Modify: `src/tunnel/darwin_relay_worker.h`
- Modify: `src/tunnel/darwin_relay_worker.cpp`
- Test: `src/unittest/darwin_relay_worker_io_test.cpp`

**Interfaces:**
- Consumes: `TqStreamLifetime::TryAcquireApi()`、binding terminal/owner phase。
- Produces: `TqDarwinReceiveCompletionState`、`CompleteDeferredQuicReceive()` 的 zero-byte active settlement、`DiscardDeferredQuicReceive()` 的 terminal disposition。

- [ ] **Step 1: 替换 `CompletionDispatched` bool**

```cpp
enum class TqDarwinReceiveCompletionState : uint8_t {
    NotRequired,
    Pending,
    Dispatching,
    ActiveCompleted,
    TerminalDiscarded,
};

struct TqDarwinPendingQuicReceive {
    std::atomic<TqDarwinReceiveCompletionState> CompletionState{
        TqDarwinReceiveCompletionState::NotRequired};
    bool ZeroLengthFinCompletionPending{false};
};
```

- [ ] **Step 2: 构造 view 时记录 zero-FIN，但不提前创建 obligation**

```cpp
receive->ZeroLengthFinCompletionPending =
    fin && receive->TotalLength == 0;
```

在把 view 发布到 precommit/event/callback-pending queue 前先设置 state=`Pending`，防止 worker 先消费 view。若 enqueue 结果决定 callback 返回 `SUCCESS` 或错误且 view 从未发布，则回滚为 `NotRequired`；`Ok`、`EventQueueFull` 和 precommit success 不回滚，callback 必须返回 `PENDING`：

```cpp
receive->CompletionState.store(
    TqDarwinReceiveCompletionState::Pending,
    std::memory_order_release);
const auto result = QueueDeferredQuicReceive(bindingOwner, receive);
if (result != TqDarwinQuicReceiveEnqueueResult::Ok &&
    result != TqDarwinQuicReceiveEnqueueResult::EventQueueFull) {
    receive->CompletionState.store(
        TqDarwinReceiveCompletionState::NotRequired,
        std::memory_order_release);
}
```

为此将 queue helper 固定为：

```cpp
TqDarwinQuicReceiveEnqueueResult QueueDeferredQuicReceive(
    const std::shared_ptr<StreamBinding>& binding,
    const std::shared_ptr<TqDarwinPendingQuicReceive>& receive);
```

- [ ] **Step 3: 增加只读 test accessor**

在 `TCPQUIC_TESTING` 下按 relay id 返回 pending receive state snapshot，不暴露 view 指针：

```cpp
TqDarwinReceiveCompletionState
PendingReceiveCompletionStateForTest(uint64_t relayId) const;
```

- [ ] **Step 4: 构建确认类型与 callback disposition 测试通过**

Run: `rtk cmake --build build --target tcpquic_darwin_relay_worker_io_test -j$(sysctl -n hw.logicalcpu)`

Expected: build PASS；pending FIN 测试仍失败于未调用 API，SUCCESS 对照 PASS。

- [ ] **Step 5: 提交状态模型**

```bash
git add src/tunnel/darwin_relay_event_queue.h src/tunnel/darwin_relay_worker.h src/tunnel/darwin_relay_worker.cpp src/unittest/darwin_relay_worker_io_test.cpp
git commit -m "refactor(macos): model receive completion obligation"
```

### Task 3: 修复 normal drain 与非 terminal discard

**Files:**
- Modify: `src/tunnel/darwin_relay_worker.cpp`
- Test: `src/unittest/darwin_relay_worker_io_test.cpp`

**Interfaces:**
- Consumes: Task 2 completion state、`TryAcquireApi()`。
- Produces: 允许显式 `ReceiveComplete(0)` 的 lease-only active settlement。

- [ ] **Step 1: 删除低层 zero-byte early-return**

```cpp
void TqDarwinRelayWorker::CompleteMsQuicReceiveFromCallback(
    MsQuicStream* stream,
    uint64_t totalLength) {
#if defined(TCPQUIC_TESTING)
    if (g_darwinRelayReceiveCompleteForTest != nullptr) {
        g_darwinRelayReceiveCompleteForTest(stream, totalLength);
        return;
    }
#endif
    if (stream != nullptr && stream->Handle != nullptr) {
        stream->ReceiveComplete(totalLength);
    }
}
```

该 helper 只能在上层成功 claim obligation 后调用，禁止普通无义务代码直接传 0。

- [ ] **Step 2: 改写 deferred completion 的入口条件**

```cpp
const bool zeroFinPending = receive->ZeroLengthFinCompletionPending;
if (receive->PendingCompleteBytes == 0 && !zeroFinPending) {
    return;
}
```

获取 lease 前 CAS `Pending -> Dispatching`；lease 失败且 owner 非 terminal 时回滚 `Pending`。lease 成功后调用：

```cpp
const uint64_t bytes = zeroFinPending ? 0 : receive->PendingCompleteBytes;
CompleteMsQuicReceiveFromCallback(lease.Stream(), bytes);
receive->ZeroLengthFinCompletionPending = false;
receive->PendingCompleteBytes = 0;
receive->CompletionState.store(
    TqDarwinReceiveCompletionState::ActiveCompleted,
    std::memory_order_release);
```

- [ ] **Step 3: normal no-slices FIN 先完成 ownership 再 SHUT_WR**

`EnqueueQuicReceiveForTcp()` 的 `remainingTcpWriteBytes == 0` 分支调用 `CompleteDeferredQuicReceive()`；只有 state=`ActiveCompleted` 后才设置 `TcpWriteShutdownQueued=true`。lease 暂时失败时保留 view 在 pending queue，由 maintenance 重试。

- [ ] **Step 4: 非 terminal discard 改为 active completion**

`DiscardDeferredQuicReceive()` 判断 owner 尚未 terminal 时，把剩余正长度字节或 zero-FIN obligation交给 `CompleteDeferredQuicReceive()`；不得将 `PendingCompleteBytes=0` 作为已完成。

- [ ] **Step 5: 运行 IO tests**

Run: `rtk cmake --build build --target tcpquic_darwin_relay_worker_io_test -j$(sysctl -n hw.logicalcpu)`

Run: `rtk ./build/bin/Release/tcpquic_darwin_relay_worker_io_test`

Expected: PASS；normal zero-FIN calls=1、bytes=0，SUCCESS 对照 calls=0。

- [ ] **Step 6: 提交 active settlement**

```bash
git add src/tunnel/darwin_relay_worker.cpp src/unittest/darwin_relay_worker_io_test.cpp
git commit -m "fix(macos): complete zero-length FIN ownership"
```

### Task 4: 覆盖 kqueue fallback、precommit、closing 和 terminal race

**Files:**
- Modify: `src/tunnel/darwin_relay_worker.cpp`
- Modify: `src/unittest/darwin_relay_worker_io_test.cpp`
- Modify: `src/unittest/darwin_relay_worker_queue_test.cpp`

**Interfaces:**
- Consumes: callback pending queue、precommit queue、binding terminal、worker stop retire。
- Produces: 所有 Darwin ownership exits 的 exactly-once 覆盖。

- [ ] **Step 1: queue-full fallback 失败测试**

将 zero-FIN view 放入 `CallbackPendingQuicReceives`，在正常 event queue full 后返回 `PENDING`；恢复 worker drain 后断言 `ReceiveComplete(0)` 一次、callback budget归零。

- [ ] **Step 2: precommit activation/rollback 测试**

activation 成功：zero-FIN active-complete一次。activation 失败但 owner active：走 active shutdown 前先结清 obligation。真实 terminal 先发生：state=`TerminalDiscarded` 且 calls=0。

- [ ] **Step 3: Closing-before-view-drain 测试**

barrier 暂停 view event，在 relay `Closing=true` 后释放；`ProcessQuicReceiveViewEvent()` 必须通过 `DiscardDeferredQuicReceive()` active-complete，而不是按 0 bytes 丢弃。

- [ ] **Step 4: lease retry 与 terminal-wins 测试**

第一次 lease 失败后断言 state=`Pending`；恢复 lease 后 calls=1。另一个 fixture 在恢复前 dispatch terminal，断言 state=`TerminalDiscarded`、calls=0。

- [ ] **Step 5: worker stop 测试**

stop drain 时保留 callback-pending zero-FIN；worker 退出前 completion state 必须是 `ActiveCompleted` 或有真实 terminal 支持的 `TerminalDiscarded`，callback pending events/bytes 均为 0。

- [ ] **Step 6: 运行 Darwin focused tests**

Run: `rtk cmake --build build --target tcpquic_darwin_relay_worker_io_test tcpquic_darwin_relay_worker_queue_test -j$(sysctl -n hw.logicalcpu)`

Run: `rtk ./build/bin/Release/tcpquic_darwin_relay_worker_io_test`

Run: `rtk ./build/bin/Release/tcpquic_darwin_relay_worker_queue_test`

Expected: 全部 PASS，无 mutex-downcall assertion、budget leak 或 duplicate completion。

- [ ] **Step 7: 提交竞态覆盖**

```bash
git add src/tunnel/darwin_relay_worker.cpp src/unittest/darwin_relay_worker_io_test.cpp src/unittest/darwin_relay_worker_queue_test.cpp
git commit -m "test(macos): cover zero-FIN queue and terminal races"
```

### Task 5: macOS 指标、系统 runner 与发布验证

**Files:**
- Modify: `src/tunnel/darwin_relay_worker.h`
- Modify: `src/tunnel/darwin_relay_worker.cpp`
- Modify: `src/runtime/relay_metrics.h`
- Modify: `src/runtime/relay_metrics.cpp`
- Create: `scripts/run-macos-terminal-convergence.sh`

**Interfaces:**
- Consumes: completion state transitions、Admin relay metrics/retentions。
- Produces: Darwin obligation metrics、baseline/soak 证据包。

- [ ] **Step 1: 增加 snapshot/JSON counters**

```text
darwin_relay_receive_completion_required
darwin_relay_receive_completion_active_completed
darwin_relay_receive_completion_terminal_discarded
darwin_relay_receive_completion_zero_length
darwin_relay_receive_completion_lease_retry
darwin_relay_receive_completion_pending
darwin_relay_receive_completion_exactly_once_violation
```

- [ ] **Step 2: 创建 macOS runner**

脚本启动受控 FIN fixture、server/client、k6 workload，并在 drain 后执行：

```bash
jq -e '
  .terminal_retained_owner_count == 0 and
  .terminal_sink_pending == 0 and
  .darwin_relay_receive_completion_pending == 0 and
  .darwin_relay_receive_completion_exactly_once_violation == 0
' "$OUT/metrics/client-after-relay.json"
```

- [ ] **Step 3: 运行 macOS focused 和全量 tests**

Run: `rtk cmake --build build -j$(sysctl -n hw.logicalcpu)`

Run: `rtk ctest --test-dir build --output-on-failure -R 'darwin_relay_worker|stream_lifetime|tcp_tunnel|tunnel_reaper'`

Expected: 100% tests passed。

- [ ] **Step 4: 运行 ASan**

Run: `rtk cmake --build build-terminal-asan --target tcpquic_darwin_relay_worker_io_test tcpquic_darwin_relay_worker_queue_test -j$(sysctl -n hw.logicalcpu)`

Run: `rtk ./build-terminal-asan/bin/Release/tcpquic_darwin_relay_worker_io_test`

Run: `rtk ./build-terminal-asan/bin/Release/tcpquic_darwin_relay_worker_queue_test`

Expected: PASS，无 ASan/LSan、mutex down-call 或 duplicate completion 报告。

- [ ] **Step 5: 运行系统 smoke/soak**

Run: `rtk env SCENARIO=baseline scripts/run-macos-terminal-convergence.sh`

Run: `rtk env SCENARIO=soak SOAK_SECONDS=1800 scripts/run-macos-terminal-convergence.sh`

Expected: drain 后 ownership、retention、sink pending、violation 全为 0；吞吐下降不超过 1%，p95/p99 回归不超过 5%。

- [ ] **Step 6: 提交指标和 runner**

```bash
git add src/tunnel/darwin_relay_worker.h src/tunnel/darwin_relay_worker.cpp src/runtime/relay_metrics.h src/runtime/relay_metrics.cpp scripts/run-macos-terminal-convergence.sh
git commit -m "test(macos): gate zero-length FIN ownership release"
```
