#include "linux_relay_buffer_pool.h"

#include <cassert>
#include <cstring>
#include <memory>
#include <type_traits>

int main() {
    static_assert(!std::is_same<TqBufferRef, std::shared_ptr<TqRelayBufferSlot>>::value,
        "TqBufferRef must not allocate shared_ptr control blocks");
    static_assert(!std::is_copy_constructible<TqBufferRef>::value,
        "TqBufferRef is expected to be move-only");
    static_assert(std::is_move_constructible<TqBufferRef>::value,
        "TqBufferRef must support ownership transfer");

    {
        TqLinuxRelayBufferPool pool(64, 4, 512);
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
        TqLinuxRelayBufferPool pool(64, 4, 32);
        TqBufferAcquireFailure reason = TqBufferAcquireFailure::None;
        auto worker = pool.AcquireWorker(&reason);
        assert(!worker);
        assert(reason == TqBufferAcquireFailure::PendingBytesLimit);
    }

    {
        TqLinuxRelayBufferPool pool(64, 2, 512);
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

    TqLinuxRelayBufferPool pool(64, 2, 128);
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
