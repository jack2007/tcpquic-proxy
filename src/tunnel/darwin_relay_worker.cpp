#if defined(__APPLE__)

#include "darwin_relay_worker.h"

#include <sys/event.h>
#include <sys/time.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <memory>
#include <utility>

namespace {
constexpr uintptr_t kWakeIdent = 1;
}

struct TqDarwinRelayWorker::RelayState {
    uint64_t Id{0};
    TqSocketHandle TcpFd{TqInvalidSocket};
    MsQuicStream* Stream{nullptr};
    TqRelayHandle* PublicHandle{nullptr};
    ITqCompressor* Compressor{nullptr};
    ITqDecompressor* Decompressor{nullptr};
    TqCompressAlgo CompressAlgo{TqCompressAlgo::None};
    bool TcpReadArmed{false};
    bool TcpWriteArmed{false};
    bool Closing{false};
};

TqDarwinRelayWorker::TqDarwinRelayWorker(const TqDarwinRelayWorkerConfig& config)
    : Config(config), EventQueue(config.EventQueueCapacity) {}

TqDarwinRelayWorker::~TqDarwinRelayWorker() {
    Stop();
}

bool TqDarwinRelayWorker::Start() {
    std::lock_guard<std::mutex> lifecycleLock(LifecycleMutex);
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
    std::unordered_map<uint64_t, std::shared_ptr<RelayState>> relays;
    {
        std::lock_guard<std::mutex> lifecycleLock(LifecycleMutex);
        if (!Running.exchange(false, std::memory_order_acq_rel) && KqueueFd < 0) {
            return;
        }

        (void)Wake();
        if (Thread.joinable()) {
            Thread.join();
        }

        if (KqueueFd >= 0) {
            {
                std::lock_guard<std::mutex> lock(RelayMutex);
                relays.swap(Relays);
            }
            for (const auto& entry : relays) {
                RemoveTcpFilters(entry.second);
            }
            close(KqueueFd);
            KqueueFd = -1;
        }
    }

    for (const auto& entry : relays) {
        ClearPublicHandle(entry.second);
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

bool TqDarwinRelayWorker::EnqueueEvent(TqDarwinRelayEvent&& event) {
    if (!EventQueue.TryPush(std::move(event))) {
        Errors.fetch_add(1, std::memory_order_relaxed);
        return false;
    }
    (void)Wake();
    return true;
}

#if defined(TCPQUIC_TESTING)
bool TqDarwinRelayWorker::StartForTest() {
    Running.store(true, std::memory_order_release);
    return true;
}

bool TqDarwinRelayWorker::EnqueueForTest(TqDarwinRelayEvent event) {
    return EnqueueEvent(std::move(event));
}

uint32_t TqDarwinRelayWorker::DrainWakeForTest() {
    return DrainWakeEvents();
}

bool TqDarwinRelayWorker::RunningForTest() const {
    return Running.load(std::memory_order_acquire);
}

void TqDarwinRelayWorker::SetRegisterTcpFiltersFailureForTest(bool fail) {
    std::lock_guard<std::mutex> lifecycleLock(LifecycleMutex);
    FailRegisterTcpFiltersForTest = fail;
}
#endif

uint32_t TqDarwinRelayWorker::DrainEvents(uint32_t budget) {
    uint32_t processed = 0;
    TqDarwinRelayEvent event{};
    while (processed < budget && EventQueue.TryPop(event)) {
        ++processed;
        switch (event.Type) {
        case TqDarwinRelayEventType::Shutdown:
            Running.store(false, std::memory_order_release);
            break;
        case TqDarwinRelayEventType::StopRelay:
            break;
        default:
            break;
        }
        event = TqDarwinRelayEvent{};
    }
    if (processed > 0) {
        EventsProcessed.fetch_add(processed, std::memory_order_relaxed);
    }
    return processed;
}

uint32_t TqDarwinRelayWorker::DrainWakeEvents() {
    uint32_t totalProcessed = 0;
    const uint32_t budget = Config.EventBudget == 0 ? 1 : Config.EventBudget;
    for (;;) {
        const uint32_t processed = DrainEvents(budget);
        totalProcessed += processed;
        if (processed < budget || EventQueue.SizeApprox() == 0) {
            break;
        }
    }
    return totalProcessed;
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

        for (int i = 0; i < count; ++i) {
            ProcessKqueueEvent(events[i]);
        }
    }
}

bool TqDarwinRelayWorker::RegisterTcpFilters(const std::shared_ptr<RelayState>& relay) {
    if (relay == nullptr || KqueueFd < 0) {
        return false;
    }
#if defined(TCPQUIC_TESTING)
    if (FailRegisterTcpFiltersForTest) {
        return false;
    }
#endif

    struct kevent changes[2];
    EV_SET(
        &changes[0],
        static_cast<uintptr_t>(relay->TcpFd),
        EVFILT_READ,
        EV_ADD | EV_ENABLE,
        0,
        0,
        reinterpret_cast<void*>(relay->Id));
    EV_SET(
        &changes[1],
        static_cast<uintptr_t>(relay->TcpFd),
        EVFILT_WRITE,
        EV_ADD | EV_DISABLE,
        0,
        0,
        reinterpret_cast<void*>(relay->Id));
    return kevent(KqueueFd, changes, 2, nullptr, 0, nullptr) == 0;
}

bool TqDarwinRelayWorker::UpdateTcpInterest(const std::shared_ptr<RelayState>& relay) {
    if (relay == nullptr || KqueueFd < 0) {
        return false;
    }

    struct kevent changes[2];
    EV_SET(
        &changes[0],
        static_cast<uintptr_t>(relay->TcpFd),
        EVFILT_READ,
        relay->TcpReadArmed ? EV_ENABLE : EV_DISABLE,
        0,
        0,
        reinterpret_cast<void*>(relay->Id));
    EV_SET(
        &changes[1],
        static_cast<uintptr_t>(relay->TcpFd),
        EVFILT_WRITE,
        relay->TcpWriteArmed ? EV_ENABLE : EV_DISABLE,
        0,
        0,
        reinterpret_cast<void*>(relay->Id));
    return kevent(KqueueFd, changes, 2, nullptr, 0, nullptr) == 0;
}

void TqDarwinRelayWorker::RemoveTcpFilters(const std::shared_ptr<RelayState>& relay) {
    if (relay == nullptr || KqueueFd < 0 || !TqSocketValid(relay->TcpFd)) {
        return;
    }

    struct kevent changes[2];
    EV_SET(&changes[0], static_cast<uintptr_t>(relay->TcpFd), EVFILT_READ, EV_DELETE, 0, 0, nullptr);
    EV_SET(&changes[1], static_cast<uintptr_t>(relay->TcpFd), EVFILT_WRITE, EV_DELETE, 0, 0, nullptr);
    (void)kevent(KqueueFd, changes, 2, nullptr, 0, nullptr);
}

void TqDarwinRelayWorker::ClearPublicHandle(const std::shared_ptr<RelayState>& relay) {
    if (relay == nullptr || relay->PublicHandle == nullptr) {
        return;
    }
    if (relay->PublicHandle->DarwinRelayId == relay->Id) {
        relay->PublicHandle->DarwinRelayId = 0;
    }
    if (relay->PublicHandle->DarwinWorker == this) {
        relay->PublicHandle->DarwinWorker = nullptr;
    }
    if (relay->PublicHandle->Backend == TqRelayBackendType::DarwinWorker) {
        relay->PublicHandle->Backend = TqRelayBackendType::None;
    }
}

std::shared_ptr<TqDarwinRelayWorker::RelayState> TqDarwinRelayWorker::FindRelay(uint64_t relayId) {
    std::lock_guard<std::mutex> lock(RelayMutex);
    const auto it = Relays.find(relayId);
    if (it == Relays.end()) {
        return nullptr;
    }
    return it->second;
}

void TqDarwinRelayWorker::ProcessKqueueEvent(const struct kevent& event) {
    if (event.filter == EVFILT_USER && event.ident == kWakeIdent) {
        (void)DrainWakeEvents();
        return;
    }

    const auto relayId = reinterpret_cast<uintptr_t>(event.udata);
    if (relayId == 0) {
        Errors.fetch_add(1, std::memory_order_relaxed);
        return;
    }

    ProcessTcpEvents(
        static_cast<uint64_t>(relayId),
        event.filter,
        static_cast<uint16_t>(event.flags),
        event.data);
}

void TqDarwinRelayWorker::ProcessTcpEvents(uint64_t relayId, int16_t filter, uint16_t flags, intptr_t data) {
    (void)data;
    std::shared_ptr<RelayState> relay = FindRelay(relayId);
    if (relay == nullptr) {
        Errors.fetch_add(1, std::memory_order_relaxed);
        return;
    }

    if ((flags & EV_ERROR) != 0) {
        relay->Closing = true;
        Errors.fetch_add(1, std::memory_order_relaxed);
        return;
    }

    if (filter != EVFILT_READ && filter != EVFILT_WRITE) {
        Errors.fetch_add(1, std::memory_order_relaxed);
    }
}

TqDarwinRelayRegistrationResult TqDarwinRelayWorker::RegisterRelayWithId(
    const TqDarwinRelayRegistration& registration) {
    std::lock_guard<std::mutex> lifecycleLock(LifecycleMutex);
    if (!Running.load(std::memory_order_acquire) || KqueueFd < 0) {
        return {};
    }
    if (!TqSocketValid(registration.TcpFd) || registration.Stream == nullptr || registration.Handle == nullptr) {
        return {};
    }
    if (!TqSetNonBlocking(registration.TcpFd)) {
        Errors.fetch_add(1, std::memory_order_relaxed);
        return {};
    }

    auto relay = std::make_shared<RelayState>();
    relay->TcpFd = registration.TcpFd;
    relay->Stream = registration.Stream;
    relay->PublicHandle = registration.Handle;
    relay->Compressor = registration.Compressor;
    relay->Decompressor = registration.Decompressor;
    relay->CompressAlgo = registration.CompressAlgo;
    relay->TcpReadArmed = true;

    {
        std::lock_guard<std::mutex> lock(RelayMutex);
        relay->Id = NextRelayId++;
        Relays.emplace(relay->Id, relay);
    }

    if (!RegisterTcpFilters(relay)) {
        RemoveTcpFilters(relay);
        Errors.fetch_add(1, std::memory_order_relaxed);
        {
            std::lock_guard<std::mutex> lock(RelayMutex);
            Relays.erase(relay->Id);
        }
        ClearPublicHandle(relay);
        return {};
    }

    registration.Handle->Backend = TqRelayBackendType::DarwinWorker;
    registration.Handle->DarwinWorker = this;
    registration.Handle->DarwinRelayId = relay->Id;
    return {true, relay->Id};
}

void TqDarwinRelayWorker::UnregisterRelay(uint64_t relayId) {
    std::shared_ptr<RelayState> relay;
    {
        std::lock_guard<std::mutex> lifecycleLock(LifecycleMutex);
        std::lock_guard<std::mutex> lock(RelayMutex);
        const auto it = Relays.find(relayId);
        if (it == Relays.end()) {
            return;
        }
        relay = it->second;
        Relays.erase(it);
        RemoveTcpFilters(relay);
    }

    ClearPublicHandle(relay);
}

TqDarwinRelayWorkerSnapshot TqDarwinRelayWorker::Snapshot() const {
    TqDarwinRelayWorkerSnapshot snapshot{};
    snapshot.EventsProcessed = EventsProcessed.load(std::memory_order_relaxed);
    snapshot.Wakeups = Wakeups.load(std::memory_order_relaxed);
    snapshot.PendingEvents = EventQueue.SizeApprox();
    {
        std::lock_guard<std::mutex> lock(RelayMutex);
        snapshot.ActiveRelays = Relays.size();
        for (const auto& entry : Relays) {
            if (entry.second->TcpReadArmed) {
                ++snapshot.TcpReadArmedRelays;
            }
            if (entry.second->TcpWriteArmed) {
                ++snapshot.TcpWriteArmedRelays;
            }
        }
    }
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
