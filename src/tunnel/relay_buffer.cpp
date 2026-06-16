#include "relay_buffer.h"

#include "relay_alloc.h"

#include <algorithm>
#include <new>
#include <utility>

void TqRelayBuffer::SetLength(size_t len) {
    UsedLength = std::min(len, StorageCapacity);
}

TqBufferView::TqBufferView(uint8_t* data, size_t len, TqBufferRef owner) noexcept
    : Data(data), Len(len), Owner(std::move(owner)) {}

TqBufferHandle::TqBufferHandle(TqBufferHandle&& other) noexcept
    : Buffer(std::exchange(other.Buffer, nullptr)),
      Budget(std::exchange(other.Budget, nullptr)),
      ReservedBytes(other.ReservedBytes) {
    other.ReservedBytes = 0;
}

TqBufferHandle& TqBufferHandle::operator=(TqBufferHandle&& other) noexcept {
    if (this != &other) {
        reset();
        Buffer = std::exchange(other.Buffer, nullptr);
        Budget = std::exchange(other.Budget, nullptr);
        ReservedBytes = other.ReservedBytes;
        other.ReservedBytes = 0;
    }
    return *this;
}

TqBufferHandle::~TqBufferHandle() {
    reset();
}

TqRelayBuffer* TqBufferHandle::get() const {
    return Buffer;
}

TqRelayBuffer* TqBufferHandle::operator->() const {
    return Buffer;
}

TqBufferHandle::operator bool() const {
    return Buffer != nullptr;
}

void TqBufferHandle::reset() {
    if (Buffer == nullptr) {
        Budget = nullptr;
        ReservedBytes = 0;
        return;
    }

    TqFreeBytes(Buffer->Storage, Buffer->StorageCapacity);
    if (Budget != nullptr && ReservedBytes > 0) {
        Budget->PendingBufferBytes.fetch_sub(ReservedBytes, std::memory_order_relaxed);
    }

    delete Buffer;
    Buffer = nullptr;
    Budget = nullptr;
    ReservedBytes = 0;
}

TqBufferRef TqAllocateRelayBuffer(
    TqRelayBufferBudget* budget,
    size_t bytes,
    TqBufferAcquireFailure* failure) {
    if (failure != nullptr) {
        *failure = TqBufferAcquireFailure::None;
    }

    if (bytes == 0) {
        return {};
    }

    if (budget == nullptr) {
        if (failure != nullptr) {
            *failure = TqBufferAcquireFailure::AllocationFailure;
        }
        return {};
    }

    uint64_t current = budget->PendingBufferBytes.load(std::memory_order_relaxed);
    while (true) {
        if (current + bytes > budget->MaxPendingBufferBytes) {
            if (failure != nullptr) {
                *failure = TqBufferAcquireFailure::PendingBytesLimit;
            }
            return {};
        }
        if (budget->PendingBufferBytes.compare_exchange_weak(
                current,
                current + bytes,
                std::memory_order_relaxed,
                std::memory_order_relaxed)) {
            break;
        }
    }

    void* ptr = TqAllocBytes(bytes);
    if (ptr == nullptr) {
        budget->PendingBufferBytes.fetch_sub(bytes, std::memory_order_relaxed);
        if (failure != nullptr) {
            *failure = TqBufferAcquireFailure::AllocationFailure;
        }
        return {};
    }

    auto* buffer = new (std::nothrow) TqRelayBuffer{};
    if (buffer == nullptr) {
        TqFreeBytes(ptr, bytes);
        budget->PendingBufferBytes.fetch_sub(bytes, std::memory_order_relaxed);
        if (failure != nullptr) {
            *failure = TqBufferAcquireFailure::AllocationFailure;
        }
        return {};
    }

    buffer->Storage = static_cast<uint8_t*>(ptr);
    buffer->StorageCapacity = bytes;
    buffer->UsedLength = 0;

    budget->AllocateCount.fetch_add(1, std::memory_order_relaxed);

    TqBufferHandle handle;
    handle.Buffer = buffer;
    handle.Budget = budget;
    handle.ReservedBytes = bytes;
    return handle;
}
