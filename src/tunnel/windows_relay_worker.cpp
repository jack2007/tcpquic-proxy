#include "windows_relay_worker.h"

#if defined(_WIN32)

#include "msquic.hpp"
#include "quic_receive_guard.h"
#include "relay_buffer.h"
#include "trace.h"

#include <algorithm>
#include <cassert>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <functional>
#include <limits>
#include <string>
#include <spdlog/spdlog.h>

#include <windows.h>

const MsQuicApi* MsQuic = nullptr;

namespace {

uint64_t NowSteadyNanos() {
    return static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count());
}

std::mutex& RuntimeWorkerLifetimeLock() {
    static std::mutex lock;
    return lock;
}

uint64_t CurrentThreadToken() {
    const auto token = static_cast<uint64_t>(std::hash<std::thread::id>{}(std::this_thread::get_id()));
    return token == 0 ? 1 : token;
}

template <typename Command>
void CompleteWindowsRelayCommand(Command& command) {
    {
        std::lock_guard<std::mutex> guard(command.Mutex);
        command.Done = true;
    }
    command.Cv.notify_one();
}

template <typename Command>
void WaitWindowsRelayCommand(Command& command) {
    std::unique_lock<std::mutex> guard(command.Mutex);
    command.Cv.wait(guard, [&command] { return command.Done; });
}

} // namespace

#if defined(TQ_UNIT_TESTING)
thread_local bool g_InSendCompleteCallback = false;

static const char* TestCallbackTypeName(TqWindowsIocpOperationType type) {
    switch (type) {
    case TqWindowsIocpOperationType::RelayReceiveReady:
        return "RelayReceiveReady";
    case TqWindowsIocpOperationType::QuicIdealSendBuffer:
        return "QuicIdealSendBuffer";
    case TqWindowsIocpOperationType::QuicPeerSendAborted:
        return "QuicPeerSendAborted";
    case TqWindowsIocpOperationType::QuicPeerReceiveAborted:
        return "QuicPeerReceiveAborted";
    case TqWindowsIocpOperationType::QuicShutdownComplete:
        return "QuicShutdownComplete";
    case TqWindowsIocpOperationType::QuicSendComplete:
        return "QuicSendComplete";
    default:
        return "Other";
    }
}
#endif

constexpr const char* kWindowsRelayCloseReasonUnknown = "unknown";
constexpr const char* kWindowsRelayCloseReasonDefault = "close_relay";

void UpdateAtomicMax(std::atomic<uint64_t>& target, uint64_t value) {
    uint64_t observed = target.load(std::memory_order_relaxed);
    while (observed < value &&
           !target.compare_exchange_weak(
               observed, value, std::memory_order_relaxed, std::memory_order_relaxed)) {
    }
}

uint64_t SaturatingFetchSub(std::atomic<uint64_t>& value, uint64_t delta) {
    uint64_t current = value.load(std::memory_order_relaxed);
    for (;;) {
        const uint64_t next = current > delta ? current - delta : 0;
        if (value.compare_exchange_weak(
                current, next, std::memory_order_relaxed, std::memory_order_relaxed)) {
            return next;
        }
    }
}

bool IsCallbackSourcedOperation(TqWindowsIocpOperationType type) {
    return type == TqWindowsIocpOperationType::RelayReceiveReady ||
           type == TqWindowsIocpOperationType::QuicIdealSendBuffer ||
           type == TqWindowsIocpOperationType::QuicPeerSendAborted ||
           type == TqWindowsIocpOperationType::QuicPeerReceiveAborted ||
           type == TqWindowsIocpOperationType::QuicShutdownComplete;
}

struct TqWindowsQuicReceiveSlice {
    const uint8_t* Data{nullptr};
    uint32_t Length{0};
};

struct TqWindowsPendingQuicReceive {
    MsQuicStream* Stream{nullptr};
    uint64_t RelayId{0};
    uint64_t Generation{0};
    std::vector<uint8_t> OwnedBuffer;
    std::vector<TqWindowsQuicReceiveSlice> Slices;
    size_t SliceIndex{0};
    size_t SliceOffset{0};
    uint64_t TotalLength{0};
    uint64_t CompletedLength{0};
    uint64_t AccountedLength{0};
    uint64_t PendingCompleteBytes{0};
    uint64_t CompleteBatchBytes{0};
    bool Fin{false};
    bool Drained{false};
    bool TcpSendPending{false};
};

struct TqWindowsQuicSendOperation {
    static constexpr uint64_t MagicValue = 0x54515753454e4431ull;
    uint64_t Magic{MagicValue};
    uint64_t RelayId{0};
    uint64_t Generation{0};
    uint64_t TotalBytes{0};
    bool Fin{false};
    QUIC_SEND_FLAGS Flags{QUIC_SEND_FLAG_NONE};
    std::vector<TqBufferRef> Buffers;
    std::vector<uint8_t> OwnedBytes;
    std::vector<QUIC_BUFFER> QuicBuffers;
};

struct TqWindowsRelayWorker::CallbackBinding {
    TqWindowsRelayWorker* Worker{nullptr};
    uint64_t RelayId{0};
    uint64_t Generation{0};
    std::atomic<RelayContext*> RelayHint{nullptr};
    std::atomic<uint32_t> CallbackRefs{0};
    std::atomic<bool> Closing{false};
};

struct TqWindowsRelayWorker::IoOperation {
    OVERLAPPED Overlapped{};
    TqWindowsIocpOperationType Event{TqWindowsIocpOperationType::TcpRecv};
    std::shared_ptr<RelayContext> Relay;
    uint64_t RelayId{0};
    uint64_t Generation{0};
    uint64_t Value{0};
    size_t Length{0};
    TqBufferRef BufferOwner;
    WSABUF WsaBuffer{};
    QUIC_BUFFER QuicBuffer{};
    std::vector<uint8_t> Buffer;
    std::shared_ptr<TqWindowsPendingQuicReceive> ReceiveView;
    size_t Offset{0};
    uint64_t PostedLength{0};
    QUIC_SEND_FLAGS QuicSendFlags{QUIC_SEND_FLAG_NONE};
    void* Control{nullptr};
};

struct TqWindowsRelayWorker::RegisterRelayCommand {
    TqSocketHandle TcpFd{TqInvalidSocket};
    MsQuicStream* Stream{nullptr};
    ITqCompressor* Compressor{nullptr};
    ITqDecompressor* Decompressor{nullptr};
    TqRelayHandle* Handle{nullptr};
    TqTuningConfig Tuning;
    TqCompressAlgo CompressAlgo{TqCompressAlgo::None};
    bool Ok{false};
    std::mutex Mutex;
    std::condition_variable Cv;
    bool Done{false};
};

struct TqWindowsRelayWorker::SnapshotCommand {
    TqWindowsRelayWorkerSnapshot Result{};
    std::mutex Mutex;
    std::condition_variable Cv;
    bool Done{false};
};

struct TqWindowsRelayWorker::RelayContext : TqRelayBufferBudget {
    explicit RelayContext(const TqTuningConfig& tuning) : Tuning(tuning) {
        MaxPendingBufferBytes = std::max<uint64_t>(
            tuning.WindowsRelayMaxPendingQuicReceiveBytesPerRelay,
            static_cast<uint64_t>(tuning.RelayIoSize) * std::max<uint32_t>(2, tuning.RelayMaxInFlightSends));
    }

    uint64_t Id{0};
    uint64_t Generation{0};
    uint64_t TraceTunnelId{0};
    std::string TraceTarget;
    TqSocketHandle TcpFd{TqInvalidSocket};
    MsQuicStream* Stream{nullptr};
    ITqCompressor* Compressor{nullptr};
    ITqDecompressor* Decompressor{nullptr};
    TqRelayHandle* PublicHandle{nullptr};
    TqTuningConfig Tuning;
    std::mutex TcpRecvOpsLock;
    std::vector<std::unique_ptr<IoOperation>> TcpRecvOpsFree;
    TqCompressAlgo CompressAlgo{TqCompressAlgo::None};
    std::atomic<bool> Closing{false};
    std::atomic<bool> TcpRecvClosed{false};
    std::atomic<bool> TcpWriteClosed{false};
    std::atomic<bool> CloseAfterDrained{false};
    std::atomic<bool> QuicSendFinSubmitted{false};
    std::atomic<bool> QuicSendFinCompleted{false};
    std::atomic<bool> StopPublished{false};
    std::atomic<uint32_t> ActiveHandlers{0};
    std::atomic<uint32_t> QueuedWorkerOps{0};
    std::atomic<uint32_t> InFlightTcpRecvs{0};
    std::atomic<uint32_t> InFlightQuicSends{0};
    std::atomic<uint32_t> InFlightTcpSends{0};
    std::atomic<uint64_t> PendingQuicReceiveBytes{0};
    std::atomic<uint64_t> PendingQuicReceiveQueueDepth{0};
    std::atomic<uint64_t> DeferredReceiveCompleteBatchPending{0};
    std::atomic<uint64_t> TcpReadBytes{0};
    std::atomic<uint64_t> TcpWriteBytes{0};
    std::atomic<uint64_t> LastTcpWriteErrno{0};
    std::atomic<uint64_t> LastTcpRecvErrno{0};
    std::atomic<uint64_t> LastTcpSendErrno{0};
    std::atomic<uint64_t> LastIocpCompletionErrno{0};
    std::atomic<uint32_t> LastIocpOperation{0};
    std::atomic<const char*> CloseReason{kWindowsRelayCloseReasonUnknown};
    std::mutex PendingReceiveLock;
    std::list<std::shared_ptr<TqWindowsPendingQuicReceive>> PendingReceives;
    std::mutex CallbackPendingQuicReceiveLock;
    std::deque<std::shared_ptr<TqWindowsPendingQuicReceive>> CallbackPendingQuicReceives;
    std::mutex PendingQuicSendLock;
    std::deque<std::unique_ptr<TqWindowsQuicSendOperation>> PendingQuicSendRetries;
    std::atomic<bool> TcpRecvPosted{false};
    std::atomic<bool> TcpReadPausedByQuicBacklog{false};
    std::atomic<uint64_t> OutstandingQuicSendBytes{0};
    std::atomic<uint64_t> MaxOutstandingQuicSendBytes{0};
    std::atomic<uint64_t> IdealSendBufferBytes{0};
    std::shared_ptr<CallbackBinding> Callback;
    std::atomic<bool> QuicReceivePaused{false};
    std::atomic<bool> ReceiveDrainQueued{false};
};

TqWindowsRelayWorker::TqWindowsRelayWorker(uint32_t workerIndex) : WorkerIndex_(workerIndex) {}
TqWindowsRelayWorker::~TqWindowsRelayWorker() { Stop(); }

bool TqWindowsRelayWorker::Start() {
    Iocp_ = ::CreateIoCompletionPort(INVALID_HANDLE_VALUE, nullptr, 0, 1);
    if (Iocp_ == nullptr) {
        Errors_.fetch_add(1, std::memory_order_relaxed);
        return false;
    }
    Stopping_.store(false);
    Thread_ = std::thread(&TqWindowsRelayWorker::Run, this);
    return true;
}

void TqWindowsRelayWorker::Stop() {
    if (Iocp_ == nullptr) {
        return;
    }
    {
        std::lock_guard<std::mutex> controlGuard(ControlCommandLock_);
        Stopping_.store(true, std::memory_order_release);
        PostStop();
    }
    if (Thread_.joinable()) {
        Thread_.join();
    }
    std::vector<std::shared_ptr<RelayContext>> relays;
    {
        std::lock_guard<std::mutex> guard(Lock_);
        relays.reserve(Relays_.size());
        for (const auto& entry : Relays_) {
            relays.push_back(entry.second);
        }
    }
    for (const auto& relay : relays) {
        CloseRelay(relay, TqRelayCloseMode::GracefulDrain, "worker_stop");
    }
    {
        std::lock_guard<std::mutex> guard(Lock_);
        Relays_.clear();
    }
    PruneRetiredCallbacks(false);
    {
        std::lock_guard<std::mutex> guard(Lock_);
        RetiredCallbacks_.clear();
    }
    ::CloseHandle(static_cast<HANDLE>(Iocp_));
    Iocp_ = nullptr;
}

TqWindowsRelayWorkerSnapshot TqWindowsRelayWorker::Snapshot() const {
    if (Iocp_ == nullptr || Stopping_.load(std::memory_order_acquire) ||
        WorkerThreadToken_.load(std::memory_order_acquire) == CurrentThreadToken()) {
        return BuildSnapshotLocal();
    }

    SnapshotCommand command{};
    {
        std::lock_guard<std::mutex> controlGuard(ControlCommandLock_);
        if (Iocp_ == nullptr || !Thread_.joinable() || Stopping_.load(std::memory_order_acquire) ||
            WorkerThreadToken_.load(std::memory_order_acquire) == CurrentThreadToken()) {
            return BuildSnapshotLocal();
        }
        auto op = std::make_unique<IoOperation>();
        op->Event = TqWindowsIocpOperationType::Snapshot;
        op->Control = &command;
        IoOperation* raw = op.release();
        if (!::PostQueuedCompletionStatus(static_cast<HANDLE>(Iocp_), 0, 0, &raw->Overlapped)) {
            delete raw;
            return BuildSnapshotLocal();
        }
        WaitWindowsRelayCommand(command);
    }
    return command.Result;
}

TqWindowsRelayWorkerSnapshot TqWindowsRelayWorker::BuildSnapshotLocal() const {
    const uint64_t snapshotStartNanos = NowSteadyNanos();
    TqWindowsRelayWorkerSnapshot snapshot{};
    snapshot.DeferredReceiveQueued = DeferredReceiveQueued_.load(std::memory_order_relaxed);
    snapshot.DeferredReceiveBytesQueued = DeferredReceiveBytesQueued_.load(std::memory_order_relaxed);
    snapshot.DeferredReceiveCompleteBytes = DeferredReceiveCompleteBytes_.load(std::memory_order_relaxed);
    snapshot.DeferredReceiveCompletes = DeferredReceiveCompletes_.load(std::memory_order_relaxed);
    snapshot.DeferredReceiveCompletionFlushes = DeferredReceiveCompletionFlushes_.load(std::memory_order_relaxed);
    snapshot.MaxPendingQuicReceiveBytes = MaxPendingQuicReceiveBytesObserved_.load(std::memory_order_relaxed);
    snapshot.MaxPendingQuicReceiveQueueDepth = MaxPendingQuicReceiveQueueObserved_.load(std::memory_order_relaxed);
    snapshot.QuicReceivePausedCount = QuicReceivePausedCount_.load(std::memory_order_relaxed);
    snapshot.QuicReceiveResumedCount = QuicReceiveResumedCount_.load(std::memory_order_relaxed);
    snapshot.TcpSendOperationsPosted = TcpSendOperationsPosted_.load(std::memory_order_relaxed);
    snapshot.TcpSendBytes = TcpSendBytes_.load(std::memory_order_relaxed);
    snapshot.TcpSendPartialCompletions = TcpSendPartialCompletions_.load(std::memory_order_relaxed);
    snapshot.TcpSendWouldBlockOrPendingCount = TcpSendWouldBlockOrPendingCount_.load(std::memory_order_relaxed);
    snapshot.TcpRecvOperationsCreated = TcpRecvOperationsCreated_.load(std::memory_order_relaxed);
    snapshot.TcpRecvOperationsReused = TcpRecvOperationsReused_.load(std::memory_order_relaxed);
    snapshot.ZstdDecompressInputBytes = ZstdDecompressInputBytes_.load(std::memory_order_relaxed);
    snapshot.ZstdDecompressOutputBytes = ZstdDecompressOutputBytes_.load(std::memory_order_relaxed);
    snapshot.ZstdDecompressCalls = ZstdDecompressCalls_.load(std::memory_order_relaxed);
    snapshot.ZstdDecompressNeedInput = ZstdDecompressNeedInput_.load(std::memory_order_relaxed);
    snapshot.ZstdDecompressNeedOutput = ZstdDecompressNeedOutput_.load(std::memory_order_relaxed);
    snapshot.ZstdDecompressFailures = ZstdDecompressFailures_.load(std::memory_order_relaxed);
    snapshot.FatalRelayResets = FatalRelayResets_.load(std::memory_order_relaxed);
    snapshot.GracefulRelayDrains = GracefulRelayDrains_.load(std::memory_order_relaxed);
    snapshot.TcpHardErrors = TcpHardErrors_.load(std::memory_order_relaxed);
    snapshot.QuicSendBackpressureEvents =
        QuicSendBackpressureEvents_.load(std::memory_order_relaxed);
    snapshot.QuicSendFatalErrors = QuicSendFatalErrors_.load(std::memory_order_relaxed);
    snapshot.QuicSendCompleteEvents = QuicSendCompleteEvents_.load(std::memory_order_relaxed);
    snapshot.WindowsCallbackIocpPostCount =
        CallbackIocpPostCount_.load(std::memory_order_relaxed);
    snapshot.WindowsCallbackIocpPostFailedCount =
        CallbackIocpPostFailedCount_.load(std::memory_order_relaxed);
    snapshot.WindowsReceiveReadyPostCount =
        ReceiveReadyPostCount_.load(std::memory_order_relaxed);
    snapshot.WindowsReceiveDrainScheduledCount =
        ReceiveDrainScheduledCount_.load(std::memory_order_relaxed);
    snapshot.WindowsReceiveDrainCoalescedCount =
        ReceiveDrainCoalescedCount_.load(std::memory_order_relaxed);
    snapshot.WindowsPostedCallbackStaleDropCount =
        PostedCallbackStaleDropCount_.load(std::memory_order_relaxed);
    snapshot.FindRelayByIdCount = FindRelayByIdCount_.load(std::memory_order_relaxed);
    snapshot.CallbackDispatchNanos = CallbackDispatchNanos_.load(std::memory_order_relaxed);
    snapshot.EventsProcessed = 0;
    snapshot.TcpReadResumeByBacklogEvents =
        TcpReadResumeByBacklogEvents_.load(std::memory_order_relaxed);
    snapshot.LateTeardownDowngradedCount =
        LateTeardownDowngradedCount_.load(std::memory_order_relaxed);
#if defined(TQ_UNIT_TESTING)
    snapshot.PostTcpRecvFromSendCompleteCallbackCount =
        PostTcpRecvFromSendCompleteCallbackCount_.load(std::memory_order_relaxed);
#endif
    snapshot.Errors = Errors_.load(std::memory_order_relaxed);
    snapshot.IocpCompletionDowngraded =
        IocpCompletionDowngraded_.load(std::memory_order_relaxed);
    snapshot.IocpStaleCompletionDropped =
        IocpStaleCompletionDropped_.load(std::memory_order_relaxed);
    snapshot.TcpSendZeroBytesGraceful =
        TcpSendZeroBytesGraceful_.load(std::memory_order_relaxed);
    std::vector<std::shared_ptr<RelayContext>> relays;
    {
        const uint64_t waitStartNanos = NowSteadyNanos();
        std::lock_guard<std::mutex> guard(Lock_);
        const uint64_t waitNanos = NowSteadyNanos() - waitStartNanos;
        snapshot.WorkerLockWaitNanos =
            WorkerLockWaitNanos_.fetch_add(waitNanos, std::memory_order_relaxed) + waitNanos;
        snapshot.WorkerLockAcquireCount =
            WorkerLockAcquireCount_.fetch_add(1, std::memory_order_relaxed) + 1;
        relays.reserve(Relays_.size());
        for (const auto& entry : Relays_) {
            if (entry.second) {
                relays.push_back(entry.second);
            }
        }
    }
    snapshot.ActiveRelays = relays.size();
    snapshot.ActiveRelayStates.reserve(relays.size());
    for (const auto& relay : relays) {
        const uint64_t pendingQuicReceiveBytes =
            relay->PendingQuicReceiveBytes.load(std::memory_order_relaxed);
        const uint64_t pendingQuicReceiveQueueDepth =
            relay->PendingQuicReceiveQueueDepth.load(std::memory_order_relaxed);
        snapshot.PendingQuicReceiveBytes += pendingQuicReceiveBytes;
        snapshot.PendingQuicReceiveQueueDepth += pendingQuicReceiveQueueDepth;
        const uint64_t pendingBufferBytes =
            relay->PendingBufferBytes.load(std::memory_order_relaxed);
        const uint64_t allocateCount =
            relay->AllocateCount.load(std::memory_order_relaxed);
        snapshot.RelayBufferBytesInUse += pendingBufferBytes;
        snapshot.RelayBufferAllocateCount += allocateCount;

        TqWindowsRelayActiveSnapshot active{};
        active.WorkerIndex = WorkerIndex_;
        active.RelayId = relay->Id;
        active.ActiveHandlers = relay->ActiveHandlers.load(std::memory_order_relaxed);
        active.QueuedWorkerOps = relay->QueuedWorkerOps.load(std::memory_order_relaxed);
        active.InFlightTcpRecvs = relay->InFlightTcpRecvs.load(std::memory_order_relaxed);
        active.InFlightTcpSends = relay->InFlightTcpSends.load(std::memory_order_relaxed);
        active.InFlightQuicSends = relay->InFlightQuicSends.load(std::memory_order_relaxed);
        active.PendingQuicReceiveBytes = pendingQuicReceiveBytes;
        active.PendingQuicReceiveQueueDepth = pendingQuicReceiveQueueDepth;
        active.TcpReadBytes = relay->TcpReadBytes.load(std::memory_order_relaxed);
        active.TcpWriteBytes = relay->TcpWriteBytes.load(std::memory_order_relaxed);
        active.LastTcpWriteErrno = relay->LastTcpWriteErrno.load(std::memory_order_relaxed);
        active.LastTcpRecvErrno = relay->LastTcpRecvErrno.load(std::memory_order_relaxed);
        active.LastTcpSendErrno = relay->LastTcpSendErrno.load(std::memory_order_relaxed);
        active.LastIocpCompletionErrno =
            relay->LastIocpCompletionErrno.load(std::memory_order_relaxed);
        active.LastIocpOperation = relay->LastIocpOperation.load(std::memory_order_relaxed);
        active.Closing = relay->Closing.load(std::memory_order_relaxed);
        active.TcpReadClosed = relay->TcpRecvClosed.load(std::memory_order_relaxed);
        active.TcpWriteClosed = relay->TcpWriteClosed.load(std::memory_order_relaxed);
        active.CloseAfterDrained = relay->CloseAfterDrained.load(std::memory_order_relaxed);
        active.QuicSendFinSubmitted =
            relay->QuicSendFinSubmitted.load(std::memory_order_relaxed);
        active.QuicSendFinCompleted =
            relay->QuicSendFinCompleted.load(std::memory_order_relaxed);
        active.StopPublished = relay->StopPublished.load(std::memory_order_relaxed);
        active.StreamDetached = relay->Stream == nullptr;
        active.TcpReadPausedByQuicBacklog =
            relay->TcpReadPausedByQuicBacklog.load(std::memory_order_relaxed);
        active.OutstandingQuicSendBytes =
            relay->OutstandingQuicSendBytes.load(std::memory_order_relaxed);
        active.MaxOutstandingQuicSendBytes =
            relay->MaxOutstandingQuicSendBytes.load(std::memory_order_relaxed);
        {
            std::lock_guard<std::mutex> pendingGuard(relay->CallbackPendingQuicReceiveLock);
            active.CallbackPendingQuicReceiveDepth =
                relay->CallbackPendingQuicReceives.size();
        }
        snapshot.ActiveRelayStates.push_back(active);
    }

    const uint64_t elapsedNanos = std::max<uint64_t>(1, NowSteadyNanos() - snapshotStartNanos);
    snapshot.SnapshotBuildNanos =
        SnapshotBuildNanos_.fetch_add(elapsedNanos, std::memory_order_relaxed) + elapsedNanos;
    snapshot.SnapshotActiveRelaysScanned =
        SnapshotActiveRelaysScanned_.fetch_add(relays.size(), std::memory_order_relaxed) +
        relays.size();
    return snapshot;
}

void TqWindowsRelayWorker::PostStop() {
    if (!::PostQueuedCompletionStatus(static_cast<HANDLE>(Iocp_), 0, 0, nullptr)) {
        Errors_.fetch_add(1, std::memory_order_relaxed);
    }
}

void TqWindowsRelayWorker::DrainRelayReceives(const std::shared_ptr<RelayContext>& relay) {
    if (!relay) {
        return;
    }

    for (;;) {
        std::shared_ptr<TqWindowsPendingQuicReceive> view;
        if (relay->PendingReceives.empty()) {
            return;
        }
        view = relay->PendingReceives.front();
        if (!view) {
            relay->PendingReceives.pop_front();
            continue;
        }

        if (view->TcpSendPending) {
            TraceReceiveViewEvent(relay, view, "drain_receive_send_pending");
            return;
        }

        if (relay->Closing.load(std::memory_order_acquire)) {
            TraceReceiveViewEvent(relay, view, "drain_receive_closing");
            (void)CompletePendingQuicReceive(relay, view);
            continue;
        }

        TraceReceiveViewEvent(relay, view, "drain_receive_front");
#if defined(TQ_UNIT_TESTING)
        if (!QuicReceiveViewDrainEnabledForTest_.load(std::memory_order_relaxed)) {
            return;
        }
#endif
        if (!TqSocketValid(relay->TcpFd)) {
            return;
        }
        const bool posted = relay->Decompressor != nullptr && relay->CompressAlgo == TqCompressAlgo::Zstd
            ? PostTcpSendFromCompressedReceiveView(relay, view)
            : PostTcpSendFromReceiveView(relay, view);
        if (!posted) {
            (void)CompletePendingQuicReceive(relay, view);
            CloseRelay(relay, TqRelayCloseMode::GracefulDrain, "post_tcp_send_receive_drain_failed");
        }
        return;
    }
}

bool TqWindowsRelayWorker::ScheduleRelayReceiveDrain(const std::shared_ptr<RelayContext>& relay) {
    if (Iocp_ == nullptr || !relay || relay->Closing.load(std::memory_order_acquire)) {
        return false;
    }
    if (relay->ReceiveDrainQueued.exchange(true, std::memory_order_acq_rel)) {
        ReceiveDrainCoalescedCount_.fetch_add(1, std::memory_order_relaxed);
        return true;
    }

    auto op = std::make_unique<IoOperation>();
    op->Event = TqWindowsIocpOperationType::RelayReceiveDrain;
    op->Relay = relay;
    op->RelayId = relay->Id;
    IoOperation* raw = op.release();
    relay->QueuedWorkerOps.fetch_add(1, std::memory_order_acq_rel);
    TraceReceiveViewEvent(relay, nullptr, "schedule_receive_drain");
    if (!::PostQueuedCompletionStatus(static_cast<HANDLE>(Iocp_), 0, 0, &raw->Overlapped)) {
        relay->QueuedWorkerOps.fetch_sub(1, std::memory_order_acq_rel);
        relay->ReceiveDrainQueued.store(false, std::memory_order_release);
        delete raw;
        return false;
    }
    ReceiveDrainScheduledCount_.fetch_add(1, std::memory_order_relaxed);
    return true;
}

void TqWindowsRelayWorker::ScheduleRelayReceiveDrainOrFail(
    const std::shared_ptr<RelayContext>& relay,
    const char* reason) {
    if (!relay || relay->Closing.load(std::memory_order_acquire)) {
        return;
    }
    if (!ScheduleRelayReceiveDrain(relay)) {
        FailRelayFatal(relay, reason);
    }
}

bool TqWindowsRelayWorker::PostCallbackOperation(
    TqWindowsIocpOperationType type,
    const std::shared_ptr<RelayContext>& relay,
    uint64_t value,
    size_t length) {
    if (Iocp_ == nullptr || !relay) {
        CallbackIocpPostFailedCount_.fetch_add(1, std::memory_order_relaxed);
        return false;
    }

    auto op = std::make_unique<IoOperation>();
    op->Event = type;
    op->Relay = relay;
    op->RelayId = relay->Id;
    op->Value = value;
    op->Length = length;
#if defined(TQ_UNIT_TESTING)
    const uint64_t postedRelayId = op->RelayId;
    const bool postedHadReceiveView = op->ReceiveView != nullptr;
#endif

    IoOperation* raw = op.release();
    relay->QueuedWorkerOps.fetch_add(1, std::memory_order_acq_rel);
    if (!::PostQueuedCompletionStatus(static_cast<HANDLE>(Iocp_), 0, 0, &raw->Overlapped)) {
        relay->QueuedWorkerOps.fetch_sub(1, std::memory_order_acq_rel);
        delete raw;
        CallbackIocpPostFailedCount_.fetch_add(1, std::memory_order_relaxed);
        return false;
    }
#if defined(TQ_UNIT_TESTING)
    {
        std::lock_guard<std::mutex> guard(LastPostedCallbackLock_);
        LastPostedCallbackType_ = type;
        LastPostedCallbackRelayId_ = postedRelayId;
        LastPostedCallbackHadReceiveView_ = postedHadReceiveView;
        PostedCallbackSequence_.push_back(type);
    }
#endif
    CallbackIocpPostCount_.fetch_add(1, std::memory_order_relaxed);
    if (type == TqWindowsIocpOperationType::RelayReceiveReady) {
        ReceiveReadyPostCount_.fetch_add(1, std::memory_order_relaxed);
    }
    return true;
}

bool TqWindowsRelayWorker::PostCallbackOperationById(
    TqWindowsIocpOperationType type,
    const CallbackBinding& binding,
    uint64_t value,
    size_t length,
    std::shared_ptr<TqWindowsPendingQuicReceive> receiveView) {
    if (Iocp_ == nullptr || binding.RelayId == 0) {
#if defined(TQ_UNIT_TESTING)
        LastCallbackPostWin32Error_.store(
            Iocp_ == nullptr ? ERROR_INVALID_HANDLE : ERROR_INVALID_PARAMETER,
            std::memory_order_relaxed);
#endif
        CallbackIocpPostFailedCount_.fetch_add(1, std::memory_order_relaxed);
        return false;
    }

    auto op = std::make_unique<IoOperation>();
    op->Event = type;
    op->RelayId = binding.RelayId;
    op->Generation = binding.Generation;
    op->Value = value;
    op->Length = length;
    op->ReceiveView = std::move(receiveView);
#if defined(TQ_UNIT_TESTING)
    const uint64_t postedRelayId = op->RelayId;
    const bool postedHadReceiveView = op->ReceiveView != nullptr;
#endif

    IoOperation* raw = op.release();
    if (!::PostQueuedCompletionStatus(static_cast<HANDLE>(Iocp_), 0, 0, &raw->Overlapped)) {
        delete raw;
#if defined(TQ_UNIT_TESTING)
        LastCallbackPostWin32Error_.store(::GetLastError(), std::memory_order_relaxed);
#endif
        CallbackIocpPostFailedCount_.fetch_add(1, std::memory_order_relaxed);
        return false;
    }
#if defined(TQ_UNIT_TESTING)
    {
        std::lock_guard<std::mutex> guard(LastPostedCallbackLock_);
        LastPostedCallbackType_ = type;
        LastPostedCallbackRelayId_ = postedRelayId;
        LastPostedCallbackHadReceiveView_ = postedHadReceiveView;
        PostedCallbackSequence_.push_back(type);
    }
#endif
    CallbackIocpPostCount_.fetch_add(1, std::memory_order_relaxed);
    if (type == TqWindowsIocpOperationType::RelayReceiveReady) {
        ReceiveReadyPostCount_.fetch_add(1, std::memory_order_relaxed);
    }
    return true;
}

void TqWindowsRelayWorker::QueueQuicSendCompleteFromCallback(
    TqWindowsQuicSendOperation* operation) {
    if (operation == nullptr) {
        return;
    }

    auto relay = FindRelayById(operation->RelayId);
    if (!relay) {
        std::unique_ptr<TqWindowsQuicSendOperation> cleanup(operation);
        return;
    }

    if (!PostCallbackOperation(
            TqWindowsIocpOperationType::QuicSendComplete,
            relay,
            reinterpret_cast<uintptr_t>(operation))) {
        std::unique_ptr<TqWindowsQuicSendOperation> cleanup(operation);
        CompleteQuicSendAccounting(relay, *cleanup);
        FailRelayFatal(relay, "post_quic_send_complete_failed");
    }
}

void TqWindowsRelayWorker::QueueQuicSendCompleteByIdFromCallback(
    const CallbackBinding& binding,
    TqWindowsQuicSendOperation* operation) {
    if (operation == nullptr) {
        return;
    }

    operation->RelayId = operation->RelayId != 0 ? operation->RelayId : binding.RelayId;
    operation->Generation =
        operation->Generation != 0 ? operation->Generation : binding.Generation;

    if (!PostCallbackOperationById(
            TqWindowsIocpOperationType::QuicSendComplete,
            binding,
            reinterpret_cast<uintptr_t>(operation))) {
        std::unique_ptr<TqWindowsQuicSendOperation> cleanup(operation);
        auto relay = ResolveRelayForSendComplete(cleanup->RelayId, cleanup->Generation);
        if (relay) {
            CompleteQuicSendAccounting(relay, *cleanup);
        }
        Errors_.fetch_add(1, std::memory_order_relaxed);
        if (relay) {
            if (relay->Closing.load(std::memory_order_acquire)) {
                TryRetireRelay(relay);
            } else {
                FailRelayFatal(relay, "post_quic_send_complete_failed");
            }
        }
    }
}

void TqWindowsRelayWorker::CompleteQuicSendAccounting(
    const std::shared_ptr<RelayContext>& relay,
    const TqWindowsQuicSendOperation& operation) {
    if (!relay) {
        return;
    }
    relay->InFlightQuicSends.fetch_sub(1, std::memory_order_acq_rel);
    SaturatingFetchSub(relay->OutstandingQuicSendBytes, operation.TotalBytes);
}

void TqWindowsRelayWorker::ProcessQuicSendCompleteOperation(
    uint64_t relayId,
    uint64_t generation,
    uintptr_t operationValue) {
    QuicSendCompleteEvents_.fetch_add(1, std::memory_order_relaxed);
    std::unique_ptr<TqWindowsQuicSendOperation> operation(
        reinterpret_cast<TqWindowsQuicSendOperation*>(operationValue));
    if (!operation || operation->Magic != TqWindowsQuicSendOperation::MagicValue) {
        return;
    }
    const uint64_t effectiveRelayId = relayId != 0 ? relayId : operation->RelayId;
    auto relay = FindRelayByIdLocal(effectiveRelayId);
    if (!relay || (generation != 0 && relay->Generation != generation)) {
        PostedCallbackStaleDropCount_.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    CompleteQuicSendAccounting(relay, *operation);
    if (operation->Fin) {
        relay->QuicSendFinCompleted.store(true, std::memory_order_release);
        TqTraceRelayStopCondition(
            "windows",
            WorkerIndex_,
            "quic_send_fin_completed",
            BuildRelayTraceState(relay));
        if (!relay->Closing.load(std::memory_order_acquire) &&
            relay->TcpRecvClosed.load(std::memory_order_acquire) &&
            relay->PendingQuicReceiveQueueDepth.load(std::memory_order_acquire) == 0 &&
            relay->InFlightTcpSends.load(std::memory_order_acquire) == 0 &&
            relay->PendingQuicReceiveBytes.load(std::memory_order_acquire) == 0 &&
            relay->TcpWriteBytes.load(std::memory_order_acquire) > 0) {
            relay->CloseAfterDrained.store(true, std::memory_order_release);
        }
    }
    if (relay->Closing.load(std::memory_order_acquire)) {
        TryRetireRelay(relay);
        return;
    }
    RetryPendingQuicSends(relay);
    MaybePostTcpRecv(relay);
    (void)CloseRelayIfDrained(relay);
}

void TqWindowsRelayWorker::SetTcpReadBackpressure(
    const std::shared_ptr<RelayContext>& relay,
    bool paused,
    const char* reason) {
    if (!relay) {
        return;
    }
    if (relay->TcpReadPausedByQuicBacklog.exchange(paused, std::memory_order_acq_rel) == paused) {
        return;
    }
    if (!paused) {
        TcpReadResumeByBacklogEvents_.fetch_add(1, std::memory_order_relaxed);
    }
    TraceRelayBackpressure(relay, paused ? "pause_tcp_read" : "resume_tcp_read", reason);
}

uint64_t TqWindowsRelayWorker::CurrentRelayIdealSendBytes(
    const std::shared_ptr<RelayContext>& relay) const {
    if (!relay) {
        return 0;
    }
    const uint64_t ideal = relay->IdealSendBufferBytes.load(std::memory_order_acquire);
    if (ideal != 0) {
        return ideal;
    }
    return relay->Tuning.WindowsRelayMaxBufferedQuicSendBytes;
}

bool TqWindowsRelayWorker::ShouldPauseTcpReadForQuicBacklog(
    const std::shared_ptr<RelayContext>& relay) const {
    const uint64_t threshold = CurrentRelayIdealSendBytes(relay);
    return relay && threshold != 0 &&
        relay->OutstandingQuicSendBytes.load(std::memory_order_acquire) >= threshold;
}

bool TqWindowsRelayWorker::ShouldResumeTcpReadForQuicBacklog(
    const std::shared_ptr<RelayContext>& relay) const {
    const uint64_t threshold = CurrentRelayIdealSendBytes(relay);
    if (!relay || threshold == 0) {
        return true;
    }
    return relay->OutstandingQuicSendBytes.load(std::memory_order_acquire) < threshold / 2;
}

bool TqWindowsRelayWorker::MaybePostTcpRecv(const std::shared_ptr<RelayContext>& relay) {
    if (!relay || relay->Closing.load(std::memory_order_acquire) ||
        relay->TcpRecvClosed.load(std::memory_order_acquire)) {
        return false;
    }
    if (ShouldPauseTcpReadForQuicBacklog(relay)) {
        SetTcpReadBackpressure(relay, true, "quic_send_backlog");
        return false;
    }
    if (relay->TcpReadPausedByQuicBacklog.load(std::memory_order_acquire) &&
        !ShouldResumeTcpReadForQuicBacklog(relay)) {
        return false;
    }
    SetTcpReadBackpressure(relay, false, "quic_send_backlog");
    if (relay->TcpFd == TqInvalidSocket) {
        return false;
    }
    bool expected = false;
    if (!relay->TcpRecvPosted.compare_exchange_strong(
            expected, true, std::memory_order_acq_rel, std::memory_order_acquire)) {
        return true;
    }
    if (!PostTcpRecv(relay)) {
        relay->TcpRecvPosted.store(false, std::memory_order_release);
        return false;
    }
    return true;
}

void TqWindowsRelayWorker::HandleQuicIdealSendBuffer(uint64_t relayId, uint64_t byteCount) {
    if (byteCount == 0) {
        return;
    }
    auto relay = FindRelayById(relayId);
    if (!relay || relay->Closing.load(std::memory_order_acquire)) {
        PostedCallbackStaleDropCount_.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    HandleQuicIdealSendBuffer(relay, byteCount);
}

void TqWindowsRelayWorker::HandleQuicIdealSendBuffer(
    const std::shared_ptr<RelayContext>& relay,
    uint64_t byteCount) {
    if (!relay || byteCount == 0) {
        return;
    }
    uint64_t ideal = relay->IdealSendBufferBytes.load(std::memory_order_relaxed);
    while (byteCount > ideal &&
        !relay->IdealSendBufferBytes.compare_exchange_weak(
            ideal, byteCount, std::memory_order_acq_rel, std::memory_order_relaxed)) {
    }
    RetryPendingQuicSends(relay);
    MaybePostTcpRecv(relay);
}

void TqWindowsRelayWorker::DrainPerRelayMaintenance() {
    std::vector<std::shared_ptr<RelayContext>> relays;
    {
        std::lock_guard<std::mutex> guard(Lock_);
        relays.reserve(Relays_.size());
        for (const auto& entry : Relays_) {
            relays.push_back(entry.second);
        }
    }
    for (const auto& relay : relays) {
        if (!relay) {
            continue;
        }
        RetryPendingQuicSends(relay);
        MaybePostTcpRecv(relay);
        (void)CloseRelayIfDrained(relay);
        TryRetireRelay(relay);
    }
}

std::shared_ptr<TqWindowsRelayWorker::RelayContext> TqWindowsRelayWorker::FindRelayById(
    uint64_t relayId) {
    FindRelayByIdCount_.fetch_add(1, std::memory_order_relaxed);
    const uint64_t waitStartNanos = NowSteadyNanos();
    std::unique_lock<std::mutex> guard(Lock_);
    WorkerLockAcquireCount_.fetch_add(1, std::memory_order_relaxed);
    WorkerLockWaitNanos_.fetch_add(
        NowSteadyNanos() - waitStartNanos, std::memory_order_relaxed);
    const auto it = Relays_.find(relayId);
    return it != Relays_.end() ? it->second : nullptr;
}

std::shared_ptr<TqWindowsRelayWorker::RelayContext> TqWindowsRelayWorker::FindRelayByIdLocal(
    uint64_t relayId) const {
    std::lock_guard<std::mutex> guard(Lock_);
    const auto it = Relays_.find(relayId);
    return it != Relays_.end() ? it->second : nullptr;
}

std::shared_ptr<TqWindowsRelayWorker::RelayContext> TqWindowsRelayWorker::ResolveRelayForCallback(
    uint64_t relayId,
    uint64_t generation) {
    auto relay = FindRelayById(relayId);
    if (!relay || relay->Generation != generation ||
        relay->Closing.load(std::memory_order_acquire)) {
        PostedCallbackStaleDropCount_.fetch_add(1, std::memory_order_relaxed);
        return nullptr;
    }
    return relay;
}

std::shared_ptr<TqWindowsRelayWorker::RelayContext> TqWindowsRelayWorker::ResolveRelayForSendComplete(
    uint64_t relayId,
    uint64_t generation) {
    auto relay = FindRelayById(relayId);
    if (!relay || relay->Generation != generation) {
        PostedCallbackStaleDropCount_.fetch_add(1, std::memory_order_relaxed);
        return nullptr;
    }
    return relay;
}

void TqWindowsRelayWorker::Run() {
    WorkerThreadToken_.store(CurrentThreadToken(), std::memory_order_release);
    while (!Stopping_.load()) {
        DrainPerRelayMaintenance();

        DWORD bytes = 0;
        ULONG_PTR key = 0;
        OVERLAPPED* overlapped = nullptr;
        const BOOL ok = ::GetQueuedCompletionStatus(
            static_cast<HANDLE>(Iocp_), &bytes, &key, &overlapped, INFINITE);
        const DWORD completionError = ok ? ERROR_SUCCESS : ::GetLastError();
        if (overlapped == nullptr) {
            DrainPerRelayMaintenance();
            if (Stopping_.load(std::memory_order_acquire)) {
                break;
            }
            continue;
        }
        std::unique_ptr<IoOperation> op(reinterpret_cast<IoOperation*>(overlapped));
        if (op->Event == TqWindowsIocpOperationType::RegisterRelay) {
            auto* command = static_cast<RegisterRelayCommand*>(op->Control);
            if (command != nullptr) {
                command->Ok = RegisterRelayLocal(*command);
                CompleteWindowsRelayCommand(*command);
            }
            continue;
        }
        if (op->Event == TqWindowsIocpOperationType::Snapshot) {
            auto* command = static_cast<SnapshotCommand*>(op->Control);
            if (command != nullptr) {
                command->Result = BuildSnapshotLocal();
                CompleteWindowsRelayCommand(*command);
            }
            continue;
        }
        const bool carriesRelayRef = static_cast<bool>(op->Relay);
        const bool callbackSourced = IsCallbackSourcedOperation(op->Event);
        auto relay = op->Relay;
        if (!relay && callbackSourced) {
            relay = FindRelayByIdLocal(op->RelayId);
            if (!relay || relay->Generation != op->Generation) {
                PostedCallbackStaleDropCount_.fetch_add(1, std::memory_order_relaxed);
                relay.reset();
            } else if (relay->Closing.load(std::memory_order_acquire) &&
                       op->Event == TqWindowsIocpOperationType::RelayReceiveReady) {
                PostedCallbackStaleDropCount_.fetch_add(1, std::memory_order_relaxed);
                relay.reset();
            }
            if (!relay && op->ReceiveView) {
                CompleteRemainingReceiveOwnership(*op->ReceiveView);
            }
        }
        if (!relay && op->Event == TqWindowsIocpOperationType::CloseRelay && op->RelayId != 0) {
            relay = FindRelayByIdLocal(op->RelayId);
        }
        if (!relay && op->Event != TqWindowsIocpOperationType::QuicSendComplete) {
            if (!ok) {
                DropStaleCompletionWithoutRelay(completionError);
            }
            if (!callbackSourced ||
                op->Event == TqWindowsIocpOperationType::RelayReceiveReady) {
                continue;
            }
        }
        if (!ok && !relay && op->Event == TqWindowsIocpOperationType::QuicSendComplete) {
            ProcessQuicSendCompleteOperation(
                op->RelayId,
                op->Generation,
                static_cast<uintptr_t>(op->Value));
            op->Value = 0;
            continue;
        }
        if (op->Event != TqWindowsIocpOperationType::TcpRecv &&
            op->Event != TqWindowsIocpOperationType::TcpSend &&
            carriesRelayRef) {
            relay->QueuedWorkerOps.fetch_sub(1, std::memory_order_acq_rel);
        }
        struct HandlerGuard {
            TqWindowsRelayWorker* Worker{nullptr};
            std::shared_ptr<RelayContext> Relay;
            HandlerGuard(TqWindowsRelayWorker* worker, std::shared_ptr<RelayContext> relay)
                : Worker(worker), Relay(std::move(relay)) {
                if (Relay) {
                    Relay->ActiveHandlers.fetch_add(1, std::memory_order_acq_rel);
                }
            }
            ~HandlerGuard() {
                if (Relay) {
                    Relay->ActiveHandlers.fetch_sub(1, std::memory_order_acq_rel);
                    Worker->TryRetireRelay(Relay);
                }
            }
        } handlerGuard{this, relay};
        if (!ok) {
            if (relay && op->Event == TqWindowsIocpOperationType::TcpRecv) {
                relay->InFlightTcpRecvs.fetch_sub(1, std::memory_order_acq_rel);
                if (completionError != ERROR_SUCCESS) {
                    StoreIocpCompletionErrno(
                        relay,
                        static_cast<uint32_t>(TqWindowsIocpOperationType::TcpRecv),
                        completionError);
                }
                if (IsIocpTeardownError(completionError)) {
                    TqTraceRelayStopCondition(
                        "windows", WorkerIndex_, "iocp_tcp_recv_teardown", BuildRelayTraceState(relay));
                    relay->TcpRecvClosed.store(true, std::memory_order_release);
                    op->BufferOwner.reset();
                    (void)CloseRelayIfDrained(relay);
                    continue;
                }
            } else if (relay && op->Event == TqWindowsIocpOperationType::TcpSend) {
                relay->InFlightTcpSends.fetch_sub(1, std::memory_order_acq_rel);
                if (completionError != ERROR_SUCCESS) {
                    StoreIocpCompletionErrno(
                        relay,
                        static_cast<uint32_t>(TqWindowsIocpOperationType::TcpSend),
                        completionError);
                }
                if (op->ReceiveView) {
                    (void)CompletePendingQuicReceive(relay, op->ReceiveView);
                }
                if (IsIocpTeardownError(completionError)) {
                    TqTraceRelayStopCondition(
                        "windows", WorkerIndex_, "iocp_tcp_send_teardown", BuildRelayTraceState(relay));
                    GracefulRelayDrains_.fetch_add(1, std::memory_order_relaxed);
                    CloseRelay(relay, TqRelayCloseMode::GracefulDrain, "iocp_tcp_send_teardown");
                    continue;
                }
            }
            bool downgrade = false;
            switch (op->Event) {
            case TqWindowsIocpOperationType::TcpRecv:
                downgrade = ShouldDowngradeTcpRecvCompletion(relay.get(), completionError);
                break;
            case TqWindowsIocpOperationType::TcpSend:
                downgrade = ShouldDowngradeTcpSendCompletion(relay.get(), completionError);
                break;
            default:
                downgrade = ShouldDowngradePostedWorkerCompletion(relay.get(), completionError);
                break;
            }
            if (downgrade) {
                DowngradeIocpCompletion(relay, "iocp_completion_error_teardown", completionError);
                continue;
            }
            if (relay->Closing.load(std::memory_order_acquire)) {
                TryRetireRelay(relay);
                continue;
            }
            RecordTcpHardErrorAndFail(relay, "iocp_completion_error", completionError);
            continue;
        }
        switch (op->Event) {
        case TqWindowsIocpOperationType::TcpRecv:
            HandleTcpRecv(std::move(op), bytes);
            break;
        case TqWindowsIocpOperationType::TcpSend:
            HandleTcpSend(std::move(op), bytes);
            break;
        case TqWindowsIocpOperationType::QuicSendComplete: {
            ProcessQuicSendCompleteOperation(
                op->RelayId,
                op->Generation,
                static_cast<uintptr_t>(op->Value));
            op->Value = 0;
            break;
        }
        case TqWindowsIocpOperationType::RelayReceiveReady:
            if (op->ReceiveView &&
                !EnqueueDeferredQuicReceiveView(relay, op->ReceiveView)) {
                CompleteRemainingReceiveOwnership(*op->ReceiveView);
                FailRelayFatal(relay, "quic_receive_queue_failed");
                break;
            }
            DrainRelayReceives(relay);
            break;
        case TqWindowsIocpOperationType::RelayReceiveDrain:
            if (op->Relay) {
                op->Relay->ReceiveDrainQueued.store(false, std::memory_order_release);
            }
            DrainRelayReceives(op->Relay);
            break;
        case TqWindowsIocpOperationType::QuicIdealSendBuffer:
            if (relay) {
                HandleQuicIdealSendBuffer(relay, op->Value);
            } else {
                HandleQuicIdealSendBuffer(op->RelayId, op->Value);
            }
            break;
        case TqWindowsIocpOperationType::QuicPeerSendAborted:
            if (relay) {
                ProcessQuicPeerAborted(relay, "stream_peer_send_aborted", op->Value);
            } else {
                ProcessQuicPeerAborted(op->RelayId, "stream_peer_send_aborted", op->Value);
            }
            break;
        case TqWindowsIocpOperationType::QuicPeerReceiveAborted:
            if (relay) {
                ProcessQuicPeerAborted(relay, "stream_peer_receive_aborted", op->Value);
            } else {
                ProcessQuicPeerAborted(op->RelayId, "stream_peer_receive_aborted", op->Value);
            }
            break;
        case TqWindowsIocpOperationType::QuicShutdownComplete:
            if (relay) {
                ProcessQuicShutdownComplete(
                    relay,
                    op->Value,
                    static_cast<uint32_t>(op->Length));
            } else {
                ProcessQuicShutdownComplete(
                    op->RelayId,
                    op->Value,
                    static_cast<uint32_t>(op->Length));
            }
            break;
        case TqWindowsIocpOperationType::QuicSendRetry:
            RetryPendingQuicSends(op->Relay);
            break;
        case TqWindowsIocpOperationType::CloseRelay:
            CloseRelay(relay, TqRelayCloseMode::GracefulDrain, "stop_relay_request");
            break;
        default:
            break;
        }

        DrainPerRelayMaintenance();
    }
    WorkerThreadToken_.store(0, std::memory_order_release);
}

bool TqWindowsRelayWorker::RegisterRelay(
    TqSocketHandle tcpFd,
    MsQuicStream* stream,
    ITqCompressor* compressor,
    ITqDecompressor* decompressor,
    TqRelayHandle* handle,
    const TqTuningConfig& tuning,
    TqCompressAlgo compressAlgo) {
    if (!TqSocketValid(tcpFd) || stream == nullptr || handle == nullptr) {
        return false;
    }
    RegisterRelayCommand command{};
    command.TcpFd = tcpFd;
    command.Stream = stream;
    command.Compressor = compressor;
    command.Decompressor = decompressor;
    command.Handle = handle;
    command.Tuning = tuning;
    command.CompressAlgo = compressAlgo;
    if (WorkerThreadToken_.load(std::memory_order_acquire) == CurrentThreadToken()) {
        return !Stopping_.load(std::memory_order_acquire) && RegisterRelayLocal(command);
    }
    {
        std::lock_guard<std::mutex> controlGuard(ControlCommandLock_);
        if (Iocp_ == nullptr || !Thread_.joinable() ||
            Stopping_.load(std::memory_order_acquire)) {
            return false;
        }
        auto op = std::make_unique<IoOperation>();
        op->Event = TqWindowsIocpOperationType::RegisterRelay;
        op->Control = &command;
        IoOperation* raw = op.release();
        if (!::PostQueuedCompletionStatus(static_cast<HANDLE>(Iocp_), 0, 0, &raw->Overlapped)) {
            delete raw;
            Errors_.fetch_add(1, std::memory_order_relaxed);
            return false;
        }
        WaitWindowsRelayCommand(command);
    }
    return command.Ok;
}

bool TqWindowsRelayWorker::RegisterRelayLocal(RegisterRelayCommand& command) {
    if (command.Stream == nullptr || command.Handle == nullptr || Iocp_ == nullptr) {
        return false;
    }
    if (TqSocketValid(command.TcpFd)) {
        if (::CreateIoCompletionPort(
                reinterpret_cast<HANDLE>(command.TcpFd),
                static_cast<HANDLE>(Iocp_),
                0,
                0) == nullptr) {
            Errors_.fetch_add(1, std::memory_order_relaxed);
            return false;
        }
    }

    auto relay = std::make_shared<RelayContext>(command.Tuning);
    relay->Id = NextRelayId_.fetch_add(1);
    relay->Generation = NextGeneration_.fetch_add(1, std::memory_order_relaxed);
    relay->TcpFd = command.TcpFd;
    relay->Stream = command.Stream;
    relay->Compressor = command.Compressor;
    relay->Decompressor = command.Decompressor;
    relay->PublicHandle = command.Handle;
    relay->Tuning = command.Tuning;
    relay->CompressAlgo = command.CompressAlgo;
    relay->Callback = std::make_shared<CallbackBinding>();
    relay->Callback->Worker = this;
    relay->Callback->RelayId = relay->Id;
    relay->Callback->Generation = relay->Generation;
    relay->Callback->RelayHint.store(relay.get(), std::memory_order_release);
    command.Stream->Callback = StreamCallback;
    command.Stream->Context = relay->Callback.get();

    {
        std::lock_guard<std::mutex> guard(Lock_);
        Relays_[relay->Id] = relay;
    }

    command.Handle->Backend = TqRelayBackendType::WindowsWorker;
    command.Handle->WindowsWorker = this;
    command.Handle->WindowsRelayId = relay->Id;
    command.Handle->WindowsWorkerIndex = WorkerIndex_;

    command.Ok = TqSocketValid(command.TcpFd) ? MaybePostTcpRecv(relay) : true;
    return command.Ok;
}

#if defined(TQ_UNIT_TESTING)
void TqWindowsRelayWorker::SetQuicReceiveViewDrainEnabledForTest(bool enabled) {
    QuicReceiveViewDrainEnabledForTest_.store(enabled, std::memory_order_relaxed);
}

bool TqWindowsRelayWorker::RegisterRelayForTest(
    MsQuicStream* stream,
    TqRelayHandle* handle,
    const TqTuningConfig& tuning,
    TqCompressAlgo compressAlgo) {
    if (stream == nullptr || handle == nullptr) {
        return false;
    }

    auto relay = std::make_shared<RelayContext>(tuning);
    relay->Id = NextRelayId_.fetch_add(1);
    relay->Generation = NextGeneration_.fetch_add(1, std::memory_order_relaxed);
    relay->TcpFd = TqInvalidSocket;
    relay->Stream = stream;
    relay->PublicHandle = handle;
    relay->Tuning = tuning;
    relay->CompressAlgo = compressAlgo;
    relay->Callback = std::make_shared<CallbackBinding>();
    relay->Callback->Worker = this;
    relay->Callback->RelayId = relay->Id;
    relay->Callback->Generation = relay->Generation;
    relay->Callback->RelayHint.store(relay.get(), std::memory_order_release);
    stream->Callback = StreamCallback;
    stream->Context = relay->Callback.get();

    {
        std::lock_guard<std::mutex> guard(Lock_);
        Relays_[relay->Id] = relay;
    }

    handle->Backend = TqRelayBackendType::WindowsWorker;
    handle->WindowsWorker = this;
    handle->WindowsRelayId = relay->Id;
    handle->WindowsWorkerIndex = WorkerIndex_;
    return true;
}

bool TqWindowsRelayWorker::TestCompleteReceiveViewForCleanup(uint64_t relayId, uint64_t completedLength) {
    std::shared_ptr<RelayContext> relay;
    {
        std::lock_guard<std::mutex> guard(Lock_);
        const auto it = Relays_.find(relayId);
        if (it != Relays_.end()) {
            relay = it->second;
        }
    }
    if (!relay) {
        return false;
    }
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(2000);
    while (relay->PendingQuicReceiveQueueDepth.load(std::memory_order_acquire) == 0 &&
           std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    std::shared_ptr<TqWindowsPendingQuicReceive> view;
    {
        std::lock_guard<std::mutex> guard(relay->PendingReceiveLock);
        if (relay->PendingReceives.empty()) {
            return false;
        }
        view = relay->PendingReceives.front();
    }
    if (!view || completedLength > view->TotalLength) {
        return false;
    }
    view->CompletedLength = completedLength;
    view->AccountedLength = completedLength;
    SaturatingFetchSub(relay->PendingQuicReceiveBytes, completedLength);
    return CompletePendingQuicReceive(relay, view);
}

bool TqWindowsRelayWorker::TestLastPostedCallbackWasReceiveReadyForTest(uint64_t relayId) const {
    std::lock_guard<std::mutex> guard(LastPostedCallbackLock_);
    return LastPostedCallbackType_ == TqWindowsIocpOperationType::RelayReceiveReady &&
        LastPostedCallbackRelayId_ == relayId && LastPostedCallbackHadReceiveView_;
}

bool TqWindowsRelayWorker::TestLastPostedCallbackWasQuicSendCompleteForTest(uint64_t relayId) const {
    std::lock_guard<std::mutex> guard(LastPostedCallbackLock_);
    return LastPostedCallbackType_ == TqWindowsIocpOperationType::QuicSendComplete &&
        LastPostedCallbackRelayId_ == relayId;
}

bool TqWindowsRelayWorker::TestPostedCallbackSequenceForTest(const char* expectedCsv) const {
    std::lock_guard<std::mutex> guard(LastPostedCallbackLock_);
    std::string actual;
    for (TqWindowsIocpOperationType type : PostedCallbackSequence_) {
        if (!actual.empty()) {
            actual.push_back(',');
        }
        actual += TestCallbackTypeName(type);
    }
    return actual == (expectedCsv != nullptr ? expectedCsv : "");
}

TqWindowsQuicSendOperation* TqWindowsRelayWorker::TestCreateQuicSendOperationForTest(
    uint64_t relayId,
    uint64_t bytes) {
    auto relay = FindRelayById(relayId);
    if (!relay) {
        return nullptr;
    }
    relay->InFlightQuicSends.fetch_add(1, std::memory_order_acq_rel);
    relay->OutstandingQuicSendBytes.fetch_add(bytes, std::memory_order_acq_rel);
    auto* operation = new TqWindowsQuicSendOperation();
    operation->RelayId = relayId;
    operation->Generation = relay->Generation;
    operation->TotalBytes = bytes;
    return operation;
}

bool TqWindowsRelayWorker::TestResolveStaleCallbackForTest(uint64_t relayId) {
    auto relay = FindRelayById(relayId);
    if (!relay || !relay->Callback) {
        return false;
    }
    const uint64_t before = PostedCallbackStaleDropCount_.load(std::memory_order_relaxed);
    auto staleRelay = ResolveRelayForCallback(relayId, relay->Generation + 1);
    return staleRelay == nullptr &&
           PostedCallbackStaleDropCount_.load(std::memory_order_relaxed) == before + 1;
}

bool TqWindowsRelayWorker::TestDispatchIdealSendBufferByIdForTest(
    uint64_t relayId,
    uint64_t byteCount) {
    auto relay = FindRelayById(relayId);
    if (!relay || !relay->Callback || byteCount == 0) {
        return false;
    }
    auto callback = relay->Callback;
    if (!PostCallbackOperationById(
            TqWindowsIocpOperationType::QuicIdealSendBuffer,
            *callback,
            byteCount)) {
        return false;
    }

    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(2000);
    while (std::chrono::steady_clock::now() < deadline) {
        if (relay->IdealSendBufferBytes.load(std::memory_order_acquire) >= byteCount) {
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return relay->IdealSendBufferBytes.load(std::memory_order_acquire) >= byteCount;
}

bool TqWindowsRelayWorker::TestCreateIocpForCallbackPostOnly() {
    if (Iocp_ != nullptr) {
        return true;
    }
    Iocp_ = ::CreateIoCompletionPort(INVALID_HANDLE_VALUE, nullptr, 0, 1);
    if (Iocp_ == nullptr) {
        Errors_.fetch_add(1, std::memory_order_relaxed);
        return false;
    }
    Stopping_.store(false, std::memory_order_release);
    return true;
}

bool TqWindowsRelayWorker::TestDrainSingleQuicSendCompleteForTest() {
    if (Iocp_ == nullptr) {
        return false;
    }

    DWORD bytes = 0;
    ULONG_PTR key = 0;
    OVERLAPPED* overlapped = nullptr;
    const BOOL ok = ::GetQueuedCompletionStatus(
        static_cast<HANDLE>(Iocp_), &bytes, &key, &overlapped, 0);
    (void)bytes;
    (void)key;
    if (overlapped == nullptr) {
        return false;
    }

    std::unique_ptr<IoOperation> op(reinterpret_cast<IoOperation*>(overlapped));
    if (op->Event != TqWindowsIocpOperationType::QuicSendComplete) {
        return false;
    }
    ProcessQuicSendCompleteOperation(
        op->RelayId,
        op->Generation,
        static_cast<uintptr_t>(op->Value));
    op->Value = 0;
    return ok != FALSE;
}

bool TqWindowsRelayWorker::TestDrainSingleReceiveReadyForTest() {
    if (Iocp_ == nullptr) {
        return false;
    }

    DWORD bytes = 0;
    ULONG_PTR key = 0;
    OVERLAPPED* overlapped = nullptr;
    const BOOL ok = ::GetQueuedCompletionStatus(
        static_cast<HANDLE>(Iocp_), &bytes, &key, &overlapped, 0);
    (void)bytes;
    (void)key;
    if (overlapped == nullptr) {
        return false;
    }

    std::unique_ptr<IoOperation> op(reinterpret_cast<IoOperation*>(overlapped));
    if (op->Event != TqWindowsIocpOperationType::RelayReceiveReady) {
        return false;
    }

    auto relay = op->Relay;
    if (!relay) {
        relay = ResolveRelayForCallback(op->RelayId, op->Generation);
        if (!relay) {
            if (op->ReceiveView) {
                CompleteRemainingReceiveOwnership(*op->ReceiveView);
            }
            return false;
        }
    }
    if (op->ReceiveView && !EnqueueDeferredQuicReceiveView(relay, op->ReceiveView)) {
        CompleteRemainingReceiveOwnership(*op->ReceiveView);
        FailRelayFatal(relay, "quic_receive_queue_failed");
        return false;
    }
    DrainRelayReceives(relay);
    return ok != FALSE;
}

bool TqWindowsRelayWorker::TestDrainPostedCallbackOperationsForTest(size_t expectedCount) {
    if (Iocp_ == nullptr) {
        return false;
    }

    bool success = true;
    for (size_t drained = 0; drained < expectedCount; ++drained) {
        DWORD bytes = 0;
        ULONG_PTR key = 0;
        OVERLAPPED* overlapped = nullptr;
        const BOOL ok = ::GetQueuedCompletionStatus(
            static_cast<HANDLE>(Iocp_), &bytes, &key, &overlapped, 0);
        (void)bytes;
        (void)key;
        if (overlapped == nullptr) {
            return false;
        }

        std::unique_ptr<IoOperation> op(reinterpret_cast<IoOperation*>(overlapped));
        if (!IsCallbackSourcedOperation(op->Event)) {
            if (op->ReceiveView) {
                CompleteRemainingReceiveOwnership(*op->ReceiveView);
            }
            success = false;
            continue;
        }

        bool resolvedCallbackById = false;
        auto relay = op->Relay;
        if (!relay) {
            relay = ResolveRelayForCallback(op->RelayId, op->Generation);
            resolvedCallbackById = static_cast<bool>(relay);
            if (!relay && op->ReceiveView) {
                CompleteRemainingReceiveOwnership(*op->ReceiveView);
            }
        }
        if (!relay) {
            continue;
        }

        switch (op->Event) {
        case TqWindowsIocpOperationType::RelayReceiveReady:
            if (op->ReceiveView &&
                !EnqueueDeferredQuicReceiveView(relay, op->ReceiveView)) {
                CompleteRemainingReceiveOwnership(*op->ReceiveView);
                FailRelayFatal(relay, "quic_receive_queue_failed");
                success = false;
                break;
            }
            DrainRelayReceives(relay);
            break;
        case TqWindowsIocpOperationType::QuicIdealSendBuffer:
            if (resolvedCallbackById) {
                HandleQuicIdealSendBuffer(relay, op->Value);
            } else {
                HandleQuicIdealSendBuffer(op->RelayId, op->Value);
            }
            break;
        case TqWindowsIocpOperationType::QuicPeerSendAborted:
            if (resolvedCallbackById) {
                ProcessQuicPeerAborted(relay, "stream_peer_send_aborted", op->Value);
            } else {
                ProcessQuicPeerAborted(op->RelayId, "stream_peer_send_aborted", op->Value);
            }
            break;
        case TqWindowsIocpOperationType::QuicPeerReceiveAborted:
            if (resolvedCallbackById) {
                ProcessQuicPeerAborted(relay, "stream_peer_receive_aborted", op->Value);
            } else {
                ProcessQuicPeerAborted(op->RelayId, "stream_peer_receive_aborted", op->Value);
            }
            break;
        case TqWindowsIocpOperationType::QuicShutdownComplete:
            if (resolvedCallbackById) {
                ProcessQuicShutdownComplete(
                    relay,
                    op->Value,
                    static_cast<uint32_t>(op->Length));
            } else {
                ProcessQuicShutdownComplete(
                    op->RelayId,
                    op->Value,
                    static_cast<uint32_t>(op->Length));
            }
            break;
        default:
            success = false;
            break;
        }
        success = success && ok != FALSE;
    }
    return success;
}

bool TqWindowsRelayWorker::TestNoWorkerEventQueueReceiveViewForTest() const {
    std::lock_guard<std::mutex> guard(Lock_);
    for (const auto& entry : Relays_) {
        const auto& relay = entry.second;
        if (!relay) {
            continue;
        }
        std::lock_guard<std::mutex> pendingGuard(relay->CallbackPendingQuicReceiveLock);
        if (!relay->CallbackPendingQuicReceives.empty()) {
            return false;
        }
    }
    return true;
}

bool TqWindowsRelayWorker::TestAdvanceReceiveViewForCompletion(uint64_t relayId, uint64_t completedLength) {
    std::shared_ptr<RelayContext> relay;
    {
        std::lock_guard<std::mutex> guard(Lock_);
        const auto it = Relays_.find(relayId);
        if (it != Relays_.end()) {
            relay = it->second;
        }
    }
    if (!relay) {
        return false;
    }
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(2000);
    while (relay->PendingQuicReceiveQueueDepth.load(std::memory_order_acquire) == 0 &&
           std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    std::shared_ptr<TqWindowsPendingQuicReceive> view;
    {
        std::lock_guard<std::mutex> guard(relay->PendingReceiveLock);
        if (relay->PendingReceives.empty()) {
            return false;
        }
        view = relay->PendingReceives.front();
    }
    if (!view || completedLength == 0 || view->CompletedLength > view->TotalLength ||
        completedLength > view->TotalLength - view->CompletedLength) {
        return false;
    }
    AdvanceReceiveView(relay, *view, completedLength);
    const bool viewComplete = view->CompletedLength >= view->TotalLength;
    FlushDeferredReceiveCompletion(*view, false);
    if (viewComplete) {
        return FinishReceiveView(relay, view);
    }
    return true;
}

bool TqWindowsRelayWorker::TestLateReceiveViewCompletionIgnored(
    uint64_t relayId,
    DWORD bytes,
    uint64_t postedLength) {
    std::shared_ptr<RelayContext> relay;
    {
        std::lock_guard<std::mutex> guard(Lock_);
        const auto it = Relays_.find(relayId);
        if (it != Relays_.end()) {
            relay = it->second;
        }
    }
    if (!relay) {
        return false;
    }
    auto view = std::make_shared<TqWindowsPendingQuicReceive>();
    view->Stream = relay->Stream;
    view->RelayId = relay->Id;
    view->TotalLength = bytes;
    view->CompletedLength = bytes;
    view->AccountedLength = bytes;
    view->Drained = true;

    auto op = std::make_unique<IoOperation>();
    op->Event = TqWindowsIocpOperationType::TcpSend;
    op->Relay = relay;
    op->ReceiveView = view;
    op->PostedLength = postedLength;
    relay->InFlightTcpSends.fetch_add(1);
    HandleTcpSend(std::move(op), bytes);
    return true;
}

bool TqWindowsRelayWorker::TestBufferedTcpSendZeroCompletion(uint64_t relayId) {
    std::shared_ptr<RelayContext> relay;
    {
        std::lock_guard<std::mutex> guard(Lock_);
        const auto it = Relays_.find(relayId);
        if (it != Relays_.end()) {
            relay = it->second;
        }
    }
    if (!relay) {
        return false;
    }
    auto op = std::make_unique<IoOperation>();
    op->Event = TqWindowsIocpOperationType::TcpSend;
    op->Relay = relay;
    op->Buffer.resize(8);
    relay->InFlightTcpSends.fetch_add(1);
    HandleTcpSend(std::move(op), 0);
    return true;
}

bool TqWindowsRelayWorker::TestRecordIocpCompletionErrorForTest(
    uint64_t relayId,
    bool tcpSend,
    DWORD error) {
    auto relay = FindRelayById(relayId);
    if (!relay || error == ERROR_SUCCESS) {
        return false;
    }
    const auto operation = tcpSend
        ? TqWindowsIocpOperationType::TcpSend
        : TqWindowsIocpOperationType::TcpRecv;
    StoreIocpCompletionErrno(relay, static_cast<uint32_t>(operation), error);
    if (relay->LastIocpCompletionErrno.load(std::memory_order_relaxed) != error ||
        relay->LastIocpOperation.load(std::memory_order_relaxed) !=
            static_cast<uint32_t>(operation)) {
        return false;
    }
    if (tcpSend) {
        if (relay->LastTcpSendErrno.load(std::memory_order_relaxed) != error ||
            relay->LastTcpWriteErrno.load(std::memory_order_relaxed) != error ||
            relay->LastTcpRecvErrno.load(std::memory_order_relaxed) != 0) {
            return false;
        }
    } else if (relay->LastTcpRecvErrno.load(std::memory_order_relaxed) != error ||
               relay->LastTcpSendErrno.load(std::memory_order_relaxed) != 0 ||
               relay->LastTcpWriteErrno.load(std::memory_order_relaxed) != 0) {
        return false;
    }
    RecordTcpHardErrorAndFail(
        relay,
        tcpSend ? "iocp_tcp_send_completion_error" : "iocp_tcp_recv_completion_error",
        error);
    return true;
}

bool TqWindowsRelayWorker::TestCloseRelayTcpSocketForPostRecvFailure(uint64_t relayId) {
    std::shared_ptr<RelayContext> relay;
    {
        std::lock_guard<std::mutex> guard(Lock_);
        const auto it = Relays_.find(relayId);
        if (it != Relays_.end()) {
            relay = it->second;
        }
    }
    if (!relay || !TqSocketValid(relay->TcpFd)) {
        return false;
    }
    (void)::shutdown(relay->TcpFd, SD_BOTH);
    TqCloseSocket(relay->TcpFd);
    relay->TcpFd = TqInvalidSocket;
    relay->TcpRecvClosed.store(true, std::memory_order_release);
    return true;
}

bool TqWindowsRelayWorker::TestMarkTcpRecvInFlightForRetirement(uint64_t relayId) {
    std::shared_ptr<RelayContext> relay;
    {
        std::lock_guard<std::mutex> guard(Lock_);
        const auto it = Relays_.find(relayId);
        if (it != Relays_.end()) {
            relay = it->second;
        }
    }
    if (!relay) {
        return false;
    }
    relay->InFlightTcpRecvs.fetch_add(1, std::memory_order_acq_rel);
    return true;
}

bool TqWindowsRelayWorker::TestCompleteTcpRecvInFlightForRetirement(uint64_t relayId) {
    std::shared_ptr<RelayContext> relay;
    {
        std::lock_guard<std::mutex> guard(Lock_);
        const auto it = Relays_.find(relayId);
        if (it != Relays_.end()) {
            relay = it->second;
        }
    }
    if (!relay) {
        return false;
    }
    relay->InFlightTcpRecvs.fetch_sub(1, std::memory_order_acq_rel);
    TryRetireRelay(relay);
    return true;
}

bool TqWindowsRelayWorker::TestMarkQuicSendInFlightForRetirement(uint64_t relayId) {
    std::shared_ptr<RelayContext> relay;
    {
        std::lock_guard<std::mutex> guard(Lock_);
        const auto it = Relays_.find(relayId);
        if (it != Relays_.end()) {
            relay = it->second;
        }
    }
    if (!relay) {
        return false;
    }
    relay->InFlightQuicSends.fetch_add(1, std::memory_order_acq_rel);
    return true;
}

bool TqWindowsRelayWorker::TestMarkTcpSendInFlightForTest(uint64_t relayId) {
    std::shared_ptr<RelayContext> relay;
    {
        std::lock_guard<std::mutex> guard(Lock_);
        const auto it = Relays_.find(relayId);
        if (it != Relays_.end()) {
            relay = it->second;
        }
    }
    if (!relay) {
        return false;
    }
    relay->InFlightTcpSends.fetch_add(1, std::memory_order_acq_rel);
    return true;
}

bool TqWindowsRelayWorker::TestHandleTcpPostFailureForTest(uint64_t relayId, int error) {
    return HandleTcpPostFailure(FindRelayById(relayId), "wsa_recv_post_failed", error);
}

bool TqWindowsRelayWorker::TestGetRelayDrainFlagsForTest(
    uint64_t relayId,
    bool* closeAfterDrained,
    bool* tcpRecvClosed) const {
    std::lock_guard<std::mutex> guard(Lock_);
    const auto it = Relays_.find(relayId);
    if (it == Relays_.end() || !it->second) {
        return false;
    }
    const auto& relay = it->second;
    if (closeAfterDrained != nullptr) {
        *closeAfterDrained = relay->CloseAfterDrained.load(std::memory_order_acquire);
    }
    if (tcpRecvClosed != nullptr) {
        *tcpRecvClosed = relay->TcpRecvClosed.load(std::memory_order_acquire);
    }
    return true;
}

bool TqWindowsRelayWorker::TestArmRelayClosingForLateDiscard(uint64_t relayId) {
    std::shared_ptr<RelayContext> relay;
    {
        std::lock_guard<std::mutex> guard(Lock_);
        const auto it = Relays_.find(relayId);
        if (it != Relays_.end()) {
            relay = it->second;
        }
    }
    if (!relay || !relay->Callback) {
        return false;
    }
    relay->Closing.store(true, std::memory_order_release);
    relay->Callback->Closing.store(true, std::memory_order_release);
    return true;
}

bool TqWindowsRelayWorker::TestCloseRelayAfterTcpHalfCloseDrain(uint64_t relayId) {
    std::shared_ptr<RelayContext> relay;
    {
        std::lock_guard<std::mutex> guard(Lock_);
        const auto it = Relays_.find(relayId);
        if (it != Relays_.end()) {
            relay = it->second;
        }
    }
    if (!relay) {
        return false;
    }
    relay->TcpRecvClosed.store(true, std::memory_order_release);
    relay->QuicSendFinSubmitted.store(true, std::memory_order_release);
    relay->QuicSendFinCompleted.store(true, std::memory_order_release);
    relay->TcpWriteBytes.store(1, std::memory_order_release);
    relay->CloseAfterDrained.store(true, std::memory_order_release);
    return CloseRelayIfDrained(relay);
}

bool TqWindowsRelayWorker::MaybePostTcpRecvForTest(uint64_t relayId) {
    return MaybePostTcpRecv(FindRelayById(relayId));
}

bool TqWindowsRelayWorker::TestGetTcpReadPausedByQuicBacklog(uint64_t relayId) const {
    std::lock_guard<std::mutex> guard(Lock_);
    const auto it = Relays_.find(relayId);
    if (it == Relays_.end() || !it->second) {
        return false;
    }
    return it->second->TcpReadPausedByQuicBacklog.load(std::memory_order_acquire);
}

void TqWindowsRelayWorker::TestConfigureQuicSendBacklog(
    uint64_t relayId,
    uint64_t maxBufferedBytes,
    uint64_t outstandingBytes) {
    auto relay = FindRelayById(relayId);
    if (!relay) {
        return;
    }
    relay->Tuning.WindowsRelayMaxBufferedQuicSendBytes = maxBufferedBytes;
    relay->IdealSendBufferBytes.store(0, std::memory_order_release);
    relay->OutstandingQuicSendBytes.store(outstandingBytes, std::memory_order_release);
}

void TqWindowsRelayWorker::TestProcessQuicSendCompleteForTest(
    uint64_t relayId,
    uint64_t completedBytes) {
    auto relay = FindRelayById(relayId);
    if (!relay) {
        return;
    }
    auto* operation = new TqWindowsQuicSendOperation();
    operation->RelayId = relayId;
    operation->Generation = relay->Generation;
    operation->TotalBytes = completedBytes;
    relay->InFlightQuicSends.fetch_add(1, std::memory_order_acq_rel);
    ProcessQuicSendCompleteOperation(
        relayId,
        operation->Generation,
        reinterpret_cast<uintptr_t>(operation));
}
#endif

bool TqWindowsRelayWorker::PostTcpRecv(const std::shared_ptr<RelayContext>& relay) {
#if defined(TQ_UNIT_TESTING)
    if (g_InSendCompleteCallback) {
        PostTcpRecvFromSendCompleteCallbackCount_.fetch_add(1, std::memory_order_relaxed);
    }
#endif
    if (!relay || relay->Closing.load() || relay->TcpRecvClosed.load()) {
        return false;
    }
    std::unique_ptr<IoOperation> op;
    if (!relay->TcpRecvOpsFree.empty()) {
        op = std::move(relay->TcpRecvOpsFree.back());
        relay->TcpRecvOpsFree.pop_back();
        TcpRecvOperationsReused_.fetch_add(1, std::memory_order_relaxed);
    }
    if (!op) {
        op = std::make_unique<IoOperation>();
        TcpRecvOperationsCreated_.fetch_add(1, std::memory_order_relaxed);
    }
    op->Overlapped = OVERLAPPED{};
    op->Event = TqWindowsIocpOperationType::TcpRecv;
    op->Relay = relay;
    op->BufferOwner.reset();
    op->Buffer.clear();
    op->QuicBuffer = QUIC_BUFFER{};
    op->ReceiveView.reset();
    op->Offset = 0;
    op->PostedLength = 0;

    TqBufferAcquireFailure failure = TqBufferAcquireFailure::None;
    op->BufferOwner = TqAllocateRelayBuffer(relay.get(), relay->Tuning.RelayIoSize, &failure);
    if (!op->BufferOwner) {
        Errors_.fetch_add(1, std::memory_order_relaxed);
        return false;
    }
    op->WsaBuffer.buf = reinterpret_cast<char*>(op->BufferOwner->Data());
    op->WsaBuffer.len = static_cast<ULONG>(op->BufferOwner->Capacity());

    DWORD flags = 0;
    DWORD received = 0;
    relay->InFlightTcpRecvs.fetch_add(1, std::memory_order_acq_rel);
    IoOperation* raw = op.release();
    const int rc = ::WSARecv(relay->TcpFd, &raw->WsaBuffer, 1, &received, &flags, &raw->Overlapped, nullptr);
    const int error = rc == 0 ? 0 : ::WSAGetLastError();
    if (rc == 0 || error == WSA_IO_PENDING) {
        return true;
    }
    relay->InFlightTcpRecvs.fetch_sub(1, std::memory_order_acq_rel);
    delete raw;
    HandleTcpPostFailure(relay, "wsa_recv_post_failed", error);
    return false;
}

void TqWindowsRelayWorker::CloseRelay(
    const std::shared_ptr<RelayContext>& relay,
    TqRelayCloseMode mode,
    const char* reason) {
    MarkRelayCloseReason(relay, reason);
    if (!relay || relay->Closing.exchange(true)) {
        return;
    }
    const char* closeReason = relay->CloseReason.load(std::memory_order_acquire);
    if (closeReason == nullptr || closeReason == kWindowsRelayCloseReasonUnknown) {
        closeReason = kWindowsRelayCloseReasonDefault;
    }
    TqTraceRelayStopCondition("windows", WorkerIndex_, closeReason, BuildRelayTraceState(relay));
    if (mode == TqRelayCloseMode::AbortReset) {
        FatalRelayResets_.fetch_add(1, std::memory_order_relaxed);
        TqResetSocket(relay->TcpFd);
    } else {
        TqCloseSocket(relay->TcpFd);
    }
    relay->TcpFd = TqInvalidSocket;
    if (relay->Callback) {
        relay->Callback->Closing.store(true, std::memory_order_release);
        relay->Callback->RelayHint.store(nullptr, std::memory_order_release);
        if (relay->Stream != nullptr && relay->Stream->Context == relay->Callback.get()) {
            relay->Stream->Callback = MsQuicStream::NoOpCallback;
            relay->Stream->Context = nullptr;
        }
    }
    relay->Stream = nullptr;
    CompleteAllPendingQuicReceives(relay);
    FinalizeQuicSendAccountingOnClose(relay);
    TryRetireRelay(relay);
}

void TqWindowsRelayWorker::MarkRelayCloseReason(
    const std::shared_ptr<RelayContext>& relay,
    const char* reason) {
    if (!relay || reason == nullptr || reason[0] == '\0') {
        return;
    }
    const char* expected = kWindowsRelayCloseReasonUnknown;
    (void)relay->CloseReason.compare_exchange_strong(
        expected,
        reason,
        std::memory_order_acq_rel,
        std::memory_order_acquire);
}

TqTraceLinuxRelayStreamState TqWindowsRelayWorker::BuildRelayTraceState(
    const std::shared_ptr<RelayContext>& relay) const {
    TqTraceLinuxRelayStreamState state{};
    if (!relay) {
        return state;
    }
    const size_t pendingQuicSends = relay->PendingQuicSendRetries.size();
    state.WorkerIndex = WorkerIndex_;
    state.RelayId = relay->Id;
    state.OutstandingQuicSends =
        pendingQuicSends + relay->InFlightQuicSends.load(std::memory_order_relaxed);
    state.OutstandingQuicSendBytes =
        relay->OutstandingQuicSendBytes.load(std::memory_order_relaxed);
    state.PendingQuicReceiveBytes =
        relay->PendingQuicReceiveBytes.load(std::memory_order_relaxed);
    state.PendingTcpWriteQueue =
        relay->PendingQuicReceiveQueueDepth.load(std::memory_order_relaxed);
    state.TcpReadBytes = relay->TcpReadBytes.load(std::memory_order_relaxed);
    state.TcpWriteBytes = relay->TcpWriteBytes.load(std::memory_order_relaxed);
    state.TcpWriteErrno = relay->LastTcpWriteErrno.load(std::memory_order_relaxed);
    state.TcpRecvErrno = relay->LastTcpRecvErrno.load(std::memory_order_relaxed);
    state.TcpSendErrno = relay->LastTcpSendErrno.load(std::memory_order_relaxed);
    state.IocpCompletionErrno =
        relay->LastIocpCompletionErrno.load(std::memory_order_relaxed);
    state.IocpOperation = relay->LastIocpOperation.load(std::memory_order_relaxed);
    state.TcpReadClosed = relay->TcpRecvClosed.load(std::memory_order_relaxed);
    state.TcpWriteClosed = relay->TcpWriteClosed.load(std::memory_order_relaxed);
    state.QuicSendFinSubmitted = relay->QuicSendFinSubmitted.load(std::memory_order_relaxed);
    state.QuicSendFinCompleted = relay->QuicSendFinCompleted.load(std::memory_order_relaxed);
    state.StreamDetached = relay->Stream == nullptr;
    state.TunnelId = relay->TraceTunnelId;
    state.Target = relay->TraceTarget.empty() ? nullptr : relay->TraceTarget.c_str();
    return state;
}

void TqWindowsRelayWorker::TraceReceiveViewEvent(
    const std::shared_ptr<RelayContext>& relay,
    const std::shared_ptr<TqWindowsPendingQuicReceive>& view,
    const char* stage,
    uint64_t value) const {
    if (!relay || !view || !TqTraceEnabled()) {
        return;
    }
    TqTraceRelayReceiveViewEvent(
        "windows",
        WorkerIndex_,
        stage,
        reinterpret_cast<uintptr_t>(view.get()),
        value,
        view->TotalLength,
        view->CompletedLength,
        view->AccountedLength,
        view->PendingCompleteBytes,
        view->SliceIndex,
        view->Slices.size(),
        view->SliceOffset,
        view->Fin,
        view->Drained,
        BuildRelayTraceState(relay));
}

void TqWindowsRelayWorker::TraceRelayBackpressure(
    const std::shared_ptr<RelayContext>& relay,
    const char* action,
    const char* reason) const {
    if (!relay || !TqTraceEnabled()) {
        return;
    }
    const uint64_t threshold = CurrentRelayIdealSendBytes(relay);
    TqTraceRelayBackpressureEvent(
        "windows",
        WorkerIndex_,
        relay->Id,
        action,
        reason,
        relay->OutstandingQuicSendBytes.load(std::memory_order_relaxed),
        threshold,
        threshold == 0 ? 0 : threshold / 2,
        0);
}

void TqWindowsRelayWorker::FailRelayFatal(const std::shared_ptr<RelayContext>& relay, const char* reason) {
    if (relay) {
        const size_t pendingQuicSends = relay->PendingQuicSendRetries.size();
        TqTraceRelayStopCondition("windows", WorkerIndex_, reason, BuildRelayTraceState(relay));
        TqTraceRelayFatalError(
            "windows",
            WorkerIndex_,
            reason,
            relay->Id,
            static_cast<uint64_t>(relay->TcpFd),
            relay->PendingQuicReceiveBytes.load(std::memory_order_relaxed),
            relay->PendingQuicReceiveQueueDepth.load(std::memory_order_relaxed),
            static_cast<uint64_t>(pendingQuicSends),
            relay->InFlightQuicSends.load(std::memory_order_relaxed),
            relay->InFlightTcpSends.load(std::memory_order_relaxed));
    }
    CloseRelay(relay, TqRelayCloseMode::AbortReset, reason);
}

void TqWindowsRelayWorker::RecordTcpHardErrorAndFail(
    const std::shared_ptr<RelayContext>& relay,
    const char* reason,
    uint64_t tcpError) {
    (void)tcpError;
    Errors_.fetch_add(1, std::memory_order_relaxed);
    TcpHardErrors_.fetch_add(1, std::memory_order_relaxed);
    FailRelayFatal(relay, reason);
}

void TqWindowsRelayWorker::StoreTcpRecvErrno(
    const std::shared_ptr<RelayContext>& relay,
    uint64_t error) {
    if (relay == nullptr || error == 0) {
        return;
    }
    relay->LastTcpRecvErrno.store(error, std::memory_order_relaxed);
}

void TqWindowsRelayWorker::StoreTcpSendErrno(
    const std::shared_ptr<RelayContext>& relay,
    uint64_t error) {
    if (relay == nullptr || error == 0) {
        return;
    }
    relay->LastTcpSendErrno.store(error, std::memory_order_relaxed);
    relay->LastTcpWriteErrno.store(error, std::memory_order_relaxed);
}

void TqWindowsRelayWorker::StoreIocpCompletionErrno(
    const std::shared_ptr<RelayContext>& relay,
    uint32_t operation,
    uint64_t error) {
    if (relay == nullptr || error == 0) {
        return;
    }
    relay->LastIocpCompletionErrno.store(error, std::memory_order_relaxed);
    relay->LastIocpOperation.store(operation, std::memory_order_relaxed);
    if (operation == static_cast<uint32_t>(TqWindowsIocpOperationType::TcpRecv)) {
        StoreTcpRecvErrno(relay, error);
    } else if (operation == static_cast<uint32_t>(TqWindowsIocpOperationType::TcpSend)) {
        StoreTcpSendErrno(relay, error);
    }
}

bool TqWindowsRelayWorker::HasPendingRelayDrainWork(
    const std::shared_ptr<RelayContext>& relay) const {
    if (!relay) {
        return false;
    }
    bool hasCallbackPending = false;
    bool hasPendingReceives = false;
    size_t pendingQuicSendRetries = 0;
    {
        std::lock_guard<std::mutex> guard(relay->CallbackPendingQuicReceiveLock);
        hasCallbackPending = !relay->CallbackPendingQuicReceives.empty();
    }
    hasPendingReceives = !relay->PendingReceives.empty();
    pendingQuicSendRetries = relay->PendingQuicSendRetries.size();
    return relay->InFlightTcpSends.load(std::memory_order_acquire) != 0 ||
           relay->PendingQuicReceiveQueueDepth.load(std::memory_order_acquire) != 0 ||
           relay->PendingQuicReceiveBytes.load(std::memory_order_acquire) != 0 ||
           hasCallbackPending ||
           hasPendingReceives ||
           relay->InFlightQuicSends.load(std::memory_order_acquire) != 0 ||
           pendingQuicSendRetries != 0 ||
           relay->OutstandingQuicSendBytes.load(std::memory_order_acquire) != 0 ||
           (relay->QuicSendFinSubmitted.load(std::memory_order_acquire) &&
            !relay->QuicSendFinCompleted.load(std::memory_order_acquire));
}

bool TqWindowsRelayWorker::HandleTcpPostFailure(
    const std::shared_ptr<RelayContext>& relay,
    const char* reason,
    int error) {
    if (error != 0) {
        if (reason != nullptr && std::strncmp(reason, "wsa_recv", 8) == 0) {
            StoreTcpRecvErrno(relay, static_cast<uint64_t>(error));
        } else {
            StoreTcpSendErrno(relay, static_cast<uint64_t>(error));
        }
    }
    if (IsTcpTeardownError(error)) {
        TqTraceRelayStopCondition("windows", WorkerIndex_, reason, BuildRelayTraceState(relay));
        if (relay != nullptr && HasPendingRelayDrainWork(relay)) {
            relay->TcpRecvClosed.store(true, std::memory_order_release);
            relay->CloseAfterDrained.store(true, std::memory_order_release);
            GracefulRelayDrains_.fetch_add(1, std::memory_order_relaxed);
            (void)CloseRelayIfDrained(relay);
            return true;
        }
        GracefulRelayDrains_.fetch_add(1, std::memory_order_relaxed);
        CloseRelay(relay, TqRelayCloseMode::GracefulDrain, reason);
        return true;
    }
    RecordTcpHardErrorAndFail(relay, reason, static_cast<uint64_t>(error));
    return false;
}

bool TqWindowsRelayWorker::HasPendingAfterStreamShutdown(
    const std::shared_ptr<RelayContext>& relay) const {
    if (!relay) {
        return false;
    }
    bool hasCallbackPending = false;
    bool hasPendingReceives = false;
    size_t pendingQuicSendRetries = 0;
    {
        std::lock_guard<std::mutex> guard(relay->CallbackPendingQuicReceiveLock);
        hasCallbackPending = !relay->CallbackPendingQuicReceives.empty();
    }
    hasPendingReceives = !relay->PendingReceives.empty();
    pendingQuicSendRetries = relay->PendingQuicSendRetries.size();
    return relay->InFlightQuicSends.load(std::memory_order_acquire) != 0 ||
           relay->OutstandingQuicSendBytes.load(std::memory_order_acquire) != 0 ||
           pendingQuicSendRetries != 0 ||
           relay->InFlightTcpSends.load(std::memory_order_acquire) != 0 ||
           relay->InFlightTcpRecvs.load(std::memory_order_acquire) != 0 ||
           relay->PendingQuicReceiveQueueDepth.load(std::memory_order_acquire) != 0 ||
           hasCallbackPending ||
           hasPendingReceives ||
           relay->PendingQuicReceiveBytes.load(std::memory_order_acquire) != 0 ||
           (relay->QuicSendFinSubmitted.load(std::memory_order_acquire) &&
            !relay->QuicSendFinCompleted.load(std::memory_order_acquire));
}

void TqWindowsRelayWorker::ProcessQuicPeerAborted(
    uint64_t relayId,
    const char* reason,
    uint64_t errorCode) {
    auto relay = FindRelayById(relayId);
    if (!relay || relay->Closing.load(std::memory_order_acquire)) {
        PostedCallbackStaleDropCount_.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    ProcessQuicPeerAborted(relay, reason, errorCode);
}

void TqWindowsRelayWorker::ProcessQuicPeerAborted(
    const std::shared_ptr<RelayContext>& relay,
    const char* reason,
    uint64_t errorCode) {
    if (!relay) {
        return;
    }
    (void)errorCode;
    if (!HasPendingAfterStreamShutdown(relay)) {
        LateTeardownDowngradedCount_.fetch_add(1, std::memory_order_relaxed);
        GracefulRelayDrains_.fetch_add(1, std::memory_order_relaxed);
        CloseRelay(relay, TqRelayCloseMode::GracefulDrain, reason);
        return;
    }
    relay->CloseAfterDrained.store(true, std::memory_order_release);
    (void)CloseRelayIfDrained(relay);
}

void TqWindowsRelayWorker::ProcessQuicShutdownComplete(
    uint64_t relayId,
    uint64_t errorCode,
    uint32_t status) {
    auto relay = FindRelayById(relayId);
    if (!relay || relay->Closing.load(std::memory_order_acquire)) {
        PostedCallbackStaleDropCount_.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    ProcessQuicShutdownComplete(relay, errorCode, status);
}

void TqWindowsRelayWorker::ProcessQuicShutdownComplete(
    const std::shared_ptr<RelayContext>& relay,
    uint64_t errorCode,
    uint32_t status) {
    if (!relay) {
        return;
    }
    (void)errorCode;
    (void)status;
    TqTraceRelayStreamShutdown("windows", BuildRelayTraceState(relay));
    if (!HasPendingAfterStreamShutdown(relay)) {
        LateTeardownDowngradedCount_.fetch_add(1, std::memory_order_relaxed);
        GracefulRelayDrains_.fetch_add(1, std::memory_order_relaxed);
        CloseRelay(relay, TqRelayCloseMode::GracefulDrain, "stream_shutdown_complete");
        return;
    }
    relay->CloseAfterDrained.store(true, std::memory_order_release);
    (void)CloseRelayIfDrained(relay);
}

void TqWindowsRelayWorker::TryRetireRelay(const std::shared_ptr<RelayContext>& relay) {
    if (!relay || !relay->Closing.load(std::memory_order_acquire)) {
        return;
    }
    if (relay->ActiveHandlers.load(std::memory_order_acquire) != 0 ||
        relay->QueuedWorkerOps.load(std::memory_order_acquire) != 0 ||
        relay->InFlightTcpRecvs.load(std::memory_order_acquire) != 0 ||
        relay->InFlightTcpSends.load(std::memory_order_acquire) != 0 ||
        relay->InFlightQuicSends.load(std::memory_order_acquire) != 0) {
        return;
    }
    if (relay->StopPublished.exchange(true, std::memory_order_acq_rel)) {
        return;
    }
    std::shared_ptr<CallbackBinding> bindingKeepAlive;
    if (relay->Callback) {
        bindingKeepAlive = relay->Callback;
        if (relay->Stream != nullptr && relay->Stream->Context == relay->Callback.get()) {
            relay->Stream->Callback = MsQuicStream::NoOpCallback;
            relay->Stream->Context = nullptr;
        }
    }
    {
        std::lock_guard<std::mutex> guard(Lock_);
        Relays_.erase(relay->Id);
        if (bindingKeepAlive) {
            RetiredCallbacks_.push_back(std::move(bindingKeepAlive));
        }
    }
    if (relay->PublicHandle != nullptr) {
        relay->PublicHandle->Stop.store(true, std::memory_order_release);
    }
    if (TqTraceEnabled()) {
        TqTraceRelayUnregister("windows", BuildRelayTraceState(relay));
    }
    PruneRetiredCallbacks(true);
}

bool TqWindowsRelayWorker::IsQuicSendBackpressureStatus(QUIC_STATUS status) const {
    return status == QUIC_STATUS_OUT_OF_MEMORY || status == QUIC_STATUS_BUFFER_TOO_SMALL;
}

bool TqWindowsRelayWorker::IsTcpTeardownError(int error) const {
    return error == WSAECONNRESET || error == WSAECONNABORTED ||
        error == WSAENOTCONN || error == WSAESHUTDOWN ||
        error == WSAENOTSOCK || error == WSA_OPERATION_ABORTED ||
        error == WSAECANCELLED;
}

bool TqWindowsRelayWorker::IsIocpTeardownError(DWORD error) const {
    return error == ERROR_OPERATION_ABORTED || error == ERROR_NETNAME_DELETED ||
        error == ERROR_CONNECTION_ABORTED || error == ERROR_GRACEFUL_DISCONNECT ||
        error == WSAECONNRESET || error == WSAECONNABORTED ||
        error == WSAENOTCONN || error == WSAESHUTDOWN ||
        error == WSAENOTSOCK || error == WSA_OPERATION_ABORTED ||
        error == WSAECANCELLED;
}

bool TqWindowsRelayWorker::ShouldDowngradeTcpRecvCompletion(
    const RelayContext* relay,
    DWORD error) const {
    if (relay == nullptr) {
        return false;
    }
    if (relay->Closing.load(std::memory_order_acquire) ||
        relay->TcpRecvClosed.load(std::memory_order_acquire)) {
        return true;
    }
    if (!TqSocketValid(relay->TcpFd)) {
        return true;
    }
    return IsIocpTeardownError(error);
}

bool TqWindowsRelayWorker::ShouldDowngradeTcpSendCompletion(
    const RelayContext* relay,
    DWORD error) const {
    if (relay == nullptr) {
        return false;
    }
    if (relay->Closing.load(std::memory_order_acquire) ||
        relay->TcpWriteClosed.load(std::memory_order_acquire)) {
        return true;
    }
    if (!TqSocketValid(relay->TcpFd)) {
        return true;
    }
    return IsIocpTeardownError(error);
}

bool TqWindowsRelayWorker::ShouldDowngradeTcpSendZeroBytes(
    const RelayContext& relay) const {
    if (relay.Closing.load(std::memory_order_acquire) ||
        relay.TcpWriteClosed.load(std::memory_order_acquire)) {
        return true;
    }
    if (relay.CloseAfterDrained.load(std::memory_order_acquire) &&
        relay.InFlightTcpSends.load(std::memory_order_acquire) == 0 &&
        relay.PendingQuicReceiveQueueDepth.load(std::memory_order_acquire) == 0) {
        return true;
    }
    return false;
}

bool TqWindowsRelayWorker::ShouldDowngradePostedWorkerCompletion(
    const RelayContext* relay,
    DWORD error) const {
    if (relay == nullptr) {
        return false;
    }
    if (relay->Closing.load(std::memory_order_acquire)) {
        return true;
    }
    return IsIocpTeardownError(error);
}

void TqWindowsRelayWorker::DowngradeIocpCompletion(
    const std::shared_ptr<RelayContext>& relay,
    const char* reason,
    DWORD completionError) {
    if (!relay) {
        return;
    }
    IocpCompletionDowngraded_.fetch_add(1, std::memory_order_relaxed);
    if (completionError != 0) {
        relay->LastIocpCompletionErrno.store(completionError, std::memory_order_relaxed);
    }
    TqTraceRelayStopCondition(
        "windows", WorkerIndex_, reason, BuildRelayTraceState(relay));
    GracefulRelayDrains_.fetch_add(1, std::memory_order_relaxed);
    if (!relay->Closing.load(std::memory_order_acquire)) {
        CloseRelay(relay, TqRelayCloseMode::GracefulDrain, reason);
    } else {
        TryRetireRelay(relay);
    }
}

void TqWindowsRelayWorker::DropStaleCompletionWithoutRelay(DWORD completionError) {
    (void)completionError;
    Errors_.fetch_add(1, std::memory_order_relaxed);
    IocpStaleCompletionDropped_.fetch_add(1, std::memory_order_relaxed);
}

bool TqWindowsRelayWorker::IsQuicSendTeardownStatus(QUIC_STATUS status) const {
    return status == QUIC_STATUS_ABORTED || status == QUIC_STATUS_INVALID_STATE ||
        status == QUIC_STATUS_CONNECTION_IDLE || status == QUIC_STATUS_CONNECTION_TIMEOUT ||
        status == QUIC_STATUS_USER_CANCELED;
}

bool TqWindowsRelayWorker::TrySubmitQuicSendOperation(
    const std::shared_ptr<RelayContext>& relay,
    TqWindowsQuicSendOperation* operation) {
    if (!relay || !operation) {
        delete operation;
        return false;
    }
    if (relay->Closing.load(std::memory_order_acquire) || relay->Stream == nullptr ||
        relay->Stream->Handle == nullptr) {
        delete operation;
        return false;
    }
    const QUIC_SEND_FLAGS flags = operation->Fin ? QUIC_SEND_FLAG_FIN : QUIC_SEND_FLAG_NONE;
    relay->InFlightQuicSends.fetch_add(1, std::memory_order_acq_rel);
    const QUIC_STATUS status = relay->Stream->Send(
        operation->QuicBuffers.empty() ? nullptr : operation->QuicBuffers.data(),
        static_cast<uint32_t>(operation->QuicBuffers.size()),
        flags,
        operation);
    if (!QUIC_FAILED(status)) {
        relay->OutstandingQuicSendBytes.fetch_add(operation->TotalBytes, std::memory_order_acq_rel);
        uint64_t outstanding =
            relay->OutstandingQuicSendBytes.load(std::memory_order_acquire);
        uint64_t maxObserved =
            relay->MaxOutstandingQuicSendBytes.load(std::memory_order_relaxed);
        while (outstanding > maxObserved &&
            !relay->MaxOutstandingQuicSendBytes.compare_exchange_weak(
                maxObserved, outstanding, std::memory_order_acq_rel, std::memory_order_relaxed)) {
        }
        if ((flags & QUIC_SEND_FLAG_FIN) != 0) {
            relay->QuicSendFinSubmitted.store(true, std::memory_order_release);
        }
        return true;
    }
    relay->InFlightQuicSends.fetch_sub(1, std::memory_order_acq_rel);
    if (IsQuicSendBackpressureStatus(status)) {
        QuicSendBackpressureEvents_.fetch_add(1, std::memory_order_relaxed);
        relay->PendingQuicSendRetries.emplace_back(operation);
        SetTcpReadBackpressure(relay, true, "quic_send_resource");
        return true;
    }
    delete operation;
    if (IsQuicSendTeardownStatus(status)) {
        relay->CloseAfterDrained.store(true, std::memory_order_release);
        (void)CloseRelayIfDrained(relay);
        return false;
    }
    QuicSendFatalErrors_.fetch_add(1, std::memory_order_relaxed);
    FailRelayFatal(relay, "quic_send_failed");
    return false;
}

void TqWindowsRelayWorker::RetryPendingQuicSends(const std::shared_ptr<RelayContext>& relay) {
    if (!relay || relay->Closing.load()) {
        return;
    }
    for (;;) {
        std::unique_ptr<TqWindowsQuicSendOperation> operation;
        if (relay->PendingQuicSendRetries.empty()) {
            return;
        }
        operation = std::move(relay->PendingQuicSendRetries.front());
        relay->PendingQuicSendRetries.pop_front();
        if (!operation) {
            continue;
        }
        if (!TrySubmitQuicSendOperation(relay, operation.release())) {
            return;
        }
        if (!relay->PendingQuicSendRetries.empty()) {
            return;
        }
        MaybePostTcpRecv(relay);
    }
}

bool TqWindowsRelayWorker::CloseRelayIfDrained(const std::shared_ptr<RelayContext>& relay) {
    if (!relay || !relay->CloseAfterDrained.load()) {
        return false;
    }
    if (HasPendingRelayDrainWork(relay)) {
        return false;
    }
    if (!relay->TcpWriteClosed.exchange(true, std::memory_order_acq_rel) &&
        TqSocketValid(relay->TcpFd)) {
        (void)TqShutdownSend(relay->TcpFd);
        GracefulRelayDrains_.fetch_add(1, std::memory_order_relaxed);
    }
    if (TqSocketValid(relay->TcpFd) &&
        !relay->TcpRecvClosed.load(std::memory_order_acquire)) {
        return true;
    }
    relay->CloseAfterDrained.store(false);
    CloseRelay(relay, TqRelayCloseMode::GracefulDrain, "close_after_drained");
    return true;
}

bool TqWindowsRelayWorker::HandleTcpReadClosed(std::unique_ptr<IoOperation> op) {
    auto relay = op ? op->Relay : nullptr;
    if (!relay || relay->Closing.load()) {
        if (relay) {
            TryRetireRelay(relay);
        }
        return false;
    }
    op->BufferOwner.reset();
    if (relay->TcpRecvClosed.exchange(true)) {
        return true;
    }
    TqTraceRelayStopCondition(
        "windows",
        WorkerIndex_,
        "tcp_read_closed",
        BuildRelayTraceState(relay));

    std::vector<uint8_t> finalOutput;
    if (relay->Compressor != nullptr) {
        if (!relay->Compressor->Compress(nullptr, 0, finalOutput, true)) {
            CloseRelay(relay, TqRelayCloseMode::GracefulDrain, "compress_final_failed");
            return false;
        }
    }
    if (!finalOutput.empty()) {
        auto sendOp = std::make_unique<TqWindowsQuicSendOperation>();
        sendOp->RelayId = relay->Id;
        sendOp->Generation = relay->Generation;
        sendOp->Fin = true;
        sendOp->Flags = QUIC_SEND_FLAG_FIN;
        sendOp->OwnedBytes = std::move(finalOutput);
        sendOp->TotalBytes = sendOp->OwnedBytes.size();
        QUIC_BUFFER buffer{};
        buffer.Buffer = sendOp->OwnedBytes.data();
        buffer.Length = static_cast<uint32_t>(sendOp->OwnedBytes.size());
        sendOp->QuicBuffers.push_back(buffer);
        op->BufferOwner.reset();
        op->Buffer.clear();
        op->Relay.reset();
        relay->TcpRecvOpsFree.push_back(std::move(op));
        if (!TrySubmitQuicSendOperation(relay, sendOp.release())) {
            CloseRelay(relay, TqRelayCloseMode::GracefulDrain, "post_quic_send_fin_failed");
            return false;
        }
        return true;
    }
    op->BufferOwner.reset();
    op->Buffer.clear();
    op->Relay.reset();
    relay->TcpRecvOpsFree.push_back(std::move(op));
    auto sendOp = std::make_unique<TqWindowsQuicSendOperation>();
    sendOp->RelayId = relay->Id;
    sendOp->Generation = relay->Generation;
    sendOp->Fin = true;
    sendOp->Flags = QUIC_SEND_FLAG_FIN;
    sendOp->TotalBytes = 0;
    return TrySubmitQuicSendOperation(relay, sendOp.release());
}

void TqWindowsRelayWorker::HandleTcpRecv(std::unique_ptr<IoOperation> op, DWORD bytes) {
    auto relay = op->Relay;
    if (!relay || relay->Closing.load()) {
        if (relay) {
            relay->InFlightTcpRecvs.fetch_sub(1, std::memory_order_acq_rel);
            TryRetireRelay(relay);
        }
        return;
    }
    relay->InFlightTcpRecvs.fetch_sub(1, std::memory_order_acq_rel);
    relay->TcpRecvPosted.store(false, std::memory_order_release);
    if (bytes == 0) {
        (void)HandleTcpReadClosed(std::move(op));
        return;
    }
    if (!op->BufferOwner) {
        Errors_.fetch_add(1, std::memory_order_relaxed);
        CloseRelay(relay, TqRelayCloseMode::GracefulDrain, "tcp_recv_buffer_missing");
        return;
    }
    relay->TcpReadBytes.fetch_add(bytes, std::memory_order_relaxed);
    op->BufferOwner->SetLength(bytes);

    auto sendOp = std::make_unique<TqWindowsQuicSendOperation>();
    sendOp->RelayId = relay->Id;
    sendOp->Generation = relay->Generation;
    sendOp->Flags = QUIC_SEND_FLAG_NONE;
    if (relay->Compressor != nullptr) {
        std::vector<uint8_t> compressed;
        if (!relay->Compressor->Compress(
                op->BufferOwner->Data(),
                static_cast<uint32_t>(op->BufferOwner->Length()),
                compressed,
                false)) {
            CloseRelay(relay, TqRelayCloseMode::GracefulDrain, "compress_tcp_recv_failed");
            return;
        }
        if (compressed.empty() && !relay->Compressor->Flush(compressed)) {
            CloseRelay(relay, TqRelayCloseMode::GracefulDrain, "compress_tcp_recv_flush_failed");
            return;
        }
        op->BufferOwner.reset();
        op->Buffer.clear();
        op->Relay.reset();
        relay->TcpRecvOpsFree.push_back(std::move(op));
        if (compressed.empty()) {
            if (!MaybePostTcpRecv(relay) && !relay->Closing.load(std::memory_order_acquire)) {
                if (relay->TcpRecvClosed.load(std::memory_order_acquire) ||
                    !TqSocketValid(relay->TcpFd)) {
                    GracefulRelayDrains_.fetch_add(1, std::memory_order_relaxed);
                    CloseRelay(relay, TqRelayCloseMode::GracefulDrain, "tcp_recv_empty_compress_closed");
                } else {
                    FailRelayFatal(relay, "post_tcp_recv_after_empty_send_failed");
                }
            }
            return;
        }
        sendOp->OwnedBytes = std::move(compressed);
        sendOp->TotalBytes = sendOp->OwnedBytes.size();
        QUIC_BUFFER buffer{};
        buffer.Buffer = sendOp->OwnedBytes.data();
        buffer.Length = static_cast<uint32_t>(sendOp->OwnedBytes.size());
        sendOp->QuicBuffers.push_back(buffer);
    } else {
        sendOp->TotalBytes = op->BufferOwner->Length();
        sendOp->Buffers.push_back(std::move(op->BufferOwner));
        QUIC_BUFFER buffer{};
        buffer.Buffer = sendOp->Buffers.back()->Data();
        buffer.Length = static_cast<uint32_t>(sendOp->Buffers.back()->Length());
        sendOp->QuicBuffers.push_back(buffer);
        op->Buffer.clear();
        op->Relay.reset();
        relay->TcpRecvOpsFree.push_back(std::move(op));
    }

    if (!TrySubmitQuicSendOperation(relay, sendOp.release())) {
        CloseRelay(relay, TqRelayCloseMode::GracefulDrain, "post_quic_send_failed");
        return;
    }
    MaybePostTcpRecv(relay);
}

bool TqWindowsRelayWorker::QueueDeferredQuicReceive(
    const std::shared_ptr<RelayContext>& relay,
    MsQuicStream* stream,
    const QUIC_BUFFER* buffers,
    uint32_t bufferCount,
    bool fin) {
    if (!relay || !relay->Callback) {
        Errors_.fetch_add(1, std::memory_order_relaxed);
        return false;
    }
    auto view = BuildDeferredQuicReceiveView(
        *relay->Callback,
        stream,
        buffers,
        bufferCount,
        fin,
        relay->Tuning.WindowsRelayQuicReceiveCompleteBatchBytes);
    if (!view) {
        if (stream == nullptr || (bufferCount != 0 && buffers == nullptr) || fin) {
            return false;
        }
        for (uint32_t i = 0; i < bufferCount; ++i) {
            if (buffers[i].Length != 0) {
                return false;
            }
        }
        return true;
    }
    return EnqueueDeferredQuicReceiveView(relay, view);
}

std::shared_ptr<TqWindowsPendingQuicReceive> TqWindowsRelayWorker::BuildDeferredQuicReceiveView(
    const CallbackBinding& binding,
    MsQuicStream* stream,
    const QUIC_BUFFER* buffers,
    uint32_t bufferCount,
    bool fin,
    uint64_t completeBatchBytes) {
    if (stream == nullptr || (bufferCount != 0 && buffers == nullptr)) {
        Errors_.fetch_add(1, std::memory_order_relaxed);
        return nullptr;
    }

    struct OffsetSlice {
        size_t Offset{0};
        uint32_t Length{0};
    };
    std::vector<OffsetSlice> offsetSlices;
    offsetSlices.reserve(bufferCount);

    uint64_t totalLength = 0;
    for (uint32_t i = 0; i < bufferCount; ++i) {
        const QUIC_BUFFER& buffer = buffers[i];
        if (buffer.Length != 0 && buffer.Buffer == nullptr) {
            Errors_.fetch_add(1, std::memory_order_relaxed);
            return nullptr;
        }
        totalLength += buffer.Length;
        if (totalLength > static_cast<uint64_t>(std::numeric_limits<size_t>::max())) {
            Errors_.fetch_add(1, std::memory_order_relaxed);
            return nullptr;
        }
        if (buffer.Length != 0) {
            offsetSlices.push_back(OffsetSlice{0, buffer.Length});
        }
    }
    if (totalLength == 0 && !fin) {
        return nullptr;
    }

    auto view = std::make_shared<TqWindowsPendingQuicReceive>();
    view->Stream = stream;
    view->RelayId = binding.RelayId;
    view->Generation = binding.Generation;
    view->Fin = fin;
    view->CompleteBatchBytes = completeBatchBytes;
    view->OwnedBuffer.reserve(static_cast<size_t>(totalLength));
    view->Slices.reserve(offsetSlices.size());

    size_t nonEmptyIndex = 0;
    for (uint32_t i = 0; i < bufferCount; ++i) {
        const QUIC_BUFFER& buffer = buffers[i];
        if (buffer.Length == 0) {
            continue;
        }
        const size_t offset = view->OwnedBuffer.size();
        view->OwnedBuffer.insert(
            view->OwnedBuffer.end(),
            buffer.Buffer,
            buffer.Buffer + buffer.Length);
        view->TotalLength += buffer.Length;
        offsetSlices[nonEmptyIndex++].Offset = offset;
    }

    for (const auto& slice : offsetSlices) {
        view->Slices.push_back(TqWindowsQuicReceiveSlice{
            view->OwnedBuffer.data() + slice.Offset,
            slice.Length});
    }
    return view;
}

bool TqWindowsRelayWorker::EnqueueDeferredQuicReceiveView(
    const std::shared_ptr<RelayContext>& relay,
    const std::shared_ptr<TqWindowsPendingQuicReceive>& view) {
    if (!relay || !view || view->RelayId != relay->Id || view->Generation != relay->Generation) {
        Errors_.fetch_add(1, std::memory_order_relaxed);
        return false;
    }
    if (relay->TcpRecvClosed.load(std::memory_order_acquire)) {
        TqTraceRelayStopCondition(
            "windows",
            WorkerIndex_,
            view->Fin ? "quic_receive_after_tcp_read_closed_fin" : "quic_receive_after_tcp_read_closed",
            BuildRelayTraceState(relay));
    }

    view->CompleteBatchBytes = relay->Tuning.WindowsRelayQuicReceiveCompleteBatchBytes;
    relay->PendingReceives.push_back(view);

    const uint64_t maxPendingBytes = relay->Tuning.WindowsRelayMaxPendingQuicReceiveBytesPerRelay;
    const uint64_t pendingBytes = relay->PendingQuicReceiveBytes.fetch_add(
        view->TotalLength, std::memory_order_relaxed) + view->TotalLength;
    const uint64_t pendingDepth = relay->PendingQuicReceiveQueueDepth.fetch_add(
        1, std::memory_order_release) + 1;
    if (maxPendingBytes != 0 && pendingBytes >= maxPendingBytes &&
        !relay->QuicReceivePaused.load(std::memory_order_acquire)) {
        SetQuicReceiveEnabled(relay, false);
    }
    DeferredReceiveQueued_.fetch_add(1, std::memory_order_relaxed);
    DeferredReceiveBytesQueued_.fetch_add(view->TotalLength, std::memory_order_relaxed);
    UpdateAtomicMax(MaxPendingQuicReceiveBytesObserved_, pendingBytes);
    UpdateAtomicMax(MaxPendingQuicReceiveQueueObserved_, pendingDepth);
    TraceReceiveViewEvent(relay, view, "queue_receive", pendingDepth);

    return true;
}

bool TqWindowsRelayWorker::PostTcpSendFromReceiveView(
    const std::shared_ptr<RelayContext>& relay,
    const std::shared_ptr<TqWindowsPendingQuicReceive>& view) {
    if (!relay || !view || relay->Closing.load()) {
        return false;
    }
    if (relay->TcpWriteClosed.load(std::memory_order_acquire)) {
        TqTraceRelayStopCondition(
            "windows",
            WorkerIndex_,
            "quic_receive_after_tcp_write_closed",
            BuildRelayTraceState(relay));
        (void)CompletePendingQuicReceive(relay, view);
        (void)CloseRelayIfDrained(relay);
        return true;
    }
    while (view->SliceIndex < view->Slices.size() &&
           view->SliceOffset >= view->Slices[view->SliceIndex].Length) {
        view->SliceOffset = 0;
        ++view->SliceIndex;
    }
    if (view->SliceIndex >= view->Slices.size()) {
        TraceReceiveViewEvent(relay, view, "post_tcp_send_no_slices");
        TraceReceiveViewEvent(relay, view, "flush_receive_complete", view->PendingCompleteBytes);
        FlushDeferredReceiveCompletion(*view, true);
        return FinishReceiveView(relay, view);
    }

    const auto& slice = view->Slices[view->SliceIndex];
    auto op = std::make_unique<IoOperation>();
    op->Event = TqWindowsIocpOperationType::TcpSend;
    op->Relay = relay;
    op->ReceiveView = view;
    op->Offset = view->SliceOffset;
    op->PostedLength = static_cast<uint64_t>(slice.Length - view->SliceOffset);

    op->WsaBuffer.buf = reinterpret_cast<char*>(const_cast<uint8_t*>(slice.Data + view->SliceOffset));
    op->WsaBuffer.len = static_cast<ULONG>(op->PostedLength);
    DWORD sent = 0;
    relay->InFlightTcpSends.fetch_add(1);
    view->TcpSendPending = true;
    TraceReceiveViewEvent(relay, view, "post_tcp_send_receive_view", op->PostedLength);
    IoOperation* raw = op.release();
    const int rc = ::WSASend(relay->TcpFd, &raw->WsaBuffer, 1, &sent, 0, &raw->Overlapped, nullptr);
    const int error = rc == 0 ? 0 : ::WSAGetLastError();
    if (rc != 0 && error != WSA_IO_PENDING) {
        relay->InFlightTcpSends.fetch_sub(1);
        view->TcpSendPending = false;
        delete raw;
        HandleTcpPostFailure(relay, "wsa_send_receive_view_failed", error);
        return false;
    }
    TcpSendOperationsPosted_.fetch_add(1, std::memory_order_relaxed);
    if (error == WSA_IO_PENDING) {
        TcpSendWouldBlockOrPendingCount_.fetch_add(1, std::memory_order_relaxed);
    }
    return true;
}

bool TqWindowsRelayWorker::PostTcpSendFromCompressedReceiveView(
    const std::shared_ptr<RelayContext>& relay,
    const std::shared_ptr<TqWindowsPendingQuicReceive>& view) {
    if (!relay || !view || relay->Closing.load() || relay->Decompressor == nullptr) {
        return false;
    }
    if (relay->TcpWriteClosed.load(std::memory_order_acquire)) {
        TqTraceRelayStopCondition(
            "windows",
            WorkerIndex_,
            "quic_receive_after_tcp_write_closed",
            BuildRelayTraceState(relay));
        (void)CompletePendingQuicReceive(relay, view);
        (void)CloseRelayIfDrained(relay);
        return true;
    }

    for (;;) {
        while (view->SliceIndex < view->Slices.size() &&
               view->SliceOffset >= view->Slices[view->SliceIndex].Length) {
            view->SliceOffset = 0;
            ++view->SliceIndex;
        }
        const bool hasInput = view->SliceIndex < view->Slices.size();
        const uint8_t* input = nullptr;
        size_t inputLength = 0;
        if (hasInput) {
            const auto& slice = view->Slices[view->SliceIndex];
            input = slice.Data + view->SliceOffset;
            inputLength = slice.Length - view->SliceOffset;
        }

        TqBufferAcquireFailure failure = TqBufferAcquireFailure::None;
        auto output = TqAllocateRelayBuffer(relay.get(), relay->Tuning.RelayIoSize, &failure);
        if (!output) {
            return true;
        }

        TqDecompressResult result{};
        ZstdDecompressCalls_.fetch_add(1, std::memory_order_relaxed);
        if (!relay->Decompressor->DecompressInto(
                input,
                inputLength,
                output->Data(),
                output->Capacity(),
                &result)) {
            ZstdDecompressFailures_.fetch_add(1, std::memory_order_relaxed);
            Errors_.fetch_add(1, std::memory_order_relaxed);
            return false;
        }
        if (result.InputConsumed > inputLength || result.OutputProduced > output->Capacity()) {
            ZstdDecompressFailures_.fetch_add(1, std::memory_order_relaxed);
            Errors_.fetch_add(1, std::memory_order_relaxed);
            return false;
        }
        if (result.NeedsMoreInput) {
            ZstdDecompressNeedInput_.fetch_add(1, std::memory_order_relaxed);
        }
        if (result.NeedsMoreOutput) {
            ZstdDecompressNeedOutput_.fetch_add(1, std::memory_order_relaxed);
        }
        if (result.InputConsumed > 0) {
            ZstdDecompressInputBytes_.fetch_add(result.InputConsumed, std::memory_order_relaxed);
            AdvanceReceiveView(relay, *view, result.InputConsumed);
            TraceReceiveViewEvent(relay, view, "flush_receive_complete", view->PendingCompleteBytes);
            FlushDeferredReceiveCompletion(*view, false);
            MaybeResumeQuicReceive(relay);
        }
        if (result.OutputProduced > 0) {
            output->SetLength(result.OutputProduced);
            ZstdDecompressOutputBytes_.fetch_add(result.OutputProduced, std::memory_order_relaxed);

            auto op = std::make_unique<IoOperation>();
            op->Event = TqWindowsIocpOperationType::TcpSend;
            op->Relay = relay;
            op->ReceiveView = view;
            op->BufferOwner = std::move(output);
            op->Offset = 0;
            op->PostedLength = result.OutputProduced;
            op->WsaBuffer.buf = reinterpret_cast<char*>(op->BufferOwner->Data());
            op->WsaBuffer.len = static_cast<ULONG>(result.OutputProduced);

            DWORD sent = 0;
            relay->InFlightTcpSends.fetch_add(1);
            view->TcpSendPending = true;
            TraceReceiveViewEvent(relay, view, "post_tcp_send_compressed_receive_view", op->PostedLength);
            IoOperation* raw = op.release();
            const int rc = ::WSASend(relay->TcpFd, &raw->WsaBuffer, 1, &sent, 0, &raw->Overlapped, nullptr);
            const int error = rc == 0 ? 0 : ::WSAGetLastError();
            if (rc != 0 && error != WSA_IO_PENDING) {
                relay->InFlightTcpSends.fetch_sub(1);
                view->TcpSendPending = false;
                delete raw;
                HandleTcpPostFailure(relay, "wsa_send_compressed_receive_view_failed", error);
                return false;
            }
            TcpSendOperationsPosted_.fetch_add(1, std::memory_order_relaxed);
            if (error == WSA_IO_PENDING) {
                TcpSendWouldBlockOrPendingCount_.fetch_add(1, std::memory_order_relaxed);
            }
            return true;
        }

        if (view->SliceIndex >= view->Slices.size() && result.NeedsMoreInput) {
            TraceReceiveViewEvent(relay, view, "post_tcp_send_compressed_needs_more_input_finish");
            TraceReceiveViewEvent(relay, view, "flush_receive_complete", view->PendingCompleteBytes);
            FlushDeferredReceiveCompletion(*view, true);
            return FinishReceiveView(relay, view);
        }
        if (result.InputConsumed == 0) {
            if (!hasInput && result.NeedsMoreInput) {
                TraceReceiveViewEvent(relay, view, "post_tcp_send_compressed_empty_finish");
                TraceReceiveViewEvent(relay, view, "flush_receive_complete", view->PendingCompleteBytes);
                FlushDeferredReceiveCompletion(*view, true);
                return FinishReceiveView(relay, view);
            }
            ZstdDecompressFailures_.fetch_add(1, std::memory_order_relaxed);
            Errors_.fetch_add(1, std::memory_order_relaxed);
            return false;
        }
    }
}

void TqWindowsRelayWorker::AdvanceReceiveView(
    const std::shared_ptr<RelayContext>& relay,
    TqWindowsPendingQuicReceive& view,
    uint64_t bytes) {
    if (bytes == 0) {
        return;
    }
    view.PendingCompleteBytes += bytes;
    view.CompletedLength += bytes;
    view.AccountedLength += bytes;
    if (relay != nullptr) {
        SaturatingFetchSub(relay->PendingQuicReceiveBytes, bytes);
    }

    uint64_t remaining = bytes;
    while (remaining > 0 && view.SliceIndex < view.Slices.size()) {
        const auto& slice = view.Slices[view.SliceIndex];
        const uint64_t available = static_cast<uint64_t>(slice.Length - view.SliceOffset);
        if (remaining >= available) {
            remaining -= available;
            ++view.SliceIndex;
            view.SliceOffset = 0;
        } else {
            view.SliceOffset += static_cast<size_t>(remaining);
            remaining = 0;
        }
    }
}

void TqWindowsRelayWorker::FlushDeferredReceiveCompletion(
    TqWindowsPendingQuicReceive& view,
    bool force) {
    if (view.PendingCompleteBytes == 0) {
        return;
    }
    if (!force && view.CompleteBatchBytes != 0 && view.PendingCompleteBytes < view.CompleteBatchBytes) {
        return;
    }
    const uint64_t completeBytes = view.PendingCompleteBytes;
    DeferredReceiveCompleteBytes_.fetch_add(completeBytes, std::memory_order_relaxed);
    DeferredReceiveCompletes_.fetch_add(1, std::memory_order_relaxed);
    DeferredReceiveCompletionFlushes_.fetch_add(1, std::memory_order_relaxed);
    if (view.Stream != nullptr && view.Stream->Handle != nullptr) {
        view.Stream->ReceiveComplete(completeBytes);
    }
    view.PendingCompleteBytes = 0;
}

void TqWindowsRelayWorker::FlushBatchedDeferredReceiveCompletion(
    const std::shared_ptr<RelayContext>& relay,
    MsQuicStream* stream) {
    if (!relay) {
        return;
    }
    const uint64_t completeBytes =
        relay->DeferredReceiveCompleteBatchPending.exchange(0, std::memory_order_acq_rel);
    if (completeBytes == 0) {
        return;
    }
    DeferredReceiveCompleteBytes_.fetch_add(completeBytes, std::memory_order_relaxed);
    DeferredReceiveCompletes_.fetch_add(1, std::memory_order_relaxed);
    DeferredReceiveCompletionFlushes_.fetch_add(1, std::memory_order_relaxed);
    if (stream != nullptr && stream->Handle != nullptr) {
        stream->ReceiveComplete(completeBytes);
    }
}

void TqWindowsRelayWorker::CompleteRemainingReceiveOwnership(TqWindowsPendingQuicReceive& view) {
    if (view.CompletedLength >= view.TotalLength) {
        view.Drained = true;
        view.CompletedLength = view.TotalLength;
        view.PendingCompleteBytes = 0;
        return;
    }
    const uint64_t remaining = view.TotalLength - view.CompletedLength;
    DeferredReceiveCompleteBytes_.fetch_add(remaining, std::memory_order_relaxed);
    DeferredReceiveCompletes_.fetch_add(1, std::memory_order_relaxed);
    DeferredReceiveCompletionFlushes_.fetch_add(1, std::memory_order_relaxed);
    if (view.Stream != nullptr && view.Stream->Handle != nullptr) {
        view.Stream->ReceiveComplete(remaining);
    }
    view.Drained = true;
    view.CompletedLength = view.TotalLength;
    view.PendingCompleteBytes = 0;
}

bool TqWindowsRelayWorker::FinishReceiveView(
    const std::shared_ptr<RelayContext>& relay,
    const std::shared_ptr<TqWindowsPendingQuicReceive>& view) {
    if (!relay || !view) {
        return false;
    }
    const auto it = std::find(relay->PendingReceives.begin(), relay->PendingReceives.end(), view);
    if (it == relay->PendingReceives.end()) {
        if (view->Drained || view->CompletedLength >= view->TotalLength) {
            TraceReceiveViewEvent(relay, view, "finish_already_drained");
            TqTraceRelayStopCondition(
                "windows",
                WorkerIndex_,
                "finish_receive_view_already_drained",
                BuildRelayTraceState(relay));
            return true;
        }
        TraceReceiveViewEvent(relay, view, "finish_missing");
        TqTraceRelayStopCondition(
            "windows",
            WorkerIndex_,
            "finish_receive_view_missing",
            BuildRelayTraceState(relay));
        return false;
    }
    if (view->PendingCompleteBytes > 0) {
        relay->DeferredReceiveCompleteBatchPending.fetch_add(
            view->PendingCompleteBytes, std::memory_order_relaxed);
        view->PendingCompleteBytes = 0;
    }
    relay->PendingReceives.erase(it);
    TraceReceiveViewEvent(relay, view, "finish_remove");
    if (view->AccountedLength < view->TotalLength) {
        const uint64_t remaining = view->TotalLength - view->AccountedLength;
        SaturatingFetchSub(relay->PendingQuicReceiveBytes, remaining);
        view->AccountedLength = view->TotalLength;
    }
    SaturatingFetchSub(relay->PendingQuicReceiveQueueDepth, 1);
    if (view->Fin) {
        TraceReceiveViewEvent(relay, view, "finish_fin");
        relay->CloseAfterDrained.store(true, std::memory_order_release);
        if (!relay->TcpWriteClosed.exchange(true, std::memory_order_acq_rel) &&
            TqSocketValid(relay->TcpFd)) {
            (void)TqShutdownSend(relay->TcpFd);
            GracefulRelayDrains_.fetch_add(1, std::memory_order_relaxed);
        }
    }
    MaybeResumeQuicReceive(relay);
    const bool hasPendingReceives = !relay->PendingReceives.empty();
    if (hasPendingReceives && !relay->Closing.load(std::memory_order_acquire)) {
        ScheduleRelayReceiveDrainOrFail(relay, "schedule_receive_drain_after_finish_failed");
    } else if (!hasPendingReceives) {
        FlushBatchedDeferredReceiveCompletion(relay, view->Stream);
    }
    (void)CloseRelayIfDrained(relay);
    return true;
}

bool TqWindowsRelayWorker::CompletePendingQuicReceive(
    const std::shared_ptr<RelayContext>& relay,
    const std::shared_ptr<TqWindowsPendingQuicReceive>& view) {
    if (!relay || !view) {
        return false;
    }
    TraceReceiveViewEvent(relay, view, "complete_pending_before");
    TraceReceiveViewEvent(relay, view, "flush_receive_complete", view->PendingCompleteBytes);
    FlushDeferredReceiveCompletion(*view, true);
    CompleteRemainingReceiveOwnership(*view);
    TraceReceiveViewEvent(relay, view, "complete_pending_after");
    return FinishReceiveView(relay, view);
}

void TqWindowsRelayWorker::SetQuicReceiveEnabled(const std::shared_ptr<RelayContext>& relay, bool enabled) {
    if (!relay) {
        return;
    }
    const bool wasPaused = relay->QuicReceivePaused.exchange(!enabled, std::memory_order_acq_rel);
    if (enabled) {
        if (!wasPaused) {
            return;
        }
        QuicReceiveResumedCount_.fetch_add(1, std::memory_order_relaxed);
        TraceRelayBackpressure(relay, "resume", "pending_quic_receive_drain");
    } else {
        if (wasPaused) {
            return;
        }
        QuicReceivePausedCount_.fetch_add(1, std::memory_order_relaxed);
        TraceRelayBackpressure(relay, "pause", "pending_quic_receive_cap");
    }
    if (relay->Stream != nullptr && relay->Stream->Handle != nullptr && MsQuic != nullptr &&
        MsQuic->StreamReceiveSetEnabled != nullptr) {
        (void)MsQuic->StreamReceiveSetEnabled(relay->Stream->Handle, enabled ? TRUE : FALSE);
    }
}

void TqWindowsRelayWorker::MaybeResumeQuicReceive(const std::shared_ptr<RelayContext>& relay) {
    if (!relay || Stopping_.load(std::memory_order_acquire) ||
        relay->Closing.load(std::memory_order_acquire) ||
        !relay->QuicReceivePaused.load(std::memory_order_acquire)) {
        return;
    }
    const uint64_t maxPendingBytes = relay->Tuning.WindowsRelayMaxPendingQuicReceiveBytesPerRelay;
    if (maxPendingBytes == 0) {
        SetQuicReceiveEnabled(relay, true);
        return;
    }
    if (relay->PendingQuicReceiveBytes.load(std::memory_order_relaxed) < maxPendingBytes / 2) {
        SetQuicReceiveEnabled(relay, true);
    }
}

void TqWindowsRelayWorker::CompleteAllPendingQuicReceives(const std::shared_ptr<RelayContext>& relay) {
    if (!relay) {
        return;
    }
    std::list<std::shared_ptr<TqWindowsPendingQuicReceive>> pending;
    pending.swap(relay->PendingReceives);
    MsQuicStream* streamForFlush = nullptr;
    for (const auto& view : pending) {
        if (view && view->Stream != nullptr) {
            streamForFlush = view->Stream;
        }
        if (view) {
            FlushDeferredReceiveCompletion(*view, true);
            CompleteRemainingReceiveOwnership(*view);
        }
        const uint64_t remaining =
            view && view->AccountedLength < view->TotalLength
                ? view->TotalLength - view->AccountedLength
                : 0;
        SaturatingFetchSub(relay->PendingQuicReceiveBytes, remaining);
        SaturatingFetchSub(relay->PendingQuicReceiveQueueDepth, 1);
    }
    relay->PendingQuicSendRetries.clear();
    FlushBatchedDeferredReceiveCompletion(relay, streamForFlush);
}

void TqWindowsRelayWorker::FinalizeQuicSendAccountingOnClose(
    const std::shared_ptr<RelayContext>& relay) {
    if (!relay) {
        return;
    }
    relay->PendingQuicSendRetries.clear();
    relay->InFlightQuicSends.store(0, std::memory_order_release);
    relay->OutstandingQuicSendBytes.store(0, std::memory_order_release);
    relay->QuicSendFinSubmitted.store(false, std::memory_order_release);
    relay->QuicSendFinCompleted.store(false, std::memory_order_release);
}

void TqWindowsRelayWorker::PruneRetiredCallbacks(bool keepNewest) {
    std::lock_guard<std::mutex> guard(Lock_);
    const CallbackBinding* newest =
        keepNewest && !RetiredCallbacks_.empty() ? RetiredCallbacks_.back().get() : nullptr;
    RetiredCallbacks_.erase(
        std::remove_if(
            RetiredCallbacks_.begin(),
            RetiredCallbacks_.end(),
            [newest](const std::shared_ptr<CallbackBinding>& binding) {
                if (!binding) {
                    return true;
                }
                if (binding.get() == newest) {
                    return false;
                }
                return binding->Closing.load(std::memory_order_acquire) &&
                    binding->CallbackRefs.load(std::memory_order_acquire) == 0;
            }),
        RetiredCallbacks_.end());
}

bool TqWindowsRelayWorker::PostTcpSend(std::unique_ptr<IoOperation> op) {
    auto relay = op->Relay;
    if (!relay || relay->Closing.load() || op->Offset >= op->Buffer.size()) {
        return false;
    }
    op->WsaBuffer.buf = reinterpret_cast<char*>(op->Buffer.data() + op->Offset);
    op->WsaBuffer.len = static_cast<ULONG>(op->Buffer.size() - op->Offset);
    DWORD sent = 0;
    relay->InFlightTcpSends.fetch_add(1);
    IoOperation* raw = op.release();
    const int rc = ::WSASend(relay->TcpFd, &raw->WsaBuffer, 1, &sent, 0, &raw->Overlapped, nullptr);
    const int error = rc == 0 ? 0 : ::WSAGetLastError();
    if (rc != 0 && error != WSA_IO_PENDING) {
        relay->InFlightTcpSends.fetch_sub(1);
        delete raw;
        HandleTcpPostFailure(relay, "wsa_send_buffer_failed", error);
        return false;
    }
    TcpSendOperationsPosted_.fetch_add(1, std::memory_order_relaxed);
    if (error == WSA_IO_PENDING) {
        TcpSendWouldBlockOrPendingCount_.fetch_add(1, std::memory_order_relaxed);
    }
    return true;
}

void TqWindowsRelayWorker::HandleTcpSend(std::unique_ptr<IoOperation> op, DWORD bytes) {
    auto relay = op->Relay;
    if (!relay) {
        return;
    }
    relay->InFlightTcpSends.fetch_sub(1);
    if (op->ReceiveView) {
        auto view = op->ReceiveView;
        view->TcpSendPending = false;
        TraceReceiveViewEvent(relay, view, "tcp_send_complete_receive_view", bytes);
        if (view->Drained) {
            return;
        }
        if (bytes == 0 || static_cast<uint64_t>(bytes) > op->PostedLength) {
            (void)CompletePendingQuicReceive(relay, view);
            if (ShouldDowngradeTcpSendZeroBytes(*relay) ||
                ShouldDowngradeTcpSendCompletion(relay.get(), ERROR_SUCCESS)) {
                TcpSendZeroBytesGraceful_.fetch_add(1, std::memory_order_relaxed);
                DowngradeIocpCompletion(relay, "tcp_send_receive_view_zero_teardown");
            } else {
                RecordTcpHardErrorAndFail(relay, "tcp_send_receive_view_completion_error");
            }
            return;
        }
        TcpSendBytes_.fetch_add(bytes, std::memory_order_relaxed);
        relay->TcpWriteBytes.fetch_add(bytes, std::memory_order_relaxed);
        if (static_cast<uint64_t>(bytes) < op->PostedLength) {
            TcpSendPartialCompletions_.fetch_add(1, std::memory_order_relaxed);
            if (!(relay->Decompressor != nullptr && relay->CompressAlgo == TqCompressAlgo::Zstd)) {
                AdvanceReceiveView(relay, *view, bytes);
                TraceReceiveViewEvent(relay, view, "flush_receive_complete", view->PendingCompleteBytes);
                FlushDeferredReceiveCompletion(*view, false);
                if (!PostTcpSendFromReceiveView(relay, view)) {
                    CloseRelay(relay, TqRelayCloseMode::GracefulDrain, "post_tcp_send_receive_view_failed");
                    return;
                }
                ScheduleRelayReceiveDrainOrFail(relay, "schedule_receive_drain_after_partial_send_failed");
                return;
            }
            op->Offset += bytes;
            op->PostedLength -= bytes;
            WSABUF buf{};
            buf.buf = op->WsaBuffer.buf + bytes;
            buf.len = static_cast<ULONG>(op->PostedLength);
            op->WsaBuffer = buf;
            DWORD sent = 0;
            relay->InFlightTcpSends.fetch_add(1);
            view->TcpSendPending = true;
            IoOperation* raw = op.release();
            const int rc = ::WSASend(relay->TcpFd, &raw->WsaBuffer, 1, &sent, 0, &raw->Overlapped, nullptr);
            const int error = rc == 0 ? 0 : ::WSAGetLastError();
            if (rc != 0 && error != WSA_IO_PENDING) {
                relay->InFlightTcpSends.fetch_sub(1);
                view->TcpSendPending = false;
                delete raw;
                (void)CompletePendingQuicReceive(relay, view);
                HandleTcpPostFailure(relay, "wsa_send_receive_view_retry_failed", error);
                return;
            } else if (error == WSA_IO_PENDING) {
                TcpSendWouldBlockOrPendingCount_.fetch_add(1, std::memory_order_relaxed);
            }
            ScheduleRelayReceiveDrainOrFail(relay, "schedule_receive_drain_after_retry_send_failed");
            return;
        }
        if (CloseRelayIfDrained(relay)) {
            return;
        }
        if (relay->Closing.load()) {
            return;
        }
        if (relay->Decompressor != nullptr && relay->CompressAlgo == TqCompressAlgo::Zstd) {
            if (!PostTcpSendFromCompressedReceiveView(relay, view)) {
                CloseRelay(relay, TqRelayCloseMode::GracefulDrain, "post_tcp_send_compressed_receive_view_failed");
                return;
            }
        } else {
            AdvanceReceiveView(relay, *view, bytes);
            TraceReceiveViewEvent(relay, view, "flush_receive_complete", view->PendingCompleteBytes);
            FlushDeferredReceiveCompletion(*view, false);
            if (!PostTcpSendFromReceiveView(relay, view)) {
                CloseRelay(relay, TqRelayCloseMode::GracefulDrain, "post_tcp_send_receive_view_failed");
                return;
            }
        }
        ScheduleRelayReceiveDrainOrFail(relay, "schedule_receive_drain_after_send_complete_failed");
        return;
    }
    TcpSendBytes_.fetch_add(bytes, std::memory_order_relaxed);
    relay->TcpWriteBytes.fetch_add(bytes, std::memory_order_relaxed);
    if (bytes == 0) {
        if (ShouldDowngradeTcpSendZeroBytes(*relay)) {
            TcpSendZeroBytesGraceful_.fetch_add(1, std::memory_order_relaxed);
            DowngradeIocpCompletion(relay, "tcp_send_completion_zero_teardown");
        } else {
            RecordTcpHardErrorAndFail(relay, "tcp_send_completion_zero");
        }
        return;
    }
    if (CloseRelayIfDrained(relay)) {
        return;
    }
    if (relay->Closing.load()) {
        return;
    }
    op->Offset += bytes;
    if (op->Offset < op->Buffer.size()) {
        TcpSendPartialCompletions_.fetch_add(1, std::memory_order_relaxed);
        if (!PostTcpSend(std::move(op))) {
            CloseRelay(relay, TqRelayCloseMode::GracefulDrain, "post_tcp_send_partial_failed");
        }
        return;
    }
}

void TqWindowsRelayWorker::SetRelayTraceContext(
    uint64_t relayId,
    uint64_t tunnelId,
    const char* target) {
    auto relay = FindRelayById(relayId);
    if (!relay || tunnelId == 0) {
        return;
    }
    relay->TraceTunnelId = tunnelId;
    if (target != nullptr) {
        relay->TraceTarget = target;
    } else {
        relay->TraceTarget.clear();
    }
}

void TqWindowsRelayWorker::StopRelay(uint64_t relayId) {
    if (relayId == 0 || Iocp_ == nullptr) {
        return;
    }
    auto op = std::make_unique<IoOperation>();
    op->Event = TqWindowsIocpOperationType::CloseRelay;
    op->RelayId = relayId;
    IoOperation* raw = op.release();
    if (!::PostQueuedCompletionStatus(static_cast<HANDLE>(Iocp_), 0, 0, &raw->Overlapped)) {
        delete raw;
        Errors_.fetch_add(1, std::memory_order_relaxed);
    }
}

QUIC_STATUS QUIC_API TqWindowsRelayWorker::StreamCallback(
    MsQuicStream* stream,
    void* context,
    QUIC_STREAM_EVENT* event) noexcept {
    auto* binding = static_cast<TqWindowsRelayWorker::CallbackBinding*>(context);
    (void)stream;
    if (binding == nullptr || binding->Worker == nullptr) {
        return QUIC_STATUS_SUCCESS;
    }
    auto* worker = binding->Worker;
    if (event == nullptr) {
        return QUIC_STATUS_SUCCESS;
    }

    binding->CallbackRefs.fetch_add(1, std::memory_order_acq_rel);
    struct CallbackRefsGuard {
        TqWindowsRelayWorker::CallbackBinding* Binding{nullptr};
        ~CallbackRefsGuard() {
            if (Binding != nullptr) {
                Binding->CallbackRefs.fetch_sub(1, std::memory_order_acq_rel);
            }
        }
    } refsGuard{binding};
    const uint64_t callbackStartNanos = NowSteadyNanos();
    struct CallbackDispatchGuard {
        std::atomic<uint64_t>* Target{nullptr};
        uint64_t StartNanos{0};
        ~CallbackDispatchGuard() {
            if (Target != nullptr) {
                Target->fetch_add(NowSteadyNanos() - StartNanos, std::memory_order_relaxed);
            }
        }
    } callbackDispatchGuard{&worker->CallbackDispatchNanos_, callbackStartNanos};

    if (event->Type == QUIC_STREAM_EVENT_SEND_COMPLETE) {
#if defined(TQ_UNIT_TESTING)
        g_InSendCompleteCallback = true;
        struct SendCompleteCallbackGuard {
            ~SendCompleteCallbackGuard() { g_InSendCompleteCallback = false; }
        } sendCompleteGuard;
#endif
        auto* sendOp = static_cast<TqWindowsQuicSendOperation*>(event->SEND_COMPLETE.ClientContext);
        worker->QueueQuicSendCompleteByIdFromCallback(*binding, sendOp);
        return QUIC_STATUS_SUCCESS;
    }

    if (event->Type == QUIC_STREAM_EVENT_RECEIVE) {
        const bool fin = (event->RECEIVE.Flags & QUIC_RECEIVE_FLAG_FIN) != 0;
        if (TqIsMsQuicFakeFinReceive(
                event->RECEIVE.AbsoluteOffset,
                event->RECEIVE.TotalBufferLength,
                event->RECEIVE.BufferCount,
                event->RECEIVE.Flags)) {
            auto relay = worker->FindRelayById(binding->RelayId);
            TqTraceRelayStreamEvent(
                "windows",
                worker->WorkerIndex_,
                binding->RelayId,
                "receive_fake_fin",
                0,
                0,
                event->RECEIVE.AbsoluteOffset,
                event->RECEIVE.TotalBufferLength,
                event->RECEIVE.BufferCount,
                static_cast<uint32_t>(event->RECEIVE.Flags),
                true,
                worker->BuildRelayTraceState(relay));
            assert(false && "MsQuic delivered FIN-only receive without known final size");
            std::abort();
        }
        if (binding->Closing.load(std::memory_order_acquire)) {
            return QUIC_STATUS_SUCCESS;
        }
        auto view = worker->BuildDeferredQuicReceiveView(
            *binding,
            stream,
            event->RECEIVE.Buffers,
            event->RECEIVE.BufferCount,
            fin,
            0);
        if (!view) {
            return QUIC_STATUS_SUCCESS;
        }
        if (worker->PostCallbackOperationById(
                TqWindowsIocpOperationType::RelayReceiveReady,
                *binding,
                0,
                0,
                view)) {
            return QUIC_STATUS_PENDING;
        }
        worker->CompleteRemainingReceiveOwnership(*view);
        return QUIC_STATUS_SUCCESS;
    }

    if (binding->Closing.load(std::memory_order_acquire)) {
        return QUIC_STATUS_SUCCESS;
    }

    if (event->Type == QUIC_STREAM_EVENT_PEER_SEND_SHUTDOWN) {
        return QUIC_STATUS_SUCCESS;
    }
    if (event->Type == QUIC_STREAM_EVENT_IDEAL_SEND_BUFFER_SIZE) {
        if (!worker->PostCallbackOperationById(
                TqWindowsIocpOperationType::QuicIdealSendBuffer,
                *binding,
                event->IDEAL_SEND_BUFFER_SIZE.ByteCount)) {
            worker->Errors_.fetch_add(1, std::memory_order_relaxed);
        }
        return QUIC_STATUS_SUCCESS;
    }
    if (event->Type == QUIC_STREAM_EVENT_PEER_SEND_ABORTED ||
        event->Type == QUIC_STREAM_EVENT_PEER_RECEIVE_ABORTED) {
        const bool peerSendAborted = event->Type == QUIC_STREAM_EVENT_PEER_SEND_ABORTED;
        if (!worker->PostCallbackOperationById(
                peerSendAborted
                    ? TqWindowsIocpOperationType::QuicPeerSendAborted
                    : TqWindowsIocpOperationType::QuicPeerReceiveAborted,
                *binding,
                peerSendAborted
                    ? event->PEER_SEND_ABORTED.ErrorCode
                    : event->PEER_RECEIVE_ABORTED.ErrorCode)) {
            worker->Errors_.fetch_add(1, std::memory_order_relaxed);
        }
        return QUIC_STATUS_SUCCESS;
    }
    if (event->Type == QUIC_STREAM_EVENT_SHUTDOWN_COMPLETE) {
        if (!worker->PostCallbackOperationById(
                TqWindowsIocpOperationType::QuicShutdownComplete,
                *binding,
                event->SHUTDOWN_COMPLETE.ConnectionErrorCode,
                static_cast<size_t>(event->SHUTDOWN_COMPLETE.ConnectionCloseStatus))) {
            worker->Errors_.fetch_add(1, std::memory_order_relaxed);
        }
        return QUIC_STATUS_SUCCESS;
    }
    return QUIC_STATUS_SUCCESS;
}

TqWindowsRelayRuntime& TqWindowsRelayRuntime::Instance() {
    static TqWindowsRelayRuntime runtime;
    return runtime;
}

bool TqWindowsRelayRuntime::Start(const TqTuningConfig& tuning) {
    std::lock_guard<std::mutex> guard(Lock_);
    if (!Workers_.empty()) {
        return true;
    }
    uint32_t workerCount = tuning.LinuxRelayWorkerCount;
    if (workerCount == 0) {
        workerCount = 1;
    }
    for (uint32_t i = 0; i < workerCount; ++i) {
        auto worker = std::make_unique<TqWindowsRelayWorker>(i);
        if (!worker->Start()) {
            Workers_.clear();
            return false;
        }
        Workers_.push_back(std::move(worker));
    }
    return true;
}

void TqWindowsRelayRuntime::Stop() {
    std::lock_guard<std::mutex> lifetimeGuard(RuntimeWorkerLifetimeLock());
    std::lock_guard<std::mutex> guard(Lock_);
    Workers_.clear();
}

TqWindowsRelayWorkerSnapshot TqWindowsRelayRuntime::Snapshot() const {
    TqWindowsRelayWorkerSnapshot total{};
    std::lock_guard<std::mutex> lifetimeGuard(RuntimeWorkerLifetimeLock());
    std::vector<TqWindowsRelayWorker*> workers;
    {
        std::lock_guard<std::mutex> guard(Lock_);
        workers.reserve(Workers_.size());
        for (const auto& worker : Workers_) {
            if (worker) {
                workers.push_back(worker.get());
            }
        }
    }
    for (const auto* worker : workers) {
        const auto snapshot = worker->Snapshot();
        total.DeferredReceiveQueued += snapshot.DeferredReceiveQueued;
        total.DeferredReceiveBytesQueued += snapshot.DeferredReceiveBytesQueued;
        total.DeferredReceiveCompleteBytes += snapshot.DeferredReceiveCompleteBytes;
        total.DeferredReceiveCompletes += snapshot.DeferredReceiveCompletes;
        total.DeferredReceiveCompletionFlushes += snapshot.DeferredReceiveCompletionFlushes;
        total.PendingQuicReceiveBytes += snapshot.PendingQuicReceiveBytes;
        total.MaxPendingQuicReceiveBytes = std::max(total.MaxPendingQuicReceiveBytes, snapshot.MaxPendingQuicReceiveBytes);
        total.PendingQuicReceiveQueueDepth += snapshot.PendingQuicReceiveQueueDepth;
        total.MaxPendingQuicReceiveQueueDepth = std::max(
            total.MaxPendingQuicReceiveQueueDepth,
            snapshot.MaxPendingQuicReceiveQueueDepth);
        total.QuicReceivePausedCount += snapshot.QuicReceivePausedCount;
        total.QuicReceiveResumedCount += snapshot.QuicReceiveResumedCount;
        total.TcpSendOperationsPosted += snapshot.TcpSendOperationsPosted;
        total.TcpSendBytes += snapshot.TcpSendBytes;
        total.TcpSendPartialCompletions += snapshot.TcpSendPartialCompletions;
        total.TcpSendWouldBlockOrPendingCount += snapshot.TcpSendWouldBlockOrPendingCount;
        total.TcpRecvOperationsCreated += snapshot.TcpRecvOperationsCreated;
        total.TcpRecvOperationsReused += snapshot.TcpRecvOperationsReused;
        total.RelayBufferBytesInUse += snapshot.RelayBufferBytesInUse;
        total.RelayBufferAllocateCount += snapshot.RelayBufferAllocateCount;
        total.ZstdDecompressInputBytes += snapshot.ZstdDecompressInputBytes;
        total.ZstdDecompressOutputBytes += snapshot.ZstdDecompressOutputBytes;
        total.ZstdDecompressCalls += snapshot.ZstdDecompressCalls;
        total.ZstdDecompressNeedInput += snapshot.ZstdDecompressNeedInput;
        total.ZstdDecompressNeedOutput += snapshot.ZstdDecompressNeedOutput;
        total.ZstdDecompressFailures += snapshot.ZstdDecompressFailures;
        total.FatalRelayResets += snapshot.FatalRelayResets;
        total.GracefulRelayDrains += snapshot.GracefulRelayDrains;
        total.TcpHardErrors += snapshot.TcpHardErrors;
        total.QuicSendBackpressureEvents += snapshot.QuicSendBackpressureEvents;
        total.QuicSendFatalErrors += snapshot.QuicSendFatalErrors;
        total.QuicSendCompleteEvents += snapshot.QuicSendCompleteEvents;
        total.WindowsCallbackIocpPostCount += snapshot.WindowsCallbackIocpPostCount;
        total.WindowsCallbackIocpPostFailedCount += snapshot.WindowsCallbackIocpPostFailedCount;
        total.WindowsReceiveReadyPostCount += snapshot.WindowsReceiveReadyPostCount;
        total.WindowsReceiveDrainScheduledCount += snapshot.WindowsReceiveDrainScheduledCount;
        total.WindowsReceiveDrainCoalescedCount += snapshot.WindowsReceiveDrainCoalescedCount;
        total.WindowsPostedCallbackStaleDropCount += snapshot.WindowsPostedCallbackStaleDropCount;
        total.WorkerLockAcquireCount += snapshot.WorkerLockAcquireCount;
        total.WorkerLockWaitNanos += snapshot.WorkerLockWaitNanos;
        total.FindRelayByIdCount += snapshot.FindRelayByIdCount;
        total.CallbackDispatchNanos += snapshot.CallbackDispatchNanos;
        total.SnapshotBuildNanos += snapshot.SnapshotBuildNanos;
        total.SnapshotActiveRelaysScanned += snapshot.SnapshotActiveRelaysScanned;
        total.EventsProcessed += snapshot.EventsProcessed;
        total.TcpReadResumeByBacklogEvents += snapshot.TcpReadResumeByBacklogEvents;
        total.LateTeardownDowngradedCount += snapshot.LateTeardownDowngradedCount;
        total.ActiveRelays += snapshot.ActiveRelays;
        total.Errors += snapshot.Errors;
        total.IocpCompletionDowngraded += snapshot.IocpCompletionDowngraded;
        total.IocpStaleCompletionDropped += snapshot.IocpStaleCompletionDropped;
        total.TcpSendZeroBytesGraceful += snapshot.TcpSendZeroBytesGraceful;
        total.ActiveRelayStates.insert(
            total.ActiveRelayStates.end(),
            snapshot.ActiveRelayStates.begin(),
            snapshot.ActiveRelayStates.end());
    }
    return total;
}

bool TqWindowsRelayRuntime::RegisterRelay(
    TqSocketHandle tcpFd,
    MsQuicStream* stream,
    ITqCompressor* compressor,
    ITqDecompressor* decompressor,
    TqRelayHandle* handle,
    const TqTuningConfig& tuning,
    TqCompressAlgo compressAlgo) {
    std::lock_guard<std::mutex> guard(Lock_);
    if (Workers_.empty()) {
        return false;
    }
    const uint64_t index = NextWorker_.fetch_add(1) % Workers_.size();
    return Workers_[static_cast<size_t>(index)]->RegisterRelay(
        tcpFd, stream, compressor, decompressor, handle, tuning, compressAlgo);
}

void TqWindowsRelayRuntime::StopRelay(TqRelayHandle* handle) {
    if (handle == nullptr || handle->Backend != TqRelayBackendType::WindowsWorker ||
        handle->WindowsWorker == nullptr) {
        return;
    }
    handle->WindowsWorker->StopRelay(handle->WindowsRelayId);
}

#endif
