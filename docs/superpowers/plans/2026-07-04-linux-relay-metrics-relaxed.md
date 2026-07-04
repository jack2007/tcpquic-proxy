# Linux Relay Metrics Relaxed Atomics Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Reduce unnecessary `seq_cst` ordering in Linux relay pure metrics atomics by converting metric-only updates and snapshot reads to `std::memory_order_relaxed`, while preserving lifecycle and callback synchronization semantics.

**Architecture:** Keep all existing fields, metrics names, snapshot structures, and control/data flow unchanged. This is a narrow memory-order refactor in `TqLinuxRelayWorker`: pure counters and diagnostic last-value fields become explicit relaxed operations; synchronization atomics remain acquire/release/acq_rel.

**Tech Stack:** C++17, Linux relay worker, existing CMake unit tests, project `rtk` command wrapper.

---

## File Structure

- Modify: `src/tunnel/linux_relay_worker.cpp`
  - Add `std::memory_order_relaxed` to metric-only `fetch_add()`, `load()`, and `store()` calls.
  - Keep lifecycle, wake, binding, handle stop, producer observation, and event queue ordering unchanged.
- Modify: `docs/relay_linux.md`
  - Mark section 3.7 as implemented after code completion.

No header change is expected in this plan.

## Guardrails

Do not relax these synchronization operations:

- `Running.load/store`
- `WakeArmed.exchange/store`
- `binding->CallbackRefs.fetch_add/fetch_sub`
- `binding->Relay.load/store`
- `binding->Handle.load/store`
- `binding->Closing.load/store`
- `relay->Handle->Stop.load/store`
- `FirstEventProducerHash`, `EventProducerThreadCount`, `MultipleEventProducerThreadsObserved`
- any atomics inside `TqLinuxRelayEventQueue`

These operations are synchronization or lifecycle protocol, not pure metrics.

## Task 1: Baseline and Build Target Check

**Files:**
- Read only: `src/tunnel/linux_relay_worker.cpp`

- [ ] **Step 1: Confirm current default-order metric sites**

Run:

```bash
rtk rg -n "fetch_add\\([^,)]*\\)|\\.load\\(\\)|\\.store\\([^,)]*\\)" src/tunnel/linux_relay_worker.cpp
```

Expected: results include metric-only calls such as `Errors.fetch_add(1)`, `TcpReadBytes.fetch_add(...)`, `snapshot.TcpReadBytes = TcpReadBytes.load()`, and synchronization calls such as `Running.load()` that must not be mechanically changed.

- [ ] **Step 2: Confirm relevant tests build before editing**

Run:

```bash
rtk cmake --build build --target tcpquic_linux_relay_worker_io_test tcpquic_router_runtime_test -j2
```

If the build is already broken before this plan starts, stop and report the pre-existing failure.

## Task 2: Relax Metric Helper Functions

**Files:**
- Modify: `src/tunnel/linux_relay_worker.cpp`

- [ ] **Step 1: Update `RecordError()` counters**

In `TqLinuxRelayWorker::RecordError()`, convert each pure metric increment to relaxed. The result should follow this pattern:

```cpp
    Errors.fetch_add(1, std::memory_order_relaxed);
```

Apply the same relaxed order to every category-specific counter inside the `switch`, for example:

```cpp
        EventQueueFullErrors.fetch_add(1, std::memory_order_relaxed);
        TcpReadBufferAcquireFailures.fetch_add(1, std::memory_order_relaxed);
        QuicSendFailures.fetch_add(1, std::memory_order_relaxed);
```

- [ ] **Step 2: Update buffer acquire failure accounting**

In `RecordBufferAcquireFailure()`, convert `pendingBudget.fetch_add(1)` and `alloc.fetch_add(1)` to relaxed:

```cpp
            pendingBudget.fetch_add(1, std::memory_order_relaxed);
            alloc.fetch_add(1, std::memory_order_relaxed);
```

- [ ] **Step 3: Update TCP write attempt/returned histograms**

In `RecordTcpWriteAttempt()` and `RecordTcpWriteReturned()`, convert total bytes and bucket counters to relaxed:

```cpp
    TcpWriteAttemptBytes.fetch_add(bytes, std::memory_order_relaxed);
    TcpWriteAttemptBytesLe64K.fetch_add(1, std::memory_order_relaxed);
```

Use the same order for all `TcpWriteAttemptBytes*` and `TcpWriteReturnedBytes*` buckets.

- [ ] **Step 4: Update QUIC receive view histograms**

In `RecordQuicReceiveView()`, convert count, bytes, size buckets, and slice buckets to relaxed:

```cpp
    QuicReceiveViewCount.fetch_add(1, std::memory_order_relaxed);
    QuicReceiveViewBytes.fetch_add(bytes, std::memory_order_relaxed);
    QuicReceiveViewSlicesGt16.fetch_add(1, std::memory_order_relaxed);
```

Keep `UpdateAtomicMax()` unchanged; it already uses relaxed load/CAS.

## Task 3: Relax Data Path Metric Updates

**Files:**
- Modify: `src/tunnel/linux_relay_worker.cpp`

- [ ] **Step 1: Update worker event and wake counters**

Convert these pure counters to relaxed:

```cpp
WakeupWrites.fetch_add(1, std::memory_order_relaxed);
EventsProcessed.fetch_add(processed, std::memory_order_relaxed);
```

Do not change `WakeArmed.exchange(true, std::memory_order_acq_rel)` or `WakeArmed.store(false, std::memory_order_release)`.

- [ ] **Step 2: Update TCP read metrics**

In TCP read paths, convert byte, batch, and max-load metric operations:

```cpp
TcpReadBytes.fetch_add(static_cast<uint64_t>(received), std::memory_order_relaxed);
TcpReadBatches.fetch_add(1, std::memory_order_relaxed);
uint64_t previous = MaxTcpReadIovUsed.load(std::memory_order_relaxed);
LastTcpReadErrno.store(savedErrno, std::memory_order_relaxed);
```

For `WaitForTcpReadBytesForTest()`, use relaxed loads because it polls a metric counter for tests only:

```cpp
if (TcpReadBytes.load(std::memory_order_relaxed) >= bytes) {
```

- [ ] **Step 3: Update TCP to QUIC send/compression metrics**

Convert send/compression counters and diagnostic last-status operations to relaxed:

```cpp
CompressedTcpBytes.fetch_add(relay->CompressionOutput.size(), std::memory_order_relaxed);
QuicSendOperations.fetch_add(1, std::memory_order_relaxed);
LastQuicSendStatus.store(static_cast<int64_t>(status), std::memory_order_relaxed);
QuicSendBackpressureEvents.fetch_add(1, std::memory_order_relaxed);
```

Use relaxed order for `TcpToQuicCompress*Failures`, `QuicSend*Failures`, `QuicSendFatalErrors`, `QuicSendOperationAllocFailures`, and log-only `LastQuicSendStatus.load()` reads.

- [ ] **Step 4: Update callback/fallback metrics without touching binding synchronization**

Convert metrics-only callback counters:

```cpp
StreamLookupScanCount.fetch_add(1, std::memory_order_relaxed);
FakeFinReceiveCount.fetch_add(1, std::memory_order_relaxed);
QuicReceiveViewBackpressureQueued.fetch_add(1, std::memory_order_relaxed);
```

Keep all `binding->...` acquire/release operations unchanged.

- [ ] **Step 5: Update QUIC receive completion, pause/resume, and TCP write metrics**

Convert these groups to relaxed:

```cpp
DeferredReceiveCompleteBytes.fetch_add(bytes, std::memory_order_relaxed);
DeferredReceiveCompletes.fetch_add(1, std::memory_order_relaxed);
DeferredReceiveCompletionFlushes.fetch_add(1, std::memory_order_relaxed);
QuicReceivePausedCount.fetch_add(1, std::memory_order_relaxed);
QuicReceiveResumedCount.fetch_add(1, std::memory_order_relaxed);
TcpWriteBytes.fetch_add(static_cast<uint64_t>(sent), std::memory_order_relaxed);
TcpWriteBatches.fetch_add(1, std::memory_order_relaxed);
TcpWriteSendmsgCalls.fetch_add(1, std::memory_order_relaxed);
TcpWritePartialCount.fetch_add(1, std::memory_order_relaxed);
TcpWriteEagainCount.fetch_add(1, std::memory_order_relaxed);
TcpWriteBurstStops.fetch_add(1, std::memory_order_relaxed);
LastTcpWriteErrno.store(savedErrno, std::memory_order_relaxed);
```

Apply the same pattern in both uncompressed and decompressed QUIC receive write paths.

- [ ] **Step 6: Update zstd decompression metrics**

Convert decompression counters to relaxed:

```cpp
ZstdDecompressCalls.fetch_add(1, std::memory_order_relaxed);
ZstdDecompressFailures.fetch_add(1, std::memory_order_relaxed);
ZstdDecompressNeedInput.fetch_add(1, std::memory_order_relaxed);
ZstdDecompressNeedOutput.fetch_add(1, std::memory_order_relaxed);
ZstdDecompressInputBytes.fetch_add(result.InputConsumed, std::memory_order_relaxed);
ZstdDecompressOutputBytes.fetch_add(result.OutputProduced, std::memory_order_relaxed);
DecompressedTcpBytes.fetch_add(result.OutputProduced, std::memory_order_relaxed);
```

## Task 4: Relax Snapshot Metric Loads

**Files:**
- Modify: `src/tunnel/linux_relay_worker.cpp`

- [ ] **Step 1: Update `SnapshotLocal()` metric loads**

In `TqLinuxRelayWorker::SnapshotLocal()`, convert every pure metrics load to:

```cpp
snapshot.TcpReadBytes = TcpReadBytes.load(std::memory_order_relaxed);
```

Apply this to worker counters, TCP/QUIC counters, histograms, compression counters, error counters, last errno/status fields, and wait metrics.

- [ ] **Step 2: Keep producer/thread synchronization loads unchanged**

Do not change this line:

```cpp
snapshot.MultipleEventProducerThreadsObserved =
    MultipleEventProducerThreadsObserved.load(std::memory_order_acquire);
```

Leave `EventProducerThreadCount` acquire semantics unchanged unless a separate review proves it is purely diagnostic.

- [ ] **Step 3: Leave active relay per-relay fields as-is unless already relaxed**

Existing relay-owned fields such as `relay->PendingBufferBytes.load(std::memory_order_relaxed)` and `relay->AllocateCount.load(std::memory_order_relaxed)` are already explicit. Do not alter `relay->Handle->Stop.load(std::memory_order_acquire)`.

## Task 5: Targeted Verification

**Files:**
- Read only after edits.

- [ ] **Step 1: Build tests**

Run:

```bash
rtk cmake --build build --target tcpquic_linux_relay_worker_io_test tcpquic_router_runtime_test -j2
```

- [ ] **Step 2: Run tests**

Run:

```bash
rtk ./build/bin/Release/tcpquic_linux_relay_worker_io_test
rtk ./build/bin/Release/tcpquic_router_runtime_test
```

- [ ] **Step 3: Check synchronization guardrails**

Run:

```bash
rtk rg -n "Running\\.|WakeArmed\\.|CallbackRefs|binding->(Relay|Handle|Closing)|Handle->Stop|MultipleEventProducerThreadsObserved|EventProducerThreadCount|FirstEventProducerHash" src/tunnel/linux_relay_worker.cpp
```

Review the output and confirm lifecycle/binding/wake/producer operations still use acquire/release/acq_rel where they did before.

- [ ] **Step 4: Check formatting**

Run:

```bash
rtk git diff --check
```

## Task 6: Documentation and Commit

**Files:**
- Modify: `docs/relay_linux.md`

- [ ] **Step 1: Update section 3.7 after implementation**

Change `3.7 metrics 原子内存序偏保守` from planned status to completed status. Mention:

- pure metrics `fetch_add/load/store` now use `memory_order_relaxed`;
- synchronization atomics are intentionally unchanged;
- no metrics names or admin API output changed.

- [ ] **Step 2: Commit only related files**

Stage only the implementation and documentation files:

```bash
rtk git add src/tunnel/linux_relay_worker.cpp docs/relay_linux.md
rtk git commit -m "Relax Linux relay metrics atomics"
```

Do not stage unrelated dirty files in the working tree.

## Success Criteria

- Linux relay pure metrics no longer rely on default `seq_cst` ordering.
- Worker lifecycle, wake, callback binding, handle stop, producer observation, and event queue synchronization are unchanged.
- Existing Linux relay and router runtime tests pass.
- `docs/relay_linux.md` accurately reflects the completed 3.7 work.
