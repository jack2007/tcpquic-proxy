#pragma once

#include "relay.h"
#include "tcp_tunnel.h"
#include <msquic.h>

#include <chrono>
#include <cstdint>
#include <functional>
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

enum class TqTerminalShutdownIntent : uint8_t {
    None,
    AbortBothImmediate,
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
    TqTerminalShutdownIntent ShutdownIntent{TqTerminalShutdownIntent::None};
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
    void RecordShutdown(
        QUIC_STATUS status,
        uint32_t attempt,
        bool submitted,
        TqTerminalShutdownIntent intent = TqTerminalShutdownIntent::None) noexcept;
    void MarkHandoffFacts(bool inTunnelRegistry, bool relayActive, bool tcpValid) noexcept;
    bool CompleteAccountingOnce() noexcept;
private:
    friend class TqStreamLifetime;
    friend class TqTerminalScheduler;
    friend struct TqTerminalSchedulerInternals;
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

class TqTerminalScheduler final {
public:
    static TqTerminalScheduler& Instance();
    void Start();
    void Stop();
    bool ScheduleRetry(
        std::weak_ptr<TqStreamLifetime> owner,
        std::shared_ptr<TqTerminalLedger> ledger,
        std::shared_ptr<TqTerminalEscalation> escalation,
        uint64_t errorCode,
        uint32_t completedAttempt) noexcept;
    void ArmWatchdog(
        std::weak_ptr<TqStreamLifetime> owner,
        std::shared_ptr<TqTerminalLedger> ledger,
        std::shared_ptr<TqTerminalEscalation> escalation,
        uint64_t errorCode,
        std::chrono::seconds deadline) noexcept;
    void Cancel(uint64_t streamId) noexcept;
#if defined(TQ_UNIT_TESTING)
    struct TestSnapshot {
        uint64_t PendingTasks{0};
        uint64_t IndexedStreams{0};
        uint64_t CanceledStreams{0};
        bool Running{false};
        bool Joinable{false};
        uint64_t LifecycleGeneration{0};
        bool RestartRequested{false};
    };
    static void ResetForTest();
    static void AdvanceForTest(std::chrono::milliseconds delta);
    static std::chrono::steady_clock::time_point NowForTest();
    static TestSnapshot SnapshotForTest();
    static void SetBeforeExecuteForTest(std::function<void()> hook);
    static void SetBeforeWorkerReturnForTest(std::function<void()> hook);
    static void SetAfterEnqueueForTest(std::function<void()> hook);
    static void UseRealClockForTest();
    static void FailNextAllocationForTest(uint32_t count) noexcept;
    static void FailNextThreadStartForTest() noexcept;
#endif
};

struct TqTerminalShutdownResult {
    QUIC_STATUS Status{QUIC_STATUS_SUCCESS};
    bool Submitted{false};
    bool AlreadyTerminal{false};
    bool RetryScheduled{false};
    uint32_t Attempt{0};
};

struct TqTerminalMetrics {
    uint64_t HandoffStarted{0};
    uint64_t HandoffCompleted{0};
    uint64_t HandoffFailed{0};
    uint64_t ShutdownSubmitted{0};
    uint64_t ShutdownPending{0};
    uint64_t ShutdownSyncFailure{0};
    uint64_t ShutdownRetry{0};
    uint64_t TerminalObserved{0};
    uint64_t WatchdogArmed{0};
    uint64_t WatchdogCanceled{0};
    uint64_t WatchdogTimeout{0};
    uint64_t ConnectionEscalation{0};
    uint64_t TerminalTimeoutPending{0};
    uint64_t TerminalSinkPending{0};
    uint64_t DuplicateStopSuppressed{0};
    uint64_t DuplicateShutdownSuppressed{0};
    uint64_t DuplicateTerminalSuppressed{0};
    uint64_t ExactlyOnceViolation{0};
    uint64_t SchedulerFailure{0};
};

TqTerminalMetrics TqTerminalMetricsSnapshot() noexcept;

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
const char* TqTerminalShutdownIntentName(TqTerminalShutdownIntent intent) noexcept;
const char* TqTerminalEventName(TqTerminalEvent event) noexcept;
void TqRecordTerminalExactlyOnceViolation() noexcept;
uint64_t TqTerminalExactlyOnceViolationCount() noexcept;
#if defined(TQ_UNIT_TESTING)
void TqResetTerminalMetricsForTest() noexcept;
#endif
