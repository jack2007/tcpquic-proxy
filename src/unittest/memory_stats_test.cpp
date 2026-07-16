#include "memory_stats.h"
#include "relay_alloc.h"

#include <nlohmann/json.hpp>

#include <string>

#if TCPQUIC_USE_MIMALLOC
#include <mimalloc.h>
#include <mimalloc-stats.h>
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

    nlohmann::json json;
    try {
        json = nlohmann::json::parse(TqMemoryAllocatorStatsJson(stats));
    } catch (const nlohmann::json::exception&) {
        return 17;
    }
    if (json["status"] != "dumped") return 18;
    if (json["compiled_relay_backend"] != "native") return 46;
    if (json["allocator"] != "mimalloc") return 19;
    if (json["enabled"] != true) return 20;
    if (json["available"] != true) return 21;
    if (json["requested_current_bytes"] != 123) return 22;
    if (json["requested_total_bytes"] != 1000) return 23;
    if (json["requested_freed_bytes"] != 877) return 24;
    if (json["requested_peak_bytes"] != 456) return 25;
    if (json["reserved_current_bytes"] != 2048) return 26;
    if (json["committed_current_bytes"] != 1024) return 27;
    if (json["page_committed_current_bytes"] != 512) return 28;
    if (json["normal_alloc_count"] != 10) return 29;
    if (json["huge_alloc_count"] != 1) return 30;
    if (json["mmap_calls"] != 2) return 31;
    if (json["commit_calls"] != 3) return 32;
    if (json["purge_calls"] != 4) return 33;
    if (json["threads_current"] != 5) return 44;

    const TqMemoryAllocatorStats snapshot = TqSnapshotMemoryAllocatorStats();
#if TCPQUIC_USE_MIMALLOC
    TqResetRelayAllocStatsForTesting();
    void* trackedAllocation = TqMalloc(64);
    if (trackedAllocation == nullptr) return 46;
    TqMemoryAllocatorStats trackedSnapshot = TqSnapshotMemoryAllocatorStats();
    TqFree(trackedAllocation);
    if (trackedSnapshot.RequestedCurrentBytes != 64) return 47;
    if (trackedSnapshot.RequestedTotalBytes != 64) return 48;
    if (trackedSnapshot.RequestedFreedBytes != 0) return 49;
    if (trackedSnapshot.RequestedPeakBytes != 64) return 50;
    if (trackedSnapshot.NormalAllocCount != 1) return 51;
    TqResetRelayAllocStatsForTesting();

    mi_stats_t rawStats;
    mi_stats_init(&rawStats);
    rawStats.malloc_normal.current = 4096;
    rawStats.malloc_normal.total = 8192;
    rawStats.malloc_normal.peak = 4096;
    rawStats.committed.current = 16384;
    rawStats.page_committed.current = 65536;
    const TqMemoryAllocatorStats mappedStats = TqMemoryAllocatorStatsFromMimallocStats(rawStats);
    if (mappedStats.PageCommittedCurrentBytes > mappedStats.CommittedCurrentBytes) return 43;

    if (!snapshot.MimallocEnabled) return 34;
    if (!snapshot.Available) return 35;

    void* allocation = TqMalloc(4096);
    if (allocation == nullptr) return 36;
    static_cast<unsigned char*>(allocation)[0] = 0x5a;
    const TqMemoryAllocatorStats allocatedSnapshot = TqSnapshotMemoryAllocatorStats();
    TqFree(allocation);
    const TqMemoryAllocatorStats freedSnapshot = TqSnapshotMemoryAllocatorStats();

    if (allocatedSnapshot.RequestedCurrentBytes == 0) return 37;
    if (allocatedSnapshot.RequestedTotalBytes == 0) return 38;
    if (allocatedSnapshot.NormalAllocCount == 0) return 39;
    if (freedSnapshot.RequestedCurrentBytes >= allocatedSnapshot.RequestedCurrentBytes) return 42;
    if (allocatedSnapshot.PageCommittedCurrentBytes > allocatedSnapshot.CommittedCurrentBytes) return 44;
    if (freedSnapshot.PageCommittedCurrentBytes > freedSnapshot.CommittedCurrentBytes) return 45;
#else
    if (snapshot.MimallocEnabled) return 40;
    if (snapshot.Available) return 41;
#endif

    return 0;
}
