#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <vector>

class TqRelayBufferPool;

enum class TqBufferDomain {
    Worker,
};

enum class TqBufferAcquireFailure {
    None,
    PendingBytesLimit,
    SlotLimit,
    AllocationFailure,
};

class TqRelayBufferSlot final {
public:
    explicit TqRelayBufferSlot(size_t capacity);

    TqRelayBufferSlot(const TqRelayBufferSlot&) = delete;
    TqRelayBufferSlot& operator=(const TqRelayBufferSlot&) = delete;

    uint8_t* Data();
    const uint8_t* Data() const;
    size_t Capacity() const;
    void SetLength(size_t length);
    size_t Length() const;

private:
    friend class TqRelayBufferPool;

    std::vector<uint8_t> Storage;
    size_t UsedLength{0};
};

class TqBufferHandle final {
public:
    TqBufferHandle() = default;
    TqBufferHandle(
        TqRelayBufferPool* pool,
        TqRelayBufferSlot* slot,
        TqBufferDomain domain) noexcept;
    TqBufferHandle(TqBufferHandle&& other) noexcept;
    TqBufferHandle& operator=(TqBufferHandle&& other) noexcept;
    ~TqBufferHandle();

    TqBufferHandle(const TqBufferHandle&) = delete;
    TqBufferHandle& operator=(const TqBufferHandle&) = delete;

    TqRelayBufferSlot* get() const;
    TqRelayBufferSlot* operator->() const;
    TqRelayBufferSlot& operator*() const;
    explicit operator bool() const;
    void reset();
    void abandon();

private:
    friend class TqRelayBufferPool;

    TqRelayBufferPool* Pool{nullptr};
    TqRelayBufferSlot* Slot{nullptr};
    TqBufferDomain Domain{TqBufferDomain::Worker};
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

class TqRelayBufferPool final {
public:
    TqRelayBufferPool(size_t chunkSize, size_t maxBuffers, uint64_t maxPendingBytes);
    ~TqRelayBufferPool();

    TqRelayBufferPool(const TqRelayBufferPool&) = delete;
    TqRelayBufferPool& operator=(const TqRelayBufferPool&) = delete;

    void Reserve(size_t slotCount);
    TqBufferRef Acquire(TqBufferDomain domain, TqBufferAcquireFailure* failure = nullptr);
    TqBufferRef AcquireWorker(TqBufferAcquireFailure* failure = nullptr);
    TqBufferRef Acquire();
    size_t ChunkSize() const;
    size_t FreeCount() const;
    uint64_t PendingBytes() const;
    uint64_t AcquireCount() const;
    uint64_t MaxPendingBytes() const;

private:
    friend class TqBufferHandle;

    bool ReservePending(TqBufferAcquireFailure* failure);
    void ReleasePending();
    void ReleaseWorker(TqRelayBufferSlot* slot);

    size_t ChunkBytes;
    size_t MaxBuffers;
    uint64_t MaxBytes;
    std::vector<TqRelayBufferSlot*> WorkerFree;
    size_t WorkerMaxSlots;
    size_t WorkerAllocated{0};
    std::atomic<uint64_t> WorkerPending{0};
    std::atomic<uint64_t> PendingReserved{0};
    std::atomic<uint64_t> Acquires{0};
    mutable std::mutex WorkerLock;
};
