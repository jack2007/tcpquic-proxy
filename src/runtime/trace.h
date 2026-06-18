#pragma once

#include "config.h"
#include "control_protocol.h"
#include "platform_socket.h"

#include "relay_metrics.h"

#include <cstdint>
#include <string>
#include <vector>

struct MsQuicConnection;
typedef struct QUIC_STATISTICS_V2 QUIC_STATISTICS_V2;

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
    uint64_t TcpReadBytes{0};
    uint64_t TcpWriteBytes{0};
    uint64_t TcpWriteErrno{0};
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
void TqTraceLogLine(const char* line);
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
std::vector<std::string> TqFormatTraceQuicStatsLines(const QUIC_STATISTICS_V2& stats);
std::string TqFormatTraceNetworkStatsLine(const TqTraceNetworkStats& stats);
void TqTraceLinuxRelayStreamShutdown(const TqTraceLinuxRelayStreamState& state);
std::string TqFormatTraceLinuxRelayStreamShutdownLine(
    const TqTraceLinuxRelayStreamState& state);
std::string TqFormatTraceRelayStateLine(
    const char* eventName,
    const char* backend,
    const TqTraceLinuxRelayStreamState& state);
void TqTraceRelayStreamEvent(
    const char* backend,
    uint32_t workerIndex,
    uint64_t relayId,
    const char* streamEvent,
    uint64_t errorCode,
    uint32_t status,
    uint64_t absoluteOffset,
    uint64_t totalBufferLength,
    uint32_t bufferCount,
    uint32_t receiveFlags,
    bool fin,
    const TqTraceLinuxRelayStreamState& state);
void TqTraceRelayStopCondition(
    const char* backend,
    uint32_t workerIndex,
    const char* trigger,
    const TqTraceLinuxRelayStreamState& state);
void TqTraceRelayBackpressureEvent(
    const char* backend,
    uint32_t workerIndex,
    uint64_t relayId,
    const char* action,
    const char* reason,
    uint64_t outstandingQuicSendBytes,
    uint64_t pauseThreshold,
    uint64_t resumeThreshold,
    uint64_t readAheadBytes);
void TqTraceRelayStreamShutdown(
    const char* backend,
    const TqTraceLinuxRelayStreamState& state);
void TqTraceRelayUnregister(
    const char* backend,
    const TqTraceLinuxRelayStreamState& state);
void TqTraceRelayFatalError(
    const char* backend,
    const char* reason,
    uint64_t relayId,
    uint64_t socketOrFd,
    uint64_t pendingQuicReceiveBytes,
    uint64_t pendingQuicReceiveQueue,
    uint64_t pendingQuicSends,
    uint64_t inflightQuicSends,
    uint64_t inflightTcpSends);
std::string TqFormatRelayMetricsSnapshotLine(const TqRelayMetricsSnapshot& metrics);

uint64_t TqTraceStreamStarted(
    MsQuicConnection* connection,
    uint32_t connId,
    const char* role,
    const char* target,
    uint8_t compressFlags);
void TqTraceOpenResult(uint64_t tunnelId, bool ok, TqOpenError error, uint32_t connIdField);
void TqTraceRelayStarted(uint64_t tunnelId);
void TqTraceRelayStopping(
    uint64_t tunnelId,
    const char* role,
    const char* target,
    const char* backend,
    uint64_t relayId,
    const char* reason);
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
