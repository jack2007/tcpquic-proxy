#include "libuv_allocator.h"

#include <atomic>
#include <cassert>
#include <mutex>
#include <new>

#if defined(TQ_UNIT_TESTING) && TQ_UNIT_TESTING
#include <shared_mutex>
#include <thread>
#endif

#if TCPQUIC_USE_MIMALLOC
#include <mimalloc.h>
#endif

namespace {

std::once_flag gInstallOnce;

enum class AllocatorPhase : std::uint8_t {
    NotAttempted,
    Installing,
    Installed,
    Failed,
};

std::atomic<AllocatorPhase> gPhase{AllocatorPhase::NotAttempted};
std::atomic<int> gStatus{0};
std::atomic<std::uint64_t> gMallocCalls{0};
std::atomic<std::uint64_t> gReallocCalls{0};
std::atomic<std::uint64_t> gCallocCalls{0};
std::atomic<std::uint64_t> gFreeCalls{0};

#if TCPQUIC_USE_MIMALLOC
void* TqUvMiMalloc(std::size_t size) {
    gMallocCalls.fetch_add(1, std::memory_order_relaxed);
    return mi_malloc(size);
}

void* TqUvMiRealloc(void* pointer, std::size_t size) {
    gReallocCalls.fetch_add(1, std::memory_order_relaxed);
    return mi_realloc(pointer, size);
}

void* TqUvMiCalloc(std::size_t count, std::size_t size) {
    gCallocCalls.fetch_add(1, std::memory_order_relaxed);
    return mi_calloc(count, size);
}

void TqUvMiFree(void* pointer) {
    gFreeCalls.fetch_add(1, std::memory_order_relaxed);
    mi_free(pointer);
}

using ReplaceAllocatorFn = int (*)(uv_malloc_func,
                                   uv_realloc_func,
                                   uv_calloc_func,
                                   uv_free_func);
ReplaceAllocatorFn gReplaceAllocator = uv_replace_allocator;
#endif

#if defined(TQ_UNIT_TESTING) && TQ_UNIT_TESTING
std::shared_mutex gTestOperationMutex;
std::thread::id gTestControlThread;
std::atomic<bool> gRealReplacementCalled{false};

bool TqUvClaimTestControlThread() {
    const auto currentThread = std::this_thread::get_id();
    if (gTestControlThread == std::thread::id{}) {
        gTestControlThread = currentThread;
    }
    const bool isControlThread = gTestControlThread == currentThread;
    assert(isControlThread);
    return isControlThread;
}
#endif

} // namespace

bool TqUvInstallAllocator() noexcept {
#if defined(TQ_UNIT_TESTING) && TQ_UNIT_TESTING
    std::shared_lock operationLock(gTestOperationMutex);
#endif
#if TCPQUIC_USE_MIMALLOC
    std::call_once(gInstallOnce, [] {
        gPhase.store(AllocatorPhase::Installing, std::memory_order_release);
        const auto replaceAllocator = gReplaceAllocator;
#if defined(TQ_UNIT_TESTING) && TQ_UNIT_TESTING
        if (replaceAllocator == uv_replace_allocator) {
            gRealReplacementCalled.store(true, std::memory_order_relaxed);
        }
#endif
        const int status = replaceAllocator(
            TqUvMiMalloc, TqUvMiRealloc, TqUvMiCalloc, TqUvMiFree);
        gStatus.store(status, std::memory_order_relaxed);
        gPhase.store(status == 0 ? AllocatorPhase::Installed
                                 : AllocatorPhase::Failed,
                     std::memory_order_release);
    });
    return gPhase.load(std::memory_order_acquire) == AllocatorPhase::Installed;
#else
    return true;
#endif
}

TqUvAllocatorSnapshot TqUvAllocatorStatus() noexcept {
#if defined(TQ_UNIT_TESTING) && TQ_UNIT_TESTING
    std::shared_lock operationLock(gTestOperationMutex);
#endif
    const auto phase = gPhase.load(std::memory_order_acquire);
    return {
#if TCPQUIC_USE_MIMALLOC
        TqUvAllocatorMode::Mimalloc,
#else
        TqUvAllocatorMode::System,
#endif
        phase != AllocatorPhase::NotAttempted,
        phase == AllocatorPhase::Installing,
        phase == AllocatorPhase::Installed,
        phase == AllocatorPhase::Failed
            ? gStatus.load(std::memory_order_relaxed)
            : 0,
        gMallocCalls.load(std::memory_order_relaxed),
        gReallocCalls.load(std::memory_order_relaxed),
        gCallocCalls.load(std::memory_order_relaxed),
        gFreeCalls.load(std::memory_order_relaxed),
    };
}

#if defined(TQ_UNIT_TESTING) && TQ_UNIT_TESTING
void TqUvSetReplaceAllocatorForTest(TqUvReplaceAllocatorFn replaceAllocator) {
    std::unique_lock controlLock(gTestOperationMutex, std::try_to_lock);
    assert(controlLock.owns_lock());
    if (!controlLock.owns_lock() || !TqUvClaimTestControlThread()) {
        return;
    }
#if TCPQUIC_USE_MIMALLOC
    if (replaceAllocator != nullptr &&
        gPhase.load(std::memory_order_acquire) ==
            AllocatorPhase::NotAttempted) {
        gReplaceAllocator = replaceAllocator;
    }
#else
    (void)replaceAllocator;
#endif
}

void TqUvResetAllocatorStateForTest() {
    std::unique_lock controlLock(gTestOperationMutex, std::try_to_lock);
    assert(controlLock.owns_lock());
    if (!controlLock.owns_lock() || !TqUvClaimTestControlThread()) {
        return;
    }
    const bool realReplacementCalled =
        gRealReplacementCalled.load(std::memory_order_relaxed);
    assert(!realReplacementCalled);
    if (realReplacementCalled) {
        return;
    }
    gInstallOnce.~once_flag();
    new (&gInstallOnce) std::once_flag();
    gPhase.store(AllocatorPhase::NotAttempted, std::memory_order_relaxed);
    gStatus.store(0, std::memory_order_relaxed);
    gMallocCalls.store(0, std::memory_order_relaxed);
    gReallocCalls.store(0, std::memory_order_relaxed);
    gCallocCalls.store(0, std::memory_order_relaxed);
    gFreeCalls.store(0, std::memory_order_relaxed);
#if TCPQUIC_USE_MIMALLOC
    gReplaceAllocator = uv_replace_allocator;
#endif
}
#endif
