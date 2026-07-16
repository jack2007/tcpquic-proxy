#include "libuv_relay_worker.h"

#include "libuv_allocator.h"

#include <algorithm>
#include <cassert>
#include <chrono>
#include <new>
#include <utility>

TqUvActivationMutex::~TqUvActivationMutex() noexcept {
    Destroy();
}

bool TqUvActivationMutex::Initialize() noexcept {
    if (Initialized_) {
        return true;
    }
    if (!TqUvInstallAllocator()) {
        return false;
    }
    if (uv_mutex_init(&Mutex_) != 0) {
        return false;
    }
    Initialized_ = true;
    return true;
}

void TqUvActivationMutex::Destroy() noexcept {
    if (!Initialized_) {
        return;
    }
    Initialized_ = false;
    uv_mutex_destroy(&Mutex_);
#if defined(TQ_UNIT_TESTING) && TQ_UNIT_TESTING
    ++DestroyCountForTest_;
#endif
}

bool TqUvActivationMutex::Ready() const noexcept {
    return Initialized_;
}

bool TqUvActivationMutex::Lock() noexcept {
    if (!Initialized_) {
        return false;
    }
    uv_mutex_lock(&Mutex_);
    return true;
}

bool TqUvActivationMutex::Unlock() noexcept {
    if (!Initialized_) {
        return false;
    }
    uv_mutex_unlock(&Mutex_);
    return true;
}

#if defined(TQ_UNIT_TESTING) && TQ_UNIT_TESTING
std::uint64_t TqUvActivationMutex::DestroyCountForTest() const noexcept {
    return DestroyCountForTest_;
}
#endif

namespace {

constexpr std::uint64_t TqUvSafetyTimerIntervalMs = 25;
constexpr std::uint32_t TqUvAsyncSendAttemptLimit = 4;

#if defined(TQ_UNIT_TESTING) && TQ_UNIT_TESTING
const TqUvCallAdapter gProductionCalls{
    &uv_loop_init,
    &uv_loop_close,
    &uv_async_init,
    &uv_async_send,
    &uv_tcp_init,
    &uv_tcp_open,
    &uv_read_start,
    &uv_read_stop,
    &uv_write,
    &uv_shutdown,
    &uv_close,
};
#endif

} // namespace

std::uint64_t TqUvLoopLagMicros(
    std::uint64_t previousNanos,
    std::uint64_t nowNanos,
    std::uint64_t intervalNanos) noexcept {
    if (previousNanos == 0 || nowNanos <= previousNanos) {
        return 0;
    }
    const std::uint64_t elapsed = nowNanos - previousNanos;
    return elapsed > intervalNanos
        ? (elapsed - intervalNanos) / 1000
        : 0;
}

#if defined(TQ_UNIT_TESTING) && TQ_UNIT_TESTING
const TqUvCallAdapter& TqUvProductionCalls() noexcept {
    return gProductionCalls;
}
#endif

struct TqUvRelayWorker::RegisterCommand final {
    explicit RegisterCommand(TqUvRelayRegistration registration)
        : Registration(std::move(registration)) {
        const int mutexStatus = uv_mutex_init(&Mutex);
        assert(mutexStatus == 0);
        if (mutexStatus != 0) {
            return;
        }
        MutexInitialized = true;
        const int conditionStatus = uv_cond_init(&Condition);
        assert(conditionStatus == 0);
        if (conditionStatus != 0) {
            uv_mutex_destroy(&Mutex);
            MutexInitialized = false;
            return;
        }
        ConditionInitialized = true;
    }

    ~RegisterCommand() {
        if (ConditionInitialized) {
            uv_cond_destroy(&Condition);
        }
        if (MutexInitialized) {
            uv_mutex_destroy(&Mutex);
        }
    }

    uv_mutex_t Mutex{};
    uv_cond_t Condition{};
    TqUvRelayRegistration Registration;
    TqUvRelayRegistrationResult Result;
    TqUvRegisterCommandState State{TqUvRegisterCommandState::Queued};
    bool MutexInitialized{false};
    bool ConditionInitialized{false};
};

TqUvRelayWorker::TqUvRelayWorker(TqUvRelayWorkerConfig config)
    : Config_(std::move(config)),
#if defined(TQ_UNIT_TESTING) && TQ_UNIT_TESTING
      CallsForTest_(Config_.Calls),
#endif
      Queue_(
          Config_.QueueCapacity
#if defined(TQ_UNIT_TESTING) && TQ_UNIT_TESTING
          , Config_.QueueMutexInitForTest,
          Config_.BeforeQueuePopForTest
#endif
      )
#if defined(TQ_UNIT_TESTING) && TQ_UNIT_TESTING
      , FailStopScanOnceForTest_(Config_.FailStopScanOnceForTest)
#endif
      {}

TqUvRelayWorker::~TqUvRelayWorker() {
    StopAndJoin();
}

bool TqUvRelayWorker::StartAndWaitReady() {
    if (!Queue_.Initialized() || ThreadCreated_ || LifecycleSyncInitialized_) {
        return false;
    }

    const int mutexStatus = uv_mutex_init(&LifecycleMutex_);
    if (mutexStatus != 0) {
        return false;
    }
    const int conditionStatus = uv_cond_init(&LifecycleCondition_);
    if (conditionStatus != 0) {
        uv_mutex_destroy(&LifecycleMutex_);
        return false;
    }
    LifecycleSyncInitialized_ = true;

    const int threadStatus = uv_thread_create(&Thread_, &ThreadEntry, this);
    if (threadStatus != 0) {
        uv_cond_destroy(&LifecycleCondition_);
        uv_mutex_destroy(&LifecycleMutex_);
        LifecycleSyncInitialized_ = false;
        return false;
    }
    ThreadCreated_ = true;

    uv_mutex_lock(&LifecycleMutex_);
    while (!ReadyReported_) {
        uv_cond_wait(&LifecycleCondition_, &LifecycleMutex_);
    }
    const int readyStatus = ReadyStatus_;
    uv_mutex_unlock(&LifecycleMutex_);
    if (readyStatus != 0) {
        StopAndJoin();
        return false;
    }
    return true;
}

void TqUvRelayWorker::StopForStartupRollback() {
    StopAndJoin();
}

bool TqUvRelayWorker::Post(LocalCommand command) {
    if (!command) {
        return false;
    }
    if (IsLoopThread()) {
        if (!Accepting_.load(std::memory_order_acquire)) {
            return false;
        }
        PostLocal(std::move(command));
        return true;
    }
    return Enqueue(Command{std::move(command)});
}

bool TqUvRelayWorker::PostDeferred(LocalCommand command) {
    if (!command) {
        return false;
    }
#if defined(TQ_UNIT_TESTING) && TQ_UNIT_TESTING
    if (Config_.FailDeferredPostForTest != nullptr &&
        Config_.FailDeferredPostForTest()) {
        return false;
    }
#endif
    return Enqueue(Command{std::move(command)});
}

bool TqUvRelayWorker::QueueSendComplete(
    TqUvQuicSendOperation* operation,
    bool cancelled) noexcept {
    if (operation == nullptr) {
        return false;
    }
    bool expected = false;
    if (!operation->CompletionQueued.compare_exchange_strong(
            expected, true, std::memory_order_acq_rel,
            std::memory_order_acquire)) {
        return true;
    }
    operation->CompletionCancelled.store(cancelled, std::memory_order_release);
    try {
        if (Enqueue(Command{SendCompleteCommand{operation}})) {
            return true;
        }
    } catch (...) {
        // The operation itself is the allocation-free reliable fallback node.
    }
    auto* head = SendCompletionFallbackHead_.load(std::memory_order_acquire);
    do {
        operation->FallbackNext = head;
    } while (!SendCompletionFallbackHead_.compare_exchange_weak(
        head, operation, std::memory_order_release, std::memory_order_acquire));
    SendCompletionFallbacks_.fetch_add(1, std::memory_order_relaxed);
    (void)WakeLoopUntilAccepted();
    return true;
}

void TqUvRelayWorker::PostLocal(LocalCommand command) {
    assert(IsLoopThread());
    if (command) {
        command(*this);
        CommandsExecuted_.fetch_add(1, std::memory_order_relaxed);
    }
}

TqUvRelayRegistrationResult TqUvRelayWorker::RegisterRelayWithId(
    TqUvRelayRegistration registration) {
    if (IsLoopThread()) {
        return RegisterRelayWithIdLocal(std::move(registration));
    }
    if (!Accepting_.load(std::memory_order_acquire)) {
        return {};
    }

    auto command = std::make_shared<RegisterCommand>(std::move(registration));
    if (!command->MutexInitialized || !command->ConditionInitialized) {
        return {};
    }
    TrackRegistration(command);
#if defined(TQ_UNIT_TESTING) && TQ_UNIT_TESTING
    {
        std::lock_guard<std::mutex> guard(TestRegistrationMutex_);
        TestRegistration_ = command;
    }
#endif
    if (!Enqueue(Command{command})) {
        uv_mutex_lock(&command->Mutex);
        if (command->State == TqUvRegisterCommandState::Queued) {
            command->State = TqUvRegisterCommandState::Cancelled;
        }
        uv_mutex_unlock(&command->Mutex);
        UntrackRegistration(command);
        return {};
    }

    const auto deadline = std::chrono::steady_clock::now() +
        std::chrono::milliseconds(Config_.ControlCommandTimeoutMs);
    uv_mutex_lock(&command->Mutex);
    while (command->State == TqUvRegisterCommandState::Queued) {
        const auto now = std::chrono::steady_clock::now();
        if (now >= deadline) {
            command->State = TqUvRegisterCommandState::Cancelled;
            uv_mutex_unlock(&command->Mutex);
            UntrackRegistration(command);
            return {};
        }
        const auto remaining = std::chrono::duration_cast<std::chrono::nanoseconds>(
            deadline - now);
        (void)uv_cond_timedwait(
            &command->Condition,
            &command->Mutex,
            static_cast<std::uint64_t>(remaining.count()));
    }
    while (command->State == TqUvRegisterCommandState::Executing) {
        uv_cond_wait(&command->Condition, &command->Mutex);
    }
    const auto result = command->Result;
    uv_mutex_unlock(&command->Mutex);
    UntrackRegistration(command);
    return result;
}

bool TqUvRelayWorker::AcceptStop(
    const std::shared_ptr<const TqUvRelayCommittedState>& committed) {
    if (committed == nullptr || committed->Control == nullptr ||
        committed->RelayId == 0 ||
        committed->WorkerIndex != Config_.WorkerIndex ||
        committed->WorkerIdentity != reinterpret_cast<std::uintptr_t>(this) ||
        committed->ControlGeneration != committed->Control->Generation ||
        !Running_.load(std::memory_order_acquire)) {
        return false;
    }
    (void)committed->Control->SignalStop(committed->ControlGeneration);
    StopRequestSequence_.fetch_add(1, std::memory_order_release);
    // The stop fact lives in the control and is scanned by every safety-timer
    // callback. uv_async_send is only an acceleration, never admission.
    (void)WakeLoopUntilAccepted();
    return true;
}

TqUvRelayRegistrationResult TqUvRelayWorker::RegisterRelayWithIdLocal(
    TqUvRelayRegistration registration) {
    assert(IsLoopThread());
#if defined(TQ_UNIT_TESTING) && TQ_UNIT_TESTING
    if (Config_.RegisterLocalForTest != nullptr) {
        try {
            return Config_.RegisterLocalForTest(*this, std::move(registration));
        } catch (...) {
            return {};
        }
    }
#endif
    TqUvRelayRegistrationResult result{};
    std::shared_ptr<TqUvRelayState> relay;
    std::shared_ptr<TqUvStreamBinding> binding;
    std::shared_ptr<TqUvRelayCommittedState> committed;
    bool targetPublished = false;
    try {
    if (registration.TcpSocket == TqInvalidSocket ||
        registration.Stream == nullptr ||
        registration.StreamOwner == nullptr ||
        registration.StopControl == nullptr ||
        registration.ControlGeneration == 0 ||
        registration.ControlGeneration != registration.StopControl->Generation ||
        registration.TcpReadChunkSize == 0 ||
        registration.MaxPendingBufferBytes == 0 ||
        registration.MaxBufferedQuicSendBytes == 0 ||
        registration.ResumeBufferedQuicSendBytes >
            registration.MaxBufferedQuicSendBytes) {
        return result;
    }

    relay = std::make_shared<TqUvRelayState>();
    binding = std::make_shared<TqUvStreamBinding>();
#if defined(TQ_UNIT_TESTING) && TQ_UNIT_TESTING
    if (!Config_.FailCommittedAllocationForTest) {
#endif
        committed = std::shared_ptr<TqUvRelayCommittedState>(
            new (std::nothrow) TqUvRelayCommittedState{});
#if defined(TQ_UNIT_TESTING) && TQ_UNIT_TESTING
    }
#endif
    if (committed == nullptr) {
        return result;
    }
    if (!relay->ActivationMutex.Initialize()) {
        return result;
    }

    relay->Worker = this;
    relay->WorkerIndex = Config_.WorkerIndex;
    relay->RelayId = NextRelayId_++;
    relay->ControlGeneration = registration.ControlGeneration;
    relay->Stream = registration.Stream;
    relay->StreamOwner = registration.StreamOwner;
    relay->StopControl = registration.StopControl;
    const auto terminalLedger = registration.StreamOwner->TerminalLedger();
    if (terminalLedger == nullptr) {
        return result;
    }
    std::atomic_store(
        &registration.StopControl->TerminalHandoff,
        std::make_shared<TqTerminalHandoffControl>(
            registration.ControlGeneration,
            terminalLedger,
            registration.StopControl->TerminalEscalation));
    relay->Binding = binding;
    relay->Compressor = registration.Compressor;
    relay->Decompressor = registration.Decompressor;
    relay->CompressAlgo = registration.CompressAlgo;
    relay->PrecommitMaxPendingBytes = registration.PrecommitMaxPendingBytes;
    relay->TcpReadChunkSize = registration.TcpReadChunkSize;
    relay->TcpReadBufferBudget.MaxPendingBufferBytes =
        registration.MaxPendingBufferBytes;
    relay->MaxBufferedQuicSendBytes =
        registration.MaxBufferedQuicSendBytes;
    relay->ResumeBufferedQuicSendBytes =
        registration.ResumeBufferedQuicSendBytes;
    relay->QuicToTcpCallBudget = registration.WorkerEventBudget == 0
        ? 1 : registration.WorkerEventBudget;
    relay->QuicToTcpByteBudgetPerTick =
        registration.WorkerByteBudgetPerTick == 0
            ? std::max<std::uint64_t>(registration.PrecommitMaxPendingBytes, 1)
            : registration.WorkerByteBudgetPerTick;
    binding->Worker = this;
    binding->Relay = relay;
    binding->StopControl = registration.StopControl;
    binding->RelayId = relay->RelayId;
    binding->ControlGeneration = registration.ControlGeneration;
    relay->TcpHandle.data = relay.get();
#if defined(TQ_UNIT_TESTING) && TQ_UNIT_TESTING
    if (Config_.FailRelayMapInsertForTest) {
        throw std::bad_alloc{};
    }
#endif
    Relays_.emplace(relay->RelayId, relay);
    result.RelayId = relay->RelayId;
    committed->WorkerIdentity = reinterpret_cast<std::uintptr_t>(this);
    committed->RelayId = relay->RelayId;
    committed->WorkerIndex = Config_.WorkerIndex;
    committed->Control = registration.StopControl;
    committed->ControlGeneration = registration.ControlGeneration;

    const int initStatus = CallTcpInit(&Loop_, &relay->TcpHandle);
    if (initStatus != 0) {
        relay->Activation = TqUvActivation::Failed;
        binding->Activation.store(TqUvActivation::Failed, std::memory_order_release);
        RecordRegistrationSnapshotForTest(*relay);
        EraseRelay(relay->RelayId);
        return result;
    }
    relay->TcpHandleInitialized = true;
    relay->TcpHandle.data = relay.get();

    const std::uint64_t expectedGeneration =
        registration.StreamOwner->RouteGeneration();
    // PublishTarget increments exactly once on success. Initialize every
    // callback-visible identity before publication; never patch it afterward.
    binding->RouteGeneration = expectedGeneration + 1;
    relay->RouteGeneration = binding->RouteGeneration;
    if (!registration.StreamOwner->PublishTarget(
            expectedGeneration, binding)) {
        relay->Activation = TqUvActivation::Failed;
        binding->Activation.store(TqUvActivation::Failed, std::memory_order_release);
        RecordRegistrationSnapshotForTest(*relay);
        CloseRelay(relay);
        return result;
    }
    targetPublished = true;
    const int openStatus = CallTcpOpen(
        &relay->TcpHandle, static_cast<uv_os_sock_t>(registration.TcpSocket));
    if (openStatus != 0) {
        // uv_tcp_open did not consume the fd, but the published stream target
        // still requires the same real terminal handoff as an active relay.
        relay->SocketOwnership = TqUvSocketOwnership::CallerOwned;
        if (registration.ReleaseAccountingOnFailedRegistration) {
            relay->StopControl->ReleaseAccountingAtBackendTerminal.store(
                true, std::memory_order_release);
        }
        TqUvRequestTerminal(*relay, TqUvTerminalTrigger::TcpError);
        TqUvProcessTerminalFactsLocal(*this, *relay);
        result.TerminalCleanupPending = true;
        RecordRegistrationSnapshotForTest(*relay);
        return result;
    }
    relay->SocketOwnership = TqUvSocketOwnership::UvHandleOwned;
    result.TcpFdConsumed = true;

    const int readStatus = CallReadStart(
        reinterpret_cast<uv_stream_t*>(&relay->TcpHandle),
        &TqUvOnTcpAlloc,
        &TqUvOnTcpRead);
    if (readStatus != 0) {
        if (registration.ReleaseAccountingOnFailedRegistration) {
            relay->StopControl->ReleaseAccountingAtBackendTerminal.store(
                true, std::memory_order_release);
        }
        TqUvRequestTerminal(*relay, TqUvTerminalTrigger::TcpError);
        TqUvProcessTerminalFactsLocal(*this, *relay);
        result.TerminalCleanupPending = true;
        RecordRegistrationSnapshotForTest(*relay);
        return result;
    }
    relay->TcpReadStarted = true;

    if (!relay->ActivationMutex.Lock()) {
        if (registration.ReleaseAccountingOnFailedRegistration) {
            relay->StopControl->ReleaseAccountingAtBackendTerminal.store(
                true, std::memory_order_release);
        }
        TqUvRequestTerminal(*relay, TqUvTerminalTrigger::QueueFailure);
        result.TerminalCleanupPending = true;
        RecordRegistrationSnapshotForTest(*relay);
        return result;
    }
    const auto activation = binding->Activation.load(std::memory_order_acquire);
    const bool stopped =
        registration.StopControl->Stop.load(std::memory_order_acquire);
    const bool activationLost = activation != TqUvActivation::Prepared || stopped ||
        relay->QuicShutdownObserved.load(std::memory_order_acquire) ||
        relay->TerminalTriggerMask.load(std::memory_order_acquire) != 0;
    if (activationLost) {
        if (registration.ReleaseAccountingOnFailedRegistration) {
            relay->StopControl->ReleaseAccountingAtBackendTerminal.store(
                true, std::memory_order_release);
        }
        const auto terminalActivation =
            activation == TqUvActivation::Failed
                ? TqUvActivation::Failed
                : TqUvActivation::Terminal;
        relay->Activation = terminalActivation;
        binding->Activation.store(terminalActivation, std::memory_order_release);
        SettlePrecommit(relay, false);
    } else {
        relay->Activation = TqUvActivation::Active;
#if defined(TCPQUIC_LIBUV_QUIC_TO_TCP_READY) && \
    TCPQUIC_LIBUV_QUIC_TO_TCP_READY
        relay->AdmittedQuicReceiveBytes.store(
            relay->PendingQuicReceiveBytes, std::memory_order_release);
        relay->QuicToTcpPressureBytes.store(
            relay->PendingQuicReceiveBytes, std::memory_order_release);
#endif
        binding->Activation.store(TqUvActivation::Active, std::memory_order_release);
        relay->SocketOwnership = TqUvSocketOwnership::ActiveRelayOwned;
        SettlePrecommit(relay, true);
        PublishActiveRelay(*relay);
    }
    (void)relay->ActivationMutex.Unlock();

#if defined(TCPQUIC_LIBUV_QUIC_TO_TCP_READY) && \
    TCPQUIC_LIBUV_QUIC_TO_TCP_READY
    if (!activationLost) {
        TqUvProcessQuicToTcp(*this, *relay);
    }
#endif

    if (activationLost) {
        TqUvRequestTerminal(*relay, TqUvTerminalTrigger::RuntimeStop);
        TqUvProcessTerminalFactsLocal(*this, *relay);
        result.TerminalCleanupPending = true;
        RecordRegistrationSnapshotForTest(*relay);
        return result;
    }

    result.Committed = std::move(committed);
    result.Ok = true;
    RecordRegistrationSnapshotForTest(*relay);
    return result;
    } catch (...) {
        if (relay == nullptr) {
            return result;
        }
        result.TcpFdConsumed =
            relay->SocketOwnership != TqUvSocketOwnership::CallerOwned;
        if (targetPublished) {
            if (registration.ReleaseAccountingOnFailedRegistration) {
                relay->StopControl->ReleaseAccountingAtBackendTerminal.store(
                    true, std::memory_order_release);
            }
            TqUvRequestTerminal(*relay, TqUvTerminalTrigger::AllocationFailure);
            TqUvProcessTerminalFactsLocal(*this, *relay);
            result.TerminalCleanupPending = true;
        } else if (relay->ActivationMutex.Ready() &&
                   relay->ActivationMutex.Lock()) {
            relay->Activation = TqUvActivation::Failed;
            if (binding != nullptr) {
                binding->Activation.store(
                    TqUvActivation::Failed, std::memory_order_release);
            }
            SettlePrecommit(relay, false);
            (void)relay->ActivationMutex.Unlock();
        }
        RecordRegistrationSnapshotForTest(*relay);
        if (!targetPublished && relay->TcpHandleInitialized) {
            CloseRelay(relay);
        } else if (!targetPublished && relay->RelayId != 0) {
            EraseRelay(relay->RelayId);
        }
        return result;
    }
}

TqUvRelayWorkerSnapshot TqUvRelayWorker::Snapshot() const {
    const auto quicToTcp =
        QuicToTcpPendingBytes_.load(std::memory_order_relaxed);
    const auto tcpToQuic =
        TcpToQuicPendingBytes_.load(std::memory_order_relaxed);
    const auto asyncCallbacks =
        AsyncCallbacks_.load(std::memory_order_relaxed);
    const auto timerCallbacks =
        SafetyTimerCallbacks_.load(std::memory_order_relaxed);
    const auto wakeSuccesses =
        AsyncWakeSuccesses_.load(std::memory_order_relaxed);
    const auto wakeFailures =
        AsyncWakeFailures_.load(std::memory_order_relaxed);
    const auto pendingBytes = tcpToQuic > UINT64_MAX - quicToTcp
        ? UINT64_MAX
        : quicToTcp + tcpToQuic;
    return {
        Config_.WorkerIndex,
        Running_.load(std::memory_order_acquire),
        static_cast<std::uint64_t>(Queue_.Size()),
        wakeSuccesses + wakeFailures,
        wakeSuccesses,
        wakeFailures,
        AsyncWakeCoalesced_.load(std::memory_order_relaxed),
        asyncCallbacks,
        timerCallbacks,
        LoopIterations_.load(std::memory_order_relaxed),
        CommandsExecuted_.load(std::memory_order_relaxed),
        ActiveRelays_.load(std::memory_order_relaxed),
        pendingBytes,
        quicToTcp,
        tcpToQuic,
        LoopLagMicros_.load(std::memory_order_relaxed),
        StopScanPasses_.load(std::memory_order_relaxed),
        StopScanFailures_.load(std::memory_order_relaxed),
        SendCompletionCommands_.load(std::memory_order_acquire),
        SendCompletionFallbacks_.load(std::memory_order_relaxed),
        static_cast<std::uint64_t>(Config_.QueueCapacity),
        TcpReadBytes_.load(std::memory_order_relaxed),
        TcpWriteBytes_.load(std::memory_order_relaxed),
        CompressedTcpBytes_.load(std::memory_order_relaxed),
        DecompressedTcpBytes_.load(std::memory_order_relaxed),
        ZstdDecompressInputBytes_.load(std::memory_order_relaxed),
        ZstdDecompressOutputBytes_.load(std::memory_order_relaxed),
        ZstdDecompressCalls_.load(std::memory_order_relaxed),
        ZstdDecompressNeedInput_.load(std::memory_order_relaxed),
        ZstdDecompressNeedOutput_.load(std::memory_order_relaxed),
        ZstdDecompressFailures_.load(std::memory_order_relaxed),
        TcpToQuicCompressFailures_.load(std::memory_order_relaxed),
        QuicReceiveDecompressFailures_.load(std::memory_order_relaxed),
    };
}

bool TqUvRelayWorker::AddPendingBytes(
    TqUvRelayState& relay,
    TqUvPendingDirection direction,
    std::uint64_t bytes) noexcept {
    if (bytes == 0) {
        return true;
    }
    auto& relayBytes = direction == TqUvPendingDirection::QuicToTcp
        ? relay.AccountedQuicToTcpBytes
        : relay.AccountedTcpToQuicBytes;
    auto& workerBytes = direction == TqUvPendingDirection::QuicToTcp
        ? QuicToTcpPendingBytes_
        : TcpToQuicPendingBytes_;
    auto observed = workerBytes.load(std::memory_order_relaxed);
    for (;;) {
        if (bytes > UINT64_MAX - observed) {
#if defined(TQ_UNIT_TESTING) && TQ_UNIT_TESTING
            if (Config_.BeforePendingOverflowReturnForTest != nullptr) {
                Config_.BeforePendingOverflowReturnForTest();
            }
#endif
            return false;
        }
        if (workerBytes.compare_exchange_weak(
                observed,
                observed + bytes,
                std::memory_order_relaxed,
                std::memory_order_relaxed)) {
            break;
        }
    }
    auto relayObserved = relayBytes.load(std::memory_order_relaxed);
    for (;;) {
        if (bytes > UINT64_MAX - relayObserved) {
            workerBytes.fetch_sub(bytes, std::memory_order_relaxed);
            return false;
        }
        if (relayBytes.compare_exchange_weak(
                relayObserved,
                relayObserved + bytes,
                std::memory_order_relaxed,
                std::memory_order_relaxed)) {
            break;
        }
    }
    return true;
}

bool TqUvRelayWorker::CompletePendingBytes(
    TqUvRelayState& relay,
    TqUvPendingDirection direction,
    std::uint64_t bytes) noexcept {
    if (bytes == 0) {
        return true;
    }
    auto& relayBytes = direction == TqUvPendingDirection::QuicToTcp
        ? relay.AccountedQuicToTcpBytes
        : relay.AccountedTcpToQuicBytes;
    auto& workerBytes = direction == TqUvPendingDirection::QuicToTcp
        ? QuicToTcpPendingBytes_
        : TcpToQuicPendingBytes_;
    auto relayObserved = relayBytes.load(std::memory_order_relaxed);
    for (;;) {
        if (bytes > relayObserved) {
            return false;
        }
        if (relayBytes.compare_exchange_weak(
                relayObserved,
                relayObserved - bytes,
                std::memory_order_relaxed,
                std::memory_order_relaxed)) {
            break;
        }
    }
    workerBytes.fetch_sub(bytes, std::memory_order_relaxed);
    return true;
}

QUIC_STATUS TqUvStreamBinding::OnStreamEvent(
    MsQuicStream* stream,
    QUIC_STREAM_EVENT* event,
    std::uint64_t routeGeneration) noexcept {
    if (event == nullptr) {
        return QUIC_STATUS_SUCCESS;
    }
    if (event->Type == QUIC_STREAM_EVENT_SEND_COMPLETE &&
        routeGeneration == RouteGeneration) {
        auto* operation = static_cast<TqUvQuicSendOperation*>(
            event->SEND_COMPLETE.ClientContext);
        if (Worker != nullptr && operation != nullptr) {
            (void)Worker->QueueSendComplete(
                operation, event->SEND_COMPLETE.Canceled != 0);
        }
        return QUIC_STATUS_SUCCESS;
    }
    auto relay = Relay.lock();
    if (!relay) {
        return QUIC_STATUS_SUCCESS;
    }
    const auto terminalRoute =
        TerminalRouteGeneration.load(std::memory_order_acquire);
    if (event->Type == QUIC_STREAM_EVENT_RECEIVE &&
        (routeGeneration != RouteGeneration || terminalRoute != 0)) {
        return QUIC_STATUS_SUCCESS;
    }
    if (event->Type == QUIC_STREAM_EVENT_SHUTDOWN_COMPLETE) {
        const auto currentTerminalRoute = relay->StreamOwner != nullptr
            ? relay->StreamOwner->RouteGeneration()
            : terminalRoute;
        const bool activeRouteTerminal =
            routeGeneration == RouteGeneration &&
            routeGeneration == currentTerminalRoute;
        const bool publishedTerminalRoute =
            routeGeneration > RouteGeneration &&
            routeGeneration == currentTerminalRoute;
        if (!activeRouteTerminal && !publishedTerminalRoute) {
            return QUIC_STATUS_SUCCESS;
        }
        TerminalRouteGeneration.store(
            currentTerminalRoute, std::memory_order_release);
    } else if (routeGeneration != RouteGeneration &&
               (terminalRoute == 0 || routeGeneration != terminalRoute)) {
        return QUIC_STATUS_SUCCESS;
    }
    if (event->Type == QUIC_STREAM_EVENT_PEER_SEND_ABORTED ||
        event->Type == QUIC_STREAM_EVENT_PEER_RECEIVE_ABORTED) {
        TqUvRequestTerminal(*relay, TqUvTerminalTrigger::QuicAbort);
        return QUIC_STATUS_SUCCESS;
    }
    if (event->Type == QUIC_STREAM_EVENT_SHUTDOWN_COMPLETE) {
        relay->QuicShutdownObserved.store(true, std::memory_order_release);
        if (Activation.load(std::memory_order_acquire) !=
            TqUvActivation::Terminal) {
            TqUvRequestTerminal(*relay, TqUvTerminalTrigger::QuicAbort);
        } else if (Worker != nullptr) {
            Worker->WakeForDurableFacts();
        }
        return QUIC_STATUS_SUCCESS;
    }
    if (!relay->ActivationMutex.Lock()) {
        return QUIC_STATUS_SUCCESS;
    }

    QUIC_STATUS status = QUIC_STATUS_SUCCESS;
    const auto phase = Activation.load(std::memory_order_acquire);
    if (event->Type == QUIC_STREAM_EVENT_RECEIVE &&
        phase == TqUvActivation::Prepared && !PrecommitSettled.load()) {
        if (event->RECEIVE.TotalBufferLength == 0) {
            if ((event->RECEIVE.Flags & QUIC_RECEIVE_FLAG_FIN) != 0) {
                relay->QuicFinObserved.store(true, std::memory_order_release);
            }
            (void)relay->ActivationMutex.Unlock();
            if (Worker != nullptr) {
                Worker->WakeForDurableFacts();
            }
            return QUIC_STATUS_SUCCESS;
        }
        try {
            auto pending = std::make_shared<TqUvPendingQuicReceive>();
            pending->Stream = stream;
            pending->StreamOwner = relay->StreamOwner;
            pending->RelayId = RelayId;
            pending->RouteGeneration = routeGeneration;
            pending->ControlGeneration = ControlGeneration;
            pending->Fin =
                (event->RECEIVE.Flags & QUIC_RECEIVE_FLAG_FIN) != 0;
            pending->Slices.reserve(event->RECEIVE.BufferCount);
            for (std::uint32_t index = 0;
                 index < event->RECEIVE.BufferCount;
                 ++index) {
                const auto& buffer = event->RECEIVE.Buffers[index];
                pending->Slices.push_back({buffer.Buffer, buffer.Length});
                pending->TotalBytes += buffer.Length;
            }
            if (pending->TotalBytes > relay->PrecommitMaxPendingBytes -
                    std::min(
                        relay->PendingQuicReceiveBytes,
                        relay->PrecommitMaxPendingBytes)) {
                Activation.store(TqUvActivation::Failed, std::memory_order_release);
                relay->Activation = TqUvActivation::Failed;
                (void)relay->ActivationMutex.Unlock();
                return QUIC_STATUS_OUT_OF_MEMORY;
            }
            relay->PrecommitReceives.push_back(pending);
            if (!Worker->AddPendingBytes(
                    *relay,
                    TqUvPendingDirection::QuicToTcp,
                    pending->TotalBytes)) {
                relay->PrecommitReceives.pop_back();
                Activation.store(TqUvActivation::Failed, std::memory_order_release);
                relay->Activation = TqUvActivation::Failed;
                (void)relay->ActivationMutex.Unlock();
                return QUIC_STATUS_OUT_OF_MEMORY;
            }
            relay->PendingQuicReceiveBytes += pending->TotalBytes;
            if (event->RECEIVE.TotalBufferLength != 0) {
                status = QUIC_STATUS_PENDING;
            }
        } catch (...) {
            Activation.store(TqUvActivation::Failed, std::memory_order_release);
            relay->Activation = TqUvActivation::Failed;
            status = QUIC_STATUS_OUT_OF_MEMORY;
        }
    } else if (event->Type == QUIC_STREAM_EVENT_RECEIVE &&
               phase == TqUvActivation::Active) {
        (void)relay->ActivationMutex.Unlock();
        return TqUvAcceptQuicReceive(relay, stream, *event);
    }
    (void)relay->ActivationMutex.Unlock();
    return status;
}

#if defined(TQ_UNIT_TESTING) && TQ_UNIT_TESTING
bool TqUvRelayWorker::StopForTest() {
    if (IsLoopThread()) {
        return false;
    }
    StopAndJoin();
    return true;
}

bool TqUvRelayWorker::StopRequestedForTest() const noexcept {
    return StopRequested_.load(std::memory_order_acquire);
}

bool TqUvRelayWorker::IsLoopThreadForTest() const noexcept {
    return IsLoopThread();
}

TqUvRegisterCommandState TqUvRelayWorker::RegistrationStateForTest() const {
    std::shared_ptr<RegisterCommand> command;
    {
        std::lock_guard<std::mutex> guard(TestRegistrationMutex_);
        command = TestRegistration_.lock();
    }
    if (!command) {
        return TqUvRegisterCommandState::Completed;
    }
    uv_mutex_lock(&command->Mutex);
    const auto state = command->State;
    uv_mutex_unlock(&command->Mutex);
    return state;
}

TqUvRegistrationTestSnapshot TqUvRelayWorker::RegistrationSnapshotForTest(
    std::uint64_t relayId) const {
    std::lock_guard<std::mutex> guard(RegistrationSnapshotMutex_);
    const auto found = RegistrationSnapshots_.find(relayId);
    return found != RegistrationSnapshots_.end()
        ? found->second
        : TqUvRegistrationTestSnapshot{};
}

void TqUvRelayWorker::SignalRegistrationForTest() {
    std::shared_ptr<RegisterCommand> command;
    {
        std::lock_guard<std::mutex> guard(TestRegistrationMutex_);
        command = TestRegistration_.lock();
    }
    if (!command) {
        return;
    }
    uv_mutex_lock(&command->Mutex);
    uv_cond_broadcast(&command->Condition);
    uv_mutex_unlock(&command->Mutex);
}
#endif

void TqUvRelayWorker::ThreadEntry(void* context) {
    static_cast<TqUvRelayWorker*>(context)->ThreadMain();
}

void TqUvRelayWorker::AsyncEntry(uv_async_t* async) {
    auto* worker = static_cast<TqUvRelayWorker*>(async->data);
    worker->AsyncCallbacks_.fetch_add(1, std::memory_order_relaxed);
    worker->HandleWake();
}

void TqUvRelayWorker::SafetyTimerEntry(uv_timer_t* timer) {
    auto* worker = static_cast<TqUvRelayWorker*>(timer->data);
    const std::uint64_t now = uv_hrtime();
    const std::uint64_t expectedNanos =
        TqUvSafetyTimerIntervalMs * 1000 * 1000;
    const std::uint64_t lagMicros = TqUvLoopLagMicros(
        worker->LastSafetyTimerNanos_, now, expectedNanos);
    if (lagMicros != 0) {
        std::uint64_t observed =
            worker->LoopLagMicros_.load(std::memory_order_relaxed);
        while (observed < lagMicros &&
               !worker->LoopLagMicros_.compare_exchange_weak(
                   observed,
                   lagMicros,
                   std::memory_order_relaxed)) {
        }
    }
    worker->LastSafetyTimerNanos_ = now;
    worker->SafetyTimerCallbacks_.fetch_add(1, std::memory_order_relaxed);
    worker->HandleWake();
#if defined(TQ_UNIT_TESTING) && TQ_UNIT_TESTING
    if (worker->Config_.AfterSafetyTimerForTest != nullptr) {
        worker->Config_.AfterSafetyTimerForTest(*worker);
    }
#endif
}

void TqUvRelayWorker::PrepareEntry(uv_prepare_t* prepare) {
    auto* worker = static_cast<TqUvRelayWorker*>(prepare->data);
    worker->LoopIterations_.fetch_add(1, std::memory_order_relaxed);
}

void TqUvRelayWorker::AsyncClosed(uv_handle_t*) {}

void TqUvRelayWorker::ThreadMain() {
    LoopThreadId_ = std::this_thread::get_id();
#if defined(TQ_UNIT_TESTING) && TQ_UNIT_TESTING
    if (Config_.LoopInitCallsForTest != nullptr) {
        Config_.LoopInitCallsForTest->fetch_add(1, std::memory_order_relaxed);
    }
#endif
    int status = CallLoopInit(&Loop_);
    if (status != 0) {
        ReportReady(status);
        return;
    }

    Async_.data = this;
    status = CallAsyncInit(&Loop_, &Async_, &AsyncEntry);
    if (status != 0) {
        ReportReady(status);
        CallLoopClose(&Loop_);
        return;
    }

    SafetyTimer_.data = this;
    status = uv_timer_init(&Loop_, &SafetyTimer_);
    if (status != 0) {
        CallClose(reinterpret_cast<uv_handle_t*>(&Async_), &AsyncClosed);
        uv_run(&Loop_, UV_RUN_DEFAULT);
        CallLoopClose(&Loop_);
        ReportReady(status);
        return;
    }
    Prepare_.data = this;
    status = uv_prepare_init(&Loop_, &Prepare_);
    if (status != 0) {
        CallClose(reinterpret_cast<uv_handle_t*>(&Async_), &AsyncClosed);
        CallClose(reinterpret_cast<uv_handle_t*>(&SafetyTimer_), &AsyncClosed);
        uv_run(&Loop_, UV_RUN_DEFAULT);
        CallLoopClose(&Loop_);
        ReportReady(status);
        return;
    }
    status = uv_prepare_start(&Prepare_, &PrepareEntry);
    if (status != 0) {
        CallClose(reinterpret_cast<uv_handle_t*>(&Async_), &AsyncClosed);
        CallClose(reinterpret_cast<uv_handle_t*>(&SafetyTimer_), &AsyncClosed);
        CallClose(reinterpret_cast<uv_handle_t*>(&Prepare_), &AsyncClosed);
        uv_run(&Loop_, UV_RUN_DEFAULT);
        CallLoopClose(&Loop_);
        ReportReady(status);
        return;
    }
    uv_unref(reinterpret_cast<uv_handle_t*>(&Prepare_));
    status = uv_timer_start(
        &SafetyTimer_,
        &SafetyTimerEntry,
        TqUvSafetyTimerIntervalMs,
        TqUvSafetyTimerIntervalMs);
    if (status != 0) {
        CallClose(reinterpret_cast<uv_handle_t*>(&Async_), &AsyncClosed);
        CallClose(reinterpret_cast<uv_handle_t*>(&SafetyTimer_), &AsyncClosed);
        uv_prepare_stop(&Prepare_);
        CallClose(reinterpret_cast<uv_handle_t*>(&Prepare_), &AsyncClosed);
        uv_run(&Loop_, UV_RUN_DEFAULT);
        CallLoopClose(&Loop_);
        ReportReady(status);
        return;
    }
    LastSafetyTimerNanos_ = uv_hrtime();

    Running_.store(true, std::memory_order_release);
    Accepting_.store(true, std::memory_order_release);
    ReportReady(0);
    uv_run(&Loop_, UV_RUN_DEFAULT);
    Accepting_.store(false, std::memory_order_release);
    Running_.store(false, std::memory_order_release);
    const int closeStatus = CallLoopClose(&Loop_);
    assert(closeStatus == 0);
    (void)closeStatus;
}

void TqUvRelayWorker::HandleWake() noexcept {
    bool shouldClose = false;
    std::size_t commandCount = 0;
    try {
        {
            std::lock_guard<std::mutex> guard(AdmissionMutex_);
            shouldClose = StopRequested_.load(std::memory_order_acquire);
            commandCount = Queue_.Size();
        }
        for (std::size_t index = 0; index < commandCount; ++index) {
            Command command;
            {
                std::lock_guard<std::mutex> guard(AdmissionMutex_);
                if (!Queue_.TryPop(command)) {
                    break;
                }
            }
            DispatchCommand(command);
        }
    } catch (...) {
        // Never let C++ exceptions cross AsyncEntry/SafetyTimerEntry. Commands
        // not yet popped remain durable in Queue_ for the next timer pass.
    }
    DrainSendCompletionFallbacks();
    RetryPendingQuicSends();
    try {
        for (auto current = Relays_.begin(); current != Relays_.end();) {
            auto relay = current->second;
            ++current;
            if (relay != nullptr &&
                (relay->TerminalStarted ||
                 relay->TerminalTriggerMask.load(std::memory_order_acquire) != 0 ||
                 relay->QuicFinObserved.load(std::memory_order_acquire) ||
                 relay->QuicShutdownObserved.load(std::memory_order_acquire))) {
                TqUvProcessTerminalFactsLocal(*this, *relay);
            }
        }
    } catch (...) {
        // Terminal facts remain durable; the safety timer retries the scan.
    }
    ProcessSignaledStops();
    if (shouldClose) {
        BeginCloseHandles();
    }
}

void TqUvRelayWorker::DrainSendCompletionFallbacks() noexcept {
    auto* pending = SendCompletionFallbackHead_.exchange(
        nullptr, std::memory_order_acq_rel);
    while (pending != nullptr) {
        auto* next = pending->FallbackNext;
        pending->FallbackNext = nullptr;
        TqUvHandleSendComplete(
            *this,
            *pending,
            pending->CompletionCancelled.load(std::memory_order_acquire));
        SendCompletionCommands_.fetch_add(1, std::memory_order_release);
        pending = next;
    }
}

void TqUvRelayWorker::RetryPendingQuicSends() noexcept {
    try {
        for (auto& item : Relays_) {
            if (item.second && !item.second->PendingQuicSendRetries.empty()) {
                TqUvRetryPendingQuicSends(*this, *item.second);
            }
        }
    } catch (...) {
        // The safety timer retries the durable per-relay retry queue.
    }
}

void TqUvRelayWorker::ProcessSignaledStops() noexcept {
    const auto requested = StopRequestSequence_.load(std::memory_order_acquire);
    if (requested == ObservedStopSequence_) {
        return;
    }
    try {
#if defined(TQ_UNIT_TESTING) && TQ_UNIT_TESTING
        if (FailStopScanOnceForTest_) {
            FailStopScanOnceForTest_ = false;
            throw std::bad_alloc{};
        }
#endif
        bool converged = true;
        for (auto current = Relays_.begin(); current != Relays_.end();) {
            // Advance first: a synchronous test close callback may erase the
            // current relay, but unordered_map keeps other iterators valid.
            auto relay = current->second;
            ++current;
            if (relay->StopControl == nullptr ||
                !relay->StopControl->Stop.load(std::memory_order_acquire)) {
                continue;
            }
            TqUvRequestTerminal(*relay, TqUvTerminalTrigger::RuntimeStop);
            TqUvProcessTerminalFactsLocal(*this, *relay);
            RecordRegistrationSnapshotForTest(*relay);
        }
        StopScanPasses_.fetch_add(1, std::memory_order_relaxed);
        if (converged) {
            ObservedStopSequence_ = requested;
        }
    } catch (...) {
        // Never cross the libuv C callback boundary. Since observed is not
        // advanced, the durable control facts are retried on the next tick.
        StopScanFailures_.fetch_add(1, std::memory_order_relaxed);
    }
}

void TqUvRelayWorker::BeginCloseHandles() noexcept {
    if (CloseRequested_.load(std::memory_order_acquire)) {
        return;
    }
    try {
        std::size_t processed = 0;
        for (auto current = Relays_.begin(); current != Relays_.end();) {
#if defined(TQ_UNIT_TESTING) && TQ_UNIT_TESTING
            if (Config_.BeforeCloseRelayForTest != nullptr) {
                Config_.BeforeCloseRelayForTest(processed);
            }
#endif
            // Advance before CloseRelay: test adapters may invoke the close
            // callback synchronously and erase the current relay.
            auto relay = current->second;
            ++current;
            TqUvRequestTerminal(*relay, TqUvTerminalTrigger::RuntimeStop);
            TqUvProcessTerminalFactsLocal(*this, *relay);
            RecordRegistrationSnapshotForTest(*relay);
            ++processed;
        }

        // Runtime stop is a draining state, not permission to abandon relay
        // ownership. Keep async/prepare/timer alive until every relay has
        // observed the real terminal callback and retired all local work.
        if (!Relays_.empty()) {
            return;
        }

        if (!AsyncCloseStarted_) {
            AsyncCloseStarted_ = true;
            try {
                CallClose(
                    reinterpret_cast<uv_handle_t*>(&Async_), &AsyncClosed);
            } catch (...) {
                AsyncCloseStarted_ = false;
                throw;
            }
        }
        if (!PrepareCloseStarted_) {
            PrepareCloseStarted_ = true;
            try {
                // Keep the safety timer active until every unreferenced handle
                // close has been accepted, so a partial close remains retryable.
                CallClose(
                    reinterpret_cast<uv_handle_t*>(&Prepare_), &AsyncClosed);
            } catch (...) {
                PrepareCloseStarted_ = false;
                throw;
            }
        }
        if (!SafetyTimerCloseStarted_) {
            SafetyTimerCloseStarted_ = true;
            try {
                // uv_close stops an active timer. Keeping it active until this
                // call succeeds preserves a retry source after partial close.
                CallClose(
                    reinterpret_cast<uv_handle_t*>(&SafetyTimer_), &AsyncClosed);
            } catch (...) {
                SafetyTimerCloseStarted_ = false;
                throw;
            }
        }
        CloseRequested_.store(true, std::memory_order_release);
    } catch (...) {
        // StopRequested_ remains durable. Already-started closes are marked on
        // their handle/relay and the next timer/wake resumes the remaining set.
    }
}

void TqUvRelayWorker::DispatchCommand(Command& command) {
    if (auto* local = std::get_if<LocalCommand>(&command)) {
        PostLocal(std::move(*local));
        return;
    }
    if (auto* registration =
            std::get_if<std::shared_ptr<RegisterCommand>>(&command)) {
        const auto& item = *registration;
        uv_mutex_lock(&item->Mutex);
        if (item->State == TqUvRegisterCommandState::Cancelled) {
            uv_mutex_unlock(&item->Mutex);
            return;
        }
        assert(item->State == TqUvRegisterCommandState::Queued);
        item->State = TqUvRegisterCommandState::Executing;
        uv_mutex_unlock(&item->Mutex);

        TqUvRelayRegistrationResult result{};
        try {
            result = RegisterRelayWithIdLocal(std::move(item->Registration));
        } catch (...) {
            // Never allow an exception to strand a synchronous waiter.
        }

        uv_mutex_lock(&item->Mutex);
        item->Result = std::move(result);
        item->State = TqUvRegisterCommandState::Completed;
        uv_cond_broadcast(&item->Condition);
        uv_mutex_unlock(&item->Mutex);
        CommandsExecuted_.fetch_add(1, std::memory_order_relaxed);
        return;
    }

    if (auto* completion = std::get_if<SendCompleteCommand>(&command)) {
        auto* operation = completion->Operation;
        if (operation != nullptr) {
            TqUvHandleSendComplete(
                *this,
                *operation,
                operation->CompletionCancelled.load(std::memory_order_acquire));
            SendCompletionCommands_.fetch_add(1, std::memory_order_release);
        }
        return;
    }

}

bool TqUvRelayWorker::Enqueue(Command command) {
    std::lock_guard<std::mutex> guard(AdmissionMutex_);
    if (!Accepting_.load(std::memory_order_acquire)) {
        return false;
    }
#if defined(TQ_UNIT_TESTING) && TQ_UNIT_TESTING
    if (Config_.AfterAdmissionCheckForTest != nullptr) {
        Config_.AfterAdmissionCheckForTest();
    }
#endif
    const auto pushed = Queue_.TryPush(std::move(command));
    if (!pushed.Accepted) {
        return false;
    }
    if (!pushed.ShouldWake) {
        AsyncWakeCoalesced_.fetch_add(1, std::memory_order_relaxed);
        return true;
    }
    (void)WakeLoopUntilAccepted();
    return true;
}

bool TqUvRelayWorker::WakeLoopUntilAccepted() {
    for (std::uint32_t attempt = 0;
         attempt < TqUvAsyncSendAttemptLimit &&
         Running_.load(std::memory_order_acquire);
         ++attempt) {
        if (CallAsyncSend(&Async_) == 0) {
            AsyncWakeSuccesses_.fetch_add(1, std::memory_order_relaxed);
            return true;
        }
        AsyncWakeFailures_.fetch_add(1, std::memory_order_relaxed);
        std::this_thread::yield();
    }
    return false;
}

void TqUvRelayWorker::WakeForDurableFacts() noexcept {
    try {
        (void)WakeLoopUntilAccepted();
    } catch (...) {
        // The safety timer scans durable relay facts without a queued payload.
    }
}

void TqUvRelayWorker::TrackRegistration(
    const std::shared_ptr<RegisterCommand>& command) {
    std::lock_guard<std::mutex> guard(PendingRegistrationMutex_);
    PendingRegistrations_.push_back(command);
}

void TqUvRelayWorker::UntrackRegistration(
    const std::shared_ptr<RegisterCommand>& command) {
    std::lock_guard<std::mutex> guard(PendingRegistrationMutex_);
    PendingRegistrations_.erase(
        std::remove_if(
            PendingRegistrations_.begin(),
            PendingRegistrations_.end(),
            [&](const std::weak_ptr<RegisterCommand>& candidate) {
                const auto item = candidate.lock();
                return !item || item == command;
            }),
        PendingRegistrations_.end());
}

void TqUvRelayWorker::CancelQueuedRegistrations() {
    std::vector<std::shared_ptr<RegisterCommand>> pending;
    {
        std::lock_guard<std::mutex> guard(PendingRegistrationMutex_);
        for (const auto& candidate : PendingRegistrations_) {
            if (auto command = candidate.lock()) {
                pending.push_back(std::move(command));
            }
        }
    }
    for (const auto& command : pending) {
        uv_mutex_lock(&command->Mutex);
        if (command->State == TqUvRegisterCommandState::Queued) {
            command->State = TqUvRegisterCommandState::Cancelled;
            uv_cond_broadcast(&command->Condition);
        }
        uv_mutex_unlock(&command->Mutex);
    }
}

int TqUvRelayWorker::CallLoopInit(uv_loop_t* loop) {
#if defined(TQ_UNIT_TESTING) && TQ_UNIT_TESTING
    if (CallsForTest_ != nullptr) {
        return CallsForTest_->LoopInit(loop);
    }
#endif
    return uv_loop_init(loop);
}

int TqUvRelayWorker::CallLoopClose(uv_loop_t* loop) {
#if defined(TQ_UNIT_TESTING) && TQ_UNIT_TESTING
    if (CallsForTest_ != nullptr) {
        return CallsForTest_->LoopClose(loop);
    }
#endif
    return uv_loop_close(loop);
}

int TqUvRelayWorker::CallAsyncInit(
    uv_loop_t* loop,
    uv_async_t* async,
    uv_async_cb callback) {
#if defined(TQ_UNIT_TESTING) && TQ_UNIT_TESTING
    if (CallsForTest_ != nullptr) {
        return CallsForTest_->AsyncInit(loop, async, callback);
    }
#endif
    return uv_async_init(loop, async, callback);
}

int TqUvRelayWorker::CallAsyncSend(uv_async_t* async) {
#if defined(TQ_UNIT_TESTING) && TQ_UNIT_TESTING
    if (CallsForTest_ != nullptr) {
        return CallsForTest_->AsyncSend(async);
    }
#endif
    return uv_async_send(async);
}

int TqUvRelayWorker::CallTcpInit(uv_loop_t* loop, uv_tcp_t* tcp) {
#if defined(TQ_UNIT_TESTING) && TQ_UNIT_TESTING
    if (CallsForTest_ != nullptr) {
        return CallsForTest_->TcpInit(loop, tcp);
    }
#endif
    return uv_tcp_init(loop, tcp);
}

int TqUvRelayWorker::CallTcpOpen(uv_tcp_t* tcp, uv_os_sock_t socket) {
#if defined(TQ_UNIT_TESTING) && TQ_UNIT_TESTING
    if (CallsForTest_ != nullptr) {
        return CallsForTest_->TcpOpen(tcp, socket);
    }
#endif
    return uv_tcp_open(tcp, socket);
}

int TqUvRelayWorker::CallReadStart(
    uv_stream_t* stream,
    uv_alloc_cb allocate,
    uv_read_cb read) {
#if defined(TQ_UNIT_TESTING) && TQ_UNIT_TESTING
    if (CallsForTest_ != nullptr) {
        return CallsForTest_->ReadStart(stream, allocate, read);
    }
#endif
    return uv_read_start(stream, allocate, read);
}

int TqUvRelayWorker::CallReadStop(uv_stream_t* stream) {
#if defined(TQ_UNIT_TESTING) && TQ_UNIT_TESTING
    if (CallsForTest_ != nullptr) {
        return CallsForTest_->ReadStop(stream);
    }
#endif
    return uv_read_stop(stream);
}

int TqUvRelayWorker::CallWrite(
    uv_write_t* request,
    uv_stream_t* stream,
    const uv_buf_t buffers[],
    unsigned int bufferCount,
    uv_write_cb complete) {
#if defined(TQ_UNIT_TESTING) && TQ_UNIT_TESTING
    if (CallsForTest_ != nullptr) {
        return CallsForTest_->Write(
            request, stream, buffers, bufferCount, complete);
    }
#endif
    return uv_write(request, stream, buffers, bufferCount, complete);
}

int TqUvRelayWorker::CallShutdown(
    uv_shutdown_t* request,
    uv_stream_t* stream,
    uv_shutdown_cb complete) {
#if defined(TQ_UNIT_TESTING) && TQ_UNIT_TESTING
    if (CallsForTest_ != nullptr) {
        return CallsForTest_->Shutdown(request, stream, complete);
    }
#endif
    return uv_shutdown(request, stream, complete);
}

void TqUvRelayWorker::CallClose(uv_handle_t* handle, uv_close_cb callback) {
#if defined(TQ_UNIT_TESTING) && TQ_UNIT_TESTING
    if (CallsForTest_ != nullptr) {
        CallsForTest_->Close(handle, callback);
        return;
    }
#endif
    uv_close(handle, callback);
}

void TqUvRelayWorker::PublishActiveRelay(TqUvRelayState& relay) noexcept {
    if (relay.MetricsActive) {
        return;
    }
    relay.MetricsActive = true;
    ActiveRelays_.fetch_add(1, std::memory_order_relaxed);
}

void TqUvRelayWorker::EraseRelay(std::uint64_t relayId) noexcept {
    const auto found = Relays_.find(relayId);
    if (found == Relays_.end()) {
        return;
    }
    auto& relay = *found->second;
    if (relay.MetricsActive) {
        relay.MetricsActive = false;
        ActiveRelays_.fetch_sub(1, std::memory_order_relaxed);
    }
    (void)CompletePendingBytes(
        relay,
        TqUvPendingDirection::QuicToTcp,
        relay.AccountedQuicToTcpBytes.load(std::memory_order_relaxed));
    (void)CompletePendingBytes(
        relay,
        TqUvPendingDirection::TcpToQuic,
        relay.AccountedTcpToQuicBytes.load(std::memory_order_relaxed));
    Relays_.erase(found);
}

void TqUvRelayWorker::CloseRelay(
    const std::shared_ptr<TqUvRelayState>& relay) {
    if (!relay || !relay->TcpHandleInitialized || relay->TcpClosePending ||
        relay->TcpCloseCompleted) {
        return;
    }
    if (relay->TcpReadStarted) {
        (void)CallReadStop(reinterpret_cast<uv_stream_t*>(&relay->TcpHandle));
        relay->TcpReadStarted = false;
    }
    relay->TcpClosePending = true;
    try {
        CallClose(
            reinterpret_cast<uv_handle_t*>(&relay->TcpHandle),
            &TqUvOnTcpClosed);
    } catch (...) {
        relay->TcpClosePending = false;
        throw;
    }
}

void TqUvRelayWorker::SettlePrecommit(
    const std::shared_ptr<TqUvRelayState>& relay,
    bool drain) {
    if (!relay || relay->PrecommitSettled) {
        return;
    }
    relay->PrecommitSettled = true;
    relay->Binding->PrecommitSettled.store(true, std::memory_order_release);
    if (drain) {
        ++relay->PrecommitDrainCount;
        return;
    }
    ++relay->PrecommitDiscardCount;
    std::uint64_t discarded = 0;
    for (const auto& pending : relay->PrecommitReceives) {
        discarded += pending->TotalBytes - pending->CompletedBytes;
        pending->Settled.store(true, std::memory_order_release);
    }
    relay->PrecommitReceives.clear();
    (void)CompletePendingBytes(
        *relay, TqUvPendingDirection::QuicToTcp, discarded);
    relay->PendingQuicReceiveBytes = 0;
    if (discarded != 0 && relay->StreamOwner != nullptr) {
        auto lease = relay->StreamOwner->TryAcquireReceiveApi();
        if (lease && lease.Stream() != nullptr) {
            lease.Stream()->ReceiveComplete(discarded);
        }
    }
}

void TqUvRelayWorker::RecordRegistrationSnapshotForTest(
    const TqUvRelayState& relay) {
#if defined(TQ_UNIT_TESTING) && TQ_UNIT_TESTING
    try {
        std::lock_guard<std::mutex> guard(RegistrationSnapshotMutex_);
        RegistrationSnapshots_[relay.RelayId] = {
            relay.Activation,
            relay.SocketOwnership,
            relay.PrecommitSettled,
            relay.PrecommitDrainCount,
            relay.PrecommitDiscardCount,
        };
    } catch (...) {
        // Diagnostics must never break the production ownership transaction.
    }
#else
    (void)relay;
#endif
}

void TqUvOnTcpClosed(uv_handle_t* handle) {
    auto* relay = static_cast<TqUvRelayState*>(handle->data);
    if (relay == nullptr || relay->Worker == nullptr) {
        return;
    }
    auto* worker = relay->Worker;
    relay->TcpClosePending = false;
    relay->TcpCloseCompleted = true;
    if (relay->SocketOwnership != TqUvSocketOwnership::CallerOwned) {
        relay->SocketOwnership = TqUvSocketOwnership::Closed;
    }
    worker->RecordRegistrationSnapshotForTest(*relay);
    if (!relay->TerminalStarted) {
        worker->EraseRelay(relay->RelayId);
        return;
    }
    TqUvCheckTerminalConvergence(*worker, *relay);
}

void TqUvOnTcpAlloc(
    uv_handle_t* handle,
    std::size_t suggested,
    uv_buf_t* buffer) {
    if (buffer == nullptr) {
        return;
    }
    *buffer = uv_buf_init(nullptr, 0);
    auto* relay = handle != nullptr
        ? static_cast<TqUvRelayState*>(handle->data)
        : nullptr;
    if (relay == nullptr) {
        return;
    }
    TqBufferAcquireFailure failure =
        TqBufferAcquireFailure::AllocationFailure;
    if (TqUvAllocateTcpReadBuffer(
            *relay, suggested, *buffer, &failure)) {
        relay->TcpReadAcquireFailure = TqBufferAcquireFailure::None;
        return;
    }
    relay->TcpReadAcquireFailure =
        failure == TqBufferAcquireFailure::None
            ? TqBufferAcquireFailure::AllocationFailure
            : failure;
}

void TqUvOnTcpRead(
    uv_stream_t* stream,
    ssize_t received,
    const uv_buf_t* buffer) {
    auto* relay = stream != nullptr
        ? static_cast<TqUvRelayState*>(stream->data)
        : nullptr;
    if (relay == nullptr || relay->Worker == nullptr || buffer == nullptr) {
        return;
    }
    TqUvHandleTcpRead(*relay->Worker, *relay, received, *buffer);
}

#if defined(TQ_UNIT_TESTING) && TQ_UNIT_TESTING
int TqUvRelayWorker::CallTcpInitForTest(uv_loop_t* loop, uv_tcp_t* tcp) {
    return CallTcpInit(loop, tcp);
}

int TqUvRelayWorker::CallTcpOpenForTest(uv_tcp_t* tcp, uv_os_sock_t socket) {
    return CallTcpOpen(tcp, socket);
}

int TqUvRelayWorker::CallReadStartForTest(
    uv_stream_t* stream,
    uv_alloc_cb allocate,
    uv_read_cb read) {
    return CallReadStart(stream, allocate, read);
}

int TqUvRelayWorker::CallWriteForTest(
    uv_write_t* request,
    uv_stream_t* stream,
    const uv_buf_t buffers[],
    unsigned int bufferCount,
    uv_write_cb complete) {
    return CallWrite(request, stream, buffers, bufferCount, complete);
}

int TqUvRelayWorker::CallShutdownForTest(
    uv_shutdown_t* request,
    uv_stream_t* stream,
    uv_shutdown_cb complete) {
    return CallShutdown(request, stream, complete);
}

std::shared_ptr<TqUvRelayState> TqUvRelayWorker::RelayForTest(
    std::uint64_t relayId) const {
    const auto found = Relays_.find(relayId);
    return found != Relays_.end() ? found->second : nullptr;
}
#endif

void TqUvRelayWorker::StopAndJoin() {
    if (!ThreadCreated_) {
        if (LifecycleSyncInitialized_) {
            uv_cond_destroy(&LifecycleCondition_);
            uv_mutex_destroy(&LifecycleMutex_);
            LifecycleSyncInitialized_ = false;
        }
        return;
    }

    if (Running_.load(std::memory_order_acquire)) {
        {
            std::lock_guard<std::mutex> guard(AdmissionMutex_);
            Accepting_.store(false, std::memory_order_release);
            CancelQueuedRegistrations();
            StopRequested_.store(true, std::memory_order_release);
        }
        (void)WakeLoopUntilAccepted();
    }
    uv_thread_join(&Thread_);
    ThreadCreated_ = false;
    if (LifecycleSyncInitialized_) {
        uv_cond_destroy(&LifecycleCondition_);
        uv_mutex_destroy(&LifecycleMutex_);
        LifecycleSyncInitialized_ = false;
    }
}

void TqUvRelayWorker::ReportReady(int status) {
    uv_mutex_lock(&LifecycleMutex_);
    ReadyStatus_ = status;
    ReadyReported_ = true;
    uv_cond_broadcast(&LifecycleCondition_);
    uv_mutex_unlock(&LifecycleMutex_);
}

bool TqUvRelayWorker::IsLoopThread() const noexcept {
    return LoopThreadId_ != std::thread::id{} &&
           LoopThreadId_ == std::this_thread::get_id();
}

TqUvRelayRuntime& TqUvRelayRuntime::Instance() {
    static TqUvRelayRuntime runtime;
    return runtime;
}

TqUvRelayRuntime::~TqUvRelayRuntime() {
    for (auto& worker : Workers_) {
        worker->StopForStartupRollback();
    }
}

bool TqUvRelayRuntime::Start(const TqTuningConfig& tuning) {
    std::lock_guard<std::mutex> guard(Lock_);
    if (State_ == TqUvRelayRuntimeState::Running) {
        return true;
    }
    if (State_ != TqUvRelayRuntimeState::Stopped) {
        return false;
    }

#if defined(TQ_UNIT_TESTING) && TQ_UNIT_TESTING
    const auto allocatorInstaller = AllocatorInstallerForTest_ != nullptr
        ? AllocatorInstallerForTest_
        : &TqUvInstallAllocator;
    LoopInitCallsForTest_.store(0, std::memory_order_relaxed);
    StartedWorkersForTest_ = 0;
    RolledBackWorkersForTest_ = 0;
#else
    const auto allocatorInstaller = &TqUvInstallAllocator;
#endif
    if (!allocatorInstaller()) {
        AllocatorInstalledBeforeLoop_ = false;
        return false;
    }
    AllocatorInstalledBeforeLoop_ = true;
    State_ = TqUvRelayRuntimeState::Starting;

#if defined(TQ_UNIT_TESTING) && TQ_UNIT_TESTING
    const auto* calls = CallsForTest_ != nullptr
        ? CallsForTest_
        : &TqUvProductionCalls();
#endif
    const std::uint32_t workerCount =
        std::max<std::uint32_t>(1, tuning.RelayWorkerCount);
    std::vector<std::unique_ptr<TqUvRelayWorker>> staged;
    staged.reserve(workerCount);
    for (std::uint32_t index = 0; index < workerCount; ++index) {
        auto config = MakeWorkerConfig(
            tuning,
            index
#if defined(TQ_UNIT_TESTING) && TQ_UNIT_TESTING
            , calls,
            &LoopInitCallsForTest_
#endif
        );
        auto worker = std::unique_ptr<TqUvRelayWorker>(
            new TqUvRelayWorker(std::move(config)));
        if (!worker->StartAndWaitReady()) {
            for (auto& started : staged) {
                started->StopForStartupRollback();
#if defined(TQ_UNIT_TESTING) && TQ_UNIT_TESTING
                ++RolledBackWorkersForTest_;
#endif
            }
            State_ = TqUvRelayRuntimeState::Stopped;
            return false;
        }
#if defined(TQ_UNIT_TESTING) && TQ_UNIT_TESTING
        ++StartedWorkersForTest_;
#endif
        staged.push_back(std::move(worker));
    }
    Workers_ = std::move(staged);
    NextWorker_ = 0;
    State_ = TqUvRelayRuntimeState::Running;
    return true;
}

TqUvRelayWorker* TqUvRelayRuntime::PickWorker() {
    std::lock_guard<std::mutex> guard(Lock_);
    if (State_ != TqUvRelayRuntimeState::Running || Workers_.empty()) {
        return nullptr;
    }
    auto* worker = Workers_[NextWorker_ % Workers_.size()].get();
    NextWorker_ = (NextWorker_ + 1) % Workers_.size();
    return worker;
}

bool TqUvRelayRuntime::StopRelay(
    const std::shared_ptr<const TqUvRelayCommittedState>& committed) {
    if (committed == nullptr || committed->Control == nullptr) {
        return false;
    }
    TqUvRelayWorker* worker = nullptr;
    {
        std::lock_guard<std::mutex> guard(Lock_);
        if (State_ != TqUvRelayRuntimeState::Running ||
            committed->WorkerIndex >= Workers_.size()) {
            return false;
        }
        auto* candidate = Workers_[committed->WorkerIndex].get();
        if (reinterpret_cast<std::uintptr_t>(candidate) !=
            committed->WorkerIdentity) {
            return false;
        }
        worker = candidate;
    }
    return worker->AcceptStop(committed);
}

std::vector<TqUvRelayWorkerSnapshot> TqUvRelayRuntime::SnapshotWorkers() const {
    std::lock_guard<std::mutex> guard(Lock_);
    std::vector<TqUvRelayWorkerSnapshot> snapshots;
    snapshots.reserve(Workers_.size());
    for (const auto& worker : Workers_) {
        snapshots.push_back(worker->Snapshot());
    }
    return snapshots;
}

#if defined(TQ_UNIT_TESTING) && TQ_UNIT_TESTING
void TqUvRelayRuntime::StopForTest() {
    std::vector<std::unique_ptr<TqUvRelayWorker>> workers;
    {
        std::lock_guard<std::mutex> guard(Lock_);
        workers.swap(Workers_);
        State_ = TqUvRelayRuntimeState::Stopped;
        NextWorker_ = 0;
    }
    for (auto& worker : workers) {
        worker->StopForStartupRollback();
    }
}

std::size_t TqUvRelayRuntime::WorkerCountForTest() const {
    std::lock_guard<std::mutex> guard(Lock_);
    return Workers_.size();
}

bool TqUvRelayRuntime::AllocatorInstalledBeforeLoopForTest() const {
    std::lock_guard<std::mutex> guard(Lock_);
    return AllocatorInstalledBeforeLoop_;
}

std::uint64_t TqUvRelayRuntime::LoopInitCallsForTest() const {
    return LoopInitCallsForTest_.load(std::memory_order_relaxed);
}

std::uint64_t TqUvRelayRuntime::StartedWorkersForTest() const {
    std::lock_guard<std::mutex> guard(Lock_);
    return StartedWorkersForTest_;
}

std::uint64_t TqUvRelayRuntime::RolledBackWorkersForTest() const {
    std::lock_guard<std::mutex> guard(Lock_);
    return RolledBackWorkersForTest_;
}

void TqUvRelayRuntime::SetAllocatorInstallerForTest(
    AllocatorInstaller installer) {
    std::lock_guard<std::mutex> guard(Lock_);
    assert(State_ == TqUvRelayRuntimeState::Stopped);
    AllocatorInstallerForTest_ = installer;
}

void TqUvRelayRuntime::SetCallAdapterForTest(const TqUvCallAdapter* calls) {
    std::lock_guard<std::mutex> guard(Lock_);
    assert(State_ == TqUvRelayRuntimeState::Stopped);
    CallsForTest_ = calls;
}

void TqUvRelayRuntime::ResetTestHooksForTest() {
    std::lock_guard<std::mutex> guard(Lock_);
    assert(State_ == TqUvRelayRuntimeState::Stopped);
    AllocatorInstallerForTest_ = nullptr;
    CallsForTest_ = nullptr;
}
#endif

TqUvRelayWorkerConfig TqUvRelayRuntime::MakeWorkerConfig(
    const TqTuningConfig& tuning,
    std::uint32_t index
#if defined(TQ_UNIT_TESTING) && TQ_UNIT_TESTING
    , const TqUvCallAdapter* calls,
    std::atomic<std::uint64_t>* loopInitCalls
#endif
) {
    TqUvRelayWorkerConfig config{};
    config.WorkerIndex = index;
    config.QueueCapacity =
        std::max<std::size_t>(1, tuning.RelayEventQueueCapacity);
#if defined(TQ_UNIT_TESTING) && TQ_UNIT_TESTING
    config.Calls = calls;
    config.LoopInitCallsForTest = loopInitCalls;
#endif
    return config;
}
