#include "relay_buffer_pool.h"

#include <algorithm>
#include <new>
#include <utility>

TqRelayBufferSlot::TqRelayBufferSlot(size_t capacity)
    : Storage(capacity) {}

uint8_t* TqRelayBufferSlot::Data() {
    return Storage.data();
}

const uint8_t* TqRelayBufferSlot::Data() const {
    return Storage.data();
}

size_t TqRelayBufferSlot::Capacity() const {
    return Storage.size();
}

void TqRelayBufferSlot::SetLength(size_t length) {
    UsedLength = std::min(length, Storage.size());
}

size_t TqRelayBufferSlot::Length() const {
    return UsedLength;
}

TqBufferHandle::TqBufferHandle(
    TqRelayBufferPool* pool,
    TqRelayBufferSlot* slot,
    TqBufferDomain domain) noexcept
    : Pool(pool), Slot(slot), Domain(domain) {}

TqBufferHandle::TqBufferHandle(TqBufferHandle&& other) noexcept
    : Pool(std::exchange(other.Pool, nullptr)),
      Slot(std::exchange(other.Slot, nullptr)),
      Domain(other.Domain) {}

TqBufferHandle& TqBufferHandle::operator=(TqBufferHandle&& other) noexcept {
    if (this != &other) {
        reset();
        Pool = std::exchange(other.Pool, nullptr);
        Slot = std::exchange(other.Slot, nullptr);
        Domain = other.Domain;
    }
    return *this;
}

TqBufferHandle::~TqBufferHandle() {
    reset();
}

TqRelayBufferSlot* TqBufferHandle::get() const {
    return Slot;
}

TqRelayBufferSlot* TqBufferHandle::operator->() const {
    return Slot;
}

TqRelayBufferSlot& TqBufferHandle::operator*() const {
    return *Slot;
}

TqBufferHandle::operator bool() const {
    return Slot != nullptr;
}

void TqBufferHandle::reset() {
    if (Pool == nullptr || Slot == nullptr) {
        Pool = nullptr;
        Slot = nullptr;
        return;
    }
    auto* pool = Pool;
    auto* slot = Slot;
    Pool = nullptr;
    Slot = nullptr;
    pool->ReleaseWorker(slot);
}

void TqBufferHandle::abandon() {
    reset();
}

TqBufferView::TqBufferView(uint8_t* data, size_t len, TqBufferRef owner) noexcept
    : Data(data), Len(len), Owner(std::move(owner)) {}

TqRelayBufferPool::TqRelayBufferPool(
    size_t chunkSize,
    size_t maxBuffers,
    uint64_t maxPendingBytes)
    : ChunkBytes(chunkSize),
      MaxBuffers(maxBuffers),
      MaxBytes(maxPendingBytes),
      WorkerMaxSlots(maxBuffers) {}

TqRelayBufferPool::~TqRelayBufferPool() {
    std::lock_guard<std::mutex> workerGuard(WorkerLock);
    for (auto* buffer : WorkerFree) {
        delete buffer;
    }
    WorkerFree.clear();
}

void TqRelayBufferPool::Reserve(size_t slotCount) {
    const size_t capped = std::min(slotCount, MaxBuffers);
    std::lock_guard<std::mutex> workerGuard(WorkerLock);
    WorkerMaxSlots = capped;
    while (WorkerAllocated < WorkerMaxSlots) {
        auto* buffer = new (std::nothrow) TqRelayBufferSlot(ChunkBytes);
        if (buffer == nullptr) {
            return;
        }
        WorkerFree.push_back(buffer);
        ++WorkerAllocated;
    }
}

TqBufferRef TqRelayBufferPool::Acquire(
    TqBufferDomain domain,
    TqBufferAcquireFailure* failure) {
    (void)domain;
    return AcquireWorker(failure);
}

TqBufferRef TqRelayBufferPool::AcquireWorker(TqBufferAcquireFailure* failure) {
    if (failure != nullptr) {
        *failure = TqBufferAcquireFailure::None;
    }
    if (!ReservePending(failure)) {
        return {};
    }

    TqRelayBufferSlot* buffer = nullptr;
    {
        std::lock_guard<std::mutex> workerGuard(WorkerLock);
        if (!WorkerFree.empty()) {
            buffer = WorkerFree.back();
            WorkerFree.pop_back();
        } else {
            if (WorkerAllocated >= WorkerMaxSlots) {
                ReleasePending();
                if (failure != nullptr) {
                    *failure = TqBufferAcquireFailure::SlotLimit;
                }
                return {};
            }
            buffer = new (std::nothrow) TqRelayBufferSlot(ChunkBytes);
            if (buffer == nullptr) {
                ReleasePending();
                if (failure != nullptr) {
                    *failure = TqBufferAcquireFailure::AllocationFailure;
                }
                return {};
            }
            ++WorkerAllocated;
        }
    }

    buffer->UsedLength = 0;
    WorkerPending.fetch_add(ChunkBytes, std::memory_order_relaxed);
    Acquires.fetch_add(1, std::memory_order_relaxed);
    return TqBufferRef(this, buffer, TqBufferDomain::Worker);
}

TqBufferRef TqRelayBufferPool::Acquire() {
    return AcquireWorker();
}

size_t TqRelayBufferPool::ChunkSize() const {
    return ChunkBytes;
}

size_t TqRelayBufferPool::FreeCount() const {
    std::lock_guard<std::mutex> workerGuard(WorkerLock);
    return WorkerFree.size();
}

size_t TqRelayBufferPool::AllocatedCount() const {
    std::lock_guard<std::mutex> workerGuard(WorkerLock);
    return WorkerAllocated;
}

uint64_t TqRelayBufferPool::PendingBytes() const {
    return PendingReserved.load(std::memory_order_relaxed);
}

uint64_t TqRelayBufferPool::AcquireCount() const {
    return Acquires.load(std::memory_order_relaxed);
}

uint64_t TqRelayBufferPool::MaxPendingBytes() const {
    return MaxBytes;
}

bool TqRelayBufferPool::ReservePending(TqBufferAcquireFailure* failure) {
    uint64_t reserved = PendingReserved.load(std::memory_order_relaxed);
    for (;;) {
        if (reserved > MaxBytes || ChunkBytes > MaxBytes - reserved) {
            if (failure != nullptr) {
                *failure = TqBufferAcquireFailure::PendingBytesLimit;
            }
            return false;
        }
        if (PendingReserved.compare_exchange_weak(
                reserved,
                reserved + ChunkBytes,
                std::memory_order_relaxed,
                std::memory_order_relaxed)) {
            return true;
        }
    }
}

void TqRelayBufferPool::ReleasePending() {
    uint64_t reserved = PendingReserved.load(std::memory_order_relaxed);
    for (;;) {
        const uint64_t next = reserved >= ChunkBytes ? reserved - ChunkBytes : 0;
        if (PendingReserved.compare_exchange_weak(
                reserved,
                next,
                std::memory_order_relaxed,
                std::memory_order_relaxed)) {
            return;
        }
    }
}

void TqRelayBufferPool::ReleaseWorker(TqRelayBufferSlot* buffer) {
    if (buffer == nullptr) {
        return;
    }
    buffer->UsedLength = 0;
    const uint64_t workerBefore = WorkerPending.load(std::memory_order_relaxed);
    if (workerBefore >= ChunkBytes) {
        WorkerPending.fetch_sub(ChunkBytes, std::memory_order_relaxed);
    } else {
        WorkerPending.store(0, std::memory_order_relaxed);
    }
    ReleasePending();
    std::lock_guard<std::mutex> workerGuard(WorkerLock);
    WorkerFree.push_back(buffer);
}
