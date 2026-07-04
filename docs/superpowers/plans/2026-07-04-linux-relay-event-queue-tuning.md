# Linux Relay Event Queue Tuning Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 为 Linux relay event queue 增加生产可配置容量，并把队列容量、CAS 重试和多 producer 观测字段暴露到 metrics/admin JSON，提升 queue full 排障闭环能力。

**Architecture:** 配置仍沿用 `TqConfig -> TqTuningConfig -> TqLinuxRelayRuntime::Start() -> TqLinuxRelayWorkerConfig` 的现有链路；队列观测由 `TqLinuxRelayEventQueue` 提供低成本 relaxed counter，worker snapshot 聚合到 runtime metrics，再由 `relay_metrics.cpp` 输出 JSON。本计划不修改 QUIC receive hysteresis，不改变 event queue 数据结构语义。

**Tech Stack:** C++17, Linux relay worker/event queue, nlohmann/json, CMake, 现有 `tcpquic_tuning_test`、`tcpquic_linux_relay_worker_queue_test`、`tcpquic_router_runtime_test`。

---

## File Structure

- Modify: `src/config/config.h`
- Modify: `src/config/config.cpp`
- Modify: `src/config/tuning.h`
- Modify: `src/config/tuning.cpp`
- Modify: `src/tunnel/linux_relay_event_queue.h`
- Modify: `src/tunnel/linux_relay_worker.h`
- Modify: `src/tunnel/linux_relay_worker.cpp`
- Modify: `src/runtime/relay_metrics.h`
- Modify: `src/runtime/relay_metrics.cpp`
- Modify: `src/unittest/tuning_test.cpp`
- Modify: `src/unittest/linux_relay_worker_queue_test.cpp`
- Modify: `src/unittest/router_runtime_test.cpp`
- Modify: `docs/admin-api/interface.md`
- Modify: `docs/relay_linux.md`

## Task 1: Add config and tuning fields for Linux event queue capacity

**Files:**
- Modify: `src/config/config.h`
- Modify: `src/config/tuning.h`
- Modify: `src/config/tuning.cpp`
- Test: `src/unittest/tuning_test.cpp`

- [x] **Step 1: Add failing CLI/tuning test**

In `src/unittest/tuning_test.cpp`, after the existing `--linux-relay-tcp-write-burst-bytes` parse case, add:

```cpp
    {
        TqConfig cfg{};
        std::string err;
        char arg0[] = "tcpquic-proxy";
        char arg1[] = "client";
        char arg2[] = "--peer";
        char arg3[] = "127.0.0.1:4433";
        char arg4[] = "--cert";
        char arg5[] = "cert.pem";
        char arg6[] = "--key";
        char arg7[] = "key.pem";
        char arg8[] = "--ca";
        char arg9[] = "ca.pem";
        char arg10[] = "--linux-relay-event-queue-capacity";
        char arg11[] = "65535";
        char* argv[] = {
            arg0, arg1, arg2, arg3, arg4, arg5, arg6, arg7,
            arg8, arg9, arg10, arg11};
        assert(TqParseArgs(12, argv, cfg, err));
        TqFinalizeConfig(cfg);
        assert(cfg.Tuning.LinuxRelayEventQueueCapacity == 65536);
    }
```

Run:

```bash
rtk cmake --build build --target tcpquic_tuning_test -j$(nproc)
```

Expected: compile fails because `LinuxRelayEventQueueCapacity` and the config override field do not exist.

- [x] **Step 2: Add config/tuning fields and normalization helper**

In `src/config/config.h`, add to `TqConfig` near other Linux relay overrides:

```cpp
uint32_t TuningOverrideLinuxRelayEventQueueCapacity{0};
```

In `src/config/tuning.h`, add constants near the validation constants:

```cpp
constexpr uint32_t TqLinuxRelayEventQueueCapacityMin = 1024;
constexpr uint32_t TqLinuxRelayEventQueueCapacityMax = 1048576;
```

Add to `TqTuningConfig` near `LinuxRelayWorkerEventBudget`:

```cpp
uint32_t LinuxRelayEventQueueCapacity{4096};
```

Declare:

```cpp
uint32_t TqNormalizeLinuxRelayEventQueueCapacity(uint32_t capacity);
```

In `src/config/tuning.cpp`, add the helper near `ClampU32()`:

```cpp
uint32_t TqNormalizeLinuxRelayEventQueueCapacity(uint32_t capacity) {
    uint32_t normalized = TqLinuxRelayEventQueueCapacityMin;
    while (normalized < capacity && normalized < TqLinuxRelayEventQueueCapacityMax) {
        normalized <<= 1;
    }
    return normalized;
}
```

In `ApplyCustomOverrides()`, add:

```cpp
    if (cfg.TuningOverrideLinuxRelayEventQueueCapacity > 0) {
        out.LinuxRelayEventQueueCapacity =
            TqNormalizeLinuxRelayEventQueueCapacity(cfg.TuningOverrideLinuxRelayEventQueueCapacity);
    }
```

- [x] **Step 3: Verify CLI/tuning test compiles enough to reach parser failure**

Run:

```bash
rtk cmake --build build --target tcpquic_tuning_test -j$(nproc)
rtk build/bin/Release/tcpquic_tuning_test
```

Expected: test fails at `TqParseArgs()` because the CLI option is still unknown.

## Task 2: Wire CLI and JSON parsing

**Files:**
- Modify: `src/config/config.cpp`
- Test: `src/unittest/tuning_test.cpp`

- [x] **Step 1: Add failing invalid-value tests**

In `src/unittest/tuning_test.cpp`, after the successful CLI case from Task 1, add:

```cpp
    {
        TqConfig cfg{};
        std::string err;
        char arg0[] = "tcpquic-proxy";
        char arg1[] = "client";
        char arg2[] = "--peer";
        char arg3[] = "127.0.0.1:4433";
        char arg4[] = "--linux-relay-event-queue-capacity";
        char arg5[] = "1023";
        char* argv[] = {arg0, arg1, arg2, arg3, arg4, arg5};
        assert(!TqParseArgs(6, argv, cfg, err));
        assert(err.find("invalid value for --linux-relay-event-queue-capacity") != std::string::npos);
    }

    {
        TqConfig cfg{};
        std::string err;
        char arg0[] = "tcpquic-proxy";
        char arg1[] = "client";
        char arg2[] = "--peer";
        char arg3[] = "127.0.0.1:4433";
        char arg4[] = "--linux-relay-event-queue-capacity";
        char arg5[] = "1048577";
        char* argv[] = {arg0, arg1, arg2, arg3, arg4, arg5};
        assert(!TqParseArgs(6, argv, cfg, err));
        assert(err.find("invalid value for --linux-relay-event-queue-capacity") != std::string::npos);
    }
```

Run:

```bash
rtk build/bin/Release/tcpquic_tuning_test
```

Expected: still fails because parser does not support the option.

- [x] **Step 2: Implement CLI parsing**

In `TqPrintUsage()` in `src/config/config.cpp`, add after `--linux-relay-tcp-write-burst-bytes`:

```cpp
        "  --linux-relay-event-queue-capacity <events>\n"
        "                              Linux relay worker event queue capacity\n"
```

In `TqParseArgs()`, near the other Linux relay options, add:

```cpp
        } else if (GetOptionValue(arg, "--linux-relay-event-queue-capacity", value)) {
            if (value == nullptr) {
                value = NextArg(i, argc, argv, "--linux-relay-event-queue-capacity", err);
                if (value == nullptr) {
                    return false;
                }
            }
            if (!ParseUint32InRange(
                    value,
                    TqLinuxRelayEventQueueCapacityMin,
                    TqLinuxRelayEventQueueCapacityMax,
                    cfg.TuningOverrideLinuxRelayEventQueueCapacity)) {
                err = "invalid value for --linux-relay-event-queue-capacity";
                return false;
            }
```

- [x] **Step 3: Add JSON parse test**

In `src/unittest/tuning_test.cpp`, add these includes near the top:

```cpp
#include <cstdio>
#include <fstream>
```

After the CLI invalid-value tests, add:

```cpp
    {
        const char* path = "/tmp/tcpquic-event-queue-capacity-test.json";
        {
            std::ofstream file(path);
            file <<
                "{"
                "\"client\":{},"
                "\"relay\":{\"linux\":{\"event_queue_capacity\":32767}}"
                "}";
        }

        TqConfig cfg{};
        std::string err;
        char arg0[] = "tcpquic-proxy";
        char arg1[] = "client";
        char arg2[] = "--peer";
        char arg3[] = "127.0.0.1:4433";
        char arg4[] = "--config";
        char arg5[] = "/tmp/tcpquic-event-queue-capacity-test.json";
        char* argv[] = {arg0, arg1, arg2, arg3, arg4, arg5};
        assert(TqParseArgs(6, argv, cfg, err));
        TqFinalizeConfig(cfg);
        assert(cfg.Tuning.LinuxRelayEventQueueCapacity == 32768);
        std::remove(path);
    }

    {
        const char* path = "/tmp/tcpquic-event-queue-capacity-invalid-test.json";
        {
            std::ofstream file(path);
            file <<
                "{"
                "\"client\":{},"
                "\"relay\":{\"linux\":{\"event_queue_capacity\":1023}}"
                "}";
        }

        TqConfig cfg{};
        std::string err;
        char arg0[] = "tcpquic-proxy";
        char arg1[] = "client";
        char arg2[] = "--peer";
        char arg3[] = "127.0.0.1:4433";
        char arg4[] = "--config";
        char arg5[] = "/tmp/tcpquic-event-queue-capacity-invalid-test.json";
        char* argv[] = {arg0, arg1, arg2, arg3, arg4, arg5};
        assert(!TqParseArgs(6, argv, cfg, err));
        assert(err.find("invalid relay.linux.event_queue_capacity") != std::string::npos);
        std::remove(path);
    }
```

Run:

```bash
rtk cmake --build build --target tcpquic_tuning_test -j$(nproc)
rtk build/bin/Release/tcpquic_tuning_test
```

Expected: JSON test fails with `unknown relay.linux key: event_queue_capacity`.

- [x] **Step 4: Implement JSON parsing**

In `ParseLinuxRelayConfig()` in `src/config/config.cpp`, add:

```cpp
            } else if (key == "event_queue_capacity") {
                if (!ReadUint32InRange(
                        item.value(),
                        TqLinuxRelayEventQueueCapacityMin,
                        TqLinuxRelayEventQueueCapacityMax,
                        cfg.TuningOverrideLinuxRelayEventQueueCapacity)) {
                    return Error("invalid relay.linux.event_queue_capacity");
                }
```

Run:

```bash
rtk cmake --build build --target tcpquic_tuning_test -j$(nproc)
rtk build/bin/Release/tcpquic_tuning_test
```

Expected: `tcpquic_tuning_test` passes.

## Task 3: Apply configured capacity to Linux relay workers

**Files:**
- Modify: `src/tunnel/linux_relay_worker.cpp`
- Modify: `src/tunnel/linux_relay_worker.h`
- Test: `src/unittest/linux_relay_worker_queue_test.cpp`

- [x] **Step 1: Add failing worker snapshot capacity test**

In `src/unittest/linux_relay_worker_queue_test.cpp`, add a new block before `return 0;`:

```cpp
    {
        TqLinuxRelayWorkerConfig config{};
        config.EventQueueCapacity = 3;
        TqLinuxRelayWorker worker(config);
        assert(worker.StartForTest());
        TqLinuxRelayWorkerSnapshot snapshot = worker.Snapshot();
        assert(snapshot.EventQueueCapacity == 4);
        worker.Stop();
    }
```

Run:

```bash
rtk cmake --build build --target tcpquic_linux_relay_worker_queue_test -j$(nproc)
```

Expected: compile fails because `EventQueueCapacity` is not in `TqLinuxRelayWorkerSnapshot`.

- [x] **Step 2: Add worker snapshot capacity field**

In `src/tunnel/linux_relay_worker.h`, add to `TqLinuxRelayWorkerSnapshot` near `PendingEvents`:

```cpp
uint64_t EventQueueCapacity{0};
```

In `TqLinuxRelayWorker::SnapshotLocal()` in `src/tunnel/linux_relay_worker.cpp`, add:

```cpp
snapshot.EventQueueCapacity = EventQueue.Capacity();
```

In `TqLinuxRelayRuntime::Snapshot()` aggregation, add:

```cpp
total.EventQueueCapacity = std::max(total.EventQueueCapacity, snapshot.EventQueueCapacity);
```

In `TqLinuxRelayRuntime::SnapshotWorkers()`, no additional code is needed because the worker snapshot already carries the field.

- [x] **Step 3: Pass tuning capacity into worker config**

In `TqLinuxRelayRuntime::Start()` in `src/tunnel/linux_relay_worker.cpp`, after `config.EventBudget = tuning.LinuxRelayWorkerEventBudget;`, add:

```cpp
config.EventQueueCapacity = tuning.LinuxRelayEventQueueCapacity;
```

Run:

```bash
rtk cmake --build build --target tcpquic_linux_relay_worker_queue_test -j$(nproc)
rtk build/bin/Release/tcpquic_linux_relay_worker_queue_test
```

Expected: test passes.

## Task 4: Add event queue CAS retry stats

**Files:**
- Modify: `src/tunnel/linux_relay_event_queue.h`
- Modify: `src/tunnel/linux_relay_worker.h`
- Modify: `src/tunnel/linux_relay_worker.cpp`
- Test: `src/unittest/linux_relay_worker_queue_test.cpp`

- [x] **Step 1: Add failing snapshot assertions**

In the existing multi-producer block in `src/unittest/linux_relay_worker_queue_test.cpp`, after:

```cpp
assert(snapshot.MultipleEventProducerThreadsObserved);
```

add:

```cpp
const uint64_t pushCasRetries = snapshot.EventQueuePushCasRetries;
assert(pushCasRetries == snapshot.EventQueuePushCasRetries);
assert(snapshot.EventQueuePopCasRetries == 0);
```

The first assertion is intentionally non-strict because CAS retry timing is scheduling dependent. The field existence is the regression guard.

Run:

```bash
rtk cmake --build build --target tcpquic_linux_relay_worker_queue_test -j$(nproc)
```

Expected: compile fails because the snapshot fields do not exist.

- [x] **Step 2: Add queue stats struct and counters**

In `src/tunnel/linux_relay_event_queue.h`, add before `class TqLinuxRelayEventQueue`:

```cpp
struct TqLinuxRelayEventQueueStats {
    uint64_t PushCasRetries{0};
    uint64_t PopCasRetries{0};
};
```

Add public method:

```cpp
TqLinuxRelayEventQueueStats Stats() const {
    TqLinuxRelayEventQueueStats stats{};
    stats.PushCasRetries = PushCasRetries.load(std::memory_order_relaxed);
    stats.PopCasRetries = PopCasRetries.load(std::memory_order_relaxed);
    return stats;
}
```

Add private counters:

```cpp
alignas(64) std::atomic<uint64_t> PushCasRetries{0};
alignas(64) std::atomic<uint64_t> PopCasRetries{0};
```

In `TryPush()`, change the failed CAS branch to count retries:

```cpp
                if (EnqueuePos.compare_exchange_weak(
                        position,
                        position + 1,
                        std::memory_order_relaxed,
                        std::memory_order_relaxed)) {
                    break;
                }
                PushCasRetries.fetch_add(1, std::memory_order_relaxed);
```

In `TryPop()`, change the failed CAS branch to:

```cpp
                if (DequeuePos.compare_exchange_weak(
                        position,
                        position + 1,
                        std::memory_order_relaxed,
                        std::memory_order_relaxed)) {
                    break;
                }
                PopCasRetries.fetch_add(1, std::memory_order_relaxed);
```

- [x] **Step 3: Add worker snapshot fields**

In `src/tunnel/linux_relay_worker.h`, add to `TqLinuxRelayWorkerSnapshot` near event queue fields:

```cpp
uint64_t EventQueuePushCasRetries{0};
uint64_t EventQueuePopCasRetries{0};
```

In `SnapshotLocal()`:

```cpp
const TqLinuxRelayEventQueueStats queueStats = EventQueue.Stats();
snapshot.EventQueuePushCasRetries = queueStats.PushCasRetries;
snapshot.EventQueuePopCasRetries = queueStats.PopCasRetries;
```

In runtime aggregation:

```cpp
total.EventQueuePushCasRetries += snapshot.EventQueuePushCasRetries;
total.EventQueuePopCasRetries += snapshot.EventQueuePopCasRetries;
```

Run:

```bash
rtk cmake --build build --target tcpquic_linux_relay_worker_queue_test -j$(nproc)
rtk build/bin/Release/tcpquic_linux_relay_worker_queue_test
```

Expected: test passes.

## Task 5: Expose new fields through relay metrics JSON

**Files:**
- Modify: `src/runtime/relay_metrics.h`
- Modify: `src/runtime/relay_metrics.cpp`
- Test: `src/unittest/router_runtime_test.cpp`

- [x] **Step 1: Add failing JSON key assertions**

In `src/unittest/router_runtime_test.cpp`, find the metrics JSON assertion block that checks `"linux_relay_event_queue_full_errors":`. Add checks:

```cpp
        if (bodyText.find("\"linux_relay_event_queue_capacity\":") == std::string::npos) return 261;
        if (bodyText.find("\"linux_relay_event_queue_push_cas_retries\":") == std::string::npos) return 262;
        if (bodyText.find("\"linux_relay_event_queue_pop_cas_retries\":") == std::string::npos) return 263;
        if (bodyText.find("\"linux_relay_event_producer_threads_observed\":") == std::string::npos) return 264;
        if (bodyText.find("\"linux_relay_multiple_event_producer_threads_observed\":") == std::string::npos) return 265;
```

Run:

```bash
rtk cmake --build build --target tcpquic_router_runtime_test -j$(nproc)
rtk build/bin/Release/tcpquic_router_runtime_test
```

Expected: test fails because keys are missing.

- [x] **Step 2: Add relay metrics fields**

In `src/runtime/relay_metrics.h`, add to `TqRelayMetricsSnapshot` near existing Linux relay event fields:

```cpp
uint64_t LinuxRelayEventQueueCapacity{0};
uint64_t LinuxRelayEventQueuePushCasRetries{0};
uint64_t LinuxRelayEventQueuePopCasRetries{0};
uint64_t LinuxRelayEventProducerThreadsObserved{0};
bool LinuxRelayMultipleEventProducerThreadsObserved{false};
```

In `ConvertLinuxRelayMetrics()` in `src/runtime/relay_metrics.cpp`, map:

```cpp
metrics.LinuxRelayEventQueueCapacity = snapshot.EventQueueCapacity;
metrics.LinuxRelayEventQueuePushCasRetries = snapshot.EventQueuePushCasRetries;
metrics.LinuxRelayEventQueuePopCasRetries = snapshot.EventQueuePopCasRetries;
metrics.LinuxRelayEventProducerThreadsObserved = snapshot.EventProducerThreadsObserved;
metrics.LinuxRelayMultipleEventProducerThreadsObserved =
    snapshot.MultipleEventProducerThreadsObserved;
```

In `TqRelayMetricsJson()`, output after `linux_relay_event_queue_full_errors`:

```cpp
    out << ",\"linux_relay_event_queue_capacity\":"
        << metrics.LinuxRelayEventQueueCapacity;
    out << ",\"linux_relay_event_queue_push_cas_retries\":"
        << metrics.LinuxRelayEventQueuePushCasRetries;
    out << ",\"linux_relay_event_queue_pop_cas_retries\":"
        << metrics.LinuxRelayEventQueuePopCasRetries;
    out << ",\"linux_relay_event_producer_threads_observed\":"
        << metrics.LinuxRelayEventProducerThreadsObserved;
    out << ",\"linux_relay_multiple_event_producer_threads_observed\":"
        << (metrics.LinuxRelayMultipleEventProducerThreadsObserved ? "true" : "false");
```

Run:

```bash
rtk cmake --build build --target tcpquic_router_runtime_test -j$(nproc)
rtk build/bin/Release/tcpquic_router_runtime_test
```

Expected: test passes.

## Task 6: Add worker detail capacity field

**Files:**
- Modify: `src/runtime/relay_metrics.h`
- Modify: `src/runtime/relay_metrics.cpp`
- Test: `src/unittest/router_runtime_test.cpp`

- [x] **Step 1: Add failing worker JSON assertion**

In `src/unittest/router_runtime_test.cpp`, in the relay workers response assertions near `per_worker_active_relays`, add:

```cpp
        if (relayWorkersResp.find("\"event_queue_capacity\":") == std::string::npos) return 350;
```

Run:

```bash
rtk cmake --build build --target tcpquic_router_runtime_test -j$(nproc)
rtk build/bin/Release/tcpquic_router_runtime_test
```

Expected: test fails because worker detail lacks `event_queue_capacity`.

- [x] **Step 2: Add worker snapshot JSON field**

In `src/runtime/relay_metrics.h`, add to `TqRelayWorkerSnapshot`:

```cpp
uint64_t EventQueueCapacity{0};
```

In `ConvertLinuxRelayWorkerSnapshot()`:

```cpp
worker.EventQueueCapacity = snapshot.EventQueueCapacity;
```

In `TqRelayWorkerJsonValue()` or the worker JSON writer in `src/runtime/relay_metrics.cpp`, add:

```cpp
{"event_queue_capacity", worker.EventQueueCapacity},
```

Run:

```bash
rtk cmake --build build --target tcpquic_router_runtime_test -j$(nproc)
rtk build/bin/Release/tcpquic_router_runtime_test
```

Expected: test passes.

## Task 7: Update docs

**Files:**
- Modify: `docs/admin-api/interface.md`
- Modify: `docs/relay_linux.md`

- [x] **Step 1: Update admin API metrics field list**

In `docs/admin-api/interface.md`, document these Linux metrics:

```text
linux_relay_event_queue_capacity
linux_relay_event_queue_push_cas_retries
linux_relay_event_queue_pop_cas_retries
linux_relay_event_producer_threads_observed
linux_relay_multiple_event_producer_threads_observed
```

Also document `event_queue_capacity` in relay worker detail objects.

- [x] **Step 2: Update relay Linux doc**

In `docs/relay_linux.md`:

- Move `3.2 Event queue capacity 未暴露配置` from unresolved wording to planned/implemented wording after code lands.
- Keep `3.3 QUIC receive 背压没有 hysteresis` unchanged.
- Update 排障优先级 to mention comparing `linux_relay_pending_events` with `linux_relay_event_queue_capacity`, and interpreting `push_cas_retries` with `multiple_event_producer_threads_observed`.

- [x] **Step 3: Check docs diff**

Run:

```bash
rtk git diff --check -- docs/admin-api/interface.md docs/relay_linux.md
```

Expected: no output and exit 0.

## Task 8: Final verification and commit

**Files:**
- All modified files from prior tasks.

- [x] **Step 1: Build test targets**

Run:

```bash
rtk cmake --build build --target tcpquic_tuning_test tcpquic_linux_relay_worker_queue_test tcpquic_router_runtime_test -j$(nproc)
```

Expected: build succeeds.

- [x] **Step 2: Run tests**

Run:

```bash
rtk build/bin/Release/tcpquic_tuning_test
rtk build/bin/Release/tcpquic_linux_relay_worker_queue_test
rtk build/bin/Release/tcpquic_router_runtime_test
```

Expected: all three commands exit 0.

- [x] **Step 3: Run whitespace check**

Run:

```bash
rtk git diff --check -- \
  src/config/config.h \
  src/config/config.cpp \
  src/config/tuning.h \
  src/config/tuning.cpp \
  src/tunnel/linux_relay_event_queue.h \
  src/tunnel/linux_relay_worker.h \
  src/tunnel/linux_relay_worker.cpp \
  src/runtime/relay_metrics.h \
  src/runtime/relay_metrics.cpp \
  src/unittest/tuning_test.cpp \
  src/unittest/linux_relay_worker_queue_test.cpp \
  src/unittest/router_runtime_test.cpp \
  docs/admin-api/interface.md \
  docs/relay_linux.md
```

Expected: no output and exit 0.

- [x] **Step 4: Commit**

Run:

```bash
rtk git add \
  src/config/config.h \
  src/config/config.cpp \
  src/config/tuning.h \
  src/config/tuning.cpp \
  src/tunnel/linux_relay_event_queue.h \
  src/tunnel/linux_relay_worker.h \
  src/tunnel/linux_relay_worker.cpp \
  src/runtime/relay_metrics.h \
  src/runtime/relay_metrics.cpp \
  src/unittest/tuning_test.cpp \
  src/unittest/linux_relay_worker_queue_test.cpp \
  src/unittest/router_runtime_test.cpp \
  docs/admin-api/interface.md \
  docs/relay_linux.md
rtk git commit -m "Add Linux relay event queue tuning"
```

Expected: commit includes only the implementation, tests, and docs for this plan.
