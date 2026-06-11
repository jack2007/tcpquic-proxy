#include "linux_relay_buffer_pool.h"

#include <algorithm>
#include <new>

TqRelayBuffer::TqRelayBuffer(TqLinuxRelayBufferPool* pool, size_t capacity)
    : Pool(pool), Storage(capacity) {}

TqRelayBuffer::~TqRelayBuffer() = default;

uint8_t* TqRelayBuffer::Data() {
    return Storage.data();
}

const uint8_t* TqRelayBuffer::Data() const {
    return Storage.data();
}

size_t TqRelayBuffer::Capacity() const {
    return Storage.size();
}

void TqRelayBuffer::SetLength(size_t length) {
    UsedLength = std::min(length, Storage.size());
}

size_t TqRelayBuffer::Length() const {
    return UsedLength;
}

TqLinuxRelayBufferPool::TqLinuxRelayBufferPool(
    size_t chunkSize,
    size_t maxBuffers,
    uint64_t maxPendingBytes)
    : ChunkBytes(chunkSize), MaxBuffers(maxBuffers), MaxBytes(maxPendingBytes) {}

TqLinuxRelayBufferPool::~TqLinuxRelayBufferPool() {
    for (auto* buffer : Free) {
        delete buffer;
    }
    Free.clear();
}

TqBufferRef TqLinuxRelayBufferPool::Acquire() {
    std::lock_guard<std::mutex> guard(Lock);
    if (Pending + ChunkBytes > MaxBytes) {
        return {};
    }

    TqRelayBuffer* buffer = nullptr;
    if (!Free.empty()) {
        buffer = Free.back();
        Free.pop_back();
    } else {
        if (AllocatedBuffers >= MaxBuffers) {
            return {};
        }
        buffer = new (std::nothrow) TqRelayBuffer(this, ChunkBytes);
        if (buffer == nullptr) {
            return {};
        }
        ++AllocatedBuffers;
    }

    buffer->UsedLength = 0;
    Pending += ChunkBytes;
    ++Acquires;
    return TqBufferRef(buffer, [this](TqRelayBuffer* released) {
        Release(released);
    });
}

size_t TqLinuxRelayBufferPool::ChunkSize() const {
    return ChunkBytes;
}

size_t TqLinuxRelayBufferPool::FreeCount() const {
    std::lock_guard<std::mutex> guard(Lock);
    return Free.size();
}

uint64_t TqLinuxRelayBufferPool::PendingBytes() const {
    std::lock_guard<std::mutex> guard(Lock);
    return Pending;
}

uint64_t TqLinuxRelayBufferPool::AcquireCount() const {
    std::lock_guard<std::mutex> guard(Lock);
    return Acquires;
}

uint64_t TqLinuxRelayBufferPool::MaxPendingBytes() const {
    return MaxBytes;
}

void TqLinuxRelayBufferPool::Release(TqRelayBuffer* buffer) {
    if (buffer == nullptr) {
        return;
    }
    std::lock_guard<std::mutex> guard(Lock);
    buffer->UsedLength = 0;
    Pending -= std::min<uint64_t>(Pending, ChunkBytes);
    Free.push_back(buffer);
}
