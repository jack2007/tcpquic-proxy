#pragma once

#include <cstdint>
#include <string>
#include <vector>

struct MsQuicConnection;

using TqTunnelAbortFn = void (*)(void* context);
using TqTunnelDrainFn = void (*)(void* context);

struct TqTunnelRegistryMetadata {
    std::string PeerId;
    std::string ConnectionId;
    std::string Target;
    std::string Role;
    std::string Ingress;
    std::string Compress;
    std::string RelayBackend;
    uint32_t WorkerIndex{0};
};

struct TqTunnelSnapshot {
    std::string TunnelId;
    std::string PeerId;
    std::string ConnectionId;
    std::string State;
    std::string Target;
    std::string Role;
    std::string Ingress;
    std::string Compress;
    std::string CreatedAt;
    uint64_t DurationMs{0};
    uint64_t TcpReadBytes{0};
    uint64_t TcpWriteBytes{0};
    uint64_t PendingBytes{0};
    std::string RelayBackend;
    uint32_t WorkerIndex{0};
    std::string LastError;
};

void TqRegisterConnectionTunnel(
    MsQuicConnection* connection,
    void* tunnelContext,
    TqTunnelAbortFn abortFn,
    TqTunnelDrainFn drainFn = nullptr);

void TqUpdateConnectionTunnelMetadata(
    MsQuicConnection* connection,
    void* tunnelContext,
    const TqTunnelRegistryMetadata& metadata);

void TqUnregisterConnectionTunnel(MsQuicConnection* connection, void* tunnelContext);

uint32_t TqCountConnectionTunnels(MsQuicConnection* connection);
uint32_t TqAbortConnectionTunnels(MsQuicConnection* connection);
std::vector<TqTunnelSnapshot> TqSnapshotTunnels();
bool TqGetTunnelSnapshot(const std::string& tunnelId, TqTunnelSnapshot& out);
bool TqAbortTunnelById(const std::string& tunnelId);
bool TqDrainTunnelById(const std::string& tunnelId);
