#pragma once

#include <cstdint>
#include <string>

struct TqMemoryAllocatorStats {
    bool MimallocEnabled{false};
    bool Available{false};
    uint64_t RequestedCurrentBytes{0};
    uint64_t RequestedTotalBytes{0};
    uint64_t RequestedFreedBytes{0};
    uint64_t RequestedPeakBytes{0};
    uint64_t ReservedCurrentBytes{0};
    uint64_t CommittedCurrentBytes{0};
    uint64_t PageCommittedCurrentBytes{0};
    uint64_t NormalAllocCount{0};
    uint64_t HugeAllocCount{0};
    uint64_t MmapCalls{0};
    uint64_t CommitCalls{0};
    uint64_t PurgeCalls{0};
    uint64_t ThreadsCurrent{0};
};

TqMemoryAllocatorStats TqSnapshotMemoryAllocatorStats();
std::string TqFormatMemoryAllocatorStatsLine(const TqMemoryAllocatorStats& stats);
std::string TqMemoryAllocatorStatsJson(const TqMemoryAllocatorStats& stats);
void TqDumpMemoryAllocatorStatsToLog(const TqMemoryAllocatorStats& stats);
TqMemoryAllocatorStats TqDumpMemoryAllocatorStatsToLog();
