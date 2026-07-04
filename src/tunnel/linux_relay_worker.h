#pragma once

#include "compress.h"
#include "relay_buffer.h"
#include "linux_relay_event_queue.h"
#include "platform_socket.h"
#include "relay.h"
#include "relay_error.h"
#include "tuning.h"

#include <msquic.hpp>

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

struct MsQuicStream;
struct QUIC_STREAM_EVENT;

struct TqLinuxRelayWorkerConfig {
    uint32_t WorkerIndex{0};
    uint32_t EventBudget{4096};
    uint64_t ByteBudgetPerTick{64ull * 1024 * 1024};
    size_t ReadChunkSize{128 * 1024};
    size_t ReadBatchBytes{1024 * 1024};
    uint32_t MaxIov{16};
    uint64_t TcpWriteMaxBytes{0};
    uint64_t TcpWriteBurstBytes{0};
    uint64_t MaxPendingBufferBytes{256ull * 1024 * 1024};
    uint64_t MaxPendingQuicReceiveBytesPerRelay{0};
    uint64_t DeferredReceiveCompleteBatchBytes{0};
    uint32_t MaxInFlightQuicSends{0};
    uint64_t MaxBufferedQuicSendBytes{0};
    uint32_t TcpKeepaliveIdleSec{10};
    uint32_t TcpKeepaliveIntervalSec{2};
    uint32_t TcpKeepaliveCount{3};
    uint32_t TcpUserTimeoutMs{10000};
    bool UseDynamicQuicReadAhead{false};
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
    bool SinkQuicReceives{false};
    std::atomic<uint64_t>* SinkQuicReceiveBytes{nullptr};
};

struct TqLinuxRelayRegistrationResult {
    bool Ok{false};
    uint64_t RelayId{0};
};

struct TqLinuxRelaySendOperation {
    static constexpr uint64_t MagicValue = 0x5451524c59534e44ULL; // 'TQRLYSND'

    uint64_t Magic{MagicValue};
    uint64_t RelayId{0};
    uint64_t TotalBytes{0};
    bool Fin{false};
    std::vector<TqBufferView> Views;
    std::vector<QUIC_BUFFER> QuicBuffers;
};

struct TqLinuxRelayActiveSnapshot {
    uint32_t WorkerIndex{0};
    uint64_t RelayId{0};
    int TcpFd{-1};
    uint32_t InFlightQuicSends{0};
    uint64_t PendingQuicReceiveBytes{0};
    uint64_t PendingQuicReceiveQueueDepth{0};
    uint64_t CallbackPendingQuicReceiveDepth{0};
    uint64_t OutstandingQuicSendBytes{0};
    uint64_t PendingQuicSendRetries{0};
    uint64_t IdealSendBytes{0};
    uint64_t TcpReadBytes{0};
    uint64_t TcpWriteBytes{0};
    uint64_t TcpWriteEagainCount{0};
    uint64_t EpollOutEvents{0};
    uint64_t PendingTcpWriteQueueDepth{0};
    uint64_t PendingTcpWriteBytes{0};
    uint64_t RelayBufferBytesInUse{0};
    uint64_t LastTcpWriteErrno{0};
    bool Closing{false};
    bool TcpReadClosed{false};
    bool TcpWriteClosed{false};
    bool TcpReadArmed{false};
    bool TcpWriteArmed{false};
    bool TcpReadPausedByQuicBacklog{false};
    bool QuicSendFinSubmitted{false};
    bool QuicSendFinCompleted{false};
    bool StopPublished{false};
    bool StreamDetached{false};
    std::string LocalAddress;
    std::string PeerAddress;
};

struct TqLinuxRelayWorkerSnapshot {
    uint32_t WorkerIndex{0};
    uint64_t EventsProcessed{0};
    uint64_t WakeupWrites{0};
    uint64_t PendingEvents{0};
    uint64_t PendingBytes{0};
    uint64_t RelayBufferBytesInUse{0};
    uint64_t ActiveRelays{0};
    uint64_t ActiveTcpRelays{0};
    uint64_t ActiveSinkRelays{0};
    uint64_t ActiveQuicSendRelays{0};
    uint64_t CurrentPendingQuicReceiveBytes{0};
    uint64_t CurrentPendingQuicReceiveQueue{0};
    uint64_t TcpReadArmedRelays{0};
    uint64_t TcpReadDisabledRelays{0};
    uint64_t TcpWriteArmedRelays{0};
    uint64_t ClosingRelays{0};
    uint64_t TcpReadClosedRelays{0};
    uint64_t TcpWriteShutdownQueuedRelays{0};
    uint64_t OutstandingQuicSends{0};
    uint64_t OutstandingQuicSendBytes{0};
    uint64_t MaxBufferedQuicSendBytes{0};
    uint64_t PendingTcpWriteQueue{0};
    uint64_t PendingTcpWriteBytes{0};
    uint64_t MaxWorkerPendingBytes{0};
    uint64_t MaxWorkerActiveRelays{0};
    uint64_t MaxRelayPendingQuicReceiveBytes{0};
    uint64_t MaxRelayPendingQuicReceiveQueue{0};
    uint64_t MaxRelayTcpWriteEagainCount{0};
    uint64_t HotRelayId{0};
    uint32_t HotRelayWorkerIndex{0};
    int HotRelayTcpFd{-1};
    uint64_t HotRelayPendingQuicReceiveBytes{0};
    uint64_t HotRelayPendingQuicReceiveQueue{0};
    uint64_t HotRelayTcpWriteBytes{0};
    uint64_t HotRelayTcpReadBytes{0};
    uint64_t HotRelayOutstandingQuicSends{0};
    uint64_t HotRelayOutstandingQuicSendBytes{0};
    uint64_t HotRelayPendingQuicSendRetries{0};
    uint64_t HotRelayIdealSendBytes{0};
    uint64_t HotRelayTcpWriteEagainCount{0};
    uint64_t HotRelayEpollOutEvents{0};
    bool HotRelayTcpReadArmed{false};
    bool HotRelayTcpWriteArmed{false};
    std::string HotRelayLocalAddress;
    std::string HotRelayPeerAddress;
    uint64_t SnapshotActiveRelaysScanned{0};
    std::vector<TqLinuxRelayActiveSnapshot> ActiveRelayStates;
    uint64_t TcpReadBatches{0};
    uint64_t TcpReadBytes{0};
    uint64_t QuicSendOperations{0};
    uint64_t MaxTcpReadIovUsed{0};
    uint64_t TcpWriteBatches{0};
    uint64_t TcpWriteBytes{0};
    uint64_t MaxTcpWriteIovUsed{0};
    uint64_t TcpWriteSendmsgCalls{0};
    uint64_t TcpWriteAttemptBytes{0};
    uint64_t MaxTcpWriteAttemptBytes{0};
    uint64_t MaxTcpWriteSendmsgBytes{0};
    uint64_t TcpWriteAttemptBytesLe64K{0};
    uint64_t TcpWriteAttemptBytesLe256K{0};
    uint64_t TcpWriteAttemptBytesLe1M{0};
    uint64_t TcpWriteAttemptBytesLe4M{0};
    uint64_t TcpWriteAttemptBytesGt4M{0};
    uint64_t TcpWriteReturnedBytesLe64K{0};
    uint64_t TcpWriteReturnedBytesLe256K{0};
    uint64_t TcpWriteReturnedBytesLe1M{0};
    uint64_t TcpWriteReturnedBytesLe4M{0};
    uint64_t TcpWriteReturnedBytesGt4M{0};
    uint64_t TcpWriteEagainCount{0};
    uint64_t TcpWritePartialCount{0};
    uint64_t TcpWriteBurstStops{0};
    uint64_t ReadDisabledCount{0};
    uint64_t BufferAcquireCount{0};
    uint64_t StreamLookupScanCount{0};
    uint64_t CompressedTcpBytes{0};
    uint64_t DecompressedTcpBytes{0};
    uint64_t ZstdDecompressInputBytes{0};
    uint64_t ZstdDecompressOutputBytes{0};
    uint64_t ZstdDecompressCalls{0};
    uint64_t ZstdDecompressNeedInput{0};
    uint64_t ZstdDecompressNeedOutput{0};
    uint64_t ZstdDecompressFailures{0};
    uint64_t DeferredReceiveCompleteBytes{0};
    uint64_t DeferredReceiveCompletes{0};
    uint64_t DeferredReceiveCompletionFlushes{0};
    uint64_t MaxPendingQuicReceiveBytes{0};
    uint64_t MaxPendingQuicReceiveQueue{0};
    uint64_t QuicReceiveViewCount{0};
    uint64_t QuicReceiveViewBytes{0};
    uint64_t MaxQuicReceiveViewBytes{0};
    uint64_t MaxQuicReceiveViewSlices{0};
    uint64_t QuicReceiveViewBytesLe64K{0};
    uint64_t QuicReceiveViewBytesLe256K{0};
    uint64_t QuicReceiveViewBytesLe1M{0};
    uint64_t QuicReceiveViewBytesLe4M{0};
    uint64_t QuicReceiveViewBytesGt4M{0};
    uint64_t QuicReceiveViewSlices1{0};
    uint64_t QuicReceiveViewSlices2To4{0};
    uint64_t QuicReceiveViewSlices5To16{0};
    uint64_t QuicReceiveViewSlicesGt16{0};
    uint64_t QuicReceivePausedCount{0};
    uint64_t QuicReceiveResumedCount{0};
    uint64_t QuicReceiveViewBackpressureQueued{0};
    uint64_t Errors{0};
    uint64_t EventQueueFullErrors{0};
    uint64_t TcpReadBufferAcquireFailures{0};
    uint64_t TcpReadBufferAcquirePendingBudgetFailures{0};
    uint64_t TcpReadBufferAcquireAllocFailures{0};
    uint64_t TcpToQuicCompressFailures{0};
    uint64_t TcpToQuicCompressUpdateFailures{0};
    uint64_t TcpToQuicCompressFlushFailures{0};
    uint64_t TcpToQuicBufferAcquireFailures{0};
    uint64_t TcpToQuicBufferAcquirePendingBudgetFailures{0};
    uint64_t TcpToQuicBufferAcquireAllocFailures{0};
    uint64_t QuicSendFailures{0};
    uint64_t QuicSendBufferTooLargeFailures{0};
    uint64_t QuicSendOperationAllocFailures{0};
    uint64_t QuicSendApiFailures{0};
    uint64_t QuicSendBackpressureEvents{0};
    uint64_t QuicSendFatalErrors{0};
    uint64_t QuicReceiveViewFailures{0};
    uint64_t QuicReceiveViewAllocFailures{0};
    uint64_t QuicReceiveViewNullBufferFailures{0};
    uint64_t QuicReceiveViewEmptyFailures{0};
    uint64_t QuicReceiveViewEnqueueFailures{0};
    uint64_t QuicReceiveDecompressFailures{0};
    uint64_t QuicReceiveTcpBufferAcquireFailures{0};
    uint64_t QuicReceiveTcpBufferAcquirePendingBudgetFailures{0};
    uint64_t QuicReceiveTcpBufferAcquireAllocFailures{0};
    uint64_t TcpWriteHardErrors{0};
    uint64_t LastTcpWriteErrno{0};
    uint64_t TcpReadHardErrors{0};
    uint64_t LastTcpReadErrno{0};
    uint64_t FatalRelayResets{0};
    int64_t LastQuicSendStatus{0};
    uint64_t EventProducerThreadsObserved{0};
    bool MultipleEventProducerThreadsObserved{false};
};

#if defined(TQ_UNIT_TESTING)
using TqLinuxRelayStreamSendForTest = QUIC_STATUS (*)(
    MsQuicStream* stream,
    const QUIC_BUFFER* buffers,
    uint32_t bufferCount,
    QUIC_SEND_FLAGS flags,
    void* context);

void TqLinuxRelaySetStreamSendForTest(TqLinuxRelayStreamSendForTest sendFn);
#endif

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
    bool FlushTcpWritableForTest(int tcpFd);
    bool DispatchTcpEventsForTest(uint64_t relayId, uint32_t events);
    QUIC_STATUS DispatchStreamEventForTest(MsQuicStream* stream, QUIC_STREAM_EVENT* event);
#if defined(TQ_UNIT_TESTING)
    bool RelayIndexesConsistentForTest() const;
#endif

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
        QuicReceiveView,
        QuicReceiveDecompress,
        QuicReceiveTcpBufferAcquire,
        TcpWriteHard,
        TcpReadHard
    };

    struct RelayState;
    struct StreamRelayBinding;
    struct RegisterRelayCommand {
        TqLinuxRelayRegistration Registration;
        TqLinuxRelayRegistrationResult Result;
        std::mutex Mutex;
        std::condition_variable Cv;
        bool Done{false};
    };

    struct UnregisterRelayCommand {
        uint64_t RelayId{0};
        std::mutex Mutex;
        std::condition_variable Cv;
        bool Done{false};
    };

    struct SnapshotCommand {
        TqLinuxRelayWorkerSnapshot Result;
        std::mutex Mutex;
        std::condition_variable Cv;
        bool Done{false};
    };

    struct TakeCapturedQuicBytesForTestCommand {
        int TcpFd{-1};
        std::vector<uint8_t> Result;
        std::mutex Mutex;
        std::condition_variable Cv;
        bool Done{false};
    };

    struct EnqueueQuicReceiveForTestCommand {
        int TcpFd{-1};
        std::vector<uint8_t> Data;
        bool Fin{false};
        bool Result{false};
        std::mutex Mutex;
        std::condition_variable Cv;
        bool Done{false};
    };

    struct FlushTcpWritableForTestCommand {
        int TcpFd{-1};
        bool Result{false};
        std::mutex Mutex;
        std::condition_variable Cv;
        bool Done{false};
    };

    struct RelayIndexesConsistentForTestCommand {
        bool Result{false};
        std::mutex Mutex;
        std::condition_variable Cv;
        bool Done{false};
    };

    struct DispatchTcpEventsForTestCommand {
        uint64_t RelayId{0};
        uint32_t Events{0};
        bool Result{false};
        std::mutex Mutex;
        std::condition_variable Cv;
        bool Done{false};
    };

    bool IsWorkerThread() const;
    TqLinuxRelayRegistrationResult RegisterRelayWithIdLocal(
        const TqLinuxRelayRegistration& registration);
    void UnregisterRelayLocal(uint64_t relayId);
    TqLinuxRelayWorkerSnapshot SnapshotLocal() const;
    bool RelayIndexesConsistentForTestLocal() const;
    std::vector<uint8_t> TakeCapturedQuicBytesForTestLocal(int tcpFd);
    bool EnqueueQuicReceiveForTestLocal(
        int tcpFd,
        const uint8_t* data,
        size_t length,
        bool fin);
    bool FlushTcpWritableForTestLocal(int tcpFd);
    bool DispatchTcpEventsForTestLocal(uint64_t relayId, uint32_t events);
    void CompleteRegisterCommand(
        RegisterRelayCommand* command,
        TqLinuxRelayRegistrationResult result);
    void CompleteUnregisterCommand(UnregisterRelayCommand* command);
    void CompleteSnapshotCommand(SnapshotCommand* command, TqLinuxRelayWorkerSnapshot snapshot);
    void CompleteTakeCapturedQuicBytesForTestCommand(
        TakeCapturedQuicBytesForTestCommand* command,
        std::vector<uint8_t> result);
    void CompleteEnqueueQuicReceiveForTestCommand(
        EnqueueQuicReceiveForTestCommand* command,
        bool result);
    void CompleteFlushTcpWritableForTestCommand(
        FlushTcpWritableForTestCommand* command,
        bool result);
    void CompleteRelayIndexesConsistentForTestCommand(
        RelayIndexesConsistentForTestCommand* command,
        bool result);
    void CompleteDispatchTcpEventsForTestCommand(
        DispatchTcpEventsForTestCommand* command,
        bool result);
    void RecordError(RelayErrorKind kind);
    void RecordBufferAcquireFailure(RelayErrorKind kind, TqBufferAcquireFailure failure);
    void RecordTcpWriteAttempt(uint64_t bytes);
    void RecordTcpWriteReturned(uint64_t bytes);
    void RecordQuicReceiveView(uint64_t bytes, uint64_t slices);
    void Wake();
    void RecordEventProducer();
    size_t DrainEvents(size_t budget);
    void TraceRelayStreamEvent(
        RelayState* relay,
        const char* streamEvent,
        uint64_t errorCode,
        uint32_t status,
        uint64_t absoluteOffset,
        uint64_t totalBufferLength,
        uint32_t bufferCount,
        uint32_t receiveFlags,
        bool fin);
    void SetRelayStop(RelayState* relay, const char* trigger);
    void AbortRelayAndRelease(RelayState* relay, const char* trigger, bool abortStream);
    void DetachRelayStreamBinding(
        RelayState* relay,
        MsQuicStream* stream,
        StreamRelayBinding* binding);
    bool HasPendingAfterStreamShutdown(RelayState* relay) const;
    void CompleteAndDiscardQuicReceive(TqPendingQuicReceive& view);
    void FailRelayFatal(RelayState* relay, const char* trigger, bool abortStream);
    uint64_t CurrentMaxBufferedQuicSendBytes() const;
    uint64_t CurrentResumeBufferedQuicSendBytes() const;
    uint64_t CurrentRelayIdealSendBytes(const RelayState* relay) const;
    void HandleQuicIdealSendBuffer(uint64_t relayId, uint64_t byteCount);
    bool ShouldPauseTcpReadForQuicBacklog(const RelayState* relay) const;
    bool ShouldResumeTcpReadForQuicBacklog(const RelayState* relay) const;
    void SetTcpReadBackpressure(RelayState* relay, bool paused, const char* reason);
    void DrainTcpReadable(RelayState* relay);
    bool BuildTcpToQuicViews(
        RelayState* relay,
        std::vector<TqBufferView>& input,
        std::vector<TqBufferView>& output);
    bool FinishTcpToQuic(RelayState* relay);
    void MaybeStopFullyClosedRelay(RelayState* relay, const char* trigger);
    bool SubmitTcpBatchToQuic(
        RelayState* relay,
        std::vector<TqBufferView>& views,
        QUIC_SEND_FLAGS flags = QUIC_SEND_FLAG_NONE);
    bool TrySubmitQuicSendOperation(RelayState* relay, TqLinuxRelaySendOperation* operation);
    void RetryPendingQuicSends(RelayState* relay);
    bool IsQuicSendBackpressureStatus(QUIC_STATUS status) const;
    void CompleteQuicSend(void* context);
    std::shared_ptr<RelayState> FindRelayById(uint64_t relayId);
    std::shared_ptr<RelayState> FindRelayByFd(int tcpFd);
    uint64_t FindRelayIdByStream(MsQuicStream* stream);
    bool QueueDeferredQuicReceive(
        RelayState* relay,
        MsQuicStream* stream,
        const QUIC_BUFFER* buffers,
        uint32_t bufferCount,
        bool fin);
    void ProcessQuicReceiveViewEvent(TqLinuxRelayEvent& event);
    void DrainCallbackPendingQuicReceives(RelayState* relay);
    void FlushDeferredQuicReceives(RelayState* relay);
    bool DrainCompressedQuicReceiveView(RelayState* relay, TqPendingQuicReceive& view);
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
    void AbortRelayFromCallback(
        uint64_t relayId,
        StreamRelayBinding* binding,
        MsQuicStream* stream);
    void ProcessQuicPeerSendAborted(uint64_t relayId, uint64_t errorCode);
    void ProcessQuicPeerReceiveAborted(uint64_t relayId, uint64_t errorCode);
    void ProcessQuicShutdownComplete(uint64_t relayId, uint64_t errorCode, uint32_t status);
    void ProcessQuicReceiveEvent(TqLinuxRelayEvent& event);
    void PurgeRetiredRelaysIfIdle();
    bool EnqueueQuicReceive(RelayState* relay, const uint8_t* data, size_t length, bool fin);
    void FlushTcpWrites(RelayState* relay);
    void UpdateTcpInterest(RelayState* relay);
    void ArmTcpWritable(RelayState* relay, bool enabled);
    void ProcessTcpEvents(uint64_t relayId, uint32_t events);
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
    mutable std::mutex ControlLock;
    std::thread::id WorkerThreadId;
    std::atomic<uint64_t> EventsProcessed{0};
    std::atomic<uint64_t> WakeupWrites{0};
    std::deque<std::shared_ptr<RelayState>> Relays;
    std::deque<std::shared_ptr<RelayState>> RetiredRelays;
    std::unordered_map<uint64_t, std::shared_ptr<RelayState>> RelaysById;
    std::unordered_map<uint64_t, std::shared_ptr<RelayState>> RetiredRelaysById;
    uint64_t NextRelayId{1};
    std::atomic<uint64_t> TcpReadBatches{0};
    std::atomic<uint64_t> TcpReadBytes{0};
    std::atomic<uint64_t> QuicSendOperations{0};
    std::atomic<uint64_t> MaxTcpReadIovUsed{0};
    std::atomic<uint64_t> TcpWriteBatches{0};
    std::atomic<uint64_t> TcpWriteBytes{0};
    std::atomic<uint64_t> MaxTcpWriteIovUsed{0};
    std::atomic<uint64_t> TcpWriteSendmsgCalls{0};
    std::atomic<uint64_t> TcpWriteAttemptBytes{0};
    std::atomic<uint64_t> MaxTcpWriteAttemptBytes{0};
    std::atomic<uint64_t> MaxTcpWriteSendmsgBytes{0};
    std::atomic<uint64_t> TcpWriteAttemptBytesLe64K{0};
    std::atomic<uint64_t> TcpWriteAttemptBytesLe256K{0};
    std::atomic<uint64_t> TcpWriteAttemptBytesLe1M{0};
    std::atomic<uint64_t> TcpWriteAttemptBytesLe4M{0};
    std::atomic<uint64_t> TcpWriteAttemptBytesGt4M{0};
    std::atomic<uint64_t> TcpWriteReturnedBytesLe64K{0};
    std::atomic<uint64_t> TcpWriteReturnedBytesLe256K{0};
    std::atomic<uint64_t> TcpWriteReturnedBytesLe1M{0};
    std::atomic<uint64_t> TcpWriteReturnedBytesLe4M{0};
    std::atomic<uint64_t> TcpWriteReturnedBytesGt4M{0};
    std::atomic<uint64_t> TcpWriteEagainCount{0};
    std::atomic<uint64_t> TcpWritePartialCount{0};
    std::atomic<uint64_t> TcpWriteBurstStops{0};
    std::atomic<uint64_t> ReadDisabledCount{0};
    std::atomic<uint64_t> StreamLookupScanCount{0};
    std::atomic<uint64_t> CompressedTcpBytes{0};
    std::atomic<uint64_t> DecompressedTcpBytes{0};
    std::atomic<uint64_t> ZstdDecompressInputBytes{0};
    std::atomic<uint64_t> ZstdDecompressOutputBytes{0};
    std::atomic<uint64_t> ZstdDecompressCalls{0};
    std::atomic<uint64_t> ZstdDecompressNeedInput{0};
    std::atomic<uint64_t> ZstdDecompressNeedOutput{0};
    std::atomic<uint64_t> ZstdDecompressFailures{0};
    std::atomic<uint64_t> DeferredReceiveCompleteBytes{0};
    std::atomic<uint64_t> DeferredReceiveCompletes{0};
    std::atomic<uint64_t> DeferredReceiveCompletionFlushes{0};
    std::atomic<uint64_t> MaxPendingQuicReceiveBytesObserved{0};
    std::atomic<uint64_t> MaxPendingQuicReceiveQueueObserved{0};
    std::atomic<uint64_t> QuicReceiveViewCount{0};
    std::atomic<uint64_t> QuicReceiveViewBytes{0};
    std::atomic<uint64_t> MaxQuicReceiveViewBytes{0};
    std::atomic<uint64_t> MaxQuicReceiveViewSlices{0};
    std::atomic<uint64_t> QuicReceiveViewBytesLe64K{0};
    std::atomic<uint64_t> QuicReceiveViewBytesLe256K{0};
    std::atomic<uint64_t> QuicReceiveViewBytesLe1M{0};
    std::atomic<uint64_t> QuicReceiveViewBytesLe4M{0};
    std::atomic<uint64_t> QuicReceiveViewBytesGt4M{0};
    std::atomic<uint64_t> QuicReceiveViewSlices1{0};
    std::atomic<uint64_t> QuicReceiveViewSlices2To4{0};
    std::atomic<uint64_t> QuicReceiveViewSlices5To16{0};
    std::atomic<uint64_t> QuicReceiveViewSlicesGt16{0};
    std::atomic<uint64_t> QuicReceivePausedCount{0};
    std::atomic<uint64_t> QuicReceiveResumedCount{0};
    std::atomic<uint64_t> QuicReceiveViewBackpressureQueued{0};
    std::atomic<uint64_t> Errors{0};
    std::atomic<uint64_t> EventQueueFullErrors{0};
    std::atomic<uint64_t> TcpReadBufferAcquireFailures{0};
    std::atomic<uint64_t> TcpReadBufferAcquirePendingBudgetFailures{0};
    std::atomic<uint64_t> TcpReadBufferAcquireAllocFailures{0};
    std::atomic<uint64_t> TcpToQuicCompressFailures{0};
    std::atomic<uint64_t> TcpToQuicCompressUpdateFailures{0};
    std::atomic<uint64_t> TcpToQuicCompressFlushFailures{0};
    std::atomic<uint64_t> TcpToQuicBufferAcquireFailures{0};
    std::atomic<uint64_t> TcpToQuicBufferAcquirePendingBudgetFailures{0};
    std::atomic<uint64_t> TcpToQuicBufferAcquireAllocFailures{0};
    std::atomic<uint64_t> QuicSendFailures{0};
    std::atomic<uint64_t> QuicSendBufferTooLargeFailures{0};
    std::atomic<uint64_t> QuicSendOperationAllocFailures{0};
    std::atomic<uint64_t> QuicSendApiFailures{0};
    std::atomic<uint64_t> QuicSendBackpressureEvents{0};
    std::atomic<uint64_t> QuicSendFatalErrors{0};
    std::atomic<uint64_t> QuicReceiveViewFailures{0};
    std::atomic<uint64_t> QuicReceiveViewAllocFailures{0};
    std::atomic<uint64_t> QuicReceiveViewNullBufferFailures{0};
    std::atomic<uint64_t> QuicReceiveViewEmptyFailures{0};
    std::atomic<uint64_t> QuicReceiveViewEnqueueFailures{0};
    std::atomic<uint64_t> QuicReceiveDecompressFailures{0};
    std::atomic<uint64_t> QuicReceiveTcpBufferAcquireFailures{0};
    std::atomic<uint64_t> QuicReceiveTcpBufferAcquirePendingBudgetFailures{0};
    std::atomic<uint64_t> QuicReceiveTcpBufferAcquireAllocFailures{0};
    std::atomic<uint64_t> TcpWriteHardErrors{0};
    std::atomic<uint64_t> LastTcpWriteErrno{0};
    std::atomic<uint64_t> TcpReadHardErrors{0};
    std::atomic<uint64_t> LastTcpReadErrno{0};
    std::atomic<uint64_t> FatalRelayResets{0};
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
    std::vector<TqLinuxRelayWorkerSnapshot> SnapshotWorkers() const;

private:
    TqLinuxRelayRuntime() = default;
    mutable std::mutex Lock;
    std::vector<std::unique_ptr<TqLinuxRelayWorker>> Workers;
    size_t NextWorker{0};
};
