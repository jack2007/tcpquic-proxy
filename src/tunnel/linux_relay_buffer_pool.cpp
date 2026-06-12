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

void TqBufferHandle::abandon() {
    if (Slot == nullptr) {
        Pool = nullptr;
        return;
    }
    auto* slot = Slot;
    Pool = nullptr;
    Slot = nullptr;
    delete slot;
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
    std::lock_guard<std::mutex> guard(IngressLock);
    for (auto* buffer : IngressFree) {
        delete buffer;
    }
    IngressFree.clear();
}

void TqLinuxRelayBufferPool::Reserve(size_t slotCount) {
    const size_t capped = std::min(slotCount, MaxBuffers);
    Reserve(capped - (capped / 2), capped / 2);
}

void TqLinuxRelayBufferPool::Reserve(size_t workerSlots, size_t ingressSlots) {
    const size_t total = std::min(workerSlots + ingressSlots, MaxBuffers);
    WorkerMaxSlots = std::min(workerSlots, total);
    IngressMaxSlots = total - WorkerMaxSlots;

    while (WorkerAllocated < WorkerMaxSlots) {
        auto* buffer = new (std::nothrow) TqRelayBufferSlot(ChunkBytes);
        if (buffer == nullptr) {
            return;
        }
        WorkerFree.push_back(buffer);
        ++WorkerAllocated;
    }
    {
        std::lock_guard<std::mutex> guard(IngressLock);
        while (IngressAllocated < IngressMaxSlots) {
            auto* buffer = new (std::nothrow) TqRelayBufferSlot(ChunkBytes);
            if (buffer == nullptr) {
                return;
            }
            IngressFree.push_back(buffer);
            ++IngressAllocated;
        }
    }
}

TqBufferRef TqLinuxRelayBufferPool::AcquireWorker(TqBufferAcquireFailure* failure) {
    if (failure != nullptr) {
        *failure = TqBufferAcquireFailure::None;
    }
    if (PendingBytes() + ChunkBytes > MaxBytes) {
        if (failure != nullptr) {
            *failure = TqBufferAcquireFailure::PendingBytesLimit;
        }
        return {};
    }

    TqRelayBufferSlot* buffer = nullptr;
    if (!WorkerFree.empty()) {
        buffer = WorkerFree.back();
        WorkerFree.pop_back();
    } else {
        if (WorkerAllocated >= WorkerMaxSlots) {
            if (failure != nullptr) {
                *failure = TqBufferAcquireFailure::SlotLimit;
            }
            return {};
        }
        buffer = new (std::nothrow) TqRelayBufferSlot(ChunkBytes);
        if (buffer == nullptr) {
            if (failure != nullptr) {
                *failure = TqBufferAcquireFailure::AllocationFailure;
            }
            return {};
        }
        ++WorkerAllocated;
    }

    buffer->UsedLength = 0;
    WorkerPending.fetch_add(ChunkBytes, std::memory_order_relaxed);
    ++Acquires;
    return TqBufferRef(this, buffer, TqBufferDomain::Worker);
}

TqBufferRef TqLinuxRelayBufferPool::AcquireIngress(TqBufferAcquireFailure* failure) {
    if (failure != nullptr) {
        *failure = TqBufferAcquireFailure::None;
    }
    if (PendingBytes() + ChunkBytes > MaxBytes) {
        if (failure != nullptr) {
            *failure = TqBufferAcquireFailure::PendingBytesLimit;
        }
        return {};
    }

    TqRelayBufferSlot* buffer = nullptr;
    {
        std::lock_guard<std::mutex> guard(IngressLock);
        if (!IngressFree.empty()) {
            buffer = IngressFree.back();
            IngressFree.pop_back();
        } else {
            if (IngressAllocated >= IngressMaxSlots) {
                if (failure != nullptr) {
                    *failure = TqBufferAcquireFailure::SlotLimit;
                }
                return {};
            }
            buffer = new (std::nothrow) TqRelayBufferSlot(ChunkBytes);
            if (buffer == nullptr) {
                if (failure != nullptr) {
                    *failure = TqBufferAcquireFailure::AllocationFailure;
                }
                return {};
            }
            ++IngressAllocated;
        }
    }

    buffer->UsedLength = 0;
    IngressPending.fetch_add(ChunkBytes, std::memory_order_relaxed);
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
    const uint64_t ingressBefore = IngressPending.load(std::memory_order_relaxed);
    if (ingressBefore >= ChunkBytes) {
        IngressPending.fetch_sub(ChunkBytes, std::memory_order_relaxed);
    } else {
        IngressPending.store(0, std::memory_order_relaxed);
    }
    WorkerPending.fetch_add(ChunkBytes, std::memory_order_relaxed);
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
    return WorkerPending.load(std::memory_order_relaxed) +
        IngressPending.load(std::memory_order_relaxed);
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
    const uint64_t workerBefore = WorkerPending.load(std::memory_order_relaxed);
    if (workerBefore >= ChunkBytes) {
        WorkerPending.fetch_sub(ChunkBytes, std::memory_order_relaxed);
    } else {
        WorkerPending.store(0, std::memory_order_relaxed);
    }
    WorkerFree.push_back(buffer);
}

void TqLinuxRelayBufferPool::ReleaseIngress(TqRelayBufferSlot* buffer) {
    if (buffer == nullptr) {
        return;
    }
    buffer->UsedLength = 0;
    const uint64_t ingressBefore = IngressPending.load(std::memory_order_relaxed);
    if (ingressBefore >= ChunkBytes) {
        IngressPending.fetch_sub(ChunkBytes, std::memory_order_relaxed);
    } else {
        IngressPending.store(0, std::memory_order_relaxed);
    }
    std::lock_guard<std::mutex> guard(IngressLock);
    IngressFree.push_back(buffer);
}
