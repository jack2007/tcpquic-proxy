#include "stream_lifetime.h"

#include <new>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <unordered_map>
#include <utility>


namespace {

std::mutex g_terminalRetentionLock;
struct TerminalRetention {
    std::shared_ptr<TqStreamLifetime> Owner;
    std::shared_ptr<TqTerminalLedger> Ledger;
    std::chrono::steady_clock::time_point Since;
};
std::unordered_map<TqStreamLifetime*, TerminalRetention> g_terminalRetentions;

struct SendCompletionRetention {
    std::shared_ptr<TqStreamLifetime> Owner;
    TqStreamLifetime::RouteSnapshot Route;
    void* DeliveredContext{nullptr};
    std::function<void()> Cleanup;
    std::chrono::steady_clock::time_point Since{std::chrono::steady_clock::now()};
};

std::mutex g_sendCompletionLock;
std::unordered_map<void*, SendCompletionRetention> g_sendCompletions;
std::unordered_map<void*, TqStreamLifetime*> g_claimedSendCompletions;
std::atomic<uint64_t> g_terminalApiSuppressedCount{0};
std::atomic<uint64_t> g_nextSendNonce{1};
std::atomic<uint64_t> g_sendCompletionPreSubmitRollbacks{0};
std::atomic<uint64_t> g_sendCompletionUnknownClaims{0};
std::atomic<uint64_t> g_sendCompletionDuplicateClaims{0};
std::atomic<uint64_t> g_detachedOwnerDestroyCount{0};
#if defined(TQ_UNIT_TESTING)
std::atomic<bool> g_failNextRegisterSendCompletion{false};
std::atomic<uint64_t> g_nextTestTerminalId{1};
std::mutex g_terminalSnapshotHookLock;
std::function<void()> g_beforeTerminalSnapshotHook;
#endif
thread_local TqStreamCallbackTarget* g_activeCallbackTarget = nullptr;

bool SameTerminalIdentity(
    const TqTerminalIdentity& left,
    const TqTerminalIdentity& right) noexcept {
    return left.StreamId == right.StreamId &&
           left.TunnelId == right.TunnelId &&
           left.ConnectionId == right.ConnectionId &&
           left.ConnectionGeneration == right.ConnectionGeneration &&
           left.Role == right.Role && left.Backend == right.Backend;
}

QUIC_STATUS QUIC_API EmergencyAcceptedCallback(
    HQUIC stream,
    void*,
    QUIC_STREAM_EVENT* event) {
    if (event != nullptr && event->Type == QUIC_STREAM_EVENT_SHUTDOWN_COMPLETE) {
        MsQuic->StreamClose(stream);
    } else {
        (void)MsQuic->StreamShutdown(
            stream, QUIC_STREAM_SHUTDOWN_FLAG_ABORT, 0);
    }
    return QUIC_STATUS_SUCCESS;
}

} // namespace

TqStreamLifetime::TqStreamLifetime(
    Phase phase,
    std::shared_ptr<Target> initialTarget) noexcept
    : Phase_(phase), Target_(std::move(initialTarget)) {}

TqStreamLifetime::~TqStreamLifetime() noexcept {
    MsQuicStream* stream = nullptr;
    std::vector<void*> completionKeys;
    bool detachedStreamForTest = false;
    {
        std::lock_guard<std::mutex> guard(ControlMutex_);
        stream = std::exchange(Stream_, nullptr);
        detachedStreamForTest = DetachedStreamForTest_;
        TerminalPhase_ = TerminalPhase::Closed;
        Phase_ = Phase::Closed;
        Target_.reset();
        completionKeys.reserve(SendKeyEnvelopes_.size());
        for (const auto& envelope : SendKeyEnvelopes_) {
            completionKeys.push_back(envelope.get());
        }
    }
    {
        std::lock_guard<std::mutex> guard(g_sendCompletionLock);
        for (void* key : completionKeys) {
            g_claimedSendCompletions.erase(key);
        }
    }
    if (detachedStreamForTest) {
        g_detachedOwnerDestroyCount.fetch_add(1, std::memory_order_relaxed);
    } else {
        delete stream;
    }
}

bool TqStreamLifetime::InstallStream(MsQuicStream* stream) noexcept {
    if (stream == nullptr || !stream->IsValid() || stream->CleanUpMode != CleanUpManual) {
        return false;
    }
    std::lock_guard<std::mutex> guard(ControlMutex_);
    if (Stream_ != nullptr || Phase_ == Phase::Closed) {
        return false;
    }
    Stream_ = stream;
    return true;
}

std::shared_ptr<TqStreamLifetime> TqStreamLifetime::OpenOutgoing(
    const MsQuicConnection& connection,
    QUIC_STREAM_OPEN_FLAGS flags,
    std::shared_ptr<TqStreamLifetime::Target> initialTarget,
    TqTerminalIdentity identity,
    uint32_t watchdogSeconds) noexcept {
    auto owner = std::shared_ptr<TqStreamLifetime>(
        new (std::nothrow) TqStreamLifetime(Phase::CreatedNotStarted, std::move(initialTarget)));
    if (!owner) {
        return nullptr;
    }
    owner->BindTerminalIdentity(identity, watchdogSeconds);
    if (owner->TerminalLedger() == nullptr) {
        return nullptr;
    }
    owner->RetainUntilTerminal();
    auto* stream = new (std::nothrow) MsQuicStream(
        connection, flags, CleanUpManual, Callback, owner.get());
    if (!owner->InstallStream(stream)) {
        delete stream;
        owner->ReleaseTerminalRetention();
        return nullptr;
    }
    return owner;
}

std::shared_ptr<TqStreamLifetime> TqStreamLifetime::AdoptAccepted(
    HQUIC rawStream,
    std::shared_ptr<TqStreamLifetime::Target> initialTarget,
    TqTerminalIdentity identity,
    uint32_t watchdogSeconds) noexcept {
    if (rawStream == nullptr) {
        return nullptr;
    }
    auto owner = std::shared_ptr<TqStreamLifetime>(
        new (std::nothrow) TqStreamLifetime(Phase::Started, std::move(initialTarget)));
    if (!owner) {
        RejectAccepted(rawStream);
        return nullptr;
    }
    owner->BindTerminalIdentity(identity, watchdogSeconds);
    if (owner->TerminalLedger() == nullptr) {
        RejectAccepted(rawStream);
        return nullptr;
    }
    // accepted stream 已经启动；在安装 callback 前先建立 terminal retention。
    owner->RetainUntilTerminal();
    auto* stream = new (std::nothrow) MsQuicStream(
        rawStream, CleanUpManual, Callback, owner.get());
    if (!owner->InstallStream(stream)) {
        delete stream;
        owner->ReleaseTerminalRetention();
        RejectAccepted(rawStream);
        return nullptr;
    }
    return owner;
}

#if defined(TQ_UNIT_TESTING)
std::shared_ptr<TqStreamLifetime> TqStreamLifetime::CreateForTest(
    Phase phase,
    std::shared_ptr<Target> initialTarget) {
    auto owner = std::shared_ptr<TqStreamLifetime>(
        new TqStreamLifetime(phase, std::move(initialTarget)));
    if (phase == Phase::Starting || phase == Phase::Started) {
        const uint64_t id = g_nextTestTerminalId.fetch_add(1, std::memory_order_relaxed);
        owner->BindTerminalIdentity(
            TqTerminalIdentity{
                id, id, id, id,
                TqTunnelRole::ClientOpen, TqRelayBackendType::LinuxWorker},
            5);
        owner->RetainUntilTerminal();
    }
    return owner;
}

bool TqStreamLifetime::InstallStreamForTest(MsQuicStream* stream) noexcept {
    return InstallStream(stream);
}

void TqStreamLifetime::ReleaseStreamForTest() noexcept {
    std::lock_guard<std::mutex> guard(ControlMutex_);
    Stream_ = nullptr;
}

bool TqStreamLifetime::InstallDetachedStreamForTest(MsQuicStream* stream) noexcept {
    if (stream == nullptr) {
        return false;
    }
    std::lock_guard<std::mutex> guard(ControlMutex_);
    if (Stream_ != nullptr || Phase_ == Phase::Closed) {
        return false;
    }
    stream->CleanUpMode = CleanUpManual;
    stream->Callback = Callback;
    stream->Context = this;
    Stream_ = stream;
    DetachedStreamForTest_ = true;
    return true;
}

void* TqStreamLifetime::TargetContextForTest() const noexcept {
    std::lock_guard<std::mutex> guard(ControlMutex_);
    return Target_ != nullptr ? Target_->ContextForTest() : nullptr;
}

void TqStreamLifetime::SetFailNextRegisterSendCompletionForTest(bool fail) noexcept {
    g_failNextRegisterSendCompletion.store(fail, std::memory_order_release);
}

void TqStreamLifetime::ResetTestDetachedOwnerDestroyCountForTest() noexcept {
    g_detachedOwnerDestroyCount.store(0, std::memory_order_relaxed);
}

uint64_t TqStreamLifetime::TestDetachedOwnerDestroyCountForTest() noexcept {
    return g_detachedOwnerDestroyCount.load(std::memory_order_relaxed);
}

bool TqStreamLifetime::SendDirectionCompleteForTest() const noexcept {
    std::lock_guard<std::mutex> guard(ControlMutex_);
    return SendDirectionComplete_;
}

uint64_t TqStreamLifetime::CancelOnLossErrorCodeForTest() const noexcept {
    std::lock_guard<std::mutex> guard(ControlMutex_);
    return CancelOnLossErrorCode_;
}

void TqStreamLifetime::SetShutdownHookForTest(ShutdownHookForTest hook) noexcept {
    std::lock_guard<std::mutex> guard(ControlMutex_);
    ShutdownHookForTest_ = std::move(hook);
}

void TqStreamLifetime::SetBeforeTerminalLedgerRecordHookForTest(
    BeforeTerminalLedgerRecordHookForTest hook) noexcept {
    std::lock_guard<std::mutex> guard(ControlMutex_);
    BeforeTerminalLedgerRecordHookForTest_ = std::move(hook);
}

bool TqStreamLifetime::TerminalRetryOwnedForTest() const noexcept {
    std::lock_guard<std::mutex> guard(ControlMutex_);
    return TerminalRetryOwned_;
}
#endif

#if defined(TQ_UNIT_TESTING) || defined(TCPQUIC_TUNNEL_TESTING)
QUIC_STATUS TqStreamLifetime::DispatchForTest(QUIC_STREAM_EVENT* event) noexcept {
    MsQuicStream* stream = nullptr;
    {
        std::lock_guard<std::mutex> guard(ControlMutex_);
        stream = Stream_;
    }
    return Dispatch(stream, event);
}
#endif

TqStreamLifetime::Phase TqStreamLifetime::GetPhase() const noexcept {
    std::lock_guard<std::mutex> guard(ControlMutex_);
    return Phase_;
}

uint64_t TqStreamLifetime::RouteGeneration() const noexcept {
    std::lock_guard<std::mutex> guard(ControlMutex_);
    return RouteGeneration_;
}

bool TqStreamLifetime::BeginStart() noexcept {
    {
        std::lock_guard<std::mutex> guard(ControlMutex_);
        if (Phase_ != Phase::CreatedNotStarted) {
            return false;
        }
        Phase_ = Phase::Starting;
    }
    RetainUntilTerminal();
    return true;
}

bool TqStreamLifetime::CompleteStart(QUIC_STATUS status) noexcept {
    if (QUIC_SUCCEEDED(status)) {
        std::lock_guard<std::mutex> guard(ControlMutex_);
        if (Phase_ == Phase::Starting) {
            Phase_ = Phase::Started;
            return true;
        }
        return Phase_ == Phase::Started || Phase_ == Phase::TerminalPublished;
    }
    return static_cast<bool>(PublishStartFailureAndTakeTarget(status).TargetOwner);
}

TqStreamLifetime::ApiLease TqStreamLifetime::TryAcquireApi() noexcept {
    std::lock_guard<std::mutex> guard(ControlMutex_);
    if (Stream_ == nullptr ||
        DetachedStreamForTest_ ||
        (Phase_ != Phase::Starting && Phase_ != Phase::Started)) {
        return {};
    }
    return ApiLease(shared_from_this());
}

TqStreamLifetime::ApiLease TqStreamLifetime::TryAcquireReceiveApi() noexcept {
    std::lock_guard<std::mutex> guard(ControlMutex_);
    if (Stream_ == nullptr ||
        (Phase_ != Phase::Starting && Phase_ != Phase::Started)) {
        if (Phase_ == Phase::TerminalPublished || Phase_ == Phase::Closed) {
            g_terminalApiSuppressedCount.fetch_add(1, std::memory_order_relaxed);
        }
        return {};
    }
#if defined(TQ_UNIT_TESTING)
    if (DenyReceiveApiLeasesForTest_ > 0) {
        --DenyReceiveApiLeasesForTest_;
        return {};
    }
#endif
    return ApiLease(shared_from_this());
}

#if defined(TQ_UNIT_TESTING)
void TqStreamLifetime::DenyReceiveApiLeasesForTest(uint32_t count) noexcept {
    std::lock_guard<std::mutex> guard(ControlMutex_);
    DenyReceiveApiLeasesForTest_ = count;
}
#endif

TqStreamLifetime::ApiLease TqStreamLifetime::TryAcquireSendApi() noexcept {
    return TryAcquireReceiveApi();
}

bool TqStreamLifetime::ReceiveDirectionAborted() const noexcept {
    std::lock_guard<std::mutex> guard(ControlMutex_);
    return SubmittedReceiveAbort_;
}

void* TqStreamLifetime::RegisterSendCompletion(
    void* deliveredContext,
    std::function<void()> completionCleanup) noexcept {
#if defined(TQ_UNIT_TESTING)
    if (g_failNextRegisterSendCompletion.exchange(false, std::memory_order_acq_rel)) {
        return nullptr;
    }
#endif
    RouteSnapshot route{};
    void* clientContext = nullptr;
    {
        std::lock_guard<std::mutex> guard(ControlMutex_);
        if ((Phase_ != Phase::Starting && Phase_ != Phase::Started) || !Target_) {
            return nullptr;
        }
        route.TargetOwner = Target_;
        route.Generation = RouteGeneration_;
        auto envelope = std::unique_ptr<uint64_t>(new (std::nothrow) uint64_t(
            g_nextSendNonce.fetch_add(1, std::memory_order_relaxed)));
        if (!envelope) {
            return nullptr;
        }
        clientContext = envelope.get();
        SendKeyEnvelopes_.push_back(std::move(envelope));
    }
    std::lock_guard<std::mutex> guard(g_sendCompletionLock);
    const bool inserted = g_sendCompletions.emplace(
        clientContext,
        SendCompletionRetention{
            shared_from_this(),
            std::move(route),
            deliveredContext,
            std::move(completionCleanup),
            std::chrono::steady_clock::now()}).second;
    return inserted ? clientContext : nullptr;
}

TqStreamLifetime::SendCompletionReservation TqStreamLifetime::ReserveSendCompletion(
    void* deliveredContext,
    std::function<void()> completionCleanup) noexcept {
    void* key = RegisterSendCompletion(deliveredContext, std::move(completionCleanup));
    if (key == nullptr) {
        return {};
    }
    return SendCompletionReservation(shared_from_this(), key);
}

TqStreamLifetime::SendCompletionReservation::SendCompletionReservation(
    std::shared_ptr<TqStreamLifetime> owner,
    void* key) noexcept
    : Owner_(std::move(owner)), Key_(key), Armed_(Owner_ != nullptr && Key_ != nullptr) {}

TqStreamLifetime::SendCompletionReservation::SendCompletionReservation(
    SendCompletionReservation&& other) noexcept
    : Owner_(std::move(other.Owner_)), Key_(other.Key_), Armed_(other.Armed_) {
    other.Key_ = nullptr;
    other.Armed_ = false;
}

TqStreamLifetime::SendCompletionReservation&
TqStreamLifetime::SendCompletionReservation::operator=(
    SendCompletionReservation&& other) noexcept {
    if (this == &other) {
        return *this;
    }
    if (Armed_ && Owner_ != nullptr && Key_ != nullptr) {
        if (Owner_->CancelSendCompletion(Key_)) {
            g_sendCompletionPreSubmitRollbacks.fetch_add(1, std::memory_order_relaxed);
        }
    }
    Owner_ = std::move(other.Owner_);
    Key_ = other.Key_;
    Armed_ = other.Armed_;
    other.Key_ = nullptr;
    other.Armed_ = false;
    return *this;
}

TqStreamLifetime::SendCompletionReservation::~SendCompletionReservation() noexcept {
    if (Armed_ && Owner_ != nullptr && Key_ != nullptr) {
        if (Owner_->CancelSendCompletion(Key_)) {
            g_sendCompletionPreSubmitRollbacks.fetch_add(1, std::memory_order_relaxed);
        }
    }
}

void TqStreamLifetime::SendCompletionReservation::Dismiss() noexcept {
    Armed_ = false;
    Key_ = nullptr;
    Owner_.reset();
}

bool TqStreamLifetime::SendCompletionReservation::Cancel() noexcept {
    if (!Armed_ || Owner_ == nullptr || Key_ == nullptr) {
        Armed_ = false;
        Key_ = nullptr;
        Owner_.reset();
        return false;
    }
    const bool canceled = Owner_->CancelSendCompletion(Key_);
    if (canceled) {
        g_sendCompletionPreSubmitRollbacks.fetch_add(1, std::memory_order_relaxed);
    }
    Armed_ = false;
    Key_ = nullptr;
    Owner_.reset();
    return canceled;
}

void TqStreamLifetime::RejectAccepted(HQUIC rawStream) noexcept {
    if (rawStream == nullptr || MsQuic == nullptr) {
        return;
    }
    MsQuic->SetCallbackHandler(
        rawStream,
        reinterpret_cast<void*>(EmergencyAcceptedCallback),
        nullptr);
    (void)MsQuic->StreamShutdown(rawStream, QUIC_STREAM_SHUTDOWN_FLAG_ABORT, 0);
}

QUIC_STATUS TqStreamLifetime::DispatchBufferedEvent(QUIC_STREAM_EVENT* event) noexcept {
    auto guard = shared_from_this();
    return Dispatch(Stream_, event);
}

bool TqStreamLifetime::CancelSendCompletion(void* clientContext) noexcept {
    std::shared_ptr<TqStreamLifetime> released;
    std::function<void()> cleanup;
    {
        std::lock_guard<std::mutex> guard(g_sendCompletionLock);
        auto it = g_sendCompletions.find(clientContext);
        if (it == g_sendCompletions.end() || it->second.Owner.get() != this) {
            return false;
        }
        released = std::move(it->second.Owner);
        cleanup = std::move(it->second.Cleanup);
        g_sendCompletions.erase(it);
    }
    if (cleanup) {
        cleanup();
    }
    return true;
}

#if defined(TQ_UNIT_TESTING)
bool TqStreamLifetime::InjectSendCompletionForTest(
    void* clientContext,
    void* deliveredContext,
    std::function<void()> completionCleanup) noexcept {
    if (clientContext == nullptr) {
        return false;
    }
    RouteSnapshot route{};
    {
        std::lock_guard<std::mutex> guard(ControlMutex_);
        if ((Phase_ != Phase::Starting && Phase_ != Phase::Started) || !Target_) {
            return false;
        }
        route.TargetOwner = Target_;
        route.Generation = RouteGeneration_;
    }
    std::lock_guard<std::mutex> guard(g_sendCompletionLock);
    g_sendCompletions.erase(clientContext);
    return g_sendCompletions.emplace(
        clientContext,
        SendCompletionRetention{
            shared_from_this(),
            std::move(route),
            deliveredContext,
            std::move(completionCleanup)}).second;
}
#endif

QUIC_STATUS TqStreamLifetime::RequestShutdown(
    ShutdownIntent intent,
    uint64_t errorCode) noexcept {
    std::shared_ptr<TqStreamLifetime> lease;
    MsQuicStream* stream = nullptr;
    uint8_t sendRequest = 0;
    bool receiveRequest = false;
    bool immediateRequest = false;
    bool ownsSendReservation = false;
    bool ownsReceiveReservation = false;
    bool ownsImmediateReservation = false;
    {
        std::lock_guard<std::mutex> guard(ControlMutex_);
        if (Stream_ == nullptr ||
            (Phase_ != Phase::Starting && Phase_ != Phase::Started)) {
            return QUIC_STATUS_INVALID_STATE;
        }
        switch (intent) {
        case ShutdownIntent::GracefulSend:
            DesiredSendShutdown_ = DesiredSendShutdown_ < 1 ? 1 : DesiredSendShutdown_;
            break;
        case ShutdownIntent::AbortSend:
            DesiredSendShutdown_ = 2;
            break;
        case ShutdownIntent::AbortReceive:
            DesiredReceiveAbort_ = true;
            break;
        case ShutdownIntent::AbortBothImmediate:
            DesiredSendShutdown_ = 2;
            DesiredReceiveAbort_ = true;
            DesiredImmediate_ = true;
            break;
        case ShutdownIntent::AbortBoth:
            DesiredSendShutdown_ = 2;
            DesiredReceiveAbort_ = true;
            break;
        }
        if (DesiredSendShutdown_ > SubmittedSendShutdown_ &&
            DesiredSendShutdown_ > ReservedSendShutdown_) {
            sendRequest = DesiredSendShutdown_;
            ReservedSendShutdown_ = sendRequest;
            ownsSendReservation = true;
        }
        if (DesiredReceiveAbort_ && !SubmittedReceiveAbort_ && !ReservedReceiveAbort_) {
            receiveRequest = true;
            ReservedReceiveAbort_ = true;
            ownsReceiveReservation = true;
        }
        if (intent == ShutdownIntent::AbortBothImmediate &&
            DesiredImmediate_ && !SubmittedImmediate_ && !ReservedImmediate_) {
            immediateRequest = true;
            ReservedImmediate_ = true;
            ownsImmediateReservation = true;
            // IMMEDIATE is a strength upgrade of abort-both, so it must carry
            // both directions even if an ordinary abort-both is in flight.
            sendRequest = 2;
            receiveRequest = true;
        }
        if (sendRequest == 0 && !receiveRequest) {
            return QUIC_STATUS_SUCCESS;
        }
        lease = shared_from_this();
        stream = Stream_;
    }

    QUIC_STREAM_SHUTDOWN_FLAGS flags = QUIC_STREAM_SHUTDOWN_FLAG_NONE;
    if (sendRequest == 1) {
        flags = static_cast<QUIC_STREAM_SHUTDOWN_FLAGS>(
            static_cast<uint32_t>(flags) |
            static_cast<uint32_t>(QUIC_STREAM_SHUTDOWN_FLAG_GRACEFUL));
    } else if (sendRequest == 2) {
        flags = static_cast<QUIC_STREAM_SHUTDOWN_FLAGS>(
            static_cast<uint32_t>(flags) |
            static_cast<uint32_t>(QUIC_STREAM_SHUTDOWN_FLAG_ABORT_SEND));
    }
    if (receiveRequest) {
        flags = static_cast<QUIC_STREAM_SHUTDOWN_FLAGS>(
            static_cast<uint32_t>(flags) |
            static_cast<uint32_t>(QUIC_STREAM_SHUTDOWN_FLAG_ABORT_RECEIVE));
    }
    if (immediateRequest) {
        flags = static_cast<QUIC_STREAM_SHUTDOWN_FLAGS>(
            static_cast<uint32_t>(flags) |
            static_cast<uint32_t>(QUIC_STREAM_SHUTDOWN_FLAG_IMMEDIATE));
    }
    QUIC_STATUS status = QUIC_STATUS_SUCCESS;
    bool detachedStreamForTest = false;
    {
        std::lock_guard<std::mutex> guard(ControlMutex_);
        detachedStreamForTest = DetachedStreamForTest_;
    }
    if (!detachedStreamForTest) {
        status = stream->Shutdown(errorCode, flags);
    }
    {
        std::lock_guard<std::mutex> guard(ControlMutex_);
        if (ownsSendReservation && ReservedSendShutdown_ == sendRequest) {
            ReservedSendShutdown_ = 0;
        }
        if (QUIC_SUCCEEDED(status) &&
            sendRequest != 0 && SubmittedSendShutdown_ < sendRequest) {
            SubmittedSendShutdown_ = sendRequest;
        }
        if (ownsReceiveReservation && ReservedReceiveAbort_) {
            ReservedReceiveAbort_ = false;
        }
        if (QUIC_SUCCEEDED(status) && receiveRequest) {
            SubmittedReceiveAbort_ = true;
        }
        if (ownsImmediateReservation && ReservedImmediate_) {
            ReservedImmediate_ = false;
        }
        if (QUIC_SUCCEEDED(status) && immediateRequest) {
            SubmittedImmediate_ = true;
        }
    }
    return status;
}

TerminalPhase TqStreamLifetime::GetTerminalPhase() const noexcept {
    std::lock_guard<std::mutex> guard(ControlMutex_);
    return TerminalPhase_;
}

QUIC_STATUS TqStreamLifetime::CallTerminalShutdown(
    MsQuicStream* stream,
    uint64_t errorCode,
    QUIC_STREAM_SHUTDOWN_FLAGS flags) noexcept {
#if defined(TQ_UNIT_TESTING)
    ShutdownHookForTest hook;
    {
        std::lock_guard<std::mutex> guard(ControlMutex_);
        hook = ShutdownHookForTest_;
    }
    if (hook) {
        return hook(errorCode, flags);
    }
#endif
    return stream->Shutdown(errorCode, flags);
}

TqTerminalShutdownResult TqStreamLifetime::BeginTerminalShutdown(
    uint64_t errorCode,
    std::shared_ptr<Target> terminalSink,
    std::shared_ptr<TqTerminalEscalation> escalation) noexcept {
    TqTerminalShutdownResult result{};
    std::shared_ptr<TqStreamLifetime> lease;
    std::shared_ptr<TqTerminalLedger> ledger;
    MsQuicStream* stream = nullptr;
    {
        std::unique_lock<std::mutex> guard(ControlMutex_);
        if (TerminalPhase_ == TerminalPhase::TerminalObserved ||
            Phase_ == Phase::TerminalPublished) {
            result.AlreadyTerminal = true;
            return result;
        }
        if (TerminalPhase_ == TerminalPhase::ShutdownSubmitted) {
            result.Submitted = true;
            result.Attempt = TerminalShutdownAttempt_;
            return result;
        }
        if (TerminalPhase_ == TerminalPhase::ShutdownReserved) {
            result.Attempt = TerminalShutdownAttempt_;
            return result;
        }
        if (Stream_ == nullptr || TerminalLedger_ == nullptr ||
            (Phase_ != Phase::Starting && Phase_ != Phase::Started) ||
            terminalSink == nullptr) {
            result.Status = QUIC_STATUS_INVALID_STATE;
            const auto illegalLedger = TerminalLedger_;
            guard.unlock();
            if (escalation != nullptr && illegalLedger != nullptr) {
                bool call = false;
                {
                    std::lock_guard<std::mutex> ledgerGuard(illegalLedger->Mutex_);
                    if (!illegalLedger->State_.ConnectionEscalated) {
                        illegalLedger->State_.ConnectionEscalated = true;
                        illegalLedger->State_.Watchdog =
                            TqTerminalWatchdogState::Escalated;
                        call = true;
                    }
                }
                if (call) {
                    const auto& identity = illegalLedger->Identity();
                    escalation->RequestConnectionShutdown(
                        identity.ConnectionId, identity.StreamId,
                        result.Status, errorCode);
                }
            }
            return result;
        }
        Target_ = std::move(terminalSink);
        TerminalSink_ = Target_;
        ++RouteGeneration_;
        TerminalEscalation_ = std::move(escalation);
        TerminalErrorCode_ = errorCode;
        TerminalPhase_ = TerminalPhase::ShutdownReserved;
        TerminalRetryOwned_ = false;
        ++TerminalShutdownAttempt_;
        result.Attempt = TerminalShutdownAttempt_;
        {
            std::lock_guard<std::mutex> ledgerGuard(TerminalLedger_->Mutex_);
            TerminalLedger_->State_.Phase = TerminalPhase::ShutdownReserved;
            TerminalLedger_->State_.ErrorCode = errorCode;
            TerminalLedger_->State_.ShutdownAttempt = TerminalShutdownAttempt_;
        }
        lease = shared_from_this();
        ledger = TerminalLedger_;
        stream = Stream_;
    }

    const auto flags = static_cast<QUIC_STREAM_SHUTDOWN_FLAGS>(
        QUIC_STREAM_SHUTDOWN_FLAG_ABORT | QUIC_STREAM_SHUTDOWN_FLAG_IMMEDIATE);
    result.Status = CallTerminalShutdown(stream, errorCode, flags);

    {
        std::lock_guard<std::mutex> guard(ControlMutex_);
        if (TerminalPhase_ == TerminalPhase::TerminalObserved ||
            Phase_ == Phase::TerminalPublished) {
            result.AlreadyTerminal = true;
            return result;
        }
        if (QUIC_SUCCEEDED(result.Status)) {
            TerminalPhase_ = TerminalPhase::ShutdownSubmitted;
            result.Submitted = true;
        } else if (TerminalPhase_ == TerminalPhase::ShutdownReserved) {
            TerminalPhase_ = TerminalPhase::Active;
            TerminalRetryOwned_ = true;
        }
    }
#if defined(TQ_UNIT_TESTING)
    BeforeTerminalLedgerRecordHookForTest beforeLedgerRecord;
    {
        std::lock_guard<std::mutex> guard(ControlMutex_);
        beforeLedgerRecord = BeforeTerminalLedgerRecordHookForTest_;
    }
    if (beforeLedgerRecord) {
        beforeLedgerRecord();
    }
#endif
    ledger->RecordShutdown(result.Status, result.Attempt, result.Submitted);
    std::shared_ptr<TqTerminalEscalation> schedulerEscalation;
    uint64_t schedulerErrorCode = 0;
    uint32_t schedulerWatchdogSeconds = 5;
    {
        std::lock_guard<std::mutex> guard(ControlMutex_);
        schedulerEscalation = TerminalEscalation_;
        schedulerErrorCode = TerminalErrorCode_;
        schedulerWatchdogSeconds = TerminalWatchdogSeconds_;
    }
    if (result.Submitted) {
        TqTerminalScheduler::Instance().ArmWatchdog(
            weak_from_this(), ledger, schedulerEscalation, schedulerErrorCode,
            std::chrono::seconds(schedulerWatchdogSeconds));
    } else if (!result.AlreadyTerminal && QUIC_FAILED(result.Status)) {
        result.RetryScheduled = TqTerminalScheduler::Instance().ScheduleRetry(
            weak_from_this(), ledger, schedulerEscalation, schedulerErrorCode,
            result.Attempt);
        if (!result.RetryScheduled) {
            (void)TqTerminalScheduler::Instance().ScheduleRetry(
                weak_from_this(), ledger, schedulerEscalation, schedulerErrorCode, 4);
        }
    }
    return result;
}

TqTerminalShutdownResult TqStreamLifetime::RetryTerminalShutdown() noexcept {
    std::shared_ptr<Target> sink;
    std::shared_ptr<TqTerminalEscalation> escalation;
    uint64_t errorCode = 0;
    {
        std::lock_guard<std::mutex> guard(ControlMutex_);
        sink = TerminalSink_;
        escalation = TerminalEscalation_;
        errorCode = TerminalErrorCode_;
    }
    return BeginTerminalShutdown(errorCode, std::move(sink), std::move(escalation));
}

TqStreamCallbackTarget::TqStreamCallbackTarget(
    CallbackFn callback,
    void* context) noexcept
    : Callback_(callback), Context_(context) {}

QUIC_STATUS TqStreamCallbackTarget::OnStreamEvent(
    MsQuicStream* stream,
    QUIC_STREAM_EVENT* event,
    uint64_t) noexcept {
    CallbackFn callback = nullptr;
    void* context = nullptr;
    {
        std::lock_guard<std::mutex> guard(Lock_);
        if (!Accepting_ || Callback_ == nullptr || Context_ == nullptr) {
            return QUIC_STATUS_SUCCESS;
        }
        ++ActiveCalls_;
        callback = Callback_;
        context = Context_;
    }
    auto* previous = g_activeCallbackTarget;
    g_activeCallbackTarget = this;
    const QUIC_STATUS status = callback(stream, context, event);
    g_activeCallbackTarget = previous;
    {
        std::lock_guard<std::mutex> guard(Lock_);
        if (--ActiveCalls_ == 0) {
            Drained_.notify_all();
        }
    }
    return status;
}

void TqStreamCallbackTarget::Detach() noexcept {
    std::unique_lock<std::mutex> guard(Lock_);
    Accepting_ = false;
    Context_ = nullptr;
    if (g_activeCallbackTarget != this) {
        Drained_.wait(guard, [this] { return ActiveCalls_ == 0; });
    }
}

#if defined(TQ_UNIT_TESTING)
void* TqStreamCallbackTarget::ContextForTest() const noexcept {
    std::lock_guard<std::mutex> guard(Lock_);
    return Context_;
}
#endif

MsQuicStream* TqStreamLifetime::ApiLease::Stream() const noexcept {
    return Owner_ != nullptr ? Owner_->Stream_ : nullptr;
}

bool TqStreamLifetime::PublishTarget(
    uint64_t expectedGeneration,
    std::shared_ptr<Target> target,
    uint64_t* publishedGeneration) noexcept {
    if (!target) {
        return false;
    }
    std::lock_guard<std::mutex> guard(ControlMutex_);
    if ((Phase_ != Phase::Starting && Phase_ != Phase::Started) ||
        expectedGeneration != RouteGeneration_) {
        return false;
    }
    Target_ = std::move(target);
    ++RouteGeneration_;
    if (publishedGeneration != nullptr) {
        *publishedGeneration = RouteGeneration_;
    }
    return true;
}

TqStreamLifetime::RouteSnapshot TqStreamLifetime::PublishStartFailureAndTakeTarget(
    QUIC_STATUS status) noexcept {
    RouteSnapshot snapshot{};
    {
        std::lock_guard<std::mutex> guard(ControlMutex_);
        if (Phase_ != Phase::Starting) {
            return snapshot;
        }
        Phase_ = Phase::StartFailed;
        StartFailureStatus_ = status;
        snapshot.TargetOwner = std::move(Target_);
        snapshot.Generation = RouteGeneration_;
    }
    ReleaseTerminalRetention();
    return snapshot;
}

TqStreamLifetime::RouteSnapshot TqStreamLifetime::PublishTerminalAndTakeTarget() noexcept {
    RouteSnapshot snapshot{};
    {
        std::lock_guard<std::mutex> guard(ControlMutex_);
        if (Phase_ == Phase::TerminalPublished || Phase_ == Phase::Closed ||
            Phase_ == Phase::StartFailed || Phase_ == Phase::CreatedNotStarted) {
            return snapshot;
        }
        TerminalPhase_ = TerminalPhase::TerminalObserved;
        Phase_ = Phase::TerminalPublished;
        if (TerminalLedger_ != nullptr) {
            TerminalLedger_->RecordEvent(TqTerminalEvent::ShutdownComplete);
        }
        snapshot.TargetOwner = std::move(Target_);
        snapshot.Generation = RouteGeneration_;
    }
    if (TerminalLedger_ != nullptr) {
        TqTerminalScheduler::Instance().Cancel(TerminalLedger_->Identity().StreamId);
    }
    ReleaseTerminalRetention();
    return snapshot;
}

void TqStreamLifetime::RetainUntilTerminal() {
    std::lock_guard<std::mutex> controlGuard(ControlMutex_);
    RetainUntilTerminalLocked();
}

void TqStreamLifetime::RetainUntilTerminalLocked() {
    std::lock_guard<std::mutex> guard(g_terminalRetentionLock);
    if (!TerminalRetained_) {
        if (TerminalLedger_ == nullptr) {
            TqRecordTerminalExactlyOnceViolation();
            return;
        }
        const auto inserted = g_terminalRetentions.emplace(
            this,
            TerminalRetention{
                shared_from_this(), TerminalLedger_, std::chrono::steady_clock::now()});
        if (!inserted.second ||
            inserted.first->second.Ledger.get() != TerminalLedger_.get()) {
            TqRecordTerminalExactlyOnceViolation();
            return;
        }
        TerminalRetained_ = true;
    }
}

void TqStreamLifetime::BindTerminalIdentity(
    TqTerminalIdentity identity,
    uint32_t watchdogSeconds) noexcept {
    std::lock_guard<std::mutex> guard(ControlMutex_);
    if (TerminalLedger_ != nullptr) {
        const auto bound = TerminalLedger_->Identity();
        if (!SameTerminalIdentity(bound, identity) ||
            TerminalWatchdogSeconds_ != watchdogSeconds) {
            TqRecordTerminalExactlyOnceViolation();
        }
        return;
    }
    if (watchdogSeconds < 5 || watchdogSeconds > 30) {
        TqRecordTerminalExactlyOnceViolation();
        return;
    }
    try {
        TerminalLedger_ = std::make_shared<TqTerminalLedger>(identity);
        TerminalWatchdogSeconds_ = watchdogSeconds;
    } catch (...) {
        TerminalLedger_.reset();
    }
}

std::shared_ptr<TqTerminalLedger>
TqStreamLifetime::TerminalLedger() const noexcept {
    std::lock_guard<std::mutex> guard(ControlMutex_);
    return TerminalLedger_;
}

void TqStreamLifetime::ReleaseTerminalRetention() {
    std::shared_ptr<TqStreamLifetime> released;
    {
        std::lock_guard<std::mutex> guard(g_terminalRetentionLock);
        auto it = g_terminalRetentions.find(this);
        if (it != g_terminalRetentions.end()) {
            released = std::move(it->second.Owner);
            g_terminalRetentions.erase(it);
        }
        TerminalRetained_ = false;
    }
    // released 在锁外析构，避免 wrapper close 或 target 析构发生在 registry 锁内。
}

TqStreamLifetime::RetentionSnapshot
TqStreamLifetime::SnapshotTerminalRetentions() noexcept {
    RetentionSnapshot snapshot{};
    const auto now = std::chrono::steady_clock::now();
    std::lock_guard<std::mutex> guard(g_terminalRetentionLock);
    snapshot.OwnerCount = g_terminalRetentions.size();
    for (const auto& item : g_terminalRetentions) {
        const auto age = std::chrono::duration_cast<std::chrono::milliseconds>(
            now - item.second.Since).count();
        snapshot.OldestAgeMs = std::max<uint64_t>(
            snapshot.OldestAgeMs,
            age > 0 ? static_cast<uint64_t>(age) : 0);
    }
    return snapshot;
}

std::vector<TqTerminalLedgerSnapshot> TqSnapshotTerminalRetentions(
    const TqTerminalRetentionFilter& filter) {
    std::vector<std::shared_ptr<TqTerminalLedger>> ledgers;
    {
        std::lock_guard<std::mutex> guard(g_terminalRetentionLock);
        ledgers.reserve(g_terminalRetentions.size());
        for (const auto& item : g_terminalRetentions) {
            ledgers.push_back(item.second.Ledger);
        }
    }

#if defined(TQ_UNIT_TESTING)
    std::function<void()> beforeSnapshot;
    {
        std::lock_guard<std::mutex> guard(g_terminalSnapshotHookLock);
        beforeSnapshot = g_beforeTerminalSnapshotHook;
    }
    if (beforeSnapshot) {
        beforeSnapshot();
    }
#endif

    std::vector<TqTerminalLedgerSnapshot> snapshots;
    snapshots.reserve(ledgers.size());
    const auto now = std::chrono::steady_clock::now();
    for (const auto& ledger : ledgers) {
        if (ledger == nullptr) {
            continue;
        }
        auto snapshot = ledger->Snapshot(now);
        if (filter.Backend != TqRelayBackendType::None &&
            snapshot.Identity.Backend != filter.Backend) {
            continue;
        }
        if (filter.ConnectionId != 0 &&
            snapshot.Identity.ConnectionId != filter.ConnectionId) {
            continue;
        }
        if (filter.TunnelId != 0 && snapshot.Identity.TunnelId != filter.TunnelId) {
            continue;
        }
        if (filter.HasPhase && snapshot.Phase != filter.Phase) {
            continue;
        }
        snapshots.push_back(snapshot);
    }
    return snapshots;
}

TqStreamLifetime::SendCompletionSnapshot
TqStreamLifetime::SnapshotSendCompletions() noexcept {
    SendCompletionSnapshot snapshot{};
    const auto now = std::chrono::steady_clock::now();
    {
        std::lock_guard<std::mutex> guard(g_sendCompletionLock);
        snapshot.ActiveCount = g_sendCompletions.size();
        for (const auto& item : g_sendCompletions) {
            const auto age = std::chrono::duration_cast<std::chrono::milliseconds>(
                now - item.second.Since).count();
            snapshot.OldestAgeMs = std::max<uint64_t>(
                snapshot.OldestAgeMs,
                age > 0 ? static_cast<uint64_t>(age) : 0);
        }
    }
    snapshot.PreSubmitRollbacks =
        g_sendCompletionPreSubmitRollbacks.load(std::memory_order_relaxed);
    snapshot.UnknownClaims =
        g_sendCompletionUnknownClaims.load(std::memory_order_relaxed);
    snapshot.DuplicateClaims =
        g_sendCompletionDuplicateClaims.load(std::memory_order_relaxed);
    return snapshot;
}

void TqStreamLifetime::RecordUnknownSendClaim() noexcept {
    g_sendCompletionUnknownClaims.fetch_add(1, std::memory_order_relaxed);
}

void TqStreamLifetime::RecordDuplicateSendClaim() noexcept {
    g_sendCompletionDuplicateClaims.fetch_add(1, std::memory_order_relaxed);
}

TqStreamLifetime::RegistrySnapshot TqStreamLifetime::SnapshotRegistries() noexcept {
    RegistrySnapshot snapshot{};
    snapshot.TerminalApiSuppressedCount =
        g_terminalApiSuppressedCount.load(std::memory_order_relaxed);
    {
        std::lock_guard<std::mutex> guard(g_sendCompletionLock);
        snapshot.SendCompletionCount = g_sendCompletions.size();
    }
    return snapshot;
}

#if defined(TQ_UNIT_TESTING)
void TqStreamLifetime::ResetLifecycleRegistriesForTest() noexcept {
    {
        std::lock_guard<std::mutex> guard(g_terminalRetentionLock);
        g_terminalRetentions.clear();
    }
    {
        std::lock_guard<std::mutex> guard(g_sendCompletionLock);
        g_sendCompletions.clear();
        g_claimedSendCompletions.clear();
    }
    g_terminalApiSuppressedCount.store(0, std::memory_order_relaxed);
    TqResetTerminalMetricsForTest();
    SetBeforeTerminalRetentionSnapshotForTest({});
}

void TqStreamLifetime::SetBeforeTerminalRetentionSnapshotForTest(
    std::function<void()> hook) noexcept {
    std::lock_guard<std::mutex> guard(g_terminalSnapshotHookLock);
    g_beforeTerminalSnapshotHook = std::move(hook);
}
#endif

QUIC_STATUS QUIC_API TqStreamLifetime::Callback(
    MsQuicStream* stream,
    void* context,
    QUIC_STREAM_EVENT* event) noexcept {
    auto* rawOwner = static_cast<TqStreamLifetime*>(context);
    if (rawOwner == nullptr) {
        return QUIC_STATUS_SUCCESS;
    }
    std::shared_ptr<TqStreamLifetime> guard;
    try {
        guard = rawOwner->shared_from_this();
    } catch (...) {
        return QUIC_STATUS_SUCCESS;
    }
    return guard->Dispatch(stream, event);
}

QUIC_STATUS TqStreamLifetime::Dispatch(
    MsQuicStream* stream,
    QUIC_STREAM_EVENT* event) noexcept {
    if (event == nullptr) {
        return QUIC_STATUS_SUCCESS;
    }

    RouteSnapshot snapshot{};
    if (event->Type == QUIC_STREAM_EVENT_SEND_COMPLETE) {
        SendCompletionRetention completion{};
        {
            std::lock_guard<std::mutex> guard(g_sendCompletionLock);
            auto it = g_sendCompletions.find(event->SEND_COMPLETE.ClientContext);
            if (it == g_sendCompletions.end() || it->second.Owner.get() != this) {
                const auto claimed =
                    g_claimedSendCompletions.find(event->SEND_COMPLETE.ClientContext);
                if (claimed != g_claimedSendCompletions.end() && claimed->second == this) {
                    RecordDuplicateSendClaim();
                } else {
                    RecordUnknownSendClaim();
                }
                return QUIC_STATUS_SUCCESS;
            }
            completion = std::move(it->second);
            g_sendCompletions.erase(it);
            g_claimedSendCompletions[event->SEND_COMPLETE.ClientContext] = this;
        }
        QUIC_STATUS status = QUIC_STATUS_SUCCESS;
        if (completion.Route.TargetOwner) {
            event->SEND_COMPLETE.ClientContext = completion.DeliveredContext;
            status = completion.Route.TargetOwner->OnStreamEvent(
                stream, event, completion.Route.Generation);
        }
        if (completion.Cleanup) {
            completion.Cleanup();
        }
        return status;
    } else if (event->Type == QUIC_STREAM_EVENT_SHUTDOWN_COMPLETE) {
        snapshot = PublishTerminalAndTakeTarget();
    } else if (event->Type == QUIC_STREAM_EVENT_SEND_SHUTDOWN_COMPLETE) {
        {
            std::lock_guard<std::mutex> guard(ControlMutex_);
            SendDirectionComplete_ = true;
            if (event->SEND_SHUTDOWN_COMPLETE.Graceful) {
                if (SubmittedSendShutdown_ < 1) {
                    SubmittedSendShutdown_ = 1;
                }
            } else if (SubmittedSendShutdown_ < 2) {
                SubmittedSendShutdown_ = 2;
            }
        }
        std::lock_guard<std::mutex> controlGuard(ControlMutex_);
        snapshot.TargetOwner = Target_;
        snapshot.Generation = RouteGeneration_;
    } else if (event->Type == QUIC_STREAM_EVENT_CANCEL_ON_LOSS) {
        constexpr uint64_t kCancelOnLossStableErrorCode = 0x54515043ULL;
        event->CANCEL_ON_LOSS.ErrorCode = kCancelOnLossStableErrorCode;
        {
            std::lock_guard<std::mutex> guard(ControlMutex_);
            CancelOnLossErrorCode_ = kCancelOnLossStableErrorCode;
        }
        (void)RequestShutdown(ShutdownIntent::AbortBoth);
        std::lock_guard<std::mutex> controlGuard(ControlMutex_);
        snapshot.TargetOwner = Target_;
        snapshot.Generation = RouteGeneration_;
    } else {
        if (event->Type == QUIC_STREAM_EVENT_START_COMPLETE) {
            if (QUIC_SUCCEEDED(event->START_COMPLETE.Status)) {
                (void)CompleteStart(event->START_COMPLETE.Status);
            } else {
                snapshot = PublishStartFailureAndTakeTarget(event->START_COMPLETE.Status);
            }
        }
        if (!snapshot.TargetOwner) {
            std::lock_guard<std::mutex> controlGuard(ControlMutex_);
            snapshot.TargetOwner = Target_;
            snapshot.Generation = RouteGeneration_;
        }
    }
    if (!snapshot.TargetOwner) {
        return QUIC_STATUS_SUCCESS;
    }
    return snapshot.TargetOwner->OnStreamEvent(stream, event, snapshot.Generation);
}
