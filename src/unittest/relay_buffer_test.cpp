#include "relay_buffer.h"
#include <cassert>

int main() {
    TqRelayBufferBudget relay{};
    relay.MaxPendingBufferBytes = 128 * 1024;

    TqBufferAcquireFailure failure = TqBufferAcquireFailure::None;
    auto first = TqAllocateRelayBuffer(&relay, 64 * 1024, &failure);
    assert(first);
    assert(failure == TqBufferAcquireFailure::None);
    assert(relay.PendingBufferBytes.load() == 64 * 1024);
    assert(relay.AllocateCount.load() == 1);

    auto second = TqAllocateRelayBuffer(&relay, 64 * 1024, &failure);
    assert(second);
    assert(relay.PendingBufferBytes.load() == 128 * 1024);

    auto denied = TqAllocateRelayBuffer(&relay, 64 * 1024, &failure);
    assert(!denied);
    assert(failure == TqBufferAcquireFailure::PendingBytesLimit);

    first.reset();
    assert(relay.PendingBufferBytes.load() == 64 * 1024);

    second.reset();
    assert(relay.PendingBufferBytes.load() == 0);
    return 0;
}
