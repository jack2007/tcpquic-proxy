#include "../platform_socket.h"
#include "../relay.h"
#include "../tcp_dialer.h"
#include "../tcp_tunnel.h"

struct MsQuicConnection;

uint32_t TqLookupServerConnectionId(MsQuicConnection* connection) {
    (void)connection;
    return 0;
}
#include <cassert>
#include <cstring>
#include <vector>

int main() {
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
