#include "server_metrics.h"

#include "relay_metrics.h"

#include <sstream>

namespace {

std::string TqJsonStringValue(const std::string& value) {
    std::string out;
    for (char ch : value) {
        switch (ch) {
        case '"': out += "\\\""; break;
        case '\\': out += "\\\\"; break;
        case '\b': out += "\\b"; break;
        case '\f': out += "\\f"; break;
        case '\n': out += "\\n"; break;
        case '\r': out += "\\r"; break;
        case '\t': out += "\\t"; break;
        default:
            if (static_cast<unsigned char>(ch) < 0x20) {
                static const char Hex[] = "0123456789abcdef";
                const unsigned char v = static_cast<unsigned char>(ch);
                out += "\\u00";
                out.push_back(Hex[v >> 4]);
                out.push_back(Hex[v & 0x0f]);
            } else {
                out.push_back(ch);
            }
            break;
        }
    }
    return out;
}

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

void TqAppendPortForwardsJson(std::ostringstream& out, const std::vector<TqPortForwardConfig>& forwards) {
    out << ",\"port_forwards\":[";
    for (size_t i = 0; i < forwards.size(); ++i) {
        if (i != 0) {
            out << ',';
        }
        out << '{';
        TqAppendJsonString(out, "listen", forwards[i].Listen);
        out << ',';
        TqAppendJsonString(out, "target", TqPortForwardTargetText(forwards[i]));
        out << '}';
    }
    out << ']';
}

void TqAppendStringArrayJson(
    std::ostringstream& out,
    const char* name,
    const std::vector<std::string>& values) {
    out << ",\"" << name << "\":[";
    for (size_t i = 0; i < values.size(); ++i) {
        if (i != 0) {
            out << ',';
        }
        out << '"' << TqJsonStringValue(values[i]) << '"';
    }
    out << ']';
}

} // namespace

std::string TqClientMetricsJson(const TqClientMetrics& metrics, uint64_t uptimeSeconds) {
    std::ostringstream out;
    out << '{';
    TqAppendJsonString(out, "role", "client");
    out << ',';
    TqAppendJsonString(out, "status",
        metrics.LastError.empty() && metrics.ConnectedConnections > 0 ? "healthy" : "degraded");
    out << ',';
    TqAppendJsonString(out, "quic_peer", metrics.QuicPeer);
    out << ',';
    TqAppendJsonString(out, "socks_listen", metrics.SocksListen);
    out << ',';
    TqAppendJsonString(out, "http_listen", metrics.HttpListen);
    TqAppendPortForwardsJson(out, metrics.PortForwards);
    out << ",\"uptime_seconds\":" << uptimeSeconds;
    out << ",\"connection_count\":" << metrics.ConnectionCount;
    out << ",\"connected_connections\":" << metrics.ConnectedConnections;
    TqAppendRelayMetricsJson(out, TqSnapshotRelayMetrics());
    out << ',';
    TqAppendJsonString(out, "last_error", metrics.LastError);
    out << '}';
    return out.str();
}

std::string TqClientHealthJson(const TqClientMetrics& metrics, uint64_t uptimeSeconds) {
    return TqClientMetricsJson(metrics, uptimeSeconds);
}

std::string TqServerMetricsJson(const TqServerMetrics& metrics, uint64_t uptimeSeconds) {
    std::lock_guard<std::mutex> guard(metrics.Lock);
    std::ostringstream out;
    out << '{';
    TqAppendJsonString(out, "role", "server");
    out << ',';
    TqAppendJsonString(out, "status", metrics.LastError.empty() ? "healthy" : "degraded");
    out << ',';
    TqAppendJsonString(out, "listen", metrics.Listen);
    TqAppendStringArrayJson(out, "resolved_listens", metrics.ResolvedListens);
    out << ",\"uptime_seconds\":" << uptimeSeconds;
    out << ",\"accepted_connections\":" << metrics.AcceptedConnections.load();
    out << ",\"active_streams\":" << metrics.ActiveStreams.load();
    out << ",\"total_streams\":" << metrics.TotalStreams.load();
    out << ",\"acl_denied\":" << metrics.AclDenied.load();
    out << ",\"tcp_dialing\":" << metrics.TcpDialing.load();
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
    TqAppendRelayMetricsJson(out, relayMetrics);
    out << ',';
    TqAppendJsonString(out, "last_error", metrics.LastError);
    out << '}';
    return out.str();
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
