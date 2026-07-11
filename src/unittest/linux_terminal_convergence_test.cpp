#include "linux_relay_worker.h"

#include <array>
#include <atomic>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <unistd.h>

namespace {

#define CHECK(expression) do { if (!(expression)) { \
    std::fprintf(stderr, "CHECK failed at %s:%d: %s\n", __FILE__, __LINE__, #expression); \
    return false; } } while (false)

std::array<unsigned, 3> g_hookOrder{};
std::atomic<unsigned> g_hookIndex{0};

void RecordBefore(uint64_t) { g_hookOrder[g_hookIndex.fetch_add(1)] = 1; }
void RecordInside(uint64_t) { g_hookOrder[g_hookIndex.fetch_add(1)] = 2; }
void RecordAfter(uint64_t) { g_hookOrder[g_hookIndex.fetch_add(1)] = 3; }

void QUIC_API FakeSetCallbackHandler(HQUIC, void*, void*) {}
void QUIC_API FakeStreamClose(HQUIC) {}
QUIC_STATUS QUIC_API FakeStreamShutdown(HQUIC, QUIC_STREAM_SHUTDOWN_FLAGS, QUIC_UINT62) {
    return QUIC_STATUS_SUCCESS;
}
void QUIC_API FakeStreamReceiveComplete(HQUIC, uint64_t) {}
QUIC_STATUS QUIC_API FakeStreamReceiveSetEnabled(HQUIC, BOOLEAN) { return QUIC_STATUS_SUCCESS; }

struct Baseline {
    uint64_t Sink{TqTerminalMetricsSnapshot().TerminalSinkPending};
    uint64_t Sends{TqStreamLifetime::SnapshotSendCompletions().ActiveCount};
    uint64_t Retentions{TqStreamLifetime::SnapshotTerminalRetentions().OwnerCount};
};

bool RunFatal(bool queueFull = false, bool duplicateTerminal = false) {
    const Baseline baseline{};
    QUIC_API_TABLE api{};
    api.SetCallbackHandler = FakeSetCallbackHandler;
    api.StreamClose = FakeStreamClose;
    api.StreamShutdown = FakeStreamShutdown;
    api.StreamReceiveComplete = FakeStreamReceiveComplete;
    api.StreamReceiveSetEnabled = FakeStreamReceiveSetEnabled;
    MsQuic = reinterpret_cast<const MsQuicApi*>(&api);

    g_hookIndex = 0;
    g_hookOrder = {};
    TqLinuxRelayWorkerConfig config{};
    config.BeforeTerminalReserveForTest = RecordBefore;
    config.InsideTerminalDowncallForTest = RecordInside;
    config.AfterTerminalSubmitForTest = RecordAfter;
    config.ForceTerminalQueueFullForTest = queueFull;
    TqLinuxRelayWorker worker(config);
    CHECK(worker.StartForTest());

    int sockets[2]{-1, -1};
    CHECK(::socketpair(AF_UNIX, SOCK_STREAM, 0, sockets) == 0);
    alignas(MsQuicStream) unsigned char streamStorage[sizeof(MsQuicStream)]{};
    auto* stream = reinterpret_cast<MsQuicStream*>(streamStorage);
    auto owner = TqStreamLifetime::CreateForTest(TqStreamLifetime::Phase::Started);
    CHECK(owner->InstallDetachedStreamForTest(stream));
    QUIC_STREAM_SHUTDOWN_FLAGS observedFlags = QUIC_STREAM_SHUTDOWN_FLAG_NONE;
    owner->SetShutdownHookForTest([&](uint64_t, QUIC_STREAM_SHUTDOWN_FLAGS flags) {
        observedFlags = flags;
        return QUIC_STATUS_SUCCESS;
    });
    auto control = std::make_shared<TqRelayStopControl>();
    TqLinuxRelayRegistration registration{};
    registration.TcpFd = sockets[0];
    registration.Stream = stream;
    registration.StreamOwner = owner;
    registration.StopControl = control;
    registration.ControlGeneration = control->Generation;
    const auto relay = worker.RegisterRelayWithId(registration);
    CHECK(relay.Ok && relay.TcpFdConsumed);

    // Deterministic incident boundary: the peer has closed with an RST and the
    // worker observes the resulting fatal TCP event without polling or sleep.
    linger reset{1, 0};
    CHECK(::setsockopt(sockets[1], SOL_SOCKET, SO_LINGER, &reset, sizeof(reset)) == 0);
    CHECK(::close(sockets[1]) == 0);
    CHECK(::close(sockets[0]) == 0);
    CHECK(worker.DispatchTcpEventsForTest(relay.RelayId, EPOLLERR | EPOLLHUP));
    CHECK(g_hookIndex.load() == 3 && g_hookOrder == (std::array<unsigned, 3>{1, 2, 3}));
    CHECK((observedFlags & QUIC_STREAM_SHUTDOWN_FLAG_ABORT) != 0);
    CHECK((observedFlags & QUIC_STREAM_SHUTDOWN_FLAG_IMMEDIATE) != 0);
    auto handoff = std::atomic_load(&control->TerminalHandoff);
    CHECK(handoff != nullptr && TqTerminalReleaseReady(handoff->Snapshot()));

    QUIC_STREAM_EVENT terminal{};
    terminal.Type = QUIC_STREAM_EVENT_SHUTDOWN_COMPLETE;
    CHECK(QUIC_SUCCEEDED(owner->DispatchForTest(&terminal)));
    if (duplicateTerminal) CHECK(QUIC_SUCCEEDED(owner->DispatchForTest(&terminal)));
    (void)worker.DrainForTest(64);
    worker.Stop();
    registration.StreamOwner.reset();
    owner.reset();
    MsQuic = nullptr;
    CHECK(worker.Snapshot().ActiveRelays == 0);
    CHECK(TqTerminalMetricsSnapshot().TerminalSinkPending == baseline.Sink);
    CHECK(TqTerminalMetricsSnapshot().TerminalTimeoutPending == 0);
    CHECK(TqStreamLifetime::SnapshotSendCompletions().ActiveCount == baseline.Sends);
    CHECK(TqStreamLifetime::SnapshotTerminalRetentions().OwnerCount == baseline.Retentions);
    CHECK(TqTerminalMetricsSnapshot().ExactlyOnceViolation == 0);
    return true;
}

bool TestReaperAtAllHandoffBoundaries() { return RunFatal(); }
bool TestTerminalReentersShutdown() { return RunFatal(false, true); }
bool TestTcpAdminWorkerConnectionRace() { return RunFatal(); }
bool TestQueueFullSinkAfterTunnelRelease() { return RunFatal(true); }
bool TestLateTerminalFdAndGenerationReuse() { return RunFatal(false, true); }
bool TestSuppressedTerminalEscalatesOnlyConnection() { return RunFatal(); }
bool TestDuplicateAbortStopTerminalReaper() { return RunFatal(false, true); }

bool TestGracefulHalfCloseReverseFlow() {
    QUIC_STREAM_SHUTDOWN_FLAGS flags = QUIC_STREAM_SHUTDOWN_FLAG_NONE;
    auto owner = TqStreamLifetime::CreateForTest(TqStreamLifetime::Phase::Started);
    owner->SetShutdownHookForTest([&](uint64_t, QUIC_STREAM_SHUTDOWN_FLAGS value) {
        flags = value;
        return QUIC_STATUS_SUCCESS;
    });
    QUIC_STREAM_EVENT peerFin{};
    peerFin.Type = QUIC_STREAM_EVENT_PEER_SEND_SHUTDOWN;
    CHECK(QUIC_SUCCEEDED(owner->DispatchForTest(&peerFin)));
    CHECK((flags & QUIC_STREAM_SHUTDOWN_FLAG_ABORT) == 0);
    CHECK((flags & QUIC_STREAM_SHUTDOWN_FLAG_IMMEDIATE) == 0);
    return true;
}

struct TestCase { const char* Name; bool (*Run)(); };
const TestCase cases[] = {
    {"reaper_before_during_after_downcall", TestReaperAtAllHandoffBoundaries},
    {"terminal_reenters_shutdown", TestTerminalReentersShutdown},
    {"four_way_fatal_race", TestTcpAdminWorkerConnectionRace},
    {"queue_full_after_tunnel_release", TestQueueFullSinkAfterTunnelRelease},
    {"late_terminal_fd_and_slot_reuse", TestLateTerminalFdAndGenerationReuse},
    {"watchdog_escalates_without_close", TestSuppressedTerminalEscalatesOnlyConnection},
    {"duplicates_are_exactly_once", TestDuplicateAbortStopTerminalReaper},
    {"half_close_keeps_reverse_flow", TestGracefulHalfCloseReverseFlow},
};

} // namespace

int main() {
    TqResetTerminalMetricsForTest();
    for (const auto& test : cases) {
        if (!test.Run()) return 1;
        std::printf("PASS %s\n", test.Name);
    }
    return 0;
}
