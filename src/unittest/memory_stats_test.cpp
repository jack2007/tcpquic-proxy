#include "memory_stats.h"

#include <string>

#if TCPQUIC_USE_MIMALLOC
#include <mimalloc.h>
#endif

int main() {
    TqMemoryAllocatorStats stats{};
    stats.MimallocEnabled = true;
    stats.Available = true;
    stats.RequestedCurrentBytes = 123;
    stats.RequestedTotalBytes = 1000;
    stats.RequestedFreedBytes = 877;
    stats.RequestedPeakBytes = 456;
    stats.ReservedCurrentBytes = 2048;
    stats.CommittedCurrentBytes = 1024;
    stats.PageCommittedCurrentBytes = 512;
    stats.NormalAllocCount = 10;
    stats.HugeAllocCount = 1;
    stats.MmapCalls = 2;
    stats.CommitCalls = 3;
    stats.PurgeCalls = 4;
    stats.ThreadsCurrent = 5;

    const std::string line = TqFormatMemoryAllocatorStatsLine(stats);
    if (line.find("allocator=mimalloc") == std::string::npos) return 1;
    if (line.find("enabled=1") == std::string::npos) return 2;
    if (line.find("available=1") == std::string::npos) return 3;
    if (line.find("requested_current_bytes=123") == std::string::npos) return 4;
    if (line.find("requested_total_bytes=1000") == std::string::npos) return 5;
    if (line.find("requested_freed_bytes=877") == std::string::npos) return 6;
    if (line.find("requested_peak_bytes=456") == std::string::npos) return 7;
    if (line.find("reserved_current_bytes=2048") == std::string::npos) return 8;
    if (line.find("committed_current_bytes=1024") == std::string::npos) return 9;
    if (line.find("page_committed_current_bytes=512") == std::string::npos) return 10;
    if (line.find("normal_alloc_count=10") == std::string::npos) return 11;
    if (line.find("huge_alloc_count=1") == std::string::npos) return 12;
    if (line.find("mmap_calls=2") == std::string::npos) return 13;
    if (line.find("commit_calls=3") == std::string::npos) return 14;
    if (line.find("purge_calls=4") == std::string::npos) return 15;
    if (line.find("threads_current=5") == std::string::npos) return 16;

    const std::string json = TqMemoryAllocatorStatsJson(stats);
    if (json.find("\"status\":\"dumped\"") == std::string::npos) return 17;
    if (json.find("\"allocator\":\"mimalloc\"") == std::string::npos) return 18;
    if (json.find("\"enabled\":true") == std::string::npos) return 19;
    if (json.find("\"available\":true") == std::string::npos) return 20;
    if (json.find("\"requested_current_bytes\":123") == std::string::npos) return 21;
    if (json.find("\"requested_total_bytes\":1000") == std::string::npos) return 22;
    if (json.find("\"requested_freed_bytes\":877") == std::string::npos) return 23;
    if (json.find("\"requested_peak_bytes\":456") == std::string::npos) return 24;
    if (json.find("\"reserved_current_bytes\":2048") == std::string::npos) return 25;
    if (json.find("\"committed_current_bytes\":1024") == std::string::npos) return 26;
    if (json.find("\"page_committed_current_bytes\":512") == std::string::npos) return 27;
    if (json.find("\"normal_alloc_count\":10") == std::string::npos) return 28;
    if (json.find("\"huge_alloc_count\":1") == std::string::npos) return 29;
    if (json.find("\"mmap_calls\":2") == std::string::npos) return 30;
    if (json.find("\"commit_calls\":3") == std::string::npos) return 31;
    if (json.find("\"purge_calls\":4") == std::string::npos) return 32;
    if (json.find("\"threads_current\":5") == std::string::npos) return 33;

    const TqMemoryAllocatorStats snapshot = TqSnapshotMemoryAllocatorStats();
#if TCPQUIC_USE_MIMALLOC
    if (!snapshot.MimallocEnabled) return 34;
    if (!snapshot.Available) return 35;

    void* allocation = mi_malloc(4096);
    if (allocation == nullptr) return 36;
    static_cast<unsigned char*>(allocation)[0] = 0x5a;
    const TqMemoryAllocatorStats allocatedSnapshot = TqSnapshotMemoryAllocatorStats();
    mi_free(allocation);

    if (allocatedSnapshot.RequestedCurrentBytes == 0) return 37;
    if (allocatedSnapshot.RequestedTotalBytes == 0) return 38;
    if (allocatedSnapshot.NormalAllocCount == 0) return 39;
#else
    if (snapshot.MimallocEnabled) return 40;
    if (snapshot.Available) return 41;
#endif

    return 0;
}
