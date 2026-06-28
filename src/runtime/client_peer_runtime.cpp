#include "client_peer_runtime.h"

#include <condition_variable>
#include <cstdio>
#include <functional>
#include <thread>
#include <utility>

TqClientPeerRuntime::TqClientPeerRuntime(
    std::string peerId,
    TqConfig config,
    TqClientIngressReactor* ingress,
    TqClientPeerLogMode logMode)
    : PeerId(std::move(peerId)), Config(std::move(config)), LogMode(logMode), Ingress(ingress) {}

TqClientPeerRuntime::~TqClientPeerRuntime() {
    StopAll();
}

bool TqClientPeerRuntime::Start(std::string& err) {
    Quic = std::make_unique<QuicClientSession>();
    std::weak_ptr<TqClientPeerRuntime> weakSelf = shared_from_this();
    Quic->SetDelayedTaskScheduler([weakSelf](std::chrono::milliseconds delay, std::function<void()> task) {
        auto self = weakSelf.lock();
        if (!self || self->Ingress == nullptr) {
            return false;
        }
        return self->Ingress->EnqueueDelayed(delay, std::move(task));
    });
    Quic->SetConnectionStateHandler([weakSelf](uint32_t connectedCount) {
        auto self = weakSelf.lock();
        if (!self) {
            return;
        }
        std::string listenerErr;
        if (!self->ApplyConnectionState(connectedCount, listenerErr, false)) {
            if (self->LogMode == TqClientPeerLogMode::Primary) {
                std::fprintf(stderr, "tcpquic-proxy: %s\n", listenerErr.c_str());
            } else {
                std::fprintf(stderr, "tcpquic-proxy: peer %s %s\n",
                    self->PeerId.c_str(), listenerErr.c_str());
            }
        }
    });
    if (!Quic->Start(Config)) {
        err = "failed to start QUIC client for " + PeerId;
        return false;
    }
    if (!Quic->EnsureAnyConnected()) {
        if (LogMode == TqClientPeerLogMode::Primary) {
            std::fprintf(stderr,
                "tcpquic-proxy: no connected QUIC peer at startup; listeners remain closed until reconnect\n");
        } else {
            std::fprintf(stderr,
                "tcpquic-proxy: peer %s has no connected QUIC connection; listeners remain closed until reconnect\n",
                PeerId.c_str());
        }
    }
    if (!EnableAcceptingAndApplyCurrentConnectionState(err, false)) {
        return false;
    }
    return true;
}

void TqClientPeerRuntime::StopAccepting() {
    DisableAccepting();
}

void TqClientPeerRuntime::StopAll() {
    DisableAccepting();
    std::unique_ptr<QuicClientSession> quic;
    {
        std::lock_guard<std::mutex> guard(TunnelStartMutex);
        quic = std::move(Quic);
    }
    if (quic) {
        quic->Stop();
    }
}

void TqClientPeerRuntime::AbortTunnels() {
    std::lock_guard<std::mutex> guard(TunnelStartMutex);
    if (Quic) {
        Quic->AbortAllTunnels();
    }
}

TqPeerMetrics TqClientPeerRuntime::SnapshotPeerMetrics() const {
    std::lock_guard<std::mutex> guard(TunnelStartMutex);
    TqPeerMetrics metrics;
    metrics.PeerId = PeerId;
    metrics.QuicPeer = Config.QuicPeer;
    metrics.SocksListen = Config.SocksListen;
    metrics.HttpListen = Config.HttpListen;
    metrics.PortForwards = Config.PortForwards;
    metrics.ConnectionCount = Quic ? Quic->ConnectionCount() : 0;
    metrics.ConnectedConnections = Quic ? Quic->ConnectedConnectionCount() : 0;
    metrics.State = metrics.ConnectedConnections > 0 ? "healthy" : "connecting";
    return metrics;
}

TqClientMetrics TqClientPeerRuntime::SnapshotClientMetrics() const {
    std::lock_guard<std::mutex> guard(TunnelStartMutex);
    TqClientMetrics metrics;
    metrics.QuicPeer = Config.QuicPeer;
    metrics.SocksListen = Config.SocksListen;
    metrics.HttpListen = Config.HttpListen;
    metrics.PortForwards = Config.PortForwards;
    metrics.ConnectionCount = Quic ? Quic->ConnectionCount() : 0;
    metrics.ConnectedConnections = Quic ? Quic->ConnectedConnectionCount() : 0;
    return metrics;
}

std::vector<TqConnectionSnapshot> TqClientPeerRuntime::SnapshotConnections() const {
    std::lock_guard<std::mutex> guard(TunnelStartMutex);
    return Quic ? Quic->SnapshotConnections() : std::vector<TqConnectionSnapshot>{};
}

bool TqClientPeerRuntime::SetDesiredConnectionCount(uint32_t desired, std::string& err) {
    std::lock_guard<std::mutex> guard(TunnelStartMutex);
    if (!Quic) {
        err = "QUIC session is not running";
        return false;
    }
    if (!Quic->SetDesiredConnectionCount(desired, err)) {
        return false;
    }
    return true;
}

bool TqClientPeerRuntime::StopHighestConnection(const std::string& connectionId, std::string& err) {
    std::lock_guard<std::mutex> guard(TunnelStartMutex);
    if (!Quic) {
        err = "QUIC session is not running";
        return false;
    }
    if (!Quic->StopHighestConnection(connectionId, err)) {
        return false;
    }
    return true;
}

bool TqClientPeerRuntime::ReconnectConnection(const std::string& connectionId, std::string& err) {
    std::lock_guard<std::mutex> guard(TunnelStartMutex);
    if (!Quic) {
        err = "QUIC session is not running";
        return false;
    }
    return Quic->ReconnectConnection(connectionId, err);
}

bool TqClientPeerRuntime::AbortConnectionTunnels(const std::string& connectionId, std::string& err) {
    std::lock_guard<std::mutex> guard(TunnelStartMutex);
    if (!Quic) {
        err = "QUIC session is not running";
        return false;
    }
    return Quic->AbortConnectionTunnels(connectionId, err);
}

MsQuicConnection* TqClientPeerRuntime::PickConnection() {
    std::lock_guard<std::mutex> guard(TunnelStartMutex);
    return Quic ? Quic->PickConnection() : nullptr;
}

bool TqClientPeerRuntime::EnsureAnyConnected(std::chrono::milliseconds timeout) {
    std::lock_guard<std::mutex> guard(TunnelStartMutex);
    return Quic ? Quic->EnsureAnyConnected(timeout) : false;
}

bool TqClientPeerRuntime::EnableAcceptingAndApplyCurrentConnectionState(
    std::string& err,
    bool requireConnected) {
    std::lock_guard<std::mutex> guard(ListenerMutex);
    AcceptingEnabled = true;
    const bool applied = ApplyCurrentConnectionStateLocked(err, requireConnected);
    if (!applied) {
        AcceptingEnabled = false;
        CloseListenersLocked();
    }
    return applied;
}

bool TqClientPeerRuntime::OpenListenersLocked(std::string& err) {
    if (ListenersOpen) {
        return true;
    }
    if (Ingress == nullptr) {
        err = "listener runtime is not initialized";
        return false;
    }

    TqClientIngressPeer peer{};
    peer.PeerId = PeerId;
    peer.SocksListen = Config.SocksListen;
    peer.HttpListen = Config.HttpListen;
    peer.PortForwards = Config.PortForwards;
    peer.Config = Config;
    std::weak_ptr<TqClientPeerRuntime> weakSelf = shared_from_this();
    peer.StartTunnel = [weakSelf](
                           const TunnelRequest& req,
                           TqSocketHandle fd,
                           TqClientTunnelOpenComplete onComplete) {
        auto self = weakSelf.lock();
        if (!self) {
            return static_cast<TqClientTunnelOpenHandle*>(nullptr);
        }
        return self->StartTunnel(req, fd, std::move(onComplete));
    };
    peer.AcceptTunnel = [](TqClientTunnelOpenHandle* handle) {
        return TqAcceptClientTunnelOpen(handle);
    };
    peer.RejectTunnel = [](TqClientTunnelOpenHandle* handle) {
        TqRejectClientTunnelOpen(handle);
    };
    peer.CancelTunnel = [](TqClientTunnelOpenHandle* handle) {
        TqCancelClientTunnelOpen(handle);
    };
    if (!Ingress->AddPeer(peer)) {
        err = "failed to add ingress reactor peer " + PeerId;
        return false;
    }

    ListenersOpen = true;
    if (LogMode == TqClientPeerLogMode::Primary) {
        if (!Config.SocksListen.empty()) {
            std::fprintf(stderr, "tcpquic-proxy: SOCKS5 listening on %s\n", Config.SocksListen.c_str());
        }
        if (!Config.HttpListen.empty()) {
            std::fprintf(stderr, "tcpquic-proxy: HTTP CONNECT listening on %s\n", Config.HttpListen.c_str());
        }
        for (const auto& forward : Config.PortForwards) {
            const std::string target = forward.TargetHost + ":" + std::to_string(forward.TargetPort);
            std::fprintf(stderr, "tcpquic-proxy: port forward listening on %s -> %s\n",
                forward.Listen.c_str(), target.c_str());
        }
    } else {
        if (!Config.SocksListen.empty()) {
            std::fprintf(stderr, "tcpquic-proxy: peer %s SOCKS5 listening on %s\n",
                PeerId.c_str(), Config.SocksListen.c_str());
        }
        if (!Config.HttpListen.empty()) {
            std::fprintf(stderr, "tcpquic-proxy: peer %s HTTP CONNECT listening on %s\n",
                PeerId.c_str(), Config.HttpListen.c_str());
        }
        for (const auto& forward : Config.PortForwards) {
            const std::string target = forward.TargetHost + ":" + std::to_string(forward.TargetPort);
            std::fprintf(stderr, "tcpquic-proxy: peer %s port forward listening on %s -> %s\n",
                PeerId.c_str(), forward.Listen.c_str(), target.c_str());
        }
    }
    return true;
}

bool TqClientPeerRuntime::ApplyCurrentConnectionState(std::string& err, bool requireConnected) {
    std::lock_guard<std::mutex> guard(ListenerMutex);
    return ApplyCurrentConnectionStateLocked(err, requireConnected);
}

bool TqClientPeerRuntime::ApplyConnectionState(
    uint32_t connectedCount,
    std::string& err,
    bool requireConnected) {
    std::lock_guard<std::mutex> guard(ListenerMutex);
    return ApplyConnectionStateLocked(connectedCount, err, requireConnected);
}

bool TqClientPeerRuntime::ApplyCurrentConnectionStateLocked(std::string& err, bool requireConnected) {
    if (!AcceptingEnabled) {
        CloseListenersLocked();
        if (requireConnected) {
            err = "listener accepting is disabled";
            return false;
        }
        return true;
    }
    const uint32_t connectedCount = Quic ? Quic->ConnectedConnectionCount() : 0;
    return ApplyConnectionStateLocked(connectedCount, err, requireConnected);
}

bool TqClientPeerRuntime::ApplyConnectionStateLocked(
    uint32_t connectedCount,
    std::string& err,
    bool requireConnected) {
    if (!AcceptingEnabled || connectedCount == 0) {
        CloseListenersLocked();
        if (requireConnected) {
            err = AcceptingEnabled ? "no connected QUIC connection" : "listener accepting is disabled";
            return false;
        }
        return true;
    }
    return OpenListenersLocked(err);
}

void TqClientPeerRuntime::CloseListenersLocked() {
    if (ListenersOpen && Ingress != nullptr) {
        (void)Ingress->RemovePeer(PeerId);
    }
    ListenersOpen = false;
}

void TqClientPeerRuntime::DisableAccepting() {
    std::lock_guard<std::mutex> guard(ListenerMutex);
    AcceptingEnabled = false;
    CloseListenersLocked();
}

TqClientTunnelOpenHandle* TqClientPeerRuntime::StartTunnel(
    const TunnelRequest& req,
    TqSocketHandle fd,
    TqClientTunnelOpenComplete onComplete) {
    MsQuicConnection* conn = nullptr;
    {
        std::lock_guard<std::mutex> guard(TunnelStartMutex);
        conn = Quic ? Quic->PickConnection() : nullptr;
        if (conn == nullptr) {
            if (LogMode == TqClientPeerLogMode::Primary) {
                std::fprintf(stderr, "tcpquic-proxy: no connected QUIC peer available for tunnel\n");
            } else {
                std::fprintf(stderr,
                    "tcpquic-proxy: peer %s has no connected QUIC connection for tunnel\n",
                    PeerId.c_str());
            }
            return static_cast<TqClientTunnelOpenHandle*>(nullptr);
        }
    }
    return TqStartClientTunnelAsync(conn, req, fd, Config, std::move(onComplete));
}

TqClientRuntimeManager::TqClientRuntimeManager(TqConfig baseConfig, TqClientPeerLogMode logMode)
    : BaseConfig(std::move(baseConfig)), LogMode(logMode) {}

struct TqClientRuntimeManager::DrainSignal {
    std::mutex Lock;
    std::condition_variable Cv;
    bool StopNow{false};
};

TqClientRuntimeManager::~TqClientRuntimeManager() {
    StopAll();
}

bool TqClientRuntimeManager::StartPeer(const TqPeerConfig& peer, std::string& err) {
    std::lock_guard<std::mutex> lifecycleGuard(LifecycleMutex);
    if (!peer.Enabled) {
        err = "peer is disabled: " + peer.PeerId;
        return false;
    }
    if (!EnsureIngressStarted(err)) {
        return false;
    }
    {
        std::lock_guard<std::mutex> guard(Lock);
        if (Peers.find(peer.PeerId) != Peers.end()) {
            err = "peer already running: " + peer.PeerId;
            return false;
        }
    }

    TqConfig peerCfg = TqMakePeerRuntimeConfig(BaseConfig, peer);
    auto runtime = std::make_shared<TqClientPeerRuntime>(peer.PeerId, peerCfg, Ingress.get(), LogMode);
    if (!runtime->Start(err)) {
        runtime->StopAll();
        return false;
    }
    if (LogMode != TqClientPeerLogMode::Primary) {
        const TqPeerMetrics metrics = runtime->SnapshotPeerMetrics();
        if (!peerCfg.QuicPaths.empty()) {
            for (const auto& path : peerCfg.QuicPaths) {
                std::fprintf(stderr,
                    "tcpquic-proxy: peer %s path %s local %s -> %s (%u connections)\n",
                    peer.PeerId.c_str(),
                    path.Name.c_str(),
                    path.LocalAddress.c_str(),
                    path.Peer.c_str(),
                    path.Connections);
            }
        } else {
            std::fprintf(stderr, "tcpquic-proxy: peer %s QUIC peers %s (%u connections)\n",
                peer.PeerId.c_str(), peerCfg.QuicPeer.c_str(), metrics.ConnectionCount);
        }
    }

    std::lock_guard<std::mutex> guard(Lock);
    Peers[peer.PeerId] = std::move(runtime);
    return true;
}

void TqClientRuntimeManager::StopAccepting(const std::string& peerId) {
    std::shared_ptr<TqClientPeerRuntime> runtime = Find(peerId);
    if (runtime) {
        runtime->StopAccepting();
    }
}

void TqClientRuntimeManager::StopAll() {
    std::unordered_map<std::string, std::shared_ptr<TqClientPeerRuntime>> peers;
    std::vector<DrainHandle> draining;
    {
        std::lock_guard<std::mutex> lifecycleGuard(LifecycleMutex);
        {
            std::lock_guard<std::mutex> guard(Lock);
            peers = std::move(Peers);
            Peers.clear();
        }
        draining = std::move(Draining);
        Draining.clear();
        for (const auto& handle : draining) {
            {
                std::lock_guard<std::mutex> signalGuard(handle.Signal->Lock);
                handle.Signal->StopNow = true;
            }
            handle.Signal->Cv.notify_all();
        }
        for (const auto& item : peers) {
            item.second->StopAll();
        }
        for (auto& handle : draining) {
            if (handle.Worker.joinable()) {
                handle.Worker.join();
            }
        }
        {
            std::lock_guard<std::mutex> guard(IngressLock);
            if (Ingress) {
                Ingress->Stop();
                Ingress.reset();
            }
        }
    }
}

void TqClientRuntimeManager::AbortPeerTunnels(const std::string& peerId) {
    std::shared_ptr<TqClientPeerRuntime> runtime = Find(peerId);
    if (runtime) {
        runtime->AbortTunnels();
    }
}

void TqClientRuntimeManager::DrainPeer(const std::string& peerId, uint32_t graceSeconds) {
    std::lock_guard<std::mutex> lifecycleGuard(LifecycleMutex);
    std::shared_ptr<TqClientPeerRuntime> runtime;
    {
        std::lock_guard<std::mutex> guard(Lock);
        auto it = Peers.find(peerId);
        if (it == Peers.end()) {
            return;
        }
        runtime = std::move(it->second);
        Peers.erase(it);
    }

    runtime->StopAccepting();
    auto signal = std::make_shared<DrainSignal>();
    DrainHandle handle;
    handle.Runtime = std::move(runtime);
    handle.Signal = signal;
    std::shared_ptr<TqClientPeerRuntime> workerRuntime = handle.Runtime;
    handle.Worker = std::thread([workerRuntime = std::move(workerRuntime), signal, graceSeconds]() {
        std::unique_lock<std::mutex> guard(signal->Lock);
        signal->Cv.wait_for(guard, std::chrono::seconds(graceSeconds), [&signal]() {
            return signal->StopNow;
        });
        guard.unlock();
        workerRuntime->StopAll();
    });
    Draining.push_back(std::move(handle));
}

bool TqClientRuntimeManager::SnapshotPeerMetrics(const std::string& peerId, TqPeerMetrics& out) const {
    std::shared_ptr<TqClientPeerRuntime> runtime = Find(peerId);
    if (!runtime) {
        return false;
    }
    out = runtime->SnapshotPeerMetrics();
    return true;
}

bool TqClientRuntimeManager::SnapshotClientMetrics(const std::string& peerId, TqClientMetrics& out) const {
    std::shared_ptr<TqClientPeerRuntime> runtime = Find(peerId);
    if (!runtime) {
        return false;
    }
    out = runtime->SnapshotClientMetrics();
    return true;
}

std::vector<TqConnectionSnapshot> TqClientRuntimeManager::SnapshotConnections(const std::string& peerId) const {
    std::shared_ptr<TqClientPeerRuntime> runtime = Find(peerId);
    return runtime ? runtime->SnapshotConnections() : std::vector<TqConnectionSnapshot>{};
}

bool TqClientRuntimeManager::SetDesiredConnectionCount(
    const std::string& peerId,
    uint32_t desired,
    std::string& err) {
    std::shared_ptr<TqClientPeerRuntime> runtime = Find(peerId);
    if (!runtime) {
        err = "not found";
        return false;
    }
    return runtime->SetDesiredConnectionCount(desired, err);
}

bool TqClientRuntimeManager::StopHighestConnection(
    const std::string& peerId,
    const std::string& connectionId,
    std::string& err) {
    std::shared_ptr<TqClientPeerRuntime> runtime = Find(peerId);
    if (!runtime) {
        err = "not found";
        return false;
    }
    return runtime->StopHighestConnection(connectionId, err);
}

bool TqClientRuntimeManager::ReconnectConnection(
    const std::string& peerId,
    const std::string& connectionId,
    std::string& err) {
    std::shared_ptr<TqClientPeerRuntime> runtime = Find(peerId);
    if (!runtime) {
        err = "not found";
        return false;
    }
    return runtime->ReconnectConnection(connectionId, err);
}

bool TqClientRuntimeManager::AbortConnectionTunnels(
    const std::string& peerId,
    const std::string& connectionId,
    std::string& err) {
    std::shared_ptr<TqClientPeerRuntime> runtime = Find(peerId);
    if (!runtime) {
        err = "not found";
        return false;
    }
    return runtime->AbortConnectionTunnels(connectionId, err);
}

MsQuicConnection* TqClientRuntimeManager::PickConnection(const std::string& peerId) {
    std::shared_ptr<TqClientPeerRuntime> runtime = Find(peerId);
    return runtime ? runtime->PickConnection() : nullptr;
}

bool TqClientRuntimeManager::EnsureAnyConnected(
    const std::string& peerId,
    std::chrono::milliseconds timeout) {
    std::shared_ptr<TqClientPeerRuntime> runtime = Find(peerId);
    return runtime ? runtime->EnsureAnyConnected(timeout) : false;
}

bool TqClientRuntimeManager::EnableAcceptingAndApplyCurrentConnectionState(
    const std::string& peerId,
    std::string& err,
    bool requireConnected) {
    std::shared_ptr<TqClientPeerRuntime> runtime = Find(peerId);
    return runtime ? runtime->EnableAcceptingAndApplyCurrentConnectionState(err, requireConnected) : false;
}

bool TqClientRuntimeManager::EnsureIngressStarted(std::string& err) {
    std::lock_guard<std::mutex> guard(IngressLock);
    if (Ingress) {
        return true;
    }
    auto reactor = std::make_unique<TqClientIngressReactor>();
    if (!reactor->Start()) {
        err = "failed to start client ingress reactor";
        return false;
    }
    Ingress = std::move(reactor);
    return true;
}

std::shared_ptr<TqClientPeerRuntime> TqClientRuntimeManager::Find(const std::string& peerId) const {
    std::lock_guard<std::mutex> guard(Lock);
    auto it = Peers.find(peerId);
    return it == Peers.end() ? nullptr : it->second;
}
