# Linux Relay Callback Hardening Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Make Linux relay fake FIN receive a single-relay fatal reset instead of a process abort, and expose stream lookup fallback scans in admin metrics.

**Architecture:** Keep Linux relay callback ownership unchanged. The fake FIN receive branch stays inside `TqLinuxRelayWorker::OnStreamEventWithBinding()`, records a dedicated counter, calls the existing `FailRelayFatal()` cleanup path, and returns `QUIC_STATUS_SUCCESS` because no receive buffer is retained. Stream lookup scans already have a worker snapshot counter; this plan carries it through runtime aggregation and admin metrics JSON.

**Tech Stack:** C++17, MsQuic callback types, Linux epoll relay worker, existing CMake unit tests, project `rtk` command wrapper.

---

## File Structure

- Modify: `src/tunnel/linux_relay_worker.h`
  - Add `FakeFinReceiveCount` to `TqLinuxRelayWorkerSnapshot`.
  - Add `std::atomic<uint64_t> FakeFinReceiveCount`.
- Modify: `src/tunnel/linux_relay_worker.cpp`
  - Replace fake FIN `assert(false)` / `std::abort()` with single relay fatal reset.
  - Populate and aggregate `FakeFinReceiveCount`.
  - Aggregate existing `StreamLookupScanCount` in runtime snapshot.
- Modify: `src/runtime/relay_metrics.h`
  - Add `LinuxRelayFakeFinReceiveCount`.
  - Add `LinuxRelayStreamLookupScanCount`.
- Modify: `src/runtime/relay_metrics.cpp`
  - Copy Linux worker/runtime snapshot fields into relay metrics.
  - Emit `linux_relay_fake_fin_receive_count`.
  - Emit `linux_relay_stream_lookup_scan_count`.
- Modify: `src/unittest/linux_relay_worker_io_test.cpp`
  - Add fake FIN receive regression test.
- Modify: `src/unittest/router_runtime_test.cpp`
  - Assert new admin metrics JSON fields exist.
- Modify: `docs/admin-api/interface.md`
  - Document the two new Linux relay metrics.
- Modify: `docs/relay_linux.md`
  - Mark sections 3.6 and 3.9 as planned under this Superpowers plan.

## Task 1: Fake FIN Receive Regression Test

**Files:**
- Modify: `src/unittest/linux_relay_worker_io_test.cpp`

- [ ] **Step 1: Add a failing fake FIN receive test block**

Insert this block near the other `QUIC_STREAM_EVENT_RECEIVE` callback tests in `main()` after an existing basic deferred receive case:

```cpp
    {
        TqLinuxRelayWorkerConfig config{};
        config.EventBudget = 128;
        config.ReadChunkSize = 4096;
        config.ReadBatchBytes = 64 * 1024;
        config.MaxIov = 8;
        config.MaxPendingBufferBytes = 256 * 1024;

        TqLinuxRelayWorker worker(config);
        if (!worker.Start()) {
            return 372;
        }

        int fds[2]{-1, -1};
        if (::socketpair(AF_UNIX, SOCK_STREAM, 0, fds) != 0) {
            worker.Stop();
            return 373;
        }

        QUIC_API_TABLE fakeApi{};
        InstallFakeMsQuicForSend(fakeApi);

        alignas(MsQuicStream) uint8_t fakeStreamStorage[sizeof(MsQuicStream)]{};
        std::memset(fakeStreamStorage, 0, sizeof(fakeStreamStorage));
        MsQuicStream* fakeStream = reinterpret_cast<MsQuicStream*>(fakeStreamStorage);
        fakeStream->Handle = reinterpret_cast<HQUIC>(static_cast<uintptr_t>(1));

        TqLinuxRelayRegistration registration{};
        registration.TcpFd = fds[0];
        registration.Stream = fakeStream;
        registration.EnableQuicSends = false;
        if (!worker.RegisterRelayForTest(registration)) {
            worker.Stop();
            ::close(fds[1]);
            return 374;
        }

        QUIC_STREAM_EVENT fakeFin{};
        fakeFin.Type = QUIC_STREAM_EVENT_RECEIVE;
        fakeFin.RECEIVE.AbsoluteOffset = UINT64_MAX;
        fakeFin.RECEIVE.TotalBufferLength = 0;
        fakeFin.RECEIVE.BufferCount = 0;
        fakeFin.RECEIVE.Buffers = nullptr;
        fakeFin.RECEIVE.Flags = QUIC_RECEIVE_FLAG_FIN;

        const QUIC_STATUS status = worker.DispatchStreamEventForTest(fakeStream, &fakeFin);
        if (status != QUIC_STATUS_SUCCESS) {
            std::fprintf(stderr, "expected fake FIN to return SUCCESS, got %d\n", status);
            worker.Stop();
            ::close(fds[1]);
            return 375;
        }

        const TqLinuxRelayWorkerSnapshot snapshot = worker.Snapshot();
        if (snapshot.FakeFinReceiveCount != 1) {
            std::fprintf(stderr, "expected fake FIN count 1, got %llu\n",
                static_cast<unsigned long long>(snapshot.FakeFinReceiveCount));
            worker.Stop();
            ::close(fds[1]);
            return 376;
        }
        if (snapshot.FatalRelayResets != 1) {
            std::fprintf(stderr, "expected fatal relay resets 1, got %llu\n",
                static_cast<unsigned long long>(snapshot.FatalRelayResets));
            worker.Stop();
            ::close(fds[1]);
            return 377;
        }
        if (snapshot.DeferredReceiveCompletes != 0) {
            std::fprintf(stderr, "fake FIN must not complete deferred receive buffers\n");
            worker.Stop();
            ::close(fds[1]);
            return 378;
        }
        if (snapshot.ClosingRelays == 0) {
            std::fprintf(stderr, "expected fake FIN relay to be closing\n");
            worker.Stop();
            ::close(fds[1]);
            return 379;
        }

        worker.Stop();
        ::close(fds[1]);
    }
```

- [ ] **Step 2: Run the test and confirm it fails before implementation**

Run:

```bash
rtk ./build/src/tcpquic_linux_relay_worker_io_test
```

Expected: the test process aborts or fails before returning success because current fake FIN code executes `std::abort()`.

- [ ] **Step 3: Commit the failing test only**

```bash
rtk git add src/unittest/linux_relay_worker_io_test.cpp
rtk git commit -m "test: cover linux relay fake fin handling"
```

## Task 2: Convert Fake FIN Abort to Single Relay Fatal Reset

**Files:**
- Modify: `src/tunnel/linux_relay_worker.h`
- Modify: `src/tunnel/linux_relay_worker.cpp`

- [ ] **Step 1: Add fake FIN counter fields**

In `TqLinuxRelayWorkerSnapshot` in `src/tunnel/linux_relay_worker.h`, add the field after `ReadDisabledCount`:

```cpp
    uint64_t FakeFinReceiveCount{0};
```

In `TqLinuxRelayWorker` private atomics, add the field after `ReadDisabledCount`:

```cpp
    std::atomic<uint64_t> FakeFinReceiveCount{0};
```

- [ ] **Step 2: Populate worker snapshot**

In `TqLinuxRelayWorker::SnapshotLocal()`, add this after `snapshot.ReadDisabledCount = ReadDisabledCount.load();`:

```cpp
    snapshot.FakeFinReceiveCount = FakeFinReceiveCount.load();
```

- [ ] **Step 3: Replace fake FIN abort behavior**

In `TqLinuxRelayWorker::OnStreamEventWithBinding()`, replace the current fake FIN branch:

```cpp
            TraceRelayStreamEvent(
                relay,
                "receive_fake_fin",
                0,
                0,
                event->RECEIVE.AbsoluteOffset,
                event->RECEIVE.TotalBufferLength,
                event->RECEIVE.BufferCount,
                static_cast<uint32_t>(event->RECEIVE.Flags),
                true);
            assert(false && "MsQuic delivered FIN-only receive without known final size");
            std::abort();
```

with:

```cpp
            TraceRelayStreamEvent(
                relay,
                "receive_fake_fin",
                0,
                0,
                event->RECEIVE.AbsoluteOffset,
                event->RECEIVE.TotalBufferLength,
                event->RECEIVE.BufferCount,
                static_cast<uint32_t>(event->RECEIVE.Flags),
                true);
            FakeFinReceiveCount.fetch_add(1);
            FailRelayFatal(relay, "quic_receive_fake_fin", true);
            return QUIC_STATUS_SUCCESS;
```

Do not call `QueueDeferredQuicReceive()` in this branch.

- [ ] **Step 4: Aggregate fake FIN and stream lookup counts across workers**

In `TqLinuxRelayRuntime::Snapshot()`, add these after `total.TcpWriteBurstStops += snapshot.TcpWriteBurstStops;`:

```cpp
        total.FakeFinReceiveCount += snapshot.FakeFinReceiveCount;
        total.StreamLookupScanCount += snapshot.StreamLookupScanCount;
```

`ReadDisabledCount` is already aggregated in this function; do not add a duplicate `total.ReadDisabledCount += snapshot.ReadDisabledCount;` line.

- [ ] **Step 5: Run the focused test**

```bash
rtk ./build/src/tcpquic_linux_relay_worker_io_test
```

Expected: PASS with exit code 0.

- [ ] **Step 6: Commit implementation**

```bash
rtk git add src/tunnel/linux_relay_worker.h src/tunnel/linux_relay_worker.cpp
rtk git commit -m "fix: keep linux relay fake fin isolated"
```

## Task 3: Expose Callback Hardening Metrics

**Files:**
- Modify: `src/runtime/relay_metrics.h`
- Modify: `src/runtime/relay_metrics.cpp`
- Modify: `src/unittest/router_runtime_test.cpp`

- [ ] **Step 1: Add metrics snapshot fields**

In `TqRelayMetricsSnapshot` in `src/runtime/relay_metrics.h`, add these fields after `ReadDisabledCount`:

```cpp
    uint64_t LinuxRelayFakeFinReceiveCount{0};
    uint64_t LinuxRelayStreamLookupScanCount{0};
```

- [ ] **Step 2: Copy Linux snapshot values into relay metrics**

In the Linux branch of `TqCollectRelayMetrics()` in `src/runtime/relay_metrics.cpp`, after copying `metrics.ReadDisabledCount`, add:

```cpp
    metrics.LinuxRelayFakeFinReceiveCount = snapshot.FakeFinReceiveCount;
    metrics.LinuxRelayStreamLookupScanCount = snapshot.StreamLookupScanCount;
```

- [ ] **Step 3: Emit JSON metrics**

In `TqAppendRelayMetricsJson()`, after:

```cpp
    out << ",\"linux_relay_read_disabled_count\":" << metrics.ReadDisabledCount;
```

add:

```cpp
    out << ",\"linux_relay_fake_fin_receive_count\":"
        << metrics.LinuxRelayFakeFinReceiveCount;
    out << ",\"linux_relay_stream_lookup_scan_count\":"
        << metrics.LinuxRelayStreamLookupScanCount;
```

- [ ] **Step 4: Add admin metrics field assertions**

In `src/unittest/router_runtime_test.cpp`, in the metrics JSON field assertion block, add:

```cpp
        if (bodyText.find("\"linux_relay_fake_fin_receive_count\":") == std::string::npos) return 372;
        if (bodyText.find("\"linux_relay_stream_lookup_scan_count\":") == std::string::npos) return 373;
```

- [ ] **Step 5: Run router runtime test**

```bash
rtk ./build/src/tcpquic_router_runtime_test
```

Expected: PASS with exit code 0.

- [ ] **Step 6: Commit metrics**

```bash
rtk git add src/runtime/relay_metrics.h src/runtime/relay_metrics.cpp src/unittest/router_runtime_test.cpp
rtk git commit -m "feat: expose linux relay callback diagnostics"
```

## Task 4: Documentation Updates

**Files:**
- Modify: `docs/admin-api/interface.md`
- Modify: `docs/relay_linux.md`

- [ ] **Step 1: Document new admin metrics**

In `docs/admin-api/interface.md`, add the two metrics near existing Linux relay metrics:

```markdown
- `linux_relay_fake_fin_receive_count`: Linux relay 收到 MsQuic fake FIN receive 的次数；命中时只 fatal reset 当前 relay，不应退出进程。
- `linux_relay_stream_lookup_scan_count`: Linux relay 按 stream fallback 扫描 relay 的次数；正常主路径应主要依赖 callback binding/relay id。
```

- [ ] **Step 2: Update `docs/relay_linux.md` section 3.6**

Replace section 3.6 with:

```markdown
### 3.6 收到 MsQuic fake FIN 时直接 abort 进程

现象：`OnStreamEventWithBinding()` 遇到 `TqIsMsQuicFakeFinReceive()` 时执行 `assert(false)` 和 `std::abort()`。

影响：单个 stream 的异常 receive 形态会导致整个进程退出，扩大 active tunnels 的影响面。

推进方案：本轮按 `docs/superpowers/specs/2026-07-04-linux-relay-callback-hardening-design.md` 和 `docs/superpowers/plans/2026-07-04-linux-relay-callback-hardening.md` 推进，改为记录 `receive_fake_fin` trace 和 `linux_relay_fake_fin_receive_count`，随后通过单 relay fatal reset 关闭当前 relay/stream；callback 返回 `QUIC_STATUS_SUCCESS`，因为该分支不持有 MsQuic receive buffer。
```

- [ ] **Step 3: Update `docs/relay_linux.md` section 3.9**

Replace the recommendation part of section 3.9 with:

```markdown
推进方案：本轮不新增 `RelaysByStream` / `RelaysByFd` map；先把已有 `StreamLookupScanCount` 贯通到 admin metrics，输出 `linux_relay_stream_lookup_scan_count`。如果生产中该指标持续增长，再根据调用来源决定是否增加 stream/fd map 或 slot/generation id。
```

- [ ] **Step 4: Run docs diff check**

```bash
rtk git diff --check -- docs/admin-api/interface.md docs/relay_linux.md
```

Expected: no output and exit code 0.

- [ ] **Step 5: Commit docs**

```bash
rtk git add docs/admin-api/interface.md docs/relay_linux.md
rtk git commit -m "docs: describe linux relay callback diagnostics"
```

## Task 5: Final Verification

**Files:**
- Verify only.

- [ ] **Step 1: Run focused Linux relay test**

```bash
rtk ./build/src/tcpquic_linux_relay_worker_io_test
```

Expected: PASS with exit code 0.

- [ ] **Step 2: Run router runtime test**

```bash
rtk ./build/src/tcpquic_router_runtime_test
```

Expected: PASS with exit code 0.

- [ ] **Step 3: Run diff check**

```bash
rtk git diff --check
```

Expected: no output and exit code 0.

- [ ] **Step 4: Inspect final diff**

```bash
rtk git diff --stat HEAD
```

Expected: only the files listed in this plan changed after the last commit, or no output if every task committed cleanly.

## Self-Review

- Spec coverage: covers fake FIN no-process-abort behavior, single relay fatal reset semantics, dedicated fake FIN metric, stream lookup scan metric exposure, tests, and docs.
- Placeholder scan: no placeholder markers remain.
- Type consistency: uses existing `TqLinuxRelayWorkerSnapshot`, `TqRelayMetricsSnapshot`, `FailRelayFatal()`, `DispatchStreamEventForTest()`, and existing test executable names.
- Scope check: does not implement QUIC receive hysteresis, full metrics memory-order cleanup, or new stream/fd lookup maps.
