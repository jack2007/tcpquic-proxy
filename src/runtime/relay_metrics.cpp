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

#include <nlohmann/json.hpp>

#include <sstream>

namespace {

#if defined(_WIN32) || defined(__linux__)
constexpr bool kRelayActiveRelayDetailSupported = true;
#else
constexpr bool kRelayActiveRelayDetailSupported = false;
#endif

#if defined(_WIN32)
TqRelayActiveSnapshot ConvertWindowsRelaySnapshot(const TqWindowsRelayActiveSnapshot& relay) {
    TqRelayActiveSnapshot active{};
    active.Backend = "windows";
    active.WorkerIndex = relay.WorkerIndex;
    active.RelayId = relay.RelayId;
    active.ActiveHandlers = relay.ActiveHandlers;
    active.QueuedWorkerOps = relay.QueuedWorkerOps;
    active.InFlightTcpRecvs = relay.InFlightTcpRecvs;
    active.InFlightTcpSends = relay.InFlightTcpSends;
    active.InFlightQuicSends = relay.InFlightQuicSends;
    active.PendingQuicReceiveBytes = relay.PendingQuicReceiveBytes;
    active.PendingQuicReceiveQueueDepth = relay.PendingQuicReceiveQueueDepth;
    active.CallbackPendingQuicReceiveDepth = relay.CallbackPendingQuicReceiveDepth;
    active.OutstandingQuicSendBytes = relay.OutstandingQuicSendBytes;
    active.MaxOutstandingQuicSendBytes = relay.MaxOutstandingQuicSendBytes;
    active.TcpReadBytes = relay.TcpReadBytes;
    active.TcpWriteBytes = relay.TcpWriteBytes;
    active.LastTcpWriteErrno = relay.LastTcpWriteErrno;
    active.LastTcpRecvErrno = relay.LastTcpRecvErrno;
    active.LastTcpSendErrno = relay.LastTcpSendErrno;
    active.LastIocpCompletionErrno = relay.LastIocpCompletionErrno;
    active.LastIocpOperation = relay.LastIocpOperation;
    active.Closing = relay.Closing;
    active.TcpReadClosed = relay.TcpReadClosed;
    active.TcpReadPausedByQuicBacklog = relay.TcpReadPausedByQuicBacklog;
    active.TcpWriteClosed = relay.TcpWriteClosed;
    active.CloseAfterDrained = relay.CloseAfterDrained;
    active.QuicSendFinSubmitted = relay.QuicSendFinSubmitted;
    active.QuicSendFinCompleted = relay.QuicSendFinCompleted;
    active.StopPublished = relay.StopPublished;
    active.StreamDetached = relay.StreamDetached;
    return active;
}
#endif

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

static void TqAppendJsonString(std::ostringstream& out, const char* name, const std::string& value) {
    out << '"' << name << "\":\"" << TqJsonEscape(value) << '"';
}

namespace {

TqRelayWorkerSnapshot MakeAggregateRelayWorkerSnapshot() {
    const auto metrics = TqSnapshotRelayMetrics();
    TqRelayWorkerSnapshot aggregate{};
    aggregate.Backend = metrics.Backend;
    aggregate.WorkerIndex = 0;
    aggregate.WorkerId = "aggregate";
    aggregate.ActiveRelays = metrics.ActiveRelays;
    aggregate.PendingBytes = metrics.PendingBytes;
    aggregate.TcpReadBytes = metrics.TcpReadBytes;
    aggregate.TcpWriteBytes = metrics.TcpWriteBytes;
    aggregate.Errors = metrics.Errors;
    aggregate.EventQueueCapacity = metrics.LinuxRelayEventQueueCapacity;
    return aggregate;
}

#if defined(__linux__)
TqRelayWorkerSnapshot ConvertLinuxRelayWorkerSnapshot(const TqLinuxRelayWorkerSnapshot& snapshot) {
    TqRelayWorkerSnapshot worker{};
    worker.Backend = "linux";
    worker.WorkerIndex = snapshot.WorkerIndex;
    worker.WorkerId = "linux-" + std::to_string(snapshot.WorkerIndex);
    worker.ActiveRelays = snapshot.ActiveRelays;
    worker.PendingBytes = snapshot.PendingBytes;
    worker.TcpReadBytes = snapshot.TcpReadBytes;
    worker.TcpWriteBytes = snapshot.TcpWriteBytes;
    worker.Errors = snapshot.Errors;
    worker.EventQueueCapacity = snapshot.EventQueueCapacity;
    return worker;
}

TqRelayActiveSnapshot ConvertLinuxRelaySnapshot(const TqLinuxRelayActiveSnapshot& relay) {
    TqRelayActiveSnapshot active{};
    active.Backend = "linux";
    active.WorkerIndex = relay.WorkerIndex;
    active.RelayId = relay.RelayId;
    active.InFlightQuicSends = relay.InFlightQuicSends;
    active.PendingQuicReceiveBytes = relay.PendingQuicReceiveBytes;
    active.PendingQuicReceiveQueueDepth = relay.PendingQuicReceiveQueueDepth;
    active.CallbackPendingQuicReceiveDepth = relay.CallbackPendingQuicReceiveDepth;
    active.OutstandingQuicSendBytes = relay.OutstandingQuicSendBytes;
    active.EventQueueDepth = 0;
    active.TcpReadBytes = relay.TcpReadBytes;
    active.TcpWriteBytes = relay.TcpWriteBytes;
    active.LastTcpWriteErrno = relay.LastTcpWriteErrno;
    active.Closing = relay.Closing;
    active.TcpReadClosed = relay.TcpReadClosed;
    active.TcpReadPausedByQuicBacklog = relay.TcpReadPausedByQuicBacklog;
    active.TcpWriteClosed = relay.TcpWriteClosed;
    active.QuicSendFinSubmitted = relay.QuicSendFinSubmitted;
    active.QuicSendFinCompleted = relay.QuicSendFinCompleted;
    active.StopPublished = relay.StopPublished;
    active.StreamDetached = relay.StreamDetached;
    active.TcpFd = relay.TcpFd;
    active.TcpReadArmed = relay.TcpReadArmed;
    active.TcpWriteArmed = relay.TcpWriteArmed;
    active.PendingQuicSendRetries = relay.PendingQuicSendRetries;
    active.IdealSendBytes = relay.IdealSendBytes;
    active.TcpWriteEagainCount = relay.TcpWriteEagainCount;
    active.EpollOutEvents = relay.EpollOutEvents;
    active.PendingTcpWriteQueueDepth = relay.PendingTcpWriteQueueDepth;
    active.PendingTcpWriteBytes = relay.PendingTcpWriteBytes;
    active.RelayBufferBytesInUse = relay.RelayBufferBytesInUse;
    active.LocalAddress = relay.LocalAddress;
    active.PeerAddress = relay.PeerAddress;
    return active;
}
#endif

nlohmann::json TqRelayCapabilitiesJsonValue() {
    return {
        {"active_relay_detail", kRelayActiveRelayDetailSupported},
        {"worker_detail", true},
        {"per_worker_active_relays", false},
    };
}

nlohmann::json TqActiveRelayJsonValue(const TqRelayActiveSnapshot& relay) {
    nlohmann::json body{
        {"relay_id", std::to_string(relay.RelayId)},
        {"relay_id_numeric", relay.RelayId},
        {"worker_index", relay.WorkerIndex},
        {"backend", relay.Backend},
        {"active_handlers", relay.ActiveHandlers},
        {"queued_worker_ops", relay.QueuedWorkerOps},
        {"in_flight_tcp_recvs", relay.InFlightTcpRecvs},
        {"in_flight_tcp_sends", relay.InFlightTcpSends},
        {"in_flight_quic_sends", relay.InFlightQuicSends},
        {"pending_quic_receive_bytes", relay.PendingQuicReceiveBytes},
        {"pending_quic_receive_queue_depth", relay.PendingQuicReceiveQueueDepth},
        {"callback_pending_quic_receive_depth", relay.CallbackPendingQuicReceiveDepth},
        {"outstanding_quic_send_bytes", relay.OutstandingQuicSendBytes},
        {"max_outstanding_quic_send_bytes", relay.MaxOutstandingQuicSendBytes},
        {"event_queue_depth", relay.EventQueueDepth},
        {"tcp_read_bytes", relay.TcpReadBytes},
        {"tcp_write_bytes", relay.TcpWriteBytes},
        {"last_tcp_write_errno", relay.LastTcpWriteErrno},
        {"last_tcp_recv_errno", relay.LastTcpRecvErrno},
        {"last_tcp_send_errno", relay.LastTcpSendErrno},
        {"last_iocp_completion_errno", relay.LastIocpCompletionErrno},
        {"last_iocp_operation", relay.LastIocpOperation},
        {"closing", relay.Closing},
        {"tcp_read_closed", relay.TcpReadClosed},
        {"tcp_read_paused_by_quic_backlog", relay.TcpReadPausedByQuicBacklog},
        {"tcp_write_closed", relay.TcpWriteClosed},
        {"close_after_drained", relay.CloseAfterDrained},
        {"quic_send_fin_submitted", relay.QuicSendFinSubmitted},
        {"quic_send_fin_completed", relay.QuicSendFinCompleted},
        {"stop_published", relay.StopPublished},
        {"stream_detached", relay.StreamDetached},
    };
    if (std::string(relay.Backend) == "linux") {
        body["tcp_fd"] = relay.TcpFd;
        body["tcp_read_armed"] = relay.TcpReadArmed;
        body["tcp_write_armed"] = relay.TcpWriteArmed;
        body["pending_quic_send_retries"] = relay.PendingQuicSendRetries;
        body["ideal_send_bytes"] = relay.IdealSendBytes;
        body["tcp_write_eagain_count"] = relay.TcpWriteEagainCount;
        body["epoll_out_events"] = relay.EpollOutEvents;
        body["pending_tcp_write_queue_depth"] = relay.PendingTcpWriteQueueDepth;
        body["pending_tcp_write_bytes"] = relay.PendingTcpWriteBytes;
        body["relay_buffer_bytes_in_use"] = relay.RelayBufferBytesInUse;
        body["local_address"] = relay.LocalAddress;
        body["peer_address"] = relay.PeerAddress;
    }
    return body;
}

nlohmann::json TqRelayWorkerJsonValue(const TqRelayWorkerSnapshot& worker) {
    return {
        {"worker_id", worker.WorkerId},
        {"backend", worker.Backend},
        {"worker_index", worker.WorkerIndex},
        {"active_relays", worker.ActiveRelays},
        {"pending_bytes", worker.PendingBytes},
        {"tcp_read_bytes", worker.TcpReadBytes},
        {"tcp_write_bytes", worker.TcpWriteBytes},
        {"errors", worker.Errors},
        {"event_queue_capacity", worker.EventQueueCapacity},
    };
}

} // namespace

std::vector<TqRelayActiveSnapshot> TqSnapshotActiveRelays() {
    std::vector<TqRelayActiveSnapshot> active;
#if defined(__linux__)
    const auto snapshot = TqLinuxRelayRuntime::Instance().Snapshot();
    active.reserve(snapshot.ActiveRelayStates.size());
    for (const auto& relay : snapshot.ActiveRelayStates) {
        active.push_back(ConvertLinuxRelaySnapshot(relay));
    }
#elif defined(_WIN32)
    const auto snapshot = TqWindowsRelayRuntime::Instance().Snapshot();
    active.reserve(snapshot.ActiveRelayStates.size());
    for (const auto& relay : snapshot.ActiveRelayStates) {
        active.push_back(ConvertWindowsRelaySnapshot(relay));
    }
#endif
    return active;
}

TqRelayMetricsSnapshot TqSnapshotRelayMetrics() {
    TqRelayMetricsSnapshot metrics;
#if defined(__linux__)
    const auto snapshot = TqLinuxRelayRuntime::Instance().Snapshot();
    metrics.Backend = "epoll";
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
    metrics.LinuxRelayFakeFinReceiveCount = snapshot.FakeFinReceiveCount;
    metrics.LinuxRelayLateTcpErrorAfterStreamShutdown =
        snapshot.LateTcpErrorAfterStreamShutdown;
    metrics.LinuxRelayStreamLookupScanCount = snapshot.StreamLookupScanCount;
    metrics.LinuxRelayTerminalRetainedOwnerCount = snapshot.TerminalRetainedOwnerCount;
    metrics.LinuxRelayTerminalRetainedOldestAgeMs = snapshot.TerminalRetainedOldestAgeMs;
    metrics.LinuxRelayStopRemaining = snapshot.StopRemaining;
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
    metrics.LinuxRelayEventQueueCapacity = snapshot.EventQueueCapacity;
    metrics.LinuxRelayEventQueuePushCasRetries = snapshot.EventQueuePushCasRetries;
    metrics.LinuxRelayEventQueuePopCasRetries = snapshot.EventQueuePopCasRetries;
    metrics.LinuxRelayEventProducerThreadsObserved = snapshot.EventProducerThreadsObserved;
    metrics.LinuxRelayMultipleEventProducerThreadsObserved =
        snapshot.MultipleEventProducerThreadsObserved;
    metrics.LinuxRelayControlLockWaitNanos = snapshot.ControlLockWaitNanos;
    metrics.LinuxRelayControlLockAcquireCount = snapshot.ControlLockAcquireCount;
    metrics.LinuxRelayControlCommandWaitNanos = snapshot.ControlCommandWaitNanos;
    metrics.LinuxRelayControlCommandWaitCount = snapshot.ControlCommandWaitCount;
    metrics.LinuxRelayControlCommandTimeouts = snapshot.ControlCommandTimeouts;
    metrics.LinuxRelayControlCommandEnqueueFailures =
        snapshot.ControlCommandEnqueueFailures;
    metrics.LinuxRelaySnapshotCommandWaitNanos = snapshot.SnapshotCommandWaitNanos;
    metrics.LinuxRelaySnapshotCommandWaitCount = snapshot.SnapshotCommandWaitCount;
    metrics.LinuxRelaySnapshotCommandTimeouts = snapshot.SnapshotCommandTimeouts;
    metrics.LinuxRelayRuntimeLockWaitNanos = snapshot.RuntimeLockWaitNanos;
    metrics.LinuxRelayRuntimeLockAcquireCount = snapshot.RuntimeLockAcquireCount;
    metrics.LinuxRelayRuntimeSnapshotInFlightMax = snapshot.RuntimeSnapshotInFlightMax;
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
    metrics.Backend = "kqueue";
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
    uint64_t tcpReadBytes = 0;
    uint64_t tcpWriteBytes = 0;
    uint64_t outstandingQuicSends = 0;
    uint64_t inflightTcpSends = 0;
    uint64_t closingRelays = 0;
    uint64_t tcpReadClosedRelays = 0;
    uint64_t closeAfterDrainedRelays = 0;
    uint64_t hotScore = 0;
    const TqWindowsRelayActiveSnapshot* hotRelay = nullptr;
    for (const auto& relay : snapshot.ActiveRelayStates) {
        const TqRelayActiveSnapshot active = ConvertWindowsRelaySnapshot(relay);
        tcpReadBytes += active.TcpReadBytes;
        tcpWriteBytes += active.TcpWriteBytes;
        outstandingQuicSends += active.InFlightQuicSends;
        inflightTcpSends += active.InFlightTcpSends;
        if (active.Closing) {
            ++closingRelays;
        }
        if (active.TcpReadClosed) {
            ++tcpReadClosedRelays;
        }
        if (active.CloseAfterDrained) {
            ++closeAfterDrainedRelays;
        }
        const uint64_t score =
            active.PendingQuicReceiveBytes + active.PendingQuicReceiveQueueDepth +
            active.InFlightTcpSends + active.InFlightQuicSends;
        if (hotRelay == nullptr || score > hotScore) {
            hotRelay = &relay;
            hotScore = score;
        }
    }
    metrics.Backend = "iocp";
    metrics.EventsProcessed = snapshot.EventsProcessed;
    metrics.PendingEvents = 0;
    metrics.EventQueueFullErrors = 0;
    metrics.ActiveRelays = snapshot.ActiveRelays;
    metrics.PendingBytes = snapshot.PendingQuicReceiveBytes + snapshot.RelayBufferBytesInUse;
    metrics.RelayBufferBytesInUse = snapshot.RelayBufferBytesInUse;
    metrics.CurrentPendingQuicReceiveBytes = snapshot.PendingQuicReceiveBytes;
    metrics.CurrentPendingQuicReceiveQueue = snapshot.PendingQuicReceiveQueueDepth;
    metrics.TcpReadBytes = tcpReadBytes;
    metrics.TcpWriteBytes = tcpWriteBytes;
    metrics.ClosingRelays = closingRelays;
    metrics.TcpReadClosedRelays = tcpReadClosedRelays;
    metrics.TcpWriteShutdownQueuedRelays = closeAfterDrainedRelays;
    metrics.OutstandingQuicSends = outstandingQuicSends;
    metrics.PendingTcpWriteQueue = snapshot.PendingQuicReceiveQueueDepth;
    metrics.PendingTcpWriteBytes = snapshot.PendingQuicReceiveBytes;
    metrics.TcpWriteArmedRelays = inflightTcpSends;
    metrics.QuicReceiveViewCount = snapshot.DeferredReceiveQueued;
    if (hotRelay != nullptr) {
        metrics.HotRelayId = hotRelay->RelayId;
        metrics.HotRelayWorkerIndex = hotRelay->WorkerIndex;
        metrics.HotRelayPendingQuicReceiveBytes = hotRelay->PendingQuicReceiveBytes;
        metrics.HotRelayPendingQuicReceiveQueue = hotRelay->PendingQuicReceiveQueueDepth;
        metrics.HotRelayTcpReadBytes = hotRelay->TcpReadBytes;
        metrics.HotRelayTcpWriteBytes = hotRelay->TcpWriteBytes;
        metrics.HotRelayOutstandingQuicSends = hotRelay->InFlightQuicSends;
        metrics.LastTcpWriteErrno = hotRelay->LastTcpWriteErrno;
        metrics.HotRelayTcpReadArmed = hotRelay->InFlightTcpRecvs != 0;
        metrics.HotRelayTcpWriteArmed = hotRelay->InFlightTcpSends != 0;
    }
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
    metrics.WindowsCallbackIocpPostCount = snapshot.WindowsCallbackIocpPostCount;
    metrics.WindowsCallbackIocpPostFailedCount = snapshot.WindowsCallbackIocpPostFailedCount;
    metrics.WindowsReceiveReadyPostCount = snapshot.WindowsReceiveReadyPostCount;
    metrics.WindowsReceiveDrainScheduledCount = snapshot.WindowsReceiveDrainScheduledCount;
    metrics.WindowsReceiveDrainCoalescedCount = snapshot.WindowsReceiveDrainCoalescedCount;
    metrics.WindowsPostedCallbackStaleDropCount = snapshot.WindowsPostedCallbackStaleDropCount;
    metrics.WindowsPostedTraceContextStaleDropCount = snapshot.WindowsPostedTraceContextStaleDropCount;
    metrics.WindowsRelayWorkerLockAcquireCount = snapshot.WorkerLockAcquireCount;
    metrics.WindowsRelayWorkerLockWaitNanos = snapshot.WorkerLockWaitNanos;
    metrics.WindowsRelayFindRelayByIdCount = snapshot.FindRelayByIdCount;
    metrics.WindowsRelayCallbackDispatchNanos = snapshot.CallbackDispatchNanos;
    metrics.WindowsRelayCallbackReceiveBudgetRejectedCount =
        snapshot.CallbackReceiveBudgetRejectedCount;
    metrics.WindowsRelayCallbackReceiveBudgetPausedCount =
        snapshot.CallbackReceiveBudgetPausedCount;
    metrics.WindowsRelayCallbackReceiveCopyBytes = snapshot.CallbackReceiveCopyBytes;
    metrics.WindowsRelayCallbackReceiveCopyNanos = snapshot.CallbackReceiveCopyNanos;
    metrics.WindowsRelaySnapshotBuildNanos = snapshot.SnapshotBuildNanos;
    metrics.WindowsRelaySnapshotActiveRelaysScanned = snapshot.SnapshotActiveRelaysScanned;
    metrics.WindowsRelayMaintenanceDrainCount = snapshot.MaintenanceDrainCount;
    metrics.WindowsRelayMaintenanceDrainNanos = snapshot.MaintenanceDrainNanos;
    metrics.WindowsRelayMaintenanceRelaysProcessed = snapshot.MaintenanceRelaysProcessed;
    metrics.WindowsRelayMaintenanceFullScanCount = snapshot.MaintenanceFullScanCount;
    metrics.WindowsRelayMaintenanceFullScanRelaysScanned = snapshot.MaintenanceFullScanRelaysScanned;
    metrics.WindowsRelayReceiveViewFinishLinearSearchCount = snapshot.ReceiveViewFinishLinearSearchCount;
    metrics.WindowsRelayReceiveViewFinishLinearSearchNanos = snapshot.ReceiveViewFinishLinearSearchNanos;
    metrics.WindowsRelayReceiveViewFinishNotFrontCount = snapshot.ReceiveViewFinishNotFrontCount;
#endif
    return metrics;
}

std::vector<TqRelayWorkerSnapshot> TqSnapshotRelayWorkers() {
    std::vector<TqRelayWorkerSnapshot> workers;
    workers.push_back(MakeAggregateRelayWorkerSnapshot());
#if defined(__linux__)
    const auto linuxWorkers = TqLinuxRelayRuntime::Instance().SnapshotWorkers();
    for (const auto& snapshot : linuxWorkers) {
        workers.push_back(ConvertLinuxRelayWorkerSnapshot(snapshot));
    }
#endif
    return workers;
}

std::string TqRelayActiveRelaysJson() {
    const auto relays = TqSnapshotActiveRelays();
    nlohmann::json body{
        {"capabilities", TqRelayCapabilitiesJsonValue()},
        {"relays", nlohmann::json::array()},
    };
    for (const auto& relay : relays) {
        body["relays"].push_back(TqActiveRelayJsonValue(relay));
    }
    return body.dump();
}

std::string TqRelayActiveRelayJson(const std::string& relayId, bool& found, bool& supported) {
    found = false;
    supported = kRelayActiveRelayDetailSupported;
    if (!supported) {
        return "{}";
    }

    const auto relays = TqSnapshotActiveRelays();
    for (const auto& relay : relays) {
        if (std::to_string(relay.RelayId) == relayId) {
            found = true;
            return TqActiveRelayJsonValue(relay).dump();
        }
    }
    return "{}";
}

std::string TqRelayWorkersJson() {
    const auto workers = TqSnapshotRelayWorkers();
    nlohmann::json body{
        {"capabilities", TqRelayCapabilitiesJsonValue()},
        {"workers", nlohmann::json::array()},
    };
    for (const auto& worker : workers) {
        body["workers"].push_back(TqRelayWorkerJsonValue(worker));
    }
    return body.dump();
}

std::string TqRelayWorkerDetailJson(const std::string& workerId, bool& found, bool& supported) {
    found = false;
    supported = true;
    const auto workers = TqSnapshotRelayWorkers();
    for (const auto& worker : workers) {
        if (worker.WorkerId != workerId) {
            continue;
        }
        found = true;
        nlohmann::json body = TqRelayWorkerJsonValue(worker);
        body["relays"] = nlohmann::json::array();
        return body.dump();
    }
    return "{}";
}

static void TqAppendNeutralRelayMetricsJson(std::ostringstream& out, const TqRelayMetricsSnapshot& metrics) {
    out << ",\"relay_wakeups\":" << metrics.Wakeups;
    out << ",\"relay_events_processed\":" << metrics.EventsProcessed;
    out << ",\"relay_pending_events\":" << metrics.PendingEvents;
    out << ",\"relay_pending_bytes\":" << metrics.PendingBytes;
    out << ",\"relay_buffer_bytes_in_use\":" << metrics.RelayBufferBytesInUse;
    out << ",\"relay_active_relays\":" << metrics.ActiveRelays;
    out << ",\"relay_current_pending_quic_receive_bytes\":" << metrics.CurrentPendingQuicReceiveBytes;
    out << ",\"relay_outstanding_quic_sends\":" << metrics.OutstandingQuicSends;
    out << ",\"relay_outstanding_quic_send_bytes\":" << metrics.OutstandingQuicSendBytes;
    out << ",\"relay_tcp_read_bytes\":" << metrics.TcpReadBytes;
    out << ",\"relay_tcp_write_bytes\":" << metrics.TcpWriteBytes;
    out << ",\"relay_deferred_receive_completes\":" << metrics.DeferredReceiveCompletes;
    out << ",\"relay_quic_receive_view_count\":" << metrics.QuicReceiveViewCount;
    out << ",\"relay_quic_receive_view_bytes\":" << metrics.QuicReceiveViewBytes;
    out << ",\"relay_quic_receive_paused_count\":" << metrics.QuicReceivePausedCount;
    out << ",\"relay_quic_receive_resumed_count\":" << metrics.QuicReceiveResumedCount;
    out << ",\"relay_errors\":" << metrics.Errors;
    out << ",\"relay_event_queue_full_errors\":" << metrics.EventQueueFullErrors;
    out << ",\"relay_event_queue_capacity\":" << metrics.LinuxRelayEventQueueCapacity;
    out << ",\"relay_last_quic_send_status\":" << metrics.LastQuicSendStatus;
    out << ',';
    TqAppendJsonString(out, "relay_backend", metrics.Backend);
}

static void TqAppendRelayMetricsJson(std::ostringstream& out, const TqRelayMetricsSnapshot& metrics) {
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
    out << ",\"linux_relay_fake_fin_receive_count\":"
        << metrics.LinuxRelayFakeFinReceiveCount;
    out << ",\"linux_relay_late_tcp_error_after_stream_shutdown\":"
        << metrics.LinuxRelayLateTcpErrorAfterStreamShutdown;
    out << ",\"linux_relay_terminal_retained_owner_count\":"
        << metrics.LinuxRelayTerminalRetainedOwnerCount;
    out << ",\"linux_relay_terminal_retained_oldest_age_ms\":"
        << metrics.LinuxRelayTerminalRetainedOldestAgeMs;
    out << ",\"linux_relay_stop_remaining\":"
        << metrics.LinuxRelayStopRemaining;
    out << ",\"linux_relay_stream_lookup_scan_count\":"
        << metrics.LinuxRelayStreamLookupScanCount;
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
    out << ",\"linux_relay_event_queue_capacity\":"
        << metrics.LinuxRelayEventQueueCapacity;
    out << ",\"linux_relay_event_queue_push_cas_retries\":"
        << metrics.LinuxRelayEventQueuePushCasRetries;
    out << ",\"linux_relay_event_queue_pop_cas_retries\":"
        << metrics.LinuxRelayEventQueuePopCasRetries;
    out << ",\"linux_relay_event_producer_threads_observed\":"
        << metrics.LinuxRelayEventProducerThreadsObserved;
    out << ",\"linux_relay_multiple_event_producer_threads_observed\":"
        << (metrics.LinuxRelayMultipleEventProducerThreadsObserved ? "true" : "false");
    out << ",\"linux_relay_control_lock_wait_nanos\":"
        << metrics.LinuxRelayControlLockWaitNanos;
    out << ",\"linux_relay_control_lock_acquire_count\":"
        << metrics.LinuxRelayControlLockAcquireCount;
    out << ",\"linux_relay_control_command_wait_nanos\":"
        << metrics.LinuxRelayControlCommandWaitNanos;
    out << ",\"linux_relay_control_command_wait_count\":"
        << metrics.LinuxRelayControlCommandWaitCount;
    out << ",\"linux_relay_control_command_timeouts\":"
        << metrics.LinuxRelayControlCommandTimeouts;
    out << ",\"linux_relay_control_command_enqueue_failures\":"
        << metrics.LinuxRelayControlCommandEnqueueFailures;
    out << ",\"linux_relay_snapshot_command_wait_nanos\":"
        << metrics.LinuxRelaySnapshotCommandWaitNanos;
    out << ",\"linux_relay_snapshot_command_wait_count\":"
        << metrics.LinuxRelaySnapshotCommandWaitCount;
    out << ",\"linux_relay_snapshot_command_timeouts\":"
        << metrics.LinuxRelaySnapshotCommandTimeouts;
    out << ",\"linux_relay_runtime_lock_wait_nanos\":"
        << metrics.LinuxRelayRuntimeLockWaitNanos;
    out << ",\"linux_relay_runtime_lock_acquire_count\":"
        << metrics.LinuxRelayRuntimeLockAcquireCount;
    out << ",\"linux_relay_runtime_snapshot_inflight_max\":"
        << metrics.LinuxRelayRuntimeSnapshotInFlightMax;
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
    out << ",\"windows_relay_callback_iocp_posts\":"
        << metrics.WindowsCallbackIocpPostCount;
    out << ",\"windows_relay_callback_iocp_post_failures\":"
        << metrics.WindowsCallbackIocpPostFailedCount;
    out << ",\"windows_relay_receive_ready_posts\":"
        << metrics.WindowsReceiveReadyPostCount;
    out << ",\"windows_relay_receive_drain_scheduled\":"
        << metrics.WindowsReceiveDrainScheduledCount;
    out << ",\"windows_relay_receive_drain_coalesced\":"
        << metrics.WindowsReceiveDrainCoalescedCount;
    out << ",\"windows_relay_posted_callback_stale_drops\":"
        << metrics.WindowsPostedCallbackStaleDropCount;
    out << ",\"windows_relay_posted_trace_context_stale_drops\":"
        << metrics.WindowsPostedTraceContextStaleDropCount;
    out << ",\"windows_relay_worker_lock_acquires\":"
        << metrics.WindowsRelayWorkerLockAcquireCount;
    out << ",\"windows_relay_worker_lock_wait_nanos\":"
        << metrics.WindowsRelayWorkerLockWaitNanos;
    out << ",\"windows_relay_find_relay_by_id_count\":"
        << metrics.WindowsRelayFindRelayByIdCount;
    out << ",\"windows_relay_callback_dispatch_nanos\":"
        << metrics.WindowsRelayCallbackDispatchNanos;
    out << ",\"windows_relay_callback_receive_budget_rejected_count\":"
        << metrics.WindowsRelayCallbackReceiveBudgetRejectedCount;
    out << ",\"windows_relay_callback_receive_budget_paused_count\":"
        << metrics.WindowsRelayCallbackReceiveBudgetPausedCount;
    out << ",\"windows_relay_callback_receive_copy_bytes\":"
        << metrics.WindowsRelayCallbackReceiveCopyBytes;
    out << ",\"windows_relay_callback_receive_copy_nanos\":"
        << metrics.WindowsRelayCallbackReceiveCopyNanos;
    out << ",\"windows_relay_snapshot_build_nanos\":"
        << metrics.WindowsRelaySnapshotBuildNanos;
    out << ",\"windows_relay_snapshot_active_relays_scanned\":"
        << metrics.WindowsRelaySnapshotActiveRelaysScanned;
    out << ",\"windows_relay_maintenance_drain_count\":"
        << metrics.WindowsRelayMaintenanceDrainCount;
    out << ",\"windows_relay_maintenance_drain_nanos\":"
        << metrics.WindowsRelayMaintenanceDrainNanos;
    out << ",\"windows_relay_maintenance_relays_processed\":"
        << metrics.WindowsRelayMaintenanceRelaysProcessed;
    out << ",\"windows_relay_maintenance_full_scan_count\":"
        << metrics.WindowsRelayMaintenanceFullScanCount;
    out << ",\"windows_relay_maintenance_full_scan_relays_scanned\":"
        << metrics.WindowsRelayMaintenanceFullScanRelaysScanned;
    out << ",\"windows_relay_receive_view_finish_linear_search_count\":"
        << metrics.WindowsRelayReceiveViewFinishLinearSearchCount;
    out << ",\"windows_relay_receive_view_finish_linear_search_nanos\":"
        << metrics.WindowsRelayReceiveViewFinishLinearSearchNanos;
    out << ",\"windows_relay_receive_view_finish_not_front_count\":"
        << metrics.WindowsRelayReceiveViewFinishNotFrontCount;
    out << ",\"linux_relay_last_quic_send_status\":" << metrics.LastQuicSendStatus;
}

std::string TqRelayMetricsFieldsJson(const TqRelayMetricsSnapshot& metrics) {
    std::ostringstream out;
    out << "{\"_\":0";
    TqAppendNeutralRelayMetricsJson(out, metrics);
    TqAppendRelayMetricsJson(out, metrics);
    out << '}';
    auto value = nlohmann::json::parse(out.str());
    value.erase("_");
    return value.dump();
}
