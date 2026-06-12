#include "relay_metrics.h"

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

} // namespace

void TqAppendJsonString(std::ostringstream& out, const char* name, const std::string& value) {
    out << '"' << name << "\":\"" << TqJsonEscape(value) << '"';
}

TqRelayMetricsSnapshot TqSnapshotRelayMetrics() {
    TqRelayMetricsSnapshot metrics;
#if defined(__linux__)
    const auto snapshot = TqLinuxRelayRuntime::Instance().Snapshot();
    metrics.Backend = "worker";
    metrics.Wakeups = snapshot.WakeupWrites;
    metrics.EventsProcessed = snapshot.EventsProcessed;
    metrics.PendingEvents = snapshot.PendingEvents;
    metrics.PendingBytes = snapshot.PendingBytes;
    metrics.TcpReadBytes = snapshot.TcpReadBytes;
    metrics.TcpWriteBytes = snapshot.TcpWriteBytes;
    metrics.MaxTcpReadIovUsed = snapshot.MaxTcpReadIovUsed;
    metrics.MaxTcpWriteIovUsed = snapshot.MaxTcpWriteIovUsed;
    metrics.TcpWriteSendmsgCalls = snapshot.TcpWriteSendmsgCalls;
    metrics.MaxTcpWriteSendmsgBytes = snapshot.MaxTcpWriteSendmsgBytes;
    metrics.TcpWriteEagainCount = snapshot.TcpWriteEagainCount;
    metrics.TcpWritePartialCount = snapshot.TcpWritePartialCount;
    metrics.ReadDisabledCount = snapshot.ReadDisabledCount;
    metrics.CompressedTcpBytes = snapshot.CompressedTcpBytes;
    metrics.DecompressedTcpBytes = snapshot.DecompressedTcpBytes;
    metrics.DeferredReceiveCompleteBytes = snapshot.DeferredReceiveCompleteBytes;
    metrics.DeferredReceiveCompletes = snapshot.DeferredReceiveCompletes;
    metrics.DeferredReceiveCompletionFlushes = snapshot.DeferredReceiveCompletionFlushes;
    metrics.MaxPendingQuicReceiveBytes = snapshot.MaxPendingQuicReceiveBytes;
    metrics.MaxPendingQuicReceiveQueue = snapshot.MaxPendingQuicReceiveQueue;
    metrics.QuicReceivePausedCount = snapshot.QuicReceivePausedCount;
    metrics.QuicReceiveResumedCount = snapshot.QuicReceiveResumedCount;
    metrics.Errors = snapshot.Errors;
    metrics.EventQueueFullErrors = snapshot.EventQueueFullErrors;
    metrics.TcpReadBufferAcquireFailures = snapshot.TcpReadBufferAcquireFailures;
    metrics.TcpToQuicCompressFailures = snapshot.TcpToQuicCompressFailures;
    metrics.TcpToQuicBufferAcquireFailures = snapshot.TcpToQuicBufferAcquireFailures;
    metrics.QuicSendFailures = snapshot.QuicSendFailures;
    metrics.QuicReceiveIngressBufferAcquireFailures =
        snapshot.QuicReceiveIngressBufferAcquireFailures;
    metrics.QuicReceiveViewFailures = snapshot.QuicReceiveViewFailures;
    metrics.QuicReceiveDecompressFailures = snapshot.QuicReceiveDecompressFailures;
    metrics.QuicReceiveTcpBufferAcquireFailures = snapshot.QuicReceiveTcpBufferAcquireFailures;
    metrics.TcpWriteHardErrors = snapshot.TcpWriteHardErrors;
#endif
    return metrics;
}

void TqAppendRelayMetricsJson(std::ostringstream& out, const TqRelayMetricsSnapshot& metrics) {
    out << ",\"linux_relay_wakeups\":" << metrics.Wakeups;
    out << ",\"linux_relay_events_processed\":" << metrics.EventsProcessed;
    out << ",\"linux_relay_pending_events\":" << metrics.PendingEvents;
    out << ",\"linux_relay_pending_bytes\":" << metrics.PendingBytes;
    out << ",\"linux_relay_tcp_read_bytes\":" << metrics.TcpReadBytes;
    out << ",\"linux_relay_tcp_write_bytes\":" << metrics.TcpWriteBytes;
    out << ",\"linux_relay_max_tcp_read_iov_used\":" << metrics.MaxTcpReadIovUsed;
    out << ",\"linux_relay_max_tcp_write_iov_used\":" << metrics.MaxTcpWriteIovUsed;
    out << ",\"linux_relay_tcp_write_sendmsg_calls\":" << metrics.TcpWriteSendmsgCalls;
    out << ",\"linux_relay_max_tcp_write_sendmsg_bytes\":" << metrics.MaxTcpWriteSendmsgBytes;
    out << ",\"linux_relay_tcp_write_eagain_count\":" << metrics.TcpWriteEagainCount;
    out << ",\"linux_relay_tcp_write_partial_count\":" << metrics.TcpWritePartialCount;
    out << ",\"linux_relay_read_disabled_count\":" << metrics.ReadDisabledCount;
    out << ',';
    TqAppendJsonString(out, "linux_relay_backend", metrics.Backend);
    out << ",\"linux_relay_compressed_tcp_bytes\":" << metrics.CompressedTcpBytes;
    out << ",\"linux_relay_decompressed_tcp_bytes\":" << metrics.DecompressedTcpBytes;
    out << ",\"linux_relay_deferred_receive_complete_bytes\":" << metrics.DeferredReceiveCompleteBytes;
    out << ",\"linux_relay_deferred_receive_completes\":" << metrics.DeferredReceiveCompletes;
    out << ",\"linux_relay_deferred_receive_completion_flushes\":" << metrics.DeferredReceiveCompletionFlushes;
    out << ",\"linux_relay_max_pending_quic_receive_bytes\":" << metrics.MaxPendingQuicReceiveBytes;
    out << ",\"linux_relay_max_pending_quic_receive_queue\":" << metrics.MaxPendingQuicReceiveQueue;
    out << ",\"linux_relay_quic_receive_paused_count\":" << metrics.QuicReceivePausedCount;
    out << ",\"linux_relay_quic_receive_resumed_count\":" << metrics.QuicReceiveResumedCount;
    out << ",\"linux_relay_errors\":" << metrics.Errors;
    out << ",\"linux_relay_event_queue_full_errors\":" << metrics.EventQueueFullErrors;
    out << ",\"linux_relay_tcp_read_buffer_acquire_failures\":"
        << metrics.TcpReadBufferAcquireFailures;
    out << ",\"linux_relay_tcp_to_quic_compress_failures\":"
        << metrics.TcpToQuicCompressFailures;
    out << ",\"linux_relay_tcp_to_quic_buffer_acquire_failures\":"
        << metrics.TcpToQuicBufferAcquireFailures;
    out << ",\"linux_relay_quic_send_failures\":" << metrics.QuicSendFailures;
    out << ",\"linux_relay_quic_receive_ingress_buffer_acquire_failures\":"
        << metrics.QuicReceiveIngressBufferAcquireFailures;
    out << ",\"linux_relay_quic_receive_view_failures\":" << metrics.QuicReceiveViewFailures;
    out << ",\"linux_relay_quic_receive_decompress_failures\":"
        << metrics.QuicReceiveDecompressFailures;
    out << ",\"linux_relay_quic_receive_tcp_buffer_acquire_failures\":"
        << metrics.QuicReceiveTcpBufferAcquireFailures;
    out << ",\"linux_relay_tcp_write_hard_errors\":" << metrics.TcpWriteHardErrors;
}
