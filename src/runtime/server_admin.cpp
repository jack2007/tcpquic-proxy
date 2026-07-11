#include "server_admin.h"

#include "admin_config.h"
#include "admin_memory.h"
#include "quic_session.h"
#include "relay_metrics.h"
#include "runtime_config_file_store.h"
#include "server_runtime_config.h"
#include "tunnel_registry.h"
#include "trace.h"
#include "terminal_convergence.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <charconv>
#include <functional>
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
        {"snapshot_complete", metrics.SnapshotComplete},
    };
    MergeObject(body, RelayMetricsJsonValue(metrics));
    return body.dump();
}

const char* TerminalBackendName(TqRelayBackend backend) {
    switch (backend) {
    case TqRelayBackendType::LinuxWorker: return "linux";
    case TqRelayBackendType::WindowsWorker: return "windows";
    case TqRelayBackendType::DarwinWorker: return "darwin";
    case TqRelayBackendType::None: return "none";
    }
    return "none";
}

bool ParseDecimalId(const std::string& text, uint64_t& value) {
    if (text.empty()) return false;
    value = 0;
    const auto result = std::from_chars(text.data(), text.data() + text.size(), value);
    return result.ec == std::errc{} && result.ptr == text.data() + text.size() && value != 0;
}

bool ParseTerminalRetentionPath(
    const std::string& path,
    TqTerminalRetentionFilter& filter) {
    constexpr const char* base = "/relay/terminal-retentions";
    const size_t baseLength = std::char_traits<char>::length(base);
    if (path.compare(0, baseLength, base) != 0) return false;
    if (path.size() == baseLength) return true;
    if (path[baseLength] != '?') return false;
    size_t offset = baseLength + 1;
    if (offset == path.size()) return false;
    while (offset < path.size()) {
        const size_t end = path.find('&', offset);
        const std::string item = path.substr(offset, end - offset);
        const size_t equal = item.find('=');
        if (equal == std::string::npos || equal == 0 || equal + 1 == item.size()) return false;
        const std::string key = item.substr(0, equal);
        const std::string value = item.substr(equal + 1);
        if (key == "backend") {
            if (filter.Backend != TqRelayBackendType::None) return false;
            if (value == "linux") filter.Backend = TqRelayBackendType::LinuxWorker;
            else if (value == "windows") filter.Backend = TqRelayBackendType::WindowsWorker;
            else if (value == "darwin") filter.Backend = TqRelayBackendType::DarwinWorker;
            else return false;
        } else if (key == "connection_id") {
            if (filter.ConnectionId != 0 || !ParseDecimalId(value, filter.ConnectionId)) return false;
        } else if (key == "tunnel_id") {
            if (filter.TunnelId != 0 || !ParseDecimalId(value, filter.TunnelId)) return false;
        } else if (key == "terminal_phase") {
            if (filter.HasPhase) return false;
            filter.HasPhase = true;
            if (value == "active") filter.Phase = TerminalPhase::Active;
            else if (value == "shutdown_reserved") filter.Phase = TerminalPhase::ShutdownReserved;
            else if (value == "shutdown_submitted") filter.Phase = TerminalPhase::ShutdownSubmitted;
            else if (value == "terminal_observed") filter.Phase = TerminalPhase::TerminalObserved;
            else if (value == "closed") filter.Phase = TerminalPhase::Closed;
            else return false;
        } else {
            return false;
        }
        if (end == std::string::npos) break;
        offset = end + 1;
        if (offset == path.size()) return false;
    }
    return true;
}

std::string TerminalRetentionsJson(const TqTerminalRetentionFilter& filter) {
    const auto snapshots = TqSnapshotTerminalRetentions(filter);
    nlohmann::json retentions = nlohmann::json::array();
    uint64_t oldestAgeMs = 0;
    for (const auto& snapshot : snapshots) {
        oldestAgeMs = std::max(oldestAgeMs, snapshot.RetainedAgeMs);
        retentions.push_back({
            {"stream_id", snapshot.Identity.StreamId},
            {"tunnel_id", snapshot.Identity.TunnelId},
            {"connection_id", snapshot.Identity.ConnectionId},
            {"connection_generation", snapshot.Identity.ConnectionGeneration},
            {"role", snapshot.Identity.Role == TqTunnelRole::ClientOpen ? "client" : "server"},
            {"backend", TerminalBackendName(snapshot.Identity.Backend)},
            {"terminal_phase", TqTerminalPhaseName(snapshot.Phase)},
            {"retained_age_ms", snapshot.RetainedAgeMs},
            {"shutdown_intent", TqTerminalShutdownIntentName(snapshot.ShutdownIntent)},
            {"shutdown_status", snapshot.Phase == TerminalPhase::ShutdownSubmitted ? "pending" : "none"},
            {"shutdown_attempt", snapshot.ShutdownAttempt},
            {"shutdown_submitted_at_ms", snapshot.ShutdownSubmittedAtMs},
            {"terminal_observed_at_ms", snapshot.TerminalObservedAtMs},
            {"last_stream_event", TqTerminalEventName(snapshot.LastStreamEvent)},
            {"in_tunnel_registry", snapshot.InTunnelRegistry},
            {"relay_active", snapshot.RelayActive},
            {"tcp_valid", snapshot.TcpValid},
            {"watchdog_state", TqTerminalWatchdogStateName(snapshot.Watchdog)},
            {"connection_escalated", snapshot.ConnectionEscalated},
        });
    }
    return nlohmann::json{
        {"retentions", std::move(retentions)},
        {"count", snapshots.size()},
        {"oldest_age_ms", oldestAgeMs},
    }.dump();
}

nlohmann::json ServerConnectionJsonValue(const TqServerConnectionSnapshot& connection) {
    return {
        {"connection_id", connection.ConnectionId},
        {"client_name", connection.ClientName},
        {"remote_address", connection.RemoteAddress},
        {"state", connection.State},
        {"active_streams", connection.ActiveStreams},
        {"total_streams", connection.TotalStreams},
        {"active_tunnels", connection.ActiveTunnels},
        {"last_error", connection.LastError},
        {"encryption", connection.Encryption},
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

std::string HandleServerAdmin(
    const TqHttpRequest& req,
    TqServerMetrics& metrics,
    uint64_t uptimeSeconds,
    const TqConfig& runtimeConfig,
    TqServerRuntimeConfigState* runtimeConfigState,
    TqRuntimeConfigFileStore* runtimeConfigStore,
    const TqServerAdminUpdateAcl& updateAcl) {
    std::string response;
    if (TqHandleMemoryAdmin(req, response)) {
        return response;
    }

    if (req.Method == "GET" && req.Path == "/server/config") {
        const TqConfig snapshot = runtimeConfigState != nullptr ?
            runtimeConfigState->SnapshotConfig() :
            runtimeConfig;
        std::vector<std::string> resolved;
        {
            std::lock_guard<std::mutex> guard(metrics.Lock);
            resolved = metrics.ResolvedListens;
        }
        return TqJsonResponse(200, TqServerRuntimeConfigJson(snapshot, resolved, false));
    }
    if (req.Method == "PATCH" && req.Path == "/server/config") {
        if (runtimeConfigState == nullptr) {
            return TqJsonResponse(
                503,
                TqStructuredErrorJson(
                    "not_supported",
                    "server runtime config state is not available"));
        }

        TqServerConfigPatch patch;
        std::string err;
        bool unsupported = false;
        if (!TqParseServerConfigPatch(req.Body, patch, err, unsupported)) {
            return TqJsonResponse(
                unsupported ? 503 : 400,
                unsupported ? TqStructuredErrorJson("not_supported", err) : ErrorJson(err));
        }

        TqConfig nextConfig;
        TqAcl nextAcl;
        if (!runtimeConfigState->BuildAclPatch(patch, nextConfig, nextAcl, err)) {
            return TqJsonResponse(400, ErrorJson(err));
        }

        if (nextConfig.ConfigPath.empty()) {
            return TqJsonResponse(
                503,
                TqStructuredErrorJson(
                    "not_supported",
                    "server runtime config path is not available"));
        }
        if (runtimeConfigStore == nullptr) {
            return TqJsonResponse(
                503,
                TqStructuredErrorJson(
                    "not_supported",
                    "server runtime config store is not available"));
        }

        if (!runtimeConfigStore->PatchServerAcl(
                nextConfig.AllowTargets,
                nextConfig.DenyTargets,
                err)) {
            return TqJsonResponse(503, ErrorJson(err));
        }
        if (!updateAcl || !updateAcl(nextAcl)) {
            return TqJsonResponse(
                503,
                TqStructuredErrorJson(
                    "not_supported",
                    "failed to apply server ACL runtime state"));
        }

        runtimeConfigState->Commit(nextConfig, nextAcl);
        std::vector<std::string> resolved;
        {
            std::lock_guard<std::mutex> guard(metrics.Lock);
            resolved = metrics.ResolvedListens;
        }
        return TqJsonResponse(200, TqServerRuntimeConfigJson(nextConfig, resolved, false));
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
    if (req.Method == "GET" &&
        req.Path.compare(0, 26, "/relay/terminal-retentions") == 0) {
        TqTerminalRetentionFilter filter{};
        if (!ParseTerminalRetentionPath(req.Path, filter)) {
            return TqJsonResponse(400, ErrorJson("invalid_filter"));
        }
        return TqJsonResponse(200, TerminalRetentionsJson(filter));
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
        TqRelayWorkerLookupStatus status = TqRelayWorkerLookupStatus::SnapshotUnavailable;
        const std::string body = TqRelayWorkerDetailJson(workerId, status);
        if (status == TqRelayWorkerLookupStatus::NotFound) {
            return TqJsonResponse(404, StructuredErrorJson("not_found", "not found"));
        }
        if (status == TqRelayWorkerLookupStatus::SnapshotUnavailable) {
            return TqJsonResponse(503, StructuredErrorJson(
                "snapshot_unavailable", "relay worker snapshot is unavailable"));
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

} // namespace

std::string TqHandleServerAdmin(
    const TqHttpRequest& req,
    TqServerMetrics& metrics,
    uint64_t uptimeSeconds,
    TqServerRuntimeConfigState& runtimeConfigState,
    TqRuntimeConfigFileStore* runtimeConfigStore,
    TqServerAdminUpdateAcl updateAcl) {
    return HandleServerAdmin(
        req,
        metrics,
        uptimeSeconds,
        runtimeConfigState.SnapshotConfig(),
        &runtimeConfigState,
        runtimeConfigStore,
        updateAcl);
}

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
    return HandleServerAdmin(
        req,
        metrics,
        uptimeSeconds,
        runtimeConfig,
        nullptr,
        nullptr,
        TqServerAdminUpdateAcl{});
}
