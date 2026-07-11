#include "terminal_convergence.h"

#include <atomic>

namespace {
std::atomic<uint64_t> g_terminalExactlyOnceViolations{0};
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
