#include "linux_relay_buffer_pool.h"

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
    TqLinuxRelayBufferPool* pool,
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
    const TqBufferDomain domain = Domain;
    Pool = nullptr;
    Slot = nullptr;
    if (domain == TqBufferDomain::Worker) {
        pool->ReleaseWorker(slot);
    } else {
        pool->ReleaseIngress(slot);
    }
}

TqBufferView::TqBufferView(uint8_t* data, size_t len, TqBufferRef owner) noexcept
    : Data(data), Len(len), Owner(std::move(owner)) {}

TqLinuxRelayBufferPool::TqLinuxRelayBufferPool(
    size_t chunkSize,
    size_t maxBuffers,
    uint64_t maxPendingBytes)
    : ChunkBytes(chunkSize),
      MaxBuffers(maxBuffers),
      MaxBytes(maxPendingBytes),
      WorkerMaxSlots(maxBuffers) {}

TqLinuxRelayBufferPool::~TqLinuxRelayBufferPool() {
    for (auto* buffer : WorkerFree) {
        delete buffer;
    }
    WorkerFree.clear();
    for (auto* buffer : IngressFree) {
        delete buffer;
    }
    IngressFree.clear();
}

void TqLinuxRelayBufferPool::Reserve(size_t slotCount) {
    const size_t capped = std::min(slotCount, MaxBuffers);
    IngressMaxSlots = capped / 2;
    WorkerMaxSlots = capped - IngressMaxSlots;
    if (WorkerMaxSlots == 0 && capped > 0) {
        WorkerMaxSlots = 1;
    }

    while (WorkerAllocated < WorkerMaxSlots) {
        auto* buffer = new (std::nothrow) TqRelayBufferSlot(ChunkBytes);
        if (buffer == nullptr) {
            return;
        }
        WorkerFree.push_back(buffer);
        ++WorkerAllocated;
    }
    while (IngressAllocated < IngressMaxSlots) {
        auto* buffer = new (std::nothrow) TqRelayBufferSlot(ChunkBytes);
        if (buffer == nullptr) {
            return;
        }
        IngressFree.push_back(buffer);
        ++IngressAllocated;
    }
}

TqBufferRef TqLinuxRelayBufferPool::AcquireWorker() {
    if (PendingBytes() + ChunkBytes > MaxBytes) {
        return {};
    }

    TqRelayBufferSlot* buffer = nullptr;
    if (!WorkerFree.empty()) {
        buffer = WorkerFree.back();
        WorkerFree.pop_back();
    } else {
        if (WorkerAllocated >= WorkerMaxSlots) {
            return {};
        }
        buffer = new (std::nothrow) TqRelayBufferSlot(ChunkBytes);
        if (buffer == nullptr) {
            return {};
        }
        ++WorkerAllocated;
    }

    buffer->UsedLength = 0;
    WorkerPending += ChunkBytes;
    ++Acquires;
    return TqBufferRef(this, buffer, TqBufferDomain::Worker);
}

TqBufferRef TqLinuxRelayBufferPool::AcquireIngress() {
    if (PendingBytes() + ChunkBytes > MaxBytes) {
        return {};
    }

    TqRelayBufferSlot* buffer = nullptr;
    if (!IngressFree.empty()) {
        buffer = IngressFree.back();
        IngressFree.pop_back();
    } else {
        if (IngressAllocated >= IngressMaxSlots) {
            return {};
        }
        buffer = new (std::nothrow) TqRelayBufferSlot(ChunkBytes);
        if (buffer == nullptr) {
            return {};
        }
        ++IngressAllocated;
    }

    buffer->UsedLength = 0;
    IngressPending += ChunkBytes;
    ++Acquires;
    return TqBufferRef(this, buffer, TqBufferDomain::Ingress);
}

TqBufferRef TqLinuxRelayBufferPool::TransferToWorker(TqBufferRef ingress) {
    if (!ingress || ingress.Pool != this || ingress.Domain != TqBufferDomain::Ingress) {
        return {};
    }
    auto* buffer = ingress.Slot;
    ingress.Pool = nullptr;
    ingress.Slot = nullptr;
    IngressPending -= std::min<uint64_t>(IngressPending, ChunkBytes);
    WorkerPending += ChunkBytes;
    return TqBufferRef(this, buffer, TqBufferDomain::Worker);
}

TqBufferRef TqLinuxRelayBufferPool::Acquire() {
    return AcquireWorker();
}

size_t TqLinuxRelayBufferPool::ChunkSize() const {
    return ChunkBytes;
}

size_t TqLinuxRelayBufferPool::FreeCount() const {
    return WorkerFree.size() + IngressFree.size();
}

uint64_t TqLinuxRelayBufferPool::PendingBytes() const {
    return WorkerPending + IngressPending;
}

uint64_t TqLinuxRelayBufferPool::AcquireCount() const {
    return Acquires;
}

uint64_t TqLinuxRelayBufferPool::MaxPendingBytes() const {
    return MaxBytes;
}

void TqLinuxRelayBufferPool::ReleaseWorker(TqRelayBufferSlot* buffer) {
    if (buffer == nullptr) {
        return;
    }
    buffer->UsedLength = 0;
    WorkerPending -= std::min<uint64_t>(WorkerPending, ChunkBytes);
    WorkerFree.push_back(buffer);
}

void TqLinuxRelayBufferPool::ReleaseIngress(TqRelayBufferSlot* buffer) {
    if (buffer == nullptr) {
        return;
    }
    buffer->UsedLength = 0;
    IngressPending -= std::min<uint64_t>(IngressPending, ChunkBytes);
    IngressFree.push_back(buffer);
}
