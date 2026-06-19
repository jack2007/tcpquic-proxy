#include "config.h"
#include "admin_http.h"
#include "platform_socket.h"
#include "quic_session.h"
#include "acl.h"
#include "client_ingress_reactor.h"
#include "client_tunnel_open.h"
#include "server_dial_reactor.h"
#include "router_runtime.h"
#include "server_metrics.h"
#include "speed_test.h"
#include "tcp_tunnel.h"
#include "tuning.h"
#include "tunnel_reaper.h"
#include "trace.h"

#include <chrono>
#include <cstdio>
#include <functional>
#include <memory>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <utility>

namespace {

struct TqTunnelReaperGuard {
    TqTunnelReaperGuard() { TqTunnelReaper::Instance().Start(); }
    ~TqTunnelReaperGuard() { TqTunnelReaper::Instance().Stop(); }
};

class TqMultiPeerRuntimeAdapter : public TqPeerRuntimeAdapter {
public:
    explicit TqMultiPeerRuntimeAdapter(const TqConfig& baseConfig) : BaseConfig(baseConfig) {}

    bool StartPeer(const TqPeerConfig& peer, std::string& err) override {
        TqConfig peerCfg = BaseConfig;
        peerCfg.ClientConfigPath.clear();
        peerCfg.AdminListen.clear();
        peerCfg.QuicPeer = peer.QuicPeer;
        peerCfg.SocksListen = peer.SocksListen;
        peerCfg.HttpListen = peer.HttpListen;
        peerCfg.QuicConnections = peer.QuicConnections == 0 ? BaseConfig.QuicConnections : peer.QuicConnections;
        peerCfg.Compress = peer.Compress.empty() ? BaseConfig.Compress : peer.Compress;

        if (!EnsureIngressStarted(err)) {
            return false;
        }

        auto runtime = std::make_shared<PeerRuntime>();
        runtime->PeerId = peer.PeerId;
        runtime->Config = peerCfg;
        runtime->Quic = std::make_unique<QuicClientSession>();
        runtime->Ingress = Ingress.get();
        std::weak_ptr<PeerRuntime> weakRuntime = runtime;
        runtime->StartTunnel = [weakRuntime, peerCfg](
                                   const TunnelRequest& req,
                                   TqSocketHandle fd,
                                   TqClientTunnelOpenComplete onComplete) {
            auto runtime = weakRuntime.lock();
            if (!runtime || !runtime->Quic) {
                return static_cast<TqClientTunnelOpenHandle*>(nullptr);
            }
            MsQuicConnection* conn = nullptr;
            {
                std::lock_guard<std::mutex> guard(runtime->TunnelStartMutex);
                if (!runtime->Quic || !runtime->Quic->EnsureAnyConnected()) {
                    std::fprintf(stderr,
                        "tcpquic-proxy: peer %s has no connected QUIC connection for tunnel\n",
                        peerCfg.QuicPeer.c_str());
                    return static_cast<TqClientTunnelOpenHandle*>(nullptr);
                }
                conn = runtime->Quic->PickConnection();
                if (conn == nullptr) {
                    return static_cast<TqClientTunnelOpenHandle*>(nullptr);
                }
            }
            return TqStartClientTunnelAsync(conn, req, fd, peerCfg, std::move(onComplete));
        };

        runtime->Quic->SetDelayedTaskScheduler([weakRuntime](
                                                   std::chrono::milliseconds delay,
                                                   std::function<void()> task) {
            auto runtime = weakRuntime.lock();
            if (!runtime || runtime->Ingress == nullptr) {
                return false;
            }
            return runtime->Ingress->EnqueueDelayed(delay, std::move(task));
        });
        runtime->Quic->SetConnectionStateHandler([weakRuntime](uint32_t connectedCount) {
            auto runtime = weakRuntime.lock();
            if (!runtime) {
                return;
            }
            std::string listenerErr;
            if (!runtime->ApplyConnectionState(connectedCount, listenerErr, false)) {
                std::fprintf(stderr, "tcpquic-proxy: peer %s %s\n",
                    runtime->PeerId.c_str(), listenerErr.c_str());
            }
        });

        if (!runtime->Quic->Start(peerCfg)) {
            err = "failed to start QUIC client for " + peer.PeerId;
            runtime->StopAll();
            return false;
        }

        if (!runtime->Quic->EnsureAnyConnected()) {
            std::fprintf(stderr,
                "tcpquic-proxy: peer %s has no connected QUIC connection; listeners remain closed until reconnect\n",
                peer.PeerId.c_str());
        }
        if (!runtime->EnableAcceptingAndApplyCurrentConnectionState(err, false)) {
            runtime->StopAll();
            return false;
        }
        std::fprintf(stderr, "tcpquic-proxy: peer %s QUIC peer %s (%u connections)\n",
            peer.PeerId.c_str(), peerCfg.QuicPeer.c_str(), runtime->Quic->ConnectionCount());

        std::lock_guard<std::mutex> guard(Lock);
        Peers[peer.PeerId] = std::move(runtime);
        return true;
    }

    void StopAccepting(const std::string& peerId) override {
        std::shared_ptr<PeerRuntime> runtime = Find(peerId);
        if (runtime) {
            runtime->StopAccepting();
        }
    }

    void AbortPeerTunnels(const std::string& peerId) override {
        std::shared_ptr<PeerRuntime> runtime = Find(peerId);
        if (runtime && runtime->Quic) {
            runtime->Quic->AbortAllTunnels();
        }
    }

    bool SnapshotPeerMetrics(const std::string& peerId, TqPeerMetrics& out) override {
        std::shared_ptr<PeerRuntime> runtime = Find(peerId);
        if (!runtime || !runtime->Quic) {
            return false;
        }
        out.ConnectionCount = runtime->Quic->ConnectionCount();
        out.ConnectedConnections = runtime->Quic->ConnectedConnectionCount();
        out.State = out.ConnectedConnections > 0 ? "healthy" : "connecting";
        return true;
    }

    void DrainPeer(const std::string& peerId, uint32_t graceSeconds) override {
        std::shared_ptr<PeerRuntime> runtime;
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
        std::thread([runtime = std::move(runtime), graceSeconds]() {
            std::this_thread::sleep_for(std::chrono::seconds(graceSeconds));
            runtime->StopAll();
        }).detach();
    }

private:
    struct PeerRuntime {
        std::string PeerId;
        TqConfig Config;
        std::mutex TunnelStartMutex;
        std::mutex ListenerMutex;
        std::unique_ptr<QuicClientSession> Quic;
        TqClientIngressReactor* Ingress{nullptr};
        TqClientIngressTunnelStartFn StartTunnel;
        bool AcceptingEnabled{false};
        bool ListenersOpen{false};

        ~PeerRuntime() { StopAll(); }

        bool OpenListenersLocked(std::string& err) {
            if (ListenersOpen) {
                return true;
            }
            if (Ingress == nullptr || !StartTunnel) {
                err = "listener runtime is not initialized";
                return false;
            }

            TqClientIngressPeer peer{};
            peer.PeerId = PeerId;
            peer.SocksListen = Config.SocksListen;
            peer.HttpListen = Config.HttpListen;
            peer.Config = Config;
            peer.StartTunnel = StartTunnel;
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
            std::fprintf(stderr, "tcpquic-proxy: peer %s SOCKS5 listening on %s\n",
                PeerId.c_str(), Config.SocksListen.c_str());
            if (!Config.HttpListen.empty()) {
                std::fprintf(stderr, "tcpquic-proxy: peer %s HTTP CONNECT listening on %s\n",
                    PeerId.c_str(), Config.HttpListen.c_str());
            }
            return true;
        }

        bool ApplyCurrentConnectionState(std::string& err, bool requireConnected) {
            std::lock_guard<std::mutex> guard(ListenerMutex);
            return ApplyCurrentConnectionStateLocked(err, requireConnected);
        }

        bool ApplyConnectionState(uint32_t connectedCount, std::string& err, bool requireConnected) {
            std::lock_guard<std::mutex> guard(ListenerMutex);
            return ApplyConnectionStateLocked(connectedCount, err, requireConnected);
        }

        bool EnableAcceptingAndApplyCurrentConnectionState(std::string& err, bool requireConnected) {
            std::lock_guard<std::mutex> guard(ListenerMutex);
            AcceptingEnabled = true;
            const bool applied = ApplyCurrentConnectionStateLocked(err, requireConnected);
            if (!applied) {
                AcceptingEnabled = false;
                CloseListenersLocked();
            }
            return applied;
        }

        bool ApplyCurrentConnectionStateLocked(std::string& err, bool requireConnected) {
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

        bool ApplyConnectionStateLocked(uint32_t connectedCount, std::string& err, bool requireConnected) {
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

        void CloseListenersLocked() {
            if (ListenersOpen && Ingress != nullptr) {
                (void)Ingress->RemovePeer(PeerId);
            }
            ListenersOpen = false;
        }

        void CloseListeners() {
            std::lock_guard<std::mutex> guard(ListenerMutex);
            CloseListenersLocked();
        }

        void DisableAccepting() {
            std::lock_guard<std::mutex> guard(ListenerMutex);
            AcceptingEnabled = false;
            CloseListenersLocked();
        }

        void StopAccepting() {
            DisableAccepting();
        }

        void StopAll() {
            DisableAccepting();
            if (Quic) {
                Quic->Stop();
                Quic.reset();
            }
        }
    };

    std::shared_ptr<PeerRuntime> Find(const std::string& peerId) {
        std::lock_guard<std::mutex> guard(Lock);
        auto it = Peers.find(peerId);
        return it == Peers.end() ? nullptr : it->second;
    }

    TqConfig BaseConfig;
    std::mutex Lock;
    std::mutex IngressLock;
    std::unique_ptr<TqClientIngressReactor> Ingress;
    std::unordered_map<std::string, std::shared_ptr<PeerRuntime>> Peers;

    bool EnsureIngressStarted(std::string& err) {
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
};


struct TqTraceGuard {
    ~TqTraceGuard() { TqTraceShutdown(); }
};

struct TqSinglePeerClientRuntime {
    TqSinglePeerClientRuntime(const TqConfig& config, QuicClientSession& quic)
        : Config(config), Quic(&quic) {}

    ~TqSinglePeerClientRuntime() {
        DisableAccepting();
        Ingress.Stop();
    }

    bool Start(std::string& err) {
        if (!Ingress.Start()) {
            err = "failed to start client ingress reactor";
            return false;
        }
        return true;
    }

    void SetStartTunnel(TqClientIngressTunnelStartFn startTunnel) {
        StartTunnel = std::move(startTunnel);
    }

    bool EnqueueDelayed(std::chrono::milliseconds delay, std::function<void()> task) {
        return Ingress.EnqueueDelayed(delay, std::move(task));
    }

    bool OpenListenersLocked(std::string& err) {
        if (ListenersOpen) {
            return true;
        }
        if (!StartTunnel) {
            err = "listener runtime is not initialized";
            return false;
        }

        TqClientIngressPeer peer{};
        peer.PeerId = "primary";
        peer.SocksListen = Config.SocksListen;
        peer.HttpListen = Config.HttpListen;
        peer.Config = Config;
        peer.StartTunnel = StartTunnel;
        peer.AcceptTunnel = [](TqClientTunnelOpenHandle* handle) {
            return TqAcceptClientTunnelOpen(handle);
        };
        peer.RejectTunnel = [](TqClientTunnelOpenHandle* handle) {
            TqRejectClientTunnelOpen(handle);
        };
        peer.CancelTunnel = [](TqClientTunnelOpenHandle* handle) {
            TqCancelClientTunnelOpen(handle);
        };
        if (!Ingress.AddPeer(peer)) {
            err = "failed to add ingress reactor primary peer";
            return false;
        }

        ListenersOpen = true;
        std::fprintf(stderr, "tcpquic-proxy: SOCKS5 listening on %s\n", Config.SocksListen.c_str());
        if (!Config.HttpListen.empty()) {
            std::fprintf(stderr, "tcpquic-proxy: HTTP CONNECT listening on %s\n", Config.HttpListen.c_str());
        }
        return true;
    }

    bool ApplyCurrentConnectionState(std::string& err, bool requireConnected) {
        std::lock_guard<std::mutex> guard(ListenerMutex);
        return ApplyCurrentConnectionStateLocked(err, requireConnected);
    }

    bool ApplyConnectionState(uint32_t connectedCount, std::string& err, bool requireConnected) {
        std::lock_guard<std::mutex> guard(ListenerMutex);
        return ApplyConnectionStateLocked(connectedCount, err, requireConnected);
    }

    bool EnableAcceptingAndApplyCurrentConnectionState(std::string& err, bool requireConnected) {
        std::lock_guard<std::mutex> guard(ListenerMutex);
        AcceptingEnabled = true;
        const bool applied = ApplyCurrentConnectionStateLocked(err, requireConnected);
        if (!applied) {
            AcceptingEnabled = false;
            CloseListenersLocked();
        }
        return applied;
    }

    bool ApplyCurrentConnectionStateLocked(std::string& err, bool requireConnected) {
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

    bool ApplyConnectionStateLocked(uint32_t connectedCount, std::string& err, bool requireConnected) {
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

    void CloseListenersLocked() {
        if (ListenersOpen) {
            (void)Ingress.RemovePeer("primary");
        }
        ListenersOpen = false;
    }

    void CloseListeners() {
        std::lock_guard<std::mutex> guard(ListenerMutex);
        CloseListenersLocked();
    }

    void DisableAccepting() {
        std::lock_guard<std::mutex> guard(ListenerMutex);
        AcceptingEnabled = false;
        CloseListenersLocked();
    }

    TqClientMetrics SnapshotMetrics() const {
        TqClientMetrics metrics;
        metrics.QuicPeer = Config.QuicPeer;
        metrics.SocksListen = Config.SocksListen;
        metrics.HttpListen = Config.HttpListen;
        metrics.ConnectionCount = Quic ? Quic->ConnectionCount() : 0;
        metrics.ConnectedConnections = Quic ? Quic->ConnectedConnectionCount() : 0;
        return metrics;
    }

    std::string HandleAdmin(const TqHttpRequest& req, uint64_t uptimeSeconds) const {
        const TqClientMetrics metrics = SnapshotMetrics();
        if (req.Method == "GET" && req.Path == "/health") {
            return TqJsonResponse(200, TqClientHealthJson(metrics, uptimeSeconds));
        }
        if (req.Method == "GET" && req.Path == "/metrics") {
            return TqJsonResponse(200, TqClientMetricsJson(metrics, uptimeSeconds));
        }
        return TqJsonResponse(404, "{\"error\":\"not found\"}");
    }

    TqConfig Config;
    QuicClientSession* Quic{nullptr};
    TqClientIngressReactor Ingress;
    TqClientIngressTunnelStartFn StartTunnel;
    std::mutex TunnelStartMutex;
    std::mutex ListenerMutex;
    bool AcceptingEnabled{false};
    bool ListenersOpen{false};
};

int RunSinglePeerClient(const TqConfig& cfg) {
    const auto started = std::chrono::steady_clock::now();
    QuicClientSession quic;
    const TqConfig quicCfg = cfg;

    std::string err;
    auto runtime = std::make_shared<TqSinglePeerClientRuntime>(cfg, quic);
    if (!runtime->Start(err)) {
        std::fprintf(stderr, "tcpquic-proxy: %s\n", err.c_str());
        quic.Stop();
        return 1;
    }
    std::weak_ptr<TqSinglePeerClientRuntime> weakRuntime = runtime;
    runtime->SetStartTunnel([weakRuntime, cfg](
                                const TunnelRequest& req,
                                TqSocketHandle fd,
                                TqClientTunnelOpenComplete onComplete) {
        auto runtime = weakRuntime.lock();
        if (!runtime || !runtime->Quic) {
            return static_cast<TqClientTunnelOpenHandle*>(nullptr);
        }
        MsQuicConnection* conn = nullptr;
        {
            std::lock_guard<std::mutex> guard(runtime->TunnelStartMutex);
            if (!runtime->Quic || !runtime->Quic->EnsureAnyConnected()) {
                std::fprintf(stderr, "tcpquic-proxy: no connected QUIC peer available for tunnel\n");
                return static_cast<TqClientTunnelOpenHandle*>(nullptr);
            }
            conn = runtime->Quic->PickConnection();
            if (conn == nullptr) {
                return static_cast<TqClientTunnelOpenHandle*>(nullptr);
            }
        }
        return TqStartClientTunnelAsync(conn, req, fd, cfg, std::move(onComplete));
    });

    quic.SetDelayedTaskScheduler([weakRuntime](
                                     std::chrono::milliseconds delay,
                                     std::function<void()> task) {
        auto runtime = weakRuntime.lock();
        if (!runtime) {
            return false;
        }
        return runtime->EnqueueDelayed(delay, std::move(task));
    });
    quic.SetConnectionStateHandler([weakRuntime](uint32_t connectedCount) {
        auto runtime = weakRuntime.lock();
        if (!runtime) {
            return;
        }
        std::string listenerErr;
        if (!runtime->ApplyConnectionState(connectedCount, listenerErr, false)) {
            std::fprintf(stderr, "tcpquic-proxy: %s\n", listenerErr.c_str());
        }
    });

    if (!quic.Start(quicCfg)) {
        return 1;
    }

    if (!quic.EnsureAnyConnected()) {
        std::fprintf(stderr,
            "tcpquic-proxy: no connected QUIC peer at startup; listeners remain closed until reconnect\n");
    }

    if (!runtime->EnableAcceptingAndApplyCurrentConnectionState(err, false)) {
        std::fprintf(stderr, "tcpquic-proxy: %s\n", err.c_str());
        quic.SetConnectionStateHandler(QuicClientSession::ConnectionStateHandler{});
        runtime->DisableAccepting();
        return 1;
    }

    if (cfg.SpeedTestMode != TqSpeedTestMode::None) {
        auto cleanupSpeedTest = [&]() {
            quic.SetConnectionStateHandler(QuicClientSession::ConnectionStateHandler{});
            quic.SetDelayedTaskScheduler(QuicClientSession::DelayedTaskScheduler{});
            runtime->DisableAccepting();
            quic.Stop();
        };
        if (!runtime->EnableAcceptingAndApplyCurrentConnectionState(err, true)) {
            std::fprintf(stderr, "tcpquic-proxy: speedtest ingress unavailable: %s\n", err.c_str());
            cleanupSpeedTest();
            return 1;
        }
        if (!quic.EnsureAnyConnected(std::chrono::seconds(10))) {
            std::fprintf(stderr, "tcpquic-proxy: speed test could not connect to QUIC peer\n");
            cleanupSpeedTest();
            return 1;
        }
        MsQuicConnection* controlConn = quic.PickConnection();
        if (controlConn == nullptr) {
            std::fprintf(stderr, "tcpquic-proxy: speed test has no connected QUIC control connection\n");
            cleanupSpeedTest();
            return 1;
        }
        const bool ok = TqRunIngressClientSpeedTest(*controlConn, cfg);
        cleanupSpeedTest();
        return ok ? 0 : 1;
    }

    std::fprintf(stderr, "tcpquic-proxy: QUIC peer %s (%u connections)\n",
        cfg.QuicPeer.c_str(), quic.ConnectionCount());

    std::unique_ptr<TqAdminHttpServer> admin;
    if (!cfg.AdminListen.empty()) {
        if (!TqValidateAdminListen(cfg.AdminListen, err)) {
            std::fprintf(stderr, "tcpquic-proxy: invalid admin listen: %s\n", err.c_str());
            quic.SetConnectionStateHandler(QuicClientSession::ConnectionStateHandler{});
            runtime->DisableAccepting();
            return 1;
        }
        admin.reset(new TqAdminHttpServer(cfg.AdminListen, [runtime, started](const TqHttpRequest& req) {
            const uint64_t uptimeSeconds = static_cast<uint64_t>(
                std::chrono::duration_cast<std::chrono::seconds>(std::chrono::steady_clock::now() - started).count());
            return runtime->HandleAdmin(req, uptimeSeconds);
        }));
        if (!admin->Start(err)) {
            std::fprintf(stderr, "tcpquic-proxy: failed to start admin server: %s\n", err.c_str());
            quic.SetConnectionStateHandler(QuicClientSession::ConnectionStateHandler{});
            runtime->DisableAccepting();
            return 1;
        }
        std::fprintf(stderr, "tcpquic-proxy: admin listening on %s\n", admin->ListenAddress().c_str());
    }

    while (true) {
        std::this_thread::sleep_for(std::chrono::hours(24));
    }
    return 0;
}

bool TqMakeSinglePeerConfigFromRouter(const TqConfig& cfg, TqConfig& out, std::string& err) {
    const TqPeerConfig* selected = nullptr;
    for (const auto& peer : cfg.Router.Peers) {
        if (!peer.Enabled) {
            continue;
        }
        if (selected != nullptr) {
            err = "single-peer operation requires exactly one enabled peer";
            return false;
        }
        selected = &peer;
    }
    if (selected == nullptr) {
        err = "single-peer operation requires one enabled peer";
        return false;
    }

    out = cfg;
    const std::vector<TqProxyAuthUser> proxyAuth = cfg.Router.ProxyAuth;
    out.Router = TqRouterConfig{};
    out.Router.ProxyAuth = proxyAuth;
    out.ClientConfigPath.clear();
    out.QuicPeer = selected->QuicPeer;
    out.SocksListen = selected->SocksListen.empty() ? cfg.SocksListen : selected->SocksListen;
    out.HttpListen = selected->HttpListen.empty() ? cfg.HttpListen : selected->HttpListen;
    out.QuicConnections = selected->QuicConnections == 0 ? cfg.QuicConnections : selected->QuicConnections;
    out.Compress = selected->Compress.empty() ? cfg.Compress : selected->Compress;
    return true;
}

int RunClient(const TqConfig& cfg) {
    if (cfg.Router.Peers.empty()) {
        return RunSinglePeerClient(cfg);
    }

    if (cfg.SpeedTestMode != TqSpeedTestMode::None) {
        TqConfig singlePeerCfg;
        std::string err;
        if (!TqMakeSinglePeerConfigFromRouter(cfg, singlePeerCfg, err)) {
            std::fprintf(stderr, "tcpquic-proxy: %s\n", err.c_str());
            return 1;
        }
        return RunSinglePeerClient(singlePeerCfg);
    }

    TqMultiPeerRuntimeAdapter adapter(cfg);
    TqRouterRuntime runtime(&adapter);
    std::string err;
    if (!runtime.ApplyConfig(cfg.Router, err)) {
        std::fprintf(stderr, "tcpquic-proxy: invalid router config: %s\n", err.c_str());
        return 1;
    }

    std::unique_ptr<TqAdminHttpServer> admin;
    if (!cfg.AdminListen.empty()) {
        if (!TqValidateAdminListen(cfg.AdminListen, err)) {
            std::fprintf(stderr, "tcpquic-proxy: invalid admin listen: %s\n", err.c_str());
            return 1;
        }
        admin.reset(new TqAdminHttpServer(cfg.AdminListen, [&runtime](const TqHttpRequest& req) {
            return runtime.HandleAdmin(req);
        }));
        if (!admin->Start(err)) {
            std::fprintf(stderr, "tcpquic-proxy: failed to start admin server: %s\n", err.c_str());
            return 1;
        }
        std::fprintf(stderr, "tcpquic-proxy: admin listening on %s\n", admin->ListenAddress().c_str());
    }

    while (true) {
        std::this_thread::sleep_for(std::chrono::hours(24));
    }
}

int RunServer(const TqConfig& cfg) {
    TqAcl acl;
    acl.AllowCidrs = cfg.AllowTargets;
    acl.DenyCidrs = cfg.DenyTargets;

    auto metrics = std::make_shared<TqServerMetrics>();
    auto speed = std::make_shared<TqServerSpeedTestController>();
    metrics->Listen = cfg.QuicListen;
    const auto started = std::chrono::steady_clock::now();

    TqServerDialReactor serverDial(acl);
    if (!serverDial.Start()) {
        std::lock_guard<std::mutex> guard(metrics->Lock);
        metrics->LastError = "failed to start server dial reactor";
        return 1;
    }
    struct ServerDialGuard {
        TqServerDialReactor* Reactor{nullptr};
        ~ServerDialGuard() {
            TqSetServerDialReactor(nullptr);
            if (Reactor != nullptr) {
                Reactor->Stop();
            }
        }
    } serverDialGuard{&serverDial};
    TqSetServerDialReactor(&serverDial);

    QuicServerSession quic;
    quic.SetConnectionHandler([metrics](MsQuicConnection*) {
        TqServerMetricsConnectionAccepted(*metrics);
    });
    quic.SetPeerStreamHandler([&acl, &cfg, metrics, speed](MsQuicConnection* conn, HQUIC stream) {
        TqServerMetricsStreamStarted(*metrics);
        if (stream == nullptr) {
            {
                std::lock_guard<std::mutex> guard(metrics->Lock);
                metrics->LastError = "null stream";
            }
            TqServerMetricsStreamFinished(*metrics);
            return;
        }
        TqHandleServerIncomingStream(conn, stream, acl, cfg, speed.get(), [metrics]() {
            TqServerMetricsStreamFinished(*metrics);
        }, [metrics]() {
            metrics->AclDenied.fetch_add(1);
        });
    });

    if (!quic.Start(cfg)) {
        std::lock_guard<std::mutex> guard(metrics->Lock);
        metrics->LastError = "failed to start QUIC server";
        return 1;
    }

    std::unique_ptr<TqAdminHttpServer> admin;
    if (!cfg.AdminListen.empty()) {
        std::string err;
        if (!TqValidateAdminListen(cfg.AdminListen, err)) {
            std::fprintf(stderr, "tcpquic-proxy: invalid admin listen: %s\n", err.c_str());
            return 1;
        }
        admin.reset(new TqAdminHttpServer(cfg.AdminListen, [metrics, started](const TqHttpRequest& req) {
            const uint64_t uptimeSeconds = static_cast<uint64_t>(
                std::chrono::duration_cast<std::chrono::seconds>(std::chrono::steady_clock::now() - started).count());
            if (req.Method == "GET" && req.Path == "/health") {
                return TqJsonResponse(200, TqServerHealthJson(*metrics, uptimeSeconds));
            }
            if (req.Method == "GET" && req.Path == "/metrics") {
                return TqJsonResponse(200, TqServerMetricsJson(*metrics, uptimeSeconds));
            }
            return TqJsonResponse(404, "{\"error\":\"not found\"}");
        }));
        if (!admin->Start(err)) {
            std::fprintf(stderr, "tcpquic-proxy: failed to start admin server: %s\n", err.c_str());
            return 1;
        }
        std::fprintf(stderr, "tcpquic-proxy: admin listening on %s\n", admin->ListenAddress().c_str());
    }

    std::fprintf(stderr, "tcpquic-proxy: QUIC server listening on %s\n", cfg.QuicListen.c_str());
    quic.Run();
    speed->StopAll();
    return 0;
}

} // namespace

int main(int argc, char** argv) {
    TqSocketStartup socketStartup;
    if (!socketStartup.Ok()) {
        std::fprintf(stderr, "tcpquic-proxy: failed to initialize socket runtime\n");
        return 1;
    }

    TqConfig cfg;
    std::string err;
    if (!TqParseArgs(argc, argv, cfg, err)) {
        std::fprintf(stderr, "Error: %s\n", err.c_str());
        TqPrintUsage(stderr);
        return 1;
    }

    TqFinalizeConfig(cfg);
    const char* quicProfileName =
        cfg.QuicProfile == TqQuicProfile::LowLatency ? "low-latency" : "max-throughput";
    std::fprintf(stderr, "tcpquic-proxy QUIC execution profile: %s\n", quicProfileName);
    TqTunnelReaperGuard reaperGuard;
    TqSetActiveTcpSocketBuffer(cfg.Tuning.TcpSocketBufferBytes);
    TqPrintTuning(cfg.Tuning, stderr);
    TqPrintRelayMemoryBudget(stderr);
    TqPrintRelayBackend(stderr, cfg.Tuning);
    if (TqRuntimeTuningEnabled(cfg)) {
        std::fprintf(stderr,
            "tcpquic-proxy runtime tuning: enabled (RTT/throughput feed next QUIC connection)\n");
    }
    if (TqCompressionAdaptiveEnabled(cfg)) {
        std::fprintf(stderr,
            "tcpquic-proxy compress auto: probe=%s (ratio samples choose off/zstd)\n",
            TqResolveAutoCompress(cfg));
    }

    TqTraceGuard traceGuard;
    if (cfg.Trace) {
        if (!TqTraceInit(cfg.Mode, cfg.TraceIntervalSec)) {
            std::fprintf(stderr, "tcpquic-proxy: warning: trace log init failed\n");
        } else {
            std::fprintf(stderr,
                "tcpquic-proxy: trace enabled (interval=%us, log under <exe>/log/)\n",
                cfg.TraceIntervalSec);
        }
    }

    if (cfg.Mode == TqMode::Client) {
        return RunClient(cfg);
    }

    return RunServer(cfg);
}
