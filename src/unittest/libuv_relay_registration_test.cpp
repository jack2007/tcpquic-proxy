#include "libuv_relay_worker.h"
#include "stream_lifetime.h"

#include <uv.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstring>
#include <cstdlib>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

using TqUvStopCommittedHookForTest = bool (*)(
    const std::shared_ptr<const TqUvRelayCommittedState>&);
void TqUvSetStopCommittedHookForTest(TqUvStopCommittedHookForTest hook);
void TqUvSetDataPlaneReadyForTest(bool ready);
void TqUvSetBeforePublicationHookForTest(void (*hook)());

namespace {

class CountingEscalation final : public TqTerminalEscalation {
public:
    void RequestConnectionShutdown(
        std::uint64_t,
        std::uint64_t,
        QUIC_STATUS,
        std::uint64_t) noexcept override {
        Calls.fetch_add(1, std::memory_order_relaxed);
    }
    std::atomic<std::uint32_t> Calls{0};
};

void CheckAt(bool condition, int line) {
    if (!condition) {
        std::fprintf(stderr, "registration check failed at line %d\n", line);
        std::abort();
    }
}

#define Check(condition) CheckAt((condition), __LINE__)

void DispatchRealShutdownComplete(
    const std::shared_ptr<TqStreamLifetime>& owner) {
    Check(owner != nullptr);
    Check(owner->TargetContextForTest() != nullptr);
    QUIC_STREAM_EVENT terminal{};
    terminal.Type = QUIC_STREAM_EVENT_SHUTDOWN_COMPLETE;
    Check(owner->DispatchForTest(&terminal) == QUIC_STATUS_SUCCESS);
    Check(owner->TargetContextForTest() == nullptr);
}

std::atomic<int> gTcpInitStatus{0};
std::atomic<int> gTcpOpenStatus{0};
std::atomic<int> gReadStartStatus{0};
std::atomic<int> gTcpCloseCalls{0};
std::atomic<int> gTcpCloseCallbacks{0};
std::atomic<uv_tcp_t*> gTcpHandle{nullptr};
std::mutex gTcpHandlesMutex;
std::vector<uv_tcp_t*> gTcpHandles;
std::atomic<bool> gDeferTcpClose{false};
uv_handle_t* gDeferredCloseHandle{nullptr};
uv_close_cb gDeferredCloseCallback{nullptr};
std::shared_ptr<TqStreamLifetime> gReceiveOwner;
bool gInjectPreparedReceive{false};
bool gInjectTerminalBeforeCommit{false};
std::uint32_t gPreparedReceiveLength{3};
QUIC_STATUS gExpectedPreparedReceiveStatus{QUIC_STATUS_PENDING};
bool gCaptureTcpToQuicTuning{false};
size_t gCapturedReadChunkSize{0};
uint64_t gCapturedBufferBudget{0};
uint64_t gCapturedSendPause{0};
uint64_t gCapturedSendResume{0};
std::atomic<bool> gReleaseDrainWrite{false};
std::atomic<uv_write_t*> gDrainWrite{nullptr};
std::atomic<void*> gDrainSendContext{nullptr};
std::atomic<std::uint32_t> gDrainReceiveCompletions{0};

void QUIC_API FakeReceiveComplete(HQUIC, std::uint64_t) {
    gDrainReceiveCompletions.fetch_add(1, std::memory_order_release);
}

int CaptureDrainWrite(
    uv_write_t* request,
    uv_stream_t*,
    const uv_buf_t[],
    unsigned int,
    uv_write_cb) {
    gDrainWrite.store(request, std::memory_order_release);
    return 0;
}

QUIC_STATUS CaptureDrainSend(
    MsQuicStream*,
    const QUIC_BUFFER*,
    std::uint32_t,
    QUIC_SEND_FLAGS,
    void* context) {
    gDrainSendContext.store(context, std::memory_order_release);
    return QUIC_STATUS_PENDING;
}

void CompleteDrainWriteOnLoop(TqUvRelayWorker&) {
    if (!gReleaseDrainWrite.load(std::memory_order_acquire)) {
        return;
    }
    auto* request = gDrainWrite.exchange(nullptr, std::memory_order_acq_rel);
    if (request != nullptr) {
        TqUvOnTcpWriteComplete(request, 0);
    }
}

int FakeTcpInit(uv_loop_t* loop, uv_tcp_t* tcp) {
    const int status = gTcpInitStatus.load();
    if (status != 0) {
        return status;
    }
    tcp->loop = loop;
    gTcpHandle.store(tcp);
    {
        std::lock_guard<std::mutex> guard(gTcpHandlesMutex);
        gTcpHandles.push_back(tcp);
    }
    return 0;
}

int FakeTcpOpen(uv_tcp_t*, uv_os_sock_t) {
    return gTcpOpenStatus.load();
}

int FakeReadStart(uv_stream_t* stream, uv_alloc_cb, uv_read_cb) {
    if (gCaptureTcpToQuicTuning) {
        auto* relay = static_cast<TqUvRelayState*>(stream->data);
        Check(relay != nullptr);
        gCapturedReadChunkSize = relay->TcpReadChunkSize;
        gCapturedBufferBudget =
            relay->TcpReadBufferBudget.MaxPendingBufferBytes;
        gCapturedSendPause = relay->MaxBufferedQuicSendBytes;
        gCapturedSendResume = relay->ResumeBufferedQuicSendBytes;
    }
    if (gInjectPreparedReceive) {
        QUIC_BUFFER buffer{
            gPreparedReceiveLength,
            reinterpret_cast<uint8_t*>(const_cast<char*>("abc"))};
        QUIC_STREAM_EVENT event{};
        event.Type = QUIC_STREAM_EVENT_RECEIVE;
        event.RECEIVE.TotalBufferLength = gPreparedReceiveLength;
        event.RECEIVE.BufferCount = 1;
        event.RECEIVE.Buffers = &buffer;
        auto* binding = static_cast<TqUvStreamBinding*>(
            gReceiveOwner->TargetContextForTest());
        Check(binding != nullptr);
        Check(binding->OnStreamEvent(
                  reinterpret_cast<MsQuicStream*>(0x1),
                  &event,
                  binding->RouteGeneration) == gExpectedPreparedReceiveStatus);
    }
    if (gInjectTerminalBeforeCommit) {
        QUIC_STREAM_EVENT terminal{};
        terminal.Type = QUIC_STREAM_EVENT_SHUTDOWN_COMPLETE;
        auto* binding = static_cast<TqUvStreamBinding*>(
            gReceiveOwner->TargetContextForTest());
        Check(binding->OnStreamEvent(
                  reinterpret_cast<MsQuicStream*>(0x1),
                  &terminal,
                  binding->RouteGeneration) == QUIC_STATUS_SUCCESS);
    }
    return gReadStartStatus.load();
}

int FakeReadStop(uv_stream_t*) {
    return 0;
}

void FakeClose(uv_handle_t* handle, uv_close_cb callback) {
    bool isTcp = false;
    {
        std::lock_guard<std::mutex> guard(gTcpHandlesMutex);
        for (auto* tcp : gTcpHandles) {
            if (handle == reinterpret_cast<uv_handle_t*>(tcp)) {
                isTcp = true;
                break;
            }
        }
    }
    if (isTcp) {
        ++gTcpCloseCalls;
        if (gDeferTcpClose.load()) {
            Check(gDeferredCloseHandle == nullptr);
            gDeferredCloseHandle = handle;
            gDeferredCloseCallback = callback;
            return;
        }
        if (callback != nullptr) {
            ++gTcpCloseCallbacks;
            callback(handle);
        }
        return;
    }
    uv_close(handle, callback);
}

TqUvRelayWorkerConfig Config() {
    static TqUvCallAdapter calls;
    calls = TqUvProductionCalls();
    calls.TcpInit = &FakeTcpInit;
    calls.TcpOpen = &FakeTcpOpen;
    calls.ReadStart = &FakeReadStart;
    calls.ReadStop = &FakeReadStop;
    calls.Write = &CaptureDrainWrite;
    calls.Close = &FakeClose;
    TqUvRelayWorkerConfig config{};
    config.Calls = &calls;
    config.ControlCommandTimeoutMs = 40;
    return config;
}

void ResetFakes() {
    gTcpInitStatus.store(0);
    gTcpOpenStatus.store(0);
    gReadStartStatus.store(0);
    gTcpCloseCalls.store(0);
    gTcpCloseCallbacks.store(0);
    gTcpHandle.store(nullptr);
    {
        std::lock_guard<std::mutex> guard(gTcpHandlesMutex);
        gTcpHandles.clear();
    }
    gDeferTcpClose.store(false);
    gDeferredCloseHandle = nullptr;
    gDeferredCloseCallback = nullptr;
    gReceiveOwner.reset();
    gInjectPreparedReceive = false;
    gInjectTerminalBeforeCommit = false;
    gPreparedReceiveLength = 3;
    gExpectedPreparedReceiveStatus = QUIC_STATUS_PENDING;
    gDrainSendContext.store(nullptr, std::memory_order_release);
    gDrainReceiveCompletions.store(0, std::memory_order_release);
}

int AlwaysFailAsyncSend(uv_async_t*) {
    return UV_EIO;
}

void FlushDeferredClose(TqUvRelayWorker& worker) {
    std::mutex mutex;
    std::condition_variable condition;
    bool complete = false;
    Check(worker.Post([&](TqUvRelayWorker&) {
        Check(gDeferredCloseHandle != nullptr);
        auto* handle = gDeferredCloseHandle;
        auto callback = gDeferredCloseCallback;
        gDeferredCloseHandle = nullptr;
        gDeferredCloseCallback = nullptr;
        if (callback != nullptr) {
            ++gTcpCloseCallbacks;
            callback(handle);
        }
        std::lock_guard<std::mutex> guard(mutex);
        complete = true;
        condition.notify_all();
    }));
    std::unique_lock<std::mutex> lock(mutex);
    Check(condition.wait_for(lock, std::chrono::seconds(5), [&] {
        return complete;
    }));
}

TqUvRelayRegistration Registration(
    const std::shared_ptr<TqStreamLifetime>& owner) {
    TqUvRelayRegistration registration{};
    registration.TcpSocket = static_cast<TqSocketHandle>(71);
    registration.Stream = reinterpret_cast<MsQuicStream*>(0x1);
    registration.StreamOwner = owner;
    registration.StopControl = std::make_shared<TqRelayStopControl>();
    registration.ControlGeneration = registration.StopControl->Generation;
    registration.TcpReadChunkSize = 4096;
    registration.MaxPendingBufferBytes = 32768;
    registration.MaxBufferedQuicSendBytes = 16384;
    registration.ResumeBufferedQuicSendBytes = 8192;
    registration.WorkerEventBudget = 7;
    registration.WorkerByteBudgetPerTick = 1234;
    return registration;
}

void TestPrepareAndPublishFailuresKeepCallerOwnership() {
    ResetFakes();
    TqUvRelayWorker worker(Config());
    Check(worker.StartAndWaitReady());

    auto missingOwner = Registration(nullptr);
    auto result = worker.RegisterRelayWithId(std::move(missingOwner));
    Check(!result.Ok && !result.TcpFdConsumed && result.RelayId == 0);
    Check(gTcpCloseCalls.load() == 0);

    auto terminalOwner = TqStreamLifetime::CreateForTest(
        TqStreamLifetime::Phase::CreatedNotStarted);
    result = worker.RegisterRelayWithId(Registration(terminalOwner));
    Check(!result.Ok && !result.TcpFdConsumed);
    Check(gTcpCloseCalls.load() == 0);
    Check(gTcpCloseCallbacks.load() == 0);
    Check(worker.StopForTest());
}

void TestTcpInitAndOpenFailuresDoNotConsumeSocket() {
    ResetFakes();
    TqUvRelayWorker worker(Config());
    Check(worker.StartAndWaitReady());

    gTcpInitStatus.store(UV_ENOMEM);
    auto owner = TqStreamLifetime::CreateForTest(TqStreamLifetime::Phase::Started);
    auto result = worker.RegisterRelayWithId(Registration(owner));
    Check(!result.Ok && !result.TcpFdConsumed);
    Check(gTcpCloseCalls.load() == 0);

    gTcpInitStatus.store(0);
    gTcpOpenStatus.store(UV_EBADF);
    owner = TqStreamLifetime::CreateForTest(TqStreamLifetime::Phase::Started);
    result = worker.RegisterRelayWithId(Registration(owner));
    Check(!result.Ok && !result.TcpFdConsumed);
    Check(gTcpCloseCalls.load() == 1);
    DispatchRealShutdownComplete(owner);
    Check(worker.StopForTest());
}

void TestReadFailureConsumesSocketAndDiscardsPrecommitOnce() {
    ResetFakes();
    TqUvRelayWorker worker(Config());
    Check(worker.StartAndWaitReady());
    auto owner = TqStreamLifetime::CreateForTest(TqStreamLifetime::Phase::Started);
    gReceiveOwner = owner;
    gInjectPreparedReceive = true;
    gReadStartStatus.store(UV_EIO);

    const auto result = worker.RegisterRelayWithId(Registration(owner));
    Check(!result.Ok && result.TcpFdConsumed && result.RelayId != 0);
    Check(gTcpCloseCalls.load() == 1);
    Check(gTcpCloseCallbacks.load() == 1);
    const auto settled = worker.RegistrationSnapshotForTest(result.RelayId);
    Check(settled.Activation == TqUvActivation::Terminal);
    Check(settled.PrecommitSettled);
    Check(settled.PrecommitDiscardCount == 1);
    Check(settled.PrecommitDrainCount == 0);
    DispatchRealShutdownComplete(owner);
    Check(worker.StopForTest());
}

void TestSuccessCommitsAfterReadAndDrainsPrecommitOnce() {
    ResetFakes();
    TqUvRelayWorker worker(Config());
    Check(worker.StartAndWaitReady());
    auto owner = TqStreamLifetime::CreateForTest(TqStreamLifetime::Phase::Started);
    gReceiveOwner = owner;
    gInjectPreparedReceive = true;

    const auto result = worker.RegisterRelayWithId(Registration(owner));
    Check(result.Ok && result.TcpFdConsumed && result.RelayId != 0);
    Check(result.Committed != nullptr);
    const auto handoff = std::atomic_load(
        &result.Committed->Control->TerminalHandoff);
    Check(handoff != nullptr);
    Check(handoff->Generation == result.Committed->ControlGeneration);
    Check(handoff->Ledger.get() == owner->TerminalLedger().get());
    const auto snapshot = worker.RegistrationSnapshotForTest(result.RelayId);
    Check(snapshot.Activation == TqUvActivation::Active);
    Check(snapshot.Ownership == TqUvSocketOwnership::ActiveRelayOwned);
    Check(snapshot.PrecommitSettled);
    Check(snapshot.PrecommitDrainCount == 1);
    Check(snapshot.PrecommitDiscardCount == 0);
    const auto active = worker.Snapshot();
    Check(active.ActiveRelays == 1);
    Check(active.QuicToTcpPendingBytes == gPreparedReceiveLength);
    Check(active.TcpToQuicPendingBytes == 0);
    Check(active.PendingBytes == gPreparedReceiveLength);
    auto relay = worker.RelayForTest(result.RelayId);
    Check(relay != nullptr);
    Check(relay->QuicToTcpCallBudget == 7);
    Check(relay->QuicToTcpByteBudgetPerTick == 1234);
    std::atomic<bool> ownershipCleared{false};
    Check(worker.Post([&](TqUvRelayWorker& local) {
        relay->PrecommitReceives.clear();
        relay->PendingQuicReceiveBytes = 0;
        Check(local.CompletePendingBytes(
            *relay,
            TqUvPendingDirection::QuicToTcp,
            gPreparedReceiveLength));
        ownershipCleared.store(true, std::memory_order_release);
    }));
    while (!ownershipCleared.load(std::memory_order_acquire)) {
        std::this_thread::yield();
    }
    std::thread stopper([&] { Check(worker.StopForTest()); });
    const auto terminalDeadline = std::chrono::steady_clock::now() +
        std::chrono::seconds(5);
    while (relay->TerminalBeginCount.load(std::memory_order_acquire) == 0 &&
           std::chrono::steady_clock::now() < terminalDeadline) {
        std::this_thread::yield();
    }
    Check(relay->TerminalBeginCount.load(std::memory_order_acquire) == 1);
    DispatchRealShutdownComplete(owner);
    stopper.join();
}

void TestTerminalBetweenReadAndCommitWinsActivationMutex() {
    ResetFakes();
    TqUvRelayWorker worker(Config());
    Check(worker.StartAndWaitReady());
    auto owner = TqStreamLifetime::CreateForTest(TqStreamLifetime::Phase::Started);
    gReceiveOwner = owner;
    gInjectPreparedReceive = true;
    gInjectTerminalBeforeCommit = true;

    const auto result = worker.RegisterRelayWithId(Registration(owner));
    Check(!result.Ok && result.TcpFdConsumed);
    const auto snapshot = worker.RegistrationSnapshotForTest(result.RelayId);
    Check(snapshot.Activation == TqUvActivation::Terminal);
    Check(snapshot.PrecommitSettled);
    Check(snapshot.PrecommitDiscardCount == 1);
    Check(gTcpCloseCalls.load() == 1);
    DispatchRealShutdownComplete(owner);
    Check(worker.StopForTest());
}

void TestPrecommitLimitFailsAndSettlesExactlyOnce() {
    ResetFakes();
    TqUvRelayWorker worker(Config());
    Check(worker.StartAndWaitReady());
    auto owner = TqStreamLifetime::CreateForTest(TqStreamLifetime::Phase::Started);
    gReceiveOwner = owner;
    gInjectPreparedReceive = true;
    gPreparedReceiveLength = 3;
    gExpectedPreparedReceiveStatus = QUIC_STATUS_OUT_OF_MEMORY;
    auto registration = Registration(owner);
    registration.PrecommitMaxPendingBytes = 2;

    const auto result = worker.RegisterRelayWithId(std::move(registration));
    Check(!result.Ok && result.TcpFdConsumed);
    const auto snapshot = worker.RegistrationSnapshotForTest(result.RelayId);
    Check(snapshot.Activation == TqUvActivation::Terminal);
    Check(snapshot.PrecommitSettled);
    Check(snapshot.PrecommitDiscardCount == 1);
    Check(gTcpCloseCalls.load() == 1);
    DispatchRealShutdownComplete(owner);
    Check(worker.StopForTest());
}

void TestCommittedAllocationFailurePrecedesPublication() {
    ResetFakes();
    auto config = Config();
    config.FailCommittedAllocationForTest = true;
    TqUvRelayWorker worker(config);
    Check(worker.StartAndWaitReady());
    auto owner = TqStreamLifetime::CreateForTest(TqStreamLifetime::Phase::Started);

    const auto result = worker.RegisterRelayWithId(Registration(owner));
    Check(!result.Ok && !result.TcpFdConsumed);
    Check(owner->TargetContextForTest() == nullptr);
    Check(gTcpCloseCalls.load() == 0);
    Check(worker.StopForTest());
}

void TestRelayMapAllocationFailureCompletesWaiterAndKeepsWorkerAlive() {
    ResetFakes();
    auto config = Config();
    config.FailRelayMapInsertForTest = true;
    TqUvRelayWorker worker(config);
    Check(worker.StartAndWaitReady());
    auto owner = TqStreamLifetime::CreateForTest(TqStreamLifetime::Phase::Started);

    const auto failed = worker.RegisterRelayWithId(Registration(owner));
    Check(!failed.Ok && !failed.TcpFdConsumed);
    Check(owner->TargetContextForTest() == nullptr);
    Check(gTcpCloseCalls.load() == 0);
    Check(worker.StopForTest());
}

void TestDeferredClosePreservesCallerAndUvOwnedLifetimes() {
    ResetFakes();
    TqUvRelayWorker worker(Config());
    Check(worker.StartAndWaitReady());
    gDeferTcpClose.store(true);
    gTcpOpenStatus.store(UV_EBADF);
    auto owner = TqStreamLifetime::CreateForTest(TqStreamLifetime::Phase::Started);
    auto result = worker.RegisterRelayWithId(Registration(owner));
    const auto firstRelayId = result.RelayId;
    Check(!result.Ok && !result.TcpFdConsumed);
    Check(gTcpCloseCalls.load() == 1 && gTcpCloseCallbacks.load() == 0);
    FlushDeferredClose(worker);
    Check(gTcpCloseCallbacks.load() == 1);
    auto firstRelay = worker.RelayForTest(firstRelayId);
    Check(firstRelay != nullptr);
    DispatchRealShutdownComplete(owner);
    const auto firstReleaseDeadline = std::chrono::steady_clock::now() +
        std::chrono::seconds(5);
    while (firstRelay->TerminalReleaseCount.load(std::memory_order_acquire) == 0 &&
           std::chrono::steady_clock::now() < firstReleaseDeadline) {
        std::this_thread::yield();
    }
    Check(firstRelay->TerminalReleaseCount.load(std::memory_order_acquire) == 1);

    gTcpOpenStatus.store(0);
    gReadStartStatus.store(UV_EIO);
    owner = TqStreamLifetime::CreateForTest(TqStreamLifetime::Phase::Started);
    result = worker.RegisterRelayWithId(Registration(owner));
    Check(!result.Ok && result.TcpFdConsumed);
    Check(gTcpCloseCalls.load() == 2 && gTcpCloseCallbacks.load() == 1);
    FlushDeferredClose(worker);
    Check(gTcpCloseCallbacks.load() == 2);
    gDeferTcpClose.store(false);
    auto terminalRelay = worker.RelayForTest(result.RelayId);
    Check(terminalRelay != nullptr);
    DispatchRealShutdownComplete(owner);
    const auto releaseDeadline = std::chrono::steady_clock::now() +
        std::chrono::seconds(5);
    while (terminalRelay->TerminalReleaseCount.load(
               std::memory_order_acquire) == 0 &&
           std::chrono::steady_clock::now() < releaseDeadline) {
        std::this_thread::yield();
    }
    Check(terminalRelay->TerminalReleaseCount.load(
              std::memory_order_acquire) == 1);
    Check(worker.StopForTest());
}

void TestProductionFacadeFailsClosedUntilBothDataPlanesAreReady() {
    ResetFakes();
    const auto activeBefore = TqGetActiveRelayCount();
    auto owner = TqStreamLifetime::CreateForTest(TqStreamLifetime::Phase::Started);
    TqRelayHandle handle{};
    bool consumed = true;
    TqTuningConfig tuning{};
    Check(!TqRelayStartManaged(
        static_cast<TqSocketHandle>(71),
        reinterpret_cast<MsQuicStream*>(0x1),
        owner,
        nullptr,
        nullptr,
        &handle,
        tuning,
        TqCompressAlgo::None,
        &consumed));
    Check(!consumed);
    Check(handle.Backend == TqRelayBackendType::None);
    Check(owner->TargetContextForTest() == nullptr);
    Check(TqGetActiveRelayCount() == activeBefore);
}

TqUvRelayWorker* gStopWorker{nullptr};
std::mutex gPublicationMutex;
std::condition_variable gPublicationCondition;
bool gPublicationHookEntered{false};
bool gReleasePublicationHook{false};

void BlockingPublicationHook() {
    std::unique_lock<std::mutex> lock(gPublicationMutex);
    gPublicationHookEntered = true;
    gPublicationCondition.notify_all();
    gPublicationCondition.wait(lock, [] { return gReleasePublicationHook; });
}

bool AcceptLocalStop(
    const std::shared_ptr<const TqUvRelayCommittedState>& committed) {
    return gStopWorker != nullptr && gStopWorker->AcceptStop(committed);
}

void TestQueueFullConcurrentFacadeStopConvergesExactlyOnce() {
    ResetFakes();
    auto config = Config();
    config.QueueCapacity = 1;
    config.ControlCommandTimeoutMs = 500;
    auto calls = *config.Calls;
    calls.AsyncSend = &AlwaysFailAsyncSend;
    config.Calls = &calls;
    TqUvRelayWorker worker(config);
    Check(worker.StartAndWaitReady());
    auto owner = TqStreamLifetime::CreateForTest(TqStreamLifetime::Phase::Started);
    gReceiveOwner = owner;
    const auto registered = worker.RegisterRelayWithId(Registration(owner));
    Check(registered.Ok && registered.Committed != nullptr);

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
    Check(worker.Post([](TqUvRelayWorker&) {}));

    const auto activeBefore = TqGetActiveRelayCount();
    (void)TqRelayRegisterActive();
    TqRelayHandle handle{};
    handle.Control = registered.Committed->Control;
    handle.ControlGeneration = registered.Committed->ControlGeneration;
    handle.Backend = TqRelayBackendType::LibuvWorker;
    std::atomic_store(&handle.LibuvCommitted, registered.Committed);
    gStopWorker = &worker;
    TqUvSetStopCommittedHookForTest(&AcceptLocalStop);
    std::thread first([&] { TqRelayStop(&handle); });
    std::thread second([&] { TqRelayStop(&handle); });
    first.join();
    second.join();
    {
        std::lock_guard<std::mutex> lock(blockerMutex);
        releaseBlocker = true;
    }
    blockerCondition.notify_all();

    const auto deadline = std::chrono::steady_clock::now() +
        std::chrono::seconds(5);
    while (gTcpCloseCallbacks.load() == 0 &&
           std::chrono::steady_clock::now() < deadline) {
        std::this_thread::yield();
    }
    Check(gTcpCloseCalls.load() == 1 && gTcpCloseCallbacks.load() == 1);
    Check(owner->TargetContextForTest() != nullptr);
    Check(!registered.Committed->Control->ActiveAccountingReleased.load());
    Check(handle.Backend == TqRelayBackendType::LibuvWorker);
    DispatchRealShutdownComplete(owner);
    const auto handoff = std::atomic_load(
        &registered.Committed->Control->TerminalHandoff);
    const auto handoffDeadline = std::chrono::steady_clock::now() +
        std::chrono::seconds(5);
    while (handoff != nullptr && !TqTerminalReleaseReady(handoff->Snapshot()) &&
           std::chrono::steady_clock::now() < handoffDeadline) {
        std::this_thread::yield();
    }
    Check(handoff != nullptr && TqTerminalReleaseReady(handoff->Snapshot()));
    TqRelayStop(&handle);
    Check(owner->TargetContextForTest() == nullptr);
    const auto snapshot = worker.RegistrationSnapshotForTest(registered.RelayId);
    Check(snapshot.Activation == TqUvActivation::Terminal);
    Check(snapshot.PrecommitSettled);
    Check(snapshot.PrecommitDrainCount == 1);
    Check(snapshot.PrecommitDiscardCount == 0);
    Check(TqGetActiveRelayCount() == activeBefore);
    Check(registered.Committed->Control->ActiveAccountingReleased.load());
    TqUvSetStopCommittedHookForTest(nullptr);
    gStopWorker = nullptr;
    Check(worker.StopForTest());
}

void TestNoStopSteadyStateSkipsRelayMapScan() {
    ResetFakes();
    TqUvRelayWorker worker(Config());
    Check(worker.StartAndWaitReady());
    std::vector<std::shared_ptr<TqStreamLifetime>> owners;
    owners.reserve(32);
    for (int index = 0; index < 32; ++index) {
        auto owner = TqStreamLifetime::CreateForTest(
            TqStreamLifetime::Phase::Started);
        const auto result = worker.RegisterRelayWithId(Registration(owner));
        Check(result.Ok);
        owners.push_back(std::move(owner));
    }
    const auto timerDeadline = std::chrono::steady_clock::now() +
        std::chrono::milliseconds(120);
    while (std::chrono::steady_clock::now() < timerDeadline) {
        std::this_thread::yield();
    }
    const auto snapshot = worker.Snapshot();
    Check(snapshot.SafetyTimerCallbacks >= 2);
    Check(snapshot.StopScanPasses == 0);
    Check(snapshot.StopScanFailures == 0);
    std::thread stopper([&] { Check(worker.StopForTest()); });
    const auto stopDeadline = std::chrono::steady_clock::now() +
        std::chrono::seconds(5);
    while (!worker.StopRequestedForTest() &&
           std::chrono::steady_clock::now() < stopDeadline) {
        std::this_thread::yield();
    }
    Check(worker.StopRequestedForTest());
    for (const auto& owner : owners) {
        DispatchRealShutdownComplete(owner);
    }
    stopper.join();
}

void TestStopScanExceptionIsContainedAndRetriedNextTick() {
    ResetFakes();
    auto config = Config();
    config.FailStopScanOnceForTest = true;
    TqUvRelayWorker worker(config);
    Check(worker.StartAndWaitReady());
    auto owner = TqStreamLifetime::CreateForTest(TqStreamLifetime::Phase::Started);
    const auto registered = worker.RegisterRelayWithId(Registration(owner));
    Check(registered.Ok);
    Check(worker.AcceptStop(registered.Committed));

    const auto deadline = std::chrono::steady_clock::now() +
        std::chrono::seconds(5);
    TqUvRelayWorkerSnapshot snapshot{};
    do {
        snapshot = worker.Snapshot();
        if (snapshot.StopScanFailures == 1 &&
            snapshot.StopScanPasses >= 1 &&
            gTcpCloseCallbacks.load() == 1) {
            break;
        }
        std::this_thread::yield();
    } while (std::chrono::steady_clock::now() < deadline);
    Check(snapshot.StopScanFailures == 1);
    Check(snapshot.StopScanPasses >= 1);
    Check(gTcpCloseCalls.load() == 1 && gTcpCloseCallbacks.load() == 1);
    Check(owner->TargetContextForTest() != nullptr);
    DispatchRealShutdownComplete(owner);
    Check(worker.StopForTest());
}

void TestStopBetweenRegistrationAndPublicationCannotBeLost() {
    ResetFakes();
    auto calls = TqUvProductionCalls();
    calls.TcpInit = &FakeTcpInit;
    calls.TcpOpen = &FakeTcpOpen;
    calls.ReadStart = &FakeReadStart;
    calls.ReadStop = &FakeReadStop;
    calls.Close = &FakeClose;
    auto& runtime = TqUvRelayRuntime::Instance();
    runtime.SetCallAdapterForTest(&calls);
    TqUvSetDataPlaneReadyForTest(true);
    {
        std::lock_guard<std::mutex> guard(gPublicationMutex);
        gPublicationHookEntered = false;
        gReleasePublicationHook = false;
    }
    TqUvSetBeforePublicationHookForTest(&BlockingPublicationHook);

    const auto activeBefore = TqGetActiveRelayCount();
    auto owner = TqStreamLifetime::CreateForTest(TqStreamLifetime::Phase::Started);
    TqRelayHandle handle{};
    TqTuningConfig tuning{};
    tuning.RelayWorkerCount = 1;
    bool started = true;
    bool consumed = false;
    std::thread starter([&] {
        started = TqRelayStartManaged(
            static_cast<TqSocketHandle>(71),
            reinterpret_cast<MsQuicStream*>(0x1),
            owner,
            nullptr,
            nullptr,
            &handle,
            tuning,
            TqCompressAlgo::None,
            &consumed);
    });
    {
        std::unique_lock<std::mutex> lock(gPublicationMutex);
        Check(gPublicationCondition.wait_for(
            lock, std::chrono::seconds(5), [] { return gPublicationHookEntered; }));
    }
    TqRelayStop(&handle);
    {
        std::lock_guard<std::mutex> guard(gPublicationMutex);
        gReleasePublicationHook = true;
    }
    gPublicationCondition.notify_all();
    starter.join();
    Check(!started && consumed);
    Check(handle.Stop.load(std::memory_order_acquire));
    Check(TqRelayLibuvCommittedSnapshot(&handle) == nullptr);
    Check(handle.Backend == TqRelayBackendType::None);

    const auto deadline = std::chrono::steady_clock::now() +
        std::chrono::seconds(5);
    while (gTcpCloseCallbacks.load() == 0 &&
           std::chrono::steady_clock::now() < deadline) {
        std::this_thread::yield();
    }
    Check(gTcpCloseCalls.load() == 1 && gTcpCloseCallbacks.load() == 1);
    Check(owner->TargetContextForTest() != nullptr);
    DispatchRealShutdownComplete(owner);
    const auto releaseDeadline = std::chrono::steady_clock::now() +
        std::chrono::seconds(5);
    while (TqGetActiveRelayCount() != activeBefore &&
           std::chrono::steady_clock::now() < releaseDeadline) {
        std::this_thread::yield();
    }
    Check(TqGetActiveRelayCount() == activeBefore);
    TqUvSetBeforePublicationHookForTest(nullptr);
    TqUvSetDataPlaneReadyForTest(false);
    runtime.StopForTest();
    runtime.ResetTestHooksForTest();
}

void TestFacadeCarriesExistingTcpToQuicTuningIntoRegistration() {
    ResetFakes();
    auto calls = TqUvProductionCalls();
    calls.TcpInit = &FakeTcpInit;
    calls.TcpOpen = &FakeTcpOpen;
    calls.ReadStart = &FakeReadStart;
    calls.ReadStop = &FakeReadStop;
    calls.Close = &FakeClose;
    auto& runtime = TqUvRelayRuntime::Instance();
    runtime.SetCallAdapterForTest(&calls);
    TqUvSetDataPlaneReadyForTest(true);
    gCaptureTcpToQuicTuning = true;

    auto owner = TqStreamLifetime::CreateForTest(TqStreamLifetime::Phase::Started);
    TqRelayHandle handle{};
    TqTuningConfig tuning{};
    tuning.RelayWorkerCount = 1;
    tuning.RelayReadChunkSize = 4096;
    tuning.MaxPendingBufferBytesPerRelay = 65536;
    bool consumed = false;
    Check(TqRelayStartManaged(
        static_cast<TqSocketHandle>(72),
        reinterpret_cast<MsQuicStream*>(0x1),
        owner,
        nullptr,
        nullptr,
        &handle,
        tuning,
        TqCompressAlgo::None,
        &consumed));
    Check(consumed);
    Check(gCapturedReadChunkSize == 4096);
    Check(gCapturedBufferBudget == 65536);
    Check(gCapturedSendPause == 65536);
    Check(gCapturedSendResume == 32768);

    TqRelayStop(&handle);
    DispatchRealShutdownComplete(owner);
    TqRelayStop(&handle);
    gCaptureTcpToQuicTuning = false;
    TqUvSetDataPlaneReadyForTest(false);
    runtime.StopForTest();
    runtime.ResetTestHooksForTest();
}

void TestFacadeCarriesPreboundTerminalEscalationIntoHandoff() {
    ResetFakes();
    auto calls = TqUvProductionCalls();
    calls.TcpInit = &FakeTcpInit;
    calls.TcpOpen = &FakeTcpOpen;
    calls.ReadStart = &FakeReadStart;
    calls.ReadStop = &FakeReadStop;
    calls.Close = &FakeClose;
    auto& runtime = TqUvRelayRuntime::Instance();
    runtime.SetCallAdapterForTest(&calls);
    TqUvSetDataPlaneReadyForTest(true);

    auto owner = TqStreamLifetime::CreateForTest(
        TqStreamLifetime::Phase::Started);
    auto escalation = std::make_shared<CountingEscalation>();
    TqRelayHandle handle{};
    Check(handle.Control != nullptr);
    handle.Control->TerminalEscalation = escalation;
    auto originalControl = handle.Control;
    TqTuningConfig tuning{};
    tuning.RelayWorkerCount = 1;
    bool consumed = false;
    Check(TqRelayStartManaged(
        static_cast<TqSocketHandle>(73),
        reinterpret_cast<MsQuicStream*>(0x1),
        owner,
        nullptr,
        nullptr,
        &handle,
        tuning,
        TqCompressAlgo::None,
        &consumed));
    Check(consumed);
    Check(handle.Control == originalControl);
    const auto handoff = std::atomic_load(&handle.Control->TerminalHandoff);
    Check(handoff != nullptr && handoff->Escalation == escalation);

    TqRelayStop(&handle);
    const auto deadline = std::chrono::steady_clock::now() +
        std::chrono::seconds(5);
    while (escalation->Calls.load(std::memory_order_acquire) == 0 &&
           std::chrono::steady_clock::now() < deadline) {
        std::this_thread::yield();
    }
    Check(escalation->Calls.load(std::memory_order_acquire) == 1);
    DispatchRealShutdownComplete(owner);
    TqRelayStop(&handle);
    TqUvSetDataPlaneReadyForTest(false);
    runtime.StopForTest();
    runtime.ResetTestHooksForTest();
}

void TestWorkerStopDrainsPendingWriteSendFallbackAndLateShutdown() {
    ResetFakes();
    gReleaseDrainWrite.store(false, std::memory_order_release);
    gDrainWrite.store(nullptr, std::memory_order_release);
    auto config = Config();
    config.QueueCapacity = 1;
    config.AfterSafetyTimerForTest = &CompleteDrainWriteOnLoop;
    TqUvRelayWorker worker(config);
    Check(worker.StartAndWaitReady());
    static QUIC_API_TABLE api{};
    api.StreamReceiveComplete = &FakeReceiveComplete;
    MsQuic = reinterpret_cast<const MsQuicApi*>(&api);
    alignas(MsQuicStream) unsigned char streamStorage[sizeof(MsQuicStream)]{};
    auto* stream = reinterpret_cast<MsQuicStream*>(streamStorage);
    stream->Handle = reinterpret_cast<HQUIC>(static_cast<std::uintptr_t>(1));
    stream->CleanUpMode = CleanUpManual;
    stream->Callback = MsQuicStream::NoOpCallback;
    stream->Context = nullptr;
    auto owner = TqStreamLifetime::CreateForTest(
        TqStreamLifetime::Phase::Started);
    Check(owner->InstallDetachedStreamForTest(stream));
    owner->SetShutdownHookForTest(
        [](std::uint64_t, QUIC_STREAM_SHUTDOWN_FLAGS) {
            return QUIC_STATUS_PENDING;
        });
    const auto registered = worker.RegisterRelayWithId(Registration(owner));
    Check(registered.Ok);
    auto relay = worker.RelayForTest(registered.RelayId);
    Check(relay != nullptr);

    TqUvSetStreamSendHookForTest(&CaptureDrainSend);
    std::vector<std::uint8_t> receivePayload(101, 0x5a);
    QUIC_BUFFER receiveBuffer{
        static_cast<std::uint32_t>(receivePayload.size()),
        receivePayload.data()};
    QUIC_STREAM_EVENT receive{};
    receive.Type = QUIC_STREAM_EVENT_RECEIVE;
    receive.RECEIVE.TotalBufferLength = receivePayload.size();
    receive.RECEIVE.BufferCount = 1;
    receive.RECEIVE.Buffers = &receiveBuffer;
    Check(relay->Binding->OnStreamEvent(
              stream, &receive, relay->RouteGeneration) ==
          QUIC_STATUS_PENDING);
    const auto writeDeadline = std::chrono::steady_clock::now() +
        std::chrono::seconds(5);
    while (gDrainWrite.load(std::memory_order_acquire) == nullptr &&
           std::chrono::steady_clock::now() < writeDeadline) {
        std::this_thread::yield();
    }
    Check(gDrainWrite.load(std::memory_order_acquire) != nullptr);

    std::atomic<bool> staged{false};
    Check(worker.Post([&](TqUvRelayWorker& local) {
        std::vector<std::uint8_t> tcpPayload(103, 0xa5);
        auto buffer = TqUvStageTcpReadBufferForTest(
            *relay, tcpPayload.size());
        Check(buffer.base != nullptr);
        std::memcpy(buffer.base, tcpPayload.data(), tcpPayload.size());
        TqUvHandleTcpRead(
            local, *relay, static_cast<ssize_t>(tcpPayload.size()), buffer);
        staged.store(true, std::memory_order_release);
    }));
    while (!staged.load(std::memory_order_acquire)) {
        std::this_thread::yield();
    }

    std::atomic<bool> stopped{false};
    std::thread stopper([&] {
        Check(worker.StopForTest());
        stopped.store(true, std::memory_order_release);
    });
    const auto terminalDeadline = std::chrono::steady_clock::now() +
        std::chrono::seconds(5);
    while (relay->TerminalBeginCount.load(std::memory_order_acquire) == 0 &&
           !stopped.load(std::memory_order_acquire) &&
           std::chrono::steady_clock::now() < terminalDeadline) {
        std::this_thread::yield();
    }
    Check(relay->TerminalBeginCount.load(std::memory_order_acquire) == 1);
    Check(!stopped.load(std::memory_order_acquire));

    void* sendContext = gDrainSendContext.load(std::memory_order_acquire);
    Check(sendContext != nullptr);
    QUIC_STREAM_EVENT sendComplete{};
    sendComplete.Type = QUIC_STREAM_EVENT_SEND_COMPLETE;
    sendComplete.SEND_COMPLETE.ClientContext = sendContext;
    sendComplete.SEND_COMPLETE.Canceled = FALSE;
    Check(owner->DispatchForTest(&sendComplete) == QUIC_STATUS_SUCCESS);
    gReleaseDrainWrite.store(true, std::memory_order_release);
    const auto drainDeadline = std::chrono::steady_clock::now() +
        std::chrono::seconds(5);
    while ((gDrainReceiveCompletions.load(std::memory_order_acquire) == 0 ||
            worker.Snapshot().SendCompletionCommands == 0) &&
           std::chrono::steady_clock::now() < drainDeadline) {
        std::this_thread::yield();
    }
    Check(gDrainReceiveCompletions.load(std::memory_order_acquire) == 1);
    Check(worker.Snapshot().SendCompletionCommands == 1);
    DispatchRealShutdownComplete(owner);
    stopper.join();
    Check(stopped.load(std::memory_order_acquire));
    Check(relay->TerminalReleaseCount.load(std::memory_order_acquire) == 1);
    Check(relay->TcpWrites.empty());
    Check(relay->QuicSends.empty());
    Check(relay->PendingTcpWriteBytes == 0);
    Check(relay->PendingQuicSendBytes == 0);
    const auto snapshot = worker.Snapshot();
    Check(snapshot.SendCompletionFallbacks >= 1);
    Check(snapshot.SendCompletionCommands == 1);
    TqUvSetStreamSendHookForTest(nullptr);
    MsQuic = nullptr;
}

} // namespace

int main() {
    TestPrepareAndPublishFailuresKeepCallerOwnership();
    TestTcpInitAndOpenFailuresDoNotConsumeSocket();
    TestReadFailureConsumesSocketAndDiscardsPrecommitOnce();
    TestSuccessCommitsAfterReadAndDrainsPrecommitOnce();
    TestTerminalBetweenReadAndCommitWinsActivationMutex();
    TestPrecommitLimitFailsAndSettlesExactlyOnce();
    TestCommittedAllocationFailurePrecedesPublication();
    TestRelayMapAllocationFailureCompletesWaiterAndKeepsWorkerAlive();
    TestDeferredClosePreservesCallerAndUvOwnedLifetimes();
    TestProductionFacadeFailsClosedUntilBothDataPlanesAreReady();
    TestQueueFullConcurrentFacadeStopConvergesExactlyOnce();
    TestNoStopSteadyStateSkipsRelayMapScan();
    TestStopScanExceptionIsContainedAndRetriedNextTick();
    TestStopBetweenRegistrationAndPublicationCannotBeLost();
    TestFacadeCarriesExistingTcpToQuicTuningIntoRegistration();
    TestFacadeCarriesPreboundTerminalEscalationIntoHandoff();
    TestWorkerStopDrainsPendingWriteSendFallbackAndLateShutdown();
    return 0;
}
