#include "quic_session.h"
#include "quic_address.h"

#include <atomic>
#include <chrono>
#include <functional>
#include <thread>

static int TestFixedDelayRetrySchedulesAndRestartsSlot() {
    QuicClientSession session;
    std::atomic<int> scheduled{0};
    std::atomic<int> startCalls{0};
    std::function<void()> retryTask;

    session.MarkReconnectStartedForTest(1);
    session.SetReconnectTestHooks(QuicClientSession::ReconnectTestHooks{
        [&](size_t index) {
            if (index != 0) {
                return false;
            }
            startCalls.fetch_add(1, std::memory_order_relaxed);
            return true;
        }});
    session.SetDelayedTaskScheduler(
        [&](std::chrono::milliseconds delay, std::function<void()> task) {
            if (delay != std::chrono::milliseconds(3000)) {
                return false;
            }
            scheduled.fetch_add(1, std::memory_order_relaxed);
            retryTask = std::move(task);
            return true;
        });

    session.ScheduleStartRetryForTest(0);
    if (scheduled.load(std::memory_order_relaxed) != 1 || !retryTask) {
        return 10;
    }
    retryTask();
    if (startCalls.load(std::memory_order_relaxed) != 1) {
        return 11;
    }
    session.Stop();
    return 0;
}

static int TestDelayedRetryDropsAfterStop() {
    QuicClientSession session;
    std::atomic<int> startCalls{0};
    std::function<void()> retryTask;

    session.MarkReconnectStartedForTest(1);
    session.SetReconnectTestHooks(QuicClientSession::ReconnectTestHooks{
        [&](size_t) {
            startCalls.fetch_add(1, std::memory_order_relaxed);
            return true;
        }});
    session.SetDelayedTaskScheduler(
        [&](std::chrono::milliseconds delay, std::function<void()> task) {
            if (delay != std::chrono::milliseconds(3000)) {
                return false;
            }
            retryTask = std::move(task);
            return true;
        });

    session.ScheduleStartRetryForTest(0);
    if (!retryTask) {
        return 20;
    }
    session.Stop();
    retryTask();
    return startCalls.load(std::memory_order_relaxed) == 0 ? 0 : 21;
}

static int TestFixedDelayRetryCoalescesDuplicates() {
    QuicClientSession session;
    std::atomic<int> scheduled{0};
    std::atomic<int> startCalls{0};
    std::function<void()> retryTask;

    session.MarkReconnectStartedForTest(1);
    session.SetReconnectTestHooks(QuicClientSession::ReconnectTestHooks{
        [&](size_t) {
            startCalls.fetch_add(1, std::memory_order_relaxed);
            return true;
        }});
    session.SetDelayedTaskScheduler(
        [&](std::chrono::milliseconds, std::function<void()> task) {
            scheduled.fetch_add(1, std::memory_order_relaxed);
            retryTask = std::move(task);
            return true;
        });

    session.ScheduleStartRetryForTest(0);
    session.ScheduleStartRetryForTest(0);
    if (scheduled.load(std::memory_order_relaxed) != 1 || !retryTask) {
        return 30;
    }
    retryTask();
    if (startCalls.load(std::memory_order_relaxed) != 1) {
        return 31;
    }
    session.Stop();
    return 0;
}

static int TestRejectedSchedulerAllowsLaterRetry() {
    QuicClientSession session;
    std::atomic<int> rejectedCalls{0};
    std::atomic<int> acceptedCalls{0};
    std::function<void()> retryTask;

    session.MarkReconnectStartedForTest(1);
    session.SetDelayedTaskScheduler(
        [&](std::chrono::milliseconds, std::function<void()>) {
            rejectedCalls.fetch_add(1, std::memory_order_relaxed);
            return false;
        });
    session.ScheduleStartRetryForTest(0);
    if (rejectedCalls.load(std::memory_order_relaxed) != 1) {
        return 40;
    }

    session.SetDelayedTaskScheduler(
        [&](std::chrono::milliseconds, std::function<void()> task) {
            acceptedCalls.fetch_add(1, std::memory_order_relaxed);
            retryTask = std::move(task);
            return true;
        });
    session.ScheduleStartRetryForTest(0);
    if (acceptedCalls.load(std::memory_order_relaxed) != 1 || !retryTask) {
        return 41;
    }
    session.Stop();
    return 0;
}

static int TestConnectionStartPendingIsAccepted() {
    if (!QuicClientSession::ConnectionStartAcceptedForTest(QUIC_STATUS_SUCCESS)) {
        return 50;
    }
    if (!QuicClientSession::ConnectionStartAcceptedForTest(QUIC_STATUS_PENDING)) {
        return 51;
    }
    if (QuicClientSession::ConnectionStartAcceptedForTest(QUIC_STATUS_ABORTED)) {
        return 52;
    }
    return 0;
}

static int TestShutdownCompleteSchedulesSlotRestart() {
    QuicClientSession session;
    std::atomic<int> scheduled{0};
    std::atomic<int> startCalls{0};
    std::function<void()> retryTask;

    session.MarkReconnectStartedForTest(1);
    session.SetReconnectTestHooks(QuicClientSession::ReconnectTestHooks{
        [&](size_t index) {
            if (index != 0) {
                return false;
            }
            startCalls.fetch_add(1, std::memory_order_relaxed);
            return true;
        }});
    session.SetDelayedTaskScheduler(
        [&](std::chrono::milliseconds delay, std::function<void()> task) {
            if (delay != std::chrono::milliseconds(3000)) {
                return false;
            }
            scheduled.fetch_add(1, std::memory_order_relaxed);
            retryTask = std::move(task);
            return true;
        });

    session.RestartSlotAfterShutdownCompleteForTest(0, 0);
    if (scheduled.load(std::memory_order_relaxed) != 1 || !retryTask) {
        return 75;
    }
    if (startCalls.load(std::memory_order_relaxed) != 0) {
        return 76;
    }
    retryTask();
    if (startCalls.load(std::memory_order_relaxed) != 1) {
        return 77;
    }
    session.Stop();
    return 0;
}

static int TestConnectionSnapshotAndSlotControls() {
    QuicClientSession session;
    std::atomic<int> startCalls{0};
    session.MarkReconnectStartedForTest(2);
    session.SetReconnectTestHooks(QuicClientSession::ReconnectTestHooks{
        [&](size_t) {
            startCalls.fetch_add(1, std::memory_order_relaxed);
            return true;
        }});

    auto snapshots = session.SnapshotConnections();
    if (snapshots.size() != 2) return 80;
    if (snapshots[0].ConnectionId != "conn-0") return 81;
    if (snapshots[1].ConnectionId != "conn-1") return 82;
    if (snapshots[0].SlotIndex != 0 || snapshots[1].SlotIndex != 1) return 83;
    if (snapshots[0].Generation != 0) return 84;
    if (snapshots[0].State != "connecting") return 85;

    std::string err;
    if (!session.SetDesiredConnectionCount(3, err)) return 86;
    if (startCalls.load(std::memory_order_relaxed) != 1) return 96;
    snapshots = session.SnapshotConnections();
    if (snapshots.size() != 3) return 87;
    if (snapshots[2].ConnectionId != "conn-2") return 88;

    if (session.StopHighestConnection("conn-0", err)) return 89;
    if (err.find("highest") == std::string::npos) return 90;

    if (!session.ReconnectConnection("conn-1", err)) return 91;
    if (startCalls.load(std::memory_order_relaxed) != 2) return 97;
    snapshots = session.SnapshotConnections();
    if (snapshots[1].Generation != 1) return 92;

    if (!session.StopHighestConnection("conn-2", err)) return 93;
    snapshots = session.SnapshotConnections();
    if (snapshots.size() != 2) return 94;

    if (!session.AbortConnectionTunnels("conn-1", err)) return 95;
    session.Stop();
    return 0;
}

static int TestQuicPathSlotExpansion() {
    TqConfig cfg;
    cfg.QuicPaths.push_back(TqQuicPathConfig{"cmcc", "10.10.1.2", "36.1.1.10:443", 2});
    cfg.QuicPaths.push_back(TqQuicPathConfig{"ctcc", "10.20.1.2", "59.1.1.10:443", 1});

    std::vector<TqClientSlotPath> slots;
    std::string err;
    if (!TqBuildClientSlotPaths(cfg, slots, err)) return 100;
    if (slots.size() != 3) return 101;
    if (slots[0].Name != "cmcc" ||
        slots[0].LocalAddress != "10.10.1.2" ||
        slots[0].PeerHost != "36.1.1.10" ||
        slots[0].PeerPort != 443 ||
        slots[0].PeerText != "36.1.1.10:443") {
        return 102;
    }
    if (slots[1].Name != "cmcc" ||
        slots[1].LocalAddress != "10.10.1.2" ||
        slots[1].PeerHost != "36.1.1.10" ||
        slots[1].PeerPort != 443 ||
        slots[1].PeerText != "36.1.1.10:443") {
        return 103;
    }
    if (slots[2].Name != "ctcc" ||
        slots[2].LocalAddress != "10.20.1.2" ||
        slots[2].PeerHost != "59.1.1.10" ||
        slots[2].PeerPort != 443 ||
        slots[2].PeerText != "59.1.1.10:443") {
        return 104;
    }
    return 0;
}

static int TestQuicPeerListSlotExpansionRoundRobin() {
    TqConfig cfg;
    cfg.QuicPeer = "36.1.1.10:443,59.1.1.10:8443";
    cfg.QuicConnections = 5;

    std::vector<TqClientSlotPath> slots;
    std::string err;
    if (!TqBuildClientSlotPaths(cfg, slots, err)) return 110;
    if (slots.size() != 5) return 111;

    const char* expectedHosts[] = {
        "36.1.1.10",
        "59.1.1.10",
        "36.1.1.10",
        "59.1.1.10",
        "36.1.1.10",
    };
    const uint16_t expectedPorts[] = {443, 8443, 443, 8443, 443};
    const char* expectedTexts[] = {
        "36.1.1.10:443",
        "59.1.1.10:8443",
        "36.1.1.10:443",
        "59.1.1.10:8443",
        "36.1.1.10:443",
    };
    for (size_t i = 0; i < slots.size(); ++i) {
        if (slots[i].Name != "default") return 112;
        if (!slots[i].LocalAddress.empty()) return 113;
        if (slots[i].PeerHost != expectedHosts[i]) return 114;
        if (slots[i].PeerPort != expectedPorts[i]) return 115;
        if (slots[i].PeerText != expectedTexts[i]) return 116;
    }
    return 0;
}

static int TestQuicPathModeRejectsSlotTopologyMutation() {
    TqConfig cfg;
    cfg.QuicPaths.push_back(TqQuicPathConfig{"cmcc", "10.10.1.2", "36.1.1.10:443", 2});
    cfg.QuicPaths.push_back(TqQuicPathConfig{"ctcc", "10.20.1.2", "59.1.1.10:443", 1});

    QuicClientSession session;
    session.MarkReconnectStartedForTest(3, cfg);

    std::string err;
    if (session.SetDesiredConnectionCount(4, err)) return 120;
    if (err.find("path-mode uses fixed connection slots") == std::string::npos) return 121;

    err.clear();
    if (session.StopHighestConnection("conn-2", err)) return 122;
    if (err.find("path-mode uses fixed connection slots") == std::string::npos) return 123;

    session.Stop();
    return 0;
}

static int TestStartSlotUsesConfiguredPathSlots() {
    TqConfig cfg;
    cfg.QuicPaths.push_back(TqQuicPathConfig{"cmcc", "10.10.1.2", "36.1.1.10:443", 2});
    cfg.QuicPaths.push_back(TqQuicPathConfig{"ctcc", "10.20.1.2", "59.1.1.10:443", 1});

    QuicClientSession session;
    session.MarkReconnectStartedForTest(3, cfg);

    std::vector<TqClientSlotPath> observed;
    QuicClientSession::ReconnectTestHooks hooks;
    hooks.StartSlotPathObserver = [&](size_t index, const TqClientSlotPath& path) {
        if (index != observed.size()) {
            observed.push_back(TqClientSlotPath{"bad-index", "", "", 0, ""});
            return;
        }
        observed.push_back(path);
    };
    hooks.StartSlotOverride = [](size_t) {
        return true;
    };
    session.SetReconnectTestHooks(std::move(hooks));

    std::string err;
    if (!session.ReconnectConnection("conn-0", err)) return 130;
    if (!session.ReconnectConnection("conn-1", err)) return 131;
    if (!session.ReconnectConnection("conn-2", err)) return 132;
    if (observed.size() != 3) return 133;
    if (observed[0].Name != "cmcc" ||
        observed[0].LocalAddress != "10.10.1.2" ||
        observed[0].PeerHost != "36.1.1.10" ||
        observed[0].PeerPort != 443) {
        return 134;
    }
    if (observed[1].Name != "cmcc" ||
        observed[1].LocalAddress != "10.10.1.2" ||
        observed[1].PeerHost != "36.1.1.10" ||
        observed[1].PeerPort != 443) {
        return 135;
    }
    if (observed[2].Name != "ctcc" ||
        observed[2].LocalAddress != "10.20.1.2" ||
        observed[2].PeerHost != "59.1.1.10" ||
        observed[2].PeerPort != 443) {
        return 136;
    }
    session.Stop();
    return 0;
}

static int TestStartSlotUsesPeerListRoundRobinPaths() {
    TqConfig cfg;
    cfg.QuicPeer = "36.1.1.10:443,59.1.1.10:8443";
    cfg.QuicConnections = 2;

    QuicClientSession session;
    session.MarkReconnectStartedForTest(2, cfg);

    std::vector<TqClientSlotPath> observed;
    QuicClientSession::ReconnectTestHooks hooks;
    hooks.StartSlotPathObserver = [&](size_t, const TqClientSlotPath& path) {
        observed.push_back(path);
    };
    hooks.StartSlotOverride = [](size_t) {
        return true;
    };
    session.SetReconnectTestHooks(std::move(hooks));

    std::string err;
    if (!session.ReconnectConnection("conn-0", err)) return 140;
    if (!session.ReconnectConnection("conn-1", err)) return 141;
    if (observed.size() != 2) return 142;
    if (observed[0].PeerHost != "36.1.1.10" ||
        observed[0].PeerPort != 443 ||
        !observed[0].LocalAddress.empty()) {
        return 143;
    }
    if (observed[1].PeerHost != "59.1.1.10" ||
        observed[1].PeerPort != 8443 ||
        !observed[1].LocalAddress.empty()) {
        return 144;
    }
    session.Stop();
    return 0;
}

static int TestScheme2CredentialConfig() {
    TqConfig client;
    client.QuicCa = "ca.crt";
    TqCredentialConfigSnapshot clientCred =
        TqBuildCredentialConfigSnapshotForTest(client, false);
    if (clientCred.Type != QUIC_CREDENTIAL_TYPE_NONE) {
        return 60;
    }
    if (!(clientCred.Flags & QUIC_CREDENTIAL_FLAG_CLIENT)) {
        return 61;
    }
    if (!(clientCred.Flags & QUIC_CREDENTIAL_FLAG_SET_CA_CERTIFICATE_FILE)) {
        return 62;
    }
    if (clientCred.Flags & QUIC_CREDENTIAL_FLAG_NO_CERTIFICATE_VALIDATION) {
        return 63;
    }
    if (clientCred.HasCertificateFile) {
        return 64;
    }
    if (clientCred.CaCertificateFile != "ca.crt") {
        return 65;
    }

    TqConfig server;
    server.QuicCert = "server.crt";
    server.QuicKey = "server.key";
    TqCredentialConfigSnapshot serverCred =
        TqBuildCredentialConfigSnapshotForTest(server, true);
    if (serverCred.Type != QUIC_CREDENTIAL_TYPE_CERTIFICATE_FILE) {
        return 66;
    }
    if (serverCred.Flags & QUIC_CREDENTIAL_FLAG_CLIENT) {
        return 67;
    }
    if (serverCred.Flags & QUIC_CREDENTIAL_FLAG_REQUIRE_CLIENT_AUTHENTICATION) {
        return 68;
    }
    if (serverCred.Flags & QUIC_CREDENTIAL_FLAG_SET_CA_CERTIFICATE_FILE) {
        return 69;
    }
    if (!serverCred.HasCertificateFile) {
        return 70;
    }
    if (!serverCred.CaCertificateFile.empty()) {
        return 71;
    }
    return 0;
}

int main() {
    if (int rc = TestFixedDelayRetrySchedulesAndRestartsSlot()) return rc;
    if (int rc = TestDelayedRetryDropsAfterStop()) return rc;
    if (int rc = TestFixedDelayRetryCoalescesDuplicates()) return rc;
    if (int rc = TestRejectedSchedulerAllowsLaterRetry()) return rc;
    if (int rc = TestConnectionStartPendingIsAccepted()) return rc;
    if (int rc = TestShutdownCompleteSchedulesSlotRestart()) return rc;
    if (int rc = TestConnectionSnapshotAndSlotControls()) return rc;
    if (int rc = TestQuicPathSlotExpansion()) return rc;
    if (int rc = TestQuicPeerListSlotExpansionRoundRobin()) return rc;
    if (int rc = TestQuicPathModeRejectsSlotTopologyMutation()) return rc;
    if (int rc = TestStartSlotUsesConfiguredPathSlots()) return rc;
    if (int rc = TestStartSlotUsesPeerListRoundRobinPaths()) return rc;
    if (int rc = TestScheme2CredentialConfig()) return rc;
    return 0;
}
