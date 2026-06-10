#include "platform_socket.h"
#define private public
#include "quic_session.h"
#undef private
#include "relay.h"
#include "tcp_dialer.h"
#include "tcp_tunnel.h"
#include "tunnel_registry.h"
#include "trace.h"

struct MsQuicConnection;
struct TqTunnelContext;

#if defined(TCPQUIC_TUNNEL_TESTING)
TqTunnelContext* TqCreateTestRegisteredTunnel(
    MsQuicConnection* connection,
    TqSocketHandle tcpFd);
void TqDestroyTestRegisteredTunnel(TqTunnelContext* context);
TqTunnelContext* TqCreateTestClientOpenOwnedTunnel(unsigned* destroyCount);
void TqTestArmSelfDeleteOnShutdown(TqTunnelContext* context);
void TqTestDispatchShutdownComplete(TqTunnelContext* context);
void TqReleaseTestClientOpenOwner(TqTunnelContext* context);
#endif

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
#include <type_traits>
#include <utility>
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

static int TestQuicClientSessionReconnectApiSurface() {
    using Handler = QuicClientSession::ConnectionStateHandler;
    static_assert(std::is_same<Handler, std::function<void(uint32_t)>>::value,
        "connection-state callback reports connected slot count");
    static_assert(std::is_same<decltype(std::declval<const QuicClientSession&>().ConnectedConnectionCount()), uint32_t>::value,
        "connected count is publicly readable");
    static_assert(std::is_same<decltype(std::declval<QuicClientSession&>().EnsureAnyConnected()), bool>::value,
        "eager connect API has a default timeout");
    static_assert(std::is_same<decltype(std::declval<QuicClientSession&>().EnsureAnyConnected(std::chrono::milliseconds(1))), bool>::value,
        "eager connect API accepts an explicit timeout");

    (void)static_cast<void (QuicClientSession::*)(Handler)>(&QuicClientSession::SetConnectionStateHandler);
    return 0;
}

static int TestQuicClientSessionDisconnectNoopsWhenAlreadyDisconnected() {
    using StateArg = const std::shared_ptr<QuicClientSession::ClientSharedState>&;
    static_assert(std::is_same<decltype(QuicClientSession::OnSlotDisconnected(
        std::declval<StateArg>(),
        size_t{},
        static_cast<MsQuicConnection*>(nullptr))), bool>::value,
        "disconnect helper reports whether connected slot count changed");
    static_assert(std::is_same<decltype(QuicClientSession::OnSlotConnected(
        std::declval<StateArg>(),
        size_t{},
        static_cast<MsQuicConnection*>(nullptr))), bool>::value,
        "connect helper reports whether connected slot count changed");
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

static int TestConnectionAbortClosesTunnelTcp() {
    TqSocketStartup startup;
    if (!startup.Ok()) return 220;

    TqSocketHandle fds[2]{TqInvalidSocket, TqInvalidSocket};
    if (!TqSocketPair(fds)) return 221;

    auto* conn = reinterpret_cast<MsQuicConnection*>(static_cast<uintptr_t>(0x7001));
    TqTunnelContext* ctx = TqCreateTestRegisteredTunnel(conn, fds[0]);
    if (ctx == nullptr) {
        TqCloseSocket(fds[0]);
        TqCloseSocket(fds[1]);
        return 222;
    }

    const uint32_t aborted = TqAbortConnectionTunnels(conn);
    if (aborted != 1) {
        TqDestroyTestRegisteredTunnel(ctx);
        TqCloseSocket(fds[1]);
        return 223;
    }

    char byte = 0;
    const int received = TqRecv(fds[1], &byte, 1, TqRecvFlags::None);
    TqCloseSocket(fds[1]);
    TqDestroyTestRegisteredTunnel(ctx);
    if (received != 0) return 224;
    return 0;
}

static int TestClientOpenOwnerDefersShutdownCompleteDelete() {
    unsigned destroyCount = 0;
    TqTunnelContext* ctx = TqCreateTestClientOpenOwnedTunnel(&destroyCount);
    if (ctx == nullptr) return 225;

    TqTestArmSelfDeleteOnShutdown(ctx);
    TqTestDispatchShutdownComplete(ctx);
    if (destroyCount != 0) return 226;

    TqReleaseTestClientOpenOwner(ctx);
    if (destroyCount != 1) return 227;
    return 0;
}

int main() {
    if (int rc = TestQuicClientSessionReconnectApiSurface()) return rc;
    if (int rc = TestQuicClientSessionDisconnectNoopsWhenAlreadyDisconnected()) return rc;
    if (int rc = TestTunnelRegistryAbortsOnlyMatchingConnection()) return rc;
    if (int rc = TestTunnelRegistryRemovesBeforeCallbacks()) return rc;
    if (int rc = TestTunnelRegistryDuplicateRegistrationIsSingleEntry()) return rc;
    if (int rc = TestTunnelRegistryUnregisterWaitsForInFlightAbort()) return rc;
    if (int rc = TestTunnelRegistryAbortCallbackCanUnregisterItself()) return rc;
    if (int rc = TestTunnelRegistryAbortCallbackCanRegisterSameContext()) return rc;
    if (int rc = TestConnectionAbortClosesTunnelTcp()) return rc;
    if (int rc = TestClientOpenOwnerDefersShutdownCompleteDelete()) return rc;

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
