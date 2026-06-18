#if defined(__APPLE__)

#include "darwin_relay_worker.h"

#include <sys/event.h>
#include <sys/time.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>

namespace {
constexpr uintptr_t kWakeIdent = 1;
}

TqDarwinRelayWorker::TqDarwinRelayWorker(const TqDarwinRelayWorkerConfig& config)
    : Config(config) {}

TqDarwinRelayWorker::~TqDarwinRelayWorker() {
    Stop();
}

bool TqDarwinRelayWorker::Start() {
    if (Running.load(std::memory_order_acquire)) {
        return true;
    }

    KqueueFd = kqueue();
    if (KqueueFd < 0) {
        Errors.fetch_add(1, std::memory_order_relaxed);
        return false;
    }

    struct kevent change;
    EV_SET(&change, kWakeIdent, EVFILT_USER, EV_ADD | EV_CLEAR, 0, 0, nullptr);
    if (kevent(KqueueFd, &change, 1, nullptr, 0, nullptr) != 0) {
        Errors.fetch_add(1, std::memory_order_relaxed);
        close(KqueueFd);
        KqueueFd = -1;
        return false;
    }

    Running.store(true, std::memory_order_release);
    Thread = std::thread(&TqDarwinRelayWorker::Run, this);
    return true;
}

void TqDarwinRelayWorker::Stop() {
    if (!Running.exchange(false, std::memory_order_acq_rel)) {
        return;
    }

    (void)Wake();
    if (Thread.joinable()) {
        Thread.join();
    }

    if (KqueueFd >= 0) {
        close(KqueueFd);
        KqueueFd = -1;
    }
}

bool TqDarwinRelayWorker::Wake() {
    if (KqueueFd < 0) {
        return false;
    }

    struct kevent change;
    EV_SET(&change, kWakeIdent, EVFILT_USER, 0, NOTE_TRIGGER, 0, nullptr);
    if (kevent(KqueueFd, &change, 1, nullptr, 0, nullptr) != 0) {
        Errors.fetch_add(1, std::memory_order_relaxed);
        return false;
    }

    Wakeups.fetch_add(1, std::memory_order_relaxed);
    return true;
}

void TqDarwinRelayWorker::Run() {
    struct kevent events[16];
    while (Running.load(std::memory_order_acquire)) {
        const int count = kevent(KqueueFd, nullptr, 0, events, 16, nullptr);
        if (count < 0) {
            if (errno == EINTR) {
                continue;
            }
            Errors.fetch_add(1, std::memory_order_relaxed);
            continue;
        }

        EventsProcessed.fetch_add(static_cast<uint64_t>(count), std::memory_order_relaxed);
        for (int i = 0; i < count; ++i) {
            if (events[i].filter == EVFILT_USER && events[i].ident == kWakeIdent) {
                continue;
            }
        }
    }
}

TqDarwinRelayRegistrationResult TqDarwinRelayWorker::RegisterRelayWithId(
    const TqDarwinRelayRegistration& registration) {
    (void)registration;
    return {};
}

void TqDarwinRelayWorker::UnregisterRelay(uint64_t relayId) {
    (void)relayId;
}

TqDarwinRelayWorkerSnapshot TqDarwinRelayWorker::Snapshot() const {
    TqDarwinRelayWorkerSnapshot snapshot{};
    snapshot.EventsProcessed = EventsProcessed.load(std::memory_order_relaxed);
    snapshot.Wakeups = Wakeups.load(std::memory_order_relaxed);
    snapshot.Errors = Errors.load(std::memory_order_relaxed);
    return snapshot;
}

QUIC_STATUS QUIC_API TqDarwinRelayWorker::StreamCallback(
    MsQuicStream* stream,
    void* context,
    QUIC_STREAM_EVENT* event) noexcept {
    (void)stream;
    (void)context;
    (void)event;
    return QUIC_STATUS_SUCCESS;
}

TqDarwinRelayRuntime& TqDarwinRelayRuntime::Instance() {
    static TqDarwinRelayRuntime runtime;
    return runtime;
}

bool TqDarwinRelayRuntime::Start(const TqTuningConfig& tuning) {
    std::lock_guard<std::mutex> lock(Mutex);
    if (Started) {
        return true;
    }

    const uint32_t count = std::max<uint32_t>(1, tuning.LinuxRelayWorkerCount);
    Workers.reserve(count);
    for (uint32_t i = 0; i < count; ++i) {
        TqDarwinRelayWorkerConfig config{};
        config.WorkerIndex = i;
        config.EventBudget = tuning.LinuxRelayWorkerEventBudget;
        config.ByteBudgetPerTick = tuning.LinuxRelayWorkerByteBudgetPerTick;
        config.ReadChunkSize = tuning.LinuxRelayReadChunkSize;
        config.ReadBatchBytes = tuning.LinuxRelayReadBatchBytes;
        config.MaxIov = tuning.LinuxRelayMaxIov;
        config.TcpWriteMaxBytes = tuning.LinuxRelayTcpWriteMaxBytes;
        config.TcpWriteBurstBytes = tuning.LinuxRelayTcpWriteBurstBytes;
        config.MaxPendingQuicReceiveBytesPerRelay = tuning.LinuxRelayPerTunnelPendingBytes;
        config.DeferredReceiveCompleteBatchBytes = tuning.LinuxRelayQuicReceiveCompleteBatchBytes;
        config.MaxInFlightQuicSends = tuning.RelayMaxInFlightSends;
        config.MaxBufferedQuicSendBytes = tuning.MaxPendingBufferBytesPerRelay;

        auto worker = std::make_unique<TqDarwinRelayWorker>(config);
        if (!worker->Start()) {
            for (auto& startedWorker : Workers) {
                startedWorker->Stop();
            }
            Workers.clear();
            NextWorker = 0;
            return false;
        }
        Workers.push_back(std::move(worker));
    }

    Started = true;
    return true;
}

void TqDarwinRelayRuntime::Stop() {
    std::lock_guard<std::mutex> lock(Mutex);
    for (auto& worker : Workers) {
        worker->Stop();
    }
    Workers.clear();
    Started = false;
    NextWorker = 0;
}

TqDarwinRelayWorker* TqDarwinRelayRuntime::PickWorker() {
    std::lock_guard<std::mutex> lock(Mutex);
    if (Workers.empty()) {
        return nullptr;
    }

    TqDarwinRelayWorker* worker = Workers[NextWorker % Workers.size()].get();
    ++NextWorker;
    return worker;
}

void TqDarwinRelayRuntime::StopRelay(TqRelayHandle* handle) {
    if (handle == nullptr || handle->DarwinWorker == nullptr || handle->DarwinRelayId == 0) {
        return;
    }
    handle->DarwinWorker->UnregisterRelay(handle->DarwinRelayId);
}

#endif
