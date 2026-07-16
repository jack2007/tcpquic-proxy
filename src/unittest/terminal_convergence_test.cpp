#define TQ_UNIT_TESTING 1
#include "stream_lifetime.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <cstdio>
#include <memory>
#include <mutex>
#include <thread>

#define CHECK(condition) do { if (!(condition)) { std::fprintf(stderr, "CHECK failed at %s:%d: %s\n", __FILE__, __LINE__, #condition); std::abort(); } } while (false)

const MsQuicApi* MsQuic = nullptr;

namespace {

std::atomic<uint32_t> g_receiveDisabled{0};

void QUIC_API FakeSetCallbackHandler(HQUIC, void*, void*) {}

void QUIC_API FakeStreamClose(HQUIC) {}

QUIC_STATUS QUIC_API FakeStreamReceiveSetEnabled(HQUIC, BOOLEAN enabled) {
    if (!enabled) {
        g_receiveDisabled.fetch_add(1, std::memory_order_relaxed);
    }
    return QUIC_STATUS_SUCCESS;
}

class CountingEscalation final : public TqTerminalEscalation {
public:
    void RequestConnectionShutdown(
        uint64_t, uint64_t, QUIC_STATUS, uint64_t) noexcept override { ++Calls; }
    std::atomic<uint32_t> Calls{0};
};

class CapturingTarget final : public TqStreamLifetime::Target {
public:
    QUIC_STATUS OnStreamEvent(
        MsQuicStream*, QUIC_STREAM_EVENT* event, uint64_t) noexcept override {
        ++Calls;
        LastContext = event->Type == QUIC_STREAM_EVENT_SEND_COMPLETE
            ? event->SEND_COMPLETE.ClientContext
            : nullptr;
        return QUIC_STATUS_SUCCESS;
    }
    uint32_t Calls{0};
    void* LastContext{nullptr};
};

void Reset() {
    TqTerminalScheduler::ResetForTest();
    const auto scheduler = TqTerminalScheduler::SnapshotForTest();
    CHECK(!scheduler.Running);
    CHECK(!scheduler.Joinable);
    TqStreamLifetime::ResetLifecycleRegistriesForTest();
}

std::shared_ptr<TqStreamLifetime> MakeStartedOwner(uint64_t streamId) {
    static QUIC_API_TABLE fakeApi{};
    static bool initialized = false;
    if (!initialized) {
        fakeApi.SetCallbackHandler = FakeSetCallbackHandler;
        fakeApi.StreamClose = FakeStreamClose;
        initialized = true;
    }
    MsQuic = reinterpret_cast<const MsQuicApi*>(&fakeApi);
    return TqStreamLifetime::AdoptAccepted(
        reinterpret_cast<HQUIC>(static_cast<uintptr_t>(streamId + 1)),
        std::make_shared<CapturingTarget>(),
        TqTerminalIdentity{
            streamId, 133, 2, 7, TqTunnelRole::ClientOpen,
            TqRelayBackendType::LinuxWorker}, 5);
}

void TestRetryBackoffAndWatchdogBoundaries() {
    Reset();
    auto owner = MakeStartedOwner(44);
    uint32_t shutdownCalls = 0;
    owner->SetShutdownHookForTest(
        [&](uint64_t, QUIC_STREAM_SHUTDOWN_FLAGS) {
            ++shutdownCalls;
            return QUIC_STATUS_OUT_OF_MEMORY;
        });
    auto escalation = std::make_shared<CountingEscalation>();
    auto sink = TqTerminalSink::Create(owner, owner->TerminalLedger());
    const auto first = owner->BeginTerminalShutdown(91, sink, escalation);
    CHECK(first.RetryScheduled);
    CHECK(shutdownCalls == 1);
    TqTerminalScheduler::AdvanceForTest(std::chrono::milliseconds(9));
    CHECK(shutdownCalls == 1);
    TqTerminalScheduler::AdvanceForTest(std::chrono::milliseconds(1));
    CHECK(shutdownCalls == 2);
    TqTerminalScheduler::AdvanceForTest(std::chrono::milliseconds(50));
    CHECK(shutdownCalls == 3);
    TqTerminalScheduler::AdvanceForTest(std::chrono::milliseconds(250));
    CHECK(shutdownCalls == 4);
    CHECK(escalation->Calls == 1);
    TqTerminalScheduler::AdvanceForTest(std::chrono::seconds(30));
    CHECK(owner->TerminalLedger()->Snapshot(TqTerminalScheduler::NowForTest()).Watchdog ==
          TqTerminalWatchdogState::TerminalTimeout);
}

void TestTerminalCancelsRetryAndWatchdog() {
    Reset();
    auto owner = MakeStartedOwner(9);
    owner->SetShutdownHookForTest(
        [](uint64_t, QUIC_STREAM_SHUTDOWN_FLAGS) { return QUIC_STATUS_PENDING; });
    auto escalation = std::make_shared<CountingEscalation>();
    auto sink = TqTerminalSink::Create(owner, owner->TerminalLedger());
    CHECK(owner->BeginTerminalShutdown(91, sink, escalation).Submitted);
    QUIC_STREAM_EVENT terminal{};
    terminal.Type = QUIC_STREAM_EVENT_SHUTDOWN_COMPLETE;
    CHECK(owner->DispatchForTest(&terminal) == QUIC_STATUS_SUCCESS);
    TqTerminalScheduler::AdvanceForTest(std::chrono::seconds(40));
    CHECK(escalation->Calls == 0);
    CHECK(TqTerminalMetricsSnapshot().TerminalTimeoutPending == 0);
    CHECK(TqTerminalMetricsSnapshot().WatchdogCanceled == 1);
    CHECK(owner->DispatchForTest(&terminal) == QUIC_STATUS_SUCCESS);
    TqTerminalScheduler::Instance().Cancel(9);
    CHECK(TqTerminalMetricsSnapshot().WatchdogCanceled == 1);
}

void TestSubmittedShutdownEscalatesAtFiveSecondsThenTimesOut() {
    Reset();
    auto owner = MakeStartedOwner(19);
    owner->SetShutdownHookForTest(
        [](uint64_t, QUIC_STREAM_SHUTDOWN_FLAGS) { return QUIC_STATUS_PENDING; });
    auto escalation = std::make_shared<CountingEscalation>();
    auto sink = TqTerminalSink::Create(owner, owner->TerminalLedger());
    CHECK(owner->BeginTerminalShutdown(91, sink, escalation).Submitted);
    CHECK(owner->TerminalLedger()->Snapshot(TqTerminalScheduler::NowForTest()).Watchdog ==
          TqTerminalWatchdogState::Armed);
    TqTerminalScheduler::AdvanceForTest(std::chrono::milliseconds(4999));
    CHECK(escalation->Calls == 0);
    TqTerminalScheduler::AdvanceForTest(std::chrono::milliseconds(1));
    CHECK(escalation->Calls == 1);
    CHECK(TqTerminalMetricsSnapshot().TerminalTimeoutPending == 1);
    TqTerminalScheduler::AdvanceForTest(std::chrono::seconds(30));
    CHECK(owner->TerminalLedger()->Snapshot(TqTerminalScheduler::NowForTest()).Watchdog ==
          TqTerminalWatchdogState::TerminalTimeout);
    CHECK(TqTerminalMetricsSnapshot().TerminalTimeoutPending == 0);
}

void TestGracefulCompleteNeverArmsFatalWatchdog() {
    Reset();
    auto owner = MakeStartedOwner(190);
    owner->SetShutdownHookForTest(
        [](uint64_t, QUIC_STREAM_SHUTDOWN_FLAGS flags) {
            CHECK(flags == QUIC_STREAM_SHUTDOWN_FLAG_GRACEFUL);
            return QUIC_STATUS_PENDING;
        });
    auto escalation = std::make_shared<CountingEscalation>();
    auto sink = TqTerminalSink::Create(owner, owner->TerminalLedger());
    CHECK(owner->BeginTerminalShutdown(
        0, sink, escalation, TqTerminalShutdownIntent::GracefulComplete).Submitted);
    CHECK(owner->TerminalLedger()->Snapshot(TqTerminalScheduler::NowForTest()).Watchdog ==
          TqTerminalWatchdogState::Idle);
    TqTerminalScheduler::AdvanceForTest(std::chrono::seconds(35));
    const auto snapshot = owner->TerminalLedger()->Snapshot(
        TqTerminalScheduler::NowForTest());
    CHECK(snapshot.Watchdog == TqTerminalWatchdogState::Idle);
    CHECK(escalation->Calls == 0);
    CHECK(TqTerminalMetricsSnapshot().WatchdogArmed == 0);
    CHECK(TqTerminalMetricsSnapshot().WatchdogTimeout == 0);
    CHECK(TqTerminalMetricsSnapshot().TerminalTimeoutPending == 0);
}

void TestSubmittedGracefulShutdownUpgradesToAbortExactlyOnce() {
    Reset();
    auto owner = MakeStartedOwner(191);
    std::vector<QUIC_STREAM_SHUTDOWN_FLAGS> flagsSeen;
    owner->SetShutdownHookForTest(
        [&](uint64_t, QUIC_STREAM_SHUTDOWN_FLAGS flags) {
            flagsSeen.push_back(flags);
            return QUIC_STATUS_PENDING;
        });
    auto escalation = std::make_shared<CountingEscalation>();
    auto sink = TqTerminalSink::Create(owner, owner->TerminalLedger());
    CHECK(owner->BeginTerminalShutdown(
        0, sink, escalation,
        TqTerminalShutdownIntent::GracefulComplete).Submitted);
    CHECK(flagsSeen.size() == 1);
    CHECK(flagsSeen[0] == QUIC_STREAM_SHUTDOWN_FLAG_GRACEFUL);

    const auto upgraded = owner->UpgradeTerminalShutdownToAbort(
        92, escalation);
    CHECK(upgraded.Submitted);
    CHECK(flagsSeen.size() == 2);
    CHECK((flagsSeen[1] & QUIC_STREAM_SHUTDOWN_FLAG_ABORT) != 0);
    CHECK((flagsSeen[1] & QUIC_STREAM_SHUTDOWN_FLAG_IMMEDIATE) != 0);
    CHECK(escalation->Calls.load() == 1);
    const auto ledger = owner->TerminalLedger()->Snapshot(
        TqTerminalScheduler::NowForTest());
    CHECK(ledger.ShutdownIntent ==
          TqTerminalShutdownIntent::AbortBothImmediate);
    CHECK(ledger.ShutdownAttempt == 2);
    CHECK(ledger.ConnectionEscalated);

    const auto duplicate = owner->UpgradeTerminalShutdownToAbort(
        92, escalation);
    CHECK(duplicate.Submitted);
    CHECK(flagsSeen.size() == 2);
    CHECK(escalation->Calls.load() == 1);
}

void TestSubmittedGracefulAbortUpgradeRetriesSyncFailure() {
    Reset();
    auto owner = MakeStartedOwner(192);
    std::atomic<std::uint32_t> calls{0};
    owner->SetShutdownHookForTest(
        [&](uint64_t, QUIC_STREAM_SHUTDOWN_FLAGS flags) {
            const auto call = ++calls;
            if (call == 1) {
                CHECK(flags == QUIC_STREAM_SHUTDOWN_FLAG_GRACEFUL);
                return QUIC_STATUS_PENDING;
            }
            CHECK((flags & QUIC_STREAM_SHUTDOWN_FLAG_ABORT) != 0);
            return call == 2 ? QUIC_STATUS_OUT_OF_MEMORY
                             : QUIC_STATUS_PENDING;
        });
    auto sink = TqTerminalSink::Create(owner, owner->TerminalLedger());
    CHECK(owner->BeginTerminalShutdown(
        0, sink, nullptr,
        TqTerminalShutdownIntent::GracefulComplete).Submitted);

    const auto failed = owner->UpgradeTerminalShutdownToAbort(93);
    CHECK(!failed.Submitted);
    CHECK(QUIC_FAILED(failed.Status));
    const auto retried = owner->UpgradeTerminalShutdownToAbort(93);
    CHECK(retried.Submitted);
    CHECK(calls.load() == 3);
    CHECK(owner->UpgradeTerminalShutdownToAbort(93).Submitted);
    CHECK(calls.load() == 3);
}

void TestTerminalBetweenOwnerCommitAndSchedulerArmIsIrreversible() {
    Reset();
    auto owner = MakeStartedOwner(20);
    owner->SetShutdownHookForTest(
        [](uint64_t, QUIC_STREAM_SHUTDOWN_FLAGS) { return QUIC_STATUS_PENDING; });
    std::mutex lock;
    std::condition_variable cv;
    bool entered = false;
    bool terminalDone = false;
    owner->SetBeforeTerminalLedgerRecordHookForTest([&] {
        std::unique_lock<std::mutex> guard(lock);
        entered = true;
        cv.notify_all();
        cv.wait(guard, [&] { return terminalDone; });
    });
    auto escalation = std::make_shared<CountingEscalation>();
    auto sink = TqTerminalSink::Create(owner, owner->TerminalLedger());
    TqTerminalShutdownResult result{};
    std::thread shutdown([&] {
        result = owner->BeginTerminalShutdown(91, sink, escalation);
    });
    {
        std::unique_lock<std::mutex> guard(lock);
        cv.wait(guard, [&] { return entered; });
    }
    QUIC_STREAM_EVENT terminal{};
    terminal.Type = QUIC_STREAM_EVENT_SHUTDOWN_COMPLETE;
    CHECK(owner->DispatchForTest(&terminal) == QUIC_STATUS_SUCCESS);
    {
        std::lock_guard<std::mutex> guard(lock);
        terminalDone = true;
    }
    cv.notify_all();
    shutdown.join();
    TqTerminalScheduler::AdvanceForTest(std::chrono::seconds(40));
    CHECK(escalation->Calls == 0);
    const auto ledger = owner->TerminalLedger()->Snapshot(TqTerminalScheduler::NowForTest());
    CHECK(ledger.Phase == TerminalPhase::TerminalObserved);
    CHECK(ledger.Watchdog == TqTerminalWatchdogState::Idle);
    CHECK(TqTerminalScheduler::SnapshotForTest().PendingTasks == 0);
}

void TestTerminalBetweenOwnerCommitAndRetryScheduleIsIrreversible() {
    Reset();
    auto owner = MakeStartedOwner(26);
    uint32_t shutdownCalls = 0;
    owner->SetShutdownHookForTest(
        [&](uint64_t, QUIC_STREAM_SHUTDOWN_FLAGS) {
            ++shutdownCalls;
            return QUIC_STATUS_OUT_OF_MEMORY;
        });
    owner->SetBeforeTerminalLedgerRecordHookForTest([&] {
        QUIC_STREAM_EVENT terminal{};
        terminal.Type = QUIC_STREAM_EVENT_SHUTDOWN_COMPLETE;
        CHECK(owner->DispatchForTest(&terminal) == QUIC_STATUS_SUCCESS);
    });
    auto escalation = std::make_shared<CountingEscalation>();
    const auto result = owner->BeginTerminalShutdown(
        91, TqTerminalSink::Create(owner, owner->TerminalLedger()), escalation);
    CHECK(!result.RetryScheduled);
    CHECK(shutdownCalls == 1);
    TqTerminalScheduler::AdvanceForTest(std::chrono::seconds(40));
    CHECK(escalation->Calls == 0);
    CHECK(owner->TerminalLedger()->Snapshot(TqTerminalScheduler::NowForTest()).Watchdog ==
          TqTerminalWatchdogState::Idle);
    CHECK(TqTerminalScheduler::SnapshotForTest().PendingTasks == 0);
}

void TestCancelWinsAgainstTaskAlreadyRemovedFromHeap() {
    Reset();
    auto owner = MakeStartedOwner(21);
    owner->SetShutdownHookForTest(
        [](uint64_t, QUIC_STREAM_SHUTDOWN_FLAGS) { return QUIC_STATUS_PENDING; });
    auto escalation = std::make_shared<CountingEscalation>();
    auto sink = TqTerminalSink::Create(owner, owner->TerminalLedger());
    CHECK(owner->BeginTerminalShutdown(91, sink, escalation).Submitted);
    std::mutex lock;
    std::condition_variable cv;
    bool popped = false;
    bool release = false;
    TqTerminalScheduler::SetBeforeExecuteForTest([&] {
        std::unique_lock<std::mutex> guard(lock);
        popped = true;
        cv.notify_all();
        cv.wait(guard, [&] { return release; });
    });
    std::thread advance([] {
        TqTerminalScheduler::AdvanceForTest(std::chrono::seconds(5));
    });
    {
        std::unique_lock<std::mutex> guard(lock);
        cv.wait(guard, [&] { return popped; });
    }
    TqTerminalScheduler::Instance().Cancel(21);
    {
        std::lock_guard<std::mutex> guard(lock);
        release = true;
    }
    cv.notify_all();
    advance.join();
    TqTerminalScheduler::SetBeforeExecuteForTest({});
    CHECK(escalation->Calls == 0);
    CHECK(owner->TerminalLedger()->Snapshot(TqTerminalScheduler::NowForTest()).Watchdog ==
          TqTerminalWatchdogState::Canceled);
}

void TestSchedulerAllocationFailuresEscalateWithoutDamagingExistingTasks() {
    Reset();
    auto first = MakeStartedOwner(22);
    first->SetShutdownHookForTest(
        [](uint64_t, QUIC_STREAM_SHUTDOWN_FLAGS) { return QUIC_STATUS_PENDING; });
    auto firstEscalation = std::make_shared<CountingEscalation>();
    CHECK(first->BeginTerminalShutdown(
        91, TqTerminalSink::Create(first, first->TerminalLedger()),
        firstEscalation).Submitted);
    const auto before = TqTerminalScheduler::SnapshotForTest();
    TqTerminalScheduler::FailNextAllocationForTest(1);
    auto second = MakeStartedOwner(23);
    second->SetShutdownHookForTest(
        [](uint64_t, QUIC_STREAM_SHUTDOWN_FLAGS) { return QUIC_STATUS_PENDING; });
    auto secondEscalation = std::make_shared<CountingEscalation>();
    CHECK(second->BeginTerminalShutdown(
        91, TqTerminalSink::Create(second, second->TerminalLedger()),
        secondEscalation).Submitted);
    CHECK(secondEscalation->Calls == 1);
    const auto after = TqTerminalScheduler::SnapshotForTest();
    CHECK(after.PendingTasks == before.PendingTasks + 1);
    CHECK(after.IndexedStreams == before.IndexedStreams + 1);
    TqTerminalScheduler::AdvanceForTest(std::chrono::seconds(5));
    CHECK(firstEscalation->Calls == 1);
}

void TestRetryAndThreadStartFailuresEscalateOnce() {
    Reset();
    TqTerminalScheduler::FailNextAllocationForTest(1);
    auto retryOwner = MakeStartedOwner(24);
    retryOwner->SetShutdownHookForTest(
        [](uint64_t, QUIC_STREAM_SHUTDOWN_FLAGS) { return QUIC_STATUS_OUT_OF_MEMORY; });
    auto retryEscalation = std::make_shared<CountingEscalation>();
    const auto retry = retryOwner->BeginTerminalShutdown(
        91, TqTerminalSink::Create(retryOwner, retryOwner->TerminalLedger()),
        retryEscalation);
    CHECK(!retry.RetryScheduled);
    CHECK(retryEscalation->Calls == 1);
    CHECK(TqTerminalMetricsSnapshot().SchedulerFailure == 1);

    Reset();
    TqTerminalScheduler::UseRealClockForTest();
    TqTerminalScheduler::FailNextThreadStartForTest();
    auto armOwner = MakeStartedOwner(25);
    armOwner->SetShutdownHookForTest(
        [](uint64_t, QUIC_STREAM_SHUTDOWN_FLAGS) { return QUIC_STATUS_PENDING; });
    auto armEscalation = std::make_shared<CountingEscalation>();
    CHECK(armOwner->BeginTerminalShutdown(
        91, TqTerminalSink::Create(armOwner, armOwner->TerminalLedger()),
        armEscalation).Submitted);
    CHECK(armEscalation->Calls == 1);
    CHECK(TqTerminalMetricsSnapshot().SchedulerFailure == 1);
    TqTerminalScheduler::Instance().Stop();
}

void TestStopJoinRejectsScheduleAndEscalatesOnce() {
    Reset();
    TqTerminalScheduler::UseRealClockForTest();
    std::mutex lock;
    std::condition_variable cv;
    bool workerReturning = false;
    bool releaseWorker = false;
    TqTerminalScheduler::SetBeforeWorkerReturnForTest([&] {
        std::unique_lock<std::mutex> guard(lock);
        workerReturning = true;
        cv.notify_all();
        cv.wait(guard, [&] { return releaseWorker; });
    });
    TqTerminalScheduler::Instance().Start();
    std::thread stopper([] { TqTerminalScheduler::Instance().Stop(); });
    {
        std::unique_lock<std::mutex> guard(lock);
        cv.wait(guard, [&] { return workerReturning; });
    }
    auto owner = MakeStartedOwner(27);
    owner->SetShutdownHookForTest(
        [](uint64_t, QUIC_STREAM_SHUTDOWN_FLAGS) { return QUIC_STATUS_OUT_OF_MEMORY; });
    auto escalation = std::make_shared<CountingEscalation>();
    const auto result = owner->BeginTerminalShutdown(
        91, TqTerminalSink::Create(owner, owner->TerminalLedger()), escalation);
    CHECK(!result.RetryScheduled);
    CHECK(escalation->Calls == 1);
    auto armOwner = MakeStartedOwner(29);
    armOwner->SetShutdownHookForTest(
        [](uint64_t, QUIC_STREAM_SHUTDOWN_FLAGS) { return QUIC_STATUS_PENDING; });
    auto armEscalation = std::make_shared<CountingEscalation>();
    CHECK(armOwner->BeginTerminalShutdown(
        91, TqTerminalSink::Create(armOwner, armOwner->TerminalLedger()),
        armEscalation).Submitted);
    CHECK(armEscalation->Calls == 1);
    {
        std::lock_guard<std::mutex> guard(lock);
        releaseWorker = true;
    }
    cv.notify_all();
    stopper.join();
    TqTerminalScheduler::SetBeforeWorkerReturnForTest({});
    const auto restarted = TqTerminalScheduler::SnapshotForTest();
    CHECK(restarted.LifecycleGeneration != 0);
    CHECK(restarted.PendingTasks == 2);
    TqTerminalScheduler::AdvanceForTest(std::chrono::seconds(30));
    CHECK(owner->TerminalLedger()->Snapshot(TqTerminalScheduler::NowForTest()).Watchdog ==
          TqTerminalWatchdogState::TerminalTimeout);
    CHECK(armOwner->TerminalLedger()->Snapshot(TqTerminalScheduler::NowForTest()).Watchdog ==
          TqTerminalWatchdogState::TerminalTimeout);
}

void TestStopCannotSplitEnqueueFromWorkerStart() {
    Reset();
    TqTerminalScheduler::UseRealClockForTest();
    std::mutex lock;
    std::condition_variable cv;
    bool enqueued = false;
    bool releaseEnqueue = false;
    TqTerminalScheduler::SetAfterEnqueueForTest([&] {
        std::unique_lock<std::mutex> guard(lock);
        enqueued = true;
        cv.notify_all();
        cv.wait(guard, [&] { return releaseEnqueue; });
    });
    auto owner = MakeStartedOwner(28);
    owner->SetShutdownHookForTest(
        [](uint64_t, QUIC_STREAM_SHUTDOWN_FLAGS) { return QUIC_STATUS_PENDING; });
    auto escalation = std::make_shared<CountingEscalation>();
    std::atomic<bool> submitted{false};
    std::thread scheduler([&] {
        submitted.store(owner->BeginTerminalShutdown(
            91, TqTerminalSink::Create(owner, owner->TerminalLedger()),
            escalation).Submitted, std::memory_order_release);
    });
    {
        std::unique_lock<std::mutex> guard(lock);
        cv.wait(guard, [&] { return enqueued; });
    }
    std::atomic<bool> stopReturned{false};
    std::thread stopper([&] {
        TqTerminalScheduler::Instance().Stop();
        stopReturned.store(true, std::memory_order_release);
    });
    CHECK(!stopReturned.load(std::memory_order_acquire));
    {
        std::lock_guard<std::mutex> guard(lock);
        releaseEnqueue = true;
    }
    cv.notify_all();
    scheduler.join();
    CHECK(submitted.load(std::memory_order_acquire));
    stopper.join();
    TqTerminalScheduler::SetAfterEnqueueForTest({});
}

void TestCompletedStreamsLeaveNoSchedulerIndexEntries() {
    Reset();
    for (uint64_t id = 30; id != 34; ++id) {
        auto owner = MakeStartedOwner(id);
        owner->SetShutdownHookForTest(
            [](uint64_t, QUIC_STREAM_SHUTDOWN_FLAGS) { return QUIC_STATUS_PENDING; });
        auto escalation = std::make_shared<CountingEscalation>();
        CHECK(owner->BeginTerminalShutdown(
            91, TqTerminalSink::Create(owner, owner->TerminalLedger()),
            escalation).Submitted);
        QUIC_STREAM_EVENT terminal{};
        terminal.Type = QUIC_STREAM_EVENT_SHUTDOWN_COMPLETE;
        CHECK(owner->DispatchForTest(&terminal) == QUIC_STATUS_SUCCESS);
    }
    const auto snapshot = TqTerminalScheduler::SnapshotForTest();
    CHECK(snapshot.PendingTasks == 0);
    CHECK(snapshot.IndexedStreams == 0);
    CHECK(snapshot.CanceledStreams == 0);
}

void TestStartWaitsForJoinableOldWorkerAndStopDrainsTasks() {
    Reset();
    TqTerminalScheduler::UseRealClockForTest();
    std::mutex lock;
    std::condition_variable cv;
    bool oldWorkerReturning = false;
    bool releaseOldWorker = false;
    TqTerminalScheduler::SetBeforeWorkerReturnForTest([&] {
        std::unique_lock<std::mutex> guard(lock);
        oldWorkerReturning = true;
        cv.notify_all();
        cv.wait(guard, [&] { return releaseOldWorker; });
    });
    TqTerminalScheduler::Instance().Start();
    std::thread stopper([] { TqTerminalScheduler::Instance().Stop(); });
    {
        std::unique_lock<std::mutex> guard(lock);
        cv.wait(guard, [&] { return oldWorkerReturning; });
    }
    std::atomic<bool> startReturned{false};
    std::thread starter([&] {
        TqTerminalScheduler::Instance().Start();
        startReturned.store(true, std::memory_order_release);
    });
    {
        std::lock_guard<std::mutex> guard(lock);
        releaseOldWorker = true;
    }
    cv.notify_all();
    stopper.join();
    starter.join();
    CHECK(startReturned.load(std::memory_order_acquire));
    const auto running = TqTerminalScheduler::SnapshotForTest();
    CHECK(running.Running && running.Joinable);
    TqTerminalScheduler::SetBeforeWorkerReturnForTest({});
    TqTerminalScheduler::Instance().Stop();
    const auto stopped = TqTerminalScheduler::SnapshotForTest();
    CHECK(!stopped.Running && !stopped.Joinable);
    CHECK(stopped.PendingTasks == 0 && stopped.IndexedStreams == 0);
}

void TestTerminalSinkDoesNotOwnOwnerAndAccountsOnce() {
    Reset();
    auto owner = TqStreamLifetime::CreateForTest(
        TqStreamLifetime::Phase::CreatedNotStarted);
    owner->BindTerminalIdentity(TqTerminalIdentity{
        17, 133, 2, 7, TqTunnelRole::ClientOpen,
        TqRelayBackendType::LinuxWorker}, 5);
    auto ledger = owner->TerminalLedger();
    std::weak_ptr<TqStreamLifetime> weak = owner;
    auto sink = TqTerminalSink::Create(owner, ledger);
    CHECK(sink != nullptr);
    owner.reset();
    CHECK(weak.expired());
    QUIC_STREAM_EVENT sendDone{};
    sendDone.Type = QUIC_STREAM_EVENT_SEND_SHUTDOWN_COMPLETE;
    CHECK(sink->OnStreamEvent(nullptr, &sendDone, 3) == QUIC_STATUS_SUCCESS);
    CHECK(ledger->Snapshot(std::chrono::steady_clock::now()).LastStreamEvent ==
          TqTerminalEvent::SendShutdownComplete);
    QUIC_STREAM_EVENT terminal{};
    terminal.Type = QUIC_STREAM_EVENT_SHUTDOWN_COMPLETE;
    CHECK(sink->OnStreamEvent(nullptr, &terminal, 3) == QUIC_STATUS_SUCCESS);
    CHECK(sink->OnStreamEvent(nullptr, &terminal, 3) == QUIC_STATUS_SUCCESS);
    CHECK(ledger->Snapshot(std::chrono::steady_clock::now()).AccountingCompleted);
    const auto metrics = TqTerminalMetricsSnapshot();
    CHECK(metrics.TerminalSinkPending == 0);
    CHECK(metrics.DuplicateTerminalSuppressed == 1);
}

void TestIdentityRebindKeepsOriginalLedger() {
    Reset();
    auto owner = TqStreamLifetime::CreateForTest(
        TqStreamLifetime::Phase::CreatedNotStarted);
    TqTerminalIdentity identity{
        17, 133, 2, 7, TqTunnelRole::ClientOpen,
        TqRelayBackendType::LinuxWorker};
    owner->BindTerminalIdentity(identity, 5);
    const auto original = owner->TerminalLedger();
    identity.TunnelId = 134;
    owner->BindTerminalIdentity(identity, 5);
    CHECK(owner->TerminalLedger().get() == original.get());
    CHECK(TqTerminalMetricsSnapshot().ExactlyOnceViolation == 1);
}

void TestSinkRejectsMissingOrMismatchedOwnerLedger() {
    Reset();
    auto owner = TqStreamLifetime::CreateForTest(
        TqStreamLifetime::Phase::CreatedNotStarted);
    owner->BindTerminalIdentity(TqTerminalIdentity{
        17, 133, 2, 7, TqTunnelRole::ClientOpen,
        TqRelayBackendType::LinuxWorker}, 5);
    auto other = std::make_shared<TqTerminalLedger>(TqTerminalIdentity{});
    CHECK(TqTerminalSink::Create(owner, nullptr) == nullptr);
    CHECK(TqTerminalSink::Create(owner, other) == nullptr);
    std::weak_ptr<TqStreamLifetime> expired;
    CHECK(TqTerminalSink::Create(expired, owner->TerminalLedger()) == nullptr);
    CHECK(TqTerminalMetricsSnapshot().ExactlyOnceViolation == 3);
}

void TestSinkPendingRollsBackWhenNeverPublishedOrShutdownRejected() {
    Reset();
    auto owner = TqStreamLifetime::CreateForTest(
        TqStreamLifetime::Phase::CreatedNotStarted);
    owner->BindTerminalIdentity(TqTerminalIdentity{
        17, 133, 2, 7, TqTunnelRole::ClientOpen,
        TqRelayBackendType::LinuxWorker}, 5);
    {
        auto unused = TqTerminalSink::Create(owner, owner->TerminalLedger());
        CHECK(TqTerminalMetricsSnapshot().TerminalSinkPending == 1);
    }
    CHECK(TqTerminalMetricsSnapshot().TerminalSinkPending == 0);

    auto rejected = TqTerminalSink::Create(owner, owner->TerminalLedger());
    const auto result = owner->BeginTerminalShutdown(91, rejected, nullptr);
    CHECK(result.Status == QUIC_STATUS_INVALID_STATE);
    rejected.reset();
    CHECK(TqTerminalMetricsSnapshot().TerminalSinkPending == 0);
}

void TestSinkControlBlockFailureDoesNotConsumeAnotherPendingObligation() {
    Reset();
    auto owner = TqStreamLifetime::CreateForTest(
        TqStreamLifetime::Phase::CreatedNotStarted);
    owner->BindTerminalIdentity(TqTerminalIdentity{
        17, 133, 2, 7, TqTunnelRole::ClientOpen,
        TqRelayBackendType::LinuxWorker}, 5);
    auto first = TqTerminalSink::Create(owner, owner->TerminalLedger());
    CHECK(first != nullptr);
    CHECK(TqTerminalMetricsSnapshot().TerminalSinkPending == 1);
    TqTerminalSink::SetFailNextControlBlockForTest(true);
    CHECK(TqTerminalSink::Create(owner, owner->TerminalLedger()) == nullptr);
    CHECK(TqTerminalMetricsSnapshot().TerminalSinkPending == 1);
    first.reset();
    CHECK(TqTerminalMetricsSnapshot().TerminalSinkPending == 0);
}

void TestSinkPendingHandlesAlreadyTerminalAndMultipleSinks() {
    Reset();
    auto owner = TqStreamLifetime::CreateForTest(TqStreamLifetime::Phase::Started);
    auto ledger = owner->TerminalLedger();
    QUIC_STREAM_EVENT terminal{};
    terminal.Type = QUIC_STREAM_EVENT_SHUTDOWN_COMPLETE;
    CHECK(owner->DispatchForTest(&terminal) == QUIC_STATUS_SUCCESS);
    std::atomic<uint32_t> lateCallbacks{0};
    auto late = TqTerminalSink::Create(owner, ledger, [&] { ++lateCallbacks; });
    CHECK(TqTerminalMetricsSnapshot().TerminalSinkPending == 1);
    CHECK(owner->BeginTerminalShutdown(91, late, nullptr).AlreadyTerminal);
    late->CompleteAlreadyTerminal();
    CHECK(lateCallbacks == 1);
    CHECK(ledger->Snapshot(std::chrono::steady_clock::now()).AccountingCompleted);
    late.reset();
    CHECK(TqTerminalMetricsSnapshot().TerminalSinkPending == 0);

    Reset();
    owner = TqStreamLifetime::CreateForTest(TqStreamLifetime::Phase::Started);
    ledger = owner->TerminalLedger();
    auto first = TqTerminalSink::Create(owner, ledger);
    auto second = TqTerminalSink::Create(owner, ledger);
    CHECK(TqTerminalMetricsSnapshot().TerminalSinkPending == 2);
    CHECK(first->OnStreamEvent(nullptr, &terminal, 1) == QUIC_STATUS_SUCCESS);
    CHECK(TqTerminalMetricsSnapshot().TerminalSinkPending == 1);
    CHECK(second->OnStreamEvent(nullptr, &terminal, 1) == QUIC_STATUS_SUCCESS);
    CHECK(TqTerminalMetricsSnapshot().TerminalSinkPending == 0);
    CHECK(first->OnStreamEvent(nullptr, &terminal, 1) == QUIC_STATUS_SUCCESS);
    CHECK(TqTerminalMetricsSnapshot().TerminalSinkPending == 0);
    first.reset();
    second.reset();
    CHECK(TqTerminalMetricsSnapshot().TerminalSinkPending == 0);
}

void TestSinkAndAlreadyTerminalCompletionRaceIsExactlyOnce() {
    Reset();
    auto owner = MakeStartedOwner(191);
    auto ledger = owner->TerminalLedger();
    std::atomic<uint32_t> callbacks{0};
    auto sink = TqTerminalSink::Create(owner, ledger, [&] { ++callbacks; });
    QUIC_STREAM_EVENT terminal{};
    terminal.Type = QUIC_STREAM_EVENT_SHUTDOWN_COMPLETE;
    std::thread eventThread([&] {
        CHECK(sink->OnStreamEvent(nullptr, &terminal, 1) == QUIC_STATUS_SUCCESS);
    });
    std::thread alreadyThread([&] { sink->CompleteAlreadyTerminal(); });
    eventThread.join();
    alreadyThread.join();
    CHECK(callbacks == 1);
    CHECK(ledger->Snapshot(std::chrono::steady_clock::now()).AccountingCompleted);
    CHECK(TqTerminalMetricsSnapshot().TerminalObserved == 1);
    CHECK(TqTerminalMetricsSnapshot().TerminalSinkPending == 0);
}

void TestOwnerClaimsSendCompletionBeforeTerminalSink() {
    Reset();
    auto target = std::make_shared<CapturingTarget>();
    auto owner = TqStreamLifetime::CreateForTest(
        TqStreamLifetime::Phase::Started, target);
    auto ledger = owner->TerminalLedger();
    auto sink = TqTerminalSink::Create(owner, ledger);
    void* delivered = reinterpret_cast<void*>(static_cast<uintptr_t>(0x1234));
    void* directDelivered = reinterpret_cast<void*>(static_cast<uintptr_t>(0x2345));
    auto reservation = owner->ReserveSendCompletion(delivered);
    CHECK(reservation);
    void* key = reservation.Key();
    reservation.Dismiss();
    void* directKey = owner->RegisterSendCompletion(directDelivered);
    CHECK(directKey != nullptr);
    CHECK(owner->PublishTarget(owner->RouteGeneration(), sink));

    QUIC_STREAM_EVENT complete{};
    complete.Type = QUIC_STREAM_EVENT_SEND_COMPLETE;
    complete.SEND_COMPLETE.ClientContext = key;
    CHECK(owner->DispatchForTest(&complete) == QUIC_STATUS_SUCCESS);
    CHECK(target->Calls == 1);
    CHECK(target->LastContext == delivered);
    CHECK(ledger->Snapshot(std::chrono::steady_clock::now()).LastStreamEvent ==
          TqTerminalEvent::None);

    complete.SEND_COMPLETE.ClientContext = directKey;
    CHECK(owner->DispatchForTest(&complete) == QUIC_STATUS_SUCCESS);
    CHECK(target->Calls == 2);
    CHECK(target->LastContext == directDelivered);

    complete.SEND_COMPLETE.ClientContext = key;
    CHECK(owner->DispatchForTest(&complete) == QUIC_STATUS_SUCCESS);
    complete.SEND_COMPLETE.ClientContext =
        reinterpret_cast<void*>(static_cast<uintptr_t>(0x5678));
    CHECK(owner->DispatchForTest(&complete) == QUIC_STATUS_SUCCESS);
    CHECK(target->Calls == 2);
    CHECK(ledger->Snapshot(std::chrono::steady_clock::now()).LastStreamEvent ==
          TqTerminalEvent::None);
    const auto completions = TqStreamLifetime::SnapshotSendCompletions();
    CHECK(completions.DuplicateClaims == 1);
    CHECK(completions.UnknownClaims == 1);
    QUIC_STREAM_EVENT terminal{};
    terminal.Type = QUIC_STREAM_EVENT_SHUTDOWN_COMPLETE;
    CHECK(owner->DispatchForTest(&terminal) == QUIC_STATUS_SUCCESS);
    CHECK(TqTerminalMetricsSnapshot().TerminalSinkPending == 0);
}

void TestSinkRecordsNonTerminalEvents() {
    Reset();
    auto owner = TqStreamLifetime::CreateForTest(
        TqStreamLifetime::Phase::CreatedNotStarted);
    owner->BindTerminalIdentity(TqTerminalIdentity{
        17, 133, 2, 7, TqTunnelRole::ClientOpen,
        TqRelayBackendType::LinuxWorker}, 5);
    auto ledger = owner->TerminalLedger();
    auto sink = TqTerminalSink::Create(owner, ledger);
    const QUIC_STREAM_EVENT_TYPE types[] = {
        QUIC_STREAM_EVENT_START_COMPLETE,
        QUIC_STREAM_EVENT_PEER_SEND_ABORTED,
        QUIC_STREAM_EVENT_PEER_RECEIVE_ABORTED,
        QUIC_STREAM_EVENT_CANCEL_ON_LOSS,
        QUIC_STREAM_EVENT_IDEAL_SEND_BUFFER_SIZE,
    };
    const TqTerminalEvent expected[] = {
        TqTerminalEvent::StartComplete,
        TqTerminalEvent::PeerSendAborted,
        TqTerminalEvent::PeerReceiveAborted,
        TqTerminalEvent::CancelOnLoss,
        TqTerminalEvent::IdealSendBufferSize,
    };
    for (size_t i = 0; i < sizeof(types) / sizeof(types[0]); ++i) {
        QUIC_STREAM_EVENT event{};
        event.Type = types[i];
        CHECK(sink->OnStreamEvent(nullptr, &event, 3) == QUIC_STATUS_SUCCESS);
        CHECK(ledger->Snapshot(std::chrono::steady_clock::now()).LastStreamEvent ==
              expected[i]);
    }

    static QUIC_API_TABLE fakeApi{};
    fakeApi.SetCallbackHandler = FakeSetCallbackHandler;
    fakeApi.StreamClose = FakeStreamClose;
    fakeApi.StreamReceiveSetEnabled = FakeStreamReceiveSetEnabled;
    MsQuic = reinterpret_cast<const MsQuicApi*>(&fakeApi);
    MsQuicStream stream(reinterpret_cast<HQUIC>(1), CleanUpManual);
    QUIC_STREAM_EVENT receive{};
    receive.Type = QUIC_STREAM_EVENT_RECEIVE;
    receive.RECEIVE.TotalBufferLength = 123;
    g_receiveDisabled.store(0, std::memory_order_relaxed);
    CHECK(sink->OnStreamEvent(&stream, &receive, 3) == QUIC_STATUS_SUCCESS);
    CHECK(g_receiveDisabled.load(std::memory_order_relaxed) == 1);
    CHECK(receive.RECEIVE.TotalBufferLength == 0);
    CHECK(ledger->Snapshot(std::chrono::steady_clock::now()).LastStreamEvent ==
          TqTerminalEvent::ReceiveAfterHandoff);
}

void TestRetentionSnapshotFiltersFinalLedger() {
    Reset();
    auto owner = TqStreamLifetime::CreateForTest(
        TqStreamLifetime::Phase::CreatedNotStarted);
    const TqTerminalIdentity identity{
        17, 133, 2, 7, TqTunnelRole::ClientOpen,
        TqRelayBackendType::LinuxWorker};
    owner->BindTerminalIdentity(identity, 5);
    CHECK(owner->BeginStart());
    TqTerminalRetentionFilter filter{};
    filter.Backend = TqRelayBackendType::LinuxWorker;
    filter.ConnectionId = 2;
    filter.TunnelId = 133;
    auto snapshots = TqSnapshotTerminalRetentions(filter);
    CHECK(snapshots.size() == 1);
    CHECK(snapshots[0].Identity.StreamId == 17);
    filter.TunnelId = 134;
    CHECK(TqSnapshotTerminalRetentions(filter).empty());
    filter = {};
    CHECK(TqSnapshotTerminalRetentions(filter).size() == 1);
    filter.HasPhase = true;
    filter.Phase = TerminalPhase::ShutdownSubmitted;
    CHECK(TqSnapshotTerminalRetentions(filter).empty());
    filter.Phase = TerminalPhase::Active;
    CHECK(TqSnapshotTerminalRetentions(filter).size() == 1);

    QUIC_STREAM_EVENT terminal{};
    terminal.Type = QUIC_STREAM_EVENT_SHUTDOWN_COMPLETE;
    CHECK(owner->DispatchForTest(&terminal) == QUIC_STATUS_SUCCESS);
    CHECK(TqSnapshotTerminalRetentions({}).empty());
}

void TestRetentionSnapshotCopiesLedgerBeforeReadingIt() {
    Reset();
    auto owner = TqStreamLifetime::CreateForTest(TqStreamLifetime::Phase::Started);
    std::mutex lock;
    std::condition_variable cv;
    bool entered = false;
    bool release = false;
    TqStreamLifetime::SetBeforeTerminalRetentionSnapshotForTest([&] {
        std::unique_lock<std::mutex> guard(lock);
        entered = true;
        cv.notify_all();
        cv.wait(guard, [&] { return release; });
    });
    std::thread snapshotter([&] { (void)TqSnapshotTerminalRetentions({}); });
    {
        std::unique_lock<std::mutex> guard(lock);
        cv.wait(guard, [&] { return entered; });
    }
    QUIC_STREAM_EVENT terminal{};
    terminal.Type = QUIC_STREAM_EVENT_SHUTDOWN_COMPLETE;
    CHECK(owner->DispatchForTest(&terminal) == QUIC_STATUS_SUCCESS);
    CHECK(TqStreamLifetime::SnapshotTerminalRetentions().OwnerCount == 0);
    {
        std::lock_guard<std::mutex> guard(lock);
        release = true;
    }
    cv.notify_all();
    snapshotter.join();
    TqStreamLifetime::SetBeforeTerminalRetentionSnapshotForTest({});
}

void TestRetentionAdminFilterIsStrictAndSchemaIsCanonical() {
    TqTerminalRetentionFilter filter{};
    CHECK(TqParseTerminalRetentionPath(
        "/relay/terminal-retentions?backend=linux&connection_id=2&tunnel_id=133&terminal_phase=active",
        filter));
    CHECK(filter.Backend == TqRelayBackendType::LinuxWorker);
    CHECK(filter.ConnectionId == 2);
    CHECK(filter.TunnelId == 133);
    CHECK(filter.HasPhase && filter.Phase == TerminalPhase::Active);

    const char* invalid[] = {
        "/relay/terminal-retentions?backend=linux&backend=darwin",
        "/relay/terminal-retentions?unknown=x",
        "/relay/terminal-retentions?connection_id=0",
        "/relay/terminal-retentions?connection_id=+2",
        "/relay/terminal-retentions?connection_id=18446744073709551616",
        "/relay/terminal-retentions?terminal_phase=closing",
        "/relay/terminal-retentions?",
        "/relay/terminal-retentions?backend=linux&",
    };
    for (const char* path : invalid) {
        TqTerminalRetentionFilter rejected{};
        CHECK(!TqParseTerminalRetentionPath(path, rejected));
    }

    const std::string json = TqTerminalRetentionsJson({});
    for (const char* field : {"\"retentions\"", "\"count\"", "\"oldest_age_ms\""}) {
        CHECK(json.find(field) != std::string::npos);
    }
}

void TestRetentionAgeDiagnosticsLogThresholdsOnceAndClearOnTerminal() {
    Reset();
    auto owner = MakeStartedOwner(811);
    CHECK(TqTerminalRetentionDiagnosticsForTest().TrackedStreams == 0);
    TqTerminalScheduler::AdvanceForTest(std::chrono::milliseconds(5100));
    auto diagnostics = TqTerminalRetentionDiagnosticsForTest();
    CHECK(diagnostics.WarningLogs == 1);
    CHECK(diagnostics.CriticalLogs == 0);
    CHECK(diagnostics.TrackedStreams == 1);
    TqTerminalScheduler::AdvanceForTest(std::chrono::seconds(10));
    CHECK(TqTerminalRetentionDiagnosticsForTest().WarningLogs == 1);
    TqTerminalScheduler::AdvanceForTest(std::chrono::milliseconds(15001));
    diagnostics = TqTerminalRetentionDiagnosticsForTest();
    CHECK(diagnostics.WarningLogs == 1);
    CHECK(diagnostics.CriticalLogs == 1);
    TqTerminalScheduler::AdvanceForTest(std::chrono::seconds(10));
    CHECK(TqTerminalRetentionDiagnosticsForTest().CriticalLogs == 1);

    QUIC_STREAM_EVENT terminal{};
    terminal.Type = QUIC_STREAM_EVENT_SHUTDOWN_COMPLETE;
    CHECK(owner->DispatchForTest(&terminal) == QUIC_STATUS_SUCCESS);
    TqTerminalScheduler::AdvanceForTest(std::chrono::milliseconds(1));
    CHECK(TqTerminalRetentionDiagnosticsForTest().TrackedStreams == 0);
}

void TestRetentionDiagnosticsAllocationFailuresDoNotConsumeOnceBits() {
    using Stage = TqTerminalScheduler::DiagnosticAllocationStage;
    for (const Stage stage : {Stage::Snapshot, Stage::State, Stage::Log}) {
        Reset();
        auto owner = MakeStartedOwner(820 + static_cast<uint64_t>(stage));
        TqTerminalScheduler::FailNextDiagnosticAllocationForTest(stage);
        TqTerminalScheduler::AdvanceForTest(std::chrono::seconds(31));
        auto metrics = TqTerminalMetricsSnapshot();
        auto diagnostics = TqTerminalRetentionDiagnosticsForTest();
        CHECK(metrics.SchedulerFailure == 1);
        CHECK(diagnostics.WarningLogs == 0);
        CHECK(diagnostics.CriticalLogs == 0);

        TqTerminalScheduler::AdvanceForTest(std::chrono::milliseconds(1));
        metrics = TqTerminalMetricsSnapshot();
        diagnostics = TqTerminalRetentionDiagnosticsForTest();
        CHECK(metrics.SchedulerFailure == 1);
        CHECK(diagnostics.WarningLogs == 1);
        CHECK(diagnostics.CriticalLogs == 1);
        TqTerminalScheduler::AdvanceForTest(std::chrono::milliseconds(1));
        CHECK(TqTerminalRetentionDiagnosticsForTest().WarningLogs == 1);
        CHECK(TqTerminalRetentionDiagnosticsForTest().CriticalLogs == 1);
        QUIC_STREAM_EVENT terminal{};
        terminal.Type = QUIC_STREAM_EVENT_SHUTDOWN_COMPLETE;
        CHECK(owner->DispatchForTest(&terminal) == QUIC_STATUS_SUCCESS);
    }
}

void TestRetentionDiagnosticEmitFailureRollsBackReservation() {
    for (const bool throwException : {false, true}) {
        Reset();
        auto owner = MakeStartedOwner(830 + static_cast<uint64_t>(throwException));
        TqTerminalScheduler::FailNextDiagnosticEmitForTest(throwException);
        TqTerminalScheduler::AdvanceForTest(std::chrono::seconds(6));
        auto metrics = TqTerminalMetricsSnapshot();
        auto diagnostics = TqTerminalRetentionDiagnosticsForTest();
        CHECK(metrics.SchedulerFailure == 1);
        CHECK(diagnostics.WarningLogs == 0);
        CHECK(diagnostics.CriticalLogs == 0);

        TqTerminalScheduler::AdvanceForTest(std::chrono::milliseconds(1));
        metrics = TqTerminalMetricsSnapshot();
        diagnostics = TqTerminalRetentionDiagnosticsForTest();
        CHECK(metrics.SchedulerFailure == 1);
        CHECK(diagnostics.WarningLogs == 1);
        CHECK(diagnostics.CriticalLogs == 0);
        TqTerminalScheduler::AdvanceForTest(std::chrono::milliseconds(1));
        CHECK(TqTerminalRetentionDiagnosticsForTest().WarningLogs == 1);
        QUIC_STREAM_EVENT terminal{};
        terminal.Type = QUIC_STREAM_EVENT_SHUTDOWN_COMPLETE;
        CHECK(owner->DispatchForTest(&terminal) == QUIC_STATUS_SUCCESS);
    }
}

void TestRealSchedulerPollsEmptyHeapAndKeysDiagnosticsByFullIdentity() {
    Reset();
    TqTerminalScheduler::UseRealClockForTest();
    TqTerminalScheduler::SetDiagnosticPollIntervalForTest(std::chrono::milliseconds(1));
    auto first = MakeStartedOwner(912);
    auto second = TqStreamLifetime::CreateForTest(TqStreamLifetime::Phase::CreatedNotStarted);
    second->BindTerminalIdentity(
        {912, 133, 3, 8, TqTunnelRole::ServerOpen,
         TqRelayBackendType::DarwinWorker}, 5);
    CHECK(second->BeginStart());
    TqTerminalScheduler::SetDiagnosticNowForTest(
        std::chrono::steady_clock::now() + std::chrono::seconds(31));
    TqTerminalScheduler::Instance().Start();
    for (unsigned i = 0; i != 200 &&
         TqTerminalRetentionDiagnosticsForTest().CriticalLogs != 2; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    auto diagnostics = TqTerminalRetentionDiagnosticsForTest();
    CHECK(diagnostics.WarningLogs == 2);
    CHECK(diagnostics.CriticalLogs == 2);
    CHECK(diagnostics.TrackedStreams == 2);
    QUIC_STREAM_EVENT terminal{};
    terminal.Type = QUIC_STREAM_EVENT_SHUTDOWN_COMPLETE;
    CHECK(first->DispatchForTest(&terminal) == QUIC_STATUS_SUCCESS);
    for (unsigned i = 0; i != 200 &&
         TqTerminalRetentionDiagnosticsForTest().TrackedStreams != 1; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    CHECK(TqTerminalRetentionDiagnosticsForTest().TrackedStreams == 1);
    CHECK(second->DispatchForTest(&terminal) == QUIC_STATUS_SUCCESS);
    for (unsigned i = 0; i != 200 &&
         TqTerminalRetentionDiagnosticsForTest().TrackedStreams != 0; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    CHECK(TqTerminalRetentionDiagnosticsForTest().TrackedStreams == 0);
    TqTerminalScheduler::Instance().Stop();
}

void TestShutdownStatusNamesAreStable() {
    CHECK(TqTerminalShutdownStatusName(QUIC_STATUS_SUCCESS) == "success");
    CHECK(TqTerminalShutdownStatusName(QUIC_STATUS_PENDING) == "pending");
    CHECK(TqTerminalShutdownStatusName(QUIC_STATUS_OUT_OF_MEMORY) == "out_of_memory");
    CHECK(TqTerminalShutdownStatusName(QUIC_STATUS_INVALID_STATE) == "invalid_state");
    CHECK(TqTerminalShutdownStatusName(static_cast<QUIC_STATUS>(0x12345678)) ==
          "unknown(305419896)");
}

void TestRetentionJsonUsesActualStatusAndUnixWallClock() {
    Reset();
    auto owner = MakeStartedOwner(913);
    const auto ledger = owner->TerminalLedger();
    const uint64_t before = static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count());
    const struct { QUIC_STATUS Status; const char* Name; bool Submitted; } cases[] = {
        {QUIC_STATUS_SUCCESS, "success", true},
        {QUIC_STATUS_PENDING, "pending", true},
        {QUIC_STATUS_OUT_OF_MEMORY, "out_of_memory", false},
        {QUIC_STATUS_INVALID_STATE, "invalid_state", false},
        {static_cast<QUIC_STATUS>(0x12345678), "unknown(305419896)", false},
    };
    uint32_t attempt = 0;
    for (const auto& item : cases) {
        ledger->RecordShutdown(item.Status, ++attempt, item.Submitted);
        const std::string json = TqTerminalRetentionsJson({});
        CHECK(json.find(std::string("\"shutdown_status\":\"") + item.Name + "\"") !=
              std::string::npos);
    }
    const auto submitted = ledger->Snapshot(std::chrono::steady_clock::now());
    const uint64_t after = static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count());
    CHECK(submitted.ShutdownSubmittedAtMs >= before);
    CHECK(submitted.ShutdownSubmittedAtMs <= after);
    QUIC_STREAM_EVENT terminal{};
    terminal.Type = QUIC_STREAM_EVENT_SHUTDOWN_COMPLETE;
    CHECK(owner->DispatchForTest(&terminal) == QUIC_STATUS_SUCCESS);
    const auto terminalSnapshot = ledger->Snapshot(std::chrono::steady_clock::now());
    CHECK(terminalSnapshot.TerminalObservedAtMs >= submitted.ShutdownSubmittedAtMs);
    CHECK(terminalSnapshot.TerminalObservedAtMs <= static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count()));
}

} // namespace

int main() {
    TestRetryBackoffAndWatchdogBoundaries();
    TestTerminalCancelsRetryAndWatchdog();
    TestSubmittedShutdownEscalatesAtFiveSecondsThenTimesOut();
    TestGracefulCompleteNeverArmsFatalWatchdog();
    TestSubmittedGracefulShutdownUpgradesToAbortExactlyOnce();
    TestSubmittedGracefulAbortUpgradeRetriesSyncFailure();
    TestTerminalBetweenOwnerCommitAndSchedulerArmIsIrreversible();
    TestTerminalBetweenOwnerCommitAndRetryScheduleIsIrreversible();
    TestCancelWinsAgainstTaskAlreadyRemovedFromHeap();
    TestSchedulerAllocationFailuresEscalateWithoutDamagingExistingTasks();
    TestRetryAndThreadStartFailuresEscalateOnce();
    TestCompletedStreamsLeaveNoSchedulerIndexEntries();
    TestStartWaitsForJoinableOldWorkerAndStopDrainsTasks();
    TestStopJoinRejectsScheduleAndEscalatesOnce();
    for (int iteration = 0; iteration != 64; ++iteration) {
        TestStopCannotSplitEnqueueFromWorkerStart();
    }
    TestTerminalSinkDoesNotOwnOwnerAndAccountsOnce();
    TestIdentityRebindKeepsOriginalLedger();
    TestSinkRejectsMissingOrMismatchedOwnerLedger();
    TestSinkPendingRollsBackWhenNeverPublishedOrShutdownRejected();
    TestSinkControlBlockFailureDoesNotConsumeAnotherPendingObligation();
    TestSinkPendingHandlesAlreadyTerminalAndMultipleSinks();
    TestSinkAndAlreadyTerminalCompletionRaceIsExactlyOnce();
    TestOwnerClaimsSendCompletionBeforeTerminalSink();
    TestSinkRecordsNonTerminalEvents();
    TestRetentionSnapshotFiltersFinalLedger();
    TestRetentionSnapshotCopiesLedgerBeforeReadingIt();
    TestRetentionAdminFilterIsStrictAndSchemaIsCanonical();
    TestRetentionAgeDiagnosticsLogThresholdsOnceAndClearOnTerminal();
    TestRetentionDiagnosticsAllocationFailuresDoNotConsumeOnceBits();
    TestRetentionDiagnosticEmitFailureRollsBackReservation();
    TestRealSchedulerPollsEmptyHeapAndKeysDiagnosticsByFullIdentity();
    TestShutdownStatusNamesAreStable();
    TestRetentionJsonUsesActualStatusAndUnixWallClock();
    return 0;
}
