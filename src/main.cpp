#include "config.h"
#include "admin_http.h"
#include "admin_auth.h"
#include "crash_dump.h"
#include "platform_socket.h"
#include "client_peer_runtime.h"
#include "acl.h"
#include "server_dial_reactor.h"
#include "router_runtime.h"
#include "runtime_config_file_store.h"
#include "server_admin.h"
#include "server_metrics.h"
#include "server_runtime_config.h"
#include "speed_test.h"
#include "tuning.h"
#include "tunnel_reaper.h"
#include "shutdown.h"
#include "trace.h"

#include <chrono>
#include <cstdio>
#include <filesystem>
#include <memory>
#include <mutex>
#include <system_error>
#include <thread>

namespace {

struct TqTunnelReaperGuard {
    TqTunnelReaperGuard() { TqTunnelReaper::Instance().Start(); }
    ~TqTunnelReaperGuard() { TqTunnelReaper::Instance().Stop(); }
};

class TqMultiPeerRuntimeAdapter : public TqPeerRuntimeAdapter {
public:
    explicit TqMultiPeerRuntimeAdapter(const TqConfig& baseConfig) : Manager(baseConfig) {}

    bool StartPeer(const TqPeerConfig& peer, std::string& err) override {
        return Manager.StartPeer(peer, err);
    }

    void StopAccepting(const std::string& peerId) override {
        Manager.StopAccepting(peerId);
    }

    void AbortPeerTunnels(const std::string& peerId) override {
        Manager.AbortPeerTunnels(peerId);
    }

    bool SnapshotPeerMetrics(const std::string& peerId, TqPeerMetrics& out) override {
        return Manager.SnapshotPeerMetrics(peerId, out);
    }

    std::vector<TqConnectionSnapshot> SnapshotConnections(const std::string& peerId) override {
        return Manager.SnapshotConnections(peerId);
    }

    bool SetDesiredConnectionCount(const std::string& peerId, uint32_t desired, std::string& err) override {
        return Manager.SetDesiredConnectionCount(peerId, desired, err);
    }

    bool StopHighestConnection(
        const std::string& peerId,
        const std::string& connectionId,
        std::string& err) override {
        return Manager.StopHighestConnection(peerId, connectionId, err);
    }

    bool ReconnectConnection(
        const std::string& peerId,
        const std::string& connectionId,
        std::string& err) override {
        return Manager.ReconnectConnection(peerId, connectionId, err);
    }

    bool AbortConnectionTunnels(
        const std::string& peerId,
        const std::string& connectionId,
        std::string& err) override {
        return Manager.AbortConnectionTunnels(peerId, connectionId, err);
    }

    void DrainPeer(const std::string& peerId, uint32_t graceSeconds) override {
        Manager.DrainPeer(peerId, graceSeconds);
    }

private:
    TqClientRuntimeManager Manager;
};


struct TqTraceGuard {
    ~TqTraceGuard() {
        TqDiagStatsShutdown();
        TqTraceShutdown();
    }
};

TqAdminHttpServerOptions TqMakeAdminOptions(const TqConfig& cfg) {
    TqAdminHttpServerOptions options;
    options.AdminThreads = cfg.AdminThreads;
    const char* role = cfg.Mode == TqMode::Client ? "client" : "server";
    options.Role = role;
    options.TokenFile = cfg.AdminTokenFile.empty() ? TqAdminAuth::DefaultTokenFilePath(role) : cfg.AdminTokenFile;
    options.PersistTokenFile = !cfg.AdminTokenFile.empty();
    options.EnableTokenAuth = true;
    return options;
}

void TqPrintAdminStarted(const TqAdminHttpServer& admin, const TqAdminHttpServerOptions& options) {
    std::fprintf(stderr, "tcpquic-proxy: admin listening on %s\n", admin.ListenAddress().c_str());
    std::fprintf(stderr, "tcpquic-proxy: admin token file %s\n", options.TokenFile.c_str());
}

std::string TqAbsolutePathString(const std::string& path) {
    std::error_code ec;
    const std::filesystem::path absolute = std::filesystem::absolute(path, ec);
    return ec ? path : absolute.string();
}

int RunSinglePeerClient(const TqConfig& cfg) {
    std::string err;
    const TqPeerConfig primary = TqMakePrimaryPeerConfig(cfg);

    if (cfg.SpeedTestMode != TqSpeedTestMode::None) {
        TqClientRuntimeManager manager(cfg, TqClientPeerLogMode::Primary);
        if (!manager.StartPeer(primary, err)) {
            std::fprintf(stderr, "tcpquic-proxy: %s\n", err.c_str());
            return 1;
        }
        if (!manager.EnsureAnyConnected("primary", std::chrono::seconds(10))) {
            std::fprintf(stderr, "tcpquic-proxy: speed test could not connect to QUIC peer\n");
            manager.StopAll();
            return 1;
        }
        if (!manager.EnableAcceptingAndApplyCurrentConnectionState("primary", err, true)) {
            std::fprintf(stderr, "tcpquic-proxy: speedtest ingress unavailable: %s\n", err.c_str());
            manager.StopAll();
            return 1;
        }
        MsQuicConnection* controlConn = manager.PickConnection("primary");
        if (controlConn == nullptr) {
            std::fprintf(stderr, "tcpquic-proxy: speed test has no connected QUIC control connection\n");
            manager.StopAll();
            return 1;
        }
        const bool ok = TqRunIngressClientSpeedTest(*controlConn, cfg);
        manager.AbortPeerTunnels(primary.PeerId);
        manager.StopAll();
        return ok ? 0 : 1;
    }

    const TqRouterConfig router = TqMakeSinglePeerRouterConfig(cfg);
    TqMultiPeerRuntimeAdapter adapter(cfg);
    TqRouterRuntime runtime(&adapter, cfg);
    if (!runtime.ApplyConfig(router, err)) {
        std::fprintf(stderr, "tcpquic-proxy: invalid router config: %s\n", err.c_str());
        return 1;
    }

    TqPeerMetrics startupMetrics;
    if (!runtime.GetPeer(primary.PeerId, startupMetrics)) {
        std::fprintf(stderr, "tcpquic-proxy: client runtime unavailable\n");
        return 1;
    }
    std::fprintf(stderr, "tcpquic-proxy: QUIC peer %s (%u connections)\n",
        cfg.QuicPeer.c_str(), startupMetrics.ConnectionCount);

    std::unique_ptr<TqAdminHttpServer> admin;
    if (!cfg.AdminListen.empty()) {
        if (!TqValidateAdminListen(cfg.AdminListen, err)) {
            std::fprintf(stderr, "tcpquic-proxy: invalid admin listen: %s\n", err.c_str());
            return 1;
        }
        const TqAdminHttpServerOptions adminOptions = TqMakeAdminOptions(cfg);
        admin.reset(new TqAdminHttpServer(cfg.AdminListen, [&runtime](const TqHttpRequest& req) {
            return runtime.HandleAdmin(req);
        }, adminOptions));
        if (!admin->Start(err)) {
            std::fprintf(stderr, "tcpquic-proxy: failed to start admin server: %s\n", err.c_str());
            return 1;
        }
        TqPrintAdminStarted(*admin, adminOptions);
    }

    TqWaitForInterrupt();
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
    out.QuicPaths = selected->QuicPaths;
    out.SocksListen = selected->SocksListen.empty() ? cfg.SocksListen : selected->SocksListen;
    out.HttpListen = selected->HttpListen.empty() ? cfg.HttpListen : selected->HttpListen;
    out.QuicConnections = selected->QuicConnections == 0 ? cfg.QuicConnections : selected->QuicConnections;
    out.Compress = selected->Compress.empty() ? cfg.Compress : selected->Compress;
    return true;
}

int RunClient(const TqConfig& cfg) {
    const std::string configPath = !cfg.ConfigPath.empty() ? cfg.ConfigPath : cfg.ClientConfigPath;
    if (!configPath.empty()) {
        const std::string path = TqAbsolutePathString(configPath);
        std::fprintf(stderr, "tcpquic-proxy: client config file %s\n", path.c_str());
    }

    if (cfg.Router.Peers.empty() && !cfg.QuicPeer.empty()) {
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
    TqRouterRuntime runtime(&adapter, cfg);
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
        const TqAdminHttpServerOptions adminOptions = TqMakeAdminOptions(cfg);
        admin.reset(new TqAdminHttpServer(cfg.AdminListen, [&runtime](const TqHttpRequest& req) {
            return runtime.HandleAdmin(req);
        }, adminOptions));
        if (!admin->Start(err)) {
            std::fprintf(stderr, "tcpquic-proxy: failed to start admin server: %s\n", err.c_str());
            return 1;
        }
        TqPrintAdminStarted(*admin, adminOptions);
    }

    TqWaitForInterrupt();
    return 0;
}

int RunServer(const TqConfig& cfg) {
    if (!cfg.ConfigPath.empty()) {
        const std::string path = TqAbsolutePathString(cfg.ConfigPath);
        std::fprintf(stderr, "tcpquic-proxy: server config file %s\n", path.c_str());
    }

    TqAcl acl;
    acl.AllowCidrs = cfg.AllowTargets;
    acl.DenyCidrs = cfg.DenyTargets;
    TqServerRuntimeConfigState runtimeConfigState(cfg);
    std::unique_ptr<TqRuntimeConfigFileStore> runtimeConfigStore;
    if (!cfg.ConfigPath.empty()) {
        runtimeConfigStore.reset(new TqRuntimeConfigFileStore(cfg.ConfigPath));
    }

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
        TqServerConnectionStreamStarted(conn);
        const uint32_t serverConnId = TqLookupServerConnectionId(conn);
        TqHandleServerIncomingStream(conn, stream, acl, cfg, speed.get(), [metrics, serverConnId]() {
            TqServerConnectionStreamFinishedById(serverConnId);
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
    {
        std::lock_guard<std::mutex> guard(metrics->Lock);
        metrics->ResolvedListens = quic.ResolvedListenAddresses();
    }

    std::unique_ptr<TqAdminHttpServer> admin;
    if (!cfg.AdminListen.empty()) {
        std::string err;
        if (!TqValidateAdminListen(cfg.AdminListen, err)) {
            std::fprintf(stderr, "tcpquic-proxy: invalid admin listen: %s\n", err.c_str());
            return 1;
        }
        const TqAdminHttpServerOptions adminOptions = TqMakeAdminOptions(cfg);
        admin.reset(new TqAdminHttpServer(
            cfg.AdminListen,
            [metrics, started, &serverDial, &runtimeConfigState, &runtimeConfigStore](const TqHttpRequest& req) {
            const uint64_t uptimeSeconds = static_cast<uint64_t>(
                std::chrono::duration_cast<std::chrono::seconds>(std::chrono::steady_clock::now() - started).count());
            metrics->TcpDialing.store(serverDial.PendingCount());
            if (req.Method == "GET" && req.Path == "/health") {
                return TqJsonResponse(200, TqServerHealthJson(*metrics, uptimeSeconds));
            }
            if (req.Method == "GET" && req.Path == "/metrics") {
                return TqJsonResponse(200, TqServerMetricsJson(*metrics, uptimeSeconds));
            }
            return TqHandleServerAdmin(
                req,
                *metrics,
                uptimeSeconds,
                runtimeConfigState,
                runtimeConfigStore.get(),
                [&serverDial](const TqAcl& nextAcl) {
                    serverDial.UpdateAcl(nextAcl);
                    return true;
                });
        },
            adminOptions));
        if (!admin->Start(err)) {
            std::fprintf(stderr, "tcpquic-proxy: failed to start admin server: %s\n", err.c_str());
            return 1;
        }
        TqPrintAdminStarted(*admin, adminOptions);
    }

    std::fprintf(stderr, "tcpquic-proxy: QUIC server listening on %s\n", cfg.QuicListen.c_str());
    std::thread quicThread([&quic] { quic.Run(); });
    TqWaitForInterrupt();
    quic.Stop();
    TqFinalStopServerConnectionCleanup();
    if (quicThread.joinable()) {
        quicThread.join();
    }
    speed->StopAll();
    return 0;
}

} // namespace

int main(int argc, char** argv) {
    TqAdminAuth::SetRuntimeBinaryName(argc > 0 ? argv[0] : nullptr);
    TqInstallCrashDumpHandler();

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
    if (cfg.ShowUsage) {
        TqPrintUsage(stdout);
        return 0;
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
        if (!TqTraceInit(cfg.Mode, cfg.TraceIntervalSec, cfg.TraceLogLevel)) {
            std::fprintf(stderr, "tcpquic-proxy: warning: trace log init failed\n");
        } else {
            const char* traceLevelName =
                cfg.TraceLogLevel == TqConfig::TraceLevel::Debug ? "debug" : "info";
            std::fprintf(stderr,
                "tcpquic-proxy: trace enabled (level=%s, interval=%us, log under <exe>/log/)\n",
                traceLevelName,
                cfg.TraceIntervalSec);
        }
    }
    if (cfg.DiagStats) {
        if (TqDiagStatsInit(cfg.DiagStatsIntervalSec)) {
            std::fprintf(stderr,
                "tcpquic-proxy: diag stats enabled (interval=%us, stderr)\n",
                cfg.DiagStatsIntervalSec);
        }
    }

    TqInstallInterruptHandler();

    if (cfg.Mode == TqMode::Client) {
        return RunClient(cfg);
    }

    return RunServer(cfg);
}
