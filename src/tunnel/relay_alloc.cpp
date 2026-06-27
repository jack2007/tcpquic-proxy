#include "relay_alloc.h"

#include <algorithm>
#include <atomic>
#include <cstdlib>
#include <cstring>
#include <limits>

#if TCPQUIC_USE_MIMALLOC
#include <mimalloc.h>
#endif

namespace {

constexpr uint64_t TqRelayAllocMagic = 0x7471616c6c6f6301ULL;

struct alignas(std::max_align_t) TqRelayAllocHeader {
  uint64_t Magic;
  size_t RequestedBytes;
};

std::atomic<uint64_t> gRequestedCurrentBytes{0};
std::atomic<uint64_t> gRequestedTotalBytes{0};
std::atomic<uint64_t> gRequestedFreedBytes{0};
std::atomic<uint64_t> gRequestedPeakBytes{0};
std::atomic<uint64_t> gNormalAllocCount{0};

void* TqRawMalloc(size_t bytes) {
#if TCPQUIC_USE_MIMALLOC
  return mi_malloc(bytes);
#else
  return std::malloc(bytes);
#endif
}

void* TqRawCalloc(size_t count, size_t bytes) {
#if TCPQUIC_USE_MIMALLOC
  return mi_calloc(count, bytes);
#else
  return std::calloc(count, bytes);
#endif
}

void TqRawFree(void* ptr) {
#if TCPQUIC_USE_MIMALLOC
  mi_free(ptr);
#else
  std::free(ptr);
#endif
}

void TqUpdatePeak(uint64_t current) {
  uint64_t peak = gRequestedPeakBytes.load(std::memory_order_relaxed);
  while (current > peak &&
      !gRequestedPeakBytes.compare_exchange_weak(
          peak,
          current,
          std::memory_order_relaxed,
          std::memory_order_relaxed)) {
  }
}

void TqRecordAlloc(size_t bytes) {
  const uint64_t amount = static_cast<uint64_t>(bytes);
  const uint64_t current = gRequestedCurrentBytes.fetch_add(amount, std::memory_order_relaxed) + amount;
  gRequestedTotalBytes.fetch_add(amount, std::memory_order_relaxed);
  gNormalAllocCount.fetch_add(1, std::memory_order_relaxed);
  TqUpdatePeak(current);
}

void TqRecordFree(size_t bytes) {
  const uint64_t amount = static_cast<uint64_t>(bytes);
  gRequestedCurrentBytes.fetch_sub(amount, std::memory_order_relaxed);
  gRequestedFreedBytes.fetch_add(amount, std::memory_order_relaxed);
}

TqRelayAllocHeader* TqHeaderFromUserPtr(void* ptr) {
  return reinterpret_cast<TqRelayAllocHeader*>(ptr) - 1;
}

void* TqUserPtrFromHeader(TqRelayAllocHeader* header) {
  return static_cast<void*>(header + 1);
}

} // namespace

void* TqMalloc(size_t bytes) {
  if (bytes > std::numeric_limits<size_t>::max() - sizeof(TqRelayAllocHeader)) {
    return nullptr;
  }
  auto* header = static_cast<TqRelayAllocHeader*>(TqRawMalloc(sizeof(TqRelayAllocHeader) + bytes));
  if (header == nullptr) {
    return nullptr;
  }
  header->Magic = TqRelayAllocMagic;
  header->RequestedBytes = bytes;
  TqRecordAlloc(bytes);
  return TqUserPtrFromHeader(header);
}

void* TqCalloc(size_t count, size_t bytes) {
  if (bytes != 0 && count > std::numeric_limits<size_t>::max() / bytes) {
    return nullptr;
  }
  const size_t requested = count * bytes;
  if (requested > std::numeric_limits<size_t>::max() - sizeof(TqRelayAllocHeader)) {
    return nullptr;
  }
  auto* header = static_cast<TqRelayAllocHeader*>(
      TqRawCalloc(1, sizeof(TqRelayAllocHeader) + requested));
  if (header == nullptr) {
    return nullptr;
  }
  header->Magic = TqRelayAllocMagic;
  header->RequestedBytes = requested;
  TqRecordAlloc(requested);
  return TqUserPtrFromHeader(header);
}

void* TqRealloc(void* ptr, size_t bytes) {
  if (ptr == nullptr) {
    return TqMalloc(bytes);
  }
  if (bytes == 0) {
    TqFree(ptr);
    return nullptr;
  }

  TqRelayAllocHeader* oldHeader = TqHeaderFromUserPtr(ptr);
  if (oldHeader->Magic != TqRelayAllocMagic) {
    return nullptr;
  }

  if (bytes > std::numeric_limits<size_t>::max() - sizeof(TqRelayAllocHeader)) {
    return nullptr;
  }
  auto* newHeader = static_cast<TqRelayAllocHeader*>(TqRawMalloc(sizeof(TqRelayAllocHeader) + bytes));
  if (newHeader == nullptr) {
    return nullptr;
  }
  newHeader->Magic = TqRelayAllocMagic;
  newHeader->RequestedBytes = bytes;

  const size_t oldBytes = oldHeader->RequestedBytes;
  void* newPtr = TqUserPtrFromHeader(newHeader);
  std::memcpy(newPtr, ptr, std::min(oldBytes, bytes));
  oldHeader->Magic = 0;
  TqRecordFree(oldBytes);
  TqRawFree(oldHeader);
  TqRecordAlloc(bytes);
  return newPtr;
}

void TqFree(void* ptr) {
  if (ptr == nullptr) return;
  TqRelayAllocHeader* header = TqHeaderFromUserPtr(ptr);
  if (header->Magic != TqRelayAllocMagic) {
    return;
  }
  const size_t bytes = header->RequestedBytes;
  header->Magic = 0;
  TqRecordFree(bytes);
  TqRawFree(header);
}

void* TqAllocBytes(size_t bytes) {
  return TqMalloc(bytes);
}

void TqFreeBytes(void* ptr, size_t bytes) {
  (void)bytes;
  TqFree(ptr);
}

TqRelayAllocStats TqSnapshotRelayAllocStats() {
  TqRelayAllocStats stats;
  stats.RequestedCurrentBytes = gRequestedCurrentBytes.load(std::memory_order_relaxed);
  stats.RequestedTotalBytes = gRequestedTotalBytes.load(std::memory_order_relaxed);
  stats.RequestedFreedBytes = gRequestedFreedBytes.load(std::memory_order_relaxed);
  stats.RequestedPeakBytes = gRequestedPeakBytes.load(std::memory_order_relaxed);
  stats.NormalAllocCount = gNormalAllocCount.load(std::memory_order_relaxed);
  return stats;
}

void TqResetRelayAllocStatsForTesting() {
  gRequestedCurrentBytes.store(0, std::memory_order_relaxed);
  gRequestedTotalBytes.store(0, std::memory_order_relaxed);
  gRequestedFreedBytes.store(0, std::memory_order_relaxed);
  gRequestedPeakBytes.store(0, std::memory_order_relaxed);
  gNormalAllocCount.store(0, std::memory_order_relaxed);
}
