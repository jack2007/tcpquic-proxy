#pragma once

#include "config.h"
#include "control_protocol.h"
#include "platform_socket.h"

#include "relay_metrics.h"

#include <cstddef>
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
    uint32_t BytesInFlightMax{0};
    uint32_t BbrState{0};
    uint32_t BbrRecoveryState{0};
    uint32_t BbrRecoveryWindow{0};
    uint32_t BbrPacingGain{0};
    uint32_t BbrCwndGain{0};
    uint64_t BbrMinRttUs{0};
    uint64_t BbrSendQuantum{0};
    bool BbrAppLimited{false};
    uint64_t SendFlushCount{0};
    uint64_t SendFlushPacingDelayedCount{0};
    uint64_t SendFlushCcBlockedCount{0};
    uint64_t SendFlushSchedulingCount{0};
    uint64_t SendFlushAmplificationBlockedCount{0};
    uint64_t SendFlushNoWorkCount{0};
    uint32_t SendFlushLastAllowance{0};
    uint32_t SendFlushLastPathAllowance{0};
    uint32_t SendFlushLastResult{0};
    uint32_t SendFlushLastDatagrams{0};
    uint32_t OutFlowBlockedReasons{0};
    uint64_t LossDetectionEventCount{0};
    uint64_t LossDetectionFackPacketCount{0};
    uint64_t LossDetectionRackPacketCount{0};
    uint64_t LostRetransmittableBytes{0};
    uint32_t LastLostRetransmittableBytes{0};
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
    uint64_t TcpRecvErrno{0};
    uint64_t TcpSendErrno{0};
    uint64_t IocpCompletionErrno{0};
    uint32_t IocpOperation{0};
    bool TcpReadClosed{false};
    bool TcpWriteClosed{false};
    bool QuicSendFinSubmitted{false};
    bool QuicSendFinCompleted{false};
    bool TcpWriteShutdownQueued{false};
    bool StreamDetached{false};
    uint64_t TunnelId{0};
    const char* Target{nullptr};
};

bool TqTraceInit(
    TqMode mode,
    uint32_t statsIntervalSec,
    TqConfig::TraceLevel level = TqConfig::TraceLevel::Info);
void TqTraceShutdown();
bool TqTraceEnabled();
bool TqDiagStatsInit(uint32_t statsIntervalSec);
void TqDiagStatsShutdown();
bool TqDiagStatsEnabled();
bool TqApplyDiagnosticsRuntime(const TqConfig& cfg);

std::string TqTraceGlobalSnapshot();

void TqTraceQuicConnecting(const char* role, uint32_t slot, const char* peer);
void TqTraceQuicIncoming();
void TqTraceLogLine(const char* line);
void TqTraceQuicConnected(
    MsQuicConnection* connection,
    uint32_t connId,
    const char* role,
    uint32_t slot,
    const char* encryption = "");
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
void TqTraceRelayReceiveViewEvent(
    const char* backend,
    uint32_t workerIndex,
    const char* stage,
    uintptr_t viewId,
    uint64_t value,
    uint64_t totalLength,
    uint64_t completedLength,
    uint64_t accountedLength,
    uint64_t pendingCompleteBytes,
    size_t sliceIndex,
    size_t sliceCount,
    size_t sliceOffset,
    bool fin,
    bool drained,
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
void TqTraceWindowsReceiveFlowDiag(
    uint32_t workerIndex,
    uint64_t relayId,
    const char* phase,
    const char* detail,
    uint64_t pendingBytes,
    uint64_t pendingQueue,
    bool quicReceivePaused,
    uint64_t receiveBytes,
    uint64_t limitBytes,
    uint64_t extra);
void TqTraceRelayStreamShutdown(
    const char* backend,
    const TqTraceLinuxRelayStreamState& state);
void TqTraceRelayUnregister(
    const char* backend,
    const TqTraceLinuxRelayStreamState& state);
void TqTraceRelayFatalError(
    const char* backend,
    uint32_t workerIndex,
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
void TqTraceRelayStarted(
    uint64_t tunnelId,
    const char* backend,
    uint32_t workerIndex,
    uint64_t relayId);
void TqTraceRelayStopping(
    uint64_t tunnelId,
    const char* role,
    const char* target,
    const char* backend,
    uint32_t workerIndex,
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
void TqTraceTargetTcpFailed(uint64_t tunnelId, const char* target, TqOpenError error);
void TqTraceTargetTcpClosed(uint64_t tunnelId);

bool TqFormatSocketPeerAddr(TqSocketHandle fd, std::string& out);

void TqTraceIncOpenTx(uint32_t connId);
void TqTraceIncOpenRx(uint32_t connId);

#if defined(TQ_UNIT_TESTING)
void TqResetRelayTraceCallCountsForTest();
uint64_t TqRelayTraceStopConditionCountForTest();
uint64_t TqRelayTraceUnregisterCountForTest();
#endif
