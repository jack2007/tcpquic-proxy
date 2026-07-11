#include "trace.h"
#include "msquic.hpp"

bool g_trace_stub_enabled = false;
bool g_diag_stats_stub_enabled = false;
unsigned g_trace_init_count = 0;
unsigned g_trace_shutdown_count = 0;
unsigned g_diag_stats_init_count = 0;
unsigned g_diag_stats_shutdown_count = 0;
uint32_t g_trace_stub_interval_sec = 0;
uint32_t g_diag_stats_stub_interval_sec = 0;
TqConfig::TraceLevel g_trace_stub_level = TqConfig::TraceLevel::Info;

void TqTraceProxyStubReset() {
    g_trace_stub_enabled = false;
    g_diag_stats_stub_enabled = false;
    g_trace_init_count = 0;
    g_trace_shutdown_count = 0;
    g_diag_stats_init_count = 0;
    g_diag_stats_shutdown_count = 0;
    g_trace_stub_interval_sec = 0;
    g_diag_stats_stub_interval_sec = 0;
    g_trace_stub_level = TqConfig::TraceLevel::Info;
}

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

bool TqTraceInit(TqMode, uint32_t intervalSec, TqConfig::TraceLevel level) {
    g_trace_stub_enabled = true;
    g_trace_stub_interval_sec = intervalSec;
    g_trace_stub_level = level;
    ++g_trace_init_count;
    return true;
}

void TqTraceShutdown() {
    g_trace_stub_enabled = false;
    ++g_trace_shutdown_count;
}

bool TqTraceEnabled() { return g_trace_stub_enabled; }

bool TqDiagStatsInit(uint32_t intervalSec) {
    g_diag_stats_stub_enabled = true;
    g_diag_stats_stub_interval_sec = intervalSec;
    ++g_diag_stats_init_count;
    return true;
}

void TqDiagStatsShutdown() {
    g_diag_stats_stub_enabled = false;
    ++g_diag_stats_shutdown_count;
}

bool TqDiagStatsEnabled() { return g_diag_stats_stub_enabled; }

bool TqApplyDiagnosticsRuntime(const TqConfig& cfg) {
    if (cfg.Trace) {
        if (TqTraceEnabled()) {
            TqTraceShutdown();
        }
        if (!TqTraceInit(cfg.Mode, cfg.TraceIntervalSec, cfg.TraceLogLevel)) {
            return false;
        }
    } else if (TqTraceEnabled()) {
        TqTraceShutdown();
    }
    if (cfg.DiagStats) {
        if (!TqDiagStatsInit(cfg.DiagStatsIntervalSec)) {
            return false;
        }
    } else if (TqDiagStatsEnabled()) {
        TqDiagStatsShutdown();
    }
    return true;
}

std::string TqTraceGlobalSnapshot() { return {}; }

void TqTraceQuicConnecting(const char*, uint32_t, const char*) {
}

void TqTraceQuicIncoming() {
}

void TqTraceLogLine(const char*) {
}

void TqTraceQuicConnected(MsQuicConnection*, uint32_t, const char*, uint32_t, const char*) {
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

void TqTraceRelayStopping(uint64_t, const char*, const char*, const char*, uint32_t, uint64_t, const char*) {
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

void TqTraceRelayHalfClose(
    const char*,
    uint32_t,
    const char*,
    const TqTraceLinuxRelayStreamState&,
    const char*,
    bool,
    bool) {
}

void TqTraceRelayReceiveViewEvent(
    const char*,
    uint32_t,
    const char*,
    uintptr_t,
    uint64_t,
    uint64_t,
    uint64_t,
    uint64_t,
    uint64_t,
    size_t,
    size_t,
    size_t,
    bool,
    bool,
    const TqTraceLinuxRelayStreamState&) {
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
    uint32_t,
    const char*,
    uint64_t,
    uint64_t,
    uint64_t,
    uint64_t,
    uint64_t,
    uint64_t,
    uint64_t) {
}

#if defined(TQ_UNIT_TESTING)
void TqResetRelayTraceCallCountsForTest() {
}

uint64_t TqRelayTraceStopConditionCountForTest() {
    return 0;
}

uint64_t TqRelayTraceUnregisterCountForTest() {
    return 0;
}
#endif

const MsQuicApi* MsQuic = nullptr;

uint32_t TqCountConnectionTunnels(MsQuicConnection*) { return 0; }
uint32_t TqAbortConnectionTunnels(MsQuicConnection*) { return 0; }
