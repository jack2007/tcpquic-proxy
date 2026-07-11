#include "stream_lifetime.h"

#include <atomic>
#include <algorithm>
#include <charconv>
#include <condition_variable>
#include <cstdio>
#include <queue>
#include <sstream>
#include <thread>
#include <unordered_map>
#include <new>

struct TqTerminalSchedulerInternals {
    static TqTerminalShutdownResult Retry(
        const std::shared_ptr<TqStreamLifetime>& owner) noexcept {
        return owner->RetryTerminalShutdown();
    }
    static void MarkEscalated(
        const std::shared_ptr<TqTerminalLedger>& ledger,
        bool& call,
        QUIC_STATUS& status) noexcept {
        std::lock_guard<std::mutex> guard(ledger->Mutex_);
        if (ledger->State_.Phase != TerminalPhase::TerminalObserved &&
            ledger->State_.Phase != TerminalPhase::Closed &&
            ledger->State_.Watchdog != TqTerminalWatchdogState::Canceled &&
            ledger->State_.Watchdog != TqTerminalWatchdogState::TerminalTimeout &&
            !ledger->State_.ConnectionEscalated) {
            ledger->State_.Watchdog = TqTerminalWatchdogState::Escalated;
            ledger->State_.ConnectionEscalated = true;
            status = ledger->State_.ShutdownStatus;
            call = true;
        }
    }
    static bool MarkTimedOut(const std::shared_ptr<TqTerminalLedger>& ledger) noexcept {
        std::lock_guard<std::mutex> guard(ledger->Mutex_);
        if (ledger->State_.Watchdog != TqTerminalWatchdogState::Escalated) return false;
        ledger->State_.Watchdog = TqTerminalWatchdogState::TerminalTimeout;
        return true;
    }
    template <typename Enqueue>
    static bool ArmAndEnqueue(
        const std::shared_ptr<TqTerminalLedger>& ledger,
        Enqueue&& enqueue) noexcept {
        std::lock_guard<std::mutex> guard(ledger->Mutex_);
        if (ledger->State_.Phase != TerminalPhase::ShutdownSubmitted ||
            ledger->State_.Watchdog != TqTerminalWatchdogState::Idle) return false;
        ledger->State_.Watchdog = TqTerminalWatchdogState::Armed;
        return enqueue();
    }
    static bool Cancel(const std::shared_ptr<TqTerminalLedger>& ledger) noexcept {
        std::lock_guard<std::mutex> guard(ledger->Mutex_);
        if (ledger->State_.Watchdog == TqTerminalWatchdogState::Canceled ||
            ledger->State_.Watchdog == TqTerminalWatchdogState::TerminalTimeout) return false;
        ledger->State_.Watchdog = TqTerminalWatchdogState::Canceled;
        return true;
    }
    static bool RetryAllowed(const std::shared_ptr<TqTerminalLedger>& ledger) noexcept {
        std::lock_guard<std::mutex> guard(ledger->Mutex_);
        return ledger->State_.Phase != TerminalPhase::TerminalObserved &&
            ledger->State_.Phase != TerminalPhase::Closed &&
            ledger->State_.Watchdog != TqTerminalWatchdogState::Canceled;
    }
    template <typename Enqueue>
    static bool RetryAndEnqueue(
        const std::shared_ptr<TqTerminalLedger>& ledger,
        Enqueue&& enqueue) noexcept {
        std::lock_guard<std::mutex> guard(ledger->Mutex_);
        if (ledger->State_.Phase == TerminalPhase::TerminalObserved ||
            ledger->State_.Phase == TerminalPhase::Closed ||
            ledger->State_.Watchdog == TqTerminalWatchdogState::Canceled) return false;
        return enqueue();
    }
};

bool StartWorkerWithLifecycleLocked() noexcept;

namespace {
std::atomic<uint64_t> g_terminalExactlyOnceViolations{0};
std::atomic<uint64_t> g_terminalObserved{0};
std::atomic<uint64_t> g_terminalSinkPending{0};
std::atomic<uint64_t> g_duplicateTerminalSuppressed{0};
std::atomic<uint64_t> g_shutdownSubmitted{0};
std::atomic<uint64_t> g_shutdownPending{0};
std::atomic<uint64_t> g_shutdownSyncFailure{0};
std::atomic<uint64_t> g_shutdownRetry{0};
std::atomic<uint64_t> g_watchdogArmed{0};
std::atomic<uint64_t> g_watchdogCanceled{0};
std::atomic<uint64_t> g_watchdogTimeout{0};
std::atomic<uint64_t> g_connectionEscalation{0};
std::atomic<uint64_t> g_terminalTimeoutPending{0};
std::atomic<uint64_t> g_schedulerFailure{0};
std::atomic<uint64_t> g_handoffStarted{0};
std::atomic<uint64_t> g_handoffCompleted{0};
std::atomic<uint64_t> g_handoffFailed{0};
struct RetentionDiagnosticState {
    std::atomic<uint8_t> Bits{0};
};
std::mutex g_retentionDiagnosticLock;
std::unordered_map<std::string, std::shared_ptr<RetentionDiagnosticState>>
    g_retentionDiagnosticStates;
std::atomic<uint64_t> g_retentionWarningLogs{0};
std::atomic<uint64_t> g_retentionCriticalLogs{0};
#if defined(TQ_UNIT_TESTING)
std::atomic<bool> g_failNextTerminalSinkControlBlock{false};
std::atomic<uint8_t> g_failNextDiagnosticAllocation{0};
std::atomic<uint8_t> g_failNextDiagnosticEmit{0};
#endif

void ReleaseTerminalSinkPending() noexcept {
    auto pending = g_terminalSinkPending.load(std::memory_order_relaxed);
    while (pending != 0 &&
           !g_terminalSinkPending.compare_exchange_weak(
               pending, pending - 1,
               std::memory_order_relaxed,
               std::memory_order_relaxed)) {
    }
}

void PollTerminalRetentionDiagnostics(
    std::chrono::steady_clock::time_point now) noexcept {
    try {
        auto inject = [](TqTerminalScheduler::DiagnosticAllocationStage stage) {
#if defined(TQ_UNIT_TESTING)
            const uint8_t expected = static_cast<uint8_t>(stage) + 1;
            uint8_t value = expected;
            if (g_failNextDiagnosticAllocation.compare_exchange_strong(
                    value, 0, std::memory_order_relaxed)) {
                throw std::bad_alloc();
            }
#else
            (void)stage;
#endif
        };
        inject(TqTerminalScheduler::DiagnosticAllocationStage::Snapshot);
        const auto snapshots = TqSnapshotTerminalRetentionsAt({}, now);
        struct DiagnosticValue { uint64_t StreamId; uint64_t AgeMs; };
        std::unordered_map<std::string, DiagnosticValue> ages;
        ages.reserve(snapshots.size());
        for (const auto& snapshot : snapshots) {
            const auto& id = snapshot.Identity;
            const std::string key = std::to_string(id.ConnectionId) + ":" +
                std::to_string(id.ConnectionGeneration) + ":" +
                std::to_string(id.StreamId) + ":" +
                std::to_string(static_cast<unsigned>(id.Role)) + ":" +
                std::to_string(static_cast<unsigned>(id.Backend));
            ages[key] = {id.StreamId, snapshot.RetainedAgeMs};
        }
        struct DiagnosticLog {
            std::string Key;
            uint64_t StreamId;
            uint64_t AgeMs;
            const char* Severity;
            uint8_t CommittedBit;
            uint8_t ReservedBit;
        };
        struct ReservationTicket {
            std::shared_ptr<RetentionDiagnosticState> State;
            uint8_t CommittedBit{0};
            uint8_t ReservedBit{0};
            size_t CandidateIndex{0};
            bool Committed{false};
            ReservationTicket(
                std::shared_ptr<RetentionDiagnosticState> state,
                uint8_t committedBit,
                uint8_t reservedBit,
                size_t candidateIndex) noexcept
                : State(std::move(state)),
                  CommittedBit(committedBit),
                  ReservedBit(reservedBit),
                  CandidateIndex(candidateIndex) {}
            ReservationTicket(const ReservationTicket&) = delete;
            ReservationTicket& operator=(const ReservationTicket&) = delete;
            ReservationTicket(ReservationTicket&& other) noexcept
                : State(std::move(other.State)),
                  CommittedBit(other.CommittedBit),
                  ReservedBit(other.ReservedBit),
                  CandidateIndex(other.CandidateIndex),
                  Committed(other.Committed) {
                other.Committed = true;
            }
            ReservationTicket& operator=(ReservationTicket&&) = delete;
            ~ReservationTicket() noexcept {
                if (!Committed && State) {
                    State->Bits.fetch_and(
                        static_cast<uint8_t>(~ReservedBit), std::memory_order_release);
                }
            }
            void Commit() noexcept {
                State->Bits.fetch_or(CommittedBit, std::memory_order_release);
                State->Bits.fetch_and(
                    static_cast<uint8_t>(~ReservedBit), std::memory_order_release);
                Committed = true;
            }
        };
        inject(TqTerminalScheduler::DiagnosticAllocationStage::Log);
        std::vector<DiagnosticLog> candidates;
        candidates.reserve(ages.size() * 2);
        for (const auto& item : ages) {
            if (item.second.AgeMs > 5000) {
                candidates.push_back(
                    {item.first, item.second.StreamId, item.second.AgeMs,
                     "warning", 1u, 4u});
            }
            if (item.second.AgeMs > 30000) {
                candidates.push_back(
                    {item.first, item.second.StreamId, item.second.AgeMs,
                     "critical", 2u, 8u});
            }
        }
        std::vector<std::shared_ptr<RetentionDiagnosticState>> candidateStates;
        candidateStates.reserve(candidates.size());
        std::vector<ReservationTicket> tickets;
        tickets.reserve(candidates.size());
        {
            std::lock_guard<std::mutex> guard(g_retentionDiagnosticLock);
            for (auto it = g_retentionDiagnosticStates.begin();
                 it != g_retentionDiagnosticStates.end();) {
                if (ages.find(it->first) == ages.end()) {
                    it = g_retentionDiagnosticStates.erase(it);
                } else {
                    ++it;
                }
            }
            inject(TqTerminalScheduler::DiagnosticAllocationStage::State);
            g_retentionDiagnosticStates.reserve(
                g_retentionDiagnosticStates.size() + ages.size());
            for (const auto& item : ages) {
                auto found = g_retentionDiagnosticStates.find(item.first);
                if (found == g_retentionDiagnosticStates.end()) {
                    found = g_retentionDiagnosticStates.emplace(
                        item.first, std::make_shared<RetentionDiagnosticState>()).first;
                }
            }
            for (const auto& candidate : candidates) {
                candidateStates.push_back(
                    g_retentionDiagnosticStates.find(candidate.Key)->second);
            }
        }
        for (size_t i = 0; i != candidates.size(); ++i) {
            const auto& candidate = candidates[i];
            const auto& state = candidateStates[i];
            uint8_t bits = state->Bits.load(std::memory_order_acquire);
            while ((bits & (candidate.CommittedBit | candidate.ReservedBit)) == 0) {
                if (state->Bits.compare_exchange_weak(
                        bits, static_cast<uint8_t>(bits | candidate.ReservedBit),
                        std::memory_order_acq_rel, std::memory_order_acquire)) {
                    tickets.emplace_back(
                        state, candidate.CommittedBit, candidate.ReservedBit, i);
                    break;
                }
            }
        }
        bool emitFailed = false;
        for (auto& ticket : tickets) {
            const auto& item = candidates[ticket.CandidateIndex];
            int emitResult = 0;
#if defined(TQ_UNIT_TESTING)
            const uint8_t mode = g_failNextDiagnosticEmit.exchange(
                0, std::memory_order_relaxed);
            if (mode == 2) throw std::bad_alloc();
            if (mode == 1) {
                emitResult = -1;
            } else
#endif
            emitResult = std::fprintf(stderr,
                "tcpquic-proxy: terminal retention %s stream_id=%llu oldest_age_ms=%llu\n",
                item.Severity, static_cast<unsigned long long>(item.StreamId),
                static_cast<unsigned long long>(item.AgeMs));
            if (emitResult < 0) {
                emitFailed = true;
                continue;
            }
            ticket.Commit();
            if (ticket.CommittedBit == 1u) {
                g_retentionWarningLogs.fetch_add(1, std::memory_order_relaxed);
            } else {
                g_retentionCriticalLogs.fetch_add(1, std::memory_order_relaxed);
            }
        }
        if (emitFailed) {
            g_schedulerFailure.fetch_add(1, std::memory_order_relaxed);
        }
    } catch (...) {
        g_schedulerFailure.fetch_add(1, std::memory_order_relaxed);
    }
}

enum class ScheduledKind : uint8_t { Retry, Watchdog, Timeout };

struct ScheduledTask {
    std::chrono::steady_clock::time_point Due{};
    uint64_t Sequence{0};
    ScheduledKind Kind{ScheduledKind::Retry};
    uint64_t StreamId{0};
    uint64_t ErrorCode{0};
    uint32_t CompletedAttempt{0};
    std::weak_ptr<TqStreamLifetime> Owner;
    std::shared_ptr<TqTerminalLedger> Ledger;
    std::shared_ptr<TqTerminalEscalation> Escalation;
};

struct LaterTask {
    bool operator()(const ScheduledTask& left, const ScheduledTask& right) const {
        return left.Due > right.Due ||
            (left.Due == right.Due && left.Sequence > right.Sequence);
    }
};

class TaskQueue : public std::priority_queue<
    ScheduledTask, std::vector<ScheduledTask>, LaterTask> {
public:
    uint64_t Erase(uint64_t streamId) noexcept {
        uint64_t timeouts = 0;
        const auto end = std::remove_if(c.begin(), c.end(), [&](const auto& task) {
            if (task.StreamId != streamId) return false;
            if (task.Kind == ScheduledKind::Timeout) ++timeouts;
            return true;
        });
        c.erase(end, c.end());
        std::make_heap(c.begin(), c.end(), comp);
        return timeouts;
    }
    bool HasStream(uint64_t streamId) const noexcept {
        return std::any_of(c.begin(), c.end(),
            [&](const auto& task) { return task.StreamId == streamId; });
    }
    std::vector<ScheduledTask> TakeAll() noexcept {
        return std::move(c);
    }
    void ShiftEarlier(std::chrono::milliseconds delta) noexcept {
        for (auto& task : c) task.Due -= delta;
        std::make_heap(c.begin(), c.end(), comp);
    }
};

struct SchedulerState {
    std::mutex Mutex;
    std::condition_variable Wake;
    TaskQueue Tasks;
    std::unordered_map<uint64_t, std::shared_ptr<TqTerminalLedger>> Ledgers;
    std::thread Worker;
    bool Running{false};
    bool Stopping{false};
    enum class Lifecycle : uint8_t { Ready, Running, Stopping, Stopped };
    Lifecycle State{Lifecycle::Ready};
    uint64_t LifecycleGeneration{0};
    bool RestartRequested{false};
    uint64_t Sequence{0};
#if defined(TQ_UNIT_TESTING)
    bool FakeClock{false};
    std::chrono::steady_clock::time_point FakeNow{std::chrono::steady_clock::now()};
    std::function<void()> BeforeExecute;
    std::function<void()> BeforeWorkerReturn;
    std::function<void()> AfterEnqueue;
    uint32_t FailAllocations{0};
    bool FailThreadStart{false};
    std::chrono::milliseconds DiagnosticPollInterval{std::chrono::seconds(1)};
    bool HasDiagnosticNow{false};
    std::chrono::steady_clock::time_point DiagnosticNow{};
#endif
    ~SchedulerState() {
        {
            std::lock_guard<std::mutex> guard(Mutex);
            Stopping = true;
        }
        Wake.notify_all();
        if (Worker.joinable()) Worker.join();
    }
};

SchedulerState g_scheduler;

bool InjectAllocationFailureLocked() noexcept {
#if defined(TQ_UNIT_TESTING)
    if (g_scheduler.FailAllocations != 0) {
        --g_scheduler.FailAllocations;
        g_schedulerFailure.fetch_add(1, std::memory_order_relaxed);
        return true;
    }
#endif
    return false;
}

void CleanupIndexLocked(uint64_t streamId) noexcept {
    if (!g_scheduler.Tasks.HasStream(streamId)) {
        g_scheduler.Ledgers.erase(streamId);
    }
}

std::chrono::steady_clock::time_point SchedulerNowLocked() {
#if defined(TQ_UNIT_TESTING)
    if (g_scheduler.FakeClock) return g_scheduler.FakeNow;
#endif
    return std::chrono::steady_clock::now();
}

std::chrono::steady_clock::time_point DiagnosticNowLocked() {
#if defined(TQ_UNIT_TESTING)
    if (g_scheduler.HasDiagnosticNow) return g_scheduler.DiagnosticNow;
#endif
    return std::chrono::steady_clock::now();
}

std::chrono::milliseconds DiagnosticPollIntervalLocked() {
#if defined(TQ_UNIT_TESTING)
    return g_scheduler.DiagnosticPollInterval;
#else
    return std::chrono::seconds(1);
#endif
}

void DecrementPendingTimeout() noexcept {
    auto value = g_terminalTimeoutPending.load(std::memory_order_relaxed);
    while (value != 0 && !g_terminalTimeoutPending.compare_exchange_weak(
        value, value - 1, std::memory_order_relaxed, std::memory_order_relaxed)) {}
}

bool EnqueueTaskLocked(ScheduledTask task) noexcept {
    if (InjectAllocationFailureLocked()) return false;
    const auto streamId = task.StreamId;
    const bool existed = g_scheduler.Ledgers.find(streamId) != g_scheduler.Ledgers.end();
    try {
        g_scheduler.Ledgers[streamId] = task.Ledger;
        task.Sequence = ++g_scheduler.Sequence;
        g_scheduler.Tasks.push(std::move(task));
        return true;
    } catch (...) {
        if (!existed) g_scheduler.Ledgers.erase(streamId);
        g_schedulerFailure.fetch_add(1, std::memory_order_relaxed);
        return false;
    }
}

void RunAfterEnqueueHookForTestLocked() noexcept {
#if defined(TQ_UNIT_TESTING)
    std::function<void()> hook;
    try { hook = g_scheduler.AfterEnqueue; } catch (...) {}
    if (hook) hook();
#endif
}

bool QueueTimeout(const ScheduledTask& source) noexcept {
    std::unique_lock<std::mutex> guard(g_scheduler.Mutex);
    ScheduledTask timeout = source;
    timeout.Kind = ScheduledKind::Timeout;
    timeout.Due = SchedulerNowLocked() + std::chrono::seconds(30);
    if (!EnqueueTaskLocked(std::move(timeout))) {
        return false;
    }
    g_terminalTimeoutPending.fetch_add(1, std::memory_order_relaxed);
    RunAfterEnqueueHookForTestLocked();
    if (g_scheduler.State == SchedulerState::Lifecycle::Stopping) {
        g_scheduler.RestartRequested = true;
        return true;
    }
    if (g_scheduler.State == SchedulerState::Lifecycle::Stopped) {
        g_scheduler.State = SchedulerState::Lifecycle::Ready;
    }
    if (!StartWorkerWithLifecycleLocked()) {
        g_scheduler.Tasks.Erase(source.StreamId);
        CleanupIndexLocked(source.StreamId);
        DecrementPendingTimeout();
        return false;
    }
    g_scheduler.Wake.notify_all();
    return true;
}

void Escalate(const ScheduledTask& task) noexcept {
    bool call = false;
    QUIC_STATUS status = QUIC_STATUS_SUCCESS;
    TqTerminalSchedulerInternals::MarkEscalated(task.Ledger, call, status);
    if (!call) return;
    g_connectionEscalation.fetch_add(1, std::memory_order_relaxed);
    if (task.Escalation) {
        const auto& identity = task.Ledger->Identity();
        task.Escalation->RequestConnectionShutdown(
            identity.ConnectionId, identity.StreamId, status, task.ErrorCode);
    }
    (void)QueueTimeout(task);
}

void ExecuteTask(ScheduledTask task) noexcept {
    const auto cleanup = [&] {
        std::lock_guard<std::mutex> guard(g_scheduler.Mutex);
        CleanupIndexLocked(task.StreamId);
    };
#if defined(TQ_UNIT_TESTING)
    std::function<void()> beforeExecute;
    {
        std::lock_guard<std::mutex> guard(g_scheduler.Mutex);
        try {
            beforeExecute = g_scheduler.BeforeExecute;
        } catch (...) {
            g_schedulerFailure.fetch_add(1, std::memory_order_relaxed);
        }
    }
    if (beforeExecute) beforeExecute();
#endif
    if (task.Kind == ScheduledKind::Retry) {
        if (!TqTerminalSchedulerInternals::RetryAllowed(task.Ledger)) {
            cleanup();
            return;
        }
        auto owner = task.Owner.lock();
        if (owner) (void)TqTerminalSchedulerInternals::Retry(owner);
        cleanup();
        return;
    }
    if (task.Kind == ScheduledKind::Watchdog) {
        Escalate(task);
        cleanup();
        return;
    }
    const bool timedOut = TqTerminalSchedulerInternals::MarkTimedOut(task.Ledger);
    DecrementPendingTimeout();
    if (timedOut) g_watchdogTimeout.fetch_add(1, std::memory_order_relaxed);
    cleanup();
}

#if defined(TQ_UNIT_TESTING)
void RunDueTasksForTest() {
    for (;;) {
        ScheduledTask task;
        {
            std::lock_guard<std::mutex> guard(g_scheduler.Mutex);
            if (g_scheduler.Tasks.empty() ||
                g_scheduler.Tasks.top().Due > SchedulerNowLocked()) return;
            task = g_scheduler.Tasks.top();
            g_scheduler.Tasks.pop();
        }
        ExecuteTask(std::move(task));
    }
}
#endif
}

TqTerminalScheduler& TqTerminalScheduler::Instance() {
    static TqTerminalScheduler scheduler;
    return scheduler;
}

bool StartWorkerWithLifecycleLocked() noexcept {
#if defined(TQ_UNIT_TESTING)
    if (g_scheduler.FakeClock) {
        g_scheduler.State = SchedulerState::Lifecycle::Running;
        return true;
    }
#endif
    if (g_scheduler.Running) {
        g_scheduler.State = SchedulerState::Lifecycle::Running;
        return true;
    }
    if (g_scheduler.Worker.joinable()) return false;
    try {
        if (InjectAllocationFailureLocked()) return false;
#if defined(TQ_UNIT_TESTING)
        if (g_scheduler.FailThreadStart) {
            g_scheduler.FailThreadStart = false;
            g_schedulerFailure.fetch_add(1, std::memory_order_relaxed);
            return false;
        }
#endif
        g_scheduler.Stopping = false;
        g_scheduler.Running = true;
        g_scheduler.State = SchedulerState::Lifecycle::Running;
        g_scheduler.Worker = std::thread([] {
        std::unique_lock<std::mutex> lock(g_scheduler.Mutex);
        while (!g_scheduler.Stopping) {
            if (g_scheduler.Tasks.empty()) {
                const auto interval = DiagnosticPollIntervalLocked();
                if (g_scheduler.Wake.wait_for(lock, interval) == std::cv_status::timeout) {
                    const auto now = DiagnosticNowLocked();
                    lock.unlock();
                    PollTerminalRetentionDiagnostics(now);
                    lock.lock();
                }
                continue;
            }
            const auto due = g_scheduler.Tasks.top().Due;
            const auto diagnosticDue = std::min(
                due, std::chrono::steady_clock::now() + DiagnosticPollIntervalLocked());
            if (g_scheduler.Wake.wait_until(lock, diagnosticDue) != std::cv_status::timeout) continue;
            if (std::chrono::steady_clock::now() < due) {
                lock.unlock();
                PollTerminalRetentionDiagnostics(std::chrono::steady_clock::now());
                lock.lock();
                continue;
            }
            ScheduledTask task = g_scheduler.Tasks.top();
            g_scheduler.Tasks.pop();
            lock.unlock();
            ExecuteTask(std::move(task));
            PollTerminalRetentionDiagnostics(std::chrono::steady_clock::now());
            lock.lock();
        }
        g_scheduler.Running = false;
#if defined(TQ_UNIT_TESTING)
        std::function<void()> beforeReturn;
        try {
            beforeReturn = g_scheduler.BeforeWorkerReturn;
        } catch (...) {
            g_schedulerFailure.fetch_add(1, std::memory_order_relaxed);
        }
        lock.unlock();
        if (beforeReturn) beforeReturn();
#endif
    });
    } catch (...) {
        g_scheduler.Running = false;
        g_scheduler.State = SchedulerState::Lifecycle::Ready;
        g_schedulerFailure.fetch_add(1, std::memory_order_relaxed);
        return false;
    }
    return true;
}

void TqTerminalScheduler::Start() {
    std::lock_guard<std::mutex> guard(g_scheduler.Mutex);
    if (g_scheduler.State == SchedulerState::Lifecycle::Stopping) {
        g_scheduler.RestartRequested = true;
        return;
    }
    if (g_scheduler.State == SchedulerState::Lifecycle::Stopped) {
        g_scheduler.State = SchedulerState::Lifecycle::Ready;
    }
    (void)StartWorkerWithLifecycleLocked();
}

void TqTerminalScheduler::Stop() {
    std::vector<ScheduledTask> stoppedTasks;
    std::unordered_map<uint64_t, std::shared_ptr<TqTerminalLedger>> stoppedLedgers;
    std::thread stoppedWorker;
    uint64_t generation = 0;
    {
        std::unique_lock<std::mutex> guard(g_scheduler.Mutex);
        while (g_scheduler.State == SchedulerState::Lifecycle::Stopping) {
            g_scheduler.Wake.wait(guard);
        }
        if (g_scheduler.State == SchedulerState::Lifecycle::Stopped) return;
        g_scheduler.State = SchedulerState::Lifecycle::Stopping;
        generation = ++g_scheduler.LifecycleGeneration;
        g_scheduler.Stopping = true;
        g_scheduler.RestartRequested = false;
        stoppedTasks = g_scheduler.Tasks.TakeAll();
        stoppedLedgers = std::move(g_scheduler.Ledgers);
        if (g_scheduler.Worker.joinable()) {
            stoppedWorker = std::move(g_scheduler.Worker);
        }
    }
    g_scheduler.Wake.notify_all();
    for (const auto& task : stoppedTasks) {
        if (task.Kind == ScheduledKind::Timeout) {
            DecrementPendingTimeout();
        }
    }
    for (const auto& item : stoppedLedgers) {
        if (TqTerminalSchedulerInternals::Cancel(item.second)) {
            g_watchdogCanceled.fetch_add(1, std::memory_order_relaxed);
        }
    }
    if (stoppedWorker.joinable()) stoppedWorker.join();
    {
        std::lock_guard<std::mutex> guard(g_scheduler.Mutex);
        if (g_scheduler.LifecycleGeneration == generation &&
            g_scheduler.State == SchedulerState::Lifecycle::Stopping) {
            g_scheduler.Running = false;
            g_scheduler.Stopping = false;
            if (g_scheduler.RestartRequested || !g_scheduler.Tasks.empty()) {
                g_scheduler.RestartRequested = false;
                g_scheduler.State = SchedulerState::Lifecycle::Ready;
                (void)StartWorkerWithLifecycleLocked();
            } else {
                g_scheduler.State = SchedulerState::Lifecycle::Stopped;
            }
        }
    }
    g_scheduler.Wake.notify_all();
}

bool TqTerminalScheduler::ScheduleRetry(
    std::weak_ptr<TqStreamLifetime> owner,
    std::shared_ptr<TqTerminalLedger> ledger,
    std::shared_ptr<TqTerminalEscalation> escalation,
    uint64_t errorCode,
    uint32_t completedAttempt) noexcept {
    if (!ledger || completedAttempt == 0) return false;
    if (completedAttempt >= 4) {
        ScheduledTask task{};
        task.StreamId = ledger->Identity().StreamId;
        task.ErrorCode = errorCode;
        task.Ledger = std::move(ledger);
        task.Escalation = std::move(escalation);
        {
            std::lock_guard<std::mutex> guard(g_scheduler.Mutex);
            task.Due = SchedulerNowLocked();
        }
        Escalate(task);
        return false;
    }
    static constexpr std::chrono::milliseconds delays[] = {
        std::chrono::milliseconds(10), std::chrono::milliseconds(50),
        std::chrono::milliseconds(250)};
    ScheduledTask task{};
    task.Kind = ScheduledKind::Retry;
    task.StreamId = ledger->Identity().StreamId;
    task.ErrorCode = errorCode;
    task.CompletedAttempt = completedAttempt;
    task.Owner = std::move(owner);
    task.Ledger = std::move(ledger);
    task.Escalation = std::move(escalation);
    {
        std::lock_guard<std::mutex> guard(g_scheduler.Mutex);
        task.Due = SchedulerNowLocked() + delays[completedAttempt - 1];
    }
    const auto streamId = task.StreamId;
    const bool queued = TqTerminalSchedulerInternals::RetryAndEnqueue(
        task.Ledger, [&] {
            std::lock_guard<std::mutex> guard(g_scheduler.Mutex);
            if (g_scheduler.State == SchedulerState::Lifecycle::Stopping ||
                g_scheduler.State == SchedulerState::Lifecycle::Stopped) return false;
            if (!EnqueueTaskLocked(std::move(task))) return false;
            RunAfterEnqueueHookForTestLocked();
            if (StartWorkerWithLifecycleLocked()) return true;
            g_scheduler.Tasks.Erase(streamId);
            CleanupIndexLocked(streamId);
            return false;
        });
    if (!queued) return false;
    g_scheduler.Wake.notify_all();
    return true;
}

void TqTerminalScheduler::ArmWatchdog(
    std::weak_ptr<TqStreamLifetime> owner,
    std::shared_ptr<TqTerminalLedger> ledger,
    std::shared_ptr<TqTerminalEscalation> escalation,
    uint64_t errorCode,
    std::chrono::seconds deadline) noexcept {
    if (!ledger) return;
    ScheduledTask task{};
    task.Kind = ScheduledKind::Watchdog;
    task.StreamId = ledger->Identity().StreamId;
    task.ErrorCode = errorCode;
    task.Owner = std::move(owner);
    task.Ledger = std::move(ledger);
    task.Escalation = std::move(escalation);
    {
        std::lock_guard<std::mutex> guard(g_scheduler.Mutex);
        task.Due = SchedulerNowLocked() + deadline;
    }
    const auto failedTask = task;
    const auto streamId = task.StreamId;
    const bool queued = TqTerminalSchedulerInternals::ArmAndEnqueue(
        task.Ledger, [&] {
            std::lock_guard<std::mutex> guard(g_scheduler.Mutex);
            if (g_scheduler.State == SchedulerState::Lifecycle::Stopping ||
                g_scheduler.State == SchedulerState::Lifecycle::Stopped) return false;
            if (!EnqueueTaskLocked(std::move(task))) return false;
            RunAfterEnqueueHookForTestLocked();
            if (StartWorkerWithLifecycleLocked()) return true;
            g_scheduler.Tasks.Erase(streamId);
            CleanupIndexLocked(streamId);
            return false;
        });
    if (!queued) {
        Escalate(failedTask);
        return;
    }
    g_watchdogArmed.fetch_add(1, std::memory_order_relaxed);
    g_scheduler.Wake.notify_all();
}

void TqTerminalScheduler::Cancel(uint64_t streamId) noexcept {
    std::shared_ptr<TqTerminalLedger> ledger;
    uint64_t removedTimeouts = 0;
    {
        std::lock_guard<std::mutex> guard(g_scheduler.Mutex);
        const auto found = g_scheduler.Ledgers.find(streamId);
        if (found != g_scheduler.Ledgers.end()) ledger = found->second;
    }
    if (ledger && TqTerminalSchedulerInternals::Cancel(ledger)) {
        g_watchdogCanceled.fetch_add(1, std::memory_order_relaxed);
    }
    {
        std::lock_guard<std::mutex> guard(g_scheduler.Mutex);
        removedTimeouts = g_scheduler.Tasks.Erase(streamId);
        g_scheduler.Ledgers.erase(streamId);
    }
    while (removedTimeouts-- != 0) DecrementPendingTimeout();
    g_scheduler.Wake.notify_all();
}

#if defined(TQ_UNIT_TESTING)
void TqTerminalScheduler::ResetForTest() {
    Instance().Stop();
    g_retentionWarningLogs.store(0, std::memory_order_relaxed);
    g_retentionCriticalLogs.store(0, std::memory_order_relaxed);
    {
        std::lock_guard<std::mutex> diagnosticGuard(g_retentionDiagnosticLock);
        g_retentionDiagnosticStates.clear();
    }
    std::lock_guard<std::mutex> guard(g_scheduler.Mutex);
    g_scheduler.Tasks = {};
    g_scheduler.Ledgers.clear();
    g_scheduler.Sequence = 0;
    g_scheduler.Stopping = false;
    g_scheduler.State = SchedulerState::Lifecycle::Ready;
    g_scheduler.FakeClock = true;
    g_scheduler.FakeNow = std::chrono::steady_clock::now();
    g_scheduler.BeforeExecute = {};
    g_scheduler.BeforeWorkerReturn = {};
    g_scheduler.AfterEnqueue = {};
    g_scheduler.FailAllocations = 0;
    g_scheduler.FailThreadStart = false;
    g_scheduler.DiagnosticPollInterval = std::chrono::seconds(1);
    g_scheduler.HasDiagnosticNow = false;
}

void TqTerminalScheduler::AdvanceForTest(std::chrono::milliseconds delta) {
    {
        std::lock_guard<std::mutex> guard(g_scheduler.Mutex);
        if (g_scheduler.FakeClock) {
            g_scheduler.FakeNow += delta;
        } else {
            g_scheduler.Tasks.ShiftEarlier(delta);
        }
    }
    RunDueTasksForTest();
    PollTerminalRetentionDiagnostics(NowForTest());
}

std::chrono::steady_clock::time_point TqTerminalScheduler::NowForTest() {
    std::lock_guard<std::mutex> guard(g_scheduler.Mutex);
    return SchedulerNowLocked();
}

TqTerminalScheduler::TestSnapshot TqTerminalScheduler::SnapshotForTest() {
    std::lock_guard<std::mutex> guard(g_scheduler.Mutex);
    TestSnapshot snapshot{};
    snapshot.PendingTasks = g_scheduler.Tasks.size();
    snapshot.IndexedStreams = g_scheduler.Ledgers.size();
    snapshot.CanceledStreams = 0;
    snapshot.Running = g_scheduler.Running;
    snapshot.Joinable = g_scheduler.Worker.joinable();
    snapshot.LifecycleGeneration = g_scheduler.LifecycleGeneration;
    snapshot.RestartRequested = g_scheduler.RestartRequested;
    return snapshot;
}

void TqTerminalScheduler::SetBeforeExecuteForTest(std::function<void()> hook) {
    std::lock_guard<std::mutex> guard(g_scheduler.Mutex);
    g_scheduler.BeforeExecute = std::move(hook);
}

void TqTerminalScheduler::SetBeforeWorkerReturnForTest(std::function<void()> hook) {
    std::lock_guard<std::mutex> guard(g_scheduler.Mutex);
    g_scheduler.BeforeWorkerReturn = std::move(hook);
}

void TqTerminalScheduler::SetAfterEnqueueForTest(std::function<void()> hook) {
    std::lock_guard<std::mutex> guard(g_scheduler.Mutex);
    g_scheduler.AfterEnqueue = std::move(hook);
}

void TqTerminalScheduler::UseRealClockForTest() {
    std::lock_guard<std::mutex> guard(g_scheduler.Mutex);
    g_scheduler.FakeClock = false;
}

void TqTerminalScheduler::FailNextAllocationForTest(uint32_t count) noexcept {
    std::lock_guard<std::mutex> guard(g_scheduler.Mutex);
    g_scheduler.FailAllocations = count;
}

void TqTerminalScheduler::FailNextThreadStartForTest() noexcept {
    std::lock_guard<std::mutex> guard(g_scheduler.Mutex);
    g_scheduler.FailThreadStart = true;
}

void TqTerminalScheduler::SetDiagnosticPollIntervalForTest(
    std::chrono::milliseconds interval) {
    std::lock_guard<std::mutex> guard(g_scheduler.Mutex);
    g_scheduler.DiagnosticPollInterval = interval;
    g_scheduler.Wake.notify_all();
}

void TqTerminalScheduler::SetDiagnosticNowForTest(
    std::chrono::steady_clock::time_point now) {
    std::lock_guard<std::mutex> guard(g_scheduler.Mutex);
    g_scheduler.DiagnosticNow = now;
    g_scheduler.HasDiagnosticNow = true;
    g_scheduler.Wake.notify_all();
}

void TqTerminalScheduler::FailNextDiagnosticAllocationForTest(
    DiagnosticAllocationStage stage) noexcept {
    g_failNextDiagnosticAllocation.store(
        static_cast<uint8_t>(stage) + 1, std::memory_order_relaxed);
}

void TqTerminalScheduler::FailNextDiagnosticEmitForTest(
    bool throwException) noexcept {
    g_failNextDiagnosticEmit.store(
        throwException ? 2u : 1u, std::memory_order_relaxed);
}
#endif

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
        if (State_.Watchdog == TqTerminalWatchdogState::Armed ||
            State_.Watchdog == TqTerminalWatchdogState::Escalated) {
            State_.Watchdog = TqTerminalWatchdogState::Canceled;
            g_watchdogCanceled.fetch_add(1, std::memory_order_relaxed);
        }
        State_.TerminalObservedAtMs = static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::system_clock::now().time_since_epoch()).count());
    }
}

void TqTerminalLedger::RecordShutdown(
    QUIC_STATUS status,
    uint32_t attempt,
    bool submitted,
    TqTerminalShutdownIntent intent) noexcept {
    if (submitted) {
        g_shutdownSubmitted.fetch_add(1, std::memory_order_relaxed);
        if (status == QUIC_STATUS_PENDING) {
            g_shutdownPending.fetch_add(1, std::memory_order_relaxed);
        }
    } else if (QUIC_FAILED(status)) {
        g_shutdownSyncFailure.fetch_add(1, std::memory_order_relaxed);
    }
    if (attempt > 1) g_shutdownRetry.fetch_add(1, std::memory_order_relaxed);
    std::lock_guard<std::mutex> guard(Mutex_);
    if (attempt < State_.ShutdownAttempt) {
        return;
    }
    State_.ShutdownStatus = status;
    State_.ShutdownIntent = intent;
    State_.ShutdownAttempt = attempt;
    if (State_.Phase != TerminalPhase::TerminalObserved &&
        State_.Phase != TerminalPhase::Closed) {
        State_.Phase = submitted ? TerminalPhase::ShutdownSubmitted : TerminalPhase::Active;
    }
    if (submitted) {
        State_.ShutdownSubmittedAtMs = static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::system_clock::now().time_since_epoch()).count());
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
    std::shared_ptr<TqTerminalLedger> ledger,
    std::function<void()> onTerminal) noexcept
    : Owner_(std::move(owner)), Ledger_(std::move(ledger)),
      OnTerminal_(std::move(onTerminal)) {}

TqTerminalSink::~TqTerminalSink() noexcept {
    ReleasePendingOnce();
}

void TqTerminalSink::ReleasePendingOnce() noexcept {
    if (Pending_.exchange(false, std::memory_order_acq_rel)) {
        ReleaseTerminalSinkPending();
    }
}

void TqTerminalSink::ArmPending() noexcept {
    Pending_.store(true, std::memory_order_release);
}

#if defined(TQ_UNIT_TESTING)
void TqTerminalSink::SetFailNextControlBlockForTest(bool fail) noexcept {
    g_failNextTerminalSinkControlBlock.store(fail, std::memory_order_release);
}
#endif

std::shared_ptr<TqTerminalSink> TqTerminalSink::Create(
    std::weak_ptr<TqStreamLifetime> owner,
    std::shared_ptr<TqTerminalLedger> ledger,
    std::function<void()> onTerminal) noexcept {
    auto lockedOwner = owner.lock();
    if (lockedOwner == nullptr || ledger == nullptr ||
        lockedOwner->TerminalLedger().get() != ledger.get()) {
        TqRecordTerminalExactlyOnceViolation();
        return nullptr;
    }
    auto* raw = new (std::nothrow) TqTerminalSink(
        std::move(owner), std::move(ledger), std::move(onTerminal));
    if (raw == nullptr) {
        return nullptr;
    }
#if defined(TQ_UNIT_TESTING)
    if (g_failNextTerminalSinkControlBlock.exchange(false, std::memory_order_acq_rel)) {
        delete raw;
        return nullptr;
    }
#endif
    try {
        std::shared_ptr<TqTerminalSink> sink(raw);
        g_terminalSinkPending.fetch_add(1, std::memory_order_relaxed);
        sink->ArmPending();
        return sink;
    } catch (...) {
        // shared_ptr 的 pointer constructor 分配 control block 失败时负责 delete raw。
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
        } else {
            g_duplicateTerminalSuppressed.fetch_add(1, std::memory_order_relaxed);
        }
        ReleasePendingOnce();
        if (OnTerminal_) {
            auto callback = std::move(OnTerminal_);
            callback();
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

const char* TqTerminalShutdownIntentName(TqTerminalShutdownIntent intent) noexcept {
    switch (intent) {
    case TqTerminalShutdownIntent::None: return "none";
    case TqTerminalShutdownIntent::GracefulComplete: return "graceful_complete";
    case TqTerminalShutdownIntent::AbortBothImmediate: return "abort_both_immediate";
    }
    return "unknown";
}

std::string TqTerminalShutdownStatusName(QUIC_STATUS status) {
    if (status == QUIC_STATUS_SUCCESS) return "success";
    if (status == QUIC_STATUS_PENDING) return "pending";
    if (status == QUIC_STATUS_OUT_OF_MEMORY) return "out_of_memory";
    if (status == QUIC_STATUS_INVALID_STATE) return "invalid_state";
    return "unknown(" + std::to_string(static_cast<uint32_t>(status)) + ")";
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

bool TqParseTerminalRetentionPath(
    const std::string& path, TqTerminalRetentionFilter& filter) {
    constexpr const char* base = "/relay/terminal-retentions";
    constexpr size_t baseLength = 26;
    if (path.compare(0, baseLength, base) != 0) return false;
    if (path.size() == baseLength) return true;
    if (path[baseLength] != '?' || path.size() == baseLength + 1) return false;
    size_t offset = baseLength + 1;
    while (offset < path.size()) {
        const size_t end = path.find('&', offset);
        const std::string item = path.substr(offset, end - offset);
        const size_t equal = item.find('=');
        if (equal == std::string::npos || equal == 0 || equal + 1 == item.size()) return false;
        const std::string key = item.substr(0, equal);
        const std::string value = item.substr(equal + 1);
        auto parseId = [](const std::string& text, uint64_t& id) {
            if (text.empty()) return false;
            id = 0;
            const auto parsed = std::from_chars(text.data(), text.data() + text.size(), id);
            return parsed.ec == std::errc{} && parsed.ptr == text.data() + text.size() && id != 0;
        };
        if (key == "backend") {
            if (filter.Backend != TqRelayBackendType::None) return false;
            if (value == "linux") filter.Backend = TqRelayBackendType::LinuxWorker;
            else if (value == "windows") filter.Backend = TqRelayBackendType::WindowsWorker;
            else if (value == "darwin") filter.Backend = TqRelayBackendType::DarwinWorker;
            else return false;
        } else if (key == "connection_id") {
            if (filter.ConnectionId != 0 || !parseId(value, filter.ConnectionId)) return false;
        } else if (key == "tunnel_id") {
            if (filter.TunnelId != 0 || !parseId(value, filter.TunnelId)) return false;
        } else if (key == "terminal_phase") {
            if (filter.HasPhase) return false;
            filter.HasPhase = true;
            if (value == "active") filter.Phase = TerminalPhase::Active;
            else if (value == "shutdown_reserved") filter.Phase = TerminalPhase::ShutdownReserved;
            else if (value == "shutdown_submitted") filter.Phase = TerminalPhase::ShutdownSubmitted;
            else if (value == "terminal_observed") filter.Phase = TerminalPhase::TerminalObserved;
            else if (value == "closed") filter.Phase = TerminalPhase::Closed;
            else return false;
        } else return false;
        if (end == std::string::npos) break;
        offset = end + 1;
        if (offset == path.size()) return false;
    }
    return true;
}

std::string TqTerminalRetentionsJson(const TqTerminalRetentionFilter& filter) {
    const auto snapshots = TqSnapshotTerminalRetentions(filter);
    std::ostringstream out;
    out << "{\"retentions\":[";
    uint64_t oldest = 0;
    bool first = true;
    for (const auto& snapshot : snapshots) {
        oldest = std::max(oldest, snapshot.RetainedAgeMs);
        const char* backend = "none";
        switch (snapshot.Identity.Backend) {
        case TqRelayBackendType::LinuxWorker: backend = "linux"; break;
        case TqRelayBackendType::WindowsWorker: backend = "windows"; break;
        case TqRelayBackendType::DarwinWorker: backend = "darwin"; break;
        case TqRelayBackendType::None: break;
        }
        if (!first) out << ',';
        first = false;
        out << "{\"stream_id\":" << snapshot.Identity.StreamId
            << ",\"tunnel_id\":" << snapshot.Identity.TunnelId
            << ",\"connection_id\":" << snapshot.Identity.ConnectionId
            << ",\"connection_generation\":" << snapshot.Identity.ConnectionGeneration
            << ",\"role\":\"" << (snapshot.Identity.Role == TqTunnelRole::ClientOpen ? "client" : "server")
            << "\",\"backend\":\"" << backend
            << "\",\"terminal_phase\":\"" << TqTerminalPhaseName(snapshot.Phase)
            << "\",\"retained_age_ms\":" << snapshot.RetainedAgeMs
            << ",\"shutdown_intent\":\"" << TqTerminalShutdownIntentName(snapshot.ShutdownIntent)
            << "\",\"shutdown_status\":\"" << TqTerminalShutdownStatusName(snapshot.ShutdownStatus)
            << "\",\"shutdown_attempt\":" << snapshot.ShutdownAttempt
            << ",\"shutdown_submitted_at_ms\":" << snapshot.ShutdownSubmittedAtMs
            << ",\"terminal_observed_at_ms\":" << snapshot.TerminalObservedAtMs
            << ",\"last_stream_event\":\"" << TqTerminalEventName(snapshot.LastStreamEvent)
            << "\",\"in_tunnel_registry\":" << (snapshot.InTunnelRegistry ? "true" : "false")
            << ",\"relay_active\":" << (snapshot.RelayActive ? "true" : "false")
            << ",\"tcp_valid\":" << (snapshot.TcpValid ? "true" : "false")
            << ",\"watchdog_state\":\"" << TqTerminalWatchdogStateName(snapshot.Watchdog)
            << "\",\"connection_escalated\":" << (snapshot.ConnectionEscalated ? "true" : "false")
            << '}';
    }
    out << "],\"count\":" << snapshots.size()
        << ",\"oldest_age_ms\":" << oldest << '}';
    return out.str();
}

void TqRecordTerminalExactlyOnceViolation() noexcept {
    g_terminalExactlyOnceViolations.fetch_add(1, std::memory_order_relaxed);
}
void TqRecordTerminalHandoffStarted() noexcept { g_handoffStarted.fetch_add(1); }
void TqRecordTerminalHandoffCompleted() noexcept { g_handoffCompleted.fetch_add(1); }
void TqRecordTerminalHandoffFailed() noexcept { g_handoffFailed.fetch_add(1); }

uint64_t TqTerminalExactlyOnceViolationCount() noexcept {
    return g_terminalExactlyOnceViolations.load(std::memory_order_relaxed);
}

TqTerminalMetrics TqTerminalMetricsSnapshot() noexcept {
    TqTerminalMetrics snapshot{};
    snapshot.TerminalObserved = g_terminalObserved.load(std::memory_order_relaxed);
    snapshot.ShutdownSubmitted = g_shutdownSubmitted.load(std::memory_order_relaxed);
    snapshot.ShutdownPending = g_shutdownPending.load(std::memory_order_relaxed);
    snapshot.ShutdownSyncFailure = g_shutdownSyncFailure.load(std::memory_order_relaxed);
    snapshot.ShutdownRetry = g_shutdownRetry.load(std::memory_order_relaxed);
    snapshot.WatchdogArmed = g_watchdogArmed.load(std::memory_order_relaxed);
    snapshot.WatchdogCanceled = g_watchdogCanceled.load(std::memory_order_relaxed);
    snapshot.WatchdogTimeout = g_watchdogTimeout.load(std::memory_order_relaxed);
    snapshot.ConnectionEscalation =
        g_connectionEscalation.load(std::memory_order_relaxed);
    snapshot.TerminalTimeoutPending =
        g_terminalTimeoutPending.load(std::memory_order_relaxed);
    snapshot.TerminalSinkPending =
        g_terminalSinkPending.load(std::memory_order_relaxed);
    snapshot.DuplicateTerminalSuppressed =
        g_duplicateTerminalSuppressed.load(std::memory_order_relaxed);
    snapshot.ExactlyOnceViolation =
        g_terminalExactlyOnceViolations.load(std::memory_order_relaxed);
    snapshot.SchedulerFailure = g_schedulerFailure.load(std::memory_order_relaxed);
    snapshot.HandoffStarted = g_handoffStarted.load(std::memory_order_relaxed);
    snapshot.HandoffCompleted = g_handoffCompleted.load(std::memory_order_relaxed);
    snapshot.HandoffFailed = g_handoffFailed.load(std::memory_order_relaxed);
    return snapshot;
}

#if defined(TQ_UNIT_TESTING)
void TqResetTerminalMetricsForTest() noexcept {
    g_terminalExactlyOnceViolations.store(0, std::memory_order_relaxed);
    g_terminalObserved.store(0, std::memory_order_relaxed);
    g_terminalSinkPending.store(0, std::memory_order_relaxed);
    g_duplicateTerminalSuppressed.store(0, std::memory_order_relaxed);
    g_shutdownSubmitted.store(0, std::memory_order_relaxed);
    g_shutdownPending.store(0, std::memory_order_relaxed);
    g_shutdownSyncFailure.store(0, std::memory_order_relaxed);
    g_shutdownRetry.store(0, std::memory_order_relaxed);
    g_watchdogArmed.store(0, std::memory_order_relaxed);
    g_watchdogCanceled.store(0, std::memory_order_relaxed);
    g_watchdogTimeout.store(0, std::memory_order_relaxed);
    g_connectionEscalation.store(0, std::memory_order_relaxed);
    g_terminalTimeoutPending.store(0, std::memory_order_relaxed);
    g_schedulerFailure.store(0, std::memory_order_relaxed);
    g_handoffStarted.store(0, std::memory_order_relaxed);
    g_handoffCompleted.store(0, std::memory_order_relaxed);
    g_handoffFailed.store(0, std::memory_order_relaxed);
    g_failNextTerminalSinkControlBlock.store(false, std::memory_order_relaxed);
    g_failNextDiagnosticAllocation.store(0, std::memory_order_relaxed);
    g_failNextDiagnosticEmit.store(0, std::memory_order_relaxed);
    g_retentionWarningLogs.store(0, std::memory_order_relaxed);
    g_retentionCriticalLogs.store(0, std::memory_order_relaxed);
    std::lock_guard<std::mutex> guard(g_retentionDiagnosticLock);
    g_retentionDiagnosticStates.clear();
}

TqTerminalRetentionDiagnosticsTestSnapshot
TqTerminalRetentionDiagnosticsForTest() noexcept {
    TqTerminalRetentionDiagnosticsTestSnapshot snapshot{};
    snapshot.WarningLogs = g_retentionWarningLogs.load(std::memory_order_relaxed);
    snapshot.CriticalLogs = g_retentionCriticalLogs.load(std::memory_order_relaxed);
    std::lock_guard<std::mutex> guard(g_retentionDiagnosticLock);
    snapshot.TrackedStreams = g_retentionDiagnosticStates.size();
    return snapshot;
}
#endif
