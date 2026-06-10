#include "linux_relay_buffer_pool.h"

#include <cassert>
#include <cstring>

int main() {
    TqLinuxRelayBufferPool pool(64, 2, 128);
    assert(pool.PendingBytes() == 0);
    assert(pool.FreeCount() == 0);

    auto first = pool.Acquire();
    auto second = pool.Acquire();
    auto third = pool.Acquire();
    assert(first);
    assert(second);
    assert(!third);
    assert(pool.PendingBytes() == 128);

    std::memset(first->Data(), 0xAB, first->Capacity());
    TqBufferView view{first->Data(), 17, first};
    assert(view.Len == 17);
    assert(view.Owner);

    first.reset();
    assert(pool.PendingBytes() == 128);
    view.Owner.reset();
    assert(pool.PendingBytes() == 64);

    second.reset();
    assert(pool.PendingBytes() == 0);
    assert(pool.FreeCount() == 2);

    auto reused = pool.Acquire();
    assert(reused);
    assert(pool.PendingBytes() == 64);
    reused.reset();

    return 0;
}
