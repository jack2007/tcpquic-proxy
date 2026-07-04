# Windows Relay Trace Context Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 修复 `docs/relay_win.md` 3.6 记录的 Windows relay trace context 跨线程直接写 `RelayContext::TraceTarget` 数据竞争。

**Architecture:** `TqRelaySetTraceContext()` 仍保持 fire-and-forget API，但 Windows 后端不再从调用线程直接写 relay 字段；`TqWindowsRelayWorker::SetRelayTraceContext()` 只向 worker IOCP 投递 `SetTraceContext` operation，并把 `tunnelId`、`target` 拷贝进 operation。worker 线程在 `Run()` 中解析 relay id/generation 后更新 `RelayContext::TraceTunnelId` 和 `TraceTarget`，因此 trace state 读写与现有 relay 状态推进保持同一线程模型。

**Tech Stack:** C++17, Windows IOCP, MsQuic stream callback, CMake, `tcpquic_windows_relay_worker_test`。

---

## File Structure

- Modify: `src/tunnel/windows_relay_worker.h`
- Modify: `src/tunnel/windows_relay_worker.cpp`
- Modify: `src/unittest/windows_relay_worker_test.cpp`
- Modify: `docs/relay_win.md`

## Design

### Current State

`TqRelaySetTraceContext()` 在 Windows 下调用 `TqWindowsRelayWorker::SetRelayTraceContext()`。当前实现通过 `FindRelayById()` 拿到 `std::shared_ptr<RelayContext>` 后，直接从调用线程写：

```cpp
relay->TraceTunnelId = tunnelId;
relay->TraceTarget = target;
```

worker 线程里的 `BuildRelayTraceState()` 同时读取：

```cpp
state.TunnelId = relay->TraceTunnelId;
state.Target = relay->TraceTarget.empty() ? nullptr : relay->TraceTarget.c_str();
```

`TraceTarget` 是 `std::string`，外部线程写与 worker 线程读之间没有同一把锁，也没有通过 IOCP 串行化，属于 C++ 数据竞争风险。

### Target Behavior

- `SetRelayTraceContext()` 必须把 `target` 内容复制进 `IoOperation`，不能保存调用方传入的裸指针。
- 对已启动 worker：投递 `TqWindowsIocpOperationType::SetTraceContext`，由 worker 线程执行字段写入。
- 对未启动、已停止、relay id 为 0、`tunnelId == 0` 或投递失败：静默返回，并在投递失败时递增 `Errors_`，保持现有 API 不向调用方返回错误的行为。
- 对迟到 operation：如果 relay 已退休或 generation 不匹配，直接丢弃，不复活 relay，不写 trace 字段。
- `BuildRelayTraceState()` 继续不加 trace mutex；实现后的不变量是 trace 字段只在 worker 线程写，测试 helper 只在单测中用于观察。

## Task 1: Add a Failing Regression Test

**Files:**
- Modify: `src/tunnel/windows_relay_worker.h`
- Modify: `src/tunnel/windows_relay_worker.cpp`
- Modify: `src/unittest/windows_relay_worker_test.cpp`

- [ ] **Step 1: Expose trace state for Windows worker tests**

In `src/tunnel/windows_relay_worker.h`, inside the existing `#if defined(TQ_UNIT_TESTING)` public helper block, add:

```cpp
    bool TestGetRelayTraceStateForTest(
        uint64_t relayId,
        TqTraceLinuxRelayStreamState* out) const;
```

In `src/tunnel/windows_relay_worker.cpp`, inside the `#if defined(TQ_UNIT_TESTING)` helper implementations, add:

```cpp
bool TqWindowsRelayWorker::TestGetRelayTraceStateForTest(
    uint64_t relayId,
    TqTraceLinuxRelayStreamState* out) const {
    if (out == nullptr) {
        return false;
    }
    auto relay = FindRelayByIdLocal(relayId);
    if (!relay) {
        return false;
    }
    *out = BuildRelayTraceState(relay);
    return true;
}
```

- [ ] **Step 2: Add the failing serialization test**

In `src/unittest/windows_relay_worker_test.cpp`, add this helper function before `main()`:

```cpp
bool TestWindowsRelayTraceContextUsesWorkerQueue() {
    QUIC_API_TABLE fakeApi{};
    fakeApi.StreamReceiveComplete = FakeStreamReceiveComplete;
    fakeApi.StreamReceiveSetEnabled = FakeStreamReceiveSetEnabled;
    MsQuic = reinterpret_cast<const MsQuicApi*>(&fakeApi);

    TqSocketHandle pair[2]{TqInvalidSocket, TqInvalidSocket};
    if (!TqSocketPair(pair)) {
        MsQuic = nullptr;
        return false;
    }
    if (!TqSetNonBlocking(pair[1])) {
        TqCloseSocket(pair[0]);
        TqCloseSocket(pair[1]);
        MsQuic = nullptr;
        return false;
    }

    TqWindowsRelayWorker worker;
    if (!worker.Start()) {
        TqCloseSocket(pair[0]);
        TqCloseSocket(pair[1]);
        MsQuic = nullptr;
        return false;
    }

    alignas(MsQuicStream) unsigned char streamStorage[sizeof(MsQuicStream)]{};
    auto* stream = reinterpret_cast<MsQuicStream*>(streamStorage);
    stream->Callback = MsQuicStream::NoOpCallback;
    stream->Context = nullptr;
    stream->Handle = reinterpret_cast<HQUIC>(static_cast<uintptr_t>(1));

    TqRelayHandle handle{};
    TqTuningConfig tuning{};
    tuning.RelayIoSize = 4096;
    if (!worker.RegisterRelay(pair[0], stream, nullptr, nullptr, &handle, tuning, TqCompressAlgo::None)) {
        worker.Stop();
        TqCloseSocket(pair[1]);
        MsQuic = nullptr;
        return false;
    }

    TqTraceLinuxRelayStreamState before{};
    if (!worker.TestGetRelayTraceStateForTest(handle.WindowsRelayId, &before)) {
        worker.Stop();
        TqCloseSocket(pair[1]);
        MsQuic = nullptr;
        return false;
    }
    if (before.TunnelId != 0 || before.Target != nullptr) {
        worker.Stop();
        TqCloseSocket(pair[1]);
        MsQuic = nullptr;
        return false;
    }

    worker.SetRelayTraceContext(handle.WindowsRelayId, 12345, "queued.example:443");

    TqTraceLinuxRelayStreamState immediate{};
    if (!worker.TestGetRelayTraceStateForTest(handle.WindowsRelayId, &immediate)) {
        worker.Stop();
        TqCloseSocket(pair[1]);
        MsQuic = nullptr;
        return false;
    }
    if (immediate.TunnelId != 0 || immediate.Target != nullptr) {
        worker.Stop();
        TqCloseSocket(pair[1]);
        MsQuic = nullptr;
        return false;
    }

    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(2000);
    bool sawTrace = false;
    while (std::chrono::steady_clock::now() < deadline) {
        TqTraceLinuxRelayStreamState after{};
        if (worker.TestGetRelayTraceStateForTest(handle.WindowsRelayId, &after) &&
            after.TunnelId == 12345 &&
            after.Target != nullptr &&
            std::strcmp(after.Target, "queued.example:443") == 0) {
            sawTrace = true;
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }

    worker.Stop();
    TqCloseSocket(pair[1]);
    MsQuic = nullptr;
    return sawTrace;
}
```

In `main()`, add a guarded call before the final `return 0;`:

```cpp
    if (!TestWindowsRelayTraceContextUsesWorkerQueue()) {
        return 123;
    }
```

- [ ] **Step 3: Run the test and verify it fails for the right reason**

Run on Windows:

```bash
rtk cmake --build build --target tcpquic_windows_relay_worker_test -j
rtk build/bin/Release/tcpquic_windows_relay_worker_test
```

Expected: the test executable returns `123` because the current `SetRelayTraceContext()` writes `TraceTunnelId` and `TraceTarget` immediately from the caller thread, so the “immediate” trace state is already populated.

## Task 2: Add a Worker-Serialized Trace Context Operation

**Files:**
- Modify: `src/tunnel/windows_relay_worker.h`
- Modify: `src/tunnel/windows_relay_worker.cpp`

- [ ] **Step 1: Add the IOCP operation type and storage**

In `src/tunnel/windows_relay_worker.h`, extend `TqWindowsIocpOperationType` after `CloseRelay`:

```cpp
    SetTraceContext,
```

In `src/tunnel/windows_relay_worker.cpp`, extend `struct TqWindowsRelayWorker::IoOperation` with a copied target string:

```cpp
    std::string Text;
```

Because `windows_relay_worker.cpp` already uses `std::string` in `RelayContext`, no new include is needed unless the compiler reports one. If it does, add this near the other standard includes in `windows_relay_worker.cpp`:

```cpp
#include <string>
```

- [ ] **Step 2: Add a worker-local handler declaration**

In `src/tunnel/windows_relay_worker.h`, add this private method near `StopRelay()`/callback operation helpers:

```cpp
    void ApplyRelayTraceContext(
        const std::shared_ptr<RelayContext>& relay,
        uint64_t tunnelId,
        const std::string& target);
```

- [ ] **Step 3: Implement the worker-local handler**

In `src/tunnel/windows_relay_worker.cpp`, add the implementation before `SetRelayTraceContext()`:

```cpp
void TqWindowsRelayWorker::ApplyRelayTraceContext(
    const std::shared_ptr<RelayContext>& relay,
    uint64_t tunnelId,
    const std::string& target) {
    if (!relay || tunnelId == 0) {
        return;
    }
    relay->TraceTunnelId = tunnelId;
    relay->TraceTarget = target;
}
```

- [ ] **Step 4: Change `SetRelayTraceContext()` to only post an IOCP operation**

Replace the body of `TqWindowsRelayWorker::SetRelayTraceContext()` in `src/tunnel/windows_relay_worker.cpp` with:

```cpp
void TqWindowsRelayWorker::SetRelayTraceContext(
    uint64_t relayId,
    uint64_t tunnelId,
    const char* target) {
    if (relayId == 0 || tunnelId == 0 || Iocp_ == nullptr) {
        return;
    }

    auto relay = FindRelayById(relayId);
    if (!relay) {
        return;
    }

    auto op = std::make_unique<IoOperation>();
    op->Event = TqWindowsIocpOperationType::SetTraceContext;
    op->RelayId = relayId;
    op->Generation = relay->Generation;
    op->Value = tunnelId;
    if (target != nullptr) {
        op->Text = target;
    }

    IoOperation* raw = op.release();
    if (!::PostQueuedCompletionStatus(static_cast<HANDLE>(Iocp_), 0, 0, &raw->Overlapped)) {
        delete raw;
        Errors_.fetch_add(1, std::memory_order_relaxed);
    }
}
```

This version still reads `relay->Generation` from the caller thread after `FindRelayById()`, but it does not write `TraceTunnelId` or `TraceTarget`. The generation is immutable for the relay lifetime and is already used by callback-sourced operations to drop stale work.

- [ ] **Step 5: Dispatch `SetTraceContext` in the worker loop**

In `TqWindowsRelayWorker::Run()`, update relay resolution so `SetTraceContext` behaves like `CloseRelay` and resolves by `RelayId`:

```cpp
        if (!relay &&
            (op->Event == TqWindowsIocpOperationType::CloseRelay ||
             op->Event == TqWindowsIocpOperationType::SetTraceContext) &&
            op->RelayId != 0) {
            relay = FindRelayByIdLocal(op->RelayId);
            if (relay && op->Generation != 0 && relay->Generation != op->Generation) {
                relay.reset();
            }
        }
```

Replace the existing single-event `CloseRelay` block with this combined block.

Then add a `switch` case before `CloseRelay`:

```cpp
        case TqWindowsIocpOperationType::SetTraceContext:
            ApplyRelayTraceContext(relay, op->Value, op->Text);
            break;
```

- [ ] **Step 6: Verify the focused test passes**

Run on Windows:

```bash
rtk cmake --build build --target tcpquic_windows_relay_worker_test -j
rtk build/bin/Release/tcpquic_windows_relay_worker_test
```

Expected: `tcpquic_windows_relay_worker_test` exits with code `0`.

## Task 3: Add Stale Operation and Null Target Coverage

**Files:**
- Modify: `src/unittest/windows_relay_worker_test.cpp`
- Modify: `src/tunnel/windows_relay_worker.h`
- Modify: `src/tunnel/windows_relay_worker.cpp`

- [ ] **Step 1: Add a test helper for stale trace operation injection**

In `src/tunnel/windows_relay_worker.h`, inside the `TQ_UNIT_TESTING` block, add:

```cpp
    bool TestPostTraceContextForTest(
        uint64_t relayId,
        uint64_t generation,
        uint64_t tunnelId,
        const char* target);
```

In `src/tunnel/windows_relay_worker.cpp`, add:

```cpp
bool TqWindowsRelayWorker::TestPostTraceContextForTest(
    uint64_t relayId,
    uint64_t generation,
    uint64_t tunnelId,
    const char* target) {
    if (Iocp_ == nullptr || relayId == 0 || tunnelId == 0) {
        return false;
    }
    auto op = std::make_unique<IoOperation>();
    op->Event = TqWindowsIocpOperationType::SetTraceContext;
    op->RelayId = relayId;
    op->Generation = generation;
    op->Value = tunnelId;
    if (target != nullptr) {
        op->Text = target;
    }
    IoOperation* raw = op.release();
    if (!::PostQueuedCompletionStatus(static_cast<HANDLE>(Iocp_), 0, 0, &raw->Overlapped)) {
        delete raw;
        return false;
    }
    return true;
}
```

- [ ] **Step 2: Add stale generation coverage**

In `src/unittest/windows_relay_worker_test.cpp`, add this helper before `main()`:

```cpp
bool TestWindowsRelayTraceContextDropsStaleGeneration() {
    QUIC_API_TABLE fakeApi{};
    fakeApi.StreamReceiveComplete = FakeStreamReceiveComplete;
    fakeApi.StreamReceiveSetEnabled = FakeStreamReceiveSetEnabled;
    MsQuic = reinterpret_cast<const MsQuicApi*>(&fakeApi);

    TqSocketHandle pair[2]{TqInvalidSocket, TqInvalidSocket};
    if (!TqSocketPair(pair)) {
        MsQuic = nullptr;
        return false;
    }

    TqWindowsRelayWorker worker;
    if (!worker.Start()) {
        TqCloseSocket(pair[0]);
        TqCloseSocket(pair[1]);
        MsQuic = nullptr;
        return false;
    }

    alignas(MsQuicStream) unsigned char streamStorage[sizeof(MsQuicStream)]{};
    auto* stream = reinterpret_cast<MsQuicStream*>(streamStorage);
    stream->Callback = MsQuicStream::NoOpCallback;
    stream->Context = nullptr;
    stream->Handle = reinterpret_cast<HQUIC>(static_cast<uintptr_t>(1));

    TqRelayHandle handle{};
    TqTuningConfig tuning{};
    tuning.RelayIoSize = 4096;
    if (!worker.RegisterRelay(pair[0], stream, nullptr, nullptr, &handle, tuning, TqCompressAlgo::None)) {
        worker.Stop();
        TqCloseSocket(pair[1]);
        MsQuic = nullptr;
        return false;
    }

    const uint64_t staleGeneration = 999999;
    if (!worker.TestPostTraceContextForTest(
            handle.WindowsRelayId,
            staleGeneration,
            777,
            "stale.example:443")) {
        worker.Stop();
        TqCloseSocket(pair[1]);
        MsQuic = nullptr;
        return false;
    }

    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    TqTraceLinuxRelayStreamState state{};
    const bool ok = worker.TestGetRelayTraceStateForTest(handle.WindowsRelayId, &state);
    worker.Stop();
    TqCloseSocket(pair[1]);
    MsQuic = nullptr;
    return ok && state.TunnelId == 0 && state.Target == nullptr;
}
```

Add this call in `main()`:

```cpp
    if (!TestWindowsRelayTraceContextDropsStaleGeneration()) {
        return 124;
    }
```

- [ ] **Step 3: Add null target coverage**

In `src/unittest/windows_relay_worker_test.cpp`, add this helper before `main()`:

```cpp
bool TestWindowsRelayTraceContextAllowsNullTarget() {
    QUIC_API_TABLE fakeApi{};
    fakeApi.StreamReceiveComplete = FakeStreamReceiveComplete;
    fakeApi.StreamReceiveSetEnabled = FakeStreamReceiveSetEnabled;
    MsQuic = reinterpret_cast<const MsQuicApi*>(&fakeApi);

    TqSocketHandle pair[2]{TqInvalidSocket, TqInvalidSocket};
    if (!TqSocketPair(pair)) {
        MsQuic = nullptr;
        return false;
    }

    TqWindowsRelayWorker worker;
    if (!worker.Start()) {
        TqCloseSocket(pair[0]);
        TqCloseSocket(pair[1]);
        MsQuic = nullptr;
        return false;
    }

    alignas(MsQuicStream) unsigned char streamStorage[sizeof(MsQuicStream)]{};
    auto* stream = reinterpret_cast<MsQuicStream*>(streamStorage);
    stream->Callback = MsQuicStream::NoOpCallback;
    stream->Context = nullptr;
    stream->Handle = reinterpret_cast<HQUIC>(static_cast<uintptr_t>(1));

    TqRelayHandle handle{};
    TqTuningConfig tuning{};
    tuning.RelayIoSize = 4096;
    if (!worker.RegisterRelay(pair[0], stream, nullptr, nullptr, &handle, tuning, TqCompressAlgo::None)) {
        worker.Stop();
        TqCloseSocket(pair[1]);
        MsQuic = nullptr;
        return false;
    }

    worker.SetRelayTraceContext(handle.WindowsRelayId, 888, nullptr);

    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(2000);
    bool sawTrace = false;
    while (std::chrono::steady_clock::now() < deadline) {
        TqTraceLinuxRelayStreamState state{};
        if (worker.TestGetRelayTraceStateForTest(handle.WindowsRelayId, &state) &&
            state.TunnelId == 888 &&
            state.Target == nullptr) {
            sawTrace = true;
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }

    worker.Stop();
    TqCloseSocket(pair[1]);
    MsQuic = nullptr;
    return sawTrace;
}
```

Add this call in `main()`:

```cpp
    if (!TestWindowsRelayTraceContextAllowsNullTarget()) {
        return 125;
    }
```

- [ ] **Step 4: Run the focused test again**

Run on Windows:

```bash
rtk cmake --build build --target tcpquic_windows_relay_worker_test -j
rtk build/bin/Release/tcpquic_windows_relay_worker_test
```

Expected: `tcpquic_windows_relay_worker_test` exits with code `0`, including return-code paths `123`、`124`、`125` not triggered.

## Task 4: Update Documentation and Run Final Verification

**Files:**
- Modify: `docs/relay_win.md`

- [ ] **Step 1: Update section 3.6**

Replace the recommendation text under `### 3.6 trace context 从外部线程直接写入 relay 字段` with:

```markdown
修复方向：

- `TqWindowsRelayWorker::SetRelayTraceContext()` 不直接写 `RelayContext`，而是复制 `target` 并投递 `SetTraceContext` IOCP operation。
- worker 线程解析 relay id/generation 后更新 `TraceTunnelId` 和 `TraceTarget`，与 `BuildRelayTraceState()` 的读取保持同一 worker 串行模型。
- 迟到 operation 在 relay 不存在或 generation 不匹配时丢弃，不复活 relay。

验证：

- `tcpquic_windows_relay_worker_test` 覆盖 trace context 不会从调用线程立即写入、stale generation 被丢弃、`target == nullptr` 时只设置 tunnel id 并保持空 target。
```

- [ ] **Step 2: Run Windows relay tests**

Run on Windows:

```bash
rtk cmake --build build --target tcpquic_windows_relay_worker_test tcpquic_windows_reactor_test -j
rtk build/bin/Release/tcpquic_windows_relay_worker_test
rtk build/bin/Release/tcpquic_windows_reactor_test
```

Expected: both executables exit with code `0`.

- [ ] **Step 3: Build the main target on Windows**

Run on Windows:

```bash
rtk cmake --build build --target tcpquic-proxy -j
```

Expected: build completes without compile or link errors.

- [ ] **Step 4: Commit**

Run:

```bash
rtk git status --short
rtk git add src/tunnel/windows_relay_worker.h src/tunnel/windows_relay_worker.cpp src/unittest/windows_relay_worker_test.cpp docs/relay_win.md
rtk git commit -m "fix: serialize windows relay trace context updates"
```

Expected: commit contains only the Windows trace context fix, regression tests, and the section 3.6 documentation update.

## Self-Review

- Spec coverage: 计划覆盖 `docs/relay_win.md` 3.6 的主建议“投递 IOCP operation，由 worker 线程更新 trace 字段”，并保留 `target == nullptr` 语义和迟到 operation 防护。
- Placeholder scan: 本计划没有遗留占位标记，也没有未展开的“写测试”步骤。
- Type consistency: 新 operation 名为 `SetTraceContext`，handler 名为 `ApplyRelayTraceContext()`，测试 helper 名为 `TestGetRelayTraceStateForTest()` 和 `TestPostTraceContextForTest()`；这些名称在任务间保持一致。
