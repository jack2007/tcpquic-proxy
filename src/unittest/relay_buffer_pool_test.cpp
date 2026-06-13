#include "relay_buffer_pool.h"

#include <atomic>
#include <cassert>
#include <cstring>
#include <memory>
#include <thread>
#include <type_traits>
#include <vector>

int main() {
    static_assert(!std::is_same<TqBufferRef, std::shared_ptr<TqRelayBufferSlot>>::value,
        "TqBufferRef must not allocate shared_ptr control blocks");
    static_assert(!std::is_copy_constructible<TqBufferRef>::value,
        "TqBufferRef is expected to be move-only");
    static_assert(std::is_move_constructible<TqBufferRef>::value,
        "TqBufferRef must support ownership transfer");

    {
        TqRelayBufferPool pool(4096, 2, 12288);
        pool.Reserve(2);

        TqBufferAcquireFailure failure = TqBufferAcquireFailure::None;
        auto first = pool.Acquire(TqBufferDomain::Worker, &failure);
        auto second = pool.AcquireWorker(&failure);
        auto denied = pool.AcquireWorker(&failure);
        assert(first);
        assert(second);
        assert(!denied);
        assert(failure == TqBufferAcquireFailure::SlotLimit);
        assert(pool.PendingBytes() == 8192);
    }

    {
        TqRelayBufferPool pool(64, 4, 512);
        pool.Reserve(4);
        assert(pool.FreeCount() == 4);

        auto a = pool.AcquireWorker();
        auto b = pool.AcquireWorker();
        assert(a);
        assert(b);
        assert(pool.PendingBytes() == 128);

        a.reset();
        assert(pool.PendingBytes() == 64);

        b.reset();
        assert(pool.PendingBytes() == 0);
        assert(pool.FreeCount() == 4);
    }

    {
        TqRelayBufferPool pool(64, 4, 32);
        TqBufferAcquireFailure reason = TqBufferAcquireFailure::None;
        auto worker = pool.AcquireWorker(&reason);
        assert(!worker);
        assert(reason == TqBufferAcquireFailure::PendingBytesLimit);
    }

    {
        TqRelayBufferPool pool(64, 2, 512);
        pool.Reserve(2);
        TqBufferAcquireFailure reason = TqBufferAcquireFailure::None;
        auto first = pool.AcquireWorker(&reason);
        auto second = pool.AcquireWorker(&reason);
        auto third = pool.AcquireWorker(&reason);
        assert(first);
        assert(second);
        assert(!third);
        assert(reason == TqBufferAcquireFailure::SlotLimit);
    }

    {
        TqRelayBufferPool pool(64, 2, 128);
        auto first = pool.AcquireWorker();
        assert(first);
        assert(pool.PendingBytes() == 64);
        first.abandon();
        assert(!first);
        assert(pool.PendingBytes() == 0);
        assert(pool.FreeCount() == 1);
    }

    {
        constexpr size_t ThreadCount = 16;
        TqRelayBufferPool pool(1, ThreadCount, 1);
        pool.Reserve(ThreadCount);
        std::atomic<size_t> successes{0};
        std::atomic<size_t> maxObservedPending{0};
        std::vector<std::thread> threads;

        for (size_t i = 0; i < ThreadCount; ++i) {
            threads.emplace_back([&]() {
                TqBufferAcquireFailure failure = TqBufferAcquireFailure::None;
                auto buffer = pool.AcquireWorker(&failure);
                const size_t pending = static_cast<size_t>(pool.PendingBytes());
                size_t observed = maxObservedPending.load(std::memory_order_relaxed);
                while (observed < pending &&
                       !maxObservedPending.compare_exchange_weak(
                           observed,
                           pending,
                           std::memory_order_relaxed,
                           std::memory_order_relaxed)) {
                }
                if (buffer) {
                    successes.fetch_add(1, std::memory_order_relaxed);
                } else {
                    assert(failure == TqBufferAcquireFailure::PendingBytesLimit ||
                        failure == TqBufferAcquireFailure::SlotLimit);
                }
            });
        }
        for (auto& thread : threads) {
            thread.join();
        }

        assert(successes.load(std::memory_order_relaxed) <= 1);
        assert(maxObservedPending.load(std::memory_order_relaxed) <= 1);
        assert(pool.PendingBytes() == 0);
        assert(pool.AcquireCount() == successes.load(std::memory_order_relaxed));
    }

    {
        constexpr size_t ThreadCount = 8;
        constexpr size_t Iterations = 256;
        TqRelayBufferPool pool(1, ThreadCount, ThreadCount);
        pool.Reserve(ThreadCount);
        std::atomic<bool> stopReader{false};
        std::atomic<size_t> acquired{0};
        std::thread reader([&]() {
            while (!stopReader.load(std::memory_order_relaxed)) {
                assert(pool.FreeCount() <= ThreadCount);
                assert(pool.PendingBytes() <= ThreadCount);
            }
        });
        std::vector<std::thread> threads;

        for (size_t i = 0; i < ThreadCount; ++i) {
            threads.emplace_back([&]() {
                for (size_t j = 0; j < Iterations; ++j) {
                    auto buffer = pool.AcquireWorker();
                    if (buffer) {
                        acquired.fetch_add(1, std::memory_order_relaxed);
                        buffer.reset();
                    }
                    assert(pool.PendingBytes() <= ThreadCount);
                }
            });
        }
        for (auto& thread : threads) {
            thread.join();
        }
        stopReader.store(true, std::memory_order_relaxed);
        reader.join();

        assert(acquired.load(std::memory_order_relaxed) == pool.AcquireCount());
        assert(pool.PendingBytes() == 0);
        assert(pool.FreeCount() == ThreadCount);
    }

    TqRelayBufferPool pool(64, 2, 128);
    assert(pool.PendingBytes() == 0);
    assert(pool.FreeCount() == 0);

    auto first = pool.AcquireWorker();
    auto second = pool.AcquireWorker();
    auto third = pool.AcquireWorker();
    assert(first);
    assert(second);
    assert(!third);
    assert(pool.PendingBytes() == 128);

    std::memset(first->Data(), 0xAB, first->Capacity());
    uint8_t* firstData = first->Data();
    TqBufferView view{firstData, 17, std::move(first)};
    assert(view.Len == 17);
    assert(view.Owner);

    assert(pool.PendingBytes() == 128);
    view.Owner.reset();
    assert(pool.PendingBytes() == 64);

    second.reset();
    assert(pool.PendingBytes() == 0);
    assert(pool.FreeCount() == 2);

    auto reused = pool.AcquireWorker();
    assert(reused);
    assert(pool.PendingBytes() == 64);
    reused.reset();

    return 0;
}
