#if defined(__APPLE__)

#include "darwin_relay_worker.h"
#include "quic_receive_guard.h"
#include "trace.h"

#include <sys/socket.h>
#include <sys/event.h>
#include <sys/time.h>
#include <sys/uio.h>
#include <unistd.h>

#include <algorithm>
#include <cassert>
#include <cerrno>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <memory>
#include <new>
#include <unordered_map>
#include <utility>
#include <vector>

namespace {
constexpr uintptr_t kWakeIdent = 1;
constexpr std::chrono::milliseconds kControlCommandWakeRetryInterval{10};

#if defined(TCPQUIC_TESTING)
TqDarwinRelayStreamSendForTest g_darwinRelayStreamSendForTest = nullptr;
TqDarwinRelayReceiveCompleteForTest g_darwinRelayReceiveCompleteForTest = nullptr;
TqDarwinRelayReceiveSetEnabledForTest g_darwinRelayReceiveSetEnabledForTest = nullptr;
TqDarwinRelaySendMsgForTest g_darwinRelaySendMsgForTest = nullptr;

bool TqDarwinRelayStreamUsableForTest(MsQuicStream* stream) {
    return reinterpret_cast<uintptr_t>(stream) > 4096;
}
#endif

bool SetNoSigPipe(TqSocketHandle fd) {
    int enabled = 1;
    return setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &enabled, sizeof(enabled)) == 0;
}

ssize_t SendMsgNoSignal(TqSocketHandle fd, const struct msghdr* msg) {
#if defined(TCPQUIC_TESTING)
    if (g_darwinRelaySendMsgForTest != nullptr) {
        return g_darwinRelaySendMsgForTest(fd, msg);
    }
#endif
    return sendmsg(fd, msg, 0);
}
}

#if defined(__GNUC__)
__attribute__((weak)) const MsQuicApi* MsQuic = nullptr;
#endif

namespace {
QUIC_STATUS TqDarwinRelayStreamSend(
    MsQuicStream* stream,
    const QUIC_BUFFER* buffers,
    uint32_t bufferCount,
    QUIC_SEND_FLAGS flags,
    void* context) {
#if defined(TCPQUIC_TESTING)
    if (g_darwinRelayStreamSendForTest != nullptr) {
        return g_darwinRelayStreamSendForTest(stream, buffers, bufferCount, flags, context);
    }
#endif
    return stream->Send(buffers, bufferCount, flags, context);
}
}

struct TqDarwinRelayWorker::CompletionState {
    mutable std::mutex Mutex;
    std::unordered_map<TqDarwinRelaySendOperation*, KnownSendOperationInfo> KnownSendOperations;
    std::atomic<uint64_t> KnownSendOperationCount{0};
};

struct TqDarwinRelayWorker::StreamBinding : public std::enable_shared_from_this<TqDarwinRelayWorker::StreamBinding> {
    std::atomic<TqDarwinRelayWorker*> Worker{nullptr};
    std::atomic<MsQuicStream*> RetiredStream{nullptr};
    std::atomic<bool> Active{true};
    std::atomic<uint32_t> CallbackRefs{0};
    std::atomic<uint64_t> CallbackPendingReceiveBytes{0};
    std::atomic<uint64_t> CallbackPendingReceiveEvents{0};
    std::shared_ptr<CompletionState> Completions;
    uint64_t RelayId{0};
    std::weak_ptr<RelayState> Relay;
};

struct TqDarwinRelayPendingTcpWrite {
    TqBufferView View;
    std::shared_ptr<TqDarwinPendingQuicReceive> Receive;
    uint64_t ReceiveBytesRemaining{0};
};

struct TqDarwinRelayWorker::RelayState {
    mutable std::mutex Mutex;
    uint64_t Id{0};
    TqSocketHandle TcpFd{TqInvalidSocket};
    MsQuicStream* Stream{nullptr};
    TqRelayHandle* PublicHandle{nullptr};
    ITqCompressor* Compressor{nullptr};
    ITqDecompressor* Decompressor{nullptr};
    TqCompressAlgo CompressAlgo{TqCompressAlgo::None};
    bool EnableQuicSends{true};
    std::shared_ptr<StreamBinding> Binding;
    bool TcpReadArmed{false};
    bool TcpWriteArmed{false};
    bool TcpReadPausedByQuicBacklog{false};
    bool TcpReadClosed{false};
    bool TcpWriteClosed{false};
    bool QuicSendClosed{false};
    bool QuicReceiveClosed{false};
    bool QuicSendFinSubmitted{false};
    bool QuicSendFinCompleted{false};
    bool Closing{false};
    uint64_t SubmittingQuicSends{0};
    TqRelayBufferBudget TcpReadBuffers;
    uint64_t InFlightQuicSends{0};
    uint64_t InFlightQuicSendBytes{0};
    uint64_t TcpReadBytes{0};
    std::deque<std::shared_ptr<TqDarwinPendingQuicReceive>> PendingQuicReceives;
    uint64_t PendingQuicReceiveBytes{0};
    bool QuicReceivePaused{false};
    std::deque<std::shared_ptr<TqDarwinRelayPendingTcpWrite>> PendingTcpWrites;
    uint64_t PendingTcpWriteBytes{0};
    bool TcpWriteShutdownQueued{false};
    uint64_t TcpWriteBytes{0};
    bool CompressedReceiveRejected{false};
    std::deque<std::unique_ptr<TqDarwinRelaySendOperation>> PendingQuicSends;
    std::vector<uint8_t> CompressionOutput;
    std::vector<uint8_t> DecompressionOutput;

    RelayState(const TqDarwinRelayRegistration& registration, const TqDarwinRelayWorkerConfig& config)
        : TcpFd(registration.TcpFd),
          Stream(registration.Stream),
          PublicHandle(registration.Handle),
          Compressor(registration.Compressor),
          Decompressor(registration.Decompressor),
          CompressAlgo(registration.CompressAlgo),
          EnableQuicSends(registration.EnableQuicSends) {
        TcpReadBuffers.MaxPendingBufferBytes = config.MaxBufferedQuicSendBytes != 0
            ? config.MaxBufferedQuicSendBytes
            : config.ReadBatchBytes * 2;
    }
};

TqDarwinRelayWorker::TqDarwinRelayWorker(const TqDarwinRelayWorkerConfig& config)
    : Config(config), EventQueue(config.EventQueueCapacity) {}

TqDarwinRelayWorker::~TqDarwinRelayWorker() {
    Stop();
    DetachRetiredBindingsForDestruction();
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

    {
        std::lock_guard<std::mutex> lock(WorkerThreadIdMutex);
        WorkerThreadId = std::thread::id{};
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
            PurgeQueuedEventsForStop();
            if (!WaitForKnownOperationsToDrain()) {
                Errors.fetch_add(1, std::memory_order_relaxed);
            }
            PurgeRetiredRelaysIfSafe();
            {
                std::lock_guard<std::mutex> lock(WorkerThreadIdMutex);
                WorkerThreadId = std::thread::id{};
            }
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
                RetireRelay(entry.second);
            }
            close(KqueueFd);
            KqueueFd = -1;
        }
        PurgeQueuedEventsForStop();
    }

    if (!WaitForKnownOperationsToDrain()) {
        Errors.fetch_add(1, std::memory_order_relaxed);
    }
    PurgeRetiredRelaysIfSafe();
    {
        std::lock_guard<std::mutex> lock(WorkerThreadIdMutex);
        WorkerThreadId = std::thread::id{};
    }
}

bool TqDarwinRelayWorker::Wake() const {
    if (KqueueFd < 0) {
        return false;
    }

    struct kevent change;
    EV_SET(&change, kWakeIdent, EVFILT_USER, 0, NOTE_TRIGGER, 0, nullptr);
#if defined(TCPQUIC_TESTING)
    uint32_t wakeFailures = WakeFailuresForTest.load(std::memory_order_acquire);
    while (wakeFailures != 0) {
        if (WakeFailuresForTest.compare_exchange_weak(
                wakeFailures,
                wakeFailures - 1,
                std::memory_order_acq_rel,
                std::memory_order_acquire)) {
            errno = EIO;
            Errors.fetch_add(1, std::memory_order_relaxed);
            return false;
        }
    }
#endif
    for (;;) {
        if (kevent(KqueueFd, &change, 1, nullptr, 0, nullptr) == 0) {
            break;
        }
        if (errno == EINTR) {
            continue;
        }
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

TqDarwinRelayWorker::ControlEnqueueResult TqDarwinRelayWorker::EnqueueControlEvent(
    TqDarwinRelayEvent&& event) const {
    if (!EventQueue.TryPush(std::move(event))) {
        Errors.fetch_add(1, std::memory_order_relaxed);
        return ControlEnqueueResult::Failed;
    }
    return Wake() ? ControlEnqueueResult::QueuedAndWoken : ControlEnqueueResult::QueuedWakeFailed;
}

bool TqDarwinRelayWorker::IsWorkerThread() const {
    std::lock_guard<std::mutex> lock(WorkerThreadIdMutex);
    return WorkerThreadId == std::this_thread::get_id();
}

#if defined(TCPQUIC_TESTING)
bool TqDarwinRelayWorker::StartForTest() {
    if (KqueueFd < 0) {
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
    }
    Running.store(true, std::memory_order_release);
    {
        std::lock_guard<std::mutex> lock(WorkerThreadIdMutex);
        WorkerThreadId = std::this_thread::get_id();
    }
    return true;
}

bool TqDarwinRelayWorker::EnqueueForTest(TqDarwinRelayEvent event) {
    return EnqueueEvent(std::move(event));
}

bool TqDarwinRelayWorker::DrainOneEventForTest() {
    return DrainEvents(1) == 1;
}

uint32_t TqDarwinRelayWorker::DrainWakeForTest() {
    return DrainWakeEvents();
}

bool TqDarwinRelayWorker::RunningForTest() const {
    return Running.load(std::memory_order_acquire);
}

uint64_t TqDarwinRelayWorker::FindRelayLockedCountForTest() const {
    return FindRelayLockedCount.load(std::memory_order_relaxed);
}

uint64_t TqDarwinRelayWorker::FindRelayLocalCountForTest() const {
    return FindRelayLocalCount.load(std::memory_order_relaxed);
}

void TqDarwinRelayWorker::SetRegisterTcpFiltersFailureForTest(bool fail) {
    std::lock_guard<std::mutex> lifecycleLock(LifecycleMutex);
    FailRegisterTcpFiltersForTest = fail;
}

void TqDarwinRelayWorker::SetWakeFailuresForTest(uint32_t failures) {
    WakeFailuresForTest.store(failures, std::memory_order_release);
}

void TqDarwinRelayWorker::SetStreamSendForTest(TqDarwinRelayStreamSendForTest sendFn) {
    g_darwinRelayStreamSendForTest = sendFn;
}

void TqDarwinRelayWorker::SetReceiveCompleteForTest(TqDarwinRelayReceiveCompleteForTest completeFn) {
    g_darwinRelayReceiveCompleteForTest = completeFn;
}

void TqDarwinRelayWorker::SetReceiveSetEnabledForTest(TqDarwinRelayReceiveSetEnabledForTest setEnabledFn) {
    g_darwinRelayReceiveSetEnabledForTest = setEnabledFn;
}

void TqDarwinRelayWorker::SetSendMsgForTest(TqDarwinRelaySendMsgForTest sendMsgFn) {
    g_darwinRelaySendMsgForTest = sendMsgFn;
}

bool TqDarwinRelayWorker::FlushTcpWritableForTest(uint64_t relayId) {
    auto relay = FindRelay(relayId);
    if (relay == nullptr) {
        return false;
    }
    return FlushTcpWrites(relay);
}

bool TqDarwinRelayWorker::InvokeTcpEventForTest(uint64_t relayId, int16_t filter, uint16_t flags, intptr_t data) {
    ProcessTcpEvents(relayId, filter, flags, data);
    return true;
}

bool TqDarwinRelayWorker::InvokeQuicReceiveViewForTest(
    const std::shared_ptr<TqDarwinPendingQuicReceive>& receive) {
    if (receive == nullptr) {
        return false;
    }
    ProcessQuicReceiveViewEvent(receive);
    return true;
}

void* TqDarwinRelayWorker::StreamCallbackContextForTest(uint64_t relayId) {
    auto relay = FindRelay(relayId);
    if (relay == nullptr) {
        return nullptr;
    }
    std::lock_guard<std::mutex> relayLock(relay->Mutex);
    return relay->Binding.get();
}

std::shared_ptr<void> TqDarwinRelayWorker::StreamCallbackContextOwnerForTest(uint64_t relayId) {
    auto relay = FindRelay(relayId);
    if (relay == nullptr) {
        return nullptr;
    }
    std::lock_guard<std::mutex> relayLock(relay->Mutex);
    return relay->Binding;
}

uint64_t TqDarwinRelayWorker::KnownSendOperationCountForTest() {
    std::lock_guard<std::mutex> lock(KnownSendMutex);
    return KnownSendOperations.size();
}

uint64_t TqDarwinRelayWorker::PendingQuicSendCountForTest(uint64_t relayId) {
    auto relay = FindRelay(relayId);
    if (relay == nullptr) {
        return 0;
    }
    std::lock_guard<std::mutex> relayLock(relay->Mutex);
    return relay->PendingQuicSends.size();
}

uint64_t TqDarwinRelayWorker::InFlightQuicSendCountForTest(uint64_t relayId) {
    auto relay = FindRelay(relayId);
    if (relay == nullptr) {
        return 0;
    }
    std::lock_guard<std::mutex> relayLock(relay->Mutex);
    return relay->InFlightQuicSends;
}

uint64_t TqDarwinRelayWorker::CompleteOneInFlightSendForTest(uint64_t relayId) {
    TqDarwinRelaySendOperation* operation = nullptr;
    {
        std::lock_guard<std::mutex> lock(KnownSendMutex);
        for (const auto& entry : KnownSendOperations) {
            if (entry.first != nullptr && entry.second.RelayId == relayId) {
                operation = entry.first;
                break;
            }
        }
    }
    if (operation == nullptr) {
        return 0;
    }
    uint64_t bytes = 0;
    {
        std::lock_guard<std::mutex> lock(KnownSendMutex);
        const auto it = KnownSendOperations.find(operation);
        if (it != KnownSendOperations.end()) {
            bytes = it->second.TotalBytes;
        }
    }
    CompleteQuicSend(operation);
    return bytes;
}

bool TqDarwinRelayWorker::CorruptOneInFlightSendMagicForTest(uint64_t relayId) {
    std::lock_guard<std::mutex> lock(KnownSendMutex);
    for (const auto& entry : KnownSendOperations) {
        if (entry.first != nullptr && entry.second.RelayId == relayId) {
            entry.first->Magic = 0;
            return true;
        }
    }
    return false;
}

uint64_t TqDarwinRelayWorker::PendingQuicReceiveBytesForTest(uint64_t relayId) {
    auto relay = FindRelay(relayId);
    if (relay == nullptr) {
        return 0;
    }
    std::lock_guard<std::mutex> relayLock(relay->Mutex);
    return relay->PendingQuicReceiveBytes;
}

uint64_t TqDarwinRelayWorker::PendingTcpWriteBytesForTest(uint64_t relayId) {
    auto relay = FindRelay(relayId);
    if (relay == nullptr) {
        return 0;
    }
    std::lock_guard<std::mutex> relayLock(relay->Mutex);
    return relay->PendingTcpWriteBytes;
}
#endif

void TqDarwinRelayWorker::CompleteRegisterCommand(
    RegisterRelayCommand* command,
    TqDarwinRelayRegistrationResult result) {
    if (command == nullptr) {
        return;
    }
    {
        std::lock_guard<std::mutex> lock(command->Mutex);
        command->Result = result;
        command->Done = true;
    }
    command->Cv.notify_one();
}

void TqDarwinRelayWorker::CompleteUnregisterCommand(UnregisterRelayCommand* command) {
    if (command == nullptr) {
        return;
    }
    {
        std::lock_guard<std::mutex> lock(command->Mutex);
        command->Done = true;
    }
    command->Cv.notify_one();
}

void TqDarwinRelayWorker::CompleteSnapshotCommand(
    SnapshotCommand* command,
    TqDarwinRelayWorkerSnapshot result) {
    if (command == nullptr) {
        return;
    }
    {
        std::lock_guard<std::mutex> lock(command->Mutex);
        command->Result = result;
        command->Done = true;
    }
    command->Cv.notify_one();
}

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
        case TqDarwinRelayEventType::QuicReceiveView:
            ProcessQuicReceiveViewEvent(event.ReceiveView);
            break;
        case TqDarwinRelayEventType::QuicSendComplete:
            CompleteQuicSend(reinterpret_cast<TqDarwinRelaySendOperation*>(static_cast<uintptr_t>(event.Value)));
            break;
        case TqDarwinRelayEventType::QuicPeerSendAborted:
        case TqDarwinRelayEventType::QuicPeerReceiveAborted:
        case TqDarwinRelayEventType::QuicShutdownComplete:
            if (auto relay = FindRelay(event.RelayId)) {
                CloseRelay(relay);
            }
            break;
        case TqDarwinRelayEventType::QuicIdealSendBuffer:
            if (auto relay = FindRelay(event.RelayId)) {
                RetryPendingQuicSends(relay);
            }
            break;
        case TqDarwinRelayEventType::RegisterRelay: {
            auto* command = static_cast<RegisterRelayCommand*>(event.Control);
            if (command == nullptr) {
                Errors.fetch_add(1, std::memory_order_relaxed);
                break;
            }
            CompleteRegisterCommand(command, RegisterRelayWithIdLocal(command->Registration));
            break;
        }
        case TqDarwinRelayEventType::UnregisterRelay: {
            auto* command = static_cast<UnregisterRelayCommand*>(event.Control);
            if (command == nullptr) {
                Errors.fetch_add(1, std::memory_order_relaxed);
                break;
            }
            UnregisterRelayLocal(command->RelayId);
            CompleteUnregisterCommand(command);
            break;
        }
        case TqDarwinRelayEventType::Snapshot: {
            auto* command = static_cast<SnapshotCommand*>(event.Control);
            if (command == nullptr) {
                Errors.fetch_add(1, std::memory_order_relaxed);
                break;
            }
            CompleteSnapshotCommand(command, SnapshotLocal());
            break;
        }
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

void TqDarwinRelayWorker::PurgeQueuedEventsForStop() {
    uint32_t processed = 0;
    TqDarwinRelayEvent event{};
    while (EventQueue.TryPop(event)) {
        ++processed;
        switch (event.Type) {
        case TqDarwinRelayEventType::QuicReceiveView:
            ReleaseCallbackReceiveBudget(event.ReceiveView);
            (void)DiscardDeferredQuicReceive(nullptr, event.ReceiveView);
            break;
        case TqDarwinRelayEventType::QuicSendComplete:
            CompleteQuicSend(reinterpret_cast<TqDarwinRelaySendOperation*>(static_cast<uintptr_t>(event.Value)));
            break;
        case TqDarwinRelayEventType::QuicPeerSendAborted:
        case TqDarwinRelayEventType::QuicPeerReceiveAborted:
        case TqDarwinRelayEventType::QuicShutdownComplete:
            if (auto relay = FindRelay(event.RelayId)) {
                CloseRelay(relay);
            }
            break;
        case TqDarwinRelayEventType::QuicIdealSendBuffer:
            if (auto relay = FindRelay(event.RelayId)) {
                RetryPendingQuicSends(relay);
            }
            break;
        case TqDarwinRelayEventType::RegisterRelay:
            CompleteRegisterCommand(static_cast<RegisterRelayCommand*>(event.Control), {});
            break;
        case TqDarwinRelayEventType::UnregisterRelay:
            CompleteUnregisterCommand(static_cast<UnregisterRelayCommand*>(event.Control));
            break;
        case TqDarwinRelayEventType::Snapshot:
            CompleteSnapshotCommand(static_cast<SnapshotCommand*>(event.Control), SnapshotLocal());
            break;
        default:
            break;
        }
        event = TqDarwinRelayEvent{};
    }
    if (processed > 0) {
        EventsProcessed.fetch_add(processed, std::memory_order_relaxed);
    }
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
    {
        std::lock_guard<std::mutex> lock(WorkerThreadIdMutex);
        WorkerThreadId = std::this_thread::get_id();
    }
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

    bool readArmed = false;
    bool writeArmed = false;
    {
        std::lock_guard<std::mutex> relayLock(relay->Mutex);
        readArmed = relay->TcpReadArmed;
        writeArmed = relay->TcpWriteArmed;
    }

    struct kevent changes[2];
    EV_SET(
        &changes[0],
        static_cast<uintptr_t>(relay->TcpFd),
        EVFILT_READ,
        readArmed ? EV_ENABLE : EV_DISABLE,
        0,
        0,
        reinterpret_cast<void*>(relay->Id));
    EV_SET(
        &changes[1],
        static_cast<uintptr_t>(relay->TcpFd),
        EVFILT_WRITE,
        writeArmed ? EV_ENABLE : EV_DISABLE,
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

void TqDarwinRelayWorker::RetireRelay(
    const std::shared_ptr<RelayState>& relay,
    uint32_t retainedCallbackRefs) {
    if (relay == nullptr) {
        return;
    }
    std::shared_ptr<StreamBinding> binding;
    std::deque<std::shared_ptr<TqDarwinPendingQuicReceive>> receivesToDiscard;
    {
        std::unique_lock<std::mutex> relayLock(relay->Mutex);
        relay->Closing = true;
        relay->TcpReadClosed = true;
        relay->TcpWriteClosed = true;
        relay->QuicReceiveClosed = true;
        relay->TcpReadArmed = false;
        relay->TcpWriteArmed = false;
        if (TqSocketValid(relay->TcpFd)) {
            TqCloseSocket(relay->TcpFd);
            relay->TcpFd = TqInvalidSocket;
        }
        relay->PendingQuicSends.clear();
        static constexpr uint32_t kMaxSubmittingYields = 100000;
        uint32_t submittingYields = 0;
        while (relay->SubmittingQuicSends != 0 && submittingYields < kMaxSubmittingYields) {
            ++submittingYields;
            relayLock.unlock();
            std::this_thread::yield();
            relayLock.lock();
        }
        if (relay->SubmittingQuicSends != 0) {
            Errors.fetch_add(1, std::memory_order_relaxed);
        }
        binding = relay->Binding;
        relay->PendingQuicSends.clear();
        receivesToDiscard.swap(relay->PendingQuicReceives);
        relay->PendingQuicReceiveBytes = 0;
        relay->PendingTcpWrites.clear();
        relay->PendingTcpWriteBytes = 0;
        relay->Binding.reset();
        const auto completions = binding != nullptr ? binding->Completions : nullptr;
        const bool hasKnownOperations = completions != nullptr &&
            completions->KnownSendOperationCount.load(std::memory_order_acquire) != 0;
        if (relay->Stream != nullptr && binding != nullptr &&
#if defined(TCPQUIC_TESTING)
            TqDarwinRelayStreamUsableForTest(relay->Stream) &&
#endif
            relay->Stream->Context == binding.get()) {
            if (hasKnownOperations) {
                binding->RetiredStream.store(relay->Stream, std::memory_order_release);
            } else {
                relay->Stream->Callback = MsQuicStream::NoOpCallback;
                relay->Stream->Context = nullptr;
            }
        }
        relay->Stream = nullptr;
    }
    for (const auto& receive : receivesToDiscard) {
        (void)DiscardDeferredQuicReceive(nullptr, receive);
    }
    if (binding != nullptr) {
        binding->Active.store(false, std::memory_order_release);
        static constexpr uint32_t kMaxCallbackRefYields = 100000;
        uint32_t callbackRefYields = 0;
        while (binding->CallbackRefs.load(std::memory_order_acquire) > retainedCallbackRefs &&
               callbackRefYields < kMaxCallbackRefYields) {
            ++callbackRefYields;
            std::this_thread::yield();
        }
        if (binding->CallbackRefs.load(std::memory_order_acquire) > retainedCallbackRefs) {
            Errors.fetch_add(1, std::memory_order_relaxed);
        }
        binding->Relay.reset();
        const auto completions = binding->Completions;
        if (completions == nullptr || completions->KnownSendOperationCount.load(std::memory_order_acquire) == 0) {
            binding->Worker.store(nullptr, std::memory_order_release);
        }
    }
    ClearPublicHandle(relay);
    std::lock_guard<std::mutex> lock(RelayMutex);
    uint64_t inFlight = 0;
    {
        std::lock_guard<std::mutex> relayLock(relay->Mutex);
        inFlight = relay->InFlightQuicSends;
    }
    if (inFlight != 0) {
        RetiredRelays.push_back(relay);
    }
    if (binding != nullptr) {
        RetiredStreamBindings.push_back(std::move(binding));
    }
}

void TqDarwinRelayWorker::CloseRelay(
    const std::shared_ptr<RelayState>& relay,
    uint32_t retainedCallbackRefs) {
    if (relay == nullptr) {
        return;
    }

    {
        std::lock_guard<std::mutex> lock(RelayMutex);
        const auto it = Relays.find(relay->Id);
        if (it == Relays.end() || it->second != relay) {
            return;
        }
        Relays.erase(it);
    }
    RemoveTcpFilters(relay);
    RetireRelay(relay, retainedCallbackRefs);
    PurgeRetiredRelaysIfSafe();
}

void TqDarwinRelayWorker::PurgeRetiredRelaysIfSafe() {
    std::lock_guard<std::mutex> lock(RelayMutex);
    for (auto it = RetiredRelays.begin(); it != RetiredRelays.end();) {
        uint64_t sends = 0;
        {
            std::lock_guard<std::mutex> relayLock((*it)->Mutex);
            sends = (*it)->InFlightQuicSends;
        }
        if (sends == 0) {
            it = RetiredRelays.erase(it);
        } else {
            ++it;
        }
    }
    for (auto it = RetiredStreamBindings.begin(); it != RetiredStreamBindings.end();) {
        const auto completions = (*it)->Completions;
        const bool hasKnownOperations = completions != nullptr &&
            completions->KnownSendOperationCount.load(std::memory_order_acquire) != 0;
        const bool hasCallbackReceives =
            (*it)->CallbackPendingReceiveEvents.load(std::memory_order_acquire) != 0;
        if ((*it)->CallbackRefs.load(std::memory_order_acquire) == 0 && !hasKnownOperations &&
            !hasCallbackReceives) {
            ClearRetiredStreamCallbackIfSafe(it->get());
            (*it)->Worker.store(nullptr, std::memory_order_release);
            it = RetiredStreamBindings.erase(it);
        } else {
            ++it;
        }
    }
}

bool TqDarwinRelayWorker::WaitForKnownOperationsToDrain() {
    static constexpr uint32_t kMaxDrainYields = 100000;
    for (uint32_t attempt = 0; attempt < kMaxDrainYields; ++attempt) {
        bool knownOperationsDrained = false;
        {
            std::lock_guard<std::mutex> lock(KnownSendMutex);
            knownOperationsDrained = KnownSendOperations.empty();
        }
        bool drained = knownOperationsDrained;
        if (drained) {
            {
                std::lock_guard<std::mutex> lock(RelayMutex);
                for (const auto& binding : RetiredStreamBindings) {
                    const auto completions = binding->Completions;
                    if (completions != nullptr &&
                        completions->KnownSendOperationCount.load(std::memory_order_acquire) != 0) {
                        drained = false;
                        break;
                    }
                    if (binding->CallbackPendingReceiveEvents.load(std::memory_order_acquire) != 0) {
                        drained = false;
                        break;
                    }
                }
            }
        }
        if (drained) {
            return true;
        }
        std::this_thread::yield();
    }
    {
        std::lock_guard<std::mutex> lock(KnownSendMutex);
        if (!KnownSendOperations.empty()) {
            return false;
        }
    }
    std::lock_guard<std::mutex> lock(RelayMutex);
    for (const auto& binding : RetiredStreamBindings) {
        const auto completions = binding->Completions;
        if (completions != nullptr &&
            completions->KnownSendOperationCount.load(std::memory_order_acquire) != 0) {
            return false;
        }
        if (binding->CallbackPendingReceiveEvents.load(std::memory_order_acquire) != 0) {
            return false;
        }
    }
    return true;
}

void TqDarwinRelayWorker::DetachRetiredBindingsForDestruction() {
    const bool drained = WaitForKnownOperationsToDrain();
    std::lock_guard<std::mutex> lock(RelayMutex);
    for (const auto& binding : RetiredStreamBindings) {
        binding->Active.store(false, std::memory_order_release);
        if (drained) {
            ClearRetiredStreamCallbackIfSafe(binding.get());
        }
        binding->Worker.store(nullptr, std::memory_order_release);
    }
    if (drained) {
        RetiredStreamBindings.clear();
    }
}

void TqDarwinRelayWorker::RegisterKnownSendOperation(
    TqDarwinRelaySendOperation* operation,
    const KnownSendOperationInfo& info) {
    auto binding = std::static_pointer_cast<StreamBinding>(info.BindingOwner);
    {
        std::lock_guard<std::mutex> lock(KnownSendMutex);
        KnownSendOperations.emplace(operation, info);
    }
    if (binding != nullptr && binding->Completions != nullptr) {
        std::lock_guard<std::mutex> completionLock(binding->Completions->Mutex);
        binding->Completions->KnownSendOperations.emplace(operation, info);
        binding->Completions->KnownSendOperationCount.fetch_add(1, std::memory_order_acq_rel);
    }
}

void TqDarwinRelayWorker::RegisterKnownSendOperationLocal(
    TqDarwinRelaySendOperation* operation,
    const KnownSendOperationInfo& info) {
    auto binding = std::static_pointer_cast<StreamBinding>(info.BindingOwner);
    {
        std::lock_guard<std::mutex> lock(KnownSendMutex);
        KnownSendOperations.emplace(operation, info);
    }
    if (binding != nullptr && binding->Completions != nullptr) {
        std::lock_guard<std::mutex> completionLock(binding->Completions->Mutex);
        binding->Completions->KnownSendOperations.emplace(operation, info);
        binding->Completions->KnownSendOperationCount.fetch_add(1, std::memory_order_acq_rel);
    }
}

bool TqDarwinRelayWorker::MarkKnownSendOperationSubmitted(
    TqDarwinRelaySendOperation* operation,
    KnownSendOperationInfo* info) {
    std::shared_ptr<CompletionState> completions;
    {
        std::lock_guard<std::mutex> lock(KnownSendMutex);
        const auto it = KnownSendOperations.find(operation);
        if (it == KnownSendOperations.end()) {
            return false;
        }
        it->second.Submitting = false;
        if (info != nullptr) {
            *info = it->second;
        }
        if (auto binding = std::static_pointer_cast<StreamBinding>(it->second.BindingOwner)) {
            completions = binding->Completions;
        }
    }
    if (completions != nullptr) {
        std::lock_guard<std::mutex> completionLock(completions->Mutex);
        const auto it = completions->KnownSendOperations.find(operation);
        if (it != completions->KnownSendOperations.end()) {
            it->second.Submitting = false;
            if (info != nullptr) {
                *info = it->second;
            }
        }
    }
    return true;
}

bool TqDarwinRelayWorker::MarkKnownSendOperationSubmittedLocal(
    TqDarwinRelaySendOperation* operation,
    KnownSendOperationInfo* info) {
    std::shared_ptr<CompletionState> completions;
    {
        std::lock_guard<std::mutex> lock(KnownSendMutex);
        const auto it = KnownSendOperations.find(operation);
        if (it == KnownSendOperations.end()) {
            return false;
        }
        it->second.Submitting = false;
        if (info != nullptr) {
            *info = it->second;
        }
        if (auto binding = std::static_pointer_cast<StreamBinding>(it->second.BindingOwner)) {
            completions = binding->Completions;
        }
    }
    if (completions != nullptr) {
        std::lock_guard<std::mutex> completionLock(completions->Mutex);
        const auto it = completions->KnownSendOperations.find(operation);
        if (it != completions->KnownSendOperations.end()) {
            it->second.Submitting = false;
            if (info != nullptr) {
                *info = it->second;
            }
        }
    }
    return true;
}

bool TqDarwinRelayWorker::TryClaimKnownSendCompletionEvent(
    StreamBinding* binding,
    TqDarwinRelaySendOperation* operation,
    KnownSendOperationInfo* info) {
    if (binding == nullptr || operation == nullptr || binding->Completions == nullptr) {
        return false;
    }
    std::lock_guard<std::mutex> completionLock(binding->Completions->Mutex);
    const auto it = binding->Completions->KnownSendOperations.find(operation);
    if (it == binding->Completions->KnownSendOperations.end()) {
        return false;
    }
    if (info != nullptr) {
        *info = it->second;
    }
    it->second.CompletionEventClaimed = true;
    return true;
}

bool TqDarwinRelayWorker::UnregisterCompletionStateOperation(
    const std::shared_ptr<CompletionState>& state,
    TqDarwinRelaySendOperation* operation,
    KnownSendOperationInfo* info) {
    if (state == nullptr) {
        return false;
    }
    std::lock_guard<std::mutex> completionLock(state->Mutex);
    const auto it = state->KnownSendOperations.find(operation);
    if (it == state->KnownSendOperations.end()) {
        return false;
    }
    KnownSendOperationInfo localInfo = it->second;
    if (info != nullptr) {
        *info = localInfo;
    }
    state->KnownSendOperations.erase(it);
    state->KnownSendOperationCount.fetch_sub(1, std::memory_order_acq_rel);
    return true;
}

bool TqDarwinRelayWorker::UnregisterKnownSendOperation(
    TqDarwinRelaySendOperation* operation,
    KnownSendOperationInfo* info) {
    KnownSendOperationInfo localInfo{};
    bool removedFromWorker = false;
    {
        std::lock_guard<std::mutex> lock(KnownSendMutex);
        const auto it = KnownSendOperations.find(operation);
        if (it != KnownSendOperations.end()) {
            localInfo = it->second;
            KnownSendOperations.erase(it);
            removedFromWorker = true;
        }
    }
    if (!removedFromWorker) {
        return false;
    }
    if (info != nullptr) {
        *info = localInfo;
    }
    if (auto binding = std::static_pointer_cast<StreamBinding>(localInfo.BindingOwner)) {
        (void)UnregisterCompletionStateOperation(binding->Completions, operation, nullptr);
        ClearRetiredStreamCallbackIfSafe(binding.get());
    }
    return true;
}

bool TqDarwinRelayWorker::UnregisterKnownSendOperationLocal(
    TqDarwinRelaySendOperation* operation,
    KnownSendOperationInfo* info) {
    KnownSendOperationInfo localInfo{};
    bool removedFromWorker = false;
    {
        std::lock_guard<std::mutex> lock(KnownSendMutex);
        const auto it = KnownSendOperations.find(operation);
        if (it != KnownSendOperations.end()) {
            localInfo = it->second;
            KnownSendOperations.erase(it);
            removedFromWorker = true;
        }
    }
    if (!removedFromWorker) {
        return false;
    }
    if (info != nullptr) {
        *info = localInfo;
    }
    if (auto binding = std::static_pointer_cast<StreamBinding>(localInfo.BindingOwner)) {
        (void)UnregisterCompletionStateOperation(binding->Completions, operation, nullptr);
        ClearRetiredStreamCallbackIfSafe(binding.get());
    }
    return true;
}

void TqDarwinRelayWorker::ClearRetiredStreamCallbackIfSafe(StreamBinding* binding) {
    if (binding == nullptr) {
        return;
    }
    const auto completions = binding->Completions;
    if (completions != nullptr &&
        completions->KnownSendOperationCount.load(std::memory_order_acquire) != 0) {
        return;
    }
    MsQuicStream* stream = binding->RetiredStream.load(std::memory_order_acquire);
    if (stream == nullptr) {
        return;
    }
#if defined(TCPQUIC_TESTING)
    if (!TqDarwinRelayStreamUsableForTest(stream)) {
        return;
    }
#endif
    if (stream->Context == binding) {
        stream->Callback = MsQuicStream::NoOpCallback;
        stream->Context = nullptr;
        binding->RetiredStream.store(nullptr, std::memory_order_release);
    }
}

bool TqDarwinRelayWorker::CompleteDetachedQuicSend(StreamBinding* binding, TqDarwinRelaySendOperation* operation) {
    if (binding == nullptr || operation == nullptr) {
        return false;
    }
    KnownSendOperationInfo info{};
    if (!UnregisterCompletionStateOperation(binding->Completions, operation, &info)) {
        return false;
    }
    ClearRetiredStreamCallbackIfSafe(binding);
    delete operation;
    return true;
}

std::shared_ptr<TqDarwinRelayWorker::RelayState> TqDarwinRelayWorker::FindRelay(uint64_t relayId) {
#if defined(TCPQUIC_TESTING)
    FindRelayLockedCount.fetch_add(1, std::memory_order_relaxed);
#endif
    std::lock_guard<std::mutex> lock(RelayMutex);
    return FindRelayLocal(relayId);
}

std::shared_ptr<TqDarwinRelayWorker::RelayState> TqDarwinRelayWorker::FindRelayLocal(uint64_t relayId) const {
#if defined(TCPQUIC_TESTING)
    FindRelayLocalCount.fetch_add(1, std::memory_order_relaxed);
#endif
    const auto it = Relays.find(relayId);
    if (it != Relays.end()) {
        return it->second;
    }
    return nullptr;
}

std::shared_ptr<TqDarwinRelayWorker::RelayState> TqDarwinRelayWorker::FindRetiredRelay(uint64_t relayId) {
    std::lock_guard<std::mutex> lock(RelayMutex);
    return FindRetiredRelayLocal(relayId);
}

std::shared_ptr<TqDarwinRelayWorker::RelayState> TqDarwinRelayWorker::FindRetiredRelayLocal(uint64_t relayId) const {
    for (const auto& relay : RetiredRelays) {
        if (relay->Id == relayId) {
            return relay;
        }
    }
    return nullptr;
}

void TqDarwinRelayWorker::AssertWorkerThreadForRelayState() const {
#if defined(TCPQUIC_TESTING) || !defined(NDEBUG)
    assert(IsWorkerThread() && "RelayState data-plane fields are worker-thread-only");
#endif
}

bool TqDarwinRelayWorker::IsRelayClosingLocal(const RelayState& relay) const {
    AssertWorkerThreadForRelayState();
    return relay.Closing;
}

void TqDarwinRelayWorker::MarkRelayClosingLocal(RelayState& relay) const {
    AssertWorkerThreadForRelayState();
    relay.Closing = true;
}

MsQuicStream* TqDarwinRelayWorker::RelayStreamLocal(const RelayState& relay) const {
    AssertWorkerThreadForRelayState();
    return relay.Stream;
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
        return;
    }

    if ((flags & EV_ERROR) != 0) {
        Errors.fetch_add(1, std::memory_order_relaxed);
        CloseRelay(relay);
        return;
    }

    if (filter == EVFILT_READ) {
        if (!DrainTcpReadable(relay)) {
            CloseRelay(relay);
        }
        return;
    }
    if (filter == EVFILT_WRITE) {
        if (!FlushTcpWrites(relay)) {
            CloseRelay(relay);
        }
        return;
    }
}

bool TqDarwinRelayWorker::DrainTcpReadable(const std::shared_ptr<RelayState>& relay) {
    if (relay == nullptr) {
        return true;
    }
    {
        std::lock_guard<std::mutex> relayLock(relay->Mutex);
        if (relay->Closing || relay->TcpReadClosed) {
            return true;
        }
    }
    if (ShouldPauseTcpReadForQuicBacklog(relay)) {
        return SetTcpReadBackpressure(relay, true);
    }

    uint64_t readBytes = 0;
    const uint64_t batchBudget = Config.ReadBatchBytes == 0
        ? Config.ReadChunkSize
        : Config.ReadBatchBytes;
    const uint64_t byteBudget = Config.ByteBudgetPerTick == 0
        ? batchBudget
        : Config.ByteBudgetPerTick;
    const uint64_t tickBudget = std::max<uint64_t>(1, std::min(batchBudget, byteBudget));
    const size_t configuredChunkSize = std::max<size_t>(1, Config.ReadChunkSize);
    const size_t maxIov = std::max<size_t>(1, std::min<size_t>(Config.MaxIov == 0 ? 1 : Config.MaxIov, 1024));

    while (readBytes < tickBudget) {
        std::vector<TqBufferRef> buffers;
        std::vector<iovec> iov;
        buffers.reserve(maxIov);
        iov.reserve(maxIov);

        for (size_t i = 0; i < maxIov && readBytes < tickBudget; ++i) {
            const uint64_t remainingBudget = tickBudget - readBytes;
            const size_t readSize = static_cast<size_t>(std::min<uint64_t>(configuredChunkSize, remainingBudget));
            TqBufferAcquireFailure failure = TqBufferAcquireFailure::None;
            auto buffer = TqAllocateRelayBuffer(&relay->TcpReadBuffers, readSize, &failure);
            if (!buffer) {
                if (failure == TqBufferAcquireFailure::PendingBytesLimit) {
                    return SetTcpReadBackpressure(relay, true);
                }
                Errors.fetch_add(1, std::memory_order_relaxed);
                break;
            }
            iovec item{};
            item.iov_base = buffer->Data();
            item.iov_len = buffer->Capacity();
            iov.push_back(item);
            buffers.push_back(std::move(buffer));
        }
        if (iov.empty()) {
            break;
        }

        const ssize_t received = ::readv(relay->TcpFd, iov.data(), static_cast<int>(iov.size()));
        if (received > 0) {
            size_t remaining = static_cast<size_t>(received);
            std::vector<TqBufferView> views;
            views.reserve(buffers.size());
            for (auto& buffer : buffers) {
                if (remaining == 0) {
                    break;
                }
                const size_t length = std::min(buffer->Capacity(), remaining);
                buffer->SetLength(length);
                uint8_t* data = buffer->Data();
                views.push_back(TqBufferView{data, length, std::move(buffer)});
                remaining -= length;
            }

            readBytes += static_cast<uint64_t>(received);
            {
                std::lock_guard<std::mutex> relayLock(relay->Mutex);
                relay->TcpReadBytes += static_cast<uint64_t>(received);
            }
            TcpReadBatches.fetch_add(1, std::memory_order_relaxed);
            TcpReadBytes.fetch_add(static_cast<uint64_t>(received), std::memory_order_relaxed);

            std::vector<TqBufferView> sendViews;
            if (relay->Compressor == nullptr || relay->CompressAlgo == TqCompressAlgo::None) {
                sendViews = std::move(views);
            } else {
                relay->CompressionOutput.clear();
                for (const auto& view : views) {
                    if (!relay->Compressor->Compress(view.Data, view.Len, relay->CompressionOutput, false)) {
                        Errors.fetch_add(1, std::memory_order_relaxed);
                        return false;
                    }
                }
                if (relay->CompressionOutput.empty() &&
                    !relay->Compressor->Flush(relay->CompressionOutput)) {
                    Errors.fetch_add(1, std::memory_order_relaxed);
                    return false;
                }
                size_t offset = 0;
                while (offset < relay->CompressionOutput.size()) {
                    TqBufferAcquireFailure failure = TqBufferAcquireFailure::None;
                    auto buffer = TqAllocateRelayBuffer(&relay->TcpReadBuffers, configuredChunkSize, &failure);
                    if (!buffer) {
                        Errors.fetch_add(1, std::memory_order_relaxed);
                        return false;
                    }
                    const size_t length = std::min(buffer->Capacity(), relay->CompressionOutput.size() - offset);
                    std::memcpy(buffer->Data(), relay->CompressionOutput.data() + offset, length);
                    buffer->SetLength(length);
                    uint8_t* data = buffer->Data();
                    sendViews.push_back(TqBufferView{data, length, std::move(buffer)});
                    offset += length;
                }
                views.clear();
            }

            if (!SubmitTcpBatchToQuic(relay, std::move(sendViews), false)) {
                return false;
            }
            bool shouldPause = false;
            {
                std::lock_guard<std::mutex> relayLock(relay->Mutex);
                shouldPause = Config.MaxInFlightQuicSends != 0 &&
                    relay->InFlightQuicSends >= Config.MaxInFlightQuicSends;
            }
            if (shouldPause) {
                if (!SetTcpReadBackpressure(relay, true)) {
                    return false;
                }
                break;
            }
            if (ShouldPauseTcpReadForQuicBacklog(relay)) {
                if (!SetTcpReadBackpressure(relay, true)) {
                    return false;
                }
                break;
            }
            continue;
        }
        if (received == 0) {
            {
                std::lock_guard<std::mutex> relayLock(relay->Mutex);
                relay->TcpReadClosed = true;
                relay->TcpReadArmed = false;
            }
            if (!UpdateTcpInterest(relay)) {
                Errors.fetch_add(1, std::memory_order_relaxed);
                std::lock_guard<std::mutex> relayLock(relay->Mutex);
                relay->Closing = true;
                return false;
            }
            std::vector<TqBufferView> finViews;
            if (relay->Compressor != nullptr && relay->CompressAlgo != TqCompressAlgo::None) {
                relay->CompressionOutput.clear();
                if (!relay->Compressor->Compress(nullptr, 0, relay->CompressionOutput, true)) {
                    Errors.fetch_add(1, std::memory_order_relaxed);
                    return false;
                }
                size_t offset = 0;
                while (offset < relay->CompressionOutput.size()) {
                    TqBufferAcquireFailure failure = TqBufferAcquireFailure::None;
                    auto buffer = TqAllocateRelayBuffer(&relay->TcpReadBuffers, configuredChunkSize, &failure);
                    if (!buffer) {
                        Errors.fetch_add(1, std::memory_order_relaxed);
                        return false;
                    }
                    const size_t length = std::min(buffer->Capacity(), relay->CompressionOutput.size() - offset);
                    std::memcpy(buffer->Data(), relay->CompressionOutput.data() + offset, length);
                    buffer->SetLength(length);
                    uint8_t* data = buffer->Data();
                    finViews.push_back(TqBufferView{data, length, std::move(buffer)});
                    offset += length;
                }
            }
            return SubmitTcpBatchToQuic(relay, std::move(finViews), true);
        }
        if (errno == EINTR) {
            continue;
        }
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            break;
        }
        Errors.fetch_add(1, std::memory_order_relaxed);
        {
            std::lock_guard<std::mutex> relayLock(relay->Mutex);
            relay->Closing = true;
        }
        return false;
    }
    return true;
}

bool TqDarwinRelayWorker::SubmitTcpBatchToQuic(
    const std::shared_ptr<RelayState>& relay,
    std::vector<TqBufferView>&& views,
    bool fin) {
    if (relay == nullptr) {
        return false;
    }
    bool enableQuicSends = false;
    {
        std::lock_guard<std::mutex> relayLock(relay->Mutex);
        if (relay->Closing) {
            return false;
        }
        if (fin) {
            relay->QuicSendFinSubmitted = true;
        }
        enableQuicSends = relay->EnableQuicSends;
    }
    if (views.empty() && !fin) {
        return true;
    }
    if (!enableQuicSends) {
        if (fin) {
            std::lock_guard<std::mutex> relayLock(relay->Mutex);
            relay->QuicSendFinCompleted = true;
            relay->QuicSendClosed = true;
        }
        return true;
    }

    auto operation = std::unique_ptr<TqDarwinRelaySendOperation>(new (std::nothrow) TqDarwinRelaySendOperation{});
    if (!operation) {
        Errors.fetch_add(1, std::memory_order_relaxed);
        return false;
    }
    uint64_t relayId = 0;
    std::shared_ptr<StreamBinding> binding;
    {
        std::lock_guard<std::mutex> relayLock(relay->Mutex);
        relayId = relay->Id;
        binding = relay->Binding;
    }
    operation->RelayId = relayId;
    operation->Fin = fin;
    operation->BindingOwner = binding;
    operation->Views = std::move(views);
    operation->QuicBuffers.reserve(operation->Views.size());
    for (const auto& view : operation->Views) {
        if (view.Len > std::numeric_limits<uint32_t>::max()) {
            Errors.fetch_add(1, std::memory_order_relaxed);
            return false;
        }
        QUIC_BUFFER buffer{};
        buffer.Buffer = view.Data;
        buffer.Length = static_cast<uint32_t>(view.Len);
        operation->QuicBuffers.push_back(buffer);
        operation->TotalBytes += view.Len;
    }

    return TrySubmitQuicSendOperation(relay, std::move(operation));
}

bool TqDarwinRelayWorker::TrySubmitQuicSendOperation(
    const std::shared_ptr<RelayState>& relay,
    std::unique_ptr<TqDarwinRelaySendOperation> operation) {
    if (relay == nullptr || operation == nullptr) {
        Errors.fetch_add(1, std::memory_order_relaxed);
        return false;
    }

    MsQuicStream* stream = nullptr;
    const bool workerThread = IsWorkerThread();
    TqDarwinRelaySendOperation* raw = operation.get();
    KnownSendOperationInfo info{};
    info.RelayId = raw->RelayId;
    info.TotalBytes = raw->TotalBytes;
    info.Fin = raw->Fin;
    info.Submitting = true;
    {
        std::lock_guard<std::mutex> relayLock(relay->Mutex);
        if (relay->Closing || relay->Stream == nullptr) {
            Errors.fetch_add(1, std::memory_order_relaxed);
            return false;
        }
        stream = relay->Stream;
        info.BindingOwner = relay->Binding;
        raw->BindingOwner = relay->Binding;
        ++relay->SubmittingQuicSends;
        ++relay->InFlightQuicSends;
        relay->InFlightQuicSendBytes += raw->TotalBytes;
    }
    if (workerThread) {
        RegisterKnownSendOperationLocal(raw, info);
    } else {
        RegisterKnownSendOperation(raw, info);
    }
    const QUIC_STATUS status = TqDarwinRelayStreamSend(
        stream,
        raw->QuicBuffers.empty() ? nullptr : raw->QuicBuffers.data(),
        static_cast<uint32_t>(raw->QuicBuffers.size()),
        raw->Fin ? QUIC_SEND_FLAG_FIN : QUIC_SEND_FLAG_NONE,
        raw);

    KnownSendOperationInfo submittedInfo{};
    const bool completionAlreadyRan = workerThread
        ? !MarkKnownSendOperationSubmittedLocal(raw, &submittedInfo)
        : !MarkKnownSendOperationSubmitted(raw, &submittedInfo);
    {
        std::lock_guard<std::mutex> relayLock(relay->Mutex);
        if (relay->SubmittingQuicSends > 0) {
            --relay->SubmittingQuicSends;
        }
    }
    if (workerThread && submittedInfo.CompletionEventClaimed) {
        (void)DrainEvents(1);
    }
    if (completionAlreadyRan) {
        (void)operation.release();
        return true;
    }

    if (QUIC_FAILED(status)) {
        if (submittedInfo.CompletionEventClaimed) {
            (void)operation.release();
            return true;
        }
        KnownSendOperationInfo removedInfo{};
        if (workerThread) {
            (void)UnregisterKnownSendOperationLocal(raw, &removedInfo);
        } else {
            (void)UnregisterKnownSendOperation(raw, &removedInfo);
        }
        {
            std::lock_guard<std::mutex> relayLock(relay->Mutex);
            if (relay->InFlightQuicSends > 0) {
                --relay->InFlightQuicSends;
            }
            relay->InFlightQuicSendBytes = relay->InFlightQuicSendBytes >= removedInfo.TotalBytes
                ? relay->InFlightQuicSendBytes - removedInfo.TotalBytes
                : 0;
        }
        if (status == QUIC_STATUS_OUT_OF_MEMORY || status == QUIC_STATUS_BUFFER_TOO_SMALL) {
            QuicSendBackpressureEvents.fetch_add(1, std::memory_order_relaxed);
            if (!SetTcpReadBackpressure(relay, true)) {
                return false;
            }
            std::lock_guard<std::mutex> relayLock(relay->Mutex);
            if (relay->Closing) {
                return false;
            }
            relay->PendingQuicSends.push_back(std::move(operation));
            return true;
        }
        Errors.fetch_add(1, std::memory_order_relaxed);
        std::lock_guard<std::mutex> relayLock(relay->Mutex);
        relay->Closing = true;
        return false;
    }

    (void)operation.release();
    return true;
}

bool TqDarwinRelayWorker::EnqueueQuicSendCompleteFromCallback(
    uint64_t relayId,
    TqDarwinRelaySendOperation* operation) {
    if (!Running.load(std::memory_order_acquire)) {
        return false;
    }
    TqDarwinRelayEvent event{};
    event.Type = TqDarwinRelayEventType::QuicSendComplete;
    event.RelayId = relayId;
    event.Value = static_cast<uint64_t>(reinterpret_cast<uintptr_t>(operation));
    return EnqueueEvent(std::move(event));
}

void TqDarwinRelayWorker::RetryPendingQuicSends(const std::shared_ptr<RelayState>& relay) {
    if (relay == nullptr) {
        return;
    }
    for (;;) {
        std::unique_ptr<TqDarwinRelaySendOperation> operation;
        {
            std::lock_guard<std::mutex> relayLock(relay->Mutex);
            if (relay->Closing || relay->PendingQuicSends.empty()) {
                break;
            }
            operation = std::move(relay->PendingQuicSends.front());
            relay->PendingQuicSends.pop_front();
        }
        if (!TrySubmitQuicSendOperation(relay, std::move(operation))) {
            std::lock_guard<std::mutex> relayLock(relay->Mutex);
            relay->Closing = true;
            return;
        }
        bool hasPending = false;
        {
            std::lock_guard<std::mutex> relayLock(relay->Mutex);
            hasPending = !relay->PendingQuicSends.empty();
        }
        if (hasPending) {
            return;
        }
    }
    if (ShouldResumeTcpReadForQuicBacklog(relay)) {
        (void)SetTcpReadBackpressure(relay, false);
    }
}

void TqDarwinRelayWorker::CompleteQuicSend(TqDarwinRelaySendOperation* operation) {
    if (operation == nullptr) {
        return;
    }
    const bool workerThread = IsWorkerThread();
    KnownSendOperationInfo info{};
    const bool unregistered = workerThread
        ? UnregisterKnownSendOperationLocal(operation, &info)
        : UnregisterKnownSendOperation(operation, &info);
    if (!unregistered) {
        Errors.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    if (operation->Magic != TqDarwinRelaySendOperation::MagicValue) {
        Errors.fetch_add(1, std::memory_order_relaxed);
    }
    auto relay = FindRelay(info.RelayId);
    if (relay == nullptr) {
        relay = FindRetiredRelay(info.RelayId);
    }
    delete operation;
    if (relay != nullptr) {
        bool closing = false;
        {
            std::lock_guard<std::mutex> relayLock(relay->Mutex);
            if (relay->InFlightQuicSends > 0) {
                --relay->InFlightQuicSends;
            }
            relay->InFlightQuicSendBytes = relay->InFlightQuicSendBytes >= info.TotalBytes
                ? relay->InFlightQuicSendBytes - info.TotalBytes
                : 0;
            if (info.Fin) {
                relay->QuicSendFinCompleted = true;
                relay->QuicSendClosed = true;
            }
            closing = relay->Closing;
        }
        if (!closing && !info.Submitting) {
            RetryPendingQuicSends(relay);
            if (ShouldResumeTcpReadForQuicBacklog(relay)) {
                (void)SetTcpReadBackpressure(relay, false);
            }
        }
    }
    PurgeRetiredRelaysIfSafe();
}

bool TqDarwinRelayWorker::ShouldPauseTcpReadForQuicBacklog(const std::shared_ptr<RelayState>& relay) const {
    if (relay == nullptr) {
        return false;
    }
    std::lock_guard<std::mutex> relayLock(relay->Mutex);
    if (Config.MaxInFlightQuicSends != 0 && relay->InFlightQuicSends >= Config.MaxInFlightQuicSends) {
        return true;
    }
    if (Config.MaxBufferedQuicSendBytes != 0 && relay->InFlightQuicSendBytes >= Config.MaxBufferedQuicSendBytes) {
        return true;
    }
    return false;
}

bool TqDarwinRelayWorker::ShouldResumeTcpReadForQuicBacklog(const std::shared_ptr<RelayState>& relay) const {
    if (relay == nullptr) {
        return false;
    }
    std::lock_guard<std::mutex> relayLock(relay->Mutex);
    if (relay->TcpReadClosed) {
        return false;
    }
    const bool sendsBelowLimit = Config.MaxInFlightQuicSends == 0 ||
        relay->InFlightQuicSends < Config.MaxInFlightQuicSends;
    const uint64_t resumeBytes = Config.MaxBufferedQuicSendBytes / 2;
    const bool bytesBelowLimit = Config.MaxBufferedQuicSendBytes == 0 ||
        relay->InFlightQuicSendBytes <= resumeBytes;
    return sendsBelowLimit && bytesBelowLimit;
}

bool TqDarwinRelayWorker::SetTcpReadBackpressure(const std::shared_ptr<RelayState>& relay, bool paused) {
    if (relay == nullptr) {
        return false;
    }
    bool oldPaused = false;
    bool oldArmed = false;
    {
        std::lock_guard<std::mutex> relayLock(relay->Mutex);
        if (relay->TcpReadPausedByQuicBacklog == paused) {
            return true;
        }
        oldPaused = relay->TcpReadPausedByQuicBacklog;
        oldArmed = relay->TcpReadArmed;
        relay->TcpReadPausedByQuicBacklog = paused;
        relay->TcpReadArmed = !paused && !relay->TcpReadClosed;
    }
    if (UpdateTcpInterest(relay)) {
        return true;
    }
    {
        std::lock_guard<std::mutex> relayLock(relay->Mutex);
        relay->TcpReadPausedByQuicBacklog = oldPaused;
        relay->TcpReadArmed = oldArmed;
        relay->Closing = true;
    }
    Errors.fetch_add(1, std::memory_order_relaxed);
    return false;
}

uint64_t TqDarwinRelayWorker::MaxPendingQuicReceiveBytesPerRelay() const {
    if (Config.MaxPendingQuicReceiveBytesPerRelay != 0) {
        return Config.MaxPendingQuicReceiveBytesPerRelay;
    }
    return Config.ReadBatchBytes == 0 ? 1024 * 1024 : Config.ReadBatchBytes;
}

uint64_t TqDarwinRelayWorker::LowPendingQuicReceiveBytesPerRelay() const {
    return MaxPendingQuicReceiveBytesPerRelay() / 2;
}

bool TqDarwinRelayWorker::ReserveCallbackReceiveBudget(StreamBinding* binding, uint64_t bytes) {
    if (binding == nullptr) {
        return false;
    }
    const uint64_t limit = MaxPendingQuicReceiveBytesPerRelay();
    if (bytes > limit) {
        uint64_t expectedEvents = 0;
        if (!binding->CallbackPendingReceiveEvents.compare_exchange_strong(
                expectedEvents,
                1,
                std::memory_order_acq_rel,
                std::memory_order_acquire)) {
            return false;
        }
        uint64_t current = binding->CallbackPendingReceiveBytes.load(std::memory_order_acquire);
        for (;;) {
            if (binding->CallbackPendingReceiveBytes.compare_exchange_weak(
                    current,
                    current + bytes,
                    std::memory_order_acq_rel,
                    std::memory_order_acquire)) {
                return true;
            }
        }
    }

    binding->CallbackPendingReceiveEvents.fetch_add(1, std::memory_order_acq_rel);
    uint64_t current = binding->CallbackPendingReceiveBytes.load(std::memory_order_acquire);
    for (;;) {
        if (current > limit - bytes) {
            ReleaseCallbackReceiveBudget(binding, 0);
            return false;
        }
        if (binding->CallbackPendingReceiveBytes.compare_exchange_weak(
                current,
                current + bytes,
                std::memory_order_acq_rel,
                std::memory_order_acquire)) {
            return true;
        }
    }
}

void TqDarwinRelayWorker::ReleaseCallbackReceiveBudget(StreamBinding* binding, uint64_t bytes) {
    if (binding == nullptr) {
        return;
    }
    uint64_t currentBytes = binding->CallbackPendingReceiveBytes.load(std::memory_order_acquire);
    for (;;) {
        const uint64_t nextBytes = currentBytes >= bytes ? currentBytes - bytes : 0;
        if (binding->CallbackPendingReceiveBytes.compare_exchange_weak(
                currentBytes,
                nextBytes,
                std::memory_order_acq_rel,
                std::memory_order_acquire)) {
            break;
        }
    }

    uint64_t currentEvents = binding->CallbackPendingReceiveEvents.load(std::memory_order_acquire);
    while (currentEvents != 0) {
        if (binding->CallbackPendingReceiveEvents.compare_exchange_weak(
                currentEvents,
                currentEvents - 1,
                std::memory_order_acq_rel,
                std::memory_order_acquire)) {
            break;
        }
    }
}

void TqDarwinRelayWorker::ReleaseCallbackReceiveBudget(
    const std::shared_ptr<TqDarwinPendingQuicReceive>& receive) {
    if (receive == nullptr) {
        return;
    }
    auto binding = std::static_pointer_cast<StreamBinding>(receive->BindingOwner);
    ReleaseCallbackReceiveBudget(binding.get(), receive->TotalLength);
    receive->BindingOwner.reset();
}

bool TqDarwinRelayWorker::QueueDeferredQuicReceive(
    const std::shared_ptr<StreamBinding>& binding,
    MsQuicStream* stream,
    const QUIC_BUFFER* buffers,
    uint32_t bufferCount,
    bool fin) {
    if (binding == nullptr || stream == nullptr) {
        return false;
    }
    if (bufferCount != 0 && buffers == nullptr) {
        return false;
    }
    if (!fin && (buffers == nullptr || bufferCount == 0)) {
        return false;
    }

    auto receive = std::shared_ptr<TqDarwinPendingQuicReceive>(new (std::nothrow) TqDarwinPendingQuicReceive{});
    if (!receive) {
        Errors.fetch_add(1, std::memory_order_relaxed);
        return false;
    }
    receive->RelayId = binding->RelayId;
    receive->Stream = stream;
    receive->BindingOwner = binding;
    receive->Fin = fin;
    receive->Slices.reserve(bufferCount);

    for (uint32_t i = 0; i < bufferCount; ++i) {
        if (buffers[i].Length == 0) {
            continue;
        }
        if (buffers[i].Buffer == nullptr) {
            Errors.fetch_add(1, std::memory_order_relaxed);
            return false;
        }
        receive->Slices.push_back(TqDarwinQuicReceiveSlice{buffers[i].Buffer, buffers[i].Length});
        receive->TotalLength += buffers[i].Length;
    }
    if (receive->TotalLength == 0 && !fin) {
        Errors.fetch_add(1, std::memory_order_relaxed);
        return false;
    }
    if (!ReserveCallbackReceiveBudget(binding.get(), receive->TotalLength)) {
        Errors.fetch_add(1, std::memory_order_relaxed);
        return false;
    }
    QuicReceiveViewCount.fetch_add(1, std::memory_order_relaxed);
    QuicReceiveViewBytes.fetch_add(receive->TotalLength, std::memory_order_relaxed);

    TqDarwinRelayEvent event{};
    event.Type = TqDarwinRelayEventType::QuicReceiveView;
    event.RelayId = receive->RelayId;
    event.TotalLength = receive->TotalLength;
    event.Fin = fin;
    event.ReceiveView = receive;
    if (!EnqueueEvent(std::move(event))) {
        ReleaseCallbackReceiveBudget(receive);
        QuicReceiveViewCount.fetch_sub(1, std::memory_order_relaxed);
        QuicReceiveViewBytes.fetch_sub(receive->TotalLength, std::memory_order_relaxed);
        return false;
    }
    return true;
}

void TqDarwinRelayWorker::ProcessQuicReceiveViewEvent(
    const std::shared_ptr<TqDarwinPendingQuicReceive>& receive) {
    if (receive == nullptr) {
        return;
    }
    auto relay = FindRelay(receive->RelayId);
    if (relay == nullptr) {
        ReleaseCallbackReceiveBudget(receive);
        (void)DiscardDeferredQuicReceive(nullptr, receive);
        return;
    }
    bool shouldDiscard = false;
    {
        std::lock_guard<std::mutex> relayLock(relay->Mutex);
        if (relay->Closing) {
            shouldDiscard = true;
        } else {
            relay->PendingQuicReceiveBytes += receive->TotalLength;
            relay->PendingQuicReceives.push_back(receive);
        }
    }
    ReleaseCallbackReceiveBudget(receive);
    if (shouldDiscard) {
        (void)DiscardDeferredQuicReceive(relay, receive);
        return;
    }
    if (receive->PendingCompleteBytes != 0) {
        CompleteDeferredQuicReceive(relay, receive);
        return;
    }
    MaybePauseQuicReceive(relay);
    {
        std::lock_guard<std::mutex> relayLock(relay->Mutex);
        shouldDiscard = relay->Closing;
    }
    if (shouldDiscard) {
        (void)DiscardDeferredQuicReceive(relay, receive);
        return;
    }
    if (!EnqueueQuicReceiveForTcp(relay, receive)) {
        std::lock_guard<std::mutex> relayLock(relay->Mutex);
        relay->Closing = true;
    }
    (void)FlushTcpWrites(relay);
}

bool TqDarwinRelayWorker::EnqueueQuicReceiveForTcp(
    const std::shared_ptr<RelayState>& relay,
    const std::shared_ptr<TqDarwinPendingQuicReceive>& receive) {
    if (relay == nullptr || receive == nullptr) {
        return false;
    }

    bool needsDecompress = false;
    ITqDecompressor* decompressor = nullptr;
    {
        std::lock_guard<std::mutex> relayLock(relay->Mutex);
        if (relay->Closing) {
            return false;
        }
        needsDecompress = relay->Decompressor != nullptr && relay->CompressAlgo == TqCompressAlgo::Zstd;
        decompressor = relay->Decompressor;
    }

    std::deque<std::shared_ptr<TqDarwinRelayPendingTcpWrite>> tcpWrites;
    const size_t chunkSize = std::max<size_t>(1, Config.ReadChunkSize);
    auto appendOutput = [&](const uint8_t* data, size_t size) -> bool {
        size_t offset = 0;
        while (offset < size) {
            TqBufferAcquireFailure failure = TqBufferAcquireFailure::None;
            auto buffer = TqAllocateRelayBuffer(&relay->TcpReadBuffers, chunkSize, &failure);
            if (!buffer) {
                Errors.fetch_add(1, std::memory_order_relaxed);
                return false;
            }
            const size_t length = std::min(buffer->Capacity(), size - offset);
            std::memcpy(buffer->Data(), data + offset, length);
            buffer->SetLength(length);
            uint8_t* bufferData = buffer->Data();
            TqBufferView view{bufferData, length, std::move(buffer)};
            auto write = std::make_shared<TqDarwinRelayPendingTcpWrite>();
            write->View = std::move(view);
            write->Receive = receive;
            write->ReceiveBytesRemaining = length;
            tcpWrites.push_back(std::move(write));
            offset += length;
        }
        return true;
    };

    if (needsDecompress) {
        if (decompressor == nullptr) {
            (void)DiscardDeferredQuicReceive(relay, receive);
            {
                std::lock_guard<std::mutex> relayLock(relay->Mutex);
                relay->CompressedReceiveRejected = true;
                relay->Closing = true;
            }
            return true;
        }
        std::vector<uint8_t> decompressed;
        for (const auto& slice : receive->Slices) {
            if (!decompressor->Decompress(slice.Data, slice.Length, decompressed)) {
                (void)DiscardDeferredQuicReceive(relay, receive);
                {
                    std::lock_guard<std::mutex> relayLock(relay->Mutex);
                    relay->CompressedReceiveRejected = true;
                    relay->Closing = true;
                }
                return true;
            }
        }
        if (!decompressed.empty() && !appendOutput(decompressed.data(), decompressed.size())) {
            (void)DiscardDeferredQuicReceive(relay, receive);
            {
                std::lock_guard<std::mutex> relayLock(relay->Mutex);
                relay->Closing = true;
            }
            return false;
        }
    } else {
        for (const auto& slice : receive->Slices) {
            if (slice.Length != 0 && !appendOutput(slice.Data, slice.Length)) {
                (void)DiscardDeferredQuicReceive(relay, receive);
                {
                    std::lock_guard<std::mutex> relayLock(relay->Mutex);
                    relay->Closing = true;
                }
                return false;
            }
        }
    }

    std::shared_ptr<TqDarwinPendingQuicReceive> completedReceive;
    {
        std::lock_guard<std::mutex> relayLock(relay->Mutex);
        if (relay->Closing) {
            return false;
        }
        uint64_t remainingTcpWriteBytes = 0;
        for (auto& write : tcpWrites) {
            if (write == nullptr) {
                continue;
            }
            remainingTcpWriteBytes += write->ReceiveBytesRemaining;
            relay->PendingTcpWriteBytes += write->View.Len;
            relay->PendingTcpWrites.push_back(std::move(write));
        }
        if (remainingTcpWriteBytes == 0) {
            receive->PendingCompleteBytes = receive->PendingCompleteBytes == 0 && receive->CompletedLength >= receive->TotalLength
                ? 0
                : receive->TotalLength;
            receive->CompletedLength = receive->TotalLength;
            relay->PendingQuicReceiveBytes = relay->PendingQuicReceiveBytes >= receive->TotalLength
                ? relay->PendingQuicReceiveBytes - receive->TotalLength
                : 0;
            for (auto it = relay->PendingQuicReceives.begin(); it != relay->PendingQuicReceives.end(); ++it) {
                if (*it == receive) {
                    relay->PendingQuicReceives.erase(it);
                    break;
                }
            }
            completedReceive = receive;
        }
        if (receive->Fin) {
            relay->TcpWriteShutdownQueued = true;
        }
    }
    if (completedReceive != nullptr) {
        CompleteDeferredQuicReceive(relay, completedReceive);
    }
    MaybeResumeQuicReceive(relay);
    return true;
}

bool TqDarwinRelayWorker::DiscardDeferredQuicReceive(
    const std::shared_ptr<RelayState>& relay,
    const std::shared_ptr<TqDarwinPendingQuicReceive>& receive) {
    if (receive == nullptr) {
        return false;
    }
    ReleaseCallbackReceiveBudget(receive);
    uint64_t bytesToComplete = 0;
    {
        if (relay != nullptr) {
            std::lock_guard<std::mutex> relayLock(relay->Mutex);
            bytesToComplete = receive->PendingCompleteBytes == 0 && receive->CompletedLength >= receive->TotalLength
                ? 0
                : receive->TotalLength;
            const uint64_t bytesToDebit = receive->TotalLength >= receive->CompletedLength
                ? receive->TotalLength - receive->CompletedLength
                : 0;
            receive->CompletedLength = receive->TotalLength;
            receive->PendingCompleteBytes = bytesToComplete;
            relay->PendingQuicReceiveBytes = relay->PendingQuicReceiveBytes >= bytesToDebit
                ? relay->PendingQuicReceiveBytes - bytesToDebit
                : 0;
            for (auto it = relay->PendingQuicReceives.begin(); it != relay->PendingQuicReceives.end(); ++it) {
                if (*it == receive) {
                    relay->PendingQuicReceives.erase(it);
                    break;
                }
            }
        } else {
            bytesToComplete = receive->PendingCompleteBytes == 0 && receive->CompletedLength >= receive->TotalLength
                ? 0
                : receive->TotalLength;
            receive->CompletedLength = receive->TotalLength;
            receive->PendingCompleteBytes = bytesToComplete;
        }
    }
    CompleteDeferredQuicReceive(relay, receive);
    return true;
}

bool TqDarwinRelayWorker::FlushTcpWrites(const std::shared_ptr<RelayState>& relay) {
    if (relay == nullptr) {
        return true;
    }

    uint64_t burstBytes = 0;
    for (;;) {
        std::vector<iovec> iov;
        std::vector<std::shared_ptr<TqDarwinRelayPendingTcpWrite>> writeSnapshot;
        iov.reserve(std::max<uint32_t>(1, Config.MaxIov));
        writeSnapshot.reserve(std::max<uint32_t>(1, Config.MaxIov));
        uint64_t attemptedBytes = 0;
        {
            std::lock_guard<std::mutex> relayLock(relay->Mutex);
            if (relay->Closing) {
                return false;
            }
            if (Config.TcpWriteBurstBytes != 0 && burstBytes >= Config.TcpWriteBurstBytes) {
                relay->TcpWriteArmed = true;
                return true;
            }
            uint64_t maxWriteBytes = Config.TcpWriteMaxBytes;
            if (Config.TcpWriteBurstBytes != 0) {
                const uint64_t remainingBurst = Config.TcpWriteBurstBytes - burstBytes;
                maxWriteBytes = maxWriteBytes == 0 ? remainingBurst : std::min(maxWriteBytes, remainingBurst);
            }
            if (!relay->PendingTcpWrites.empty()) {
                for (const auto& write : relay->PendingTcpWrites) {
                    if (write == nullptr) {
                        continue;
                    }
                    const auto& view = write->View;
                    if (iov.size() >= std::max<uint32_t>(1, Config.MaxIov) ||
                        (maxWriteBytes != 0 && attemptedBytes >= maxWriteBytes)) {
                        break;
                    }
                    uint64_t length = view.Len;
                    if (maxWriteBytes != 0 && attemptedBytes + length > maxWriteBytes) {
                        length = maxWriteBytes - attemptedBytes;
                    }
                    if (length == 0) {
                        break;
                    }
                    iovec item{};
                    item.iov_base = view.Data;
                    item.iov_len = static_cast<size_t>(length);
                    iov.push_back(item);
                    writeSnapshot.push_back(write);
                    attemptedBytes += length;
                }
            } else if (relay->TcpWriteShutdownQueued) {
                (void)::shutdown(relay->TcpFd, SHUT_WR);
                relay->TcpWriteShutdownQueued = false;
                relay->TcpWriteArmed = false;
                relay->TcpWriteClosed = true;
                return true;
            } else {
                relay->TcpWriteArmed = false;
                return true;
            }
        }

        if (iov.empty()) {
            return true;
        }
        msghdr message{};
        message.msg_iov = iov.data();
        message.msg_iovlen = iov.size();
        const ssize_t sent = SendMsgNoSignal(relay->TcpFd, &message);
        if (sent > 0) {
            uint64_t remaining = static_cast<uint64_t>(sent);
            burstBytes += remaining;
            std::vector<std::shared_ptr<TqDarwinPendingQuicReceive>> completedReceives;
            {
                std::lock_guard<std::mutex> relayLock(relay->Mutex);
                if (relay->Closing) {
                    return false;
                }
                relay->TcpWriteBytes += static_cast<uint64_t>(sent);
                TcpWriteBatches.fetch_add(1, std::memory_order_relaxed);
                TcpWriteBytes.fetch_add(static_cast<uint64_t>(sent), std::memory_order_relaxed);
                while (remaining > 0 && !relay->PendingTcpWrites.empty()) {
                    auto front = relay->PendingTcpWrites.front();
                    if (front == nullptr) {
                        relay->PendingTcpWrites.pop_front();
                        continue;
                    }
                    auto receive = front->Receive;
                    const uint64_t consumed = std::min<uint64_t>(remaining, front->View.Len);
                    if (front->ReceiveBytesRemaining >= consumed) {
                        front->ReceiveBytesRemaining -= consumed;
                    } else {
                        front->ReceiveBytesRemaining = 0;
                    }
                    if (remaining >= front->View.Len) {
                        remaining -= front->View.Len;
                        relay->PendingTcpWriteBytes = relay->PendingTcpWriteBytes >= front->View.Len
                            ? relay->PendingTcpWriteBytes - front->View.Len
                            : 0;
                        relay->PendingTcpWrites.pop_front();
                    } else {
                        front->View.Data += remaining;
                        front->View.Len -= remaining;
                        relay->PendingTcpWriteBytes = relay->PendingTcpWriteBytes >= remaining
                            ? relay->PendingTcpWriteBytes - remaining
                            : 0;
                        remaining = 0;
                    }
                    bool receiveHasPendingWrites = false;
                    if (receive != nullptr) {
                        for (const auto& pendingWrite : relay->PendingTcpWrites) {
                            if (pendingWrite != nullptr && pendingWrite->Receive == receive &&
                                pendingWrite->ReceiveBytesRemaining != 0) {
                                receiveHasPendingWrites = true;
                                break;
                            }
                        }
                    }
                    if (receive != nullptr && !receiveHasPendingWrites &&
                        receive->PendingCompleteBytes == 0 && receive->CompletedLength < receive->TotalLength) {
                        receive->PendingCompleteBytes = receive->TotalLength;
                        receive->CompletedLength = receive->TotalLength;
                        relay->PendingQuicReceiveBytes = relay->PendingQuicReceiveBytes >= receive->TotalLength
                            ? relay->PendingQuicReceiveBytes - receive->TotalLength
                            : 0;
                        for (auto it = relay->PendingQuicReceives.begin(); it != relay->PendingQuicReceives.end(); ++it) {
                            if (*it == receive) {
                                relay->PendingQuicReceives.erase(it);
                                break;
                            }
                        }
                        completedReceives.push_back(receive);
                    }
                }
            }
            for (const auto& receive : completedReceives) {
                CompleteDeferredQuicReceive(relay, receive);
            }
            MaybeResumeQuicReceive(relay);
            continue;
        }
        if (sent < 0 && errno == EINTR) {
            continue;
        }
        if (sent < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            {
                std::lock_guard<std::mutex> relayLock(relay->Mutex);
                relay->TcpWriteArmed = true;
            }
            (void)UpdateTcpInterest(relay);
            return true;
        }
        Errors.fetch_add(1, std::memory_order_relaxed);
        std::deque<std::shared_ptr<TqDarwinPendingQuicReceive>> receivesToDiscard;
        {
            std::lock_guard<std::mutex> relayLock(relay->Mutex);
            relay->Closing = true;
            receivesToDiscard.swap(relay->PendingQuicReceives);
            relay->PendingQuicReceiveBytes = 0;
            relay->PendingTcpWriteBytes = 0;
            relay->PendingTcpWrites.clear();
        }
        for (const auto& pending : receivesToDiscard) {
            (void)DiscardDeferredQuicReceive(nullptr, pending);
        }
        return false;
    }
}

void TqDarwinRelayWorker::CompleteDeferredQuicReceive(
    const std::shared_ptr<RelayState>&,
    const std::shared_ptr<TqDarwinPendingQuicReceive>& receive) {
    if (receive == nullptr || receive->PendingCompleteBytes == 0) {
        return;
    }
    const uint64_t bytes = receive->PendingCompleteBytes;
    receive->PendingCompleteBytes = 0;
    DeferredReceiveCompletes.fetch_add(1, std::memory_order_relaxed);
#if defined(TCPQUIC_TESTING)
    if (g_darwinRelayReceiveCompleteForTest != nullptr) {
        g_darwinRelayReceiveCompleteForTest(receive->Stream, bytes);
        return;
    }
#endif
    if (receive->Stream != nullptr && receive->Stream->Handle != nullptr) {
        receive->Stream->ReceiveComplete(bytes);
    }
}

bool TqDarwinRelayWorker::SetQuicReceiveEnabled(const std::shared_ptr<RelayState>& relay, bool enabled) {
    if (relay == nullptr) {
        return false;
    }
    MsQuicStream* stream = nullptr;
    {
        std::lock_guard<std::mutex> relayLock(relay->Mutex);
        stream = relay->Stream;
    }
    QUIC_STATUS status = QUIC_STATUS_SUCCESS;
#if defined(TCPQUIC_TESTING)
    if (g_darwinRelayReceiveSetEnabledForTest != nullptr) {
        status = g_darwinRelayReceiveSetEnabledForTest(stream, enabled);
    } else
#endif
    if (stream != nullptr && stream->Handle != nullptr) {
        status = stream->ReceiveSetEnabled(enabled);
    }
    if (QUIC_FAILED(status)) {
        Errors.fetch_add(1, std::memory_order_relaxed);
        return false;
    }
    if (enabled) {
        QuicReceiveResumedCount.fetch_add(1, std::memory_order_relaxed);
    } else {
        QuicReceivePausedCount.fetch_add(1, std::memory_order_relaxed);
    }
    return true;
}

void TqDarwinRelayWorker::MaybePauseQuicReceive(const std::shared_ptr<RelayState>& relay) {
    if (relay == nullptr) {
        return;
    }
    bool shouldPause = false;
    {
        std::lock_guard<std::mutex> relayLock(relay->Mutex);
        const uint64_t pendingPressure = relay->PendingQuicReceiveBytes + relay->PendingTcpWriteBytes;
        if (!relay->QuicReceivePaused && pendingPressure >= MaxPendingQuicReceiveBytesPerRelay()) {
            relay->QuicReceivePaused = true;
            shouldPause = true;
        }
    }
    if (shouldPause && !SetQuicReceiveEnabled(relay, false)) {
        std::lock_guard<std::mutex> relayLock(relay->Mutex);
        relay->QuicReceivePaused = false;
        relay->Closing = true;
    }
}

void TqDarwinRelayWorker::MaybeResumeQuicReceive(const std::shared_ptr<RelayState>& relay) {
    if (relay == nullptr) {
        return;
    }
    bool shouldResume = false;
    {
        std::lock_guard<std::mutex> relayLock(relay->Mutex);
        const uint64_t pendingPressure = relay->PendingQuicReceiveBytes + relay->PendingTcpWriteBytes;
        if (relay->QuicReceivePaused && pendingPressure <= LowPendingQuicReceiveBytesPerRelay()) {
            relay->QuicReceivePaused = false;
            shouldResume = true;
        }
    }
    if (shouldResume && !SetQuicReceiveEnabled(relay, true)) {
        std::lock_guard<std::mutex> relayLock(relay->Mutex);
        relay->QuicReceivePaused = true;
        relay->Closing = true;
    }
}

TqDarwinRelayRegistrationResult TqDarwinRelayWorker::RegisterRelayWithIdLocal(
    const TqDarwinRelayRegistration& registration) {
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
    if (!SetNoSigPipe(registration.TcpFd) && errno != ENOPROTOOPT) {
        Errors.fetch_add(1, std::memory_order_relaxed);
        return {};
    }

    auto relay = std::make_shared<RelayState>(registration, Config);
    {
        std::lock_guard<std::mutex> relayLock(relay->Mutex);
        relay->TcpReadArmed = true;
    }
    auto binding = std::make_shared<StreamBinding>();
    binding->Worker.store(this, std::memory_order_release);
    binding->Completions = std::make_shared<CompletionState>();
    relay->Binding = binding;

    {
        std::lock_guard<std::mutex> lock(RelayMutex);
        relay->Id = NextRelayId++;
        binding->RelayId = relay->Id;
        binding->Relay = relay;
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
#if defined(TCPQUIC_TESTING)
    if (TqDarwinRelayStreamUsableForTest(registration.Stream)) {
        registration.Stream->Callback = TqDarwinRelayWorker::StreamCallback;
        registration.Stream->Context = binding.get();
    }
#else
    registration.Stream->Callback = TqDarwinRelayWorker::StreamCallback;
    registration.Stream->Context = binding.get();
#endif
    return {true, relay->Id};
}

TqDarwinRelayRegistrationResult TqDarwinRelayWorker::RegisterRelayWithId(
    const TqDarwinRelayRegistration& registration) {
    if (IsWorkerThread()) {
        return RegisterRelayWithIdLocal(registration);
    }

    RegisterRelayCommand command{};
    command.Registration = registration;
    {
        std::lock_guard<std::mutex> lifecycleLock(LifecycleMutex);
        if (!Running.load(std::memory_order_acquire) || KqueueFd < 0) {
            return RegisterRelayWithIdLocal(registration);
        }

        TqDarwinRelayEvent event{};
        event.Type = TqDarwinRelayEventType::RegisterRelay;
        event.Control = &command;
        if (EnqueueControlEvent(std::move(event)) == ControlEnqueueResult::Failed) {
            return {};
        }
    }

    std::unique_lock<std::mutex> lock(command.Mutex);
    while (!command.Done) {
        if (command.Cv.wait_for(
                lock,
                kControlCommandWakeRetryInterval,
                [&command] { return command.Done; })) {
            break;
        }
        lock.unlock();
        (void)Wake();
        lock.lock();
    }
    return command.Result;
}

void TqDarwinRelayWorker::UnregisterRelayLocal(uint64_t relayId) {
    std::shared_ptr<RelayState> relay;
    {
        std::lock_guard<std::mutex> lock(RelayMutex);
        const auto it = Relays.find(relayId);
        if (it == Relays.end()) {
            return;
        }
        relay = it->second;
        Relays.erase(it);
        RemoveTcpFilters(relay);
    }

    RetireRelay(relay);
    PurgeRetiredRelaysIfSafe();
}

void TqDarwinRelayWorker::UnregisterRelay(uint64_t relayId) {
    if (IsWorkerThread()) {
        UnregisterRelayLocal(relayId);
        return;
    }

    // Unregister must complete even when the worker thread is blocked mid-flush
    // (for example during a partial TCP write). Eventizing this path would
    // deadlock because the worker cannot drain control events until the flush
    // returns.
    UnregisterRelayLocal(relayId);
}

TqDarwinRelayWorkerSnapshot TqDarwinRelayWorker::SnapshotLocal() const {
    TqDarwinRelayWorkerSnapshot snapshot{};
    snapshot.EventsProcessed = EventsProcessed.load(std::memory_order_relaxed);
    snapshot.Wakeups = Wakeups.load(std::memory_order_relaxed);
    snapshot.PendingEvents = EventQueue.SizeApprox();
    snapshot.TcpReadBatches = TcpReadBatches.load(std::memory_order_relaxed);
    snapshot.TcpReadBytes = TcpReadBytes.load(std::memory_order_relaxed);
    snapshot.TcpWriteBatches = TcpWriteBatches.load(std::memory_order_relaxed);
    snapshot.TcpWriteBytes = TcpWriteBytes.load(std::memory_order_relaxed);
    snapshot.QuicReceiveViewCount = QuicReceiveViewCount.load(std::memory_order_relaxed);
    snapshot.QuicReceiveViewBytes = QuicReceiveViewBytes.load(std::memory_order_relaxed);
    snapshot.DeferredReceiveCompletes = DeferredReceiveCompletes.load(std::memory_order_relaxed);
    snapshot.QuicSendBackpressureEvents = QuicSendBackpressureEvents.load(std::memory_order_relaxed);
    snapshot.QuicReceivePausedCount = QuicReceivePausedCount.load(std::memory_order_relaxed);
    snapshot.QuicReceiveResumedCount = QuicReceiveResumedCount.load(std::memory_order_relaxed);
    {
        std::lock_guard<std::mutex> lock(RelayMutex);
        snapshot.ActiveRelays = Relays.size();
        for (const auto& entry : Relays) {
            const auto& relay = entry.second;
            if (relay == nullptr) {
                continue;
            }
            std::lock_guard<std::mutex> relayLock(relay->Mutex);
            if (relay->TcpReadArmed) {
                ++snapshot.TcpReadArmedRelays;
            }
            if (relay->TcpWriteArmed) {
                ++snapshot.TcpWriteArmedRelays;
            }
            snapshot.OutstandingQuicSends += relay->InFlightQuicSends;
            snapshot.OutstandingQuicSendBytes += relay->InFlightQuicSendBytes;
            snapshot.OutstandingQuicSends += relay->PendingQuicSends.size();
            const uint64_t receivePathPendingBytes = std::max(
                relay->PendingQuicReceiveBytes,
                relay->PendingTcpWriteBytes);
            uint64_t relayPendingBytes = receivePathPendingBytes;
            for (const auto& pendingSend : relay->PendingQuicSends) {
                if (pendingSend != nullptr) {
                    snapshot.OutstandingQuicSendBytes += pendingSend->TotalBytes;
                    relayPendingBytes += pendingSend->TotalBytes;
                }
            }
            snapshot.CurrentPendingQuicReceiveBytes += relay->PendingQuicReceiveBytes;
            snapshot.PendingTcpWriteQueue += relay->PendingTcpWrites.size();
            snapshot.PendingTcpWriteBytes += relay->PendingTcpWriteBytes;
            snapshot.PendingBytes += relayPendingBytes;
        }
    }
    snapshot.Errors = Errors.load(std::memory_order_relaxed);
    return snapshot;
}

TqDarwinRelayWorkerSnapshot TqDarwinRelayWorker::Snapshot() const {
    if (IsWorkerThread()) {
        return SnapshotLocal();
    }

    SnapshotCommand command{};
    {
        std::lock_guard<std::mutex> lifecycleLock(LifecycleMutex);
        if (!Running.load(std::memory_order_acquire) || KqueueFd < 0) {
            return SnapshotLocal();
        }

        TqDarwinRelayEvent event{};
        event.Type = TqDarwinRelayEventType::Snapshot;
        event.Control = &command;
        if (EnqueueControlEvent(std::move(event)) == ControlEnqueueResult::Failed) {
            return SnapshotLocal();
        }
    }

    std::unique_lock<std::mutex> lock(command.Mutex);
    while (!command.Done) {
        if (command.Cv.wait_for(
                lock,
                kControlCommandWakeRetryInterval,
                [&command] { return command.Done; })) {
            break;
        }
        lock.unlock();
        (void)Wake();
        lock.lock();
    }
    return command.Result;
}

QUIC_STATUS QUIC_API TqDarwinRelayWorker::StreamCallback(
    MsQuicStream* stream,
    void* context,
    QUIC_STREAM_EVENT* event) noexcept {
    (void)stream;
    if (context == nullptr || event == nullptr) {
        return QUIC_STATUS_SUCCESS;
    }
    auto* binding = static_cast<StreamBinding*>(context);
    std::shared_ptr<StreamBinding> bindingOwner;
    try {
        bindingOwner = binding->shared_from_this();
    } catch (const std::bad_weak_ptr&) {
        return QUIC_STATUS_SUCCESS;
    }
    binding->CallbackRefs.fetch_add(1, std::memory_order_acq_rel);
    struct CallbackRefGuard {
        StreamBinding* Binding{nullptr};
        ~CallbackRefGuard() {
            Binding->CallbackRefs.fetch_sub(1, std::memory_order_acq_rel);
        }
    } guard{binding};
    if (event->Type == QUIC_STREAM_EVENT_SEND_COMPLETE) {
        TqDarwinRelaySendOperation* operation =
            reinterpret_cast<TqDarwinRelaySendOperation*>(event->SEND_COMPLETE.ClientContext);
        TqDarwinRelayWorker* worker = binding->Worker.load(std::memory_order_acquire);
        KnownSendOperationInfo info{};
        if (worker != nullptr && worker->TryClaimKnownSendCompletionEvent(binding, operation, &info)) {
            if (info.CompletionEventClaimed) {
                return QUIC_STATUS_SUCCESS;
            }
            if (worker->EnqueueQuicSendCompleteFromCallback(info.RelayId, operation)) {
                return QUIC_STATUS_SUCCESS;
            }
            worker->CompleteQuicSend(operation);
            return QUIC_STATUS_SUCCESS;
        }
        if (CompleteDetachedQuicSend(binding, operation)) {
            return QUIC_STATUS_SUCCESS;
        }
        if (worker != nullptr) {
            worker->Errors.fetch_add(1, std::memory_order_relaxed);
        }
    }
    if (event->Type == QUIC_STREAM_EVENT_PEER_SEND_ABORTED ||
        event->Type == QUIC_STREAM_EVENT_PEER_RECEIVE_ABORTED ||
        event->Type == QUIC_STREAM_EVENT_SHUTDOWN_COMPLETE) {
        TqDarwinRelayWorker* worker = binding->Worker.load(std::memory_order_acquire);
        if (worker != nullptr) {
            auto relay = worker->FindRelay(binding->RelayId);
            if (relay != nullptr) {
                std::lock_guard<std::mutex> relayLock(relay->Mutex);
                relay->Closing = true;
                relay->TcpReadArmed = false;
                relay->TcpWriteArmed = false;
            }
            TqDarwinRelayEvent queuedEvent{};
            queuedEvent.RelayId = binding->RelayId;
            if (event->Type == QUIC_STREAM_EVENT_PEER_SEND_ABORTED) {
                queuedEvent.Type = TqDarwinRelayEventType::QuicPeerSendAborted;
            } else if (event->Type == QUIC_STREAM_EVENT_PEER_RECEIVE_ABORTED) {
                queuedEvent.Type = TqDarwinRelayEventType::QuicPeerReceiveAborted;
            } else {
                queuedEvent.Type = TqDarwinRelayEventType::QuicShutdownComplete;
            }
            if (!worker->Running.load(std::memory_order_acquire) ||
                !worker->EnqueueEvent(std::move(queuedEvent))) {
                if (relay != nullptr) {
                    worker->CloseRelay(relay, 1);
                }
            }
        }
        return QUIC_STATUS_SUCCESS;
    }
    if (event->Type == QUIC_STREAM_EVENT_RECEIVE) {
        TqDarwinRelayWorker* worker = binding->Worker.load(std::memory_order_acquire);
        if (worker == nullptr || !binding->Active.load(std::memory_order_acquire)) {
            return QUIC_STATUS_SUCCESS;
        }
        const bool fin = (event->RECEIVE.Flags & QUIC_RECEIVE_FLAG_FIN) != 0;
        if (TqIsMsQuicFakeFinReceive(
                event->RECEIVE.AbsoluteOffset,
                event->RECEIVE.TotalBufferLength,
                event->RECEIVE.BufferCount,
                event->RECEIVE.Flags)) {
            auto relay = bindingOwner->Relay.lock();
            if (relay == nullptr) {
                return QUIC_STATUS_SUCCESS;
            }
            TqTraceLinuxRelayStreamState state{};
            {
                std::lock_guard<std::mutex> relayLock(relay->Mutex);
                state.WorkerIndex = worker->Config.WorkerIndex;
                state.RelayId = relay->Id;
                state.OutstandingQuicSends = relay->PendingQuicSends.size();
                state.OutstandingQuicSendBytes = relay->InFlightQuicSendBytes;
                state.PendingTcpWriteQueue = relay->PendingTcpWrites.size();
                state.PendingTcpWriteBytes = relay->PendingTcpWriteBytes;
                state.PendingQuicReceiveBytes = relay->PendingQuicReceiveBytes;
                state.TcpReadBytes = relay->TcpReadBytes;
                state.TcpWriteBytes = relay->TcpWriteBytes;
                state.TcpReadClosed = relay->TcpReadClosed;
                state.TcpWriteClosed = relay->TcpWriteClosed;
                state.QuicSendFinSubmitted = relay->QuicSendFinSubmitted;
                state.QuicSendFinCompleted = relay->QuicSendFinCompleted;
            }
            TqTraceRelayStreamEvent(
                "darwin",
                worker->Config.WorkerIndex,
                relay->Id,
                "receive_fake_fin",
                0,
                0,
                event->RECEIVE.AbsoluteOffset,
                event->RECEIVE.TotalBufferLength,
                event->RECEIVE.BufferCount,
                static_cast<uint32_t>(event->RECEIVE.Flags),
                true,
                state);
            assert(false && "MsQuic delivered FIN-only receive without known final size");
            std::abort();
        }
        if (!worker->QueueDeferredQuicReceive(
                bindingOwner,
                stream,
                event->RECEIVE.Buffers,
                event->RECEIVE.BufferCount,
                fin)) {
            worker->Errors.fetch_add(1, std::memory_order_relaxed);
            return QUIC_STATUS_SUCCESS;
        }
        return QUIC_STATUS_PENDING;
    }
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

TqDarwinRelayWorkerSnapshot TqDarwinRelayRuntime::Snapshot() const {
    std::lock_guard<std::mutex> lock(Mutex);
    TqDarwinRelayWorkerSnapshot snapshot{};
    for (const auto& worker : Workers) {
        if (worker == nullptr) {
            continue;
        }
        const auto workerSnapshot = worker->Snapshot();
        snapshot.EventsProcessed += workerSnapshot.EventsProcessed;
        snapshot.Wakeups += workerSnapshot.Wakeups;
        snapshot.PendingEvents += workerSnapshot.PendingEvents;
        snapshot.PendingBytes += workerSnapshot.PendingBytes;
        snapshot.ActiveRelays += workerSnapshot.ActiveRelays;
        snapshot.TcpReadArmedRelays += workerSnapshot.TcpReadArmedRelays;
        snapshot.TcpWriteArmedRelays += workerSnapshot.TcpWriteArmedRelays;
        snapshot.QuicReceivePausedCount += workerSnapshot.QuicReceivePausedCount;
        snapshot.QuicReceiveResumedCount += workerSnapshot.QuicReceiveResumedCount;
        snapshot.CurrentPendingQuicReceiveBytes += workerSnapshot.CurrentPendingQuicReceiveBytes;
        snapshot.PendingTcpWriteQueue += workerSnapshot.PendingTcpWriteQueue;
        snapshot.PendingTcpWriteBytes += workerSnapshot.PendingTcpWriteBytes;
        snapshot.OutstandingQuicSends += workerSnapshot.OutstandingQuicSends;
        snapshot.OutstandingQuicSendBytes += workerSnapshot.OutstandingQuicSendBytes;
        snapshot.TcpReadBatches += workerSnapshot.TcpReadBatches;
        snapshot.TcpReadBytes += workerSnapshot.TcpReadBytes;
        snapshot.TcpWriteBatches += workerSnapshot.TcpWriteBatches;
        snapshot.TcpWriteBytes += workerSnapshot.TcpWriteBytes;
        snapshot.QuicReceiveViewCount += workerSnapshot.QuicReceiveViewCount;
        snapshot.QuicReceiveViewBytes += workerSnapshot.QuicReceiveViewBytes;
        snapshot.DeferredReceiveCompletes += workerSnapshot.DeferredReceiveCompletes;
        snapshot.QuicSendBackpressureEvents += workerSnapshot.QuicSendBackpressureEvents;
        snapshot.Errors += workerSnapshot.Errors;
    }
    return snapshot;
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
