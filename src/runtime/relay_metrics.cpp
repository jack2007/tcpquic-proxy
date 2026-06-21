#include "relay_metrics.h"

#if defined(__linux__)
#include "linux_relay_worker.h"
#endif
#if defined(__APPLE__)
#include "darwin_relay_worker.h"
#endif
#if defined(_WIN32)
#include "windows_relay_worker.h"
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
    metrics.RelayBufferBytesInUse = snapshot.RelayBufferBytesInUse;
    metrics.ActiveRelays = snapshot.ActiveRelays;
    metrics.ActiveTcpRelays = snapshot.ActiveTcpRelays;
    metrics.ActiveSinkRelays = snapshot.ActiveSinkRelays;
    metrics.ActiveQuicSendRelays = snapshot.ActiveQuicSendRelays;
    metrics.CurrentPendingQuicReceiveBytes = snapshot.CurrentPendingQuicReceiveBytes;
    metrics.CurrentPendingQuicReceiveQueue = snapshot.CurrentPendingQuicReceiveQueue;
    metrics.TcpReadArmedRelays = snapshot.TcpReadArmedRelays;
    metrics.TcpReadDisabledRelays = snapshot.TcpReadDisabledRelays;
    metrics.TcpWriteArmedRelays = snapshot.TcpWriteArmedRelays;
    metrics.ClosingRelays = snapshot.ClosingRelays;
    metrics.TcpReadClosedRelays = snapshot.TcpReadClosedRelays;
    metrics.TcpWriteShutdownQueuedRelays = snapshot.TcpWriteShutdownQueuedRelays;
    metrics.OutstandingQuicSends = snapshot.OutstandingQuicSends;
    metrics.OutstandingQuicSendBytes = snapshot.OutstandingQuicSendBytes;
    metrics.MaxBufferedQuicSendBytes = snapshot.MaxBufferedQuicSendBytes;
    metrics.PendingTcpWriteQueue = snapshot.PendingTcpWriteQueue;
    metrics.PendingTcpWriteBytes = snapshot.PendingTcpWriteBytes;
    metrics.MaxWorkerPendingBytes = snapshot.MaxWorkerPendingBytes;
    metrics.MaxWorkerActiveRelays = snapshot.MaxWorkerActiveRelays;
    metrics.MaxRelayPendingQuicReceiveBytes = snapshot.MaxRelayPendingQuicReceiveBytes;
    metrics.MaxRelayPendingQuicReceiveQueue = snapshot.MaxRelayPendingQuicReceiveQueue;
    metrics.MaxRelayTcpWriteEagainCount = snapshot.MaxRelayTcpWriteEagainCount;
    metrics.HotRelayId = snapshot.HotRelayId;
    metrics.HotRelayWorkerIndex = snapshot.HotRelayWorkerIndex;
    metrics.HotRelayTcpFd = snapshot.HotRelayTcpFd;
    metrics.HotRelayPendingQuicReceiveBytes = snapshot.HotRelayPendingQuicReceiveBytes;
    metrics.HotRelayPendingQuicReceiveQueue = snapshot.HotRelayPendingQuicReceiveQueue;
    metrics.HotRelayTcpWriteBytes = snapshot.HotRelayTcpWriteBytes;
    metrics.HotRelayTcpReadBytes = snapshot.HotRelayTcpReadBytes;
    metrics.HotRelayOutstandingQuicSends = snapshot.HotRelayOutstandingQuicSends;
    metrics.HotRelayOutstandingQuicSendBytes = snapshot.HotRelayOutstandingQuicSendBytes;
    metrics.HotRelayPendingQuicSendRetries = snapshot.HotRelayPendingQuicSendRetries;
    metrics.HotRelayIdealSendBytes = snapshot.HotRelayIdealSendBytes;
    metrics.HotRelayTcpWriteEagainCount = snapshot.HotRelayTcpWriteEagainCount;
    metrics.HotRelayEpollOutEvents = snapshot.HotRelayEpollOutEvents;
    metrics.HotRelayTcpReadArmed = snapshot.HotRelayTcpReadArmed;
    metrics.HotRelayTcpWriteArmed = snapshot.HotRelayTcpWriteArmed;
    metrics.HotRelayLocalAddress = snapshot.HotRelayLocalAddress;
    metrics.HotRelayPeerAddress = snapshot.HotRelayPeerAddress;
    metrics.TcpReadBatches = snapshot.TcpReadBatches;
    metrics.TcpReadBytes = snapshot.TcpReadBytes;
    metrics.QuicSendOperations = snapshot.QuicSendOperations;
    metrics.TcpWriteBatches = snapshot.TcpWriteBatches;
    metrics.TcpWriteBytes = snapshot.TcpWriteBytes;
    metrics.MaxTcpReadIovUsed = snapshot.MaxTcpReadIovUsed;
    metrics.MaxTcpWriteIovUsed = snapshot.MaxTcpWriteIovUsed;
    metrics.TcpWriteSendmsgCalls = snapshot.TcpWriteSendmsgCalls;
    metrics.TcpWriteAttemptBytes = snapshot.TcpWriteAttemptBytes;
    metrics.MaxTcpWriteAttemptBytes = snapshot.MaxTcpWriteAttemptBytes;
    metrics.MaxTcpWriteSendmsgBytes = snapshot.MaxTcpWriteSendmsgBytes;
    metrics.TcpWriteAttemptBytesLe64K = snapshot.TcpWriteAttemptBytesLe64K;
    metrics.TcpWriteAttemptBytesLe256K = snapshot.TcpWriteAttemptBytesLe256K;
    metrics.TcpWriteAttemptBytesLe1M = snapshot.TcpWriteAttemptBytesLe1M;
    metrics.TcpWriteAttemptBytesLe4M = snapshot.TcpWriteAttemptBytesLe4M;
    metrics.TcpWriteAttemptBytesGt4M = snapshot.TcpWriteAttemptBytesGt4M;
    metrics.TcpWriteReturnedBytesLe64K = snapshot.TcpWriteReturnedBytesLe64K;
    metrics.TcpWriteReturnedBytesLe256K = snapshot.TcpWriteReturnedBytesLe256K;
    metrics.TcpWriteReturnedBytesLe1M = snapshot.TcpWriteReturnedBytesLe1M;
    metrics.TcpWriteReturnedBytesLe4M = snapshot.TcpWriteReturnedBytesLe4M;
    metrics.TcpWriteReturnedBytesGt4M = snapshot.TcpWriteReturnedBytesGt4M;
    metrics.TcpWriteEagainCount = snapshot.TcpWriteEagainCount;
    metrics.TcpWritePartialCount = snapshot.TcpWritePartialCount;
    metrics.TcpWriteBurstStops = snapshot.TcpWriteBurstStops;
    metrics.ReadDisabledCount = snapshot.ReadDisabledCount;
    metrics.CompressedTcpBytes = snapshot.CompressedTcpBytes;
    metrics.DecompressedTcpBytes = snapshot.DecompressedTcpBytes;
    metrics.ZstdDecompressInputBytes = snapshot.ZstdDecompressInputBytes;
    metrics.ZstdDecompressOutputBytes = snapshot.ZstdDecompressOutputBytes;
    metrics.ZstdDecompressCalls = snapshot.ZstdDecompressCalls;
    metrics.ZstdDecompressNeedInput = snapshot.ZstdDecompressNeedInput;
    metrics.ZstdDecompressNeedOutput = snapshot.ZstdDecompressNeedOutput;
    metrics.ZstdDecompressFailures = snapshot.ZstdDecompressFailures;
    metrics.DeferredReceiveCompleteBytes = snapshot.DeferredReceiveCompleteBytes;
    metrics.DeferredReceiveCompletes = snapshot.DeferredReceiveCompletes;
    metrics.DeferredReceiveCompletionFlushes = snapshot.DeferredReceiveCompletionFlushes;
    metrics.MaxPendingQuicReceiveBytes = snapshot.MaxPendingQuicReceiveBytes;
    metrics.MaxPendingQuicReceiveQueue = snapshot.MaxPendingQuicReceiveQueue;
    metrics.QuicReceiveViewCount = snapshot.QuicReceiveViewCount;
    metrics.QuicReceiveViewBytes = snapshot.QuicReceiveViewBytes;
    metrics.MaxQuicReceiveViewBytes = snapshot.MaxQuicReceiveViewBytes;
    metrics.MaxQuicReceiveViewSlices = snapshot.MaxQuicReceiveViewSlices;
    metrics.QuicReceiveViewBytesLe64K = snapshot.QuicReceiveViewBytesLe64K;
    metrics.QuicReceiveViewBytesLe256K = snapshot.QuicReceiveViewBytesLe256K;
    metrics.QuicReceiveViewBytesLe1M = snapshot.QuicReceiveViewBytesLe1M;
    metrics.QuicReceiveViewBytesLe4M = snapshot.QuicReceiveViewBytesLe4M;
    metrics.QuicReceiveViewBytesGt4M = snapshot.QuicReceiveViewBytesGt4M;
    metrics.QuicReceiveViewSlices1 = snapshot.QuicReceiveViewSlices1;
    metrics.QuicReceiveViewSlices2To4 = snapshot.QuicReceiveViewSlices2To4;
    metrics.QuicReceiveViewSlices5To16 = snapshot.QuicReceiveViewSlices5To16;
    metrics.QuicReceiveViewSlicesGt16 = snapshot.QuicReceiveViewSlicesGt16;
    metrics.QuicReceivePausedCount = snapshot.QuicReceivePausedCount;
    metrics.QuicReceiveResumedCount = snapshot.QuicReceiveResumedCount;
    metrics.Errors = snapshot.Errors;
    metrics.EventQueueFullErrors = snapshot.EventQueueFullErrors;
    metrics.TcpReadBufferAcquireFailures = snapshot.TcpReadBufferAcquireFailures;
    metrics.TcpReadBufferAcquirePendingBudgetFailures =
        snapshot.TcpReadBufferAcquirePendingBudgetFailures;
    metrics.TcpReadBufferAcquireAllocFailures = snapshot.TcpReadBufferAcquireAllocFailures;
    metrics.TcpToQuicCompressFailures = snapshot.TcpToQuicCompressFailures;
    metrics.TcpToQuicCompressUpdateFailures = snapshot.TcpToQuicCompressUpdateFailures;
    metrics.TcpToQuicCompressFlushFailures = snapshot.TcpToQuicCompressFlushFailures;
    metrics.TcpToQuicBufferAcquireFailures = snapshot.TcpToQuicBufferAcquireFailures;
    metrics.TcpToQuicBufferAcquirePendingBudgetFailures =
        snapshot.TcpToQuicBufferAcquirePendingBudgetFailures;
    metrics.TcpToQuicBufferAcquireAllocFailures =
        snapshot.TcpToQuicBufferAcquireAllocFailures;
    metrics.QuicSendFailures = snapshot.QuicSendFailures;
    metrics.QuicSendBufferTooLargeFailures = snapshot.QuicSendBufferTooLargeFailures;
    metrics.QuicSendOperationAllocFailures = snapshot.QuicSendOperationAllocFailures;
    metrics.QuicSendApiFailures = snapshot.QuicSendApiFailures;
    metrics.QuicSendBackpressureEvents = snapshot.QuicSendBackpressureEvents;
    metrics.QuicSendFatalErrors = snapshot.QuicSendFatalErrors;
    metrics.QuicReceiveViewFailures = snapshot.QuicReceiveViewFailures;
    metrics.QuicReceiveViewBackpressureQueued = snapshot.QuicReceiveViewBackpressureQueued;
    metrics.QuicReceiveViewAllocFailures = snapshot.QuicReceiveViewAllocFailures;
    metrics.QuicReceiveViewNullBufferFailures = snapshot.QuicReceiveViewNullBufferFailures;
    metrics.QuicReceiveViewEmptyFailures = snapshot.QuicReceiveViewEmptyFailures;
    metrics.QuicReceiveViewEnqueueFailures = snapshot.QuicReceiveViewEnqueueFailures;
    metrics.QuicReceiveDecompressFailures = snapshot.QuicReceiveDecompressFailures;
    metrics.QuicReceiveTcpBufferAcquireFailures = snapshot.QuicReceiveTcpBufferAcquireFailures;
    metrics.QuicReceiveTcpBufferAcquirePendingBudgetFailures =
        snapshot.QuicReceiveTcpBufferAcquirePendingBudgetFailures;
    metrics.QuicReceiveTcpBufferAcquireAllocFailures =
        snapshot.QuicReceiveTcpBufferAcquireAllocFailures;
    metrics.TcpWriteHardErrors = snapshot.TcpWriteHardErrors;
    metrics.LastTcpWriteErrno = snapshot.LastTcpWriteErrno;
    metrics.TcpReadHardErrors = snapshot.TcpReadHardErrors;
    metrics.LastTcpReadErrno = snapshot.LastTcpReadErrno;
    metrics.FatalRelayResets = snapshot.FatalRelayResets;
    metrics.LastQuicSendStatus = snapshot.LastQuicSendStatus;
#elif defined(__APPLE__)
    const auto snapshot = TqDarwinRelayRuntime::Instance().Snapshot();
    metrics.Backend = "worker";
    metrics.Wakeups = snapshot.Wakeups;
    metrics.EventsProcessed = snapshot.EventsProcessed;
    metrics.PendingEvents = snapshot.PendingEvents;
    metrics.PendingBytes = snapshot.PendingBytes;
    metrics.ActiveRelays = snapshot.ActiveRelays;
    metrics.CurrentPendingQuicReceiveBytes = snapshot.CurrentPendingQuicReceiveBytes;
    metrics.TcpReadArmedRelays = snapshot.TcpReadArmedRelays;
    metrics.TcpWriteArmedRelays = snapshot.TcpWriteArmedRelays;
    metrics.OutstandingQuicSends = snapshot.OutstandingQuicSends;
    metrics.OutstandingQuicSendBytes = snapshot.OutstandingQuicSendBytes;
    metrics.PendingTcpWriteQueue = snapshot.PendingTcpWriteQueue;
    metrics.PendingTcpWriteBytes = snapshot.PendingTcpWriteBytes;
    metrics.TcpReadBatches = snapshot.TcpReadBatches;
    metrics.TcpReadBytes = snapshot.TcpReadBytes;
    metrics.TcpWriteBatches = snapshot.TcpWriteBatches;
    metrics.TcpWriteBytes = snapshot.TcpWriteBytes;
    metrics.DeferredReceiveCompletes = snapshot.DeferredReceiveCompletes;
    metrics.QuicReceiveViewCount = snapshot.QuicReceiveViewCount;
    metrics.QuicReceiveViewBytes = snapshot.QuicReceiveViewBytes;
    metrics.QuicReceivePausedCount = snapshot.QuicReceivePausedCount;
    metrics.QuicReceiveResumedCount = snapshot.QuicReceiveResumedCount;
    metrics.Errors = snapshot.Errors;
    metrics.QuicSendBackpressureEvents = snapshot.QuicSendBackpressureEvents;
#elif defined(_WIN32)
    const auto snapshot = TqWindowsRelayRuntime::Instance().Snapshot();
    metrics.Backend = "worker";
    metrics.ActiveRelays = snapshot.ActiveRelays;
    metrics.PendingBytes = snapshot.PendingQuicReceiveBytes + snapshot.RelayBufferBytesInUse;
    metrics.RelayBufferBytesInUse = snapshot.RelayBufferBytesInUse;
    metrics.CurrentPendingQuicReceiveBytes = snapshot.PendingQuicReceiveBytes;
    metrics.CurrentPendingQuicReceiveQueue = snapshot.PendingQuicReceiveQueueDepth;
    metrics.TcpReadBytes = snapshot.TcpSendBytes;
    metrics.TcpWriteBytes = snapshot.TcpSendBytes;
    metrics.ZstdDecompressInputBytes = snapshot.ZstdDecompressInputBytes;
    metrics.ZstdDecompressOutputBytes = snapshot.ZstdDecompressOutputBytes;
    metrics.ZstdDecompressCalls = snapshot.ZstdDecompressCalls;
    metrics.ZstdDecompressNeedInput = snapshot.ZstdDecompressNeedInput;
    metrics.ZstdDecompressNeedOutput = snapshot.ZstdDecompressNeedOutput;
    metrics.ZstdDecompressFailures = snapshot.ZstdDecompressFailures;
    metrics.DeferredReceiveCompleteBytes = snapshot.DeferredReceiveCompleteBytes;
    metrics.DeferredReceiveCompletes = snapshot.DeferredReceiveCompletes;
    metrics.DeferredReceiveCompletionFlushes = snapshot.DeferredReceiveCompletionFlushes;
    metrics.MaxPendingQuicReceiveBytes = snapshot.MaxPendingQuicReceiveBytes;
    metrics.MaxPendingQuicReceiveQueue = snapshot.MaxPendingQuicReceiveQueueDepth;
    metrics.QuicReceivePausedCount = snapshot.QuicReceivePausedCount;
    metrics.QuicReceiveResumedCount = snapshot.QuicReceiveResumedCount;
    metrics.FatalRelayResets = snapshot.FatalRelayResets;
    metrics.TcpHardErrors = snapshot.TcpHardErrors;
    metrics.GracefulRelayDrains = snapshot.GracefulRelayDrains;
    metrics.QuicSendBackpressureEvents = snapshot.QuicSendBackpressureEvents;
    metrics.QuicSendFatalErrors = snapshot.QuicSendFatalErrors;
    metrics.Errors = snapshot.Errors;
#endif
    return metrics;
}

void TqAppendRelayMetricsJson(std::ostringstream& out, const TqRelayMetricsSnapshot& metrics) {
    out << ",\"linux_relay_wakeups\":" << metrics.Wakeups;
    out << ",\"linux_relay_events_processed\":" << metrics.EventsProcessed;
    out << ",\"linux_relay_pending_events\":" << metrics.PendingEvents;
    out << ",\"linux_relay_pending_bytes\":" << metrics.PendingBytes;
    out << ",\"linux_relay_buffer_bytes_in_use\":" << metrics.RelayBufferBytesInUse;
    out << ",\"linux_relay_active_relays\":" << metrics.ActiveRelays;
    out << ",\"linux_relay_active_tcp_relays\":" << metrics.ActiveTcpRelays;
    out << ",\"linux_relay_active_sink_relays\":" << metrics.ActiveSinkRelays;
    out << ",\"linux_relay_active_quic_send_relays\":" << metrics.ActiveQuicSendRelays;
    out << ",\"linux_relay_current_pending_quic_receive_bytes\":"
        << metrics.CurrentPendingQuicReceiveBytes;
    out << ",\"linux_relay_current_pending_quic_receive_queue\":"
        << metrics.CurrentPendingQuicReceiveQueue;
    out << ",\"linux_relay_tcp_read_armed_relays\":" << metrics.TcpReadArmedRelays;
    out << ",\"linux_relay_tcp_read_disabled_relays\":" << metrics.TcpReadDisabledRelays;
    out << ",\"linux_relay_tcp_write_armed_relays\":" << metrics.TcpWriteArmedRelays;
    out << ",\"linux_relay_closing_relays\":" << metrics.ClosingRelays;
    out << ",\"linux_relay_tcp_read_closed_relays\":" << metrics.TcpReadClosedRelays;
    out << ",\"linux_relay_tcp_write_shutdown_queued_relays\":"
        << metrics.TcpWriteShutdownQueuedRelays;
    out << ",\"linux_relay_outstanding_quic_sends\":" << metrics.OutstandingQuicSends;
    out << ",\"linux_relay_outstanding_quic_send_bytes\":" << metrics.OutstandingQuicSendBytes;
    out << ",\"linux_relay_max_buffered_quic_send_bytes\":"
        << metrics.MaxBufferedQuicSendBytes;
    out << ",\"linux_relay_pending_tcp_write_queue\":" << metrics.PendingTcpWriteQueue;
    out << ",\"linux_relay_pending_tcp_write_bytes\":" << metrics.PendingTcpWriteBytes;
    out << ",\"linux_relay_max_worker_pending_bytes\":" << metrics.MaxWorkerPendingBytes;
    out << ",\"linux_relay_max_worker_active_relays\":" << metrics.MaxWorkerActiveRelays;
    out << ",\"linux_relay_max_relay_pending_quic_receive_bytes\":"
        << metrics.MaxRelayPendingQuicReceiveBytes;
    out << ",\"linux_relay_max_relay_pending_quic_receive_queue\":"
        << metrics.MaxRelayPendingQuicReceiveQueue;
    out << ",\"linux_relay_max_relay_tcp_write_eagain_count\":"
        << metrics.MaxRelayTcpWriteEagainCount;
    out << ",\"linux_relay_hot_relay_id\":" << metrics.HotRelayId;
    out << ",\"linux_relay_hot_relay_worker_index\":" << metrics.HotRelayWorkerIndex;
    out << ",\"linux_relay_hot_relay_tcp_fd\":" << metrics.HotRelayTcpFd;
    out << ",\"linux_relay_hot_relay_pending_quic_receive_bytes\":"
        << metrics.HotRelayPendingQuicReceiveBytes;
    out << ",\"linux_relay_hot_relay_pending_quic_receive_queue\":"
        << metrics.HotRelayPendingQuicReceiveQueue;
    out << ",\"linux_relay_hot_relay_tcp_write_bytes\":"
        << metrics.HotRelayTcpWriteBytes;
    out << ",\"linux_relay_hot_relay_tcp_read_bytes\":"
        << metrics.HotRelayTcpReadBytes;
    out << ",\"linux_relay_hot_relay_outstanding_quic_sends\":"
        << metrics.HotRelayOutstandingQuicSends;
    out << ",\"linux_relay_hot_relay_outstanding_quic_send_bytes\":"
        << metrics.HotRelayOutstandingQuicSendBytes;
    out << ",\"linux_relay_hot_relay_pending_quic_send_retries\":"
        << metrics.HotRelayPendingQuicSendRetries;
    out << ",\"linux_relay_hot_relay_ideal_send_bytes\":"
        << metrics.HotRelayIdealSendBytes;
    out << ",\"linux_relay_hot_relay_tcp_write_eagain_count\":"
        << metrics.HotRelayTcpWriteEagainCount;
    out << ",\"linux_relay_hot_relay_epollout_events\":"
        << metrics.HotRelayEpollOutEvents;
    out << ",\"linux_relay_hot_relay_tcp_read_armed\":"
        << (metrics.HotRelayTcpReadArmed ? "true" : "false");
    out << ",\"linux_relay_hot_relay_tcp_write_armed\":"
        << (metrics.HotRelayTcpWriteArmed ? "true" : "false");
    out << ',';
    TqAppendJsonString(out, "linux_relay_hot_relay_local", metrics.HotRelayLocalAddress);
    out << ',';
    TqAppendJsonString(out, "linux_relay_hot_relay_peer", metrics.HotRelayPeerAddress);
    out << ",\"linux_relay_tcp_read_batches\":" << metrics.TcpReadBatches;
    out << ",\"linux_relay_tcp_read_bytes\":" << metrics.TcpReadBytes;
    out << ",\"linux_relay_quic_send_operations\":" << metrics.QuicSendOperations;
    out << ",\"linux_relay_tcp_write_batches\":" << metrics.TcpWriteBatches;
    out << ",\"linux_relay_tcp_write_bytes\":" << metrics.TcpWriteBytes;
    out << ",\"linux_relay_max_tcp_read_iov_used\":" << metrics.MaxTcpReadIovUsed;
    out << ",\"linux_relay_max_tcp_write_iov_used\":" << metrics.MaxTcpWriteIovUsed;
    out << ",\"linux_relay_tcp_write_sendmsg_calls\":" << metrics.TcpWriteSendmsgCalls;
    out << ",\"linux_relay_tcp_write_attempt_bytes\":" << metrics.TcpWriteAttemptBytes;
    out << ",\"linux_relay_max_tcp_write_attempt_bytes\":" << metrics.MaxTcpWriteAttemptBytes;
    out << ",\"linux_relay_max_tcp_write_sendmsg_bytes\":" << metrics.MaxTcpWriteSendmsgBytes;
    out << ",\"linux_relay_tcp_write_attempt_bytes_le_64k\":" << metrics.TcpWriteAttemptBytesLe64K;
    out << ",\"linux_relay_tcp_write_attempt_bytes_le_256k\":" << metrics.TcpWriteAttemptBytesLe256K;
    out << ",\"linux_relay_tcp_write_attempt_bytes_le_1m\":" << metrics.TcpWriteAttemptBytesLe1M;
    out << ",\"linux_relay_tcp_write_attempt_bytes_le_4m\":" << metrics.TcpWriteAttemptBytesLe4M;
    out << ",\"linux_relay_tcp_write_attempt_bytes_gt_4m\":" << metrics.TcpWriteAttemptBytesGt4M;
    out << ",\"linux_relay_tcp_write_returned_bytes_le_64k\":" << metrics.TcpWriteReturnedBytesLe64K;
    out << ",\"linux_relay_tcp_write_returned_bytes_le_256k\":" << metrics.TcpWriteReturnedBytesLe256K;
    out << ",\"linux_relay_tcp_write_returned_bytes_le_1m\":" << metrics.TcpWriteReturnedBytesLe1M;
    out << ",\"linux_relay_tcp_write_returned_bytes_le_4m\":" << metrics.TcpWriteReturnedBytesLe4M;
    out << ",\"linux_relay_tcp_write_returned_bytes_gt_4m\":" << metrics.TcpWriteReturnedBytesGt4M;
    out << ",\"linux_relay_tcp_write_eagain_count\":" << metrics.TcpWriteEagainCount;
    out << ",\"linux_relay_tcp_write_partial_count\":" << metrics.TcpWritePartialCount;
    out << ",\"linux_relay_tcp_write_burst_stops\":" << metrics.TcpWriteBurstStops;
    out << ",\"linux_relay_read_disabled_count\":" << metrics.ReadDisabledCount;
    out << ',';
    TqAppendJsonString(out, "linux_relay_backend", metrics.Backend);
    out << ",\"linux_relay_compressed_tcp_bytes\":" << metrics.CompressedTcpBytes;
    out << ",\"linux_relay_decompressed_tcp_bytes\":" << metrics.DecompressedTcpBytes;
    out << ",\"linux_relay_zstd_decompress_input_bytes\":"
        << metrics.ZstdDecompressInputBytes;
    out << ",\"linux_relay_zstd_decompress_output_bytes\":"
        << metrics.ZstdDecompressOutputBytes;
    out << ",\"linux_relay_zstd_decompress_calls\":" << metrics.ZstdDecompressCalls;
    out << ",\"linux_relay_zstd_decompress_need_input\":"
        << metrics.ZstdDecompressNeedInput;
    out << ",\"linux_relay_zstd_decompress_need_output\":"
        << metrics.ZstdDecompressNeedOutput;
    out << ",\"linux_relay_zstd_decompress_failures\":"
        << metrics.ZstdDecompressFailures;
    out << ",\"linux_relay_deferred_receive_complete_bytes\":" << metrics.DeferredReceiveCompleteBytes;
    out << ",\"linux_relay_deferred_receive_completes\":" << metrics.DeferredReceiveCompletes;
    out << ",\"linux_relay_deferred_receive_completion_flushes\":" << metrics.DeferredReceiveCompletionFlushes;
    out << ",\"linux_relay_max_pending_quic_receive_bytes\":" << metrics.MaxPendingQuicReceiveBytes;
    out << ",\"linux_relay_max_pending_quic_receive_queue\":" << metrics.MaxPendingQuicReceiveQueue;
    out << ",\"linux_relay_quic_receive_view_count\":" << metrics.QuicReceiveViewCount;
    out << ",\"linux_relay_quic_receive_view_bytes\":" << metrics.QuicReceiveViewBytes;
    out << ",\"linux_relay_max_quic_receive_view_bytes\":" << metrics.MaxQuicReceiveViewBytes;
    out << ",\"linux_relay_max_quic_receive_view_slices\":" << metrics.MaxQuicReceiveViewSlices;
    out << ",\"linux_relay_quic_receive_view_bytes_le_64k\":" << metrics.QuicReceiveViewBytesLe64K;
    out << ",\"linux_relay_quic_receive_view_bytes_le_256k\":" << metrics.QuicReceiveViewBytesLe256K;
    out << ",\"linux_relay_quic_receive_view_bytes_le_1m\":" << metrics.QuicReceiveViewBytesLe1M;
    out << ",\"linux_relay_quic_receive_view_bytes_le_4m\":" << metrics.QuicReceiveViewBytesLe4M;
    out << ",\"linux_relay_quic_receive_view_bytes_gt_4m\":" << metrics.QuicReceiveViewBytesGt4M;
    out << ",\"linux_relay_quic_receive_view_slices_1\":" << metrics.QuicReceiveViewSlices1;
    out << ",\"linux_relay_quic_receive_view_slices_2_to_4\":" << metrics.QuicReceiveViewSlices2To4;
    out << ",\"linux_relay_quic_receive_view_slices_5_to_16\":" << metrics.QuicReceiveViewSlices5To16;
    out << ",\"linux_relay_quic_receive_view_slices_gt_16\":" << metrics.QuicReceiveViewSlicesGt16;
    out << ",\"linux_relay_quic_receive_paused_count\":" << metrics.QuicReceivePausedCount;
    out << ",\"linux_relay_quic_receive_resumed_count\":" << metrics.QuicReceiveResumedCount;
    out << ",\"linux_relay_errors\":" << metrics.Errors;
    out << ",\"linux_relay_event_queue_full_errors\":" << metrics.EventQueueFullErrors;
    out << ",\"linux_relay_tcp_read_buffer_acquire_failures\":"
        << metrics.TcpReadBufferAcquireFailures;
    out << ",\"linux_relay_tcp_read_buffer_acquire_pending_budget_failures\":"
        << metrics.TcpReadBufferAcquirePendingBudgetFailures;
    out << ",\"linux_relay_tcp_read_buffer_acquire_alloc_failures\":"
        << metrics.TcpReadBufferAcquireAllocFailures;
    out << ",\"linux_relay_tcp_to_quic_compress_failures\":"
        << metrics.TcpToQuicCompressFailures;
    out << ",\"linux_relay_tcp_to_quic_compress_update_failures\":"
        << metrics.TcpToQuicCompressUpdateFailures;
    out << ",\"linux_relay_tcp_to_quic_compress_flush_failures\":"
        << metrics.TcpToQuicCompressFlushFailures;
    out << ",\"linux_relay_tcp_to_quic_buffer_acquire_failures\":"
        << metrics.TcpToQuicBufferAcquireFailures;
    out << ",\"linux_relay_tcp_to_quic_buffer_acquire_pending_budget_failures\":"
        << metrics.TcpToQuicBufferAcquirePendingBudgetFailures;
    out << ",\"linux_relay_tcp_to_quic_buffer_acquire_alloc_failures\":"
        << metrics.TcpToQuicBufferAcquireAllocFailures;
    out << ",\"linux_relay_quic_send_failures\":" << metrics.QuicSendFailures;
    out << ",\"linux_relay_quic_send_buffer_too_large_failures\":"
        << metrics.QuicSendBufferTooLargeFailures;
    out << ",\"linux_relay_quic_send_operation_alloc_failures\":"
        << metrics.QuicSendOperationAllocFailures;
    out << ",\"linux_relay_quic_send_api_failures\":" << metrics.QuicSendApiFailures;
    out << ",\"linux_relay_quic_send_backpressure_events\":"
        << metrics.QuicSendBackpressureEvents;
    out << ",\"linux_relay_quic_send_fatal_errors\":"
        << metrics.QuicSendFatalErrors;
    out << ",\"linux_relay_quic_receive_view_failures\":" << metrics.QuicReceiveViewFailures;
    out << ",\"linux_relay_quic_receive_view_backpressure_queued\":"
        << metrics.QuicReceiveViewBackpressureQueued;
    out << ",\"linux_relay_quic_receive_view_alloc_failures\":"
        << metrics.QuicReceiveViewAllocFailures;
    out << ",\"linux_relay_quic_receive_view_null_buffer_failures\":"
        << metrics.QuicReceiveViewNullBufferFailures;
    out << ",\"linux_relay_quic_receive_view_empty_failures\":"
        << metrics.QuicReceiveViewEmptyFailures;
    out << ",\"linux_relay_quic_receive_view_enqueue_failures\":"
        << metrics.QuicReceiveViewEnqueueFailures;
    out << ",\"linux_relay_quic_receive_decompress_failures\":"
        << metrics.QuicReceiveDecompressFailures;
    out << ",\"linux_relay_quic_receive_tcp_buffer_acquire_failures\":"
        << metrics.QuicReceiveTcpBufferAcquireFailures;
    out << ",\"linux_relay_quic_receive_tcp_buffer_acquire_pending_budget_failures\":"
        << metrics.QuicReceiveTcpBufferAcquirePendingBudgetFailures;
    out << ",\"linux_relay_quic_receive_tcp_buffer_acquire_alloc_failures\":"
        << metrics.QuicReceiveTcpBufferAcquireAllocFailures;
    out << ",\"linux_relay_tcp_write_hard_errors\":" << metrics.TcpWriteHardErrors;
    out << ",\"linux_relay_last_tcp_write_errno\":" << metrics.LastTcpWriteErrno;
    out << ",\"linux_relay_tcp_read_hard_errors\":" << metrics.TcpReadHardErrors;
    out << ",\"linux_relay_last_tcp_read_errno\":" << metrics.LastTcpReadErrno;
    out << ",\"linux_relay_fatal_relay_resets\":" << metrics.FatalRelayResets;
    out << ",\"windows_relay_tcp_hard_errors\":" << metrics.TcpHardErrors;
    out << ",\"windows_relay_graceful_relay_drains\":" << metrics.GracefulRelayDrains;
    out << ",\"linux_relay_last_quic_send_status\":" << metrics.LastQuicSendStatus;
}
