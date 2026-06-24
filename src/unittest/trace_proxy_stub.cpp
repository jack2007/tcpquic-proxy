#include "trace.h"
#include "msquic.hpp"

void TqTraceProxyAccepted(TqTraceProxyProto, TqSocketHandle) {
}

void TqTraceProxyRejected(TqTraceProxyProto, TqSocketHandle, int, const char*) {
}

void TqTraceProxyTunnelOk(TqTraceProxyProto, const char*, uint64_t) {
}

void TqTraceProxyTunnelFail(TqTraceProxyProto, const char*, TqOpenError) {
}

void TqTraceProxyClosed(TqTraceProxyProto, TqSocketHandle) {
}

bool TqTraceInit(TqMode, uint32_t) { return true; }

void TqTraceShutdown() {
}

bool TqTraceEnabled() { return false; }

bool TqDiagStatsInit(uint32_t) { return true; }

void TqDiagStatsShutdown() {
}

bool TqDiagStatsEnabled() { return false; }

std::string TqTraceGlobalSnapshot() { return {}; }

void TqTraceQuicConnecting(const char*, uint32_t, const char*) {
}

void TqTraceQuicIncoming() {
}

void TqTraceLogLine(const char*) {
}

void TqTraceQuicConnected(MsQuicConnection*, uint32_t, const char*, uint32_t) {
}

void TqTraceQuicShutdownTransport(MsQuicConnection*, uint32_t, const char*, uint32_t, uint64_t) {
}

void TqTraceQuicShutdownPeer(MsQuicConnection*, uint32_t, const char*, uint64_t) {
}

void TqTraceQuicDisconnected(MsQuicConnection*, uint32_t, const char*) {
}

void TqTraceQuicNetworkStats(MsQuicConnection*, const TqTraceNetworkStats&) {
}

std::string TqFormatTraceNetworkStatsLine(const TqTraceNetworkStats&) { return {}; }

void TqTraceRelayStopping(uint64_t, const char*, const char*, const char*, uint64_t, const char*) {
}

void TqTraceRelayStreamEvent(
    const char*,
    uint32_t,
    uint64_t,
    const char*,
    uint64_t,
    uint32_t,
    uint64_t,
    uint64_t,
    uint32_t,
    uint32_t,
    bool,
    const TqTraceLinuxRelayStreamState&) {
}

void TqTraceRelayStopCondition(
    const char*, uint32_t, const char*, const TqTraceLinuxRelayStreamState&) {
}

void TqTraceRelayBackpressureEvent(
    const char*,
    uint32_t,
    uint64_t,
    const char*,
    const char*,
    uint64_t,
    uint64_t,
    uint64_t,
    uint64_t) {
}

void TqTraceRelayStreamShutdown(const char*, const TqTraceLinuxRelayStreamState&) {
}

void TqTraceRelayUnregister(const char*, const TqTraceLinuxRelayStreamState&) {
}

void TqTraceRelayFatalError(
    const char*,
    const char*,
    uint64_t,
    uint64_t,
    uint64_t,
    uint64_t,
    uint64_t,
    uint64_t,
    uint64_t) {
}

const MsQuicApi* MsQuic = nullptr;

uint32_t TqCountConnectionTunnels(MsQuicConnection*) { return 0; }
uint32_t TqAbortConnectionTunnels(MsQuicConnection*) { return 0; }
