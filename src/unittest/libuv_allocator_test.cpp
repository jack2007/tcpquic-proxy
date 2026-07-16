#include "tunnel/libuv_allocator.h"

#ifdef NDEBUG
#undef NDEBUG
#endif
#include <cassert>
#include <atomic>
#include <cstddef>
#include <thread>

namespace {

int ReplaceCalls = 0;
uv_malloc_func InstalledMalloc = nullptr;
uv_realloc_func InstalledRealloc = nullptr;
uv_calloc_func InstalledCalloc = nullptr;
uv_free_func InstalledFree = nullptr;
std::atomic<bool> BlockingReplaceEntered{false};
std::atomic<bool> ReleaseBlockingReplace{false};
std::atomic<int> BlockingReplaceStatus{0};

int FakeReplace(uv_malloc_func malloc_func,
                uv_realloc_func realloc_func,
                uv_calloc_func calloc_func,
                uv_free_func free_func) {
    ++ReplaceCalls;
    InstalledMalloc = malloc_func;
    InstalledRealloc = realloc_func;
    InstalledCalloc = calloc_func;
    InstalledFree = free_func;
    return 0;
}

int FailingReplace(uv_malloc_func,
                   uv_realloc_func,
                   uv_calloc_func,
                   uv_free_func) {
    ++ReplaceCalls;
    return UV_EINVAL;
}

[[maybe_unused]] int BlockingReplace(uv_malloc_func,
                                     uv_realloc_func,
                                     uv_calloc_func,
                                     uv_free_func) {
    ++ReplaceCalls;
    BlockingReplaceEntered.store(true, std::memory_order_release);
    while (!ReleaseBlockingReplace.load(std::memory_order_acquire)) {
        std::this_thread::yield();
    }
    return BlockingReplaceStatus.load(std::memory_order_relaxed);
}

void ResetHarness() {
    ReplaceCalls = 0;
    InstalledMalloc = nullptr;
    InstalledRealloc = nullptr;
    InstalledCalloc = nullptr;
    InstalledFree = nullptr;
    BlockingReplaceEntered.store(false, std::memory_order_relaxed);
    ReleaseBlockingReplace.store(false, std::memory_order_relaxed);
    BlockingReplaceStatus.store(0, std::memory_order_relaxed);
    TqUvResetAllocatorStateForTest();
}

void TestSuccessfulInstallIsExactlyOnce() {
    ResetHarness();
    TqUvSetReplaceAllocatorForTest(FakeReplace);

    assert(TqUvInstallAllocator());
    assert(TqUvInstallAllocator());

#if TCPQUIC_USE_MIMALLOC
    assert(ReplaceCalls == 1);
    const auto status = TqUvAllocatorStatus();
    assert(status.Mode == TqUvAllocatorMode::Mimalloc);
    assert(status.Attempted);
    assert(!status.InProgress);
    assert(status.Installed);
    assert(status.Status == 0);
#else
    assert(ReplaceCalls == 0);
    const auto status = TqUvAllocatorStatus();
    assert(status.Mode == TqUvAllocatorMode::System);
    assert(!status.Attempted);
    assert(!status.InProgress);
    assert(!status.Installed);
    assert(status.Status == 0);
#endif
}

void TestFailedInstallIsSticky() {
    ResetHarness();
    TqUvSetReplaceAllocatorForTest(FailingReplace);

#if TCPQUIC_USE_MIMALLOC
    assert(!TqUvInstallAllocator());
    assert(!TqUvInstallAllocator());
    assert(ReplaceCalls == 1);
    const auto status = TqUvAllocatorStatus();
    assert(status.Mode == TqUvAllocatorMode::Mimalloc);
    assert(status.Attempted);
    assert(!status.InProgress);
    assert(!status.Installed);
    assert(status.Status == UV_EINVAL);
#else
    assert(TqUvInstallAllocator());
    assert(TqUvInstallAllocator());
    assert(ReplaceCalls == 0);
#endif
}

void TestConcurrentSnapshotPublishesCoherentPhase(int replacementStatus) {
#if TCPQUIC_USE_MIMALLOC
    ResetHarness();
    BlockingReplaceStatus.store(replacementStatus, std::memory_order_relaxed);
    TqUvSetReplaceAllocatorForTest(BlockingReplace);

    bool installResult = false;
    std::thread installer([&] { installResult = TqUvInstallAllocator(); });
    while (!BlockingReplaceEntered.load(std::memory_order_acquire)) {
        std::this_thread::yield();
    }

    const auto installing = TqUvAllocatorStatus();
    assert(installing.Mode == TqUvAllocatorMode::Mimalloc);
    assert(installing.Attempted);
    assert(installing.InProgress);
    assert(!installing.Installed);
    assert(installing.Status == 0);

    ReleaseBlockingReplace.store(true, std::memory_order_release);
    installer.join();

    const auto finished = TqUvAllocatorStatus();
    assert(finished.Attempted);
    assert(!finished.InProgress);
    assert(finished.Installed == (replacementStatus == 0));
    assert(finished.Status == replacementStatus);
    assert(installResult == (replacementStatus == 0));
    assert(ReplaceCalls == 1);
#else
    (void)replacementStatus;
#endif
}

void TestAllocatorCallbacksAreCounted() {
#if TCPQUIC_USE_MIMALLOC
    ResetHarness();
    TqUvSetReplaceAllocatorForTest(FakeReplace);
    assert(TqUvInstallAllocator());

    void* block = InstalledMalloc(8);
    assert(block != nullptr);
    block = InstalledRealloc(block, 16);
    assert(block != nullptr);
    void* zeroed = InstalledCalloc(2, 8);
    assert(zeroed != nullptr);
    InstalledFree(block);
    InstalledFree(zeroed);

    const auto status = TqUvAllocatorStatus();
    assert(status.MallocCalls == 1);
    assert(status.ReallocCalls == 1);
    assert(status.CallocCalls == 1);
    assert(status.FreeCalls == 2);
#endif
}

} // namespace

int main() {
    // The injected replacement never calls a libuv API. Every allocation made
    // through a captured callback is freed before the next test resets state.
    TestSuccessfulInstallIsExactlyOnce();
    TestFailedInstallIsSticky();
    TestConcurrentSnapshotPublishesCoherentPhase(0);
    TestConcurrentSnapshotPublishesCoherentPhase(UV_EINVAL);
    TestAllocatorCallbacksAreCounted();
    return 0;
}
