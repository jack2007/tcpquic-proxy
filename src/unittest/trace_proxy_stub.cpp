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

std::string TqTraceGlobalSnapshot() { return {}; }

void TqTraceQuicConnecting(const char*, uint32_t, const char*) {
}

void TqTraceQuicIncoming() {
}

void TqTraceQuicConnected(MsQuicConnection*, uint32_t, const char*, uint32_t) {
}

void TqTraceQuicShutdownTransport(MsQuicConnection*, uint32_t, const char*, uint32_t, uint64_t) {
}

void TqTraceQuicShutdownPeer(MsQuicConnection*, uint32_t, const char*, uint64_t) {
}

void TqTraceQuicDisconnected(MsQuicConnection*, uint32_t, const char*) {
}

const MsQuicApi* MsQuic = nullptr;

uint32_t TqAbortConnectionTunnels(MsQuicConnection*) { return 0; }
