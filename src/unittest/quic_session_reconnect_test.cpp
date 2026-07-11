#include "quic_session.h"
#include "control_protocol.h"
#include "quic_address.h"

#include <atomic>
#include <chrono>
#include <cstring>
#include <functional>
#include <thread>

static int TestServerNewConnectionDisable1RttStatusClassification() {
    if (std::strcmp(TqClassifyServerNewConnectionDisable1RttStatusForTest(QUIC_STATUS_SUCCESS), "disabled") != 0) return 601;
    if (std::strcmp(TqClassifyServerNewConnectionDisable1RttStatusForTest(QUIC_STATUS_INVALID_STATE), "enabled") != 0) return 602;
    if (std::strcmp(TqClassifyServerNewConnectionDisable1RttStatusForTest(QUIC_STATUS_OUT_OF_MEMORY), "error") != 0) return 603;
    return 0;
}

static int TestTerminalEscalationRejectsOldGeneration() {
    QuicClientSession session;
    session.MarkReconnectStartedForTest(1);
    session.MarkSlotConnectedForTest(
        0, reinterpret_cast<MsQuicConnection*>(static_cast<uintptr_t>(0x6101)));
    const auto before = session.SnapshotConnections().front();
    auto escalation = session.MakeTerminalEscalation(
        before.SlotIndex, before.NumericConnectionId, before.Generation);
    std::string err;
    if (!session.ReconnectConnection(before.ConnectionId, err)) return 6101;
    escalation->RequestConnectionShutdown(
        before.NumericConnectionId, 17, QUIC_STATUS_INVALID_STATE, 91);
    if (session.ConnectionShutdownCallsForTest(0) != 0) return 6102;
    if (session.TerminalEscalationGenerationMismatchForTest() != 1) return 6103;
    session.Stop();
    return 0;
}

static int TestTerminalEscalationIsExactlyOnce() {
    QuicClientSession session;
    session.MarkReconnectStartedForTest(1);
    session.MarkSlotConnectedForTest(
        0, reinterpret_cast<MsQuicConnection*>(static_cast<uintptr_t>(0x6201)));
    const auto before = session.SnapshotConnections().front();
    auto escalation = session.MakeTerminalEscalation(
        before.SlotIndex, before.NumericConnectionId, before.Generation);
    escalation->RequestConnectionShutdown(
        before.NumericConnectionId, 18, QUIC_STATUS_INVALID_STATE, 92);
    escalation->RequestConnectionShutdown(
        before.NumericConnectionId, 18, QUIC_STATUS_INVALID_STATE, 92);
    if (session.ConnectionShutdownCallsForTest(0) != 1) return 6201;
    if (session.TerminalEscalationDuplicateForTest() != 1) return 6202;
    session.Stop();
    return 0;
}

static int TestTerminalEscalationSuppressesClosingConnection() {
    QuicClientSession session;
    session.MarkReconnectStartedForTest(1);
    session.MarkSlotConnectedForTest(
        0, reinterpret_cast<MsQuicConnection*>(static_cast<uintptr_t>(0x6301)));
    const auto before = session.SnapshotConnections().front();
    auto escalation = session.MakeTerminalEscalation(
        before.SlotIndex, before.NumericConnectionId, before.Generation);
    session.MarkSlotClosingForTest(0);
    escalation->RequestConnectionShutdown(
        before.NumericConnectionId, 19, QUIC_STATUS_INVALID_STATE, 93);
    if (session.ConnectionShutdownCallsForTest(0) != 0) return 6301;
    if (session.TerminalEscalationClosingSuppressedForTest() != 1) return 6302;
    session.Stop();
    return 0;
}

static int TestTerminalEscalationPinsConnectionAcrossSlotErase() {
    QuicClientSession session;
    session.MarkReconnectStartedForTest(1);
    std::atomic<bool> destroyed{false};
    auto owner = std::shared_ptr<MsQuicConnection>(
        reinterpret_cast<MsQuicConnection*>(static_cast<uintptr_t>(0x6351)),
        [&](MsQuicConnection*) { destroyed.store(true, std::memory_order_release); });
    session.MarkSlotConnectedForTest(0, owner.get());
    const auto before = session.SnapshotConnections().front();
    session.MarkSlotConnectedForTest(0, owner);
    std::mutex lock;
    std::condition_variable cv;
    bool pinned = false;
    bool release = false;
    QuicClientSession::ReconnectTestHooks hooks;
    hooks.BeforeTerminalConnectionShutdown = [&] {
        std::unique_lock<std::mutex> guard(lock);
        pinned = true;
        cv.notify_all();
        cv.wait(guard, [&] { return release; });
    };
    session.SetReconnectTestHooks(std::move(hooks));
    auto escalation = session.MakeTerminalEscalation(
        before.SlotIndex, before.NumericConnectionId, before.Generation);
    owner.reset();
    std::thread worker([&] {
        escalation->RequestConnectionShutdown(
            before.NumericConnectionId, 24, QUIC_STATUS_INVALID_STATE, 98);
    });
    {
        std::unique_lock<std::mutex> guard(lock);
        cv.wait(guard, [&] { return pinned; });
    }
    session.ClearSlotConnectionForTest(0);
    if (destroyed.load(std::memory_order_acquire)) return 6351;
    {
        std::lock_guard<std::mutex> guard(lock);
        release = true;
    }
    cv.notify_all();
    worker.join();
    if (!destroyed.load(std::memory_order_acquire)) return 6352;
    return 0;
}

static int TestQueuedTerminalOperationRejectsReconnectedSlot() {
    QuicClientSession session;
    session.MarkReconnectStartedForTest(1);
    auto* connection = reinterpret_cast<MsQuicConnection*>(static_cast<uintptr_t>(0x6361));
    session.MarkSlotConnectedForTest(0, connection);
    const auto before = session.SnapshotConnections().front();
    std::function<void()> queued;
    session.SetDelayedTaskScheduler([&](std::chrono::milliseconds delay, std::function<void()> task) {
        if (delay != std::chrono::milliseconds(0)) return false;
        queued = std::move(task);
        return true;
    });
    auto escalation = session.MakeTerminalEscalation(
        before.SlotIndex, before.NumericConnectionId, before.Generation);
    escalation->RequestConnectionShutdown(
        before.NumericConnectionId, 27, QUIC_STATUS_INVALID_STATE, 101);
    if (!queued) return 6361;
    std::string err;
    if (!session.ReconnectConnection(before.ConnectionId, err)) return 6362;
    queued();
    if (session.ConnectionShutdownCallsForTest(0) != 0) return 6363;
    session.Stop();
    return 0;
}

static int TestServerTerminalEscalationRegistrySafety() {
    TqResetServerTerminalEscalationCountersForTest();
    const HQUIC firstHandle = reinterpret_cast<HQUIC>(static_cast<uintptr_t>(0x6401));
    const uint32_t firstId = TqRegisterServerConnectionForTest(firstHandle);
    TqServerConnectionSnapshot first{};
    if (!TqGetServerConnectionSnapshot("srv-" + std::to_string(firstId), first)) return 6401;
    auto firstEscalation = TqMakeServerTerminalEscalation(
        {first.NumericConnectionId, first.Generation});
    if (!firstEscalation) return 6402;
    firstEscalation->RequestConnectionShutdown(
        first.NumericConnectionId, 20, QUIC_STATUS_INVALID_STATE, 94);
    firstEscalation->RequestConnectionShutdown(
        first.NumericConnectionId, 20, QUIC_STATUS_INVALID_STATE, 94);
    if (TqServerConnectionShutdownCallsForTest() != 1) return 6403;
    if (TqServerTerminalDuplicateForTest() != 1) return 6404;

    const HQUIC closingHandle = reinterpret_cast<HQUIC>(static_cast<uintptr_t>(0x6402));
    const uint32_t closingId = TqRegisterServerConnectionForTest(closingHandle);
    TqServerConnectionSnapshot closing{};
    if (!TqGetServerConnectionSnapshot("srv-" + std::to_string(closingId), closing)) return 6405;
    auto closingEscalation = TqMakeServerTerminalEscalation(
        {closing.NumericConnectionId, closing.Generation});
    TqMarkServerConnectionClosingForTest(closingHandle);
    closingEscalation->RequestConnectionShutdown(
        closing.NumericConnectionId, 21, QUIC_STATUS_INVALID_STATE, 95);
    if (TqServerConnectionShutdownCallsForTest() != 1) return 6406;
    if (TqServerTerminalClosingSuppressedForTest() != 1) return 6407;

    TqUnregisterServerConnectionForTest(firstHandle);
    const uint32_t replacementId = TqRegisterServerConnectionForTest(firstHandle);
    TqServerConnectionSnapshot replacement{};
    if (!TqGetServerConnectionSnapshot(
            "srv-" + std::to_string(replacementId), replacement)) return 6408;
    firstEscalation->RequestConnectionShutdown(
        first.NumericConnectionId, 22, QUIC_STATUS_INVALID_STATE, 96);
    if (TqServerConnectionShutdownCallsForTest() != 1) return 6409;
    auto replacementEscalation = TqMakeServerTerminalEscalation(
        {replacement.NumericConnectionId, replacement.Generation});
    replacementEscalation->RequestConnectionShutdown(
        replacement.NumericConnectionId, 23, QUIC_STATUS_INVALID_STATE, 97);
    if (TqServerConnectionShutdownCallsForTest() != 2) return 6410;

    TqUnregisterServerConnectionForTest(firstHandle);
    TqUnregisterServerConnectionForTest(closingHandle);
    return 0;
}

static int TestServerTerminalEscalationPinsOwnerAcrossRecordErase() {
    TqResetServerTerminalEscalationCountersForTest();
    const HQUIC handle = reinterpret_cast<HQUIC>(static_cast<uintptr_t>(0x6451));
    std::atomic<bool> destroyed{false};
    auto owner = std::shared_ptr<MsQuicConnection>(
        reinterpret_cast<MsQuicConnection*>(static_cast<uintptr_t>(0x6452)),
        [&](MsQuicConnection*) { destroyed.store(true, std::memory_order_release); });
    const uint32_t id = TqRegisterServerConnectionOwnerForTest(handle, owner);
    TqServerConnectionSnapshot snapshot{};
    if (!TqGetServerConnectionSnapshot("srv-" + std::to_string(id), snapshot)) return 6451;
    auto escalation = TqMakeServerTerminalEscalation(
        {snapshot.NumericConnectionId, snapshot.Generation});
    std::mutex lock;
    std::condition_variable cv;
    bool pinned = false;
    bool release = false;
    TqSetBeforeServerTerminalShutdownForTest([&] {
        std::unique_lock<std::mutex> guard(lock);
        pinned = true;
        cv.notify_all();
        cv.wait(guard, [&] { return release; });
    });
    owner.reset();
    std::thread worker([&] {
        escalation->RequestConnectionShutdown(
            snapshot.NumericConnectionId, 26, QUIC_STATUS_INVALID_STATE, 100);
    });
    {
        std::unique_lock<std::mutex> guard(lock);
        cv.wait(guard, [&] { return pinned; });
    }
    TqUnregisterServerConnectionForTest(handle);
    if (destroyed.load(std::memory_order_acquire)) return 6452;
    {
        std::lock_guard<std::mutex> guard(lock);
        release = true;
    }
    cv.notify_all();
    worker.join();
    TqSetBeforeServerTerminalShutdownForTest({});
    if (!destroyed.load(std::memory_order_acquire)) return 6453;
    return 0;
}

static int TestServerDeferredCleanupWaitsForOuterTrampolineAndDrainsFailures() {
    std::mutex lock;
    std::condition_variable cv;
    bool outerReturned = false;
    bool waitEntered = false;
    std::atomic<bool> destroyed{false};
    auto owner = std::shared_ptr<MsQuicConnection>(
        reinterpret_cast<MsQuicConnection*>(static_cast<uintptr_t>(0x6461)),
        [&](MsQuicConnection*) { destroyed.store(true, std::memory_order_release); });
    if (!TqDeferServerConnectionOwnerForTest(owner, [&] {
            std::unique_lock<std::mutex> guard(lock);
            waitEntered = true;
            cv.notify_all();
            cv.wait(guard, [&] { return outerReturned; });
        })) return 6461;
    owner.reset();
    {
        std::unique_lock<std::mutex> guard(lock);
        cv.wait(guard, [&] { return waitEntered; });
    }
    if (destroyed.load(std::memory_order_acquire)) return 6462;
    {
        std::lock_guard<std::mutex> guard(lock);
        outerReturned = true;
    }
    cv.notify_all();
    TqDrainServerConnectionCleanupForTest();
    if (!destroyed.load(std::memory_order_acquire)) return 6463;

    std::atomic<uint32_t> failedDestroyed{0};
    TqFailNextServerCleanupEnqueueForTest();
    for (uintptr_t i = 0; i < 256; ++i) {
        auto failedOwner = std::shared_ptr<MsQuicConnection>(
            reinterpret_cast<MsQuicConnection*>(0x6500 + i),
            [&](MsQuicConnection*) { failedDestroyed.fetch_add(1); });
        if (!TqDeferServerConnectionOwnerForTest(failedOwner, [] {})) return 6464;
    }
    if (failedDestroyed.load() != 0) return 6465;
    TqDrainServerConnectionCleanupForTest();
    if (failedDestroyed.load() != 256) return 6466;
    return 0;
}

static int TestServerCleanupSessionWatermarkAllowsLateEnqueue() {
    auto tracker = TqMakeServerCleanupTrackerForTest();
    std::mutex lock;
    std::condition_variable cv;
    bool firstEntered = false;
    bool releaseFirst = false;
    bool releaseLate = false;
    auto makeOwner = [](uintptr_t value) {
        return std::shared_ptr<MsQuicConnection>(
            reinterpret_cast<MsQuicConnection*>(value), [](MsQuicConnection*) {});
    };
    if (!TqDeferServerConnectionOwnerForTrackerForTest(
            tracker, makeOwner(0x6471), [&] {
                std::unique_lock<std::mutex> guard(lock);
                firstEntered = true;
                cv.notify_all();
                cv.wait(guard, [&] { return releaseFirst; });
            })) return 6471;
    {
        std::unique_lock<std::mutex> guard(lock);
        cv.wait(guard, [&] { return firstEntered; });
    }
    const uint64_t watermark = TqServerConnectionCleanupWatermarkForTest(tracker);
    bool drained = false;
    std::thread drain([&] {
        TqDrainServerConnectionCleanupThroughForTest(tracker, watermark);
        std::lock_guard<std::mutex> guard(lock);
        drained = true;
        cv.notify_all();
    });
    if (!TqDeferServerConnectionOwnerForTrackerForTest(
            tracker, makeOwner(0x6472), [&] {
                std::unique_lock<std::mutex> guard(lock);
                cv.wait(guard, [&] { return releaseLate; });
            })) return 6472;
    {
        std::lock_guard<std::mutex> guard(lock);
        releaseFirst = true;
    }
    cv.notify_all();
    bool drainedBeforeLate = false;
    {
        std::unique_lock<std::mutex> guard(lock);
        drainedBeforeLate = cv.wait_for(
            guard, std::chrono::seconds(2), [&] { return drained; });
    }
    {
        std::lock_guard<std::mutex> guard(lock);
        releaseLate = true;
    }
    cv.notify_all();
    drain.join();
    if (!drainedBeforeLate) return 6473;
    TqDrainServerConnectionCleanupTrackerForTest(tracker);
    return 0;
}

static int TestServerCleanupConcurrentSessionDrains() {
    auto firstTracker = TqMakeServerCleanupTrackerForTest();
    auto secondTracker = TqMakeServerCleanupTrackerForTest();
    std::mutex lock;
    std::condition_variable cv;
    bool release = false;
    auto wait = [&] {
        std::unique_lock<std::mutex> guard(lock);
        cv.wait(guard, [&] { return release; });
    };
    auto owner = [](uintptr_t value) {
        return std::shared_ptr<MsQuicConnection>(
            reinterpret_cast<MsQuicConnection*>(value), [](MsQuicConnection*) {});
    };
    if (!TqDeferServerConnectionOwnerForTrackerForTest(
            firstTracker, owner(0x6481), wait)) return 6481;
    if (!TqDeferServerConnectionOwnerForTrackerForTest(
            secondTracker, owner(0x6482), wait)) return 6482;
    std::atomic<uint32_t> drained{0};
    std::thread first([&] {
        TqDrainServerConnectionCleanupTrackerForTest(firstTracker);
        drained.fetch_add(1);
    });
    std::thread second([&] {
        TqDrainServerConnectionCleanupTrackerForTest(secondTracker);
        drained.fetch_add(1);
    });
    if (drained.load() != 0) return 6483;
    {
        std::lock_guard<std::mutex> guard(lock);
        release = true;
    }
    cv.notify_all();
    first.join();
    second.join();
    return drained.load() == 2 ? 0 : 6484;
}

static int TestServerCleanupDuplicateEnqueueIsSuppressed() {
    std::mutex lock;
    std::condition_variable cv;
    bool release = false;
    const uint64_t before = TqServerConnectionCleanupDuplicateCountForTest();
    auto owner = std::shared_ptr<MsQuicConnection>(
        reinterpret_cast<MsQuicConnection*>(static_cast<uintptr_t>(0x6491)),
        [](MsQuicConnection*) {});
    if (!TqDuplicateServerConnectionCleanupEnqueueForTest(owner, [&] {
            std::unique_lock<std::mutex> guard(lock);
            cv.wait(guard, [&] { return release; });
        })) return 6491;
    if (TqServerConnectionCleanupDuplicateCountForTest() != before + 1) return 6492;
    {
        std::lock_guard<std::mutex> guard(lock);
        release = true;
    }
    cv.notify_all();
    TqDrainServerConnectionCleanupForTest();
    return 0;
}

static int TestServerCleanupFinalStopIsSerializedAndClosesAdmission() {
    std::thread first([] { TqFinalStopServerConnectionCleanupForTest(); });
    std::thread second([] { TqFinalStopServerConnectionCleanupForTest(); });
    first.join();
    second.join();
    TqFinalStopServerConnectionCleanupForTest();
    auto owner = std::shared_ptr<MsQuicConnection>(
        reinterpret_cast<MsQuicConnection*>(static_cast<uintptr_t>(0x6492)),
        [](MsQuicConnection*) {});
    if (TqDeferServerConnectionOwnerForTest(owner, [] {})) return 6493;
    return 0;
}

static int TestFixedDelayRetrySchedulesAndRestartsSlot() {
    QuicClientSession session;
    std::atomic<int> scheduled{0};
    std::atomic<int> startCalls{0};
    std::function<void()> retryTask;

    session.MarkReconnectStartedForTest(1);
    session.SetReconnectTestHooks(QuicClientSession::ReconnectTestHooks{
        [&](size_t index) {
            if (index != 0) {
                return false;
            }
            startCalls.fetch_add(1, std::memory_order_relaxed);
            return true;
        }});
    session.SetDelayedTaskScheduler(
        [&](std::chrono::milliseconds delay, std::function<void()> task) {
            if (delay != std::chrono::milliseconds(3000)) {
                return false;
            }
            scheduled.fetch_add(1, std::memory_order_relaxed);
            retryTask = std::move(task);
            return true;
        });

    session.ScheduleStartRetryForTest(0);
    if (scheduled.load(std::memory_order_relaxed) != 1 || !retryTask) {
        return 10;
    }
    retryTask();
    if (startCalls.load(std::memory_order_relaxed) != 1) {
        return 11;
    }
    session.Stop();
    return 0;
}

static int TestDelayedRetryDropsAfterStop() {
    QuicClientSession session;
    std::atomic<int> startCalls{0};
    std::function<void()> retryTask;

    session.MarkReconnectStartedForTest(1);
    session.SetReconnectTestHooks(QuicClientSession::ReconnectTestHooks{
        [&](size_t) {
            startCalls.fetch_add(1, std::memory_order_relaxed);
            return true;
        }});
    session.SetDelayedTaskScheduler(
        [&](std::chrono::milliseconds delay, std::function<void()> task) {
            if (delay != std::chrono::milliseconds(3000)) {
                return false;
            }
            retryTask = std::move(task);
            return true;
        });

    session.ScheduleStartRetryForTest(0);
    if (!retryTask) {
        return 20;
    }
    session.Stop();
    retryTask();
    return startCalls.load(std::memory_order_relaxed) == 0 ? 0 : 21;
}

static int TestFixedDelayRetryCoalescesDuplicates() {
    QuicClientSession session;
    std::atomic<int> scheduled{0};
    std::atomic<int> startCalls{0};
    std::function<void()> retryTask;

    session.MarkReconnectStartedForTest(1);
    session.SetReconnectTestHooks(QuicClientSession::ReconnectTestHooks{
        [&](size_t) {
            startCalls.fetch_add(1, std::memory_order_relaxed);
            return true;
        }});
    session.SetDelayedTaskScheduler(
        [&](std::chrono::milliseconds, std::function<void()> task) {
            scheduled.fetch_add(1, std::memory_order_relaxed);
            retryTask = std::move(task);
            return true;
        });

    session.ScheduleStartRetryForTest(0);
    session.ScheduleStartRetryForTest(0);
    if (scheduled.load(std::memory_order_relaxed) != 1 || !retryTask) {
        return 30;
    }
    retryTask();
    if (startCalls.load(std::memory_order_relaxed) != 1) {
        return 31;
    }
    session.Stop();
    return 0;
}

static int TestRejectedSchedulerAllowsLaterRetry() {
    QuicClientSession session;
    std::atomic<int> rejectedCalls{0};
    std::atomic<int> acceptedCalls{0};
    std::function<void()> retryTask;

    session.MarkReconnectStartedForTest(1);
    session.SetDelayedTaskScheduler(
        [&](std::chrono::milliseconds, std::function<void()>) {
            rejectedCalls.fetch_add(1, std::memory_order_relaxed);
            return false;
        });
    session.ScheduleStartRetryForTest(0);
    if (rejectedCalls.load(std::memory_order_relaxed) != 1) {
        return 40;
    }

    session.SetDelayedTaskScheduler(
        [&](std::chrono::milliseconds, std::function<void()> task) {
            acceptedCalls.fetch_add(1, std::memory_order_relaxed);
            retryTask = std::move(task);
            return true;
        });
    session.ScheduleStartRetryForTest(0);
    if (acceptedCalls.load(std::memory_order_relaxed) != 1 || !retryTask) {
        return 41;
    }
    session.Stop();
    return 0;
}

static int TestConnectionStartPendingIsAccepted() {
    if (!QuicClientSession::ConnectionStartAcceptedForTest(QUIC_STATUS_SUCCESS)) {
        return 50;
    }
    if (!QuicClientSession::ConnectionStartAcceptedForTest(QUIC_STATUS_PENDING)) {
        return 51;
    }
    if (QuicClientSession::ConnectionStartAcceptedForTest(QUIC_STATUS_ABORTED)) {
        return 52;
    }
    return 0;
}

static int TestShutdownCompleteSchedulesSlotRestart() {
    QuicClientSession session;
    std::atomic<int> scheduled{0};
    std::atomic<int> startCalls{0};
    std::function<void()> retryTask;

    session.MarkReconnectStartedForTest(1);
    session.SetReconnectTestHooks(QuicClientSession::ReconnectTestHooks{
        [&](size_t index) {
            if (index != 0) {
                return false;
            }
            startCalls.fetch_add(1, std::memory_order_relaxed);
            return true;
        }});
    session.SetDelayedTaskScheduler(
        [&](std::chrono::milliseconds delay, std::function<void()> task) {
            if (delay != std::chrono::milliseconds(3000)) {
                return false;
            }
            scheduled.fetch_add(1, std::memory_order_relaxed);
            retryTask = std::move(task);
            return true;
        });

    session.RestartSlotAfterShutdownCompleteForTest(0, 0);
    if (scheduled.load(std::memory_order_relaxed) != 1 || !retryTask) {
        return 75;
    }
    if (startCalls.load(std::memory_order_relaxed) != 0) {
        return 76;
    }
    retryTask();
    if (startCalls.load(std::memory_order_relaxed) != 1) {
        return 77;
    }
    session.Stop();
    return 0;
}

static int TestConnectionSnapshotAndSlotControls() {
    QuicClientSession session;
    std::atomic<int> startCalls{0};
    session.MarkReconnectStartedForTest(2);
    session.SetReconnectTestHooks(QuicClientSession::ReconnectTestHooks{
        [&](size_t) {
            startCalls.fetch_add(1, std::memory_order_relaxed);
            return true;
        }});

    auto snapshots = session.SnapshotConnections();
    if (snapshots.size() != 2) return 80;
    if (snapshots[0].ConnectionId != "conn-0") return 81;
    if (snapshots[1].ConnectionId != "conn-1") return 82;
    if (snapshots[0].SlotIndex != 0 || snapshots[1].SlotIndex != 1) return 83;
    if (snapshots[0].Generation != 0) return 84;
    if (snapshots[0].State != "connecting") return 85;

    std::string err;
    if (!session.SetDesiredConnectionCount(3, err)) return 86;
    if (startCalls.load(std::memory_order_relaxed) != 1) return 96;
    snapshots = session.SnapshotConnections();
    if (snapshots.size() != 3) return 87;
    if (snapshots[2].ConnectionId != "conn-2") return 88;

    if (session.StopHighestConnection("conn-0", err)) return 89;
    if (err.find("highest") == std::string::npos) return 90;

    if (!session.ReconnectConnection("conn-1", err)) return 91;
    if (startCalls.load(std::memory_order_relaxed) != 2) return 97;
    snapshots = session.SnapshotConnections();
    if (snapshots[1].Generation != 1) return 92;

    if (!session.StopHighestConnection("conn-2", err)) return 93;
    snapshots = session.SnapshotConnections();
    if (snapshots.size() != 2) return 94;

    if (!session.AbortConnectionTunnels("conn-1", err)) return 95;
    session.Stop();
    return 0;
}

static int TestPickConnectionWithIdReturnsSlotId() {
    QuicClientSession session;
    session.MarkReconnectStartedForTest(2);

    auto* first = reinterpret_cast<MsQuicConnection*>(static_cast<uintptr_t>(0x1000));
    auto* second = reinterpret_cast<MsQuicConnection*>(static_cast<uintptr_t>(0x1001));
    session.MarkSlotConnectedForTest(0, first);
    session.MarkSlotConnectedForTest(1, second);

    const auto picked = session.PickConnectionWithId();
    if (picked.Connection != first && picked.Connection != second) return 130;
    if (picked.Connection == first && picked.ConnectionId != "conn-0") return 131;
    if (picked.Connection == second && picked.ConnectionId != "conn-1") return 132;
    if (picked.NumericConnectionId == 0 || picked.TerminalEscalation == nullptr) return 133;
    TqTerminalConnectionKey legacyKey{};
    std::shared_ptr<TqTerminalEscalation> legacyEscalation;
    if (TqLookupClientTerminalConnection(
            picked.Connection, legacyKey, legacyEscalation)) return 136;
    std::string err;
    if (!session.ReconnectConnection(picked.ConnectionId, err)) return 134;
    if (TqLookupClientTerminalConnection(
            picked.Connection, legacyKey, legacyEscalation)) return 137;
    picked.TerminalEscalation->RequestConnectionShutdown(
        picked.NumericConnectionId, 25, QUIC_STATUS_INVALID_STATE, 99);
    if (session.TerminalEscalationGenerationMismatchForTest() != 1) return 135;

    session.Stop();
    return 0;
}

static int TestPickConnectionWithIdReturnsEmptyWhenDisconnected() {
    QuicClientSession session;
    session.MarkReconnectStartedForTest(1);

    const auto picked = session.PickConnectionWithId();
    if (picked.Connection != nullptr) return 140;
    if (!picked.ConnectionId.empty()) return 141;

    session.Stop();
    return 0;
}

static int TestQuicPathSlotExpansion() {
    TqConfig cfg;
    cfg.QuicPaths.push_back(TqQuicPathConfig{"cmcc", "10.10.1.2", "36.1.1.10:443", 2});
    cfg.QuicPaths.push_back(TqQuicPathConfig{"ctcc", "10.20.1.2", "59.1.1.10:443", 1});

    std::vector<TqClientSlotPath> slots;
    std::string err;
    if (!TqBuildClientSlotPaths(cfg, slots, err)) return 100;
    if (slots.size() != 3) return 101;
    if (slots[0].Name != "cmcc" ||
        slots[0].LocalAddress != "10.10.1.2" ||
        slots[0].PeerHost != "36.1.1.10" ||
        slots[0].PeerPort != 443 ||
        slots[0].PeerText != "36.1.1.10:443") {
        return 102;
    }
    if (slots[1].Name != "cmcc" ||
        slots[1].LocalAddress != "10.10.1.2" ||
        slots[1].PeerHost != "36.1.1.10" ||
        slots[1].PeerPort != 443 ||
        slots[1].PeerText != "36.1.1.10:443") {
        return 103;
    }
    if (slots[2].Name != "ctcc" ||
        slots[2].LocalAddress != "10.20.1.2" ||
        slots[2].PeerHost != "59.1.1.10" ||
        slots[2].PeerPort != 443 ||
        slots[2].PeerText != "59.1.1.10:443") {
        return 104;
    }
    return 0;
}

static int TestQuicPeerListSlotExpansionRoundRobin() {
    TqConfig cfg;
    cfg.QuicPeer = "36.1.1.10:443,59.1.1.10:8443";
    cfg.QuicConnections = 5;

    std::vector<TqClientSlotPath> slots;
    std::string err;
    if (!TqBuildClientSlotPaths(cfg, slots, err)) return 110;
    if (slots.size() != 5) return 111;

    const char* expectedHosts[] = {
        "36.1.1.10",
        "59.1.1.10",
        "36.1.1.10",
        "59.1.1.10",
        "36.1.1.10",
    };
    const uint16_t expectedPorts[] = {443, 8443, 443, 8443, 443};
    const char* expectedTexts[] = {
        "36.1.1.10:443",
        "59.1.1.10:8443",
        "36.1.1.10:443",
        "59.1.1.10:8443",
        "36.1.1.10:443",
    };
    for (size_t i = 0; i < slots.size(); ++i) {
        if (slots[i].Name != "default") return 112;
        if (!slots[i].LocalAddress.empty()) return 113;
        if (slots[i].PeerHost != expectedHosts[i]) return 114;
        if (slots[i].PeerPort != expectedPorts[i]) return 115;
        if (slots[i].PeerText != expectedTexts[i]) return 116;
    }
    return 0;
}

static int TestQuicPathModeRejectsSlotTopologyMutation() {
    TqConfig cfg;
    cfg.QuicPaths.push_back(TqQuicPathConfig{"cmcc", "10.10.1.2", "36.1.1.10:443", 2});
    cfg.QuicPaths.push_back(TqQuicPathConfig{"ctcc", "10.20.1.2", "59.1.1.10:443", 1});

    QuicClientSession session;
    session.MarkReconnectStartedForTest(3, cfg);

    std::string err;
    if (session.SetDesiredConnectionCount(4, err)) return 120;
    if (err.find("path-mode uses fixed connection slots") == std::string::npos) return 121;

    err.clear();
    if (session.StopHighestConnection("conn-2", err)) return 122;
    if (err.find("path-mode uses fixed connection slots") == std::string::npos) return 123;

    session.Stop();
    return 0;
}

static int TestStartSlotUsesConfiguredPathSlots() {
    TqConfig cfg;
    cfg.QuicPaths.push_back(TqQuicPathConfig{"cmcc", "10.10.1.2", "36.1.1.10:443", 2});
    cfg.QuicPaths.push_back(TqQuicPathConfig{"ctcc", "10.20.1.2", "59.1.1.10:443", 1});

    QuicClientSession session;
    session.MarkReconnectStartedForTest(3, cfg);

    std::vector<TqClientSlotPath> observed;
    QuicClientSession::ReconnectTestHooks hooks;
    hooks.StartSlotPathObserver = [&](size_t index, const TqClientSlotPath& path) {
        if (index != observed.size()) {
            observed.push_back(TqClientSlotPath{"bad-index", "", "", 0, ""});
            return;
        }
        observed.push_back(path);
    };
    hooks.StartSlotOverride = [](size_t) {
        return true;
    };
    session.SetReconnectTestHooks(std::move(hooks));

    std::string err;
    if (!session.ReconnectConnection("conn-0", err)) return 130;
    if (!session.ReconnectConnection("conn-1", err)) return 131;
    if (!session.ReconnectConnection("conn-2", err)) return 132;
    if (observed.size() != 3) return 133;
    if (observed[0].Name != "cmcc" ||
        observed[0].LocalAddress != "10.10.1.2" ||
        observed[0].PeerHost != "36.1.1.10" ||
        observed[0].PeerPort != 443) {
        return 134;
    }
    if (observed[1].Name != "cmcc" ||
        observed[1].LocalAddress != "10.10.1.2" ||
        observed[1].PeerHost != "36.1.1.10" ||
        observed[1].PeerPort != 443) {
        return 135;
    }
    if (observed[2].Name != "ctcc" ||
        observed[2].LocalAddress != "10.20.1.2" ||
        observed[2].PeerHost != "59.1.1.10" ||
        observed[2].PeerPort != 443) {
        return 136;
    }
    session.Stop();
    return 0;
}

static int TestStartSlotUsesPeerListRoundRobinPaths() {
    TqConfig cfg;
    cfg.QuicPeer = "36.1.1.10:443,59.1.1.10:8443";
    cfg.QuicConnections = 2;

    QuicClientSession session;
    session.MarkReconnectStartedForTest(2, cfg);

    std::vector<TqClientSlotPath> observed;
    QuicClientSession::ReconnectTestHooks hooks;
    hooks.StartSlotPathObserver = [&](size_t, const TqClientSlotPath& path) {
        observed.push_back(path);
    };
    hooks.StartSlotOverride = [](size_t) {
        return true;
    };
    session.SetReconnectTestHooks(std::move(hooks));

    std::string err;
    if (!session.ReconnectConnection("conn-0", err)) return 140;
    if (!session.ReconnectConnection("conn-1", err)) return 141;
    if (observed.size() != 2) return 142;
    if (observed[0].PeerHost != "36.1.1.10" ||
        observed[0].PeerPort != 443 ||
        !observed[0].LocalAddress.empty()) {
        return 143;
    }
    if (observed[1].PeerHost != "59.1.1.10" ||
        observed[1].PeerPort != 8443 ||
        !observed[1].LocalAddress.empty()) {
        return 144;
    }
    session.Stop();
    return 0;
}

static int TestStartSlotDoesNotOverwriteChangedGeneration() {
    QuicClientSession session;
    TqConfig cfg{};
    cfg.Mode = TqMode::Client;
    cfg.QuicPeer = "127.0.0.1:4433";
    cfg.QuicConnections = 1;
    cfg.QuicCa = "cert/ca.crt";
    cfg.QuicDisable1RttEncryption = false;

    std::atomic<int> beforePublishCalls{0};
    std::atomic<bool> reconnectDuringPublish{false};
    std::atomic<bool> deletedStaleGeneration{false};
    if (!session.Start(cfg)) return 1801;

    QuicClientSession::ReconnectTestHooks hooks;
    hooks.BeforePublishSlot = [&](size_t index) {
        if (index != 0) {
            return;
        }
        beforePublishCalls.fetch_add(1, std::memory_order_acq_rel);
        if (!reconnectDuringPublish.exchange(false, std::memory_order_acq_rel)) {
            return;
        }
        std::string err;
        (void)session.ReconnectConnection("conn-0", err);
    };
    hooks.ContextDeleted = [&](size_t index, uint64_t generation) {
        if (index == 0 && generation == 1) {
            deletedStaleGeneration.store(true, std::memory_order_release);
        }
    };
    session.SetReconnectTestHooks(std::move(hooks));

    reconnectDuringPublish.store(true, std::memory_order_release);
    std::string err;
    if (!session.ReconnectConnection("conn-0", err)) {
        session.Stop();
        return 1802;
    }
    for (int i = 0; i < 50 && !deletedStaleGeneration.load(std::memory_order_acquire); ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    if (!deletedStaleGeneration.load(std::memory_order_acquire)) {
        session.Stop();
        return 1804;
    }
    session.Stop();

    if (beforePublishCalls.load(std::memory_order_acquire) < 2) return 1803;
    return 0;
}

static int TestStartSlotCleansUnpublishedContextOnLocalBindFailure() {
    QuicClientSession session;
    TqConfig cfg{};
    cfg.Mode = TqMode::Client;
    cfg.QuicCa = "cert/ca.crt";
    cfg.QuicDisable1RttEncryption = false;
    cfg.QuicPaths.push_back(TqQuicPathConfig{"bad-local", "not-an-ip", "127.0.0.1:4433", 1});

    if (!session.Start(cfg)) return 1811;

    std::atomic<int> deletedContexts{0};
    QuicClientSession::ReconnectTestHooks hooks;
    hooks.ContextDeleted = [&](size_t index, uint64_t generation) {
        if (index == 0 && generation == 1) {
            deletedContexts.fetch_add(1, std::memory_order_acq_rel);
        }
    };
    session.SetReconnectTestHooks(std::move(hooks));

    std::string err;
    if (!session.ReconnectConnection("conn-0", err)) {
        session.Stop();
        return 1812;
    }
    for (int i = 0; i < 50 && deletedContexts.load(std::memory_order_acquire) == 0; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    session.Stop();

    if (deletedContexts.load(std::memory_order_acquire) == 0) return 1813;
    return 0;
}

static int TestConnectionStartFailureCleansContextAndSchedulesRetry() {
    QuicClientSession session;
    TqConfig cfg{};
    cfg.Mode = TqMode::Client;
    cfg.QuicPeer = "127.0.0.1:4433";
    cfg.QuicConnections = 1;
    cfg.QuicCa = "cert/ca.crt";
    cfg.QuicDisable1RttEncryption = false;

    std::atomic<int> scheduled{0};
    std::function<void()> retryTask;
    session.SetDelayedTaskScheduler(
        [&](std::chrono::milliseconds, std::function<void()> task) {
            scheduled.fetch_add(1, std::memory_order_acq_rel);
            retryTask = std::move(task);
            return true;
        });

    if (!session.Start(cfg)) return 1821;

    std::atomic<int> deletedContexts{0};
    QuicClientSession::ReconnectTestHooks hooks;
    hooks.ConnectionStartOverride = [](size_t index) {
        return index == 0 ? QUIC_STATUS_ABORTED : QUIC_STATUS_SUCCESS;
    };
    hooks.ContextDeleted = [&](size_t index, uint64_t generation) {
        if (index == 0 && generation == 1) {
            deletedContexts.fetch_add(1, std::memory_order_acq_rel);
        }
    };
    session.SetReconnectTestHooks(std::move(hooks));

    std::string err;
    if (!session.ReconnectConnection("conn-0", err)) {
        session.Stop();
        return 1822;
    }
    for (int i = 0; i < 50 && deletedContexts.load(std::memory_order_acquire) == 0; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    session.Stop();

    if (deletedContexts.load(std::memory_order_acquire) == 0) return 1823;
    if (scheduled.load(std::memory_order_acquire) == 0 || !retryTask) return 1824;
    return 0;
}

static int TestConnectionSnapshotIncludesConfiguredPathMetadata() {
    TqConfig cfg;
    cfg.QuicPaths.push_back(TqQuicPathConfig{"cmcc", "10.10.1.2", "36.1.1.10:443", 2});
    cfg.QuicPaths.push_back(TqQuicPathConfig{"ctcc", "10.20.1.2", "59.1.1.10:443", 1});

    QuicClientSession session;
    session.MarkReconnectStartedForTest(3, cfg);

    const auto snapshots = session.SnapshotConnections();
    if (snapshots.size() != 3) return 150;
    if (snapshots[0].PathName != "cmcc" ||
        snapshots[0].LocalAddress != "10.10.1.2" ||
        snapshots[0].PeerAddress != "36.1.1.10:443") {
        return 151;
    }
    if (snapshots[1].PathName != "cmcc" ||
        snapshots[1].LocalAddress != "10.10.1.2" ||
        snapshots[1].PeerAddress != "36.1.1.10:443") {
        return 152;
    }
    if (snapshots[2].PathName != "ctcc" ||
        snapshots[2].LocalAddress != "10.20.1.2" ||
        snapshots[2].PeerAddress != "59.1.1.10:443") {
        return 153;
    }
    session.Stop();
    return 0;
}

static int TestPickConnectionRoundRobinSkipsUnavailableSlots() {
    auto* conn0 = reinterpret_cast<MsQuicConnection*>(static_cast<uintptr_t>(0x1000));
    auto* conn1 = reinterpret_cast<MsQuicConnection*>(static_cast<uintptr_t>(0x1001));
    auto* conn2 = reinterpret_cast<MsQuicConnection*>(static_cast<uintptr_t>(0x2000));

    QuicClientSession session;
    std::function<void()> retryTask;

    session.MarkReconnectStartedForTest(4);
    session.SetDelayedTaskScheduler(
        [&](std::chrono::milliseconds, std::function<void()> task) {
            retryTask = std::move(task);
            return true;
        });
    session.MarkSlotConnectedForTest(0, conn0);
    session.MarkSlotConnectedForTest(1, conn1);
    session.ScheduleStartRetryForTest(1);
    session.MarkSlotConnectedForTest(2, conn2);
    session.MarkSlotConnectedForTest(3, nullptr);

    if (session.PickConnectionForTest() != conn0) return 160;
    if (session.PickConnectionForTest() != conn2) return 161;
    if (session.PickConnectionForTest() != conn0) return 162;

    session.MarkSlotDisconnectedForTest(0);
    if (session.PickConnectionForTest() != conn2) return 163;
    if (session.PickConnectionForTest() != conn2) return 164;
    if (session.PickConnectionForTest() != conn2) return 165;

    if (!retryTask) return 166;
    session.Stop();
    return 0;
}

static int TestScheme2CredentialConfig() {
    TqConfig client;
    client.QuicCa = "ca.crt";
    TqCredentialConfigSnapshot clientCred =
        TqBuildCredentialConfigSnapshotForTest(client, false);
    if (clientCred.Type != QUIC_CREDENTIAL_TYPE_NONE) {
        return 60;
    }
    if (!(clientCred.Flags & QUIC_CREDENTIAL_FLAG_CLIENT)) {
        return 61;
    }
    if (!(clientCred.Flags & QUIC_CREDENTIAL_FLAG_SET_CA_CERTIFICATE_FILE)) {
        return 62;
    }
    if (clientCred.Flags & QUIC_CREDENTIAL_FLAG_NO_CERTIFICATE_VALIDATION) {
        return 63;
    }
    if (clientCred.HasCertificateFile) {
        return 64;
    }
    if (clientCred.CaCertificateFile != "ca.crt") {
        return 65;
    }

    TqConfig server;
    server.QuicCert = "server.crt";
    server.QuicKey = "server.key";
    TqCredentialConfigSnapshot serverCred =
        TqBuildCredentialConfigSnapshotForTest(server, true);
    if (serverCred.Type != QUIC_CREDENTIAL_TYPE_CERTIFICATE_FILE) {
        return 66;
    }
    if (serverCred.Flags & QUIC_CREDENTIAL_FLAG_CLIENT) {
        return 67;
    }
    if (serverCred.Flags & QUIC_CREDENTIAL_FLAG_REQUIRE_CLIENT_AUTHENTICATION) {
        return 68;
    }
    if (serverCred.Flags & QUIC_CREDENTIAL_FLAG_SET_CA_CERTIFICATE_FILE) {
        return 69;
    }
    if (!serverCred.HasCertificateFile) {
        return 70;
    }
    if (!serverCred.CaCertificateFile.empty()) {
        return 71;
    }
    return 0;
}

static int TestServerConnectionSnapshotIncludesClientName() {
    HQUIC handle = reinterpret_cast<HQUIC>(static_cast<uintptr_t>(0x4100));

    const uint32_t id = TqRegisterServerConnectionForTest(handle);
    if (id == 0) return 1901;
    if (!TqSetServerConnectionClientNameForTest(handle, "office-a")) {
        TqUnregisterServerConnectionForTest(handle);
        return 1902;
    }
    if (TqSetServerConnectionClientNameForTest(handle, "bad name")) {
        TqUnregisterServerConnectionForTest(handle);
        return 1905;
    }
    HQUIC unknownHandle = reinterpret_cast<HQUIC>(static_cast<uintptr_t>(0x4101));
    if (TqSetServerConnectionClientNameForTest(unknownHandle, "office-b")) {
        TqUnregisterServerConnectionForTest(handle);
        return 1906;
    }
    auto snapshots = TqSnapshotServerConnections();
    bool found = false;
    for (const auto& snapshot : snapshots) {
        if (snapshot.ConnectionId == "srv-" + std::to_string(id)) {
            found = true;
            if (snapshot.ClientName != "office-a") {
                TqUnregisterServerConnectionForTest(handle);
                return 1903;
            }
        }
    }
    if (!found) {
        TqUnregisterServerConnectionForTest(handle);
        return 1904;
    }
    TqUnregisterServerConnectionForTest(handle);
    return 0;
}

static int TestClientHelloSentAfterConnected() {
    TqConfig cfg;
    cfg.ClientName = "office-a";
    cfg.QuicPeer = "127.0.0.1:443";

    QuicClientSession session;
    session.MarkReconnectStartedForTest(1, cfg);

    int helloSends = 0;
    TqClientHello decoded{};
    QuicClientSession::ReconnectTestHooks hooks;
    hooks.SendClientHelloOverride = [&](
        MsQuicConnection* connection,
        QUIC_STREAM_OPEN_FLAGS openFlags,
        QUIC_SEND_FLAGS sendFlags,
        const std::vector<uint8_t>& payload) {
        if (connection != reinterpret_cast<MsQuicConnection*>(static_cast<uintptr_t>(0x5100))) {
            return false;
        }
        if (openFlags != QUIC_STREAM_OPEN_FLAG_NONE) {
            return false;
        }
        if (sendFlags != static_cast<QUIC_SEND_FLAGS>(QUIC_SEND_FLAG_START | QUIC_SEND_FLAG_FIN)) {
            return false;
        }
        ++helloSends;
        return TqDecodeClientHello(payload.data(), payload.size(), decoded);
    };
    session.SetReconnectTestHooks(std::move(hooks));

    session.SendClientHelloForTest(reinterpret_cast<MsQuicConnection*>(static_cast<uintptr_t>(0x5100)));
    if (helloSends != 1) return 1910;
    if (decoded.ClientName != "office-a") return 1911;

    TqConfig emptyCfg = cfg;
    emptyCfg.ClientName.clear();
    QuicClientSession emptySession;
    emptySession.MarkReconnectStartedForTest(1, emptyCfg);
    int emptySends = 0;
    QuicClientSession::ReconnectTestHooks emptyHooks;
    emptyHooks.SendClientHelloOverride = [&](
        MsQuicConnection*,
        QUIC_STREAM_OPEN_FLAGS,
        QUIC_SEND_FLAGS,
        const std::vector<uint8_t>&) {
        ++emptySends;
        return true;
    };
    emptySession.SetReconnectTestHooks(std::move(emptyHooks));
    emptySession.SendClientHelloForTest(reinterpret_cast<MsQuicConnection*>(static_cast<uintptr_t>(0x5101)));
    if (emptySends != 0) return 1912;

    TqConfig invalidCfg = cfg;
    invalidCfg.ClientName = "bad name";
    QuicClientSession invalidSession;
    invalidSession.MarkReconnectStartedForTest(1, invalidCfg);
    int invalidSends = 0;
    QuicClientSession::ReconnectTestHooks invalidHooks;
    invalidHooks.SendClientHelloOverride = [&](
        MsQuicConnection*,
        QUIC_STREAM_OPEN_FLAGS,
        QUIC_SEND_FLAGS,
        const std::vector<uint8_t>&) {
        ++invalidSends;
        return true;
    };
    invalidSession.SetReconnectTestHooks(std::move(invalidHooks));
    invalidSession.SendClientHelloForTest(reinterpret_cast<MsQuicConnection*>(static_cast<uintptr_t>(0x5102)));
    if (invalidSends != 0) return 1913;

    session.Stop();
    emptySession.Stop();
    invalidSession.Stop();
    return 0;
}

int main() {
    if (int rc = TestServerNewConnectionDisable1RttStatusClassification()) return rc;
    if (int rc = TestTerminalEscalationRejectsOldGeneration()) return rc;
    if (int rc = TestTerminalEscalationIsExactlyOnce()) return rc;
    if (int rc = TestTerminalEscalationSuppressesClosingConnection()) return rc;
    if (int rc = TestTerminalEscalationPinsConnectionAcrossSlotErase()) return rc;
    if (int rc = TestQueuedTerminalOperationRejectsReconnectedSlot()) return rc;
    if (int rc = TestServerTerminalEscalationRegistrySafety()) return rc;
    if (int rc = TestServerTerminalEscalationPinsOwnerAcrossRecordErase()) return rc;
    if (int rc = TestServerDeferredCleanupWaitsForOuterTrampolineAndDrainsFailures()) return rc;
    if (int rc = TestServerCleanupSessionWatermarkAllowsLateEnqueue()) return rc;
    if (int rc = TestServerCleanupConcurrentSessionDrains()) return rc;
    if (int rc = TestServerCleanupDuplicateEnqueueIsSuppressed()) return rc;
    if (int rc = TestFixedDelayRetrySchedulesAndRestartsSlot()) return rc;
    if (int rc = TestDelayedRetryDropsAfterStop()) return rc;
    if (int rc = TestFixedDelayRetryCoalescesDuplicates()) return rc;
    if (int rc = TestRejectedSchedulerAllowsLaterRetry()) return rc;
    if (int rc = TestConnectionStartPendingIsAccepted()) return rc;
    if (int rc = TestShutdownCompleteSchedulesSlotRestart()) return rc;
    if (int rc = TestConnectionSnapshotAndSlotControls()) return rc;
    if (int rc = TestPickConnectionWithIdReturnsSlotId()) return rc;
    if (int rc = TestPickConnectionWithIdReturnsEmptyWhenDisconnected()) return rc;
    if (int rc = TestQuicPathSlotExpansion()) return rc;
    if (int rc = TestQuicPeerListSlotExpansionRoundRobin()) return rc;
    if (int rc = TestQuicPathModeRejectsSlotTopologyMutation()) return rc;
    if (int rc = TestStartSlotUsesConfiguredPathSlots()) return rc;
    if (int rc = TestStartSlotUsesPeerListRoundRobinPaths()) return rc;
    if (int rc = TestStartSlotDoesNotOverwriteChangedGeneration()) return rc;
    if (int rc = TestStartSlotCleansUnpublishedContextOnLocalBindFailure()) return rc;
    if (int rc = TestConnectionStartFailureCleansContextAndSchedulesRetry()) return rc;
    if (int rc = TestConnectionSnapshotIncludesConfiguredPathMetadata()) return rc;
    if (int rc = TestPickConnectionRoundRobinSkipsUnavailableSlots()) return rc;
    if (int rc = TestScheme2CredentialConfig()) return rc;
    if (int rc = TestServerConnectionSnapshotIncludesClientName()) return rc;
    if (int rc = TestClientHelloSentAfterConnected()) return rc;
    if (int rc = TestServerCleanupFinalStopIsSerializedAndClosesAdmission()) return rc;
    return 0;
}
