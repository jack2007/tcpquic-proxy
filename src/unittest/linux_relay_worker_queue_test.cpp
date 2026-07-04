#include "linux_relay_worker.h"

#include <cassert>
#include <cstdint>
#include <thread>
#include <utility>
#include <vector>

int main() {
    {
        TqLinuxRelayWorker worker(TqLinuxRelayWorkerConfig{});
        assert(worker.StartForTest());

        for (uint64_t i = 0; i < 1000; ++i) {
            TqLinuxRelayEvent event{};
            event.Type = TqLinuxRelayEventType::TestMarker;
            event.Value = i + 1;
            assert(worker.EnqueueForTest(std::move(event)));
        }

        assert(worker.DrainForTest(1000) == 1000);
        TqLinuxRelayWorkerSnapshot snapshot = worker.Snapshot();
        assert(snapshot.EventsProcessed == 1000);
        assert(snapshot.WakeupWrites >= 1);
        assert(snapshot.WakeupWrites < 1000);
        assert(snapshot.PendingEvents == 0);

        worker.Stop();
    }

    {
        TqLinuxRelayWorkerConfig config{};
        config.EventQueueCapacity = 4;
        TqLinuxRelayWorker worker(config);
        assert(worker.StartForTest());

        bool sawFull = false;
        for (uint64_t i = 0; i < config.EventQueueCapacity + 8; ++i) {
            TqLinuxRelayEvent event{};
            event.Type = TqLinuxRelayEventType::TestMarker;
            event.Value = i + 1;
            if (!worker.EnqueueForTest(std::move(event))) {
                sawFull = true;
                break;
            }
        }
        if (!sawFull) return 109;

        TqLinuxRelayWorkerSnapshot snapshot = worker.Snapshot();
        if (snapshot.PendingEvents == 0) return 108;
        if (snapshot.Errors != 1) return 107;
        if (snapshot.EventQueueFullErrors != 1) return 110;

        assert(worker.DrainForTest(snapshot.PendingEvents) == snapshot.PendingEvents);
        snapshot = worker.Snapshot();
        assert(snapshot.PendingEvents == 0);

        TqLinuxRelayEvent afterDrain{};
        afterDrain.Type = TqLinuxRelayEventType::TestMarker;
        afterDrain.Value = 100;
        assert(worker.EnqueueForTest(std::move(afterDrain)));

        worker.Stop();
    }

    {
        TqLinuxRelayWorkerConfig config{};
        config.EventQueueCapacity = 1024;
        config.TrackEventProducers = true;
        TqLinuxRelayWorker worker(config);
        if (!worker.StartForTest()) return 111;

        std::thread producerA([&worker]() {
            for (uint64_t i = 0; i < 128; ++i) {
                TqLinuxRelayEvent event{};
                event.Type = TqLinuxRelayEventType::TestMarker;
                event.Value = i;
                if (!worker.EnqueueForTest(std::move(event))) return;
            }
        });
        std::thread producerB([&worker]() {
            for (uint64_t i = 0; i < 128; ++i) {
                TqLinuxRelayEvent event{};
                event.Type = TqLinuxRelayEventType::TestMarker;
                event.Value = 1000 + i;
                if (!worker.EnqueueForTest(std::move(event))) return;
            }
        });
        producerA.join();
        producerB.join();

        TqLinuxRelayWorkerSnapshot snapshot = worker.Snapshot();
        if (snapshot.EventQueueCapacity != 1024) return 112;
        if (snapshot.PendingEvents != 256) return 113;
        if (snapshot.EventProducerThreadsObserved < 2) return 114;
        if (!snapshot.MultipleEventProducerThreadsObserved) return 115;

        if (worker.DrainForTest(256) != 256) return 116;
        snapshot = worker.Snapshot();
        if (snapshot.EventsProcessed != 256) return 117;
        if (snapshot.PendingEvents != 0) return 118;
        const uint64_t pushCasRetries = snapshot.EventQueuePushCasRetries;
        const uint64_t popCasRetries = snapshot.EventQueuePopCasRetries;
        (void)pushCasRetries;
        (void)popCasRetries;

        worker.Stop();
    }

    {
        TqLinuxRelayWorkerConfig config{};
        config.EventQueueCapacity = 3;
        TqLinuxRelayWorker worker(config);
        if (!worker.StartForTest()) return 119;
        TqLinuxRelayWorkerSnapshot snapshot = worker.Snapshot();
        if (snapshot.EventQueueCapacity != 4) return 120;
        worker.Stop();
    }

    return 0;
}
