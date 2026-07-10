#if defined(__APPLE__)

#include "darwin_relay_worker.h"
#include "relay_metrics.h"

#include <sys/socket.h>
#include <unistd.h>

#include <cstdio>
#include <cstdlib>
#include <sstream>
#include <string>

namespace {

bool Contains(const std::string& text, const char* needle) {
    return text.find(needle) != std::string::npos;
}

void CheckContains(const std::string& text, const char* needle) {
    if (!Contains(text, needle)) {
        std::fprintf(stderr, "missing JSON fragment: %s\n", needle);
        std::abort();
    }
}

void Check(bool condition, const char* expression) {
    if (!condition) {
        std::fprintf(stderr, "check failed: %s\n", expression);
        std::abort();
    }
}

void CloseSocketPairAfterRelayOwned(int relayTcpFd, int fds[2]) {
    const int peerFd = relayTcpFd == fds[0] ? fds[1] : fds[0];
    Check(close(peerFd) == 0, "close peer fd");
}

void EventizedSnapshotCountsActiveRelay() {
    int fds[2]{TqInvalidSocket, TqInvalidSocket};
    Check(socketpair(AF_UNIX, SOCK_STREAM, 0, fds) == 0, "socketpair succeeds");

    TqDarwinRelayWorker worker(TqDarwinRelayWorkerConfig{});
    TqRelayHandle handle{};
    Check(worker.Start(), "worker.Start()");

    TqDarwinRelayRegistration registration{};
    registration.TcpFd = fds[0];
    registration.Stream = reinterpret_cast<MsQuicStream*>(static_cast<uintptr_t>(1));
    registration.Handle = &handle;

    const TqDarwinRelayRegistrationResult result = worker.RegisterRelayWithId(registration);
    Check(result.Ok, "RegisterRelayWithId ok");

    const TqDarwinRelayWorkerSnapshot snapshot = worker.Snapshot();
    Check(snapshot.Errors == 0, "snapshot.Errors == 0");
    Check(snapshot.ActiveRelays == 1, "snapshot.ActiveRelays == 1");

    worker.UnregisterRelay(result.RelayId);
    worker.Stop();
    CloseSocketPairAfterRelayOwned(registration.TcpFd, fds);
}

} // namespace

int main() {
    TqRelayMetricsSnapshot runtimeSnapshot = TqSnapshotRelayMetrics();
    Check(std::string(runtimeSnapshot.Backend) == "kqueue", "runtimeSnapshot.Backend == kqueue");

    TqRelayMetricsSnapshot metrics{};
    metrics.Backend = "test";
    metrics.ActiveRelays = 7;
    metrics.Errors = 3;
    metrics.OutstandingQuicSendBytes = 11;
    metrics.TcpReadBatches = 22;
    metrics.TcpWriteBatches = 33;
    metrics.PendingBytes = 44;
    metrics.CurrentPendingQuicReceiveBytes = 55;
    metrics.PendingTcpWriteQueue = 66;
    metrics.PendingTcpWriteBytes = 77;
    metrics.QuicReceiveViewCount = 88;
    metrics.QuicReceiveViewBytes = 99;
    metrics.DeferredReceiveCompletes = 111;
    metrics.QuicSendBackpressureEvents = 222;
    metrics.LinuxRelayTerminalRetainedOwnerCount = 3;
    metrics.LinuxRelayTerminalRetainedOldestAgeMs = 42;
    metrics.LinuxRelayStopRemaining = 5;

    const std::string json = TqRelayMetricsFieldsJson(metrics);
    CheckContains(json, "\"relay_backend\":\"test\"");
    CheckContains(json, "\"linux_relay_backend\":\"test\"");
    CheckContains(json, "\"relay_active_relays\":7");
    CheckContains(json, "\"linux_relay_active_relays\":7");
    CheckContains(json, "\"relay_errors\":3");
    CheckContains(json, "\"linux_relay_errors\":3");
    CheckContains(json, "\"linux_relay_outstanding_quic_send_bytes\":11");
    CheckContains(json, "\"linux_relay_tcp_read_batches\":22");
    CheckContains(json, "\"linux_relay_tcp_write_batches\":33");
    CheckContains(json, "\"linux_relay_pending_bytes\":44");
    CheckContains(json, "\"linux_relay_current_pending_quic_receive_bytes\":55");
    CheckContains(json, "\"linux_relay_pending_tcp_write_queue\":66");
    CheckContains(json, "\"linux_relay_pending_tcp_write_bytes\":77");
    CheckContains(json, "\"linux_relay_quic_receive_view_count\":88");
    CheckContains(json, "\"linux_relay_quic_receive_view_bytes\":99");
    CheckContains(json, "\"linux_relay_deferred_receive_completes\":111");
    CheckContains(json, "\"linux_relay_quic_send_backpressure_events\":222");
    CheckContains(json, "\"linux_relay_terminal_retained_owner_count\":3");
    CheckContains(json, "\"linux_relay_terminal_retained_oldest_age_ms\":42");
    CheckContains(json, "\"linux_relay_stop_remaining\":5");
    EventizedSnapshotCountsActiveRelay();
    return 0;
}

#else
int main() { return 0; }
#endif
