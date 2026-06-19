# Callback-Driven Client Reconnect Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Remove per-peer client reconnect threads, drive QUIC reconnect from MsQuic callbacks, and use the shared client ingress reactor for fixed-delay retry after synchronous start failures.

**Architecture:** `QuicClientSession` no longer owns a reconnect thread. Async disconnects restart the affected slot after `SHUTDOWN_COMPLETE`; synchronous start failures are scheduled through an injected delayed-task scheduler implemented by `TqClientIngressReactor`. Client ingress listen ports remain closed unless the corresponding peer has at least one connected QUIC connection; warmup is removed and speedtest uses the normal ingress path.

**Tech Stack:** C++17, MsQuic callback API, existing `TqClientIngressReactor`, CMake, assert-style unit tests.

---

## File Structure

Modify:

- `src/ingress/client_ingress_reactor.h`  
  Add delayed task queue API and private processing helpers.
- `src/ingress/client_ingress_reactor.cpp`  
  Process due delayed tasks in the ingress worker loop and compute reactor wait timeout from the nearest due task.
- `src/protocol/quic_session.h`  
  Remove reconnect thread state and add `DelayedTaskScheduler`.
- `src/protocol/quic_session.cpp`  
  Remove reconnect loop, restart slots from `SHUTDOWN_COMPLETE`, schedule fixed-delay retries for synchronous start failures, and simplify `EnsureConnected()`.
- `src/main.cpp`  
  Wire `QuicClientSession` delayed scheduler to the relevant `TqClientIngressReactor`, delete warmup branch, and route speedtest through ingress-only startup.
- `src/runtime/speed_test.*`  
  Remove direct `QuicClientSession` client speedtest entry point and add ingress-driven HTTP CONNECT speedtest helper code.
- `src/runtime/warmup.*`  
  Delete warmup implementation after references are removed.
- `src/CMakeLists.txt`  
  Remove warmup sources/tests and update speedtest test sources.
- `src/unittest/client_ingress_reactor_test.cpp`  
  Add delayed task tests.
- `src/unittest/client_tunnel_open_test.cpp` / `src/unittest/tcp_tunnel_test.cpp`  
  Update `QuicClientSession` API surface tests.
- `src/unittest/speed_test_test.cpp`  
  Remove direct `QuicClientSession` stubs for speedtest client path; add ingress-path expectations.
- `docs/thread-model_cn.md`  
  Remove client reconnect thread and warmup descriptions; document callback-driven reconnect.

Create:

- `src/unittest/quic_session_reconnect_test.cpp`
  Cover callback-driven slot restart and fixed-delay retry scheduling.

---

### Task 1: Add Delayed Tasks to Client Ingress Reactor

**Files:**
- Modify: `src/ingress/client_ingress_reactor.h`
- Modify: `src/ingress/client_ingress_reactor.cpp`
- Modify: `src/unittest/client_ingress_reactor_test.cpp`

- [ ] **Step 1: Add failing delayed task test**

In `src/unittest/client_ingress_reactor_test.cpp`, add a test that starts the reactor, queues a delayed task, and verifies it does not run before the delay but does run without external socket events.

```cpp
static int TestDelayedTaskRunsAfterDelay() {
    TqClientIngressReactor reactor;
    if (!reactor.Start()) {
        return 920;
    }

    std::atomic<int> calls{0};
    const auto start = std::chrono::steady_clock::now();
    if (!reactor.EnqueueDelayed(std::chrono::milliseconds(100), [&]() {
            const auto elapsed = std::chrono::steady_clock::now() - start;
            if (elapsed >= std::chrono::milliseconds(80)) {
                calls.fetch_add(1, std::memory_order_relaxed);
            } else {
                calls.fetch_add(100, std::memory_order_relaxed);
            }
        })) {
        reactor.Stop();
        return 921;
    }

    std::this_thread::sleep_for(std::chrono::milliseconds(40));
    if (calls.load(std::memory_order_relaxed) != 0) {
        reactor.Stop();
        return 922;
    }

    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (calls.load(std::memory_order_relaxed) == 0 &&
           std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    const int observed = calls.load(std::memory_order_relaxed);
    reactor.Stop();
    return observed == 1 ? 0 : 923;
}
```

Call it from `main()`:

```cpp
    if (int rc = TestDelayedTaskRunsAfterDelay()) return rc;
```

- [ ] **Step 2: Run the failing test**

```bash
rtk cmake --build build-regression --target tcpquic_client_ingress_reactor_test -j4
rtk ./build-regression/bin/Release/tcpquic_client_ingress_reactor_test
```

Expected: build fails because `TqClientIngressReactor::EnqueueDelayed` is not defined.

- [ ] **Step 3: Add delayed task declarations**

In `src/ingress/client_ingress_reactor.h`, add `<chrono>` and `<vector>` includes:

```cpp
#include <chrono>
#include <vector>
```

Add public method:

```cpp
bool EnqueueDelayed(std::chrono::milliseconds delay, std::function<void()> task);
```

Add private type and helpers:

```cpp
struct DelayedTask {
    std::chrono::steady_clock::time_point Due;
    uint64_t Order{0};
    std::function<void()> Task;
};

void ProcessDueDelayedTasks();
int NextRunTimeoutMsLocked() const;
```

Add members:

```cpp
std::vector<DelayedTask> DelayedTasks;
uint64_t NextDelayedTaskOrder{1};
```

- [ ] **Step 4: Implement delayed task queue**

In `src/ingress/client_ingress_reactor.cpp`, add:

```cpp
bool TqClientIngressReactor::EnqueueDelayed(
    std::chrono::milliseconds delay,
    std::function<void()> task) {
    if (!task) {
        return false;
    }
    if (delay < std::chrono::milliseconds(0)) {
        delay = std::chrono::milliseconds(0);
    }
    {
        std::lock_guard<std::mutex> lock(Mutex);
        if (!Running.load(std::memory_order_acquire)) {
            return false;
        }
        DelayedTasks.push_back(DelayedTask{
            std::chrono::steady_clock::now() + delay,
            NextDelayedTaskOrder++,
            std::move(task)});
    }
    (void)Reactor.Wake();
    return true;
}

void TqClientIngressReactor::ProcessDueDelayedTasks() {
    std::vector<std::function<void()>> tasks;
    const auto now = std::chrono::steady_clock::now();
    {
        std::lock_guard<std::mutex> lock(Mutex);
        auto out = DelayedTasks.begin();
        for (auto it = DelayedTasks.begin(); it != DelayedTasks.end(); ++it) {
            if (it->Due <= now) {
                tasks.push_back(std::move(it->Task));
            } else {
                if (out != it) {
                    *out = std::move(*it);
                }
                ++out;
            }
        }
        DelayedTasks.erase(out, DelayedTasks.end());
    }
    for (auto& task : tasks) {
        if (task) {
            task();
        }
    }
}

int TqClientIngressReactor::NextRunTimeoutMsLocked() const {
    constexpr int kMaxTimeoutMs = 50;
    if (DelayedTasks.empty()) {
        return kMaxTimeoutMs;
    }
    const auto now = std::chrono::steady_clock::now();
    auto earliest = DelayedTasks.front().Due;
    for (const auto& task : DelayedTasks) {
        if (task.Due < earliest ||
            (task.Due == earliest && task.Order < DelayedTasks.front().Order)) {
            earliest = task.Due;
        }
    }
    if (earliest <= now) {
        return 0;
    }
    const auto delay = std::chrono::duration_cast<std::chrono::milliseconds>(earliest - now);
    if (delay.count() <= 0) {
        return 0;
    }
    return static_cast<int>(std::min<int64_t>(kMaxTimeoutMs, delay.count()));
}
```

Change `Run()` to process due tasks and use dynamic timeout:

```cpp
void TqClientIngressReactor::Run() {
    while (Running.load(std::memory_order_acquire)) {
        ProcessPendingTasks();
        ProcessDueDelayedTasks();
        int timeoutMs = 50;
        {
            std::lock_guard<std::mutex> lock(Mutex);
            timeoutMs = NextRunTimeoutMsLocked();
        }
        (void)Reactor.RunOnce(timeoutMs);
    }
}
```

In `Stop()`, clear delayed tasks alongside `PendingTasks`:

```cpp
PendingTasks.clear();
DelayedTasks.clear();
```

- [ ] **Step 5: Run client ingress reactor test**

```bash
rtk cmake --build build-regression --target tcpquic_client_ingress_reactor_test -j4
rtk ./build-regression/bin/Release/tcpquic_client_ingress_reactor_test
```

Expected: target builds and exits `0`.

- [ ] **Step 6: Commit**

```bash
rtk git add src/ingress/client_ingress_reactor.h src/ingress/client_ingress_reactor.cpp src/unittest/client_ingress_reactor_test.cpp
rtk git commit -m "feat(ingress): add delayed task scheduling"
```

---

### Task 2: Remove Per-Session Reconnect Thread API

**Files:**
- Modify: `src/protocol/quic_session.h`
- Modify: `src/protocol/quic_session.cpp`
- Modify: `src/unittest/tcp_tunnel_test.cpp`

- [ ] **Step 1: Update API surface test first**

In `src/unittest/tcp_tunnel_test.cpp`, update `TestQuicClientSessionReconnectApiSurface()` to assert the public delayed scheduler API exists:

```cpp
static int TestQuicClientSessionReconnectApiSurface() {
    using Handler = QuicClientSession::ConnectionStateHandler;
    using Scheduler = QuicClientSession::DelayedTaskScheduler;
    static_assert(std::is_same<decltype(std::declval<const QuicClientSession&>().ConnectedConnectionCount()), uint32_t>::value,
        "ConnectedConnectionCount must remain available");
    static_assert(std::is_same<decltype(std::declval<QuicClientSession&>().EnsureAnyConnected()), bool>::value,
        "EnsureAnyConnected default overload must remain available");
    static_assert(std::is_same<decltype(std::declval<QuicClientSession&>().EnsureAnyConnected(std::chrono::milliseconds(1))), bool>::value,
        "EnsureAnyConnected timeout overload must remain available");
    (void)static_cast<void (QuicClientSession::*)(Handler)>(&QuicClientSession::SetConnectionStateHandler);
    (void)static_cast<void (QuicClientSession::*)(Scheduler)>(&QuicClientSession::SetDelayedTaskScheduler);
    return 0;
}
```

- [ ] **Step 2: Run failing build**

```bash
rtk cmake --build build-regression --target tcpquic_tunnel_test -j4
```

Expected: build fails because `DelayedTaskScheduler` and `SetDelayedTaskScheduler` do not exist.

- [ ] **Step 3: Add scheduler API and remove reconnect thread fields**

In `src/protocol/quic_session.h`, add the public type and method:

```cpp
using DelayedTaskScheduler =
    std::function<bool(std::chrono::milliseconds delay, std::function<void()> task)>;

void SetDelayedTaskScheduler(DelayedTaskScheduler scheduler);
```

In `ClientSharedState`, remove:

```cpp
std::thread ReconnectThread;
std::chrono::milliseconds ReconnectInterval{std::chrono::milliseconds(3000)};
std::shared_ptr<ClientReconnectGate> ReconnectGate;
```

Add:

```cpp
DelayedTaskScheduler Scheduler;
```

Remove `ClientReconnectGate` and declarations for:

```cpp
void StartReconnectLoop();
void StopReconnectLoop(const std::shared_ptr<ClientSharedState>& state);
static void RunReconnectLoop(
    std::shared_ptr<ClientSharedState> state,
    std::shared_ptr<ClientReconnectGate> gate);
```

Add declarations:

```cpp
bool StartSlot(size_t index);
void StartAllSlots();
void ScheduleStartRetry(size_t index);
static void RestartSlotAfterShutdownComplete(
    std::shared_ptr<ClientSharedState> state,
    size_t slotIndex);
```

- [ ] **Step 4: Remove reconnect loop implementation**

In `src/protocol/quic_session.cpp`, remove `StartReconnectLoop()`, `StopReconnectLoop()`, and `RunReconnectLoop()`.

Implement scheduler setter:

```cpp
void QuicClientSession::SetDelayedTaskScheduler(DelayedTaskScheduler scheduler) {
    std::lock_guard<std::mutex> guard(State->Lock);
    State->Scheduler = std::move(scheduler);
}
```

In `Start()`, replace:

```cpp
StartReconnectLoop();
return true;
```

with:

```cpp
StartAllSlots();
return true;
```

In `Stop(bool clearHandlers)`, remove:

```cpp
StopReconnectLoop(state);
```

and set stopping directly before shutting down slots:

```cpp
{
    std::lock_guard<std::mutex> guard(state->Lock);
    state->Started = false;
    state->Stopping = true;
    state->Scheduler = {};
}
state->StateChanged.notify_all();
```

Keep the existing slot shutdown/orphan cleanup after this state transition.

- [ ] **Step 5: Rename slot start helpers**

Rename `StartSlotLocked(size_t index)` to `StartSlot(size_t index)`.

Rename `StartAllDueSlots()` to `StartAllSlots()`:

```cpp
void QuicClientSession::StartAllSlots() {
    const size_t count = Config.QuicConnections;
    for (size_t i = 0; i < count; ++i) {
        {
            std::lock_guard<std::mutex> guard(State->Lock);
            if (!State->Started || State->Stopping) {
                return;
            }
        }
        (void)StartSlot(i);
    }
}
```

Replace all call sites of `StartSlotLocked` with `StartSlot`.

- [ ] **Step 6: Build tunnel test**

```bash
rtk cmake --build build-regression --target tcpquic_tunnel_test -j4
```

Expected: target builds successfully.

- [ ] **Step 7: Commit**

```bash
rtk git add src/protocol/quic_session.h src/protocol/quic_session.cpp src/unittest/tcp_tunnel_test.cpp
rtk git commit -m "refactor(quic): remove client reconnect thread state"
```

---

### Task 3: Callback-Driven Slot Restart and Fixed-Delay Retry

**Files:**
- Modify: `src/protocol/quic_session.cpp`
- Modify: `src/protocol/quic_session.h`
- Create: `src/unittest/quic_session_reconnect_test.cpp`
- Modify: `src/CMakeLists.txt`

- [ ] **Step 1: Add reconnect behavior test seam**

If existing tests cannot synthesize MsQuic callbacks cleanly, add `TQ_UNIT_TESTING` hooks to `QuicClientSession`:

In `src/protocol/quic_session.h`:

```cpp
#if defined(TQ_UNIT_TESTING)
public:
    struct ReconnectTestHooks {
        std::function<bool(size_t index)> StartSlotOverride;
    };
    void SetReconnectTestHooks(ReconnectTestHooks hooks);
#endif
```

In private state:

```cpp
#if defined(TQ_UNIT_TESTING)
ReconnectTestHooks TestHooks;
#endif
```

In `src/protocol/quic_session.cpp`:

```cpp
#if defined(TQ_UNIT_TESTING)
void QuicClientSession::SetReconnectTestHooks(ReconnectTestHooks hooks) {
    std::lock_guard<std::mutex> guard(State->Lock);
    TestHooks = std::move(hooks);
}
#endif
```

- [ ] **Step 2: Add failing fixed-delay retry test**

Create `src/unittest/quic_session_reconnect_test.cpp`:

```cpp
#include "quic_session.h"

#include <atomic>
#include <chrono>
#include <thread>

int main() {
    QuicClientSession session;
    std::atomic<int> scheduled{0};
    session.SetDelayedTaskScheduler([&](std::chrono::milliseconds delay, std::function<void()> task) {
        if (delay != std::chrono::milliseconds(100)) {
            return false;
        }
        ++scheduled;
        std::thread([task = std::move(task)]() mutable {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
            task();
        }).detach();
        return true;
    });
    (void)session;
    return scheduled.load() == 0 ? 0 : 1;
}
```

This compile-only test establishes the scheduler API before the behavior-specific restart assertions are introduced through the `TQ_UNIT_TESTING` hooks in this task.

- [ ] **Step 3: Add CMake target**

In `src/CMakeLists.txt`, add:

```cmake
add_executable(tcpquic_quic_session_reconnect_test
    unittest/quic_session_reconnect_test.cpp
    protocol/quic_session.cpp
    tunnel/relay.cpp
    tunnel/tunnel_registry.cpp
    config/tuning.cpp
    ${TCPQUIC_PLATFORM_SOURCES}
)
tcpquic_target_include_dirs(tcpquic_quic_session_reconnect_test)
target_link_libraries(tcpquic_quic_session_reconnect_test PRIVATE Threads::Threads inc warnings msquic logging base_link spdlog::spdlog)
target_compile_definitions(tcpquic_quic_session_reconnect_test PRIVATE TQ_UNIT_TESTING=1)
set_property(TARGET tcpquic_quic_session_reconnect_test PROPERTY FOLDER "tools")
set_property(TARGET tcpquic_quic_session_reconnect_test APPEND PROPERTY BUILD_RPATH "$ORIGIN")
```

Append it to `TCPQUIC_TEST_TARGETS`.

- [ ] **Step 4: Implement fixed-delay retry helper**

In `src/protocol/quic_session.cpp`, add:

```cpp
namespace {
constexpr auto TqClientStartRetryDelay = std::chrono::milliseconds(100);
}

void QuicClientSession::ScheduleStartRetry(size_t index) {
    DelayedTaskScheduler scheduler;
    std::weak_ptr<ClientSharedState> weakState;
    {
        std::lock_guard<std::mutex> guard(State->Lock);
        if (!State->Started || State->Stopping) {
            return;
        }
        scheduler = State->Scheduler;
        weakState = State;
    }
    if (!scheduler) {
        return;
    }
    (void)scheduler(TqClientStartRetryDelay, [this, weakState, index]() {
        auto state = weakState.lock();
        if (!state) {
            return;
        }
        {
            std::lock_guard<std::mutex> guard(state->Lock);
            if (!state->Started || state->Stopping || index >= state->Slots.size()) {
                return;
            }
        }
        (void)StartSlot(index);
    });
}
```

When `StartSlot(index)` hits a synchronous failure, call `ScheduleStartRetry(index)` before returning `false`.

- [ ] **Step 5: Add session pointer to connection context**

In `src/protocol/quic_session.h`, change `ClientConnContext` to carry the owning session pointer:

```cpp
struct ClientConnContext {
    QuicClientSession* Session{nullptr};
    std::shared_ptr<ClientSharedState> State;
    size_t SlotIndex{0};
};
```

In `StartSlot(size_t index)`, set it when creating a new context:

```cpp
newContext = new (std::nothrow) ClientConnContext{this, state, index};
```

In `src/protocol/quic_session.cpp`, implement the instance restart method:

```cpp
void QuicClientSession::RestartSlotAfterShutdownComplete(
    const std::shared_ptr<ClientSharedState>& state,
    size_t slotIndex) {
    {
        std::lock_guard<std::mutex> guard(state->Lock);
        if (!state->Started || state->Stopping || slotIndex >= state->Slots.size()) {
            return;
        }
    }
    (void)StartSlot(slotIndex);
}
```

- [ ] **Step 6: Update shutdown callback**

In `QUIC_CONNECTION_EVENT_SHUTDOWN_COMPLETE`, save the owning session before old context cleanup and restart after deleting the old context:

```cpp
QuicClientSession* session = slotContext->Session;

TqClientDebugLog("event-shutdown-complete", slotIndex, connection);
(void)TqAbortConnectionTunnels(connection);
{
    char line[256];
    std::snprintf(
        line,
        sizeof(line),
        "tcpquic-proxy: QUIC client disconnected peer=%s slot=%zu",
        TqFormatConnectionAddr(connection, QUIC_PARAM_CONN_REMOTE_ADDRESS).c_str(),
        slotIndex + 1);
    std::fprintf(stderr, "%s\n", line);
    TqTraceLogLine(line);
}
if (TqTraceEnabled()) {
    TqUnregisterClientTraceConnection(connection);
    TqTraceQuicDisconnected(
        connection,
        static_cast<uint32_t>(slotIndex + 1),
        "client");
}
if (state) {
    auto notification = QuicClientSession::OnSlotDisconnected(state, slotIndex, connection);
    {
        std::lock_guard<std::mutex> guard(state->Lock);
        if (slotIndex < state->Slots.size() &&
            state->Slots[slotIndex].Context == slotContext) {
            state->Slots[slotIndex].Context = nullptr;
        }
    }
    state->StateChanged.notify_all();
    QuicClientSession::NotifyConnectionStateChanged(std::move(notification));
}
connection->Close();
if (state) {
    QuicClientSession::DropOrphanedConnection(state, connection);
}
delete slotContext;
if (session != nullptr && state) {
    session->RestartSlotAfterShutdownComplete(state, slotIndex);
}
```

Expected behavior: restart occurs after old context is removed.

- [ ] **Step 7: Run reconnect and tunnel tests**

```bash
rtk cmake --build build-regression --target tcpquic_quic_session_reconnect_test tcpquic_tunnel_test -j4
rtk ./build-regression/bin/Release/tcpquic_quic_session_reconnect_test
rtk ./build-regression/bin/Release/tcpquic_tunnel_test
```

Expected: both executables exit `0`.

- [ ] **Step 8: Commit**

```bash
rtk git add src/protocol/quic_session.h src/protocol/quic_session.cpp src/unittest/quic_session_reconnect_test.cpp src/CMakeLists.txt
rtk git commit -m "feat(quic): restart client slots from callbacks"
```

---

### Task 4: Wire Scheduler from Client Runtime to Ingress Reactor

**Files:**
- Modify: `src/main.cpp`
- Modify: `src/unittest/client_ingress_reactor_test.cpp`
- Modify: `src/unittest/router_runtime_test.cpp`

- [ ] **Step 1: Wire single-peer scheduler**

In `RunSinglePeerClient`, after creating `runtime` and before relying on QUIC retry scheduling, set:

```cpp
quic.SetDelayedTaskScheduler([weakRuntime](std::chrono::milliseconds delay, std::function<void()> task) {
    auto runtime = weakRuntime.lock();
    if (!runtime) {
        return false;
    }
    return runtime->EnqueueDelayed(delay, std::move(task));
});
```

Expose a forwarding method on `TqSinglePeerClientRuntime`:

```cpp
bool EnqueueDelayed(std::chrono::milliseconds delay, std::function<void()> task) {
    return Ingress.EnqueueDelayed(delay, std::move(task));
}
```

- [ ] **Step 2: Wire multi-peer scheduler**

In the multi-peer `PeerRuntime` setup path, after `runtime->Ingress = Ingress.get();`, set:

```cpp
runtime->Quic->SetDelayedTaskScheduler([weakRuntime](std::chrono::milliseconds delay, std::function<void()> task) {
    auto runtime = weakRuntime.lock();
    if (!runtime || runtime->Ingress == nullptr) {
        return false;
    }
    return runtime->Ingress->EnqueueDelayed(delay, std::move(task));
});
```

- [ ] **Step 3: Verify peer listen gating remains connected-count based**

Confirm single-peer and multi-peer connection state handlers call `ApplyCurrentConnectionState(...)` and that function opens listen only when connected count is greater than zero. If the code opens listen when `AcceptingEnabled` is true but connected count is zero, change it to:

```cpp
const bool shouldOpen = AcceptingEnabled && Quic != nullptr && Quic->ConnectedConnectionCount() > 0;
if (shouldOpen && !ListenersOpen) {
    return OpenListeners(err);
}
if (!shouldOpen && ListenersOpen) {
    CloseListeners();
}
return true;
```

- [ ] **Step 4: Run client ingress tests**

```bash
rtk cmake --build build-regression --target tcpquic_client_ingress_reactor_test tcpquic_router_runtime_test -j4
rtk ./build-regression/bin/Release/tcpquic_client_ingress_reactor_test
rtk ./build-regression/bin/Release/tcpquic_router_runtime_test
```

Expected: both executables exit `0`.

- [ ] **Step 5: Commit**

```bash
rtk git add src/main.cpp src/unittest/client_ingress_reactor_test.cpp src/unittest/router_runtime_test.cpp
rtk git commit -m "feat(client): schedule reconnect retry on ingress reactor"
```

---

### Task 5: Delete Warmup

**Files:**
- Delete: `src/runtime/warmup.h`
- Delete: `src/runtime/warmup.cpp`
- Modify: `src/main.cpp`
- Modify: `src/CMakeLists.txt`
- Modify: `src/config/config.h`
- Modify: `src/config/config.cpp`
- Modify: `src/unittest/config_router_test.cpp`
- Modify: `docs/thread-model_cn.md`

- [ ] **Step 1: Remove warmup runtime branch**

In `src/main.cpp`, delete:

```cpp
if (cfg.WarmupMb > 0) {
    if (!quic.EnsureAnyConnected()) {
        std::fprintf(stderr, "tcpquic-proxy: failed to connect to QUIC peer for warmup\n");
        return 1;
    }
    if (!TqRunClientWarmup(quic, cfg)) {
        std::fprintf(stderr, "tcpquic-proxy: client warmup failed\n");
        return 1;
    }
}
```

Remove `#include "warmup.h"` from `src/main.cpp`.

- [ ] **Step 2: Remove config surface**

In `src/config/config.h`, remove warmup fields such as:

```cpp
uint32_t WarmupMb{0};
```

In `src/config/config.cpp`, remove CLI parsing and usage text for warmup options. Remove JSON parsing for warmup fields.

- [ ] **Step 3: Remove warmup files and CMake entries**

Delete:

```bash
rtk git rm src/runtime/warmup.h src/runtime/warmup.cpp
```

In `src/CMakeLists.txt`, remove `runtime/warmup.cpp` from all source lists.

- [ ] **Step 4: Update config tests**

In `src/unittest/config_router_test.cpp`, remove assertions that expect warmup CLI or JSON parsing. Add a rejection test for the removed CLI option:

```cpp
{
    TqConfig cfg;
    std::string err;
    const char* args[] = {"tcpquic-proxy", "client", "--peer", "127.0.0.1:14444",
        "--warmup-mb", "1", "--cert", "a.crt", "--key", "a.key", "--ca", "ca.crt"};
    if (ParseArgs(10, const_cast<char**>(args), cfg, err)) return 170;
    if (err.find("warmup") == std::string::npos) return 171;
}
```

Assert the parser returns failure and the error text contains `--warmup-mb`.

- [ ] **Step 5: Build config and proxy targets**

```bash
rtk cmake --build build-regression --target tcpquic-proxy tcpquic_config_router_test -j4
rtk ./build-regression/bin/Release/tcpquic_config_router_test
```

Expected: build succeeds and config test exits `0`.

- [ ] **Step 6: Commit**

```bash
rtk git add src/main.cpp src/CMakeLists.txt src/config/config.h src/config/config.cpp src/unittest/config_router_test.cpp docs/thread-model_cn.md
rtk git commit -m "refactor(client): remove warmup mode"
```

---

### Task 6: Route Speedtest Through Client Ingress

**Files:**
- Modify: `src/main.cpp`
- Modify: `src/runtime/speed_test.h`
- Modify: `src/runtime/speed_test.cpp`
- Modify: `src/unittest/speed_test_test.cpp`
- Modify: `docs/thread-model_cn.md`

- [ ] **Step 1: Remove direct client speedtest call**

In `RunSinglePeerClient`, delete:

```cpp
if (cfg.SpeedTestMode != TqSpeedTestMode::None) {
    const bool ok = TqRunClientSpeedTest(quic, cfg);
    quic.Stop();
    return ok ? 0 : 1;
}
```

Do not create a special `quicCfg` for speedtest:

```cpp
const TqConfig quicCfg = cfg;
```

- [ ] **Step 2: Remove direct speedtest client API**

In `src/runtime/speed_test.h`, remove:

```cpp
bool TqRunClientSpeedTest(QuicClientSession& quic, const TqConfig& cfg);
```

In `src/runtime/speed_test.cpp`, remove the implementation of `TqRunClientSpeedTest(...)` and helper functions used only by that direct path.

- [ ] **Step 3: Define ingress speedtest behavior**

Choose HTTP CONNECT as the required ingress path for built-in client speedtest. Add a helper that opens a TCP connection to `cfg.HttpListen` and issues a CONNECT request for the speedtest target:

```cpp
bool TqRunIngressClientSpeedTest(const TqConfig& cfg);
```

This helper must fail fast if `cfg.HttpListen` is empty or not listening. It must not access `QuicClientSession` directly.

- [ ] **Step 4: Wire speedtest after ingress startup**

After `runtime->EnableAcceptingAndApplyCurrentConnectionState(err, false)` succeeds, if `cfg.SpeedTestMode != None`, run:

```cpp
const bool ok = TqRunIngressClientSpeedTest(cfg);
runtime->DisableAccepting();
quic.Stop();
return ok ? 0 : 1;
```

Because listen ports are only open when QUIC is connected, this guarantees speedtest uses the same ingress gating as real client traffic.

- [ ] **Step 5: Update speedtest tests**

In `src/unittest/speed_test_test.cpp`, remove stubs for:

```cpp
bool QuicClientSession::EnsureAnyConnected(std::chrono::milliseconds);
MsQuicConnection* QuicClientSession::PickConnection();
```

Add tests for `TqRunIngressClientSpeedTest` using a local fake HTTP CONNECT server:

```cpp
static int TestIngressSpeedTestRequiresHttpListen() {
    TqConfig cfg;
    cfg.SpeedTestMode = TqSpeedTestMode::Upload;
    cfg.HttpListen.clear();
    return TqRunIngressClientSpeedTest(cfg) ? 200 : 0;
}
```

Add a fake listener test that verifies the helper sends an HTTP CONNECT request and fails cleanly if the fake listener rejects it.

- [ ] **Step 6: Build and run speedtest tests**

```bash
rtk cmake --build build-regression --target tcpquic_speed_test_test tcpquic-proxy -j4
rtk ./build-regression/bin/Release/tcpquic_speed_test_test
```

Expected: test exits `0`.

- [ ] **Step 7: Commit**

```bash
rtk git add src/main.cpp src/runtime/speed_test.h src/runtime/speed_test.cpp src/unittest/speed_test_test.cpp docs/thread-model_cn.md
rtk git commit -m "refactor(speedtest): route client speedtest through ingress"
```

---

### Task 7: Update Thread Model Documentation

**Files:**
- Modify: `docs/thread-model_cn.md`
- Modify: `docs/superpowers/specs/2026-06-19-callback-driven-client-reconnect-design.md`

- [ ] **Step 1: Remove reconnect thread from thread count**

In `docs/thread-model_cn.md`, remove:

```markdown
- **client reconnect thread**：每个 `QuicClientSession` 启动一个 reconnect loop，用于补齐断开的 QUIC connection slot。
```

Change the client thread count block from:

```text
+ P * 1 client reconnect thread
```

to no reconnect-thread line.

- [ ] **Step 2: Add callback-driven reconnect wording**

Add:

```markdown
- **client QUIC reconnect**：不再使用每个 peer 一个 reconnect thread。MsQuic connection callback 在 `SHUTDOWN_COMPLETE` 后立即重建对应 connection slot；同步 `ConnectionStart` 失败由 `TqClientIngressReactor` 的 delayed task 固定延迟重试。
```

- [ ] **Step 3: Document ingress gating**

Add:

```markdown
peer 的 SOCKS5 / HTTP CONNECT listen 端口只在该 peer 至少有一个 connected QUIC connection 时开放。connected count 变为 0 时，runtime 关闭该 peer listen 端口并清理仍处于 handshake / opening 阶段的 client socket。
```

- [ ] **Step 4: Remove warmup wording and update speedtest wording**

Remove warmup references. Change speedtest wording to:

```markdown
内置 client speedtest 通过普通 HTTP CONNECT ingress 路径发起，不直接访问 `QuicClientSession`。因此 speedtest 与真实代理流量共享同一套 listen 开放规则、handshake、OPEN 和 relay 路径。
```

- [ ] **Step 5: Verify docs**

```bash
rtk proxy git diff --check
rtk rg -n "reconnect thread|warmup|Warmup|TqRunClientWarmup|TqRunClientSpeedTest" docs/thread-model_cn.md src
```

Expected: no reconnect thread or warmup references remain in production docs/source. `TqRunClientSpeedTest` should not appear.

- [ ] **Step 6: Commit**

```bash
rtk git add docs/thread-model_cn.md docs/superpowers/specs/2026-06-19-callback-driven-client-reconnect-design.md
rtk git commit -m "docs: document callback-driven client reconnect"
```

---

### Task 8: Full Regression

**Files:**
- No source changes expected unless verification exposes issues.

- [ ] **Step 1: Build representative Linux targets**

```bash
rtk cmake --build build-regression --target \
  tcpquic-proxy \
  tcpquic_linux_reactor_test \
  tcpquic_ares_dns_resolver_test \
  tcpquic_server_dial_reactor_test \
  tcpquic_client_ingress_reactor_test \
  tcpquic_client_tunnel_open_test \
  tcpquic_tunnel_test \
  tcpquic_speed_test_test \
  tcpquic_quic_session_reconnect_test \
  tcpquic_router_runtime_test \
  tcpquic_config_router_test \
  -j4
```

Expected: build succeeds.

- [ ] **Step 2: Run representative Linux tests**

```bash
rtk ./build-regression/bin/Release/tcpquic_linux_reactor_test
rtk ./build-regression/bin/Release/tcpquic_ares_dns_resolver_test
rtk ./build-regression/bin/Release/tcpquic_server_dial_reactor_test
rtk ./build-regression/bin/Release/tcpquic_client_ingress_reactor_test
rtk ./build-regression/bin/Release/tcpquic_client_tunnel_open_test
rtk ./build-regression/bin/Release/tcpquic_tunnel_test
rtk ./build-regression/bin/Release/tcpquic_speed_test_test
rtk ./build-regression/bin/Release/tcpquic_quic_session_reconnect_test
rtk ./build-regression/bin/Release/tcpquic_router_runtime_test
rtk ./build-regression/bin/Release/tcpquic_config_router_test
```

Expected: all executables exit `0`.

- [ ] **Step 3: Build and run macOS targets**

On macOS:

```bash
rtk cmake --build build --target \
  tcpquic-proxy \
  tcpquic_darwin_reactor_test \
  tcpquic_ares_dns_resolver_test \
  tcpquic_server_dial_reactor_test \
  tcpquic_client_ingress_reactor_test \
  tcpquic_speed_test_test \
  tcpquic_quic_session_reconnect_test \
  -j4
rtk ./build/src/tcpquic_darwin_reactor_test
rtk ./build/src/tcpquic_ares_dns_resolver_test
rtk ./build/src/tcpquic_server_dial_reactor_test
rtk ./build/src/tcpquic_client_ingress_reactor_test
rtk ./build/src/tcpquic_speed_test_test
rtk ./build/src/tcpquic_quic_session_reconnect_test
```

Expected: all build targets succeed and all tests exit `0`.

- [ ] **Step 4: Manual integration smoke**

Run these checks:

1. Start client with server down. Verify peer SOCKS5 / HTTP CONNECT listen ports are not open.
2. Start server. Verify callback-driven reconnect opens peer ingress after QUIC connects.
3. Stop server. Verify connected count becomes 0 and peer ingress closes.
4. Restart server. Verify `SHUTDOWN_COMPLETE` callback triggers immediate slot rebuild and ingress reopens.
5. Run client speedtest through HTTP CONNECT ingress. Verify it fails if ingress is closed and succeeds only when QUIC is connected.

Expected: behavior matches all five checks.

- [ ] **Step 5: Final status**

```bash
rtk git status --short --branch
rtk git log --oneline --decorate -8
```

Expected: branch contains the planned commits and no unintended tracked modifications remain.

## Self-Review

- Spec coverage: Tasks cover delayed scheduler, reconnect thread removal, callback restart, ingress scheduler wiring, warmup deletion, speedtest ingress routing, docs, and regression.
- Marker scan: No incomplete-work markers are present. Steps include exact files, commands, and expected outcomes.
- Type consistency: The plan consistently uses `DelayedTaskScheduler`, `SetDelayedTaskScheduler`, `EnqueueDelayed`, `StartSlot`, and `StartAllSlots`.
