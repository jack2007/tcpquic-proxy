#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>

enum class TqBufferAcquireFailure { None, PendingBytesLimit, AllocationFailure };

struct TqRelayBufferBudget {
    std::atomic<uint64_t> PendingBufferBytes{0};
    uint64_t MaxPendingBufferBytes{0};
    std::atomic<uint64_t> AllocateCount{0};
};

struct TqRelayBuffer {
    uint8_t* Storage{nullptr};
    size_t StorageCapacity{0};
    size_t UsedLength{0};

    uint8_t* Data() { return Storage; }
    const uint8_t* Data() const { return Storage; }
    size_t Capacity() const { return StorageCapacity; }
    size_t Length() const { return UsedLength; }
    void SetLength(size_t len);
};

class TqBufferHandle {
public:
    TqBufferHandle() = default;
    ~TqBufferHandle();
    TqBufferHandle(TqBufferHandle&&) noexcept;
    TqBufferHandle& operator=(TqBufferHandle&&) noexcept;
    TqBufferHandle(const TqBufferHandle&) = delete;
    TqBufferHandle& operator=(const TqBufferHandle&) = delete;

    TqRelayBuffer* get() const;
    TqRelayBuffer* operator->() const;
    explicit operator bool() const;
    void reset();
    void abandon() { reset(); }

private:
    friend TqBufferHandle TqAllocateRelayBuffer(
        TqRelayBufferBudget* budget, size_t bytes, TqBufferAcquireFailure* failure);
    TqRelayBuffer* Buffer{nullptr};
    TqRelayBufferBudget* Budget{nullptr};
    size_t ReservedBytes{0};
};

using TqBufferRef = TqBufferHandle;

struct TqBufferView {
    uint8_t* Data{nullptr};
    size_t Len{0};
    TqBufferRef Owner;

    TqBufferView() = default;
    TqBufferView(uint8_t* data, size_t len, TqBufferRef owner) noexcept;

    TqBufferView(TqBufferView&& other) noexcept = default;
    TqBufferView& operator=(TqBufferView&& other) noexcept = default;
    TqBufferView(const TqBufferView&) = delete;
    TqBufferView& operator=(const TqBufferView&) = delete;
};

TqBufferRef TqAllocateRelayBuffer(
    TqRelayBufferBudget* budget,
    size_t bytes,
    TqBufferAcquireFailure* failure = nullptr);
