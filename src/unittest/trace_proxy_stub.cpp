#include "trace.h"

void TqTraceProxyAccepted(TqTraceProxyProto, TqSocketHandle) {
}

void TqTraceProxyTunnelOk(TqTraceProxyProto, const char*, uint64_t) {
}

void TqTraceProxyTunnelFail(TqTraceProxyProto, const char*, TqOpenError) {
}

void TqTraceProxyClosed(TqTraceProxyProto, TqSocketHandle) {
}
