#include "tunnel_registry.h"

#include <algorithm>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <vector>

namespace {

struct TqRegisteredTunnel {
    uint64_t Id{0};
    void* Context{nullptr};
    TqTunnelAbortFn Abort{nullptr};
    bool Aborting{false};
    std::thread::id AbortThread{};
};

std::mutex g_tunnelRegistryLock;
std::condition_variable g_tunnelRegistryWakeup;
std::unordered_map<MsQuicConnection*, std::vector<TqRegisteredTunnel>> g_tunnelRegistry;
uint64_t g_nextTunnelRegistryId{1};

} // namespace

void TqRegisterConnectionTunnel(
    MsQuicConnection* connection,
    void* tunnelContext,
    TqTunnelAbortFn abortFn) {
    if (connection == nullptr || tunnelContext == nullptr || abortFn == nullptr) {
        return;
    }

    std::unique_lock<std::mutex> guard(g_tunnelRegistryLock);
    const std::thread::id currentThread = std::this_thread::get_id();
    for (;;) {
        auto& tunnels = g_tunnelRegistry[connection];
        bool hasAbortingMatch = false;
        bool hasSameThreadAbortingMatch = false;
        for (auto& tunnel : tunnels) {
            if (tunnel.Context != tunnelContext) {
                continue;
            }
            if (tunnel.Aborting) {
                if (tunnel.AbortThread == currentThread) {
                    hasSameThreadAbortingMatch = true;
                    continue;
                } else {
                    hasAbortingMatch = true;
                    break;
                }
            }

            tunnel.Abort = abortFn;
            return;
        }

        if (!hasAbortingMatch || hasSameThreadAbortingMatch) {
            tunnels.push_back({g_nextTunnelRegistryId++, tunnelContext, abortFn, false, {}});
            return;
        }

        g_tunnelRegistryWakeup.wait(guard);
    }
}

void TqUnregisterConnectionTunnel(MsQuicConnection* connection, void* tunnelContext) {
    if (connection == nullptr || tunnelContext == nullptr) {
        return;
    }

    std::unique_lock<std::mutex> guard(g_tunnelRegistryLock);
    const std::thread::id currentThread = std::this_thread::get_id();
    for (;;) {
        auto found = g_tunnelRegistry.find(connection);
        if (found == g_tunnelRegistry.end()) {
            return;
        }

        auto& tunnels = found->second;
        bool hasAbortingMatch = false;
        tunnels.erase(
            std::remove_if(
                tunnels.begin(),
                tunnels.end(),
                [tunnelContext, currentThread, &hasAbortingMatch](const TqRegisteredTunnel& tunnel) {
                    if (tunnel.Context != tunnelContext) {
                        return false;
                    }
                    if (tunnel.Aborting) {
                        if (tunnel.AbortThread != currentThread) {
                            hasAbortingMatch = true;
                        }
                        return false;
                    }
                    return true;
                }),
            tunnels.end());

        if (tunnels.empty()) {
            g_tunnelRegistry.erase(found);
        }

        if (!hasAbortingMatch) {
            return;
        }

        g_tunnelRegistryWakeup.wait(guard);
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

        for (auto& tunnel : found->second) {
            if (!tunnel.Aborting) {
                tunnel.Aborting = true;
                tunnel.AbortThread = std::this_thread::get_id();
                tunnels.push_back(tunnel);
            }
        }
    }

    for (const auto& tunnel : tunnels) {
        tunnel.Abort(tunnel.Context);
    }

    {
        std::lock_guard<std::mutex> guard(g_tunnelRegistryLock);
        auto found = g_tunnelRegistry.find(connection);
        if (found != g_tunnelRegistry.end()) {
            auto& registered = found->second;
            registered.erase(
                std::remove_if(
                    registered.begin(),
                    registered.end(),
                    [&tunnels](const TqRegisteredTunnel& tunnel) {
                        if (!tunnel.Aborting) {
                            return false;
                        }
                        return std::any_of(
                            tunnels.begin(),
                            tunnels.end(),
                            [&tunnel](const TqRegisteredTunnel& aborted) {
                                return aborted.Id == tunnel.Id;
                            });
                    }),
                registered.end());

            if (registered.empty()) {
                g_tunnelRegistry.erase(found);
            }
        }
    }
    g_tunnelRegistryWakeup.notify_all();

    return static_cast<uint32_t>(tunnels.size());
}
