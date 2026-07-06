#define TQ_UNIT_TESTING 1
#include "linux_relay_worker.h"

#include <cassert>
#include <cstdint>
#include <thread>
#include <utility>
#include <vector>

#include <sys/socket.h>
#include <unistd.h>

bool TqLinuxRelayWorkerEnqueueCancelledRegisterForTest(
    TqLinuxRelayWorker& worker,
    const TqLinuxRelayRegistration& registration) {
    auto command = std::make_shared<TqLinuxRelayWorker::RegisterRelayCommand>();
    command->Registration = registration;
    {
        std::lock_guard<std::mutex> guard(command->Mutex);
        command->Cancelled = true;
    }

    TqLinuxRelayEvent event{};
    event.Type = TqLinuxRelayEventType::RegisterRelay;
    event.Control = command.get();
    event.ControlOwner = command;
    return worker.EnqueueForTest(std::move(event));
}

int main() {
    {
        TqLinuxRelayWorker worker(TqLinuxRelayWorkerConfig{});
        const uint64_t wake = worker.EncodeEpollWakeForTest();
        const uint64_t relayThirty = worker.EncodeEpollRelayForTest(30);
        if (!worker.IsEpollWakeForTest(wake)) return 131;
        if (worker.IsEpollWakeForTest(relayThirty)) return 132;

        uint64_t decoded = 0;
        if (!worker.DecodeEpollRelayForTest(relayThirty, decoded)) return 133;
        if (decoded != 30) return 134;
        if (worker.DecodeEpollRelayForTest(wake, decoded)) return 135;
        if (worker.DecodeEpollRelayForTest(0, decoded)) return 136;
    }

    {
        TqLinuxRelayWorker worker(TqLinuxRelayWorkerConfig{});

        worker.SetNextRelayIdForTest(0);
        if (worker.AllocateRelayIdForTest() != 1) return 137;

        worker.SetNextRelayIdForTest(1ull << 63);
        if (worker.AllocateRelayIdForTest() != 1) return 138;

        worker.SetNextRelayIdForTest((1ull << 63) - 1);
        if (worker.AllocateRelayIdForTest() != ((1ull << 63) - 1)) return 139;
        if (worker.AllocateRelayIdForTest() != 1) return 140;
    }

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

    {
        TqLinuxRelayWorkerConfig config{};
        config.EventQueueCapacity = 1024;
        TqLinuxRelayWorker worker(config);
        if (!worker.Start()) return 121;

        const TqLinuxRelayWorkerSnapshot snapshot = worker.Snapshot();
        if (snapshot.ControlCommandWaitCount == 0) return 123;
        if (snapshot.ControlCommandWaitNanos == 0) return 124;
        if (snapshot.SnapshotCommandWaitCount == 0) return 125;
        if (snapshot.SnapshotCommandWaitNanos == 0) return 126;
        worker.Stop();
    }

    {
        TqLinuxRelayWorkerConfig config{};
        config.EventQueueCapacity = 1024;
        TqLinuxRelayWorker worker(config);
        if (!worker.StartForTest()) return 127;

        int sockets[2]{-1, -1};
        if (::socketpair(AF_UNIX, SOCK_STREAM, 0, sockets) != 0) return 128;

        TqLinuxRelayRegistration registration{};
        registration.TcpFd = sockets[0];
        if (!TqLinuxRelayWorkerEnqueueCancelledRegisterForTest(worker, registration)) return 129;
        if (worker.DrainForTest(1) != 1) return 130;

        const TqLinuxRelayWorkerSnapshot snapshot = worker.Snapshot();
        if (snapshot.ActiveRelays != 0) return 131;

        ::close(sockets[0]);
        ::close(sockets[1]);
        worker.Stop();
    }

    return 0;
}
