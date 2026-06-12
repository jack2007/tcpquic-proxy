#include "server_metrics.h"

#if defined(__linux__)
#include "linux_relay_worker.h"
#endif

#include <array>
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

struct TqRelayMetricsSnapshot {
    static constexpr size_t SizeBucketCount = 6;

    const char* Backend{"unsupported"};
    uint64_t Wakeups{0};
    uint64_t EventsProcessed{0};
    uint64_t PendingEvents{0};
    uint64_t PendingBytes{0};
    uint64_t TcpReadBytes{0};
    uint64_t TcpWriteBytes{0};
    uint64_t ReadDisabledCount{0};
    uint64_t CompressedTcpBytes{0};
    uint64_t DecompressedTcpBytes{0};
    uint64_t InlineSendmsgCalls{0};
    uint64_t InlineWriteBytes{0};
    uint64_t InlineFullSuccess{0};
    uint64_t InlinePartial{0};
    uint64_t InlineEagain{0};
    uint64_t InlineBudgetExceeded{0};
    uint64_t InlineFallbackBytes{0};
    uint64_t InlineCallbackCount{0};
    uint64_t InlineCallbackAvgUsec{0};
    uint64_t InlineCallbackMaxUsec{0};
    std::array<uint64_t, SizeBucketCount> QuicSendBytesBuckets{};
    std::array<uint64_t, SizeBucketCount> QuicReceiveBytesBuckets{};
    uint64_t Errors{0};
};

TqRelayMetricsSnapshot TqSnapshotRelayMetrics(const TqServerMetrics* fallback = nullptr) {
    TqRelayMetricsSnapshot metrics;
#if defined(__linux__)
    (void)fallback;
    const auto linuxRelay = TqLinuxRelayRuntime::Instance().Snapshot();
    metrics.Backend = "worker";
    metrics.Wakeups = linuxRelay.WakeupWrites;
    metrics.EventsProcessed = linuxRelay.EventsProcessed;
    metrics.PendingEvents = linuxRelay.PendingEvents;
    metrics.PendingBytes = linuxRelay.PendingBytes;
    metrics.TcpReadBytes = linuxRelay.TcpReadBytes;
    metrics.TcpWriteBytes = linuxRelay.TcpWriteBytes;
    metrics.ReadDisabledCount = linuxRelay.ReadDisabledCount;
    metrics.CompressedTcpBytes = linuxRelay.CompressedTcpBytes;
    metrics.DecompressedTcpBytes = linuxRelay.DecompressedTcpBytes;
    metrics.InlineSendmsgCalls = linuxRelay.InlineSendmsgCalls;
    metrics.InlineWriteBytes = linuxRelay.InlineWriteBytes;
    metrics.InlineFullSuccess = linuxRelay.InlineFullSuccessCount;
    metrics.InlinePartial = linuxRelay.InlinePartialCount;
    metrics.InlineEagain = linuxRelay.InlineEagainCount;
    metrics.InlineBudgetExceeded = linuxRelay.InlineBudgetExceededCount;
    metrics.InlineFallbackBytes = linuxRelay.InlineFallbackBytes;
    metrics.InlineCallbackCount = linuxRelay.InlineCallbackCount;
    metrics.InlineCallbackAvgUsec =
        linuxRelay.InlineCallbackCount == 0 ? 0 : linuxRelay.InlineCallbackTotalUsec / linuxRelay.InlineCallbackCount;
    metrics.InlineCallbackMaxUsec = linuxRelay.InlineCallbackMaxUsec;
    metrics.QuicSendBytesBuckets = linuxRelay.QuicSendBytesBuckets;
    metrics.QuicReceiveBytesBuckets = linuxRelay.QuicReceiveBytesBuckets;
    metrics.Errors = linuxRelay.Errors;
#else
    if (fallback != nullptr) {
        metrics.Wakeups = fallback->LinuxRelayWakeups;
        metrics.EventsProcessed = fallback->LinuxRelayEventsProcessed;
        metrics.PendingEvents = fallback->LinuxRelayPendingEvents;
        metrics.PendingBytes = fallback->LinuxRelayPendingBytes;
        metrics.TcpReadBytes = fallback->LinuxRelayTcpReadBytes;
        metrics.TcpWriteBytes = fallback->LinuxRelayTcpWriteBytes;
        metrics.ReadDisabledCount = fallback->LinuxRelayReadDisabledCount;
    }
#endif
    return metrics;
}

void TqAppendSizeBucketsJson(
    std::ostringstream& out,
    const char* name,
    const std::array<uint64_t, TqRelayMetricsSnapshot::SizeBucketCount>& buckets) {
    out << ",\"" << name << "\":{";
    out << "\"le_16k\":" << buckets[0];
    out << ",\"le_64k\":" << buckets[1];
    out << ",\"le_128k\":" << buckets[2];
    out << ",\"le_256k\":" << buckets[3];
    out << ",\"le_1m\":" << buckets[4];
    out << ",\"gt_1m\":" << buckets[5];
    out << '}';
}

void TqAppendRelayMetricsJson(std::ostringstream& out, const TqRelayMetricsSnapshot& metrics) {
    out << ",\"linux_relay_wakeups\":" << metrics.Wakeups;
    out << ",\"linux_relay_events_processed\":" << metrics.EventsProcessed;
    out << ",\"linux_relay_pending_events\":" << metrics.PendingEvents;
    out << ",\"linux_relay_pending_bytes\":" << metrics.PendingBytes;
    out << ",\"linux_relay_tcp_read_bytes\":" << metrics.TcpReadBytes;
    out << ",\"linux_relay_tcp_write_bytes\":" << metrics.TcpWriteBytes;
    out << ",\"linux_relay_read_disabled_count\":" << metrics.ReadDisabledCount;
    out << ',';
    TqAppendJsonString(out, "linux_relay_backend", metrics.Backend);
    out << ",\"linux_relay_compressed_tcp_bytes\":" << metrics.CompressedTcpBytes;
    out << ",\"linux_relay_decompressed_tcp_bytes\":" << metrics.DecompressedTcpBytes;
    out << ",\"linux_relay_inline_sendmsg_calls\":" << metrics.InlineSendmsgCalls;
    out << ",\"linux_relay_inline_write_bytes\":" << metrics.InlineWriteBytes;
    out << ",\"linux_relay_inline_full_success\":" << metrics.InlineFullSuccess;
    out << ",\"linux_relay_inline_partial\":" << metrics.InlinePartial;
    out << ",\"linux_relay_inline_eagain\":" << metrics.InlineEagain;
    out << ",\"linux_relay_inline_budget_exceeded\":" << metrics.InlineBudgetExceeded;
    out << ",\"linux_relay_inline_fallback_bytes\":" << metrics.InlineFallbackBytes;
    out << ",\"linux_relay_inline_callback_count\":" << metrics.InlineCallbackCount;
    out << ",\"linux_relay_inline_callback_avg_usec\":" << metrics.InlineCallbackAvgUsec;
    out << ",\"linux_relay_inline_callback_max_usec\":" << metrics.InlineCallbackMaxUsec;
    TqAppendSizeBucketsJson(out, "linux_relay_quic_send_bytes_buckets", metrics.QuicSendBytesBuckets);
    TqAppendSizeBucketsJson(out, "linux_relay_quic_receive_bytes_buckets", metrics.QuicReceiveBytesBuckets);
    out << ",\"linux_relay_errors\":" << metrics.Errors;
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
    out << ",\"uptime_seconds\":" << uptimeSeconds;
    out << ",\"accepted_connections\":" << metrics.AcceptedConnections.load();
    out << ",\"active_streams\":" << metrics.ActiveStreams.load();
    out << ",\"total_streams\":" << metrics.TotalStreams.load();
    out << ",\"acl_denied\":" << metrics.AclDenied.load();

    TqAppendRelayMetricsJson(out, TqSnapshotRelayMetrics(&metrics));
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
