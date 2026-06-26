#include "memory_stats.h"

#include <cstdio>
#include <sstream>

#ifndef TCPQUIC_USE_MIMALLOC
#define TCPQUIC_USE_MIMALLOC 0
#endif

#if TCPQUIC_USE_MIMALLOC
#include <mimalloc-stats.h>
#endif

namespace {

#if TCPQUIC_USE_MIMALLOC
uint64_t TqNonNegativeInt64(int64_t value) {
    return value <= 0 ? 0 : static_cast<uint64_t>(value);
}
#endif

const char* TqBoolJson(bool value) {
    return value ? "true" : "false";
}

} // namespace

TqMemoryAllocatorStats TqSnapshotMemoryAllocatorStats() {
    TqMemoryAllocatorStats out{};
#if TCPQUIC_USE_MIMALLOC
    out.MimallocEnabled = true;

    mi_stats_t stats;
    mi_stats_init(&stats);
    if (!mi_stats_get(&stats)) {
        return out;
    }

    out.Available = true;
    out.RequestedCurrentBytes = TqNonNegativeInt64(stats.malloc_requested.current);
    out.RequestedTotalBytes = TqNonNegativeInt64(stats.malloc_requested.total);
    out.RequestedPeakBytes = TqNonNegativeInt64(stats.malloc_requested.peak);
    out.RequestedFreedBytes = out.RequestedTotalBytes >= out.RequestedCurrentBytes
        ? out.RequestedTotalBytes - out.RequestedCurrentBytes
        : 0;
    out.ReservedCurrentBytes = TqNonNegativeInt64(stats.reserved.current);
    out.CommittedCurrentBytes = TqNonNegativeInt64(stats.committed.current);
    out.PageCommittedCurrentBytes = TqNonNegativeInt64(stats.page_committed.current);
    out.NormalAllocCount = TqNonNegativeInt64(stats.malloc_normal_count.total);
    out.HugeAllocCount = TqNonNegativeInt64(stats.malloc_huge_count.total);
    out.MmapCalls = TqNonNegativeInt64(stats.mmap_calls.total);
    out.CommitCalls = TqNonNegativeInt64(stats.commit_calls.total);
    out.PurgeCalls = TqNonNegativeInt64(stats.purge_calls.total);
    out.ThreadsCurrent = TqNonNegativeInt64(stats.threads.current);
#endif
    return out;
}

std::string TqFormatMemoryAllocatorStatsLine(const TqMemoryAllocatorStats& stats) {
    char buffer[1536];
    std::snprintf(
        buffer,
        sizeof(buffer),
        "allocator=mimalloc enabled=%d available=%d "
        "requested_current_bytes=%llu requested_total_bytes=%llu "
        "requested_freed_bytes=%llu requested_peak_bytes=%llu "
        "reserved_current_bytes=%llu committed_current_bytes=%llu "
        "page_committed_current_bytes=%llu normal_alloc_count=%llu "
        "huge_alloc_count=%llu mmap_calls=%llu commit_calls=%llu "
        "purge_calls=%llu threads_current=%llu",
        stats.MimallocEnabled ? 1 : 0,
        stats.Available ? 1 : 0,
        static_cast<unsigned long long>(stats.RequestedCurrentBytes),
        static_cast<unsigned long long>(stats.RequestedTotalBytes),
        static_cast<unsigned long long>(stats.RequestedFreedBytes),
        static_cast<unsigned long long>(stats.RequestedPeakBytes),
        static_cast<unsigned long long>(stats.ReservedCurrentBytes),
        static_cast<unsigned long long>(stats.CommittedCurrentBytes),
        static_cast<unsigned long long>(stats.PageCommittedCurrentBytes),
        static_cast<unsigned long long>(stats.NormalAllocCount),
        static_cast<unsigned long long>(stats.HugeAllocCount),
        static_cast<unsigned long long>(stats.MmapCalls),
        static_cast<unsigned long long>(stats.CommitCalls),
        static_cast<unsigned long long>(stats.PurgeCalls),
        static_cast<unsigned long long>(stats.ThreadsCurrent));
    return buffer;
}

std::string TqMemoryAllocatorStatsJson(const TqMemoryAllocatorStats& stats) {
    std::ostringstream out;
    out << "{\"status\":\"dumped\""
        << ",\"allocator\":\"mimalloc\""
        << ",\"enabled\":" << TqBoolJson(stats.MimallocEnabled)
        << ",\"available\":" << TqBoolJson(stats.Available)
        << ",\"requested_current_bytes\":" << stats.RequestedCurrentBytes
        << ",\"requested_total_bytes\":" << stats.RequestedTotalBytes
        << ",\"requested_freed_bytes\":" << stats.RequestedFreedBytes
        << ",\"requested_peak_bytes\":" << stats.RequestedPeakBytes
        << ",\"reserved_current_bytes\":" << stats.ReservedCurrentBytes
        << ",\"committed_current_bytes\":" << stats.CommittedCurrentBytes
        << ",\"page_committed_current_bytes\":" << stats.PageCommittedCurrentBytes
        << ",\"normal_alloc_count\":" << stats.NormalAllocCount
        << ",\"huge_alloc_count\":" << stats.HugeAllocCount
        << ",\"mmap_calls\":" << stats.MmapCalls
        << ",\"commit_calls\":" << stats.CommitCalls
        << ",\"purge_calls\":" << stats.PurgeCalls
        << ",\"threads_current\":" << stats.ThreadsCurrent
        << '}';
    return out.str();
}

void TqDumpMemoryAllocatorStatsToLog(const TqMemoryAllocatorStats& stats) {
    std::fprintf(
        stderr,
        "tcpquic-proxy memory_allocator_stats: %s\n",
        TqFormatMemoryAllocatorStatsLine(stats).c_str());
    std::fflush(stderr);
}

TqMemoryAllocatorStats TqDumpMemoryAllocatorStatsToLog() {
    const TqMemoryAllocatorStats stats = TqSnapshotMemoryAllocatorStats();
    TqDumpMemoryAllocatorStatsToLog(stats);
    return stats;
}
