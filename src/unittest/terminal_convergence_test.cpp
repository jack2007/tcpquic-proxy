#define TQ_UNIT_TESTING 1
#include "stream_lifetime.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <memory>
#include <mutex>
#include <thread>

#define CHECK(condition) do { if (!(condition)) std::abort(); } while (false)

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
    TqStreamLifetime::ResetLifecycleRegistriesForTest();
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
    auto late = TqTerminalSink::Create(owner, ledger);
    CHECK(TqTerminalMetricsSnapshot().TerminalSinkPending == 1);
    CHECK(owner->BeginTerminalShutdown(91, late, nullptr).AlreadyTerminal);
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

} // namespace

int main() {
    TestTerminalSinkDoesNotOwnOwnerAndAccountsOnce();
    TestIdentityRebindKeepsOriginalLedger();
    TestSinkRejectsMissingOrMismatchedOwnerLedger();
    TestSinkPendingRollsBackWhenNeverPublishedOrShutdownRejected();
    TestSinkControlBlockFailureDoesNotConsumeAnotherPendingObligation();
    TestSinkPendingHandlesAlreadyTerminalAndMultipleSinks();
    TestOwnerClaimsSendCompletionBeforeTerminalSink();
    TestSinkRecordsNonTerminalEvents();
    TestRetentionSnapshotFiltersFinalLedger();
    TestRetentionSnapshotCopiesLedgerBeforeReadingIt();
    return 0;
}
