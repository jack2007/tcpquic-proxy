#include "server_admin.h"

#include "admin_config.h"
#include "admin_memory.h"
#include "quic_session.h"
#include "relay_metrics.h"
#include "tunnel_registry.h"

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
    std::ostringstream out;
    out << "{\"error\":\"" << err << "\"}";
    return out.str();
}

void AppendServerConnectionJson(std::ostringstream& out, const TqServerConnectionSnapshot& connection) {
    out << '{';
    TqAppendJsonString(out, "connection_id", connection.ConnectionId);
    out << ',';
    TqAppendJsonString(out, "remote_address", connection.RemoteAddress);
    out << ',';
    TqAppendJsonString(out, "state", connection.State);
    out << ",\"active_streams\":" << connection.ActiveStreams;
    out << ",\"total_streams\":" << connection.TotalStreams;
    out << ",\"active_tunnels\":" << connection.ActiveTunnels;
    out << ',';
    TqAppendJsonString(out, "last_error", connection.LastError);
    out << '}';
}

std::string ServerConnectionsJson(const std::vector<TqServerConnectionSnapshot>& connections) {
    std::ostringstream out;
    out << "{\"connections\":[";
    for (size_t i = 0; i < connections.size(); ++i) {
        if (i != 0) {
            out << ',';
        }
        AppendServerConnectionJson(out, connections[i]);
    }
    out << "]}";
    return out.str();
}

std::string ServerConnectionJson(const TqServerConnectionSnapshot& connection) {
    std::ostringstream out;
    AppendServerConnectionJson(out, connection);
    return out.str();
}

void AppendTunnelJson(std::ostringstream& out, const TqTunnelSnapshot& tunnel) {
    out << '{';
    TqAppendJsonString(out, "tunnel_id", tunnel.TunnelId);
    out << ',';
    TqAppendJsonString(out, "peer_id", tunnel.PeerId);
    out << ',';
    TqAppendJsonString(out, "connection_id", tunnel.ConnectionId);
    out << ',';
    TqAppendJsonString(out, "state", tunnel.State);
    out << ',';
    TqAppendJsonString(out, "target", tunnel.Target);
    out << ',';
    TqAppendJsonString(out, "role", tunnel.Role);
    out << ",\"duration_ms\":" << tunnel.DurationMs;
    out << ",\"active\":true";
    out << '}';
}

std::string ServerTunnelsJson() {
    const auto all = TqSnapshotTunnels();
    std::ostringstream out;
    out << "{\"tunnels\":[";
    bool first = true;
    for (const auto& tunnel : all) {
        if (tunnel.Role != "server") {
            continue;
        }
        if (!first) {
            out << ',';
        }
        first = false;
        AppendTunnelJson(out, tunnel);
    }
    out << "]}";
    return out.str();
}

std::string ServerTunnelJson(const TqTunnelSnapshot& tunnel) {
    std::ostringstream out;
    AppendTunnelJson(out, tunnel);
    return out.str();
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
    if (req.Method == "GET" && req.Path == "/diagnostics") {
        return TqJsonResponse(200, TqDiagnosticsJson(runtimeConfig));
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
            return TqJsonResponse(202, "{\"status\":\"aborting\"}");
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
                return TqJsonResponse(202, "{\"status\":\"aborting\"}");
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
            return TqJsonResponse(202, "{\"status\":\"aborting\"}");
        }
        if (tunnelPath.Action == "drain") {
            if (!TqDrainTunnelById(tunnelPath.TunnelId)) {
                return TqJsonResponse(404, ErrorJson("not found"));
            }
            return TqJsonResponse(202, "{\"status\":\"draining\"}");
        }
        return TqJsonResponse(404, ErrorJson("not found"));
    }
    if (req.Method == "GET" && req.Path == "/server/tunnels") {
        return TqJsonResponse(200, ServerTunnelsJson());
    }
    return TqJsonResponse(404, ErrorJson("not found"));
}
