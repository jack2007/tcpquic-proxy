#include "../tcp_dialer.h"
#include "../tcp_tunnel.h"

#include <cassert>

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
    assert(socketpair(AF_UNIX, SOCK_STREAM, 0, fds) == 0);
    const TqTunnelStartResult badConn = TqStartClientTunnel(nullptr, req, fds[0], cfg);
    assert(!badConn.Ok);
    assert(badConn.Error == TqOpenError::Internal);
    assert(::close(fds[1]) == 0);

    std::vector<sockaddr_storage> empty;
    assert(TqDialTcp(empty, 1).Fd < 0);

    TqAcl acl;
    bool completed = false;
    bool aclDenied = false;
    TqHandleServerPeerStream(nullptr, nullptr, acl, cfg, [&completed]() { completed = true; }, [&aclDenied]() { aclDenied = true; });
    assert(!completed);
    assert(!aclDenied);

    return 0;
}
