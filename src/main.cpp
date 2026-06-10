#include "config.h"
#include "admin_http.h"
#include "platform_socket.h"
#include "quic_session.h"
#include "acl.h"
#include "http_connect_server.h"
#include "router_runtime.h"
#include "server_metrics.h"
#include "socks5_server.h"
#include "tcp_tunnel.h"
#include "thread_pool.h"
#include "tuning.h"
#include "tunnel_reaper.h"
#include "warmup.h"
#include "trace.h"

#include <chrono>
#include <cstdio>
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
        peerCfg.QuicReconnectIntervalMs = peer.QuicReconnectIntervalMs == 0
            ? BaseConfig.QuicReconnectIntervalMs
            : peer.QuicReconnectIntervalMs;
        peerCfg.Compress = peer.Compress.empty() ? BaseConfig.Compress : peer.Compress;

        auto runtime = std::make_shared<PeerRuntime>();
        runtime->PeerId = peer.PeerId;
        runtime->Config = peerCfg;
        runtime->Quic = std::make_unique<QuicClientSession>();
        if (!runtime->Quic->Start(peerCfg)) {
            err = "failed to start QUIC client for " + peer.PeerId;
            return false;
        }

        runtime->Pool = std::make_unique<TqThreadPool>(peerCfg.HandshakeThreads);
        runtime->Pool->Start();
        std::weak_ptr<PeerRuntime> weakRuntime = runtime;
        runtime->StartTunnel = [weakRuntime, peerCfg](const TunnelRequest& req, TqSocketHandle fd) {
            auto runtime = weakRuntime.lock();
            if (!runtime || !runtime->Quic) {
                return TqTunnelStartResult{false, TqOpenError::Internal};
            }
            MsQuicConnection* conn = nullptr;
            {
                std::lock_guard<std::mutex> guard(runtime->TunnelStartMutex);
                if (!runtime->Quic || !runtime->Quic->EnsureAnyConnected()) {
                    std::fprintf(stderr, "tcpquic-proxy: peer %s has no connected QUIC connection for tunnel\n", peerCfg.QuicPeer.c_str());
                    return TqTunnelStartResult{false, TqOpenError::Internal};
                }
                conn = runtime->Quic->PickConnection();
                if (conn == nullptr) {
                    return TqTunnelStartResult{false, TqOpenError::Internal};
                }
            }
            return TqStartClientTunnel(conn, req, fd, peerCfg);
        };

        runtime->Quic->SetConnectionStateHandler([weakRuntime](uint32_t connectedCount) {
            (void)connectedCount;
            auto runtime = weakRuntime.lock();
            if (!runtime) {
                return;
            }
            std::string listenerErr;
            if (!runtime->ApplyCurrentConnectionState(listenerErr, false)) {
                std::fprintf(stderr, "tcpquic-proxy: peer %s %s\n",
                    runtime->PeerId.c_str(), listenerErr.c_str());
            }
        });

        if (!runtime->Quic->EnsureAnyConnected()) {
            err = "failed to connect QUIC client for " + peer.PeerId;
            runtime->StopAll();
            return false;
        }
        if (!runtime->EnableAcceptingAndApplyCurrentConnectionState(err)) {
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
        std::unique_ptr<TqThreadPool> Pool;
        std::unique_ptr<TqSocks5Server> Socks;
        std::unique_ptr<TqHttpConnectServer> Http;
        TunnelStartFn StartTunnel;
        bool AcceptingEnabled{false};

        ~PeerRuntime() { StopAll(); }

        bool OpenListenersLocked(std::string& err) {
            if (Socks) {
                return true;
            }
            if (!Pool || !StartTunnel) {
                err = "listener runtime is not initialized";
                return false;
            }

            auto socks = std::make_unique<TqSocks5Server>(Config.SocksListen, StartTunnel, Pool.get());
            if (!socks->Start(err)) {
                return false;
            }

            std::unique_ptr<TqHttpConnectServer> http;
            if (!Config.HttpListen.empty()) {
                http = std::make_unique<TqHttpConnectServer>(Config.HttpListen, StartTunnel, Pool.get());
                if (!http->Start(err)) {
                    socks->Stop();
                    return false;
                }
            }

            Socks = std::move(socks);
            Http = std::move(http);
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

        bool EnableAcceptingAndApplyCurrentConnectionState(std::string& err) {
            std::lock_guard<std::mutex> guard(ListenerMutex);
            AcceptingEnabled = true;
            const bool applied = ApplyCurrentConnectionStateLocked(err, true);
            if (!applied) {
                AcceptingEnabled = false;
                CloseListenersLocked();
            }
            return applied;
        }

        bool ApplyCurrentConnectionStateLocked(std::string& err, bool requireConnected) {
            const uint32_t connectedCount = Quic ? Quic->ConnectedConnectionCount() : 0;
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
            if (Socks) {
                Socks->Stop();
                Socks.reset();
            }
            if (Http) {
                Http->Stop();
                Http.reset();
            }
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
            if (Pool) {
                Pool->Stop();
                Pool.reset();
            }
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
    std::unordered_map<std::string, std::shared_ptr<PeerRuntime>> Peers;
};


struct TqTraceGuard {
    ~TqTraceGuard() { TqTraceShutdown(); }
};

struct TqSinglePeerClientRuntime {
    TqSinglePeerClientRuntime(const TqConfig& config, QuicClientSession& quic)
        : Config(config), Quic(&quic), Pool(config.HandshakeThreads) {}

    ~TqSinglePeerClientRuntime() {
        DisableAccepting();
        Pool.Stop();
    }

    void Start() {
        Pool.Start();
    }

    void SetStartTunnel(TunnelStartFn startTunnel) {
        StartTunnel = std::move(startTunnel);
    }

    bool OpenListenersLocked(std::string& err) {
        if (Socks) {
            return true;
        }
        if (!StartTunnel) {
            err = "listener runtime is not initialized";
            return false;
        }

        auto socks = std::make_unique<TqSocks5Server>(Config.SocksListen, StartTunnel, &Pool);
        if (!socks->Start(err)) {
            return false;
        }

        std::unique_ptr<TqHttpConnectServer> http;
        if (!Config.HttpListen.empty()) {
            http = std::make_unique<TqHttpConnectServer>(Config.HttpListen, StartTunnel, &Pool);
            if (!http->Start(err)) {
                socks->Stop();
                return false;
            }
        }

        Socks = std::move(socks);
        Http = std::move(http);
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

    bool EnableAcceptingAndApplyCurrentConnectionState(std::string& err) {
        std::lock_guard<std::mutex> guard(ListenerMutex);
        AcceptingEnabled = true;
        const bool applied = ApplyCurrentConnectionStateLocked(err, true);
        if (!applied) {
            AcceptingEnabled = false;
            CloseListenersLocked();
        }
        return applied;
    }

    bool ApplyCurrentConnectionStateLocked(std::string& err, bool requireConnected) {
        const uint32_t connectedCount = Quic ? Quic->ConnectedConnectionCount() : 0;
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
        if (Socks) {
            Socks->Stop();
            Socks.reset();
        }
        if (Http) {
            Http->Stop();
            Http.reset();
        }
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

    TqConfig Config;
    QuicClientSession* Quic{nullptr};
    TqThreadPool Pool;
    std::mutex TunnelStartMutex;
    std::mutex ListenerMutex;
    TunnelStartFn StartTunnel;
    std::unique_ptr<TqSocks5Server> Socks;
    std::unique_ptr<TqHttpConnectServer> Http;
    bool AcceptingEnabled{false};
};

int RunSinglePeerClient(const TqConfig& cfg) {
    QuicClientSession quic;
    if (!quic.Start(cfg)) {
        return 1;
    }

    if (!quic.EnsureAnyConnected()) {
        std::fprintf(stderr, "tcpquic-proxy: failed to connect to QUIC peer at startup\n");
        return 1;
    }

    if (cfg.WarmupMb > 0) {
        if (!TqRunClientWarmup(quic, cfg)) {
            std::fprintf(stderr, "tcpquic-proxy: client warmup failed\n");
            return 1;
        }
    }

    auto runtime = std::make_shared<TqSinglePeerClientRuntime>(cfg, quic);
    runtime->Start();
    std::weak_ptr<TqSinglePeerClientRuntime> weakRuntime = runtime;
    runtime->SetStartTunnel([weakRuntime, cfg](const TunnelRequest& req, TqSocketHandle fd) {
        auto runtime = weakRuntime.lock();
        if (!runtime || !runtime->Quic) {
            return TqTunnelStartResult{false, TqOpenError::Internal};
        }
        MsQuicConnection* conn = nullptr;
        {
            std::lock_guard<std::mutex> guard(runtime->TunnelStartMutex);
            if (!runtime->Quic || !runtime->Quic->EnsureAnyConnected()) {
                std::fprintf(stderr, "tcpquic-proxy: no connected QUIC peer available for tunnel\n");
                return TqTunnelStartResult{false, TqOpenError::Internal};
            }
            conn = runtime->Quic->PickConnection();
            if (conn == nullptr) {
                return TqTunnelStartResult{false, TqOpenError::Internal};
            }
        }
        return TqStartClientTunnel(conn, req, fd, cfg);
    });

    quic.SetConnectionStateHandler([weakRuntime](uint32_t connectedCount) {
        (void)connectedCount;
        auto runtime = weakRuntime.lock();
        if (!runtime) {
            return;
        }
        std::string listenerErr;
        if (!runtime->ApplyCurrentConnectionState(listenerErr, false)) {
            std::fprintf(stderr, "tcpquic-proxy: %s\n", listenerErr.c_str());
        }
    });

    if (!quic.EnsureAnyConnected()) {
        std::fprintf(stderr, "tcpquic-proxy: failed to connect to QUIC peer at startup\n");
        quic.SetConnectionStateHandler(QuicClientSession::ConnectionStateHandler{});
        runtime->DisableAccepting();
        runtime->Pool.Stop();
        return 1;
    }

    std::string err;
    if (!runtime->EnableAcceptingAndApplyCurrentConnectionState(err)) {
        std::fprintf(stderr, "tcpquic-proxy: %s\n", err.c_str());
        quic.SetConnectionStateHandler(QuicClientSession::ConnectionStateHandler{});
        runtime->DisableAccepting();
        runtime->Pool.Stop();
        return 1;
    }

    std::fprintf(stderr, "tcpquic-proxy: QUIC peer %s (%u connections)\n",
        cfg.QuicPeer.c_str(), quic.ConnectionCount());

    while (true) {
        std::this_thread::sleep_for(std::chrono::hours(24));
    }
    return 0;
}

int RunClient(const TqConfig& cfg) {
    if (cfg.ClientConfigPath.empty()) {
        return RunSinglePeerClient(cfg);
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
    metrics->Listen = cfg.QuicListen;
    const auto started = std::chrono::steady_clock::now();

    QuicServerSession quic;
    quic.SetConnectionHandler([metrics](MsQuicConnection*) {
        TqServerMetricsConnectionAccepted(*metrics);
    });
    quic.SetPeerStreamHandler([&acl, &cfg, metrics](MsQuicConnection* conn, HQUIC stream) {
        TqServerMetricsStreamStarted(*metrics);
        if (stream == nullptr) {
            {
                std::lock_guard<std::mutex> guard(metrics->Lock);
                metrics->LastError = "null stream";
            }
            TqServerMetricsStreamFinished(*metrics);
            return;
        }
        TqHandleServerPeerStream(conn, stream, acl, cfg, [metrics]() {
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
    TqSetRelayMemoryBudget(cfg.MaxMemoryMb);
    TqPrintTuning(cfg.Tuning, stderr);
    if (cfg.MaxMemoryMb > 0) {
        std::fprintf(stderr,
            "tcpquic-proxy relay memory budget: %u MB (pool scales by active tunnels)\n",
            cfg.MaxMemoryMb);
    }
    if (TqRuntimeTuningEnabled(cfg)) {
        std::fprintf(stderr,
            "tcpquic-proxy runtime tuning: enabled (RTT/throughput feed next QUIC connection)\n");
    }
    if (TqCompressionAdaptiveEnabled(cfg)) {
        std::fprintf(stderr,
            "tcpquic-proxy compress auto: probe=%s (ratio samples choose off/lz4/zstd)\n",
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
