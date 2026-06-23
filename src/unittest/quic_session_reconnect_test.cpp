#include "quic_session.h"

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
            if (delay != std::chrono::milliseconds(100)) {
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
            if (delay != std::chrono::milliseconds(100)) {
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
    if (int rc = TestScheme2CredentialConfig()) return rc;
    return 0;
}
