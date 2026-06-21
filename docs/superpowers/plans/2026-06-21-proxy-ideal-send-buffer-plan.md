# tcpquic-proxy Ideal Send Buffer Backpressure Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Validate tcpquic-proxy 1x1 lossy-WAN throughput when relay TCP-read backpressure follows `QUIC_STREAM_EVENT_IDEAL_SEND_BUFFER_SIZE` and QUIC windows/core ideal cap are enlarged.

**Architecture:** Keep Linux relay ownership intact by enqueueing stream ideal-send events from the MsQuic callback into the relay worker event queue. Replace BDP-derived global read-ahead as the send-backlog pause/resume threshold with per-relay ideal-send bytes, while fixing srw/fcw and MsQuic corecap for this validation branch.

**Tech Stack:** C++17 tcpquic-proxy, MsQuic C core, CMake, Linux epoll relay, existing project unit-test executables and DGX shell scripts.

---

## File Map

- Modify `third_party/msquic/src/core/quicdef.h`
  - Set `QUIC_MAX_IDEAL_SEND_BUFFER_SIZE` to `1GiB`.
- Modify `src/config/tuning.h`
  - Add constants or fields for ideal-send backpressure mode if needed.
- Modify `src/config/tuning.cpp`
  - Fix validation profile windows/read-ahead defaults.
  - Stop reducing relay send depth from old runtime observation logic in this validation mode.
  - Make tuning logs show fixed `srw/fcw` and ideal-send mode.
- Modify `src/protocol/quic_session.cpp`
  - Force `StreamRecvWindowDefault` and `ConnFlowControlWindow` to `2GiB`.
  - Stop using `NETWORK_STATISTICS` as the relay read-ahead source for this validation.
- Modify `src/tunnel/linux_relay_event_queue.h`
  - Add a worker event type for stream ideal-send bytes.
- Modify `src/tunnel/linux_relay_worker.h`
  - Add per-relay `IdealSendBufferBytes`.
  - Add handler declarations if needed.
- Modify `src/tunnel/linux_relay_worker.cpp`
  - Capture `QUIC_STREAM_EVENT_IDEAL_SEND_BUFFER_SIZE`.
  - Update per-relay ideal send bytes on the worker thread.
  - Use ideal-send bytes for pause/resume thresholds.
- Modify `src/runtime/trace.h`
  - Add trace helper declaration for ideal-send updates.
- Modify `src/runtime/trace.cpp`
  - Add trace output for ideal-send updates and threshold source.
- Modify tests under `src/unittest/`
  - Add unit coverage for tuning constants and Linux relay ideal-send threshold behavior.

## Task 1: Fix MsQuic Core Ideal Send Cap

**Files:**
- Modify: `third_party/msquic/src/core/quicdef.h`
- Test: build `core` through `tcpquic-proxy`

- [ ] **Step 1: Edit the cap**

Change:

```c
#define QUIC_MAX_IDEAL_SEND_BUFFER_SIZE         0x8000000 // 134217728
```

to:

```c
#define QUIC_MAX_IDEAL_SEND_BUFFER_SIZE         0x40000000 // 1073741824
```

- [ ] **Step 2: Verify the value by search**

Run:

```bash
rtk rg -n "QUIC_MAX_IDEAL_SEND_BUFFER_SIZE" third_party/msquic/src/core/quicdef.h third_party/msquic/src/core/send_buffer.c
```

Expected:

```text
third_party/msquic/src/core/quicdef.h:...:#define QUIC_MAX_IDEAL_SEND_BUFFER_SIZE         0x40000000 // 1073741824
```

- [ ] **Step 3: Build**

Run:

```bash
rtk cmake --build build-plan --config Release --target tcpquic-proxy --parallel 8
```

Expected: target `tcpquic-proxy` builds successfully.

## Task 2: Fix srw/fcw to 2GiB for Validation

**Files:**
- Modify: `src/config/tuning.h`
- Modify: `src/config/tuning.cpp`
- Modify: `src/protocol/quic_session.cpp`
- Test: `src/unittest/tuning_test.cpp`

- [ ] **Step 1: Add constants**

Add constants near the tuning defaults:

```cpp
constexpr uint32_t TqValidationFlowWindowBytes = 0x80000000u;
constexpr uint64_t TqValidationInitialIdealSendFallbackBytes = 64ull * 1024 * 1024;
```

If the constants must be shared across translation units, declare them in `src/config/tuning.h`:

```cpp
constexpr uint32_t TqValidationFlowWindowBytes = 0x80000000u;
constexpr uint64_t TqValidationInitialIdealSendFallbackBytes = 64ull * 1024 * 1024;
```

- [ ] **Step 2: Force validation windows**

In `TqMakeMsQuicSettings()` set:

```cpp
settings.SetStreamRecvWindowDefault(TqValidationFlowWindowBytes);
settings.SetConnFlowControlWindow(TqValidationFlowWindowBytes);
```

The branch is for validation, so do not keep old BDP-derived window selection active for this path.

- [ ] **Step 3: Update tuning output**

Make `TqPrintTuning()` show:

```text
srw=2147483648 fcw=2147483648 ... ideal_backpressure=stream_event
```

Keep existing fields so old scripts remain parseable.

- [ ] **Step 4: Add/adjust unit test**

In `src/unittest/tuning_test.cpp`, add or update a test that checks:

```cpp
TqConfig cfg{};
cfg.TuningMode = TqTuningMode::Custom;
TqFinalizeTuning(cfg);
TQ_TEST_REQUIRE(cfg.Tuning.StreamRecvWindow == 0x80000000u);
TQ_TEST_REQUIRE(cfg.Tuning.ConnFlowControlWindow == 0x80000000u);
```

If the test currently expects 512MiB/500MB, update it only for this validation branch.

- [ ] **Step 5: Run the test executable**

Build the test target visible in this repo. If unsure, list targets first:

```bash
rtk cmake --build build-plan --target help | rg "tuning|test"
```

Then run the specific tuning test binary from `build-plan/bin/Release/` or `build-plan/src/` as produced by CMake.

Expected: tuning tests pass.

## Task 3: Add Linux Relay Ideal-Send Event

**Files:**
- Modify: `src/tunnel/linux_relay_event_queue.h`
- Modify: `src/tunnel/linux_relay_worker.h`
- Modify: `src/tunnel/linux_relay_worker.cpp`
- Test: `src/unittest/linux_relay_worker_io_test.cpp`

- [ ] **Step 1: Add event type**

Add a Linux relay event type:

```cpp
QuicIdealSendBuffer,
```

The event value carries the `ByteCount` from:

```cpp
event->IDEAL_SEND_BUFFER_SIZE.ByteCount
```

- [ ] **Step 2: Add relay state**

Add to `RelayState`:

```cpp
uint64_t IdealSendBufferBytes{TqValidationInitialIdealSendFallbackBytes};
bool HasIdealSendBufferEvent{false};
```

- [ ] **Step 3: Handle stream callback**

In `OnStreamEventWithBinding()`, before receive handling, add:

```cpp
if (event->Type == QUIC_STREAM_EVENT_IDEAL_SEND_BUFFER_SIZE) {
    TqLinuxRelayEvent queued{};
    queued.Type = TqLinuxRelayEventType::QuicIdealSendBuffer;
    queued.RelayId = relayId;
    queued.Value = static_cast<uintptr_t>(event->IDEAL_SEND_BUFFER_SIZE.ByteCount);
    if (!Enqueue(std::move(queued))) {
        return QUIC_STATUS_OUT_OF_MEMORY;
    }
    return QUIC_STATUS_SUCCESS;
}
```

Use the actual fields available on `TqLinuxRelayEvent`. If it does not currently have `RelayId`, add one rather than overloading unrelated fields.

- [ ] **Step 4: Handle worker event**

In the worker event dispatch switch, add:

```cpp
case TqLinuxRelayEventType::QuicIdealSendBuffer:
    HandleQuicIdealSendBuffer(event.RelayId, static_cast<uint64_t>(event.Value));
    break;
```

Implement:

```cpp
void TqLinuxRelayWorker::HandleQuicIdealSendBuffer(uint64_t relayId, uint64_t byteCount) {
    auto relay = FindRelayById(relayId);
    if (relay == nullptr || byteCount == 0) {
        return;
    }
    relay->IdealSendBufferBytes = byteCount;
    relay->HasIdealSendBufferEvent = true;
    TraceLinuxRelayIdealSendBuffer(Config.WorkerIndex, relayId, byteCount, relay->OutstandingQuicSendBytes);
    if (!relay->TcpReadClosed &&
        ShouldResumeTcpReadForQuicBacklog(relay.get())) {
        SetTcpReadBackpressure(relay.get(), false, "ideal_send_buffer");
        ArmTcpReadable(relay.get(), true);
    }
}
```

Adjust `.get()` based on whether `FindRelayById()` returns a raw pointer or smart pointer.

- [ ] **Step 5: Add callback unit test**

In `src/unittest/linux_relay_worker_io_test.cpp`, add a test using `DispatchStreamEventForTest()` that creates:

```cpp
QUIC_STREAM_EVENT event{};
event.Type = QUIC_STREAM_EVENT_IDEAL_SEND_BUFFER_SIZE;
event.IDEAL_SEND_BUFFER_SIZE.ByteCount = 512ull * 1024 * 1024;
```

Expected behavior:

- dispatch returns `QUIC_STATUS_SUCCESS`
- worker later reports relay `IdealSendBufferBytes == 512MiB`
- no TCP read pause is triggered if outstanding is below the new threshold

## Task 4: Replace Pause/Resume Threshold Source

**Files:**
- Modify: `src/tunnel/linux_relay_worker.cpp`
- Test: `src/unittest/linux_relay_worker_io_test.cpp`

- [ ] **Step 1: Add helper**

Add:

```cpp
uint64_t TqLinuxRelayWorker::CurrentRelayIdealSendBytes(const RelayState* relay) const {
    if (relay == nullptr) {
        return TqValidationInitialIdealSendFallbackBytes;
    }
    return relay->IdealSendBufferBytes != 0
        ? relay->IdealSendBufferBytes
        : TqValidationInitialIdealSendFallbackBytes;
}
```

- [ ] **Step 2: Update pause logic**

Replace:

```cpp
const uint64_t pauseThreshold = CurrentMaxBufferedQuicSendBytes();
```

with:

```cpp
const uint64_t pauseThreshold = CurrentRelayIdealSendBytes(relay);
```

in `ShouldPauseTcpReadForQuicBacklog()`.

- [ ] **Step 3: Update resume logic**

Replace the resume threshold source with a strict comparison against the same ideal-send value:

```cpp
return relay->OutstandingQuicSendBytes < CurrentRelayIdealSendBytes(relay);
```

- [ ] **Step 4: Update trace threshold**

In `SetTcpReadBackpressure()`, trace:

```cpp
pause_threshold = CurrentRelayIdealSendBytes(relay)
resume_threshold = pause_threshold
read_ahead = pause_threshold
```

This preserves the current trace schema while making the source explicit in the reason string. `resume_threshold` is equal to `pause_threshold`, but the resume condition is strict `<`, while the pause condition is `>=`.

- [ ] **Step 5: Add threshold unit test**

Create a test with relay state:

```cpp
relay->IdealSendBufferBytes = 512ull * 1024 * 1024;
relay->OutstandingQuicSendBytes = 511ull * 1024 * 1024;
TQ_TEST_REQUIRE(!worker.ShouldPauseTcpReadForQuicBacklogForTest(relay));
TQ_TEST_REQUIRE(worker.ShouldResumeTcpReadForQuicBacklogForTest(relay));
relay->OutstandingQuicSendBytes = 512ull * 1024 * 1024;
TQ_TEST_REQUIRE(worker.ShouldPauseTcpReadForQuicBacklogForTest(relay));
TQ_TEST_REQUIRE(!worker.ShouldResumeTcpReadForQuicBacklogForTest(relay));
```

Expose test-only wrappers only if the test cannot use existing public probes.

## Task 5: Stop BDP-Derived Read-Ahead From Driving Backpressure

**Files:**
- Modify: `src/config/tuning.cpp`
- Modify: `src/protocol/quic_session.cpp`
- Test: `src/unittest/linux_relay_worker_io_test.cpp`

- [ ] **Step 1: Leave network stats trace intact**

Keep `TqTraceQuicNetworkStats()` calls so test artifacts still include:

```text
posted
ideal
bytes_in_flight
bbr_bw_mbps
srtt
cwnd
```

- [ ] **Step 2: Bypass read-ahead update**

Remove or guard these calls for this validation branch:

```cpp
TqRelayUpdateQuicReadAheadFromNetworkStats(
    event->NETWORK_STATISTICS.Bandwidth,
    event->NETWORK_STATISTICS.SmoothedRTT);
```

Do this in both client and server connection callbacks.

- [ ] **Step 3: Keep compatibility**

Do not delete `TqRelayUpdateQuicReadAheadFromNetworkStats()` yet. Leave it compiled for old tests and future comparison branches.

- [ ] **Step 4: Verify no BDP update controls threshold**

Run:

```bash
rtk rg -n "TqRelayUpdateQuicReadAheadFromNetworkStats\\(" src
```

Expected: no active call sites in `quic_session.cpp`, or guarded call sites that are disabled in this validation branch.

## Task 6: Add Trace Output for Ideal-Send Updates

**Files:**
- Modify: `src/runtime/trace.h`
- Modify: `src/runtime/trace.cpp`
- Modify: `src/tunnel/linux_relay_worker.cpp`
- Test: manual trace grep from local/loopback run if available

- [ ] **Step 1: Add trace helper**

Declare:

```cpp
void TqTraceLinuxRelayIdealSendBuffer(
    uint32_t worker,
    uint64_t relayId,
    uint64_t idealSendBytes,
    uint64_t outstandingQuicSendBytes);
```

- [ ] **Step 2: Implement trace helper**

Emit:

```text
event=relay_ideal_send_buffer worker=<n> relay=<id> ideal_send_bytes=<bytes> outstanding_quic_send_bytes=<bytes>
```

- [ ] **Step 3: Call helper**

Call it from `HandleQuicIdealSendBuffer()`.

- [ ] **Step 4: Verify trace format**

Run a local smoke test or unit-level trace test and grep:

```bash
rtk rg -n "relay_ideal_send_buffer|relay_backpressure" build-plan src docs
```

Expected: helper exists and trace line is emitted in a run artifact when stream ideal-send events occur.

## Task 7: Build and Local Verification

**Files:**
- Build output: `build-plan/bin/Release/tcpquic-proxy`

- [ ] **Step 1: Configure**

Run:

```bash
rtk cmake -S . -B build-plan -DCMAKE_BUILD_TYPE=Release
```

Expected: configure succeeds.

- [ ] **Step 2: Build**

Run:

```bash
rtk cmake --build build-plan --config Release --target tcpquic-proxy --parallel 8
```

Expected: `Built target tcpquic-proxy`.

- [ ] **Step 3: Run available tests**

Run:

```bash
rtk ctest --test-dir build-plan --output-on-failure
```

Expected: tests pass, or if CTest has no tests, run the relevant unit-test binaries directly from the build tree.

- [ ] **Step 4: Check tuning output**

Run:

```bash
rtk build-plan/bin/Release/tcpquic-proxy --help
```

Then start a minimal server/client command used by existing scripts and confirm startup log includes:

```text
srw=2147483648
fcw=2147483648
ideal_backpressure=stream_event
```

## Task 8: DGX Validation

**Files:**
- Create artifacts under `docs/dgx-proxy-ideal-send-buffer-20260621-*`

- [ ] **Step 1: Sync binaries**

Use the existing DGX scripts or rsync pattern to copy:

```text
build-plan/bin/Release/tcpquic-proxy
build-plan/msquic/bin/Release/libmsquic.so*
```

If this build is static against MsQuic, sync only the proxy binary and required cert/assets.

- [ ] **Step 2: Run 100ms + 5% loss**

Use:

```text
netem delay 100ms loss 5% limit 5000000
```

Record:

```text
speed_download
relay_ideal_send_buffer
relay_backpressure
posted/ideal/bytes_in_flight
loss_rate
perf top
```

- [ ] **Step 3: Run 200ms + 5% loss**

Use:

```text
netem delay 200ms loss 5% limit 5000000
```

Record the same fields as the 100ms case.

- [ ] **Step 4: Compare with old proxy**

Compare against:

```text
100ms + 5%: 1.415-1.776 Gbps
200ms + 5%: 0.601 Gbps
```

Also compare against secnetperf low baseline:

```text
100ms + 5% postbuf=128MiB: 3.580 Gbps
200ms + 5% postbuf=128MiB: 1.790 Gbps
```

- [ ] **Step 5: Decide next step**

If `posted` remains far below `ideal` after this change, continue investigating relay-side data supply and downstream TCP write. If `posted` tracks `ideal` but throughput remains low, shift analysis to MsQuic recovery/pacing and single-stream scheduling.

## Current Baseline in This Worktree

Already completed before writing this plan:

```bash
rtk git submodule update --init --recursive
rtk cmake -S . -B build-plan -DCMAKE_BUILD_TYPE=Release
rtk cmake --build build-plan --config Release --target tcpquic-proxy --parallel 8
rtk ctest --test-dir build-plan --output-on-failure
```

Observed:

- CMake configure succeeded.
- `tcpquic-proxy` target built successfully.
- CTest reported: `No tests were found!!!`
