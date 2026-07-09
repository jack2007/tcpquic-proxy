# Linux Relay Stream Lifetime Hardening Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 修复 Linux relay 在 stream shutdown complete 后收到 late TCP socket error 时访问失效 MsQuic stream handle 导致段错误的问题。

**Architecture:** 在 Linux relay 中显式记录 stream shutdown 生命周期状态，并在 shutdown complete 时 detach stream binding。`AbortRelayAndRelease()` 只在 relay 仍拥有 active stream binding 且未进入 `Closing`、`StreamShutdownComplete`、`TcpWriteClosed/tcp_write_shutdown_complete` 这些终态时调用 `MsQuicStream::Shutdown()`；late TCP error 只清理 TCP 和 relay 本地队列。新增一个诊断计数贯通 worker snapshot、relay metrics 和 admin JSON。

**Tech Stack:** C++17, MsQuic C++ wrapper, Linux epoll relay worker, CMake unit tests, project `rtk` command wrapper.

---

## File Structure

- Modify: `src/tunnel/linux_relay_worker.h`
  - Add `LateTcpErrorAfterStreamShutdown` to `TqLinuxRelayWorkerSnapshot`.
  - Add `std::atomic<uint64_t> LateTcpErrorAfterStreamShutdown`.
  - Declare `HasAbortableStream(RelayState*)`.
- Modify: `src/tunnel/linux_relay_worker.cpp`
  - Add `RelayState::StreamShutdownComplete`.
  - Set `StreamShutdownComplete` during `ProcessQuicShutdownComplete()`.
  - Detach stream binding before late local cleanup can reuse the stream pointer.
  - Gate stream abort in `AbortRelayAndRelease()` behind `HasAbortableStream()`.
  - Make `HasAbortableStream()` return false for `Closing`, `StreamShutdownComplete`, and `TcpWriteClosed`.
  - Compare Linux stream API call sites against the Windows/Darwin close model: only active relay + active binding may call stream APIs.
  - Increment late TCP error metric in `ProcessTcpEvents()`.
  - Populate and aggregate the new metric in snapshots.
- Modify: `src/runtime/relay_metrics.h`
  - Add `LinuxRelayLateTcpErrorAfterStreamShutdown`.
- Modify: `src/runtime/relay_metrics.cpp`
  - Copy the new Linux snapshot field into `TqRelayMetricsSnapshot`.
  - Emit `linux_relay_late_tcp_error_after_stream_shutdown`.
- Modify: `src/unittest/linux_relay_worker_io_test.cpp`
  - Count fake `StreamShutdown` calls.
  - Add shutdown-complete + late TCP error regression test.
  - Add tcp-write-shutdown-complete + late TCP error regression test.
  - Add active-stream TCP hard error guard test.
- Modify: `src/unittest/router_runtime_test.cpp`
  - Assert admin metrics JSON includes the new field.

## Task 1: Add Regression Tests for Shutdown-Complete Late TCP Error

**Files:**
- Modify: `src/unittest/linux_relay_worker_io_test.cpp`

- [ ] **Step 1: Add fake StreamShutdown counters**

Near the existing fake MsQuic helpers at the top of `src/unittest/linux_relay_worker_io_test.cpp`, replace:

```cpp
QUIC_STATUS QUIC_API FakeStreamShutdown(HQUIC, QUIC_STREAM_SHUTDOWN_FLAGS, QUIC_UINT62) {
    return QUIC_STATUS_SUCCESS;
}
```

with:

```cpp
std::mutex g_FakeStreamShutdownMutex;
uint64_t g_FakeStreamShutdownCalls = 0;

QUIC_STATUS QUIC_API FakeStreamShutdown(HQUIC, QUIC_STREAM_SHUTDOWN_FLAGS, QUIC_UINT62) {
    std::lock_guard<std::mutex> guard(g_FakeStreamShutdownMutex);
    ++g_FakeStreamShutdownCalls;
    return QUIC_STATUS_SUCCESS;
}

void ResetFakeStreamShutdownCalls() {
    std::lock_guard<std::mutex> guard(g_FakeStreamShutdownMutex);
    g_FakeStreamShutdownCalls = 0;
}

uint64_t ReadFakeStreamShutdownCalls() {
    std::lock_guard<std::mutex> guard(g_FakeStreamShutdownMutex);
    return g_FakeStreamShutdownCalls;
}
```

- [ ] **Step 2: Add the late TCP error test**

Insert this block near the other shutdown-complete relay tests in `main()`:

```cpp
    {
        QUIC_API_TABLE fakeApi{};
        InstallFakeMsQuicForSend(fakeApi);
        ResetFakeStreamShutdownCalls();

        TqLinuxRelayWorkerConfig config{};
        config.EventBudget = 128;
        config.ReadChunkSize = 4096;
        config.ReadBatchBytes = 64 * 1024;
        config.MaxIov = 8;
        config.MaxPendingBufferBytes = 256 * 1024;

        TqLinuxRelayWorker worker(config);
        if (!worker.StartForTest()) {
            MsQuic = nullptr;
            return 4101;
        }

        int fds[2]{-1, -1};
        if (!MakeTcpLoopbackPair(fds)) {
            worker.Stop();
            MsQuic = nullptr;
            return 4102;
        }

        alignas(MsQuicStream) uint8_t fakeStreamStorage[sizeof(MsQuicStream)]{};
        std::memset(fakeStreamStorage, 0, sizeof(fakeStreamStorage));
        MsQuicStream* fakeStream = reinterpret_cast<MsQuicStream*>(fakeStreamStorage);
        fakeStream->Handle = reinterpret_cast<HQUIC>(static_cast<uintptr_t>(0x1234));

        TqRelayHandle handle{};
        TqLinuxRelayRegistration registration{};
        registration.TcpFd = fds[0];
        registration.Stream = fakeStream;
        registration.Handle = &handle;
        registration.EnableQuicSends = false;
        const TqLinuxRelayRegistrationResult registered = worker.RegisterRelayWithId(registration);
        if (!registered.Ok) {
            worker.Stop();
            ::close(fds[1]);
            MsQuic = nullptr;
            return 4103;
        }

        QUIC_STREAM_EVENT shutdownEvent{};
        shutdownEvent.Type = QUIC_STREAM_EVENT_SHUTDOWN_COMPLETE;
        if (QUIC_FAILED(worker.DispatchStreamEventForTest(fakeStream, &shutdownEvent))) {
            worker.Stop();
            ::close(fds[1]);
            MsQuic = nullptr;
            return 4104;
        }

        const TqLinuxRelayWorkerSnapshot afterShutdown = worker.Snapshot();
        bool streamDetached = false;
        for (const auto& relay : afterShutdown.ActiveRelayStates) {
            if (relay.RelayId == registered.RelayId) {
                streamDetached = relay.StreamDetached;
            }
        }
        if (!streamDetached) {
            std::fprintf(stderr, "expected stream detached after shutdown complete\n");
            worker.Stop();
            ::close(fds[1]);
            MsQuic = nullptr;
            return 4105;
        }

        linger rst{};
        rst.l_onoff = 1;
        rst.l_linger = 0;
        (void)::setsockopt(fds[1], SOL_SOCKET, SO_LINGER, &rst, sizeof(rst));
        ::close(fds[1]);
        fds[1] = -1;

        const uint64_t beforeShutdownCalls = ReadFakeStreamShutdownCalls();
        if (!worker.DispatchEncodedEpollEventForTest(
                worker.EncodeEpollRelayForTest(registered.RelayId),
                EPOLLERR | EPOLLHUP | EPOLLRDHUP)) {
            worker.Stop();
            if (fds[1] >= 0) {
                ::close(fds[1]);
            }
            MsQuic = nullptr;
            return 4106;
        }

        const TqLinuxRelayWorkerSnapshot afterLateError = worker.Snapshot();
        if (ReadFakeStreamShutdownCalls() != beforeShutdownCalls) {
            std::fprintf(stderr, "late TCP error after shutdown complete must not abort stream\n");
            worker.Stop();
            if (fds[1] >= 0) {
                ::close(fds[1]);
            }
            MsQuic = nullptr;
            return 4107;
        }
        if (afterLateError.LateTcpErrorAfterStreamShutdown == 0) {
            std::fprintf(stderr, "expected late TCP error after stream shutdown metric\n");
            worker.Stop();
            if (fds[1] >= 0) {
                ::close(fds[1]);
            }
            MsQuic = nullptr;
            return 4108;
        }

        worker.Stop();
        if (fds[1] >= 0) {
            ::close(fds[1]);
        }
        MsQuic = nullptr;
    }
```

This uses the existing `MakeTcpLoopbackPair()` helper plus `SO_LINGER {1,0}` to create a TCP reset, then dispatches the encoded relay event directly so the regression does not depend on `epoll_wait()` timing.

- [ ] **Step 3: Add the tcp-write-shutdown-complete hard error guard test**

Add a second negative-abort test for the local terminal close path:

1. Register a relay with a fake `MsQuicStream`.
2. Drive the relay until snapshot state for that relay reports `TcpWriteClosed == true`, using an existing QUIC receive FIN / TCP write shutdown-complete test helper if present.
3. Do not dispatch `QUIC_STREAM_EVENT_SHUTDOWN_COMPLETE` in this test; the purpose is to prove `tcp_write_shutdown_complete` alone makes the stream non-abortable.
4. Reset the TCP peer with `SO_LINGER {1,0}` and dispatch `EPOLLERR | EPOLLHUP | EPOLLRDHUP`.
5. Assert `ReadFakeStreamShutdownCalls()` does not increase after `TcpWriteClosed == true`.

The failure message should include:

```cpp
std::fprintf(stderr, "late TCP error after tcp_write_shutdown_complete must not abort stream\n");
```

- [ ] **Step 4: Add the active-stream hard error guard test**

Insert this block after the late TCP error test:

```cpp
    {
        QUIC_API_TABLE fakeApi{};
        InstallFakeMsQuicForSend(fakeApi);
        ResetFakeStreamShutdownCalls();

        TqLinuxRelayWorkerConfig config{};
        config.EventBudget = 128;
        TqLinuxRelayWorker worker(config);
        if (!worker.StartForTest()) {
            MsQuic = nullptr;
            return 4111;
        }

        int fds[2]{-1, -1};
        if (!MakeTcpLoopbackPair(fds)) {
            worker.Stop();
            MsQuic = nullptr;
            return 4112;
        }

        alignas(MsQuicStream) uint8_t fakeStreamStorage[sizeof(MsQuicStream)]{};
        std::memset(fakeStreamStorage, 0, sizeof(fakeStreamStorage));
        MsQuicStream* fakeStream = reinterpret_cast<MsQuicStream*>(fakeStreamStorage);
        fakeStream->Handle = reinterpret_cast<HQUIC>(static_cast<uintptr_t>(0x5678));

        TqRelayHandle handle{};
        TqLinuxRelayRegistration registration{};
        registration.TcpFd = fds[0];
        registration.Stream = fakeStream;
        registration.Handle = &handle;
        registration.EnableQuicSends = false;
        const TqLinuxRelayRegistrationResult registered = worker.RegisterRelayWithId(registration);
        if (!registered.Ok) {
            worker.Stop();
            ::close(fds[1]);
            MsQuic = nullptr;
            return 4113;
        }

        linger rst{};
        rst.l_onoff = 1;
        rst.l_linger = 0;
        (void)::setsockopt(fds[1], SOL_SOCKET, SO_LINGER, &rst, sizeof(rst));
        ::close(fds[1]);
        fds[1] = -1;

        if (!worker.DispatchEncodedEpollEventForTest(
                worker.EncodeEpollRelayForTest(registered.RelayId),
                EPOLLERR | EPOLLHUP | EPOLLRDHUP)) {
            worker.Stop();
            if (fds[1] >= 0) {
                ::close(fds[1]);
            }
            MsQuic = nullptr;
            return 4114;
        }

        if (ReadFakeStreamShutdownCalls() != 1) {
            std::fprintf(stderr, "active stream hard TCP error must abort stream exactly once\n");
            worker.Stop();
            if (fds[1] >= 0) {
                ::close(fds[1]);
            }
            MsQuic = nullptr;
            return 4115;
        }

        worker.Stop();
        if (fds[1] >= 0) {
            ::close(fds[1]);
        }
        MsQuic = nullptr;
    }
```

- [ ] **Step 5: Run the focused test before implementation**

Run:

```bash
rtk cmake --build build --target tcpquic_linux_relay_worker_io_test -j$(nproc)
rtk build/bin/Release/tcpquic_linux_relay_worker_io_test
```

Expected before implementation: build fails because `LateTcpErrorAfterStreamShutdown` does not exist, or the late TCP error tests fail because stream is not detached / `TcpWriteClosed` is not treated as non-abortable / shutdown is called.

- [ ] **Step 6: Commit the failing tests**

```bash
rtk git add src/unittest/linux_relay_worker_io_test.cpp
rtk git commit -m "test: cover linux relay late tcp error after stream shutdown"
```

## Task 2: Add Stream Shutdown State and Abort Guard

**Files:**
- Modify: `src/tunnel/linux_relay_worker.h`
- Modify: `src/tunnel/linux_relay_worker.cpp`

- [ ] **Step 1: Add snapshot and worker counter fields**

In `TqLinuxRelayWorkerSnapshot` in `src/tunnel/linux_relay_worker.h`, add after `FakeFinReceiveCount`:

```cpp
    uint64_t LateTcpErrorAfterStreamShutdown{0};
```

In the worker atomic counters in `src/tunnel/linux_relay_worker.h`, add after `FakeFinReceiveCount`:

```cpp
    std::atomic<uint64_t> LateTcpErrorAfterStreamShutdown{0};
```

- [ ] **Step 2: Declare the abortability helper**

In the private method declarations in `src/tunnel/linux_relay_worker.h`, add near `AbortRelayAndRelease(...)`:

```cpp
    bool HasAbortableStream(RelayState* relay) const;
```

- [ ] **Step 3: Add relay stream shutdown state**

In `struct TqLinuxRelayWorker::RelayState` in `src/tunnel/linux_relay_worker.cpp`, add after `bool QuicSendFinCompleted{false};`:

```cpp
    bool StreamShutdownComplete{false};
```

- [ ] **Step 4: Implement `HasAbortableStream()`**

Add this function before `AbortRelayAndRelease()`:

```cpp
bool TqLinuxRelayWorker::HasAbortableStream(RelayState* relay) const {
    if (relay == nullptr ||
        relay->Closing ||
        relay->StreamShutdownComplete ||
        relay->TcpWriteClosed ||
        relay->Stream == nullptr) {
        return false;
    }
    auto* binding = relay->StreamBinding;
    if (binding == nullptr || binding->Closing.load(std::memory_order_acquire)) {
        return false;
    }
    if (binding->Relay.load(std::memory_order_acquire) != relay) {
        return false;
    }
    return relay->Stream->Handle != nullptr;
}
```

This intentionally repeats the `Closing` state even though `AbortRelayAndRelease()` also has `alreadyClosing`; the helper must be safe if future stream-abort call sites use it directly.

- [ ] **Step 5: Gate stream abort in `AbortRelayAndRelease()`**

In `TqLinuxRelayWorker::AbortRelayAndRelease()`, replace:

```cpp
    auto* binding = relay->StreamBinding;
    if (!alreadyClosing && abortStream && relay->Stream != nullptr && relay->Stream->Handle != nullptr) {
        (void)relay->Stream->Shutdown(0, QUIC_STREAM_SHUTDOWN_FLAG_ABORT);
    }
```

with:

```cpp
    auto* binding = relay->StreamBinding;
    if (!alreadyClosing && abortStream && HasAbortableStream(relay)) {
        (void)relay->Stream->Shutdown(0, QUIC_STREAM_SHUTDOWN_FLAG_ABORT);
    }
```

This preserves active-stream abort behavior and blocks closing, shutdown-complete, and tcp-write-shutdown-complete late errors from dereferencing stale stream state.

- [ ] **Step 6: Mark and detach on stream shutdown complete**

In `TqLinuxRelayWorker::ProcessQuicShutdownComplete()`, after the null/closing guard and before trace emission, add:

```cpp
    relay->StreamShutdownComplete = true;
```

Then make the detach unconditional before the pending-state branch:

```cpp
    auto* binding = relay->StreamBinding;
    MsQuicStream* stream = relay->Stream;
    DetachRelayStreamBinding(relay.get(), stream, binding);
```

The resulting structure should be:

```cpp
    relay->StreamShutdownComplete = true;
    TraceRelayStreamEvent(...);
    ...
    auto* binding = relay->StreamBinding;
    MsQuicStream* stream = relay->Stream;
    DetachRelayStreamBinding(relay.get(), stream, binding);

    const bool hasPending = HasPendingAfterStreamShutdown(relay.get());
    if (hasPending) {
        ...
        AbortRelayAndRelease(relay.get(), "stream_shutdown_complete_with_pending", false);
    } else {
        MaybeStopFullyClosedRelay(relay.get(), "stream_shutdown_complete");
    }
```

Do not pass `abortStream=true` from the shutdown-complete-with-pending branch; the stream is already complete.

- [ ] **Step 7: Audit stream API ownership against Windows/Darwin close model**

Before running tests, inspect Linux stream API call sites and verify they are either:

- protected by `HasAbortableStream()`, or
- in a path that has just received an active stream callback before detach, or
- part of `DetachRelayStreamBinding()` itself.

Use Windows/Darwin as the behavioral reference: once `Closing`, binding closing, or `TcpWriteClosed` is observed, late network events must not call stream APIs again.

Run:

```bash
rtk rg -n "->Shutdown\\(|SetCallbackHandler|SetContext|StreamBinding|TcpWriteClosed|Closing" src/tunnel/linux_relay_worker.cpp src/tunnel/windows_relay_worker.cpp src/tunnel/darwin_relay_worker.cpp
```

Expected: Linux stream aborts are centralized behind `HasAbortableStream()` and shutdown-complete detach; there is no remaining late-error path that calls `Stream::Shutdown()` based only on non-null stream pointers.

- [ ] **Step 8: Run the focused test**

Run:

```bash
rtk cmake --build build --target tcpquic_linux_relay_worker_io_test -j$(nproc)
rtk build/bin/Release/tcpquic_linux_relay_worker_io_test
```

Expected: the new late-error test and active-stream guard test pass.

- [ ] **Step 9: Commit lifecycle fix**

```bash
rtk git add src/tunnel/linux_relay_worker.h src/tunnel/linux_relay_worker.cpp src/unittest/linux_relay_worker_io_test.cpp
rtk git commit -m "fix: detach linux relay stream after shutdown complete"
```

## Task 3: Count Late TCP Errors After Stream Shutdown

**Files:**
- Modify: `src/tunnel/linux_relay_worker.cpp`

- [ ] **Step 1: Increment worker metric in TCP error path**

In `TqLinuxRelayWorker::ProcessTcpEvents()`, inside:

```cpp
        if (socketError != 0) {
```

add before `relay->LastTcpWriteErrno = ...`:

```cpp
            if (relay->StreamShutdownComplete) {
                LateTcpErrorAfterStreamShutdown.fetch_add(1);
            }
```

- [ ] **Step 2: Populate local snapshot**

In `TqLinuxRelayWorker::SnapshotLocal()`, after `snapshot.FakeFinReceiveCount = FakeFinReceiveCount.load();`, add:

```cpp
    snapshot.LateTcpErrorAfterStreamShutdown =
        LateTcpErrorAfterStreamShutdown.load();
```

- [ ] **Step 3: Aggregate runtime snapshot**

In `TqLinuxRelayRuntime::Snapshot()`, after `total.FakeFinReceiveCount += snapshot.FakeFinReceiveCount;`, add:

```cpp
        total.LateTcpErrorAfterStreamShutdown += snapshot.LateTcpErrorAfterStreamShutdown;
```

- [ ] **Step 4: Run the focused test**

Run:

```bash
rtk cmake --build build --target tcpquic_linux_relay_worker_io_test -j$(nproc)
rtk build/bin/Release/tcpquic_linux_relay_worker_io_test
```

Expected: the late TCP error regression sees `LateTcpErrorAfterStreamShutdown > 0`.

- [ ] **Step 5: Commit metric collection**

```bash
rtk git add src/tunnel/linux_relay_worker.h src/tunnel/linux_relay_worker.cpp src/unittest/linux_relay_worker_io_test.cpp
rtk git commit -m "chore: count linux relay late tcp errors after stream shutdown"
```

## Task 4: Expose the Metric Through Relay Metrics JSON

**Files:**
- Modify: `src/runtime/relay_metrics.h`
- Modify: `src/runtime/relay_metrics.cpp`
- Modify: `src/unittest/router_runtime_test.cpp`

- [ ] **Step 1: Add metrics snapshot field**

In `TqRelayMetricsSnapshot` in `src/runtime/relay_metrics.h`, add after `LinuxRelayFakeFinReceiveCount`:

```cpp
    uint64_t LinuxRelayLateTcpErrorAfterStreamShutdown{0};
```

- [ ] **Step 2: Copy Linux snapshot into relay metrics**

In `TqSnapshotRelayMetrics()` in `src/runtime/relay_metrics.cpp`, add after:

```cpp
    metrics.LinuxRelayFakeFinReceiveCount = snapshot.FakeFinReceiveCount;
```

this line:

```cpp
    metrics.LinuxRelayLateTcpErrorAfterStreamShutdown =
        snapshot.LateTcpErrorAfterStreamShutdown;
```

- [ ] **Step 3: Emit JSON field**

In `TqAppendRelayMetricsJson()` in `src/runtime/relay_metrics.cpp`, add after `linux_relay_fake_fin_receive_count`:

```cpp
    out << ",\"linux_relay_late_tcp_error_after_stream_shutdown\":"
        << metrics.LinuxRelayLateTcpErrorAfterStreamShutdown;
```

- [ ] **Step 4: Add router runtime metrics assertion**

In `src/unittest/router_runtime_test.cpp`, near existing relay metrics JSON field assertions, add:

```cpp
        if (body.find("linux_relay_late_tcp_error_after_stream_shutdown") ==
            std::string::npos) {
            return 341;
        }
```

Use an unused return code in the surrounding block if `341` already exists in that scope.

- [ ] **Step 5: Run focused metrics tests**

Run:

```bash
rtk cmake --build build --target tcpquic_router_runtime_test tcpquic_linux_relay_worker_io_test -j$(nproc)
rtk build/bin/Release/tcpquic_router_runtime_test
rtk build/bin/Release/tcpquic_linux_relay_worker_io_test
```

Expected: metrics JSON contains `linux_relay_late_tcp_error_after_stream_shutdown`, and Linux relay lifecycle tests still pass.

- [ ] **Step 6: Commit metrics exposure**

```bash
rtk git add src/runtime/relay_metrics.h src/runtime/relay_metrics.cpp src/unittest/router_runtime_test.cpp
rtk git commit -m "chore: expose linux relay late tcp error metric"
```

## Task 5: Full Verification Against the Crash Scenario

**Files:**
- Verify only; no source changes expected.

- [ ] **Step 1: Run formatting and diff checks**

Run:

```bash
rtk git diff --check
```

Expected: no whitespace errors.

- [ ] **Step 2: Build focused targets**

Run:

```bash
rtk cmake --build build --target tcpquic_linux_relay_worker_io_test tcpquic_router_runtime_test -j$(nproc)
```

Expected: build succeeds.

- [ ] **Step 3: Run focused tests**

Run:

```bash
rtk build/bin/Release/tcpquic_linux_relay_worker_io_test
rtk build/bin/Release/tcpquic_router_runtime_test
```

Expected: both tests exit 0.

- [ ] **Step 4: Confirm the historical crash path is covered**

Verify the test source contains all of these strings:

```bash
rtk rg -n "LateTcpErrorAfterStreamShutdown|late TCP error after shutdown complete|tcp_write_shutdown_complete must not abort stream|active stream hard TCP error|HasAbortableStream|TcpWriteClosed" src/unittest/linux_relay_worker_io_test.cpp src/tunnel/linux_relay_worker.cpp
```

Expected: matches in the new regression test, `ProcessTcpEvents()`, and snapshot plumbing.

- [ ] **Step 5: Compare against crash evidence**

Use the original crash evidence as the acceptance checklist:

```text
Crash path: receive_fin -> tcp_write_shutdown_complete -> tcp_socket_error -> AbortRelayAndRelease -> stale MsQuic stream handle
Expected after fix: receive_fin -> tcp_write_shutdown_complete marks TcpWriteClosed -> shutdown_complete detach -> tcp_socket_error -> AbortRelayAndRelease skips Stream::Shutdown
```

The tests added in Task 1 must assert `ReadFakeStreamShutdownCalls()` does not increase after shutdown complete and after tcp_write_shutdown_complete.

- [ ] **Step 6: Final commit if verification required additional edits**

If Task 5 required edits, commit them:

```bash
rtk git add src/tunnel/linux_relay_worker.* src/runtime/relay_metrics.* src/unittest/linux_relay_worker_io_test.cpp src/unittest/router_runtime_test.cpp
rtk git commit -m "test: verify linux relay stream lifetime hardening"
```

If Task 5 made no edits, skip this commit.

## Notes for Implementers

- The crash-time binary appeared as `raypx2 (deleted)` in the dump. Always validate behavior with tests against the current source, not by assuming the running binary matched the saved file.
- Do not treat `MsQuicStream::Handle != nullptr` as a validity check after shutdown complete. The wrapper object or the underlying handle can already be invalid.
- `DetachRelayStreamBinding()` intentionally retains retired bindings for late callback safety. Use that path instead of deleting bindings directly.
- `AbortRelayAndRelease(..., false)` remains appropriate from shutdown-complete paths because MsQuic has already completed the stream.
- `TcpWriteClosed` is the Linux relay state corresponding to `tcp_write_shutdown_complete`; once it is true, late TCP socket errors must not trigger another stream abort even if `StreamShutdownComplete` has not been observed yet.
- Keep Linux aligned with Windows/Darwin ownership rules: after local close/binding close, late network events clean local relay state only and do not call stream APIs.

## Self-Review

- Every requirement in `docs/superpowers/specs/2026-07-09-linux-relay-stream-lifetime-design.md` maps to a task above.
- No task depends on live Crashpad/core artifacts; the regression is reproduced through deterministic unit tests.
- The plan preserves active-stream abort behavior with a guard test, so the fix is not a broad removal of TCP hard error handling.
- The plan explicitly covers `Closing`, `StreamShutdownComplete`, and `TcpWriteClosed/tcp_write_shutdown_complete` as non-abortable states.
- No incomplete sections remain.
