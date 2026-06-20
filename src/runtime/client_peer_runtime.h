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
#include <unordered_map>

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

class TqClientPeerRuntime : public std::enable_shared_from_this<TqClientPeerRuntime> {
public:
    TqClientPeerRuntime(std::string peerId, TqConfig config, TqClientIngressReactor* ingress);
    ~TqClientPeerRuntime();

    bool Start(std::string& err);
    void StopAccepting();
    void StopAll();
    void AbortTunnels();
    TqPeerMetrics SnapshotPeerMetrics() const;
    TqClientMetrics SnapshotClientMetrics() const;
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
    mutable std::mutex TunnelStartMutex;
    mutable std::mutex ListenerMutex;
    std::unique_ptr<QuicClientSession> Quic;
    TqClientIngressReactor* Ingress{nullptr};
    bool AcceptingEnabled{false};
    bool ListenersOpen{false};
};
