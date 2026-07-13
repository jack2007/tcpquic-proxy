#if defined(__APPLE__)

#include "darwin_relay_event_queue.h"
#include "darwin_relay_worker.h"
#include "relay.h"
#include "relay_metrics.h"
#include "tuning.h"

#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <sstream>
#include <string>

namespace {

bool Contains(const std::string& text, const char* needle) {
    return text.find(needle) != std::string::npos;
}

void CheckContains(const std::string& text, const char* needle) {
    if (!Contains(text, needle)) {
        std::fprintf(stderr, "missing JSON fragment: %s\n", needle);
        std::abort();
    }
}

void Check(bool condition, const char* expression) {
    if (!condition) {
        std::fprintf(stderr, "check failed: %s\n", expression);
        std::abort();
    }
}

void CloseSocketPairAfterRelayOwned(int relayTcpFd, int fds[2]) {
    const int peerFd = relayTcpFd == fds[0] ? fds[1] : fds[0];
    Check(close(peerFd) == 0, "close peer fd");
}

void AssertReleaseGateGaugesZero(const char* where) {
    const TqDarwinRelayWorkerSnapshot empty{};
    (void)empty;
    const auto metrics = TqSnapshotRelayMetrics();
    if (metrics.RelayActiveControls != 0 ||
        metrics.RelayPreparedRelays != 0 ||
        metrics.RelayActiveSendReservations != 0 ||
        metrics.RelayPendingReceiveActive != 0 ||
        metrics.RelayShutdownSinkActive != 0 ||
        metrics.RelayStopRemaining != 0) {
        std::fprintf(
            stderr,
            "%s: release-gate gauges not zero "
            "(controls=%llu prepared=%llu send_res=%llu recv=%llu sink=%llu stop=%llu)\n",
            where,
            static_cast<unsigned long long>(metrics.RelayActiveControls),
            static_cast<unsigned long long>(metrics.RelayPreparedRelays),
            static_cast<unsigned long long>(metrics.RelayActiveSendReservations),
            static_cast<unsigned long long>(metrics.RelayPendingReceiveActive),
            static_cast<unsigned long long>(metrics.RelayShutdownSinkActive),
            static_cast<unsigned long long>(metrics.RelayStopRemaining));
        std::abort();
    }
}

std::atomic<uint32_t> g_workerSnapshotHookCalls{0};

void CountWorkerSnapshotHook(TqDarwinRelayWorker*) {
    g_workerSnapshotHookCalls.fetch_add(1, std::memory_order_relaxed);
}

void SnapshotWorkersRunningAndStopped() {
    auto& runtime = TqDarwinRelayRuntime::Instance();
    runtime.Stop();

    TqTuningConfig tuning{};
    tuning.RelayWorkerCount = 3;
    tuning.RelayEventQueueCapacity = 1000; // → normalized 1024
    Check(runtime.Start(tuning), "runtime.Start(3)");

    const auto running = TqSnapshotRelayWorkers();
    Check(running.SnapshotComplete, "running.SnapshotComplete");
    Check(running.IdentitiesComplete, "running.IdentitiesComplete");
    Check(running.Workers.size() == 4, "aggregate + 3 darwin rows");
    Check(running.Workers[0].WorkerId == "aggregate", "workers[0] aggregate");
    Check(std::string(running.Workers[0].Backend) == "kqueue", "aggregate backend kqueue");
    Check(running.Workers[0].SnapshotComplete, "aggregate complete");

    const uint64_t expectedCapacity =
        TqDarwinRelayEventQueue::NormalizeCapacityForTest(1000);
    Check(expectedCapacity == 1024, "normalized capacity 1024");
    Check(running.Workers[0].EventQueueCapacity == expectedCapacity, "aggregate capacity max");

    uint64_t sumActive = 0;
    uint64_t sumPending = 0;
    uint64_t sumRead = 0;
    uint64_t sumWrite = 0;
    uint64_t sumErrors = 0;
    uint64_t maxCapacity = 0;
    for (size_t i = 1; i < running.Workers.size(); ++i) {
        const auto& row = running.Workers[i];
        const uint32_t index = static_cast<uint32_t>(i - 1);
        Check(row.WorkerIndex == index, "per-row worker_index matches slot");
        Check(row.WorkerId == ("darwin-" + std::to_string(index)), "darwin-N id");
        Check(std::string(row.Backend) == "darwin", "per-row backend darwin");
        Check(row.SnapshotComplete, "per-row complete");
        Check(row.EventQueueCapacity == expectedCapacity, "per-row normalized capacity");
        sumActive += row.ActiveRelays;
        sumPending += row.PendingBytes;
        sumRead += row.TcpReadBytes;
        sumWrite += row.TcpWriteBytes;
        sumErrors += row.Errors;
        maxCapacity = std::max(maxCapacity, row.EventQueueCapacity);
    }
    Check(running.Workers[0].ActiveRelays == sumActive, "aggregate ActiveRelays sum");
    Check(running.Workers[0].PendingBytes == sumPending, "aggregate PendingBytes sum");
    Check(running.Workers[0].TcpReadBytes == sumRead, "aggregate TcpReadBytes sum");
    Check(running.Workers[0].TcpWriteBytes == sumWrite, "aggregate TcpWriteBytes sum");
    Check(running.Workers[0].Errors == sumErrors, "aggregate Errors sum");
    Check(running.Workers[0].EventQueueCapacity == maxCapacity, "aggregate capacity max");

    const std::string listJson = TqRelayWorkersJson();
    CheckContains(listJson, "\"snapshot_complete\":true");
    CheckContains(listJson, "\"worker_id\":\"aggregate\"");
    CheckContains(listJson, "\"worker_id\":\"darwin-0\"");
    CheckContains(listJson, "\"worker_id\":\"darwin-1\"");
    CheckContains(listJson, "\"worker_id\":\"darwin-2\"");

    TqRelayWorkerLookupStatus detailStatus = TqRelayWorkerLookupStatus::SnapshotUnavailable;
    const std::string detail = TqRelayWorkerDetailJson("darwin-1", detailStatus);
    Check(detailStatus == TqRelayWorkerLookupStatus::Ok, "darwin-1 detail Ok");
    CheckContains(detail, "\"worker_id\":\"darwin-1\"");
    CheckContains(detail, "\"worker_index\":1");

    runtime.Stop();
    const auto stopped = TqSnapshotRelayWorkers();
    Check(stopped.SnapshotComplete, "stopped.SnapshotComplete");
    Check(stopped.IdentitiesComplete, "stopped.IdentitiesComplete");
    Check(stopped.Workers.size() == 1, "stopped only aggregate");
    Check(stopped.Workers[0].WorkerId == "aggregate", "stopped aggregate id");
    Check(stopped.Workers[0].SnapshotComplete, "stopped aggregate complete");
}

void WorkersJsonSingleSamplesEachWorkerOnce() {
    auto& runtime = TqDarwinRelayRuntime::Instance();
    runtime.Stop();

    TqTuningConfig tuning{};
    tuning.RelayWorkerCount = 2;
    tuning.RelayEventQueueCapacity = 64;
    runtime.SetBeforeWorkerSnapshotHookForTest(CountWorkerSnapshotHook);
    Check(runtime.Start(tuning), "runtime.Start(2) for single-sample");

    g_workerSnapshotHookCalls.store(0, std::memory_order_relaxed);
    const std::string json = TqRelayWorkersJson();
    CheckContains(json, "\"worker_id\":\"darwin-0\"");
    CheckContains(json, "\"worker_id\":\"darwin-1\"");
    Check(
        g_workerSnapshotHookCalls.load(std::memory_order_relaxed) == 2,
        "each worker Snapshot once per /relay/workers");

    runtime.Stop();
    runtime.SetBeforeWorkerSnapshotHookForTest(nullptr);
}

void MetricsSnapshotCompleteCapacityAndGateStats() {
    auto& runtime = TqDarwinRelayRuntime::Instance();
    runtime.Stop();

    TqTuningConfig tuning{};
    tuning.RelayWorkerCount = 2;
    tuning.RelayEventQueueCapacity = 1000;
    Check(runtime.Start(tuning), "runtime.Start for metrics");

    const auto metrics = TqSnapshotRelayMetrics();
    Check(metrics.SnapshotComplete, "metrics.SnapshotComplete");
    Check(std::string(metrics.Backend) == "kqueue", "metrics backend");
    const uint64_t expectedCapacity =
        TqDarwinRelayEventQueue::NormalizeCapacityForTest(1000);
    Check(metrics.RelayEventQueueCapacity == expectedCapacity, "metrics capacity");
    Check(metrics.RelayRuntimeSnapshotAcquireCount >= 1, "support acquire counted");

    const std::string json = TqRelayMetricsFieldsJson(metrics);
    CheckContains(json, "\"relay_snapshot_complete\":true");
    CheckContains(json, "\"relay_event_queue_capacity\":1024");
    CheckContains(json, "\"relay_runtime_snapshot_inflight\"");
    CheckContains(json, "\"relay_runtime_snapshot_inflight_max\"");
    CheckContains(json, "\"relay_runtime_snapshot_acquire_count\"");
    CheckContains(json, "\"relay_runtime_snapshot_failure_count\"");
    CheckContains(json, "\"relay_runtime_snapshot_stop_wait_count\"");
    CheckContains(json, "\"relay_snapshot_execution_busy\"");
    CheckContains(json, "\"relay_snapshot_execution_deadline_timeouts\"");
    CheckContains(json, "\"relay_snapshot_execution_detached_late_commands\"");

    runtime.Stop();
}

void IncompleteIdentityDoesNotVacuousCompleteAggregate() {
    auto& runtime = TqDarwinRelayRuntime::Instance();
    runtime.Stop();

    TqTuningConfig tuning{};
    tuning.RelayWorkerCount = 2;
    Check(runtime.Start(tuning), "runtime.Start before incomplete");
    runtime.FailNextWorkerRefMaterializationForTest();

    const auto result = TqSnapshotRelayWorkers();
    Check(!result.IdentitiesComplete, "IdentitiesComplete=false on acquire failure");
    Check(!result.SnapshotComplete, "SnapshotComplete=false on acquire failure");
    Check(result.Workers.size() == 1, "still emit aggregate row");
    Check(result.Workers[0].WorkerId == "aggregate", "aggregate present");
    Check(!result.Workers[0].SnapshotComplete, "empty aggregate not vacuous true");

    runtime.Stop();
}

void EventizedSnapshotCountsActiveRelay() {
    int fds[2]{TqInvalidSocket, TqInvalidSocket};
    Check(socketpair(AF_UNIX, SOCK_STREAM, 0, fds) == 0, "socketpair succeeds");

    TqDarwinRelayWorker worker(TqDarwinRelayWorkerConfig{});
    TqRelayHandle handle{};
    Check(worker.Start(), "worker.Start()");

    TqDarwinRelayRegistration registration{};
    registration.TcpFd = fds[0];
    registration.Stream = reinterpret_cast<MsQuicStream*>(static_cast<uintptr_t>(1));
    if (handle.Control == nullptr) {
        handle.Control = std::make_shared<TqRelayStopControl>();
        handle.ControlGeneration = handle.Control->Generation;
    }
    registration.Control = handle.Control;
    registration.ControlGeneration = handle.Control->Generation;

    const TqDarwinRelayRegistrationResult result = worker.RegisterRelayWithId(registration);
    Check(result.Ok, "RegisterRelayWithId ok");
    handle.Stop.store(false, std::memory_order_release);
    handle.Control = registration.Control;
    handle.ControlGeneration = registration.Control->Generation;
    handle.Backend = TqRelayBackendType::DarwinWorker;
    handle.DarwinWorker = &worker;
    handle.DarwinRelayId = result.RelayId;

    const TqDarwinRelayWorkerSnapshot snapshot = worker.Snapshot();
    Check(snapshot.Errors == 0, "snapshot.Errors == 0");
    Check(snapshot.ActiveRelays == 1, "snapshot.ActiveRelays == 1");
    // Legacy (no StreamOwner) path activates without prepare/commit counters.

    worker.UnregisterRelay(result.RelayId);
    worker.Stop();
    CloseSocketPairAfterRelayOwned(registration.TcpFd, fds);
}

void MultiWorkerAggregationUsesMaxForGlobalGauges() {
    TqDarwinRelayWorkerSnapshot a{};
    a.ActiveRelays = 2;
    a.StopRemaining = 2;
    a.PreparedRelays = 1;
    a.CommitSuccessCount = 3;
    a.TerminalBeforeCommitRollbacks = 1;
    a.ActivationFailureCount = 2;
    a.PrecommitBytes = 10;
    a.PrecommitDepth = 1;
    a.PendingReceiveActive = 4;
    a.DeferredReceiveDiscards = 5;
    a.ActiveFailureAllocationFailed = 1;
    a.WorkerExitedPurgeEvents = 7;
    a.TerminalRetainedOwnerCount = 1;
    a.TerminalRetainedOldestAgeMs = 40;
    a.ActiveSendReservations = 2;
    a.PreSubmitSendRollbacks = 9;
    a.SendReservationOldestAgeMs = 15;
    a.ShutdownSinkActive = 1;
    a.StopOldestAgeMs = 20;
    a.ReceiveCompletionRequired = 4;
    a.ReceiveCompletionActiveCompleted = 2;
    a.ReceiveCompletionTerminalDiscarded = 1;
    a.ReceiveCompletionZeroLength = 3;
    a.ReceiveCompletionLeaseRetry = 1;
    a.ReceiveCompletionPending = 1;
    a.ReceiveCompletionExactlyOnceViolation = 0;

    TqDarwinRelayWorkerSnapshot b{};
    b.ActiveRelays = 3;
    b.StopRemaining = 1;
    b.PreparedRelays = 2;
    b.CommitSuccessCount = 4;
    b.TerminalBeforeCommitRollbacks = 0;
    b.ActivationFailureCount = 1;
    b.PrecommitBytes = 5;
    b.PrecommitDepth = 2;
    b.PendingReceiveActive = 1;
    b.DeferredReceiveDiscards = 2;
    b.ActiveFailureAllocationFailed = 3;
    b.WorkerExitedPurgeEvents = 1;
    b.TerminalRetainedOwnerCount = 1; // same global gauge — must not become 2
    b.TerminalRetainedOldestAgeMs = 50;
    b.ActiveSendReservations = 2; // same global gauge
    b.PreSubmitSendRollbacks = 9;
    b.SendReservationOldestAgeMs = 12;
    b.ShutdownSinkActive = 1;
    b.StopOldestAgeMs = 8;
    b.ReceiveCompletionRequired = 5;
    b.ReceiveCompletionActiveCompleted = 3;
    b.ReceiveCompletionTerminalDiscarded = 2;
    b.ReceiveCompletionZeroLength = 1;
    b.ReceiveCompletionLeaseRetry = 2;
    b.ReceiveCompletionPending = 2;
    b.ReceiveCompletionExactlyOnceViolation = 1;

    TqDarwinRelayWorkerSnapshot total{};
    TqAccumulateDarwinRelayWorkerSnapshot(total, a);
    TqAccumulateDarwinRelayWorkerSnapshot(total, b);

    Check(total.ActiveRelays == 5, "sum ActiveRelays");
    Check(total.StopRemaining == 3, "sum StopRemaining");
    Check(total.PreparedRelays == 3, "sum PreparedRelays");
    Check(total.CommitSuccessCount == 7, "sum CommitSuccessCount");
    Check(total.TerminalBeforeCommitRollbacks == 1, "sum TerminalBeforeCommitRollbacks");
    Check(total.ActivationFailureCount == 3, "sum ActivationFailureCount");
    Check(total.PrecommitBytes == 15, "sum PrecommitBytes");
    Check(total.PrecommitDepth == 3, "sum PrecommitDepth");
    Check(total.PendingReceiveActive == 5, "sum PendingReceiveActive");
    Check(total.DeferredReceiveDiscards == 7, "sum DeferredReceiveDiscards");
    Check(total.ActiveFailureAllocationFailed == 4, "sum ActiveFailureAllocationFailed");
    Check(total.WorkerExitedPurgeEvents == 8, "sum WorkerExitedPurgeEvents");
    Check(total.TerminalRetainedOwnerCount == 1, "max TerminalRetainedOwnerCount (no double count)");
    Check(total.TerminalRetainedOldestAgeMs == 50, "max TerminalRetainedOldestAgeMs");
    Check(total.ActiveSendReservations == 2, "max ActiveSendReservations");
    Check(total.PreSubmitSendRollbacks == 9, "max PreSubmitSendRollbacks");
    Check(total.SendReservationOldestAgeMs == 15, "max SendReservationOldestAgeMs");
    Check(total.ShutdownSinkActive == 1, "max ShutdownSinkActive");
    Check(total.StopOldestAgeMs == 20, "max StopOldestAgeMs");
    Check(total.ReceiveCompletionRequired == 9, "sum ReceiveCompletionRequired");
    Check(total.ReceiveCompletionActiveCompleted == 5, "sum ReceiveCompletionActiveCompleted");
    Check(total.ReceiveCompletionTerminalDiscarded == 3, "sum ReceiveCompletionTerminalDiscarded");
    Check(total.ReceiveCompletionZeroLength == 4, "sum ReceiveCompletionZeroLength");
    Check(total.ReceiveCompletionLeaseRetry == 3, "sum ReceiveCompletionLeaseRetry");
    Check(total.ReceiveCompletionPending == 3, "sum ReceiveCompletionPending");
    Check(
        total.ReceiveCompletionExactlyOnceViolation == 1,
        "sum ReceiveCompletionExactlyOnceViolation");
}

void ControlMetricsAndReleaseGateJson() {
    const uint64_t mismatchBefore =
        TqRelayControlGenerationMismatchCount().load(std::memory_order_relaxed);
    const uint64_t stopBefore =
        TqRelayControlStopSignaledCount().load(std::memory_order_relaxed);
    const uint64_t dupBefore =
        TqRelayAccountingDuplicateReleaseCount().load(std::memory_order_relaxed);

    auto control = std::make_shared<TqRelayStopControl>();
    Check(control->SignalStop(control->Generation + 1) == false, "mismatch SignalStop");
    Check(control->SignalStop(control->Generation) == true, "matching SignalStop");
    Check(control->SignalStop(control->Generation) == true, "idempotent SignalStop");
    Check(control->ReleaseActiveAccountingOnce() == true, "first accounting release");
    // First release already unregistered; second is duplicate (no prior RegisterActive).
    Check(control->ReleaseActiveAccountingOnce() == false, "duplicate accounting release");

    Check(
        TqRelayControlGenerationMismatchCount().load(std::memory_order_relaxed) ==
            mismatchBefore + 1,
        "generation mismatch counted");
    Check(
        TqRelayControlStopSignaledCount().load(std::memory_order_relaxed) == stopBefore + 1,
        "stop signaled counted once");
    Check(
        TqRelayAccountingDuplicateReleaseCount().load(std::memory_order_relaxed) ==
            dupBefore + 1,
        "duplicate accounting release counted");

    TqRelayMetricsSnapshot metrics{};
    metrics.Backend = "test";
    metrics.RelayActiveControls = 0;
    metrics.RelayControlStopSignaled = 1;
    metrics.RelayControlGenerationMismatch = 2;
    metrics.RelayAccountingDuplicateRelease = 3;
    metrics.RelayPreparedRelays = 0;
    metrics.RelayCommitSuccessCount = 4;
    metrics.RelayTerminalBeforeCommitRollbacks = 5;
    metrics.RelayActivationFailureCount = 6;
    metrics.RelayPrecommitBytes = 7;
    metrics.RelayPrecommitDepth = 8;
    metrics.RelayActiveSendReservations = 0;
    metrics.RelaySendReservationRollbacks = 9;
    metrics.RelaySendReservationOldestAgeMs = 10;
    metrics.RelayPendingReceiveActive = 0;
    metrics.RelayReceiveDiscards = 11;
    metrics.RelayActiveFailureAllocationFailed = 12;
    metrics.RelayActiveFailureBudgetExceeded = 13;
    metrics.RelayActiveFailureQueueFull = 14;
    metrics.RelayShutdownSinkActive = 0;
    metrics.RelayWorkerExitedPurgeEvents = 15;
    metrics.RelayStopRemaining = 0;
    metrics.RelayStopOldestAgeMs = 16;
    metrics.DarwinRelayReceiveCompletionRequired = 7;
    metrics.DarwinRelayReceiveCompletionActiveCompleted = 4;
    metrics.DarwinRelayReceiveCompletionTerminalDiscarded = 2;
    metrics.DarwinRelayReceiveCompletionZeroLength = 3;
    metrics.DarwinRelayReceiveCompletionLeaseRetry = 1;
    metrics.DarwinRelayReceiveCompletionPending = 0;
    metrics.DarwinRelayReceiveCompletionExactlyOnceViolation = 0;
    metrics.LinuxRelayTerminalRetainedOwnerCount = 3;
    metrics.LinuxRelayTerminalRetainedOldestAgeMs = 42;
    metrics.LinuxRelayStopRemaining = 5;
    metrics.ActiveRelays = 7;
    metrics.Errors = 3;

    const std::string json = TqRelayMetricsFieldsJson(metrics);
    CheckContains(json, "\"relay_backend\":\"test\"");
    CheckContains(json, "\"relay_active_controls\":0");
    CheckContains(json, "\"relay_control_stop_signaled\":1");
    CheckContains(json, "\"relay_control_generation_mismatch\":2");
    CheckContains(json, "\"relay_accounting_duplicate_release\":3");
    CheckContains(json, "\"relay_prepared_relays\":0");
    CheckContains(json, "\"relay_commit_success_count\":4");
    CheckContains(json, "\"relay_terminal_before_commit_rollbacks\":5");
    CheckContains(json, "\"relay_activation_failure_count\":6");
    CheckContains(json, "\"relay_precommit_bytes\":7");
    CheckContains(json, "\"relay_precommit_depth\":8");
    CheckContains(json, "\"relay_active_send_reservations\":0");
    CheckContains(json, "\"relay_send_reservation_rollbacks\":9");
    CheckContains(json, "\"relay_send_reservation_oldest_age_ms\":10");
    CheckContains(json, "\"relay_pending_receive_active\":0");
    CheckContains(json, "\"relay_receive_discards\":11");
    CheckContains(json, "\"relay_active_failure_allocation_failed\":12");
    CheckContains(json, "\"relay_active_failure_budget_exceeded\":13");
    CheckContains(json, "\"relay_active_failure_queue_full\":14");
    CheckContains(json, "\"relay_shutdown_sink_active\":0");
    CheckContains(json, "\"relay_worker_exited_purge_events\":15");
    CheckContains(json, "\"relay_stop_remaining\":0");
    CheckContains(json, "\"relay_stop_oldest_age_ms\":16");
    CheckContains(json, "\"darwin_relay_receive_completion_required\":7");
    CheckContains(json, "\"darwin_relay_receive_completion_active_completed\":4");
    CheckContains(json, "\"darwin_relay_receive_completion_terminal_discarded\":2");
    CheckContains(json, "\"darwin_relay_receive_completion_zero_length\":3");
    CheckContains(json, "\"darwin_relay_receive_completion_lease_retry\":1");
    CheckContains(json, "\"darwin_relay_receive_completion_pending\":0");
    CheckContains(json, "\"darwin_relay_receive_completion_exactly_once_violation\":0");
    CheckContains(json, "\"linux_relay_terminal_retained_owner_count\":3");
    CheckContains(json, "\"linux_relay_stop_remaining\":5");
}

void FocusedCaseEndsWithZeroReleaseGateGauges() {
    const uint32_t activeBaseline = TqGetActiveRelayCount();
    int fds[2]{TqInvalidSocket, TqInvalidSocket};
    Check(socketpair(AF_UNIX, SOCK_STREAM, 0, fds) == 0, "socketpair succeeds");

    TqDarwinRelayWorker worker(TqDarwinRelayWorkerConfig{});
    Check(worker.Start(), "worker.Start()");

    TqRelayHandle handle{};
    handle.Control = std::make_shared<TqRelayStopControl>();
    handle.ControlGeneration = handle.Control->Generation;

    TqDarwinRelayRegistration registration{};
    registration.TcpFd = fds[0];
    registration.Stream = reinterpret_cast<MsQuicStream*>(static_cast<uintptr_t>(1));
    registration.Control = handle.Control;
    registration.ControlGeneration = handle.Control->Generation;

    const TqDarwinRelayRegistrationResult result = worker.RegisterRelayWithId(registration);
    Check(result.Ok, "RegisterRelayWithId ok");
    handle.Backend = TqRelayBackendType::DarwinWorker;
    handle.DarwinWorker = &worker;
    handle.DarwinRelayId = result.RelayId;

    worker.UnregisterRelay(result.RelayId);
    worker.Stop();
    CloseSocketPairAfterRelayOwned(registration.TcpFd, fds);

    Check(TqGetActiveRelayCount() == activeBaseline, "active accounting restored");
    const auto snapshot = worker.Snapshot();
    Check(snapshot.ActiveRelays == 0, "ActiveRelays == 0");
    Check(snapshot.PreparedRelays == 0, "PreparedRelays == 0");
    Check(snapshot.ActiveSendReservations == 0, "ActiveSendReservations == 0");
    Check(snapshot.PendingReceiveActive == 0, "PendingReceiveActive == 0");
    Check(snapshot.ShutdownSinkActive == 0, "ShutdownSinkActive == 0");
    Check(snapshot.StopRemaining == 0, "StopRemaining == 0");
}

} // namespace

int main() {
    TqDarwinRelayRuntime::Instance().Stop();

    TqRelayMetricsSnapshot runtimeSnapshot = TqSnapshotRelayMetrics();
    Check(std::string(runtimeSnapshot.Backend) == "kqueue", "runtimeSnapshot.Backend == kqueue");
    Check(runtimeSnapshot.SnapshotComplete, "stopped metrics complete");

    ControlMetricsAndReleaseGateJson();
    MultiWorkerAggregationUsesMaxForGlobalGauges();
    EventizedSnapshotCountsActiveRelay();
    FocusedCaseEndsWithZeroReleaseGateGauges();
    SnapshotWorkersRunningAndStopped();
    WorkersJsonSingleSamplesEachWorkerOnce();
    MetricsSnapshotCompleteCapacityAndGateStats();
    IncompleteIdentityDoesNotVacuousCompleteAggregate();
    AssertReleaseGateGaugesZero("after focused metrics cases");
    return 0;
}

#else
int main() { return 0; }
#endif
