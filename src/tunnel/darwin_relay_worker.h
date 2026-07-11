#pragma once

#if defined(__APPLE__)

#include "compress.h"
#include "darwin_relay_event_queue.h"
#include "msquic.hpp"
#include "platform_socket.h"
#include "relay.h"
#include "relay_buffer.h"
#include "stream_lifetime.h"
#include "tuning.h"

#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <shared_mutex>
#include <thread>
#include <unordered_map>
#include <vector>

struct MsQuicStream;
struct QUIC_STREAM_EVENT;

struct TqDarwinRelayRegistration {
    TqSocketHandle TcpFd{TqInvalidSocket};
    MsQuicStream* Stream{nullptr};
    // 新路径使用公共 manual-cleanup owner；非空时 worker 只发布 router
    // target，不改写已经启动 wrapper 的 Callback/Context。
    std::shared_ptr<TqStreamLifetime> StreamOwner;
    // Shared control + generation only. Caller publishes Backend/worker/id onto
    // the tunnel-owned handle after a successful commit.
    std::shared_ptr<TqRelayStopControl> Control;
    uint64_t ControlGeneration{0};
    ITqCompressor* Compressor{nullptr};
    ITqDecompressor* Decompressor{nullptr};
    TqCompressAlgo CompressAlgo{TqCompressAlgo::None};
    bool EnableQuicSends{true};
};

struct TqDarwinRelayRegistrationResult {
    bool Ok{false};
    bool TcpFdConsumed{false};
    uint64_t RelayId{0};
};

enum class TqDarwinRelayCloseDisposition {
    ActiveShutdown,
    TerminalLogicalDetach,
};

enum class TqDarwinSendOperationState : uint32_t {
    Created = 0,
    Registered = 1,
    Submitted = 2,
    CompletionClaimed = 3,
    Completed = 4,
    Detached = 5,
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
    std::atomic<uint32_t> State{static_cast<uint32_t>(TqDarwinSendOperationState::Created)};
    uint64_t CompletionRelayId{0};
    uint64_t CompletionTotalBytes{0};
    bool CompletionFin{false};
    std::shared_ptr<void> CompletionBindingOwner;

    bool TryTransition(TqDarwinSendOperationState expected, TqDarwinSendOperationState desired) {
        uint32_t value = static_cast<uint32_t>(expected);
        return State.compare_exchange_strong(
            value,
            static_cast<uint32_t>(desired),
            std::memory_order_acq_rel,
            std::memory_order_acquire);
    }

    bool TryMarkRegistered() {
        return TryTransition(TqDarwinSendOperationState::Created, TqDarwinSendOperationState::Registered);
    }

    bool TryMarkSubmitted() {
        uint32_t value = static_cast<uint32_t>(TqDarwinSendOperationState::Registered);
        return State.compare_exchange_strong(
            value,
            static_cast<uint32_t>(TqDarwinSendOperationState::Submitted),
            std::memory_order_acq_rel,
            std::memory_order_acquire);
    }

    bool TryClaimCompletion() {
        uint32_t value = State.load(std::memory_order_acquire);
        for (;;) {
            const auto state = static_cast<TqDarwinSendOperationState>(value);
            if (state == TqDarwinSendOperationState::CompletionClaimed ||
                state == TqDarwinSendOperationState::Completed ||
                state == TqDarwinSendOperationState::Detached) {
                return false;
            }
            if (State.compare_exchange_weak(
                    value,
                    static_cast<uint32_t>(TqDarwinSendOperationState::CompletionClaimed),
                    std::memory_order_acq_rel,
                    std::memory_order_acquire)) {
                return true;
            }
        }
    }

    bool TryMarkCompleted() {
        uint32_t value = State.load(std::memory_order_acquire);
        for (;;) {
            const auto state = static_cast<TqDarwinSendOperationState>(value);
            if (state == TqDarwinSendOperationState::Completed) {
                return false;
            }
            if (state == TqDarwinSendOperationState::Detached) {
                return false;
            }
            if (State.compare_exchange_weak(
                    value,
                    static_cast<uint32_t>(TqDarwinSendOperationState::Completed),
                    std::memory_order_acq_rel,
                    std::memory_order_acquire)) {
                return true;
            }
        }
    }

    bool MarkDetached() {
        uint32_t value = State.load(std::memory_order_acquire);
        for (;;) {
            const auto state = static_cast<TqDarwinSendOperationState>(value);
            if (state == TqDarwinSendOperationState::Completed ||
                state == TqDarwinSendOperationState::Detached ||
                state == TqDarwinSendOperationState::CompletionClaimed) {
                return false;
            }
            if (State.compare_exchange_weak(
                    value,
                    static_cast<uint32_t>(TqDarwinSendOperationState::Detached),
                    std::memory_order_acq_rel,
                    std::memory_order_acquire)) {
                return true;
            }
        }
    }

    bool IsCompletionClaimed() const {
        return static_cast<TqDarwinSendOperationState>(State.load(std::memory_order_acquire)) ==
            TqDarwinSendOperationState::CompletionClaimed;
    }

    bool IsSubmitted() const {
        const auto state = static_cast<TqDarwinSendOperationState>(State.load(std::memory_order_acquire));
        return state == TqDarwinSendOperationState::Submitted ||
            state == TqDarwinSendOperationState::CompletionClaimed ||
            state == TqDarwinSendOperationState::Completed;
    }
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
#if defined(TCPQUIC_TESTING)
    bool FailPrepareForTest{false};
    bool FailCommitForTest{false};
    bool FailManagedBindingForTest{false};
    // Fires immediately after PublishTarget returns (publish window), before the
    // next registration statement. Identity fields must already be initialized.
    void (*AfterPublishHookForTest)(TqDarwinRelayWorker*, uint64_t){nullptr};
    // Fires after inactive filters are installed and immediately before the
    // activation-mutex final commit check (barrier A).
    void (*BeforeCommitFinalCheckHookForTest)(TqDarwinRelayWorker*, uint64_t){nullptr};
    // Fires after Prepared->Active wins under the activation mutex, before
    // filter enable / precommit drain (barrier B).
    void (*AfterCommitActivationHookForTest)(TqDarwinRelayWorker*, uint64_t){nullptr};
    // Invoked at the start of queue-full terminal/active handoff so tests can
    // destroy and reuse handle storage before the fallback touches control.
    void (*BeforeTerminalHandoffHookForTest)(TqDarwinRelayWorker*, uint64_t){nullptr};
#endif
};

#if defined(TCPQUIC_TESTING)
struct TqDarwinBindingPublishIdentitySnapshot {
    uint64_t RelayId{0};
    uint64_t RouteGeneration{0};
    uint64_t ControlGeneration{0};
    bool RelayLockable{false};
    bool StreamOwnerLockable{false};
    size_t PrecommitDepth{0};
};
#endif

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
    uint64_t DeferredReceiveDiscards{0};
    uint64_t ReceiveFailSafeCount{0};
    uint64_t LateTerminalReceiveCount{0};
    uint64_t QuicSendBackpressureEvents{0};
    uint64_t Errors{0};
    uint64_t EventQueueFullErrors{0};
    uint64_t WakeFailures{0};
    uint64_t CallbackReceiveBudgetRejects{0};
    uint64_t QuicReceiveEnqueueFailures{0};
    uint64_t QuicReceiveViewBackpressureQueued{0};
    uint64_t TerminalRetainedOwnerCount{0};
    uint64_t TerminalRetainedOldestAgeMs{0};
    uint64_t StopRemaining{0};
};

enum class TqDarwinQuicReceiveEnqueueResult : uint8_t {
    Ok = 0,
    InvalidArgs,
    AllocationFailed,
    EmptyNonFin,
    NullBuffer,
    CallbackBudgetRejected,
    EventQueueFull,
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
    bool DrainOneEventForTest();
    uint32_t DrainWakeForTest();
    uint32_t PendingEventsForTest() const;
    bool RunningForTest() const;
    void SetRegisterTcpFiltersFailureForTest(bool fail);
    void SetWakeFailuresForTest(uint32_t failures);
    void SetStreamSendForTest(TqDarwinRelayStreamSendForTest sendFn);
    void SetReceiveCompleteForTest(TqDarwinRelayReceiveCompleteForTest completeFn);
    void SetReceiveSetEnabledForTest(TqDarwinRelayReceiveSetEnabledForTest setEnabledFn);
    void SetSendMsgForTest(TqDarwinRelaySendMsgForTest sendMsgFn);
    bool FlushTcpWritableForTest(uint64_t relayId);
    bool InvokeTcpEventForTest(uint64_t relayId, int16_t filter, uint16_t flags, intptr_t data);
    bool InvokeQuicReceiveViewForTest(const std::shared_ptr<TqDarwinPendingQuicReceive>& receive);
    uint64_t FindRelayLockedCountForTest() const;
    uint64_t CommittedRelayCountForTest() const;
    uint64_t FindRelayLocalCountForTest() const;
    uint64_t RetiredRelayPurgeCountForTest() const;
    uint64_t KnownSendLockedCountForTest() const;
    uint64_t CompletionStateLockedCountForTest() const;
    uint64_t ActiveSendLocalRegisterCountForTest() const;
    uint64_t ActiveSendLocalCompleteCountForTest() const;
    uint64_t FallbackSendCompletionCountForTest() const;
    void* StreamCallbackContextForTest(uint64_t relayId);
    std::shared_ptr<void> StreamCallbackContextOwnerForTest(uint64_t relayId);
    std::shared_ptr<void> DetachRelayFromActiveMapForTest(uint64_t relayId);
    uint64_t KnownSendOperationCountForTest();
    uint64_t PendingQuicSendCountForTest(uint64_t relayId);
    uint64_t InFlightQuicSendCountForTest(uint64_t relayId);
    uint64_t InFlightQuicSendCountFromRelayForTest(const std::shared_ptr<void>& relayOwner);
    uint64_t PendingQuicSendCountFromRelayForTest(const std::shared_ptr<void>& relayOwner);
    uint64_t CompleteOneInFlightSendForTest(uint64_t relayId);
    bool CorruptOneInFlightSendMagicForTest(uint64_t relayId);
    uint64_t PendingQuicReceiveBytesForTest(uint64_t relayId);
    uint64_t CallbackPendingReceiveBytesForTest(uint64_t relayId);
    uint64_t PendingTcpWriteBytesForTest(uint64_t relayId);
    uint64_t EventQueueFullErrorsForTest() const;
    uint64_t CallbackReceiveBudgetRejectsForTest() const;
    uint64_t QuicReceiveEnqueueFailuresForTest() const;
    uint64_t QuicReceiveViewBackpressureQueuedForTest() const;
    void SetRunningForTest(bool running);
    void MarkWorkerThreadExitedForTest();
    bool BindingActiveForTest(uint64_t relayId);
    bool BindingTerminalForTest(uint64_t relayId);
    TqDarwinBindingPublishIdentitySnapshot BindingPublishIdentityForTest(uint64_t relayId) const;
    TqDarwinBindingPublishIdentitySnapshot LastPublishIdentityForTest() const;
    size_t PrecommitQueueDepthForTest(uint64_t relayId) const;
    uint64_t TcpFilterInstallCountForTest() const;
    uint64_t TcpFilterDeleteCountForTest() const;
    uint64_t TcpFdCloseCountForTest() const;
    uint64_t MapPublicationCountForTest() const;
    uint64_t StreamBindingDestructorCountForTest() const;
    uint64_t RelayStateDestructorCountForTest() const;
    std::shared_ptr<TqStreamLifetime> StreamOwnerForTest(uint64_t relayId);
    uint64_t RetiredStreamBindingCountForTest();
    uint64_t RetiredRelayCountForTest();
    std::shared_ptr<void> RetiredRelayOwnerForTest(uint64_t relayId);
    std::shared_ptr<void> ActiveRelayOwnerForTest(uint64_t relayId);
    bool EnqueueRelayCloseEventForTest(
        const std::shared_ptr<void>& relayOwner,
        TqDarwinRelayEventType type,
        uint64_t relayId);
    MsQuicStream* RelayStreamForTest(uint64_t relayId);
    uint32_t BindingCallbackRefsForTest(uint64_t relayId);
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
        bool CompletionEventClaimed{false};
        std::shared_ptr<void> BindingOwner;
    };
    struct RegisterRelayCommand {
        TqDarwinRelayRegistration Registration;
        TqDarwinRelayRegistrationResult Result;
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
        TqDarwinRelayWorkerSnapshot Result;
        std::mutex Mutex;
        std::condition_variable Cv;
        bool Done{false};
    };
    enum class ControlEnqueueResult {
        Failed,
        QueuedAndWoken,
        QueuedWakeFailed,
    };

    void Run();
    bool IsWorkerThread() const;
    bool WorkerThreadExited() const;
    bool Wake() const;
    bool EnqueueEvent(TqDarwinRelayEvent&& event);
    ControlEnqueueResult EnqueueControlEvent(TqDarwinRelayEvent&& event) const;
    uint32_t DrainEvents(uint32_t budget);
    void PurgeQueuedEventsForStop();
    TqDarwinRelayRegistrationResult RegisterRelayWithIdLocal(
        const TqDarwinRelayRegistration& registration);
    void UnregisterRelayLocal(uint64_t relayId);
    TqDarwinRelayWorkerSnapshot SnapshotLocal() const;
    static void CompleteRegisterCommand(
        RegisterRelayCommand* command,
        TqDarwinRelayRegistrationResult result);
    static void CompleteUnregisterCommand(UnregisterRelayCommand* command);
    static void CompleteSnapshotCommand(
        SnapshotCommand* command,
        TqDarwinRelayWorkerSnapshot result);
    uint32_t DrainWakeEvents();
    bool RegisterTcpFilters(const std::shared_ptr<RelayState>& relay);
    bool InstallInactiveTcpFilters(const std::shared_ptr<RelayState>& relay);
    bool EnableTcpFilters(const std::shared_ptr<RelayState>& relay);
    bool UpdateTcpInterest(const std::shared_ptr<RelayState>& relay);
    bool UpdateTcpInterestLocal(const std::shared_ptr<RelayState>& relay);
    void RemoveTcpFilters(const std::shared_ptr<RelayState>& relay);
    void CloseRelayTcpFdOnce(const std::shared_ptr<RelayState>& relay);
    enum class PreparedCommitDisposition : uint8_t {
        CommitActive = 0,
        RollbackTerminal,
        RollbackFailed,
    };
    struct PreparedRelayToken;
    PreparedCommitDisposition TryCommitPreparedActivation(
        const std::shared_ptr<RelayState>& relay,
        StreamBinding* binding,
        const TqDarwinRelayRegistration& registration);
    void RollbackPreparedRelay(
        PreparedRelayToken& token,
        TqStreamLifetime::ShutdownIntent intent);
    std::shared_ptr<RelayState> FindRelay(uint64_t relayId);
    std::shared_ptr<RelayState> FindRetiredRelay(uint64_t relayId);
    // Raw worker-thread lookup; non-worker lifecycle access must use eventized commands.
    std::shared_ptr<RelayState> FindRelayLocal(uint64_t relayId) const;
    std::shared_ptr<RelayState> FindRetiredRelayLocal(uint64_t relayId) const;
    void AssertWorkerThreadForRelayState() const;
    bool IsRelayClosingLocal(const RelayState& relay) const;
    void MarkRelayClosingLocal(RelayState& relay) const;
    MsQuicStream* RelayStreamLocal(const RelayState& relay) const;
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
    bool EnqueueQuicSendCompleteFromCallback(uint64_t relayId, TqDarwinRelaySendOperation* operation);
    bool EnqueueRelayCloseFromCallback(
        const std::shared_ptr<RelayState>& relay,
        TqDarwinRelayEventType type);
    void CompleteQuicSend(TqDarwinRelaySendOperation* operation);
    bool ShouldPauseTcpReadForQuicBacklog(const std::shared_ptr<RelayState>& relay) const;
    bool ShouldResumeTcpReadForQuicBacklog(const std::shared_ptr<RelayState>& relay) const;
    bool SetTcpReadBackpressure(const std::shared_ptr<RelayState>& relay, bool paused);
    std::shared_ptr<TqDarwinPendingQuicReceive> BuildPendingQuicReceive(
        RelayState* relay,
        const std::shared_ptr<StreamBinding>& binding,
        MsQuicStream* stream,
        const QUIC_BUFFER* buffers,
        uint32_t bufferCount,
        bool fin);
    bool QueuePrecommitQuicReceive(
        RelayState* relay,
        StreamBinding* binding,
        const std::shared_ptr<StreamBinding>& bindingOwner,
        MsQuicStream* stream,
        const QUIC_BUFFER* buffers,
        uint32_t bufferCount,
        bool fin,
        bool& handled);
    TqDarwinQuicReceiveEnqueueResult QueueDeferredQuicReceive(
        const std::shared_ptr<StreamBinding>& binding,
        MsQuicStream* stream,
        const QUIC_BUFFER* buffers,
        uint32_t bufferCount,
        bool fin);
    uint64_t MaxPendingQuicReceiveBytesPerRelay() const;
    uint64_t LowPendingQuicReceiveBytesPerRelay() const;
    bool ReserveCallbackReceiveBudget(StreamBinding* binding, uint64_t bytes);
    void ReleaseCallbackReceiveBudget(StreamBinding* binding, uint64_t bytes);
    void ReleaseCallbackReceiveBudget(const std::shared_ptr<TqDarwinPendingQuicReceive>& receive);
    void ProcessQuicReceiveViewEvent(const std::shared_ptr<TqDarwinPendingQuicReceive>& receive);
    void FlushCallbackPendingQuicReceives(StreamBinding* binding);
    void FlushAllCallbackPendingQuicReceivesLocal();
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
    void CompleteMsQuicReceiveFromCallback(MsQuicStream* stream, uint64_t totalLength);
    void PauseMsQuicReceiveFromCallback(MsQuicStream* stream);
    bool SetQuicReceiveEnabled(const std::shared_ptr<RelayState>& relay, bool enabled);
    void MaybePauseQuicReceive(const std::shared_ptr<RelayState>& relay);
    void MaybeResumeQuicReceive(const std::shared_ptr<RelayState>& relay);
    QUIC_STATUS OnStreamEventWithBinding(
        MsQuicStream* stream,
        QUIC_STREAM_EVENT* event,
        StreamBinding* binding) noexcept;
    void ActivateManagedBinding(const std::shared_ptr<RelayState>& relay, StreamBinding* binding);
    // Under ActivationMutex: take precommit once for drain or discard. Returns
    // false if already settled (or empty after marking settled).
    bool TakePrecommitForSettlementLocked(
        StreamBinding* binding,
        std::deque<std::shared_ptr<TqDarwinPendingQuicReceive>>& out);
    void DiscardPrecommitReceives(
        std::deque<std::shared_ptr<TqDarwinPendingQuicReceive>>& pending);
    void FailManagedBinding(RelayState* relay, StreamBinding* binding);
    void SealManagedBindingTerminal(RelayState* relay, StreamBinding* binding);
    void HandoffTerminalCloseToShutdownSink(
        const std::shared_ptr<RelayState>& relay,
        StreamBinding* binding);
    void HandoffActiveShutdownFromCallback(
        const std::shared_ptr<RelayState>& relay,
        StreamBinding* binding);
    void ProcessPeerSendShutdown(const std::shared_ptr<RelayState>& relay);
    void ProcessSendShutdownComplete(const std::shared_ptr<RelayState>& relay);
    void RequestRelayShutdown(
        const std::shared_ptr<RelayState>& relay,
        TqStreamLifetime::ShutdownIntent intent);
    void RetireRelay(
        const std::shared_ptr<RelayState>& relay,
        TqDarwinRelayCloseDisposition disposition =
            TqDarwinRelayCloseDisposition::ActiveShutdown);
    void CloseRelay(
        const std::shared_ptr<RelayState>& relay,
        TqDarwinRelayCloseDisposition disposition =
            TqDarwinRelayCloseDisposition::ActiveShutdown);
    void PurgeRetiredRelaysIfSafe();
    bool WaitForKnownOperationsToDrain();
    void DetachActiveSendOperationsForStop();
    void DetachRetiredBindingsForDestruction();
    void RegisterKnownSendOperation(
        TqDarwinRelaySendOperation* operation,
        const KnownSendOperationInfo& info);
    // Worker-thread normal path entry point; conservatively shares KnownSendMutex with fallback paths.
    void RegisterKnownSendOperationLocal(
        TqDarwinRelaySendOperation* operation,
        const KnownSendOperationInfo& info);
    bool MarkKnownSendOperationSubmitted(
        TqDarwinRelaySendOperation* operation,
        KnownSendOperationInfo* info = nullptr);
    bool MarkKnownSendOperationSubmittedLocal(
        TqDarwinRelaySendOperation* operation,
        KnownSendOperationInfo* info = nullptr);
    // Claims a SEND_COMPLETE callback before enqueue; info reports the prior claim state.
    bool TryClaimKnownSendCompletionEvent(
        TqDarwinRelaySendOperation* operation,
        KnownSendOperationInfo* info);
    bool UnregisterKnownSendOperation(
        TqDarwinRelaySendOperation* operation,
        KnownSendOperationInfo* info);
    bool UnregisterKnownSendOperationLocal(
        TqDarwinRelaySendOperation* operation,
        KnownSendOperationInfo* info);
    static bool UnregisterCompletionStateOperation(
        const std::shared_ptr<CompletionState>& state,
        TqDarwinRelaySendOperation* operation,
        KnownSendOperationInfo* info,
        TqDarwinRelayWorker* testingWorker = nullptr);
    static bool CompleteDetachedQuicSend(StreamBinding* binding, TqDarwinRelaySendOperation* operation);

    struct CallbackEndpoint;

    TqDarwinRelayWorkerConfig Config;
    std::shared_ptr<CallbackEndpoint> StreamCallbackEndpoint;
    int KqueueFd{-1};
    std::atomic<bool> Running{false};
    mutable TqDarwinRelayEventQueue EventQueue;
    std::thread Thread;
    std::thread::id WorkerThreadId;
    mutable std::mutex WorkerThreadIdMutex;
    mutable std::mutex LifecycleMutex;
    mutable std::mutex RelayMutex;
    // Serializes worker-local map reads with lifecycle map/deque mutation without taking RelayMutex.
    mutable std::shared_mutex RelayMapAccessMutex;
    mutable std::mutex KnownSendMutex;
    mutable std::mutex ActiveSendMutex;
    std::unordered_map<uint64_t, std::shared_ptr<RelayState>> Relays;
    std::deque<std::shared_ptr<RelayState>> RetiredRelays;
    std::unordered_map<TqDarwinRelaySendOperation*, KnownSendOperationInfo> KnownSendOperations;
    std::unordered_map<TqDarwinRelaySendOperation*, KnownSendOperationInfo> ActiveSendOperations;
    std::vector<std::shared_ptr<StreamBinding>> RetiredStreamBindings;
    uint64_t NextRelayId{1};
#if defined(TCPQUIC_TESTING)
    bool FailRegisterTcpFiltersForTest{false};
    mutable std::atomic<uint32_t> WakeFailuresForTest{0};
    mutable std::atomic<uint64_t> FindRelayLockedCount{0};
    mutable std::atomic<uint64_t> FindRelayLocalCount{0};
    mutable std::atomic<uint64_t> RetiredRelayPurgeCount{0};
    mutable std::atomic<uint64_t> KnownSendLockedCount{0};
    mutable std::atomic<uint64_t> CompletionStateLockedCount{0};
    mutable std::atomic<uint64_t> ActiveSendLocalRegisterCount{0};
    mutable std::atomic<uint64_t> ActiveSendLocalCompleteCount{0};
    mutable std::atomic<uint64_t> ActiveSendOperationsSize{0};
    mutable std::atomic<uint64_t> FallbackSendCompletionCount{0};
    mutable std::atomic<uint64_t> TcpFilterInstallCount{0};
    mutable std::atomic<uint64_t> TcpFilterDeleteCount{0};
    mutable std::atomic<uint64_t> TcpFdCloseCount{0};
    mutable std::atomic<uint64_t> MapPublicationCount{0};
    mutable std::atomic<uint64_t> StreamBindingDestructorCount{0};
    mutable std::atomic<uint64_t> RelayStateDestructorCount{0};
    mutable TqDarwinBindingPublishIdentitySnapshot LastPublishIdentity{};
#endif
    std::atomic<uint64_t> EventsProcessed{0};
    mutable std::atomic<uint64_t> Wakeups{0};
    std::atomic<uint64_t> TcpReadBatches{0};
    std::atomic<uint64_t> TcpReadBytes{0};
    std::atomic<uint64_t> TcpWriteBatches{0};
    std::atomic<uint64_t> TcpWriteBytes{0};
    std::atomic<uint64_t> QuicReceiveViewCount{0};
    std::atomic<uint64_t> QuicReceiveViewBytes{0};
    std::atomic<uint64_t> DeferredReceiveCompletes{0};
    std::atomic<uint64_t> DeferredReceiveDiscards{0};
    std::atomic<uint64_t> ReceiveFailSafeCount{0};
    std::atomic<uint64_t> LateTerminalReceiveCount{0};
    std::atomic<uint64_t> QuicSendBackpressureEvents{0};
    std::atomic<uint64_t> QuicReceivePausedCount{0};
    std::atomic<uint64_t> QuicReceiveResumedCount{0};
    mutable std::atomic<uint64_t> Errors{0};
    mutable std::atomic<uint64_t> EventQueueFullErrors{0};
    mutable std::atomic<uint64_t> WakeFailures{0};
    mutable std::atomic<uint64_t> CallbackReceiveBudgetRejects{0};
    mutable std::atomic<uint64_t> QuicReceiveEnqueueFailures{0};
    mutable std::atomic<uint64_t> QuicReceiveViewBackpressureQueued{0};
};

class TqDarwinRelayRuntime final {
public:
    static TqDarwinRelayRuntime& Instance();

    bool Start(const TqTuningConfig& tuning);
    void Stop();
    TqDarwinRelayWorkerSnapshot Snapshot() const;
    TqDarwinRelayWorker* PickWorker();

private:
    TqDarwinRelayRuntime() = default;

    mutable std::mutex Mutex;
    bool Started{false};
    uint32_t NextWorker{0};
    std::vector<std::unique_ptr<TqDarwinRelayWorker>> Workers;
};

#endif
