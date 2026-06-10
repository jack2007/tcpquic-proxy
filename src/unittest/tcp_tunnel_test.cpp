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
#include <cstring>
#include <vector>

static unsigned g_abort_a = 0;
static unsigned g_abort_b = 0;
static unsigned g_reentrant_abort = 0;
static uint32_t g_reentrant_aborted = 0;
static MsQuicConnection* g_reentrant_conn = nullptr;

static void CountAbortA(void*) { ++g_abort_a; }
static void CountAbortB(void*) { ++g_abort_b; }
static void CountReentrantAbort(void*) {
    ++g_reentrant_abort;
    g_reentrant_aborted = TqAbortConnectionTunnels(g_reentrant_conn);
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

int main() {
    if (int rc = TestTunnelRegistryAbortsOnlyMatchingConnection()) return rc;
    if (int rc = TestTunnelRegistryRemovesBeforeCallbacks()) return rc;

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
