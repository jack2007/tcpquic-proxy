#include "tunnel_registry.h"

#include <mutex>
#include <unordered_map>
#include <vector>

namespace {

struct TqRegisteredTunnel {
    void* Context{nullptr};
    TqTunnelAbortFn Abort{nullptr};
};

std::mutex g_tunnelRegistryLock;
std::unordered_map<MsQuicConnection*, std::vector<TqRegisteredTunnel>> g_tunnelRegistry;

} // namespace

void TqRegisterConnectionTunnel(
    MsQuicConnection* connection,
    void* tunnelContext,
    TqTunnelAbortFn abortFn) {
    if (connection == nullptr || tunnelContext == nullptr || abortFn == nullptr) {
        return;
    }

    std::lock_guard<std::mutex> guard(g_tunnelRegistryLock);
    g_tunnelRegistry[connection].push_back({tunnelContext, abortFn});
}

void TqUnregisterConnectionTunnel(MsQuicConnection* connection, void* tunnelContext) {
    if (connection == nullptr || tunnelContext == nullptr) {
        return;
    }

    std::lock_guard<std::mutex> guard(g_tunnelRegistryLock);
    auto found = g_tunnelRegistry.find(connection);
    if (found == g_tunnelRegistry.end()) {
        return;
    }

    auto& tunnels = found->second;
    for (auto it = tunnels.begin(); it != tunnels.end(); ++it) {
        if (it->Context == tunnelContext) {
            tunnels.erase(it);
            break;
        }
    }

    if (tunnels.empty()) {
        g_tunnelRegistry.erase(found);
    }
}

uint32_t TqAbortConnectionTunnels(MsQuicConnection* connection) {
    if (connection == nullptr) {
        return 0;
    }

    std::vector<TqRegisteredTunnel> tunnels;
    {
        std::lock_guard<std::mutex> guard(g_tunnelRegistryLock);
        auto found = g_tunnelRegistry.find(connection);
        if (found == g_tunnelRegistry.end()) {
            return 0;
        }

        tunnels = std::move(found->second);
        g_tunnelRegistry.erase(found);
    }

    for (const auto& tunnel : tunnels) {
        tunnel.Abort(tunnel.Context);
    }

    return static_cast<uint32_t>(tunnels.size());
}
