#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <vector>

class TqLinuxRelayBufferPool;

enum class TqBufferDomain {
    Worker,
    Ingress,
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
    friend class TqLinuxRelayBufferPool;

    std::vector<uint8_t> Storage;
    size_t UsedLength{0};
};

class TqBufferHandle final {
public:
    TqBufferHandle() = default;
    TqBufferHandle(
        TqLinuxRelayBufferPool* pool,
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
    friend class TqLinuxRelayBufferPool;

    TqLinuxRelayBufferPool* Pool{nullptr};
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

class TqLinuxRelayBufferPool final {
public:
    TqLinuxRelayBufferPool(size_t chunkSize, size_t maxBuffers, uint64_t maxPendingBytes);
    ~TqLinuxRelayBufferPool();

    TqLinuxRelayBufferPool(const TqLinuxRelayBufferPool&) = delete;
    TqLinuxRelayBufferPool& operator=(const TqLinuxRelayBufferPool&) = delete;

    void Reserve(size_t slotCount);
    TqBufferRef AcquireWorker();
    TqBufferRef AcquireIngress();
    TqBufferRef TransferToWorker(TqBufferRef ingress);
    TqBufferRef Acquire();
    size_t ChunkSize() const;
    size_t FreeCount() const;
    uint64_t PendingBytes() const;
    uint64_t AcquireCount() const;
    uint64_t MaxPendingBytes() const;

private:
    friend class TqBufferHandle;

    void ReleaseWorker(TqRelayBufferSlot* slot);
    void ReleaseIngress(TqRelayBufferSlot* slot);

    size_t ChunkBytes;
    size_t MaxBuffers;
    uint64_t MaxBytes;
    std::vector<TqRelayBufferSlot*> WorkerFree;
    std::vector<TqRelayBufferSlot*> IngressFree;
    size_t WorkerMaxSlots;
    size_t IngressMaxSlots{0};
    size_t WorkerAllocated{0};
    size_t IngressAllocated{0};
    std::atomic<uint64_t> WorkerPending{0};
    std::atomic<uint64_t> IngressPending{0};
    uint64_t Acquires{0};
    // Ingress free-list is touched from MsQuic callback thread(s) and the relay
    // worker when receive events are dropped after dequeue; worker slots stay
    // worker-thread-only.
    mutable std::mutex IngressLock;
};
