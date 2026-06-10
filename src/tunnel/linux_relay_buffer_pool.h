#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <vector>

class TqLinuxRelayBufferPool;

class TqRelayBuffer final {
public:
    TqRelayBuffer(TqLinuxRelayBufferPool* pool, size_t capacity);
    ~TqRelayBuffer();

    TqRelayBuffer(const TqRelayBuffer&) = delete;
    TqRelayBuffer& operator=(const TqRelayBuffer&) = delete;

    uint8_t* Data();
    const uint8_t* Data() const;
    size_t Capacity() const;
    void SetLength(size_t length);
    size_t Length() const;

private:
    friend class TqLinuxRelayBufferPool;

    TqLinuxRelayBufferPool* Pool;
    std::vector<uint8_t> Storage;
    size_t UsedLength{0};
};

using TqBufferRef = std::shared_ptr<TqRelayBuffer>;

struct TqBufferView {
    uint8_t* Data{nullptr};
    size_t Len{0};
    TqBufferRef Owner;
};

class TqLinuxRelayBufferPool final {
public:
    TqLinuxRelayBufferPool(size_t chunkSize, size_t maxBuffers, uint64_t maxPendingBytes);
    ~TqLinuxRelayBufferPool();

    TqLinuxRelayBufferPool(const TqLinuxRelayBufferPool&) = delete;
    TqLinuxRelayBufferPool& operator=(const TqLinuxRelayBufferPool&) = delete;

    TqBufferRef Acquire();
    size_t ChunkSize() const;
    size_t FreeCount() const;
    uint64_t PendingBytes() const;
    uint64_t MaxPendingBytes() const;

private:
    friend class TqRelayBuffer;

    void Release(TqRelayBuffer* buffer);

    size_t ChunkBytes;
    size_t MaxBuffers;
    uint64_t MaxBytes;
    mutable std::mutex Lock;
    std::vector<TqRelayBuffer*> Free;
    size_t AllocatedBuffers{0};
    uint64_t Pending{0};
};
