#include "../relay.h"
#include "../tcp_dialer.h"
#include "../tcp_tunnel.h"

struct MsQuicConnection;

uint32_t TqLookupServerConnectionId(MsQuicConnection* connection) {
    (void)connection;
    return 0;
}
#include <cstring>
#include <sys/socket.h>
#include <unistd.h>
#include <vector>

int main() {
    TunnelRequest req{};
    req.AddrType = TQ_ADDR_DOMAIN;
    std::strncpy(req.Host, "example.test", sizeof(req.Host) - 1);
    req.Port = 443;
    req.CompressFlags = TQ_FLAG_COMPRESS;

    TqConfig cfg{};
    cfg.Compress = "off";

    int fds[2]{-1, -1};
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, fds) != 0) return 1;
    const TqTunnelStartResult badConn = TqStartClientTunnel(nullptr, req, fds[0], cfg);
    if (badConn.Ok) return 2;
    if (badConn.Error != TqOpenError::Internal) return 3;
    if (::close(fds[1]) != 0) return 4;

    std::vector<sockaddr_storage> empty;
    if (TqDialTcp(empty, 1).Fd >= 0) return 5;

    TqAcl acl;
    bool completed = false;
    bool aclDenied = false;
    TqHandleServerPeerStream(nullptr, nullptr, acl, cfg, [&completed]() { completed = true; }, [&aclDenied]() { aclDenied = true; });
    if (completed) return 6;
    if (aclDenied) return 7;

    {
        TqRelayHandle handle{};
        if (TqRelayLinuxFastPathEnabled(&handle)) return 8;
        if (handle.Backend != TqRelayBackendType::None) return 9;
    }

    return 0;
}
