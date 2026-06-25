#pragma once

#include "relay_buffer.h"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

struct TqWindowsPendingQuicReceive;
struct TqWindowsQuicSendOperation;

enum class TqWindowsRelayTaskType {
    TestMarker,
    TcpWritable,
    QuicReceive,
    QuicReceiveView,
    QuicSendComplete,
    QuicIdealSendBuffer,
    QuicPeerSendAborted,
    QuicPeerReceiveAborted,
    QuicShutdownComplete,
    Shutdown,
};

struct TqWindowsRelayTask {
    TqWindowsRelayTaskType Type{TqWindowsRelayTaskType::Shutdown};
    uint64_t RelayId{0};
    uint64_t Value{0};
    void* Relay{nullptr};
    TqBufferRef Buffer;
    std::vector<TqBufferRef> Buffers;
    size_t Length{0};
    size_t TotalLength{0};
    bool Fin{false};
    std::shared_ptr<TqWindowsPendingQuicReceive> ReceiveView;
};

class TqWindowsRelayEventQueue final {
public:
    explicit TqWindowsRelayEventQueue(size_t capacity)
        : CapacityValue(NormalizeCapacity(capacity)),
          Mask(CapacityValue - 1),
          Slots(new Cell[CapacityValue]) {
        for (size_t i = 0; i < CapacityValue; ++i) {
            Slots[i].Sequence.store(i, std::memory_order_relaxed);
        }
    }

    TqWindowsRelayEventQueue(const TqWindowsRelayEventQueue&) = delete;
    TqWindowsRelayEventQueue& operator=(const TqWindowsRelayEventQueue&) = delete;

    bool TryPush(TqWindowsRelayTask&& event) {
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

    bool TryPop(TqWindowsRelayTask& event) {
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
        cell->Event = TqWindowsRelayTask{};
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

private:
    struct Cell {
        std::atomic<size_t> Sequence{0};
        TqWindowsRelayTask Event;
    };

    static size_t NormalizeCapacity(size_t capacity) {
        size_t normalized = 2;
        while (normalized < capacity) {
            normalized <<= 1;
        }
        return normalized;
    }

    const size_t CapacityValue;
    const size_t Mask;
    std::unique_ptr<Cell[]> Slots;
#if defined(_MSC_VER)
#pragma warning(push)
#pragma warning(disable : 4324)
#endif
    alignas(64) std::atomic<size_t> EnqueuePos{0};
    alignas(64) std::atomic<size_t> DequeuePos{0};
#if defined(_MSC_VER)
#pragma warning(pop)
#endif
};
