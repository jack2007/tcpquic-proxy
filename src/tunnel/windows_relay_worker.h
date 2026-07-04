#pragma once

#include "compress.h"
#include "msquic.hpp"
#include "platform_socket.h"
#include "relay.h"
#include "relay_error.h"
#include "trace.h"
#include "tuning.h"

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

struct MsQuicStream;
struct QUIC_STREAM_EVENT;
struct TqWindowsPendingQuicReceive;
struct TqWindowsQuicSendOperation;

enum class TqWindowsIocpOperationType : uint32_t {
    TcpRecv,
    TcpSend,
    QuicSendComplete,
    QuicSendRetry,
    CloseRelay,
    SetTraceContext,
    StopWorker,
    RegisterRelay,
    Snapshot,
    RelayReceiveReady,
    RelayReceiveDrain,
    QuicIdealSendBuffer,
    QuicPeerSendAborted,
    QuicPeerReceiveAborted,
    QuicShutdownComplete,
#if defined(TQ_UNIT_TESTING)
    TestBlockWorkerQueue,
#endif
};

struct TqWindowsRelayActiveSnapshot {
    uint32_t WorkerIndex{0};
    uint64_t RelayId{0};
    uint32_t ActiveHandlers{0};
    uint32_t QueuedWorkerOps{0};
    uint32_t InFlightTcpRecvs{0};
    uint32_t InFlightTcpSends{0};
    uint32_t InFlightQuicSends{0};
    uint64_t PendingQuicReceiveBytes{0};
    uint64_t PendingQuicReceiveQueueDepth{0};
    uint64_t CallbackPendingQuicReceiveDepth{0};
    uint64_t OutstandingQuicSendBytes{0};
    uint64_t MaxOutstandingQuicSendBytes{0};
    uint64_t TcpReadBytes{0};
    uint64_t TcpWriteBytes{0};
    uint64_t LastTcpWriteErrno{0};
    uint64_t LastTcpRecvErrno{0};
    uint64_t LastTcpSendErrno{0};
    uint64_t LastIocpCompletionErrno{0};
    uint32_t LastIocpOperation{0};
    bool Closing{false};
    bool TcpReadClosed{false};
    bool TcpReadPausedByQuicBacklog{false};
    bool TcpWriteClosed{false};
    bool CloseAfterDrained{false};
    bool QuicSendFinSubmitted{false};
    bool QuicSendFinCompleted{false};
    bool StopPublished{false};
    bool StreamDetached{false};
};

struct TqWindowsRelayWorkerSnapshot {
    uint64_t DeferredReceiveQueued{0};
    uint64_t DeferredReceiveBytesQueued{0};
    uint64_t DeferredReceiveCompleteBytes{0};
    uint64_t DeferredReceiveCompletes{0};
    uint64_t DeferredReceiveCompletionFlushes{0};
    uint64_t PendingQuicReceiveBytes{0};
    uint64_t MaxPendingQuicReceiveBytes{0};
    uint64_t PendingQuicReceiveQueueDepth{0};
    uint64_t MaxPendingQuicReceiveQueueDepth{0};
    uint64_t QuicReceivePausedCount{0};
    uint64_t QuicReceiveResumedCount{0};
    uint64_t TcpSendOperationsPosted{0};
    uint64_t TcpSendBytes{0};
    uint64_t TcpSendPartialCompletions{0};
    uint64_t TcpSendWouldBlockOrPendingCount{0};
    uint64_t TcpRecvOperationsCreated{0};
    uint64_t TcpRecvOperationsReused{0};
    uint64_t RelayBufferBytesInUse{0};
    uint64_t RelayBufferAllocateCount{0};
    uint64_t ZstdDecompressInputBytes{0};
    uint64_t ZstdDecompressOutputBytes{0};
    uint64_t ZstdDecompressCalls{0};
    uint64_t ZstdDecompressNeedInput{0};
    uint64_t ZstdDecompressNeedOutput{0};
    uint64_t ZstdDecompressFailures{0};
    uint64_t FatalRelayResets{0};
    uint64_t GracefulRelayDrains{0};
    uint64_t TcpHardErrors{0};
    uint64_t QuicSendBackpressureEvents{0};
    uint64_t QuicSendFatalErrors{0};
    uint64_t QuicSendCompleteEvents{0};
    uint64_t WindowsCallbackIocpPostCount{0};
    uint64_t WindowsCallbackIocpPostFailedCount{0};
    uint64_t WindowsReceiveReadyPostCount{0};
    uint64_t WindowsReceiveDrainScheduledCount{0};
    uint64_t WindowsReceiveDrainCoalescedCount{0};
    uint64_t WindowsPostedCallbackStaleDropCount{0};
    uint64_t WindowsPostedTraceContextStaleDropCount{0};
    uint64_t WorkerLockAcquireCount{0};
    uint64_t WorkerLockWaitNanos{0};
    uint64_t FindRelayByIdCount{0};
    uint64_t CallbackDispatchNanos{0};
    uint64_t SnapshotBuildNanos{0};
    uint64_t SnapshotActiveRelaysScanned{0};
    uint64_t MaintenanceDrainCount{0};
    uint64_t MaintenanceDrainNanos{0};
    uint64_t MaintenanceRelaysProcessed{0};
    uint64_t MaintenanceFullScanCount{0};
    uint64_t MaintenanceFullScanRelaysScanned{0};
    uint64_t ReceiveViewFinishLinearSearchCount{0};
    uint64_t ReceiveViewFinishLinearSearchNanos{0};
    uint64_t ReceiveViewFinishNotFrontCount{0};
    uint64_t EventsProcessed{0};
    uint64_t TcpReadResumeByBacklogEvents{0};
    uint64_t LateTeardownDowngradedCount{0};
#if defined(TQ_UNIT_TESTING)
    uint64_t PostTcpRecvFromSendCompleteCallbackCount{0};
#endif
    uint64_t Errors{0};
    uint64_t ActiveRelays{0};
    uint64_t IocpCompletionDowngraded{0};
    uint64_t IocpStaleCompletionDropped{0};
    uint64_t TcpSendZeroBytesGraceful{0};
    std::vector<TqWindowsRelayActiveSnapshot> ActiveRelayStates;
};

#if defined(TQ_UNIT_TESTING)
struct TqWindowsRelayWorkerQueueBlockForTest {
    std::mutex Mutex;
    std::condition_variable Cv;
    bool Entered{false};
    bool Release{false};
};
#endif

class TqWindowsRelayWorker {
public:
    explicit TqWindowsRelayWorker(uint32_t workerIndex = 0);
    ~TqWindowsRelayWorker();

    bool Start();
    void Stop();
    TqWindowsRelayWorkerSnapshot Snapshot() const;

    bool RegisterRelay(
        TqSocketHandle tcpFd,
        MsQuicStream* stream,
        ITqCompressor* compressor,
        ITqDecompressor* decompressor,
        TqRelayHandle* handle,
        const TqTuningConfig& tuning,
        TqCompressAlgo compressAlgo);

#if defined(TQ_UNIT_TESTING)
    bool RegisterRelayForTest(
        MsQuicStream* stream,
        TqRelayHandle* handle,
        const TqTuningConfig& tuning,
        TqCompressAlgo compressAlgo);
    void SetQuicReceiveViewDrainEnabledForTest(bool enabled);
    bool TestLastPostedCallbackWasReceiveReadyForTest(uint64_t relayId) const;
    bool TestLastPostedCallbackWasQuicSendCompleteForTest(uint64_t relayId) const;
    bool TestPostedCallbackSequenceForTest(const char* expectedCsv) const;
    TqWindowsQuicSendOperation* TestCreateQuicSendOperationForTest(uint64_t relayId, uint64_t bytes);
    bool TestResolveStaleCallbackForTest(uint64_t relayId);
    bool TestDispatchIdealSendBufferByIdForTest(uint64_t relayId, uint64_t byteCount);
    bool TestHasIocpForTest() const { return Iocp_ != nullptr; }
    DWORD TestLastCallbackPostWin32ErrorForTest() const {
        return LastCallbackPostWin32Error_.load(std::memory_order_relaxed);
    }
    bool TestCreateIocpForCallbackPostOnly();
    bool TestDrainSingleQuicSendCompleteForTest();
    bool TestDrainSingleReceiveReadyForTest();
    bool TestDrainPostedCallbackOperationsForTest(size_t expectedCount);
    bool TestNoWorkerEventQueueReceiveViewForTest() const;
    bool TestCompleteReceiveViewForCleanup(uint64_t relayId, uint64_t completedLength);
    bool TestEnqueueReceiveViewForTest(uint64_t relayId, uint64_t byteCount);
    bool TestCompleteSecondReceiveViewForTest(uint64_t relayId, uint64_t completedLength);
    bool TestAdvanceReceiveViewForCompletion(uint64_t relayId, uint64_t completedLength);
    bool TestLateReceiveViewCompletionIgnored(uint64_t relayId, DWORD bytes, uint64_t postedLength);
    bool TestBufferedTcpSendZeroCompletion(uint64_t relayId);
    bool TestCloseRelayTcpSocketForPostRecvFailure(uint64_t relayId);
    bool TestMarkTcpRecvInFlightForRetirement(uint64_t relayId);
    bool TestCompleteTcpRecvInFlightForRetirement(uint64_t relayId);
    bool TestMarkQuicSendInFlightForRetirement(uint64_t relayId);
    bool TestMarkTcpSendInFlightForTest(uint64_t relayId);
    bool TestRecordIocpCompletionErrorForTest(uint64_t relayId, bool tcpSend, DWORD error);
    bool TestHandleTcpPostFailureForTest(uint64_t relayId, int error);
    bool TestGetRelayDrainFlagsForTest(
        uint64_t relayId,
        bool* closeAfterDrained,
        bool* tcpRecvClosed) const;
    bool TestGetRelayTraceStateForTest(
        uint64_t relayId,
        TqTraceLinuxRelayStreamState* out,
        std::string* targetStorage) const;
    bool TestPostTraceContextForTest(
        uint64_t relayId,
        uint64_t generation,
        uint64_t tunnelId,
        const char* target);
    void TestForceNextTraceContextPostFailureForTest();
    bool TestPostWorkerQueueBlockForTest(TqWindowsRelayWorkerQueueBlockForTest* block);
    bool TestWaitWorkerQueueBlockEnteredForTest(
        TqWindowsRelayWorkerQueueBlockForTest& block,
        uint32_t timeoutMs) const;
    void TestReleaseWorkerQueueBlockForTest(TqWindowsRelayWorkerQueueBlockForTest& block) const;
    bool TestArmRelayClosingForLateDiscard(uint64_t relayId);
    bool TestCloseRelayAfterTcpHalfCloseDrain(uint64_t relayId);
    bool MaybePostTcpRecvForTest(uint64_t relayId);
    bool TestGetTcpReadPausedByQuicBacklog(uint64_t relayId) const;
    void TestConfigureQuicSendBacklog(uint64_t relayId, uint64_t maxBufferedBytes, uint64_t outstandingBytes);
    void TestProcessQuicSendCompleteForTest(uint64_t relayId, uint64_t completedBytes);
    void SetMaintenanceBudgetForTest(size_t budget);
    bool TestScheduleMaintenanceForTest(uint64_t relayId);
    void TestDrainMaintenanceForTest();
#endif

    void StopRelay(uint64_t relayId);
    void SetRelayTraceContext(uint64_t relayId, uint64_t tunnelId, const char* target);
    static QUIC_STATUS QUIC_API StreamCallback(
        MsQuicStream* stream,
        void* context,
        QUIC_STREAM_EVENT* event) noexcept;

private:
    struct RelayContext;
    struct IoOperation;
    struct CallbackBinding;
    struct RegisterRelayCommand;
    struct SnapshotCommand;

    void Run();
    void PostStop();
    bool RegisterRelayLocal(RegisterRelayCommand& command);
    TqWindowsRelayWorkerSnapshot BuildSnapshotLocal() const;
    void DrainPerRelayMaintenance();
    void ScheduleRelayMaintenance(const std::shared_ptr<RelayContext>& relay);
    bool RelayNeedsMaintenance(const std::shared_ptr<RelayContext>& relay) const;
    void EnqueueFullMaintenanceScan();
    void ProcessQuicSendCompleteOperation(
        uint64_t relayId,
        uint64_t generation,
        uintptr_t operationValue);
    void ProcessQuicPeerAborted(uint64_t relayId, const char* reason, uint64_t errorCode);
    void ProcessQuicPeerAborted(
        const std::shared_ptr<RelayContext>& relay,
        const char* reason,
        uint64_t errorCode);
    void ProcessQuicShutdownComplete(uint64_t relayId, uint64_t errorCode, uint32_t status);
    void ProcessQuicShutdownComplete(
        const std::shared_ptr<RelayContext>& relay,
        uint64_t errorCode,
        uint32_t status);
    void HandleQuicIdealSendBuffer(uint64_t relayId, uint64_t byteCount);
    void HandleQuicIdealSendBuffer(const std::shared_ptr<RelayContext>& relay, uint64_t byteCount);
    void ApplyRelayTraceContext(
        const std::shared_ptr<RelayContext>& relay,
        uint64_t tunnelId,
        const std::string& target);
    void QueueQuicSendCompleteFromCallback(TqWindowsQuicSendOperation* operation);
    void QueueQuicSendCompleteByIdFromCallback(
        const CallbackBinding& binding,
        TqWindowsQuicSendOperation* operation);
    std::shared_ptr<RelayContext> FindRelayById(uint64_t relayId);
    bool PostCallbackOperation(
        TqWindowsIocpOperationType type,
        const std::shared_ptr<RelayContext>& relay,
        uint64_t value = 0,
        size_t length = 0);
    bool PostCallbackOperationById(
        TqWindowsIocpOperationType type,
        const CallbackBinding& binding,
        uint64_t value = 0,
        size_t length = 0,
        std::shared_ptr<TqWindowsPendingQuicReceive> receiveView = nullptr);
    std::shared_ptr<TqWindowsPendingQuicReceive> BuildDeferredQuicReceiveView(
        const CallbackBinding& binding,
        MsQuicStream* stream,
        const QUIC_BUFFER* buffers,
        uint32_t bufferCount,
        bool fin,
        uint64_t completeBatchBytes);
    bool EnqueueDeferredQuicReceiveView(
        const std::shared_ptr<RelayContext>& relay,
        const std::shared_ptr<TqWindowsPendingQuicReceive>& view);
    std::shared_ptr<RelayContext> ResolveRelayForCallback(uint64_t relayId, uint64_t generation);
    std::shared_ptr<RelayContext> ResolveRelayForSendComplete(uint64_t relayId, uint64_t generation);
    std::shared_ptr<RelayContext> FindRelayByIdLocal(uint64_t relayId) const;

    bool PostTcpRecv(const std::shared_ptr<RelayContext>& relay);
    uint64_t CurrentRelayIdealSendBytes(const std::shared_ptr<RelayContext>& relay) const;
    bool ShouldPauseTcpReadForQuicBacklog(const std::shared_ptr<RelayContext>& relay) const;
    bool ShouldResumeTcpReadForQuicBacklog(const std::shared_ptr<RelayContext>& relay) const;
    bool MaybePostTcpRecv(const std::shared_ptr<RelayContext>& relay);
    void SetTcpReadBackpressure(
        const std::shared_ptr<RelayContext>& relay,
        bool paused,
        const char* reason);
    void HandleTcpRecv(std::unique_ptr<IoOperation> op, DWORD bytes);
    bool HandleTcpReadClosed(std::unique_ptr<IoOperation> op);
    void HandleTcpSend(std::unique_ptr<IoOperation> op, DWORD bytes);
    void TryRetireRelay(const std::shared_ptr<RelayContext>& relay, bool traceState = true);
    bool ScheduleRelayReceiveDrain(const std::shared_ptr<RelayContext>& relay);
    void ScheduleRelayReceiveDrainOrFail(const std::shared_ptr<RelayContext>& relay, const char* reason);
    void DrainRelayReceives(const std::shared_ptr<RelayContext>& relay);
    bool QueueDeferredQuicReceive(
        const std::shared_ptr<RelayContext>& relay,
        MsQuicStream* stream,
        const QUIC_BUFFER* buffers,
        uint32_t bufferCount,
        bool fin);
    bool PostTcpSendFromReceiveView(
        const std::shared_ptr<RelayContext>& relay,
        const std::shared_ptr<TqWindowsPendingQuicReceive>& view);
    bool PostTcpSendFromCompressedReceiveView(
        const std::shared_ptr<RelayContext>& relay,
        const std::shared_ptr<TqWindowsPendingQuicReceive>& view);
    void AdvanceReceiveView(
        const std::shared_ptr<RelayContext>& relay,
        TqWindowsPendingQuicReceive& view,
        uint64_t bytes);
    void FlushDeferredReceiveCompletion(TqWindowsPendingQuicReceive& view, bool force);
    void FlushBatchedDeferredReceiveCompletion(
        const std::shared_ptr<RelayContext>& relay,
        MsQuicStream* stream);
    void CompleteRemainingReceiveOwnership(TqWindowsPendingQuicReceive& view);
    void CompleteQuicSendAccounting(
        const std::shared_ptr<RelayContext>& relay,
        const TqWindowsQuicSendOperation& operation);
    bool FinishReceiveView(
        const std::shared_ptr<RelayContext>& relay,
        const std::shared_ptr<TqWindowsPendingQuicReceive>& view);
    bool CompletePendingQuicReceive(
        const std::shared_ptr<RelayContext>& relay,
        const std::shared_ptr<TqWindowsPendingQuicReceive>& view);
    void CompleteAllPendingQuicReceives(const std::shared_ptr<RelayContext>& relay);
    void FinalizeQuicSendAccountingOnClose(const std::shared_ptr<RelayContext>& relay);
    void SetQuicReceiveEnabled(const std::shared_ptr<RelayContext>& relay, bool enabled);
    void MaybeResumeQuicReceive(const std::shared_ptr<RelayContext>& relay);
    void PruneRetiredCallbacks(bool keepNewest);
    TqTraceLinuxRelayStreamState BuildRelayTraceState(const std::shared_ptr<RelayContext>& relay) const;
    void TraceReceiveViewEvent(
        const std::shared_ptr<RelayContext>& relay,
        const std::shared_ptr<TqWindowsPendingQuicReceive>& view,
        const char* stage,
        uint64_t value = 0) const;
    void TraceRelayBackpressure(
        const std::shared_ptr<RelayContext>& relay,
        const char* action,
        const char* reason) const;
    bool PostTcpSend(std::unique_ptr<IoOperation> op);
    bool TrySubmitQuicSendOperation(
        const std::shared_ptr<RelayContext>& relay,
        TqWindowsQuicSendOperation* operation);
    void RetryPendingQuicSends(const std::shared_ptr<RelayContext>& relay);
    void CloseRelay(
        const std::shared_ptr<RelayContext>& relay,
        TqRelayCloseMode mode,
        const char* reason = nullptr,
        bool traceState = true);
    void MarkRelayCloseReason(const std::shared_ptr<RelayContext>& relay, const char* reason);
    bool CloseRelayIfDrained(const std::shared_ptr<RelayContext>& relay);
    bool HasPendingAfterStreamShutdown(const std::shared_ptr<RelayContext>& relay) const;
    void FailRelayFatal(const std::shared_ptr<RelayContext>& relay, const char* reason);
    void FailRelayFatalFromCallback(const std::shared_ptr<RelayContext>& relay, const char* reason);
    void RecordTcpHardErrorAndFail(
        const std::shared_ptr<RelayContext>& relay,
        const char* reason,
        uint64_t tcpError = 0);
    void StoreTcpRecvErrno(const std::shared_ptr<RelayContext>& relay, uint64_t error);
    void StoreTcpSendErrno(const std::shared_ptr<RelayContext>& relay, uint64_t error);
    void StoreIocpCompletionErrno(
        const std::shared_ptr<RelayContext>& relay,
        uint32_t operation,
        uint64_t error);
    bool HandleTcpPostFailure(
        const std::shared_ptr<RelayContext>& relay,
        const char* reason,
        int error);
    bool HasPendingRelayDrainWork(const std::shared_ptr<RelayContext>& relay) const;
    bool IsTcpTeardownError(int error) const;
    bool IsIocpTeardownError(DWORD error) const;
    bool IsQuicSendBackpressureStatus(QUIC_STATUS status) const;
    bool IsQuicSendTeardownStatus(QUIC_STATUS status) const;
    bool ShouldDowngradeTcpRecvCompletion(const RelayContext* relay, DWORD error) const;
    bool ShouldDowngradeTcpSendCompletion(const RelayContext* relay, DWORD error) const;
    bool ShouldDowngradeTcpSendZeroBytes(const RelayContext& relay) const;
    bool ShouldDowngradePostedWorkerCompletion(const RelayContext* relay, DWORD error) const;
    void DowngradeIocpCompletion(
        const std::shared_ptr<RelayContext>& relay,
        const char* reason,
        DWORD completionError = 0);
    void DropStaleCompletionWithoutRelay(DWORD completionError);

    void* Iocp_{nullptr};
    std::thread Thread_;
    std::atomic<bool> Stopping_{false};
    std::atomic<uint64_t> WorkerThreadToken_{0};
    std::atomic<uint64_t> NextRelayId_{1};
    std::atomic<uint64_t> NextGeneration_{1};
    std::atomic<uint64_t> DeferredReceiveQueued_{0};
    std::atomic<uint64_t> DeferredReceiveBytesQueued_{0};
    std::atomic<uint64_t> DeferredReceiveCompleteBytes_{0};
    std::atomic<uint64_t> DeferredReceiveCompletes_{0};
    std::atomic<uint64_t> DeferredReceiveCompletionFlushes_{0};
    std::atomic<uint64_t> MaxPendingQuicReceiveBytesObserved_{0};
    std::atomic<uint64_t> MaxPendingQuicReceiveQueueObserved_{0};
    std::atomic<uint64_t> QuicReceivePausedCount_{0};
    std::atomic<uint64_t> QuicReceiveResumedCount_{0};
    std::atomic<uint64_t> TcpSendOperationsPosted_{0};
    std::atomic<uint64_t> TcpSendBytes_{0};
    std::atomic<uint64_t> TcpSendPartialCompletions_{0};
    std::atomic<uint64_t> TcpSendWouldBlockOrPendingCount_{0};
    std::atomic<uint64_t> TcpRecvOperationsCreated_{0};
    std::atomic<uint64_t> TcpRecvOperationsReused_{0};
    std::atomic<uint64_t> ZstdDecompressInputBytes_{0};
    std::atomic<uint64_t> ZstdDecompressOutputBytes_{0};
    std::atomic<uint64_t> ZstdDecompressCalls_{0};
    std::atomic<uint64_t> ZstdDecompressNeedInput_{0};
    std::atomic<uint64_t> ZstdDecompressNeedOutput_{0};
    std::atomic<uint64_t> ZstdDecompressFailures_{0};
    std::atomic<uint64_t> FatalRelayResets_{0};
    std::atomic<uint64_t> GracefulRelayDrains_{0};
    std::atomic<uint64_t> LateTeardownDowngradedCount_{0};
    std::atomic<uint64_t> TcpHardErrors_{0};
    std::atomic<uint64_t> QuicSendBackpressureEvents_{0};
    std::atomic<uint64_t> QuicSendFatalErrors_{0};
    std::atomic<uint64_t> QuicSendCompleteEvents_{0};
    std::atomic<uint64_t> TcpReadResumeByBacklogEvents_{0};
    std::atomic<uint64_t> Errors_{0};
    std::atomic<uint64_t> IocpCompletionDowngraded_{0};
    std::atomic<uint64_t> IocpStaleCompletionDropped_{0};
    std::atomic<uint64_t> TcpSendZeroBytesGraceful_{0};
    uint32_t WorkerIndex_{0};
    std::atomic<uint64_t> CallbackIocpPostCount_{0};
    std::atomic<uint64_t> CallbackIocpPostFailedCount_{0};
    std::atomic<uint64_t> ReceiveReadyPostCount_{0};
    std::atomic<uint64_t> ReceiveDrainScheduledCount_{0};
    std::atomic<uint64_t> ReceiveDrainCoalescedCount_{0};
    std::atomic<uint64_t> PostedCallbackStaleDropCount_{0};
    std::atomic<uint64_t> PostedTraceContextStaleDropCount_{0};
    mutable std::atomic<uint64_t> WorkerLockAcquireCount_{0};
    mutable std::atomic<uint64_t> WorkerLockWaitNanos_{0};
    std::atomic<uint64_t> FindRelayByIdCount_{0};
    std::atomic<uint64_t> CallbackDispatchNanos_{0};
    mutable std::atomic<uint64_t> SnapshotBuildNanos_{0};
    mutable std::atomic<uint64_t> SnapshotActiveRelaysScanned_{0};
    std::atomic<uint64_t> MaintenanceDrainCount_{0};
    std::atomic<uint64_t> MaintenanceDrainNanos_{0};
    std::atomic<uint64_t> MaintenanceRelaysProcessed_{0};
    std::atomic<uint64_t> MaintenanceFullScanCount_{0};
    std::atomic<uint64_t> MaintenanceFullScanRelaysScanned_{0};
    std::atomic<uint64_t> ReceiveViewFinishLinearSearchCount_{0};
    std::atomic<uint64_t> ReceiveViewFinishLinearSearchNanos_{0};
    std::atomic<uint64_t> ReceiveViewFinishNotFrontCount_{0};
#if defined(TQ_UNIT_TESTING)
    std::atomic<bool> ForceTraceContextPostFailureForTest_{false};
    std::atomic<bool> QuicReceiveViewDrainEnabledForTest_{true};
    std::atomic<uint64_t> PostTcpRecvFromSendCompleteCallbackCount_{0};
    std::atomic<DWORD> LastCallbackPostWin32Error_{0};
    mutable std::mutex LastPostedCallbackLock_;
    TqWindowsIocpOperationType LastPostedCallbackType_{TqWindowsIocpOperationType::TcpRecv};
    uint64_t LastPostedCallbackRelayId_{0};
    bool LastPostedCallbackHadReceiveView_{false};
    std::vector<TqWindowsIocpOperationType> PostedCallbackSequence_;
#endif
    std::deque<std::shared_ptr<RelayContext>> MaintenanceQueue_;
    std::unordered_set<uint64_t> MaintenanceQueuedRelayIds_;
    size_t MaintenanceBudget_{64};
    uint32_t MaintenanceFullScanCountdown_{0};
    mutable std::mutex ControlCommandLock_;
    mutable std::mutex Lock_;
    std::unordered_map<uint64_t, std::shared_ptr<RelayContext>> Relays_;
    // Retired bindings keep late callbacks on old stream contexts safe until Stop or ref-counted pruning.
    std::vector<std::shared_ptr<CallbackBinding>> RetiredCallbacks_;
};

class TqWindowsRelayRuntime {
public:
    static TqWindowsRelayRuntime& Instance();

    bool Start(const TqTuningConfig& tuning);
    void Stop();
    bool RegisterRelay(
        TqSocketHandle tcpFd,
        MsQuicStream* stream,
        ITqCompressor* compressor,
        ITqDecompressor* decompressor,
        TqRelayHandle* handle,
        const TqTuningConfig& tuning,
        TqCompressAlgo compressAlgo);
    TqWindowsRelayWorkerSnapshot Snapshot() const;
    void StopRelay(TqRelayHandle* handle);

private:
    mutable std::mutex Lock_;
    std::vector<std::unique_ptr<TqWindowsRelayWorker>> Workers_;
    std::atomic<uint64_t> NextWorker_{0};
};
