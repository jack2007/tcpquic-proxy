#include "libuv_relay_worker.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

namespace {

void CheckAt(bool value, int line) {
    if (!value) {
        std::fprintf(stderr, "terminal check failed at line %d\n", line);
        std::abort();
    }
}

#define Check(value) CheckAt((value), __LINE__)

std::atomic<std::uint64_t> gReadStops{0};
std::atomic<std::uint64_t> gShutdowns{0};
std::atomic<std::uint64_t> gCloses{0};
uv_handle_t* gCloseHandle{nullptr};
uv_close_cb gCloseCallback{nullptr};

void QUIC_API FakeSetCallbackHandler(HQUIC, void*, void*) {}
void QUIC_API FakeStreamClose(HQUIC) {}

class NullTarget final : public TqStreamLifetime::Target {
public:
    QUIC_STATUS OnStreamEvent(
        MsQuicStream*, QUIC_STREAM_EVENT*, std::uint64_t) noexcept override {
        return QUIC_STATUS_SUCCESS;
    }
};

class CountingEscalation final : public TqTerminalEscalation {
public:
    void RequestConnectionShutdown(
        std::uint64_t, std::uint64_t, QUIC_STATUS, std::uint64_t) noexcept override {
        ++Calls;
    }
    std::atomic<std::uint32_t> Calls{0};
};

std::shared_ptr<TqStreamLifetime> StartedOwner(std::uint64_t streamId) {
    static QUIC_API_TABLE api{};
    api.SetCallbackHandler = &FakeSetCallbackHandler;
    api.StreamClose = &FakeStreamClose;
    MsQuic = reinterpret_cast<const MsQuicApi*>(&api);
    return TqStreamLifetime::AdoptAccepted(
        reinterpret_cast<HQUIC>(static_cast<std::uintptr_t>(streamId + 1)),
        std::make_shared<NullTarget>(),
        TqTerminalIdentity{
            streamId, 73, 1, 1, TqTunnelRole::ClientOpen,
            TqRelayBackendType::LibuvWorker},
        5);
}

int ReadStop(uv_stream_t*) {
    ++gReadStops;
    return 0;
}

int Shutdown(uv_shutdown_t*, uv_stream_t*, uv_shutdown_cb) {
    ++gShutdowns;
    return 0;
}

void Close(uv_handle_t* handle, uv_close_cb callback) {
    ++gCloses;
    gCloseHandle = handle;
    gCloseCallback = callback;
}

int TcpInit(uv_loop_t*, uv_tcp_t*) { return 0; }
int TcpOpen(uv_tcp_t*, uv_os_sock_t) { return 0; }
int ReadStart(uv_stream_t*, uv_alloc_cb, uv_read_cb) { return 0; }

void SynchronousRelayClose(uv_handle_t* handle, uv_close_cb callback) {
    if (handle->type == UV_UNKNOWN_HANDLE) {
        callback(handle);
        return;
    }
    uv_close(handle, callback);
}

std::shared_ptr<TqUvRelayState> Relay(TqUvRelayWorker& worker) {
    auto relay = std::make_shared<TqUvRelayState>();
    relay->Worker = &worker;
    relay->RelayId = 73;
    relay->RouteGeneration = 11;
    relay->ControlGeneration = 19;
    relay->StopControl = std::make_shared<TqRelayStopControl>();
    std::atomic_store(
        &relay->StopControl->TerminalHandoff,
        std::make_shared<TqTerminalHandoffControl>(
            relay->StopControl->Generation,
            std::shared_ptr<TqTerminalLedger>{}));
    Check(relay->ActivationMutex.Initialize());
    relay->Activation = TqUvActivation::Active;
    relay->Binding = std::make_shared<TqUvStreamBinding>();
    relay->Binding->Worker = &worker;
    relay->Binding->Relay = relay;
    relay->Binding->RelayId = relay->RelayId;
    relay->Binding->RouteGeneration = relay->RouteGeneration;
    relay->Binding->ControlGeneration = relay->ControlGeneration;
    relay->Binding->Activation.store(TqUvActivation::Active);
    relay->TcpHandle.data = relay.get();
    relay->TcpHandleInitialized = true;
    relay->TcpReadStarted = true;
    return relay;
}

std::unique_ptr<TqUvRelayWorker> Worker(TqUvCallAdapter& calls) {
    calls = TqUvProductionCalls();
    calls.ReadStop = &ReadStop;
    calls.Shutdown = &Shutdown;
    calls.Close = &Close;
    return std::make_unique<TqUvRelayWorker>(
        TqUvRelayWorkerConfig{.Calls = &calls});
}

void BeginTerminal(
    TqUvRelayWorker& worker,
    TqUvRelayState& relay,
    TqUvTerminalTrigger trigger) {
    TqUvRequestTerminal(relay, trigger);
    TqUvProcessTerminalFactsLocal(worker, relay);
}

void ErrorSealsAndStartsHandleCloseExactlyOnce() {
    TqUvCallAdapter calls{};
    auto worker = Worker(calls);
    auto relay = Relay(*worker);
    BeginTerminal(*worker, *relay, TqUvTerminalTrigger::TcpError);
    BeginTerminal(*worker, *relay, TqUvTerminalTrigger::QueueFailure);
    Check(relay->Activation == TqUvActivation::Terminal);
    Check(!relay->TcpReadStarted);
    Check(relay->TcpClosePending);
    Check(gReadStops == 1);
    Check(gCloses == 1);
}

void QuicFinDrainsWritesBeforeTcpShutdown() {
    TqUvCallAdapter calls{};
    auto worker = Worker(calls);
    auto relay = Relay(*worker);
    relay->QuicFinObserved = true;
    relay->PendingTcpWriteBytes = 1;
    TqUvCheckTerminalConvergence(*worker, *relay);
    Check(!relay->TcpShutdownPending);
    relay->PendingTcpWriteBytes = 0;
    TqUvCheckTerminalConvergence(*worker, *relay);
    Check(relay->TcpShutdownPending);
    Check(gShutdowns == 1);
}

void NormalFinUsesCompleteConvergencePredicate() {
    TqUvCallAdapter calls{};
    auto worker = Worker(calls);
    auto relay = Relay(*worker);
    relay->TcpReadClosed = true;
    relay->QuicFinSubmitted = true;
    relay->QuicFinCompleted = true;
    relay->QuicFinObserved = true;
    relay->TcpWriteClosed = true;
    TqUvCheckTerminalConvergence(*worker, *relay);
    Check(relay->TerminalStarted);
    Check(relay->TcpClosePending);
}

void ConcurrentTerminalPublishersRunHandleSequenceOnlyOnConsumer() {
    TqUvCallAdapter calls{};
    auto worker = Worker(calls);
    auto relay = Relay(*worker);
    const auto closes = gCloses.load();
    std::thread first([&] {
        TqUvRequestTerminal(*relay, TqUvTerminalTrigger::RuntimeStop);
    });
    std::thread second([&] {
        TqUvRequestTerminal(*relay, TqUvTerminalTrigger::QueueFailure);
    });
    first.join();
    second.join();
    Check(gCloses.load() == closes);
    TqUvProcessTerminalFactsLocal(*worker, *relay);
    Check(relay->TerminalBeginCount == 1);
    Check(gCloses == closes + 1);
}

void CloseWaitsForRealQuicTerminalAndReleasesOnce() {
    TqUvCallAdapter calls{};
    auto worker = Worker(calls);
    auto relay = Relay(*worker);
    BeginTerminal(*worker, *relay, TqUvTerminalTrigger::TcpError);
    relay->TerminalHandoffComplete.store(false);
    Check(gCloseHandle != nullptr && gCloseCallback != nullptr);
    gCloseCallback(gCloseHandle);
    Check(relay->TcpCloseCompleted);
    Check(!relay->TerminalReleased);
    relay->QuicShutdownObserved.store(true);
    TqUvCheckTerminalConvergence(*worker, *relay);
    TqUvCheckTerminalConvergence(*worker, *relay);
    Check(relay->TerminalReleased);
    Check(relay->TerminalReleaseCount == 1);
}

void AdmittedReceiveBlocksBackendReleaseUntilCallbackSettles() {
    TqUvCallAdapter calls{};
    auto worker = Worker(calls);
    auto relay = Relay(*worker);
    relay->AdmittedQuicReceiveBytes.store(1, std::memory_order_release);
    BeginTerminal(*worker, *relay, TqUvTerminalTrigger::TcpError);
    const auto handoff = std::atomic_load(
        &relay->StopControl->TerminalHandoff);
    Check(handoff != nullptr && handoff->HandoffStartedCounted.load());
    Check(!TqTerminalReleaseReady(handoff->Snapshot()));
    Check(gCloseHandle != nullptr && gCloseCallback != nullptr);
    gCloseCallback(gCloseHandle);
    relay->QuicShutdownObserved.store(true, std::memory_order_release);
    TqUvCheckTerminalConvergence(*worker, *relay);
    Check(!relay->LocalOwnershipDrained);
    Check(!relay->TerminalReleased);
    auto facts = handoff->Snapshot();
    Check(facts.DataPlaneStopped);
    Check(facts.TerminalHandoffComplete);
    Check(!facts.LocalOperationOwnershipTransferredOrDrained);

    relay->AdmittedQuicReceiveBytes.store(0, std::memory_order_release);
    TqUvCheckTerminalConvergence(*worker, *relay);
    Check(relay->TerminalReleased);
    Check(relay->TerminalReleaseCount == 1);
    Check(TqTerminalReleaseReady(handoff->Snapshot()));
}

void LocalOwnershipPredicateRejectsEveryOutstandingByteClass() {
    TqUvCallAdapter calls{};
    auto worker = Worker(calls);
    auto relay = Relay(*worker);
    Check(TqUvLocalOperationOwnershipDrained(*relay));

    relay->PendingTcpReadBytes = 1;
    Check(!TqUvLocalOperationOwnershipDrained(*relay));
    relay->PendingTcpReadBytes = 0;
    relay->AccountedQuicToTcpBytes = 1;
    Check(!TqUvLocalOperationOwnershipDrained(*relay));
    relay->AccountedQuicToTcpBytes = 0;
    relay->AccountedTcpToQuicBytes = 1;
    Check(!TqUvLocalOperationOwnershipDrained(*relay));
    relay->AccountedTcpToQuicBytes = 0;
    relay->QuicToTcpPressureBytes.store(1, std::memory_order_release);
    Check(!TqUvLocalOperationOwnershipDrained(*relay));
    relay->QuicToTcpPressureBytes.store(0, std::memory_order_release);
    relay->AdmittedQuicReceiveBytes.store(1, std::memory_order_release);
    Check(!TqUvLocalOperationOwnershipDrained(*relay));
}

void QueueFullKeepsTerminalFactForSafetyScan() {
    TqUvCallAdapter calls = TqUvProductionCalls();
    calls.TcpInit = &TcpInit;
    calls.TcpOpen = &TcpOpen;
    calls.ReadStart = &ReadStart;
    calls.ReadStop = &ReadStop;
    calls.Close = &SynchronousRelayClose;
    TqUvRelayWorkerConfig config{};
    config.Calls = &calls;
    config.QueueCapacity = 1;
    TqUvRelayWorker worker(config);
    Check(worker.StartAndWaitReady());

    auto owner = StartedOwner(800);
    owner->SetShutdownHookForTest(
        [](std::uint64_t, QUIC_STREAM_SHUTDOWN_FLAGS) {
            return QUIC_STATUS_PENDING;
        });
    auto stop = std::make_shared<TqRelayStopControl>();
    TqUvRelayRegistration registration{};
    registration.TcpSocket = static_cast<TqSocketHandle>(91);
    registration.Stream = reinterpret_cast<MsQuicStream*>(0x1);
    registration.StreamOwner = owner;
    registration.StopControl = stop;
    registration.ControlGeneration = stop->Generation;
    registration.TcpReadChunkSize = 4096;
    registration.MaxPendingBufferBytes = 32768;
    registration.MaxBufferedQuicSendBytes = 16384;
    registration.ResumeBufferedQuicSendBytes = 8192;
    const auto result = worker.RegisterRelayWithId(std::move(registration));
    Check(result.Ok);
    auto relay = worker.RelayForTest(result.RelayId);
    Check(relay != nullptr);

    std::mutex mutex;
    std::condition_variable condition;
    bool blocked = false;
    bool release = false;
    Check(worker.Post([&](TqUvRelayWorker&) {
        std::unique_lock<std::mutex> lock(mutex);
        blocked = true;
        condition.notify_all();
        condition.wait(lock, [&] { return release; });
    }));
    {
        std::unique_lock<std::mutex> lock(mutex);
        Check(condition.wait_for(lock, std::chrono::seconds(2), [&] {
            return blocked;
        }));
    }
    Check(worker.Post([](TqUvRelayWorker&) {}));
    QUIC_STREAM_EVENT event{};
    event.Type = QUIC_STREAM_EVENT_PEER_SEND_ABORTED;
    Check(relay->Binding->OnStreamEvent(
        reinterpret_cast<MsQuicStream*>(0x1),
        &event,
        relay->Binding->RouteGeneration) == QUIC_STATUS_SUCCESS);
    Check(relay->TerminalTriggerMask.load(std::memory_order_acquire) != 0);
    {
        std::lock_guard<std::mutex> guard(mutex);
        release = true;
    }
    condition.notify_all();
    const auto deadline = std::chrono::steady_clock::now() +
        std::chrono::seconds(2);
    while (relay->TerminalBeginCount.load(std::memory_order_acquire) == 0 &&
           std::chrono::steady_clock::now() < deadline) {
        std::this_thread::yield();
    }
    Check(relay->TerminalBeginCount.load(std::memory_order_acquire) == 1);
    QUIC_STREAM_EVENT terminal{};
    terminal.Type = QUIC_STREAM_EVENT_SHUTDOWN_COMPLETE;
    Check(owner->DispatchForTest(&terminal) == QUIC_STATUS_SUCCESS);
    const auto releaseDeadline = std::chrono::steady_clock::now() +
        std::chrono::seconds(2);
    while (relay->TerminalReleaseCount.load(std::memory_order_acquire) == 0 &&
           std::chrono::steady_clock::now() < releaseDeadline) {
        std::this_thread::yield();
    }
    Check(relay->TerminalReleaseCount.load(std::memory_order_acquire) == 1);
    Check(worker.StopForTest());
}

void AbortPublishedAfterMaskExchangeUpgradesGracefulSelection() {
    TqUvCallAdapter calls{};
    auto worker = Worker(calls);
    auto relay = Relay(*worker);
    TqUvRequestTerminal(*relay, TqUvTerminalTrigger::TcpEof);
    Check(relay->ActivationMutex.Lock());
    std::thread consumer([&] {
        TqUvProcessTerminalFactsLocal(*worker, *relay);
    });
    const auto deadline = std::chrono::steady_clock::now() +
        std::chrono::seconds(2);
    while (relay->TerminalTriggerMask.load(std::memory_order_acquire) != 0 &&
           std::chrono::steady_clock::now() < deadline) {
        std::this_thread::yield();
    }
    Check(relay->TerminalTriggerMask.load(std::memory_order_acquire) == 0);
    TqUvRequestTerminal(*relay, TqUvTerminalTrigger::QuicAbort);
    Check(relay->ActivationMutex.Unlock());
    consumer.join();
    Check(relay->TerminalAborted);
    Check(relay->TerminalTriggerMask.load(std::memory_order_acquire) == 0);
}

void SchedulerRetryUsesCurrentOwnerTerminalGeneration() {
    TqTerminalScheduler::ResetForTest();
    TqUvCallAdapter calls{};
    auto worker = Worker(calls);
    auto relay = Relay(*worker);
    auto owner = StartedOwner(802);
    Check(owner != nullptr);
    relay->StreamOwner = owner;
    relay->RouteGeneration = owner->RouteGeneration();
    relay->Binding->RouteGeneration = relay->RouteGeneration;
    relay->Binding->Relay = relay;
    std::atomic<std::uint32_t> shutdownCalls{0};
    owner->SetShutdownHookForTest(
        [&](std::uint64_t, QUIC_STREAM_SHUTDOWN_FLAGS flags) {
            Check((flags & QUIC_STREAM_SHUTDOWN_FLAG_ABORT) != 0);
            const auto call = ++shutdownCalls;
            return call < 3 ? QUIC_STATUS_OUT_OF_MEMORY : QUIC_STATUS_PENDING;
        });

    BeginTerminal(*worker, *relay, TqUvTerminalTrigger::QuicAbort);
    Check(shutdownCalls.load() == 1);
    const auto firstGeneration = owner->RouteGeneration();
    TqTerminalScheduler::AdvanceForTest(std::chrono::milliseconds(10));
    Check(shutdownCalls.load() == 2);
    const auto secondGeneration = owner->RouteGeneration();
    Check(secondGeneration > firstGeneration);
    TqTerminalScheduler::AdvanceForTest(std::chrono::milliseconds(50));
    Check(shutdownCalls.load() == 3);
    const auto currentGeneration = owner->RouteGeneration();
    Check(currentGeneration > secondGeneration);
    const auto ledger = owner->TerminalLedger()->Snapshot(
        TqTerminalScheduler::NowForTest());
    Check(ledger.ShutdownAttempt == 3);
    Check(ledger.Phase == TerminalPhase::ShutdownSubmitted);

    QUIC_STREAM_EVENT terminal{};
    terminal.Type = QUIC_STREAM_EVENT_SHUTDOWN_COMPLETE;
    Check(relay->Binding->OnStreamEvent(
        reinterpret_cast<MsQuicStream*>(0x1), &terminal, secondGeneration) ==
        QUIC_STATUS_SUCCESS);
    Check(!relay->QuicShutdownObserved.load(std::memory_order_acquire));
    Check(owner->DispatchForTest(&terminal) == QUIC_STATUS_SUCCESS);
    Check(relay->QuicShutdownObserved.load(std::memory_order_acquire));
    Check(relay->Binding->TerminalRouteGeneration.load(
              std::memory_order_acquire) == currentGeneration);
    TqUvCheckTerminalConvergence(*worker, *relay);
    Check(relay->TerminalHandoffSubmitted);
    Check(relay->TerminalHandoffStatus == QUIC_STATUS_PENDING);
    Check(!relay->TerminalHandoffRetryScheduled);
    TqTerminalScheduler::ResetForTest();
}

void SubmittedGracefulHandoffDurablyUpgradesToAbort() {
    TqTerminalScheduler::ResetForTest();
    TqUvCallAdapter calls{};
    auto worker = Worker(calls);
    auto relay = Relay(*worker);
    auto owner = StartedOwner(803);
    Check(owner != nullptr);
    relay->StreamOwner = owner;
    relay->RouteGeneration = owner->RouteGeneration();
    relay->Binding->RouteGeneration = relay->RouteGeneration;
    relay->Binding->Relay = relay;
    auto escalation = std::make_shared<CountingEscalation>();
    relay->StopControl->TerminalEscalation = escalation;
    const auto handoff = std::make_shared<TqTerminalHandoffControl>(
        relay->ControlGeneration, owner->TerminalLedger(), escalation);
    std::atomic_store(&relay->StopControl->TerminalHandoff, handoff);
    std::vector<QUIC_STREAM_SHUTDOWN_FLAGS> flagsSeen;
    owner->SetShutdownHookForTest(
        [&](std::uint64_t, QUIC_STREAM_SHUTDOWN_FLAGS flags) {
            flagsSeen.push_back(flags);
            return QUIC_STATUS_PENDING;
        });

    BeginTerminal(*worker, *relay, TqUvTerminalTrigger::TcpEof);
    Check(flagsSeen.size() == 1);
    Check(flagsSeen[0] == QUIC_STREAM_SHUTDOWN_FLAG_GRACEFUL);
    BeginTerminal(*worker, *relay, TqUvTerminalTrigger::TcpError);
    Check(relay->TerminalAbortUpgradePending);
    Check(relay->TerminalAbortUpgradeApplied);
    Check(handoff->AbortUpgradePending.load(std::memory_order_acquire));
    Check(handoff->AbortUpgradeApplied.load(std::memory_order_acquire));
    Check(flagsSeen.size() == 2);
    Check((flagsSeen[1] & QUIC_STREAM_SHUTDOWN_FLAG_ABORT) != 0);
    Check(escalation->Calls.load() == 1);
    const auto ledger = owner->TerminalLedger()->Snapshot(
        TqTerminalScheduler::NowForTest());
    Check(ledger.ShutdownIntent ==
          TqTerminalShutdownIntent::AbortBothImmediate);

    BeginTerminal(*worker, *relay, TqUvTerminalTrigger::QuicAbort);
    Check(flagsSeen.size() == 2);
    Check(escalation->Calls.load() == 1);
    Check(relay->TerminalAbortUpgradePending);

    Check(gCloseHandle != nullptr && gCloseCallback != nullptr);
    gCloseCallback(gCloseHandle);
    QUIC_STREAM_EVENT terminal{};
    terminal.Type = QUIC_STREAM_EVENT_SHUTDOWN_COMPLETE;
    Check(owner->DispatchForTest(&terminal) == QUIC_STATUS_SUCCESS);
    TqUvCheckTerminalConvergence(*worker, *relay);
    Check(!relay->TerminalAbortUpgradePending);
    Check(relay->TerminalAbortUpgradeApplied);
    Check(relay->TerminalReleaseCount.load(std::memory_order_acquire) == 1);
    TqTerminalScheduler::ResetForTest();
}

void TerminalGenerationAcceptsOnlyActiveOrTerminalRoute() {
    TqUvCallAdapter calls{};
    auto worker = Worker(calls);
    auto relay = Relay(*worker);
    QUIC_STREAM_EVENT event{};
    event.Type = QUIC_STREAM_EVENT_SHUTDOWN_COMPLETE;
    Check(relay->Binding->OnStreamEvent(
        reinterpret_cast<MsQuicStream*>(0x1), &event, 999) ==
        QUIC_STATUS_SUCCESS);
    Check(!relay->QuicShutdownObserved.load());
    relay->Binding->TerminalRouteGeneration.store(42);
    QUIC_STREAM_EVENT staleReceive{};
    staleReceive.Type = QUIC_STREAM_EVENT_RECEIVE;
    staleReceive.RECEIVE.Flags = QUIC_RECEIVE_FLAG_FIN;
    Check(relay->Binding->OnStreamEvent(
        reinterpret_cast<MsQuicStream*>(0x1), &staleReceive,
        relay->RouteGeneration) == QUIC_STATUS_SUCCESS);
    Check(!relay->QuicFinObserved.load(std::memory_order_acquire));
    Check(relay->Binding->OnStreamEvent(
        reinterpret_cast<MsQuicStream*>(0x1), &event, 42) ==
        QUIC_STATUS_SUCCESS);
    Check(relay->QuicShutdownObserved.load());
}

void GracefulSubmitFailureDurablyEscalatesToAbortRetry() {
    TqUvCallAdapter calls{};
    auto worker = Worker(calls);
    auto relay = Relay(*worker);
    auto owner = StartedOwner(801);
    Check(owner != nullptr);
    auto escalation = std::make_shared<CountingEscalation>();
    relay->StreamOwner = owner;
    relay->StopControl->TerminalEscalation = escalation;
    std::atomic_store(
        &relay->StopControl->TerminalHandoff,
        std::make_shared<TqTerminalHandoffControl>(
            relay->ControlGeneration, owner->TerminalLedger(), escalation));
    std::atomic<std::uint32_t> callsSeen{0};
    std::atomic<QUIC_STREAM_SHUTDOWN_FLAGS> secondFlags{
        QUIC_STREAM_SHUTDOWN_FLAG_NONE};
    owner->SetShutdownHookForTest(
        [&](std::uint64_t, QUIC_STREAM_SHUTDOWN_FLAGS flags) {
            const auto call = ++callsSeen;
            if (call == 1) {
                Check(flags == QUIC_STREAM_SHUTDOWN_FLAG_GRACEFUL);
                return QUIC_STATUS_OUT_OF_MEMORY;
            }
            secondFlags.store(flags, std::memory_order_release);
            return QUIC_STATUS_PENDING;
        });

    BeginTerminal(*worker, *relay, TqUvTerminalTrigger::TcpEof);

    Check(callsSeen.load() == 2);
    Check(relay->TerminalHandoffEscalated);
    Check(relay->TerminalHandoffSubmitted);
    Check(!relay->TerminalHandoffRetryPending);
    Check((secondFlags.load() & QUIC_STREAM_SHUTDOWN_FLAG_ABORT) != 0);
    Check(relay->Binding->TerminalRouteGeneration.load() ==
          owner->RouteGeneration());
}

void DurableTerminalRequestDoesNotRunHandleApiOrAllocateCommand() {
    gReadStops.store(0);
    gCloses.store(0);
    TqUvCallAdapter calls{};
    auto worker = Worker(calls);
    auto relay = Relay(*worker);
    const auto before = worker->Snapshot();

    TqUvRequestTerminal(*relay, TqUvTerminalTrigger::RuntimeStop);

    const auto after = worker->Snapshot();
    Check(relay->Activation == TqUvActivation::Active);
    Check(!relay->TerminalStarted);
    Check(relay->TerminalTriggerMask.load(std::memory_order_acquire) != 0);
    Check(gReadStops.load() == 0);
    Check(gCloses.load() == 0);
    Check(after.PendingCommands == before.PendingCommands);
}

void QuicAbortCallbackPublishesFactWithoutRunningTerminalLocally() {
    TqUvCallAdapter calls{};
    auto worker = Worker(calls);
    auto relay = Relay(*worker);
    QUIC_STREAM_EVENT event{};
    event.Type = QUIC_STREAM_EVENT_PEER_SEND_ABORTED;

    Check(relay->Binding->OnStreamEvent(
        reinterpret_cast<MsQuicStream*>(0x1), &event,
        relay->RouteGeneration) == QUIC_STATUS_SUCCESS);

    Check(relay->Activation == TqUvActivation::Active);
    Check(!relay->TerminalStarted);
    Check(relay->TerminalTriggerMask.load(std::memory_order_acquire) != 0);
}

} // namespace

int main() {
    gReadStops = 0;
    gShutdowns = 0;
    gCloses = 0;
    ErrorSealsAndStartsHandleCloseExactlyOnce();
    QuicFinDrainsWritesBeforeTcpShutdown();
    NormalFinUsesCompleteConvergencePredicate();
    ConcurrentTerminalPublishersRunHandleSequenceOnlyOnConsumer();
    CloseWaitsForRealQuicTerminalAndReleasesOnce();
    AdmittedReceiveBlocksBackendReleaseUntilCallbackSettles();
    LocalOwnershipPredicateRejectsEveryOutstandingByteClass();
    QueueFullKeepsTerminalFactForSafetyScan();
    AbortPublishedAfterMaskExchangeUpgradesGracefulSelection();
    SchedulerRetryUsesCurrentOwnerTerminalGeneration();
    SubmittedGracefulHandoffDurablyUpgradesToAbort();
    TerminalGenerationAcceptsOnlyActiveOrTerminalRoute();
    GracefulSubmitFailureDurablyEscalatesToAbortRetry();
    DurableTerminalRequestDoesNotRunHandleApiOrAllocateCommand();
    QuicAbortCallbackPublishesFactWithoutRunningTerminalLocally();
    return 0;
}
