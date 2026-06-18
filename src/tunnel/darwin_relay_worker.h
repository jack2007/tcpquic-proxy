#pragma once

#if defined(__APPLE__)

#include "compress.h"
#include "darwin_relay_event_queue.h"
#include "msquic.hpp"
#include "platform_socket.h"
#include "relay.h"
#include "tuning.h"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

struct MsQuicStream;
struct QUIC_STREAM_EVENT;

struct TqDarwinRelayRegistration {
    TqSocketHandle TcpFd{TqInvalidSocket};
    MsQuicStream* Stream{nullptr};
    TqRelayHandle* Handle{nullptr};
    ITqCompressor* Compressor{nullptr};
    ITqDecompressor* Decompressor{nullptr};
    TqCompressAlgo CompressAlgo{TqCompressAlgo::None};
    bool EnableQuicSends{true};
};

struct TqDarwinRelayRegistrationResult {
    bool Ok{false};
    uint64_t RelayId{0};
};

struct TqDarwinRelayWorkerConfig {
    uint32_t WorkerIndex{0};
    uint32_t EventBudget{4096};
    uint64_t ByteBudgetPerTick{64ull * 1024 * 1024};
    size_t ReadChunkSize{128 * 1024};
    size_t ReadBatchBytes{1024 * 1024};
    uint32_t MaxIov{16};
    uint64_t TcpWriteMaxBytes{0};
    uint64_t TcpWriteBurstBytes{0};
    uint64_t MaxPendingQuicReceiveBytesPerRelay{0};
    uint64_t DeferredReceiveCompleteBatchBytes{0};
    uint32_t MaxInFlightQuicSends{0};
    uint64_t MaxBufferedQuicSendBytes{0};
    size_t EventQueueCapacity{4096};
};

struct TqDarwinRelayWorkerSnapshot {
    uint64_t EventsProcessed{0};
    uint64_t Wakeups{0};
    uint64_t PendingEvents{0};
    uint64_t ActiveRelays{0};
    uint64_t TcpReadArmedRelays{0};
    uint64_t TcpWriteArmedRelays{0};
    uint64_t QuicReceivePausedCount{0};
    uint64_t QuicReceiveResumedCount{0};
    uint64_t Errors{0};
};

class TqDarwinRelayWorker final {
public:
    explicit TqDarwinRelayWorker(const TqDarwinRelayWorkerConfig& config);
    ~TqDarwinRelayWorker();

    TqDarwinRelayWorker(const TqDarwinRelayWorker&) = delete;
    TqDarwinRelayWorker& operator=(const TqDarwinRelayWorker&) = delete;

    bool Start();
    void Stop();
#if defined(TCPQUIC_TESTING)
    bool StartForTest();
    bool EnqueueForTest(TqDarwinRelayEvent event);
    uint32_t DrainWakeForTest();
    bool RunningForTest() const;
#endif
    TqDarwinRelayRegistrationResult RegisterRelayWithId(const TqDarwinRelayRegistration& registration);
    void UnregisterRelay(uint64_t relayId);
    TqDarwinRelayWorkerSnapshot Snapshot() const;

    static QUIC_STATUS QUIC_API StreamCallback(
        _In_ MsQuicStream* stream,
        _In_opt_ void* context,
        _Inout_ QUIC_STREAM_EVENT* event) noexcept;

private:
    void Run();
    bool Wake();
    bool EnqueueEvent(TqDarwinRelayEvent&& event);
    uint32_t DrainEvents(uint32_t budget);
    uint32_t DrainWakeEvents();

    TqDarwinRelayWorkerConfig Config;
    int KqueueFd{-1};
    std::atomic<bool> Running{false};
    TqDarwinRelayEventQueue EventQueue;
    std::thread Thread;
    std::atomic<uint64_t> EventsProcessed{0};
    std::atomic<uint64_t> Wakeups{0};
    std::atomic<uint64_t> Errors{0};
};

class TqDarwinRelayRuntime final {
public:
    static TqDarwinRelayRuntime& Instance();

    bool Start(const TqTuningConfig& tuning);
    void Stop();
    TqDarwinRelayWorker* PickWorker();
    void StopRelay(TqRelayHandle* handle);

private:
    TqDarwinRelayRuntime() = default;

    std::mutex Mutex;
    bool Started{false};
    uint32_t NextWorker{0};
    std::vector<std::unique_ptr<TqDarwinRelayWorker>> Workers;
};

#endif
