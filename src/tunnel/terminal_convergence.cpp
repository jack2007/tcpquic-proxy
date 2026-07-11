#include "stream_lifetime.h"

#include <atomic>
#include <new>

namespace {
std::atomic<uint64_t> g_terminalExactlyOnceViolations{0};
std::atomic<uint64_t> g_terminalObserved{0};
std::atomic<uint64_t> g_terminalSinkPending{0};
std::atomic<uint64_t> g_duplicateTerminalSuppressed{0};
}

TqTerminalLedger::TqTerminalLedger(TqTerminalIdentity identity) noexcept {
    State_.Identity = identity;
}

TqTerminalLedgerSnapshot TqTerminalLedger::Snapshot(
    std::chrono::steady_clock::time_point now) const {
    std::lock_guard<std::mutex> guard(Mutex_);
    auto snapshot = State_;
    const auto age = std::chrono::duration_cast<std::chrono::milliseconds>(
        now - RetainedSince_).count();
    snapshot.RetainedAgeMs = age > 0 ? static_cast<uint64_t>(age) : 0;
    return snapshot;
}

const TqTerminalIdentity& TqTerminalLedger::Identity() const noexcept {
    return State_.Identity;
}

void TqTerminalLedger::RecordEvent(TqTerminalEvent event) noexcept {
    std::lock_guard<std::mutex> guard(Mutex_);
    State_.LastStreamEvent = event;
    if (event == TqTerminalEvent::ShutdownComplete) {
        State_.Phase = TerminalPhase::TerminalObserved;
        State_.TerminalObservedAtMs = static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now().time_since_epoch()).count());
    }
}

void TqTerminalLedger::RecordShutdown(
    QUIC_STATUS status,
    uint32_t attempt,
    bool submitted) noexcept {
    std::lock_guard<std::mutex> guard(Mutex_);
    if (attempt < State_.ShutdownAttempt) {
        return;
    }
    State_.ShutdownStatus = status;
    State_.ShutdownAttempt = attempt;
    if (State_.Phase != TerminalPhase::TerminalObserved &&
        State_.Phase != TerminalPhase::Closed) {
        State_.Phase = submitted ? TerminalPhase::ShutdownSubmitted : TerminalPhase::Active;
    }
    if (submitted) {
        State_.ShutdownSubmittedAtMs = static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now().time_since_epoch()).count());
    }
}

void TqTerminalLedger::MarkHandoffFacts(
    bool inTunnelRegistry,
    bool relayActive,
    bool tcpValid) noexcept {
    std::lock_guard<std::mutex> guard(Mutex_);
    State_.InTunnelRegistry = inTunnelRegistry;
    State_.RelayActive = relayActive;
    State_.TcpValid = tcpValid;
}

bool TqTerminalLedger::CompleteAccountingOnce() noexcept {
    std::lock_guard<std::mutex> guard(Mutex_);
    if (State_.AccountingCompleted) {
        return false;
    }
    State_.AccountingCompleted = true;
    return true;
}

TqTerminalSink::TqTerminalSink(
    std::weak_ptr<TqStreamLifetime> owner,
    std::shared_ptr<TqTerminalLedger> ledger) noexcept
    : Owner_(std::move(owner)), Ledger_(std::move(ledger)) {}

std::shared_ptr<TqTerminalSink> TqTerminalSink::Create(
    std::weak_ptr<TqStreamLifetime> owner,
    std::shared_ptr<TqTerminalLedger> ledger) noexcept {
    auto lockedOwner = owner.lock();
    if (lockedOwner == nullptr || ledger == nullptr ||
        lockedOwner->TerminalLedger().get() != ledger.get()) {
        TqRecordTerminalExactlyOnceViolation();
        return nullptr;
    }
    auto* raw = new (std::nothrow) TqTerminalSink(std::move(owner), std::move(ledger));
    if (raw == nullptr) {
        return nullptr;
    }
    try {
        std::shared_ptr<TqTerminalSink> sink(raw);
        g_terminalSinkPending.fetch_add(1, std::memory_order_relaxed);
        return sink;
    } catch (...) {
        delete raw;
        return nullptr;
    }
}

QUIC_STATUS TqTerminalSink::OnStreamEvent(
    MsQuicStream* stream,
    QUIC_STREAM_EVENT* event,
    uint64_t) noexcept {
    if (event == nullptr || Ledger_ == nullptr) {
        return QUIC_STATUS_SUCCESS;
    }
    switch (event->Type) {
    case QUIC_STREAM_EVENT_START_COMPLETE:
        Ledger_->RecordEvent(TqTerminalEvent::StartComplete);
        break;
    case QUIC_STREAM_EVENT_RECEIVE:
        Ledger_->RecordEvent(TqTerminalEvent::ReceiveAfterHandoff);
        if (stream != nullptr) {
            (void)stream->ReceiveSetEnabled(false);
        }
        event->RECEIVE.TotalBufferLength = 0;
        break;
    case QUIC_STREAM_EVENT_SEND_COMPLETE:
        Ledger_->RecordEvent(TqTerminalEvent::SendComplete);
        break;
    case QUIC_STREAM_EVENT_PEER_SEND_ABORTED:
        Ledger_->RecordEvent(TqTerminalEvent::PeerSendAborted);
        break;
    case QUIC_STREAM_EVENT_PEER_SEND_SHUTDOWN:
        break;
    case QUIC_STREAM_EVENT_PEER_RECEIVE_ABORTED:
        Ledger_->RecordEvent(TqTerminalEvent::PeerReceiveAborted);
        break;
    case QUIC_STREAM_EVENT_SEND_SHUTDOWN_COMPLETE:
        Ledger_->RecordEvent(TqTerminalEvent::SendShutdownComplete);
        break;
    case QUIC_STREAM_EVENT_SHUTDOWN_COMPLETE:
        Ledger_->RecordEvent(TqTerminalEvent::ShutdownComplete);
        if (Ledger_->CompleteAccountingOnce()) {
            g_terminalObserved.fetch_add(1, std::memory_order_relaxed);
            g_terminalSinkPending.fetch_sub(1, std::memory_order_relaxed);
        } else {
            g_duplicateTerminalSuppressed.fetch_add(1, std::memory_order_relaxed);
        }
        break;
    case QUIC_STREAM_EVENT_CANCEL_ON_LOSS:
        Ledger_->RecordEvent(TqTerminalEvent::CancelOnLoss);
        break;
    case QUIC_STREAM_EVENT_IDEAL_SEND_BUFFER_SIZE:
        Ledger_->RecordEvent(TqTerminalEvent::IdealSendBufferSize);
        break;
    case QUIC_STREAM_EVENT_PEER_ACCEPTED:
        break;
    }
    return QUIC_STATUS_SUCCESS;
}

const char* TqTerminalPhaseName(TerminalPhase phase) noexcept {
    switch (phase) {
    case TerminalPhase::Active: return "active";
    case TerminalPhase::ShutdownReserved: return "shutdown_reserved";
    case TerminalPhase::ShutdownSubmitted: return "shutdown_submitted";
    case TerminalPhase::TerminalObserved: return "terminal_observed";
    case TerminalPhase::Closed: return "closed";
    }
    return "unknown";
}

const char* TqTerminalWatchdogStateName(TqTerminalWatchdogState state) noexcept {
    switch (state) {
    case TqTerminalWatchdogState::Idle: return "idle";
    case TqTerminalWatchdogState::Armed: return "armed";
    case TqTerminalWatchdogState::Canceled: return "canceled";
    case TqTerminalWatchdogState::Escalated: return "escalated";
    case TqTerminalWatchdogState::TerminalTimeout: return "terminal_timeout";
    }
    return "unknown";
}

const char* TqTerminalEventName(TqTerminalEvent event) noexcept {
    switch (event) {
    case TqTerminalEvent::None: return "none";
    case TqTerminalEvent::StartComplete: return "start_complete";
    case TqTerminalEvent::ReceiveAfterHandoff: return "receive_after_handoff";
    case TqTerminalEvent::SendComplete: return "send_complete";
    case TqTerminalEvent::PeerSendAborted: return "peer_send_aborted";
    case TqTerminalEvent::PeerReceiveAborted: return "peer_receive_aborted";
    case TqTerminalEvent::SendShutdownComplete: return "send_shutdown_complete";
    case TqTerminalEvent::ShutdownComplete: return "shutdown_complete";
    case TqTerminalEvent::CancelOnLoss: return "cancel_on_loss";
    case TqTerminalEvent::IdealSendBufferSize: return "ideal_send_buffer_size";
    }
    return "unknown";
}

void TqRecordTerminalExactlyOnceViolation() noexcept {
    g_terminalExactlyOnceViolations.fetch_add(1, std::memory_order_relaxed);
}

uint64_t TqTerminalExactlyOnceViolationCount() noexcept {
    return g_terminalExactlyOnceViolations.load(std::memory_order_relaxed);
}

TqTerminalMetrics TqTerminalMetricsSnapshot() noexcept {
    TqTerminalMetrics snapshot{};
    snapshot.TerminalObserved = g_terminalObserved.load(std::memory_order_relaxed);
    snapshot.TerminalSinkPending =
        g_terminalSinkPending.load(std::memory_order_relaxed);
    snapshot.DuplicateTerminalSuppressed =
        g_duplicateTerminalSuppressed.load(std::memory_order_relaxed);
    snapshot.ExactlyOnceViolation =
        g_terminalExactlyOnceViolations.load(std::memory_order_relaxed);
    return snapshot;
}

#if defined(TQ_UNIT_TESTING)
void TqResetTerminalMetricsForTest() noexcept {
    g_terminalExactlyOnceViolations.store(0, std::memory_order_relaxed);
    g_terminalObserved.store(0, std::memory_order_relaxed);
    g_terminalSinkPending.store(0, std::memory_order_relaxed);
    g_duplicateTerminalSuppressed.store(0, std::memory_order_relaxed);
}
#endif
