#include "router_runtime.h"
#include "server_metrics.h"

#include <string>
#include <vector>

class FakeAdapter : public TqPeerRuntimeAdapter {
public:
    std::vector<std::string> Started;
    std::vector<std::string> Stopped;
    std::vector<std::string> Drained;
    std::vector<std::string> AbortAll;
    uint32_t FailStarts{0};
    uint32_t ConnectedConnections{0};

    bool StartPeer(const TqPeerConfig& peer, std::string& err) override {
        Started.push_back(peer.PeerId);
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

    void DrainPeer(const std::string& peerId, uint32_t) override {
        Drained.push_back(peerId);
    }

    void AbortPeerTunnels(const std::string& peerId) override {
        AbortAll.push_back(peerId);
    }

    bool SnapshotPeerMetrics(const std::string& peerId, TqPeerMetrics& out) override {
        (void)peerId;
        out.ConnectionCount = 4;
        out.ConnectedConnections = ConnectedConnections;
        out.State = ConnectedConnections > 0 ? "healthy" : "connecting";
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

static std::string JsonBody(const std::string& response) {
    const size_t body = response.find("\r\n\r\n");
    return body == std::string::npos ? std::string{} : response.substr(body + 4);
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
        std::string json = runtime.ConfigJson();
        if (json.find("\"peer_id\":\"agent-c\"") == std::string::npos) return 12;
        std::string metrics = runtime.MetricsJson();
        if (metrics.find("\"role\":\"client\"") == std::string::npos) return 13;
        if (metrics.find("\"state\":\"connecting\"") == std::string::npos) return 14;
    }
    {
        TqRouterRuntime adminRuntime;
        TqHttpRequest getHealth = Request("GET", "/health", "");
        std::string health = adminRuntime.HandleAdmin(getHealth);
        if (health.find("HTTP/1.1 200 OK") == std::string::npos) return 20;
        if (health.find("\"status\":\"healthy\"") == std::string::npos) return 21;
        TqHttpRequest putConfig = Request("PUT", "/config", "{\"version\":1,\"peers\":[{\"peer_id\":\"agent-d\",\"quic_peer\":\"127.0.0.1:14446\",\"socks_listen\":\"127.0.0.1:11003\",\"quic_reconnect_interval_ms\":5000}]}");
        std::string put = adminRuntime.HandleAdmin(putConfig);
        if (put.find("HTTP/1.1 200 OK") == std::string::npos) return 22;
        if (adminRuntime.SnapshotMetrics().Peers.size() != 1) return 23;
        if (adminRuntime.SnapshotConfig().Peers[0].QuicReconnectIntervalMs != 5000) return 95;
        TqHttpRequest getMetrics = Request("GET", "/metrics", "");
        std::string metrics = adminRuntime.HandleAdmin(getMetrics);
        if (metrics.find("HTTP/1.1 200 OK") == std::string::npos) return 25;
        if (metrics.find("\"peer_id\":\"agent-d\"") == std::string::npos) return 26;
        TqHttpRequest getConfig = Request("GET", "/config", "");
        std::string config = adminRuntime.HandleAdmin(getConfig);
        if (config.find("HTTP/1.1 200 OK") == std::string::npos) return 27;
        if (config.find("\"peer_id\":\"agent-d\"") == std::string::npos) return 28;
        if (config.find("\"quic_reconnect_interval_ms\":5000") == std::string::npos) return 96;
        TqHttpRequest putRoundTrip = Request("PUT", "/config", JsonBody(config));
        std::string roundTrip = adminRuntime.HandleAdmin(putRoundTrip);
        if (roundTrip.find("HTTP/1.1 200 OK") == std::string::npos) return 74;
        if (adminRuntime.SnapshotConfig().Peers[0].QuicReconnectIntervalMs != 5000) return 97;
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
        TqServerMetrics serverMetrics;
        serverMetrics.Listen = "127.0.0.1:14444";
        serverMetrics.AcceptedConnections = 3;
        serverMetrics.ActiveStreams = 2;
        serverMetrics.TotalStreams = 5;
        serverMetrics.AclDenied = 1;
        serverMetrics.LastError = "acl denied";
        std::string health = TqServerHealthJson(serverMetrics, 7);
        if (health.find("\"role\":\"server\"") == std::string::npos) return 34;
        if (health.find("\"listen\":\"127.0.0.1:14444\"") == std::string::npos) return 35;
        if (health.find("\"uptime_seconds\":7") == std::string::npos) return 36;
        if (health.find("\"accepted_connections\":3") == std::string::npos) return 37;
        if (health.find("\"active_streams\":2") == std::string::npos) return 38;
        if (health.find("\"total_streams\":5") == std::string::npos) return 39;
        if (health.find("\"acl_denied\":1") == std::string::npos) return 40;
        if (health.find("\"last_error\":\"acl denied\"") == std::string::npos) return 41;
        TqServerMetrics connectionMetrics;
        TqServerMetricsConnectionAccepted(connectionMetrics);
        std::string connectionJson = TqServerMetricsJson(connectionMetrics, 0);
        if (connectionJson.find("\"accepted_connections\":1") == std::string::npos) return 66;
        if (connectionJson.find("\"total_streams\":0") == std::string::npos) return 67;
        if (connectionJson.find("\"active_streams\":0") == std::string::npos) return 68;
        TqServerMetrics streamMetrics;
        TqServerMetricsStreamStarted(streamMetrics);
        std::string startedJson = TqServerMetricsJson(streamMetrics, 0);
        if (startedJson.find("\"accepted_connections\":0") == std::string::npos) return 63;
        if (startedJson.find("\"total_streams\":1") == std::string::npos) return 64;
        if (startedJson.find("\"active_streams\":1") == std::string::npos) return 65;
        TqServerMetricsStreamFinished(streamMetrics);
        std::string streamJson = TqServerMetricsJson(streamMetrics, 0);
        if (streamJson.find("\"accepted_connections\":0") == std::string::npos) return 42;
        if (streamJson.find("\"total_streams\":1") == std::string::npos) return 43;
        if (streamJson.find("\"active_streams\":0") == std::string::npos) return 44;
    }
    {
        TqServerMetrics metrics;
        const std::string body = TqServerMetricsJson(metrics, 0);
        if (body.find("\"linux_relay_wakeups\"") == std::string::npos) return 95;
        if (body.find("\"linux_relay_events_processed\"") == std::string::npos) return 96;
        if (body.find("\"linux_relay_pending_events\"") == std::string::npos) return 97;
        if (body.find("\"linux_relay_pending_bytes\"") == std::string::npos) return 98;
        if (body.find("\"linux_relay_tcp_read_bytes\"") == std::string::npos) return 99;
        if (body.find("\"linux_relay_tcp_write_bytes\"") == std::string::npos) return 100;
        if (body.find("\"linux_relay_read_disabled_count\"") == std::string::npos) return 101;
#if defined(__linux__)
        if (body.find("\"linux_relay_backend\":\"worker\"") == std::string::npos) return 102;
#else
        if (body.find("\"linux_relay_backend\":\"unsupported\"") == std::string::npos) return 102;
#endif
        if (body.find("\"linux_relay_compressed_tcp_bytes\":") == std::string::npos) return 103;
        if (body.find("\"linux_relay_decompressed_tcp_bytes\":") == std::string::npos) return 104;
        if (body.find("\"linux_relay_errors\":") == std::string::npos) return 105;
    }
    return 0;
}
