# Linux Relay Control Plane Wait Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 收敛 Linux relay runtime/worker 控制面持锁等待，给同步 command wait 增加 timeout 和 metrics，避免 admin snapshot 或 worker 忙时放大建链/拆链阻塞。

**Architecture:** Runtime snapshot 先复制 worker 指针并用 in-flight guard 保护 worker lifetime，再释放 `Runtime::Lock` 后逐 worker snapshot；worker register/unregister/snapshot 只短暂持 `ControlLock` 读取运行状态，不持锁等待 command `Cv`。同步 command 使用 shared ownership 支撑 timeout 后晚到 completion，metrics 聚合到现有 relay metrics JSON。

**Tech Stack:** C++17, Linux relay worker/event queue, std::mutex/condition_variable/atomic, nlohmann/json, CMake, `tcpquic_linux_relay_worker_io_test`, `tcpquic_linux_relay_worker_queue_test`, `tcpquic_router_runtime_test`。

---

## File Structure

- Modify: `src/tunnel/linux_relay_event_queue.h`
- Modify: `src/tunnel/linux_relay_worker.h`
- Modify: `src/tunnel/linux_relay_worker.cpp`
- Modify: `src/runtime/relay_metrics.h`
- Modify: `src/runtime/relay_metrics.cpp`
- Modify: `src/unittest/linux_relay_worker_io_test.cpp`
- Modify: `src/unittest/linux_relay_worker_queue_test.cpp`
- Modify: `src/unittest/router_runtime_test.cpp`
- Modify: `docs/admin-api/interface.md`
- Modify: `docs/relay_linux.md`

## Task 1: Add shared command ownership and wait helper

**Files:**
- Modify: `src/tunnel/linux_relay_event_queue.h`
- Modify: `src/tunnel/linux_relay_worker.h`
- Modify: `src/tunnel/linux_relay_worker.cpp`
- Test: `src/unittest/linux_relay_worker_queue_test.cpp`

- [ ] **Step 1: Write failing timeout-lifetime test**

In `src/unittest/linux_relay_worker_queue_test.cpp`, add a block before `return 0;`:

```cpp
    {
        TqLinuxRelayWorkerConfig config{};
        config.EventQueueCapacity = 1024;
        TqLinuxRelayWorker worker(config);
        if (!worker.Start()) return 121;

        const TqLinuxRelayWorkerSnapshot snapshot = worker.Snapshot();
        if (snapshot.ControlCommandWaitCount == 0) return 123;
        if (snapshot.ControlCommandWaitNanos == 0) return 124;
        if (snapshot.SnapshotCommandWaitCount == 0) return 125;
        if (snapshot.SnapshotCommandWaitNanos == 0) return 126;
        worker.Stop();
    }
```

Run:

```bash
rtk cmake --build build --target tcpquic_linux_relay_worker_queue_test -j$(nproc)
```

Expected: compile fails because `ControlCommandWaitCount` and `ControlCommandWaitNanos` do not exist.

- [ ] **Step 2: Add control owner to event**

In `src/tunnel/linux_relay_event_queue.h`, add a shared owner field to `TqLinuxRelayEvent`:

```cpp
std::shared_ptr<void> ControlOwner;
```

Keep the existing `void* Control` field. The worker event switch continues using `event.Control`, while `event.ControlOwner` keeps heap command lifetime valid until the event is destroyed.

- [ ] **Step 3: Add command wait metrics fields**

In `src/tunnel/linux_relay_worker.h`, add to `TqLinuxRelayWorkerSnapshot`:

```cpp
uint64_t ControlLockWaitNanos{0};
uint64_t ControlLockAcquireCount{0};
uint64_t ControlCommandWaitNanos{0};
uint64_t ControlCommandWaitCount{0};
uint64_t ControlCommandTimeouts{0};
uint64_t ControlCommandEnqueueFailures{0};
uint64_t SnapshotCommandWaitNanos{0};
uint64_t SnapshotCommandWaitCount{0};
uint64_t SnapshotCommandTimeouts{0};
```

Add worker atomics near existing error/control counters:

```cpp
mutable std::atomic<uint64_t> ControlLockWaitNanos{0};
mutable std::atomic<uint64_t> ControlLockAcquireCount{0};
mutable std::atomic<uint64_t> ControlCommandWaitNanos{0};
mutable std::atomic<uint64_t> ControlCommandWaitCount{0};
mutable std::atomic<uint64_t> ControlCommandTimeouts{0};
mutable std::atomic<uint64_t> ControlCommandEnqueueFailures{0};
mutable std::atomic<uint64_t> SnapshotCommandWaitNanos{0};
mutable std::atomic<uint64_t> SnapshotCommandWaitCount{0};
mutable std::atomic<uint64_t> SnapshotCommandTimeouts{0};
```

In `SnapshotLocal()`, copy these atomics into the snapshot.

- [ ] **Step 4: Add timeout config and wait helpers**

In `src/tunnel/linux_relay_worker.h`, add to `TqLinuxRelayWorkerConfig`:

```cpp
uint32_t ControlCommandTimeoutMs{5000};
```

Do not expose this field through CLI, config JSON, or admin JSON in this plan.

In `src/tunnel/linux_relay_worker.cpp`, add near command completion helpers:

```cpp
namespace {
constexpr uint32_t kLinuxRelayControlEnqueueRetries = 8;
}
```

Add helper method declarations in `TqLinuxRelayWorker`:

```cpp
bool WaitRegisterCommand(RegisterRelayCommand& command) const;
bool WaitUnregisterCommand(UnregisterRelayCommand& command) const;
bool WaitSnapshotCommand(SnapshotCommand& command) const;
```

Implement this local helper in `src/tunnel/linux_relay_worker.cpp`:

```cpp
template <typename Command>
bool WaitForCommandDone(
    Command& command,
    uint32_t timeoutMs,
    std::atomic<uint64_t>& waitNanos,
    std::atomic<uint64_t>& waitCount,
    std::atomic<uint64_t>& timeouts) {
    const auto start = std::chrono::steady_clock::now();
    const auto deadline = start + std::chrono::milliseconds(timeoutMs);
    std::unique_lock<std::mutex> lock(command.Mutex);
    const bool done = command.Cv.wait_until(lock, deadline, [&command]() {
        return command.Done;
    });
    const auto elapsed = std::chrono::steady_clock::now() - start;
    command.WaitNanos = static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(elapsed).count());
    waitNanos.fetch_add(command.WaitNanos, std::memory_order_relaxed);
    waitCount.fetch_add(1, std::memory_order_relaxed);
    if (!done) {
        timeouts.fetch_add(1, std::memory_order_relaxed);
    }
    return done;
}
```

Then implement the three member methods explicitly:

```cpp
bool TqLinuxRelayWorker::WaitRegisterCommand(RegisterRelayCommand& command) const {
    return WaitForCommandDone(
        command,
        Config.ControlCommandTimeoutMs,
        ControlCommandWaitNanos,
        ControlCommandWaitCount,
        ControlCommandTimeouts);
}

bool TqLinuxRelayWorker::WaitUnregisterCommand(UnregisterRelayCommand& command) const {
    return WaitForCommandDone(
        command,
        Config.ControlCommandTimeoutMs,
        ControlCommandWaitNanos,
        ControlCommandWaitCount,
        ControlCommandTimeouts);
}

bool TqLinuxRelayWorker::WaitSnapshotCommand(SnapshotCommand& command) const {
    const bool done = WaitForCommandDone(
        command,
        Config.ControlCommandTimeoutMs,
        ControlCommandWaitNanos,
        ControlCommandWaitCount,
        ControlCommandTimeouts);
    SnapshotCommandWaitNanos.fetch_add(command.WaitNanos, std::memory_order_relaxed);
    SnapshotCommandWaitCount.fetch_add(1, std::memory_order_relaxed);
    if (!done) {
        SnapshotCommandTimeouts.fetch_add(1, std::memory_order_relaxed);
    }
    return done;
}
```

To support the snapshot-specific accumulation above, add `uint64_t WaitNanos{0};` to each command struct and have `WaitForCommandDone()` assign it before returning.

- [ ] **Step 5: Run queue test**

Run:

```bash
rtk cmake --build build --target tcpquic_linux_relay_worker_queue_test -j$(nproc)
rtk build/bin/Release/tcpquic_linux_relay_worker_queue_test
```

Expected: test passes after Steps 2-4 are complete.

## Task 2: Remove ControlLock while waiting in production paths

**Files:**
- Modify: `src/tunnel/linux_relay_worker.h`
- Modify: `src/tunnel/linux_relay_worker.cpp`
- Test: `src/unittest/linux_relay_worker_io_test.cpp`

- [ ] **Step 1: Add control state helper**

In `src/tunnel/linux_relay_worker.h`, add private struct and methods:

```cpp
struct ControlState {
    bool Running{false};
    bool IsWorkerThread{false};
};

ControlState GetControlState() const;
std::unique_lock<std::mutex> AcquireControlLockForMetrics() const;
```

In `src/tunnel/linux_relay_worker.cpp`, implement a lock helper that records wait duration:

```cpp
std::unique_lock<std::mutex> TqLinuxRelayWorker::AcquireControlLockForMetrics() const {
    const auto start = std::chrono::steady_clock::now();
    std::unique_lock<std::mutex> lock(ControlLock);
    const auto elapsed = std::chrono::steady_clock::now() - start;
    ControlLockWaitNanos.fetch_add(
        static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(elapsed).count()),
        std::memory_order_relaxed);
    ControlLockAcquireCount.fetch_add(1, std::memory_order_relaxed);
    return lock;
}
```

Use the helper in `GetControlState()`:

```cpp
TqLinuxRelayWorker::ControlState TqLinuxRelayWorker::GetControlState() const {
    const auto current = std::this_thread::get_id();
    auto guard = AcquireControlLockForMetrics();
    ControlState state{};
    state.Running = Running.load(std::memory_order_acquire);
    state.IsWorkerThread = current == WorkerThreadId;
    return state;
}
```

- [ ] **Step 2: Update RegisterRelayWithId**

Replace the current `RegisterRelayWithId()` body with this shape:

```cpp
TqLinuxRelayRegistrationResult TqLinuxRelayWorker::RegisterRelayWithId(
    const TqLinuxRelayRegistration& registration) {
    if (IsWorkerThread()) {
        return RegisterRelayWithIdLocal(registration);
    }

    const ControlState state = GetControlState();
    if (!state.Running || state.IsWorkerThread) {
        return RegisterRelayWithIdLocal(registration);
    }

    auto command = std::make_shared<RegisterRelayCommand>();
    command->Registration = registration;

    for (uint32_t attempt = 0; attempt < kLinuxRelayControlEnqueueRetries; ++attempt) {
        TqLinuxRelayEvent event{};
        event.Type = TqLinuxRelayEventType::RegisterRelay;
        event.Control = command.get();
        event.ControlOwner = command;
        if (Enqueue(std::move(event))) {
            if (!WaitRegisterCommand(*command)) {
                return {};
            }
            return command->Result;
        }
        ControlCommandEnqueueFailures.fetch_add(1, std::memory_order_relaxed);
        Wake();
        std::this_thread::yield();
    }
    return {};
}
```

Do not hold `ControlLock` during `WaitRegisterCommand()`.

- [ ] **Step 3: Update UnregisterRelay**

Apply the same pattern to `UnregisterRelay(uint64_t relayId)`:

```cpp
auto command = std::make_shared<UnregisterRelayCommand>();
command->RelayId = relayId;
...
event.Control = command.get();
event.ControlOwner = command;
...
if (WaitUnregisterCommand(*command)) return;
```

On timeout or enqueue exhaustion, increment `ControlCommandTimeouts` or `ControlCommandEnqueueFailures` and return.

- [ ] **Step 4: Update Snapshot**

Apply the same pattern to `Snapshot()`:

```cpp
auto command = std::make_shared<SnapshotCommand>();
...
if (!WaitSnapshotCommand(*command)) {
    return {};
}
return command->Result;
```

`WaitSnapshotCommand()` must update both general control command wait metrics and snapshot-specific wait metrics.

- [ ] **Step 5: Add no-ControlLock-wait regression test**

In `src/unittest/linux_relay_worker_io_test.cpp`, add a test block near existing runtime/worker control tests:

```cpp
    {
        TqLinuxRelayWorkerConfig config{};
        config.EventQueueCapacity = 1024;
        TqLinuxRelayWorker worker(config);
        if (!worker.Start()) return 3401;

        std::atomic<bool> snapshotStarted{false};
        std::atomic<bool> registerReturned{false};
        std::thread snapshotThread([&]() {
            snapshotStarted.store(true, std::memory_order_release);
            (void)worker.Snapshot();
        });

        while (!snapshotStarted.load(std::memory_order_acquire)) {
            std::this_thread::yield();
        }

        int fds[2]{-1, -1};
        if (::socketpair(AF_UNIX, SOCK_STREAM, 0, fds) != 0) {
            snapshotThread.join();
            worker.Stop();
            return 3403;
        }

        TqLinuxRelayRegistration registration{};
        registration.TcpFd = fds[0];
        registration.SinkQuicReceives = false;
        registration.EnableQuicSends = false;
        registration.Stream = nullptr;
        registration.Handle = nullptr;
        TqLinuxRelayRegistrationResult result = worker.RegisterRelayWithId(registration);
        registerReturned.store(true, std::memory_order_release);

        snapshotThread.join();
        if (!registerReturned.load(std::memory_order_acquire)) return 3402;
        if (result.Ok) {
            worker.UnregisterRelay(result.RelayId);
        } else {
            ::close(fds[0]);
        }
        worker.Stop();
        ::close(fds[1]);
        (void)result;
    }
```

- [ ] **Step 6: Run worker IO test**

Run:

```bash
rtk cmake --build build --target tcpquic_linux_relay_worker_io_test -j$(nproc)
rtk build/bin/Release/tcpquic_linux_relay_worker_io_test
```

Expected: test passes.

## Task 3: Runtime snapshot in-flight guard and lock wait metrics

**Files:**
- Modify: `src/tunnel/linux_relay_worker.h`
- Modify: `src/tunnel/linux_relay_worker.cpp`
- Test: `src/unittest/linux_relay_worker_io_test.cpp`

- [ ] **Step 1: Add runtime in-flight fields**

In `TqLinuxRelayRuntime`, add:

```cpp
mutable std::mutex SnapshotLock;
mutable std::condition_variable SnapshotCv;
mutable uint32_t RuntimeSnapshotsInFlight{0};
mutable std::atomic<uint64_t> RuntimeLockWaitNanos{0};
mutable std::atomic<uint64_t> RuntimeLockAcquireCount{0};
mutable std::atomic<uint64_t> RuntimeSnapshotInFlightMax{0};
```

Add helper methods:

```cpp
std::unique_lock<std::mutex> AcquireRuntimeLockForMetrics() const;
std::vector<TqLinuxRelayWorker*> AcquireSnapshotWorkers() const;
void ReleaseSnapshotWorkers() const;
```

- [ ] **Step 2: Implement runtime lock wait helper**

Add a runtime lock acquire helper in `src/tunnel/linux_relay_worker.cpp`:

```cpp
std::unique_lock<std::mutex> TqLinuxRelayRuntime::AcquireRuntimeLockForMetrics() const {
    const auto start = std::chrono::steady_clock::now();
    std::unique_lock<std::mutex> lock(Lock);
    const auto elapsed = std::chrono::steady_clock::now() - start;
    RuntimeLockWaitNanos.fetch_add(
        static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(elapsed).count()),
        std::memory_order_relaxed);
    RuntimeLockAcquireCount.fetch_add(1, std::memory_order_relaxed);
    return lock;
}
```

Use it instead of direct `std::lock_guard<std::mutex> guard(Lock)` in `Start()`, `Stop()`, `PickWorker()`, `AcquireSnapshotWorkers()`, and `SnapshotWorkers()`.

- [ ] **Step 3: Implement AcquireSnapshotWorkers**

```cpp
std::vector<TqLinuxRelayWorker*> TqLinuxRelayRuntime::AcquireSnapshotWorkers() const {
    std::vector<TqLinuxRelayWorker*> workers;
    {
        auto runtimeGuard = AcquireRuntimeLockForMetrics();
        workers.reserve(Workers.size());
        for (const auto& worker : Workers) {
            workers.push_back(worker.get());
        }
        std::lock_guard<std::mutex> snapshotGuard(SnapshotLock);
        ++RuntimeSnapshotsInFlight;
        RuntimeSnapshotInFlightMax.store(
            std::max<uint64_t>(
                RuntimeSnapshotInFlightMax.load(std::memory_order_relaxed),
                RuntimeSnapshotsInFlight),
            std::memory_order_relaxed);
    }
    return workers;
}
```

Always increment `RuntimeSnapshotsInFlight`, including when `workers` is empty. `Snapshot()` and `SnapshotWorkers()` must always create the release guard immediately after `AcquireSnapshotWorkers()` returns.

- [ ] **Step 4: Update Runtime::Snapshot and SnapshotWorkers**

Use a local RAII guard:

```cpp
const auto workers = AcquireSnapshotWorkers();
struct SnapshotReleaseGuard {
    const TqLinuxRelayRuntime* Runtime;
    ~SnapshotReleaseGuard() { Runtime->ReleaseSnapshotWorkers(); }
} release{this};
```

Then iterate `workers` without holding `Runtime::Lock`.

- [ ] **Step 5: Update Stop**

Before clearing `Workers`, wait for snapshots:

```cpp
{
    std::unique_lock<std::mutex> snapshotGuard(SnapshotLock);
    SnapshotCv.wait(snapshotGuard, [this]() {
        return RuntimeSnapshotsInFlight == 0;
    });
}
Workers.clear();
NextWorker = 0;
```

This wait happens while `Runtime::Lock` is held in `Stop()`, which prevents new snapshot worker lists from being copied while stop is pending.

- [ ] **Step 6: Add runtime snapshot/PickWorker regression test**

In `src/unittest/linux_relay_worker_io_test.cpp`, add a runtime test:

```cpp
    {
        TqLinuxRelayRuntime::Instance().Stop();
        TqTuningConfig tuning{};
        tuning.LinuxRelayWorkerCount = 1;
        tuning.LinuxRelayEventQueueCapacity = 1024;
        if (!TqLinuxRelayRuntime::Instance().Start(tuning)) return 3410;
        std::atomic<bool> stop{false};
        std::thread snapshots([&]() {
            for (uint32_t i = 0; i < 100 && !stop.load(std::memory_order_acquire); ++i) {
                (void)TqLinuxRelayRuntime::Instance().Snapshot();
            }
        });
        bool pickOk = true;
        for (uint32_t i = 0; i < 100; ++i) {
            if (TqLinuxRelayRuntime::Instance().PickWorker() == nullptr) {
                pickOk = false;
                break;
            }
        }
        stop.store(true, std::memory_order_release);
        snapshots.join();
        const auto snapshot = TqLinuxRelayRuntime::Instance().Snapshot();
        TqLinuxRelayRuntime::Instance().Stop();
        if (!pickOk) return 3411;
        if (snapshot.RuntimeLockAcquireCount == 0) return 3412;
    }
```

Use the exact `RuntimeLockAcquireCount` field added in Task 4.

- [ ] **Step 7: Run worker IO test**

Run:

```bash
rtk cmake --build build --target tcpquic_linux_relay_worker_io_test -j$(nproc)
rtk build/bin/Release/tcpquic_linux_relay_worker_io_test
```

Expected: test passes.

## Task 4: Expose control wait metrics in snapshots and JSON

**Files:**
- Modify: `src/tunnel/linux_relay_worker.h`
- Modify: `src/tunnel/linux_relay_worker.cpp`
- Modify: `src/runtime/relay_metrics.h`
- Modify: `src/runtime/relay_metrics.cpp`
- Test: `src/unittest/router_runtime_test.cpp`

- [ ] **Step 1: Add runtime metrics fields to worker snapshot**

In `TqLinuxRelayWorkerSnapshot`, add:

```cpp
uint64_t RuntimeLockWaitNanos{0};
uint64_t RuntimeLockAcquireCount{0};
uint64_t RuntimeSnapshotInFlightMax{0};
```

In `TqLinuxRelayRuntime::Snapshot()` aggregate result, set these from runtime atomics:

```cpp
total.RuntimeLockWaitNanos = RuntimeLockWaitNanos.load(std::memory_order_relaxed);
total.RuntimeLockAcquireCount = RuntimeLockAcquireCount.load(std::memory_order_relaxed);
total.RuntimeSnapshotInFlightMax = RuntimeSnapshotInFlightMax.load(std::memory_order_relaxed);
```

- [ ] **Step 2: Aggregate worker control metrics**

In `TqLinuxRelayRuntime::Snapshot()`, sum these per worker:

```cpp
total.ControlCommandWaitNanos += snapshot.ControlCommandWaitNanos;
total.ControlCommandWaitCount += snapshot.ControlCommandWaitCount;
total.ControlCommandTimeouts += snapshot.ControlCommandTimeouts;
total.ControlCommandEnqueueFailures += snapshot.ControlCommandEnqueueFailures;
total.SnapshotCommandWaitNanos += snapshot.SnapshotCommandWaitNanos;
total.SnapshotCommandWaitCount += snapshot.SnapshotCommandWaitCount;
total.SnapshotCommandTimeouts += snapshot.SnapshotCommandTimeouts;
total.ControlLockWaitNanos += snapshot.ControlLockWaitNanos;
total.ControlLockAcquireCount += snapshot.ControlLockAcquireCount;
```

- [ ] **Step 3: Add relay metrics fields**

In `src/runtime/relay_metrics.h`, add to `TqRelayMetricsSnapshot`:

```cpp
uint64_t LinuxRelayControlLockWaitNanos{0};
uint64_t LinuxRelayControlLockAcquireCount{0};
uint64_t LinuxRelayControlCommandWaitNanos{0};
uint64_t LinuxRelayControlCommandWaitCount{0};
uint64_t LinuxRelayControlCommandTimeouts{0};
uint64_t LinuxRelayControlCommandEnqueueFailures{0};
uint64_t LinuxRelaySnapshotCommandWaitNanos{0};
uint64_t LinuxRelaySnapshotCommandWaitCount{0};
uint64_t LinuxRelaySnapshotCommandTimeouts{0};
uint64_t LinuxRelayRuntimeLockWaitNanos{0};
uint64_t LinuxRelayRuntimeLockAcquireCount{0};
uint64_t LinuxRelayRuntimeSnapshotInFlightMax{0};
```

Map them in `TqSnapshotRelayMetrics()` from the Linux runtime snapshot.

- [ ] **Step 4: Output JSON fields**

In `TqAppendRelayMetricsJson()`, after existing Linux event queue fields, output:

```cpp
out << ",\"linux_relay_control_lock_wait_nanos\":"
    << metrics.LinuxRelayControlLockWaitNanos;
out << ",\"linux_relay_control_lock_acquire_count\":"
    << metrics.LinuxRelayControlLockAcquireCount;
out << ",\"linux_relay_control_command_wait_nanos\":"
    << metrics.LinuxRelayControlCommandWaitNanos;
out << ",\"linux_relay_control_command_wait_count\":"
    << metrics.LinuxRelayControlCommandWaitCount;
out << ",\"linux_relay_control_command_timeouts\":"
    << metrics.LinuxRelayControlCommandTimeouts;
out << ",\"linux_relay_control_command_enqueue_failures\":"
    << metrics.LinuxRelayControlCommandEnqueueFailures;
out << ",\"linux_relay_snapshot_command_wait_nanos\":"
    << metrics.LinuxRelaySnapshotCommandWaitNanos;
out << ",\"linux_relay_snapshot_command_wait_count\":"
    << metrics.LinuxRelaySnapshotCommandWaitCount;
out << ",\"linux_relay_snapshot_command_timeouts\":"
    << metrics.LinuxRelaySnapshotCommandTimeouts;
out << ",\"linux_relay_runtime_lock_wait_nanos\":"
    << metrics.LinuxRelayRuntimeLockWaitNanos;
out << ",\"linux_relay_runtime_lock_acquire_count\":"
    << metrics.LinuxRelayRuntimeLockAcquireCount;
out << ",\"linux_relay_runtime_snapshot_inflight_max\":"
    << metrics.LinuxRelayRuntimeSnapshotInFlightMax;
```

- [ ] **Step 5: Add router runtime JSON assertions**

In `src/unittest/router_runtime_test.cpp`, near existing Linux relay metrics key checks, add:

```cpp
if (bodyText.find("\"linux_relay_control_lock_wait_nanos\":") == std::string::npos) return 360;
if (bodyText.find("\"linux_relay_control_lock_acquire_count\":") == std::string::npos) return 361;
if (bodyText.find("\"linux_relay_control_command_wait_nanos\":") == std::string::npos) return 362;
if (bodyText.find("\"linux_relay_control_command_wait_count\":") == std::string::npos) return 363;
if (bodyText.find("\"linux_relay_control_command_timeouts\":") == std::string::npos) return 364;
if (bodyText.find("\"linux_relay_control_command_enqueue_failures\":") == std::string::npos) return 365;
if (bodyText.find("\"linux_relay_snapshot_command_wait_nanos\":") == std::string::npos) return 366;
if (bodyText.find("\"linux_relay_snapshot_command_wait_count\":") == std::string::npos) return 367;
if (bodyText.find("\"linux_relay_snapshot_command_timeouts\":") == std::string::npos) return 368;
if (bodyText.find("\"linux_relay_runtime_lock_wait_nanos\":") == std::string::npos) return 369;
if (bodyText.find("\"linux_relay_runtime_lock_acquire_count\":") == std::string::npos) return 370;
if (bodyText.find("\"linux_relay_runtime_snapshot_inflight_max\":") == std::string::npos) return 371;
```

- [ ] **Step 6: Run router runtime test**

Run:

```bash
rtk cmake --build build --target tcpquic_router_runtime_test -j$(nproc)
rtk build/bin/Release/tcpquic_router_runtime_test
```

Expected: test passes.

## Task 5: Update docs

**Files:**
- Modify: `docs/admin-api/interface.md`
- Modify: `docs/relay_linux.md`

- [ ] **Step 1: Update admin API docs**

In `docs/admin-api/interface.md`, document the new Linux metrics:

```text
linux_relay_control_lock_wait_nanos
linux_relay_control_lock_acquire_count
linux_relay_control_command_wait_nanos
linux_relay_control_command_wait_count
linux_relay_control_command_timeouts
linux_relay_control_command_enqueue_failures
linux_relay_snapshot_command_wait_nanos
linux_relay_snapshot_command_wait_count
linux_relay_snapshot_command_timeouts
linux_relay_runtime_lock_wait_nanos
linux_relay_runtime_lock_acquire_count
linux_relay_runtime_snapshot_inflight_max
```

- [ ] **Step 2: Update relay Linux doc**

In `docs/relay_linux.md`:

- Change `3.4 runtime snapshot 持锁范围偏大` to current-status wording after implementation.
- Change `3.5 ControlLock 持锁等待 worker command` to current-status wording after implementation.
- Change `3.8` to say queue metrics and control wait metrics are now available, while any remaining gaps are lower-priority details such as per-relay callback pending top N.
- Keep `3.3 QUIC receive 背压没有 hysteresis` unchanged.
- Keep `3.6 fake FIN abort` unchanged.

- [ ] **Step 3: Run docs check**

Run:

```bash
rtk git diff --check -- docs/admin-api/interface.md docs/relay_linux.md
```

Expected: no output and exit 0.

## Task 6: Final verification and commit

**Files:**
- All files touched in this plan.

- [ ] **Step 1: Build targets**

Run:

```bash
rtk cmake --build build --target tcpquic_linux_relay_worker_io_test tcpquic_linux_relay_worker_queue_test tcpquic_router_runtime_test -j$(nproc)
```

Expected: build succeeds.

- [ ] **Step 2: Run tests**

Run:

```bash
rtk build/bin/Release/tcpquic_linux_relay_worker_io_test
rtk build/bin/Release/tcpquic_linux_relay_worker_queue_test
rtk build/bin/Release/tcpquic_router_runtime_test
```

Expected: all three exit 0.

- [ ] **Step 3: Run whitespace check**

Run:

```bash
rtk git diff --check -- \
  src/tunnel/linux_relay_event_queue.h \
  src/tunnel/linux_relay_worker.h \
  src/tunnel/linux_relay_worker.cpp \
  src/runtime/relay_metrics.h \
  src/runtime/relay_metrics.cpp \
  src/unittest/linux_relay_worker_io_test.cpp \
  src/unittest/linux_relay_worker_queue_test.cpp \
  src/unittest/router_runtime_test.cpp \
  docs/admin-api/interface.md \
  docs/relay_linux.md
```

Expected: no output and exit 0.

- [ ] **Step 4: Commit**

Run:

```bash
rtk git add \
  src/tunnel/linux_relay_event_queue.h \
  src/tunnel/linux_relay_worker.h \
  src/tunnel/linux_relay_worker.cpp \
  src/runtime/relay_metrics.h \
  src/runtime/relay_metrics.cpp \
  src/unittest/linux_relay_worker_io_test.cpp \
  src/unittest/linux_relay_worker_queue_test.cpp \
  src/unittest/router_runtime_test.cpp \
  docs/admin-api/interface.md \
  docs/relay_linux.md
rtk git commit -m "Reduce Linux relay control wait contention"
```

Expected: commit includes only implementation, tests, and docs for this plan.
