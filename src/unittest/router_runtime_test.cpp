#include "admin_config.h"
#include "client_peer_runtime.h"
#include "router_runtime.h"
#include "server_metrics.h"
#include "trace.h"
#include "tunnel_registry.h"
#if defined(__APPLE__)
#include "darwin_relay_worker.h"
#include "tuning.h"
#endif

#include <nlohmann/json.hpp>

#include <atomic>
#include <filesystem>
#include <fstream>
#include <limits>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

bool g_router_trace_enabled = false;
bool g_router_diag_stats_enabled = false;
unsigned g_router_trace_init_count = 0;
unsigned g_router_trace_shutdown_count = 0;
unsigned g_router_diag_stats_init_count = 0;
unsigned g_router_diag_stats_shutdown_count = 0;
uint32_t g_router_trace_interval_sec = 0;
uint32_t g_router_diag_stats_interval_sec = 0;
TqConfig::TraceLevel g_router_trace_level = TqConfig::TraceLevel::Info;

void ResetTraceRuntimeStub() {
    g_router_trace_enabled = false;
    g_router_diag_stats_enabled = false;
    g_router_trace_init_count = 0;
    g_router_trace_shutdown_count = 0;
    g_router_diag_stats_init_count = 0;
    g_router_diag_stats_shutdown_count = 0;
    g_router_trace_interval_sec = 0;
    g_router_diag_stats_interval_sec = 0;
    g_router_trace_level = TqConfig::TraceLevel::Info;
}

bool TqTraceInit(TqMode, uint32_t intervalSec, TqConfig::TraceLevel level) {
    g_router_trace_enabled = true;
    g_router_trace_interval_sec = intervalSec;
    g_router_trace_level = level;
    ++g_router_trace_init_count;
    return true;
}

void TqTraceShutdown() {
    g_router_trace_enabled = false;
    ++g_router_trace_shutdown_count;
}

bool TqTraceEnabled() {
    return g_router_trace_enabled;
}

bool TqDiagStatsInit(uint32_t intervalSec) {
    g_router_diag_stats_enabled = true;
    g_router_diag_stats_interval_sec = intervalSec;
    ++g_router_diag_stats_init_count;
    return true;
}

void TqDiagStatsShutdown() {
    g_router_diag_stats_enabled = false;
    ++g_router_diag_stats_shutdown_count;
}

bool TqDiagStatsEnabled() {
    return g_router_diag_stats_enabled;
}

bool TqApplyDiagnosticsRuntime(const TqConfig& cfg) {
    if (cfg.Trace) {
        if (TqTraceEnabled()) {
            TqTraceShutdown();
        }
        if (!TqTraceInit(cfg.Mode, cfg.TraceIntervalSec, cfg.TraceLogLevel)) {
            return false;
        }
    } else if (TqTraceEnabled()) {
        TqTraceShutdown();
    }
    if (cfg.DiagStats) {
        if (!TqDiagStatsInit(cfg.DiagStatsIntervalSec)) {
            return false;
        }
    } else if (TqDiagStatsEnabled()) {
        TqDiagStatsShutdown();
    }
    return true;
}

std::atomic<unsigned> g_adminTunnelAbortCalls{0};

void AdminTunnelAbort(void*) {
    g_adminTunnelAbortCalls.fetch_add(1);
}

static std::string TempClientConfigPath(const std::string& name) {
    static unsigned counter = 0;
    std::ostringstream pathName;
    pathName << name << "-" << counter++ << ".json";
    return (std::filesystem::temp_directory_path() / pathName.str()).string();
}

static std::string ReadTextFile(const std::string& path) {
    std::ifstream file(path);
    if (!file) {
        return {};
    }
    return std::string((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
}

void TqTraceLogLine(const char* line) {
    (void)line;
}

void TqTraceRelayStreamEvent(
    const char* backend,
    uint32_t workerIndex,
    uint64_t relayId,
    const char* streamEvent,
    uint64_t errorCode,
    uint32_t status,
    uint64_t absoluteOffset,
    uint64_t totalBufferLength,
    uint32_t bufferCount,
    uint32_t receiveFlags,
    bool fin,
    const TqTraceLinuxRelayStreamState& state) {
    (void)backend;
    (void)workerIndex;
    (void)relayId;
    (void)streamEvent;
    (void)errorCode;
    (void)status;
    (void)absoluteOffset;
    (void)totalBufferLength;
    (void)bufferCount;
    (void)receiveFlags;
    (void)fin;
    (void)state;
}

void TqTraceRelayStopCondition(
    const char* backend,
    uint32_t workerIndex,
    const char* trigger,
    const TqTraceLinuxRelayStreamState& state) {
    (void)backend;
    (void)workerIndex;
    (void)trigger;
    (void)state;
}

void TqTraceRelayHalfClose(
    const char* backend,
    uint32_t workerIndex,
    const char* trigger,
    const TqTraceLinuxRelayStreamState& state,
    const char* blockers,
    bool tcpReadArmed,
    bool tcpReadPausedByQuicBacklog) {
    (void)backend;
    (void)workerIndex;
    (void)trigger;
    (void)state;
    (void)blockers;
    (void)tcpReadArmed;
    (void)tcpReadPausedByQuicBacklog;
}

void TqTraceRelayReceiveViewEvent(
    const char* backend,
    uint32_t workerIndex,
    const char* stage,
    uintptr_t viewId,
    uint64_t value,
    uint64_t totalLength,
    uint64_t completedLength,
    uint64_t accountedLength,
    uint64_t pendingCompleteBytes,
    size_t sliceIndex,
    size_t sliceCount,
    size_t sliceOffset,
    bool fin,
    bool drained,
    const TqTraceLinuxRelayStreamState& state) {
    (void)backend;
    (void)workerIndex;
    (void)stage;
    (void)viewId;
    (void)value;
    (void)totalLength;
    (void)completedLength;
    (void)accountedLength;
    (void)pendingCompleteBytes;
    (void)sliceIndex;
    (void)sliceCount;
    (void)sliceOffset;
    (void)fin;
    (void)drained;
    (void)state;
}

void TqTraceRelayBackpressureEvent(
    const char* backend,
    uint32_t workerIndex,
    uint64_t relayId,
    const char* action,
    const char* reason,
    uint64_t outstandingQuicSendBytes,
    uint64_t pauseThreshold,
    uint64_t resumeThreshold,
    uint64_t readAheadBytes) {
    (void)backend;
    (void)workerIndex;
    (void)relayId;
    (void)action;
    (void)reason;
    (void)outstandingQuicSendBytes;
    (void)pauseThreshold;
    (void)resumeThreshold;
    (void)readAheadBytes;
}

void TqTraceRelayStreamShutdown(
    const char* backend,
    const TqTraceLinuxRelayStreamState& state) {
    (void)backend;
    (void)state;
}

void TqTraceRelayUnregister(
    const char* backend,
    const TqTraceLinuxRelayStreamState& state) {
    (void)backend;
    (void)state;
}

void TqTraceRelayFatalError(
    const char* backend,
    uint32_t workerIndex,
    const char* reason,
    uint64_t relayId,
    uint64_t socketOrFd,
    uint64_t pendingQuicReceiveBytes,
    uint64_t pendingQuicReceiveQueue,
    uint64_t pendingQuicSends,
    uint64_t inflightQuicSends,
    uint64_t inflightTcpSends) {
    (void)backend;
    (void)workerIndex;
    (void)reason;
    (void)relayId;
    (void)socketOrFd;
    (void)pendingQuicReceiveBytes;
    (void)pendingQuicReceiveQueue;
    (void)pendingQuicSends;
    (void)inflightQuicSends;
    (void)inflightTcpSends;
}

class FakeAdapter : public TqPeerRuntimeAdapter {
public:
    std::vector<std::string> Started;
    std::vector<std::string> Stopped;
    std::vector<std::string> Drained;
    std::vector<uint32_t> DrainGraces;
    std::vector<std::string> AbortAll;
    TqPeerConfig LastStartedPeer;
    uint32_t FailStarts{0};
    uint32_t ConnectedConnections{0};
    std::vector<TqConnectionSnapshot> Connections;
    std::vector<uint32_t> DesiredConnectionCounts;
    std::vector<std::string> ReconnectedConnections;
    std::vector<std::string> DeletedConnections;
    std::vector<std::string> AbortedConnectionTunnels;
    uint64_t ActiveStreams{0};
    uint64_t TotalStreams{0};
    uint64_t RetryScheduledTotal{11};
    uint64_t RetryExecutedTotal{7};
    uint64_t RetryStaleDroppedTotal{3};
    uint64_t RetryScheduleFailedTotal{1};

    bool StartPeer(const TqPeerConfig& peer, std::string& err) override {
        Started.push_back(peer.PeerId);
        LastStartedPeer = peer;
        if (FailStarts != 0) {
            --FailStarts;
            err = "start failed";
            return false;
        }
        return true;
    }

    void StopAccepting(const std::string& peerId) override {
        Stopped.push_back(peerId);
    }

    void DrainPeer(const std::string& peerId, uint32_t graceSeconds) override {
        Drained.push_back(peerId);
        DrainGraces.push_back(graceSeconds);
    }

    void AbortPeerTunnels(const std::string& peerId) override {
        AbortAll.push_back(peerId);
    }

    bool SnapshotPeerMetrics(const std::string& peerId, TqPeerMetrics& out) override {
        (void)peerId;
        out.ConnectionCount = 4;
        out.ConnectedConnections = ConnectedConnections;
        out.ActiveStreams = ActiveStreams;
        out.TotalStreams = TotalStreams;
        out.RetryScheduledTotal = RetryScheduledTotal;
        out.RetryExecutedTotal = RetryExecutedTotal;
        out.RetryStaleDroppedTotal = RetryStaleDroppedTotal;
        out.RetryScheduleFailedTotal = RetryScheduleFailedTotal;
        out.State = ConnectedConnections > 0 ? "healthy" : "connecting";
        return true;
    }

    TqClientIngressDiagnostics SnapshotIngressDiagnostics() override {
        return TqClientIngressDiagnostics{17, 41, 1000, 250, 500, 2500};
    }

    std::vector<TqConnectionSnapshot> SnapshotConnections(const std::string& peerId) override {
        (void)peerId;
        return Connections;
    }

    bool SetDesiredConnectionCount(const std::string& peerId, uint32_t desired, std::string&) override {
        (void)peerId;
        DesiredConnectionCounts.push_back(desired);
        Connections.resize(desired);
        for (uint32_t i = 0; i < desired; ++i) {
            Connections[i].ConnectionId = "conn-" + std::to_string(i);
            Connections[i].SlotIndex = i;
            Connections[i].State = "connecting";
        }
        return true;
    }

    bool ReconnectConnection(const std::string& peerId, const std::string& connectionId, std::string&) override {
        (void)peerId;
        ReconnectedConnections.push_back(connectionId);
        return true;
    }

    bool StopHighestConnection(const std::string& peerId, const std::string& connectionId, std::string& err) override {
        (void)peerId;
        if (Connections.empty() || Connections.back().ConnectionId != connectionId) {
            err = "only highest connection slot can be removed";
            return false;
        }
        DeletedConnections.push_back(connectionId);
        Connections.pop_back();
        return true;
    }

    bool AbortConnectionTunnels(const std::string& peerId, const std::string& connectionId, std::string&) override {
        (void)peerId;
        AbortedConnectionTunnels.push_back(connectionId);
        return true;
    }
};

static TqPeerConfig Peer(const std::string& id, const std::string& listen, bool enabled = true) {
    TqPeerConfig p;
    p.PeerId = id;
    p.QuicPeer = "127.0.0.1:14444";
    p.SocksListen = listen;
    p.HttpListen = "";
    p.QuicConnections = 2;
    p.Compress = "auto";
    p.Enabled = enabled;
    return p;
}

static TqPortForwardConfig Forward(const std::string& listen, const std::string& host, uint16_t port) {
    TqPortForwardConfig forward;
    forward.Listen = listen;
    forward.TargetHost = host;
    forward.TargetPort = port;
    return forward;
}

static TqQuicPathConfig QuicPath(const std::string& name, const std::string& local, const std::string& peer, uint32_t connections) {
    TqQuicPathConfig path;
    path.Name = name;
    path.LocalAddress = local;
    path.Peer = peer;
    path.Connections = connections;
    return path;
}

static std::string JsonBody(const std::string& response) {
    const size_t body = response.find("\r\n\r\n");
    return body == std::string::npos ? std::string{} : response.substr(body + 4);
}

static bool ParseJson(const std::string& text, nlohmann::json& out) {
    try {
        out = nlohmann::json::parse(text);
        return true;
    } catch (const nlohmann::json::exception&) {
        return false;
    }
}

static bool HasRetryTotals(const nlohmann::json& peer, uint64_t expected) {
    return peer.at("retry_scheduled_total").get<uint64_t>() == expected &&
        peer.at("retry_executed_total").get<uint64_t>() == expected &&
        peer.at("retry_stale_dropped_total").get<uint64_t>() == expected &&
        peer.at("retry_schedule_failed_total").get<uint64_t>() == expected;
}

static bool HasPortForward(const nlohmann::json& forwards, const char* listen, const char* target) {
    if (!forwards.is_array()) {
        return false;
    }
    for (const auto& forward : forwards) {
        if (forward["listen"] == listen && forward["target"] == target) {
            return true;
        }
    }
    return false;
}

static TqHttpRequest Request(const std::string& method, const std::string& path, const std::string& body = "") {
    TqHttpRequest req;
    req.Method = method;
    req.Path = path;
    req.Body = body;
    return req;
}

int main() {
    {
        TqConfig cfg;
        cfg.Mode = TqMode::Client;
        cfg.ConfigPath = "client.json";
        cfg.AdminListen = "127.0.0.1:18080";
        cfg.AdminTokenFile = "/tmp/token.json";
        cfg.AdminThreads = 2;
        cfg.QuicCa = "certs/ca.crt";
        cfg.QuicProfile = TqQuicProfile::MaxThroughput;
        cfg.QuicDisable1RttEncryption = true;
        cfg.QuicConnections = 4;
        cfg.QuicConnectionStreamCount = 1024;
        cfg.QuicKeepAliveIntervalMs = 5000;
        cfg.Compress = "off";
        cfg.CompressLevel = 1;
        cfg.ClientName = "office-a";
        TqPeerConfig peer;
        peer.PeerId = "agent-a";
        peer.QuicPeer = "127.0.0.1:14444";
        peer.SocksListen = "127.0.0.1:11001";
        peer.QuicConnections = 4;
        peer.Enabled = true;
        cfg.Router.Peers.push_back(peer);

        nlohmann::json json;
        if (!ParseJson(TqRuntimeConfigJson(cfg, false), json)) return 930;
        if (json["role"] != "client") return 931;
        if (json["client_name"] != "office-a") return 924;
        if (json["router"]["peers"][0]["id"] != "agent-a") return 932;
        if (json["router"]["peers"][0].contains("client_name")) return 925;
        if (json["router"]["peers"][0]["proto_peer"] != "127.0.0.1:14444") return 933;
        if (json["router"]["peers"][0]["proto_connections"] != 4) return 934;
        if (json.dump().find("\"token\"") != std::string::npos) return 929;
    }
    {
        nlohmann::json json;
        if (!ParseJson(TqStructuredErrorJson("bad\"code\\x", "line\nbreak"), json)) return 935;
        if (json["error"]["code"] != "bad\"code\\x") return 936;
        if (json["error"]["message"] != "line\nbreak") return 937;
    }
    {
        TqConfig cfg;
        cfg.QuicKey = "raw-secret-key.pem";
        nlohmann::json json;
        if (!ParseJson(TqRuntimeConfigJson(cfg, true), json)) return 938;
        if (json["tls"]["key"] != "***") return 939;
        if (json.dump().find("raw-secret-key.pem") != std::string::npos) return 940;
    }
    {
        TqConfig cfg;
        cfg.Router.ProxyAuth.push_back({"alice", "raw-proxy-password"});
        cfg.SocksListen = "127.0.0.1:11080";
        cfg.HttpListen = "127.0.0.1:18080";
        cfg.QuicPeer = "127.0.0.1:14444";
        cfg.QuicConnections = 3;
        cfg.QuicConnectionStreamCount = 2048;
        cfg.QuicKeepAliveIntervalMs = 7000;
        cfg.SpeedTestMode = TqSpeedTestMode::Download;
        cfg.SpeedTestDurationSec = 11;
        cfg.HandshakeThreads = 6;
        cfg.Compress = "zstd";
        cfg.CompressLevel = 3;
        cfg.ClientName = "edge-public";
        nlohmann::json json;
        if (!ParseJson(TqClientPublicConfigJson(cfg), json)) return 990;
        if (json["client_name"] != "edge-public") return 988;
        if (json["proxy_auth"][0]["password"] != "***") return 939;
        if (json.dump().find("raw-proxy-password") != std::string::npos) return 952;
        if (json["socks_listen"] != "127.0.0.1:11080") return 991;
        if (json["http_listen"] != "127.0.0.1:18080") return 992;
        if (json["proto"]["peer"] != "127.0.0.1:14444") return 993;
        if (json["proto"]["connections"] != 3) return 994;
        if (json["proto"]["connection_stream_count"] != 2048) return 995;
        if (json["proto"]["keep_alive_interval_ms"] != 7000) return 996;
        if (json["speed_test"]["mode"] != "download") return 997;
        if (json["speed_test"]["duration_sec"] != 11) return 998;
        if (json["handshake_threads"] != 6) return 999;
        if (json["compression"]["mode"] != "zstd") return 986;
        if (json["compression"]["level"] != 3) return 987;
    }
    {
        TqConfig cfg;
        TqPeerConfig peer;
        peer.PeerId = "agent-public";
        peer.QuicPeer = "127.0.0.1:24444";
        peer.QuicConnections = 8;
        nlohmann::json json;
        if (!ParseJson(TqPeerPublicConfigJson(cfg, peer), json)) return 953;
        if (json["id"] != "agent-public") return 954;
        if (json.contains("client_name")) return 949;
        if (json["proto_peer"] != "127.0.0.1:24444") return 955;
        if (json["proto_connections"] != 8) return 956;
    }
    {
        TqConfig cfg;
        const std::vector<std::string> resolvedListens = {"0.0.0.0:443", "[::]:443"};
        nlohmann::json json;
        if (!ParseJson(TqServerRuntimeConfigJson(cfg, resolvedListens, false), json)) return 957;
        if (json["role"] != "server") return 958;
        if (json["resolved_listens"] != resolvedListens) return 959;
        if (json["quic"].value("encryption_policy", "") != "client-choice") return 964;
        if (json["quic"].contains("disable_1rtt_encryption")) return 965;
    }
    {
        TqConfig cfg;
        cfg.Trace = true;
        cfg.TraceIntervalSec = 7;
        cfg.TraceLogLevel = TqConfig::TraceLevel::Debug;
        cfg.DiagStats = true;
        cfg.DiagStatsIntervalSec = 9;
        nlohmann::json json;
        if (!ParseJson(TqDiagnosticsJson(cfg), json)) return 960;
        if (json["trace"] != true) return 961;
        if (json["trace_interval_sec"] != 7) return 962;
        if (json["trace_level"] != "debug") return 963;
        if (json["diag_stats"] != true) return 981;
        if (json["diag_stats_interval_sec"] != 9) return 982;
    }
    {
        TqConfig cfg;
        cfg.Mode = TqMode::Client;
        cfg.AdminListen = "127.0.0.1:18080";
        cfg.QuicConnections = 2;
        TqPeerConfig peer;
        peer.PeerId = "agent-public";
        peer.QuicPeer = "127.0.0.1:14460";
        peer.SocksListen = "127.0.0.1:11060";
        cfg.Router.Peers.push_back(peer);
        TqRouterRuntime runtime(nullptr, cfg);

        std::string runtimeConfig = runtime.HandleAdmin(Request("GET", "/runtime/config", ""));
        if (runtimeConfig.find("HTTP/1.1 200") == std::string::npos) return 983;
        if (runtimeConfig.find("\"proto_peer\":\"127.0.0.1:14460\"") == std::string::npos) return 984;
        if (runtimeConfig.find("\"disable_1rtt_encryption\"") == std::string::npos) return 966;
        if (runtimeConfig.find("\"encryption_policy\"") != std::string::npos) return 967;

        std::string patchRuntime = runtime.HandleAdmin(Request(
            "PATCH",
            "/runtime/config",
            "{\"compression\":{\"mode\":\"zstd\",\"level\":2},\"tuning\":{\"max_memory_mb\":512}}"));
        if (patchRuntime.find("HTTP/1.1 200 OK") == std::string::npos) return 9916;
        if (patchRuntime.find("\"compress\":\"zstd\"") == std::string::npos) return 9917;
        if (patchRuntime.find("\"compress_level\":2") == std::string::npos) return 9918;
        if (patchRuntime.find("\"max_memory_mb\":512") == std::string::npos) return 9919;

        std::string unsupportedRuntime = runtime.HandleAdmin(Request("PATCH", "/runtime/config", "{\"admin\":{\"listen\":\"127.0.0.1:18082\"}}"));
        if (unsupportedRuntime.find("HTTP/1.1 503 Service Unavailable") == std::string::npos) return 9920;
        if (unsupportedRuntime.find("\"code\":\"not_supported\"") == std::string::npos) return 9921;
        if (unsupportedRuntime.find("admin.listen") == std::string::npos) return 9922;

        std::string invalidRuntime = runtime.HandleAdmin(Request("PATCH", "/runtime/config", "{\"compression\":{\"level\":0}}"));
        if (invalidRuntime.find("HTTP/1.1 400") == std::string::npos) return 9923;
        std::string afterInvalidRuntime = runtime.HandleAdmin(Request("GET", "/runtime/config", ""));
        if (afterInvalidRuntime.find("\"compress\":\"zstd\"") == std::string::npos) return 9924;
        if (afterInvalidRuntime.find("\"compress_level\":2") == std::string::npos) return 9925;

        std::string clientConfig = runtime.HandleAdmin(Request("GET", "/client/config", ""));
        if (clientConfig.find("\"peers\"") == std::string::npos) return 985;
        if (clientConfig.find("\"connection_stream_count\"") == std::string::npos) return 9901;
        if (clientConfig.find("\"keep_alive_interval_ms\"") == std::string::npos) return 9902;
        if (clientConfig.find("\"handshake_threads\"") == std::string::npos) return 9903;
        if (clientConfig.find("\"speed_test\"") == std::string::npos) return 9904;

        std::string peerConfig = runtime.HandleAdmin(Request("GET", "/peers/agent-public/config", ""));
        if (peerConfig.find("HTTP/1.1 200") == std::string::npos) return 986;
        if (peerConfig.find("\"id\":\"agent-public\"") == std::string::npos) return 987;
        if (peerConfig.find("\"proxy_auth\"") != std::string::npos) return 9905;

        std::string missingPeerConfig = runtime.HandleAdmin(Request("GET", "/peers/missing/config", ""));
        if (missingPeerConfig.find("HTTP/1.1 404") == std::string::npos) return 989;

        std::string diag = runtime.HandleAdmin(Request("GET", "/diagnostics", ""));
        if (diag.find("\"diag_stats\"") == std::string::npos) return 988;

        ResetTraceRuntimeStub();
        std::string patchDiag = runtime.HandleAdmin(Request(
            "PATCH",
            "/diagnostics",
            "{\"trace\":false,\"trace_interval_sec\":31,\"trace_level\":\"debug\",\"diag_stats\":true,\"diag_stats_interval_sec\":6}"));
        if (patchDiag.find("HTTP/1.1 200 OK") == std::string::npos) return 9906;
        if (patchDiag.find("\"trace\":false") == std::string::npos) return 9907;
        if (patchDiag.find("\"trace_interval_sec\":31") == std::string::npos) return 9908;
        if (patchDiag.find("\"trace_level\":\"debug\"") == std::string::npos) return 9909;
        if (patchDiag.find("\"diag_stats\":true") == std::string::npos) return 9910;
        if (patchDiag.find("\"diag_stats_interval_sec\":6") == std::string::npos) return 9911;
        if (!g_router_diag_stats_enabled) return 9916;
        if (g_router_diag_stats_init_count != 1) return 9917;
        if (g_router_diag_stats_interval_sec != 6) return 9918;
        if (g_router_trace_init_count != 0) return 9919;

        std::string patchTrace = runtime.HandleAdmin(Request(
            "PATCH",
            "/diagnostics",
            "{\"trace\":true,\"trace_interval_sec\":11,\"trace_level\":\"info\",\"diag_stats\":false}"));
        if (patchTrace.find("HTTP/1.1 200 OK") == std::string::npos) return 9920;
        if (!g_router_trace_enabled) return 9921;
        if (g_router_trace_init_count != 1) return 9922;
        if (g_router_trace_interval_sec != 11) return 9923;
        if (g_router_trace_level != TqConfig::TraceLevel::Info) return 9924;
        if (g_router_diag_stats_enabled) return 9925;
        if (g_router_diag_stats_shutdown_count != 1) return 9926;

        std::string invalidDiag = runtime.HandleAdmin(Request("PATCH", "/diagnostics", "{\"trace_interval_sec\":0}"));
        if (invalidDiag.find("HTTP/1.1 400") == std::string::npos) return 9912;
        std::string afterInvalidDiag = runtime.HandleAdmin(Request("GET", "/diagnostics", ""));
        if (afterInvalidDiag.find("\"trace_interval_sec\":11") == std::string::npos) return 9913;

        std::string invalidLevel = runtime.HandleAdmin(Request("PATCH", "/diagnostics", "{\"trace_level\":\"verbose\"}"));
        if (invalidLevel.find("HTTP/1.1 400") == std::string::npos) return 9914;

        std::string unknownDiag = runtime.HandleAdmin(Request("PATCH", "/diagnostics", "{\"unknown\":true}"));
        if (unknownDiag.find("HTTP/1.1 400") == std::string::npos) return 9915;
    }
    {
        FakeAdapter adapter;
        adapter.ConnectedConnections = 2;
        TqRouterRuntime adapterRuntime(&adapter);
        TqRouterConfig cfg;
        cfg.Peers.push_back(Peer("agent-metrics", "127.0.0.1:11013"));
        std::string err;
        if (!adapterRuntime.ApplyConfig(cfg, err)) return 106;
        std::string metrics = adapterRuntime.MetricsJson();
        if (metrics.find("\"connection_count\":4") == std::string::npos) return 107;
        if (metrics.find("\"connected_connections\":2") == std::string::npos) return 108;
        if (metrics.find("\"state\":\"healthy\"") == std::string::npos) return 109;
        if (metrics.find("\"ingress_delayed_task_queue_depth\":17") == std::string::npos) return 2511;
        if (metrics.find("\"ingress_delayed_task_queue_depth_max\":41") == std::string::npos) return 2512;
        if (metrics.find("\"ingress_reactor_timeout_overshoot_p95_us\":250") == std::string::npos) return 2513;
        if (metrics.find("\"ingress_reactor_timeout_overshoot_p99_us\":500") == std::string::npos) return 2514;
        if (metrics.find("\"ingress_reactor_timeout_overshoot_samples\":1000") == std::string::npos) return 2515;
        if (metrics.find("\"ingress_reactor_timeout_overshoot_max_us\":2500") == std::string::npos) return 2516;
    }
    {
        TqRouterRuntime runtime;
        nlohmann::json metrics;
        if (!ParseJson(runtime.MetricsJson(), metrics)) return 2518;
        if (metrics.at("ingress_delayed_task_queue_depth") != 0 ||
            metrics.at("ingress_delayed_task_queue_depth_max") != 0 ||
            metrics.at("ingress_reactor_timeout_overshoot_samples") != 0 ||
            metrics.at("ingress_reactor_timeout_overshoot_p95_us") != 0 ||
            metrics.at("ingress_reactor_timeout_overshoot_p99_us") != 0 ||
            metrics.at("ingress_reactor_timeout_overshoot_max_us") != 0) return 2519;
    }
    {
        FakeAdapter adapter;
        adapter.ActiveStreams = 2;
        adapter.TotalStreams = 5;
        adapter.ConnectedConnections = 1;
        adapter.RetryScheduledTotal = std::numeric_limits<uint64_t>::max();
        adapter.RetryExecutedTotal = std::numeric_limits<uint64_t>::max();
        adapter.RetryStaleDroppedTotal = std::numeric_limits<uint64_t>::max();
        adapter.RetryScheduleFailedTotal = std::numeric_limits<uint64_t>::max();
        TqRouterRuntime adapterRuntime(&adapter);
        TqRouterConfig cfg;
        cfg.Peers.push_back(Peer("agent-metrics-peers", "127.0.0.1:11020"));
        std::string err;
        if (!adapterRuntime.ApplyConfig(cfg, err)) return 2301;
        nlohmann::json metricsJson;
        if (!ParseJson(JsonBody(adapterRuntime.HandleAdmin(Request("GET", "/metrics", ""))), metricsJson)) return 2401;
        if (!HasRetryTotals(metricsJson.at("peers").at(0), std::numeric_limits<uint64_t>::max())) return 2402;
        TqHttpRequest list = Request("GET", "/peers", "");
        std::string listResp = adapterRuntime.HandleAdmin(list);
        const std::string listBody = JsonBody(listResp);
        if (listResp.find("HTTP/1.1 200 OK") == std::string::npos) return 2302;
        if (listBody.find("\"peer_id\":\"agent-metrics-peers\"") == std::string::npos) return 2308;
        if (listBody.find("\"active_streams\":2") == std::string::npos) return 2303;
        if (listBody.find("\"total_streams\":5") == std::string::npos) return 2304;
        nlohmann::json listJson;
        if (!ParseJson(listBody, listJson)) return 2403;
        if (!HasRetryTotals(listJson.at("peers").at(0), std::numeric_limits<uint64_t>::max())) return 2404;
        TqHttpRequest get = Request("GET", "/peers/agent-metrics-peers", "");
        std::string getResp = adapterRuntime.HandleAdmin(get);
        const std::string getBody = JsonBody(getResp);
        if (getResp.find("HTTP/1.1 200 OK") == std::string::npos) return 2305;
        if (getBody.find("\"peer_id\":\"agent-metrics-peers\"") == std::string::npos) return 2309;
        if (getBody.find("\"active_streams\":2") == std::string::npos) return 2306;
        if (getBody.find("\"total_streams\":5") == std::string::npos) return 2307;
        nlohmann::json getJson;
        if (!ParseJson(getBody, getJson)) return 2405;
        if (!HasRetryTotals(getJson, std::numeric_limits<uint64_t>::max())) return 2406;
    }
    {
        FakeAdapter adapter;
        TqRouterRuntime adapterRuntime(&adapter);
        TqRouterConfig cfg;
        TqPeerConfig disabled = Peer("agent-metrics-disabled", "127.0.0.1:11021");
        disabled.Enabled = false;
        cfg.Peers.push_back(disabled);
        std::string err;
        if (!adapterRuntime.ApplyConfig(cfg, err)) return 2407;
        nlohmann::json metricsJson;
        if (!ParseJson(adapterRuntime.MetricsJson(), metricsJson)) return 2408;
        if (!HasRetryTotals(metricsJson.at("peers").at(0), 0)) return 2409;
        if (metricsJson.at("ingress_delayed_task_queue_depth") != 17 ||
            metricsJson.at("ingress_delayed_task_queue_depth_max") != 41 ||
            metricsJson.at("ingress_reactor_timeout_overshoot_samples") != 1000 ||
            metricsJson.at("ingress_reactor_timeout_overshoot_p95_us") != 250 ||
            metricsJson.at("ingress_reactor_timeout_overshoot_p99_us") != 500 ||
            metricsJson.at("ingress_reactor_timeout_overshoot_max_us") != 2500) return 2517;
    }
    {
        FakeAdapter adapter;
        adapter.FailStarts = 1;
        TqRouterRuntime adapterRuntime(&adapter);
        TqRouterConfig cfg;
        cfg.Peers.push_back(Peer("agent-retry", "127.0.0.1:11011"));
        std::string err;
        if (!adapterRuntime.ApplyConfig(cfg, err)) return 83;
        auto failedMetrics = adapterRuntime.SnapshotMetrics();
        if (failedMetrics.Peers.size() != 1 || failedMetrics.Peers[0].State != "down") return 84;
        if (failedMetrics.Peers[0].LastError != "start failed") return 85;
        if (!adapterRuntime.ApplyConfig(cfg, err)) return 86;
        if (adapter.Started.size() != 2 || adapter.Started[1] != "agent-retry") return 87;
        auto retriedMetrics = adapterRuntime.SnapshotMetrics();
        if (retriedMetrics.Peers.size() != 1 || retriedMetrics.Peers[0].State != "connecting") return 88;
        if (!retriedMetrics.Peers[0].LastError.empty()) return 89;
    }
    {
        FakeAdapter adapter;
        TqRouterRuntime adapterRuntime(&adapter);
        TqRouterConfig cfg;
        cfg.Peers.push_back(Peer("agent-remove", "127.0.0.1:11012"));
        std::string err;
        if (!adapterRuntime.ApplyConfig(cfg, err)) return 90;
        TqRouterConfig empty;
        if (!adapterRuntime.ApplyConfig(empty, err)) return 91;
        if (adapter.Drained.size() != 1 || adapter.Drained[0] != "agent-remove") return 92;
        if (adapter.AbortAll.size() != 1 || adapter.AbortAll[0] != "agent-remove") return 93;
        if (!adapterRuntime.ApplyConfig(empty, err)) return 94;
        if (adapter.Drained.size() != 1) return 95;
        if (adapter.AbortAll.size() != 1) return 96;
    }
    {
        FakeAdapter adapter;
        TqRouterRuntime adapterRuntime(&adapter);
        TqRouterConfig cfg;
        cfg.Peers.push_back(Peer("agent-cleanup", "127.0.0.1:11014"));
        std::string err;
        if (!adapterRuntime.ApplyConfig(cfg, err)) return 110;
        cfg.Peers[0].Enabled = false;
        if (!adapterRuntime.ApplyConfig(cfg, err)) return 111;
        if (adapter.Stopped.size() != 1 || adapter.Stopped[0] != "agent-cleanup") return 112;
        if (adapter.AbortAll.size() != 1 || adapter.AbortAll[0] != "agent-cleanup") return 113;
    }
    {
        FakeAdapter adapter;
        TqRouterRuntime adapterRuntime(&adapter);
        TqRouterConfig cfg;
        cfg.Peers.push_back(Peer("agent-change", "127.0.0.1:11015"));
        std::string err;
        if (!adapterRuntime.ApplyConfig(cfg, err)) return 114;
        cfg.Peers[0].SocksListen = "127.0.0.1:11016";
        if (!adapterRuntime.ApplyConfig(cfg, err)) return 115;
        if (adapter.Stopped.size() != 1 || adapter.Stopped[0] != "agent-change") return 116;
        if (adapter.AbortAll.size() != 1 || adapter.AbortAll[0] != "agent-change") return 117;
        if (adapter.Drained.size() != 1 || adapter.Drained[0] != "agent-change") return 118;
        if (adapter.Started.size() != 2 || adapter.Started[1] != "agent-change") return 119;
    }
    {
        FakeAdapter adapter;
        TqRouterRuntime adapterRuntime(&adapter);
        TqRouterConfig cfg;
        cfg.Peers.push_back(Peer("agent-forward", ""));
        cfg.Peers[0].PortForwards.push_back(Forward("127.0.0.1:15432", "db.internal", 5432));
        std::string err;
        if (!adapterRuntime.ApplyConfig(cfg, err)) return 183;
        if (adapter.LastStartedPeer.PortForwards.size() != 1) return 184;
    }
    {
        FakeAdapter adapter;
        TqRouterRuntime adapterRuntime(&adapter);
        TqRouterConfig cfg;
        cfg.Peers.push_back(Peer("agent-forward-change", ""));
        cfg.Peers[0].PortForwards.push_back(Forward("127.0.0.1:15432", "db.internal", 5432));
        std::string err;
        if (!adapterRuntime.ApplyConfig(cfg, err)) return 185;
        if (adapter.LastStartedPeer.PortForwards.size() != 1) return 191;
        if (adapter.LastStartedPeer.PortForwards[0].Listen != "127.0.0.1:15432") return 132;
        if (adapter.LastStartedPeer.PortForwards[0].TargetHost != "db.internal") return 133;
        if (adapter.LastStartedPeer.PortForwards[0].TargetPort != 5432) return 134;
        cfg.Peers[0].PortForwards[0].TargetPort = 5433;
        if (!adapterRuntime.ApplyConfig(cfg, err)) return 186;
        if (adapter.Stopped.size() != 1 || adapter.Stopped[0] != "agent-forward-change") return 187;
        if (adapter.AbortAll.size() != 1 || adapter.AbortAll[0] != "agent-forward-change") return 188;
        if (adapter.Drained.size() != 1 || adapter.Drained[0] != "agent-forward-change") return 189;
        if (adapter.Started.size() != 2 || adapter.Started[1] != "agent-forward-change") return 190;
    }
    {
        FakeAdapter adapter;
        TqRouterRuntime adapterRuntime(&adapter);
        TqRouterConfig cfg;
        cfg.Peers.push_back(Peer("agent-path-change", "127.0.0.1:11017"));
        cfg.Peers[0].QuicPaths.push_back(QuicPath("cmcc", "10.0.0.2", "36.1.1.10:443", 2));
        std::string err;
        if (!adapterRuntime.ApplyConfig(cfg, err)) return 940;
        if (adapter.LastStartedPeer.QuicPaths.size() != 1) return 941;
        if (adapter.LastStartedPeer.QuicPaths[0].LocalAddress != "10.0.0.2") return 942;

        cfg.Peers[0].QuicPaths[0].LocalAddress = "10.0.0.3";
        if (!adapterRuntime.ApplyConfig(cfg, err)) return 943;
        if (adapter.Started.size() != 2 || adapter.LastStartedPeer.QuicPaths[0].LocalAddress != "10.0.0.3") return 944;
        if (adapter.Stopped.size() != 1 || adapter.Stopped.back() != "agent-path-change") return 945;

        cfg.Peers[0].QuicPaths[0].Peer = "59.1.1.10:443";
        if (!adapterRuntime.ApplyConfig(cfg, err)) return 946;
        if (adapter.Started.size() != 3 || adapter.LastStartedPeer.QuicPaths[0].Peer != "59.1.1.10:443") return 947;
        if (adapter.Stopped.size() != 2 || adapter.Stopped.back() != "agent-path-change") return 948;

        cfg.Peers[0].QuicPaths[0].Connections = 3;
        if (!adapterRuntime.ApplyConfig(cfg, err)) return 949;
        if (adapter.Started.size() != 4 || adapter.LastStartedPeer.QuicPaths[0].Connections != 3) return 950;
        if (adapter.Stopped.size() != 3 || adapter.Stopped.back() != "agent-path-change") return 951;
    }
    {
        TqRouterRuntime runtime;
        TqRouterConfig cfg;
        cfg.Peers.push_back(Peer("agent-forward-json", ""));
        cfg.Peers[0].PortForwards.push_back(Forward("127.0.0.1:15432", "db.internal", 5432));
        std::string err;
        if (!runtime.ApplyConfig(cfg, err)) return 191;
        nlohmann::json configJson;
        if (!ParseJson(runtime.ConfigJson(), configJson)) return 192;
        if (!HasPortForward(configJson["peers"][0]["port_forwards"], "127.0.0.1:15432", "db.internal:5432")) return 193;
        nlohmann::json metricsJson;
        if (!ParseJson(runtime.MetricsJson(), metricsJson)) return 194;
        if (!HasPortForward(metricsJson["peers"][0]["port_forwards"], "127.0.0.1:15432", "db.internal:5432")) return 195;
    }
    {
        FakeAdapter adapter;
        TqRouterRuntime adapterRuntime(&adapter);
        TqRouterConfig cfg;
        cfg.Peers.push_back(Peer("agent-a", "127.0.0.1:11001"));
        std::string err;
        if (!adapterRuntime.ApplyConfig(cfg, err)) return 75;
        if (adapter.Started.size() != 1 || adapter.Started[0] != "agent-a") return 76;

        cfg.Peers.push_back(Peer("agent-b", "127.0.0.1:11002"));
        if (!adapterRuntime.ApplyConfig(cfg, err)) return 77;
        if (adapter.Started.size() != 2 || adapter.Started[1] != "agent-b") return 78;

        cfg.Peers[1].Enabled = false;
        if (!adapterRuntime.ApplyConfig(cfg, err)) return 79;
        if (adapter.Stopped.size() != 1 || adapter.Stopped[0] != "agent-b") return 80;

        cfg.Peers.pop_back();
        if (!adapterRuntime.ApplyConfig(cfg, err)) return 81;
        if (adapter.Drained.empty() || adapter.Drained.back() != "agent-b") return 82;
    }
    TqRouterRuntime runtime;
    {
        TqRouterConfig cfg;
        cfg.Peers.push_back(Peer("agent-b", "127.0.0.1:11001"));
        std::string err;
        if (!runtime.ApplyConfig(cfg, err)) return 1;
        auto metrics = runtime.SnapshotMetrics();
        if (metrics.Peers.size() != 1) return 2;
        if (metrics.Peers[0].PeerId != "agent-b") return 3;
        if (metrics.Peers[0].State != "connecting") return 4;
        if (!metrics.Peers[0].Enabled) return 5;
    }
    {
        TqRouterConfig cfg;
        cfg.Peers.push_back(Peer("agent-b", "127.0.0.1:11001", false));
        std::string err;
        if (!runtime.ApplyConfig(cfg, err)) return 6;
        auto metrics = runtime.SnapshotMetrics();
        if (metrics.Peers[0].State != "disabled") return 7;
        if (metrics.Peers[0].Enabled) return 8;
    }
    {
        TqRouterConfig cfg;
        cfg.Peers.push_back(Peer("agent-c", "127.0.0.1:11002"));
        std::string err;
        if (!runtime.ApplyConfig(cfg, err)) return 9;
        auto metrics = runtime.SnapshotMetrics();
        if (metrics.Peers.size() != 2) return 10;
        bool sawDraining = false;
        bool sawAgentC = false;
        for (const auto& peer : metrics.Peers) {
            if (peer.PeerId == "agent-b" && peer.State == "draining") sawDraining = true;
            if (peer.PeerId == "agent-c" && peer.State == "connecting") sawAgentC = true;
        }
        if (!sawDraining || !sawAgentC) return 11;
    }
    {
        TqRouterConfig cfg;
        cfg.Peers.push_back(Peer("agent-d", "127.0.0.1:11003"));
        cfg.Peers.push_back(Peer("agent-e", "127.0.0.1:11004"));
        std::string err;
        if (TqValidateSinglePeerStartupBridge(cfg, err)) return 50;
        if (err.find("multiple enabled peers") == std::string::npos) return 51;
        cfg.Peers[1].Enabled = false;
        if (!TqValidateSinglePeerStartupBridge(cfg, err)) return 52;
    }
    {
        TqConfig single;
        single.QuicPeer = "127.0.0.1:14444";
        single.SocksListen = "127.0.0.1:11001";
        single.HttpListen = "127.0.0.1:18001";
        single.QuicConnections = 4;
        single.Compress = "zstd";
        single.Router.ProxyAuth.push_back({"alice", "secret"});

        TqRouterConfig router = TqMakeSinglePeerRouterConfig(single);
        if (router.Peers.size() != 1) return 63;
        if (router.Peers[0].PeerId != "primary") return 64;
        if (router.Peers[0].QuicPeer != "127.0.0.1:14444") return 65;
        if (router.Peers[0].SocksListen != "127.0.0.1:11001") return 66;
        if (router.Peers[0].HttpListen != "127.0.0.1:18001") return 67;
        if (router.Peers[0].QuicConnections != 4) return 68;
        if (router.Peers[0].Compress != "zstd") return 69;
        if (router.ProxyAuth.size() != 1 || router.ProxyAuth[0].Username != "alice") return 70;
    }
    {
        nlohmann::json json;
        if (!ParseJson(runtime.ConfigJson(), json)) return 12;
        if (json["peers"][0]["peer_id"] != "agent-c") return 13;
        nlohmann::json metrics;
        if (!ParseJson(runtime.MetricsJson(), metrics)) return 14;
        if (metrics["role"] != "client") return 15;
        bool foundConnecting = false;
        for (const auto& peerJson : metrics["peers"]) {
            if (peerJson.value("state", "") == "connecting") {
                foundConnecting = true;
            }
        }
        if (!foundConnecting) return 16;
    }
    {
        TqRouterRuntime adminRuntime;
        TqHttpRequest getHealth = Request("GET", "/health", "");
        std::string health = adminRuntime.HandleAdmin(getHealth);
        if (health.find("HTTP/1.1 200 OK") == std::string::npos) return 20;
        if (health.find("\"status\":\"healthy\"") == std::string::npos) return 21;
        TqHttpRequest putConfig = Request("PUT", "/config", "{\"version\":1,\"peers\":[{\"peer_id\":\"agent-d\",\"quic_peer\":\"127.0.0.1:14446\",\"socks_listen\":\"127.0.0.1:11003\"}]}");
        std::string put = adminRuntime.HandleAdmin(putConfig);
        if (put.find("HTTP/1.1 200 OK") == std::string::npos) return 22;
        if (adminRuntime.SnapshotMetrics().Peers.size() != 1) return 23;
        TqHttpRequest getMetrics = Request("GET", "/metrics", "");
        std::string metrics = adminRuntime.HandleAdmin(getMetrics);
        if (metrics.find("HTTP/1.1 200 OK") == std::string::npos) return 25;
        if (metrics.find("\"peer_id\":\"agent-d\"") == std::string::npos) return 26;
        TqHttpRequest getConfig = Request("GET", "/config", "");
        std::string config = adminRuntime.HandleAdmin(getConfig);
        if (config.find("HTTP/1.1 200 OK") == std::string::npos) return 27;
        if (config.find("\"peer_id\":\"agent-d\"") == std::string::npos) return 28;
        if (config.find("quic_reconnect_interval_ms") != std::string::npos) return 259;
        TqHttpRequest memoryDump = Request("POST", "/memory/allocator:dump", "");
        std::string memoryDumpResp = adminRuntime.HandleAdmin(memoryDump);
        if (memoryDumpResp.find("HTTP/1.1 200 OK") == std::string::npos) return 318;
        if (memoryDumpResp.find("\"status\":\"dumped\"") == std::string::npos) return 319;
        if (memoryDumpResp.find("\"allocator\":\"mimalloc\"") == std::string::npos) return 320;
        TqHttpRequest putRoundTrip = Request("PUT", "/config", JsonBody(config));
        std::string roundTrip = adminRuntime.HandleAdmin(putRoundTrip);
        if (roundTrip.find("HTTP/1.1 200 OK") == std::string::npos) return 74;
        TqHttpRequest removedReconnect = Request("PUT", "/config", "{\"version\":1,\"peers\":[{\"peer_id\":\"agent-reconnect\",\"quic_peer\":\"127.0.0.1:14449\",\"socks_listen\":\"127.0.0.1:11006\",\"quic_reconnect_interval_ms\":5000}]}");
        std::string removedReconnectResp = adminRuntime.HandleAdmin(removedReconnect);
        if (removedReconnectResp.find("HTTP/1.1 400") == std::string::npos) return 97;
        if (removedReconnectResp.find("quic_reconnect_interval_ms") == std::string::npos) return 98;
        TqHttpRequest disable = Request("POST", "/peers/agent-d/disable", "");
        std::string disabled = adminRuntime.HandleAdmin(disable);
        if (disabled.find("HTTP/1.1 200 OK") == std::string::npos) return 29;
        if (disabled.find("\"enabled\":false") == std::string::npos) return 30;
        TqHttpRequest enable = Request("POST", "/peers/agent-d/enable", "");
        std::string enabled = adminRuntime.HandleAdmin(enable);
        if (enabled.find("HTTP/1.1 200 OK") == std::string::npos) return 31;
        if (enabled.find("\"enabled\":true") == std::string::npos) return 32;
        TqHttpRequest unknown = Request("GET", "/unknown", "");
        std::string unknownResp = adminRuntime.HandleAdmin(unknown);
        if (unknownResp.find("HTTP/1.1 404") == std::string::npos) return 33;
        TqHttpRequest bad = Request("PUT", "/config", "{\"version\":2,\"peers\":[]}");
        std::string badResp = adminRuntime.HandleAdmin(bad);
        if (badResp.find("HTTP/1.1 400") == std::string::npos) return 24;
        TqHttpRequest zeroConnections = Request("PUT", "/config", "{\"version\":1,\"peers\":[{\"peer_id\":\"agent-zero\",\"quic_peer\":\"127.0.0.1:14447\",\"socks_listen\":\"127.0.0.1:11004\",\"quic_connections\":0}]}");
        std::string zeroResp = adminRuntime.HandleAdmin(zeroConnections);
        if (zeroResp.find("HTTP/1.1 400") == std::string::npos) return 49;
        TqHttpRequest leadingZeroConnections = Request("PUT", "/config", "{\"version\":1,\"peers\":[{\"peer_id\":\"agent-leading-zero\",\"quic_peer\":\"127.0.0.1:14448\",\"socks_listen\":\"127.0.0.1:11005\",\"quic_connections\":004}]}");
        std::string leadingZeroResp = adminRuntime.HandleAdmin(leadingZeroConnections);
        if (leadingZeroResp.find("HTTP/1.1 400") == std::string::npos) return 73;
    }
    {
        const std::string path = TempClientConfigPath("tcpquic-admin-persist-config");
        std::filesystem::remove(path);
        TqConfig runtimeConfig;
        runtimeConfig.Mode = TqMode::Client;
        runtimeConfig.ClientConfigPath = path;
        TqRouterRuntime adminRuntime(nullptr, runtimeConfig);
        std::string err;
        if (!adminRuntime.ApplyConfig(TqRouterConfig{}, err)) return 352;
        const std::string initialSaved = ReadTextFile(path);
        if (initialSaved.find("\"peers\":[]") == std::string::npos) return 353;

        TqHttpRequest create = Request("POST", "/peers", "{\"peer_id\":\"persisted-peer\",\"quic_peer\":\"127.0.0.1:14460\",\"socks_listen\":\"127.0.0.1:11060\",\"enabled\":false}");
        std::string createResp = adminRuntime.HandleAdmin(create);
        if (createResp.find("HTTP/1.1 201 Created") == std::string::npos) return 347;

        const std::string saved = ReadTextFile(path);
        if (saved.find("\"peer_id\":\"persisted-peer\"") == std::string::npos) return 348;
        if (saved.find("\"client_name\"") != std::string::npos) return 354;
        if (saved.find("\"enabled\":false") == std::string::npos) return 349;

        TqHttpRequest enable = Request("POST", "/peers/persisted-peer:enable", "");
        std::string enableResp = adminRuntime.HandleAdmin(enable);
        if (enableResp.find("HTTP/1.1 200 OK") == std::string::npos) return 350;
        const std::string enabledSaved = ReadTextFile(path);
        if (enabledSaved.find("\"enabled\":true") == std::string::npos) return 351;
    }
    {
        const std::string path = TempClientConfigPath("tcpquic-admin-persist-runtime-config");
        std::filesystem::remove(path);
        TqConfig runtimeConfig;
        runtimeConfig.Mode = TqMode::Client;
        runtimeConfig.ConfigPath = path;
        runtimeConfig.QuicCa = "ca.crt";
        runtimeConfig.ClientName = "linux-test-host";
        TqRouterRuntime adminRuntime(nullptr, runtimeConfig);
        std::string err;
        if (!adminRuntime.ApplyConfig(TqRouterConfig{}, err)) return 321;
        const std::string initialSaved = ReadTextFile(path);
        if (initialSaved.find("\"peers\": []") == std::string::npos) return 322;
        if (initialSaved.find("\"client_name\": \"linux-test-host\"") == std::string::npos) return 323;

        TqHttpRequest create = Request("POST", "/peers", "{\"peer_id\":\"runtime-peer\",\"quic_peer\":\"127.0.0.1:14461\",\"socks_listen\":\"127.0.0.1:11061\",\"enabled\":false}");
        std::string createResp = adminRuntime.HandleAdmin(create);
        if (createResp.find("HTTP/1.1 201 Created") == std::string::npos) return 324;

        const std::string saved = ReadTextFile(path);
        if (saved.find("\"id\": \"runtime-peer\"") == std::string::npos) return 325;
        if (saved.find("\"proto_peer\": \"127.0.0.1:14461\"") == std::string::npos) return 326;
        if (saved.find("\"socks_listen\": \"127.0.0.1:11061\"") == std::string::npos) return 327;
        if (saved.find("\"enabled\": false") == std::string::npos) return 328;
        if (saved.find("\"proto_connections\"") != std::string::npos) return 329;
    }
    {
        FakeAdapter adapter;
        TqRouterRuntime adminRuntime(&adapter);
        TqHttpRequest create = Request("POST", "/peers", "{\"peer_id\":\"agent-crud\",\"quic_peer\":\"127.0.0.1:14460\",\"socks_listen\":\"127.0.0.1:11060\",\"quic_connections\":2,\"compress\":\"auto\",\"enabled\":true}");
        std::string createResp = adminRuntime.HandleAdmin(create);
        if (createResp.find("HTTP/1.1 201 Created") == std::string::npos) return 260;
        if (adapter.Started.size() != 1 || adapter.Started[0] != "agent-crud") return 261;

        TqHttpRequest duplicate = Request("POST", "/peers", create.Body);
        std::string duplicateResp = adminRuntime.HandleAdmin(duplicate);
        if (duplicateResp.find("HTTP/1.1 409 Conflict") == std::string::npos) return 262;

        TqHttpRequest list = Request("GET", "/peers", "");
        std::string listResp = adminRuntime.HandleAdmin(list);
        if (listResp.find("HTTP/1.1 200 OK") == std::string::npos) return 263;
        if (listResp.find("\"peer_id\":\"agent-crud\"") == std::string::npos) return 264;
        if (listResp.find("\"client_name\":\"crud-name\"") != std::string::npos) return 284;
        if (listResp.find("\"state\":\"connecting\"") == std::string::npos) return 265;

        TqHttpRequest get = Request("GET", "/peers/agent-crud", "");
        std::string getResp = adminRuntime.HandleAdmin(get);
        if (getResp.find("HTTP/1.1 200 OK") == std::string::npos) return 266;
        if (getResp.find("\"quic_peer\":\"127.0.0.1:14460\"") == std::string::npos) return 267;
        if (getResp.find("\"client_name\":\"crud-name\"") != std::string::npos) return 285;

        TqHttpRequest replace = Request("PUT", "/peers/agent-crud", "{\"peer_id\":\"agent-crud\",\"quic_peer\":\"127.0.0.1:14460\",\"socks_listen\":\"127.0.0.1:11061\",\"quic_connections\":3,\"compress\":\"auto\",\"enabled\":true}");
        std::string replaceResp = adminRuntime.HandleAdmin(replace);
        if (replaceResp.find("HTTP/1.1 200 OK") == std::string::npos) return 268;
        if (adapter.Stopped.empty() || adapter.Stopped.back() != "agent-crud") return 269;
        if (adapter.Started.size() != 2 || adapter.LastStartedPeer.SocksListen != "127.0.0.1:11061") return 270;

        TqHttpRequest rejectedPatch = Request("PATCH", "/peers/agent-crud", "{\"client_name\":\"crud-patched\"}");
        std::string rejectedPatchResp = adminRuntime.HandleAdmin(rejectedPatch);
        if (rejectedPatchResp.find("HTTP/1.1 400") == std::string::npos) return 286;

        TqHttpRequest patch = Request("PATCH", "/peers/agent-crud", "{\"socks_listen\":\"127.0.0.1:11062\"}");
        std::string patchResp = adminRuntime.HandleAdmin(patch);
        if (patchResp.find("HTTP/1.1 200 OK") == std::string::npos) return 271;
        if (adapter.Started.size() != 3 || adapter.LastStartedPeer.SocksListen != "127.0.0.1:11062") return 272;

        TqHttpRequest disable = Request("POST", "/peers/agent-crud:disable", "");
        std::string disableResp = adminRuntime.HandleAdmin(disable);
        if (disableResp.find("HTTP/1.1 200 OK") == std::string::npos) return 273;
        if (disableResp.find("\"enabled\":false") == std::string::npos) return 274;

        TqHttpRequest enable = Request("POST", "/peers/agent-crud:enable", "");
        std::string enableResp = adminRuntime.HandleAdmin(enable);
        if (enableResp.find("HTTP/1.1 200 OK") == std::string::npos) return 275;
        if (enableResp.find("\"enabled\":true") == std::string::npos) return 276;

        TqHttpRequest drain = Request("POST", "/peers/agent-crud:drain", "{\"grace_seconds\":1}");
        std::string drainResp = adminRuntime.HandleAdmin(drain);
        if (drainResp.find("HTTP/1.1 202 Accepted") == std::string::npos) return 277;
        if (adapter.Drained.empty() || adapter.Drained.back() != "agent-crud") return 278;
        if (adapter.DrainGraces.empty() || adapter.DrainGraces.back() != 1) return 284;

        TqHttpRequest abort = Request("POST", "/peers/agent-crud:abort-tunnels", "");
        std::string abortResp = adminRuntime.HandleAdmin(abort);
        if (abortResp.find("HTTP/1.1 202 Accepted") == std::string::npos) return 279;
        if (adapter.AbortAll.empty() || adapter.AbortAll.back() != "agent-crud") return 280;

        TqHttpRequest rejectDelete = Request("DELETE", "/peers/agent-crud", "");
        std::string rejectDeleteResp = adminRuntime.HandleAdmin(rejectDelete);
        if (rejectDeleteResp.find("HTTP/1.1 409 Conflict") == std::string::npos) return 281;

        TqHttpRequest deletePeer = Request("DELETE", "/peers/agent-crud", "{\"mode\": \"abort\"}");
        std::string deleteResp = adminRuntime.HandleAdmin(deletePeer);
        if (deleteResp.find("HTTP/1.1 200 OK") == std::string::npos) return 282;
        if (adminRuntime.SnapshotConfig().Peers.size() != 0) return 283;
    }
    {
        FakeAdapter adapter;
        TqRouterRuntime adminRuntime(&adapter);
        TqHttpRequest create = Request("POST", "/peers", "{\"id\":\"agent-alias\",\"proto_peer\":\"127.0.0.1:14470\",\"socks_listen\":\"127.0.0.1:11070\",\"proto_connections\":2,\"compress\":\"auto\",\"enabled\":true}");
        std::string createResp = adminRuntime.HandleAdmin(create);
        if (createResp.find("HTTP/1.1 201 Created") == std::string::npos) return 333;
        if (adapter.Started.size() != 1 || adapter.LastStartedPeer.PeerId != "agent-alias") return 334;
        if (adapter.LastStartedPeer.QuicPeer != "127.0.0.1:14470") return 335;
        if (adapter.LastStartedPeer.QuicConnections != 2) return 336;

        TqHttpRequest replace = Request("PUT", "/peers/agent-alias", "{\"id\":\"agent-alias\",\"proto_peer\":\"127.0.0.1:14471\",\"socks_listen\":\"127.0.0.1:11071\",\"proto_connections\":3,\"compress\":\"zstd\",\"enabled\":true}");
        std::string replaceResp = adminRuntime.HandleAdmin(replace);
        if (replaceResp.find("HTTP/1.1 200 OK") == std::string::npos) return 337;
        if (adapter.LastStartedPeer.QuicPeer != "127.0.0.1:14471") return 338;
        if (adapter.LastStartedPeer.QuicConnections != 3) return 339;

        TqHttpRequest patch = Request("PATCH", "/peers/agent-alias", "{\"proto_peer\":\"127.0.0.1:14472\",\"proto_connections\":4}");
        std::string patchResp = adminRuntime.HandleAdmin(patch);
        if (patchResp.find("HTTP/1.1 200 OK") == std::string::npos) return 340;
        if (adapter.LastStartedPeer.QuicPeer != "127.0.0.1:14472") return 341;
        if (adapter.LastStartedPeer.QuicConnections != 4) return 342;

        TqHttpRequest conflictCreate = Request("POST", "/peers", "{\"peer_id\":\"agent-conflict\",\"id\":\"agent-conflict\",\"quic_peer\":\"127.0.0.1:14473\",\"socks_listen\":\"127.0.0.1:11073\"}");
        std::string conflictCreateResp = adminRuntime.HandleAdmin(conflictCreate);
        if (conflictCreateResp.find("HTTP/1.1 400") == std::string::npos) return 343;
        if (conflictCreateResp.find("conflicting peer field aliases") == std::string::npos) return 344;

        TqHttpRequest conflictPatch = Request("PATCH", "/peers/agent-alias", "{\"quic_connections\":5,\"proto_connections\":5}");
        std::string conflictPatchResp = adminRuntime.HandleAdmin(conflictPatch);
        if (conflictPatchResp.find("HTTP/1.1 400") == std::string::npos) return 345;
        if (conflictPatchResp.find("conflicting peer field aliases") == std::string::npos) return 346;
    }
    {
        FakeAdapter adapter;
        TqRouterRuntime adminRuntime(&adapter);
        const std::string createBody =
            "{\"peer_id\":\"agent-path-admin\","
            "\"quic_peer\":\"127.0.0.1:14461\","
            "\"socks_listen\":\"127.0.0.1:11070\","
            "\"quic_connections\":2,"
            "\"paths\":[{\"name\":\"cmcc\",\"local\":\"10.0.0.2\",\"peer\":\"36.1.1.10:443\",\"connections\":2}],"
            "\"enabled\":true}";
        TqHttpRequest create = Request("POST", "/peers", createBody);
        std::string createResp = adminRuntime.HandleAdmin(create);
        if (createResp.find("HTTP/1.1 201 Created") == std::string::npos) return 960;
        if (adapter.Started.size() != 1 || adapter.Started[0] != "agent-path-admin") return 961;
        if (adapter.LastStartedPeer.QuicPaths.size() != 1) return 962;
        if (adapter.LastStartedPeer.QuicPaths[0].Name != "cmcc") return 963;
        if (adapter.LastStartedPeer.QuicPaths[0].LocalAddress != "10.0.0.2") return 964;
        if (adapter.LastStartedPeer.QuicPaths[0].Peer != "36.1.1.10:443") return 965;
        if (adapter.LastStartedPeer.QuicPaths[0].Connections != 2) return 966;

        TqHttpRequest getConfig = Request("GET", "/config", "");
        std::string configResp = adminRuntime.HandleAdmin(getConfig);
        if (configResp.find("HTTP/1.1 200 OK") == std::string::npos) return 967;
        nlohmann::json configJson;
        if (!ParseJson(JsonBody(configResp), configJson)) return 968;
        if (configJson["peers"][0]["paths"][0]["name"] != "cmcc") return 968;
        if (configJson["peers"][0]["paths"][0]["local"] != "10.0.0.2") return 968;
        if (configJson["peers"][0]["paths"][0]["peer"] != "36.1.1.10:443") return 968;
        if (configJson["peers"][0]["paths"][0]["connections"] != 2) return 968;

        TqHttpRequest listPeers = Request("GET", "/peers", "");
        std::string listResp = adminRuntime.HandleAdmin(listPeers);
        if (listResp.find("HTTP/1.1 200 OK") == std::string::npos) return 969;
        nlohmann::json listJson;
        if (!ParseJson(JsonBody(listResp), listJson)) return 970;
        if (listJson["peers"][0]["paths"][0]["name"] != "cmcc") return 970;
        if (listJson["peers"][0]["paths"][0]["local"] != "10.0.0.2") return 970;
        if (listJson["peers"][0]["paths"][0]["peer"] != "36.1.1.10:443") return 970;
        if (listJson["peers"][0]["paths"][0]["connections"] != 2) return 970;

        TqHttpRequest patch = Request(
            "PATCH",
            "/peers/agent-path-admin",
            "{\"paths\":[{\"name\":\"cmcc\",\"local\":\"10.0.0.3\",\"peer\":\"59.1.1.10:443\",\"connections\":3}]}");
        std::string patchResp = adminRuntime.HandleAdmin(patch);
        if (patchResp.find("HTTP/1.1 200 OK") == std::string::npos) return 971;
        if (adapter.Stopped.size() != 1 || adapter.Stopped.back() != "agent-path-admin") return 972;
        if (adapter.Drained.size() != 1 || adapter.Drained.back() != "agent-path-admin") return 973;
        if (adapter.Started.size() != 2 || adapter.LastStartedPeer.QuicPaths.size() != 1) return 974;
        if (adapter.LastStartedPeer.QuicPaths[0].LocalAddress != "10.0.0.3") return 975;
        if (adapter.LastStartedPeer.QuicPaths[0].Peer != "59.1.1.10:443") return 976;
        if (adapter.LastStartedPeer.QuicPaths[0].Connections != 3) return 977;

        TqHttpRequest clear = Request("PATCH", "/peers/agent-path-admin", "{\"paths\":[]}");
        std::string clearResp = adminRuntime.HandleAdmin(clear);
        if (clearResp.find("HTTP/1.1 200 OK") == std::string::npos) return 978;
        if (adapter.Stopped.size() != 2 || adapter.Stopped.back() != "agent-path-admin") return 979;
        if (adapter.Started.size() != 3 || !adapter.LastStartedPeer.QuicPaths.empty()) return 980;
    }
    {
        TqRouterRuntime adminRuntime;
        std::atomic<bool> start{false};
        std::atomic<uint32_t> failures{0};
        std::vector<std::thread> threads;
        for (uint32_t i = 0; i < 16; ++i) {
            threads.emplace_back([&adminRuntime, &start, &failures, i]() {
                while (!start.load(std::memory_order_acquire)) {
                    std::this_thread::yield();
                }
                const std::string peerId = "agent-race-" + std::to_string(i);
                const std::string body = "{\"peer_id\":\"" + peerId +
                    "\",\"quic_peer\":\"127.0.0.1:14460\",\"socks_listen\":\"127.0.0.1:" +
                    std::to_string(11100 + i) + "\",\"enabled\":false}";
                TqHttpRequest create = Request("POST", "/peers", body);
                const std::string createResp = adminRuntime.HandleAdmin(create);
                if (createResp.find("HTTP/1.1 201 Created") == std::string::npos) {
                    failures.fetch_add(1, std::memory_order_relaxed);
                }
            });
        }
        start.store(true, std::memory_order_release);
        for (auto& thread : threads) {
            thread.join();
        }
        if (failures.load(std::memory_order_relaxed) != 0) return 285;
        if (adminRuntime.SnapshotConfig().Peers.size() != 16) return 286;
    }
    {
        TqRouterRuntime adminRuntime;
        const std::string validConfig =
            "{\"version\":1,\"peers\":[{\"peer_id\":\"agent-forward-admin\","
            "\"quic_peer\":\"127.0.0.1:14450\","
            "\"socks_listen\":\"\","
            "\"port_forwards\":["
            "{\"listen\":\"127.0.0.1:15432\",\"target\":\"db.internal:5432\"},"
            "{\"listen\":\"[::1]:18080\",\"target\":\"[2001:db8::10]:443\"}"
            "]}]}";
        TqHttpRequest putConfig = Request("PUT", "/config", validConfig);
        std::string put = adminRuntime.HandleAdmin(putConfig);
        if (put.find("HTTP/1.1 200 OK") == std::string::npos) return 198;
        nlohmann::json putJson;
        if (!ParseJson(JsonBody(put), putJson)) return 199;
        if (!HasPortForward(putJson["peers"][0]["port_forwards"], "127.0.0.1:15432", "db.internal:5432")) return 199;
        if (!HasPortForward(putJson["peers"][0]["port_forwards"], "[::1]:18080", "[2001:db8::10]:443")) return 200;

        TqHttpRequest getConfig = Request("GET", "/config", "");
        std::string config = adminRuntime.HandleAdmin(getConfig);
        if (config.find("HTTP/1.1 200 OK") == std::string::npos) return 201;
        nlohmann::json configJson;
        if (!ParseJson(JsonBody(config), configJson)) return 202;
        if (!HasPortForward(configJson["peers"][0]["port_forwards"], "127.0.0.1:15432", "db.internal:5432")) return 202;
        if (!HasPortForward(configJson["peers"][0]["port_forwards"], "[::1]:18080", "[2001:db8::10]:443")) return 203;

        TqHttpRequest roundTrip = Request("PUT", "/config", JsonBody(config));
        std::string roundTripResp = adminRuntime.HandleAdmin(roundTrip);
        if (roundTripResp.find("HTTP/1.1 200 OK") == std::string::npos) return 204;

        const TqRouterConfig beforeReject = adminRuntime.SnapshotConfig();
        if (beforeReject.Peers.size() != 1 || beforeReject.Peers[0].PortForwards.size() != 2) return 205;

        const std::string longHost(256, 'a');
        const std::string overlongTargetConfig =
            "{\"version\":1,\"peers\":[{\"peer_id\":\"agent-bad-long-target\","
            "\"quic_peer\":\"127.0.0.1:14454\","
            "\"socks_listen\":\"\","
            "\"port_forwards\":[{\"listen\":\"127.0.0.1:15433\",\"target\":\"" + longHost + ":5432\"}]}]}";
        const char* invalidConfigs[] = {
            "{\"version\":1,\"peers\":[{\"peer_id\":\"agent-bad-object\",\"quic_peer\":\"127.0.0.1:14451\",\"socks_listen\":\"\",\"port_forwards\":{\"listen\":\"127.0.0.1:15432\",\"target\":\"db.internal:5432\"}}]}",
            "{\"version\":1,\"peers\":[{\"peer_id\":\"agent-bad-array\",\"quic_peer\":\"127.0.0.1:14452\",\"socks_listen\":\"\",\"port_forwards\":[\"127.0.0.1:15432=db.internal:5432\"]}]}",
            "{\"version\":1,\"peers\":[{\"peer_id\":\"agent-bad-target\",\"quic_peer\":\"127.0.0.1:14453\",\"socks_listen\":\"\",\"port_forwards\":[{\"listen\":\"127.0.0.1:15432\",\"target\":\"2001:db8::10:443\"}]}]}",
            overlongTargetConfig.c_str()
        };
        for (const char* invalidConfig : invalidConfigs) {
            TqHttpRequest invalidPut = Request("PUT", "/config", invalidConfig);
            std::string invalidResp = adminRuntime.HandleAdmin(invalidPut);
            if (invalidResp.find("HTTP/1.1 400") == std::string::npos) return 206;
            const TqRouterConfig afterReject = adminRuntime.SnapshotConfig();
            if (afterReject.Peers.size() != 1) return 207;
            if (afterReject.Peers[0].PeerId != "agent-forward-admin") return 208;
            if (afterReject.Peers[0].PortForwards.size() != 2) return 209;
            if (afterReject.Peers[0].PortForwards[0].Listen != "127.0.0.1:15432") return 210;
            if (afterReject.Peers[0].PortForwards[0].TargetHost != "db.internal") return 211;
            if (afterReject.Peers[0].PortForwards[0].TargetPort != 5432) return 212;
            if (afterReject.Peers[0].PortForwards[1].TargetHost != "2001:db8::10") return 213;
            if (afterReject.Peers[0].PortForwards[1].TargetPort != 443) return 214;
        }
    }
    {
        FakeAdapter adapter;
        adapter.Connections.resize(2);
        adapter.Connections[0].ConnectionId = "conn-0";
        adapter.Connections[0].SlotIndex = 0;
        adapter.Connections[0].State = "connected";
        adapter.Connections[0].Connected = true;
        adapter.Connections[0].PathName = "cmcc";
        adapter.Connections[0].LocalAddress = "10.10.1.2";
        adapter.Connections[0].PeerAddress = "36.1.1.10:443";
        adapter.Connections[1].ConnectionId = "conn-1";
        adapter.Connections[1].SlotIndex = 1;
        adapter.Connections[1].State = "connecting";

        TqRouterRuntime adminRuntime(&adapter);
        TqRouterConfig cfg;
        cfg.Peers.push_back(Peer("agent-conn", "127.0.0.1:11200"));
        cfg.Peers[0].QuicConnections = 0;
        std::string err;
        if (!adminRuntime.ApplyConfig(cfg, err)) return 287;

        TqHttpRequest list = Request("GET", "/peers/agent-conn/connections", "");
        std::string listResp = adminRuntime.HandleAdmin(list);
        if (listResp.find("HTTP/1.1 200 OK") == std::string::npos) return 288;
        if (listResp.find("\"connection_id\":\"conn-0\"") == std::string::npos) return 289;
        if (listResp.find("\"state\":\"connected\"") == std::string::npos) return 290;
        if (listResp.find("\"path\":\"cmcc\"") == std::string::npos) return 930;
        if (listResp.find("\"local\":\"10.10.1.2\"") == std::string::npos) return 931;
        if (listResp.find("\"peer\":\"36.1.1.10:443\"") == std::string::npos) return 932;

        TqHttpRequest get = Request("GET", "/peers/agent-conn/connections/conn-1", "");
        std::string getResp = adminRuntime.HandleAdmin(get);
        if (getResp.find("HTTP/1.1 200 OK") == std::string::npos) return 291;
        if (getResp.find("\"slot_index\":1") == std::string::npos) return 292;

        TqHttpRequest add = Request("POST", "/peers/agent-conn/connections", "");
        std::string addResp = adminRuntime.HandleAdmin(add);
        if (addResp.find("HTTP/1.1 201 Created") == std::string::npos) return 293;
        if (adapter.DesiredConnectionCounts.empty() || adapter.DesiredConnectionCounts.back() != 3) return 294;
        if (adminRuntime.SnapshotConfig().Peers[0].QuicConnections != 3) return 295;

        TqHttpRequest deleteMiddle = Request("DELETE", "/peers/agent-conn/connections/conn-0", "");
        std::string deleteMiddleResp = adminRuntime.HandleAdmin(deleteMiddle);
        if (deleteMiddleResp.find("HTTP/1.1 409 Conflict") == std::string::npos) return 296;

        TqHttpRequest reconnect = Request("POST", "/peers/agent-conn/connections/conn-1:reconnect", "");
        std::string reconnectResp = adminRuntime.HandleAdmin(reconnect);
        if (reconnectResp.find("HTTP/1.1 202 Accepted") == std::string::npos) return 297;
        if (adapter.ReconnectedConnections.empty() || adapter.ReconnectedConnections.back() != "conn-1") return 298;

        TqHttpRequest abort = Request("POST", "/peers/agent-conn/connections/conn-1:abort-tunnels", "");
        std::string abortResp = adminRuntime.HandleAdmin(abort);
        if (abortResp.find("HTTP/1.1 202 Accepted") == std::string::npos) return 299;
        if (adapter.AbortedConnectionTunnels.empty() || adapter.AbortedConnectionTunnels.back() != "conn-1") return 300;

        TqHttpRequest deleteHighest = Request("DELETE", "/peers/agent-conn/connections/conn-2", "");
        std::string deleteHighestResp = adminRuntime.HandleAdmin(deleteHighest);
        if (deleteHighestResp.find("HTTP/1.1 200 OK") == std::string::npos) return 301;
        if (adapter.DeletedConnections.empty() || adapter.DeletedConnections.back() != "conn-2") return 302;
        if (adminRuntime.SnapshotConfig().Peers[0].QuicConnections != 2) return 303;
    }
    {
        TqRouterRuntime adminRuntime;
        auto* connection = reinterpret_cast<MsQuicConnection*>(0x11);
        int context = 0;
        TqRegisterConnectionTunnel(connection, &context, &AdminTunnelAbort);
        TqTunnelRegistryMetadata metadata;
        metadata.PeerId = "agent-tunnel";
        metadata.ConnectionId = "conn-0";
        metadata.Target = "127.0.0.1:8080";
        metadata.Role = "client";
        metadata.Ingress = "http";
        metadata.Compress = "none";
        metadata.RelayBackend = "linux";
        TqUpdateConnectionTunnelMetadata(connection, &context, metadata);

        TqHttpRequest list = Request("GET", "/tunnels", "");
        std::string listResp = adminRuntime.HandleAdmin(list);
        if (listResp.find("HTTP/1.1 200 OK") == std::string::npos) return 304;
        if (listResp.find("\"peer_id\":\"agent-tunnel\"") == std::string::npos) return 305;
        if (listResp.find("\"target\":\"127.0.0.1:8080\"") == std::string::npos) return 306;
        const auto tunnels = TqSnapshotTunnels();
        if (tunnels.empty()) return 307;

        TqHttpRequest get = Request("GET", "/tunnels/" + tunnels[0].TunnelId, "");
        std::string getResp = adminRuntime.HandleAdmin(get);
        if (getResp.find("HTTP/1.1 200 OK") == std::string::npos) return 308;
        if (getResp.find("\"connection_id\":\"conn-0\"") == std::string::npos) return 309;

        TqHttpRequest drain = Request("POST", "/tunnels/" + tunnels[0].TunnelId + ":drain", "");
        std::string drainResp = adminRuntime.HandleAdmin(drain);
        if (drainResp.find("HTTP/1.1 202 Accepted") == std::string::npos) return 310;
        TqTunnelSnapshot drained;
        if (!TqGetTunnelSnapshot(tunnels[0].TunnelId, drained) || drained.State != "draining") return 311;

        TqHttpRequest relayMetrics = Request("GET", "/relay/metrics", "");
        std::string relayMetricsResp = adminRuntime.HandleAdmin(relayMetrics);
        if (relayMetricsResp.find("HTTP/1.1 200 OK") == std::string::npos) return 312;
        if (relayMetricsResp.find("\"active_relays\":") == std::string::npos) return 313;
        if (relayMetricsResp.find("\"snapshot_complete\":true") == std::string::npos) return 1366;
        if (relayMetricsResp.find("\"linux_relay_event_queue_capacity\"") == std::string::npos) return 354;
        if (relayMetricsResp.find("\"linux_relay_event_queue_push_cas_retries\"") == std::string::npos) return 355;
        if (relayMetricsResp.find("\"linux_relay_event_queue_pop_cas_retries\"") == std::string::npos) return 356;
        if (relayMetricsResp.find("\"linux_relay_event_producer_threads_observed\"") == std::string::npos) return 357;
        if (relayMetricsResp.find("\"linux_relay_multiple_event_producer_threads_observed\"") == std::string::npos) return 358;
        if (relayMetricsResp.find("\"terminal_exactly_once_violation\":") == std::string::npos ||
            relayMetricsResp.find("\"terminal_timeout_pending\":") == std::string::npos ||
            relayMetricsResp.find("\"terminal_sink_pending\":") == std::string::npos ||
            relayMetricsResp.find("\"terminal_retained_owner_count\":") == std::string::npos ||
            relayMetricsResp.find("\"terminal_retained_oldest_age_ms\":") == std::string::npos) return 507;

        const std::string retentions = adminRuntime.HandleAdmin(
            Request("GET", "/relay/terminal-retentions?backend=linux", ""));
        if (retentions.find("HTTP/1.1 200 OK") == std::string::npos ||
            retentions.find("\"retentions\":[") == std::string::npos ||
            retentions.find("\"count\":") == std::string::npos ||
            retentions.find("\"oldest_age_ms\":") == std::string::npos) return 505;
        const std::string invalidRetentions = adminRuntime.HandleAdmin(
            Request("GET", "/relay/terminal-retentions?terminal_phase=closing", ""));
        if (invalidRetentions.find("HTTP/1.1 400") == std::string::npos ||
            invalidRetentions.find("invalid_filter") == std::string::npos) return 506;

        TqHttpRequest relayWorkers = Request("GET", "/relay/workers", "");
        std::string relayWorkersResp = adminRuntime.HandleAdmin(relayWorkers);
        if (relayWorkersResp.find("HTTP/1.1 200 OK") == std::string::npos) return 314;
        if (relayWorkersResp.find("\"worker_id\":\"aggregate\"") == std::string::npos) return 315;
        if (relayWorkersResp.find("\"capabilities\":{") == std::string::npos) return 347;
        if (relayWorkersResp.find("\"worker_detail\":true") == std::string::npos) return 348;
        if (relayWorkersResp.find("\"per_worker_active_relays\":false") == std::string::npos) return 349;
        if (relayWorkersResp.find("\"errors\":") == std::string::npos) return 350;
        if (relayWorkersResp.find("\"event_queue_capacity\":") == std::string::npos) return 359;
        if (relayWorkersResp.find("\"snapshot_complete\":true") == std::string::npos) return 1367;

        TqHttpRequest activeRelays = Request("GET", "/relay/active-relays", "");
        std::string activeRelaysResp = adminRuntime.HandleAdmin(activeRelays);
        if (activeRelaysResp.find("HTTP/1.1 200 OK") == std::string::npos) return 318;
        if (activeRelaysResp.find("\"capabilities\":{") == std::string::npos) return 319;
        if (activeRelaysResp.find("\"relays\":[") == std::string::npos) return 320;
        nlohmann::json activeRelaysJson;
        if (!ParseJson(JsonBody(activeRelaysResp), activeRelaysJson)) return 1320;
#if defined(__linux__) || defined(_WIN32)
        if (activeRelaysJson["capabilities"]["active_relay_detail"] != true) return 1321;
        if (activeRelaysJson["capabilities"]["per_worker_active_relays"] != false) return 1322;
#endif

        TqHttpRequest missingRelay = Request("GET", "/relay/active-relays/relay-missing", "");
        std::string missingRelayResp = adminRuntime.HandleAdmin(missingRelay);
        if (missingRelayResp.find("HTTP/1.1 500 Internal Server Error") != std::string::npos) return 321;
#if defined(__linux__) || defined(_WIN32)
        if (missingRelayResp.find("HTTP/1.1 404 Not Found") == std::string::npos) return 325;
        if (missingRelayResp.find("\"code\":\"not_found\"") == std::string::npos) return 326;
#else
        if (missingRelayResp.find("HTTP/1.1 503 Service Unavailable") == std::string::npos) return 325;
        if (missingRelayResp.find("\"code\":\"not_supported\"") == std::string::npos) return 326;
#endif

        TqHttpRequest activeRelaysEmptyId = Request("GET", "/relay/active-relays/", "");
        std::string activeRelaysEmptyIdResp = adminRuntime.HandleAdmin(activeRelaysEmptyId);
        if (activeRelaysEmptyIdResp.find("HTTP/1.1 404 Not Found") == std::string::npos) return 327;

        TqHttpRequest activeRelaysNestedId = Request("GET", "/relay/active-relays/relay-1/extra", "");
        std::string activeRelaysNestedIdResp = adminRuntime.HandleAdmin(activeRelaysNestedId);
        if (activeRelaysNestedIdResp.find("HTTP/1.1 404 Not Found") == std::string::npos) return 328;

        TqHttpRequest aggregateWorker = Request("GET", "/relay/workers/aggregate", "");
        std::string aggregateWorkerResp = adminRuntime.HandleAdmin(aggregateWorker);
        if (aggregateWorkerResp.find("HTTP/1.1 200 OK") == std::string::npos) return 322;
        if (aggregateWorkerResp.find("\"worker_id\":\"aggregate\"") == std::string::npos) return 323;
        if (aggregateWorkerResp.find("\"worker_index\":0") == std::string::npos) return 351;
        if (aggregateWorkerResp.find("\"relays\":[") == std::string::npos) return 352;
        if (aggregateWorkerResp.find("\"errors\":") == std::string::npos) return 353;

#if defined(__APPLE__)
        {
            // Stopped runtime: list is complete aggregate-only. Start briefly to
            // assert darwin-N rows, capacity, and detail lookup without Windows.
            auto& darwinRuntime = TqDarwinRelayRuntime::Instance();
            darwinRuntime.Stop();
            TqTuningConfig darwinTuning{};
            darwinTuning.RelayWorkerCount = 2;
            darwinTuning.RelayEventQueueCapacity = 1000;
            if (!darwinRuntime.Start(darwinTuning)) return 1370;

            TqHttpRequest darwinWorkers = Request("GET", "/relay/workers", "");
            std::string darwinWorkersResp = adminRuntime.HandleAdmin(darwinWorkers);
            if (darwinWorkersResp.find("HTTP/1.1 200 OK") == std::string::npos) return 1371;
            if (darwinWorkersResp.find("\"snapshot_complete\":true") == std::string::npos) return 1372;
            if (darwinWorkersResp.find("\"worker_id\":\"aggregate\"") == std::string::npos) return 1373;
            if (darwinWorkersResp.find("\"worker_id\":\"darwin-0\"") == std::string::npos) return 1374;
            if (darwinWorkersResp.find("\"worker_id\":\"darwin-1\"") == std::string::npos) return 1375;
            if (darwinWorkersResp.find("\"worker_index\":0") == std::string::npos) return 1376;
            if (darwinWorkersResp.find("\"worker_index\":1") == std::string::npos) return 1377;
            if (darwinWorkersResp.find("\"event_queue_capacity\":1024") == std::string::npos) return 1378;

            TqHttpRequest darwin0 = Request("GET", "/relay/workers/darwin-0", "");
            std::string darwin0Resp = adminRuntime.HandleAdmin(darwin0);
            if (darwin0Resp.find("HTTP/1.1 200 OK") == std::string::npos) return 1379;
            if (darwin0Resp.find("\"worker_id\":\"darwin-0\"") == std::string::npos) return 1380;

            TqHttpRequest darwinMetrics = Request("GET", "/relay/metrics", "");
            std::string darwinMetricsResp = adminRuntime.HandleAdmin(darwinMetrics);
            if (darwinMetricsResp.find("HTTP/1.1 200 OK") == std::string::npos) return 1381;
            if (darwinMetricsResp.find("\"snapshot_complete\":true") == std::string::npos) return 1382;
            if (darwinMetricsResp.find("\"relay_runtime_snapshot_inflight_max\"") ==
                std::string::npos) {
                return 1383;
            }
            if (darwinMetricsResp.find("\"relay_snapshot_execution_busy\"") ==
                std::string::npos) {
                return 1384;
            }

            darwinRuntime.Stop();
            TqHttpRequest stoppedWorkers = Request("GET", "/relay/workers", "");
            std::string stoppedWorkersResp = adminRuntime.HandleAdmin(stoppedWorkers);
            if (stoppedWorkersResp.find("\"worker_id\":\"darwin-0\"") != std::string::npos) {
                return 1385;
            }
            if (stoppedWorkersResp.find("\"snapshot_complete\":true") == std::string::npos) {
                return 1386;
            }
        }
#endif

        TqHttpRequest missingWorker = Request("GET", "/relay/workers/worker-1", "");
        std::string missingWorkerResp = adminRuntime.HandleAdmin(missingWorker);
        if (missingWorkerResp.find("HTTP/1.1 500 Internal Server Error") != std::string::npos) return 329;
        if (missingWorkerResp.find("HTTP/1.1 404 Not Found") == std::string::npos) return 330;
        if (missingWorkerResp.find("\"code\":\"not_found\"") == std::string::npos) return 324;

        TqHttpRequest workersEmptyId = Request("GET", "/relay/workers/", "");
        std::string workersEmptyIdResp = adminRuntime.HandleAdmin(workersEmptyId);
        if (workersEmptyIdResp.find("HTTP/1.1 404 Not Found") == std::string::npos) return 331;

        TqHttpRequest workersNestedId = Request("GET", "/relay/workers/a/b", "");
        std::string workersNestedIdResp = adminRuntime.HandleAdmin(workersNestedId);
        if (workersNestedIdResp.find("HTTP/1.1 404 Not Found") == std::string::npos) return 332;

        TqHttpRequest abort = Request("POST", "/tunnels/" + tunnels[0].TunnelId + ":abort", "");
        std::string abortResp = adminRuntime.HandleAdmin(abort);
        if (abortResp.find("HTTP/1.1 202 Accepted") == std::string::npos) return 316;
        if (g_adminTunnelAbortCalls.load() == 0) return 317;

        TqUnregisterConnectionTunnel(connection, &context);
    }
    {
        TqRouterRuntime unicodeRuntime;
        const std::string escapedPeerId = "\\u" "0061gent-d";
        TqHttpRequest putConfig = Request("PUT", "/config", "{\"version\":1,\"peers\":[{\"peer_id\":\"" + escapedPeerId + "\",\"quic_peer\":\"127.0.0.1:14446\",\"socks_listen\":\"127.0.0.1:11003\"}]}");
        if (putConfig.Body.find("\\u0061gent-d") == std::string::npos) return 48;
        std::string put = unicodeRuntime.HandleAdmin(putConfig);
        if (put.find("HTTP/1.1 200 OK") == std::string::npos) return 45;
        auto metrics = unicodeRuntime.SnapshotMetrics();
        if (metrics.Peers.size() != 1) return 46;
        if (metrics.Peers[0].PeerId != "agent-d") return 47;
        TqHttpRequest surrogatePair = Request("PUT", "/config", "{\"version\":1,\"peers\":[{\"peer_id\":\"\\uD83D\\uDE00\",\"quic_peer\":\"127.0.0.1:14446\",\"socks_listen\":\"127.0.0.1:11003\"}]}");
        std::string surrogatePairResp = unicodeRuntime.HandleAdmin(surrogatePair);
        if (surrogatePairResp.find("HTTP/1.1 400") == std::string::npos) return 69;
        if (surrogatePairResp.find("unicode surrogate") == std::string::npos) return 71;
        TqHttpRequest loneSurrogate = Request("PUT", "/config", "{\"version\":1,\"peers\":[{\"peer_id\":\"\\uD83D\",\"quic_peer\":\"127.0.0.1:14446\",\"socks_listen\":\"127.0.0.1:11003\"}]}");
        std::string loneSurrogateResp = unicodeRuntime.HandleAdmin(loneSurrogate);
        if (loneSurrogateResp.find("HTTP/1.1 400") == std::string::npos) return 70;
        if (loneSurrogateResp.find("unicode surrogate") == std::string::npos) return 72;
    }
    {
        TqRouterRuntime bridgeRuntime(true);
        TqRouterConfig startup;
        startup.Peers.push_back(Peer("agent-a", "127.0.0.1:11001"));
        std::string err;
        if (!bridgeRuntime.ApplyConfig(startup, err)) return 53;
        TqHttpRequest putSecond = Request("PUT", "/config", "{\"version\":1,\"peers\":[{\"peer_id\":\"agent-a\",\"quic_peer\":\"127.0.0.1:14444\",\"socks_listen\":\"127.0.0.1:11001\"},{\"peer_id\":\"agent-b\",\"quic_peer\":\"127.0.0.1:14445\",\"socks_listen\":\"127.0.0.1:11002\"}]}");
        std::string resp = bridgeRuntime.HandleAdmin(putSecond);
        if (resp.find("HTTP/1.1 400") == std::string::npos) return 54;
        if (resp.find("multiple enabled peers") == std::string::npos) return 55;
        if (bridgeRuntime.SnapshotConfig().Peers.size() != 1) return 56;
    }
    {
        TqRouterRuntime bridgeRuntime(true);
        TqRouterConfig startup;
        startup.Peers.push_back(Peer("agent-a", "127.0.0.1:11001"));
        startup.Peers.push_back(Peer("agent-b", "127.0.0.1:11002", false));
        std::string err;
        if (!bridgeRuntime.ApplyConfig(startup, err)) return 57;
        TqHttpRequest disableActive = Request("POST", "/peers/agent-a/disable", "");
        std::string disableResp = bridgeRuntime.HandleAdmin(disableActive);
        if (disableResp.find("HTTP/1.1 400") == std::string::npos) return 58;
        if (disableResp.find("single active peer") == std::string::npos) return 59;
        TqHttpRequest putChanged = Request("PUT", "/config", "{\"version\":1,\"peers\":[{\"peer_id\":\"agent-a\",\"quic_peer\":\"127.0.0.1:14444\",\"socks_listen\":\"127.0.0.1:11001\",\"enabled\":false},{\"peer_id\":\"agent-b\",\"quic_peer\":\"127.0.0.1:14445\",\"socks_listen\":\"127.0.0.1:11002\"}]}");
        std::string putResp = bridgeRuntime.HandleAdmin(putChanged);
        if (putResp.find("HTTP/1.1 400") == std::string::npos) return 60;
        if (putResp.find("single active peer") == std::string::npos) return 61;
        auto snapshot = bridgeRuntime.SnapshotConfig();
        if (snapshot.Peers[0].PeerId != "agent-a" || !snapshot.Peers[0].Enabled) return 62;
    }
    {
        TqClientMetrics metrics;
        metrics.QuicPeer = "127.0.0.1:14444";
        metrics.SocksListen = "127.0.0.1:1080";
        metrics.HttpListen = "127.0.0.1:8080";
        metrics.PortForwards.push_back(Forward("127.0.0.1:15432", "db.internal", 5432));
        metrics.ConnectionCount = 4;
        metrics.ConnectedConnections = 3;
        nlohmann::json body;
        if (!ParseJson(TqClientMetricsJson(metrics, 9), body)) return 216;
        if (body["role"] != "client") return 217;
        if (body["status"] != "healthy") return 218;
        if (body["quic_peer"] != "127.0.0.1:14444") return 180;
        if (body["socks_listen"] != "127.0.0.1:1080") return 181;
        if (body["http_listen"] != "127.0.0.1:8080") return 215;
        if (body["port_forwards"][0]["listen"] != "127.0.0.1:15432") return 219;
        if (body["port_forwards"][0]["target"] != "db.internal:5432") return 220;
        if (body["uptime_seconds"] != 9) return 221;
        if (body["connection_count"] != 4) return 222;
        if (body["connected_connections"] != 3) return 223;
        if (!body.contains("linux_relay_tcp_write_sendmsg_calls")) return 224;
        if (!body.contains("linux_relay_deferred_receive_complete_bytes")) return 225;
        if (!body.contains("linux_relay_max_pending_quic_receive_bytes")) return 226;
        if (!body.contains("linux_relay_active_relays")) return 227;
        if (!body.contains("linux_relay_max_worker_pending_bytes")) return 228;
        if (!body.contains("linux_relay_max_relay_pending_quic_receive_bytes")) return 229;
        if (!body.contains("linux_relay_hot_relay_id")) return 230;
        if (!body.contains("linux_relay_hot_relay_local")) return 183;
        if (!body.contains("linux_relay_hot_relay_peer")) return 184;
    }
    {
        TqServerMetrics serverMetrics;
        serverMetrics.Listen = "127.0.0.1:14444";
        serverMetrics.AcceptedConnections = 3;
        serverMetrics.ActiveStreams = 2;
        serverMetrics.TotalStreams = 5;
        serverMetrics.AclDenied = 1;
        serverMetrics.TcpDialing = 4;
        serverMetrics.LastError = "acl denied";
        nlohmann::json health;
        if (!ParseJson(TqServerHealthJson(serverMetrics, 7), health)) return 34;
        if (health["role"] != "server") return 35;
        if (health["listen"] != "127.0.0.1:14444") return 36;
        if (health["uptime_seconds"] != 7) return 37;
        if (health["accepted_connections"] != 3) return 38;
        if (health["active_streams"] != 2) return 39;
        if (health["total_streams"] != 5) return 40;
        if (health["acl_denied"] != 1) return 182;
        if (health["tcp_dialing"] != 4) return 41;
        if (health["last_error"] != "acl denied") return 45;
        TqServerMetrics connectionMetrics;
        TqServerMetricsConnectionAccepted(connectionMetrics);
        nlohmann::json connectionJson;
        if (!ParseJson(TqServerMetricsJson(connectionMetrics, 0), connectionJson)) return 66;
        if (connectionJson["accepted_connections"] != 1) return 67;
        if (connectionJson["total_streams"] != 0) return 68;
        if (connectionJson["active_streams"] != 0) return 69;
        TqServerMetrics streamMetrics;
        TqServerMetricsStreamStarted(streamMetrics);
        nlohmann::json startedJson;
        if (!ParseJson(TqServerMetricsJson(streamMetrics, 0), startedJson)) return 63;
        if (startedJson["accepted_connections"] != 0) return 64;
        if (startedJson["total_streams"] != 1) return 65;
        if (startedJson["active_streams"] != 1) return 70;
        TqServerMetricsStreamFinished(streamMetrics);
        nlohmann::json streamJson;
        if (!ParseJson(TqServerMetricsJson(streamMetrics, 0), streamJson)) return 42;
        if (streamJson["accepted_connections"] != 0) return 43;
        if (streamJson["total_streams"] != 1) return 44;
        if (streamJson["active_streams"] != 0) return 46;
    }
    {
        TqServerMetrics metrics;
        nlohmann::json body;
        if (!ParseJson(TqServerMetricsJson(metrics, 0), body)) return 231;
        const std::string bodyText = body.dump();
        if (!body.contains("linux_relay_wakeups")) return 232;
        if (!body.contains("linux_relay_events_processed")) return 233;
        if (!body.contains("linux_relay_pending_events")) return 234;
        if (!body.contains("linux_relay_pending_bytes")) return 171;
        if (body["linux_relay_buffer_bytes_in_use"] != 0) return 172;
        if (body.contains("linux_relay_worker_slots_allocated")) return 173;
        if (body.contains("linux_relay_worker_slots_free")) return 174;
        if (bodyText.find("\"linux_relay_tcp_read_bytes\"") == std::string::npos) return 99;
        if (bodyText.find("\"linux_relay_tcp_write_bytes\"") == std::string::npos) return 100;
        if (bodyText.find("\"linux_relay_read_disabled_count\"") == std::string::npos) return 101;
        if (bodyText.find("\"linux_relay_fake_fin_receive_count\":") == std::string::npos) return 372;
        if (bodyText.find("\"linux_relay_late_tcp_error_after_stream_shutdown\":") == std::string::npos) return 374;
        if (bodyText.find("\"linux_relay_stream_lookup_scan_count\":") == std::string::npos) return 373;
        if (bodyText.find("\"linux_relay_terminal_retained_owner_count\":") == std::string::npos) return 375;
        if (bodyText.find("\"linux_relay_terminal_retained_oldest_age_ms\":") == std::string::npos) return 376;
        if (bodyText.find("\"linux_relay_stop_remaining\":") == std::string::npos) return 377;
        if (bodyText.find("\"relay_active_controls\":") == std::string::npos) return 378;
        if (bodyText.find("\"relay_control_generation_mismatch\":") == std::string::npos) return 379;
        if (bodyText.find("\"relay_prepared_relays\":") == std::string::npos) return 380;
        if (bodyText.find("\"relay_active_send_reservations\":") == std::string::npos) return 381;
        if (bodyText.find("\"relay_shutdown_sink_active\":") == std::string::npos) return 382;
        if (bodyText.find("\"relay_stop_remaining\":") == std::string::npos) return 383;
#if defined(__linux__)
        if (bodyText.find("\"linux_relay_backend\":\"epoll\"") == std::string::npos) return 235;
#elif defined(__APPLE__)
        if (bodyText.find("\"linux_relay_backend\":\"kqueue\"") == std::string::npos) return 235;
#elif defined(_WIN32)
        if (bodyText.find("\"linux_relay_backend\":\"iocp\"") == std::string::npos) return 235;
#else
        if (bodyText.find("\"linux_relay_backend\":\"unsupported\"") == std::string::npos) return 236;
#endif
        if (bodyText.find("\"linux_relay_compressed_tcp_bytes\":") == std::string::npos) return 103;
        if (bodyText.find("\"linux_relay_decompressed_tcp_bytes\":") == std::string::npos) return 104;
        if (bodyText.find("\"linux_relay_zstd_decompress_input_bytes\":") == std::string::npos) return 237;
        if (bodyText.find("\"linux_relay_zstd_decompress_output_bytes\":") == std::string::npos) return 238;
        if (bodyText.find("\"linux_relay_zstd_decompress_calls\":") == std::string::npos) return 239;
        if (bodyText.find("\"linux_relay_zstd_decompress_need_input\":") == std::string::npos) return 240;
        if (bodyText.find("\"linux_relay_zstd_decompress_need_output\":") == std::string::npos) return 241;
        if (bodyText.find("\"linux_relay_zstd_decompress_failures\":") == std::string::npos) return 242;
        if (bodyText.find("ingress_buffer") != std::string::npos) return 243;
        if (bodyText.find("\"linux_relay_errors\":") == std::string::npos) return 105;
        if (bodyText.find("\"linux_relay_event_queue_full_errors\":") == std::string::npos) return 260;
        if (bodyText.find("\"linux_relay_control_lock_wait_nanos\":") == std::string::npos) return 360;
        if (bodyText.find("\"linux_relay_control_lock_acquire_count\":") == std::string::npos) return 361;
        if (bodyText.find("\"linux_relay_control_command_wait_nanos\":") == std::string::npos) return 362;
        if (bodyText.find("\"linux_relay_control_command_wait_count\":") == std::string::npos) return 363;
        if (bodyText.find("\"linux_relay_control_command_timeouts\":") == std::string::npos) return 364;
        if (bodyText.find("\"linux_relay_control_command_enqueue_failures\":") == std::string::npos) return 365;
        if (bodyText.find("\"linux_relay_snapshot_command_wait_nanos\":") == std::string::npos) return 366;
        if (bodyText.find("\"linux_relay_snapshot_command_wait_count\":") == std::string::npos) return 367;
        if (bodyText.find("\"linux_relay_snapshot_command_timeouts\":") == std::string::npos) return 368;
        if (bodyText.find("\"linux_relay_runtime_lock_wait_nanos\":") == std::string::npos) return 369;
        if (bodyText.find("\"linux_relay_runtime_lock_acquire_count\":") == std::string::npos) return 370;
        if (bodyText.find("\"linux_relay_runtime_snapshot_inflight_max\":") == std::string::npos) return 371;
        if (bodyText.find("\"linux_relay_tcp_read_buffer_acquire_failures\":") == std::string::npos) return 261;
        if (bodyText.find("\"linux_relay_tcp_read_buffer_acquire_pending_budget_failures\":") == std::string::npos) return 137;
        if (bodyText.find("\"linux_relay_tcp_read_buffer_acquire_alloc_failures\":") == std::string::npos) return 139;
        if (bodyText.find("\"linux_relay_quic_send_failures\":") == std::string::npos) return 262;
        if (bodyText.find("\"linux_relay_quic_send_buffer_too_large_failures\":") == std::string::npos) return 140;
        if (bodyText.find("\"linux_relay_quic_send_operation_alloc_failures\":") == std::string::npos) return 141;
        if (bodyText.find("\"linux_relay_quic_send_api_failures\":") == std::string::npos) return 142;
        if (bodyText.find("\"linux_relay_quic_send_backpressure_events\":") == std::string::npos) return 244;
        if (bodyText.find("\"linux_relay_quic_send_fatal_errors\":") == std::string::npos) return 245;
        if (bodyText.find("\"linux_relay_quic_receive_view_backpressure_queued\":") == std::string::npos) return 246;
        if (bodyText.find("\"linux_relay_quic_receive_decompress_failures\":") == std::string::npos) return 135;
        if (bodyText.find("\"linux_relay_quic_receive_view_alloc_failures\":") == std::string::npos) return 143;
        if (bodyText.find("\"linux_relay_quic_receive_view_null_buffer_failures\":") == std::string::npos) return 144;
        if (bodyText.find("\"linux_relay_quic_receive_view_empty_failures\":") == std::string::npos) return 145;
        if (bodyText.find("\"linux_relay_quic_receive_view_enqueue_failures\":") == std::string::npos) return 146;
        if (bodyText.find("\"linux_relay_tcp_write_hard_errors\":") == std::string::npos) return 136;
        if (bodyText.find("\"linux_relay_tcp_read_hard_errors\":") == std::string::npos) return 160;
        if (bodyText.find("\"linux_relay_last_tcp_read_errno\":") == std::string::npos) return 161;
        if (bodyText.find("\"linux_relay_fatal_relay_resets\":") == std::string::npos) return 247;
        if (bodyText.find("\"linux_relay_tcp_write_sendmsg_calls\":") == std::string::npos) return 126;
        if (bodyText.find("\"linux_relay_max_tcp_write_sendmsg_bytes\":") == std::string::npos) return 127;
        if (bodyText.find("\"linux_relay_tcp_write_eagain_count\":") == std::string::npos) return 128;
        if (bodyText.find("\"linux_relay_tcp_write_partial_count\":") == std::string::npos) return 129;
        if (bodyText.find("\"linux_relay_deferred_receive_completion_flushes\":") == std::string::npos) return 130;
        if (bodyText.find("\"linux_relay_max_pending_quic_receive_queue\":") == std::string::npos) return 131;
        if (bodyText.find("\"linux_relay_max_worker_active_relays\":") == std::string::npos) return 248;
        if (bodyText.find("\"linux_relay_max_relay_pending_quic_receive_queue\":") == std::string::npos) return 249;
        if (bodyText.find("\"linux_relay_max_relay_tcp_write_eagain_count\":") == std::string::npos) return 250;
        if (bodyText.find("\"linux_relay_hot_relay_worker_index\":") == std::string::npos) return 251;
        if (bodyText.find("\"linux_relay_hot_relay_tcp_write_eagain_count\":") == std::string::npos) return 252;
        if (bodyText.find("\"linux_relay_hot_relay_pending_quic_receive_bytes\":") == std::string::npos) return 253;
        if (bodyText.find("\"linux_relay_hot_relay_epollout_events\":") == std::string::npos) return 254;
        if (bodyText.find("\"linux_relay_hot_relay_tcp_write_armed\":") == std::string::npos) return 154;
        if (bodyText.find("\"linux_relay_active_tcp_relays\":") == std::string::npos) return 155;
        if (bodyText.find("\"linux_relay_active_sink_relays\":") == std::string::npos) return 156;
        if (bodyText.find("\"linux_relay_active_quic_send_relays\":") == std::string::npos) return 255;
        if (bodyText.find("\"linux_relay_current_pending_quic_receive_bytes\":") == std::string::npos) return 256;
        if (bodyText.find("\"linux_relay_current_pending_quic_receive_queue\":") == std::string::npos) return 257;
        if (bodyText.find("\"linux_relay_tcp_read_armed_relays\":") == std::string::npos) return 258;
        if (bodyText.find("\"linux_relay_tcp_read_disabled_relays\":") == std::string::npos) return 163;
        if (bodyText.find("\"linux_relay_tcp_write_armed_relays\":") == std::string::npos) return 164;
        if (bodyText.find("\"linux_relay_closing_relays\":") == std::string::npos) return 165;
        if (bodyText.find("\"linux_relay_tcp_read_closed_relays\":") == std::string::npos) return 166;
        if (bodyText.find("\"linux_relay_tcp_write_shutdown_queued_relays\":") == std::string::npos) return 167;
        if (bodyText.find("\"linux_relay_outstanding_quic_sends\":") == std::string::npos) return 168;
        if (bodyText.find("\"linux_relay_pending_tcp_write_queue\":") == std::string::npos) return 169;
        if (bodyText.find("\"linux_relay_pending_tcp_write_bytes\":") == std::string::npos) return 170;
    }
    return 0;
}
