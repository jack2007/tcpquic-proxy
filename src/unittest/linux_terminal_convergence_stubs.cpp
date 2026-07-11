#include "quic_session.h"
#include "trace.h"
#include "tunnel_registry.h"

uint32_t TqLookupServerConnectionId(MsQuicConnection*) { return 0; }
std::string TqLookupServerConnectionPeerId(MsQuicConnection*) { return {}; }
bool TqSetServerConnectionClientName(MsQuicConnection*, const std::string&) { return false; }
uint32_t TqLookupClientTraceConnId(MsQuicConnection*) { return 0; }
bool TqLookupClientTerminalConnection(
    MsQuicConnection*, TqTerminalConnectionKey&,
    std::shared_ptr<TqTerminalEscalation>&) noexcept { return false; }
bool TqLookupServerTerminalConnection(
    MsQuicConnection*, TqTerminalConnectionKey&,
    std::shared_ptr<TqTerminalEscalation>&) noexcept { return false; }
uint64_t TqTraceStreamStarted(MsQuicConnection*, uint32_t, const char*, const char*, uint8_t) { return 0; }
void TqTraceIncOpenTx(uint32_t) {}
void TqTraceIncOpenRx(uint32_t) {}
void TqTraceOpenResult(uint64_t, bool, TqOpenError, uint32_t) {}
void TqTraceRelayStarted(uint64_t, const char*, uint32_t, uint64_t) {}
void TqTraceStreamClosed(uint64_t, const char*, const char*, bool, TqOpenError) {}
void TqTraceTargetTcpDialing(uint64_t, const char*) {}
void TqTraceTargetTcpConnected(uint64_t, TqSocketHandle) {}
void TqTraceTargetTcpFailed(uint64_t, const char*, TqOpenError) {}
void TqTraceTargetTcpClosed(uint64_t) {}
void TqRegisterConnectionTunnel(MsQuicConnection*, void*, TqTunnelAbortFn, TqTunnelDrainFn) {}
void TqUpdateConnectionTunnelMetadata(MsQuicConnection*, void*, const TqTunnelRegistryMetadata&) {}
void TqUnregisterConnectionTunnel(MsQuicConnection*, void*) {}
