#pragma once

#if defined(__APPLE__)

#include "compress.h"
#include "darwin_relay_event_queue.h"
#include "msquic.hpp"
#include "platform_socket.h"
#include "relay.h"
#include "relay_buffer.h"
#include "tuning.h"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <thread>
#include <unordered_map>
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

struct TqDarwinRelaySendOperation {
    static constexpr uint64_t MagicValue = 0x545144415257534eULL;

    uint64_t Magic{MagicValue};
    uint64_t RelayId{0};
    uint64_t TotalBytes{0};
    bool Fin{false};
    std::shared_ptr<void> BindingOwner;
    std::vector<TqBufferView> Views;
    std::vector<QUIC_BUFFER> QuicBuffers;
};

#if defined(TCPQUIC_TESTING)
using TqDarwinRelayStreamSendForTest = QUIC_STATUS (*)(
    MsQuicStream* stream,
    const QUIC_BUFFER* buffers,
    uint32_t bufferCount,
    QUIC_SEND_FLAGS flags,
    void* context);
using TqDarwinRelayReceiveCompleteForTest = void (*)(MsQuicStream* stream, uint64_t byteCount);
using TqDarwinRelayReceiveSetEnabledForTest = QUIC_STATUS (*)(MsQuicStream* stream, bool enabled);
using TqDarwinRelaySendMsgForTest = ssize_t (*)(TqSocketHandle fd, const struct msghdr* msg);
#endif

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
    uint64_t PendingBytes{0};
    uint64_t ActiveRelays{0};
    uint64_t TcpReadArmedRelays{0};
    uint64_t TcpWriteArmedRelays{0};
    uint64_t QuicReceivePausedCount{0};
    uint64_t QuicReceiveResumedCount{0};
    uint64_t CurrentPendingQuicReceiveBytes{0};
    uint64_t PendingTcpWriteQueue{0};
    uint64_t PendingTcpWriteBytes{0};
    uint64_t OutstandingQuicSends{0};
    uint64_t OutstandingQuicSendBytes{0};
    uint64_t TcpReadBatches{0};
    uint64_t TcpReadBytes{0};
    uint64_t TcpWriteBatches{0};
    uint64_t TcpWriteBytes{0};
    uint64_t QuicReceiveViewCount{0};
    uint64_t QuicReceiveViewBytes{0};
    uint64_t DeferredReceiveCompletes{0};
    uint64_t QuicSendBackpressureEvents{0};
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
    void SetRegisterTcpFiltersFailureForTest(bool fail);
    void SetStreamSendForTest(TqDarwinRelayStreamSendForTest sendFn);
    void SetReceiveCompleteForTest(TqDarwinRelayReceiveCompleteForTest completeFn);
    void SetReceiveSetEnabledForTest(TqDarwinRelayReceiveSetEnabledForTest setEnabledFn);
    void SetSendMsgForTest(TqDarwinRelaySendMsgForTest sendMsgFn);
    bool FlushTcpWritableForTest(uint64_t relayId);
    bool InvokeTcpEventForTest(uint64_t relayId, int16_t filter, uint16_t flags, intptr_t data);
    bool InvokeQuicReceiveViewForTest(const std::shared_ptr<TqDarwinPendingQuicReceive>& receive);
    void* StreamCallbackContextForTest(uint64_t relayId);
    std::shared_ptr<void> StreamCallbackContextOwnerForTest(uint64_t relayId);
    uint64_t KnownSendOperationCountForTest();
    uint64_t PendingQuicSendCountForTest(uint64_t relayId);
    uint64_t InFlightQuicSendCountForTest(uint64_t relayId);
    uint64_t CompleteOneInFlightSendForTest(uint64_t relayId);
    bool CorruptOneInFlightSendMagicForTest(uint64_t relayId);
    uint64_t PendingQuicReceiveBytesForTest(uint64_t relayId);
    uint64_t PendingTcpWriteBytesForTest(uint64_t relayId);
#endif
    TqDarwinRelayRegistrationResult RegisterRelayWithId(const TqDarwinRelayRegistration& registration);
    void UnregisterRelay(uint64_t relayId);
    TqDarwinRelayWorkerSnapshot Snapshot() const;

    static QUIC_STATUS QUIC_API StreamCallback(
        _In_ MsQuicStream* stream,
        _In_opt_ void* context,
        _Inout_ QUIC_STREAM_EVENT* event) noexcept;

private:
    struct RelayState;
    struct CompletionState;
    struct StreamBinding;
    struct KnownSendOperationInfo {
        uint64_t RelayId{0};
        uint64_t TotalBytes{0};
        bool Fin{false};
        bool Submitting{false};
        std::shared_ptr<void> BindingOwner;
    };

    void Run();
    bool Wake();
    bool EnqueueEvent(TqDarwinRelayEvent&& event);
    uint32_t DrainEvents(uint32_t budget);
    uint32_t DrainWakeEvents();
    bool RegisterTcpFilters(const std::shared_ptr<RelayState>& relay);
    bool UpdateTcpInterest(const std::shared_ptr<RelayState>& relay);
    void RemoveTcpFilters(const std::shared_ptr<RelayState>& relay);
    void ClearPublicHandle(const std::shared_ptr<RelayState>& relay);
    std::shared_ptr<RelayState> FindRelay(uint64_t relayId);
    std::shared_ptr<RelayState> FindRetiredRelay(uint64_t relayId);
    void ProcessKqueueEvent(const struct kevent& event);
    void ProcessTcpEvents(uint64_t relayId, int16_t filter, uint16_t flags, intptr_t data);
    bool DrainTcpReadable(const std::shared_ptr<RelayState>& relay);
    bool SubmitTcpBatchToQuic(
        const std::shared_ptr<RelayState>& relay,
        std::vector<TqBufferView>&& views,
        bool fin);
    bool TrySubmitQuicSendOperation(
        const std::shared_ptr<RelayState>& relay,
        std::unique_ptr<TqDarwinRelaySendOperation> operation);
    void RetryPendingQuicSends(const std::shared_ptr<RelayState>& relay);
    void CompleteQuicSend(TqDarwinRelaySendOperation* operation);
    bool ShouldPauseTcpReadForQuicBacklog(const std::shared_ptr<RelayState>& relay) const;
    bool ShouldResumeTcpReadForQuicBacklog(const std::shared_ptr<RelayState>& relay) const;
    bool SetTcpReadBackpressure(const std::shared_ptr<RelayState>& relay, bool paused);
    bool QueueDeferredQuicReceive(
        const std::shared_ptr<RelayState>& relay,
        MsQuicStream* stream,
        const QUIC_BUFFER* buffers,
        uint32_t bufferCount,
        bool fin);
    uint64_t MaxPendingQuicReceiveBytesPerRelay() const;
    uint64_t LowPendingQuicReceiveBytesPerRelay() const;
    void ProcessQuicReceiveViewEvent(const std::shared_ptr<TqDarwinPendingQuicReceive>& receive);
    bool EnqueueQuicReceiveForTcp(
        const std::shared_ptr<RelayState>& relay,
        const std::shared_ptr<TqDarwinPendingQuicReceive>& receive);
    bool DiscardDeferredQuicReceive(
        const std::shared_ptr<RelayState>& relay,
        const std::shared_ptr<TqDarwinPendingQuicReceive>& receive);
    bool FlushTcpWrites(const std::shared_ptr<RelayState>& relay);
    void CompleteDeferredQuicReceive(
        const std::shared_ptr<RelayState>& relay,
        const std::shared_ptr<TqDarwinPendingQuicReceive>& receive);
    bool SetQuicReceiveEnabled(const std::shared_ptr<RelayState>& relay, bool enabled);
    void MaybePauseQuicReceive(const std::shared_ptr<RelayState>& relay);
    void MaybeResumeQuicReceive(const std::shared_ptr<RelayState>& relay);
    void RetireRelay(const std::shared_ptr<RelayState>& relay);
    void CloseRelay(const std::shared_ptr<RelayState>& relay);
    void PurgeRetiredRelaysIfSafe();
    bool WaitForKnownOperationsToDrain();
    void DetachRetiredBindingsForDestruction();
    void RegisterKnownSendOperation(
        TqDarwinRelaySendOperation* operation,
        const KnownSendOperationInfo& info);
    bool MarkKnownSendOperationSubmitted(TqDarwinRelaySendOperation* operation);
    bool UnregisterKnownSendOperation(
        TqDarwinRelaySendOperation* operation,
        KnownSendOperationInfo* info);
    static bool UnregisterCompletionStateOperation(
        const std::shared_ptr<CompletionState>& state,
        TqDarwinRelaySendOperation* operation,
        KnownSendOperationInfo* info);
    static void ClearRetiredStreamCallbackIfSafe(StreamBinding* binding);
    static bool CompleteDetachedQuicSend(StreamBinding* binding, TqDarwinRelaySendOperation* operation);
    bool IsKnownSendOperation(TqDarwinRelaySendOperation* operation) const;

    TqDarwinRelayWorkerConfig Config;
    int KqueueFd{-1};
    std::atomic<bool> Running{false};
    TqDarwinRelayEventQueue EventQueue;
    std::thread Thread;
    mutable std::mutex LifecycleMutex;
    mutable std::mutex RelayMutex;
    std::unordered_map<uint64_t, std::shared_ptr<RelayState>> Relays;
    std::deque<std::shared_ptr<RelayState>> RetiredRelays;
    std::unordered_map<TqDarwinRelaySendOperation*, KnownSendOperationInfo> KnownSendOperations;
    std::vector<std::shared_ptr<StreamBinding>> RetiredStreamBindings;
    uint64_t NextRelayId{1};
#if defined(TCPQUIC_TESTING)
    bool FailRegisterTcpFiltersForTest{false};
#endif
    std::atomic<uint64_t> EventsProcessed{0};
    std::atomic<uint64_t> Wakeups{0};
    std::atomic<uint64_t> TcpReadBatches{0};
    std::atomic<uint64_t> TcpReadBytes{0};
    std::atomic<uint64_t> TcpWriteBatches{0};
    std::atomic<uint64_t> TcpWriteBytes{0};
    std::atomic<uint64_t> QuicReceiveViewCount{0};
    std::atomic<uint64_t> QuicReceiveViewBytes{0};
    std::atomic<uint64_t> DeferredReceiveCompletes{0};
    std::atomic<uint64_t> QuicSendBackpressureEvents{0};
    std::atomic<uint64_t> QuicReceivePausedCount{0};
    std::atomic<uint64_t> QuicReceiveResumedCount{0};
    std::atomic<uint64_t> Errors{0};
};

class TqDarwinRelayRuntime final {
public:
    static TqDarwinRelayRuntime& Instance();

    bool Start(const TqTuningConfig& tuning);
    void Stop();
    TqDarwinRelayWorkerSnapshot Snapshot() const;
    TqDarwinRelayWorker* PickWorker();
    void StopRelay(TqRelayHandle* handle);

private:
    TqDarwinRelayRuntime() = default;

    mutable std::mutex Mutex;
    bool Started{false};
    uint32_t NextWorker{0};
    std::vector<std::unique_ptr<TqDarwinRelayWorker>> Workers;
};

#endif
