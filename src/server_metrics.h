#pragma once

#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>

struct TqServerMetrics {
    mutable std::mutex Lock;
    std::string Listen;
    std::atomic<uint64_t> AcceptedConnections{0};
    std::atomic<uint64_t> ActiveStreams{0};
    std::atomic<uint64_t> TotalStreams{0};
    std::atomic<uint64_t> AclDenied{0};
    uint64_t LinuxRelayWakeups{0};
    uint64_t LinuxRelayEventsProcessed{0};
    uint64_t LinuxRelayPendingEvents{0};
    uint64_t LinuxRelayPendingBytes{0};
    uint64_t LinuxRelayTcpReadBytes{0};
    uint64_t LinuxRelayTcpWriteBytes{0};
    uint64_t LinuxRelayReadDisabledCount{0};
    std::string LastError;
};

std::string TqServerHealthJson(const TqServerMetrics& metrics, uint64_t uptimeSeconds);
std::string TqServerMetricsJson(const TqServerMetrics& metrics, uint64_t uptimeSeconds);
void TqServerMetricsConnectionAccepted(TqServerMetrics& metrics);
void TqServerMetricsStreamStarted(TqServerMetrics& metrics);
void TqServerMetricsStreamFinished(TqServerMetrics& metrics);
