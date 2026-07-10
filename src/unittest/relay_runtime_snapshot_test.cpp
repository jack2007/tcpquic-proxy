#include "relay_runtime_snapshot.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <memory>
#include <mutex>
#include <thread>
#include <type_traits>
#include <vector>

namespace {

struct FakeWorker final {
    ~FakeWorker() { ++Destructions; }
    void ThrowingSnapshot() const { throw 7; }

    static std::atomic<uint32_t> Destructions;
};

std::atomic<uint32_t> FakeWorker::Destructions{0};
struct FakeSnapshot final {};

static_assert(!std::is_copy_constructible_v<TqRelayRuntimeSnapshotLease<FakeWorker>>);

bool Expect(bool condition, const char* message) {
    if (!condition) {
        std::fprintf(stderr, "%s\n", message);
    }
    return condition;
}

bool TestLeaseAndStopBarrier() {
    TqRelayRuntimeSnapshotSupport support;
    TqRelayRuntimeSnapshotResult<FakeSnapshot> result;
    if (!Expect(!result.SnapshotComplete && !result.IdentitiesComplete && result.Workers.empty(),
            "snapshot result must default to incomplete")) {
        return false;
    }

    std::mutex runtimeLock;
    std::vector<std::unique_ptr<FakeWorker>> workers;
    workers.emplace_back(std::make_unique<FakeWorker>());

    TqRelayRuntimeSnapshotLease<FakeWorker> lease;
    {
        std::unique_lock<std::mutex> runtimeGuard(runtimeLock);
        lease = support.AcquireWorkersLocked(runtimeGuard, workers);
    }
    if (!Expect(lease.Workers().size() == 1 && lease.Workers()[0].WorkerIndex == 0 &&
            lease.Workers()[0].Worker == workers[0].get(), "lease must retain slot identity")) {
        return false;
    }

    auto movedLease = std::move(lease);
    TqRelayRuntimeSnapshotLease<FakeWorker> assignedLease;
    assignedLease = std::move(movedLease);
    if (!Expect(support.Stats().InFlight == 1, "moving a lease must not release it")) {
        return false;
    }

    std::mutex signalLock;
    std::condition_variable signalCv;
    bool stopEntered = false;
    bool stopFinished = false;
    std::thread stopper([&]() {
        std::unique_lock<std::mutex> runtimeGuard(runtimeLock);
        {
            std::lock_guard<std::mutex> signalGuard(signalLock);
            stopEntered = true;
            signalCv.notify_all();
        }
        support.WaitForIdleLocked(runtimeGuard);
        workers.clear();
        {
            std::lock_guard<std::mutex> signalGuard(signalLock);
            stopFinished = true;
            signalCv.notify_all();
        }
    });

    {
        std::unique_lock<std::mutex> signalGuard(signalLock);
        signalCv.wait(signalGuard, [&]() { return stopEntered; });
        if (!Expect(!stopFinished && FakeWorker::Destructions.load() == 0,
                "Stop must wait for the lease before destroying workers")) {
            assignedLease = {};
            stopper.join();
            return false;
        }
    }
    assignedLease = {};
    {
        std::unique_lock<std::mutex> signalGuard(signalLock);
        signalCv.wait(signalGuard, [&]() { return stopFinished; });
    }
    stopper.join();
    if (!Expect(support.Stats().InFlight == 0 && FakeWorker::Destructions.load() == 1,
            "last lease release must unblock Stop exactly once")) {
        return false;
    }

    std::vector<std::unique_ptr<FakeWorker>> throwWorkers;
    throwWorkers.emplace_back(std::make_unique<FakeWorker>());
    try {
        std::unique_lock<std::mutex> runtimeGuard(runtimeLock);
        auto throwingLease = support.AcquireWorkersLocked(runtimeGuard, throwWorkers);
        runtimeGuard.unlock();
        throwingLease.Workers()[0].Worker->ThrowingSnapshot();
    } catch (int) {
    }
    if (!Expect(support.Stats().InFlight == 0, "exception paths must release leases")) {
        return false;
    }

    {
        std::unique_lock<std::mutex> runtimeGuard(runtimeLock);
        support.FailNextWorkerRefMaterializationForTest();
        bool threw = false;
        try {
            (void)support.AcquireWorkersLocked(runtimeGuard, throwWorkers);
        } catch (const std::bad_alloc&) {
            threw = true;
        }
        if (!Expect(threw && support.Stats().InFlight == 0,
                "worker-ref materialization failure must precede in-flight registration")) {
            return false;
        }
    }

    std::vector<std::unique_ptr<FakeWorker>> emptyWorkers;
    {
        std::unique_lock<std::mutex> runtimeGuard(runtimeLock);
        auto emptyLease = support.AcquireWorkersLocked(runtimeGuard, emptyWorkers);
        if (!Expect(emptyLease.Workers().empty(), "empty runtime must produce an empty lease")) {
            return false;
        }
    }
    return true;
}

bool TestExecutionGate() {
    TqRelayRuntimeSnapshotExecutionGate gate;
    auto requestPermit = gate.TryAcquire(std::chrono::steady_clock::now() + std::chrono::seconds(1));
    if (!Expect(static_cast<bool>(requestPermit), "first request must acquire execution permit")) {
        return false;
    }
    auto commandPermit = requestPermit;
    requestPermit.reset();
    gate.RecordDetachedLateCommand();

    const auto rejected = gate.TryAcquire(std::chrono::steady_clock::now());
    const auto busyStats = gate.Stats();
    if (!Expect(!rejected && busyStats.Busy == 1 && busyStats.Outstanding == 1 &&
            busyStats.DeadlineTimeouts == 1 && busyStats.DetachedLateCommands == 1,
            "late command must keep the execution gate busy after request timeout")) {
        return false;
    }

    std::thread completion([permit = std::move(commandPermit)]() mutable { permit.reset(); });
    completion.join();
    if (!Expect(gate.Stats().Outstanding == 0,
            "cross-thread completion must release the final permit token")) {
        return false;
    }
    auto nextPermit = gate.TryAcquire(std::chrono::steady_clock::now() + std::chrono::seconds(1));
    if (!Expect(static_cast<bool>(nextPermit) && gate.Stats().Outstanding == 1,
            "a new request may acquire only after late completion")) {
        return false;
    }
    nextPermit.reset();
    return Expect(gate.Stats().Outstanding == 0, "final permit release must clear outstanding count");
}

} // namespace

int main() {
    return (TestLeaseAndStopBarrier() && TestExecutionGate()) ? 0 : 1;
}
