#pragma once

#include <algorithm>
#include <atomic>
#include <cassert>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <limits>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <utility>
#include <vector>

enum class TqRelayRuntimeState : uint8_t {
    Stopped,
    Starting,
    Running,
    Stopping,
};

template <typename WorkerT>
struct TqRelaySnapshotWorkerRef {
    uint32_t WorkerIndex{0};
    WorkerT* Worker{nullptr};
};

template <typename SnapshotT>
struct TqRelayRuntimeSnapshotResult {
    bool SnapshotComplete{false};
    bool IdentitiesComplete{false};
    std::vector<SnapshotT> Workers;
};

struct TqRelayRuntimeSnapshotStats {
    uint32_t InFlight{0};
    uint64_t InFlightMax{0};
    uint64_t AcquireCount{0};
    uint64_t FailureCount{0};
    uint64_t StopWaitCount{0};
    uint64_t StopWaitNanos{0};
};

class TqRelayRuntimeSnapshotSupport;

template <typename WorkerT>
class TqRelayRuntimeSnapshotLease final {
public:
    TqRelayRuntimeSnapshotLease() = default;
    TqRelayRuntimeSnapshotLease(TqRelayRuntimeSnapshotLease&& other) noexcept;
    TqRelayRuntimeSnapshotLease& operator=(TqRelayRuntimeSnapshotLease&& other) noexcept;
    ~TqRelayRuntimeSnapshotLease() noexcept;

    TqRelayRuntimeSnapshotLease(const TqRelayRuntimeSnapshotLease&) = delete;
    TqRelayRuntimeSnapshotLease& operator=(const TqRelayRuntimeSnapshotLease&) = delete;

    const std::vector<TqRelaySnapshotWorkerRef<WorkerT>>& Workers() const noexcept {
        return Workers_;
    }

private:
    friend class TqRelayRuntimeSnapshotSupport;

    TqRelayRuntimeSnapshotLease(
        const TqRelayRuntimeSnapshotSupport* support,
        std::vector<TqRelaySnapshotWorkerRef<WorkerT>> workers) noexcept
        : Support_(support), Workers_(std::move(workers)) {}

    void Reset() noexcept;

    const TqRelayRuntimeSnapshotSupport* Support_{nullptr};
    std::vector<TqRelaySnapshotWorkerRef<WorkerT>> Workers_;
};

class TqRelayRuntimeSnapshotSupport final {
public:
    TqRelayRuntimeSnapshotSupport() = default;
    TqRelayRuntimeSnapshotSupport(const TqRelayRuntimeSnapshotSupport&) = delete;
    TqRelayRuntimeSnapshotSupport& operator=(const TqRelayRuntimeSnapshotSupport&) = delete;

    template <typename WorkerT>
    TqRelayRuntimeSnapshotLease<WorkerT> AcquireWorkersLocked(
        const std::unique_lock<std::mutex>& runtimeGuard,
        const std::vector<std::unique_ptr<WorkerT>>& workers) const {
        assert(runtimeGuard.owns_lock());
        static_cast<void>(runtimeGuard);

        try {
            if (workers.size() > std::numeric_limits<uint32_t>::max()) {
                throw std::length_error("relay runtime has too many workers");
            }

            std::vector<TqRelaySnapshotWorkerRef<WorkerT>> refs;
            refs.reserve(workers.size());
#ifdef TQ_UNIT_TESTING
            if (FailNextWorkerRefMaterialization_.exchange(false, std::memory_order_relaxed)) {
                throw std::bad_alloc();
            }
#endif
            for (uint32_t index = 0; index < workers.size(); ++index) {
                refs.push_back({index, workers[index].get()});
            }

            {
                std::lock_guard<std::mutex> guard(SnapshotLock_);
                ++SnapshotsInFlight_;
                SnapshotInFlightMax_ = std::max(SnapshotInFlightMax_,
                    static_cast<uint64_t>(SnapshotsInFlight_));
                ++AcquireCount_;
            }
            return TqRelayRuntimeSnapshotLease<WorkerT>(this, std::move(refs));
        } catch (...) {
            RecordFailure();
            throw;
        }
    }

    void WaitForIdleLocked(const std::unique_lock<std::mutex>& runtimeGuard) const {
        assert(runtimeGuard.owns_lock());
        static_cast<void>(runtimeGuard);
        const auto started = std::chrono::steady_clock::now();
        std::unique_lock<std::mutex> guard(SnapshotLock_);
        ++StopWaitCount_;
        SnapshotCv_.wait(guard, [this]() { return SnapshotsInFlight_ == 0; });
        StopWaitNanos_ += static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now() - started).count());
    }

    void RecordFailure() const noexcept {
        std::lock_guard<std::mutex> guard(SnapshotLock_);
        ++FailureCount_;
    }

    TqRelayRuntimeSnapshotStats Stats() const noexcept {
        std::lock_guard<std::mutex> guard(SnapshotLock_);
        return {SnapshotsInFlight_, SnapshotInFlightMax_, AcquireCount_, FailureCount_,
            StopWaitCount_, StopWaitNanos_};
    }

#ifdef TQ_UNIT_TESTING
    void FailNextWorkerRefMaterializationForTest() const noexcept {
        FailNextWorkerRefMaterialization_.store(true, std::memory_order_relaxed);
    }
#endif

private:
    template <typename WorkerT>
    friend class TqRelayRuntimeSnapshotLease;

    void ReleaseWorkers() const noexcept {
        std::lock_guard<std::mutex> guard(SnapshotLock_);
        assert(SnapshotsInFlight_ > 0);
        --SnapshotsInFlight_;
        if (SnapshotsInFlight_ == 0) {
            SnapshotCv_.notify_all();
        }
    }

    mutable std::mutex SnapshotLock_;
    mutable std::condition_variable SnapshotCv_;
    mutable uint32_t SnapshotsInFlight_{0};
    mutable uint64_t SnapshotInFlightMax_{0};
    mutable uint64_t AcquireCount_{0};
    mutable uint64_t FailureCount_{0};
    mutable uint64_t StopWaitCount_{0};
    mutable uint64_t StopWaitNanos_{0};
#ifdef TQ_UNIT_TESTING
    mutable std::atomic<bool> FailNextWorkerRefMaterialization_{false};
#endif
};

template <typename WorkerT>
inline TqRelayRuntimeSnapshotLease<WorkerT>::TqRelayRuntimeSnapshotLease(
    TqRelayRuntimeSnapshotLease&& other) noexcept
    : Support_(other.Support_), Workers_(std::move(other.Workers_)) {
    other.Support_ = nullptr;
}

template <typename WorkerT>
inline TqRelayRuntimeSnapshotLease<WorkerT>& TqRelayRuntimeSnapshotLease<WorkerT>::operator=(
    TqRelayRuntimeSnapshotLease&& other) noexcept {
    if (this != &other) {
        Reset();
        Support_ = other.Support_;
        Workers_ = std::move(other.Workers_);
        other.Support_ = nullptr;
    }
    return *this;
}

template <typename WorkerT>
inline TqRelayRuntimeSnapshotLease<WorkerT>::~TqRelayRuntimeSnapshotLease() noexcept {
    Reset();
}

template <typename WorkerT>
inline void TqRelayRuntimeSnapshotLease<WorkerT>::Reset() noexcept {
    if (Support_ != nullptr) {
        Support_->ReleaseWorkers();
        Support_ = nullptr;
    }
}

struct TqRelayRuntimeSnapshotExecutionGateStats {
    uint32_t Busy{0};
    uint32_t Outstanding{0};
    uint64_t WaitCount{0};
    uint64_t WaitNanos{0};
    uint64_t DeadlineTimeouts{0};
    uint64_t DetachedLateCommands{0};
};

struct TqRelayRuntimeSnapshotExecutionGateState final {
    void Release() noexcept {
        std::lock_guard<std::mutex> guard(Lock);
        assert(Busy);
        Busy = false;
        Cv.notify_all();
    }

    mutable std::mutex Lock;
    std::condition_variable Cv;
    bool Busy{false};
    uint64_t WaitCount{0};
    uint64_t WaitNanos{0};
    uint64_t DeadlineTimeouts{0};
    uint64_t DetachedLateCommands{0};
};

class TqRelayRuntimeSnapshotExecutionPermit final {
public:
    TqRelayRuntimeSnapshotExecutionPermit(const TqRelayRuntimeSnapshotExecutionPermit&) = delete;
    TqRelayRuntimeSnapshotExecutionPermit& operator=(const TqRelayRuntimeSnapshotExecutionPermit&) = delete;

    ~TqRelayRuntimeSnapshotExecutionPermit() {
        if (State_ != nullptr) {
            State_->Release();
        }
    }

private:
    friend class TqRelayRuntimeSnapshotExecutionGate;

    explicit TqRelayRuntimeSnapshotExecutionPermit(
        std::shared_ptr<TqRelayRuntimeSnapshotExecutionGateState> state) noexcept
        : State_(std::move(state)) {}

    std::shared_ptr<TqRelayRuntimeSnapshotExecutionGateState> State_;
};

class TqRelayRuntimeSnapshotExecutionGate final {
public:
    using Permit = std::shared_ptr<TqRelayRuntimeSnapshotExecutionPermit>;

    TqRelayRuntimeSnapshotExecutionGate()
        : State_(std::make_shared<TqRelayRuntimeSnapshotExecutionGateState>()) {}

    TqRelayRuntimeSnapshotExecutionGate(const TqRelayRuntimeSnapshotExecutionGate&) = delete;
    TqRelayRuntimeSnapshotExecutionGate& operator=(const TqRelayRuntimeSnapshotExecutionGate&) = delete;

    Permit TryAcquire(std::chrono::steady_clock::time_point deadline) const {
        const auto started = std::chrono::steady_clock::now();
        std::unique_lock<std::mutex> guard(State_->Lock);
        if (started >= deadline) {
            ++State_->DeadlineTimeouts;
            return {};
        }
        while (State_->Busy) {
            ++State_->WaitCount;
            if (std::chrono::steady_clock::now() >= deadline ||
                State_->Cv.wait_until(guard, deadline) == std::cv_status::timeout) {
                State_->WaitNanos += static_cast<uint64_t>(std::chrono::duration_cast<
                    std::chrono::nanoseconds>(std::chrono::steady_clock::now() - started).count());
                ++State_->DeadlineTimeouts;
                return {};
            }
        }
        State_->WaitNanos += static_cast<uint64_t>(std::chrono::duration_cast<
            std::chrono::nanoseconds>(std::chrono::steady_clock::now() - started).count());
        State_->Busy = true;
        return Permit(new TqRelayRuntimeSnapshotExecutionPermit(State_));
    }

    void RecordDetachedLateCommand() const noexcept {
        std::lock_guard<std::mutex> guard(State_->Lock);
        ++State_->DetachedLateCommands;
    }

    TqRelayRuntimeSnapshotExecutionGateStats Stats() const noexcept {
        std::lock_guard<std::mutex> guard(State_->Lock);
        return {State_->Busy ? 1u : 0u, State_->Busy ? 1u : 0u, State_->WaitCount,
            State_->WaitNanos, State_->DeadlineTimeouts, State_->DetachedLateCommands};
    }

private:
    std::shared_ptr<TqRelayRuntimeSnapshotExecutionGateState> State_;
};
