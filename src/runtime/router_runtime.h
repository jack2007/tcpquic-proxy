#pragma once

#include "admin_http.h"
#include "config.h"
#include "quic_session.h"

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
    std::vector<TqPortForwardConfig> PortForwards;
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
    virtual void AbortPeerTunnels(const std::string& peerId) { (void)peerId; }
    virtual void DrainPeer(const std::string& peerId, uint32_t graceSeconds) = 0;
    virtual bool SnapshotPeerMetrics(const std::string& peerId, TqPeerMetrics& out) {
        (void)peerId;
        (void)out;
        return false;
    }
    virtual std::vector<TqConnectionSnapshot> SnapshotConnections(const std::string& peerId) {
        (void)peerId;
        return {};
    }
    virtual bool SetDesiredConnectionCount(const std::string& peerId, uint32_t desired, std::string& err) {
        (void)peerId;
        (void)desired;
        err = "connection control is not available";
        return false;
    }
    virtual bool StopHighestConnection(const std::string& peerId, const std::string& connectionId, std::string& err) {
        (void)peerId;
        (void)connectionId;
        err = "connection control is not available";
        return false;
    }
    virtual bool ReconnectConnection(const std::string& peerId, const std::string& connectionId, std::string& err) {
        (void)peerId;
        (void)connectionId;
        err = "connection control is not available";
        return false;
    }
    virtual bool AbortConnectionTunnels(const std::string& peerId, const std::string& connectionId, std::string& err) {
        (void)peerId;
        (void)connectionId;
        err = "connection control is not available";
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
    std::vector<TqPeerMetrics> ListPeers() const;
    bool GetPeer(const std::string& peerId, TqPeerMetrics& out) const;
    bool CreatePeer(const TqPeerConfig& peer, std::string& err);
    bool ReplacePeer(const std::string& peerId, const TqPeerConfig& peer, std::string& err);
    bool PatchPeer(const std::string& peerId, const std::string& body, std::string& err);
    bool DeletePeer(const std::string& peerId, const std::string& mode, std::string& err);
    bool EnablePeer(const std::string& peerId, std::string& err);
    bool DisablePeer(const std::string& peerId, std::string& err);
    bool DrainPeer(const std::string& peerId, uint32_t graceSeconds, std::string& err);
    bool AbortPeerTunnels(const std::string& peerId, std::string& err);
    std::vector<TqConnectionSnapshot> ListConnections(const std::string& peerId) const;
    bool GetConnection(const std::string& peerId, const std::string& connectionId, TqConnectionSnapshot& out) const;
    bool AddConnection(const std::string& peerId, std::string& err);
    bool DeleteConnection(const std::string& peerId, const std::string& connectionId, std::string& err);
    bool ReconnectConnection(const std::string& peerId, const std::string& connectionId, std::string& err);
    bool AbortConnectionTunnels(const std::string& peerId, const std::string& connectionId, std::string& err);
    std::string ConfigJson() const;
    std::string MetricsJson() const;
    std::string HealthJson() const;
    std::string HandleAdmin(const TqHttpRequest& req);

private:
    mutable std::mutex Mutex;
    std::mutex ConnectionControlMutex;
    TqRouterConfig Config;
    std::unordered_map<std::string, TqPeerMetrics> Metrics;
    std::unordered_map<std::string, TqPeerConfig> RunningPeers;
    std::chrono::steady_clock::time_point Started{std::chrono::steady_clock::now()};
    TqPeerRuntimeAdapter* Adapter{nullptr};
    bool BridgeValidationMode{false};
    bool BridgeStartupCaptured{false};
    TqPeerConfig BridgeActivePeer;

    bool ApplyConfigLocked(const TqRouterConfig& config, std::string& err);
};

bool TqValidateSinglePeerStartupBridge(const TqRouterConfig& config, std::string& err);
