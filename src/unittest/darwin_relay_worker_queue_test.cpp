#if defined(__APPLE__)

#include "darwin_relay_event_queue.h"
#include "darwin_relay_worker.h"

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <memory>
#include <pthread.h>
#include <utility>
#include <vector>

namespace {

void CheckImpl(bool condition, int line) {
    if (!condition) {
        std::fprintf(stderr, "check failed at line %d\n", line);
        std::fflush(stderr);
        std::exit(line % 125 + 1);
    }
}

#define CHECK(condition) CheckImpl((condition), __LINE__)

struct ProducerContext {
    TqDarwinRelayEventQueue* Queue{nullptr};
    uint64_t BaseValue{0};
    uint64_t Count{0};
    std::atomic<bool>* Ok{nullptr};
};

void* ProducerMain(void* arg) {
    auto* context = static_cast<ProducerContext*>(arg);
    for (uint64_t i = 0; i < context->Count; ++i) {
        TqDarwinRelayEvent event{};
        event.Type = TqDarwinRelayEventType::TestMarker;
        event.Value = context->BaseValue + i;
        if (!context->Queue->TryPush(std::move(event))) {
            context->Ok->store(false, std::memory_order_relaxed);
            return nullptr;
        }
    }
    return nullptr;
}

TqDarwinRelayEvent Marker(uint64_t value) {
    TqDarwinRelayEvent event{};
    event.Type = TqDarwinRelayEventType::TestMarker;
    event.Value = value;
    return event;
}

void CapacityNormalizesToPowerOfTwo() {
    CHECK(TqDarwinRelayEventQueue::NormalizeCapacityForTest(0) == 2);
    CHECK(TqDarwinRelayEventQueue::NormalizeCapacityForTest(1) == 2);
    CHECK(TqDarwinRelayEventQueue::NormalizeCapacityForTest(2) == 2);
    CHECK(TqDarwinRelayEventQueue::NormalizeCapacityForTest(3) == 4);
    CHECK(TqDarwinRelayEventQueue::NormalizeCapacityForTest(9) == 16);

    const size_t maxCapacity = TqDarwinRelayEventQueue::MaxCapacityForTest();
    CHECK((maxCapacity & (maxCapacity - 1)) == 0);
    CHECK(maxCapacity == (size_t{1} << 20));
    CHECK(TqDarwinRelayEventQueue::NormalizeCapacityForTest(maxCapacity - 1) == maxCapacity);
    CHECK(TqDarwinRelayEventQueue::NormalizeCapacityForTest(maxCapacity) == maxCapacity);
    CHECK(TqDarwinRelayEventQueue::NormalizeCapacityForTest(maxCapacity + 1) == maxCapacity);
    CHECK(TqDarwinRelayEventQueue::NormalizeCapacityForTest(std::numeric_limits<size_t>::max()) ==
           maxCapacity);

    TqDarwinRelayEventQueue queue(3);
    CHECK(queue.Capacity() == 4);
}

void InitialSizeIsZero() {
    TqDarwinRelayEventQueue queue(8);
    CHECK(queue.SizeApprox() == 0);
}

void PushPopPreservesOrder() {
    TqDarwinRelayEventQueue queue(4);
    CHECK(queue.TryPush(Marker(10)));
    CHECK(queue.TryPush(Marker(20)));
    CHECK(queue.TryPush(Marker(30)));

    TqDarwinRelayEvent event{};
    CHECK(queue.TryPop(event));
    CHECK(event.Type == TqDarwinRelayEventType::TestMarker);
    CHECK(event.Value == 10);
    CHECK(queue.TryPop(event));
    CHECK(event.Type == TqDarwinRelayEventType::TestMarker);
    CHECK(event.Value == 20);
    CHECK(queue.TryPop(event));
    CHECK(event.Type == TqDarwinRelayEventType::TestMarker);
    CHECK(event.Value == 30);
}

void FullQueueRejectsAdditionalEvents() {
    TqDarwinRelayEventQueue queue(2);
    CHECK(queue.TryPush(Marker(1)));
    CHECK(queue.TryPush(Marker(2)));
    CHECK(!queue.TryPush(Marker(3)));
    CHECK(queue.SizeApprox() == 2);
}

void EmptyQueueRejectsPop() {
    TqDarwinRelayEventQueue queue(2);
    TqDarwinRelayEvent event{};
    CHECK(!queue.TryPop(event));
}

void MultiProducerPushSingleConsumerPop() {
    constexpr uint64_t eventsPerProducer = 128;
    TqDarwinRelayEventQueue queue(512);
    std::atomic<bool> producerOk{true};
    ProducerContext producerAContext{&queue, 0, eventsPerProducer, &producerOk};
    ProducerContext producerBContext{&queue, 1000, eventsPerProducer, &producerOk};
    pthread_t producerA{};
    pthread_t producerB{};

    CHECK(pthread_create(&producerA, nullptr, ProducerMain, &producerAContext) == 0);
    CHECK(pthread_create(&producerB, nullptr, ProducerMain, &producerBContext) == 0);
    CHECK(pthread_join(producerA, nullptr) == 0);
    CHECK(pthread_join(producerB, nullptr) == 0);
    CHECK(producerOk.load(std::memory_order_relaxed));
    CHECK(queue.SizeApprox() == eventsPerProducer * 2);

    std::vector<uint64_t> values;
    values.reserve(eventsPerProducer * 2);
    TqDarwinRelayEvent event{};
    while (queue.TryPop(event)) {
        CHECK(event.Type == TqDarwinRelayEventType::TestMarker);
        values.push_back(event.Value);
        event = TqDarwinRelayEvent{};
    }

    CHECK(values.size() == eventsPerProducer * 2);
    std::sort(values.begin(), values.end());
    for (uint64_t i = 0; i < eventsPerProducer; ++i) {
        CHECK(values[i] == i);
        CHECK(values[eventsPerProducer + i] == 1000 + i);
    }
    CHECK(queue.SizeApprox() == 0);
}

void EventMovePreservesOwnedMembers() {
    TqDarwinRelayEventQueue queue(2);
    auto receive = std::make_shared<TqDarwinPendingQuicReceive>();
    receive->RelayId = 42;
    receive->TotalLength = 7;
    receive->Slices.push_back(TqDarwinQuicReceiveSlice{nullptr, 7});

    TqDarwinRelayEvent event{};
    event.Type = TqDarwinRelayEventType::QuicReceiveView;
    event.Value = 123;
    event.Buffers.emplace_back();
    event.Length = 7;
    event.ReceiveView = receive;

    CHECK(queue.TryPush(std::move(event)));
    CHECK(receive.use_count() == 2);

    TqDarwinRelayEvent popped{};
    CHECK(queue.TryPop(popped));
    CHECK(popped.Type == TqDarwinRelayEventType::QuicReceiveView);
    CHECK(popped.Value == 123);
    CHECK(popped.Length == 7);
    CHECK(popped.Buffers.size() == 1);
    CHECK(popped.ReceiveView == receive);
    CHECK(popped.ReceiveView->RelayId == 42);
    CHECK(popped.ReceiveView->Slices.size() == 1);
    CHECK(receive.use_count() == 2);
}

void PendingReceiveStreamOwnerIsIndependentOfRelayMap() {
    // Task 2: pending receive retains strong ownership (StreamOwner/BindingOwner)
    // captured at build time; the queue must keep that alive without a live
    // relay-map entry.
    auto bindingOwner = std::make_shared<int>(1);
    std::weak_ptr<int> weakBinding = bindingOwner;
    auto receive = std::make_shared<TqDarwinPendingQuicReceive>();
    receive->RelayId = 7;
    receive->BindingOwner = bindingOwner;
    receive->TotalLength = 3;

    TqDarwinRelayEventQueue queue(2);
    TqDarwinRelayEvent event{};
    event.Type = TqDarwinRelayEventType::QuicReceiveView;
    event.ReceiveView = receive;
    CHECK(queue.TryPush(std::move(event)));

    bindingOwner.reset();
    CHECK(!weakBinding.expired());

    TqDarwinRelayEvent popped{};
    CHECK(queue.TryPop(popped));
    CHECK(popped.ReceiveView != nullptr);
    CHECK(popped.ReceiveView->BindingOwner != nullptr);
    CHECK(popped.ReceiveView->RelayId == 7);
    popped.ReceiveView.reset();
    receive.reset();
    CHECK(weakBinding.expired());
}

void ActiveShutdownEventMovePreservesReasonAndOwner() {
    TqDarwinRelayEventQueue queue(2);
    auto relayOwner = std::make_shared<int>(99);

    TqDarwinRelayEvent event{};
    event.Type = TqDarwinRelayEventType::QuicActiveShutdown;
    event.RelayId = 11;
    event.RelayOwner = relayOwner;
    event.Value = static_cast<uint64_t>(TqDarwinActiveShutdownReason::ReceiveAllocationFailed);

    CHECK(queue.TryPush(std::move(event)));
    CHECK(relayOwner.use_count() == 2);

    TqDarwinRelayEvent popped{};
    CHECK(queue.TryPop(popped));
    CHECK(popped.Type == TqDarwinRelayEventType::QuicActiveShutdown);
    CHECK(popped.RelayId == 11);
    CHECK(popped.RelayOwner == relayOwner);
    CHECK(
        popped.Value ==
        static_cast<uint64_t>(TqDarwinActiveShutdownReason::ReceiveAllocationFailed));
    CHECK(relayOwner.use_count() == 2);
}

void DrainWakeProcessesPastBudgetAndShutdown() {
    TqDarwinRelayWorkerConfig config{};
    config.EventBudget = 2;
    config.EventQueueCapacity = 8;
    TqDarwinRelayWorker worker(config);
    CHECK(worker.StartForTest());

    CHECK(worker.EnqueueForTest(Marker(1)));
    CHECK(worker.EnqueueForTest(Marker(2)));
    CHECK(worker.EnqueueForTest(Marker(3)));
    TqDarwinRelayEvent stopRelay{};
    stopRelay.Type = TqDarwinRelayEventType::StopRelay;
    CHECK(worker.EnqueueForTest(std::move(stopRelay)));
    TqDarwinRelayEvent shutdown{};
    shutdown.Type = TqDarwinRelayEventType::Shutdown;
    CHECK(worker.EnqueueForTest(std::move(shutdown)));

    CHECK(worker.DrainWakeForTest() == 5);
    TqDarwinRelayWorkerSnapshot snapshot = worker.Snapshot();
    CHECK(snapshot.EventsProcessed == 5);
    CHECK(snapshot.PendingEvents == 0);
    CHECK(!worker.RunningForTest());
}

} // namespace

int main() {
    CapacityNormalizesToPowerOfTwo();
    InitialSizeIsZero();
    PushPopPreservesOrder();
    FullQueueRejectsAdditionalEvents();
    EmptyQueueRejectsPop();
    MultiProducerPushSingleConsumerPop();
    EventMovePreservesOwnedMembers();
    PendingReceiveStreamOwnerIsIndependentOfRelayMap();
    ActiveShutdownEventMovePreservesReasonAndOwner();
    DrainWakeProcessesPastBudgetAndShutdown();
    return 0;
}

#endif
