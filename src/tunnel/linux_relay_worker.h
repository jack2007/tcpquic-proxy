#pragma once

#include "compress.h"
#include "linux_relay_buffer_pool.h"
#include "linux_relay_event_queue.h"
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

struct TqLinuxRelayWorkerConfig {
    uint32_t EventBudget{4096};
    uint64_t ByteBudgetPerTick{64ull * 1024 * 1024};
    size_t ReadChunkSize{128 * 1024};
    size_t ReadBatchBytes{1024 * 1024};
    uint32_t MaxIov{16};
    uint32_t WorkerSlots{128};
    uint32_t IngressSlots{128};
    uint64_t MaxPendingBytes{256ull * 1024 * 1024};
    uint64_t MaxPendingQuicReceiveBytesPerRelay{0};
    uint64_t DeferredReceiveCompleteBatchBytes{0};
    size_t EventQueueCapacity{4096};
    bool TrackEventProducers{false};
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
    uint64_t TcpWriteSendmsgCalls{0};
    uint64_t MaxTcpWriteSendmsgBytes{0};
    uint64_t TcpWriteEagainCount{0};
    uint64_t TcpWritePartialCount{0};
    uint64_t ReadDisabledCount{0};
    uint64_t BufferAcquireCount{0};
    uint64_t StreamLookupScanCount{0};
    uint64_t CompressedTcpBytes{0};
    uint64_t DecompressedTcpBytes{0};
    uint64_t DeferredReceiveCompleteBytes{0};
    uint64_t DeferredReceiveCompletes{0};
    uint64_t DeferredReceiveCompletionFlushes{0};
    uint64_t MaxPendingQuicReceiveBytes{0};
    uint64_t MaxPendingQuicReceiveQueue{0};
    uint64_t QuicReceivePausedCount{0};
    uint64_t QuicReceiveResumedCount{0};
    uint64_t Errors{0};
    uint64_t EventQueueFullErrors{0};
    uint64_t TcpReadBufferAcquireFailures{0};
    uint64_t TcpReadBufferAcquirePendingBudgetFailures{0};
    uint64_t TcpReadBufferAcquireSlotLimitFailures{0};
    uint64_t TcpReadBufferAcquireAllocFailures{0};
    uint64_t TcpToQuicCompressFailures{0};
    uint64_t TcpToQuicCompressUpdateFailures{0};
    uint64_t TcpToQuicCompressFlushFailures{0};
    uint64_t TcpToQuicBufferAcquireFailures{0};
    uint64_t TcpToQuicBufferAcquirePendingBudgetFailures{0};
    uint64_t TcpToQuicBufferAcquireSlotLimitFailures{0};
    uint64_t TcpToQuicBufferAcquireAllocFailures{0};
    uint64_t QuicSendFailures{0};
    uint64_t QuicSendBufferTooLargeFailures{0};
    uint64_t QuicSendOperationAllocFailures{0};
    uint64_t QuicSendApiFailures{0};
    uint64_t QuicReceiveIngressBufferAcquireFailures{0};
    uint64_t QuicReceiveIngressBufferAcquirePendingBudgetFailures{0};
    uint64_t QuicReceiveIngressBufferAcquireSlotLimitFailures{0};
    uint64_t QuicReceiveIngressBufferAcquireAllocFailures{0};
    uint64_t QuicReceiveViewFailures{0};
    uint64_t QuicReceiveViewAllocFailures{0};
    uint64_t QuicReceiveViewNullBufferFailures{0};
    uint64_t QuicReceiveViewEmptyFailures{0};
    uint64_t QuicReceiveViewEnqueueFailures{0};
    uint64_t QuicReceiveDecompressFailures{0};
    uint64_t QuicReceiveTcpBufferAcquireFailures{0};
    uint64_t QuicReceiveTcpBufferAcquirePendingBudgetFailures{0};
    uint64_t QuicReceiveTcpBufferAcquireSlotLimitFailures{0};
    uint64_t QuicReceiveTcpBufferAcquireAllocFailures{0};
    uint64_t TcpWriteHardErrors{0};
    uint64_t LastTcpWriteErrno{0};
    int64_t LastQuicSendStatus{0};
    uint64_t EventProducerThreadsObserved{0};
    bool MultipleEventProducerThreadsObserved{false};
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
    bool Enqueue(TqLinuxRelayEvent event);
    bool EnqueueForTest(TqLinuxRelayEvent event);
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
    enum class RelayErrorKind {
        EventQueueFull,
        TcpReadBufferAcquire,
        TcpToQuicCompress,
        TcpToQuicBufferAcquire,
        QuicSend,
        QuicReceiveIngressBufferAcquire,
        QuicReceiveView,
        QuicReceiveDecompress,
        QuicReceiveTcpBufferAcquire,
        TcpWriteHard
    };

    struct RelayState;
    struct StreamRelayBinding;
    void RecordError(RelayErrorKind kind);
    void RecordBufferAcquireFailure(RelayErrorKind kind, TqBufferAcquireFailure failure);
    void Wake();
    void RecordEventProducer();
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
    bool QueueDeferredQuicReceive(
        RelayState* relay,
        MsQuicStream* stream,
        const QUIC_BUFFER* buffers,
        uint32_t bufferCount,
        bool fin);
    void ProcessQuicReceiveViewEvent(TqLinuxRelayEvent& event);
    void FlushDeferredQuicReceives(RelayState* relay);
    void CompleteDeferredQuicReceive(MsQuicStream* stream, uint64_t bytes);
    void FlushDeferredReceiveCompletion(TqPendingQuicReceive& view, bool force);
    bool QueueDeferredQuicReceiveFromOffset(
        RelayState* relay,
        MsQuicStream* stream,
        const QUIC_BUFFER* buffers,
        uint32_t bufferCount,
        uint64_t completedPrefix,
        bool fin);
    uint64_t MaxPendingQuicReceiveBytesPerRelay() const;
    uint64_t LowPendingQuicReceiveBytesPerRelay() const;
    void MaybePauseQuicReceive(RelayState* relay);
    void MaybeResumeQuicReceive(RelayState* relay);
    void SetQuicReceiveEnabled(RelayState* relay, bool enabled);
    void ArmTcpReadable(RelayState* relay, bool enabled);
    void AbortRelayFromCallback(uint64_t relayId, MsQuicStream* stream);
    void ProcessQuicReceiveEvent(TqLinuxRelayEvent& event);
    void PurgeRetiredRelaysIfIdle();
    bool EnqueueQuicReceive(RelayState* relay, const uint8_t* data, size_t length, bool fin);
    void FlushTcpWrites(RelayState* relay);
    void UpdateTcpInterest(RelayState* relay);
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
    TqLinuxRelayEventQueue EventQueue;
    std::atomic<size_t> FirstEventProducerHash{0};
    std::atomic<uint64_t> EventProducerThreadCount{0};
    std::atomic<bool> MultipleEventProducerThreadsObserved{false};
    std::thread Thread;
    std::atomic<uint64_t> EventsProcessed{0};
    std::atomic<uint64_t> WakeupWrites{0};
    mutable std::mutex RelayLock;
    std::deque<std::shared_ptr<RelayState>> Relays;
    std::deque<std::shared_ptr<RelayState>> RetiredRelays;
    uint64_t NextRelayId{1};
    std::atomic<uint64_t> TcpReadBatches{0};
    std::atomic<uint64_t> TcpReadBytes{0};
    std::atomic<uint64_t> QuicSendOperations{0};
    std::atomic<uint64_t> MaxTcpReadIovUsed{0};
    std::atomic<uint64_t> TcpWriteBatches{0};
    std::atomic<uint64_t> TcpWriteBytes{0};
    std::atomic<uint64_t> MaxTcpWriteIovUsed{0};
    std::atomic<uint64_t> TcpWriteSendmsgCalls{0};
    std::atomic<uint64_t> MaxTcpWriteSendmsgBytes{0};
    std::atomic<uint64_t> TcpWriteEagainCount{0};
    std::atomic<uint64_t> TcpWritePartialCount{0};
    std::atomic<uint64_t> ReadDisabledCount{0};
    std::atomic<uint64_t> StreamLookupScanCount{0};
    std::atomic<uint64_t> CompressedTcpBytes{0};
    std::atomic<uint64_t> DecompressedTcpBytes{0};
    std::atomic<uint64_t> DeferredReceiveCompleteBytes{0};
    std::atomic<uint64_t> DeferredReceiveCompletes{0};
    std::atomic<uint64_t> DeferredReceiveCompletionFlushes{0};
    std::atomic<uint64_t> MaxPendingQuicReceiveBytesObserved{0};
    std::atomic<uint64_t> MaxPendingQuicReceiveQueueObserved{0};
    std::atomic<uint64_t> QuicReceivePausedCount{0};
    std::atomic<uint64_t> QuicReceiveResumedCount{0};
    std::atomic<uint64_t> Errors{0};
    std::atomic<uint64_t> EventQueueFullErrors{0};
    std::atomic<uint64_t> TcpReadBufferAcquireFailures{0};
    std::atomic<uint64_t> TcpReadBufferAcquirePendingBudgetFailures{0};
    std::atomic<uint64_t> TcpReadBufferAcquireSlotLimitFailures{0};
    std::atomic<uint64_t> TcpReadBufferAcquireAllocFailures{0};
    std::atomic<uint64_t> TcpToQuicCompressFailures{0};
    std::atomic<uint64_t> TcpToQuicCompressUpdateFailures{0};
    std::atomic<uint64_t> TcpToQuicCompressFlushFailures{0};
    std::atomic<uint64_t> TcpToQuicBufferAcquireFailures{0};
    std::atomic<uint64_t> TcpToQuicBufferAcquirePendingBudgetFailures{0};
    std::atomic<uint64_t> TcpToQuicBufferAcquireSlotLimitFailures{0};
    std::atomic<uint64_t> TcpToQuicBufferAcquireAllocFailures{0};
    std::atomic<uint64_t> QuicSendFailures{0};
    std::atomic<uint64_t> QuicSendBufferTooLargeFailures{0};
    std::atomic<uint64_t> QuicSendOperationAllocFailures{0};
    std::atomic<uint64_t> QuicSendApiFailures{0};
    std::atomic<uint64_t> QuicReceiveIngressBufferAcquireFailures{0};
    std::atomic<uint64_t> QuicReceiveIngressBufferAcquirePendingBudgetFailures{0};
    std::atomic<uint64_t> QuicReceiveIngressBufferAcquireSlotLimitFailures{0};
    std::atomic<uint64_t> QuicReceiveIngressBufferAcquireAllocFailures{0};
    std::atomic<uint64_t> QuicReceiveViewFailures{0};
    std::atomic<uint64_t> QuicReceiveViewAllocFailures{0};
    std::atomic<uint64_t> QuicReceiveViewNullBufferFailures{0};
    std::atomic<uint64_t> QuicReceiveViewEmptyFailures{0};
    std::atomic<uint64_t> QuicReceiveViewEnqueueFailures{0};
    std::atomic<uint64_t> QuicReceiveDecompressFailures{0};
    std::atomic<uint64_t> QuicReceiveTcpBufferAcquireFailures{0};
    std::atomic<uint64_t> QuicReceiveTcpBufferAcquirePendingBudgetFailures{0};
    std::atomic<uint64_t> QuicReceiveTcpBufferAcquireSlotLimitFailures{0};
    std::atomic<uint64_t> QuicReceiveTcpBufferAcquireAllocFailures{0};
    std::atomic<uint64_t> TcpWriteHardErrors{0};
    std::atomic<uint64_t> LastTcpWriteErrno{0};
    std::atomic<int64_t> LastQuicSendStatus{0};
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
