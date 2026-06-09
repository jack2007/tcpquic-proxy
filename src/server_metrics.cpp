#include "server_metrics.h"

#if defined(__linux__)
#include "linux_relay_worker.h"
#endif

#include <sstream>

namespace {

std::string TqJsonEscape(const std::string& value) {
    std::string out;
    out.reserve(value.size() + 2);
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

void TqAppendJsonString(std::ostringstream& out, const char* name, const std::string& value) {
    out << '"' << name << "\":\"" << TqJsonEscape(value) << '"';
}

} // namespace

std::string TqServerMetricsJson(const TqServerMetrics& metrics, uint64_t uptimeSeconds) {
    std::lock_guard<std::mutex> guard(metrics.Lock);
    std::ostringstream out;
    out << '{';
    TqAppendJsonString(out, "role", "server");
    out << ',';
    TqAppendJsonString(out, "status", metrics.LastError.empty() ? "healthy" : "degraded");
    out << ',';
    TqAppendJsonString(out, "listen", metrics.Listen);
    out << ",\"uptime_seconds\":" << uptimeSeconds;
    out << ",\"accepted_connections\":" << metrics.AcceptedConnections.load();
    out << ",\"active_streams\":" << metrics.ActiveStreams.load();
    out << ",\"total_streams\":" << metrics.TotalStreams.load();
    out << ",\"acl_denied\":" << metrics.AclDenied.load();

#if defined(__linux__)
    const auto linuxRelay = TqLinuxRelayRuntime::Instance().Snapshot();
    const char* linuxRelayBackend = "worker";
    const uint64_t linuxRelayWakeups = linuxRelay.WakeupWrites;
    const uint64_t linuxRelayEventsProcessed = linuxRelay.EventsProcessed;
    const uint64_t linuxRelayPendingEvents = linuxRelay.PendingEvents;
    const uint64_t linuxRelayPendingBytes = linuxRelay.PendingBytes;
    const uint64_t linuxRelayTcpReadBytes = linuxRelay.TcpReadBytes;
    const uint64_t linuxRelayTcpWriteBytes = linuxRelay.TcpWriteBytes;
    const uint64_t linuxRelayReadDisabledCount = linuxRelay.ReadDisabledCount;
    const uint64_t linuxRelayCompressedTcpBytes = linuxRelay.CompressedTcpBytes;
    const uint64_t linuxRelayDecompressedTcpBytes = linuxRelay.DecompressedTcpBytes;
    const uint64_t linuxRelayErrors = linuxRelay.Errors;
#else
    const char* linuxRelayBackend = "unsupported";
    const uint64_t linuxRelayWakeups = metrics.LinuxRelayWakeups;
    const uint64_t linuxRelayEventsProcessed = metrics.LinuxRelayEventsProcessed;
    const uint64_t linuxRelayPendingEvents = metrics.LinuxRelayPendingEvents;
    const uint64_t linuxRelayPendingBytes = metrics.LinuxRelayPendingBytes;
    const uint64_t linuxRelayTcpReadBytes = metrics.LinuxRelayTcpReadBytes;
    const uint64_t linuxRelayTcpWriteBytes = metrics.LinuxRelayTcpWriteBytes;
    const uint64_t linuxRelayReadDisabledCount = metrics.LinuxRelayReadDisabledCount;
    const uint64_t linuxRelayCompressedTcpBytes = 0;
    const uint64_t linuxRelayDecompressedTcpBytes = 0;
    const uint64_t linuxRelayErrors = 0;
#endif

    out << ",\"linux_relay_wakeups\":" << linuxRelayWakeups;
    out << ",\"linux_relay_events_processed\":" << linuxRelayEventsProcessed;
    out << ",\"linux_relay_pending_events\":" << linuxRelayPendingEvents;
    out << ",\"linux_relay_pending_bytes\":" << linuxRelayPendingBytes;
    out << ",\"linux_relay_tcp_read_bytes\":" << linuxRelayTcpReadBytes;
    out << ",\"linux_relay_tcp_write_bytes\":" << linuxRelayTcpWriteBytes;
    out << ",\"linux_relay_read_disabled_count\":" << linuxRelayReadDisabledCount;
    out << ',';
    TqAppendJsonString(out, "linux_relay_backend", linuxRelayBackend);
    out << ",\"linux_relay_compressed_tcp_bytes\":" << linuxRelayCompressedTcpBytes;
    out << ",\"linux_relay_decompressed_tcp_bytes\":" << linuxRelayDecompressedTcpBytes;
    out << ",\"linux_relay_errors\":" << linuxRelayErrors;
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
