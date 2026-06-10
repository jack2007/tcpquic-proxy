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

        auto runtime = std::make_shared<PeerRuntime>();
        runtime->Config = peerCfg;
        runtime->Quic = std::make_unique<QuicClientSession>();
        if (!runtime->Quic->Start(peerCfg)) {
            err = "failed to start QUIC client for " + peer.PeerId;
            return false;
        }

        runtime->Pool = std::make_unique<TqThreadPool>(peerCfg.HandshakeThreads);
        runtime->Pool->Start();
        TunnelStartFn startTunnel = [runtime = runtime.get(), peerCfg](const TunnelRequest& req, TqSocketHandle fd) {
            MsQuicConnection* conn = nullptr;
            {
                std::lock_guard<std::mutex> guard(runtime->TunnelStartMutex);
                if (!runtime->Quic->EnsureConnected()) {
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

        runtime->Socks = std::make_unique<TqSocks5Server>(peerCfg.SocksListen, startTunnel, runtime->Pool.get());
        if (!runtime->Socks->Start(err)) {
            runtime->StopAll();
            return false;
        }
        if (!peerCfg.HttpListen.empty()) {
            runtime->Http = std::make_unique<TqHttpConnectServer>(peerCfg.HttpListen, startTunnel, runtime->Pool.get());
            if (!runtime->Http->Start(err)) {
                runtime->StopAll();
                return false;
            }
        }

        std::fprintf(stderr, "tcpquic-proxy: peer %s SOCKS5 listening on %s\n",
            peer.PeerId.c_str(), peerCfg.SocksListen.c_str());
        if (!peerCfg.HttpListen.empty()) {
            std::fprintf(stderr, "tcpquic-proxy: peer %s HTTP CONNECT listening on %s\n",
                peer.PeerId.c_str(), peerCfg.HttpListen.c_str());
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

    bool SnapshotPeerMetrics(const std::string& peerId, TqPeerMetrics& out) override {
        std::shared_ptr<PeerRuntime> runtime = Find(peerId);
        if (!runtime || !runtime->Quic) {
            return false;
        }
        out.State = "healthy";
        out.ConnectionCount = runtime->Quic->ConnectionCount();
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
        TqConfig Config;
        std::mutex TunnelStartMutex;
        std::unique_ptr<QuicClientSession> Quic;
        std::unique_ptr<TqThreadPool> Pool;
        std::unique_ptr<TqSocks5Server> Socks;
        std::unique_ptr<TqHttpConnectServer> Http;

        ~PeerRuntime() { StopAll(); }

        void StopAccepting() {
            if (Socks) {
                Socks->Stop();
                Socks.reset();
            }
            if (Http) {
                Http->Stop();
                Http.reset();
            }
        }

        void StopAll() {
            StopAccepting();
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

int RunSinglePeerClient(const TqConfig& cfg) {
    QuicClientSession quic;
    if (!quic.Start(cfg)) {
        return 1;
    }

    if (cfg.TraceConnectOnStart || cfg.Trace) {
        if (!quic.EnsureConnected()) {
            std::fprintf(stderr, "tcpquic-proxy: failed to connect to QUIC peer at startup\n");
            return 1;
        }
    }

    if (cfg.WarmupMb > 0) {
        if (!TqRunClientWarmup(quic, cfg)) {
            std::fprintf(stderr, "tcpquic-proxy: client warmup failed\n");
            return 1;
        }
    }

    std::mutex clientTunnelStartMutex;
    TunnelStartFn startTunnel = [&quic, &cfg, &clientTunnelStartMutex](const TunnelRequest& req, TqSocketHandle fd) {
        MsQuicConnection* conn = nullptr;
        {
            std::lock_guard<std::mutex> guard(clientTunnelStartMutex);
            if (!quic.EnsureConnected()) {
                std::fprintf(stderr, "tcpquic-proxy: no connected QUIC peer available for tunnel\n");
                return TqTunnelStartResult{false, TqOpenError::Internal};
            }
            conn = quic.PickConnection();
            if (conn == nullptr) {
                return TqTunnelStartResult{false, TqOpenError::Internal};
            }
        }
        return TqStartClientTunnel(conn, req, fd, cfg);
    };

    std::fprintf(stderr, "tcpquic-proxy: SOCKS5 listening on %s\n", cfg.SocksListen.c_str());
    if (!cfg.HttpListen.empty()) {
        std::fprintf(stderr, "tcpquic-proxy: HTTP CONNECT listening on %s\n", cfg.HttpListen.c_str());
    }
    std::fprintf(stderr, "tcpquic-proxy: QUIC peer %s (%u connections)\n",
        cfg.QuicPeer.c_str(), quic.ConnectionCount());

    TqThreadPool handshakePool(cfg.HandshakeThreads);
    handshakePool.Start();

    std::thread socksThread(RunSocks5Server, cfg.SocksListen, startTunnel, &handshakePool);
    std::unique_ptr<std::thread> httpThread;
    if (!cfg.HttpListen.empty()) {
        httpThread.reset(new std::thread(RunHttpConnectServer, cfg.HttpListen, startTunnel, &handshakePool));
    }

    socksThread.join();
    if (httpThread) {
        httpThread->join();
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
