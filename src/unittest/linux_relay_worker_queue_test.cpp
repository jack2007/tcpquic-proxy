#include "linux_relay_worker.h"

#include <cassert>
#include <cstdint>
#include <vector>

int main() {
    TqLinuxRelayWorker worker(TqLinuxRelayWorkerConfig{});
    assert(worker.StartForTest());

    for (uint64_t i = 0; i < 1000; ++i) {
        TqLinuxRelayEvent event{};
        event.Type = TqLinuxRelayEventType::TestMarker;
        event.Value = i + 1;
        worker.EnqueueForTest(event);
    }

    assert(worker.DrainForTest(1000) == 1000);
    TqLinuxRelayWorkerSnapshot snapshot = worker.Snapshot();
    assert(snapshot.EventsProcessed == 1000);
    assert(snapshot.WakeupWrites >= 1);
    assert(snapshot.WakeupWrites < 1000);
    assert(snapshot.PendingEvents == 0);

    worker.Stop();
    return 0;
}
