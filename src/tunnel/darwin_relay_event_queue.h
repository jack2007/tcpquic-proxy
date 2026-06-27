#pragma once

#include "relay_buffer.h"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <utility>
#include <vector>

struct MsQuicStream;

struct TqDarwinQuicReceiveSlice {
    const uint8_t* Data{nullptr};
    uint32_t Length{0};
};

struct TqDarwinPendingQuicReceive {
    MsQuicStream* Stream{nullptr};
    uint64_t RelayId{0};
    std::shared_ptr<void> BindingOwner;
    std::vector<TqDarwinQuicReceiveSlice> Slices;
    size_t SliceIndex{0};
    size_t SliceOffset{0};
    uint64_t TotalLength{0};
    uint64_t CompletedLength{0};
    uint64_t PendingCompleteBytes{0};
    bool Fin{false};
};

enum class TqDarwinRelayEventType {
    TestMarker,
    TcpWritable,
    QuicReceive,
    QuicReceiveView,
    QuicSendComplete,
    QuicIdealSendBuffer,
    Shutdown,
    StopRelay,
};

struct TqDarwinRelayEvent {
    TqDarwinRelayEventType Type{TqDarwinRelayEventType::Shutdown};
    uint64_t Value{0};
    uint64_t RelayId{0};
    void* Relay{nullptr};
    TqBufferRef Buffer;
    std::vector<TqBufferRef> Buffers;
    size_t Length{0};
    size_t TotalLength{0};
    bool Fin{false};
    std::shared_ptr<TqDarwinPendingQuicReceive> ReceiveView;
};

class TqDarwinRelayEventQueue final {
public:
    explicit TqDarwinRelayEventQueue(size_t capacity)
        : CapacityValue(NormalizeCapacity(capacity)),
          Mask(CapacityValue - 1),
          Slots(new Cell[CapacityValue]) {
        for (size_t i = 0; i < CapacityValue; ++i) {
            Slots[i].Sequence.store(i, std::memory_order_relaxed);
        }
    }

    TqDarwinRelayEventQueue(const TqDarwinRelayEventQueue&) = delete;
    TqDarwinRelayEventQueue& operator=(const TqDarwinRelayEventQueue&) = delete;

    bool TryPush(TqDarwinRelayEvent&& event) {
        Cell* cell = nullptr;
        size_t position = EnqueuePos.load(std::memory_order_relaxed);
        for (;;) {
            cell = &Slots[position & Mask];
            const size_t sequence = cell->Sequence.load(std::memory_order_acquire);
            const intptr_t diff = static_cast<intptr_t>(sequence) - static_cast<intptr_t>(position);
            if (diff == 0) {
                if (EnqueuePos.compare_exchange_weak(
                        position,
                        position + 1,
                        std::memory_order_relaxed,
                        std::memory_order_relaxed)) {
                    break;
                }
            } else if (diff < 0) {
                return false;
            } else {
                position = EnqueuePos.load(std::memory_order_relaxed);
            }
        }
        cell->Event = std::move(event);
        cell->Sequence.store(position + 1, std::memory_order_release);
        return true;
    }

    bool TryPop(TqDarwinRelayEvent& event) {
        Cell* cell = nullptr;
        size_t position = DequeuePos.load(std::memory_order_relaxed);
        for (;;) {
            cell = &Slots[position & Mask];
            const size_t sequence = cell->Sequence.load(std::memory_order_acquire);
            const intptr_t diff = static_cast<intptr_t>(sequence) -
                static_cast<intptr_t>(position + 1);
            if (diff == 0) {
                if (DequeuePos.compare_exchange_weak(
                        position,
                        position + 1,
                        std::memory_order_relaxed,
                        std::memory_order_relaxed)) {
                    break;
                }
            } else if (diff < 0) {
                return false;
            } else {
                position = DequeuePos.load(std::memory_order_relaxed);
            }
        }
        event = std::move(cell->Event);
        cell->Event = TqDarwinRelayEvent{};
        cell->Sequence.store(position + CapacityValue, std::memory_order_release);
        return true;
    }

    size_t SizeApprox() const {
        const size_t enqueued = EnqueuePos.load(std::memory_order_acquire);
        const size_t dequeued = DequeuePos.load(std::memory_order_acquire);
        return enqueued >= dequeued ? enqueued - dequeued : 0;
    }

    size_t Capacity() const {
        return CapacityValue;
    }

#if defined(TCPQUIC_TESTING)
    static constexpr size_t MaxCapacityForTest() {
        return MaxCapacity();
    }

    static constexpr size_t NormalizeCapacityForTest(size_t capacity) {
        return NormalizeCapacity(capacity);
    }
#endif

private:
    struct Cell {
        std::atomic<size_t> Sequence{0};
        TqDarwinRelayEvent Event;
    };

    static constexpr size_t MaxCapacity() {
        return size_t{1} << 20;
    }

    static constexpr size_t NormalizeCapacity(size_t capacity) {
        if (capacity <= 2) {
            return 2;
        }
        const size_t maxCapacity = MaxCapacity();
        if (capacity >= maxCapacity) {
            return maxCapacity;
        }
        size_t normalized = 2;
        while (normalized < capacity && normalized <= maxCapacity / 2) {
            normalized <<= 1;
        }
        return normalized < capacity ? maxCapacity : normalized;
    }

    const size_t CapacityValue;
    const size_t Mask;
    std::unique_ptr<Cell[]> Slots;
    alignas(64) std::atomic<size_t> EnqueuePos{0};
    alignas(64) std::atomic<size_t> DequeuePos{0};
};
