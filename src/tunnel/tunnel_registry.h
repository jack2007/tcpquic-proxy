#pragma once

#include <cstdint>

struct MsQuicConnection;

using TqTunnelAbortFn = void (*)(void* context);

void TqRegisterConnectionTunnel(
    MsQuicConnection* connection,
    void* tunnelContext,
    TqTunnelAbortFn abortFn);

void TqUnregisterConnectionTunnel(MsQuicConnection* connection, void* tunnelContext);

uint32_t TqCountConnectionTunnels(MsQuicConnection* connection);
uint32_t TqAbortConnectionTunnels(MsQuicConnection* connection);
