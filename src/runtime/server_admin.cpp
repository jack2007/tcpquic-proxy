#include "server_admin.h"

#include "admin_config.h"
#include "admin_memory.h"
#include "quic_session.h"
#include "relay_metrics.h"
#include "tunnel_registry.h"
#include "trace.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <mutex>
#include <sstream>
#include <vector>

namespace {

struct TqServerTunnelAdminPath {
    std::string TunnelId;
    std::string Action;
};

std::string ErrorJson(const std::string& err) {
    return nlohmann::json{{"error", err}}.dump();
}

std::string StructuredErrorJson(const std::string& code, const std::string& message) {
    return nlohmann::json{{"error", {{"code", code}, {"message", message}}}}.dump();
}

nlohmann::json RelayMetricsJsonValue(const TqRelayMetricsSnapshot& metrics) {
    return nlohmann::json::parse(TqRelayMetricsFieldsJson(metrics));
}

void MergeObject(nlohmann::json& target, const nlohmann::json& source) {
    for (const auto& item : source.items()) {
        target[item.key()] = item.value();
    }
}

std::string RelayMetricsJson() {
    const auto metrics = TqSnapshotRelayMetrics();
    nlohmann::json body{
        {"backend", metrics.Backend},
        {"active_relays", metrics.ActiveRelays},
        {"pending_bytes", metrics.PendingBytes},
        {"tcp_read_bytes", metrics.TcpReadBytes},
        {"tcp_write_bytes", metrics.TcpWriteBytes},
        {"errors", metrics.Errors},
    };
    MergeObject(body, RelayMetricsJsonValue(metrics));
    return body.dump();
}

nlohmann::json ServerConnectionJsonValue(const TqServerConnectionSnapshot& connection) {
    return {
        {"connection_id", connection.ConnectionId},
        {"remote_address", connection.RemoteAddress},
        {"state", connection.State},
        {"active_streams", connection.ActiveStreams},
        {"total_streams", connection.TotalStreams},
        {"active_tunnels", connection.ActiveTunnels},
        {"last_error", connection.LastError},
    };
}

std::string ServerConnectionsJson(const std::vector<TqServerConnectionSnapshot>& connections) {
    nlohmann::json body{{"connections", nlohmann::json::array()}};
    for (const auto& connection : connections) {
        body["connections"].push_back(ServerConnectionJsonValue(connection));
    }
    return body.dump();
}

std::string ServerConnectionJson(const TqServerConnectionSnapshot& connection) {
    return ServerConnectionJsonValue(connection).dump();
}

nlohmann::json TunnelJsonValue(const TqTunnelSnapshot& tunnel) {
    return {
        {"tunnel_id", tunnel.TunnelId},
        {"peer_id", tunnel.PeerId},
        {"connection_id", tunnel.ConnectionId},
        {"state", tunnel.State},
        {"target", tunnel.Target},
        {"role", tunnel.Role},
        {"duration_ms", tunnel.DurationMs},
        {"active", true},
    };
}

std::string ServerTunnelsJson() {
    const auto all = TqSnapshotTunnels();
    nlohmann::json body{{"tunnels", nlohmann::json::array()}};
    for (const auto& tunnel : all) {
        if (tunnel.Role != "server") {
            continue;
        }
        body["tunnels"].push_back(TunnelJsonValue(tunnel));
    }
    return body.dump();
}

std::string ServerTunnelJson(const TqTunnelSnapshot& tunnel) {
    return TunnelJsonValue(tunnel).dump();
}

std::string StatusJson(const char* status) {
    return nlohmann::json{{"status", status}}.dump();
}

bool DecodePathSegment(const std::string& encoded, std::string& decoded) {
    decoded.clear();
    decoded.reserve(encoded.size());
    for (size_t i = 0; i < encoded.size(); ++i) {
        const char ch = encoded[i];
        if (ch == '/') {
            return false;
        }
        if (ch != '%') {
            decoded.push_back(ch);
            continue;
        }
        if (i + 2 >= encoded.size()) {
            return false;
        }
        auto hex = [](char v) -> int {
            if (v >= '0' && v <= '9') return v - '0';
            if (v >= 'a' && v <= 'f') return v - 'a' + 10;
            if (v >= 'A' && v <= 'F') return v - 'A' + 10;
            return -1;
        };
        const int hi = hex(encoded[i + 1]);
        const int lo = hex(encoded[i + 2]);
        if (hi < 0 || lo < 0) {
            return false;
        }
        decoded.push_back(static_cast<char>((hi << 4) | lo));
        i += 2;
    }
    return !decoded.empty();
}

bool ParseServerTunnelAdminPath(const std::string& path, TqServerTunnelAdminPath& out) {
    out = TqServerTunnelAdminPath{};
    constexpr const char* TunnelPrefix = "/server/tunnels";
    constexpr size_t tunnelPrefixLen = std::char_traits<char>::length(TunnelPrefix);
    if (path.compare(0, tunnelPrefixLen + 1, "/server/tunnels/") != 0) {
        return false;
    }

    std::string tail = path.substr(tunnelPrefixLen + 1);
    if (tail.empty() || tail.find('/') != std::string::npos) {
        return false;
    }
    const size_t actionPos = tail.find(':');
    if (actionPos != std::string::npos) {
        out.Action = tail.substr(actionPos + 1);
        if (out.Action.empty()) {
            return false;
        }
        tail.resize(actionPos);
    }
    if (!DecodePathSegment(tail, out.TunnelId)) {
        return false;
    }
    return out.TunnelId.find('/') == std::string::npos;
}

} // namespace

std::string TqHandleServerAdmin(
    const TqHttpRequest& req,
    TqServerMetrics& metrics,
    uint64_t uptimeSeconds) {
    return TqHandleServerAdmin(req, metrics, uptimeSeconds, TqConfig{});
}

std::string TqHandleServerAdmin(
    const TqHttpRequest& req,
    TqServerMetrics& metrics,
    uint64_t uptimeSeconds,
    const TqConfig& runtimeConfig) {
    std::string response;
    if (TqHandleMemoryAdmin(req, response)) {
        return response;
    }

    if (req.Method == "GET" && req.Path == "/server/config") {
        std::vector<std::string> resolved;
        {
            std::lock_guard<std::mutex> guard(metrics.Lock);
            resolved = metrics.ResolvedListens;
        }
        return TqJsonResponse(200, TqServerRuntimeConfigJson(runtimeConfig, resolved, false));
    }
    if (req.Method == "PATCH" && req.Path == "/runtime/config") {
        TqConfig patchedConfig = runtimeConfig;
        std::string err;
        bool unsupported = false;
        if (!TqApplyRuntimeConfigPatch(req.Body, patchedConfig, err, unsupported)) {
            return TqJsonResponse(
                unsupported ? 503 : 400,
                unsupported ? TqStructuredErrorJson("not_supported", err) : ErrorJson(err));
        }
        return TqJsonResponse(200, TqRuntimeConfigJson(patchedConfig, false));
    }
    if (req.Method == "GET" && req.Path == "/diagnostics") {
        return TqJsonResponse(200, TqDiagnosticsJson(runtimeConfig));
    }
    if (req.Method == "PATCH" && req.Path == "/diagnostics") {
        TqConfig patchedConfig = runtimeConfig;
        std::string err;
        if (!TqApplyDiagnosticsPatch(req.Body, patchedConfig, err)) {
            return TqJsonResponse(400, ErrorJson(err));
        }
        if (!TqApplyDiagnosticsRuntime(patchedConfig)) {
            return TqJsonResponse(
                503,
                TqStructuredErrorJson("not_supported", "failed to apply diagnostics runtime state"));
        }
        return TqJsonResponse(200, TqDiagnosticsJson(patchedConfig));
    }
    if (req.Method == "GET" && req.Path == "/relay/metrics") {
        return TqJsonResponse(200, RelayMetricsJson());
    }
    if (req.Method == "GET" && req.Path == "/relay/active-relays") {
        return TqJsonResponse(200, TqRelayActiveRelaysJson());
    }
    if (req.Method == "GET" && req.Path.compare(0, 21, "/relay/active-relays/") == 0) {
        const std::string encodedRelayId = req.Path.substr(21);
        std::string relayId;
        if (encodedRelayId.empty() || encodedRelayId.find('/') != std::string::npos ||
            !DecodePathSegment(encodedRelayId, relayId) || relayId.empty()) {
            return TqJsonResponse(404, StructuredErrorJson("not_found", "not found"));
        }
        bool found = false;
        bool supported = false;
        const std::string body = TqRelayActiveRelayJson(relayId, found, supported);
        if (!supported) {
            return TqJsonResponse(503, StructuredErrorJson(
                "not_supported", "relay active relay detail is not supported by this backend"));
        }
        if (!found) {
            return TqJsonResponse(404, StructuredErrorJson("not_found", "not found"));
        }
        return TqJsonResponse(200, body);
    }
    if (req.Method == "GET" && req.Path == "/relay/workers") {
        return TqJsonResponse(200, TqRelayWorkersJson());
    }
    if (req.Method == "GET" && req.Path.compare(0, 15, "/relay/workers/") == 0) {
        const std::string encodedWorkerId = req.Path.substr(15);
        std::string workerId;
        if (encodedWorkerId.empty() || encodedWorkerId.find('/') != std::string::npos ||
            !DecodePathSegment(encodedWorkerId, workerId) || workerId.empty()) {
            return TqJsonResponse(404, StructuredErrorJson("not_found", "not found"));
        }
        bool found = false;
        bool supported = false;
        const std::string body = TqRelayWorkerDetailJson(workerId, found, supported);
        if (!supported) {
            return TqJsonResponse(503, StructuredErrorJson(
                "not_supported", "relay worker detail is not supported by this backend"));
        }
        if (!found) {
            return TqJsonResponse(404, StructuredErrorJson("not_found", "not found"));
        }
        return TqJsonResponse(200, body);
    }
    if (req.Method == "GET" && (req.Path == "/server" || req.Path == "/server/metrics")) {
        return TqJsonResponse(200, TqServerMetricsJson(metrics, uptimeSeconds));
    }
    if (req.Method == "GET" && req.Path == "/server/connections") {
        return TqJsonResponse(200, ServerConnectionsJson(TqSnapshotServerConnections()));
    }
    if (req.Path.compare(0, 20, "/server/connections/") == 0) {
        std::string tail = req.Path.substr(20);
        std::string action;
        const size_t actionPos = tail.find(':');
        if (actionPos != std::string::npos) {
            action = tail.substr(actionPos + 1);
            tail.resize(actionPos);
        }
        std::string connectionId;
        if (!DecodePathSegment(tail, connectionId)) {
            return TqJsonResponse(404, ErrorJson("not found"));
        }
        if (action.empty()) {
            if (req.Method != "GET") {
                return TqJsonResponse(404, ErrorJson("not found"));
            }
            TqServerConnectionSnapshot connection;
            if (!TqGetServerConnectionSnapshot(connectionId, connection)) {
                return TqJsonResponse(404, ErrorJson("not found"));
            }
            return TqJsonResponse(200, ServerConnectionJson(connection));
        }
        if (req.Method == "POST" && action == "abort-tunnels") {
            if (!TqAbortServerConnectionTunnels(connectionId)) {
                return TqJsonResponse(404, ErrorJson("not found"));
            }
            return TqJsonResponse(202, StatusJson("aborting"));
        }
        return TqJsonResponse(404, ErrorJson("not found"));
    }
    if (req.Path.compare(0, 16, "/server/tunnels/") == 0) {
        TqServerTunnelAdminPath tunnelPath;
        if (!ParseServerTunnelAdminPath(req.Path, tunnelPath)) {
            return TqJsonResponse(404, ErrorJson("not found"));
        }
        TqTunnelSnapshot tunnel;
        if (!TqGetTunnelSnapshot(tunnelPath.TunnelId, tunnel) || tunnel.Role != "server") {
            return TqJsonResponse(404, ErrorJson("not found"));
        }
        if (tunnelPath.Action.empty()) {
            if (req.Method == "GET") {
                return TqJsonResponse(200, ServerTunnelJson(tunnel));
            }
            if (req.Method == "DELETE") {
                if (!TqAbortTunnelById(tunnelPath.TunnelId)) {
                    return TqJsonResponse(404, ErrorJson("not found"));
                }
                return TqJsonResponse(202, StatusJson("aborting"));
            }
            return TqJsonResponse(404, ErrorJson("not found"));
        }
        if (req.Method != "POST") {
            return TqJsonResponse(404, ErrorJson("not found"));
        }
        if (tunnelPath.Action == "abort") {
            if (!TqAbortTunnelById(tunnelPath.TunnelId)) {
                return TqJsonResponse(404, ErrorJson("not found"));
            }
            return TqJsonResponse(202, StatusJson("aborting"));
        }
        if (tunnelPath.Action == "drain") {
            if (!TqDrainTunnelById(tunnelPath.TunnelId)) {
                return TqJsonResponse(404, ErrorJson("not found"));
            }
            return TqJsonResponse(202, StatusJson("draining"));
        }
        return TqJsonResponse(404, ErrorJson("not found"));
    }
    if (req.Method == "GET" && req.Path == "/server/tunnels") {
        return TqJsonResponse(200, ServerTunnelsJson());
    }
    return TqJsonResponse(404, ErrorJson("not found"));
}
