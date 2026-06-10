#pragma once

#include "admin_http.h"
#include "config.h"

#include <chrono>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

struct TqPeerMetrics {
    std::string PeerId;
    bool Enabled{true};
    std::string QuicPeer;
    std::string SocksListen;
    std::string HttpListen;
    std::string State{"starting"};
    uint32_t ConnectionCount{0};
    uint32_t ConnectedConnections{0};
    uint64_t ActiveStreams{0};
    uint64_t TotalStreams{0};
    uint64_t Reconnects{0};
    std::string LastError;
    std::string LastConnectedAt;
};

struct TqRouterMetrics {
    std::string Role{"client"};
    std::string Status{"healthy"};
    uint64_t UptimeSeconds{0};
    std::vector<TqPeerMetrics> Peers;
};

class TqPeerRuntimeAdapter {
public:
    virtual ~TqPeerRuntimeAdapter() = default;
    virtual bool StartPeer(const TqPeerConfig& peer, std::string& err) = 0;
    virtual void StopAccepting(const std::string& peerId) = 0;
    virtual void DrainPeer(const std::string& peerId, uint32_t graceSeconds) = 0;
    virtual bool SnapshotPeerMetrics(const std::string& peerId, TqPeerMetrics& out) {
        (void)peerId;
        (void)out;
        return false;
    }
};

class TqRouterRuntime {
public:
    explicit TqRouterRuntime(TqPeerRuntimeAdapter* adapter = nullptr);
    explicit TqRouterRuntime(bool bridgeValidationMode);

    bool ApplyConfig(const TqRouterConfig& config, std::string& err);
    TqRouterConfig SnapshotConfig() const;
    TqRouterMetrics SnapshotMetrics() const;
    std::string ConfigJson() const;
    std::string MetricsJson() const;
    std::string HealthJson() const;
    std::string HandleAdmin(const TqHttpRequest& req);

private:
    mutable std::mutex Mutex;
    TqRouterConfig Config;
    std::unordered_map<std::string, TqPeerMetrics> Metrics;
    std::unordered_map<std::string, TqPeerConfig> RunningPeers;
    std::chrono::steady_clock::time_point Started{std::chrono::steady_clock::now()};
    TqPeerRuntimeAdapter* Adapter{nullptr};
    bool BridgeValidationMode{false};
    bool BridgeStartupCaptured{false};
    TqPeerConfig BridgeActivePeer;
};

bool TqValidateSinglePeerStartupBridge(const TqRouterConfig& config, std::string& err);
