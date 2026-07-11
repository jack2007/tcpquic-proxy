#include "stream_lifetime.h"

#include <new>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <unordered_map>

namespace {

std::mutex g_terminalRetentionLock;
struct TerminalRetention {
    std::shared_ptr<TqStreamLifetime> Owner;
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
std::atomic<uint64_t> g_nextSendNonce{1};
std::atomic<uint64_t> g_sendCompletionPreSubmitRollbacks{0};
std::atomic<uint64_t> g_sendCompletionUnknownClaims{0};
std::atomic<uint64_t> g_sendCompletionDuplicateClaims{0};
#if defined(TQ_UNIT_TESTING)
std::atomic<bool> g_failNextRegisterSendCompletion{false};
#endif
thread_local TqStreamCallbackTarget* g_activeCallbackTarget = nullptr;

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
    {
        std::lock_guard<std::mutex> guard(ControlMutex_);
        stream = Stream_;
        Stream_ = nullptr;
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
    delete stream;
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
    std::shared_ptr<Target> initialTarget) noexcept {
    auto owner = std::shared_ptr<TqStreamLifetime>(
        new (std::nothrow) TqStreamLifetime(Phase::CreatedNotStarted, std::move(initialTarget)));
    if (!owner) {
        return nullptr;
    }
    auto* stream = new (std::nothrow) MsQuicStream(
        connection, flags, CleanUpManual, Callback, owner.get());
    if (!owner->InstallStream(stream)) {
        delete stream;
        return nullptr;
    }
    return owner;
}

std::shared_ptr<TqStreamLifetime> TqStreamLifetime::AdoptAccepted(
    HQUIC rawStream,
    std::shared_ptr<Target> initialTarget) noexcept {
    if (rawStream == nullptr) {
        return nullptr;
    }
    auto owner = std::shared_ptr<TqStreamLifetime>(
        new (std::nothrow) TqStreamLifetime(Phase::Started, std::move(initialTarget)));
    if (!owner) {
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

void* TqStreamLifetime::TargetContextForTest() const noexcept {
    std::lock_guard<std::mutex> guard(ControlMutex_);
    return Target_ != nullptr ? Target_->ContextForTest() : nullptr;
}

void TqStreamLifetime::SetFailNextRegisterSendCompletionForTest(bool fail) noexcept {
    g_failNextRegisterSendCompletion.store(fail, std::memory_order_release);
}
#endif

#if defined(TQ_UNIT_TESTING) || defined(TCPQUIC_TUNNEL_TESTING)
QUIC_STATUS TqStreamLifetime::DispatchForTest(QUIC_STREAM_EVENT* event) noexcept {
    return Dispatch(nullptr, event);
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
        (Phase_ != Phase::Starting && Phase_ != Phase::Started)) {
        return {};
    }
    return ApiLease(shared_from_this());
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

QUIC_STATUS TqStreamLifetime::RequestShutdown(
    ShutdownIntent intent,
    uint64_t errorCode) noexcept {
    std::shared_ptr<TqStreamLifetime> lease;
    MsQuicStream* stream = nullptr;
    uint8_t sendRequest = 0;
    bool receiveRequest = false;
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
        case ShutdownIntent::AbortBoth:
            DesiredSendShutdown_ = 2;
            DesiredReceiveAbort_ = true;
            break;
        }
        if (DesiredSendShutdown_ > SubmittedSendShutdown_ &&
            DesiredSendShutdown_ > ReservedSendShutdown_) {
            sendRequest = DesiredSendShutdown_;
            ReservedSendShutdown_ = sendRequest;
        }
        if (DesiredReceiveAbort_ && !SubmittedReceiveAbort_ && !ReservedReceiveAbort_) {
            receiveRequest = true;
            ReservedReceiveAbort_ = true;
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
    const QUIC_STATUS status = stream->Shutdown(errorCode, flags);
    {
        std::lock_guard<std::mutex> guard(ControlMutex_);
        if (sendRequest != 0 && ReservedSendShutdown_ == sendRequest) {
            ReservedSendShutdown_ = 0;
            if (QUIC_SUCCEEDED(status) && SubmittedSendShutdown_ < sendRequest) {
                SubmittedSendShutdown_ = sendRequest;
            }
        }
        if (receiveRequest && ReservedReceiveAbort_) {
            ReservedReceiveAbort_ = false;
            if (QUIC_SUCCEEDED(status)) {
                SubmittedReceiveAbort_ = true;
            }
        }
    }
    return status;
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
        Phase_ = Phase::TerminalPublished;
        snapshot.TargetOwner = std::move(Target_);
        snapshot.Generation = RouteGeneration_;
    }
    ReleaseTerminalRetention();
    return snapshot;
}

void TqStreamLifetime::RetainUntilTerminal() {
    std::lock_guard<std::mutex> guard(g_terminalRetentionLock);
    if (!TerminalRetained_) {
        g_terminalRetentions.emplace(
            this,
            TerminalRetention{shared_from_this(), std::chrono::steady_clock::now()});
        TerminalRetained_ = true;
    }
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
