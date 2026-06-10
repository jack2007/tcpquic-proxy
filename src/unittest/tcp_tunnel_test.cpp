#include "platform_socket.h"
#include "relay.h"
#include "tcp_dialer.h"
#include "tcp_tunnel.h"
#include "tunnel_registry.h"
#include "trace.h"

struct MsQuicConnection;

uint32_t TqLookupServerConnectionId(MsQuicConnection* connection) {
    (void)connection;
    return 0;
}

uint32_t TqLookupClientTraceConnId(MsQuicConnection* connection) {
    (void)connection;
    return 0;
}

bool TqTraceEnabled() {
    return false;
}

uint64_t TqTraceStreamStarted(
    MsQuicConnection* connection,
    uint32_t connId,
    const char* role,
    const char* target,
    uint8_t compressFlags) {
    (void)connection;
    (void)connId;
    (void)role;
    (void)target;
    (void)compressFlags;
    return 0;
}

void TqTraceIncOpenTx(uint32_t connId) {
    (void)connId;
}

void TqTraceIncOpenRx(uint32_t connId) {
    (void)connId;
}

void TqTraceRelayStarted(uint64_t tunnelId) {
    (void)tunnelId;
}

void TqTraceOpenResult(uint64_t tunnelId, bool ok, TqOpenError error, uint32_t connIdField) {
    (void)tunnelId;
    (void)ok;
    (void)error;
    (void)connIdField;
}

void TqTraceStreamClosed(
    uint64_t tunnelId,
    const char* role,
    const char* target,
    bool relayStarted,
    TqOpenError closeReason) {
    (void)tunnelId;
    (void)role;
    (void)target;
    (void)relayStarted;
    (void)closeReason;
}

void TqTraceProxyClosed(TqTraceProxyProto proto, TqSocketHandle fd) {
    (void)proto;
    (void)fd;
}

void TqTraceTargetTcpDialing(uint64_t tunnelId, const char* target) {
    (void)tunnelId;
    (void)target;
}

void TqTraceTargetTcpConnected(uint64_t tunnelId, TqSocketHandle fd) {
    (void)tunnelId;
    (void)fd;
}

void TqTraceTargetTcpFailed(uint64_t tunnelId, TqOpenError error) {
    (void)tunnelId;
    (void)error;
}

void TqTraceTargetTcpClosed(uint64_t tunnelId) {
    (void)tunnelId;
}
#include <cassert>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <mutex>
#include <thread>
#include <vector>

static unsigned g_abort_a = 0;
static unsigned g_abort_b = 0;
static unsigned g_duplicate_abort_a = 0;
static unsigned g_duplicate_abort_b = 0;
static unsigned g_reentrant_abort = 0;
static uint32_t g_reentrant_aborted = 0;
static MsQuicConnection* g_reentrant_conn = nullptr;
static unsigned g_self_unregister_abort = 0;
static unsigned g_self_register_abort = 0;
static MsQuicConnection* g_self_unregister_conn = nullptr;
static MsQuicConnection* g_self_register_conn = nullptr;
static void* g_self_unregister_ctx = nullptr;
static void* g_self_register_ctx = nullptr;

static void CountAbortA(void*) { ++g_abort_a; }
static void CountAbortB(void*) { ++g_abort_b; }
static void CountDuplicateAbortA(void*) { ++g_duplicate_abort_a; }
static void CountDuplicateAbortB(void*) { ++g_duplicate_abort_b; }
static void CountReentrantAbort(void*) {
    ++g_reentrant_abort;
    g_reentrant_aborted = TqAbortConnectionTunnels(g_reentrant_conn);
}
static void SelfUnregisterAbort(void*) {
    ++g_self_unregister_abort;
    TqUnregisterConnectionTunnel(g_self_unregister_conn, g_self_unregister_ctx);
}
static void SelfRegisterAbort(void*) {
    ++g_self_register_abort;
    if (g_self_register_abort == 1) {
        TqRegisterConnectionTunnel(g_self_register_conn, g_self_register_ctx, SelfRegisterAbort);
    }
}

static int TestTunnelRegistryAbortsOnlyMatchingConnection() {
    auto* conn1 = reinterpret_cast<MsQuicConnection*>(static_cast<uintptr_t>(0x1001));
    auto* conn2 = reinterpret_cast<MsQuicConnection*>(static_cast<uintptr_t>(0x1002));
    int ctx1 = 1;
    int ctx2 = 2;
    g_abort_a = 0;
    g_abort_b = 0;

    TqRegisterConnectionTunnel(conn1, &ctx1, CountAbortA);
    TqRegisterConnectionTunnel(conn2, &ctx2, CountAbortB);
    const uint32_t aborted = TqAbortConnectionTunnels(conn1);
    TqUnregisterConnectionTunnel(conn2, &ctx2);

    if (aborted != 1) return 201;
    if (g_abort_a != 1) return 202;
    if (g_abort_b != 0) return 203;
    return 0;
}

static int TestTunnelRegistryRemovesBeforeCallbacks() {
    auto* conn = reinterpret_cast<MsQuicConnection*>(static_cast<uintptr_t>(0x2001));
    int ctx = 1;
    g_reentrant_abort = 0;
    g_reentrant_aborted = 99;
    g_reentrant_conn = conn;

    TqRegisterConnectionTunnel(conn, &ctx, CountReentrantAbort);
    const uint32_t aborted = TqAbortConnectionTunnels(conn);
    const uint32_t abortedAgain = TqAbortConnectionTunnels(conn);

    if (aborted != 1) return 204;
    if (g_reentrant_abort != 1) return 205;
    if (g_reentrant_aborted != 0) return 206;
    if (abortedAgain != 0) return 207;
    return 0;
}

static int TestTunnelRegistryDuplicateRegistrationIsSingleEntry() {
    auto* conn = reinterpret_cast<MsQuicConnection*>(static_cast<uintptr_t>(0x3001));
    int ctx = 1;
    g_duplicate_abort_a = 0;
    g_duplicate_abort_b = 0;

    TqRegisterConnectionTunnel(conn, &ctx, CountDuplicateAbortA);
    TqRegisterConnectionTunnel(conn, &ctx, CountDuplicateAbortB);
    const uint32_t aborted = TqAbortConnectionTunnels(conn);

    if (aborted != 1) return 208;
    if (g_duplicate_abort_a + g_duplicate_abort_b != 1) return 209;
    return 0;
}

struct TqAbortWaitProbe {
    std::mutex Lock;
    std::condition_variable Wakeup;
    bool AbortEntered{false};
    bool UnregisterStarted{false};
    bool ReleaseAbort{false};
    bool UnregisterReturnedBeforeRelease{false};
    std::atomic<bool> UnregisterReturned{false};
};

static void BlockingAbort(void* context) {
    auto* probe = static_cast<TqAbortWaitProbe*>(context);
    {
        std::lock_guard<std::mutex> guard(probe->Lock);
        probe->AbortEntered = true;
    }
    probe->Wakeup.notify_all();

    std::unique_lock<std::mutex> guard(probe->Lock);
    probe->Wakeup.wait(guard, [probe] { return probe->UnregisterStarted; });
    guard.unlock();

    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    probe->UnregisterReturnedBeforeRelease =
        probe->UnregisterReturned.load(std::memory_order_acquire);

    guard.lock();
    probe->Wakeup.wait(guard, [probe] { return probe->ReleaseAbort; });
}

static int TestTunnelRegistryUnregisterWaitsForInFlightAbort() {
    auto* conn = reinterpret_cast<MsQuicConnection*>(static_cast<uintptr_t>(0x4001));
    TqAbortWaitProbe probe;

    TqRegisterConnectionTunnel(conn, &probe, BlockingAbort);

    std::thread abortThread([conn] {
        (void)TqAbortConnectionTunnels(conn);
    });

    {
        std::unique_lock<std::mutex> guard(probe.Lock);
        if (!probe.Wakeup.wait_for(
                guard,
                std::chrono::seconds(2),
                [&probe] { return probe.AbortEntered; })) {
            probe.UnregisterStarted = true;
            probe.ReleaseAbort = true;
            probe.Wakeup.notify_all();
            abortThread.join();
            return 210;
        }
    }

    std::thread unregisterThread([conn, &probe] {
        {
            std::lock_guard<std::mutex> guard(probe.Lock);
            probe.UnregisterStarted = true;
        }
        probe.Wakeup.notify_all();
        TqUnregisterConnectionTunnel(conn, &probe);
        probe.UnregisterReturned.store(true, std::memory_order_release);
        probe.Wakeup.notify_all();
    });

    std::this_thread::sleep_for(std::chrono::milliseconds(10));
    {
        std::lock_guard<std::mutex> guard(probe.Lock);
        probe.ReleaseAbort = true;
    }
    probe.Wakeup.notify_all();

    abortThread.join();
    unregisterThread.join();

    if (probe.UnregisterReturnedBeforeRelease) return 211;
    if (!probe.UnregisterReturned.load(std::memory_order_acquire)) return 212;
    return 0;
}

static int TestTunnelRegistryAbortCallbackCanUnregisterItself() {
    auto* conn = reinterpret_cast<MsQuicConnection*>(static_cast<uintptr_t>(0x5001));
    int ctx = 1;
    g_self_unregister_abort = 0;
    g_self_unregister_conn = conn;
    g_self_unregister_ctx = &ctx;

    TqRegisterConnectionTunnel(conn, &ctx, SelfUnregisterAbort);
    const uint32_t aborted = TqAbortConnectionTunnels(conn);
    const uint32_t abortedAgain = TqAbortConnectionTunnels(conn);

    if (aborted != 1) return 213;
    if (g_self_unregister_abort != 1) return 214;
    if (abortedAgain != 0) return 215;
    return 0;
}

static int TestTunnelRegistryAbortCallbackCanRegisterSameContext() {
    auto* conn = reinterpret_cast<MsQuicConnection*>(static_cast<uintptr_t>(0x6001));
    int ctx = 1;
    g_self_register_abort = 0;
    g_self_register_conn = conn;
    g_self_register_ctx = &ctx;

    TqRegisterConnectionTunnel(conn, &ctx, SelfRegisterAbort);
    const uint32_t aborted = TqAbortConnectionTunnels(conn);
    const uint32_t abortedAgain = TqAbortConnectionTunnels(conn);
    const uint32_t abortedThird = TqAbortConnectionTunnels(conn);

    if (aborted != 1) return 216;
    if (abortedAgain != 1) return 217;
    if (abortedThird != 0) return 218;
    if (g_self_register_abort != 2) return 219;
    return 0;
}

int main() {
    if (int rc = TestTunnelRegistryAbortsOnlyMatchingConnection()) return rc;
    if (int rc = TestTunnelRegistryRemovesBeforeCallbacks()) return rc;
    if (int rc = TestTunnelRegistryDuplicateRegistrationIsSingleEntry()) return rc;
    if (int rc = TestTunnelRegistryUnregisterWaitsForInFlightAbort()) return rc;
    if (int rc = TestTunnelRegistryAbortCallbackCanUnregisterItself()) return rc;
    if (int rc = TestTunnelRegistryAbortCallbackCanRegisterSameContext()) return rc;

    TunnelRequest req{};
    req.AddrType = TQ_ADDR_DOMAIN;
    constexpr char host[] = "example.test";
    std::memcpy(req.Host, host, sizeof(host));
    req.Port = 443;
    req.CompressFlags = TQ_FLAG_COMPRESS;

    TqConfig cfg{};
    cfg.Compress = "off";

    TqSocketStartup startup;
    if (!startup.Ok()) return 1;

    TqSocketHandle fds[2]{TqInvalidSocket, TqInvalidSocket};
    if (!TqSocketPair(fds)) return 1;
    const TqTunnelStartResult badConn = TqStartClientTunnel(nullptr, req, fds[0], cfg);
    if (badConn.Ok) return 2;
    if (badConn.Error != TqOpenError::Internal) return 3;
    TqCloseSocket(fds[0]);
    TqCloseSocket(fds[1]);

    std::vector<sockaddr_storage> empty;
    if (TqSocketValid(TqDialTcp(empty, 1).Fd)) return 5;

    TqAcl acl;
    bool completed = false;
    bool aclDenied = false;
    TqHandleServerPeerStream(nullptr, nullptr, acl, cfg, [&completed]() { completed = true; }, [&aclDenied]() { aclDenied = true; });
    if (completed) return 6;
    if (aclDenied) return 7;

    {
        TqRelayHandle handle{};
        assert(!TqRelayLinuxFastPathEnabled(&handle));
#if defined(__linux__)
        handle.Backend = TqRelayBackendType::LinuxWorker;
        assert(TqRelayLinuxFastPathEnabled(&handle));
        if (handle.Backend != TqRelayBackendType::LinuxWorker) return 10;
#elif defined(_WIN32)
        handle.Backend = TqRelayBackendType::WindowsWorker;
        assert(!TqRelayLinuxFastPathEnabled(&handle));
        if (handle.Backend != TqRelayBackendType::WindowsWorker) return 10;
#endif
        handle.Backend = TqRelayBackendType::None;
    }

    return 0;
}
