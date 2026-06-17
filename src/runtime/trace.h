#pragma once

#include "config.h"
#include "control_protocol.h"
#include "platform_socket.h"

#include <cstdint>
#include <string>

struct MsQuicConnection;

struct TqTraceNetworkStats {
    uint32_t BytesInFlight{0};
    uint64_t PostedBytes{0};
    uint64_t IdealBytes{0};
    uint64_t SmoothedRttUs{0};
    uint32_t CongestionWindow{0};
    uint64_t BandwidthBytesPerSecond{0};
};

struct TqTraceLinuxRelayStreamState {
    uint32_t WorkerIndex{0};
    uint64_t RelayId{0};
    uint64_t OutstandingQuicSends{0};
    uint64_t OutstandingQuicSendBytes{0};
    uint64_t PendingTcpWriteQueue{0};
    uint64_t PendingTcpWriteBytes{0};
    uint64_t PendingQuicReceiveBytes{0};
    bool TcpReadClosed{false};
    bool TcpWriteClosed{false};
    bool QuicSendFinSubmitted{false};
    bool QuicSendFinCompleted{false};
    bool TcpWriteShutdownQueued{false};
    bool StreamDetached{false};
};

bool TqTraceInit(TqMode mode, uint32_t statsIntervalSec);
void TqTraceShutdown();
bool TqTraceEnabled();

std::string TqTraceGlobalSnapshot();

void TqTraceQuicConnecting(const char* role, uint32_t slot, const char* peer);
void TqTraceQuicIncoming();
void TqTraceQuicConnected(
    MsQuicConnection* connection,
    uint32_t connId,
    const char* role,
    uint32_t slot);
void TqTraceQuicShutdownTransport(
    MsQuicConnection* connection,
    uint32_t connId,
    const char* role,
    uint32_t status,
    uint64_t errorCode);
void TqTraceQuicShutdownPeer(
    MsQuicConnection* connection,
    uint32_t connId,
    const char* role,
    uint64_t errorCode);
void TqTraceQuicDisconnected(
    MsQuicConnection* connection,
    uint32_t connId,
    const char* role);
void TqTraceQuicNetworkStats(MsQuicConnection* connection, const TqTraceNetworkStats& stats);
std::string TqFormatTraceNetworkStatsLine(const TqTraceNetworkStats& stats);
void TqTraceLinuxRelayStreamShutdown(const TqTraceLinuxRelayStreamState& state);
std::string TqFormatTraceLinuxRelayStreamShutdownLine(
    const TqTraceLinuxRelayStreamState& state);

uint64_t TqTraceStreamStarted(
    MsQuicConnection* connection,
    uint32_t connId,
    const char* role,
    const char* target,
    uint8_t compressFlags);
void TqTraceOpenResult(uint64_t tunnelId, bool ok, TqOpenError error, uint32_t connIdField);
void TqTraceRelayStarted(uint64_t tunnelId);
void TqTraceStreamClosed(
    uint64_t tunnelId,
    const char* role,
    const char* target,
    bool relayStarted,
    TqOpenError closeReason);

enum class TqTraceProxyProto { Socks, Http };

void TqTraceProxyAccepted(TqTraceProxyProto proto, TqSocketHandle fd);
void TqTraceProxyRejected(TqTraceProxyProto proto, TqSocketHandle fd, int status, const char* reason);
void TqTraceProxyTunnelOk(TqTraceProxyProto proto, const char* target, uint64_t tunnelId);
void TqTraceProxyTunnelFail(TqTraceProxyProto proto, const char* target, TqOpenError error);
void TqTraceProxyClosed(TqTraceProxyProto proto, TqSocketHandle fd);

void TqTraceTargetTcpDialing(uint64_t tunnelId, const char* target);
void TqTraceTargetTcpConnected(uint64_t tunnelId, TqSocketHandle fd);
void TqTraceTargetTcpFailed(uint64_t tunnelId, TqOpenError error);
void TqTraceTargetTcpClosed(uint64_t tunnelId);

bool TqFormatSocketPeerAddr(TqSocketHandle fd, std::string& out);

void TqTraceIncOpenTx(uint32_t connId);
void TqTraceIncOpenRx(uint32_t connId);
