#include "server_metrics.h"

#include "relay_metrics.h"

#include <nlohmann/json.hpp>

#include <sstream>

namespace {

std::string TqPortForwardTargetText(const TqPortForwardConfig& forward) {
    std::ostringstream out;
    if (forward.TargetHost.find(':') != std::string::npos) {
        out << '[' << forward.TargetHost << ']';
    } else {
        out << forward.TargetHost;
    }
    out << ':' << forward.TargetPort;
    return out.str();
}

nlohmann::json TqPortForwardsJsonValue(const std::vector<TqPortForwardConfig>& forwards) {
    nlohmann::json values = nlohmann::json::array();
    for (const auto& forward : forwards) {
        values.push_back({
            {"listen", forward.Listen},
            {"target", TqPortForwardTargetText(forward)},
        });
    }
    return values;
}

nlohmann::json TqRelayMetricsJsonValue(const TqRelayMetricsSnapshot& metrics) {
    return nlohmann::json::parse(TqRelayMetricsFieldsJson(metrics));
}

void TqMergeObject(nlohmann::json& target, const nlohmann::json& source) {
    for (const auto& item : source.items()) {
        target[item.key()] = item.value();
    }
}

} // namespace

std::string TqClientMetricsJson(const TqClientMetrics& metrics, uint64_t uptimeSeconds) {
    nlohmann::json body{
        {"role", "client"},
        {"status", metrics.LastError.empty() && metrics.ConnectedConnections > 0 ? "healthy" : "degraded"},
        {"quic_peer", metrics.QuicPeer},
        {"socks_listen", metrics.SocksListen},
        {"http_listen", metrics.HttpListen},
        {"port_forwards", TqPortForwardsJsonValue(metrics.PortForwards)},
        {"uptime_seconds", uptimeSeconds},
        {"connection_count", metrics.ConnectionCount},
        {"connected_connections", metrics.ConnectedConnections},
        {"last_error", metrics.LastError},
    };
    TqMergeObject(body, TqRelayMetricsJsonValue(TqSnapshotRelayMetrics()));
    return body.dump();
}

std::string TqClientHealthJson(const TqClientMetrics& metrics, uint64_t uptimeSeconds) {
    return TqClientMetricsJson(metrics, uptimeSeconds);
}

std::string TqServerMetricsJson(const TqServerMetrics& metrics, uint64_t uptimeSeconds) {
    std::lock_guard<std::mutex> guard(metrics.Lock);
    nlohmann::json body{
        {"role", "server"},
        {"status", metrics.LastError.empty() ? "healthy" : "degraded"},
        {"listen", metrics.Listen},
        {"resolved_listens", metrics.ResolvedListens},
        {"uptime_seconds", uptimeSeconds},
        {"accepted_connections", metrics.AcceptedConnections.load()},
        {"active_streams", metrics.ActiveStreams.load()},
        {"total_streams", metrics.TotalStreams.load()},
        {"acl_denied", metrics.AclDenied.load()},
        {"tcp_dialing", metrics.TcpDialing.load()},
        {"last_error", metrics.LastError},
    };
    auto relayMetrics = TqSnapshotRelayMetrics();
#if !defined(__linux__)
    relayMetrics.Wakeups = metrics.LinuxRelayWakeups;
    relayMetrics.EventsProcessed = metrics.LinuxRelayEventsProcessed;
    relayMetrics.PendingEvents = metrics.LinuxRelayPendingEvents;
    relayMetrics.PendingBytes = metrics.LinuxRelayPendingBytes;
    relayMetrics.TcpReadBytes = metrics.LinuxRelayTcpReadBytes;
    relayMetrics.TcpWriteBytes = metrics.LinuxRelayTcpWriteBytes;
    relayMetrics.ReadDisabledCount = metrics.LinuxRelayReadDisabledCount;
#endif
    TqMergeObject(body, TqRelayMetricsJsonValue(relayMetrics));
    return body.dump();
}

std::string TqServerHealthJson(const TqServerMetrics& metrics, uint64_t uptimeSeconds) {
    return TqServerMetricsJson(metrics, uptimeSeconds);
}

void TqServerMetricsConnectionAccepted(TqServerMetrics& metrics) {
    metrics.AcceptedConnections.fetch_add(1);
}

void TqServerMetricsStreamStarted(TqServerMetrics& metrics) {
    metrics.ActiveStreams.fetch_add(1);
    metrics.TotalStreams.fetch_add(1);
}

void TqServerMetricsStreamFinished(TqServerMetrics& metrics) {
    metrics.ActiveStreams.fetch_sub(1);
}
