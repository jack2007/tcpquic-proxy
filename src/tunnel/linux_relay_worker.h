#pragma once

#include "compress.h"
#include "linux_relay_buffer_pool.h"
#include "relay.h"
#include "tuning.h"

#include <msquic.hpp>

#include <atomic>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

struct MsQuicStream;
struct QUIC_STREAM_EVENT;

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
    uint64_t RelayId{0};
    void* Relay{nullptr};
    TqBufferRef Buffer;
    std::vector<TqBufferRef> Buffers;
    size_t Length{0};
    size_t TotalLength{0};
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

struct TqLinuxRelayRegistration {
    int TcpFd{-1};
    MsQuicStream* Stream{nullptr};
    TqRelayHandle* Handle{nullptr};
    ITqCompressor* Compressor{nullptr};
    ITqDecompressor* Decompressor{nullptr};
    TqCompressAlgo CompressAlgo{TqCompressAlgo::None};
    bool EnableQuicSends{true};
};

struct TqLinuxRelayRegistrationResult {
    bool Ok{false};
    uint64_t RelayId{0};
};

struct TqLinuxRelaySendOperation {
    static constexpr uint64_t MagicValue = 0x5451524c59534e44ULL; // 'TQRLYSND'

    uint64_t Magic{MagicValue};
    uint64_t RelayId{0};
    std::vector<TqBufferView> Views;
    std::vector<QUIC_BUFFER> QuicBuffers;
};

struct TqLinuxRelayWorkerSnapshot {
    uint64_t EventsProcessed{0};
    uint64_t WakeupWrites{0};
    uint64_t PendingEvents{0};
    uint64_t PendingBytes{0};
    uint64_t TcpReadBatches{0};
    uint64_t TcpReadBytes{0};
    uint64_t QuicSendOperations{0};
    uint64_t MaxTcpReadIovUsed{0};
    uint64_t TcpWriteBatches{0};
    uint64_t TcpWriteBytes{0};
    uint64_t MaxTcpWriteIovUsed{0};
    uint64_t ReadDisabledCount{0};
    uint64_t BufferAcquireCount{0};
    uint64_t StreamLookupScanCount{0};
    uint64_t CompressedTcpBytes{0};
    uint64_t DecompressedTcpBytes{0};
    uint64_t Errors{0};
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
    void Enqueue(TqLinuxRelayEvent event);
    void EnqueueForTest(const TqLinuxRelayEvent& event);
    size_t DrainForTest(size_t budget);
    TqLinuxRelayWorkerSnapshot Snapshot() const;
    TqLinuxRelayRegistrationResult RegisterRelayWithId(const TqLinuxRelayRegistration& registration);
    bool RegisterRelay(const TqLinuxRelayRegistration& registration);
    bool RegisterRelayForTest(const TqLinuxRelayRegistration& registration);
    void UnregisterRelay(uint64_t relayId);
    bool WaitForObservedTcpBytesForTest(uint64_t bytes, int timeoutMs);
    std::vector<uint8_t> TakeCapturedQuicBytesForTest(int tcpFd);
    bool EnqueueQuicReceiveForTest(int tcpFd, const uint8_t* data, size_t length, bool fin);
    QUIC_STATUS DispatchStreamEventForTest(MsQuicStream* stream, QUIC_STREAM_EVENT* event);

    static QUIC_STATUS QUIC_API StreamCallback(
        _In_ MsQuicStream* stream,
        _In_opt_ void* context,
        _Inout_ QUIC_STREAM_EVENT* event) noexcept;

private:
    struct RelayState;
    struct StreamRelayBinding;
    void Wake();
    size_t DrainEvents(size_t budget);
    void DrainTcpReadable(RelayState* relay);
    bool BuildTcpToQuicViews(
        RelayState* relay,
        std::vector<TqBufferView>& input,
        std::vector<TqBufferView>& output);
    bool SubmitTcpBatchToQuic(RelayState* relay, std::vector<TqBufferView>& views);
    void CompleteQuicSend(void* context);
    std::shared_ptr<RelayState> FindRelayById(uint64_t relayId);
    std::shared_ptr<RelayState> FindRelayByFd(int tcpFd);
    uint64_t FindRelayIdByStream(MsQuicStream* stream);
    bool CopyQuicReceiveBatchToEvent(
        uint64_t relayId,
        const QUIC_BUFFER* buffers,
        uint32_t bufferCount,
        bool fin);
    void AbortRelayFromCallback(uint64_t relayId, MsQuicStream* stream);
    void ProcessQuicReceiveEvent(const TqLinuxRelayEvent& event);
    bool EnqueueQuicReceive(RelayState* relay, const uint8_t* data, size_t length, bool fin);
    void FlushTcpWrites(RelayState* relay);
    void ArmTcpWritable(RelayState* relay, bool enabled);
    QUIC_STATUS OnStreamEvent(MsQuicStream* stream, QUIC_STREAM_EVENT* event) noexcept;
    QUIC_STATUS OnStreamEventWithBinding(
        MsQuicStream* stream,
        QUIC_STREAM_EVENT* event,
        StreamRelayBinding* binding) noexcept;
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
    mutable std::mutex RelayLock;
    std::deque<std::shared_ptr<RelayState>> Relays;
    uint64_t NextRelayId{1};
    std::atomic<uint64_t> TcpReadBatches{0};
    std::atomic<uint64_t> TcpReadBytes{0};
    std::atomic<uint64_t> QuicSendOperations{0};
    std::atomic<uint64_t> MaxTcpReadIovUsed{0};
    std::atomic<uint64_t> TcpWriteBatches{0};
    std::atomic<uint64_t> TcpWriteBytes{0};
    std::atomic<uint64_t> MaxTcpWriteIovUsed{0};
    std::atomic<uint64_t> ReadDisabledCount{0};
    std::atomic<uint64_t> StreamLookupScanCount{0};
    std::atomic<uint64_t> CompressedTcpBytes{0};
    std::atomic<uint64_t> DecompressedTcpBytes{0};
    std::atomic<uint64_t> Errors{0};
    mutable std::mutex RetiredBindingLock;
    std::vector<std::unique_ptr<StreamRelayBinding>> RetiredStreamBindings;
};

class TqLinuxRelayRuntime final {
public:
    static TqLinuxRelayRuntime& Instance();
    bool Start(const TqTuningConfig& tuning);
    void Stop();
    TqLinuxRelayWorker* PickWorker();
    TqLinuxRelayWorkerSnapshot Snapshot() const;

private:
    TqLinuxRelayRuntime() = default;
    mutable std::mutex Lock;
    std::vector<std::unique_ptr<TqLinuxRelayWorker>> Workers;
    size_t NextWorker{0};
};
