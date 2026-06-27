#pragma once
#include <cstddef>
#include <cstdint>

struct TqRelayAllocStats {
    uint64_t RequestedCurrentBytes{0};
    uint64_t RequestedTotalBytes{0};
    uint64_t RequestedFreedBytes{0};
    uint64_t RequestedPeakBytes{0};
    uint64_t NormalAllocCount{0};
};

void* TqMalloc(size_t bytes);
void* TqCalloc(size_t count, size_t bytes);
void* TqRealloc(void* ptr, size_t bytes);
void TqFree(void* ptr);

void* TqAllocBytes(size_t bytes);
void TqFreeBytes(void* ptr, size_t bytes);

TqRelayAllocStats TqSnapshotRelayAllocStats();
void TqResetRelayAllocStatsForTesting();
