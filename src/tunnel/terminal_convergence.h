#pragma once

#include "relay.h"
#include "tcp_tunnel.h"
#include <msquic.h>

#include <chrono>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

class TqStreamLifetime;

enum class TerminalPhase : uint8_t {
    Active,
    ShutdownReserved,
    ShutdownSubmitted,
    TerminalObserved,
    Closed,
};

enum class TqTerminalWatchdogState : uint8_t {
    Idle,
    Armed,
    Canceled,
    Escalated,
    TerminalTimeout,
};

enum class TqTerminalEvent : uint8_t {
    None,
    StartComplete,
    ReceiveAfterHandoff,
    SendComplete,
    PeerSendAborted,
    PeerReceiveAborted,
    SendShutdownComplete,
    ShutdownComplete,
    CancelOnLoss,
    IdealSendBufferSize,
};

struct TqTerminalIdentity {
    uint64_t StreamId{0};
    uint64_t TunnelId{0};
    uint64_t ConnectionId{0};
    uint64_t ConnectionGeneration{0};
    TqTunnelRole Role{TqTunnelRole::ClientOpen};
    TqRelayBackend Backend{TqRelayBackendType::None};
};

struct TqTerminalLedgerSnapshot {
    TqTerminalIdentity Identity{};
    TerminalPhase Phase{TerminalPhase::Active};
    uint64_t RetainedAgeMs{0};
    uint64_t ErrorCode{0};
    QUIC_STATUS ShutdownStatus{QUIC_STATUS_SUCCESS};
    uint32_t ShutdownAttempt{0};
    uint64_t ShutdownSubmittedAtMs{0};
    uint64_t TerminalObservedAtMs{0};
    TqTerminalEvent LastStreamEvent{TqTerminalEvent::None};
    bool InTunnelRegistry{true};
    bool RelayActive{true};
    bool TcpValid{true};
    TqTerminalWatchdogState Watchdog{TqTerminalWatchdogState::Idle};
    bool ConnectionEscalated{false};
    bool AccountingCompleted{false};
};

class TqTerminalLedger final {
public:
    explicit TqTerminalLedger(TqTerminalIdentity identity) noexcept;
    TqTerminalLedgerSnapshot Snapshot(std::chrono::steady_clock::time_point now) const;
    const TqTerminalIdentity& Identity() const noexcept;
    void RecordEvent(TqTerminalEvent event) noexcept;
    void RecordShutdown(QUIC_STATUS status, uint32_t attempt, bool submitted) noexcept;
    void MarkHandoffFacts(bool inTunnelRegistry, bool relayActive, bool tcpValid) noexcept;
    bool CompleteAccountingOnce() noexcept;
private:
    friend class TqStreamLifetime;
    friend class TqTerminalScheduler;
    mutable std::mutex Mutex_;
    TqTerminalLedgerSnapshot State_{};
    std::chrono::steady_clock::time_point RetainedSince_{std::chrono::steady_clock::now()};
};

class TqTerminalEscalation {
public:
    virtual ~TqTerminalEscalation() = default;
    virtual void RequestConnectionShutdown(
        uint64_t connectionId,
        uint64_t streamId,
        QUIC_STATUS streamStatus,
        uint64_t errorCode) noexcept = 0;
};

struct TqTerminalShutdownResult {
    QUIC_STATUS Status{QUIC_STATUS_SUCCESS};
    bool Submitted{false};
    bool AlreadyTerminal{false};
    bool RetryScheduled{false};
    uint32_t Attempt{0};
};

struct TqTerminalRetentionFilter {
    TqRelayBackend Backend{TqRelayBackendType::None};
    uint64_t ConnectionId{0};
    uint64_t TunnelId{0};
    bool HasPhase{false};
    TerminalPhase Phase{TerminalPhase::Active};
};

std::vector<TqTerminalLedgerSnapshot> TqSnapshotTerminalRetentions(
    const TqTerminalRetentionFilter& filter = {});
const char* TqTerminalPhaseName(TerminalPhase phase) noexcept;
const char* TqTerminalWatchdogStateName(TqTerminalWatchdogState state) noexcept;
const char* TqTerminalEventName(TqTerminalEvent event) noexcept;
void TqRecordTerminalExactlyOnceViolation() noexcept;
uint64_t TqTerminalExactlyOnceViolationCount() noexcept;
