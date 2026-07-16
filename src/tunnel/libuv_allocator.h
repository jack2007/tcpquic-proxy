#pragma once

#include <cstdint>

#include <uv.h>

enum class TqUvAllocatorMode {
    Mimalloc,
    System,
};

struct TqUvAllocatorSnapshot {
    TqUvAllocatorMode Mode;
    bool Attempted;
    bool InProgress;
    bool Installed;
    int Status;
    std::uint64_t MallocCalls;
    std::uint64_t ReallocCalls;
    std::uint64_t CallocCalls;
    std::uint64_t FreeCalls;
};

bool TqUvInstallAllocator() noexcept;
TqUvAllocatorSnapshot TqUvAllocatorStatus() noexcept;

#if defined(TQ_UNIT_TESTING) && TQ_UNIT_TESTING
using TqUvReplaceAllocatorFn = int (*)(uv_malloc_func,
                                       uv_realloc_func,
                                       uv_calloc_func,
                                       uv_free_func);

// Test-only bootstrap controls. Call them from one control thread only, never
// concurrently with install/status, before any real replacement or libuv API,
// and with no allocation from a prior injected callback still alive.
void TqUvSetReplaceAllocatorForTest(TqUvReplaceAllocatorFn replaceAllocator);
void TqUvResetAllocatorStateForTest();
#endif
