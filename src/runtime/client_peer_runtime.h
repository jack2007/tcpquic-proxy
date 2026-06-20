#pragma once

#include "client_ingress_reactor.h"
#include "client_tunnel_open.h"
#include "config.h"
#include "quic_session.h"
#include "router_runtime.h"
#include "server_metrics.h"
#include "tcp_tunnel.h"

#include <chrono>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

inline TqPeerConfig TqMakePrimaryPeerConfig(const TqConfig& cfg) {
    TqPeerConfig peer;
    peer.PeerId = "primary";
    peer.Enabled = true;
    peer.QuicPeer = cfg.QuicPeer;
    peer.SocksListen = cfg.SocksListen;
    peer.HttpListen = cfg.HttpListen;
    peer.QuicConnections = cfg.QuicConnections;
    peer.Compress = cfg.Compress;
    return peer;
}

inline TqConfig TqMakePeerRuntimeConfig(const TqConfig& baseConfig, const TqPeerConfig& peer) {
    TqConfig cfg = baseConfig;
    cfg.ClientConfigPath.clear();
    cfg.AdminListen.clear();
    cfg.QuicPeer = peer.QuicPeer;
    cfg.SocksListen = peer.SocksListen;
    cfg.HttpListen = peer.HttpListen;
    cfg.QuicConnections = peer.QuicConnections == 0 ? baseConfig.QuicConnections : peer.QuicConnections;
    cfg.Compress = peer.Compress.empty() ? baseConfig.Compress : peer.Compress;
    return cfg;
}

enum class TqClientPeerLogMode {
    Peer,
    Primary,
};

class TqClientPeerRuntime : public std::enable_shared_from_this<TqClientPeerRuntime> {
public:
    TqClientPeerRuntime(
        std::string peerId,
        TqConfig config,
        TqClientIngressReactor* ingress,
        TqClientPeerLogMode logMode = TqClientPeerLogMode::Peer);
    ~TqClientPeerRuntime();

    bool Start(std::string& err);
    void StopAccepting();
    void StopAll();
    void AbortTunnels();
    TqPeerMetrics SnapshotPeerMetrics() const;
    TqClientMetrics SnapshotClientMetrics() const;
    MsQuicConnection* PickConnection();
    bool EnsureAnyConnected(std::chrono::milliseconds timeout);
    bool EnableAcceptingAndApplyCurrentConnectionState(std::string& err, bool requireConnected);

private:
    bool OpenListenersLocked(std::string& err);
    bool ApplyConnectionState(uint32_t connectedCount, std::string& err, bool requireConnected);
    bool ApplyCurrentConnectionState(std::string& err, bool requireConnected);
    bool ApplyConnectionStateLocked(uint32_t connectedCount, std::string& err, bool requireConnected);
    bool ApplyCurrentConnectionStateLocked(std::string& err, bool requireConnected);
    void CloseListenersLocked();
    void DisableAccepting();
    TqClientTunnelOpenHandle* StartTunnel(
        const TunnelRequest& req,
        TqSocketHandle fd,
        TqClientTunnelOpenComplete onComplete);

    std::string PeerId;
    TqConfig Config;
    TqClientPeerLogMode LogMode{TqClientPeerLogMode::Peer};
    mutable std::mutex TunnelStartMutex;
    mutable std::mutex ListenerMutex;
    std::unique_ptr<QuicClientSession> Quic;
    TqClientIngressReactor* Ingress{nullptr};
    bool AcceptingEnabled{false};
    bool ListenersOpen{false};
};

class TqClientRuntimeManager {
public:
    explicit TqClientRuntimeManager(
        TqConfig baseConfig,
        TqClientPeerLogMode logMode = TqClientPeerLogMode::Peer);
    ~TqClientRuntimeManager();

    bool StartPeer(const TqPeerConfig& peer, std::string& err);
    void StopAccepting(const std::string& peerId);
    void StopAll();
    void AbortPeerTunnels(const std::string& peerId);
    void DrainPeer(const std::string& peerId, uint32_t graceSeconds);
    bool SnapshotPeerMetrics(const std::string& peerId, TqPeerMetrics& out) const;
    bool SnapshotClientMetrics(const std::string& peerId, TqClientMetrics& out) const;
    MsQuicConnection* PickConnection(const std::string& peerId);
    bool EnsureAnyConnected(const std::string& peerId, std::chrono::milliseconds timeout);
    bool EnableAcceptingAndApplyCurrentConnectionState(
        const std::string& peerId,
        std::string& err,
        bool requireConnected);

private:
    struct DrainSignal;
    struct DrainHandle {
        std::shared_ptr<TqClientPeerRuntime> Runtime;
        std::shared_ptr<DrainSignal> Signal;
        std::thread Worker;
    };

    bool EnsureIngressStarted(std::string& err);
    std::shared_ptr<TqClientPeerRuntime> Find(const std::string& peerId) const;

    TqConfig BaseConfig;
    TqClientPeerLogMode LogMode{TqClientPeerLogMode::Peer};
    mutable std::mutex LifecycleMutex;
    mutable std::mutex Lock;
    mutable std::mutex IngressLock;
    std::unique_ptr<TqClientIngressReactor> Ingress;
    std::unordered_map<std::string, std::shared_ptr<TqClientPeerRuntime>> Peers;
    std::vector<DrainHandle> Draining;
};
