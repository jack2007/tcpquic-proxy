#pragma once

#include <cstdint>
#include <functional>

#include "acl.h"
#include "config.h"
#include "control_protocol.h"
#include "msquic.hpp"
#include "platform_socket.h"

struct MsQuicConnection;
struct MsQuicStream;
class TqEphemeralTargetAuthorizer;
class TqServerSpeedTestController;

struct TunnelRequest {
    uint8_t AddrType;
    char Host[256];
    uint16_t Port;
    uint8_t CompressFlags;
    uint8_t IngressTraceProto{0}; // 1=socks5, 2=http (trace)
};

struct TqTunnelStartResult {
    bool Ok{false};
    TqOpenError Error{TqOpenError::Internal};
    uint64_t TraceTunnelId{0};
};

using TunnelStartFn =
    std::function<TqTunnelStartResult(const TunnelRequest&, TqSocketHandle clientTcpFd)>;
using TqTunnelCompletionFn = std::function<void()>;
using TqTunnelAclDeniedFn = std::function<void()>;

TqTunnelStartResult TqStartClientTunnel(
    MsQuicConnection* conn,
    const TunnelRequest& req,
    TqSocketHandle clientTcpFd,
    const TqConfig& cfg);

void TqHandleServerPeerStream(
    MsQuicConnection* conn,
    HQUIC rawStream,
    const TqAcl& acl,
    const TqConfig& cfg,
    TqTunnelCompletionFn onComplete = {},
    TqTunnelAclDeniedFn onAclDenied = {});

void TqHandleServerIncomingStream(
    MsQuicConnection* conn,
    HQUIC rawStream,
    const TqAcl& acl,
    const TqConfig& cfg,
    TqServerSpeedTestController* speed,
    TqTunnelCompletionFn onComplete = {},
    TqTunnelAclDeniedFn onAclDenied = {});

#if defined(TCPQUIC_TUNNEL_TESTING)
void TqHandleServerIncomingStreamForTest(
    MsQuicConnection* conn,
    HQUIC rawStream,
    const TqAcl& acl,
    const TqConfig& cfg,
    const TqEphemeralTargetAuthorizer* authorizer,
    TqTunnelCompletionFn onComplete = {},
    TqTunnelAclDeniedFn onAclDenied = {});
#endif

struct TqTunnelContext;

bool TqTunnelRelayStopped(const TqTunnelContext* ctx);
void TqReapTunnelContext(TqTunnelContext* ctx);
