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

struct TunnelRequest {
    uint8_t AddrType;
    char Host[256];
    uint16_t Port;
    uint8_t CompressFlags;
};

struct TqTunnelStartResult {
    bool Ok{false};
    TqOpenError Error{TqOpenError::Internal};
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

struct TqTunnelContext;

bool TqTunnelRelayStopped(const TqTunnelContext* ctx);
void TqReapTunnelContext(TqTunnelContext* ctx);
