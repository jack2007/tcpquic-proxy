#include "relay_alloc.h"

#include <cstdint>
#include <cstring>

int main() {
    TqResetRelayAllocStatsForTesting();
    TqRelayAllocStats stats = TqSnapshotRelayAllocStats();
    if (stats.RequestedCurrentBytes != 0) return 6;
    if (stats.RequestedTotalBytes != 0) return 7;
    if (stats.RequestedFreedBytes != 0) return 8;
    if (stats.RequestedPeakBytes != 0) return 9;
    if (stats.NormalAllocCount != 0) return 10;

    auto* bytes = static_cast<std::uint8_t*>(TqMalloc(16));
    if (bytes == nullptr) {
        return 1;
    }
    std::memset(bytes, 0x5a, 16);
    stats = TqSnapshotRelayAllocStats();
    if (stats.RequestedCurrentBytes != 16) return 11;
    if (stats.RequestedTotalBytes != 16) return 12;
    if (stats.RequestedFreedBytes != 0) return 13;
    if (stats.RequestedPeakBytes != 16) return 14;
    if (stats.NormalAllocCount != 1) return 15;
    TqFree(bytes);
    stats = TqSnapshotRelayAllocStats();
    if (stats.RequestedCurrentBytes != 0) return 16;
    if (stats.RequestedTotalBytes != 16) return 17;
    if (stats.RequestedFreedBytes != 16) return 18;
    if (stats.RequestedPeakBytes != 16) return 19;
    if (stats.NormalAllocCount != 1) return 20;

    auto* zeroed = static_cast<std::uint8_t*>(TqCalloc(8, sizeof(std::uint8_t)));
    if (zeroed == nullptr) {
        return 2;
    }
    for (size_t i = 0; i < 8; ++i) {
        if (zeroed[i] != 0) {
            TqFree(zeroed);
            return 3;
        }
    }

    auto* grown = static_cast<std::uint8_t*>(TqRealloc(zeroed, 32));
    if (grown == nullptr) {
        TqFree(zeroed);
        return 4;
    }
    for (size_t i = 0; i < 8; ++i) {
        if (grown[i] != 0) {
            TqFree(grown);
            return 5;
        }
    }
    std::memset(grown + 8, 0xa5, 24);
    stats = TqSnapshotRelayAllocStats();
    if (stats.RequestedCurrentBytes != 32) return 21;
    if (stats.RequestedTotalBytes != 56) return 22;
    if (stats.RequestedFreedBytes != 24) return 23;
    if (stats.RequestedPeakBytes != 32) return 24;
    if (stats.NormalAllocCount != 3) return 25;
    TqFree(grown);
    stats = TqSnapshotRelayAllocStats();
    if (stats.RequestedCurrentBytes != 0) return 26;
    if (stats.RequestedTotalBytes != 56) return 27;
    if (stats.RequestedFreedBytes != 56) return 28;
    if (stats.RequestedPeakBytes != 32) return 29;
    if (stats.NormalAllocCount != 3) return 30;

    TqFree(nullptr);
    return 0;
}
