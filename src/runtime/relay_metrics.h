#pragma once

#include <cstdint>
#include <iosfwd>
#include <string>

struct TqRelayMetricsSnapshot {
    const char* Backend{"unsupported"};
    uint64_t Wakeups{0};
    uint64_t EventsProcessed{0};
    uint64_t PendingEvents{0};
    uint64_t PendingBytes{0};
    uint64_t TcpReadBytes{0};
    uint64_t TcpWriteBytes{0};
    uint64_t MaxTcpReadIovUsed{0};
    uint64_t MaxTcpWriteIovUsed{0};
    uint64_t TcpWriteSendmsgCalls{0};
    uint64_t MaxTcpWriteSendmsgBytes{0};
    uint64_t TcpWriteEagainCount{0};
    uint64_t TcpWritePartialCount{0};
    uint64_t ReadDisabledCount{0};
    uint64_t CompressedTcpBytes{0};
    uint64_t DecompressedTcpBytes{0};
    uint64_t DeferredReceiveCompleteBytes{0};
    uint64_t DeferredReceiveCompletes{0};
    uint64_t DeferredReceiveCompletionFlushes{0};
    uint64_t MaxPendingQuicReceiveBytes{0};
    uint64_t MaxPendingQuicReceiveQueue{0};
    uint64_t QuicReceivePausedCount{0};
    uint64_t QuicReceiveResumedCount{0};
    uint64_t InlineQuicReceiveAttempts{0};
    uint64_t InlineQuicReceiveFullWrites{0};
    uint64_t InlineQuicReceivePartialWrites{0};
    uint64_t InlineQuicReceiveEagainCount{0};
    uint64_t InlineQuicReceiveBudgetExceeded{0};
    uint64_t InlineQuicReceiveBytes{0};
    uint64_t MaxInlineQuicReceiveBytes{0};
    uint64_t Errors{0};
};

TqRelayMetricsSnapshot TqSnapshotRelayMetrics();
void TqAppendRelayMetricsJson(std::ostringstream& out, const TqRelayMetricsSnapshot& metrics);
void TqAppendJsonString(std::ostringstream& out, const char* name, const std::string& value);
