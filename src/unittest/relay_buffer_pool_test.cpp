#include "relay_buffer_pool.h"

#include <atomic>
#include <cassert>
#include <cstring>
#include <memory>
#include <mutex>
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
        TqRelayBufferPool pool(4096, 2, 8192);
        pool.Reserve(1, 1);

        TqBufferAcquireFailure failure = TqBufferAcquireFailure::None;
        auto worker = pool.Acquire(TqBufferDomain::Worker, &failure);
        assert(worker);
        assert(failure == TqBufferAcquireFailure::None);
        assert(worker->Capacity() == 4096);

        auto callback = pool.Acquire(TqBufferDomain::Ingress, &failure);
        assert(callback);
        assert(failure == TqBufferAcquireFailure::None);
        assert(pool.PendingBytes() == 8192);

        auto denied = pool.Acquire(TqBufferDomain::Worker, &failure);
        assert(!denied);
        assert(failure == TqBufferAcquireFailure::PendingBytesLimit ||
            failure == TqBufferAcquireFailure::SlotLimit);
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
        pool.Reserve(2, 0);
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
        TqRelayBufferPool pool(64, 4, 512);
        pool.Reserve(4);

        auto ingress = pool.AcquireIngress();
        assert(ingress);
        std::memset(ingress->Data(), 0xCD, ingress->Capacity());
        ingress->SetLength(17);
        assert(pool.PendingBytes() == 64);

        auto worker = pool.TransferToWorker(std::move(ingress));
        assert(worker);
        assert(!ingress);
        assert(pool.PendingBytes() == 64);
        assert(worker->Length() == 17);
        assert(worker->Data()[0] == 0xCD);

        worker.reset();
        assert(pool.PendingBytes() == 0);
    }

    {
        TqRelayBufferPool ownerPool(64, 4, 512);
        TqRelayBufferPool otherPool(64, 4, 512);
        auto worker = ownerPool.AcquireWorker();
        auto sameWorker = ownerPool.TransferToWorker(std::move(worker));
        assert(sameWorker);
        assert(!worker);
        assert(ownerPool.PendingBytes() == 64);
        sameWorker.reset();
        assert(ownerPool.PendingBytes() == 0);

        auto ingress = ownerPool.AcquireIngress();
        auto rejectedByOtherPool = otherPool.TransferToWorker(std::move(ingress));
        assert(rejectedByOtherPool);
        assert(!ingress);
        assert(ownerPool.PendingBytes() == 64);
        assert(otherPool.PendingBytes() == 0);
        rejectedByOtherPool.reset();
        assert(ownerPool.PendingBytes() == 0);
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
        pool.Reserve(0, ThreadCount);
        std::atomic<size_t> successes{0};
        std::atomic<size_t> maxObservedPending{0};
        std::mutex heldLock;
        std::vector<TqBufferRef> held;
        std::vector<std::thread> threads;

        for (size_t i = 0; i < ThreadCount; ++i) {
            threads.emplace_back([&]() {
                TqBufferAcquireFailure failure = TqBufferAcquireFailure::None;
                auto buffer = pool.AcquireIngress(&failure);
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
                    std::lock_guard<std::mutex> guard(heldLock);
                    held.push_back(std::move(buffer));
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
        assert(pool.PendingBytes() <= 1);
        assert(pool.AcquireCount() == successes.load(std::memory_order_relaxed));
        held.clear();
        assert(pool.PendingBytes() == 0);
    }

    {
        constexpr size_t ThreadCount = 8;
        constexpr size_t Iterations = 256;
        TqRelayBufferPool pool(1, ThreadCount, ThreadCount);
        pool.Reserve(ThreadCount, 0);
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
