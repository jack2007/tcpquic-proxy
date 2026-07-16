#include "libuv_allocator.h"
#include "libuv_relay_event_queue.h"
#include "libuv_relay_worker.h"
#include "tuning.h"

#include <uv.h>

#include <atomic>
#include <array>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <new>
#include <thread>
#include <type_traits>
#include <vector>

static_assert(std::is_same_v<
              decltype(TqUvCallAdapter::Close),
              void (*)(uv_handle_t*, uv_close_cb)>);
static_assert(std::is_same_v<
              decltype(TqUvCallAdapter::TcpInit),
              int (*)(uv_loop_t*, uv_tcp_t*)>);
static_assert(std::is_same_v<
              decltype(TqUvCallAdapter::TcpOpen),
              int (*)(uv_tcp_t*, uv_os_sock_t)>);
static_assert(std::is_same_v<
              decltype(TqUvCallAdapter::ReadStart),
              int (*)(uv_stream_t*, uv_alloc_cb, uv_read_cb)>);
static_assert(std::is_same_v<
              decltype(TqUvCallAdapter::Write),
              int (*)(uv_write_t*, uv_stream_t*, const uv_buf_t[],
                      unsigned int, uv_write_cb)>);
static_assert(std::is_same_v<
              decltype(TqUvCallAdapter::Shutdown),
              int (*)(uv_shutdown_t*, uv_stream_t*, uv_shutdown_cb)>);
static_assert(!std::is_copy_constructible_v<TqUvRelayState>);
static_assert(!std::is_copy_assignable_v<TqUvRelayState>);
static_assert(!std::is_move_constructible_v<TqUvRelayState>);
static_assert(!std::is_move_assignable_v<TqUvRelayState>);
static_assert(std::is_same_v<
              decltype(TqUvRelayState::AccountedQuicToTcpBytes),
              std::atomic<std::uint64_t>>);
static_assert(std::is_same_v<
              decltype(TqUvRelayState::AccountedTcpToQuicBytes),
              std::atomic<std::uint64_t>>);

namespace {

void CheckAt(bool condition, int line) {
    if (!condition) {
        std::fprintf(stderr, "libuv worker queue check failed at line %d\n", line);
        std::abort();
    }
}
#define Check(condition) CheckAt((condition), __LINE__)

int gLoopInitCalls = 0;
int gLoopCloseCalls = 0;
std::atomic<std::uint64_t> gAsyncSendCalls{0};
std::atomic<std::uint64_t> gAsyncInitCalls{0};
std::atomic<std::uint64_t> gTcpCloseCalls{0};
std::atomic<std::uint64_t> gWorkerCloseCalls{0};
std::atomic<std::uint64_t> gWorkerCloseAttempts{0};
std::atomic<std::uint64_t> gClosePassFaults{0};
std::atomic<std::uint64_t> gTimerCloseFaults{0};
std::atomic<bool> gArmClosePassFault{false};
std::atomic<bool> gArmTimerCloseFault{false};
#if TCPQUIC_USE_MIMALLOC
std::atomic<std::uint64_t> gAllocatorReplaceCalls{0};
std::atomic<std::uint64_t> gQueueMutexInitCalls{0};
#endif
std::mutex gRegistrationHookMutex;
std::condition_variable gRegistrationHookCondition;
bool gRegistrationHookEntered = false;
bool gReleaseRegistrationHook = false;
std::mutex gAdmissionHookMutex;
std::condition_variable gAdmissionHookCondition;
bool gAdmissionHookArmed = false;
bool gAdmissionHookEntered = false;
bool gReleaseAdmissionHook = false;
bool gFailQueuePopOnce = false;

void FailQueuePopOnce() {
    if (gFailQueuePopOnce) {
        gFailQueuePopOnce = false;
        throw std::bad_alloc{};
    }
}

void FailClosePassAfterFirstRelay(std::size_t processed) {
    if (processed == 1 &&
        gArmClosePassFault.exchange(false, std::memory_order_acq_rel)) {
        gClosePassFaults.fetch_add(1, std::memory_order_relaxed);
        throw std::bad_alloc{};
    }
}

class EmptyTarget final : public TqStreamLifetime::Target {
public:
    QUIC_STATUS OnStreamEvent(
        MsQuicStream*, QUIC_STREAM_EVENT*, std::uint64_t) noexcept override {
        return QUIC_STATUS_SUCCESS;
    }
};

int FakeTcpInit(uv_loop_t*, uv_tcp_t*) { return 0; }
int FakeTcpOpen(uv_tcp_t*, uv_os_sock_t) { return 0; }
int FakeReadStart(uv_stream_t*, uv_alloc_cb, uv_read_cb) { return 0; }
int FakeReadStop(uv_stream_t*) { return 0; }

void SynchronousTcpClose(uv_handle_t* handle, uv_close_cb callback) {
    if (handle->type == UV_UNKNOWN_HANDLE) {
        gTcpCloseCalls.fetch_add(1, std::memory_order_relaxed);
        callback(handle);
        return;
    }
    gWorkerCloseAttempts.fetch_add(1, std::memory_order_relaxed);
    if (handle->type == UV_TIMER &&
        gArmTimerCloseFault.exchange(false, std::memory_order_acq_rel)) {
        gTimerCloseFaults.fetch_add(1, std::memory_order_relaxed);
        throw std::bad_alloc{};
    }
    gWorkerCloseCalls.fetch_add(1, std::memory_order_relaxed);
    uv_close(handle, callback);
}

TqUvRelayRegistration MakeCloseTestRegistration(
    std::uint64_t socket,
    const std::shared_ptr<TqRelayStopControl>& control) {
    TqUvRelayRegistration registration{};
    registration.TcpSocket = static_cast<TqSocketHandle>(socket);
    registration.Stream = reinterpret_cast<MsQuicStream*>(0x1);
    registration.StreamOwner = TqStreamLifetime::CreateForTest(
        TqStreamLifetime::Phase::Started, std::make_shared<EmptyTarget>());
    registration.StopControl = control;
    registration.ControlGeneration = control->Generation;
    registration.TcpReadChunkSize = 4096;
    registration.MaxPendingBufferBytes = 32768;
    registration.MaxBufferedQuicSendBytes = 16384;
    registration.ResumeBufferedQuicSendBytes = 8192;
    registration.PrecommitMaxPendingBytes = 12288;
    return registration;
}
std::mutex gAsyncSendHookMutex;
std::condition_variable gAsyncSendHookCondition;
bool gAsyncSendHookEntered = false;
bool gReleaseAsyncSendHook = false;
std::mutex gStopWakeHookMutex;
std::condition_variable gStopWakeHookCondition;
bool gStopWakeSendEntered = false;
bool gReleaseStopWakeSend = false;
bool gAsyncCloseEntered = false;
std::mutex gPendingOverflowHookMutex;
std::condition_variable gPendingOverflowHookCondition;
bool gPendingOverflowHookEntered = false;
bool gReleasePendingOverflowHook = false;
std::mutex gLoopTurnHookMutex;
std::condition_variable gLoopTurnHookCondition;
bool gLoopTurnHookArmed = false;
bool gLoopTurnHookCaptured = false;
TqUvRelayWorkerSnapshot gLoopTurnAfterTimer;

bool RejectAllocatorInstall() noexcept {
    return false;
}

#if TCPQUIC_USE_MIMALLOC
int RejectAllocatorReplacement(
    uv_malloc_func,
    uv_realloc_func,
    uv_calloc_func,
    uv_free_func) {
    gAllocatorReplaceCalls.fetch_add(1, std::memory_order_relaxed);
    return UV_EPERM;
}

int CountQueueMutexInit(uv_mutex_t*) {
    gQueueMutexInitCalls.fetch_add(1, std::memory_order_relaxed);
    return UV_ENOMEM;
}
#endif

int CountLoopInit(uv_loop_t* loop) {
    ++gLoopInitCalls;
    if (gLoopInitCalls == 2) {
        return UV_EIO;
    }
    return uv_loop_init(loop);
}

int CountLoopClose(uv_loop_t* loop) {
    ++gLoopCloseCalls;
    return uv_loop_close(loop);
}

int FailFirstAsyncSend(uv_async_t* async) {
    const auto call = gAsyncSendCalls.fetch_add(1, std::memory_order_relaxed);
    if (call == 0) {
        return UV_EIO;
    }
    return uv_async_send(async);
}

int AlwaysFailAsyncSend(uv_async_t*) {
    gAsyncSendCalls.fetch_add(1, std::memory_order_relaxed);
    return UV_EIO;
}

int BlockingSuccessfulAsyncSend(uv_async_t* async) {
    std::unique_lock<std::mutex> lock(gAsyncSendHookMutex);
    gAsyncSendHookEntered = true;
    gAsyncSendHookCondition.notify_all();
    gAsyncSendHookCondition.wait(lock, [] { return gReleaseAsyncSendHook; });
    return uv_async_send(async);
}

int SendThenBlockStopWake(uv_async_t* async) {
    const int status = uv_async_send(async);
    std::unique_lock<std::mutex> lock(gStopWakeHookMutex);
    gStopWakeSendEntered = true;
    gStopWakeHookCondition.notify_all();
    gStopWakeHookCondition.wait(lock, [] { return gReleaseStopWakeSend; });
    return status;
}

void ObserveAsyncClose(uv_handle_t* handle, uv_close_cb callback) {
    if (handle->type == UV_ASYNC) {
        std::lock_guard<std::mutex> guard(gStopWakeHookMutex);
        gAsyncCloseEntered = true;
        gStopWakeHookCondition.notify_all();
    }
    uv_close(handle, callback);
}

void BlockingPendingOverflowHook() {
    std::unique_lock<std::mutex> lock(gPendingOverflowHookMutex);
    gPendingOverflowHookEntered = true;
    gPendingOverflowHookCondition.notify_all();
    gPendingOverflowHookCondition.wait(
        lock, [] { return gReleasePendingOverflowHook; });
}

void CaptureSafetyTimerInCurrentTurn(TqUvRelayWorker& worker) {
    std::lock_guard<std::mutex> guard(gLoopTurnHookMutex);
    if (!gLoopTurnHookArmed || gLoopTurnHookCaptured) {
        return;
    }
    gLoopTurnAfterTimer = worker.Snapshot();
    gLoopTurnHookCaptured = true;
    gLoopTurnHookCondition.notify_all();
}

int FailQueueMutexInit(uv_mutex_t*) {
    return UV_ENOMEM;
}

int FailAsyncInit(uv_loop_t*, uv_async_t*, uv_async_cb) {
    return UV_EIO;
}

int FailSecondAsyncInit(uv_loop_t* loop, uv_async_t* async, uv_async_cb callback) {
    const auto call = gAsyncInitCalls.fetch_add(1, std::memory_order_relaxed);
    if (call == 1) {
        return UV_EIO;
    }
    return uv_async_init(loop, async, callback);
}

TqUvRelayRegistrationResult BlockingRegistrationLocal(
    TqUvRelayWorker&,
    TqUvRelayRegistration) {
    std::unique_lock<std::mutex> lock(gRegistrationHookMutex);
    gRegistrationHookEntered = true;
    gRegistrationHookCondition.notify_all();
    gRegistrationHookCondition.wait(lock, [] { return gReleaseRegistrationHook; });
    TqUvRelayRegistrationResult result{};
    result.TcpFdConsumed = true;
    result.RelayId = 77;
    return result;
}

void BlockingAdmissionHook() {
    std::unique_lock<std::mutex> lock(gAdmissionHookMutex);
    if (!gAdmissionHookArmed) {
        return;
    }
    gAdmissionHookEntered = true;
    gAdmissionHookCondition.notify_all();
    gAdmissionHookCondition.wait(lock, [] { return gReleaseAdmissionHook; });
}

int ReturnTcpInitStatus(uv_loop_t*, uv_tcp_t*) {
    return UV_EACCES;
}

int ReturnTcpOpenStatus(uv_tcp_t*, uv_os_sock_t) {
    return UV_EADDRINUSE;
}

int ReturnReadStartStatus(uv_stream_t*, uv_alloc_cb, uv_read_cb) {
    return UV_EALREADY;
}

int ReturnWriteStatus(
    uv_write_t*,
    uv_stream_t*,
    const uv_buf_t[],
    unsigned int,
    uv_write_cb) {
    return UV_EBUSY;
}

int ReturnShutdownStatus(uv_shutdown_t*, uv_stream_t*, uv_shutdown_cb) {
    return UV_ECANCELED;
}

void TestTcpCallAdapterPathsAreConsumable() {
    auto calls = TqUvProductionCalls();
    calls.TcpInit = &ReturnTcpInitStatus;
    calls.TcpOpen = &ReturnTcpOpenStatus;
    calls.ReadStart = &ReturnReadStartStatus;
    calls.Write = &ReturnWriteStatus;
    calls.Shutdown = &ReturnShutdownStatus;
    TqUvRelayWorkerConfig config{};
    config.Calls = &calls;
    TqUvRelayWorker worker(config);

    Check(worker.CallTcpInitForTest(nullptr, nullptr) == UV_EACCES);
    Check(worker.CallTcpOpenForTest(nullptr, uv_os_sock_t{}) == UV_EADDRINUSE);
    Check(worker.CallReadStartForTest(nullptr, nullptr, nullptr) == UV_EALREADY);
    Check(worker.CallWriteForTest(nullptr, nullptr, nullptr, 0, nullptr) ==
          UV_EBUSY);
    Check(worker.CallShutdownForTest(nullptr, nullptr, nullptr) ==
          UV_ECANCELED);
}

void TestWorkerConstructionRequiresSuccessfulAllocatorBootstrap() {
#if TCPQUIC_USE_MIMALLOC
    gAllocatorReplaceCalls.store(0, std::memory_order_relaxed);
    gQueueMutexInitCalls.store(0, std::memory_order_relaxed);
    TqUvResetAllocatorStateForTest();
    TqUvSetReplaceAllocatorForTest(&RejectAllocatorReplacement);

    TqUvRelayWorkerConfig config{};
    config.QueueMutexInitForTest = &CountQueueMutexInit;
    TqUvRelayWorker worker(config);
    Check(gAllocatorReplaceCalls.load(std::memory_order_relaxed) == 1);
    Check(gQueueMutexInitCalls.load(std::memory_order_relaxed) == 0);
    Check(!worker.StartAndWaitReady());

    TqUvResetAllocatorStateForTest();
    Check(TqUvInstallAllocator());
    Check(TqUvAllocatorStatus().Installed);
#else
    Check(TqUvInstallAllocator());
#endif
}

void TestAllocatorFailurePreventsLoopInitialization() {
    auto calls = TqUvProductionCalls();
    calls.LoopInit = [](uv_loop_t*) -> int { return UV_EINVAL; };

    auto& runtime = TqUvRelayRuntime::Instance();
    runtime.SetAllocatorInstallerForTest(&RejectAllocatorInstall);
    runtime.SetCallAdapterForTest(&calls);

    TqTuningConfig tuning{};
    tuning.RelayWorkerCount = 2;
    Check(!runtime.Start(tuning));
    Check(runtime.WorkerCountForTest() == 0);
    Check(runtime.LoopInitCallsForTest() == 0);

    runtime.ResetTestHooksForTest();
}

void TestSecondWorkerFailureRollsBackFirstWorker() {
    gLoopInitCalls = 0;
    gLoopCloseCalls = 0;
    auto calls = TqUvProductionCalls();
    calls.LoopInit = &CountLoopInit;
    calls.LoopClose = &CountLoopClose;

    auto& runtime = TqUvRelayRuntime::Instance();
    runtime.SetCallAdapterForTest(&calls);

    TqTuningConfig tuning{};
    tuning.RelayWorkerCount = 2;
    Check(!runtime.Start(tuning));
    Check(runtime.WorkerCountForTest() == 0);
    Check(gLoopInitCalls == 2);
    Check(gLoopCloseCalls == 1);
    Check(runtime.StartedWorkersForTest() == 1);
    Check(runtime.RolledBackWorkersForTest() == 1);

    runtime.ResetTestHooksForTest();
}

void TestQueueFifoCapacityAndCoalescedWakeFact() {
    TqUvRelayEventQueue<std::uint64_t> queue(3);
    const auto first = queue.TryPush(10);
    const auto second = queue.TryPush(20);
    const auto third = queue.TryPush(30);
    Check(first.Accepted && first.ShouldWake);
    Check(second.Accepted && !second.ShouldWake);
    Check(third.Accepted && !third.ShouldWake);
    Check(!queue.TryPush(40).Accepted);

    std::vector<std::uint64_t> local;
    std::uint64_t value = 0;
    while (queue.TryPop(value)) {
        local.push_back(value);
    }
    Check((local == std::vector<std::uint64_t>{10, 20, 30}));
    Check(queue.Empty());
    const auto afterDrain = queue.TryPush(40);
    Check(afterDrain.Accepted && afterDrain.ShouldWake);
}

void TestQueuePopFaultRetainsFifoContents() {
    TqUvRelayEventQueue<std::uint64_t> queue(
        3, nullptr, &FailQueuePopOnce);
    Check(queue.TryPush(10).Accepted);
    Check(queue.TryPush(20).Accepted);
    gFailQueuePopOnce = true;
    std::uint64_t value = 0;
    Check(!queue.TryPop(value));
    Check(queue.Size() == 2);
    Check(queue.TryPop(value));
    Check(value == 10);
    Check(queue.TryPop(value));
    Check(value == 20);
    Check(!queue.TryPop(value));
}

void TestQueueFullDoesNotBlockMinimumStop() {
    TqUvRelayWorkerConfig config{};
    config.QueueCapacity = 1;
    TqUvRelayWorker worker(config);
    Check(worker.StartAndWaitReady());

    std::mutex mutex;
    std::condition_variable condition;
    bool blockerEntered = false;
    bool releaseBlocker = false;
    Check(worker.Post([&](TqUvRelayWorker&) {
        std::unique_lock<std::mutex> lock(mutex);
        blockerEntered = true;
        condition.notify_all();
        condition.wait(lock, [&] { return releaseBlocker; });
    }));

    {
        std::unique_lock<std::mutex> lock(mutex);
        Check(condition.wait_for(lock, std::chrono::seconds(5), [&] {
            return blockerEntered;
        }));
    }
    Check(worker.Post([](TqUvRelayWorker&) {}));
    Check(!worker.Post([](TqUvRelayWorker&) {}));

    std::thread stopper([&] { Check(worker.StopForTest()); });
    const auto stopDeadline =
        std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (!worker.StopRequestedForTest()) {
        Check(std::chrono::steady_clock::now() < stopDeadline);
        std::this_thread::yield();
    }
    {
        std::lock_guard<std::mutex> guard(mutex);
        releaseBlocker = true;
    }
    condition.notify_all();
    stopper.join();
    Check(!worker.Snapshot().Running);
}

void TestAsyncSendFailureRetriesWithoutLostWake() {
    gAsyncSendCalls.store(0, std::memory_order_relaxed);
    auto calls = TqUvProductionCalls();
    calls.AsyncSend = &FailFirstAsyncSend;
    TqUvRelayWorkerConfig config{};
    config.QueueCapacity = 4;
    config.Calls = &calls;
    TqUvRelayWorker worker(config);
    Check(worker.StartAndWaitReady());

    std::mutex mutex;
    std::condition_variable condition;
    bool executed = false;
    Check(worker.Post([&](TqUvRelayWorker&) {
        {
            std::lock_guard<std::mutex> guard(mutex);
            executed = true;
        }
        condition.notify_one();
    }));

    std::unique_lock<std::mutex> lock(mutex);
    Check(condition.wait_for(lock, std::chrono::seconds(5), [&] {
        return executed;
    }));
    lock.unlock();
    Check(gAsyncSendCalls.load(std::memory_order_relaxed) >= 2);
    const auto snapshot = worker.Snapshot();
    Check(snapshot.AsyncWakeAttempts == 2);
    Check(snapshot.AsyncWakeSuccesses == 1);
    Check(snapshot.AsyncWakeFailures == 1);
    Check(snapshot.AsyncWakeAttempts ==
          snapshot.AsyncWakeSuccesses + snapshot.AsyncWakeFailures);
    Check(snapshot.AsyncCallbacks >= 1);
    Check(snapshot.LoopIterations >= 1);
    Check(snapshot.CommandsExecuted == 1);
    Check(worker.StopForTest());
}

void TestWakeSnapshotPublishesOnlyClassifiedAttempts() {
    {
        std::lock_guard<std::mutex> guard(gAsyncSendHookMutex);
        gAsyncSendHookEntered = false;
        gReleaseAsyncSendHook = false;
    }
    auto calls = TqUvProductionCalls();
    calls.AsyncSend = &BlockingSuccessfulAsyncSend;
    TqUvRelayWorkerConfig config{};
    config.Calls = &calls;
    TqUvRelayWorker worker(config);
    Check(worker.StartAndWaitReady());

    std::thread producer([&] {
        Check(worker.Post([](TqUvRelayWorker&) {}));
    });
    {
        std::unique_lock<std::mutex> lock(gAsyncSendHookMutex);
        Check(gAsyncSendHookCondition.wait_for(
            lock, std::chrono::seconds(2), [] { return gAsyncSendHookEntered; }));
    }
    const auto inFlight = worker.Snapshot();
    Check(inFlight.AsyncWakeAttempts ==
          inFlight.AsyncWakeSuccesses + inFlight.AsyncWakeFailures);
    Check(inFlight.AsyncWakeAttempts == 0);
    {
        std::lock_guard<std::mutex> guard(gAsyncSendHookMutex);
        gReleaseAsyncSendHook = true;
    }
    gAsyncSendHookCondition.notify_all();
    producer.join();
    const auto classified = worker.Snapshot();
    Check(classified.AsyncWakeAttempts == 1);
    Check(classified.AsyncWakeSuccesses == 1);
    Check(classified.AsyncWakeFailures == 0);
    Check(worker.StopForTest());
}

void TestLoopTurnDoesNotCountMultipleCallbacksAsIterations() {
    {
        std::lock_guard<std::mutex> guard(gLoopTurnHookMutex);
        gLoopTurnHookArmed = false;
        gLoopTurnHookCaptured = false;
        gLoopTurnAfterTimer = {};
    }
    TqUvRelayWorkerConfig config{};
    config.AfterSafetyTimerForTest = &CaptureSafetyTimerInCurrentTurn;
    TqUvRelayWorker worker(config);
    Check(worker.StartAndWaitReady());

    std::mutex mutex;
    std::condition_variable condition;
    bool callbackEntered = false;
    bool releaseCallback = false;
    TqUvRelayWorkerSnapshot afterAsyncStarted;
    Check(worker.Post([&](TqUvRelayWorker& current) {
        afterAsyncStarted = current.Snapshot();
        {
            std::lock_guard<std::mutex> guard(gLoopTurnHookMutex);
            gLoopTurnHookArmed = true;
        }
        std::unique_lock<std::mutex> lock(mutex);
        callbackEntered = true;
        condition.notify_all();
        condition.wait(lock, [&] { return releaseCallback; });
    }));
    {
        std::unique_lock<std::mutex> lock(mutex);
        Check(condition.wait_for(lock, std::chrono::seconds(2), [&] {
            return callbackEntered;
        }));
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(40));
    {
        std::lock_guard<std::mutex> guard(mutex);
        releaseCallback = true;
    }
    condition.notify_all();
    {
        std::unique_lock<std::mutex> lock(gLoopTurnHookMutex);
        Check(gLoopTurnHookCondition.wait_for(
            lock, std::chrono::seconds(2), [] { return gLoopTurnHookCaptured; }));
    }
    Check(gLoopTurnAfterTimer.LoopIterations == afterAsyncStarted.LoopIterations);
    Check(gLoopTurnAfterTimer.AsyncCallbacks == afterAsyncStarted.AsyncCallbacks);
    Check(gLoopTurnAfterTimer.SafetyTimerCallbacks ==
          afterAsyncStarted.SafetyTimerCallbacks + 1);
    Check(worker.StopForTest());
}

void TestQueueMutexInitFailurePreventsWorkerStartup() {
    TqUvRelayWorkerConfig config{};
    config.QueueMutexInitForTest = &FailQueueMutexInit;
    TqUvRelayWorker worker(config);
    Check(!worker.StartAndWaitReady());
    Check(!worker.Snapshot().Running);
}

void TestQueuedRegistrationUsesFixedDeadlineAcrossSpuriousSignals() {
    TqUvRelayWorkerConfig config{};
    config.ControlCommandTimeoutMs = 60;
    TqUvRelayWorker worker(config);
    Check(worker.StartAndWaitReady());

    std::mutex mutex;
    std::condition_variable condition;
    bool blockerEntered = false;
    bool releaseBlocker = false;
    Check(worker.Post([&](TqUvRelayWorker&) {
        std::unique_lock<std::mutex> lock(mutex);
        blockerEntered = true;
        condition.notify_all();
        condition.wait(lock, [&] { return releaseBlocker; });
    }));
    {
        std::unique_lock<std::mutex> lock(mutex);
        Check(condition.wait_for(lock, std::chrono::seconds(5), [&] {
            return blockerEntered;
        }));
    }

    std::atomic<bool> registrationDone{false};
    std::chrono::steady_clock::duration elapsed{};
    std::thread waiter([&] {
        const auto started = std::chrono::steady_clock::now();
        const auto result = worker.RegisterRelayWithId(TqUvRelayRegistration{});
        elapsed = std::chrono::steady_clock::now() - started;
        Check(!result.Ok);
        registrationDone.store(true, std::memory_order_release);
    });

    const auto stateDeadline =
        std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (worker.RegistrationStateForTest() !=
           TqUvRegisterCommandState::Queued) {
        Check(std::chrono::steady_clock::now() < stateDeadline);
        std::this_thread::yield();
    }
    for (int index = 0; index < 15; ++index) {
        worker.SignalRegistrationForTest();
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    Check(registrationDone.load(std::memory_order_acquire));
    waiter.join();
    Check(elapsed < std::chrono::milliseconds(130));

    {
        std::lock_guard<std::mutex> guard(mutex);
        releaseBlocker = true;
    }
    condition.notify_all();
    Check(worker.StopForTest());
}

void TestExecutingRegistrationWaitsForFinalOwnershipResult() {
    {
        std::lock_guard<std::mutex> guard(gRegistrationHookMutex);
        gRegistrationHookEntered = false;
        gReleaseRegistrationHook = false;
    }
    TqUvRelayWorkerConfig config{};
    config.ControlCommandTimeoutMs = 30;
    config.RegisterLocalForTest = &BlockingRegistrationLocal;
    TqUvRelayWorker worker(config);
    Check(worker.StartAndWaitReady());

    std::atomic<bool> waiterDone{false};
    TqUvRelayRegistrationResult result{};
    std::thread waiter([&] {
        result = worker.RegisterRelayWithId(TqUvRelayRegistration{});
        waiterDone.store(true, std::memory_order_release);
    });
    {
        std::unique_lock<std::mutex> lock(gRegistrationHookMutex);
        Check(gRegistrationHookCondition.wait_for(
            lock,
            std::chrono::seconds(5),
            [] { return gRegistrationHookEntered; }));
    }
    Check(worker.RegistrationStateForTest() ==
          TqUvRegisterCommandState::Executing);
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    Check(!waiterDone.load(std::memory_order_acquire));
    {
        std::lock_guard<std::mutex> guard(gRegistrationHookMutex);
        gReleaseRegistrationHook = true;
    }
    gRegistrationHookCondition.notify_all();
    waiter.join();
    Check(result.TcpFdConsumed);
    Check(result.RelayId == 77);
    Check(worker.StopForTest());
}

void TestStopCancelsQueuedRegistrationAndSignalsWaiter() {
    TqUvRelayWorkerConfig config{};
    config.ControlCommandTimeoutMs = 5000;
    TqUvRelayWorker worker(config);
    Check(worker.StartAndWaitReady());

    std::mutex blockerMutex;
    std::condition_variable blockerCondition;
    bool blockerEntered = false;
    bool releaseBlocker = false;
    Check(worker.Post([&](TqUvRelayWorker&) {
        std::unique_lock<std::mutex> lock(blockerMutex);
        blockerEntered = true;
        blockerCondition.notify_all();
        blockerCondition.wait(lock, [&] { return releaseBlocker; });
    }));
    {
        std::unique_lock<std::mutex> lock(blockerMutex);
        Check(blockerCondition.wait_for(lock, std::chrono::seconds(5), [&] {
            return blockerEntered;
        }));
    }

    std::mutex waiterMutex;
    std::condition_variable waiterCondition;
    bool waiterDone = false;
    std::thread waiter([&] {
        const auto result = worker.RegisterRelayWithId(TqUvRelayRegistration{});
        Check(!result.Ok);
        {
            std::lock_guard<std::mutex> guard(waiterMutex);
            waiterDone = true;
        }
        waiterCondition.notify_all();
    });
    const auto stateDeadline =
        std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (worker.RegistrationStateForTest() !=
           TqUvRegisterCommandState::Queued) {
        Check(std::chrono::steady_clock::now() < stateDeadline);
        std::this_thread::yield();
    }

    std::thread stopper([&] { Check(worker.StopForTest()); });
    {
        std::unique_lock<std::mutex> lock(waiterMutex);
        Check(waiterCondition.wait_for(lock, std::chrono::milliseconds(500), [&] {
            return waiterDone;
        }));
    }
    waiter.join();
    {
        std::lock_guard<std::mutex> guard(blockerMutex);
        releaseBlocker = true;
    }
    blockerCondition.notify_all();
    stopper.join();
}

void TestRelayStateOwnsActivationMutexDestructionOnce() {
    TqUvActivationMutex mutex;
    Check(!mutex.Ready());
    Check(mutex.Initialize());
    Check(mutex.Ready());
    Check(mutex.Lock());
    Check(mutex.Unlock());
    mutex.Destroy();
    mutex.Destroy();
    Check(!mutex.Ready());
    Check(mutex.DestroyCountForTest() == 1);
}

void TestActualWakeCoalescingAndCallbackFifoDrain() {
    TqUvRelayWorkerConfig config{};
    config.QueueCapacity = 4;
    TqUvRelayWorker worker(config);
    Check(worker.StartAndWaitReady());

    std::mutex mutex;
    std::condition_variable condition;
    bool blockerEntered = false;
    bool releaseBlocker = false;
    std::vector<int> order;
    Check(worker.Post([&](TqUvRelayWorker&) {
        std::unique_lock<std::mutex> lock(mutex);
        blockerEntered = true;
        condition.notify_all();
        condition.wait(lock, [&] { return releaseBlocker; });
    }));
    {
        std::unique_lock<std::mutex> lock(mutex);
        Check(condition.wait_for(lock, std::chrono::seconds(5), [&] {
            return blockerEntered;
        }));
    }
    const auto wakesBefore = worker.Snapshot().AsyncWakeAttempts;
    Check(worker.Post([&](TqUvRelayWorker&) {
        std::lock_guard<std::mutex> guard(mutex);
        order.push_back(1);
    }));
    Check(worker.Post([&](TqUvRelayWorker&) {
        {
            std::lock_guard<std::mutex> guard(mutex);
            order.push_back(2);
        }
        condition.notify_all();
    }));
    Check(worker.Snapshot().AsyncWakeAttempts == wakesBefore + 1);
    {
        std::lock_guard<std::mutex> guard(mutex);
        releaseBlocker = true;
    }
    condition.notify_all();
    {
        std::unique_lock<std::mutex> lock(mutex);
        Check(condition.wait_for(lock, std::chrono::seconds(5), [&] {
            return order.size() == 2;
        }));
        Check((order == std::vector<int>{1, 2}));
    }
    Check(worker.Snapshot().AsyncCallbacks >= 2);
    Check(worker.Snapshot().AsyncWakeCoalesced >= 1);
    Check(worker.StopForTest());
}

void TestLoopLagCalculationHandlesFirstTickAndBoundaries() {
    constexpr std::uint64_t interval = 25ull * 1000 * 1000;
    Check(TqUvLoopLagMicros(0, interval, interval) == 0);
    Check(TqUvLoopLagMicros(100, 100 + interval, interval) == 0);
    Check(TqUvLoopLagMicros(100, 100 + interval + 999, interval) == 0);
    Check(TqUvLoopLagMicros(100, 100 + interval + 2000, interval) == 2);
    Check(TqUvLoopLagMicros(UINT64_MAX - 10, 5, interval) == 0);
}

void TestDirectionalPendingContractSaturatesAndClears() {
    TqUvRelayWorkerConfig config{};
    TqUvRelayWorker worker(config);
    TqUvRelayState quicToTcp;
    TqUvRelayState tcpToQuic;
    Check(worker.AddPendingBytes(
        quicToTcp, TqUvPendingDirection::QuicToTcp, UINT64_MAX));
    Check(worker.AddPendingBytes(
        tcpToQuic, TqUvPendingDirection::TcpToQuic, 1));
    auto snapshot = worker.Snapshot();
    Check(snapshot.QuicToTcpPendingBytes == UINT64_MAX);
    Check(snapshot.TcpToQuicPendingBytes == 1);
    Check(snapshot.PendingBytes == UINT64_MAX);
    Check(!worker.AddPendingBytes(
        quicToTcp, TqUvPendingDirection::QuicToTcp, 1));
    Check(!worker.CompletePendingBytes(
        tcpToQuic, TqUvPendingDirection::TcpToQuic, 2));
    Check(worker.CompletePendingBytes(
        quicToTcp, TqUvPendingDirection::QuicToTcp, UINT64_MAX));
    Check(worker.CompletePendingBytes(
        tcpToQuic, TqUvPendingDirection::TcpToQuic, 1));
    snapshot = worker.Snapshot();
    Check(snapshot.PendingBytes == 0);
    Check(snapshot.QuicToTcpPendingBytes == 0);
    Check(snapshot.TcpToQuicPendingBytes == 0);
}

void TestConcurrentPendingOverflowCannotExposeWrappedCounter() {
    {
        std::lock_guard<std::mutex> guard(gPendingOverflowHookMutex);
        gPendingOverflowHookEntered = false;
        gReleasePendingOverflowHook = false;
    }
    TqUvRelayWorkerConfig config{};
    config.BeforePendingOverflowReturnForTest = &BlockingPendingOverflowHook;
    TqUvRelayWorker worker(config);
    TqUvRelayState base;
    TqUvRelayState rejected;
    TqUvRelayState accepted;
    Check(worker.AddPendingBytes(
        base, TqUvPendingDirection::QuicToTcp, UINT64_MAX - 1));

    bool rejectedResult = true;
    std::thread overflow([&] {
        rejectedResult = worker.AddPendingBytes(
            rejected, TqUvPendingDirection::QuicToTcp, 2);
    });
    {
        std::unique_lock<std::mutex> lock(gPendingOverflowHookMutex);
        Check(gPendingOverflowHookCondition.wait_for(
            lock,
            std::chrono::seconds(2),
            [] { return gPendingOverflowHookEntered; }));
    }
    Check(worker.AddPendingBytes(
        accepted, TqUvPendingDirection::QuicToTcp, 1));
    {
        std::lock_guard<std::mutex> guard(gPendingOverflowHookMutex);
        gReleasePendingOverflowHook = true;
    }
    gPendingOverflowHookCondition.notify_all();
    overflow.join();

    Check(!rejectedResult);
    Check(rejected.AccountedQuicToTcpBytes == 0);
    Check(accepted.AccountedQuicToTcpBytes == 1);
    Check(worker.Snapshot().QuicToTcpPendingBytes == UINT64_MAX);
    Check(worker.CompletePendingBytes(
        accepted, TqUvPendingDirection::QuicToTcp, 1));
    Check(worker.CompletePendingBytes(
        base, TqUvPendingDirection::QuicToTcp, UINT64_MAX - 1));
    Check(worker.Snapshot().QuicToTcpPendingBytes == 0);
}

void TestConcurrentDirectionalAdmissionAndCompletionStayBalanced() {
    TqUvRelayWorker worker(TqUvRelayWorkerConfig{});
    TqUvRelayState relay;
    constexpr std::uint64_t operations = 200000;
    Check(worker.AddPendingBytes(
        relay, TqUvPendingDirection::QuicToTcp, 1));

    std::atomic<std::uint64_t> completions{0};
    std::thread producer([&] {
        for (std::uint64_t index = 0; index < operations; ++index) {
            Check(worker.AddPendingBytes(
                relay, TqUvPendingDirection::QuicToTcp, 1));
        }
    });
    std::thread consumer([&] {
        for (std::uint64_t index = 0; index < operations; ++index) {
            if (worker.CompletePendingBytes(
                    relay, TqUvPendingDirection::QuicToTcp, 1)) {
                completions.fetch_add(1, std::memory_order_relaxed);
            }
        }
    });
    producer.join();
    consumer.join();

    const auto expected = 1 + operations -
        completions.load(std::memory_order_relaxed);
    Check(relay.AccountedQuicToTcpBytes.load(std::memory_order_relaxed) ==
          expected);
    Check(worker.Snapshot().QuicToTcpPendingBytes == expected);
    Check(worker.CompletePendingBytes(
        relay, TqUvPendingDirection::QuicToTcp, expected));
    Check(worker.Snapshot().PendingBytes == 0);
}

void TestConcurrentPostAndCallbackDrain() {
    TqUvRelayWorkerConfig config{};
    config.QueueCapacity = 32;
    TqUvRelayWorker worker(config);
    Check(worker.StartAndWaitReady());

    constexpr int ProducerCount = 4;
    constexpr int CommandsPerProducer = 40;
    constexpr int CommandCount = ProducerCount * CommandsPerProducer;
    std::atomic<int> executed{0};
    std::array<std::atomic<int>, CommandCount> executionsById{};
    std::mutex mutex;
    std::condition_variable condition;
    std::vector<std::thread> producers;
    for (int producer = 0; producer < ProducerCount; ++producer) {
        producers.emplace_back([&, producer] {
            for (int index = 0; index < CommandsPerProducer; ++index) {
                const int commandId = producer * CommandsPerProducer + index;
                while (!worker.Post([&, commandId](TqUvRelayWorker&) {
                    executionsById[commandId].fetch_add(
                        1, std::memory_order_relaxed);
                    if (executed.fetch_add(1, std::memory_order_acq_rel) + 1 ==
                        CommandCount) {
                        condition.notify_all();
                    }
                })) {
                    std::this_thread::yield();
                }
            }
        });
    }
    for (auto& producer : producers) {
        producer.join();
    }
    std::unique_lock<std::mutex> lock(mutex);
    Check(condition.wait_for(lock, std::chrono::seconds(5), [&] {
        return executed.load(std::memory_order_acquire) ==
            CommandCount;
    }));
    lock.unlock();
    Check(worker.Snapshot().CommandsExecuted == CommandCount);
    for (const auto& count : executionsById) {
        Check(count.load(std::memory_order_relaxed) == 1);
    }
    Check(worker.StopForTest());
}

void TestStopRetriesOneAsyncSendFailure() {
    gAsyncSendCalls.store(0, std::memory_order_relaxed);
    auto calls = TqUvProductionCalls();
    calls.AsyncSend = &FailFirstAsyncSend;
    TqUvRelayWorkerConfig config{};
    config.Calls = &calls;
    TqUvRelayWorker worker(config);
    Check(worker.StartAndWaitReady());
    Check(worker.StopForTest());
    Check(gAsyncSendCalls.load(std::memory_order_relaxed) >= 2);
    Check(!worker.Snapshot().Running);
}

void TestStopWakeDrainsBeforeAsyncClose() {
    {
        std::lock_guard<std::mutex> guard(gStopWakeHookMutex);
        gStopWakeSendEntered = false;
        gReleaseStopWakeSend = false;
        gAsyncCloseEntered = false;
    }
    auto calls = TqUvProductionCalls();
    calls.AsyncSend = &SendThenBlockStopWake;
    calls.Close = &ObserveAsyncClose;
    TqUvRelayWorkerConfig config{};
    config.Calls = &calls;
    TqUvRelayWorker worker(config);
    Check(worker.StartAndWaitReady());

    std::thread stopper([&] { Check(worker.StopForTest()); });
    {
        std::unique_lock<std::mutex> lock(gStopWakeHookMutex);
        Check(gStopWakeHookCondition.wait_for(
            lock,
            std::chrono::seconds(2),
            [] { return gStopWakeSendEntered; }));
        Check(!gStopWakeHookCondition.wait_for(
            lock,
            std::chrono::milliseconds(200),
            [] { return gAsyncCloseEntered; }));
        gReleaseStopWakeSend = true;
    }
    gStopWakeHookCondition.notify_all();
    stopper.join();
    {
        std::lock_guard<std::mutex> guard(gStopWakeHookMutex);
        Check(gAsyncCloseEntered);
    }
}

void TestPersistentAsyncSendFailureUsesBoundedSafetyWake() {
    gAsyncSendCalls.store(0, std::memory_order_relaxed);
    auto calls = TqUvProductionCalls();
    calls.AsyncSend = &AlwaysFailAsyncSend;
    TqUvRelayWorkerConfig config{};
    config.Calls = &calls;
    TqUvRelayWorker worker(config);
    Check(worker.StartAndWaitReady());

    std::mutex mutex;
    std::condition_variable condition;
    bool executed = false;
    const auto postStarted = std::chrono::steady_clock::now();
    Check(worker.Post([&](TqUvRelayWorker&) {
        {
            std::lock_guard<std::mutex> guard(mutex);
            executed = true;
        }
        condition.notify_all();
    }));
    const auto postElapsed = std::chrono::steady_clock::now() - postStarted;
    Check(postElapsed < std::chrono::milliseconds(200));
    {
        std::unique_lock<std::mutex> lock(mutex);
        Check(condition.wait_for(lock, std::chrono::seconds(2), [&] {
            return executed;
        }));
    }
    const auto stopStarted = std::chrono::steady_clock::now();
    Check(worker.StopForTest());
    const auto stopElapsed = std::chrono::steady_clock::now() - stopStarted;
    Check(stopElapsed < std::chrono::milliseconds(500));
    Check(!worker.Snapshot().Running);
}

void TestClosePassBadAllocRetriesPartialSynchronousClosesExactlyOnce() {
    gTcpCloseCalls.store(0, std::memory_order_relaxed);
    gWorkerCloseCalls.store(0, std::memory_order_relaxed);
    gWorkerCloseAttempts.store(0, std::memory_order_relaxed);
    gClosePassFaults.store(0, std::memory_order_relaxed);
    gTimerCloseFaults.store(0, std::memory_order_relaxed);
    gArmClosePassFault.store(true, std::memory_order_release);
    gArmTimerCloseFault.store(true, std::memory_order_release);
    auto calls = TqUvProductionCalls();
    calls.TcpInit = &FakeTcpInit;
    calls.TcpOpen = &FakeTcpOpen;
    calls.ReadStart = &FakeReadStart;
    calls.ReadStop = &FakeReadStop;
    calls.Close = &SynchronousTcpClose;
    TqUvRelayWorkerConfig config{};
    config.Calls = &calls;
    config.BeforeCloseRelayForTest = &FailClosePassAfterFirstRelay;
    TqUvRelayWorker worker(config);
    Check(worker.StartAndWaitReady());

    std::vector<TqUvRelayRegistrationResult> registrations;
    std::vector<std::shared_ptr<TqRelayStopControl>> controls;
    std::vector<std::shared_ptr<TqUvRelayState>> relays;
    for (std::uint64_t index = 0; index < 3; ++index) {
        auto control = std::make_shared<TqRelayStopControl>();
        auto result = worker.RegisterRelayWithId(
            MakeCloseTestRegistration(100 + index, control));
        Check(result.Ok);
        auto relay = worker.RelayForTest(result.RelayId);
        Check(relay != nullptr);
        relays.push_back(std::move(relay));
        controls.push_back(std::move(control));
        registrations.push_back(std::move(result));
    }

    std::thread stopper([&] { Check(worker.StopForTest()); });
    const auto deadline = std::chrono::steady_clock::now() +
        std::chrono::seconds(5);
    for (const auto& relay : relays) {
        while (relay->TerminalBeginCount.load(std::memory_order_acquire) == 0 &&
               std::chrono::steady_clock::now() < deadline) {
            std::this_thread::yield();
        }
        Check(relay->TerminalBeginCount.load(std::memory_order_acquire) == 1);
    }
    QUIC_STREAM_EVENT terminal{};
    terminal.Type = QUIC_STREAM_EVENT_SHUTDOWN_COMPLETE;
    for (const auto& relay : relays) {
        Check(relay->StreamOwner->DispatchForTest(&terminal) ==
              QUIC_STATUS_SUCCESS);
    }
    stopper.join();
    Check(!worker.Snapshot().Running);
    Check(gClosePassFaults.load(std::memory_order_relaxed) == 1);
    Check(gTimerCloseFaults.load(std::memory_order_relaxed) == 1);
    Check(gTcpCloseCalls.load(std::memory_order_relaxed) == 3);
    Check(gWorkerCloseCalls.load(std::memory_order_relaxed) == 3);
    Check(gWorkerCloseAttempts.load(std::memory_order_relaxed) == 4);
}

void TestStopAndProducerAdmissionAreLinearized() {
    {
        std::lock_guard<std::mutex> guard(gAdmissionHookMutex);
        gAdmissionHookArmed = false;
        gAdmissionHookEntered = false;
        gReleaseAdmissionHook = false;
    }
    TqUvRelayWorkerConfig config{};
    config.QueueCapacity = 8;
    config.AfterAdmissionCheckForTest = &BlockingAdmissionHook;
    TqUvRelayWorker worker(config);
    Check(worker.StartAndWaitReady());

    std::mutex blockerMutex;
    std::condition_variable blockerCondition;
    bool blockerEntered = false;
    bool releaseBlocker = false;
    Check(worker.Post([&](TqUvRelayWorker&) {
        std::unique_lock<std::mutex> lock(blockerMutex);
        blockerEntered = true;
        blockerCondition.notify_all();
        blockerCondition.wait(lock, [&] { return releaseBlocker; });
    }));
    {
        std::unique_lock<std::mutex> lock(blockerMutex);
        Check(blockerCondition.wait_for(lock, std::chrono::seconds(5), [&] {
            return blockerEntered;
        }));
    }
    {
        std::lock_guard<std::mutex> guard(gAdmissionHookMutex);
        gAdmissionHookArmed = true;
    }

    std::atomic<int> acceptedExecutions{0};
    std::atomic<int> rejectedExecutions{0};
    bool accepted = false;
    std::thread producer([&] {
        accepted = worker.Post([&](TqUvRelayWorker&) {
            acceptedExecutions.fetch_add(1, std::memory_order_relaxed);
        });
    });
    {
        std::unique_lock<std::mutex> lock(gAdmissionHookMutex);
        Check(gAdmissionHookCondition.wait_for(
            lock,
            std::chrono::seconds(5),
            [] { return gAdmissionHookEntered; }));
    }
    std::thread stopper([&] { Check(worker.StopForTest()); });
    {
        std::lock_guard<std::mutex> guard(gAdmissionHookMutex);
        gReleaseAdmissionHook = true;
    }
    gAdmissionHookCondition.notify_all();
    producer.join();
    Check(accepted);
    const auto stopDeadline =
        std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (!worker.StopRequestedForTest()) {
        Check(std::chrono::steady_clock::now() < stopDeadline);
        std::this_thread::yield();
    }
    Check(!worker.Post([&](TqUvRelayWorker&) {
        rejectedExecutions.fetch_add(1, std::memory_order_relaxed);
    }));
    {
        std::lock_guard<std::mutex> guard(blockerMutex);
        releaseBlocker = true;
    }
    blockerCondition.notify_all();
    stopper.join();
    Check(acceptedExecutions.load(std::memory_order_relaxed) == 1);
    Check(rejectedExecutions.load(std::memory_order_relaxed) == 0);
}

void TestAsyncInitFailureClosesLoopAndDoesNotPublishWorker() {
    gLoopCloseCalls = 0;
    auto calls = TqUvProductionCalls();
    calls.AsyncInit = &FailAsyncInit;
    calls.LoopClose = &CountLoopClose;
    TqUvRelayWorkerConfig config{};
    config.Calls = &calls;
    TqUvRelayWorker worker(config);
    Check(!worker.StartAndWaitReady());
    Check(gLoopCloseCalls == 1);
    Check(!worker.Snapshot().Running);
}

void TestSecondWorkerAsyncInitFailureRollsBackFirstWorker() {
    gAsyncInitCalls.store(0, std::memory_order_relaxed);
    gLoopCloseCalls = 0;
    auto calls = TqUvProductionCalls();
    calls.AsyncInit = &FailSecondAsyncInit;
    calls.LoopClose = &CountLoopClose;
    auto& runtime = TqUvRelayRuntime::Instance();
    runtime.SetCallAdapterForTest(&calls);

    TqTuningConfig tuning{};
    tuning.RelayWorkerCount = 2;
    Check(!runtime.Start(tuning));
    Check(runtime.WorkerCountForTest() == 0);
    Check(runtime.StartedWorkersForTest() == 1);
    Check(runtime.RolledBackWorkersForTest() == 1);
    Check(gAsyncInitCalls.load(std::memory_order_relaxed) == 2);
    Check(gLoopCloseCalls == 2);
    runtime.ResetTestHooksForTest();
}

void TestRuntimeRoundRobinAndLoopLocalDispatch() {
    auto& runtime = TqUvRelayRuntime::Instance();
    TqTuningConfig tuning{};
    tuning.RelayWorkerCount = 2;
    tuning.RelayEventQueueCapacity = 16;
    Check(runtime.Start(tuning));
    Check(runtime.WorkerCountForTest() == 2);
    Check(runtime.AllocatorInstalledBeforeLoopForTest());

    auto* first = runtime.PickWorker();
    auto* second = runtime.PickWorker();
    auto* third = runtime.PickWorker();
    Check(first != nullptr);
    Check(second != nullptr);
    Check(first != second);
    Check(first == third);
    Check(runtime.SnapshotWorkers().size() == 2);

    std::mutex mutex;
    std::condition_variable condition;
    bool completed = false;
    bool localResultObserved = false;
    bool localStopRejected = false;
    Check(first->Post([&](TqUvRelayWorker& worker) {
        const auto result = worker.RegisterRelayWithId(TqUvRelayRegistration{});
        {
            std::lock_guard<std::mutex> guard(mutex);
            localResultObserved = !result.Ok && worker.IsLoopThreadForTest();
            localStopRejected = !worker.StopForTest();
            completed = true;
        }
        condition.notify_one();
    }));

    std::unique_lock<std::mutex> lock(mutex);
    Check(condition.wait_for(lock, std::chrono::seconds(5), [&] {
        return completed;
    }));
    Check(localResultObserved);
    Check(localStopRejected);
    lock.unlock();

    const auto queuedRegistration =
        second->RegisterRelayWithId(TqUvRelayRegistration{});
    Check(!queuedRegistration.Ok);
    Check(!queuedRegistration.TcpFdConsumed);
    Check(second->Snapshot().CommandsExecuted >= 1);

    runtime.StopForTest();
    Check(runtime.WorkerCountForTest() == 0);
}

} // namespace

int main() {
    TestWorkerConstructionRequiresSuccessfulAllocatorBootstrap();
    TestTcpCallAdapterPathsAreConsumable();
    TestAllocatorFailurePreventsLoopInitialization();
    TestSecondWorkerFailureRollsBackFirstWorker();
    TestQueueFifoCapacityAndCoalescedWakeFact();
    TestQueuePopFaultRetainsFifoContents();
    TestQueueFullDoesNotBlockMinimumStop();
    TestAsyncSendFailureRetriesWithoutLostWake();
    TestWakeSnapshotPublishesOnlyClassifiedAttempts();
    TestLoopTurnDoesNotCountMultipleCallbacksAsIterations();
    TestQueueMutexInitFailurePreventsWorkerStartup();
    TestQueuedRegistrationUsesFixedDeadlineAcrossSpuriousSignals();
    TestExecutingRegistrationWaitsForFinalOwnershipResult();
    TestStopCancelsQueuedRegistrationAndSignalsWaiter();
    TestRelayStateOwnsActivationMutexDestructionOnce();
    TestActualWakeCoalescingAndCallbackFifoDrain();
    TestLoopLagCalculationHandlesFirstTickAndBoundaries();
    TestDirectionalPendingContractSaturatesAndClears();
    TestConcurrentPendingOverflowCannotExposeWrappedCounter();
    TestConcurrentDirectionalAdmissionAndCompletionStayBalanced();
    TestConcurrentPostAndCallbackDrain();
    TestStopRetriesOneAsyncSendFailure();
    TestStopWakeDrainsBeforeAsyncClose();
    TestPersistentAsyncSendFailureUsesBoundedSafetyWake();
    TestClosePassBadAllocRetriesPartialSynchronousClosesExactlyOnce();
    TestStopAndProducerAdmissionAreLinearized();
    TestAsyncInitFailureClosesLoopAndDoesNotPublishWorker();
    TestSecondWorkerAsyncInitFailureRollsBackFirstWorker();
    TestRuntimeRoundRobinAndLoopLocalDispatch();
    return 0;
}
