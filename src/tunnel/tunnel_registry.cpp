#include "tunnel_registry.h"

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <ctime>
#include <iomanip>
#include <limits>
#include <mutex>
#include <sstream>
#include <thread>
#include <unordered_map>
#include <vector>

namespace {

struct TqRegisteredTunnel {
    uint64_t Id{0};
    void* Context{nullptr};
    TqTunnelAbortFn Abort{nullptr};
    TqTunnelDrainFn Drain{nullptr};
    TqTunnelRegistryMetadata Metadata;
    std::chrono::steady_clock::time_point Created{std::chrono::steady_clock::now()};
    std::time_t CreatedWallClock{std::time(nullptr)};
    bool Aborting{false};
    bool Draining{false};
    bool AbortInProgress{false};
    std::thread::id AbortThread{};
};

std::mutex g_tunnelRegistryLock;
std::condition_variable g_tunnelRegistryWakeup;
std::unordered_map<MsQuicConnection*, std::vector<TqRegisteredTunnel>> g_tunnelRegistry;
uint64_t g_nextTunnelRegistryId{1};

std::string TunnelIdText(uint64_t id) {
    return "tun-" + std::to_string(id);
}

bool ParseTunnelId(const std::string& text, uint64_t& id) {
    constexpr const char* prefix = "tun-";
    constexpr size_t prefixLen = std::char_traits<char>::length(prefix);
    if (text.compare(0, prefixLen, prefix) != 0 || text.size() == prefixLen) {
        return false;
    }
    uint64_t parsed = 0;
    for (size_t i = prefixLen; i < text.size(); ++i) {
        const char ch = text[i];
        if (ch < '0' || ch > '9') {
            return false;
        }
        const uint64_t digit = static_cast<uint64_t>(ch - '0');
        if (parsed > (std::numeric_limits<uint64_t>::max() - digit) / 10) {
            return false;
        }
        parsed = (parsed * 10) + digit;
    }
    id = parsed;
    return true;
}

std::string IsoTime(std::time_t value) {
    std::tm tm{};
#if defined(_WIN32)
    gmtime_s(&tm, &value);
#else
    gmtime_r(&value, &tm);
#endif
    std::ostringstream out;
    out << std::put_time(&tm, "%Y-%m-%dT%H:%M:%SZ");
    return out.str();
}

TqTunnelSnapshot MakeSnapshotLocked(const TqRegisteredTunnel& tunnel) {
    TqTunnelSnapshot snapshot;
    snapshot.TunnelId = TunnelIdText(tunnel.Id);
    snapshot.PeerId = tunnel.Metadata.PeerId;
    snapshot.ConnectionId = tunnel.Metadata.ConnectionId;
    snapshot.State = tunnel.Aborting ? "aborting" : (tunnel.Draining ? "draining" : "active");
    snapshot.Target = tunnel.Metadata.Target;
    snapshot.Role = tunnel.Metadata.Role;
    snapshot.Ingress = tunnel.Metadata.Ingress;
    snapshot.Compress = tunnel.Metadata.Compress;
    snapshot.CreatedAt = IsoTime(tunnel.CreatedWallClock);
    snapshot.DurationMs = static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - tunnel.Created).count());
    snapshot.RelayBackend = tunnel.Metadata.RelayBackend;
    snapshot.WorkerIndex = tunnel.Metadata.WorkerIndex;
    return snapshot;
}

bool FindTunnelByIdLocked(uint64_t id, TqRegisteredTunnel*& out) {
    for (auto& item : g_tunnelRegistry) {
        for (auto& tunnel : item.second) {
            if (tunnel.Id == id) {
                out = &tunnel;
                return true;
            }
        }
    }
    return false;
}

} // namespace

void TqRegisterConnectionTunnel(
    MsQuicConnection* connection,
    void* tunnelContext,
    TqTunnelAbortFn abortFn,
    TqTunnelDrainFn drainFn) {
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
            tunnel.Drain = drainFn;
            return;
        }

        if (!hasAbortingMatch || hasSameThreadAbortingMatch) {
            TqRegisteredTunnel tunnel;
            tunnel.Id = g_nextTunnelRegistryId++;
            tunnel.Context = tunnelContext;
            tunnel.Abort = abortFn;
            tunnel.Drain = drainFn;
            tunnels.push_back(std::move(tunnel));
            return;
        }

        g_tunnelRegistryWakeup.wait(guard);
    }
}

void TqUpdateConnectionTunnelMetadata(
    MsQuicConnection* connection,
    void* tunnelContext,
    const TqTunnelRegistryMetadata& metadata) {
    if (connection == nullptr || tunnelContext == nullptr) {
        return;
    }
    std::lock_guard<std::mutex> guard(g_tunnelRegistryLock);
    auto found = g_tunnelRegistry.find(connection);
    if (found == g_tunnelRegistry.end()) {
        return;
    }
    for (auto& tunnel : found->second) {
        if (tunnel.Context == tunnelContext) {
            tunnel.Metadata = metadata;
            return;
        }
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
                        if (tunnel.AbortInProgress && tunnel.AbortThread != currentThread) {
                            hasAbortingMatch = true;
                            return false;
                        }
                        return true;
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

uint32_t TqCountConnectionTunnels(MsQuicConnection* connection) {
    if (connection == nullptr) {
        return 0;
    }
    std::lock_guard<std::mutex> guard(g_tunnelRegistryLock);
    auto found = g_tunnelRegistry.find(connection);
    if (found == g_tunnelRegistry.end()) {
        return 0;
    }
    return static_cast<uint32_t>(std::count_if(
        found->second.begin(),
        found->second.end(),
        [](const TqRegisteredTunnel& tunnel) {
            return !tunnel.Aborting;
        }));
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
                tunnel.AbortInProgress = true;
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

std::vector<TqTunnelSnapshot> TqSnapshotTunnels() {
    std::vector<TqTunnelSnapshot> snapshots;
    std::lock_guard<std::mutex> guard(g_tunnelRegistryLock);
    for (const auto& item : g_tunnelRegistry) {
        for (const auto& tunnel : item.second) {
            snapshots.push_back(MakeSnapshotLocked(tunnel));
        }
    }
    std::sort(snapshots.begin(), snapshots.end(), [](const auto& a, const auto& b) {
        return a.TunnelId < b.TunnelId;
    });
    return snapshots;
}

bool TqGetTunnelSnapshot(const std::string& tunnelId, TqTunnelSnapshot& out) {
    uint64_t id = 0;
    if (!ParseTunnelId(tunnelId, id)) {
        return false;
    }
    std::lock_guard<std::mutex> guard(g_tunnelRegistryLock);
    TqRegisteredTunnel* tunnel = nullptr;
    if (!FindTunnelByIdLocked(id, tunnel)) {
        return false;
    }
    out = MakeSnapshotLocked(*tunnel);
    return true;
}

bool TqAbortTunnelById(const std::string& tunnelId) {
    uint64_t id = 0;
    if (!ParseTunnelId(tunnelId, id)) {
        return false;
    }

    TqRegisteredTunnel selected;
    {
        std::lock_guard<std::mutex> guard(g_tunnelRegistryLock);
        TqRegisteredTunnel* tunnel = nullptr;
        if (!FindTunnelByIdLocked(id, tunnel) || tunnel->Aborting) {
            return false;
        }
        tunnel->Aborting = true;
        tunnel->AbortInProgress = true;
        tunnel->AbortThread = std::this_thread::get_id();
        selected = *tunnel;
    }

    selected.Abort(selected.Context);
    {
        std::lock_guard<std::mutex> guard(g_tunnelRegistryLock);
        TqRegisteredTunnel* tunnel = nullptr;
        if (FindTunnelByIdLocked(id, tunnel)) {
            tunnel->AbortInProgress = false;
        }
    }
    g_tunnelRegistryWakeup.notify_all();
    return true;
}

bool TqDrainTunnelById(const std::string& tunnelId) {
    uint64_t id = 0;
    if (!ParseTunnelId(tunnelId, id)) {
        return false;
    }
    TqRegisteredTunnel selected;
    {
        std::lock_guard<std::mutex> guard(g_tunnelRegistryLock);
        TqRegisteredTunnel* tunnel = nullptr;
        if (!FindTunnelByIdLocked(id, tunnel)) {
            return false;
        }
        tunnel->Draining = true;
        selected = *tunnel;
    }
    if (selected.Drain != nullptr) {
        selected.Drain(selected.Context);
    }
    return true;
}
