#include "tunnel_registry.h"

#include <atomic>
#include <string>
#include <thread>

namespace {

std::atomic<unsigned> g_abortCalls{0};
std::atomic<unsigned> g_drainCalls{0};

void Abort(void*) {
    g_abortCalls.fetch_add(1);
}

void Drain(void*) {
    g_drainCalls.fetch_add(1);
}

} // namespace

int main() {
    auto* connection = reinterpret_cast<MsQuicConnection*>(0x1);
    int context = 0;

    TqRegisterConnectionTunnel(connection, &context, &Abort, &Drain);
    TqTunnelRegistryMetadata metadata;
    metadata.PeerId = "agent-a";
    metadata.ConnectionId = "conn-0";
    metadata.Target = "example.com:443";
    metadata.Role = "client";
    metadata.Ingress = "http";
    metadata.Compress = "zstd";
    metadata.RelayBackend = "linux";
    metadata.WorkerIndex = 2;
    TqUpdateConnectionTunnelMetadata(connection, &context, metadata);

    auto tunnels = TqSnapshotTunnels();
    if (tunnels.size() != 1) return 1;
    if (tunnels[0].TunnelId != "tun-1") return 2;
    if (tunnels[0].PeerId != "agent-a") return 3;
    if (tunnels[0].ConnectionId != "conn-0") return 4;
    if (tunnels[0].Target != "example.com:443") return 5;
    if (tunnels[0].State != "active") return 6;
    if (tunnels[0].Compress != "zstd") return 7;
    if (tunnels[0].RelayBackend != "linux") return 8;
    if (tunnels[0].WorkerIndex != 2) return 9;

    TqTunnelSnapshot one;
    if (!TqGetTunnelSnapshot("tun-1", one)) return 10;
    if (one.Target != "example.com:443") return 11;

    if (!TqDrainTunnelById("tun-1")) return 12;
    if (g_drainCalls.load() != 1) return 24;
    if (!TqGetTunnelSnapshot("tun-1", one)) return 13;
    if (one.State != "draining") return 14;
    if (TqAbortTunnelById("tun-18446744073709551617")) return 25;
    if (g_abortCalls.load() != 0) return 26;

    if (!TqAbortTunnelById("tun-1")) return 15;
    if (g_abortCalls.load() != 1) return 16;
    if (!TqGetTunnelSnapshot("tun-1", one)) return 17;
    if (one.State != "aborting") return 18;
    if (TqAbortTunnelById("tun-1")) return 19;

    TqUnregisterConnectionTunnel(connection, &context);
    if (!TqSnapshotTunnels().empty()) return 20;

    int secondContext = 0;
    TqRegisterConnectionTunnel(connection, &secondContext, &Abort, &Drain);
    auto second = TqSnapshotTunnels();
    if (second.size() != 1) return 21;
    if (!TqAbortTunnelById(second[0].TunnelId)) return 22;
    std::thread unregisterThread([&] {
        TqUnregisterConnectionTunnel(connection, &secondContext);
    });
    unregisterThread.join();
    if (!TqSnapshotTunnels().empty()) return 23;
    return 0;
}
