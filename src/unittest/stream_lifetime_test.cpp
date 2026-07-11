#define TQ_UNIT_TESTING 1
#include "stream_lifetime.h"

#include <atomic>
#include <memory>
#include <condition_variable>
#include <cstdlib>
#include <mutex>
#include <new>
#include <thread>

#define CHECK(condition) do { if (!(condition)) std::abort(); } while (false)

const MsQuicApi* MsQuic = nullptr;

namespace {

void* g_handler = nullptr;
void* g_handlerContext = nullptr;
unsigned g_closeCount = 0;
unsigned g_shutdownCount = 0;
QUIC_STREAM_SHUTDOWN_FLAGS g_lastShutdownFlags = QUIC_STREAM_SHUTDOWN_FLAG_NONE;
bool g_shutdownFail = false;
bool g_shutdownBlock = false;
bool g_shutdownEntered = false;
bool g_shutdownRelease = false;
std::mutex g_gateLock;
std::condition_variable g_gateCv;
bool g_gateEntered = false;
bool g_gateRelease = false;

QUIC_STATUS QUIC_API BlockingCallback(
    MsQuicStream*, void*, QUIC_STREAM_EVENT*) noexcept {
    std::unique_lock<std::mutex> guard(g_gateLock);
    g_gateEntered = true;
    g_gateCv.notify_all();
    g_gateCv.wait(guard, [] { return g_gateRelease; });
    return QUIC_STATUS_SUCCESS;
}

void QUIC_API FakeSetCallbackHandler(HQUIC, void* handler, void* context) {
    g_handler = handler;
    g_handlerContext = context;
}

void QUIC_API FakeStreamClose(HQUIC) {
    ++g_closeCount;
}

QUIC_STATUS QUIC_API FakeStreamShutdown(
    HQUIC, QUIC_STREAM_SHUTDOWN_FLAGS flags, QUIC_UINT62) {
    {
        std::unique_lock<std::mutex> guard(g_gateLock);
        ++g_shutdownCount;
        g_lastShutdownFlags = flags;
        if (g_shutdownBlock && g_shutdownCount == 1) {
            g_shutdownEntered = true;
            g_gateCv.notify_all();
            g_gateCv.wait(guard, [] { return g_shutdownRelease; });
        }
    }
    return g_shutdownFail ? QUIC_STATUS_INTERNAL_ERROR : QUIC_STATUS_SUCCESS;
}

bool DispatchAdapter(HQUIC handle, QUIC_STREAM_EVENT& event) {
    if (g_handler == nullptr) {
        return false;
    }
    const auto callback = reinterpret_cast<QUIC_STREAM_CALLBACK_HANDLER>(g_handler);
    return QUIC_SUCCEEDED(callback(handle, g_handlerContext, &event));
}


class CountingTarget final : public TqStreamLifetime::Target {
public:
    QUIC_STATUS OnStreamEvent(
        MsQuicStream*,
        QUIC_STREAM_EVENT* event,
        uint64_t generation) noexcept override {
        ++Calls;
        LastType = event->Type;
        LastGeneration = generation;
        return QUIC_STATUS_SUCCESS;
    }

    std::atomic<unsigned> Calls{0};
    QUIC_STREAM_EVENT_TYPE LastType{QUIC_STREAM_EVENT_START_COMPLETE};
    uint64_t LastGeneration{0};
};

class CallbackGuardTarget final : public TqStreamLifetime::Target {
public:
    QUIC_STATUS OnStreamEvent(
        MsQuicStream*, QUIC_STREAM_EVENT*, uint64_t) noexcept override {
        CHECK(TqStreamLifetime::TestDetachedOwnerDestroyCountForTest() == 0);
        return QUIC_STATUS_SUCCESS;
    }
};

std::shared_ptr<TqStreamLifetime> MakeDetachedStartedOwner(
    std::shared_ptr<TqStreamLifetime::Target> target) {
    static QUIC_API_TABLE fakeApi{};
    static bool initialized = false;
    if (!initialized) {
        fakeApi.SetCallbackHandler = FakeSetCallbackHandler;
        fakeApi.StreamClose = FakeStreamClose;
        fakeApi.StreamShutdown = FakeStreamShutdown;
        initialized = true;
    }
    MsQuic = reinterpret_cast<const MsQuicApi*>(&fakeApi);
    static uintptr_t nextHandle = 100;
    return TqStreamLifetime::AdoptAccepted(
        reinterpret_cast<HQUIC>(nextHandle++), std::move(target),
        TqTerminalIdentity{
            nextHandle, nextHandle, 2, 7,
            TqTunnelRole::ClientOpen, TqRelayBackendType::LinuxWorker},
        5);
}

} // namespace

void TestBeginTerminalShutdownPendingIsSubmittedOnce() {
    auto target = std::make_shared<CountingTarget>();
    auto sink = std::make_shared<CountingTarget>();
    auto owner = MakeDetachedStartedOwner(target);
    QUIC_STREAM_SHUTDOWN_FLAGS seen = QUIC_STREAM_SHUTDOWN_FLAG_NONE;
    uint32_t calls = 0;
    owner->SetShutdownHookForTest([&](uint64_t, QUIC_STREAM_SHUTDOWN_FLAGS flags) {
        ++calls;
        seen = flags;
        CHECK(owner->TerminalLedger()->Snapshot(std::chrono::steady_clock::now()).Phase ==
              TerminalPhase::ShutdownReserved);
        return QUIC_STATUS_PENDING;
    });
    const auto first = owner->BeginTerminalShutdown(91, sink, nullptr);
    const auto duplicate = owner->BeginTerminalShutdown(91, sink, nullptr);
    CHECK(first.Status == QUIC_STATUS_PENDING);
    CHECK(first.Submitted && first.Attempt == 1);
    CHECK(duplicate.Submitted && duplicate.Attempt == 1);
    CHECK(calls == 1);
    CHECK((seen & QUIC_STREAM_SHUTDOWN_FLAG_ABORT) != 0);
    CHECK((seen & QUIC_STREAM_SHUTDOWN_FLAG_IMMEDIATE) != 0);
    CHECK(owner->GetTerminalPhase() == TerminalPhase::ShutdownSubmitted);
    CHECK(owner->TerminalLedger()->Snapshot(std::chrono::steady_clock::now()).Phase ==
          TerminalPhase::ShutdownSubmitted);
    QUIC_STREAM_EVENT terminal{};
    terminal.Type = QUIC_STREAM_EVENT_SHUTDOWN_COMPLETE;
    CHECK(owner->DispatchForTest(&terminal) == QUIC_STATUS_SUCCESS);
}

void TestBeginTerminalShutdownFailureReturnsToActive() {
    auto owner = MakeDetachedStartedOwner(std::make_shared<CountingTarget>());
    owner->SetShutdownHookForTest([](uint64_t, QUIC_STREAM_SHUTDOWN_FLAGS) {
        return QUIC_STATUS_INVALID_STATE;
    });
    const auto result = owner->BeginTerminalShutdown(
        92, std::make_shared<CountingTarget>(), nullptr);
    CHECK(QUIC_FAILED(result.Status));
    CHECK(!result.Submitted && result.Attempt == 1);
    CHECK(owner->GetTerminalPhase() == TerminalPhase::Active);
    QUIC_STREAM_EVENT terminal{};
    terminal.Type = QUIC_STREAM_EVENT_SHUTDOWN_COMPLETE;
    CHECK(owner->DispatchForTest(&terminal) == QUIC_STATUS_SUCCESS);
}

void TestTerminalObservedCannotBeDowngradedByLateShutdownRecord() {
    auto owner = MakeDetachedStartedOwner(std::make_shared<CountingTarget>());
    auto sink = std::make_shared<CountingTarget>();
    std::mutex gapLock;
    std::condition_variable gapCv;
    bool gapEntered = false;
    bool terminalComplete = false;
    owner->SetShutdownHookForTest([](uint64_t, QUIC_STREAM_SHUTDOWN_FLAGS) {
        return QUIC_STATUS_PENDING;
    });
    owner->SetBeforeTerminalLedgerRecordHookForTest([&] {
        std::unique_lock<std::mutex> guard(gapLock);
        gapEntered = true;
        gapCv.notify_all();
        gapCv.wait(guard, [&] { return terminalComplete; });
    });
    TqTerminalShutdownResult result{};
    std::thread shutdown([&] {
        result = owner->BeginTerminalShutdown(94, sink, nullptr);
    });
    {
        std::unique_lock<std::mutex> guard(gapLock);
        gapCv.wait(guard, [&] { return gapEntered; });
    }
    QUIC_STREAM_EVENT terminal{};
    terminal.Type = QUIC_STREAM_EVENT_SHUTDOWN_COMPLETE;
    CHECK(owner->DispatchForTest(&terminal) == QUIC_STATUS_SUCCESS);
    {
        std::lock_guard<std::mutex> guard(gapLock);
        terminalComplete = true;
    }
    gapCv.notify_all();
    shutdown.join();
    CHECK(result.Submitted);
    CHECK(owner->GetTerminalPhase() == TerminalPhase::TerminalObserved);
    CHECK(owner->TerminalLedger()->Snapshot(std::chrono::steady_clock::now()).Phase ==
          TerminalPhase::TerminalObserved);
}

void TestTerminalShutdownFailureCanRetryAndSubmit() {
    auto owner = MakeDetachedStartedOwner(std::make_shared<CountingTarget>());
    uint32_t calls = 0;
    owner->SetShutdownHookForTest([&](uint64_t, QUIC_STREAM_SHUTDOWN_FLAGS) {
        return ++calls == 1 ? QUIC_STATUS_INVALID_STATE : QUIC_STATUS_SUCCESS;
    });
    auto sink = std::make_shared<CountingTarget>();
    const auto first = owner->BeginTerminalShutdown(95, sink, nullptr);
    CHECK(QUIC_FAILED(first.Status));
    CHECK(first.Attempt == 1 && !first.Submitted);
    CHECK(owner->TerminalRetryOwnedForTest());
    const auto second = owner->BeginTerminalShutdown(95, sink, nullptr);
    CHECK(QUIC_SUCCEEDED(second.Status));
    CHECK(second.Attempt == 2 && second.Submitted);
    CHECK(calls == 2);
    CHECK(!owner->TerminalRetryOwnedForTest());
    CHECK(owner->GetTerminalPhase() == TerminalPhase::ShutdownSubmitted);
    const auto ledger = owner->TerminalLedger()->Snapshot(std::chrono::steady_clock::now());
    CHECK(ledger.Phase == TerminalPhase::ShutdownSubmitted);
    CHECK(ledger.ShutdownAttempt == 2);
    CHECK(ledger.ShutdownStatus == QUIC_STATUS_SUCCESS);
    QUIC_STREAM_EVENT terminal{};
    terminal.Type = QUIC_STREAM_EVENT_SHUTDOWN_COMPLETE;
    CHECK(owner->DispatchForTest(&terminal) == QUIC_STATUS_SUCCESS);
}

void TestTerminalReentryWinsShutdownReturn() {
    auto sink = std::make_shared<CountingTarget>();
    auto owner = MakeDetachedStartedOwner(std::make_shared<CountingTarget>());
    owner->SetShutdownHookForTest([&](uint64_t, QUIC_STREAM_SHUTDOWN_FLAGS) {
        QUIC_STREAM_EVENT event{};
        event.Type = QUIC_STREAM_EVENT_SHUTDOWN_COMPLETE;
        CHECK(owner->DispatchForTest(&event) == QUIC_STATUS_SUCCESS);
        return QUIC_STATUS_INVALID_STATE;
    });
    const auto result = owner->BeginTerminalShutdown(5, sink, nullptr);
    CHECK(result.AlreadyTerminal);
    CHECK(!result.RetryScheduled);
    CHECK(owner->GetTerminalPhase() == TerminalPhase::TerminalObserved);
}

void TestAbortBothImmediateAddsImmediateFlag() {
    auto owner = MakeDetachedStartedOwner(std::make_shared<CountingTarget>());
    g_shutdownCount = 0;
    g_lastShutdownFlags = QUIC_STREAM_SHUTDOWN_FLAG_NONE;
    CHECK(QUIC_SUCCEEDED(owner->RequestShutdown(
        TqStreamLifetime::ShutdownIntent::AbortBothImmediate, 93)));
    CHECK(g_shutdownCount == 1);
    CHECK((g_lastShutdownFlags & QUIC_STREAM_SHUTDOWN_FLAG_ABORT) != 0);
    CHECK((g_lastShutdownFlags & QUIC_STREAM_SHUTDOWN_FLAG_IMMEDIATE) != 0);
    QUIC_STREAM_EVENT terminal{};
    terminal.Type = QUIC_STREAM_EVENT_SHUTDOWN_COMPLETE;
    CHECK(owner->DispatchForTest(&terminal) == QUIC_STATUS_SUCCESS);
}

void TestAbortBothDoesNotAddImmediateFlag() {
    auto owner = MakeDetachedStartedOwner(std::make_shared<CountingTarget>());
    g_shutdownCount = 0;
    g_lastShutdownFlags = QUIC_STREAM_SHUTDOWN_FLAG_NONE;
    CHECK(QUIC_SUCCEEDED(owner->RequestShutdown(
        TqStreamLifetime::ShutdownIntent::AbortBoth, 96)));
    CHECK(g_shutdownCount == 1);
    CHECK((g_lastShutdownFlags & QUIC_STREAM_SHUTDOWN_FLAG_ABORT) != 0);
    CHECK((g_lastShutdownFlags & QUIC_STREAM_SHUTDOWN_FLAG_IMMEDIATE) == 0);
    QUIC_STREAM_EVENT terminal{};
    terminal.Type = QUIC_STREAM_EVENT_SHUTDOWN_COMPLETE;
    CHECK(owner->DispatchForTest(&terminal) == QUIC_STATUS_SUCCESS);
}

void TestAbortBothImmediateUpgradesInFlightAbortBoth() {
    auto owner = MakeDetachedStartedOwner(std::make_shared<CountingTarget>());
    {
        std::lock_guard<std::mutex> guard(g_gateLock);
        g_shutdownCount = 0;
        g_lastShutdownFlags = QUIC_STREAM_SHUTDOWN_FLAG_NONE;
        g_shutdownBlock = true;
        g_shutdownEntered = false;
        g_shutdownRelease = false;
    }
    QUIC_STATUS firstStatus = QUIC_STATUS_INTERNAL_ERROR;
    std::thread ordinary([&] {
        firstStatus = owner->RequestShutdown(
            TqStreamLifetime::ShutdownIntent::AbortBoth, 97);
    });
    {
        std::unique_lock<std::mutex> guard(g_gateLock);
        g_gateCv.wait(guard, [] { return g_shutdownEntered; });
    }
    const auto immediateStatus = owner->RequestShutdown(
        TqStreamLifetime::ShutdownIntent::AbortBothImmediate, 97);
    {
        std::lock_guard<std::mutex> guard(g_gateLock);
        g_shutdownRelease = true;
    }
    g_gateCv.notify_all();
    ordinary.join();
    CHECK(QUIC_SUCCEEDED(firstStatus));
    CHECK(QUIC_SUCCEEDED(immediateStatus));
    CHECK(g_shutdownCount == 2);
    CHECK((g_lastShutdownFlags & QUIC_STREAM_SHUTDOWN_FLAG_IMMEDIATE) != 0);
    g_shutdownBlock = false;
    QUIC_STREAM_EVENT terminal{};
    terminal.Type = QUIC_STREAM_EVENT_SHUTDOWN_COMPLETE;
    CHECK(owner->DispatchForTest(&terminal) == QUIC_STATUS_SUCCESS);
}

void TestTerminalCallbackGuardDefersOwnerDestructionUntilReturn() {
    TqStreamLifetime::ResetTestDetachedOwnerDestroyCountForTest();
    auto owner = TqStreamLifetime::CreateForTest(
        TqStreamLifetime::Phase::Started,
        std::make_shared<CallbackGuardTarget>());
    alignas(MsQuicStream) static unsigned char streamStorage[sizeof(MsQuicStream)];
    auto* stream = new (streamStorage) MsQuicStream(
        reinterpret_cast<HQUIC>(static_cast<uintptr_t>(999)),
        CleanUpManual, TqStreamLifetime::Callback, owner.get());
    CHECK(owner->InstallDetachedStreamForTest(stream));
    auto* callbackContext = owner.get();
    owner.reset();
    QUIC_STREAM_EVENT terminal{};
    terminal.Type = QUIC_STREAM_EVENT_SHUTDOWN_COMPLETE;
    CHECK(TqStreamLifetime::Callback(stream, callbackContext, &terminal) ==
          QUIC_STATUS_SUCCESS);
    CHECK(TqStreamLifetime::TestDetachedOwnerDestroyCountForTest() == 1);
}

void TestTerminalPublicInterfaceDefaults() {
    TqTerminalIdentity identity{};
    identity.StreamId = 17;
    identity.TunnelId = 133;
    identity.ConnectionId = 2;
    identity.ConnectionGeneration = 7;
    identity.Role = TqTunnelRole::ClientOpen;
    identity.Backend = TqRelayBackendType::LinuxWorker;
    auto owner = TqStreamLifetime::CreateForTest(
        TqStreamLifetime::Phase::CreatedNotStarted);
    owner->BindTerminalIdentity(identity, 5);
    auto ledger = owner->TerminalLedger();
    CHECK(ledger != nullptr);
    const auto snapshot = ledger->Snapshot(std::chrono::steady_clock::now());
    CHECK(snapshot.Identity.StreamId == 17);
    CHECK(snapshot.Phase == TerminalPhase::Active);
    CHECK(snapshot.ShutdownAttempt == 0);
    CHECK(snapshot.Watchdog == TqTerminalWatchdogState::Idle);
    CHECK(snapshot.LastStreamEvent == TqTerminalEvent::None);
    CHECK(!snapshot.ConnectionEscalated);
}

void TestBindTerminalIdentityCreatesExactlyOneLedger() {
    auto owner = TqStreamLifetime::CreateForTest(
        TqStreamLifetime::Phase::CreatedNotStarted);
    const TqTerminalIdentity identity{
        17, 133, 2, 7, TqTunnelRole::ClientOpen,
        TqRelayBackendType::LinuxWorker};
    owner->BindTerminalIdentity(identity, 5);
    const auto first = owner->TerminalLedger();
    owner->BindTerminalIdentity(identity, 5);
    CHECK(first != nullptr);
    CHECK(owner->TerminalLedger().get() == first.get());
}

int main() {
    TestTerminalPublicInterfaceDefaults();
    TestBindTerminalIdentityCreatesExactlyOneLedger();
    TestBeginTerminalShutdownPendingIsSubmittedOnce();
    TestBeginTerminalShutdownFailureReturnsToActive();
    TestTerminalObservedCannotBeDowngradedByLateShutdownRecord();
    TestTerminalShutdownFailureCanRetryAndSubmit();
    TestTerminalReentryWinsShutdownReturn();
    TestAbortBothImmediateAddsImmediateFlag();
    TestAbortBothDoesNotAddImmediateFlag();
    TestAbortBothImmediateUpgradesInFlightAbortBoth();
    TestTerminalCallbackGuardDefersOwnerDestructionUntilReturn();
    {
        int context = 1;
        auto target = std::make_shared<TqStreamCallbackTarget>(BlockingCallback, &context);
        QUIC_STREAM_EVENT event{};
        event.Type = QUIC_STREAM_EVENT_RECEIVE;
        g_gateEntered = false;
        g_gateRelease = false;
        std::thread callback([&] { (void)target->OnStreamEvent(nullptr, &event, 1); });
        {
            std::unique_lock<std::mutex> guard(g_gateLock);
            g_gateCv.wait(guard, [] { return g_gateEntered; });
        }
        std::atomic<bool> detached{false};
        std::thread detach([&] {
            target->Detach();
            detached.store(true, std::memory_order_release);
        });
        std::this_thread::yield();
        if (detached.load(std::memory_order_acquire)) return 32;
        {
            std::lock_guard<std::mutex> guard(g_gateLock);
            g_gateRelease = true;
        }
        g_gateCv.notify_all();
        callback.join();
        detach.join();
        if (!detached.load(std::memory_order_acquire)) return 33;
        if (target->OnStreamEvent(nullptr, &event, 1) != QUIC_STATUS_SUCCESS) return 34;
    }

    {
        auto target = std::make_shared<CountingTarget>();
        auto owner = TqStreamLifetime::CreateForTest(
            TqStreamLifetime::Phase::Started, target);
        const auto diagnosticsBefore = TqStreamLifetime::SnapshotSendCompletions();
        unsigned cleanups = 0;
        void* first = owner->RegisterSendCompletion(nullptr, [&] { ++cleanups; });
        if (first == nullptr) return 35;
        QUIC_STREAM_EVENT complete{};
        complete.Type = QUIC_STREAM_EVENT_SEND_COMPLETE;
        complete.SEND_COMPLETE.ClientContext = first;
        (void)owner->DispatchForTest(&complete);
        complete.SEND_COMPLETE.ClientContext = first;
        (void)owner->DispatchForTest(&complete);
        if (cleanups != 1 || target->Calls != 1) return 36;
        if (TqStreamLifetime::SnapshotSendCompletions().DuplicateClaims !=
            diagnosticsBefore.DuplicateClaims + 1) return 50;
        void* second = owner->RegisterSendCompletion(nullptr, [&] { ++cleanups; });
        if (second == nullptr || second == first) return 37;
        int unknown = 0;
        complete.SEND_COMPLETE.ClientContext = &unknown;
        (void)owner->DispatchForTest(&complete);
        if (cleanups != 1) return 38;
        if (TqStreamLifetime::SnapshotSendCompletions().UnknownClaims !=
            diagnosticsBefore.UnknownClaims + 1) return 51;
        QUIC_STREAM_EVENT terminal{};
        terminal.Type = QUIC_STREAM_EVENT_SHUTDOWN_COMPLETE;
        (void)owner->DispatchForTest(&terminal);
        complete.SEND_COMPLETE.ClientContext = second;
        (void)owner->DispatchForTest(&complete);
        if (cleanups != 2) return 39;
    }

    {
        // Reservation RAII: armed destructor cancels; Dismiss keeps registry entry.
        auto target = std::make_shared<CountingTarget>();
        auto owner = TqStreamLifetime::CreateForTest(
            TqStreamLifetime::Phase::Started, target);
        const auto baseline = TqStreamLifetime::SnapshotSendCompletions();
        unsigned cleanups = 0;
        {
            auto reservation = owner->ReserveSendCompletion(nullptr, [&] { ++cleanups; });
            if (!reservation) return 40;
            if (TqStreamLifetime::SnapshotSendCompletions().ActiveCount !=
                baseline.ActiveCount + 1) {
                return 41;
            }
        }
        if (cleanups != 1) return 42;
        if (TqStreamLifetime::SnapshotSendCompletions().ActiveCount != baseline.ActiveCount) {
            return 43;
        }

        auto kept = owner->ReserveSendCompletion(nullptr, [&] { ++cleanups; });
        if (!kept) return 44;
        void* key = kept.Key();
        kept.Dismiss();
        if (TqStreamLifetime::SnapshotSendCompletions().ActiveCount !=
            baseline.ActiveCount + 1) {
            return 45;
        }
        if (!owner->CancelSendCompletion(key)) return 46;
        if (cleanups != 2) return 47;
        if (TqStreamLifetime::SnapshotSendCompletions().ActiveCount != baseline.ActiveCount) {
            return 48;
        }
        if (TqStreamLifetime::SnapshotSendCompletions().OldestAgeMs != 0 &&
            TqStreamLifetime::SnapshotSendCompletions().ActiveCount != 0) {
            return 49;
        }
    }

    {
        QUIC_API_TABLE fakeApi{};
        fakeApi.SetCallbackHandler = FakeSetCallbackHandler;
        fakeApi.StreamClose = FakeStreamClose;
        fakeApi.StreamShutdown = FakeStreamShutdown;
        MsQuic = reinterpret_cast<const MsQuicApi*>(&fakeApi);
        g_handler = nullptr;
        g_handlerContext = nullptr;
        g_closeCount = 0;
        g_shutdownCount = 0;

        const HQUIC raw = reinterpret_cast<HQUIC>(static_cast<uintptr_t>(1));
        auto target = std::make_shared<CountingTarget>();
        auto owner = TqStreamLifetime::AdoptAccepted(
            raw,
            target,
            TqTerminalIdentity{
                1, 1, 1, 1,
                TqTunnelRole::ServerOpen, TqRelayBackendType::LinuxWorker},
            5);
        if (!owner) return 25;
        if (owner->StreamForInitialization()->CleanUpMode != CleanUpManual) return 26;
        if (QUIC_FAILED(owner->RequestShutdown(
                TqStreamLifetime::ShutdownIntent::GracefulSend))) return 43;
        if (QUIC_FAILED(owner->RequestShutdown(
                TqStreamLifetime::ShutdownIntent::GracefulSend))) return 44;
        if (g_shutdownCount != 1) return 45;
        if (QUIC_FAILED(owner->RequestShutdown(
                TqStreamLifetime::ShutdownIntent::AbortSend))) return 46;
        if (g_shutdownCount != 2 ||
            (g_lastShutdownFlags & QUIC_STREAM_SHUTDOWN_FLAG_ABORT_SEND) == 0) return 47;
        if (QUIC_FAILED(owner->RequestShutdown(
                TqStreamLifetime::ShutdownIntent::AbortReceive))) return 48;
        if (g_shutdownCount != 3) return 49;
        std::weak_ptr<TqStreamLifetime> weak = owner;
        owner.reset();
        if (weak.expired()) return 27;

        QUIC_STREAM_EVENT terminal{};
        terminal.Type = QUIC_STREAM_EVENT_SHUTDOWN_COMPLETE;
        if (!DispatchAdapter(raw, terminal)) return 28;
        if (!weak.expired()) return 29;
        if (g_closeCount != 1) return 30;
        if (target->Calls != 1) return 31;

        const HQUIC rejected = reinterpret_cast<HQUIC>(static_cast<uintptr_t>(2));
        g_shutdownCount = 0;
        TqStreamLifetime::RejectAccepted(rejected);
        if (g_shutdownCount != 1) return 40;
        QUIC_STREAM_EVENT rejectedTerminal{};
        rejectedTerminal.Type = QUIC_STREAM_EVENT_SHUTDOWN_COMPLETE;
        if (!DispatchAdapter(rejected, rejectedTerminal)) return 41;
        if (g_closeCount != 2) return 42;
        MsQuic = nullptr;
    }

    {
        auto first = std::make_shared<CountingTarget>();
        auto second = std::make_shared<CountingTarget>();
        auto owner = TqStreamLifetime::CreateForTest(
            TqStreamLifetime::Phase::CreatedNotStarted, first);
        if (!owner->BeginStart()) return 1;
        if (owner->GetPhase() != TqStreamLifetime::Phase::Starting) return 2;
        const uint64_t generation = owner->RouteGeneration();
        uint64_t published = 0;
        if (!owner->PublishTarget(generation, second, &published)) return 3;
        if (published == generation) return 4;
        if (owner->PublishTarget(generation, first)) return 5;

        QUIC_STREAM_EVENT start{};
        start.Type = QUIC_STREAM_EVENT_START_COMPLETE;
        start.START_COMPLETE.Status = QUIC_STATUS_SUCCESS;
        if (QUIC_FAILED(owner->DispatchForTest(&start))) return 6;
        if (owner->GetPhase() != TqStreamLifetime::Phase::Started) return 7;
        if (first->Calls != 0 || second->Calls != 1) return 8;
        if (second->LastGeneration != published) return 9;
    }

    {
        auto target = std::make_shared<CountingTarget>();
        auto owner = TqStreamLifetime::CreateForTest(
            TqStreamLifetime::Phase::Starting, target);
        std::weak_ptr<TqStreamLifetime> weak = owner;
        auto* callbackContext = owner.get();
        owner.reset();
        if (weak.expired()) return 10; // start/terminal retention

        QUIC_STREAM_EVENT terminal{};
        terminal.Type = QUIC_STREAM_EVENT_SHUTDOWN_COMPLETE;
        if (QUIC_FAILED(TqStreamLifetime::Callback(nullptr, callbackContext, &terminal))) return 11;
        if (target->Calls != 1) return 12;
        if (!weak.expired()) return 13; // callback guard在返回后才释放最后owner
    }

    {
        auto target = std::make_shared<CountingTarget>();
        auto owner = TqStreamLifetime::CreateForTest(
            TqStreamLifetime::Phase::Started, target);
        // test owner没有wrapper，因此不能取得API lease；phase/route仍按terminal seal。
        QUIC_STREAM_EVENT terminal{};
        terminal.Type = QUIC_STREAM_EVENT_SHUTDOWN_COMPLETE;
        if (QUIC_FAILED(owner->DispatchForTest(&terminal))) return 14;
        if (owner->GetPhase() != TqStreamLifetime::Phase::TerminalPublished) return 15;
        if (owner->PublishTarget(owner->RouteGeneration(), target)) return 16;
        if (owner->TryAcquireApi()) return 17;
        if (target->Calls != 1) return 18;
        if (QUIC_FAILED(owner->DispatchForTest(&terminal))) return 19;
        if (target->Calls != 1) return 20; // duplicate terminal once-only
    }

    {
        auto target = std::make_shared<CountingTarget>();
        auto owner = TqStreamLifetime::CreateForTest(
            TqStreamLifetime::Phase::Starting, target);
        QUIC_STREAM_EVENT failed{};
        failed.Type = QUIC_STREAM_EVENT_START_COMPLETE;
        failed.START_COMPLETE.Status = QUIC_STATUS_INTERNAL_ERROR;
        if (QUIC_FAILED(owner->DispatchForTest(&failed))) return 21;
        if (owner->GetPhase() != TqStreamLifetime::Phase::StartFailed) return 22;
        if (target->Calls != 1) return 23;
        if (owner->PublishTarget(owner->RouteGeneration(), target)) return 24;
        if (owner->TryAcquireApi()) return 50;
    }

    {
        auto target = std::make_shared<CountingTarget>();
        auto owner = TqStreamLifetime::CreateForTest(
            TqStreamLifetime::Phase::CreatedNotStarted, target);
        if (!owner->BeginStart()) return 51;
        QUIC_STREAM_EVENT earlyStart{};
        earlyStart.Type = QUIC_STREAM_EVENT_START_COMPLETE;
        earlyStart.START_COMPLETE.Status = QUIC_STATUS_SUCCESS;
        if (QUIC_FAILED(owner->DispatchForTest(&earlyStart))) return 52;
        if (owner->GetPhase() != TqStreamLifetime::Phase::Started) return 53;
        if (target->Calls != 1) return 54;
    }

    {
        auto target = std::make_shared<CountingTarget>();
        auto owner = TqStreamLifetime::CreateForTest(
            TqStreamLifetime::Phase::Starting, target);
        unsigned sendCleanups = 0;
        void* sendKey = owner->RegisterSendCompletion(
            reinterpret_cast<void*>(static_cast<uintptr_t>(99)),
            [&] { ++sendCleanups; });
        if (sendKey == nullptr) return 55;
        QUIC_STREAM_EVENT failedStart{};
        failedStart.Type = QUIC_STREAM_EVENT_START_COMPLETE;
        failedStart.START_COMPLETE.Status = QUIC_STATUS_INTERNAL_ERROR;
        if (QUIC_FAILED(owner->DispatchForTest(&failedStart))) return 56;
        if (owner->GetPhase() != TqStreamLifetime::Phase::StartFailed) return 57;
        if (sendCleanups != 0) return 58;
        if (owner->PublishTarget(owner->RouteGeneration(), target)) return 59;
        if (owner->TryAcquireApi()) return 60;
        QUIC_STREAM_EVENT canceledSend{};
        canceledSend.Type = QUIC_STREAM_EVENT_SEND_COMPLETE;
        canceledSend.SEND_COMPLETE.ClientContext = sendKey;
        canceledSend.SEND_COMPLETE.Canceled = TRUE;
        if (QUIC_FAILED(owner->DispatchForTest(&canceledSend))) return 61;
        if (sendCleanups != 1) return 62;
    }

    {
        auto target = std::make_shared<CountingTarget>();
        auto owner = TqStreamLifetime::CreateForTest(
            TqStreamLifetime::Phase::Started, target);
        QUIC_STREAM_EVENT sendShutdown{};
        sendShutdown.Type = QUIC_STREAM_EVENT_SEND_SHUTDOWN_COMPLETE;
        sendShutdown.SEND_SHUTDOWN_COMPLETE.Graceful = TRUE;
        if (QUIC_FAILED(owner->DispatchForTest(&sendShutdown))) return 63;
        if (!owner->SendDirectionCompleteForTest()) return 64;
        if (owner->GetPhase() != TqStreamLifetime::Phase::Started) return 65;
        if (target->Calls != 1) return 66;
    }

    {
        auto target = std::make_shared<CountingTarget>();
        auto owner = TqStreamLifetime::CreateForTest(
            TqStreamLifetime::Phase::Started, target);
        QUIC_STREAM_EVENT cancelOnLoss{};
        cancelOnLoss.Type = QUIC_STREAM_EVENT_CANCEL_ON_LOSS;
        cancelOnLoss.CANCEL_ON_LOSS.ErrorCode = 0;
        if (QUIC_FAILED(owner->DispatchForTest(&cancelOnLoss))) return 67;
        if (owner->CancelOnLossErrorCodeForTest() != 0x54515043ULL) return 68;
        if (cancelOnLoss.CANCEL_ON_LOSS.ErrorCode != 0x54515043ULL) return 69;
        if (owner->GetPhase() != TqStreamLifetime::Phase::Started) return 70;
        if (target->Calls != 1) return 71;
    }

    {
        auto target = std::make_shared<CountingTarget>();
        auto owner = TqStreamLifetime::CreateForTest(
            TqStreamLifetime::Phase::Started, target);
        unsigned sendCleanups = 0;
        void* sendKey = owner->RegisterSendCompletion(nullptr, [&] { ++sendCleanups; });
        if (sendKey == nullptr) return 72;
        QUIC_STREAM_EVENT terminal{};
        terminal.Type = QUIC_STREAM_EVENT_SHUTDOWN_COMPLETE;
        (void)owner->DispatchForTest(&terminal);
        if (owner->GetPhase() != TqStreamLifetime::Phase::TerminalPublished) return 73;
        QUIC_STREAM_EVENT complete{};
        complete.Type = QUIC_STREAM_EVENT_SEND_COMPLETE;
        complete.SEND_COMPLETE.ClientContext = sendKey;
        (void)owner->DispatchForTest(&complete);
        if (sendCleanups != 1 || target->Calls != 2) return 74;
        (void)owner->DispatchForTest(&complete);
        if (sendCleanups != 1) return 75;
        if (owner->InjectSendCompletionForTest(
                sendKey, nullptr, [] {})) {
            return 76;
        }
    }

    return 0;
}
