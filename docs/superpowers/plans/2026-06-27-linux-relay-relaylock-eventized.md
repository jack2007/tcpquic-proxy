# Linux Relay RelayLock Eventization Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 分两个阶段优化 Linux relay 的 relay 查找和容器所有权：先用 `RelaysById` 把热路径查找从线性扫描改为 O(1)，再把 register/unregister/snapshot 事件化并移除 `RelayLock`。

**Architecture:** 阶段一保留现有同步模型，在 `RelayLock` 下维护 active/retired relay id 索引，降低查找复杂度且控制风险。阶段二把所有 relay 容器读写收敛到 worker event loop，通过同步 control event 保持外部 API 语义，最终让 relay 容器成为 worker 私有状态。

**Tech Stack:** C++17, Linux epoll/eventfd, MsQuic stream callback, `std::unordered_map`, `std::condition_variable`, existing CMake unit tests `tcpquic_linux_relay_worker_queue_test` and `tcpquic_linux_relay_worker_io_test`.

---

## File Map

- Modify: `src/tunnel/linux_relay_worker.h`
  - Add `<unordered_map>` include.
  - Add `RelaysById` and `RetiredRelaysById` for stage 1.
  - Add control event command structs and helper declarations for stage 2.
  - Remove `RelayLock` after stage 2 converts all relay container access to worker-local access.
- Modify: `src/tunnel/linux_relay_worker.cpp`
  - Update register/unregister/purge/find/snapshot logic for `RelaysById`.
  - Add worker-local helpers: register, unregister, snapshot, lookup.
  - Add control event processing for register/unregister/snapshot.
  - Preserve direct worker-thread execution path to avoid self-deadlock.
- Modify: `src/tunnel/linux_relay_event_queue.h`
  - Add control event types and a `void* Control` payload for synchronous register/unregister/snapshot commands.
- Modify: `src/unittest/linux_relay_worker_io_test.cpp`
  - Add regression tests for multi-relay lookup, unregister/retired lookup, eventized register/unregister, and snapshot consistency.
- Modify: `src/unittest/linux_relay_worker_queue_test.cpp`
  - Add queue/control event smoke coverage if control event payload changes queue behavior.
- Modify: `docs/relay_linux.md`
  - Update lock section after implementation to reflect `RelaysById` and eventized ownership.

## Phase 1: O(1) relay lookup while keeping `RelayLock`

### Task 1: Add multi-relay lookup regression test

**Files:**
- Modify: `src/unittest/linux_relay_worker_io_test.cpp`

- [ ] **Step 1: Add a test block before the final concurrent register/unregister stress block**

Add this block near the end of `main()` before the existing `constexpr int kRelayCount = 8` stress test:

```cpp
    {
        TqLinuxRelayWorkerConfig config{};
        config.EventBudget = 256;
        config.ReadChunkSize = 1024;
        config.ReadBatchBytes = 4096;
        config.MaxIov = 4;
        config.MaxPendingBufferBytes = 256 * 1024;

        TqLinuxRelayWorker worker(config);
        assert(worker.StartForTest());

        constexpr int kRelayCount = 64;
        std::vector<std::array<int, 2>> fds(kRelayCount);
        std::vector<TqLinuxRelayRegistrationResult> registrations;
        registrations.reserve(kRelayCount);

        for (int i = 0; i < kRelayCount; ++i) {
            fds[i] = std::array<int, 2>{-1, -1};
            assert(::socketpair(AF_UNIX, SOCK_STREAM, 0, fds[i].data()) == 0);

            TqLinuxRelayRegistration registration{};
            registration.TcpFd = fds[i][0];
            registration.Stream = nullptr;
            registration.Handle = nullptr;
            registration.EnableQuicSends = false;

            const auto result = worker.RegisterRelayWithId(registration);
            assert(result.Ok);
            registrations.push_back(result);
        }

        const char payload[] = "relay-index-hit";
        assert(::write(fds[kRelayCount - 1][1], payload, sizeof(payload)) ==
               static_cast<ssize_t>(sizeof(payload)));
        assert(worker.DispatchTcpEventsForTest(
            registrations[kRelayCount - 1].RelayId,
            EPOLLIN));

        const TqLinuxRelayWorkerSnapshot snapshot = worker.Snapshot();
        assert(snapshot.ActiveRelays == kRelayCount);
        assert(snapshot.TcpReadBytes >= sizeof(payload));

        for (int i = 0; i < kRelayCount; ++i) {
            worker.UnregisterRelay(registrations[i].RelayId);
            ::close(fds[i][1]);
        }
        worker.Stop();
    }
```

- [ ] **Step 2: Run the existing test target as a baseline**

Run:

```bash
rtk cmake --build build --target tcpquic_linux_relay_worker_io_test -j$(nproc)
rtk build/src/tcpquic_linux_relay_worker_io_test
```

Expected: PASS before and after implementation. This test protects behavior before refactoring lookup internals; the O(1) requirement is verified by code inspection and the `RelayLock`/scan removal checks in later tasks.

### Task 2: Add active and retired relay id maps

**Files:**
- Modify: `src/tunnel/linux_relay_worker.h`
- Modify: `src/tunnel/linux_relay_worker.cpp`

- [ ] **Step 1: Add header include and fields**

In `src/tunnel/linux_relay_worker.h`, add:

```cpp
#include <unordered_map>
```

Near `Relays` and `RetiredRelays`, change the fields to:

```cpp
    mutable std::mutex RelayLock;
    std::deque<std::shared_ptr<RelayState>> Relays;
    std::deque<std::shared_ptr<RelayState>> RetiredRelays;
    std::unordered_map<uint64_t, std::shared_ptr<RelayState>> RelaysById;
    std::unordered_map<uint64_t, std::shared_ptr<RelayState>> RetiredRelaysById;
    uint64_t NextRelayId{1};
```

- [ ] **Step 2: Update `RegisterRelayWithId()` insertion**

Inside the existing `RelayLock` block in `RegisterRelayWithId()`, update the insertion:

```cpp
    {
        std::lock_guard<std::mutex> guard(RelayLock);
        relay->Id = NextRelayId++;
        event.data.u64 = relay->Id;
        Relays.push_back(relay);
        RelaysById.emplace(relay->Id, relay);
        result.Ok = true;
        result.RelayId = relay->Id;
    }
```

- [ ] **Step 3: Update register failure rollback**

In both rollback blocks that erase from `Relays`, add `RelaysById.erase(relay->Id);`:

```cpp
            if ((*it)->Id == relay->Id) {
                Relays.erase(it);
                break;
            }
        }
        RelaysById.erase(relay->Id);
```

- [ ] **Step 4: Run the Linux relay IO test**

Run:

```bash
rtk cmake --build build --target tcpquic_linux_relay_worker_io_test -j$(nproc)
rtk build/src/tcpquic_linux_relay_worker_io_test
```

Expected: PASS.

### Task 3: Convert lookup and unregister to use O(1) maps

**Files:**
- Modify: `src/tunnel/linux_relay_worker.cpp`

- [ ] **Step 1: Replace `FindRelayById()` body**

Replace the function with:

```cpp
std::shared_ptr<TqLinuxRelayWorker::RelayState> TqLinuxRelayWorker::FindRelayById(uint64_t relayId) {
    std::lock_guard<std::mutex> guard(RelayLock);
    auto active = RelaysById.find(relayId);
    if (active != RelaysById.end()) {
        return active->second;
    }
    auto retired = RetiredRelaysById.find(relayId);
    if (retired != RetiredRelaysById.end()) {
        return retired->second;
    }
    return nullptr;
}
```

- [ ] **Step 2: Update `UnregisterRelay()` removal**

Replace the first `RelayLock` block in `UnregisterRelay()` with:

```cpp
    {
        std::lock_guard<std::mutex> guard(RelayLock);
        auto found = RelaysById.find(relayId);
        if (found != RelaysById.end()) {
            removed = found->second;
            RelaysById.erase(found);
            for (auto it = Relays.begin(); it != Relays.end(); ++it) {
                if ((*it)->Id == relayId) {
                    Relays.erase(it);
                    break;
                }
            }
        }
    }
```

- [ ] **Step 3: Update retired insertion**

At the end of `UnregisterRelay()`, update retired insertion:

```cpp
    {
        std::lock_guard<std::mutex> guard(RelayLock);
        RetiredRelays.push_back(removed);
        RetiredRelaysById.emplace(removed->Id, removed);
    }
```

- [ ] **Step 4: Update `PurgeRetiredRelaysIfIdle()`**

Replace the erase/remove block with explicit erase so the map stays consistent:

```cpp
    std::lock_guard<std::mutex> guard(RelayLock);
    for (auto it = RetiredRelays.begin(); it != RetiredRelays.end();) {
        const auto& relay = *it;
        if (relay == nullptr ||
            (relay->OutstandingQuicSends == 0 && relay->StreamBinding == nullptr)) {
            if (relay != nullptr) {
                RetiredRelaysById.erase(relay->Id);
            }
            it = RetiredRelays.erase(it);
        } else {
            ++it;
        }
    }
```

- [ ] **Step 5: Run focused tests**

Run:

```bash
rtk cmake --build build --target tcpquic_linux_relay_worker_queue_test tcpquic_linux_relay_worker_io_test -j$(nproc)
rtk build/src/tcpquic_linux_relay_worker_queue_test
rtk build/src/tcpquic_linux_relay_worker_io_test
```

Expected: both PASS.

### Task 4: Add stage 1 diagnostic assertions for map consistency

**Files:**
- Modify: `src/tunnel/linux_relay_worker.cpp`

- [ ] **Step 1: Add debug-only consistency helper**

Add this test-only public helper declaration in `linux_relay_worker.h` inside the existing `#if defined(TQ_UNIT_TESTING)` public area:

```cpp
    bool RelayIndexesConsistentForTest() const;
```

Implement in `linux_relay_worker.cpp`:

```cpp
bool TqLinuxRelayWorker::RelayIndexesConsistentForTest() const {
    std::lock_guard<std::mutex> guard(RelayLock);
    if (Relays.size() != RelaysById.size() ||
        RetiredRelays.size() != RetiredRelaysById.size()) {
        return false;
    }
    for (const auto& relay : Relays) {
        if (relay == nullptr) {
            return false;
        }
        auto found = RelaysById.find(relay->Id);
        if (found == RelaysById.end() || found->second.get() != relay.get()) {
            return false;
        }
    }
    for (const auto& relay : RetiredRelays) {
        if (relay == nullptr) {
            return false;
        }
        auto found = RetiredRelaysById.find(relay->Id);
        if (found == RetiredRelaysById.end() || found->second.get() != relay.get()) {
            return false;
        }
    }
    return true;
}
```

- [ ] **Step 2: Assert consistency in the new multi-relay test**

After registration loop and after unregister loop in `linux_relay_worker_io_test.cpp`, add:

```cpp
        assert(worker.RelayIndexesConsistentForTest());
```

If the helper is private, expose it only under `TQ_UNIT_TESTING` public section.

- [ ] **Step 3: Run focused tests**

Run:

```bash
rtk cmake --build build --target tcpquic_linux_relay_worker_io_test -j$(nproc)
rtk build/src/tcpquic_linux_relay_worker_io_test
```

Expected: PASS.

- [ ] **Step 4: Commit stage 1**

Run:

```bash
git add src/tunnel/linux_relay_worker.h src/tunnel/linux_relay_worker.cpp src/unittest/linux_relay_worker_io_test.cpp
git commit -m "perf: add O(1) Linux relay lookup index"
```

## Phase 2: Eventize register/unregister/snapshot and remove `RelayLock`

### Task 5: Add control event types and command structs

**Files:**
- Modify: `src/tunnel/linux_relay_event_queue.h`
- Modify: `src/tunnel/linux_relay_worker.h`

- [ ] **Step 1: Add event types**

In `TqLinuxRelayEventType`, add:

```cpp
    RegisterRelay,
    UnregisterRelay,
    Snapshot,
```

Keep the existing `Shutdown` value for compatibility until all call sites are renamed or confirmed.

- [ ] **Step 2: Add control payload**

In `TqLinuxRelayEvent`, add:

```cpp
    void* Control{nullptr};
```

- [ ] **Step 3: Add command structs**

In `linux_relay_worker.h`, include:

```cpp
#include <condition_variable>
```

Add private nested structs before method declarations:

```cpp
    struct RegisterRelayCommand {
        TqLinuxRelayRegistration Registration;
        TqLinuxRelayRegistrationResult Result;
        std::mutex Mutex;
        std::condition_variable Cv;
        bool Done{false};
    };

    struct UnregisterRelayCommand {
        uint64_t RelayId{0};
        std::mutex Mutex;
        std::condition_variable Cv;
        bool Done{false};
    };

    struct SnapshotCommand {
        TqLinuxRelayWorkerSnapshot Result;
        std::mutex Mutex;
        std::condition_variable Cv;
        bool Done{false};
    };
```

- [ ] **Step 4: Add helper declarations**

Add private helper declarations:

```cpp
    bool IsWorkerThread() const;
    TqLinuxRelayRegistrationResult RegisterRelayWithIdLocal(
        const TqLinuxRelayRegistration& registration);
    void UnregisterRelayLocal(uint64_t relayId);
    TqLinuxRelayWorkerSnapshot SnapshotLocal() const;
    void CompleteRegisterCommand(RegisterRelayCommand* command, TqLinuxRelayRegistrationResult result);
    void CompleteUnregisterCommand(UnregisterRelayCommand* command);
    void CompleteSnapshotCommand(SnapshotCommand* command, TqLinuxRelayWorkerSnapshot snapshot);
```

Add a worker thread id field:

```cpp
    std::thread::id WorkerThreadId;
```

### Task 6: Split current register/unregister/snapshot into local helpers

**Files:**
- Modify: `src/tunnel/linux_relay_worker.cpp`

- [ ] **Step 1: Capture worker thread id**

At the beginning of `Run()` add:

```cpp
    WorkerThreadId = std::this_thread::get_id();
```

Implement:

```cpp
bool TqLinuxRelayWorker::IsWorkerThread() const {
    return std::this_thread::get_id() == WorkerThreadId;
}
```

- [ ] **Step 2: Rename current register body to local helper**

Move the body of `RegisterRelayWithId()` into:

```cpp
TqLinuxRelayRegistrationResult TqLinuxRelayWorker::RegisterRelayWithIdLocal(
    const TqLinuxRelayRegistration& registration) {
    // existing RegisterRelayWithId body
}
```

Initially leave `RelayLock` in the local helper. This keeps behavior identical while introducing the call boundary.

Make public `RegisterRelayWithId()` call the local helper for now:

```cpp
TqLinuxRelayRegistrationResult TqLinuxRelayWorker::RegisterRelayWithId(
    const TqLinuxRelayRegistration& registration) {
    return RegisterRelayWithIdLocal(registration);
}
```

- [ ] **Step 3: Rename current unregister body to local helper**

Move the body of `UnregisterRelay()` into:

```cpp
void TqLinuxRelayWorker::UnregisterRelayLocal(uint64_t relayId) {
    // existing UnregisterRelay body
}
```

Make public `UnregisterRelay()` call the local helper for now:

```cpp
void TqLinuxRelayWorker::UnregisterRelay(uint64_t relayId) {
    UnregisterRelayLocal(relayId);
}
```

- [ ] **Step 4: Rename current snapshot body to local helper**

Move `Snapshot()` body into:

```cpp
TqLinuxRelayWorkerSnapshot TqLinuxRelayWorker::SnapshotLocal() const {
    // existing Snapshot body
}
```

Make public `Snapshot()` call the local helper for now:

```cpp
TqLinuxRelayWorkerSnapshot TqLinuxRelayWorker::Snapshot() const {
    return SnapshotLocal();
}
```

- [ ] **Step 5: Run focused tests**

Run:

```bash
rtk cmake --build build --target tcpquic_linux_relay_worker_queue_test tcpquic_linux_relay_worker_io_test -j$(nproc)
rtk build/src/tcpquic_linux_relay_worker_queue_test
rtk build/src/tcpquic_linux_relay_worker_io_test
```

Expected: both PASS.

### Task 7: Implement synchronous eventized register/unregister/snapshot wrappers

**Files:**
- Modify: `src/tunnel/linux_relay_worker.cpp`

- [ ] **Step 1: Add command completion helpers**

Add:

```cpp
void TqLinuxRelayWorker::CompleteRegisterCommand(
    RegisterRelayCommand* command,
    TqLinuxRelayRegistrationResult result) {
    if (command == nullptr) {
        return;
    }
    {
        std::lock_guard<std::mutex> guard(command->Mutex);
        command->Result = result;
        command->Done = true;
    }
    command->Cv.notify_one();
}

void TqLinuxRelayWorker::CompleteUnregisterCommand(UnregisterRelayCommand* command) {
    if (command == nullptr) {
        return;
    }
    {
        std::lock_guard<std::mutex> guard(command->Mutex);
        command->Done = true;
    }
    command->Cv.notify_one();
}

void TqLinuxRelayWorker::CompleteSnapshotCommand(
    SnapshotCommand* command,
    TqLinuxRelayWorkerSnapshot snapshot) {
    if (command == nullptr) {
        return;
    }
    {
        std::lock_guard<std::mutex> guard(command->Mutex);
        command->Result = snapshot;
        command->Done = true;
    }
    command->Cv.notify_one();
}
```

- [ ] **Step 2: Add event handling in `DrainEvents()`**

In the switch:

```cpp
        case TqLinuxRelayEventType::RegisterRelay: {
            auto* command = static_cast<RegisterRelayCommand*>(event.Control);
            CompleteRegisterCommand(
                command,
                command != nullptr
                    ? RegisterRelayWithIdLocal(command->Registration)
                    : TqLinuxRelayRegistrationResult{});
            break;
        }
        case TqLinuxRelayEventType::UnregisterRelay: {
            auto* command = static_cast<UnregisterRelayCommand*>(event.Control);
            if (command != nullptr) {
                UnregisterRelayLocal(command->RelayId);
            }
            CompleteUnregisterCommand(command);
            break;
        }
        case TqLinuxRelayEventType::Snapshot: {
            auto* command = static_cast<SnapshotCommand*>(event.Control);
            CompleteSnapshotCommand(command, SnapshotLocal());
            break;
        }
```

Leave existing `Shutdown` behavior unchanged for call sites that still use it.

- [ ] **Step 3: Eventize public `RegisterRelayWithId()`**

Replace public wrapper:

```cpp
TqLinuxRelayRegistrationResult TqLinuxRelayWorker::RegisterRelayWithId(
    const TqLinuxRelayRegistration& registration) {
    if (!Running.load(std::memory_order_acquire) || IsWorkerThread()) {
        return RegisterRelayWithIdLocal(registration);
    }

    RegisterRelayCommand command{};
    command.Registration = registration;

    TqLinuxRelayEvent event{};
    event.Type = TqLinuxRelayEventType::RegisterRelay;
    event.Control = &command;
    if (!Enqueue(std::move(event))) {
        return {};
    }

    std::unique_lock<std::mutex> lock(command.Mutex);
    command.Cv.wait(lock, [&command] { return command.Done; });
    return command.Result;
}
```

- [ ] **Step 4: Eventize public `UnregisterRelay()`**

Replace public wrapper:

```cpp
void TqLinuxRelayWorker::UnregisterRelay(uint64_t relayId) {
    if (!Running.load(std::memory_order_acquire) || IsWorkerThread()) {
        UnregisterRelayLocal(relayId);
        return;
    }

    UnregisterRelayCommand command{};
    command.RelayId = relayId;

    TqLinuxRelayEvent event{};
    event.Type = TqLinuxRelayEventType::UnregisterRelay;
    event.Control = &command;
    if (!Enqueue(std::move(event))) {
        return;
    }

    std::unique_lock<std::mutex> lock(command.Mutex);
    command.Cv.wait(lock, [&command] { return command.Done; });
}
```

- [ ] **Step 5: Eventize public `Snapshot()`**

Replace public wrapper:

```cpp
TqLinuxRelayWorkerSnapshot TqLinuxRelayWorker::Snapshot() const {
    if (!Running.load(std::memory_order_acquire) || IsWorkerThread()) {
        return SnapshotLocal();
    }

    SnapshotCommand command{};
    TqLinuxRelayEvent event{};
    event.Type = TqLinuxRelayEventType::Snapshot;
    event.Control = &command;
    if (!const_cast<TqLinuxRelayWorker*>(this)->Enqueue(std::move(event))) {
        return {};
    }

    std::unique_lock<std::mutex> lock(command.Mutex);
    command.Cv.wait(lock, [&command] { return command.Done; });
    return command.Result;
}
```

- [ ] **Step 6: Run focused tests**

Run:

```bash
rtk cmake --build build --target tcpquic_linux_relay_worker_queue_test tcpquic_linux_relay_worker_io_test -j$(nproc)
rtk build/src/tcpquic_linux_relay_worker_queue_test
rtk build/src/tcpquic_linux_relay_worker_io_test
```

Expected: both PASS.

### Task 8: Remove `RelayLock` from worker-local container access

**Files:**
- Modify: `src/tunnel/linux_relay_worker.h`
- Modify: `src/tunnel/linux_relay_worker.cpp`

- [ ] **Step 1: Replace lookup with local O(1) helper**

Add helper:

```cpp
std::shared_ptr<TqLinuxRelayWorker::RelayState> TqLinuxRelayWorker::FindRelayById(uint64_t relayId) {
    auto active = RelaysById.find(relayId);
    if (active != RelaysById.end()) {
        return active->second;
    }
    auto retired = RetiredRelaysById.find(relayId);
    if (retired != RetiredRelaysById.end()) {
        return retired->second;
    }
    return nullptr;
}
```

Do not lock. At this point all non-worker callers must reach lookup through events or worker-local direct path.

- [ ] **Step 2: Remove lock guards in local register/unregister/purge/snapshot**

In `RegisterRelayWithIdLocal()`, remove `std::lock_guard<std::mutex> guard(RelayLock);` blocks and keep the body operations directly.

In `UnregisterRelayLocal()`, replace the first locked block with direct map lookup and erase.

In `PurgeRetiredRelaysIfIdle()`, remove the lock guard.

In `SnapshotLocal()`, remove the lock guard around relay traversal.

In the post-event retry block in `DrainEvents()`, replace the locked copy:

```cpp
    std::vector<std::shared_ptr<RelayState>> activeRelays(Relays.begin(), Relays.end());
```

- [ ] **Step 3: Remove `RelayLock` field**

Delete from `linux_relay_worker.h`:

```cpp
    mutable std::mutex RelayLock;
```

Keep `<mutex>` include because other locks remain.

- [ ] **Step 4: Update `RelayIndexesConsistentForTest()`**

If the helper remains, remove the lock guard:

```cpp
bool TqLinuxRelayWorker::RelayIndexesConsistentForTest() const {
    if (Relays.size() != RelaysById.size() ||
        RetiredRelays.size() != RetiredRelaysById.size()) {
        return false;
    }
    ...
}
```

Only call it from worker-local tests or after synchronous eventized operations have completed.

- [ ] **Step 5: Verify no `RelayLock` references remain**

Run:

```bash
rtk rg -n "RelayLock|std::lock_guard<std::mutex> relayGuard" src/tunnel/linux_relay_worker.*
```

Expected: no output.

- [ ] **Step 6: Run focused tests**

Run:

```bash
rtk cmake --build build --target tcpquic_linux_relay_worker_queue_test tcpquic_linux_relay_worker_io_test -j$(nproc)
rtk build/src/tcpquic_linux_relay_worker_queue_test
rtk build/src/tcpquic_linux_relay_worker_io_test
```

Expected: both PASS.

### Task 9: Add eventized lifecycle regression tests

**Files:**
- Modify: `src/unittest/linux_relay_worker_io_test.cpp`

- [ ] **Step 1: Add concurrent eventized register/unregister test**

Replace the existing concurrent register/unregister stress block with:

```cpp
    {
        TqLinuxRelayWorkerConfig config{};
        config.EventBudget = 256;
        config.ReadChunkSize = 1024;
        config.ReadBatchBytes = 4096;
        config.MaxIov = 4;
        config.MaxPendingBufferBytes = 256 * 1024;

        TqLinuxRelayWorker worker(config);
        assert(worker.Start());

        constexpr int kThreadCount = 8;
        constexpr int kIterations = 8;
        std::vector<std::thread> threads;
        threads.reserve(kThreadCount);

        for (int t = 0; t < kThreadCount; ++t) {
            threads.emplace_back([&worker]() {
                for (int i = 0; i < kIterations; ++i) {
                    int fds[2]{-1, -1};
                    assert(::socketpair(AF_UNIX, SOCK_STREAM, 0, fds) == 0);

                    TqLinuxRelayRegistration registration{};
                    registration.TcpFd = fds[0];
                    registration.Stream = nullptr;
                    registration.Handle = nullptr;
                    registration.EnableQuicSends = false;

                    const auto result = worker.RegisterRelayWithId(registration);
                    assert(result.Ok);

                    const TqLinuxRelayWorkerSnapshot during = worker.Snapshot();
                    assert(during.ActiveRelays > 0);

                    worker.UnregisterRelay(result.RelayId);
                    ::close(fds[1]);
                }
            });
        }

        for (auto& thread : threads) {
            thread.join();
        }

        const TqLinuxRelayWorkerSnapshot finalSnapshot = worker.Snapshot();
        assert(finalSnapshot.ActiveRelays == 0);
        assert(worker.RelayIndexesConsistentForTest());
        worker.Stop();
    }
```

- [ ] **Step 2: Run the test repeatedly**

Run:

```bash
rtk cmake --build build --target tcpquic_linux_relay_worker_io_test -j$(nproc)
for i in $(seq 1 20); do rtk build/src/tcpquic_linux_relay_worker_io_test || exit 1; done
```

Expected: all 20 iterations PASS with no hang.

### Task 10: Update documentation

**Files:**
- Modify: `docs/relay_linux.md`

- [ ] **Step 1: Update the lock section**

In `docs/relay_linux.md` under `## 锁和同步开销`, update the `TqLinuxRelayWorker::RelayLock` row after implementation:

```markdown
| relay 容器索引 | `Relays` / `RetiredRelays` / `RelaysById` / `RetiredRelaysById` | TCP/QUIC event lookup、register、unregister、snapshot | 阶段一通过 `RelaysById` 把 relay id 查找从线性扫描降为 O(1)；阶段二把 register/unregister/snapshot 事件化后，relay 容器只由 worker 线程访问，不再需要 `RelayLock`。热路径剩余成本主要是 event queue 入队和 worker-local map lookup。 | 后续如果 map lookup 仍是热点，可演进为 slot/generation 或 epoll data slot 指针。 |
```

- [ ] **Step 2: Add a short implementation note**

Add:

```markdown
当前 Linux relay 查找优化分两阶段实现：第一阶段保留 worker 锁但增加 relay id map；第二阶段把容器读写收敛到 worker event loop。这样可以先降低 O(N) 扫描成本，再移除跨线程容器共享。
```

- [ ] **Step 3: Run markdown whitespace check**

Run:

```bash
rtk git diff --check -- docs/relay_linux.md docs/superpowers/specs/2026-06-27-linux-relay-relaylock-eventized-design.md docs/superpowers/plans/2026-06-27-linux-relay-relaylock-eventized.md
```

Expected: no output.

### Task 11: Full verification and commit

**Files:**
- Verify all modified files.

- [ ] **Step 1: Run Linux relay tests**

Run:

```bash
rtk cmake --build build --target tcpquic_linux_relay_worker_queue_test tcpquic_linux_relay_worker_io_test -j$(nproc)
rtk build/src/tcpquic_linux_relay_worker_queue_test
rtk build/src/tcpquic_linux_relay_worker_io_test
```

Expected: both PASS.

- [ ] **Step 2: Run full available unit test target if configured**

If the build directory has a CTest configuration, run:

```bash
rtk ctest --test-dir build --output-on-failure
```

Expected: all configured tests PASS. If this repository build does not use CTest, record that focused Linux relay tests were run instead.

- [ ] **Step 3: Check references**

Run:

```bash
rtk rg -n "RelayLock" src/tunnel/linux_relay_worker.*
rtk rg -n "FindRelayById\\(" src/tunnel/linux_relay_worker.cpp
```

Expected:

```text
RelayLock command: no output
FindRelayById command: function definition plus call sites only
```

- [ ] **Step 4: Check diff**

Run:

```bash
rtk git diff --check
rtk git diff --stat
```

Expected: no whitespace errors; stat includes only Linux relay worker, Linux relay tests, event queue header, and docs.

- [ ] **Step 5: Commit**

Run:

```bash
git add src/tunnel/linux_relay_event_queue.h src/tunnel/linux_relay_worker.h src/tunnel/linux_relay_worker.cpp src/unittest/linux_relay_worker_io_test.cpp src/unittest/linux_relay_worker_queue_test.cpp docs/relay_linux.md docs/superpowers/specs/2026-06-27-linux-relay-relaylock-eventized-design.md docs/superpowers/plans/2026-06-27-linux-relay-relaylock-eventized.md
git commit -m "perf: eventize Linux relay lookup ownership"
```

## Notes for implementers

- Do not remove `StreamRelayBinding` atomics. They protect MsQuic callback lifetime and remain necessary after `RelayLock` is removed.
- Do not make `RegisterRelayWithId()` asynchronous from the caller perspective. The caller must not observe a successful relay start until epoll registration and callback binding are complete.
- Do not use raw `RelayState*` in epoll or queued events in this plan. Keep `shared_ptr` maps first; slot/generation can be a later optimization.
- If event queue full makes register/unregister fail, return failure for register and preserve conservative stop behavior for unregister. Do not block forever waiting for a command that was not enqueued.
- Avoid holding a command completion mutex while calling `RegisterRelayWithIdLocal()`, `UnregisterRelayLocal()`, `SnapshotLocal()`, MsQuic, `epoll_ctl`, or socket close.
