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

        for (uint64_t i = 0; i < config.EventQueueCapacity; ++i) {
            TqLinuxRelayEvent event{};
            event.Type = TqLinuxRelayEventType::TestMarker;
            event.Value = i + 1;
            assert(worker.EnqueueForTest(std::move(event)));
        }

        TqLinuxRelayEvent event{};
        event.Type = TqLinuxRelayEventType::TestMarker;
        event.Value = 99;
        assert(!worker.EnqueueForTest(std::move(event)));

        TqLinuxRelayWorkerSnapshot snapshot = worker.Snapshot();
        assert(snapshot.PendingEvents == config.EventQueueCapacity);
        assert(snapshot.Errors == 1);

        assert(worker.DrainForTest(config.EventQueueCapacity) == config.EventQueueCapacity);
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
        assert(worker.StartForTest());

        std::thread producerA([&worker]() {
            for (uint64_t i = 0; i < 128; ++i) {
                TqLinuxRelayEvent event{};
                event.Type = TqLinuxRelayEventType::TestMarker;
                event.Value = i;
                assert(worker.EnqueueForTest(std::move(event)));
            }
        });
        std::thread producerB([&worker]() {
            for (uint64_t i = 0; i < 128; ++i) {
                TqLinuxRelayEvent event{};
                event.Type = TqLinuxRelayEventType::TestMarker;
                event.Value = 1000 + i;
                assert(worker.EnqueueForTest(std::move(event)));
            }
        });
        producerA.join();
        producerB.join();

        TqLinuxRelayWorkerSnapshot snapshot = worker.Snapshot();
        assert(snapshot.PendingEvents == 256);
        assert(snapshot.EventProducerThreadsObserved >= 2);
        assert(snapshot.MultipleEventProducerThreadsObserved);

        assert(worker.DrainForTest(256) == 256);
        snapshot = worker.Snapshot();
        assert(snapshot.EventsProcessed == 256);
        assert(snapshot.PendingEvents == 0);

        worker.Stop();
    }

    return 0;
}

