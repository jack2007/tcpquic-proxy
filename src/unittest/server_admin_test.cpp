#include "server_admin.h"
#include "quic_session.h"
#include "relay_metrics.h"
#include "tunnel_registry.h"

#include <string>
#include <sstream>
#include <vector>

bool g_trace_stub_enabled = false;
bool g_diag_stats_stub_enabled = false;
unsigned g_trace_init_count = 0;
unsigned g_trace_shutdown_count = 0;
unsigned g_diag_stats_init_count = 0;
unsigned g_diag_stats_shutdown_count = 0;
uint32_t g_trace_stub_interval_sec = 0;
uint32_t g_diag_stats_stub_interval_sec = 0;
TqConfig::TraceLevel g_trace_stub_level = TqConfig::TraceLevel::Info;

void TqTraceProxyStubReset() {
    g_trace_stub_enabled = false;
    g_diag_stats_stub_enabled = false;
    g_trace_init_count = 0;
    g_trace_shutdown_count = 0;
    g_diag_stats_init_count = 0;
    g_diag_stats_shutdown_count = 0;
    g_trace_stub_interval_sec = 0;
    g_diag_stats_stub_interval_sec = 0;
    g_trace_stub_level = TqConfig::TraceLevel::Info;
}

bool TqTraceInit(TqMode, uint32_t intervalSec, TqConfig::TraceLevel level) {
    g_trace_stub_enabled = true;
    g_trace_stub_interval_sec = intervalSec;
    g_trace_stub_level = level;
    ++g_trace_init_count;
    return true;
}

void TqTraceShutdown() {
    g_trace_stub_enabled = false;
    ++g_trace_shutdown_count;
}

bool TqTraceEnabled() {
    return g_trace_stub_enabled;
}

bool TqDiagStatsInit(uint32_t intervalSec) {
    g_diag_stats_stub_enabled = true;
    g_diag_stats_stub_interval_sec = intervalSec;
    ++g_diag_stats_init_count;
    return true;
}

void TqDiagStatsShutdown() {
    g_diag_stats_stub_enabled = false;
    ++g_diag_stats_shutdown_count;
}

bool TqDiagStatsEnabled() {
    return g_diag_stats_stub_enabled;
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

namespace {

bool g_abortCalled = false;
bool g_tunnelAbortCalled = false;
bool g_tunnelDrainCalled = false;

TqHttpRequest Request(const std::string& method, const std::string& path, const std::string& body = "") {
    TqHttpRequest req;
    req.Method = method;
    req.Path = path;
    req.Body = body;
    return req;
}

} // namespace

void TqAppendJsonString(std::ostringstream& out, const char* name, const std::string& value);

void TqAppendJsonString(std::ostringstream& out, const char* name, const std::string& value) {
    out << '"' << name << "\":\"" << value << '"';
}

std::string TqJsonResponse(int status, const std::string& json) {
    const char* reason = status == 200 ? "OK" :
        (status == 202 ? "Accepted" :
        (status == 503 ? "Service Unavailable" : "Not Found"));
    return "HTTP/1.1 " + std::to_string(status) + " " + reason + "\r\n\r\n" + json;
}

std::string TqServerMetricsJson(const TqServerMetrics&, uint64_t) {
    return "{\"role\":\"server\"}";
}

TqRelayMetricsSnapshot TqSnapshotRelayMetrics() {
    TqRelayMetricsSnapshot metrics;
    metrics.Backend = "test";
    metrics.ActiveRelays = 7;
    metrics.PendingBytes = 1234;
    metrics.TcpReadBytes = 11;
    metrics.TcpWriteBytes = 22;
    metrics.Errors = 3;
    return metrics;
}

std::vector<TqRelayActiveSnapshot> TqSnapshotActiveRelays() {
    return {};
}

std::string TqRelayActiveRelaysJson() {
    return "{\"capabilities\":{\"detail\":false},\"relays\":[]}";
}

std::string TqRelayActiveRelayJson(const std::string&, bool& found, bool& supported) {
    found = false;
    supported = false;
    return "{}";
}

std::string TqRelayWorkerDetailJson(const std::string& workerId, bool& found, bool& supported) {
    supported = workerId == "aggregate";
    found = supported;
    return "{\"worker_id\":\"aggregate\",\"worker_index\":0,\"errors\":3,\"relays\":[]}";
}

std::string TqRelayWorkersJson() {
    return "{\"capabilities\":{\"active_relay_detail\":false,\"worker_detail\":true,\"per_worker_active_relays\":false},\"workers\":[{\"worker_id\":\"aggregate\",\"backend\":\"test\",\"worker_index\":0,\"active_relays\":7,\"pending_bytes\":1234,\"tcp_read_bytes\":11,\"tcp_write_bytes\":22,\"errors\":3}]}";
}

void TqAppendRelayMetricsJson(std::ostringstream& out, const TqRelayMetricsSnapshot&) {
    out << ",\"linux_relay_backend\":\"test\"";
}

std::vector<TqServerConnectionSnapshot> TqSnapshotServerConnections() {
    TqServerConnectionSnapshot connection;
    connection.ConnectionId = "srv-7";
    connection.RemoteAddress = "127.0.0.1:5000";
    connection.State = "connected";
    connection.ActiveStreams = 1;
    connection.TotalStreams = 3;
    connection.ActiveTunnels = 2;
    return {connection};
}

bool TqGetServerConnectionSnapshot(const std::string& connectionId, TqServerConnectionSnapshot& out) {
    if (connectionId != "srv-7") {
        return false;
    }
    out = TqSnapshotServerConnections()[0];
    return true;
}

bool TqAbortServerConnectionTunnels(const std::string& connectionId) {
    if (connectionId != "srv-7") {
        return false;
    }
    g_abortCalled = true;
    return true;
}

std::vector<TqTunnelSnapshot> TqSnapshotTunnels() {
    TqTunnelSnapshot tunnel;
    tunnel.TunnelId = "tun-5";
    tunnel.ConnectionId = "srv-7";
    tunnel.Role = "server";
    tunnel.State = "active";
    tunnel.Target = "example.com:443";
    return {tunnel};
}

bool TqGetTunnelSnapshot(const std::string& tunnelId, TqTunnelSnapshot& out) {
    if (tunnelId == "client-tun") {
        out = TqSnapshotTunnels()[0];
        out.TunnelId = "client-tun";
        out.Role = "client";
        return true;
    }
    if (tunnelId != "tun-5") {
        return false;
    }
    out = TqSnapshotTunnels()[0];
    return true;
}

bool TqAbortTunnelById(const std::string& tunnelId) {
    if (tunnelId != "tun-5") {
        return false;
    }
    g_tunnelAbortCalled = true;
    return true;
}

bool TqDrainTunnelById(const std::string& tunnelId) {
    if (tunnelId != "tun-5") {
        return false;
    }
    g_tunnelDrainCalled = true;
    return true;
}

int main() {
    TqServerMetrics metrics;
    metrics.Listen = "127.0.0.1:1443";

    std::string server = TqHandleServerAdmin(Request("GET", "/server"), metrics, 10);
    if (server.find("HTTP/1.1 200 OK") == std::string::npos) return 1;
    if (server.find("\"role\":\"server\"") == std::string::npos) return 2;

    std::string list = TqHandleServerAdmin(Request("GET", "/server/connections"), metrics, 10);
    if (list.find("HTTP/1.1 200 OK") == std::string::npos) return 3;
    if (list.find("\"connection_id\":\"srv-7\"") == std::string::npos) return 4;

    std::string get = TqHandleServerAdmin(Request("GET", "/server/connections/srv-7"), metrics, 10);
    if (get.find("\"remote_address\":\"127.0.0.1:5000\"") == std::string::npos) return 5;

    std::string abort = TqHandleServerAdmin(Request("POST", "/server/connections/srv-7:abort-tunnels"), metrics, 10);
    if (abort.find("HTTP/1.1 202 Accepted") == std::string::npos) return 6;
    if (!g_abortCalled) return 7;

    std::string tunnels = TqHandleServerAdmin(Request("GET", "/server/tunnels"), metrics, 10);
    if (tunnels.find("HTTP/1.1 200 OK") == std::string::npos) return 8;
    if (tunnels.find("\"tunnel_id\":\"tun-5\"") == std::string::npos) return 9;

    std::string getTunnel = TqHandleServerAdmin(Request("GET", "/server/tunnels/tun-5"), metrics, 10);
    if (getTunnel.find("HTTP/1.1 200 OK") == std::string::npos) return 130;
    if (getTunnel.find("\"tunnel_id\":\"tun-5\"") == std::string::npos) return 131;

    std::string getEncodedTunnel = TqHandleServerAdmin(Request("GET", "/server/tunnels/tun%2d5"), metrics, 10);
    if (getEncodedTunnel.find("HTTP/1.1 200 OK") == std::string::npos) return 140;

    std::string abortTunnel = TqHandleServerAdmin(Request("POST", "/server/tunnels/tun-5:abort"), metrics, 10);
    if (abortTunnel.find("HTTP/1.1 202 Accepted") == std::string::npos) return 132;
    if (!g_tunnelAbortCalled) return 133;

    std::string drainTunnel = TqHandleServerAdmin(Request("POST", "/server/tunnels/tun-5:drain"), metrics, 10);
    if (drainTunnel.find("HTTP/1.1 202 Accepted") == std::string::npos) return 134;
    if (!g_tunnelDrainCalled) return 135;

    g_tunnelAbortCalled = false;
    std::string deleteTunnel = TqHandleServerAdmin(Request("DELETE", "/server/tunnels/tun-5"), metrics, 10);
    if (deleteTunnel.find("HTTP/1.1 202 Accepted") == std::string::npos) return 136;
    if (!g_tunnelAbortCalled) return 137;

    std::string missingTunnel = TqHandleServerAdmin(Request("GET", "/server/tunnels/missing"), metrics, 10);
    if (missingTunnel.find("HTTP/1.1 404 Not Found") == std::string::npos) return 138;

    std::string clientTunnel = TqHandleServerAdmin(Request("GET", "/server/tunnels/client-tun"), metrics, 10);
    if (clientTunnel.find("HTTP/1.1 404 Not Found") == std::string::npos) return 139;

    std::string relayMetrics = TqHandleServerAdmin(Request("GET", "/relay/metrics"), metrics, 10);
    if (relayMetrics.find("HTTP/1.1 200 OK") == std::string::npos) return 149;
    if (relayMetrics.find("\"backend\":\"test\"") == std::string::npos) return 150;
    if (relayMetrics.find("\"active_relays\":7") == std::string::npos) return 151;

    std::string relayWorkers = TqHandleServerAdmin(Request("GET", "/relay/workers"), metrics, 10);
    if (relayWorkers.find("HTTP/1.1 200 OK") == std::string::npos) return 152;
    if (relayWorkers.find("\"worker_id\":\"aggregate\"") == std::string::npos) return 153;
    if (relayWorkers.find("\"capabilities\":{") == std::string::npos) return 154;
    if (relayWorkers.find("\"worker_detail\":true") == std::string::npos) return 155;

    std::string activeRelays = TqHandleServerAdmin(Request("GET", "/relay/active-relays"), metrics, 10);
    if (activeRelays.find("HTTP/1.1 200 OK") == std::string::npos) return 154;
    if (activeRelays.find("\"relays\":[") == std::string::npos) return 155;

    std::string aggregateWorker = TqHandleServerAdmin(Request("GET", "/relay/workers/aggregate"), metrics, 10);
    if (aggregateWorker.find("HTTP/1.1 200 OK") == std::string::npos) return 156;
    if (aggregateWorker.find("\"relays\":[") == std::string::npos) return 157;
    if (aggregateWorker.find("\"errors\":") == std::string::npos) return 158;

    std::string missingRelay = TqHandleServerAdmin(Request("GET", "/relay/active-relays/relay-missing"), metrics, 10);
    if (missingRelay.find("HTTP/1.1 503 Service Unavailable") == std::string::npos) return 157;
    if (missingRelay.find("\"code\":\"not_supported\"") == std::string::npos) return 158;

    g_tunnelAbortCalled = false;
    g_tunnelDrainCalled = false;
    std::string clientAbortTunnel = TqHandleServerAdmin(Request("POST", "/server/tunnels/client-tun:abort"), metrics, 10);
    if (clientAbortTunnel.find("HTTP/1.1 404 Not Found") == std::string::npos) return 145;
    if (g_tunnelAbortCalled) return 146;

    std::string clientDrainTunnel = TqHandleServerAdmin(Request("POST", "/server/tunnels/client-tun:drain"), metrics, 10);
    if (clientDrainTunnel.find("HTTP/1.1 404 Not Found") == std::string::npos) return 147;
    if (g_tunnelDrainCalled) return 148;

    std::string emptyTunnel = TqHandleServerAdmin(Request("GET", "/server/tunnels/"), metrics, 10);
    if (emptyTunnel.find("HTTP/1.1 404 Not Found") == std::string::npos) return 141;

    std::string nestedTunnel = TqHandleServerAdmin(Request("GET", "/server/tunnels/tun-5/extra"), metrics, 10);
    if (nestedTunnel.find("HTTP/1.1 404 Not Found") == std::string::npos) return 142;

    std::string badPercentTunnel = TqHandleServerAdmin(Request("GET", "/server/tunnels/tun%xx"), metrics, 10);
    if (badPercentTunnel.find("HTTP/1.1 404 Not Found") == std::string::npos) return 143;

    std::string slashPercentTunnel = TqHandleServerAdmin(Request("GET", "/server/tunnels/tun%2f5"), metrics, 10);
    if (slashPercentTunnel.find("HTTP/1.1 404 Not Found") == std::string::npos) return 144;

    std::string memory = TqHandleServerAdmin(Request("POST", "/memory/allocator:dump"), metrics, 10);
    if (memory.find("HTTP/1.1 200 OK") == std::string::npos) return 10;
    if (memory.find("\"status\":\"dumped\"") == std::string::npos) return 11;
    if (memory.find("\"allocator\":\"mimalloc\"") == std::string::npos) return 12;

    std::string missing = TqHandleServerAdmin(Request("GET", "/server/connections/srv-404"), metrics, 10);
    if (missing.find("HTTP/1.1 404 Not Found") == std::string::npos) return 13;
    {
        TqConfig cfg;
        cfg.Mode = TqMode::Server;
        cfg.QuicListen = "0.0.0.0:4433";
        cfg.AllowTargets = {"127.0.0.1/32"};
        TqServerMetrics configMetrics;
        configMetrics.Listen = cfg.QuicListen;
        configMetrics.ResolvedListens = {"0.0.0.0:4433"};
        std::string config = TqHandleServerAdmin(Request("GET", "/server/config"), configMetrics, 10, cfg);
        if (config.find("HTTP/1.1 200") == std::string::npos) return 120;
        if (config.find("\"allow_targets\"") == std::string::npos) return 121;

        std::string patchRuntime = TqHandleServerAdmin(
            Request("PATCH", "/runtime/config", "{\"compression\":{\"mode\":\"zstd\",\"level\":2},\"tuning\":{\"max_memory_mb\":256}}"),
            configMetrics,
            10,
            cfg);
        if (patchRuntime.find("HTTP/1.1 200 OK") == std::string::npos) return 159;
        if (patchRuntime.find("\"compress\":\"zstd\"") == std::string::npos) return 160;
        if (patchRuntime.find("\"compress_level\":2") == std::string::npos) return 161;
        if (patchRuntime.find("\"max_memory_mb\":256") == std::string::npos) return 162;

        std::string unsupportedRuntime = TqHandleServerAdmin(
            Request("PATCH", "/runtime/config", "{\"tls\":{\"cert\":\"server.crt\"}}"),
            configMetrics,
            10,
            cfg);
        if (unsupportedRuntime.find("HTTP/1.1 503 Service Unavailable") == std::string::npos) return 163;
        if (unsupportedRuntime.find("\"code\":\"not_supported\"") == std::string::npos) return 164;

        std::string diag = TqHandleServerAdmin(Request("GET", "/diagnostics"), configMetrics, 10, cfg);
        if (diag.find("\"trace\"") == std::string::npos) return 122;
        if (diag.find("\"trace_level\":\"info\"") == std::string::npos) return 145;

        TqTraceProxyStubReset();
        std::string patchDiag = TqHandleServerAdmin(
            Request(
                "PATCH",
                "/diagnostics",
                "{\"trace\":false,\"trace_interval_sec\":12,\"trace_level\":\"debug\",\"diag_stats\":true,\"diag_stats_interval_sec\":4}"),
            configMetrics,
            10,
            cfg);
        if (patchDiag.find("HTTP/1.1 200 OK") == std::string::npos) return 146;
        if (patchDiag.find("\"trace\":false") == std::string::npos) return 147;
        if (patchDiag.find("\"trace_interval_sec\":12") == std::string::npos) return 148;
        if (patchDiag.find("\"trace_level\":\"debug\"") == std::string::npos) return 149;
        if (patchDiag.find("\"diag_stats\":true") == std::string::npos) return 150;
        if (g_trace_stub_enabled) return 165;
        if (!g_diag_stats_stub_enabled) return 166;
        if (g_diag_stats_init_count != 1) return 167;
        if (g_diag_stats_stub_interval_sec != 4) return 168;

        std::string enableTrace = TqHandleServerAdmin(
            Request(
                "PATCH",
                "/diagnostics",
                "{\"trace\":true,\"trace_interval_sec\":8,\"trace_level\":\"debug\",\"diag_stats\":false}"),
            configMetrics,
            10,
            cfg);
        if (enableTrace.find("HTTP/1.1 200 OK") == std::string::npos) return 169;
        if (!g_trace_stub_enabled) return 170;
        if (g_trace_init_count != 1) return 171;
        if (g_trace_stub_interval_sec != 8) return 172;
        if (g_trace_stub_level != TqConfig::TraceLevel::Debug) return 173;
        if (g_diag_stats_stub_enabled) return 174;
        if (g_diag_stats_shutdown_count != 1) return 175;

        std::string badDiag = TqHandleServerAdmin(
            Request("PATCH", "/diagnostics", "{\"diag_stats_interval_sec\":0}"),
            configMetrics,
            10,
            cfg);
        if (badDiag.find("HTTP/1.1 400") == std::string::npos) return 151;
    }
    return 0;
}
