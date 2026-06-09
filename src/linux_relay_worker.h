#pragma once

#include "linux_relay_buffer_pool.h"
#include "tuning.h"

#include <atomic>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <thread>

struct MsQuicStream;
struct TqRelayHandle;

enum class TqLinuxRelayEventType {
    TestMarker,
    TcpWritable,
    QuicReceive,
    QuicSendComplete,
    QuicIdealSendBuffer,
    Shutdown,
};

struct TqLinuxRelayEvent {
    TqLinuxRelayEventType Type{TqLinuxRelayEventType::Shutdown};
    uint64_t Value{0};
    void* Relay{nullptr};
    TqBufferRef Buffer;
    size_t Length{0};
    bool Fin{false};
};

struct TqLinuxRelayWorkerConfig {
    uint32_t EventBudget{4096};
    uint64_t ByteBudgetPerTick{64ull * 1024 * 1024};
    size_t ReadChunkSize{64 * 1024};
    size_t ReadBatchBytes{1024 * 1024};
    uint32_t MaxIov{16};
    uint64_t MaxPendingBytes{256ull * 1024 * 1024};
};

struct TqLinuxRelayWorkerSnapshot {
    uint64_t EventsProcessed{0};
    uint64_t WakeupWrites{0};
    uint64_t PendingEvents{0};
    uint64_t PendingBytes{0};
};

class TqLinuxRelayWorker final {
public:
    explicit TqLinuxRelayWorker(const TqLinuxRelayWorkerConfig& config);
    ~TqLinuxRelayWorker();

    TqLinuxRelayWorker(const TqLinuxRelayWorker&) = delete;
    TqLinuxRelayWorker& operator=(const TqLinuxRelayWorker&) = delete;

    bool Start();
    bool StartForTest();
    void Stop();
    void Enqueue(const TqLinuxRelayEvent& event);
    void EnqueueForTest(const TqLinuxRelayEvent& event);
    size_t DrainForTest(size_t budget);
    TqLinuxRelayWorkerSnapshot Snapshot() const;

private:
    void Wake();
    size_t DrainEvents(size_t budget);
    void Run();

    TqLinuxRelayWorkerConfig Config;
    int WakeFd{-1};
    int EpollFd{-1};
    std::atomic<bool> Running{false};
    std::atomic<bool> WakeArmed{false};
    mutable std::mutex QueueLock;
    std::deque<TqLinuxRelayEvent> Queue;
    std::thread Thread;
    std::atomic<uint64_t> EventsProcessed{0};
    std::atomic<uint64_t> WakeupWrites{0};
};
