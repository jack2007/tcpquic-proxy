#pragma once

#include <cstdint>
#include <atomic>
#include <functional>
#include <memory>

#include "acl.h"
#include "config.h"
#include "control_protocol.h"
#include "msquic.hpp"
#include "platform_socket.h"

struct MsQuicConnection;
struct MsQuicStream;
struct TqClientTunnelOpenHandle;
class TqEphemeralTargetAuthorizer;
class TqServerDialReactor;
class TqServerSpeedTestController;
class TqStreamLifetime;
struct TqRelayStopControl;
struct TqRelayHandle;
struct TqTunnelContext;
class TqLinuxRelayWorker;

enum class TqTunnelRole {
    ClientOpen,
    ServerOpen,
};

struct TunnelRequest {
    uint8_t AddrType;
    char Host[256];
    uint16_t Port;
    uint8_t CompressFlags;
    uint8_t IngressTraceProto{0}; // 1=socks5, 2=http, 3=port-forward (trace)
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

void TqSetServerDialReactor(TqServerDialReactor* reactor);

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
#if defined(__linux__)
TqTunnelContext* TqCreateTestLinuxRelayTunnel(
    std::atomic<unsigned>* destroyCount,
    TqTunnelRole role,
    TqSocketHandle tcpFd,
    std::shared_ptr<TqStreamLifetime> streamOwner,
    std::shared_ptr<TqRelayStopControl>* outControl,
    const TqConfig* configOverride = nullptr,
    TqLinuxRelayWorker* workerOverride = nullptr);
bool TqTestDispatchLinuxOwnerEvent(
    TqTunnelContext* context, QUIC_STREAM_EVENT_TYPE type);
TqRelayHandle* TqTestLinuxTunnelRelayHandle(TqTunnelContext* context);
#endif
#endif

bool TqTunnelRelayStopped(const TqTunnelContext* ctx);
bool TqTunnelTerminalReleaseReady(const TqTunnelContext* ctx);
void TqReapTunnelContext(TqTunnelContext* ctx);
