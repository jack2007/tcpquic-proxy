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
#include <atomic>
#include <cassert>
#include <cerrno>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <memory>
#include <new>
#include <stdexcept>
#include <string>
#include <thread>
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
    std::unordered_map<TqDarwinRelaySendOperation*, KnownSendOperationInfo> FallbackSendOperations;
    std::atomic<uint64_t> KnownSendOperationCount{0};
};

namespace {
std::atomic<uint64_t> g_DarwinShutdownSinkActive{0};
} // namespace

struct TqDarwinRelayWorker::CallbackEndpoint final {
    explicit CallbackEndpoint(TqDarwinRelayWorker* worker) : Worker(worker) {}

    TqDarwinRelayWorker* TryEnter() {
        std::lock_guard<std::mutex> guard(Lock);
        if (Worker == nullptr) {
            return nullptr;
        }
        ++ActiveCalls;
        return Worker;
    }

    void Leave() {
        std::lock_guard<std::mutex> guard(Lock);
        if (--ActiveCalls == 0) {
            Drained.notify_all();
        }
    }

    void CloseAndWait() {
        std::unique_lock<std::mutex> guard(Lock);
        Worker = nullptr;
        Drained.wait(guard, [this] { return ActiveCalls == 0; });
    }

    std::mutex Lock;
    std::condition_variable Drained;
    TqDarwinRelayWorker* Worker{nullptr};
    uint32_t ActiveCalls{0};
};

struct TqDarwinRelayWorker::StreamBinding final
    : TqStreamLifetime::Target,
      std::enable_shared_from_this<TqDarwinRelayWorker::StreamBinding> {
    std::atomic<TqDarwinRelayWorker*> Worker{nullptr};
    std::shared_ptr<CallbackEndpoint> Endpoint;
    std::shared_ptr<TqRelayStopControl> StopControl;
    uint64_t ControlGeneration{0};
    std::atomic<bool> Retired{false};
    std::atomic<bool> Active{true};
    std::atomic<bool> Terminal{false};
    std::atomic<bool> Closing{false};
    std::atomic<uint32_t> CallbackRefs{0};
    std::atomic<bool> CallbackQuicReceivePaused{false};
    std::atomic<uint64_t> CallbackPendingReceiveBytes{0};
    std::atomic<uint64_t> CallbackPendingReceiveEvents{0};
    std::atomic<bool> PeerSendShutdownSticky{false};
    std::atomic<bool> SendShutdownCompleteSticky{false};
    std::atomic<bool> ConvergenceCheckSticky{false};
    std::shared_ptr<CompletionState> Completions;
    uint64_t RelayId{0};
    // Immutable after initialize-before-PublishTarget: never assign/reset the
    // same weak_ptr instance, and never rewrite RouteGeneration after publish.
    std::weak_ptr<RelayState> Relay;
    std::weak_ptr<TqStreamLifetime> StreamOwner;
    uint64_t RouteGeneration{0};
    std::deque<std::shared_ptr<TqDarwinPendingQuicReceive>> CallbackPendingQuicReceives;
    mutable std::mutex CallbackPendingQuicReceivesMutex;
    enum class Activation : uint8_t { Prepared, Active, Terminal, Failed };
    std::atomic<Activation> ActivationState{Activation::Prepared};
    // Single activation mutex: phase transitions + precommit queue. No MsQuic /
    // kevent / close / wait / destructor under this lock — disposition only.
    std::mutex ActivationMutex;
    std::deque<std::shared_ptr<TqDarwinPendingQuicReceive>> PrecommitReceives;
    uint64_t PrecommitPendingBytes{0};
    uint64_t PrecommitMaxPendingBytes{0};
    // Once-only under ActivationMutex: drain or discard must clear precommit.
    bool PrecommitSettled{false};
#if defined(TCPQUIC_TESTING)
    TqDarwinRelayWorker* DestructorCounterOwner{nullptr};
#endif

    ~StreamBinding() {
#if defined(TCPQUIC_TESTING)
        if (DestructorCounterOwner != nullptr) {
            DestructorCounterOwner->StreamBindingDestructorCount.fetch_add(
                1,
                std::memory_order_relaxed);
        }
#endif
    }

    QUIC_STATUS OnStreamEvent(
        MsQuicStream* stream,
        QUIC_STREAM_EVENT* event,
        uint64_t) noexcept override {
        auto endpoint = Endpoint;
        if (endpoint != nullptr) {
            auto* worker = endpoint->TryEnter();
            if (worker == nullptr) {
                return QUIC_STATUS_SUCCESS;
            }
            struct Guard {
                std::shared_ptr<CallbackEndpoint> Endpoint;
                ~Guard() { Endpoint->Leave(); }
            } guard{std::move(endpoint)};
            return worker->OnStreamEventWithBinding(stream, event, this);
        }
        auto* worker = Worker.load(std::memory_order_acquire);
        if (worker == nullptr) {
            return QUIC_STATUS_SUCCESS;
        }
        return worker->OnStreamEventWithBinding(stream, event, this);
    }
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
    std::shared_ptr<TqStreamLifetime> StreamOwner;
    std::shared_ptr<TqRelayStopControl> StopControl;
    uint64_t ControlGeneration{0};
    ITqCompressor* Compressor{nullptr};
    ITqDecompressor* Decompressor{nullptr};
    TqCompressAlgo CompressAlgo{TqCompressAlgo::None};
    bool EnableQuicSends{true};
    bool Committed{false};
    std::shared_ptr<StreamBinding> Binding;
    std::shared_ptr<StreamBinding> ManagedBinding;
    bool TcpReadArmed{false};
    bool TcpWriteArmed{false};
    bool TcpReadPausedByQuicBacklog{false};
    bool TcpReadClosed{false};
    bool TcpWriteClosed{false};
    bool QuicSendClosed{false};
    bool QuicReceiveClosed{false};
    bool QuicSendFinSubmitted{false};
    bool QuicSendFinCompleted{false};
    // Set only on MsQuic SEND_SHUTDOWN_COMPLETE (distinct from FIN SEND_COMPLETE / H5).
    bool QuicSendShutdownCompleteObserved{false};
    bool Closing{false};
    std::atomic<bool> LogicallyDetached{false};
    std::atomic<bool> TerminalOwnerReleased{false};
    std::atomic<bool> RetiredToWorker{false};
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
    std::chrono::steady_clock::time_point RetiredAt{};

    RelayState(const TqDarwinRelayRegistration& registration, const TqDarwinRelayWorkerConfig& config)
        : TcpFd(registration.TcpFd),
          Stream([&]() -> MsQuicStream* {
              if (registration.StreamOwner != nullptr) {
                  if (MsQuicStream* owned = registration.StreamOwner->StreamForInitialization()) {
                      return owned;
                  }
              }
              return registration.Stream;
          }()),
          StreamOwner(registration.StreamOwner),
          StopControl(registration.Control),
          ControlGeneration(
              registration.ControlGeneration != 0
                  ? registration.ControlGeneration
                  : (registration.Control != nullptr ? registration.Control->Generation : 0)),
          Compressor(registration.Compressor),
          Decompressor(registration.Decompressor),
          CompressAlgo(registration.CompressAlgo),
          EnableQuicSends(registration.EnableQuicSends) {
        TcpReadBuffers.MaxPendingBufferBytes = config.MaxBufferedQuicSendBytes != 0
            ? config.MaxBufferedQuicSendBytes
            : config.ReadBatchBytes * 2;
    }

#if defined(TCPQUIC_TESTING)
    TqDarwinRelayWorker* DestructorCounterOwner{nullptr};
    ~RelayState() {
        if (DestructorCounterOwner != nullptr) {
            DestructorCounterOwner->RelayStateDestructorCount.fetch_add(
                1,
                std::memory_order_relaxed);
        }
    }
#endif
};

bool TqDarwinRelayWorker::FullyClosedPredicateReady(const RelayState& relay) {
    if (relay.Closing) {
        return false;
    }
    if (!relay.TcpReadClosed || !relay.TcpWriteClosed) {
        return false;
    }
    if (!relay.QuicSendFinSubmitted || !relay.QuicSendFinCompleted) {
        return false;
    }
    if (relay.InFlightQuicSends != 0 ||
        !relay.PendingQuicSends.empty() ||
        !relay.PendingTcpWrites.empty() ||
        !relay.PendingQuicReceives.empty() ||
        relay.PendingQuicReceiveBytes != 0 ||
        relay.PendingTcpWriteBytes != 0 ||
        relay.TcpWriteShutdownQueued) {
        return false;
    }
    if (relay.Binding != nullptr) {
        if (relay.Binding->CallbackPendingReceiveEvents.load(std::memory_order_acquire) != 0 ||
            relay.Binding->CallbackPendingReceiveBytes.load(std::memory_order_acquire) != 0) {
            return false;
        }
        // Prefer atomics over CallbackPendingQuicReceivesMutex: SnapshotLocal holds
        // RelayMutex and must not nest the callback queue lock (lock-order / lifetime).
    }
    return true;
}

void TqDarwinRelayWorker::TraceHalfClose(const RelayState& relay, const char* trigger) const {
    if (!TqTraceEnabled()) {
        return;
    }
    std::string blockers;
    auto append = [&](const char* token) {
        if (!blockers.empty()) {
            blockers.push_back(',');
        }
        blockers.append(token);
    };
    if (relay.Closing) {
        append("closing");
    }
    if (!relay.TcpReadClosed) {
        append("need_tcp_read_closed");
    }
    if (!relay.TcpWriteClosed) {
        if (relay.TcpWriteShutdownQueued) {
            append("need_tcp_shut_wr");
        } else {
            append("need_peer_fin");
        }
    }
    if (!relay.QuicSendFinSubmitted) {
        append("need_quic_fin_submitted");
    }
    if (!relay.QuicSendFinCompleted) {
        append("need_quic_fin_buffer_released");
    }
    if (!relay.QuicSendShutdownCompleteObserved) {
        append("need_send_shutdown_complete");
    }
    if (relay.InFlightQuicSends != 0) {
        append("inflight_quic_sends");
    }
    if (!relay.PendingQuicSends.empty()) {
        append("pending_quic_sends");
    }
    if (!relay.PendingTcpWrites.empty() || relay.PendingTcpWriteBytes != 0) {
        append("pending_tcp_writes");
    }
    if (!relay.PendingQuicReceives.empty() || relay.PendingQuicReceiveBytes != 0) {
        append("pending_quic_recv");
    }
    if (relay.Binding != nullptr) {
        const uint64_t cbEvents =
            relay.Binding->CallbackPendingReceiveEvents.load(std::memory_order_acquire);
        const uint64_t cbBytes =
            relay.Binding->CallbackPendingReceiveBytes.load(std::memory_order_acquire);
        if (cbEvents != 0 || cbBytes != 0) {
            append("callback_pending_recv");
        }
    }
    if (FullyClosedPredicateReady(relay)) {
        blockers = "predicate_ready_no_signal_stop";
    } else if (blockers.empty()) {
        blockers = "none";
    }

    TqTraceLinuxRelayStreamState state{};
    state.WorkerIndex = Config.WorkerIndex;
    state.RelayId = relay.Id;
    state.OutstandingQuicSends = relay.InFlightQuicSends + relay.PendingQuicSends.size();
    state.OutstandingQuicSendBytes = relay.InFlightQuicSendBytes;
    state.PendingTcpWriteQueue = relay.PendingTcpWrites.size();
    state.PendingTcpWriteBytes = relay.PendingTcpWriteBytes;
    state.PendingQuicReceiveBytes = relay.PendingQuicReceiveBytes;
    state.TcpReadBytes = relay.TcpReadBytes;
    state.TcpWriteBytes = relay.TcpWriteBytes;
    state.TcpReadClosed = relay.TcpReadClosed;
    state.TcpWriteClosed = relay.TcpWriteClosed;
    state.QuicSendFinSubmitted = relay.QuicSendFinSubmitted;
    state.QuicSendFinCompleted = relay.QuicSendFinCompleted;
    state.TcpWriteShutdownQueued = relay.TcpWriteShutdownQueued;
    state.StreamDetached = relay.Stream == nullptr && relay.StreamOwner == nullptr;
    TqTraceRelayHalfClose(
        "darwin",
        Config.WorkerIndex,
        trigger,
        state,
        blockers.c_str(),
        relay.TcpReadArmed,
        relay.TcpReadPausedByQuicBacklog);
}

struct TqDarwinRelayWorker::ShutdownSink final : TqStreamLifetime::Target {
    std::shared_ptr<TqRelayStopControl> StopControl;
    uint64_t ControlGeneration{0};
    std::weak_ptr<TqStreamLifetime> StreamOwner;
    std::shared_ptr<CompletionState> Completions;

    ShutdownSink() {
        g_DarwinShutdownSinkActive.fetch_add(1, std::memory_order_relaxed);
    }
    ~ShutdownSink() override {
        g_DarwinShutdownSinkActive.fetch_sub(1, std::memory_order_relaxed);
    }

    QUIC_STATUS OnStreamEvent(
        MsQuicStream*,
        QUIC_STREAM_EVENT* event,
        uint64_t) noexcept override {
        if (event == nullptr) {
            return QUIC_STATUS_SUCCESS;
        }
        switch (event->Type) {
        case QUIC_STREAM_EVENT_SHUTDOWN_COMPLETE: {
            if (auto owner = StreamOwner.lock()) {
                (void)owner->PublishTerminalAndTakeTarget();
            }
            if (StopControl != nullptr) {
                (void)StopControl->SignalStop(ControlGeneration);
            }
            return QUIC_STATUS_SUCCESS;
        }
        case QUIC_STREAM_EVENT_RECEIVE:
            event->RECEIVE.TotalBufferLength = 0;
            if (auto owner = StreamOwner.lock()) {
                (void)owner->RequestShutdown(TqStreamLifetime::ShutdownIntent::AbortReceive);
            }
            return QUIC_STATUS_SUCCESS;
        case QUIC_STREAM_EVENT_SEND_COMPLETE: {
            auto* operation = reinterpret_cast<TqDarwinRelaySendOperation*>(
                event->SEND_COMPLETE.ClientContext);
            if (operation == nullptr) {
                return QUIC_STATUS_SUCCESS;
            }
            KnownSendOperationInfo info{};
            if (!UnregisterCompletionStateOperation(Completions, operation, &info, nullptr)) {
                return QUIC_STATUS_SUCCESS;
            }
            if (!operation->TryMarkCompleted()) {
                return QUIC_STATUS_SUCCESS;
            }
            if (auto binding = std::static_pointer_cast<StreamBinding>(info.BindingOwner)) {
                if (auto relay = binding->Relay.lock()) {
                    std::lock_guard<std::mutex> relayLock(relay->Mutex);
                    if (relay->InFlightQuicSends > 0) {
                        --relay->InFlightQuicSends;
                    }
                    relay->InFlightQuicSendBytes =
                        relay->InFlightQuicSendBytes >= info.TotalBytes
                            ? relay->InFlightQuicSendBytes - info.TotalBytes
                            : 0;
                    if (info.Fin) {
                        relay->QuicSendFinCompleted = true;
                        relay->QuicSendClosed = true;
                    }
                }
            }
            delete operation;
            return QUIC_STATUS_SUCCESS;
        }
        case QUIC_STREAM_EVENT_CANCEL_ON_LOSS:
            event->CANCEL_ON_LOSS.ErrorCode = TqRelayStreamErrorCancelOnLoss;
            return QUIC_STATUS_SUCCESS;
        default:
            return QUIC_STATUS_SUCCESS;
        }
    }
};

TqDarwinRelayWorker::TqDarwinRelayWorker(const TqDarwinRelayWorkerConfig& config)
    : Config(config),
      StreamCallbackEndpoint(std::make_shared<CallbackEndpoint>(this)),
      EventQueue(config.EventQueueCapacity) {}

TqDarwinRelayWorker::~TqDarwinRelayWorker() {
    Stop();
    if (StreamCallbackEndpoint != nullptr) {
        StreamCallbackEndpoint->CloseAndWait();
    }
    DetachRetiredBindingsForDestruction();
}

bool TqDarwinRelayWorker::Start() {
    std::lock_guard<std::mutex> lifecycleLock(LifecycleMutex);
    if (Running.load(std::memory_order_acquire)) {
        return true;
    }
#if defined(TQ_UNIT_TESTING)
    if (Config.FailStartForTest) {
        return false;
    }
#endif

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
        if (!Running.load(std::memory_order_acquire) && KqueueFd < 0) {
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

        // Switch routes to worker-independent sinks and forbid new worker
        // callback admission before joining the kqueue thread.
        InstallShutdownSinksForStop();
        if (StreamCallbackEndpoint != nullptr) {
            StreamCallbackEndpoint->CloseAndWait();
        }

        Running.store(false, std::memory_order_release);
        (void)Wake();
        if (Thread.joinable()) {
            Thread.join();
        }

        if (KqueueFd >= 0) {
            {
                std::unique_lock<std::shared_mutex> mapAccess(RelayMapAccessMutex);
                std::lock_guard<std::mutex> lock(RelayMutex);
                relays.swap(Relays);
            }
            // Settle PENDING receive views while StreamOwner leases are still
            // acquirable. CompleteDeferred is lease-only — no bare Stream.
            SettleQueuedReceiveViewsBeforeRetire();
            for (const auto& entry : relays) {
                RequestRelayShutdown(
                    entry.second,
                    TqStreamLifetime::ShutdownIntent::AbortBoth);
                RemoveTcpFilters(entry.second);
                RetireRelay(
                    entry.second,
                    TqDarwinRelayCloseDisposition::ActiveShutdown);
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
            WakeFailures.fetch_add(1, std::memory_order_relaxed);
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
        WakeFailures.fetch_add(1, std::memory_order_relaxed);
        return false;
    }

    Wakeups.fetch_add(1, std::memory_order_relaxed);
    return true;
}

bool TqDarwinRelayWorker::EnqueueEvent(TqDarwinRelayEvent&& event) {
    if (!EventQueue.TryPush(std::move(event))) {
        Errors.fetch_add(1, std::memory_order_relaxed);
        EventQueueFullErrors.fetch_add(1, std::memory_order_relaxed);
        return false;
    }
    (void)Wake();
    return true;
}

TqDarwinRelayWorker::ControlEnqueueResult TqDarwinRelayWorker::EnqueueControlEvent(
    TqDarwinRelayEvent&& event) const {
    if (!EventQueue.TryPush(std::move(event))) {
        Errors.fetch_add(1, std::memory_order_relaxed);
        EventQueueFullErrors.fetch_add(1, std::memory_order_relaxed);
        return ControlEnqueueResult::Failed;
    }
    return Wake() ? ControlEnqueueResult::QueuedAndWoken : ControlEnqueueResult::QueuedWakeFailed;
}

bool TqDarwinRelayWorker::IsWorkerThread() const {
    std::lock_guard<std::mutex> lock(WorkerThreadIdMutex);
    return WorkerThreadId == std::this_thread::get_id();
}

bool TqDarwinRelayWorker::WorkerThreadExited() const {
    std::lock_guard<std::mutex> lock(WorkerThreadIdMutex);
    return WorkerThreadId == std::thread::id{};
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

uint32_t TqDarwinRelayWorker::PendingEventsForTest() const {
    return EventQueue.SizeApprox();
}

bool TqDarwinRelayWorker::RunningForTest() const {
    return Running.load(std::memory_order_acquire);
}

void TqDarwinRelayWorker::SetRunningForTest(bool running) {
    Running.store(running, std::memory_order_release);
}

void TqDarwinRelayWorker::MarkWorkerThreadExitedForTest() {
    std::lock_guard<std::mutex> lock(WorkerThreadIdMutex);
    WorkerThreadId = std::thread::id{};
}

uint64_t TqDarwinRelayWorker::FindRelayLockedCountForTest() const {
    return FindRelayLockedCount.load(std::memory_order_relaxed);
}

uint64_t TqDarwinRelayWorker::CommittedRelayCountForTest() const {
    uint64_t count = 0;
    std::lock_guard<std::mutex> lock(RelayMutex);
    for (const auto& entry : Relays) {
        if (entry.second != nullptr && entry.second->Committed) {
            ++count;
        }
    }
    return count;
}

uint64_t TqDarwinRelayWorker::FindRelayLocalCountForTest() const {
    return FindRelayLocalCount.load(std::memory_order_relaxed);
}

uint64_t TqDarwinRelayWorker::RetiredRelayPurgeCountForTest() const {
    return RetiredRelayPurgeCount.load(std::memory_order_relaxed);
}

uint64_t TqDarwinRelayWorker::KnownSendLockedCountForTest() const {
    return KnownSendLockedCount.load(std::memory_order_relaxed);
}

uint64_t TqDarwinRelayWorker::CompletionStateLockedCountForTest() const {
    return CompletionStateLockedCount.load(std::memory_order_relaxed);
}

uint64_t TqDarwinRelayWorker::ActiveSendLocalRegisterCountForTest() const {
    return ActiveSendLocalRegisterCount.load(std::memory_order_relaxed);
}

uint64_t TqDarwinRelayWorker::ActiveSendLocalCompleteCountForTest() const {
    return ActiveSendLocalCompleteCount.load(std::memory_order_relaxed);
}

uint64_t TqDarwinRelayWorker::FallbackSendCompletionCountForTest() const {
    return FallbackSendCompletionCount.load(std::memory_order_relaxed);
}

uint64_t TqDarwinRelayWorker::EventQueueFullErrorsForTest() const {
    return EventQueueFullErrors.load(std::memory_order_relaxed);
}

uint64_t TqDarwinRelayWorker::CallbackReceiveBudgetRejectsForTest() const {
    return CallbackReceiveBudgetRejects.load(std::memory_order_relaxed);
}

uint64_t TqDarwinRelayWorker::QuicReceiveEnqueueFailuresForTest() const {
    return QuicReceiveEnqueueFailures.load(std::memory_order_relaxed);
}

uint64_t TqDarwinRelayWorker::QuicReceiveViewBackpressureQueuedForTest() const {
    return QuicReceiveViewBackpressureQueued.load(std::memory_order_relaxed);
}

uint64_t TqDarwinRelayWorker::QuicActiveShutdownEnqueuedForTest() const {
    return QuicActiveShutdownEnqueued.load(std::memory_order_relaxed);
}

uint64_t TqDarwinRelayWorker::QuicShutdownCompleteEnqueuedForTest() const {
    return QuicShutdownCompleteEnqueued.load(std::memory_order_relaxed);
}

TqDarwinActiveShutdownReason TqDarwinRelayWorker::LastActiveShutdownReasonForTest() const {
    return static_cast<TqDarwinActiveShutdownReason>(
        LastActiveShutdownReason.load(std::memory_order_relaxed));
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
    AssertWorkerThreadForRelayState();
    auto relay = FindRelayLocal(relayId);
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
    AssertWorkerThreadForRelayState();
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

std::shared_ptr<void> TqDarwinRelayWorker::DetachRelayFromActiveMapForTest(uint64_t relayId) {
    std::unique_lock<std::shared_mutex> mapAccess(RelayMapAccessMutex);
    std::lock_guard<std::mutex> lock(RelayMutex);
    const auto it = Relays.find(relayId);
    if (it == Relays.end()) {
        return nullptr;
    }
    auto relay = it->second;
    Relays.erase(it);
    return relay;
}

uint64_t TqDarwinRelayWorker::KnownSendOperationCountForTest() {
    std::lock_guard<std::mutex> lock(KnownSendMutex);
#if defined(TCPQUIC_TESTING)
    return KnownSendOperations.size() + ActiveSendOperationsSize.load(std::memory_order_relaxed);
#else
    return KnownSendOperations.size() + ActiveSendOperations.size();
#endif
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

uint64_t TqDarwinRelayWorker::InFlightQuicSendCountFromRelayForTest(
    const std::shared_ptr<void>& relayOwner) {
    auto relay = std::static_pointer_cast<RelayState>(relayOwner);
    if (relay == nullptr) {
        return 0;
    }
    std::lock_guard<std::mutex> relayLock(relay->Mutex);
    return relay->InFlightQuicSends;
}

uint64_t TqDarwinRelayWorker::PendingQuicSendCountFromRelayForTest(
    const std::shared_ptr<void>& relayOwner) {
    auto relay = std::static_pointer_cast<RelayState>(relayOwner);
    if (relay == nullptr) {
        return 0;
    }
    std::lock_guard<std::mutex> relayLock(relay->Mutex);
    return relay->PendingQuicSends.size();
}

uint64_t TqDarwinRelayWorker::CompleteOneInFlightSendForTest(uint64_t relayId) {
    TqDarwinRelaySendOperation* operation = nullptr;
    uint64_t bytes = 0;
    {
        std::lock_guard<std::mutex> lock(KnownSendMutex);
        for (const auto& entry : KnownSendOperations) {
            if (entry.first != nullptr && entry.second.RelayId == relayId) {
                operation = entry.first;
                bytes = entry.second.TotalBytes;
                break;
            }
        }
    }
    if (operation == nullptr) {
        std::lock_guard<std::mutex> activeLock(ActiveSendMutex);
        for (const auto& entry : ActiveSendOperations) {
            if (entry.first != nullptr && entry.second.RelayId == relayId) {
                operation = entry.first;
                bytes = entry.second.TotalBytes;
                break;
            }
        }
    }
    if (operation == nullptr) {
        return 0;
    }
    CompleteQuicSend(operation);
    return bytes;
}

bool TqDarwinRelayWorker::CorruptOneInFlightSendMagicForTest(uint64_t relayId) {
    {
        std::lock_guard<std::mutex> lock(KnownSendMutex);
        for (const auto& entry : KnownSendOperations) {
            if (entry.first != nullptr && entry.second.RelayId == relayId) {
                entry.first->Magic = 0;
                return true;
            }
        }
    }
    std::lock_guard<std::mutex> activeLock(ActiveSendMutex);
    for (const auto& entry : ActiveSendOperations) {
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

uint64_t TqDarwinRelayWorker::CallbackPendingReceiveBytesForTest(uint64_t relayId) {
    auto relay = FindRelay(relayId);
    if (relay == nullptr || relay->Binding == nullptr) {
        return 0;
    }
    return relay->Binding->CallbackPendingReceiveBytes.load(std::memory_order_acquire);
}

uint64_t TqDarwinRelayWorker::PendingTcpWriteBytesForTest(uint64_t relayId) {
    auto relay = FindRelay(relayId);
    if (relay == nullptr) {
        return 0;
    }
    std::lock_guard<std::mutex> relayLock(relay->Mutex);
    return relay->PendingTcpWriteBytes;
}

bool TqDarwinRelayWorker::BindingActiveForTest(uint64_t relayId) {
    std::lock_guard<std::mutex> lock(RelayMutex);
    const auto it = Relays.find(relayId);
    if (it != Relays.end() && it->second->Binding != nullptr) {
        return it->second->Binding->Active.load(std::memory_order_acquire);
    }
    for (const auto& binding : RetiredStreamBindings) {
        if (binding->RelayId == relayId) {
            return binding->Active.load(std::memory_order_acquire);
        }
    }
    return false;
}

bool TqDarwinRelayWorker::BindingTerminalForTest(uint64_t relayId) {
    std::lock_guard<std::mutex> lock(RelayMutex);
    const auto it = Relays.find(relayId);
    if (it != Relays.end() && it->second->Binding != nullptr) {
        return it->second->Binding->Terminal.load(std::memory_order_acquire);
    }
    for (const auto& binding : RetiredStreamBindings) {
        if (binding->RelayId == relayId) {
            return binding->Terminal.load(std::memory_order_acquire);
        }
    }
    return false;
}

TqDarwinBindingPublishIdentitySnapshot TqDarwinRelayWorker::BindingPublishIdentityForTest(
    uint64_t relayId) const {
    TqDarwinBindingPublishIdentitySnapshot snapshot{};
    std::shared_ptr<StreamBinding> binding;
    {
        std::lock_guard<std::mutex> lock(RelayMutex);
        const auto it = Relays.find(relayId);
        if (it != Relays.end()) {
            binding = it->second != nullptr ? it->second->Binding : nullptr;
        }
        if (binding == nullptr) {
            for (const auto& retired : RetiredStreamBindings) {
                if (retired->RelayId == relayId) {
                    binding = retired;
                    break;
                }
            }
        }
    }
    if (binding == nullptr) {
        return snapshot;
    }
    snapshot.RelayId = binding->RelayId;
    snapshot.RouteGeneration = binding->RouteGeneration;
    snapshot.ControlGeneration = binding->ControlGeneration;
    snapshot.RelayLockable = !binding->Relay.expired();
    snapshot.StreamOwnerLockable = !binding->StreamOwner.expired();
    std::lock_guard<std::mutex> guard(binding->ActivationMutex);
    snapshot.PrecommitDepth = binding->PrecommitReceives.size();
    return snapshot;
}

TqDarwinBindingPublishIdentitySnapshot TqDarwinRelayWorker::LastPublishIdentityForTest() const {
    return LastPublishIdentity;
}

size_t TqDarwinRelayWorker::PrecommitQueueDepthForTest(uint64_t relayId) const {
    return BindingPublishIdentityForTest(relayId).PrecommitDepth;
}

uint64_t TqDarwinRelayWorker::TcpFilterInstallCountForTest() const {
    return TcpFilterInstallCount.load(std::memory_order_relaxed);
}

uint64_t TqDarwinRelayWorker::TcpFilterDeleteCountForTest() const {
    return TcpFilterDeleteCount.load(std::memory_order_relaxed);
}

uint64_t TqDarwinRelayWorker::TcpFdCloseCountForTest() const {
    return TcpFdCloseCount.load(std::memory_order_relaxed);
}

uint64_t TqDarwinRelayWorker::MapPublicationCountForTest() const {
    return MapPublicationCount.load(std::memory_order_relaxed);
}

uint64_t TqDarwinRelayWorker::StreamBindingDestructorCountForTest() const {
    return StreamBindingDestructorCount.load(std::memory_order_relaxed);
}

uint64_t TqDarwinRelayWorker::RelayStateDestructorCountForTest() const {
    return RelayStateDestructorCount.load(std::memory_order_relaxed);
}

uint64_t TqDarwinRelayWorker::SendOperationDestructorCountForTest() const {
    return TqDarwinRelaySendOperation::DestructorCount.load(std::memory_order_relaxed);
}

std::shared_ptr<TqStreamLifetime> TqDarwinRelayWorker::StreamOwnerForTest(uint64_t relayId) {
    std::lock_guard<std::mutex> lock(RelayMutex);
    const auto it = Relays.find(relayId);
    if (it != Relays.end()) {
        return it->second->StreamOwner;
    }
    for (const auto& relay : RetiredRelays) {
        if (relay->Id == relayId) {
            return relay->StreamOwner;
        }
    }
    return nullptr;
}

void TqDarwinRelayWorker::MarkRelayClosingForTest(uint64_t relayId) {
    auto relay = FindRelay(relayId);
    if (relay == nullptr) {
        return;
    }
    std::lock_guard<std::mutex> lock(relay->Mutex);
    relay->Closing = true;
}

void TqDarwinRelayWorker::SetRelayStreamForTest(uint64_t relayId, MsQuicStream* stream) {
    auto relay = FindRelay(relayId);
    if (relay == nullptr) {
        return;
    }
    std::lock_guard<std::mutex> lock(relay->Mutex);
    relay->Stream = stream;
}

uint64_t TqDarwinRelayWorker::PendingQuicSendBufferBytesForTest(uint64_t relayId) {
    auto relay = FindRelay(relayId);
    if (relay == nullptr) {
        return 0;
    }
    std::lock_guard<std::mutex> lock(relay->Mutex);
    return relay->TcpReadBuffers.PendingBufferBytes.load(std::memory_order_relaxed);
}

uint64_t TqDarwinRelayWorker::RetiredStreamBindingCountForTest() {
    std::lock_guard<std::mutex> lock(RelayMutex);
    return RetiredStreamBindings.size();
}

uint64_t TqDarwinRelayWorker::RetiredRelayCountForTest() {
    std::lock_guard<std::mutex> lock(RelayMutex);
    return RetiredRelays.size();
}

std::shared_ptr<void> TqDarwinRelayWorker::RetiredRelayOwnerForTest(uint64_t relayId) {
    std::lock_guard<std::mutex> lock(RelayMutex);
    for (const auto& relay : RetiredRelays) {
        if (relay->Id == relayId) {
            return relay;
        }
    }
    return nullptr;
}

std::shared_ptr<void> TqDarwinRelayWorker::ActiveRelayOwnerForTest(uint64_t relayId) {
    std::lock_guard<std::mutex> lock(RelayMutex);
    const auto it = Relays.find(relayId);
    if (it == Relays.end()) {
        return nullptr;
    }
    return it->second;
}

bool TqDarwinRelayWorker::EnqueueRelayCloseEventForTest(
    const std::shared_ptr<void>& relayOwner,
    TqDarwinRelayEventType type,
    uint64_t relayId) {
    TqDarwinRelayEvent event{};
    event.Type = type;
    event.RelayId = relayId;
    event.RelayOwner = relayOwner;
    return EnqueueForTest(std::move(event));
}

#if defined(TQ_UNIT_TESTING)
bool TqDarwinRelayWorker::EnqueueDetachedSnapshotCommandForTest(
    TqRelayRuntimeSnapshotExecutionGate::Permit permit) {
    auto command = std::make_shared<SnapshotCommand>();
    command->Permit = std::move(permit);
    command->State = SnapshotCommandState::Detached;
    TqDarwinRelayEvent event{};
    event.Type = TqDarwinRelayEventType::Snapshot;
    event.Control = command.get();
    event.ControlOwner = command;
    return EnqueueForTest(std::move(event));
}
#endif

MsQuicStream* TqDarwinRelayWorker::RelayStreamForTest(uint64_t relayId) {
    std::lock_guard<std::mutex> lock(RelayMutex);
    const auto it = Relays.find(relayId);
    if (it != Relays.end()) {
        std::lock_guard<std::mutex> relayLock(it->second->Mutex);
        return it->second->Stream;
    }
    for (const auto& relay : RetiredRelays) {
        if (relay->Id == relayId) {
            std::lock_guard<std::mutex> relayLock(relay->Mutex);
            return relay->Stream;
        }
    }
    return nullptr;
}

uint32_t TqDarwinRelayWorker::BindingCallbackRefsForTest(uint64_t relayId) {
    std::lock_guard<std::mutex> lock(RelayMutex);
    const auto it = Relays.find(relayId);
    if (it != Relays.end() && it->second->Binding != nullptr) {
        return it->second->Binding->CallbackRefs.load(std::memory_order_acquire);
    }
    for (const auto& binding : RetiredStreamBindings) {
        if (binding->RelayId == relayId) {
            return binding->CallbackRefs.load(std::memory_order_acquire);
        }
    }
    return 0;
}

bool TqDarwinRelayWorker::PeerSendShutdownStickyForTest(uint64_t relayId) {
    const auto relay = FindRelayLocal(relayId);
    if (relay == nullptr || relay->Binding == nullptr) {
        return false;
    }
    return relay->Binding->PeerSendShutdownSticky.load(std::memory_order_acquire);
}

bool TqDarwinRelayWorker::TcpWriteShutdownQueuedOrClosedForTest(uint64_t relayId) {
    const auto relay = FindRelayLocal(relayId);
    if (relay == nullptr) {
        return false;
    }
    return relay->TcpWriteShutdownQueued || relay->TcpWriteClosed;
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
    const std::shared_ptr<SnapshotCommand>& command,
    TqDarwinRelayWorkerSnapshot result) {
    if (!command) {
        return;
    }
    TqRelayRuntimeSnapshotExecutionGate::Permit releasedPermit;
    {
        std::lock_guard<std::mutex> lock(command->Mutex);
        if (command->State == SnapshotCommandState::Completed ||
            command->State == SnapshotCommandState::Cancelled) {
            return;
        }
        const uint32_t workerIndex = result.WorkerIndex;
        const uint64_t eventQueueCapacity = result.EventQueueCapacity;
        try {
            command->Result = std::move(result);
        } catch (...) {
            TqDarwinRelayWorkerSnapshot incomplete{};
            incomplete.WorkerIndex = workerIndex;
            incomplete.EventQueueCapacity = eventQueueCapacity;
            incomplete.SnapshotComplete = false;
            command->Result = std::move(incomplete);
        }
        command->State = SnapshotCommandState::Completed;
        releasedPermit = std::move(command->Permit);
    }
    command->Cv.notify_all();
}

void TqDarwinRelayWorker::CancelSnapshotCommand(
    const std::shared_ptr<SnapshotCommand>& command) {
    if (!command) {
        return;
    }
    TqRelayRuntimeSnapshotExecutionGate::Permit releasedPermit;
    {
        std::lock_guard<std::mutex> lock(command->Mutex);
        if (command->State == SnapshotCommandState::Completed ||
            command->State == SnapshotCommandState::Cancelled) {
            return;
        }
        command->State = SnapshotCommandState::Cancelled;
        releasedPermit = std::move(command->Permit);
    }
    command->Cv.notify_all();
}

std::shared_ptr<TqDarwinRelayWorker::SnapshotCommand>
TqDarwinRelayWorker::SnapshotCommandFromEvent(const TqDarwinRelayEvent& event) {
    if (event.ControlOwner) {
        return std::static_pointer_cast<SnapshotCommand>(event.ControlOwner);
    }
    if (event.Control != nullptr) {
        // Legacy raw-only path should not own Snapshot lifetime; refuse to
        // fabricate a shared_ptr that would delete a stack object.
        return {};
    }
    return {};
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
        case TqDarwinRelayEventType::QuicActiveShutdown:
            if (auto relay = std::static_pointer_cast<RelayState>(event.RelayOwner)) {
                CloseRelay(relay, TqDarwinRelayCloseDisposition::ActiveShutdown);
            }
            break;
        case TqDarwinRelayEventType::QuicPeerSendShutdown:
            if (auto relay = std::static_pointer_cast<RelayState>(event.RelayOwner)) {
                ProcessPeerSendShutdown(relay);
            }
            break;
        case TqDarwinRelayEventType::QuicSendShutdownComplete:
            if (auto relay = std::static_pointer_cast<RelayState>(event.RelayOwner)) {
                ProcessSendShutdownComplete(relay);
            }
            break;
        case TqDarwinRelayEventType::QuicShutdownComplete:
            if (auto relay = std::static_pointer_cast<RelayState>(event.RelayOwner)) {
                CloseRelay(relay, TqDarwinRelayCloseDisposition::TerminalLogicalDetach);
            }
            break;
        case TqDarwinRelayEventType::QuicIdealSendBuffer:
            if (auto relay = FindRelayLocal(event.RelayId)) {
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
            auto command = SnapshotCommandFromEvent(event);
            if (!command) {
                Errors.fetch_add(1, std::memory_order_relaxed);
                break;
            }
            bool cancelled = false;
            {
                std::lock_guard<std::mutex> lock(command->Mutex);
                cancelled = command->State == SnapshotCommandState::Cancelled;
            }
            if (cancelled) {
                CancelSnapshotCommand(command);
                break;
            }
            try {
#if defined(TQ_UNIT_TESTING)
                if (Config.BeforeSnapshotHookForTest != nullptr) {
                    Config.BeforeSnapshotHookForTest(this);
                }
#endif
                CompleteSnapshotCommand(command, SnapshotLocal());
            } catch (...) {
                // Control-plane sampling must never kill the worker thread.
                Errors.fetch_add(1, std::memory_order_relaxed);
                CompleteSnapshotCommand(command, MakeIncompleteSnapshot());
            }
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
    FlushAllCallbackPendingQuicReceivesLocal();
    FlushHalfCloseStickiesLocal();
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
        case TqDarwinRelayEventType::QuicActiveShutdown:
            if (auto relay = std::static_pointer_cast<RelayState>(event.RelayOwner)) {
                CloseRelay(relay, TqDarwinRelayCloseDisposition::ActiveShutdown);
            }
            break;
        case TqDarwinRelayEventType::QuicPeerSendShutdown:
        case TqDarwinRelayEventType::QuicSendShutdownComplete:
        case TqDarwinRelayEventType::QuicIdealSendBuffer:
            // Half-close / ideal-send hints are worker-thread-only data-plane
            // updates. Stop already requested AbortBoth on active relays.
            break;
        case TqDarwinRelayEventType::QuicShutdownComplete:
            if (auto relay = std::static_pointer_cast<RelayState>(event.RelayOwner)) {
                CloseRelay(relay, TqDarwinRelayCloseDisposition::TerminalLogicalDetach);
            }
            break;
        case TqDarwinRelayEventType::RegisterRelay:
            CompleteRegisterCommand(static_cast<RegisterRelayCommand*>(event.Control), {});
            break;
        case TqDarwinRelayEventType::UnregisterRelay:
            CompleteUnregisterCommand(static_cast<UnregisterRelayCommand*>(event.Control));
            break;
        case TqDarwinRelayEventType::Snapshot:
            CancelSnapshotCommand(SnapshotCommandFromEvent(event));
            break;
        default:
            break;
        }
        event = TqDarwinRelayEvent{};
    }
    if (processed > 0) {
        EventsProcessed.fetch_add(processed, std::memory_order_relaxed);
        WorkerExitedPurgeEvents.fetch_add(processed, std::memory_order_relaxed);
    }
}

void TqDarwinRelayWorker::SettleQueuedReceiveViewsBeforeRetire() {
    std::vector<TqDarwinRelayEvent> kept;
    TqDarwinRelayEvent event{};
    uint32_t settled = 0;
    while (EventQueue.TryPop(event)) {
        if (event.Type == TqDarwinRelayEventType::QuicReceiveView) {
            // Discard releases callback budget; MsQuic complete needs StreamOwner lease.
            (void)DiscardDeferredQuicReceive(nullptr, event.ReceiveView);
            ++settled;
            continue;
        }
        kept.push_back(std::move(event));
    }
    for (auto& keptEvent : kept) {
        if (!EventQueue.TryPush(std::move(keptEvent))) {
            Errors.fetch_add(1, std::memory_order_relaxed);
        }
    }
    if (settled > 0) {
        EventsProcessed.fetch_add(settled, std::memory_order_relaxed);
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
    DetachActiveSendOperationsForStop();
#if defined(TCPQUIC_TESTING)
    if (Config.BeforeWorkerExitHookForTest != nullptr) {
        Config.BeforeWorkerExitHookForTest(this);
    }
#endif
    {
        std::lock_guard<std::mutex> lock(WorkerThreadIdMutex);
        WorkerThreadId = std::thread::id{};
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
    if (kevent(KqueueFd, changes, 2, nullptr, 0, nullptr) != 0) {
        return false;
    }
#if defined(TCPQUIC_TESTING)
    TcpFilterInstallCount.fetch_add(1, std::memory_order_relaxed);
#endif
    return true;
}

bool TqDarwinRelayWorker::InstallInactiveTcpFilters(const std::shared_ptr<RelayState>& relay) {
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
        EV_ADD | EV_DISABLE,
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
    if (kevent(KqueueFd, changes, 2, nullptr, 0, nullptr) != 0) {
        return false;
    }
#if defined(TCPQUIC_TESTING)
    TcpFilterInstallCount.fetch_add(1, std::memory_order_relaxed);
#endif
    return true;
}

bool TqDarwinRelayWorker::EnableTcpFilters(const std::shared_ptr<RelayState>& relay) {
    return UpdateTcpInterest(relay);
}

// Lifecycle/fallback helper: worker data-plane paths must use UpdateTcpInterestLocal().
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

bool TqDarwinRelayWorker::UpdateTcpInterestLocal(const std::shared_ptr<RelayState>& relay) {
    if (relay == nullptr || KqueueFd < 0) {
        return false;
    }
    AssertWorkerThreadForRelayState();

    const bool readArmed = relay->TcpReadArmed;
    const bool writeArmed = relay->TcpWriteArmed;

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
    if (kevent(KqueueFd, changes, 2, nullptr, 0, nullptr) == 0) {
#if defined(TCPQUIC_TESTING)
        TcpFilterDeleteCount.fetch_add(1, std::memory_order_relaxed);
#endif
    }
}

void TqDarwinRelayWorker::CloseRelayTcpFdOnce(const std::shared_ptr<RelayState>& relay) {
    if (relay == nullptr) {
        return;
    }
    TqSocketHandle fd = TqInvalidSocket;
    {
        std::lock_guard<std::mutex> relayLock(relay->Mutex);
        if (!TqSocketValid(relay->TcpFd)) {
            return;
        }
        fd = relay->TcpFd;
        relay->TcpFd = TqInvalidSocket;
    }
    TqCloseSocket(fd);
#if defined(TCPQUIC_TESTING)
    TcpFdCloseCount.fetch_add(1, std::memory_order_relaxed);
#endif
}

#if defined(TCPQUIC_TESTING)
namespace {
thread_local int g_darwinRelayMutexDepthForTest = 0;

struct TqDarwinRelayMutexDepthGuard {
    TqDarwinRelayMutexDepthGuard() noexcept { ++g_darwinRelayMutexDepthForTest; }
    ~TqDarwinRelayMutexDepthGuard() noexcept { --g_darwinRelayMutexDepthForTest; }
    TqDarwinRelayMutexDepthGuard(const TqDarwinRelayMutexDepthGuard&) = delete;
    TqDarwinRelayMutexDepthGuard& operator=(const TqDarwinRelayMutexDepthGuard&) = delete;
};
} // namespace
#endif

void TqDarwinRelayWorker::RetireRelay(
    const std::shared_ptr<RelayState>& relay,
    TqDarwinRelayCloseDisposition disposition) {
    if (relay == nullptr) {
        return;
    }

    const bool terminalDetach =
        disposition == TqDarwinRelayCloseDisposition::TerminalLogicalDetach;
    if (terminalDetach) {
        bool expected = false;
        if (!relay->LogicallyDetached.compare_exchange_strong(
                expected,
                true,
                std::memory_order_acq_rel)) {
            return;
        }
    } else {
        if (relay->LogicallyDetached.load(std::memory_order_acquire)) {
            return;
        }
        bool expected = false;
        if (!relay->RetiredToWorker.compare_exchange_strong(
                expected,
                true,
                std::memory_order_acq_rel)) {
            return;
        }
    }

    std::shared_ptr<StreamBinding> binding;
    MsQuicStream* legacyStreamForCallbackClear = nullptr;
    std::deque<std::shared_ptr<TqDarwinPendingQuicReceive>> receivesToDiscard;
    // Lifecycle cleanup may run after worker exit or during unregister fallback; data-plane paths must not enter here for normal forwarding.
    {
        std::unique_lock<std::mutex> relayLock(relay->Mutex);
#if defined(TCPQUIC_TESTING)
        TqDarwinRelayMutexDepthGuard mutexDepthGuard;
#endif
        relay->Closing = true;
        relay->TcpReadClosed = true;
        relay->TcpWriteClosed = true;
        relay->QuicReceiveClosed = true;
        relay->TcpReadArmed = false;
        relay->TcpWriteArmed = false;
        if (TqSocketValid(relay->TcpFd)) {
            TqCloseSocket(relay->TcpFd);
            relay->TcpFd = TqInvalidSocket;
#if defined(TCPQUIC_TESTING)
            TcpFdCloseCount.fetch_add(1, std::memory_order_relaxed);
#endif
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
        // Snapshot under lock only — Discard/Complete must run outside
        // relay->Mutex (non-recursive) to avoid reentrancy/deadlock when
        // ReceiveComplete or lease paths touch relay state.
        if (!terminalDetach && relay->StreamOwner == nullptr && relay->Stream != nullptr) {
            const auto completions = binding != nullptr ? binding->Completions : nullptr;
            const bool hasKnownOperations = completions != nullptr &&
                completions->KnownSendOperationCount.load(std::memory_order_acquire) != 0;
            if (!hasKnownOperations && relay->InFlightQuicSends == 0) {
#if defined(TCPQUIC_TESTING)
                if (TqDarwinRelayStreamUsableForTest(relay->Stream)) {
                    legacyStreamForCallbackClear = relay->Stream;
                }
#endif
            }
        }
        relay->Binding.reset();
        relay->Stream = nullptr;
        if (terminalDetach) {
            if (relay->StreamOwner != nullptr) {
                relay->StreamOwner.reset();
                relay->TerminalOwnerReleased.store(true, std::memory_order_release);
            }
        }
    }
    if (legacyStreamForCallbackClear != nullptr) {
        legacyStreamForCallbackClear->Callback = MsQuicStream::NoOpCallback;
        legacyStreamForCallbackClear->Context = nullptr;
    }
    for (const auto& receive : receivesToDiscard) {
        // Prefer receive->StreamOwner lease; pass relay only for bookkeeping.
        (void)DiscardDeferredQuicReceive(relay, receive);
    }
    bool retireBinding = false;
    if (binding != nullptr) {
        std::deque<std::shared_ptr<TqDarwinPendingQuicReceive>> callbackPendingReceives;
        {
            std::lock_guard<std::mutex> guard(binding->CallbackPendingQuicReceivesMutex);
            callbackPendingReceives.swap(binding->CallbackPendingQuicReceives);
        }
        for (const auto& receive : callbackPendingReceives) {
            ReleaseCallbackReceiveBudget(receive);
            (void)DiscardDeferredQuicReceive(nullptr, receive);
        }
        binding->Active.store(false, std::memory_order_release);
        // Do not reset binding->Relay after publish (P1-4): weak expiry follows
        // RelayState destruction naturally.
        bool bindingExpected = false;
        retireBinding = binding->Retired.compare_exchange_strong(
            bindingExpected,
            true,
            std::memory_order_acq_rel);
        const auto completions = binding->Completions;
        if (terminalDetach) {
            const bool hasKnownOperations = completions != nullptr &&
                completions->KnownSendOperationCount.load(std::memory_order_acquire) != 0;
            if (!hasKnownOperations) {
                binding->Worker.store(nullptr, std::memory_order_release);
            }
        }
    }
    std::unique_lock<std::shared_mutex> mapAccess(RelayMapAccessMutex);
    std::lock_guard<std::mutex> lock(RelayMutex);
    uint64_t inFlight = 0;
    {
        std::lock_guard<std::mutex> relayLock(relay->Mutex);
        inFlight = relay->InFlightQuicSends;
    }
    const bool waitingTerminalOwner =
        relay->StreamOwner != nullptr &&
        !relay->TerminalOwnerReleased.load(std::memory_order_acquire);
    if (inFlight != 0 || waitingTerminalOwner) {
        bool alreadyRetired = false;
        for (const auto& retired : RetiredRelays) {
            if (retired == relay) {
                alreadyRetired = true;
                break;
            }
        }
        if (!alreadyRetired) {
            relay->RetiredAt = std::chrono::steady_clock::now();
            RetiredRelays.push_back(relay);
        }
    }
    if (binding != nullptr && retireBinding) {
        RetiredStreamBindings.push_back(std::move(binding));
    }
}

void TqDarwinRelayWorker::CloseRelay(
    const std::shared_ptr<RelayState>& relay,
    TqDarwinRelayCloseDisposition disposition) {
    if (relay == nullptr) {
        return;
    }

    if (disposition == TqDarwinRelayCloseDisposition::TerminalLogicalDetach &&
        relay->LogicallyDetached.load(std::memory_order_acquire)) {
        return;
    }

    bool removed = false;
    // Lifecycle cleanup may run after worker exit or during unregister fallback; data-plane paths must not enter here for normal forwarding.
    {
        std::unique_lock<std::shared_mutex> mapAccess(RelayMapAccessMutex);
        std::lock_guard<std::mutex> lock(RelayMutex);
        const auto it = Relays.find(relay->Id);
        if (it != Relays.end()) {
            if (it->second != relay) {
                return;
            }
            Relays.erase(it);
            removed = true;
        } else if (disposition == TqDarwinRelayCloseDisposition::TerminalLogicalDetach) {
            bool foundRetired = false;
            for (const auto& retired : RetiredRelays) {
                if (retired == relay) {
                    foundRetired = true;
                    break;
                }
            }
            if (!foundRetired) {
                return;
            }
        }
    }
    if (disposition == TqDarwinRelayCloseDisposition::ActiveShutdown && !removed) {
        return;
    }
    if (removed) {
        RemoveTcpFilters(relay);
        if (disposition == TqDarwinRelayCloseDisposition::ActiveShutdown) {
            RequestRelayShutdown(relay, TqStreamLifetime::ShutdownIntent::AbortBoth);
        }
    }
    RetireRelay(relay, disposition);
    PurgeRetiredRelaysIfSafe();
}

void TqDarwinRelayWorker::PurgeRetiredRelaysIfSafe() {
#if defined(TCPQUIC_TESTING)
    RetiredRelayPurgeCount.fetch_add(1, std::memory_order_relaxed);
#endif
    // Lifecycle cleanup may run after worker exit or during unregister fallback; data-plane paths must not enter here for normal forwarding.
    std::unique_lock<std::shared_mutex> mapAccess(RelayMapAccessMutex);
    std::lock_guard<std::mutex> lock(RelayMutex);
    for (auto it = RetiredRelays.begin(); it != RetiredRelays.end();) {
        uint64_t sends = 0;
        bool waitingTerminalOwner = false;
        {
            std::lock_guard<std::mutex> relayLock((*it)->Mutex);
            sends = (*it)->InFlightQuicSends;
            waitingTerminalOwner =
                (*it)->StreamOwner != nullptr &&
                !(*it)->TerminalOwnerReleased.load(std::memory_order_acquire);
        }
        if (sends == 0 && !waitingTerminalOwner) {
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
        bool hasCallbackPendingQuicReceives = false;
        {
            std::lock_guard<std::mutex> guard((*it)->CallbackPendingQuicReceivesMutex);
            hasCallbackPendingQuicReceives = !(*it)->CallbackPendingQuicReceives.empty();
        }
        bool waitingTerminalOwner = false;
        for (const auto& retired : RetiredRelays) {
            if (retired->Id == (*it)->RelayId &&
                retired->StreamOwner != nullptr &&
                !retired->TerminalOwnerReleased.load(std::memory_order_acquire) &&
                !retired->LogicallyDetached.load(std::memory_order_acquire)) {
                waitingTerminalOwner = true;
                break;
            }
        }
        if ((*it)->CallbackRefs.load(std::memory_order_acquire) == 0 && !hasKnownOperations &&
            !hasCallbackReceives && !hasCallbackPendingQuicReceives && !waitingTerminalOwner) {
            (*it)->Worker.store(nullptr, std::memory_order_release);
            it = RetiredStreamBindings.erase(it);
        } else {
            ++it;
        }
    }
}

void TqDarwinRelayWorker::DetachActiveSendOperationsForStop() {
    AssertWorkerThreadForRelayState();
    std::unordered_map<TqDarwinRelaySendOperation*, KnownSendOperationInfo> activeOperations;
    {
        std::lock_guard<std::mutex> lock(ActiveSendMutex);
        activeOperations.swap(ActiveSendOperations);
    }
#if defined(TCPQUIC_TESTING)
    ActiveSendOperationsSize.store(0, std::memory_order_relaxed);
#endif
    for (auto& entry : activeOperations) {
        TqDarwinRelaySendOperation* operation = entry.first;
        KnownSendOperationInfo info = entry.second;
        if (operation != nullptr) {
            (void)operation->MarkDetached();
        }
        if (auto binding = std::static_pointer_cast<StreamBinding>(info.BindingOwner)) {
            if (binding->Completions != nullptr) {
#if defined(TCPQUIC_TESTING)
                CompletionStateLockedCount.fetch_add(1, std::memory_order_relaxed);
#endif
                std::lock_guard<std::mutex> completionLock(binding->Completions->Mutex);
                binding->Completions->FallbackSendOperations.emplace(operation, info);
            }
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
#if defined(TCPQUIC_TESTING)
        KnownSendLockedCount.fetch_add(1, std::memory_order_relaxed);
#endif
        std::lock_guard<std::mutex> lock(KnownSendMutex);
        KnownSendOperations.emplace(operation, info);
    }
    if (binding != nullptr && binding->Completions != nullptr) {
#if defined(TCPQUIC_TESTING)
        CompletionStateLockedCount.fetch_add(1, std::memory_order_relaxed);
#endif
        std::lock_guard<std::mutex> completionLock(binding->Completions->Mutex);
        binding->Completions->FallbackSendOperations.emplace(operation, info);
        binding->Completions->KnownSendOperationCount.fetch_add(1, std::memory_order_acq_rel);
    }
}

void TqDarwinRelayWorker::RegisterKnownSendOperationLocal(
    TqDarwinRelaySendOperation* operation,
    const KnownSendOperationInfo& info) {
    AssertWorkerThreadForRelayState();
#if defined(TCPQUIC_TESTING)
    ActiveSendLocalRegisterCount.fetch_add(1, std::memory_order_relaxed);
#endif
    {
        std::lock_guard<std::mutex> lock(ActiveSendMutex);
        ActiveSendOperations.emplace(operation, info);
    }
#if defined(TCPQUIC_TESTING)
    ActiveSendOperationsSize.fetch_add(1, std::memory_order_relaxed);
#endif
    if (auto binding = std::static_pointer_cast<StreamBinding>(info.BindingOwner)) {
        if (binding->Completions != nullptr) {
            binding->Completions->KnownSendOperationCount.fetch_add(1, std::memory_order_acq_rel);
        }
    }
}

bool TqDarwinRelayWorker::MarkKnownSendOperationSubmitted(
    TqDarwinRelaySendOperation* operation,
    KnownSendOperationInfo* info) {
    std::shared_ptr<CompletionState> completions;
    {
#if defined(TCPQUIC_TESTING)
        KnownSendLockedCount.fetch_add(1, std::memory_order_relaxed);
#endif
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
#if defined(TCPQUIC_TESTING)
        CompletionStateLockedCount.fetch_add(1, std::memory_order_relaxed);
#endif
        std::lock_guard<std::mutex> completionLock(completions->Mutex);
        const auto it = completions->FallbackSendOperations.find(operation);
        if (it != completions->FallbackSendOperations.end()) {
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
    AssertWorkerThreadForRelayState();
    {
        std::lock_guard<std::mutex> lock(ActiveSendMutex);
        const auto it = ActiveSendOperations.find(operation);
        if (it == ActiveSendOperations.end()) {
            return false;
        }
        it->second.Submitting = false;
        if (info != nullptr) {
            *info = it->second;
        }
    }
    return true;
}

bool TqDarwinRelayWorker::TryClaimKnownSendCompletionEvent(
    TqDarwinRelaySendOperation* operation,
    KnownSendOperationInfo* info) {
    if (operation == nullptr) {
        return false;
    }
    bool tracked = false;
    {
        std::lock_guard<std::mutex> lock(ActiveSendMutex);
        tracked = ActiveSendOperations.find(operation) != ActiveSendOperations.end();
    }
    if (!tracked) {
        return false;
    }
    if (operation->IsCompletionClaimed()) {
        if (info != nullptr) {
            info->RelayId = operation->CompletionRelayId;
            info->TotalBytes = operation->CompletionTotalBytes;
            info->Fin = operation->CompletionFin;
            info->Submitting = !operation->IsSubmitted();
            info->CompletionEventClaimed = true;
            info->BindingOwner = operation->CompletionBindingOwner;
        }
        return true;
    }
    if (!operation->TryClaimCompletion()) {
        return false;
    }
    if (info != nullptr) {
        info->RelayId = operation->CompletionRelayId;
        info->TotalBytes = operation->CompletionTotalBytes;
        info->Fin = operation->CompletionFin;
        info->Submitting = !operation->IsSubmitted();
        info->CompletionEventClaimed = false;
        info->BindingOwner = operation->CompletionBindingOwner;
    }
    return true;
}

bool TqDarwinRelayWorker::UnregisterCompletionStateOperation(
    const std::shared_ptr<CompletionState>& state,
    TqDarwinRelaySendOperation* operation,
    KnownSendOperationInfo* info,
    TqDarwinRelayWorker* testingWorker) {
    if (state == nullptr) {
        return false;
    }
#if defined(TCPQUIC_TESTING)
    if (testingWorker != nullptr) {
        testingWorker->CompletionStateLockedCount.fetch_add(1, std::memory_order_relaxed);
    }
#else
    (void)testingWorker;
#endif
    std::lock_guard<std::mutex> completionLock(state->Mutex);
    const auto it = state->FallbackSendOperations.find(operation);
    if (it == state->FallbackSendOperations.end()) {
        return false;
    }
    KnownSendOperationInfo localInfo = it->second;
    if (info != nullptr) {
        *info = localInfo;
    }
    state->FallbackSendOperations.erase(it);
    state->KnownSendOperationCount.fetch_sub(1, std::memory_order_acq_rel);
    return true;
}

bool TqDarwinRelayWorker::UnregisterKnownSendOperation(
    TqDarwinRelaySendOperation* operation,
    KnownSendOperationInfo* info) {
    KnownSendOperationInfo localInfo{};
    bool removedFromKnown = false;
    bool removedFromActive = false;
    {
#if defined(TCPQUIC_TESTING)
        KnownSendLockedCount.fetch_add(1, std::memory_order_relaxed);
#endif
        std::lock_guard<std::mutex> lock(KnownSendMutex);
        const auto it = KnownSendOperations.find(operation);
        if (it != KnownSendOperations.end()) {
            localInfo = it->second;
            KnownSendOperations.erase(it);
            removedFromKnown = true;
        } else if (WorkerThreadExited()) {
            std::lock_guard<std::mutex> activeLock(ActiveSendMutex);
            const auto activeIt = ActiveSendOperations.find(operation);
            if (activeIt != ActiveSendOperations.end()) {
                localInfo = activeIt->second;
                ActiveSendOperations.erase(activeIt);
#if defined(TCPQUIC_TESTING)
                ActiveSendOperationsSize.fetch_sub(1, std::memory_order_relaxed);
#endif
                removedFromActive = true;
            }
        }
    }
    if (!removedFromKnown && !removedFromActive) {
        return false;
    }
    if (info != nullptr) {
        *info = localInfo;
    }
    if (auto binding = std::static_pointer_cast<StreamBinding>(localInfo.BindingOwner)) {
        if (removedFromKnown) {
            (void)UnregisterCompletionStateOperation(binding->Completions, operation, nullptr, this);
        } else {
            const auto completions = binding->Completions;
            if (completions != nullptr) {
                completions->KnownSendOperationCount.fetch_sub(1, std::memory_order_acq_rel);
            }
        }
    }
    return true;
}

bool TqDarwinRelayWorker::UnregisterKnownSendOperationLocal(
    TqDarwinRelaySendOperation* operation,
    KnownSendOperationInfo* info) {
    AssertWorkerThreadForRelayState();
    KnownSendOperationInfo localInfo{};
    {
        std::lock_guard<std::mutex> lock(ActiveSendMutex);
        const auto it = ActiveSendOperations.find(operation);
        if (it == ActiveSendOperations.end()) {
            return false;
        }
        localInfo = it->second;
        ActiveSendOperations.erase(it);
    }
#if defined(TCPQUIC_TESTING)
    ActiveSendOperationsSize.fetch_sub(1, std::memory_order_relaxed);
#endif
    if (info != nullptr) {
        *info = localInfo;
    }
    if (auto binding = std::static_pointer_cast<StreamBinding>(localInfo.BindingOwner)) {
        const auto completions = binding->Completions;
        if (completions != nullptr) {
            completions->KnownSendOperationCount.fetch_sub(1, std::memory_order_acq_rel);
        }
    }
#if defined(TCPQUIC_TESTING)
    ActiveSendLocalCompleteCount.fetch_add(1, std::memory_order_relaxed);
#endif
    return true;
}

bool TqDarwinRelayWorker::CompleteDetachedQuicSend(StreamBinding* binding, TqDarwinRelaySendOperation* operation) {
    if (binding == nullptr || operation == nullptr) {
        return false;
    }
    TqDarwinRelayWorker* worker = binding->Worker.load(std::memory_order_acquire);
    KnownSendOperationInfo info{};
    if (!UnregisterCompletionStateOperation(binding->Completions, operation, &info, worker)) {
        return false;
    }
    if (!operation->TryMarkCompleted()) {
        return true;
    }
    if (auto relay = binding->Relay.lock()) {
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
    }
#if defined(TCPQUIC_TESTING)
    if (worker != nullptr) {
        worker->FallbackSendCompletionCount.fetch_add(1, std::memory_order_relaxed);
    }
#endif
    delete operation;
    return true;
}

std::shared_ptr<TqDarwinRelayWorker::RelayState> TqDarwinRelayWorker::FindRelay(uint64_t relayId) {
#if defined(TCPQUIC_TESTING)
    FindRelayLockedCount.fetch_add(1, std::memory_order_relaxed);
#endif
    std::lock_guard<std::mutex> lock(RelayMutex);
    const auto it = Relays.find(relayId);
    if (it != Relays.end()) {
        return it->second;
    }
    return nullptr;
}

std::shared_ptr<TqDarwinRelayWorker::RelayState> TqDarwinRelayWorker::FindRelayLocal(uint64_t relayId) const {
    AssertWorkerThreadForRelayState();
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
    for (const auto& relay : RetiredRelays) {
        if (relay->Id == relayId) {
            return relay;
        }
    }
    return nullptr;
}

std::shared_ptr<TqDarwinRelayWorker::RelayState> TqDarwinRelayWorker::FindRetiredRelayLocal(uint64_t relayId) const {
    AssertWorkerThreadForRelayState();
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
    AssertWorkerThreadForRelayState();
    std::shared_ptr<RelayState> relay = FindRelayLocal(relayId);
    if (relay == nullptr) {
        return;
    }

    if ((flags & EV_ERROR) != 0) {
        Errors.fetch_add(1, std::memory_order_relaxed);
        CloseRelay(relay, TqDarwinRelayCloseDisposition::ActiveShutdown);
        return;
    }

    if (filter == EVFILT_READ) {
        if (!DrainTcpReadable(relay)) {
            CloseRelay(relay, TqDarwinRelayCloseDisposition::ActiveShutdown);
        }
        return;
    }
    if (filter == EVFILT_WRITE) {
        if (!FlushTcpWrites(relay)) {
            CloseRelay(relay, TqDarwinRelayCloseDisposition::ActiveShutdown);
        }
        return;
    }
}

bool TqDarwinRelayWorker::DrainTcpReadable(const std::shared_ptr<RelayState>& relay) {
    if (relay == nullptr) {
        return true;
    }
    AssertWorkerThreadForRelayState();
    if (relay->Closing || relay->TcpReadClosed) {
        return true;
    }
    if (relay->Binding != nullptr && !relay->Binding->Active.load(std::memory_order_acquire)) {
        return true;
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
                    if (iov.empty()) {
                        return SetTcpReadBackpressure(relay, true);
                    }
                    break;
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
            relay->TcpReadBytes += static_cast<uint64_t>(received);
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
            if (relay->Closing || relay->Binding == nullptr ||
                !relay->Binding->Active.load(std::memory_order_acquire)) {
                return true;
            }
            bool shouldPause = Config.MaxInFlightQuicSends != 0 &&
                relay->InFlightQuicSends >= Config.MaxInFlightQuicSends;
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
            relay->TcpReadClosed = true;
            relay->TcpReadArmed = false;
            if (!UpdateTcpInterestLocal(relay)) {
                Errors.fetch_add(1, std::memory_order_relaxed);
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
            const bool submitted = SubmitTcpBatchToQuic(relay, std::move(finViews), true);
            if (submitted) {
                TraceHalfClose(*relay, "tcp_eof");
            }
            return submitted;
        }
        if (errno == EINTR) {
            continue;
        }
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            break;
        }
        Errors.fetch_add(1, std::memory_order_relaxed);
        relay->Closing = true;
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
    AssertWorkerThreadForRelayState();
    if (relay->Binding != nullptr && !relay->Binding->Active.load(std::memory_order_acquire)) {
        return true;
    }
    if (relay->Closing) {
        return true;
    }
    if (fin) {
        relay->QuicSendFinSubmitted = true;
    }
    const bool enableQuicSends = relay->EnableQuicSends;
    if (views.empty() && !fin) {
        return true;
    }
    if (!enableQuicSends) {
        if (fin) {
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
    const uint64_t relayId = relay->Id;
    std::shared_ptr<StreamBinding> binding = relay->Binding;
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
    AssertWorkerThreadForRelayState();
    if (relay->Closing) {
        return true;
    }

    // Fixed establish order: lease -> reservation -> (later) known-op + in-flight.
    // Pre-submit returns rely on reservation RAII; do not scatter CancelSendCompletion.
    TqStreamLifetime::ApiLease lease;
    TqStreamLifetime::SendCompletionReservation reservation;
    void* completionKey = raw;
    if (relay->StreamOwner != nullptr) {
        lease = relay->StreamOwner->TryAcquireApi();
        if (!lease) {
            Errors.fetch_add(1, std::memory_order_relaxed);
            return false;
        }
        stream = lease.Stream();
        reservation = relay->StreamOwner->ReserveSendCompletion(raw);
        if (!reservation) {
            Errors.fetch_add(1, std::memory_order_relaxed);
            return false;
        }
        completionKey = reservation.Key();
#if defined(TCPQUIC_TESTING)
        // Injectable post-register registry reject: reservation stays armed so
        // RAII CancelSendCompletion restores the completion map on return.
        if (Config.FailNextSendCompletionRegisterForTest) {
            Config.FailNextSendCompletionRegisterForTest = false;
            Errors.fetch_add(1, std::memory_order_relaxed);
            return false;
        }
        if (Config.AfterRegisterSendCompletionHookForTest != nullptr) {
            Config.AfterRegisterSendCompletionHookForTest(this, relay->Id, raw);
        }
#endif
    } else if (relay->Stream == nullptr) {
        return false;
    } else {
        stream = relay->Stream;
    }

    std::shared_ptr<StreamBinding> binding = relay->Binding;
    if (binding == nullptr || !binding->Active.load(std::memory_order_acquire)) {
        return true;
    }
    info.BindingOwner = binding;
    raw->BindingOwner = binding;
    raw->CompletionRelayId = info.RelayId;
    raw->CompletionTotalBytes = info.TotalBytes;
    raw->CompletionFin = info.Fin;
    raw->CompletionBindingOwner = info.BindingOwner;
    if (!raw->TryMarkRegistered()) {
        Errors.fetch_add(1, std::memory_order_relaxed);
        return false;
    }
    ++relay->SubmittingQuicSends;
    ++relay->InFlightQuicSends;
    relay->InFlightQuicSendBytes += raw->TotalBytes;
    if (workerThread) {
        RegisterKnownSendOperationLocal(raw, info);
    } else {
        RegisterKnownSendOperation(raw, info);
    }
    if (!binding->Active.load(std::memory_order_acquire) || relay->Closing || relay->Stream != stream) {
        KnownSendOperationInfo removedInfo{};
        if (workerThread) {
            (void)UnregisterKnownSendOperationLocal(raw, &removedInfo);
        } else {
            (void)UnregisterKnownSendOperation(raw, &removedInfo);
        }
        if (relay->SubmittingQuicSends > 0) {
            --relay->SubmittingQuicSends;
        }
        if (relay->InFlightQuicSends > 0) {
            --relay->InFlightQuicSends;
        }
        relay->InFlightQuicSendBytes = relay->InFlightQuicSendBytes >= removedInfo.TotalBytes
            ? relay->InFlightQuicSendBytes - removedInfo.TotalBytes
            : 0;
        return true;
    }
    const QUIC_STATUS status = TqDarwinRelayStreamSend(
        stream,
        raw->QuicBuffers.empty() ? nullptr : raw->QuicBuffers.data(),
        static_cast<uint32_t>(raw->QuicBuffers.size()),
        raw->Fin ? QUIC_SEND_FLAG_FIN : QUIC_SEND_FLAG_NONE,
        completionKey);

    KnownSendOperationInfo submittedInfo{};
    const bool completionAlreadyRan = workerThread
        ? !MarkKnownSendOperationSubmittedLocal(raw, &submittedInfo)
        : !MarkKnownSendOperationSubmitted(raw, &submittedInfo);
    if (!completionAlreadyRan) {
        (void)raw->TryMarkSubmitted();
    }
    submittedInfo.CompletionEventClaimed = raw->IsCompletionClaimed();
    if (relay->SubmittingQuicSends > 0) {
        --relay->SubmittingQuicSends;
    }
    if (workerThread && submittedInfo.CompletionEventClaimed) {
        (void)DrainEvents(1);
    }
    if (completionAlreadyRan) {
        // Callback claimed the registry entry; dismiss so RAII does not cancel.
        reservation.Dismiss();
        (void)operation.release();
        return true;
    }

    if (QUIC_FAILED(status)) {
        if (submittedInfo.CompletionEventClaimed) {
            reservation.Dismiss();
            (void)operation.release();
            return true;
        }
        KnownSendOperationInfo removedInfo{};
        if (workerThread) {
            (void)UnregisterKnownSendOperationLocal(raw, &removedInfo);
        } else {
            (void)UnregisterKnownSendOperation(raw, &removedInfo);
        }
        if (relay->InFlightQuicSends > 0) {
            --relay->InFlightQuicSends;
        }
        relay->InFlightQuicSendBytes = relay->InFlightQuicSendBytes >= removedInfo.TotalBytes
            ? relay->InFlightQuicSendBytes - removedInfo.TotalBytes
            : 0;
        if (status == QUIC_STATUS_OUT_OF_MEMORY || status == QUIC_STATUS_BUFFER_TOO_SMALL) {
            QuicSendBackpressureEvents.fetch_add(1, std::memory_order_relaxed);
            // Roll back the unsubmitted completion key before re-queueing.
            (void)reservation.Cancel();
            if (!SetTcpReadBackpressure(relay, true)) {
                return false;
            }
            if (relay->Closing) {
                return false;
            }
            raw->State.store(
                static_cast<uint32_t>(TqDarwinSendOperationState::Created),
                std::memory_order_release);
            relay->PendingQuicSends.push_back(std::move(operation));
            return true;
        }
        Errors.fetch_add(1, std::memory_order_relaxed);
        relay->Closing = true;
        return false;
    }

    // Send accepted: keep registry entry until SEND_COMPLETE claims it.
    reservation.Dismiss();
    (void)operation.release();
    return true;
}

bool TqDarwinRelayWorker::EnqueueQuicSendCompleteFromCallback(
    uint64_t relayId,
    TqDarwinRelaySendOperation* operation) {
    if (operation == nullptr) {
        return false;
    }
    TqDarwinRelayEvent event{};
    event.Type = TqDarwinRelayEventType::QuicSendComplete;
    event.RelayId = relayId;
    event.Value = static_cast<uint64_t>(reinterpret_cast<uintptr_t>(operation));
    if (EventQueue.TryPush(std::move(event))) {
        (void)Wake();
        return true;
    }
    CompleteQuicSend(operation);
    return true;
}

bool TqDarwinRelayWorker::EnqueueRelayCloseFromCallback(
    const std::shared_ptr<RelayState>& relay,
    TqDarwinRelayEventType type) {
    if (relay == nullptr) {
        return false;
    }
#if defined(TCPQUIC_TESTING) && !defined(NDEBUG)
    // QuicShutdownComplete must only be produced by SHUTDOWN_COMPLETE via
    // EnqueueQuicShutdownCompleteFromCallback (P1-5 invariant).
    assert(type != TqDarwinRelayEventType::QuicShutdownComplete);
#endif

    TqDarwinRelayEvent event{};
    event.Type = type;
    event.RelayId = relay->Id;
    event.RelayOwner = relay;
    if (EventQueue.TryPush(std::move(event))) {
        (void)Wake();
        return true;
    }
    return false;
}

bool TqDarwinRelayWorker::EnqueueQuicActiveShutdownFromCallback(
    const std::shared_ptr<RelayState>& relay,
    TqDarwinActiveShutdownReason reason) {
    if (relay == nullptr) {
        return false;
    }
    switch (reason) {
    case TqDarwinActiveShutdownReason::ReceiveAllocationFailed:
        ActiveFailureAllocationFailed.fetch_add(1, std::memory_order_relaxed);
        break;
    case TqDarwinActiveShutdownReason::ReceiveBudgetExceeded:
        ActiveFailureBudgetExceeded.fetch_add(1, std::memory_order_relaxed);
        break;
    case TqDarwinActiveShutdownReason::ReceiveQueueFull:
        ActiveFailureQueueFull.fetch_add(1, std::memory_order_relaxed);
        break;
    }
#if defined(TCPQUIC_TESTING)
    LastActiveShutdownReason.store(
        static_cast<uint8_t>(reason),
        std::memory_order_relaxed);
#endif
    TqDarwinRelayEvent event{};
    event.Type = TqDarwinRelayEventType::QuicActiveShutdown;
    event.RelayId = relay->Id;
    event.RelayOwner = relay;
    event.Value = static_cast<uint64_t>(reason);
    if (EventQueue.TryPush(std::move(event))) {
#if defined(TCPQUIC_TESTING)
        QuicActiveShutdownEnqueued.fetch_add(1, std::memory_order_relaxed);
#endif
        (void)Wake();
        return true;
    }
    return false;
}

bool TqDarwinRelayWorker::EnqueueQuicShutdownCompleteFromCallback(
    const std::shared_ptr<RelayState>& relay) {
    if (relay == nullptr) {
        return false;
    }
    TqDarwinRelayEvent event{};
    event.Type = TqDarwinRelayEventType::QuicShutdownComplete;
    event.RelayId = relay->Id;
    event.RelayOwner = relay;
    if (EventQueue.TryPush(std::move(event))) {
#if defined(TCPQUIC_TESTING)
        QuicShutdownCompleteEnqueued.fetch_add(1, std::memory_order_relaxed);
#endif
        (void)Wake();
        return true;
    }
    return false;
}

void TqDarwinRelayWorker::RetryPendingQuicSends(const std::shared_ptr<RelayState>& relay) {
    if (relay == nullptr) {
        return;
    }
    AssertWorkerThreadForRelayState();
    if (relay->Binding != nullptr &&
        relay->Binding->Terminal.load(std::memory_order_acquire)) {
        return;
    }
    for (;;) {
        std::unique_ptr<TqDarwinRelaySendOperation> operation;
        if (relay->Closing || relay->PendingQuicSends.empty()) {
            break;
        }
        operation = std::move(relay->PendingQuicSends.front());
        relay->PendingQuicSends.pop_front();
        if (!TrySubmitQuicSendOperation(relay, std::move(operation))) {
            relay->Closing = true;
            return;
        }
        if (!relay->PendingQuicSends.empty()) {
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
    if (!operation->TryMarkCompleted()) {
        Errors.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    const bool workerThread = IsWorkerThread();
#if defined(TCPQUIC_TESTING)
    if (!workerThread) {
        FallbackSendCompletionCount.fetch_add(1, std::memory_order_relaxed);
    }
#endif
    KnownSendOperationInfo info{};
    bool unregistered = false;
    if (workerThread) {
        unregistered = UnregisterKnownSendOperationLocal(operation, &info);
    } else {
        unregistered = UnregisterKnownSendOperation(operation, &info);
        if (!unregistered) {
            std::lock_guard<std::mutex> lock(ActiveSendMutex);
            const auto it = ActiveSendOperations.find(operation);
            if (it != ActiveSendOperations.end()) {
                info = it->second;
                ActiveSendOperations.erase(it);
#if defined(TCPQUIC_TESTING)
                ActiveSendOperationsSize.fetch_sub(1, std::memory_order_relaxed);
#endif
                unregistered = true;
                if (auto binding = std::static_pointer_cast<StreamBinding>(info.BindingOwner)) {
                    const auto completions = binding->Completions;
                    if (completions != nullptr) {
                        completions->KnownSendOperationCount.fetch_sub(1, std::memory_order_acq_rel);
                    }
                }
            }
        }
    }
    if (!unregistered) {
        info.RelayId = operation->CompletionRelayId;
        info.TotalBytes = operation->CompletionTotalBytes;
        info.Fin = operation->CompletionFin;
        info.BindingOwner = operation->CompletionBindingOwner;
        if (info.BindingOwner == nullptr) {
            Errors.fetch_add(1, std::memory_order_relaxed);
            return;
        }
    }
    if (operation->Magic != TqDarwinRelaySendOperation::MagicValue) {
        Errors.fetch_add(1, std::memory_order_relaxed);
    }
    auto relay = workerThread ? FindRelayLocal(info.RelayId) : FindRelay(info.RelayId);
    bool completionMayReleaseRetiredStorage = !workerThread;
    if (relay == nullptr) {
        relay = workerThread ? FindRetiredRelayLocal(info.RelayId) : FindRetiredRelay(info.RelayId);
        completionMayReleaseRetiredStorage = completionMayReleaseRetiredStorage || relay != nullptr;
    }
    bool bindingRelayFallback = false;
    if (relay == nullptr) {
        if (auto binding = std::static_pointer_cast<StreamBinding>(info.BindingOwner)) {
            relay = binding->Relay.lock();
            bindingRelayFallback = relay != nullptr;
            completionMayReleaseRetiredStorage =
                completionMayReleaseRetiredStorage || bindingRelayFallback;
        }
    }
    delete operation;
    if (relay != nullptr) {
        bool closing = false;
        if (workerThread && !bindingRelayFallback) {
            AssertWorkerThreadForRelayState();
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
        } else {
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
        if (info.Fin) {
            if (workerThread && !bindingRelayFallback) {
                TraceHalfClose(*relay, "quic_fin_buffer_released");
            } else if (TqTraceEnabled()) {
                // Off-worker fallback: do not scan worker-owned queues (§8.3).
                TqTraceLinuxRelayStreamState state{};
                state.WorkerIndex = Config.WorkerIndex;
                state.RelayId = info.RelayId;
                state.QuicSendFinSubmitted = true;
                state.QuicSendFinCompleted = true;
                TqTraceRelayHalfClose(
                    "darwin",
                    Config.WorkerIndex,
                    "quic_fin_buffer_released_off_worker",
                    state,
                    "off_worker_no_queue_scan",
                    false,
                    false);
            }
        }
        if (workerThread && !bindingRelayFallback && !closing && !info.Submitting) {
            RetryPendingQuicSends(relay);
            if (ShouldResumeTcpReadForQuicBacklog(relay)) {
                (void)SetTcpReadBackpressure(relay, false);
            }
        }
    }
    if (completionMayReleaseRetiredStorage) {
        PurgeRetiredRelaysIfSafe();
    }
}

bool TqDarwinRelayWorker::ShouldPauseTcpReadForQuicBacklog(const std::shared_ptr<RelayState>& relay) const {
    if (relay == nullptr) {
        return false;
    }
    AssertWorkerThreadForRelayState();
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
    AssertWorkerThreadForRelayState();
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
    AssertWorkerThreadForRelayState();
    bool oldPaused = false;
    bool oldArmed = false;
    if (relay->TcpReadPausedByQuicBacklog == paused) {
        return true;
    }
    oldPaused = relay->TcpReadPausedByQuicBacklog;
    oldArmed = relay->TcpReadArmed;
    relay->TcpReadPausedByQuicBacklog = paused;
    relay->TcpReadArmed = !paused && !relay->TcpReadClosed;
    if (UpdateTcpInterestLocal(relay)) {
        TraceHalfClose(
            *relay,
            paused ? "tcp_read_backpressure_on" : "tcp_read_backpressure_off");
        return true;
    }
    relay->TcpReadPausedByQuicBacklog = oldPaused;
    relay->TcpReadArmed = oldArmed;
    relay->Closing = true;
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
    if (receive == nullptr || !receive->CallbackBudgetHeld) {
        return;
    }
    receive->CallbackBudgetHeld = false;
    auto binding = std::static_pointer_cast<StreamBinding>(receive->BindingOwner);
    ReleaseCallbackReceiveBudget(binding.get(), receive->TotalLength);
    receive->BindingOwner.reset();
}

std::shared_ptr<TqDarwinPendingQuicReceive> TqDarwinRelayWorker::BuildPendingQuicReceive(
    RelayState* relay,
    const std::shared_ptr<StreamBinding>& binding,
    MsQuicStream* stream,
    const QUIC_BUFFER* buffers,
    uint32_t bufferCount,
    bool fin) {
    if (binding == nullptr) {
        return {};
    }
#if defined(TCPQUIC_TESTING)
    if (Config.FailNextPendingReceiveAllocationForTest) {
        Config.FailNextPendingReceiveAllocationForTest = false;
        Errors.fetch_add(1, std::memory_order_relaxed);
        return {};
    }
#endif
    MsQuicStream* effectiveStream = stream;
    if (effectiveStream == nullptr && relay != nullptr) {
        effectiveStream = relay->Stream;
    }
    if (effectiveStream == nullptr) {
        return {};
    }
    if (bufferCount != 0 && buffers == nullptr) {
        return {};
    }
    if (!fin && (buffers == nullptr || bufferCount == 0)) {
        return {};
    }

    auto receive = std::shared_ptr<TqDarwinPendingQuicReceive>(new (std::nothrow) TqDarwinPendingQuicReceive{});
    if (!receive) {
        Errors.fetch_add(1, std::memory_order_relaxed);
        return {};
    }
    receive->RelayId = binding->RelayId;
    receive->BindingOwner = binding;
    receive->StreamOwner = binding->StreamOwner.lock();
    if (receive->StreamOwner == nullptr) {
        if (binding->Endpoint != nullptr) {
            // Managed path: owner must come from binding weak, never from the
            // relay-map strong pointer (Task 2 / P1-3 identity contract).
            Errors.fetch_add(1, std::memory_order_relaxed);
            return {};
        }
        if (relay != nullptr) {
            receive->StreamOwner = relay->StreamOwner;
        }
    }
    receive->Fin = fin;
    receive->Slices.reserve(bufferCount);

    for (uint32_t i = 0; i < bufferCount; ++i) {
        if (buffers[i].Length == 0) {
            continue;
        }
        if (buffers[i].Buffer == nullptr) {
            Errors.fetch_add(1, std::memory_order_relaxed);
            return {};
        }
        receive->Slices.push_back(TqDarwinQuicReceiveSlice{buffers[i].Buffer, buffers[i].Length});
        receive->TotalLength += buffers[i].Length;
    }
    if (receive->TotalLength == 0 && !fin) {
        Errors.fetch_add(1, std::memory_order_relaxed);
        return {};
    }
    return receive;
}

bool TqDarwinRelayWorker::QueuePrecommitQuicReceive(
    RelayState* relay,
    StreamBinding* binding,
    const std::shared_ptr<StreamBinding>& bindingOwner,
    MsQuicStream* stream,
    const QUIC_BUFFER* buffers,
    uint32_t bufferCount,
    bool fin,
    bool& handled) {
    handled = false;
    if (binding == nullptr ||
        binding->ActivationState.load(std::memory_order_acquire) ==
            StreamBinding::Activation::Active) {
        return true;
    }
    handled = true;
    if (binding->ActivationState.load(std::memory_order_acquire) ==
            StreamBinding::Activation::Failed ||
        binding->ActivationState.load(std::memory_order_acquire) ==
            StreamBinding::Activation::Terminal ||
        binding->Closing.load(std::memory_order_acquire)) {
        return false;
    }
    auto receive = BuildPendingQuicReceive(relay, bindingOwner, stream, buffers, bufferCount, fin);
    if (!receive) {
        return false;
    }
    std::lock_guard<std::mutex> guard(binding->ActivationMutex);
    const auto activation = binding->ActivationState.load(std::memory_order_acquire);
    if (activation == StreamBinding::Activation::Active) {
        handled = false;
        return true;
    }
    if (binding->PrecommitSettled ||
        activation == StreamBinding::Activation::Failed ||
        activation == StreamBinding::Activation::Terminal ||
        binding->Closing.load(std::memory_order_acquire)) {
        return false;
    }
    const uint64_t pending = receive->TotalLength - receive->CompletedLength;
    if (pending > binding->PrecommitMaxPendingBytes -
            std::min(binding->PrecommitPendingBytes, binding->PrecommitMaxPendingBytes)) {
        return false;
    }
    binding->PrecommitPendingBytes += pending;
    binding->PrecommitReceives.push_back(std::move(receive));
    return true;
}

TqDarwinQuicReceiveEnqueueResult TqDarwinRelayWorker::QueueDeferredQuicReceive(
    const std::shared_ptr<StreamBinding>& binding,
    MsQuicStream* stream,
    const QUIC_BUFFER* buffers,
    uint32_t bufferCount,
    bool fin) {
    auto recordFailure = [this](TqDarwinQuicReceiveEnqueueResult result) -> TqDarwinQuicReceiveEnqueueResult {
        QuicReceiveEnqueueFailures.fetch_add(1, std::memory_order_relaxed);
        if (result == TqDarwinQuicReceiveEnqueueResult::CallbackBudgetRejected) {
            CallbackReceiveBudgetRejects.fetch_add(1, std::memory_order_relaxed);
        }
        return result;
    };

    if (binding == nullptr) {
        return recordFailure(TqDarwinQuicReceiveEnqueueResult::InvalidArgs);
    }
    auto receive = BuildPendingQuicReceive(
        binding->Relay.lock().get(),
        binding,
        stream,
        buffers,
        bufferCount,
        fin);
    if (!receive) {
        if (bufferCount != 0 && buffers == nullptr) {
            return recordFailure(TqDarwinQuicReceiveEnqueueResult::InvalidArgs);
        }
        if (!fin && (buffers == nullptr || bufferCount == 0)) {
            return recordFailure(TqDarwinQuicReceiveEnqueueResult::InvalidArgs);
        }
        for (uint32_t i = 0; i < bufferCount; ++i) {
            if (buffers[i].Length != 0 && buffers[i].Buffer == nullptr) {
                return recordFailure(TqDarwinQuicReceiveEnqueueResult::NullBuffer);
            }
        }
        uint64_t totalLength = 0;
        for (uint32_t i = 0; i < bufferCount; ++i) {
            totalLength += buffers[i].Length;
        }
        if (totalLength == 0 && !fin) {
            return recordFailure(TqDarwinQuicReceiveEnqueueResult::EmptyNonFin);
        }
        return recordFailure(TqDarwinQuicReceiveEnqueueResult::AllocationFailed);
    }
    if (!ReserveCallbackReceiveBudget(binding.get(), receive->TotalLength)) {
        Errors.fetch_add(1, std::memory_order_relaxed);
        return recordFailure(TqDarwinQuicReceiveEnqueueResult::CallbackBudgetRejected);
    }
    receive->CallbackBudgetHeld = true;
    QuicReceiveViewCount.fetch_add(1, std::memory_order_relaxed);
    QuicReceiveViewBytes.fetch_add(receive->TotalLength, std::memory_order_relaxed);

    TqDarwinRelayEvent event{};
    event.Type = TqDarwinRelayEventType::QuicReceiveView;
    event.RelayId = receive->RelayId;
    event.TotalLength = receive->TotalLength;
    event.Fin = fin;
    event.ReceiveView = receive;
    if (!EnqueueEvent(std::move(event))) {
        QuicReceiveViewBackpressureQueued.fetch_add(1, std::memory_order_relaxed);
        {
            std::lock_guard<std::mutex> guard(binding->CallbackPendingQuicReceivesMutex);
            binding->CallbackPendingQuicReceives.push_back(receive);
        }
        binding->CallbackQuicReceivePaused.store(true, std::memory_order_release);
        // Pause with the callback stream parameter only — never a saved bare pointer.
        PauseMsQuicReceiveFromCallback(stream);
        (void)Wake();
        return TqDarwinQuicReceiveEnqueueResult::EventQueueFull;
    }
    return TqDarwinQuicReceiveEnqueueResult::Ok;
}

void TqDarwinRelayWorker::FlushCallbackPendingQuicReceives(StreamBinding* binding) {
    if (binding == nullptr) {
        return;
    }
    AssertWorkerThreadForRelayState();
    if (binding->Terminal.load(std::memory_order_acquire) ||
        !binding->Active.load(std::memory_order_acquire)) {
        return;
    }

    std::shared_ptr<RelayState> relay = binding->Relay.lock();
    if (relay != nullptr && binding->CallbackQuicReceivePaused.exchange(false, std::memory_order_acq_rel)) {
        relay->QuicReceivePaused = true;
    }

    for (;;) {
        std::shared_ptr<TqDarwinPendingQuicReceive> receive;
        {
            std::lock_guard<std::mutex> guard(binding->CallbackPendingQuicReceivesMutex);
            if (binding->CallbackPendingQuicReceives.empty()) {
                break;
            }
            receive = binding->CallbackPendingQuicReceives.front();
        }
        if (receive == nullptr) {
            std::lock_guard<std::mutex> guard(binding->CallbackPendingQuicReceivesMutex);
            if (!binding->CallbackPendingQuicReceives.empty()) {
                binding->CallbackPendingQuicReceives.pop_front();
            }
            continue;
        }

        TqDarwinRelayEvent retryEvent{};
        retryEvent.Type = TqDarwinRelayEventType::QuicReceiveView;
        retryEvent.RelayId = receive->RelayId;
        retryEvent.TotalLength = receive->TotalLength;
        retryEvent.Fin = receive->Fin;
        retryEvent.ReceiveView = receive;
        if (!EnqueueEvent(std::move(retryEvent))) {
            break;
        }

        {
            std::lock_guard<std::mutex> guard(binding->CallbackPendingQuicReceivesMutex);
            if (!binding->CallbackPendingQuicReceives.empty() &&
                binding->CallbackPendingQuicReceives.front() == receive) {
                binding->CallbackPendingQuicReceives.pop_front();
            }
        }
    }

    bool empty = false;
    {
        std::lock_guard<std::mutex> guard(binding->CallbackPendingQuicReceivesMutex);
        empty = binding->CallbackPendingQuicReceives.empty();
    }
    if (empty) {
        if (auto relay = binding->Relay.lock()) {
            MaybeResumeQuicReceive(relay);
        }
    }
}

void TqDarwinRelayWorker::FlushAllCallbackPendingQuicReceivesLocal() {
    AssertWorkerThreadForRelayState();
    for (const auto& entry : Relays) {
        const auto& relay = entry.second;
        if (relay != nullptr && relay->Binding != nullptr) {
            FlushCallbackPendingQuicReceives(relay->Binding.get());
        }
    }
}

void TqDarwinRelayWorker::FlushHalfCloseStickiesLocal() {
    AssertWorkerThreadForRelayState();
    for (const auto& entry : Relays) {
        const auto& relay = entry.second;
        if (relay != nullptr) {
            ConsumeHalfCloseStickies(relay);
        }
    }
}

void TqDarwinRelayWorker::ArmHalfCloseStickyFromCallback(
    StreamBinding* binding,
    std::atomic<bool> StreamBinding::* stickyFlag) {
    if (binding == nullptr) {
        return;
    }
    (binding->*stickyFlag).store(true, std::memory_order_release);
    (void)Wake();
}

void TqDarwinRelayWorker::ConsumeHalfCloseStickies(
    const std::shared_ptr<RelayState>& relay) {
    AssertWorkerThreadForRelayState();
    if (relay == nullptr || relay->Binding == nullptr) {
        return;
    }
    auto& binding = *relay->Binding;
    if (binding.PeerSendShutdownSticky.exchange(false, std::memory_order_acq_rel)) {
        ProcessPeerSendShutdown(relay);
    }
    if (binding.SendShutdownCompleteSticky.exchange(false, std::memory_order_acq_rel)) {
        ProcessSendShutdownComplete(relay);
    }
    (void)binding.ConvergenceCheckSticky.exchange(false, std::memory_order_acq_rel);
}

void TqDarwinRelayWorker::ProcessQuicReceiveViewEvent(
    const std::shared_ptr<TqDarwinPendingQuicReceive>& receive) {
    if (receive == nullptr) {
        return;
    }
    AssertWorkerThreadForRelayState();
    auto relay = FindRelayLocal(receive->RelayId);
    if (relay == nullptr) {
        ReleaseCallbackReceiveBudget(receive);
        (void)DiscardDeferredQuicReceive(nullptr, receive);
        return;
    }
    if (relay->Closing) {
        ReleaseCallbackReceiveBudget(receive);
        (void)DiscardDeferredQuicReceive(relay, receive);
        return;
    }
    if (auto binding = std::static_pointer_cast<StreamBinding>(receive->BindingOwner)) {
        if (binding->CallbackQuicReceivePaused.exchange(false, std::memory_order_acq_rel)) {
            relay->QuicReceivePaused = true;
        }
    }
    relay->PendingQuicReceiveBytes += receive->TotalLength;
    relay->PendingQuicReceives.push_back(receive);
    ReleaseCallbackReceiveBudget(receive);
    if (receive->PendingCompleteBytes != 0) {
        CompleteDeferredQuicReceive(relay, receive);
        return;
    }
    MaybePauseQuicReceive(relay);
    if (relay->Closing) {
        (void)DiscardDeferredQuicReceive(relay, receive);
        return;
    }
    if (!EnqueueQuicReceiveForTcp(relay, receive)) {
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

    AssertWorkerThreadForRelayState();
    if (relay->Closing) {
        return false;
    }
    const bool needsDecompress = relay->Decompressor != nullptr && relay->CompressAlgo == TqCompressAlgo::Zstd;
    ITqDecompressor* decompressor = relay->Decompressor;

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
            relay->CompressedReceiveRejected = true;
            relay->Closing = true;
            return true;
        }
        std::vector<uint8_t> decompressed;
        for (const auto& slice : receive->Slices) {
            if (!decompressor->Decompress(slice.Data, slice.Length, decompressed)) {
                (void)DiscardDeferredQuicReceive(relay, receive);
                relay->CompressedReceiveRejected = true;
                relay->Closing = true;
                return true;
            }
        }
        if (!decompressed.empty() && !appendOutput(decompressed.data(), decompressed.size())) {
            (void)DiscardDeferredQuicReceive(relay, receive);
            relay->Closing = true;
            return false;
        }
    } else {
        for (const auto& slice : receive->Slices) {
            if (slice.Length != 0 && !appendOutput(slice.Data, slice.Length)) {
                (void)DiscardDeferredQuicReceive(relay, receive);
                relay->Closing = true;
                return false;
            }
        }
    }

    std::shared_ptr<TqDarwinPendingQuicReceive> completedReceive;
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
    // Capture stream-bearing relay before budget release clears BindingOwner.
    // CompleteDeferred is lease-only via receive->StreamOwner->TryAcquireApi();
    // no bare ReceiveCompleteStream / relay->Stream fallback.
    std::shared_ptr<RelayState> completeRelay = relay;
    if (completeRelay == nullptr) {
        if (auto binding = std::static_pointer_cast<StreamBinding>(receive->BindingOwner)) {
            completeRelay = binding->Relay.lock();
        }
    }
    ReleaseCallbackReceiveBudget(receive);
    uint64_t bytesToComplete = 0;
    {
        // Worker-thread bookkeeping touches PendingQuicReceive* under the
        // worker-thread ownership rule. Stop-thread retire/settle uses the
        // else branch; MsQuic complete still requires a StreamOwner lease.
        if (relay != nullptr && IsWorkerThread()) {
            AssertWorkerThreadForRelayState();
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
    CompleteDeferredQuicReceive(completeRelay, receive);
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
        AssertWorkerThreadForRelayState();
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
            bool pendingReceiveCompletion = false;
            for (const auto& pending : relay->PendingQuicReceives) {
                if (pending != nullptr && pending->PendingCompleteBytes > 0) {
                    CompleteDeferredQuicReceive(relay, pending);
                    if (pending->PendingCompleteBytes > 0) {
                        pendingReceiveCompletion = true;
                    }
                }
            }
            if (pendingReceiveCompletion) {
                relay->TcpWriteArmed = true;
                return true;
            }
            (void)::shutdown(relay->TcpFd, SHUT_WR);
            relay->TcpWriteShutdownQueued = false;
            relay->TcpWriteArmed = false;
            relay->TcpWriteClosed = true;
            TraceHalfClose(*relay, "tcp_shut_wr");
            return true;
        } else {
            relay->TcpWriteArmed = false;
            return true;
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
            AssertWorkerThreadForRelayState();
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
            AssertWorkerThreadForRelayState();
            relay->TcpWriteArmed = true;
            (void)UpdateTcpInterestLocal(relay);
            return true;
        }
        Errors.fetch_add(1, std::memory_order_relaxed);
        std::deque<std::shared_ptr<TqDarwinPendingQuicReceive>> receivesToDiscard;
        AssertWorkerThreadForRelayState();
        relay->Closing = true;
        receivesToDiscard.swap(relay->PendingQuicReceives);
        relay->PendingQuicReceiveBytes = 0;
        relay->PendingTcpWriteBytes = 0;
        relay->PendingTcpWrites.clear();
        for (const auto& pending : receivesToDiscard) {
            (void)DiscardDeferredQuicReceive(relay, pending);
        }
        return false;
    }
}

void TqDarwinRelayWorker::CompleteDeferredQuicReceive(
    const std::shared_ptr<RelayState>& relay,
    const std::shared_ptr<TqDarwinPendingQuicReceive>& receive) {
    if (receive == nullptr || receive->PendingCompleteBytes == 0) {
        return;
    }
    const uint64_t bytes = receive->PendingCompleteBytes;
    auto bindingOwner = std::static_pointer_cast<StreamBinding>(receive->BindingOwner);
    const bool bindingTerminal =
        (relay != nullptr &&
            relay->Binding != nullptr &&
            relay->Binding->Terminal.load(std::memory_order_acquire)) ||
        (bindingOwner != nullptr &&
            bindingOwner->Terminal.load(std::memory_order_acquire));
    if (receive->StreamOwner != nullptr) {
        const auto phase = receive->StreamOwner->GetPhase();
        const bool ownerTerminal =
            phase == TqStreamLifetime::Phase::TerminalPublished ||
            phase == TqStreamLifetime::Phase::Closed;
        if (ownerTerminal || bindingTerminal) {
            if (!receive->TryClaimCompletionDispatch()) {
                return;
            }
            receive->PendingCompleteBytes = 0;
            DeferredReceiveDiscards.fetch_add(1, std::memory_order_relaxed);
            return;
        }
        auto lease = receive->StreamOwner->TryAcquireApi();
        if (!lease) {
            if (bindingOwner != nullptr &&
                !bindingOwner->Active.load(std::memory_order_acquire) &&
                !bindingTerminal) {
                if (!receive->TryClaimCompletionDispatch()) {
                    return;
                }
                receive->PendingCompleteBytes = 0;
                DeferredReceiveDiscards.fetch_add(1, std::memory_order_relaxed);
                (void)receive->StreamOwner->RequestShutdown(
                    TqStreamLifetime::ShutdownIntent::AbortReceive);
                return;
            }
            if (receive->CompletedLength >= receive->TotalLength &&
                receive->PendingCompleteBytes > 0) {
                // Bookkeeping-only complete when TryAcquireApi fails without an
                // installed stream wrapper (precommit discard); not a MsQuic call.
                if (!receive->TryClaimCompletionDispatch()) {
                    return;
                }
                receive->PendingCompleteBytes = 0;
                DeferredReceiveCompletes.fetch_add(1, std::memory_order_relaxed);
                return;
            }
            return;
        }
        if (!receive->TryClaimCompletionDispatch()) {
            return;
        }
        receive->PendingCompleteBytes = 0;
        DeferredReceiveCompletes.fetch_add(1, std::memory_order_relaxed);
#if defined(TCPQUIC_TESTING)
        assert(g_darwinRelayMutexDepthForTest == 0 &&
               "ReceiveComplete must not run under relay->Mutex");
#endif
        CompleteMsQuicReceiveFromCallback(lease.Stream(), bytes);
        return;
    }
    // No StreamOwner: discard/bookkeeping only. Never call ReceiveComplete via
    // bare relay->Stream (Task 4 lease-only contract).
    if (!receive->TryClaimCompletionDispatch()) {
        return;
    }
    receive->PendingCompleteBytes = 0;
    DeferredReceiveDiscards.fetch_add(1, std::memory_order_relaxed);
}

void TqDarwinRelayWorker::CompleteMsQuicReceiveFromCallback(
    MsQuicStream* stream,
    uint64_t totalLength) {
    if (totalLength == 0) {
        return;
    }
#if defined(TCPQUIC_TESTING)
    if (g_darwinRelayReceiveCompleteForTest != nullptr) {
        g_darwinRelayReceiveCompleteForTest(stream, totalLength);
        return;
    }
#endif
    if (stream != nullptr && stream->Handle != nullptr) {
        stream->ReceiveComplete(totalLength);
    }
}

void TqDarwinRelayWorker::PauseMsQuicReceiveFromCallback(MsQuicStream* stream) {
#if defined(TCPQUIC_TESTING)
    if (g_darwinRelayReceiveSetEnabledForTest != nullptr) {
        (void)g_darwinRelayReceiveSetEnabledForTest(stream, false);
        return;
    }
#endif
    if (stream != nullptr && stream->Handle != nullptr) {
        (void)stream->ReceiveSetEnabled(false);
    }
}

bool TqDarwinRelayWorker::SetQuicReceiveEnabled(const std::shared_ptr<RelayState>& relay, bool enabled) {
    if (relay == nullptr) {
        return false;
    }
    AssertWorkerThreadForRelayState();
    if (relay->Binding != nullptr &&
        (relay->Binding->Terminal.load(std::memory_order_acquire) ||
            !relay->Binding->Active.load(std::memory_order_acquire))) {
        return false;
    }
    MsQuicStream* stream = relay->Stream;
    QUIC_STATUS status = QUIC_STATUS_SUCCESS;
    if (relay->StreamOwner != nullptr) {
        auto lease = relay->StreamOwner->TryAcquireApi();
        if (!lease) {
            return false;
        }
        stream = lease.Stream();
    }
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
    AssertWorkerThreadForRelayState();
    const uint64_t pendingPressure = relay->PendingQuicReceiveBytes + relay->PendingTcpWriteBytes;
    const bool shouldPause = !relay->QuicReceivePaused &&
        pendingPressure >= MaxPendingQuicReceiveBytesPerRelay();
    if (shouldPause) {
        relay->QuicReceivePaused = true;
    }
    if (shouldPause && !SetQuicReceiveEnabled(relay, false)) {
        relay->QuicReceivePaused = false;
        relay->Closing = true;
    }
}

void TqDarwinRelayWorker::MaybeResumeQuicReceive(const std::shared_ptr<RelayState>& relay) {
    if (relay == nullptr) {
        return;
    }
    AssertWorkerThreadForRelayState();
    const uint64_t pendingPressure = relay->PendingQuicReceiveBytes + relay->PendingTcpWriteBytes;
    const bool shouldResume = relay->QuicReceivePaused &&
        pendingPressure <= LowPendingQuicReceiveBytesPerRelay();
    if (shouldResume) {
        relay->QuicReceivePaused = false;
    }
    if (shouldResume && !SetQuicReceiveEnabled(relay, true)) {
        relay->QuicReceivePaused = true;
        relay->Closing = true;
    }
}

void TqDarwinRelayWorker::RequestRelayShutdown(
    const std::shared_ptr<RelayState>& relay,
    TqStreamLifetime::ShutdownIntent intent) {
    if (relay != nullptr && relay->StreamOwner != nullptr) {
        (void)relay->StreamOwner->RequestShutdown(intent);
    }
    if (relay != nullptr && relay->StopControl != nullptr) {
        (void)relay->StopControl->SignalStop(relay->ControlGeneration);
    }
}

void TqDarwinRelayWorker::FailManagedBinding(RelayState* relay, StreamBinding* binding) {
    if (binding == nullptr) {
        return;
    }
    std::deque<std::shared_ptr<TqDarwinPendingQuicReceive>> pending;
    {
        std::lock_guard<std::mutex> guard(binding->ActivationMutex);
        const auto phase = binding->ActivationState.load(std::memory_order_acquire);
        if (phase != StreamBinding::Activation::Terminal &&
            phase != StreamBinding::Activation::Failed) {
            binding->ActivationState.store(
                StreamBinding::Activation::Failed,
                std::memory_order_release);
        }
        (void)TakePrecommitForSettlementLocked(binding, pending);
    }
    DiscardPrecommitReceives(pending);
    binding->Closing.store(true, std::memory_order_release);
    binding->Active.store(false, std::memory_order_release);
    // Do not reset binding->Relay after publish (P1-4).
    if (binding->StopControl != nullptr) {
        (void)binding->StopControl->SignalStop(binding->ControlGeneration);
    }
    if (relay != nullptr) {
        relay->Binding.reset();
        relay->ManagedBinding.reset();
    }
}

bool TqDarwinRelayWorker::TakePrecommitForSettlementLocked(
    StreamBinding* binding,
    std::deque<std::shared_ptr<TqDarwinPendingQuicReceive>>& out) {
    if (binding == nullptr || binding->PrecommitSettled) {
        return false;
    }
    binding->PrecommitSettled = true;
    if (binding->PrecommitReceives.empty()) {
        binding->PrecommitPendingBytes = 0;
        return false;
    }
    out.swap(binding->PrecommitReceives);
    binding->PrecommitPendingBytes = 0;
    return !out.empty();
}

void TqDarwinRelayWorker::DiscardPrecommitReceives(
    std::deque<std::shared_ptr<TqDarwinPendingQuicReceive>>& pending) {
    for (const auto& receive : pending) {
        (void)DiscardDeferredQuicReceive(nullptr, receive);
    }
    pending.clear();
}

void TqDarwinRelayWorker::SealManagedBindingTerminal(
    RelayState* relay,
    StreamBinding* binding) {
    if (binding == nullptr) {
        return;
    }
    std::deque<std::shared_ptr<TqDarwinPendingQuicReceive>> pending;
    {
        std::lock_guard<std::mutex> guard(binding->ActivationMutex);
        const auto phase = binding->ActivationState.load(std::memory_order_acquire);
        if (phase != StreamBinding::Activation::Terminal &&
            phase != StreamBinding::Activation::Failed) {
            binding->ActivationState.store(
                StreamBinding::Activation::Terminal,
                std::memory_order_release);
        }
        // If terminal races after commit (or during Prepared), take precommit
        // here so ActivateManagedBinding cannot leave PENDING receives orphaned.
        (void)TakePrecommitForSettlementLocked(binding, pending);
    }
    DiscardPrecommitReceives(pending);
    binding->Active.store(false, std::memory_order_release);
    binding->Terminal.store(true, std::memory_order_release);
    binding->Closing.store(true, std::memory_order_release);
    // Do not reset binding->Relay after publish (P1-4).
    const uint64_t generation = binding->ControlGeneration != 0
        ? binding->ControlGeneration
        : (relay != nullptr ? relay->ControlGeneration : 0);
    if (binding->StopControl != nullptr) {
        (void)binding->StopControl->SignalStop(generation);
    } else if (relay != nullptr && relay->StopControl != nullptr) {
        (void)relay->StopControl->SignalStop(generation);
    }
}

void TqDarwinRelayWorker::ActivateManagedBinding(
    const std::shared_ptr<RelayState>& relay,
    StreamBinding* binding) {
    if (relay == nullptr || binding == nullptr) {
        return;
    }
    std::deque<std::shared_ptr<TqDarwinPendingQuicReceive>> pending;
    bool drain = false;
    {
        std::lock_guard<std::mutex> guard(binding->ActivationMutex);
        const auto phase = binding->ActivationState.load(std::memory_order_acquire);
        // Post-commit disposition must always settle precommit: Active drains,
        // Terminal/Failed discards. Never return while PrecommitReceives remain.
        if (!TakePrecommitForSettlementLocked(binding, pending)) {
            return;
        }
        drain = phase == StreamBinding::Activation::Active;
    }
    if (drain) {
        while (!pending.empty()) {
            (void)ProcessQuicReceiveViewEvent(pending.front());
            pending.pop_front();
        }
        return;
    }
    DiscardPrecommitReceives(pending);
}

void TqDarwinRelayWorker::HandoffTerminalCloseToShutdownSink(
    const std::shared_ptr<RelayState>& relay,
    StreamBinding* binding) {
#if defined(TCPQUIC_TESTING)
    if (Config.BeforeTerminalHandoffHookForTest != nullptr && relay != nullptr) {
        Config.BeforeTerminalHandoffHookForTest(this, relay->Id);
    }
#endif
    const uint64_t generation = binding != nullptr && binding->ControlGeneration != 0
        ? binding->ControlGeneration
        : (relay != nullptr ? relay->ControlGeneration : 0);
    if (binding != nullptr && binding->StopControl != nullptr) {
        (void)binding->StopControl->SignalStop(generation);
    }
    if (relay != nullptr && relay->StopControl != nullptr) {
        (void)relay->StopControl->SignalStop(generation);
    }
}

void TqDarwinRelayWorker::HandoffActiveShutdownFromCallback(
    const std::shared_ptr<RelayState>& relay,
    StreamBinding* binding) {
    HandoffTerminalCloseToShutdownSink(relay, binding);
}

void TqDarwinRelayWorker::InstallShutdownSinksForStop() {
    std::vector<std::shared_ptr<RelayState>> relays;
    {
        std::lock_guard<std::mutex> lock(RelayMutex);
        relays.reserve(Relays.size());
        for (const auto& entry : Relays) {
            relays.push_back(entry.second);
        }
    }
    for (const auto& relay : relays) {
        if (relay == nullptr || relay->StreamOwner == nullptr || relay->Binding == nullptr) {
            continue;
        }
        auto binding = relay->Binding;
        auto sink = std::make_shared<ShutdownSink>();
        sink->StopControl = binding->StopControl != nullptr
            ? binding->StopControl
            : relay->StopControl;
        sink->ControlGeneration = binding->ControlGeneration != 0
            ? binding->ControlGeneration
            : relay->ControlGeneration;
        sink->StreamOwner = binding->StreamOwner;
        sink->Completions = binding->Completions;
        (void)relay->StreamOwner->PublishTarget(binding->RouteGeneration, sink);
    }
}

void TqDarwinRelayWorker::ProcessPeerSendShutdown(
    const std::shared_ptr<RelayState>& relay) {
    if (relay == nullptr) {
        return;
    }
    AssertWorkerThreadForRelayState();
    if (relay->Closing || relay->TcpWriteClosed) {
        return;
    }
    if (relay->Binding != nullptr &&
        relay->Binding->Terminal.load(std::memory_order_acquire)) {
        return;
    }
    if (FindRelayLocal(relay->Id) != relay) {
        return;
    }
    relay->TcpWriteShutdownQueued = true;
    if (relay->PendingTcpWrites.empty()) {
        relay->TcpWriteArmed = true;
        (void)UpdateTcpInterestLocal(relay);
    }
    TraceHalfClose(*relay, "peer_send_shutdown");
}

void TqDarwinRelayWorker::ProcessSendShutdownComplete(
    const std::shared_ptr<RelayState>& relay) {
    if (relay == nullptr) {
        return;
    }
    AssertWorkerThreadForRelayState();
    if (relay->Binding != nullptr &&
        relay->Binding->Terminal.load(std::memory_order_acquire)) {
        return;
    }
    if (FindRelayLocal(relay->Id) != relay) {
        return;
    }
    relay->QuicSendClosed = true;
    relay->QuicSendShutdownCompleteObserved = true;
    TraceHalfClose(*relay, "send_shutdown_complete");
}

struct TqDarwinRelayWorker::PreparedRelayToken {
    std::shared_ptr<RelayState> Relay;
    std::shared_ptr<StreamBinding> Binding;
    bool OwnsFd{false};
    bool FilterInstalled{false};
    bool MapPublished{false};
    bool RollbackDone{false};
};

TqDarwinRelayWorker::PreparedCommitDisposition
TqDarwinRelayWorker::TryCommitPreparedActivation(
    const std::shared_ptr<RelayState>& relay,
    StreamBinding* binding,
    const TqDarwinRelayRegistration& registration) {
    if (relay == nullptr || binding == nullptr) {
        return PreparedCommitDisposition::RollbackFailed;
    }
    std::lock_guard<std::mutex> guard(binding->ActivationMutex);
    const auto phase = binding->ActivationState.load(std::memory_order_acquire);
    if (phase == StreamBinding::Activation::Terminal) {
        return PreparedCommitDisposition::RollbackTerminal;
    }
    if (phase != StreamBinding::Activation::Prepared) {
        return PreparedCommitDisposition::RollbackFailed;
    }
    if (binding->Terminal.load(std::memory_order_acquire) ||
        binding->Closing.load(std::memory_order_acquire)) {
        binding->ActivationState.store(
            StreamBinding::Activation::Terminal,
            std::memory_order_release);
        return PreparedCommitDisposition::RollbackTerminal;
    }
    if (registration.StreamOwner != nullptr) {
        const auto ownerPhase = registration.StreamOwner->GetPhase();
        if (ownerPhase == TqStreamLifetime::Phase::TerminalPublished ||
            ownerPhase == TqStreamLifetime::Phase::Closed) {
            binding->ActivationState.store(
                StreamBinding::Activation::Terminal,
                std::memory_order_release);
            return PreparedCommitDisposition::RollbackTerminal;
        }
        if (registration.StreamOwner->RouteGeneration() != binding->RouteGeneration) {
            binding->ActivationState.store(
                StreamBinding::Activation::Failed,
                std::memory_order_release);
            return PreparedCommitDisposition::RollbackFailed;
        }
    }
    if (binding->ControlGeneration != relay->ControlGeneration) {
        binding->ActivationState.store(
            StreamBinding::Activation::Failed,
            std::memory_order_release);
        return PreparedCommitDisposition::RollbackFailed;
    }
    const auto mapped = Relays.find(relay->Id);
    if (mapped == Relays.end() || mapped->second != relay || binding->RelayId != relay->Id) {
        binding->ActivationState.store(
            StreamBinding::Activation::Failed,
            std::memory_order_release);
        return PreparedCommitDisposition::RollbackFailed;
    }
#if defined(TCPQUIC_TESTING)
    if (Config.FailCommitForTest) {
        binding->ActivationState.store(
            StreamBinding::Activation::Failed,
            std::memory_order_release);
        return PreparedCommitDisposition::RollbackFailed;
    }
#endif
    binding->ActivationState.store(
        StreamBinding::Activation::Active,
        std::memory_order_release);
    return PreparedCommitDisposition::CommitActive;
}

void TqDarwinRelayWorker::RollbackPreparedRelay(
    PreparedRelayToken& token,
    TqStreamLifetime::ShutdownIntent intent) {
    if (token.RollbackDone) {
        return;
    }
    token.RollbackDone = true;
    if (token.Binding != nullptr) {
        FailManagedBinding(token.Relay.get(), token.Binding.get());
    }
    if (token.Relay != nullptr) {
        RequestRelayShutdown(token.Relay, intent);
        if (token.FilterInstalled) {
            RemoveTcpFilters(token.Relay);
            token.FilterInstalled = false;
        }
        if (token.MapPublished) {
            std::unique_lock<std::shared_mutex> mapAccess(RelayMapAccessMutex);
            std::lock_guard<std::mutex> lock(RelayMutex);
            Relays.erase(token.Relay->Id);
            token.MapPublished = false;
        }
        if (token.OwnsFd) {
            CloseRelayTcpFdOnce(token.Relay);
            token.OwnsFd = false;
        }
    }
}

TqDarwinRelayRegistrationResult TqDarwinRelayWorker::RegisterRelayWithIdLocal(
    const TqDarwinRelayRegistration& registration) {
    TqDarwinRelayRegistrationResult result{};
#if defined(TCPQUIC_TESTING)
    if (Config.FailPrepareForTest) {
        return result;
    }
#endif
    if (!Running.load(std::memory_order_acquire) || KqueueFd < 0) {
        return result;
    }
    if (!TqSocketValid(registration.TcpFd) || registration.Stream == nullptr) {
        return result;
    }
    auto control = registration.Control;
    if (control == nullptr) {
        return result;
    }
    uint64_t controlGeneration = registration.ControlGeneration;
    if (controlGeneration == 0) {
        controlGeneration = control->Generation;
    }
    if (!TqSetNonBlocking(registration.TcpFd)) {
        Errors.fetch_add(1, std::memory_order_relaxed);
        return result;
    }
    if (!SetNoSigPipe(registration.TcpFd) && errno != ENOPROTOOPT) {
        Errors.fetch_add(1, std::memory_order_relaxed);
        return result;
    }

    auto relay = std::make_shared<RelayState>(registration, Config);
#if defined(TCPQUIC_TESTING)
    relay->DestructorCounterOwner = this;
#endif
    {
        std::lock_guard<std::mutex> relayLock(relay->Mutex);
        relay->TcpReadArmed = true;
    }
#if defined(TCPQUIC_TESTING)
    if (Config.FailManagedBindingForTest) {
        FailManagedBinding(relay.get(), nullptr);
        return result;
    }
#endif
    auto binding = std::make_shared<StreamBinding>();
#if defined(TCPQUIC_TESTING)
    binding->DestructorCounterOwner = this;
#endif
    binding->Worker.store(this, std::memory_order_release);
    binding->Completions = std::make_shared<CompletionState>();
    binding->PrecommitMaxPendingBytes = MaxPendingQuicReceiveBytesPerRelay();
    binding->StopControl = control;
    binding->ControlGeneration = controlGeneration;
    binding->ActivationState.store(
        StreamBinding::Activation::Prepared,
        std::memory_order_release);
    relay->Binding = binding;

    PreparedRelayToken token{};
    token.Relay = relay;
    token.Binding = binding;

    // Allocate id + initialize all callback-visible identity BEFORE PublishTarget.
    {
        std::unique_lock<std::shared_mutex> mapAccess(RelayMapAccessMutex);
        std::lock_guard<std::mutex> lock(RelayMutex);
        relay->Id = NextRelayId++;
        binding->RelayId = relay->Id;
        binding->Relay = relay;
        if (registration.StreamOwner != nullptr) {
            binding->StreamOwner = registration.StreamOwner;
            // PublishTarget increments owner RouteGeneration after Target_ is
            // published. Precompute the final generation here so callbacks never
            // observe a stale/half-updated binding->RouteGeneration.
            const uint64_t expectedGeneration =
                registration.StreamOwner->RouteGeneration();
            binding->RouteGeneration = expectedGeneration + 1;
            binding->Endpoint = StreamCallbackEndpoint;
        } else {
            binding->ActivationState.store(
                StreamBinding::Activation::Active,
                std::memory_order_release);
        }
        Relays.emplace(relay->Id, relay);
        token.MapPublished = true;
#if defined(TCPQUIC_TESTING)
        MapPublicationCount.fetch_add(1, std::memory_order_relaxed);
#endif
    }

    if (registration.StreamOwner != nullptr) {
        // expected = final - 1; binding already holds the post-publish generation.
        const uint64_t expectedGeneration = binding->RouteGeneration - 1;
        uint64_t publishedGeneration = 0;
        if (!registration.StreamOwner->PublishTarget(
                expectedGeneration,
                binding,
                &publishedGeneration) ||
            (publishedGeneration != 0 &&
             publishedGeneration != binding->RouteGeneration)) {
            RollbackPreparedRelay(token, TqStreamLifetime::ShutdownIntent::AbortBoth);
            return result;
        }
        // Do not write RouteGeneration after PublishTarget — it is already final.
        relay->ManagedBinding = binding;
        // Publish success irreversibly transfers FD ownership to the token.
        token.OwnsFd = true;
        result.TcpFdConsumed = true;

#if defined(TCPQUIC_TESTING)
        LastPublishIdentity.RelayId = binding->RelayId;
        LastPublishIdentity.RouteGeneration = binding->RouteGeneration;
        LastPublishIdentity.ControlGeneration = binding->ControlGeneration;
        LastPublishIdentity.RelayLockable = !binding->Relay.expired();
        LastPublishIdentity.StreamOwnerLockable = !binding->StreamOwner.expired();
        {
            std::lock_guard<std::mutex> guard(binding->ActivationMutex);
            LastPublishIdentity.PrecommitDepth = binding->PrecommitReceives.size();
        }
        if (Config.AfterPublishHookForTest != nullptr) {
            Config.AfterPublishHookForTest(this, relay->Id);
        }
        {
            std::lock_guard<std::mutex> guard(binding->ActivationMutex);
            LastPublishIdentity.PrecommitDepth = binding->PrecommitReceives.size();
        }
#endif
    } else {
        result.Ok = true;
        result.RelayId = relay->Id;
        if (!RegisterTcpFilters(relay)) {
            RollbackPreparedRelay(token, TqStreamLifetime::ShutdownIntent::AbortBoth);
            result.Ok = false;
            result.RelayId = 0;
            return result;
        }
        token.FilterInstalled = true;
        token.OwnsFd = true;
        result.TcpFdConsumed = true;
        relay->Committed = true;
#if defined(TCPQUIC_TESTING)
        if (TqDarwinRelayStreamUsableForTest(registration.Stream)) {
            registration.Stream->Callback = TqDarwinRelayWorker::StreamCallback;
            registration.Stream->Context = binding.get();
        }
#else
        registration.Stream->Callback = TqDarwinRelayWorker::StreamCallback;
        registration.Stream->Context = binding.get();
#endif
        return result;
    }

    if (!InstallInactiveTcpFilters(relay)) {
        RollbackPreparedRelay(token, TqStreamLifetime::ShutdownIntent::AbortBoth);
        result.Ok = false;
        result.RelayId = 0;
        return result;
    }
    token.FilterInstalled = true;

#if defined(TCPQUIC_TESTING)
    if (Config.BeforeCommitFinalCheckHookForTest != nullptr) {
        Config.BeforeCommitFinalCheckHookForTest(this, relay->Id);
    }
#endif

    const auto disposition = TryCommitPreparedActivation(relay, binding.get(), registration);
    if (disposition != PreparedCommitDisposition::CommitActive) {
        if (disposition == PreparedCommitDisposition::RollbackTerminal) {
            TerminalBeforeCommitRollbacks.fetch_add(1, std::memory_order_relaxed);
        } else {
            ActivationFailureCount.fetch_add(1, std::memory_order_relaxed);
        }
        const auto intent = disposition == PreparedCommitDisposition::RollbackTerminal
            ? TqStreamLifetime::ShutdownIntent::AbortBoth
            : TqStreamLifetime::ShutdownIntent::AbortBoth;
        RollbackPreparedRelay(token, intent);
        result.Ok = false;
        result.TcpFdConsumed = true;
        result.RelayId = 0;
        return result;
    }
    CommitSuccessCount.fetch_add(1, std::memory_order_relaxed);

#if defined(TCPQUIC_TESTING)
    if (Config.AfterCommitActivationHookForTest != nullptr) {
        Config.AfterCommitActivationHookForTest(this, relay->Id);
    }
#endif

    // Commit already linearized Prepared->Active. A terminal that arrives after
    // that point must not roll registration back; keep filters inactive and let
    // terminal cleanup own filter/FD disposition.
    const bool terminalAfterCommit =
        binding->Terminal.load(std::memory_order_acquire) ||
        binding->ActivationState.load(std::memory_order_acquire) ==
            StreamBinding::Activation::Terminal;
    if (!terminalAfterCommit) {
        if (!EnableTcpFilters(relay)) {
            RollbackPreparedRelay(token, TqStreamLifetime::ShutdownIntent::AbortBoth);
            result.Ok = false;
            result.TcpFdConsumed = true;
            result.RelayId = 0;
            return result;
        }
    }

    relay->Committed = true;
    ActivateManagedBinding(relay, binding.get());
    result.Ok = true;
    result.RelayId = relay->Id;
    result.TcpFdConsumed = true;
    // Token successfully committed — do not roll back FD/map on scope exit.
    token.OwnsFd = false;
    token.FilterInstalled = false;
    token.MapPublished = false;
    token.RollbackDone = true;
    return result;
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
        std::unique_lock<std::shared_mutex> mapAccess(RelayMapAccessMutex);
        std::lock_guard<std::mutex> lock(RelayMutex);
        const auto it = Relays.find(relayId);
        if (it == Relays.end()) {
            return;
        }
        relay = it->second;
        Relays.erase(it);
    }

    RemoveTcpFilters(relay);
    RequestRelayShutdown(relay, TqStreamLifetime::ShutdownIntent::AbortBoth);
    RetireRelay(relay, TqDarwinRelayCloseDisposition::ActiveShutdown);
    PurgeRetiredRelaysIfSafe();
}

void TqDarwinRelayWorker::UnregisterRelay(uint64_t relayId) {
    if (IsWorkerThread()) {
        UnregisterRelayLocal(relayId);
        return;
    }

    // Active RelayState data-plane fields are worker-thread-owned. Non-worker
    // lifecycle cleanup must queue while a worker can still be touching them.
    UnregisterRelayCommand command{};
    command.RelayId = relayId;
    uint32_t attempts = 0;
    for (;;) {
        bool fallbackLocal = false;
        bool queued = false;
        {
            std::lock_guard<std::mutex> lifecycleLock(LifecycleMutex);
            if (!Running.load(std::memory_order_acquire) || KqueueFd < 0) {
                fallbackLocal = true;
            } else {
                TqDarwinRelayEvent event{};
                event.Type = TqDarwinRelayEventType::UnregisterRelay;
                event.Control = &command;
                if (EventQueue.TryPush(std::move(event))) {
                    (void)Wake();
                    queued = true;
                }
            }
        }

        if (fallbackLocal) {
            UnregisterRelayLocal(relayId);
            return;
        }
        if (queued) {
            break;
        }
        if ((attempts++ & 0x3F) == 0) {
            (void)Wake();
            std::this_thread::sleep_for(kControlCommandWakeRetryInterval);
        } else {
            std::this_thread::yield();
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
}

TqDarwinRelayWorkerSnapshot TqDarwinRelayWorker::MakeIncompleteSnapshot() const {
    TqDarwinRelayWorkerSnapshot snapshot{};
    snapshot.WorkerIndex = Config.WorkerIndex;
    snapshot.EventQueueCapacity = EventQueue.Capacity();
    snapshot.SnapshotComplete = false;
    return snapshot;
}

TqDarwinRelayWorkerSnapshot TqDarwinRelayWorker::SnapshotLocal() const {
#if defined(TQ_UNIT_TESTING)
    if (Config.FailNextSnapshotLocalForTest) {
        const_cast<TqDarwinRelayWorkerConfig&>(Config).FailNextSnapshotLocalForTest = false;
        throw std::bad_alloc();
    }
#endif
    TqDarwinRelayWorkerSnapshot snapshot{};
    snapshot.WorkerIndex = Config.WorkerIndex;
    snapshot.EventQueueCapacity = EventQueue.Capacity();
    snapshot.SnapshotComplete = true;
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
    snapshot.DeferredReceiveDiscards = DeferredReceiveDiscards.load(std::memory_order_relaxed);
    snapshot.ReceiveFailSafeCount = ReceiveFailSafeCount.load(std::memory_order_relaxed);
    snapshot.LateTerminalReceiveCount = LateTerminalReceiveCount.load(std::memory_order_relaxed);
    snapshot.QuicSendBackpressureEvents = QuicSendBackpressureEvents.load(std::memory_order_relaxed);
    snapshot.CancelOnLossCount = CancelOnLossCount.load(std::memory_order_relaxed);
    snapshot.QuicReceivePausedCount = QuicReceivePausedCount.load(std::memory_order_relaxed);
    snapshot.QuicReceiveResumedCount = QuicReceiveResumedCount.load(std::memory_order_relaxed);
    const auto now = std::chrono::steady_clock::now();
    {
        std::lock_guard<std::mutex> lock(RelayMutex);
        snapshot.ActiveRelays = Relays.size();
        for (const auto& entry : Relays) {
            const auto& relay = entry.second;
            if (relay == nullptr) {
                continue;
            }
            if (relay->TcpReadArmed) {
                ++snapshot.TcpReadArmedRelays;
            }
            if (relay->TcpWriteArmed) {
                ++snapshot.TcpWriteArmedRelays;
            }
            if (relay->Closing) {
                ++snapshot.ClosingRelays;
            }
            if (relay->TcpReadClosed) {
                ++snapshot.TcpReadClosedRelays;
            }
            if (relay->TcpWriteClosed) {
                ++snapshot.TcpWriteClosedRelays;
            }
            if (relay->TcpWriteShutdownQueued) {
                ++snapshot.TcpWriteShutdownQueuedRelays;
            }
            if (relay->QuicSendFinSubmitted) {
                ++snapshot.QuicSendFinSubmittedRelays;
            }
            if (relay->QuicSendFinCompleted) {
                ++snapshot.QuicSendFinCompletedRelays;
            }
            if (relay->QuicSendShutdownCompleteObserved) {
                ++snapshot.QuicSendShutdownCompleteRelays;
            }
            if (relay->TcpReadPausedByQuicBacklog) {
                ++snapshot.TcpReadPausedByQuicBacklogRelays;
            }
            if (FullyClosedPredicateReady(*relay)) {
                ++snapshot.FullyClosedPredicateReadyRelays;
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
            snapshot.PendingReceiveActive += relay->PendingQuicReceives.size();
            snapshot.PendingTcpWriteQueue += relay->PendingTcpWrites.size();
            snapshot.PendingTcpWriteBytes += relay->PendingTcpWriteBytes;
            snapshot.PendingBytes += relayPendingBytes;
            if (relay->Binding != nullptr) {
                const auto phase =
                    relay->Binding->ActivationState.load(std::memory_order_acquire);
                if (phase == StreamBinding::Activation::Prepared) {
                    ++snapshot.PreparedRelays;
                }
                snapshot.PrecommitBytes += relay->Binding->PrecommitPendingBytes;
                snapshot.PrecommitDepth += relay->Binding->PrecommitReceives.size();
            }
        }
        snapshot.StopRemaining = Relays.size() + RetiredRelays.size();
        for (const auto& retired : RetiredRelays) {
            if (retired == nullptr ||
                retired->RetiredAt == std::chrono::steady_clock::time_point{}) {
                continue;
            }
            const auto age = std::chrono::duration_cast<std::chrono::milliseconds>(
                now - retired->RetiredAt).count();
            snapshot.StopOldestAgeMs = std::max<uint64_t>(
                snapshot.StopOldestAgeMs,
                age > 0 ? static_cast<uint64_t>(age) : 0);
        }
    }
    snapshot.Errors = Errors.load(std::memory_order_relaxed);
    snapshot.EventQueueFullErrors = EventQueueFullErrors.load(std::memory_order_relaxed);
    snapshot.WakeFailures = WakeFailures.load(std::memory_order_relaxed);
    snapshot.CallbackReceiveBudgetRejects = CallbackReceiveBudgetRejects.load(std::memory_order_relaxed);
    snapshot.QuicReceiveEnqueueFailures = QuicReceiveEnqueueFailures.load(std::memory_order_relaxed);
    snapshot.QuicReceiveViewBackpressureQueued =
        QuicReceiveViewBackpressureQueued.load(std::memory_order_relaxed);
    const auto retained = TqStreamLifetime::SnapshotTerminalRetentions();
    snapshot.TerminalRetainedOwnerCount = retained.OwnerCount;
    snapshot.TerminalRetainedOldestAgeMs = retained.OldestAgeMs;
    const auto sendCompletions = TqStreamLifetime::SnapshotSendCompletions();
    snapshot.ActiveSendReservations = sendCompletions.ActiveCount;
    snapshot.PreSubmitSendRollbacks = sendCompletions.PreSubmitRollbacks;
    snapshot.UnknownSendClaims = sendCompletions.UnknownClaims;
    snapshot.DuplicateSendClaims = sendCompletions.DuplicateClaims;
    snapshot.SendReservationOldestAgeMs = sendCompletions.OldestAgeMs;
    snapshot.CommitSuccessCount = CommitSuccessCount.load(std::memory_order_relaxed);
    snapshot.TerminalBeforeCommitRollbacks =
        TerminalBeforeCommitRollbacks.load(std::memory_order_relaxed);
    snapshot.ActivationFailureCount = ActivationFailureCount.load(std::memory_order_relaxed);
    snapshot.ActiveFailureAllocationFailed =
        ActiveFailureAllocationFailed.load(std::memory_order_relaxed);
    snapshot.ActiveFailureBudgetExceeded =
        ActiveFailureBudgetExceeded.load(std::memory_order_relaxed);
    snapshot.ActiveFailureQueueFull = ActiveFailureQueueFull.load(std::memory_order_relaxed);
    snapshot.ShutdownSinkActive = g_DarwinShutdownSinkActive.load(std::memory_order_relaxed);
    snapshot.WorkerExitedPurgeEvents = WorkerExitedPurgeEvents.load(std::memory_order_relaxed);
    return snapshot;
}

TqDarwinRelayWorkerSnapshot TqDarwinRelayWorker::Snapshot() const {
    return Snapshot(
        std::chrono::steady_clock::now() + std::chrono::seconds(5));
}

TqDarwinRelayWorkerSnapshot TqDarwinRelayWorker::Snapshot(
    std::chrono::steady_clock::time_point deadline,
    TqRelayRuntimeSnapshotExecutionGate::Permit permit) const {
    if (IsWorkerThread()) {
        return SnapshotLocal();
    }

    auto command = std::make_shared<SnapshotCommand>();
    command->Permit = std::move(permit);

    for (uint32_t attempts = 0;; ++attempts) {
        if (std::chrono::steady_clock::now() >= deadline) {
            CancelSnapshotCommand(command);
            return MakeIncompleteSnapshot();
        }
        {
            std::lock_guard<std::mutex> lifecycleLock(LifecycleMutex);
            if (!Running.load(std::memory_order_acquire) || KqueueFd < 0) {
                CancelSnapshotCommand(command);
                return SnapshotLocal();
            }

            TqDarwinRelayEvent event{};
            event.Type = TqDarwinRelayEventType::Snapshot;
            event.Control = command.get();
            event.ControlOwner = command;
            if (EventQueue.TryPush(std::move(event))) {
                (void)Wake();
                break;
            }
        }
        if ((attempts & 0x3F) == 0) {
            (void)Wake();
            std::this_thread::sleep_for(kControlCommandWakeRetryInterval);
        } else {
            std::this_thread::yield();
        }
    }

    std::unique_lock<std::mutex> lock(command->Mutex);
    while (command->State == SnapshotCommandState::Pending) {
        const auto now = std::chrono::steady_clock::now();
        if (now >= deadline) {
            break;
        }
        const auto sliceEnd = std::min(deadline, now + kControlCommandWakeRetryInterval);
        if (command->Cv.wait_until(lock, sliceEnd, [&command] {
                return command->State != SnapshotCommandState::Pending;
            })) {
            break;
        }
        lock.unlock();
        (void)Wake();
        lock.lock();
    }
    if (command->State == SnapshotCommandState::Completed) {
        return command->Result;
    }
    if (command->State == SnapshotCommandState::Cancelled) {
        return MakeIncompleteSnapshot();
    }
    if (command->State == SnapshotCommandState::Pending) {
        // Timeout: detach only. Shared command + permit stay alive until the
        // worker completes or cancels the late command.
        command->State = SnapshotCommandState::Detached;
        return MakeIncompleteSnapshot();
    }
    // Already Detached (should be rare); treat as incomplete.
    return MakeIncompleteSnapshot();
}

QUIC_STATUS TqDarwinRelayWorker::OnStreamEventWithBinding(
    MsQuicStream* stream,
    QUIC_STREAM_EVENT* event,
    StreamBinding* binding) noexcept {
    (void)stream;
    if (binding == nullptr || event == nullptr) {
        return QUIC_STATUS_SUCCESS;
    }
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
        if (worker != nullptr && worker->TryClaimKnownSendCompletionEvent(operation, &info)) {
            if (info.CompletionEventClaimed) {
                TqStreamLifetime::RecordDuplicateSendClaim();
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
        TqStreamLifetime::RecordUnknownSendClaim();
        if (worker != nullptr) {
            worker->Errors.fetch_add(1, std::memory_order_relaxed);
        }
        return QUIC_STATUS_SUCCESS;
    }
    if (event->Type == QUIC_STREAM_EVENT_CANCEL_ON_LOSS) {
        event->CANCEL_ON_LOSS.ErrorCode = TqRelayStreamErrorCancelOnLoss;
        CancelOnLossCount.fetch_add(1, std::memory_order_relaxed);
        return QUIC_STATUS_SUCCESS;
    }
    if (event->Type == QUIC_STREAM_EVENT_SEND_SHUTDOWN_COMPLETE) {
        TqDarwinRelayWorker* worker = binding->Worker.load(std::memory_order_acquire);
        if (worker == nullptr || binding->Terminal.load(std::memory_order_acquire)) {
            return QUIC_STATUS_SUCCESS;
        }
        std::shared_ptr<RelayState> relay = binding->Relay.lock();
        if (relay == nullptr) {
            return QUIC_STATUS_SUCCESS;
        }
        if (!worker->EnqueueRelayCloseFromCallback(
                relay,
                TqDarwinRelayEventType::QuicSendShutdownComplete)) {
            worker->ArmHalfCloseStickyFromCallback(
                binding,
                &StreamBinding::SendShutdownCompleteSticky);
        }
        return QUIC_STATUS_SUCCESS;
    }
    if (event->Type == QUIC_STREAM_EVENT_PEER_SEND_SHUTDOWN) {
        TqDarwinRelayWorker* worker = binding->Worker.load(std::memory_order_acquire);
        if (worker == nullptr || binding->Terminal.load(std::memory_order_acquire)) {
            return QUIC_STATUS_SUCCESS;
        }
        std::shared_ptr<RelayState> relay = binding->Relay.lock();
        if (relay == nullptr) {
            return QUIC_STATUS_SUCCESS;
        }
        if (!worker->EnqueueRelayCloseFromCallback(
                relay,
                TqDarwinRelayEventType::QuicPeerSendShutdown)) {
            worker->EventQueueFullErrors.fetch_add(1, std::memory_order_relaxed);
            if (TqTraceEnabled()) {
                TqTraceLinuxRelayStreamState state{};
                state.WorkerIndex = worker->Config.WorkerIndex;
                state.RelayId = relay->Id;
                TqTraceRelayHalfClose(
                    "darwin",
                    worker->Config.WorkerIndex,
                    "peer_shutdown_enqueue_failed",
                    state,
                    "event_queue_full",
                    false,
                    false);
            }
            worker->ArmHalfCloseStickyFromCallback(
                binding,
                &StreamBinding::PeerSendShutdownSticky);
        }
        return QUIC_STATUS_SUCCESS;
    }
    if (event->Type == QUIC_STREAM_EVENT_PEER_SEND_ABORTED ||
        event->Type == QUIC_STREAM_EVENT_PEER_RECEIVE_ABORTED ||
        event->Type == QUIC_STREAM_EVENT_SHUTDOWN_COMPLETE) {
        TqDarwinRelayWorker* worker = binding->Worker.load(std::memory_order_acquire);
        if (worker == nullptr) {
            return QUIC_STATUS_SUCCESS;
        }
        std::shared_ptr<RelayState> relay = binding->Relay.lock();
        if (relay == nullptr) {
            return QUIC_STATUS_SUCCESS;
        }
        if (event->Type == QUIC_STREAM_EVENT_SHUTDOWN_COMPLETE) {
            if (relay->StreamOwner != nullptr) {
                SealManagedBindingTerminal(relay.get(), binding);
                if (!worker->EnqueueQuicShutdownCompleteFromCallback(relay)) {
                    worker->HandoffTerminalCloseToShutdownSink(relay, binding);
                }
            } else {
                binding->Active.store(false, std::memory_order_release);
                if (binding->StopControl != nullptr) {
                    (void)binding->StopControl->SignalStop(binding->ControlGeneration);
                } else if (relay->StopControl != nullptr) {
                    (void)relay->StopControl->SignalStop(relay->ControlGeneration);
                }
                if (!worker->EnqueueQuicShutdownCompleteFromCallback(relay)) {
                    worker->HandoffTerminalCloseToShutdownSink(relay, binding);
                }
            }
            return QUIC_STATUS_SUCCESS;
        }
        if (binding->Terminal.load(std::memory_order_acquire) ||
            (relay->StreamOwner != nullptr &&
                (relay->StreamOwner->GetPhase() == TqStreamLifetime::Phase::TerminalPublished ||
                    relay->StreamOwner->GetPhase() == TqStreamLifetime::Phase::Closed))) {
            return QUIC_STATUS_SUCCESS;
        }
        if (relay->StreamOwner != nullptr) {
            const TqStreamLifetime::ShutdownIntent intent =
                event->Type == QUIC_STREAM_EVENT_PEER_SEND_ABORTED
                    ? TqStreamLifetime::ShutdownIntent::AbortReceive
                    : TqStreamLifetime::ShutdownIntent::AbortSend;
            const uint64_t errorCode =
                event->Type == QUIC_STREAM_EVENT_PEER_SEND_ABORTED
                    ? event->PEER_SEND_ABORTED.ErrorCode
                    : event->PEER_RECEIVE_ABORTED.ErrorCode;
            (void)relay->StreamOwner->RequestShutdown(intent, errorCode);
            const TqDarwinRelayEventType closeEventType =
                event->Type == QUIC_STREAM_EVENT_PEER_SEND_ABORTED
                    ? TqDarwinRelayEventType::QuicPeerSendAborted
                    : TqDarwinRelayEventType::QuicPeerReceiveAborted;
            if (!worker->EnqueueRelayCloseFromCallback(relay, closeEventType)) {
                worker->HandoffActiveShutdownFromCallback(relay, binding);
            }
            return QUIC_STATUS_SUCCESS;
        }
        binding->Active.store(false, std::memory_order_release);
        const TqDarwinRelayEventType closeEventType =
            event->Type == QUIC_STREAM_EVENT_PEER_SEND_ABORTED
                ? TqDarwinRelayEventType::QuicPeerSendAborted
                : TqDarwinRelayEventType::QuicPeerReceiveAborted;
        if (!worker->EnqueueRelayCloseFromCallback(relay, closeEventType)) {
            worker->HandoffActiveShutdownFromCallback(relay, binding);
        }
        return QUIC_STATUS_SUCCESS;
    }
    if (event->Type == QUIC_STREAM_EVENT_RECEIVE) {
        TqDarwinRelayWorker* worker = binding->Worker.load(std::memory_order_acquire);
        if (binding->Terminal.load(std::memory_order_acquire)) {
            LateTerminalReceiveCount.fetch_add(1, std::memory_order_relaxed);
            event->RECEIVE.TotalBufferLength = 0;
            return QUIC_STATUS_SUCCESS;
        }
        if (worker == nullptr || !binding->Active.load(std::memory_order_acquire)) {
            event->RECEIVE.TotalBufferLength = 0;
            if (worker != nullptr) {
                worker->ReceiveFailSafeCount.fetch_add(1, std::memory_order_relaxed);
            }
            std::shared_ptr<RelayState> relay = binding->Relay.lock();
            if (relay != nullptr && relay->StreamOwner != nullptr) {
                (void)relay->StreamOwner->RequestShutdown(
                    TqStreamLifetime::ShutdownIntent::AbortReceive);
            }
            return QUIC_STATUS_SUCCESS;
        }
        const bool fin = (event->RECEIVE.Flags & QUIC_RECEIVE_FLAG_FIN) != 0;
        if (TqIsMsQuicFakeFinReceive(
                event->RECEIVE.AbsoluteOffset,
                event->RECEIVE.TotalBufferLength,
                event->RECEIVE.BufferCount,
                event->RECEIVE.Flags)) {
            TqTraceLinuxRelayStreamState state{};
            state.WorkerIndex = worker->Config.WorkerIndex;
            state.RelayId = binding->RelayId;
            TqTraceRelayStreamEvent(
                "darwin",
                worker->Config.WorkerIndex,
                binding->RelayId,
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
        uint64_t totalLength = event->RECEIVE.TotalBufferLength;
        if (totalLength == 0 && event->RECEIVE.BufferCount > 0 && event->RECEIVE.Buffers != nullptr) {
            for (uint32_t i = 0; i < event->RECEIVE.BufferCount; ++i) {
                totalLength += event->RECEIVE.Buffers[i].Length;
            }
        }
        std::shared_ptr<RelayState> relay = binding->Relay.lock();
        bool precommitHandled = false;
        const bool precommitQueued = worker->QueuePrecommitQuicReceive(
            relay.get(),
            binding,
            bindingOwner,
            stream,
            event->RECEIVE.Buffers,
            event->RECEIVE.BufferCount,
            fin,
            precommitHandled);
        if (precommitHandled) {
            if (!precommitQueued) {
                if (relay != nullptr) {
                    worker->FailManagedBinding(relay.get(), binding);
                    worker->RequestRelayShutdown(
                        relay,
                        TqStreamLifetime::ShutdownIntent::AbortBoth);
                }
                return QUIC_STATUS_OUT_OF_MEMORY;
            }
            return QUIC_STATUS_PENDING;
        }
        const TqDarwinQuicReceiveEnqueueResult enqueueResult = worker->QueueDeferredQuicReceive(
            bindingOwner,
            stream,
            event->RECEIVE.Buffers,
            event->RECEIVE.BufferCount,
            fin);
        switch (enqueueResult) {
        case TqDarwinQuicReceiveEnqueueResult::Ok:
            return QUIC_STATUS_PENDING;
        case TqDarwinQuicReceiveEnqueueResult::InvalidArgs:
        case TqDarwinQuicReceiveEnqueueResult::EmptyNonFin:
        case TqDarwinQuicReceiveEnqueueResult::NullBuffer:
            return QUIC_STATUS_SUCCESS;
        case TqDarwinQuicReceiveEnqueueResult::AllocationFailed: {
            // P1-5: resource failure stays on ActiveShutdown — never forge
            // QuicShutdownComplete / TerminalLogicalDetach, and do not
            // ReceiveComplete buffers we never took ownership of.
            event->RECEIVE.TotalBufferLength = 0;
            if (std::shared_ptr<RelayState> relay = binding->Relay.lock()) {
                if (relay->StreamOwner != nullptr) {
                    (void)relay->StreamOwner->RequestShutdown(
                        TqStreamLifetime::ShutdownIntent::AbortBoth);
                }
                if (!worker->EnqueueQuicActiveShutdownFromCallback(
                        relay,
                        TqDarwinActiveShutdownReason::ReceiveAllocationFailed)) {
                    worker->HandoffActiveShutdownFromCallback(relay, binding);
                }
            }
            return QUIC_STATUS_OUT_OF_MEMORY;
        }
        case TqDarwinQuicReceiveEnqueueResult::CallbackBudgetRejected:
            // Non-terminal backpressure: did not take PENDING ownership.
            // Immediate complete on callback stream param; no QuicActiveShutdown
            // (ReceiveBudgetExceeded reserved for Task 5+).
            binding->CallbackQuicReceivePaused.store(true, std::memory_order_release);
            worker->PauseMsQuicReceiveFromCallback(stream);
            worker->CompleteMsQuicReceiveFromCallback(stream, totalLength);
            return QUIC_STATUS_SUCCESS;
        case TqDarwinQuicReceiveEnqueueResult::EventQueueFull:
            // Non-terminal backpressure: PENDING held on binding queue.
            // ReceiveQueueFull QuicActiveShutdown reserved for Task 5+.
            return QUIC_STATUS_PENDING;
        }
        return QUIC_STATUS_SUCCESS;
    }
    return QUIC_STATUS_SUCCESS;
}

QUIC_STATUS QUIC_API TqDarwinRelayWorker::StreamCallback(
    MsQuicStream* stream,
    void* context,
    QUIC_STREAM_EVENT* event) noexcept {
    if (context == nullptr || event == nullptr) {
        return QUIC_STATUS_SUCCESS;
    }
    auto* binding = static_cast<StreamBinding*>(context);
    TqDarwinRelayWorker* worker = binding->Worker.load(std::memory_order_acquire);
    if (worker == nullptr) {
        return QUIC_STATUS_SUCCESS;
    }
    return worker->OnStreamEventWithBinding(stream, event, binding);
}

void TqAccumulateDarwinRelayWorkerSnapshot(
    TqDarwinRelayWorkerSnapshot& total,
    const TqDarwinRelayWorkerSnapshot& part) {
    total.EventsProcessed += part.EventsProcessed;
    total.Wakeups += part.Wakeups;
    total.PendingEvents += part.PendingEvents;
    total.PendingBytes += part.PendingBytes;
    total.ActiveRelays += part.ActiveRelays;
    total.TcpReadArmedRelays += part.TcpReadArmedRelays;
    total.TcpWriteArmedRelays += part.TcpWriteArmedRelays;
    total.ClosingRelays += part.ClosingRelays;
    total.TcpReadClosedRelays += part.TcpReadClosedRelays;
    total.TcpWriteClosedRelays += part.TcpWriteClosedRelays;
    total.TcpWriteShutdownQueuedRelays += part.TcpWriteShutdownQueuedRelays;
    total.QuicSendFinSubmittedRelays += part.QuicSendFinSubmittedRelays;
    total.QuicSendFinCompletedRelays += part.QuicSendFinCompletedRelays;
    total.QuicSendShutdownCompleteRelays += part.QuicSendShutdownCompleteRelays;
    total.TcpReadPausedByQuicBacklogRelays += part.TcpReadPausedByQuicBacklogRelays;
    total.FullyClosedPredicateReadyRelays += part.FullyClosedPredicateReadyRelays;
    total.QuicReceivePausedCount += part.QuicReceivePausedCount;
    total.QuicReceiveResumedCount += part.QuicReceiveResumedCount;
    total.CurrentPendingQuicReceiveBytes += part.CurrentPendingQuicReceiveBytes;
    total.PendingTcpWriteQueue += part.PendingTcpWriteQueue;
    total.PendingTcpWriteBytes += part.PendingTcpWriteBytes;
    total.OutstandingQuicSends += part.OutstandingQuicSends;
    total.OutstandingQuicSendBytes += part.OutstandingQuicSendBytes;
    total.TcpReadBatches += part.TcpReadBatches;
    total.TcpReadBytes += part.TcpReadBytes;
    total.TcpWriteBatches += part.TcpWriteBatches;
    total.TcpWriteBytes += part.TcpWriteBytes;
    total.QuicReceiveViewCount += part.QuicReceiveViewCount;
    total.QuicReceiveViewBytes += part.QuicReceiveViewBytes;
    total.DeferredReceiveCompletes += part.DeferredReceiveCompletes;
    total.DeferredReceiveDiscards += part.DeferredReceiveDiscards;
    total.ReceiveFailSafeCount += part.ReceiveFailSafeCount;
    total.LateTerminalReceiveCount += part.LateTerminalReceiveCount;
    total.QuicSendBackpressureEvents += part.QuicSendBackpressureEvents;
    total.CancelOnLossCount += part.CancelOnLossCount;
    total.Errors += part.Errors;
    total.EventQueueFullErrors += part.EventQueueFullErrors;
    total.WakeFailures += part.WakeFailures;
    total.CallbackReceiveBudgetRejects += part.CallbackReceiveBudgetRejects;
    total.QuicReceiveEnqueueFailures += part.QuicReceiveEnqueueFailures;
    total.QuicReceiveViewBackpressureQueued += part.QuicReceiveViewBackpressureQueued;
    // Global owner/registry gauges: max, never sum across workers.
    total.TerminalRetainedOwnerCount =
        std::max(total.TerminalRetainedOwnerCount, part.TerminalRetainedOwnerCount);
    total.TerminalRetainedOldestAgeMs =
        std::max(total.TerminalRetainedOldestAgeMs, part.TerminalRetainedOldestAgeMs);
    total.ActiveSendReservations =
        std::max(total.ActiveSendReservations, part.ActiveSendReservations);
    total.PreSubmitSendRollbacks =
        std::max(total.PreSubmitSendRollbacks, part.PreSubmitSendRollbacks);
    total.UnknownSendClaims = std::max(total.UnknownSendClaims, part.UnknownSendClaims);
    total.DuplicateSendClaims =
        std::max(total.DuplicateSendClaims, part.DuplicateSendClaims);
    total.SendReservationOldestAgeMs =
        std::max(total.SendReservationOldestAgeMs, part.SendReservationOldestAgeMs);
    total.ShutdownSinkActive = std::max(total.ShutdownSinkActive, part.ShutdownSinkActive);
    total.StopRemaining += part.StopRemaining;
    total.PreparedRelays += part.PreparedRelays;
    total.CommitSuccessCount += part.CommitSuccessCount;
    total.TerminalBeforeCommitRollbacks += part.TerminalBeforeCommitRollbacks;
    total.ActivationFailureCount += part.ActivationFailureCount;
    total.PrecommitBytes += part.PrecommitBytes;
    total.PrecommitDepth += part.PrecommitDepth;
    total.PendingReceiveActive += part.PendingReceiveActive;
    total.ActiveFailureAllocationFailed += part.ActiveFailureAllocationFailed;
    total.ActiveFailureBudgetExceeded += part.ActiveFailureBudgetExceeded;
    total.ActiveFailureQueueFull += part.ActiveFailureQueueFull;
    total.WorkerExitedPurgeEvents += part.WorkerExitedPurgeEvents;
    total.StopOldestAgeMs = std::max(total.StopOldestAgeMs, part.StopOldestAgeMs);
    total.EventQueueCapacity = std::max(total.EventQueueCapacity, part.EventQueueCapacity);
    total.SnapshotComplete = total.SnapshotComplete && part.SnapshotComplete;
}

TqDarwinRelayRuntime& TqDarwinRelayRuntime::Instance() {
    static TqDarwinRelayRuntime runtime;
    return runtime;
}

std::unique_lock<std::mutex> TqDarwinRelayRuntime::AcquireRuntimeLock() const {
    return std::unique_lock<std::mutex>(Mutex);
}

std::unique_lock<std::mutex> TqDarwinRelayRuntime::TryAcquireRuntimeLockForSnapshot(
    std::chrono::steady_clock::time_point deadline) const {
    std::unique_lock<std::mutex> lock(Mutex, std::defer_lock);
    while (!lock.try_lock()) {
        if (std::chrono::steady_clock::now() >= deadline) {
            return lock;
        }
        std::this_thread::yield();
    }
    return lock;
}

bool TqDarwinRelayRuntime::Start(const TqTuningConfig& tuning) {
    auto guard = AcquireRuntimeLock();
    if (State == TqRelayRuntimeState::Running) {
        return true;
    }
    if (State != TqRelayRuntimeState::Stopped || !Workers.empty()) {
        return false;
    }

    State = TqRelayRuntimeState::Starting;
    std::vector<std::unique_ptr<TqDarwinRelayWorker>> stagedWorkers;
    try {
        const uint32_t count = std::max<uint32_t>(1, tuning.RelayWorkerCount);
        stagedWorkers.reserve(count);
        for (uint32_t i = 0; i < count; ++i) {
            TqDarwinRelayWorkerConfig config{};
            config.WorkerIndex = i;
            config.EventBudget = tuning.RelayWorkerEventBudget;
            config.ByteBudgetPerTick = tuning.RelayWorkerByteBudgetPerTick;
            config.ReadChunkSize = tuning.RelayReadChunkSize;
            config.ReadBatchBytes = tuning.RelayReadBatchBytes;
            config.MaxIov = tuning.RelayMaxIov;
            config.TcpWriteMaxBytes = tuning.RelayTcpWriteMaxBytes;
            config.TcpWriteBurstBytes = tuning.RelayTcpWriteBurstBytes;
            config.MaxPendingQuicReceiveBytesPerRelay = tuning.RelayPerTunnelPendingBytes;
            config.DeferredReceiveCompleteBatchBytes = tuning.RelayQuicReceiveCompleteBatchBytes;
            config.MaxInFlightQuicSends = tuning.RelayMaxInFlightSends;
            config.MaxBufferedQuicSendBytes = tuning.MaxPendingBufferBytesPerRelay;
            config.EventQueueCapacity = tuning.RelayEventQueueCapacity;
#if defined(TQ_UNIT_TESTING)
            config.FailStartForTest = static_cast<int32_t>(i) == FailStartWorkerIndexForTest;
            config.BeforeSnapshotHookForTest = BeforeWorkerSnapshotHookForTest;
#endif

            auto worker = std::make_unique<TqDarwinRelayWorker>(config);
            if (!worker->Start()) {
                for (auto& startedWorker : stagedWorkers) {
                    startedWorker->Stop();
                }
                State = TqRelayRuntimeState::Stopped;
                return false;
            }
            stagedWorkers.push_back(std::move(worker));
        }

        Workers.swap(stagedWorkers);
        NextWorker = 0;
        State = TqRelayRuntimeState::Running;
        return true;
    } catch (...) {
        for (auto& startedWorker : stagedWorkers) {
            startedWorker->Stop();
        }
        State = TqRelayRuntimeState::Stopped;
        return false;
    }
}

void TqDarwinRelayRuntime::Stop() {
    auto guard = AcquireRuntimeLock();
    if (State == TqRelayRuntimeState::Stopped) {
        return;
    }
    State = TqRelayRuntimeState::Stopping;
    // Stop does not wait for the snapshot execution permit. Worker Stop/purge
    // cancels outstanding Snapshot commands so late permits can release.
    SnapshotSupport.WaitForIdleLocked(guard);
    for (auto& worker : Workers) {
        worker->Stop();
    }
    Workers.clear();
    NextWorker = 0;
    State = TqRelayRuntimeState::Stopped;
}

TqDarwinRelayWorker* TqDarwinRelayRuntime::PickWorker() {
    auto guard = AcquireRuntimeLock();
    if (State != TqRelayRuntimeState::Running || Workers.empty()) {
        return nullptr;
    }

    TqDarwinRelayWorker* worker = Workers[NextWorker % Workers.size()].get();
    ++NextWorker;
    return worker;
}

TqDarwinRelayWorkerSnapshot TqDarwinRelayRuntime::Snapshot() const {
    return Snapshot(std::chrono::steady_clock::now() + std::chrono::seconds(5));
}

TqDarwinRelayWorkerSnapshot TqDarwinRelayRuntime::Snapshot(
    std::chrono::steady_clock::time_point deadline) const {
    const auto result = SnapshotWorkers(deadline);
    TqDarwinRelayWorkerSnapshot snapshot{};
    snapshot.SnapshotComplete = result.SnapshotComplete;
    for (const auto& part : result.Workers) {
        TqAccumulateDarwinRelayWorkerSnapshot(snapshot, part);
    }
    if (result.Workers.empty() && result.SnapshotComplete) {
        snapshot.SnapshotComplete = true;
    }
    return snapshot;
}

TqRelayRuntimeSnapshotStats TqDarwinRelayRuntime::SnapshotSupportStats() const {
    return SnapshotSupport.Stats();
}

TqRelayRuntimeSnapshotExecutionGateStats
TqDarwinRelayRuntime::SnapshotExecutionGateStats() const {
    return SnapshotExecutionGate.Stats();
}

TqRelayRuntimeSnapshotResult<TqDarwinRelayWorkerSnapshot>
TqDarwinRelayRuntime::SnapshotWorkers() const {
    return SnapshotWorkers(std::chrono::steady_clock::now() + std::chrono::seconds(5));
}

TqRelayRuntimeSnapshotResult<TqDarwinRelayWorkerSnapshot>
TqDarwinRelayRuntime::SnapshotWorkers(
    std::chrono::steady_clock::time_point deadline) const {
    TqRelayRuntimeSnapshotResult<TqDarwinRelayWorkerSnapshot> result{};

    // Darwin serializes outstanding snapshot commands via the execution gate.
    // Acquire the permit before the runtime lock so gate mutex never nests with
    // Runtime mutex / SnapshotLock.
    auto permit = SnapshotExecutionGate.TryAcquire(deadline);
    if (!permit) {
        SnapshotSupport.RecordFailure();
        return result;
    }

    auto runtimeGuard = TryAcquireRuntimeLockForSnapshot(deadline);
    if (!runtimeGuard.owns_lock()) {
        SnapshotSupport.RecordFailure();
        return result;
    }

    if (State != TqRelayRuntimeState::Stopped && State != TqRelayRuntimeState::Running) {
        SnapshotSupport.RecordFailure();
        return result;
    }

    try {
        auto lease = SnapshotSupport.AcquireWorkersLocked(runtimeGuard, Workers);
        const bool stopped = State == TqRelayRuntimeState::Stopped;
        runtimeGuard.unlock();

        result.IdentitiesComplete = true;
        result.SnapshotComplete = true;
        result.Workers.reserve(lease.Workers().size());
        for (const auto& workerRef : lease.Workers()) {
            TqDarwinRelayWorkerSnapshot snapshot{};
            try {
                // Shared permit ownership: each worker Snapshot may move a copy
                // onto its command so a detached late command keeps Busy=1.
                snapshot = workerRef.Worker->Snapshot(deadline, permit);
            } catch (...) {
                SnapshotSupport.RecordFailure();
                snapshot = TqDarwinRelayWorkerSnapshot{};
                snapshot.SnapshotComplete = false;
            }
            // Slot identity always comes from the lease, never worker fallback.
            snapshot.WorkerIndex = workerRef.WorkerIndex;
            result.SnapshotComplete = result.SnapshotComplete && snapshot.SnapshotComplete;
            result.Workers.push_back(std::move(snapshot));
        }
        if (stopped) {
            result.SnapshotComplete = true;
        }
        if (!result.SnapshotComplete) {
            SnapshotExecutionGate.RecordDetachedLateCommand();
        }
    } catch (...) {
        SnapshotSupport.RecordFailure();
        result.SnapshotComplete = false;
    }
    return result;
}

#if defined(TQ_UNIT_TESTING)
void TqDarwinRelayRuntime::SetFailStartWorkerIndexForTest(int32_t workerIndex) {
    auto guard = AcquireRuntimeLock();
    assert(State == TqRelayRuntimeState::Stopped);
    FailStartWorkerIndexForTest = workerIndex;
}

void TqDarwinRelayRuntime::SetBeforeWorkerSnapshotHookForTest(
    void (*hook)(TqDarwinRelayWorker*)) {
    auto guard = AcquireRuntimeLock();
    assert(State == TqRelayRuntimeState::Stopped);
    BeforeWorkerSnapshotHookForTest = hook;
}

void TqDarwinRelayRuntime::FailNextWorkerRefMaterializationForTest() const {
    SnapshotSupport.FailNextWorkerRefMaterializationForTest();
}

TqRelayRuntimeSnapshotExecutionGateStats
TqDarwinRelayRuntime::SnapshotExecutionGateStatsForTest() const {
    return SnapshotExecutionGate.Stats();
}
#endif

#endif
