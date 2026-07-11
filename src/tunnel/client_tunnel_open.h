#pragma once

#include "config.h"
#include "tcp_tunnel.h"

#include <functional>
#include <string>

struct TqClientTunnelOpenHandle;
struct MsQuicConnection;
struct TqClientPickedConnection;

using TqClientTunnelOpenComplete =
    std::function<void(TqClientTunnelOpenHandle*, TqTunnelStartResult)>;

struct TqClientTunnelMetadata {
    std::string PeerId;
    std::string ConnectionId;
};

// Starts a client tunnel OPEN without blocking for the peer OPEN response.
//
// Ownership contract:
// - If this function returns nullptr, the caller retains ownership of
//   clientTcpFd and must close or reuse it.
// - A non-null returned handle remains owned by the caller until exactly one
//   terminal API is called: TqAcceptClientTunnelOpen, TqRejectClientTunnelOpen,
//   or TqCancelClientTunnelOpen.
// - Once a non-null handle is returned, clientTcpFd ownership moves to the
//   async open handle/context until a terminal API completes cleanup or relay
//   ownership takes over after acceptance.
// - onComplete is a notification only. A successful result does not start relay
//   until the caller explicitly accepts the handle.
// - Failed completion results still require TqRejectClientTunnelOpen or
//   TqCancelClientTunnelOpen to release caller ownership of the handle; the
//   clientTcpFd remains open until that terminal call so callers can send a
//   protocol failure response first.
// - The handle pointer passed to onComplete is valid for the duration of that
//   callback and remains valid afterward until the caller invokes a terminal API.
TqClientTunnelOpenHandle* TqStartClientTunnelAsync(
    MsQuicConnection* conn,
    const TunnelRequest& req,
    TqSocketHandle clientTcpFd,
    const TqConfig& cfg,
    TqClientTunnelOpenComplete onComplete);

TqClientTunnelOpenHandle* TqStartClientTunnelAsync(
    MsQuicConnection* conn,
    const TunnelRequest& req,
    TqSocketHandle clientTcpFd,
    const TqConfig& cfg,
    TqClientTunnelOpenComplete onComplete,
    TqClientTunnelMetadata metadata);

TqClientTunnelOpenHandle* TqStartClientTunnelAsync(
    const TqClientPickedConnection& picked,
    const TunnelRequest& req,
    TqSocketHandle clientTcpFd,
    const TqConfig& cfg,
    TqClientTunnelOpenComplete onComplete,
    TqClientTunnelMetadata metadata);

void TqCancelClientTunnelOpen(TqClientTunnelOpenHandle* handle);
// Returns true only if the successful OPEN was accepted and relay was either
// started immediately or safely queued until the OPEN send completion arrives.
bool TqAcceptClientTunnelOpen(TqClientTunnelOpenHandle* handle);
void TqRejectClientTunnelOpen(TqClientTunnelOpenHandle* handle);
