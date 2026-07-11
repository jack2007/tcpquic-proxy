#define TQ_UNIT_TESTING 1
#include "stream_lifetime.h"

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <memory>

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
}

} // namespace

int main() {
    TestTerminalSinkDoesNotOwnOwnerAndAccountsOnce();
    TestIdentityRebindKeepsOriginalLedger();
    TestSinkRejectsMissingOrMismatchedOwnerLedger();
    TestSinkRecordsNonTerminalEvents();
    TestRetentionSnapshotFiltersFinalLedger();
    return 0;
}
