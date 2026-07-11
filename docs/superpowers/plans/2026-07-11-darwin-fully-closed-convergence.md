# Darwin Fully-Closed Relay Convergence Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Darwin HTTP CONNECT 半关闭完成后及时 `SignalStop`，消除 H1 僵尸 relay（兼容 Linux 谓词 + event queue 满时 sticky 可靠交付）。

**Architecture:** Worker-only `MaybePublishFullyClosedStopLocal` 复用并扩展已有 `FullyClosedPredicateReady`；callback 在 enqueue 失败时置 binding sticky + Wake，worker 消费 sticky 后再收敛。产品语义 B：`QuicSendFinCompleted`（FIN `SEND_COMPLETE`）即可，不强制 `SEND_SHUTDOWN_COMPLETE`。

**Design:** `docs/superpowers/specs/2026-07-11-darwin-fully-closed-convergence-design.md`

**Problem / evidence:** `docs/2026-07-11-darwin-http-connect-zombie-relay.md` §8 / §9.6

**Tech Stack:** C++17, Darwin kqueue relay worker, MsQuic stream events, existing `TqRelayStopControl::SignalStop`, Darwin io unit tests (`TCPQUIC_TESTING`).

---

## File Structure

| File | Responsibility |
|------|----------------|
| `src/tunnel/darwin_relay_worker.h` | Sticky atomics on `StreamBinding`；`MaybePublishFullyClosedStopLocal` / `ConsumeHalfCloseStickies` / test hooks；更新 `FullyClosedPredicateReady` 注释 |
| `src/tunnel/darwin_relay_worker.cpp` | Sticky 置位/消费；收敛 helper；触发点；enqueue 失败路径；off-worker FIN → ConvergenceCheckSticky |
| `src/unittest/darwin_relay_worker_io_test.cpp` | 确定性收敛 / sticky / 反向顺序 / callback pending 测试 |
| `docs/2026-07-11-darwin-http-connect-zombie-relay.md` | 状态改为修复中/已修 |
| `docs/relay_macos.md` | P0 条目更新 |
| `docs/superpowers/specs/2026-07-11-darwin-fully-closed-convergence-design.md` | 状态改为实现中/已完成 |

**不改：** Linux/Windows worker；MsQuic 产品语义；独立 terminal 控制环。

**构建（src-only，macOS）：**

```bash
cmake --build build --config Release --target tcpquic_darwin_relay_worker_io_test -- -j8
cmake --build build --config Release --target tcpquic_darwin_relay_metrics_test -- -j8
```

（若本地 build 目录名不同，用现有 `build` / `build-asan`；勿 clean / 勿重配 CMake，除非增量失败。）

---

### Task 1: Sticky 字段 + 消费 + peer FIN enqueue 失败路径

**Files:**
- Modify: `src/tunnel/darwin_relay_worker.h`（`StreamBinding` + 声明）
- Modify: `src/tunnel/darwin_relay_worker.cpp`（消费、PEER_SEND_SHUTDOWN / SEND_SHUTDOWN_COMPLETE 失败路径）
- Test: `src/unittest/darwin_relay_worker_io_test.cpp`

- [ ] **Step 1: 写失败测试 — queue 满时 peer FIN 仍能置 sticky 并被 worker 消费**

在 `darwin_relay_worker_io_test.cpp` 的 `main` 调用列表前增加（复用 `ManagedRelayHarness` + `EventQueueCapacity=2` 填满队列模式，参考 `ManagedReceiveAllocationFailureQueueFullKeepsOwnerViaSink`）：

```cpp
void FullyClosedPeerSendShutdownStickySurvivesQueueFull() {
    TqDarwinRelayWorkerConfig config{};
    config.EventQueueCapacity = 2;
    ManagedRelayHarness harness(config);
    CHECK(harness.OpenSocketPair());
    CHECK(harness.Register(/*enableQuicSends=*/false));

    // Fill queue so PEER_SEND_SHUTDOWN cannot TryPush.
    CHECK(harness.Worker.EnqueueForTest(TestMarkerEvent(1)));
    CHECK(harness.Worker.EnqueueForTest(TestMarkerEvent(2)));
    CHECK(harness.Worker.PendingEventsForTest() >= 2);

    QUIC_STREAM_EVENT peerShutdown{};
    peerShutdown.Type = QUIC_STREAM_EVENT_PEER_SEND_SHUTDOWN;
    CHECK(harness.DispatchViaRouter(peerShutdown) == QUIC_STATUS_SUCCESS);
    CHECK(harness.Worker.PeerSendShutdownStickyForTest(harness.Result.RelayId));

    // Drain markers then sticky consumption path (implementation will Wake + consume).
    while (harness.Worker.DrainOneEventForTest()) {
    }
    CHECK(harness.Worker.DrainWakeForTest() >= 0);
    // After Task 1: sticky consumed → TcpWriteShutdownQueued or TcpWriteClosed path armed.
    CHECK(harness.Worker.TcpWriteShutdownQueuedOrClosedForTest(harness.Result.RelayId));
    harness.StopAndClosePeer();
}
```

若尚无 `TestMarkerEvent` / test hooks，先在同文件已有 queue-full 测试中复制 marker helper；hooks 在 Step 3 添加。

在 `main()` 中注册调用（暂时可注释到实现后再打开，或先打开并接受 FAIL）。

- [ ] **Step 2: Run test — expect FAIL**

```bash
cmake --build build --config Release --target tcpquic_darwin_relay_worker_io_test -- -j8
./build/bin/Release/tcpquic_darwin_relay_worker_io_test 2>&1 | rg -n "FullyClosedPeerSendShutdownSticky|CHECK|failed" | head -40
```

Expected: 编译失败（缺 hooks）或断言失败（sticky 未置位）。

- [ ] **Step 3: 在 `StreamBinding` 增加 sticky + worker helpers**

`darwin_relay_worker.h` — 在 `StreamBinding`（cpp 内嵌 struct）增加：

```cpp
std::atomic<bool> PeerSendShutdownSticky{false};
std::atomic<bool> SendShutdownCompleteSticky{false};
std::atomic<bool> ConvergenceCheckSticky{false};
```

（字段在 `src/tunnel/darwin_relay_worker.cpp` 的 `struct StreamBinding` 内，与现有 `CallbackPendingReceiveBytes` 并列。）

Worker 声明：

```cpp
void ConsumeHalfCloseStickies(const std::shared_ptr<RelayState>& relay);
void ArmHalfCloseStickyFromCallback(
    StreamBinding* binding,
    std::atomic<bool> StreamBinding::* stickyFlag);
#if defined(TCPQUIC_TESTING)
bool PeerSendShutdownStickyForTest(uint64_t relayId);
bool TcpWriteShutdownQueuedOrClosedForTest(uint64_t relayId);
#endif
```

- [ ] **Step 4: 实现 sticky 置位与消费（尚不 SignalStop）**

`EnqueueRelayCloseFromCallback` 调用方：当 `QuicPeerSendShutdown` / `QuicSendShutdownComplete` 的 `TryPush` 失败时：

```cpp
binding->PeerSendShutdownSticky.store(true, std::memory_order_release);
(void)Wake();
// 保留现有 EventQueueFullErrors++ 与 peer_shutdown_enqueue_failed trace
```

`SEND_SHUTDOWN_COMPLETE` 路径同样对 `SendShutdownCompleteSticky`。

`ConsumeHalfCloseStickies`（仅 worker）：

```cpp
void TqDarwinRelayWorker::ConsumeHalfCloseStickies(
    const std::shared_ptr<RelayState>& relay) {
    AssertWorkerThreadForRelayState();
    if (relay == nullptr || relay->Binding == nullptr) {
        return;
    }
    auto& binding = *relay->Binding;
    if (binding.PeerSendShutdownSticky.exchange(false, std::memory_order_acq_rel)) {
        ProcessPeerSendShutdown(relay);
    }
    if (binding.SendShutdownCompleteSticky.exchange(false, std::memory_order_acq_rel)) {
        ProcessSendShutdownComplete(relay);
    }
    (void)binding.ConvergenceCheckSticky.exchange(false, std::memory_order_acq_rel);
    // ConvergenceCheckSticky 的 MaybePublish 在 Task 2 接线
}
```

在 `DrainEvents` / `DrainWake` 处理每个 relay 相关事件前后，对涉及的 relay 调用 `ConsumeHalfCloseStickies`；并在 worker 主循环每 tick 对 active map 做轻量扫描 **或** 仅在 Wake 后扫描带 sticky 的 binding（优先：在 `FlushAllCallbackPendingQuicReceivesLocal` 同类位置增加 `FlushHalfCloseStickiesLocal()`，遍历 Relays 调 `ConsumeHalfCloseStickies`）。

- [ ] **Step 5: Run test — expect PASS**

```bash
cmake --build build --config Release --target tcpquic_darwin_relay_worker_io_test -- -j8
./build/bin/Release/tcpquic_darwin_relay_worker_io_test
```

Expected: PASS（含新 sticky 测试）。

- [ ] **Step 6: Commit**

```bash
git add src/tunnel/darwin_relay_worker.h src/tunnel/darwin_relay_worker.cpp \
  src/unittest/darwin_relay_worker_io_test.cpp
git commit -m "$(cat <<'EOF'
feat(darwin-relay): sticky peer FIN delivery when event queue is full

Ensure PEER_SEND_SHUTDOWN / SEND_SHUTDOWN_COMPLETE cannot be silently
dropped; worker consumes binding stickies after Wake.
EOF
)"
```

---

### Task 2: `MaybePublishFullyClosedStopLocal` + 主路径触发

**Files:**
- Modify: `src/tunnel/darwin_relay_worker.h` / `.cpp`
- Test: `src/unittest/darwin_relay_worker_io_test.cpp`

- [ ] **Step 1: 写失败测试 — EOF + peer FIN 无 SHUTDOWN_COMPLETE 也 SignalStop**

```cpp
void FullyClosedStopAfterEofAndPeerFinWithoutShutdownComplete() {
    ManagedRelayHarness harness;
    CHECK(harness.OpenSocketPair());
    CHECK(harness.Register(/*enableQuicSends=*/false));
    auto control = harness.Handle.Control;
    CHECK(control != nullptr);
    CHECK(!control->Stop.load(std::memory_order_acquire));

    // TCP EOF on relay fd (Fds[1]): close peer write end.
    CHECK(::shutdown(harness.Fds[0], SHUT_WR) == 0);
    CHECK(harness.Worker.InvokeTcpEventForTest(
        harness.Result.RelayId, EVFILT_READ, 0, 0));

    QUIC_STREAM_EVENT peerShutdown{};
    peerShutdown.Type = QUIC_STREAM_EVENT_PEER_SEND_SHUTDOWN;
    CHECK(harness.DispatchViaRouter(peerShutdown) == QUIC_STATUS_SUCCESS);
    CHECK(harness.Worker.DrainOneEventForTest());
    CHECK(harness.Worker.FlushTcpWritableForTest(harness.Result.RelayId));

    CHECK(control->Stop.load(std::memory_order_acquire));
    CHECK(TqRelayControlStopSignaledCount().load(std::memory_order_relaxed) >= 1);
    harness.StopAndClosePeer();
}
```

- [ ] **Step 2: Run test — expect FAIL**（`control->Stop` 仍为 false）

```bash
./build/bin/Release/tcpquic_darwin_relay_worker_io_test 2>&1 | rg -n "FullyClosedStopAfterEof|CHECK failed" | head -20
```

- [ ] **Step 3: 实现 `MaybePublishFullyClosedStopLocal`**

声明（`.h`）：

```cpp
void MaybePublishFullyClosedStopLocal(
    const std::shared_ptr<RelayState>& relay,
    const char* trigger);
```

实现（对齐 Linux `MaybeStopFullyClosedRelay` + `SetRelayStop` 的 stop 侧）：

```cpp
void TqDarwinRelayWorker::MaybePublishFullyClosedStopLocal(
    const std::shared_ptr<RelayState>& relay,
    const char* trigger) {
    AssertWorkerThreadForRelayState();
    if (relay == nullptr || relay->Closing || relay->StopControl == nullptr) {
        return;
    }
    if (!FullyClosedPredicateReady(*relay)) {
        TraceHalfClose(*relay, trigger);
        return;
    }
    const bool signaled = relay->StopControl->SignalStop(relay->ControlGeneration);
    TraceHalfClose(*relay, signaled ? "fully_closed_stop" : "fully_closed_stop_generation_mismatch");
    // 可选：首次 stop 时递增测试/metrics 计数 FullyClosedStopCount
}
```

更新 `FullyClosedPredicateReady` 注释：由「never SignalStop」改为「谓词只读；stop 仅由 MaybePublishFullyClosedStopLocal 执行」。

扩展谓词：Consume sticky 之后再判；若 sticky 仍为 true（未消费），返回 false：

```cpp
if (relay.Binding != nullptr) {
    if (relay.Binding->PeerSendShutdownSticky.load(std::memory_order_acquire) ||
        relay.Binding->SendShutdownCompleteSticky.load(std::memory_order_acquire) ||
        relay.Binding->ConvergenceCheckSticky.load(std::memory_order_acquire)) {
        return false;
    }
    // existing callback pending checks...
}
```

更新 `TraceHalfClose`：当 `FullyClosedPredicateReady` 且 `StopControl->Stop` 已为 true 时，不要再写 `predicate_ready_no_signal_stop`，改为 `already_stopped`。

- [ ] **Step 4: 在触发点调用 helper**

至少：

| 位置 | trigger 字符串 |
|------|----------------|
| `DrainTcpReadable` EOF 且 `SubmitTcpBatchToQuic(..., true)` 成功后 | `tcp_eof` |
| `CompleteQuicSend` worker 路径 `info.Fin` 更新后 | `quic_fin_buffer_released` |
| `ProcessPeerSendShutdown` 末尾 | `peer_send_shutdown` |
| `FlushTcpWrites` `SHUT_WR` 完成、`TcpWriteClosed=true` 后 | `tcp_shut_wr` |
| `ProcessSendShutdownComplete` 末尾 | `send_shutdown_complete` |
| `ConsumeHalfCloseStickies` 末尾 | `half_close_sticky` |
| pending TCP write / QUIC receive 排空到空的路径末尾（`FlushTcpWrites` / receive drain） | `pending_drained` |
| `SetTcpReadBackpressure(false)` 成功后 | `tcp_read_backpressure_off` |

模式：在现有 `TraceHalfClose(*relay, trigger)` 处改为先更新状态，再：

```cpp
MaybePublishFullyClosedStopLocal(relay, trigger);
```

（helper 内部已 Trace；避免双打时可删除原 `TraceHalfClose` 或让 helper 在未 ready 时 Trace。）

- [ ] **Step 5: Run tests — expect PASS**

```bash
cmake --build build --config Release --target tcpquic_darwin_relay_worker_io_test -- -j8
./build/bin/Release/tcpquic_darwin_relay_worker_io_test
```

- [ ] **Step 6: Commit**

```bash
git add src/tunnel/darwin_relay_worker.h src/tunnel/darwin_relay_worker.cpp \
  src/unittest/darwin_relay_worker_io_test.cpp
git commit -m "$(cat <<'EOF'
feat(darwin-relay): SignalStop when fully-closed predicate is ready

Add worker-owned MaybePublishFullyClosedStopLocal aligned with Linux
MaybeStopFullyClosedRelay under product semantics B (FIN buffer released).
EOF
)"
```

---

### Task 3: Off-worker FIN completion → ConvergenceCheckSticky

**Files:**
- Modify: `src/tunnel/darwin_relay_worker.cpp`（`CompleteQuicSend` else 分支）
- Test: `src/unittest/darwin_relay_worker_io_test.cpp`

- [ ] **Step 1: 写失败测试 — fallback completion 不直接 stop，但 sticky 唤醒后 stop**

复用已有 `FallbackSendCompletionCount` / 强制 queue 满使 send completion inline 的测试模式（搜 `FallbackSendCompletionCount`）。最小断言：

```cpp
void FullyClosedOffWorkerFinSetsConvergenceStickyThenStops() {
    // Arrange: EnableQuicSends=true, submit FIN, force CompleteQuicSend off worker
    // (existing fallback path), assert ConvergenceCheckStickyForTest before drain,
    // after Consume + peer FIN + SHUT_WR, control->Stop == true.
    // 若构造成本过高：至少断言 off-worker FIN 置 ConvergenceCheckSticky 且
    // 不在 callback 线程调用 SignalStop（Stop 仍为 false 直到 DrainWake）。
}
```

实现者须对照现有 fallback 测试把 Arrange 写完整；禁止留 TBD。

- [ ] **Step 2: Run — expect FAIL**

- [ ] **Step 3: 实现**

在 `CompleteQuicSend` 的 `info.Fin` 且 `!(workerThread && !bindingRelayFallback)` 分支，更新 `QuicSendFinCompleted` 后：

```cpp
if (auto binding = relay->Binding) {
    binding->ConvergenceCheckSticky.store(true, std::memory_order_release);
    (void)Wake();
}
// 保留 quic_fin_buffer_released_off_worker trace；禁止 MaybePublish / 扫队列
```

`ConsumeHalfCloseStickies` 在 exchange `ConvergenceCheckSticky` 后调用：

```cpp
MaybePublishFullyClosedStopLocal(relay, "convergence_sticky");
```

- [ ] **Step 4: Run — expect PASS**

- [ ] **Step 5: Commit**

```bash
git commit -m "$(cat <<'EOF'
feat(darwin-relay): wake worker for fully-closed check after off-thread FIN

Off-worker CompleteQuicSend must not scan worker queues; set
ConvergenceCheckSticky and let the owner worker publish SignalStop.
EOF
)"
```

---

### Task 4: 测试矩阵补齐（反向顺序、callback pending、幂等）

**Files:**
- Test: `src/unittest/darwin_relay_worker_io_test.cpp`
- 如需：`.h` 增加 `CallbackPendingReceiveBytes` 注入或复用 `InvokeQuicReceiveViewForTest` + queue-full receive fallback

- [ ] **Step 1: 反向顺序测试**

```cpp
void FullyClosedStopPeerFinBeforeTcpEof() {
    ManagedRelayHarness harness;
    CHECK(harness.OpenSocketPair());
    CHECK(harness.Register(false));
    auto control = harness.Handle.Control;

    QUIC_STREAM_EVENT peerShutdown{};
    peerShutdown.Type = QUIC_STREAM_EVENT_PEER_SEND_SHUTDOWN;
    CHECK(harness.DispatchViaRouter(peerShutdown) == QUIC_STATUS_SUCCESS);
    CHECK(harness.Worker.DrainOneEventForTest());
    CHECK(harness.Worker.FlushTcpWritableForTest(harness.Result.RelayId));
    CHECK(!control->Stop.load(std::memory_order_acquire)); // 尚缺 TcpReadClosed

    CHECK(::shutdown(harness.Fds[0], SHUT_WR) == 0);
    CHECK(harness.Worker.InvokeTcpEventForTest(
        harness.Result.RelayId, EVFILT_READ, 0, 0));
    CHECK(control->Stop.load(std::memory_order_acquire));
    harness.StopAndClosePeer();
}
```

- [ ] **Step 2: callback pending 阻塞 stop**

构造 binding `CallbackPendingReceiveBytes > 0`（queue-full receive fallback 或 test hook），使谓词为假；排空后 stop。断言中间态 `!control->Stop`，排空后 `control->Stop`。

- [ ] **Step 3: 幂等 — 重复 peer FIN / 重复 sticky**

两次 `PEER_SEND_SHUTDOWN` + 重复置 sticky；`SignalStop` 仅首次增加 `TqRelayControlStopSignaledCount`（或 `Stop` 保持 true 且无崩溃）。

- [ ] **Step 4: Run full Darwin io + metrics tests**

```bash
cmake --build build --config Release --target tcpquic_darwin_relay_worker_io_test -- -j8
cmake --build build --config Release --target tcpquic_darwin_relay_metrics_test -- -j8
./build/bin/Release/tcpquic_darwin_relay_worker_io_test
./build/bin/Release/tcpquic_darwin_relay_metrics_test
```

Expected: PASS。

- [ ] **Step 5: Commit**

```bash
git commit -m "$(cat <<'EOF'
test(darwin-relay): cover fully-closed stop order, sticky, and pending recv

Lock reverse half-close order, callback-pending gate, and idempotent stop.
EOF
)"
```

---

### Task 5: 文档与手工回归清单

**Files:**
- Modify: `docs/2026-07-11-darwin-http-connect-zombie-relay.md`
- Modify: `docs/relay_macos.md`
- Modify: `docs/superpowers/specs/2026-07-11-darwin-fully-closed-convergence-design.md`

- [ ] **Step 1: 更新文档状态**

- 问题文档：状态 →「修复已实现（语义 B + sticky）」；§9.6 下增加「修复验证」小节（命令与期望 metrics）。
- `relay_macos.md` P0：由「待修收敛」改为「已修：fully-closed SignalStop」。
- design spec 状态 →「已实现」。

手工回归清单（写入问题文档）：

```text
1. 启动 client（HTTP CONNECT）
2. curl -x 127.0.0.1:18080 https://example.com/ 若干次后关闭
3. GET /api/v1/relay/metrics：
   - fully_closed_predicate_ready_relays 不长期 == active_relays
   - relay_control_stop_signaled 上升
   - active_relays 数秒内归零
4. rg 'fully_closed_stop|predicate_ready_no_signal_stop' log/client.log
   - 应见 fully_closed_stop；不应长期 predicate_ready_no_signal_stop
```

- [ ] **Step 2: Commit**

```bash
git add docs/2026-07-11-darwin-http-connect-zombie-relay.md docs/relay_macos.md \
  docs/superpowers/specs/2026-07-11-darwin-fully-closed-convergence-design.md
git commit -m "$(cat <<'EOF'
docs: mark Darwin fully-closed convergence as implemented

Update zombie-relay evidence doc, relay_macos P0, and design status.
EOF
)"
```

---

## Self-Review (plan vs spec)

| Spec 要求 | Task |
|-----------|------|
| 语义 B（`QuicSendFinCompleted`） | Task 2 谓 |
| Worker-only stop | Task 2 / 3 |
| Sticky peer FIN / send shutdown complete | Task 1 |
| ConvergenceCheckSticky off-worker | Task 3 |
| 谓词含 callback pending + sticky 未消费 | Task 2 / 4 |
| 触发矩阵 | Task 2 Step 4 |
| 测试：顺序 / 反向 / queue 满 / pending / 幂等 | Task 1, 2, 4 |
| 文档 | Task 5 |
| 不改 Linux/Windows | 文件边界 |

**Placeholder scan:** Task 3 Step 1 要求实现者补全 Arrange（对照现有 fallback 测试），不得提交 TBD 测试体。

**Canceled FIN:** 实现 Task 2 时核对 `CompleteQuicSend` / send path：若 `Canceled` 仍置 `QuicSendFinCompleted`，应改为不置位（与 design §4 一致）；若已正确则在 Task 2 commit message 注明「verified」。

---

## Execution Handoff

Plan complete and saved to `docs/superpowers/plans/2026-07-11-darwin-fully-closed-convergence.md`.

**Two execution options:**

1. **Subagent-Driven (recommended)** — 每 Task 新开 subagent，Task 间审查  
2. **Inline Execution** — 本会话按 executing-plans 批量推进并设检查点  

Which approach?
