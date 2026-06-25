#include "windows_relay_worker.h"

#if defined(_WIN32)

#include "msquic.hpp"
#include "relay_buffer.h"
#include "trace.h"

#include <algorithm>
#include <deque>
#include <limits>
#include <spdlog/spdlog.h>

#include <windows.h>

const MsQuicApi* MsQuic = nullptr;

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

enum class TqWindowsIocpOperationType : uint32_t {
    TcpRecv,
    TcpSend,
    QuicReceiveQueued,
    QuicReceiveViewQueued,
    QuicSendComplete,
    QuicSendRetry,
    CloseRelay,
    StopWorker,
};

struct TqWindowsQuicReceiveSlice {
    const uint8_t* Data{nullptr};
    uint32_t Length{0};
};

struct TqWindowsPendingQuicReceive {
    MsQuicStream* Stream{nullptr};
    uint64_t RelayId{0};
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
};

struct TqWindowsRelayWorker::CallbackBinding {
    TqWindowsRelayWorker* Worker{nullptr};
    uint64_t RelayId{0};
    std::atomic<RelayContext*> RelayHint{nullptr};
    std::atomic<uint32_t> CallbackRefs{0};
    std::atomic<bool> Closing{false};
};

struct TqWindowsRelayWorker::IoOperation {
    OVERLAPPED Overlapped{};
    TqWindowsIocpOperationType Event{TqWindowsIocpOperationType::TcpRecv};
    std::shared_ptr<RelayContext> Relay;
    TqBufferRef BufferOwner;
    WSABUF WsaBuffer{};
    QUIC_BUFFER QuicBuffer{};
    std::vector<uint8_t> Buffer;
    std::shared_ptr<TqWindowsPendingQuicReceive> ReceiveView;
    size_t Offset{0};
    uint64_t PostedLength{0};
    QUIC_SEND_FLAGS QuicSendFlags{QUIC_SEND_FLAG_NONE};
};

struct TqWindowsRelayWorker::RelayContext : TqRelayBufferBudget {
    explicit RelayContext(const TqTuningConfig& tuning) : Tuning(tuning) {
        MaxPendingBufferBytes = std::max<uint64_t>(
            tuning.WindowsRelayMaxPendingQuicReceiveBytesPerRelay,
            static_cast<uint64_t>(tuning.RelayIoSize) * std::max<uint32_t>(2, tuning.RelayMaxInFlightSends));
    }

    uint64_t Id{0};
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
    std::atomic<uint32_t> QueuedQuicReceives{0};
    std::atomic<uint32_t> InFlightTcpSends{0};
    std::atomic<uint64_t> PendingQuicReceiveBytes{0};
    std::atomic<uint64_t> PendingQuicReceiveQueueDepth{0};
    std::atomic<uint64_t> TcpReadBytes{0};
    std::atomic<uint64_t> TcpWriteBytes{0};
    std::atomic<uint64_t> LastTcpWriteErrno{0};
    std::atomic<const char*> CloseReason{kWindowsRelayCloseReasonUnknown};
    std::mutex PendingReceiveLock;
    std::list<std::shared_ptr<TqWindowsPendingQuicReceive>> PendingReceives;
    std::mutex PendingQuicSendLock;
    std::deque<std::unique_ptr<IoOperation>> PendingQuicSends;
    std::shared_ptr<CallbackBinding> Callback;
    std::atomic<bool> QuicReceivePaused{false};
};

TqWindowsRelayWorker::TqWindowsRelayWorker(
    uint32_t workerIndex,
    size_t queueCapacity,
    size_t eventBudget) :
    EventQueue_(queueCapacity),
    WorkerIndex_(workerIndex),
    EventBudget_(eventBudget) {}
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
    Stopping_.store(true);
    PostStop();
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
    snapshot.Errors = Errors_.load(std::memory_order_relaxed);
    snapshot.IocpCompletionDowngraded =
        IocpCompletionDowngraded_.load(std::memory_order_relaxed);
    snapshot.IocpStaleCompletionDropped =
        IocpStaleCompletionDropped_.load(std::memory_order_relaxed);
    snapshot.TcpSendZeroBytesGraceful =
        TcpSendZeroBytesGraceful_.load(std::memory_order_relaxed);
    {
        std::lock_guard<std::mutex> guard(Lock_);
        snapshot.ActiveRelays = Relays_.size();
        snapshot.ActiveRelayStates.reserve(Relays_.size());
        for (const auto& entry : Relays_) {
            if (entry.second) {
                const auto& relay = entry.second;
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
                active.QueuedQuicReceives = relay->QueuedQuicReceives.load(std::memory_order_relaxed);
                active.PendingQuicReceiveBytes = pendingQuicReceiveBytes;
                active.PendingQuicReceiveQueueDepth = pendingQuicReceiveQueueDepth;
                active.TcpReadBytes = relay->TcpReadBytes.load(std::memory_order_relaxed);
                active.TcpWriteBytes = relay->TcpWriteBytes.load(std::memory_order_relaxed);
                active.LastTcpWriteErrno = relay->LastTcpWriteErrno.load(std::memory_order_relaxed);
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
                snapshot.ActiveRelayStates.push_back(active);
            }
        }
    }

    return snapshot;
}

void TqWindowsRelayWorker::PostStop() {
    if (!::PostQueuedCompletionStatus(static_cast<HANDLE>(Iocp_), 0, 0, nullptr)) {
        Errors_.fetch_add(1, std::memory_order_relaxed);
    }
}

bool TqWindowsRelayWorker::EnqueueEvent(TqWindowsRelayTask&& task) {
    if (!EventQueue_.TryPush(std::move(task))) {
        EventQueueFullCount_.fetch_add(1, std::memory_order_relaxed);
        return false;
    }
    Wake();
    return true;
}

void TqWindowsRelayWorker::Wake() {
    if (Iocp_ == nullptr) {
        return;
    }
    if (WakeArmed_.exchange(true, std::memory_order_acq_rel)) {
        return;
    }
    if (::PostQueuedCompletionStatus(static_cast<HANDLE>(Iocp_), 0, 0, nullptr)) {
        EventQueueWakeCount_.fetch_add(1, std::memory_order_relaxed);
    } else {
        WakeArmed_.store(false, std::memory_order_release);
        EventQueueWakeFailedCount_.fetch_add(1, std::memory_order_relaxed);
    }
}

size_t TqWindowsRelayWorker::DrainEvents(size_t budget) {
    size_t processed = 0;
    while (processed < budget) {
        TqWindowsRelayTask task{};
        if (!EventQueue_.TryPop(task)) {
            break;
        }
        ProcessRelayTask(task);
        ++processed;
    }
    EventsProcessed_.fetch_add(processed, std::memory_order_relaxed);
    WakeArmed_.store(false, std::memory_order_release);
    if (EventQueue_.SizeApprox() != 0) {
        Wake();
    }
    return processed;
}

void TqWindowsRelayWorker::ProcessRelayTask(TqWindowsRelayTask& task) {
    switch (task.Type) {
    case TqWindowsRelayTaskType::TestMarker:
        break;
    case TqWindowsRelayTaskType::TcpWritable:
        break;
    case TqWindowsRelayTaskType::QuicReceive:
        break;
    case TqWindowsRelayTaskType::Shutdown:
        if (task.RelayId != 0) {
            auto relay = FindRelayById(task.RelayId);
            CloseRelay(relay, TqRelayCloseMode::GracefulDrain, "shutdown_task");
        }
        break;
    default:
        break;
    }
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
        (void)CloseRelayIfDrained(relay);
        TryRetireRelay(relay);
    }
}

std::shared_ptr<TqWindowsRelayWorker::RelayContext> TqWindowsRelayWorker::FindRelayById(
    uint64_t relayId) {
    std::lock_guard<std::mutex> guard(Lock_);
    const auto it = Relays_.find(relayId);
    return it != Relays_.end() ? it->second : nullptr;
}

void TqWindowsRelayWorker::Run() {
    while (!Stopping_.load()) {
        (void)DrainEvents(EventBudget_);
        DrainPerRelayMaintenance();

        DWORD bytes = 0;
        ULONG_PTR key = 0;
        OVERLAPPED* overlapped = nullptr;
        const BOOL ok = ::GetQueuedCompletionStatus(
            static_cast<HANDLE>(Iocp_), &bytes, &key, &overlapped, INFINITE);
        const DWORD completionError = ok ? ERROR_SUCCESS : ::GetLastError();
        if (overlapped == nullptr) {
            WakeArmed_.store(false, std::memory_order_release);
            (void)DrainEvents(EventBudget_);
            DrainPerRelayMaintenance();
            if (Stopping_.load(std::memory_order_acquire)) {
                break;
            }
            continue;
        }
        std::unique_ptr<IoOperation> op(reinterpret_cast<IoOperation*>(overlapped));
        auto relay = op->Relay;
        if (!relay) {
            if (!ok) {
                DropStaleCompletionWithoutRelay(completionError);
            }
            continue;
        }
        if (op->Event != TqWindowsIocpOperationType::TcpRecv &&
            op->Event != TqWindowsIocpOperationType::TcpSend) {
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
                    relay->LastTcpWriteErrno.store(completionError, std::memory_order_relaxed);
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
                    relay->LastTcpWriteErrno.store(completionError, std::memory_order_relaxed);
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
        case TqWindowsIocpOperationType::QuicReceiveQueued:
            HandleQuicReceiveQueued(std::move(op));
            break;
        case TqWindowsIocpOperationType::QuicReceiveViewQueued:
            HandleQuicReceiveViewQueued(std::move(op));
            break;
        case TqWindowsIocpOperationType::QuicSendRetry:
            RetryPendingQuicSends(op->Relay);
            break;
        case TqWindowsIocpOperationType::CloseRelay:
            CloseRelay(op->Relay, TqRelayCloseMode::GracefulDrain, nullptr);
            break;
        default:
            break;
        }

        (void)DrainEvents(EventBudget_);
        DrainPerRelayMaintenance();
    }
}

bool TqWindowsRelayWorker::RegisterRelay(
    TqSocketHandle tcpFd,
    MsQuicStream* stream,
    ITqCompressor* compressor,
    ITqDecompressor* decompressor,
    TqRelayHandle* handle,
    const TqTuningConfig& tuning,
    TqCompressAlgo compressAlgo) {
    if (!TqSocketValid(tcpFd) || stream == nullptr || handle == nullptr || Iocp_ == nullptr) {
        return false;
    }
    if (::CreateIoCompletionPort(
            reinterpret_cast<HANDLE>(tcpFd),
            static_cast<HANDLE>(Iocp_),
            0,
            0) == nullptr) {
        Errors_.fetch_add(1, std::memory_order_relaxed);
        return false;
    }

    auto relay = std::make_shared<RelayContext>(tuning);
    relay->Id = NextRelayId_.fetch_add(1);
    relay->TcpFd = tcpFd;
    relay->Stream = stream;
    relay->Compressor = compressor;
    relay->Decompressor = decompressor;
    relay->PublicHandle = handle;
    relay->Tuning = tuning;
    relay->CompressAlgo = compressAlgo;
    relay->Callback = std::make_shared<CallbackBinding>();
    relay->Callback->Worker = this;
    relay->Callback->RelayId = relay->Id;
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

    return PostTcpRecv(relay);
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
    relay->TcpFd = TqInvalidSocket;
    relay->Stream = stream;
    relay->PublicHandle = handle;
    relay->Tuning = tuning;
    relay->CompressAlgo = compressAlgo;
    relay->Callback = std::make_shared<CallbackBinding>();
    relay->Callback->Worker = this;
    relay->Callback->RelayId = relay->Id;
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
    FlushDeferredReceiveCompletion(*view, viewComplete);
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
    TqCloseSocket(relay->TcpFd);
    relay->TcpFd = TqInvalidSocket;
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
#endif

bool TqWindowsRelayWorker::PostTcpRecv(const std::shared_ptr<RelayContext>& relay) {
    if (!relay || relay->Closing.load() || relay->TcpRecvClosed.load()) {
        return false;
    }
    std::unique_ptr<IoOperation> op;
    {
        std::lock_guard<std::mutex> guard(relay->TcpRecvOpsLock);
        if (!relay->TcpRecvOpsFree.empty()) {
            op = std::move(relay->TcpRecvOpsFree.back());
            relay->TcpRecvOpsFree.pop_back();
            TcpRecvOperationsReused_.fetch_add(1, std::memory_order_relaxed);
        }
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
    }
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
    size_t pendingQuicSends = 0;
    {
        std::lock_guard<std::mutex> guard(relay->PendingQuicSendLock);
        pendingQuicSends = relay->PendingQuicSends.size();
    }
    state.WorkerIndex = WorkerIndex_;
    state.RelayId = relay->Id;
    state.OutstandingQuicSends =
        pendingQuicSends + relay->InFlightQuicSends.load(std::memory_order_relaxed);
    state.OutstandingQuicSendBytes = 0;
    state.PendingQuicReceiveBytes =
        relay->PendingQuicReceiveBytes.load(std::memory_order_relaxed);
    state.PendingTcpWriteQueue =
        relay->PendingQuicReceiveQueueDepth.load(std::memory_order_relaxed);
    state.TcpReadBytes = relay->TcpReadBytes.load(std::memory_order_relaxed);
    state.TcpWriteBytes = relay->TcpWriteBytes.load(std::memory_order_relaxed);
    state.TcpWriteErrno = relay->LastTcpWriteErrno.load(std::memory_order_relaxed);
    state.TcpReadClosed = relay->TcpRecvClosed.load(std::memory_order_relaxed);
    state.TcpWriteClosed = relay->TcpWriteClosed.load(std::memory_order_relaxed);
    state.QuicSendFinSubmitted = relay->QuicSendFinSubmitted.load(std::memory_order_relaxed);
    state.QuicSendFinCompleted = relay->QuicSendFinCompleted.load(std::memory_order_relaxed);
    state.StreamDetached = relay->Stream == nullptr;
    return state;
}

void TqWindowsRelayWorker::TraceRelayBackpressure(
    const std::shared_ptr<RelayContext>& relay,
    const char* action,
    const char* reason) const {
    if (!relay || !TqTraceEnabled()) {
        return;
    }
    const uint64_t maxPendingBytes = relay->Tuning.WindowsRelayMaxPendingQuicReceiveBytesPerRelay;
    TqTraceRelayBackpressureEvent(
        "windows",
        WorkerIndex_,
        relay->Id,
        action,
        reason,
        relay->PendingQuicReceiveBytes.load(std::memory_order_relaxed),
        maxPendingBytes,
        maxPendingBytes == 0 ? 0 : maxPendingBytes / 2,
        0);
}

void TqWindowsRelayWorker::FailRelayFatal(const std::shared_ptr<RelayContext>& relay, const char* reason) {
    if (relay) {
        size_t pendingQuicSends = 0;
        {
            std::lock_guard<std::mutex> guard(relay->PendingQuicSendLock);
            pendingQuicSends = relay->PendingQuicSends.size();
        }
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
    if (relay && tcpError != 0) {
        relay->LastTcpWriteErrno.store(tcpError, std::memory_order_relaxed);
    }
    Errors_.fetch_add(1, std::memory_order_relaxed);
    TcpHardErrors_.fetch_add(1, std::memory_order_relaxed);
    FailRelayFatal(relay, reason);
}

bool TqWindowsRelayWorker::HandleTcpPostFailure(
    const std::shared_ptr<RelayContext>& relay,
    const char* reason,
    int error) {
    if (relay && error != 0) {
        relay->LastTcpWriteErrno.store(static_cast<uint64_t>(error), std::memory_order_relaxed);
    }
    if (IsTcpTeardownError(error)) {
        TqTraceRelayStopCondition("windows", WorkerIndex_, reason, BuildRelayTraceState(relay));
        GracefulRelayDrains_.fetch_add(1, std::memory_order_relaxed);
        CloseRelay(relay, TqRelayCloseMode::GracefulDrain, reason);
        return true;
    }
    RecordTcpHardErrorAndFail(relay, reason, static_cast<uint64_t>(error));
    return false;
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
        relay.QueuedQuicReceives.load(std::memory_order_acquire) == 0) {
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
        relay->LastTcpWriteErrno.store(completionError, std::memory_order_relaxed);
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

bool TqWindowsRelayWorker::PostQuicSend(
    std::unique_ptr<IoOperation> op,
    QUIC_SEND_FLAGS flags,
    bool repostOnBackpressure) {
    auto relay = op ? op->Relay : nullptr;
    if (!relay || relay->Closing.load() || relay->Stream == nullptr || relay->Stream->Handle == nullptr) {
        return false;
    }
    op->QuicBuffer = QUIC_BUFFER{};
    if (!op->Buffer.empty()) {
        op->QuicBuffer.Buffer = op->Buffer.data();
        op->QuicBuffer.Length = static_cast<uint32_t>(op->Buffer.size());
    } else if (op->BufferOwner) {
        op->QuicBuffer.Buffer = op->BufferOwner->Data();
        op->QuicBuffer.Length = static_cast<uint32_t>(op->BufferOwner->Length());
    }
    op->QuicSendFlags = flags;
    relay->InFlightQuicSends.fetch_add(1);
    IoOperation* raw = op.release();
    const QUIC_STATUS status = relay->Stream->Send(
        raw->QuicBuffer.Length == 0 ? nullptr : &raw->QuicBuffer,
        raw->QuicBuffer.Length == 0 ? 0 : 1,
        flags,
        raw);
    if (!QUIC_FAILED(status)) {
        if ((flags & QUIC_SEND_FLAG_FIN) != 0) {
            relay->QuicSendFinSubmitted.store(true, std::memory_order_release);
        }
        return true;
    }
    relay->InFlightQuicSends.fetch_sub(1);
    std::unique_ptr<IoOperation> retained(raw);
    if (IsQuicSendBackpressureStatus(status)) {
        QuicSendBackpressureEvents_.fetch_add(1, std::memory_order_relaxed);
        {
            std::lock_guard<std::mutex> guard(relay->PendingQuicSendLock);
            relay->PendingQuicSends.push_back(std::move(retained));
        }
        if (repostOnBackpressure) {
            auto retry = std::make_unique<IoOperation>();
            retry->Event = TqWindowsIocpOperationType::QuicSendRetry;
            retry->Relay = relay;
            IoOperation* retryRaw = retry.release();
            relay->QueuedWorkerOps.fetch_add(1, std::memory_order_acq_rel);
            if (!::PostQueuedCompletionStatus(static_cast<HANDLE>(Iocp_), 0, 0, &retryRaw->Overlapped)) {
                relay->QueuedWorkerOps.fetch_sub(1, std::memory_order_acq_rel);
                delete retryRaw;
                Errors_.fetch_add(1, std::memory_order_relaxed);
            }
        }
        return true;
    }
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
    std::unique_ptr<IoOperation> op;
    {
        std::lock_guard<std::mutex> guard(relay->PendingQuicSendLock);
        if (relay->PendingQuicSends.empty()) {
            return;
        }
        op = std::move(relay->PendingQuicSends.front());
        relay->PendingQuicSends.pop_front();
    }
    const QUIC_SEND_FLAGS flags = op ? op->QuicSendFlags : QUIC_SEND_FLAG_NONE;
    if (!PostQuicSend(std::move(op), flags, false)) {
        return;
    }
}

bool TqWindowsRelayWorker::CloseRelayIfDrained(const std::shared_ptr<RelayContext>& relay) {
    if (!relay || !relay->CloseAfterDrained.load()) {
        return false;
    }
    if (relay->QueuedQuicReceives.load() != 0 || relay->InFlightTcpSends.load() != 0) {
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
        op->Buffer.swap(finalOutput);
        return PostQuicSend(std::move(op), QUIC_SEND_FLAG_FIN, true);
    }
    op->BufferOwner.reset();
    op->Buffer.clear();
    return PostQuicSend(std::move(op), QUIC_SEND_FLAG_FIN, true);
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
    uint8_t* sendData = op->BufferOwner->Data();
    uint32_t sendLength = static_cast<uint32_t>(op->BufferOwner->Length());
    if (relay->Compressor != nullptr) {
        std::vector<uint8_t> compressed;
        if (!relay->Compressor->Compress(sendData, sendLength, compressed, false)) {
            CloseRelay(relay, TqRelayCloseMode::GracefulDrain, "compress_tcp_recv_failed");
            return;
        }
        if (compressed.empty() && !relay->Compressor->Flush(compressed)) {
            CloseRelay(relay, TqRelayCloseMode::GracefulDrain, "compress_tcp_recv_flush_failed");
            return;
        }
        op->BufferOwner.reset();
        op->Buffer.swap(compressed);
        if (op->Buffer.empty()) {
            const bool posted = PostTcpRecv(relay);
            if (!posted && !relay->Closing.load(std::memory_order_acquire) && !relay->TcpRecvClosed.load()) {
                FailRelayFatal(relay, "post_tcp_recv_after_empty_send_failed");
            }
            return;
        }
        sendData = op->Buffer.data();
        sendLength = static_cast<uint32_t>(op->Buffer.size());
    }

    (void)sendData;
    (void)sendLength;
    (void)PostQuicSend(std::move(op), QUIC_SEND_FLAG_NONE, true);
}

void TqWindowsRelayWorker::HandleQuicReceiveViewQueued(std::unique_ptr<IoOperation> op) {
    auto relay = op->Relay;
    auto view = op->ReceiveView;
    if (!relay || !view) {
        return;
    }
#if defined(TQ_UNIT_TESTING)
    if (!QuicReceiveViewDrainEnabledForTest_.load(std::memory_order_relaxed)) {
        return;
    }
#endif
    if (relay->Closing.load()) {
        (void)CompletePendingQuicReceive(relay, view);
        return;
    }
    {
        std::lock_guard<std::mutex> guard(relay->PendingReceiveLock);
        if (relay->PendingReceives.empty() || relay->PendingReceives.front() != view) {
            return;
        }
    }
    if (relay->Decompressor != nullptr && relay->CompressAlgo == TqCompressAlgo::Zstd) {
        if (!PostTcpSendFromCompressedReceiveView(relay, view)) {
            (void)CompletePendingQuicReceive(relay, view);
            CloseRelay(relay, TqRelayCloseMode::GracefulDrain, "post_tcp_send_compressed_receive_view_failed");
        }
        return;
    }
    if (!PostTcpSendFromReceiveView(relay, view)) {
        (void)CompletePendingQuicReceive(relay, view);
        CloseRelay(relay, TqRelayCloseMode::GracefulDrain, "post_tcp_send_receive_view_failed");
    }
}

void TqWindowsRelayWorker::HandleQuicReceiveQueued(std::unique_ptr<IoOperation> op) {
    auto relay = op->Relay;
    if (!relay) {
        return;
    }
    relay->QueuedQuicReceives.fetch_sub(1);
    if (CloseRelayIfDrained(relay)) {
        return;
    }
    if (relay->Closing.load() || op->Buffer.empty()) {
        return;
    }
    if (relay->Decompressor != nullptr) {
        std::vector<uint8_t> output;
        if (!relay->Decompressor->Decompress(
                op->Buffer.data(),
                static_cast<uint32_t>(op->Buffer.size()),
                output)) {
            CloseRelay(relay, TqRelayCloseMode::GracefulDrain, "decompress_quic_receive_failed");
            return;
        }
        op->Buffer.swap(output);
    }
    if (op->Buffer.empty()) {
        (void)CloseRelayIfDrained(relay);
        return;
    }
    op->Event = TqWindowsIocpOperationType::TcpSend;
    op->Offset = 0;
    if (!PostTcpSend(std::move(op))) {
        CloseRelay(relay, TqRelayCloseMode::GracefulDrain, "post_tcp_send_quic_receive_failed");
        return;
    }
}

bool TqWindowsRelayWorker::QueueDeferredQuicReceive(
    const std::shared_ptr<RelayContext>& relay,
    MsQuicStream* stream,
    const QUIC_BUFFER* buffers,
    uint32_t bufferCount,
    bool fin) {
    if (!relay || stream == nullptr || (bufferCount != 0 && buffers == nullptr)) {
        Errors_.fetch_add(1, std::memory_order_relaxed);
        return false;
    }

    uint64_t totalLength = 0;
    for (uint32_t i = 0; i < bufferCount; ++i) {
        const QUIC_BUFFER& buffer = buffers[i];
        if (buffer.Length != 0 && buffer.Buffer == nullptr) {
            Errors_.fetch_add(1, std::memory_order_relaxed);
            return false;
        }
        totalLength += buffer.Length;
    }
    if (totalLength > static_cast<uint64_t>(std::numeric_limits<size_t>::max())) {
        Errors_.fetch_add(1, std::memory_order_relaxed);
        return false;
    }

    auto view = std::make_shared<TqWindowsPendingQuicReceive>();
    view->Stream = stream;
    view->RelayId = relay->Id;
    view->Fin = fin;
    view->CompleteBatchBytes = relay->Tuning.WindowsRelayQuicReceiveCompleteBatchBytes;
    view->OwnedBuffer.reserve(static_cast<size_t>(totalLength));
    view->Slices.reserve(bufferCount);
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
        view->Slices.push_back(TqWindowsQuicReceiveSlice{
            view->OwnedBuffer.data() + offset,
            buffer.Length});
        view->TotalLength += buffer.Length;
    }
    if (view->TotalLength == 0 && !fin) {
        return true;
    }
    if (view->TotalLength == 0 && fin) {
        relay->CloseAfterDrained.store(true, std::memory_order_release);
        if (!relay->TcpWriteClosed.exchange(true, std::memory_order_acq_rel) &&
            TqSocketValid(relay->TcpFd)) {
            (void)TqShutdownSend(relay->TcpFd);
            GracefulRelayDrains_.fetch_add(1, std::memory_order_relaxed);
        }
        (void)CloseRelayIfDrained(relay);
        return true;
    }
    if (relay->TcpRecvClosed.load(std::memory_order_acquire)) {
        TqTraceRelayStopCondition(
            "windows",
            WorkerIndex_,
            fin ? "quic_receive_after_tcp_read_closed_fin" : "quic_receive_after_tcp_read_closed",
            BuildRelayTraceState(relay));
    }

    auto op = std::make_unique<IoOperation>();
    op->Event = TqWindowsIocpOperationType::QuicReceiveViewQueued;
    op->Relay = relay;
    op->ReceiveView = view;

    const uint64_t maxPendingBytes = relay->Tuning.WindowsRelayMaxPendingQuicReceiveBytesPerRelay;
    uint64_t pendingBytes = 0;
    if (maxPendingBytes == 0) {
        pendingBytes = relay->PendingQuicReceiveBytes.fetch_add(
            view->TotalLength, std::memory_order_relaxed) + view->TotalLength;
    } else {
        pendingBytes = relay->PendingQuicReceiveBytes.fetch_add(
            view->TotalLength, std::memory_order_relaxed) + view->TotalLength;
        if (pendingBytes >= maxPendingBytes &&
            !relay->QuicReceivePaused.load(std::memory_order_acquire)) {
            SetQuicReceiveEnabled(relay, false);
        }
    }
    const uint64_t pendingDepth = relay->PendingQuicReceiveQueueDepth.fetch_add(
        1, std::memory_order_relaxed) + 1;
    {
        std::lock_guard<std::mutex> guard(relay->PendingReceiveLock);
        relay->PendingReceives.push_back(view);
    }
    relay->QueuedQuicReceives.fetch_add(1);
    DeferredReceiveQueued_.fetch_add(1, std::memory_order_relaxed);
    DeferredReceiveBytesQueued_.fetch_add(view->TotalLength, std::memory_order_relaxed);
    UpdateAtomicMax(MaxPendingQuicReceiveBytesObserved_, pendingBytes);
    UpdateAtomicMax(MaxPendingQuicReceiveQueueObserved_, pendingDepth);

    IoOperation* raw = op.release();
    relay->QueuedWorkerOps.fetch_add(1, std::memory_order_acq_rel);
    if (!::PostQueuedCompletionStatus(static_cast<HANDLE>(Iocp_), 0, 0, &raw->Overlapped)) {
        relay->QueuedWorkerOps.fetch_sub(1, std::memory_order_acq_rel);
        {
            std::lock_guard<std::mutex> guard(relay->PendingReceiveLock);
            relay->PendingReceives.remove(view);
        }
        relay->QueuedQuicReceives.fetch_sub(1);
        SaturatingFetchSub(relay->PendingQuicReceiveBytes, view->TotalLength);
        SaturatingFetchSub(relay->PendingQuicReceiveQueueDepth, 1);
        DeferredReceiveQueued_.fetch_sub(1, std::memory_order_relaxed);
        DeferredReceiveBytesQueued_.fetch_sub(view->TotalLength, std::memory_order_relaxed);
        delete raw;
        Errors_.fetch_add(1, std::memory_order_relaxed);
        return false;
    }
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
    IoOperation* raw = op.release();
    const int rc = ::WSASend(relay->TcpFd, &raw->WsaBuffer, 1, &sent, 0, &raw->Overlapped, nullptr);
    const int error = rc == 0 ? 0 : ::WSAGetLastError();
    if (rc != 0 && error != WSA_IO_PENDING) {
        relay->InFlightTcpSends.fetch_sub(1);
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
            IoOperation* raw = op.release();
            const int rc = ::WSASend(relay->TcpFd, &raw->WsaBuffer, 1, &sent, 0, &raw->Overlapped, nullptr);
            const int error = rc == 0 ? 0 : ::WSAGetLastError();
            if (rc != 0 && error != WSA_IO_PENDING) {
                relay->InFlightTcpSends.fetch_sub(1);
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
            FlushDeferredReceiveCompletion(*view, true);
            return FinishReceiveView(relay, view);
        }
        if (result.InputConsumed == 0) {
            if (!hasInput && result.NeedsMoreInput) {
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
    {
        std::lock_guard<std::mutex> guard(relay->PendingReceiveLock);
        const auto it = std::find(relay->PendingReceives.begin(), relay->PendingReceives.end(), view);
        if (it == relay->PendingReceives.end()) {
            if (view->Drained || view->CompletedLength >= view->TotalLength) {
                TqTraceRelayStopCondition(
                    "windows",
                    WorkerIndex_,
                    "finish_receive_view_already_drained",
                    BuildRelayTraceState(relay));
                return true;
            }
            TqTraceRelayStopCondition(
                "windows",
                WorkerIndex_,
                "finish_receive_view_missing",
                BuildRelayTraceState(relay));
            return false;
        }
        relay->PendingReceives.erase(it);
    }
    relay->QueuedQuicReceives.fetch_sub(1);
    if (view->AccountedLength < view->TotalLength) {
        const uint64_t remaining = view->TotalLength - view->AccountedLength;
        SaturatingFetchSub(relay->PendingQuicReceiveBytes, remaining);
        view->AccountedLength = view->TotalLength;
    }
    SaturatingFetchSub(relay->PendingQuicReceiveQueueDepth, 1);
    if (view->Fin) {
        relay->CloseAfterDrained.store(true, std::memory_order_release);
        if (!relay->TcpWriteClosed.exchange(true, std::memory_order_acq_rel) &&
            TqSocketValid(relay->TcpFd)) {
            (void)TqShutdownSend(relay->TcpFd);
            GracefulRelayDrains_.fetch_add(1, std::memory_order_relaxed);
        }
    }
    MaybeResumeQuicReceive(relay);
    std::shared_ptr<TqWindowsPendingQuicReceive> nextView;
    {
        std::lock_guard<std::mutex> guard(relay->PendingReceiveLock);
        if (!relay->PendingReceives.empty()) {
            nextView = relay->PendingReceives.front();
        }
    }
    if (nextView && !relay->Closing.load(std::memory_order_acquire)) {
        auto op = std::make_unique<IoOperation>();
        op->Event = TqWindowsIocpOperationType::QuicReceiveViewQueued;
        op->Relay = relay;
        op->ReceiveView = nextView;
        IoOperation* raw = op.release();
        relay->QueuedWorkerOps.fetch_add(1, std::memory_order_acq_rel);
        if (!::PostQueuedCompletionStatus(static_cast<HANDLE>(Iocp_), 0, 0, &raw->Overlapped)) {
            relay->QueuedWorkerOps.fetch_sub(1, std::memory_order_acq_rel);
            delete raw;
            Errors_.fetch_add(1, std::memory_order_relaxed);
        }
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
    FlushDeferredReceiveCompletion(*view, true);
    CompleteRemainingReceiveOwnership(*view);
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
    {
        std::lock_guard<std::mutex> guard(relay->PendingReceiveLock);
        pending.swap(relay->PendingReceives);
    }
    for (const auto& view : pending) {
        if (view) {
            FlushDeferredReceiveCompletion(*view, true);
            CompleteRemainingReceiveOwnership(*view);
        }
        const uint64_t remaining =
            view && view->AccountedLength < view->TotalLength
                ? view->TotalLength - view->AccountedLength
                : 0;
        relay->QueuedQuicReceives.fetch_sub(1);
        SaturatingFetchSub(relay->PendingQuicReceiveBytes, remaining);
        SaturatingFetchSub(relay->PendingQuicReceiveQueueDepth, 1);
    }
    {
        std::lock_guard<std::mutex> guard(relay->PendingQuicSendLock);
        relay->PendingQuicSends.clear();
    }
}

void TqWindowsRelayWorker::FinalizeQuicSendAccountingOnClose(
    const std::shared_ptr<RelayContext>& relay) {
    if (!relay) {
        return;
    }
    {
        std::lock_guard<std::mutex> guard(relay->PendingQuicSendLock);
        relay->PendingQuicSends.clear();
    }
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
                FlushDeferredReceiveCompletion(*view, false);
                if (!PostTcpSendFromReceiveView(relay, view)) {
                    CloseRelay(relay, TqRelayCloseMode::GracefulDrain, "post_tcp_send_receive_view_failed");
                }
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
            IoOperation* raw = op.release();
            const int rc = ::WSASend(relay->TcpFd, &raw->WsaBuffer, 1, &sent, 0, &raw->Overlapped, nullptr);
            const int error = rc == 0 ? 0 : ::WSAGetLastError();
            if (rc != 0 && error != WSA_IO_PENDING) {
                relay->InFlightTcpSends.fetch_sub(1);
                delete raw;
                (void)CompletePendingQuicReceive(relay, view);
                HandleTcpPostFailure(relay, "wsa_send_receive_view_retry_failed", error);
            } else if (error == WSA_IO_PENDING) {
                TcpSendWouldBlockOrPendingCount_.fetch_add(1, std::memory_order_relaxed);
            }
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
            }
        } else {
            AdvanceReceiveView(relay, *view, bytes);
            const bool viewComplete = view->CompletedLength >= view->TotalLength;
            FlushDeferredReceiveCompletion(*view, viewComplete);
            if (!PostTcpSendFromReceiveView(relay, view)) {
                CloseRelay(relay, TqRelayCloseMode::GracefulDrain, "post_tcp_send_receive_view_failed");
            }
        }
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

void TqWindowsRelayWorker::StopRelay(uint64_t relayId) {
    std::shared_ptr<RelayContext> relay;
    {
        std::lock_guard<std::mutex> guard(Lock_);
        const auto it = Relays_.find(relayId);
        if (it != Relays_.end()) {
            relay = it->second;
        }
    }
    if (!relay) {
        return;
    }
    MarkRelayCloseReason(relay, "stop_relay_request");
    auto op = std::make_unique<IoOperation>();
    op->Event = TqWindowsIocpOperationType::CloseRelay;
    op->Relay = relay;
    IoOperation* raw = op.release();
    relay->QueuedWorkerOps.fetch_add(1, std::memory_order_acq_rel);
    if (!::PostQueuedCompletionStatus(static_cast<HANDLE>(Iocp_), 0, 0, &raw->Overlapped)) {
        relay->QueuedWorkerOps.fetch_sub(1, std::memory_order_acq_rel);
        delete raw;
        FailRelayFatal(relay, "post_close_relay_failed");
    }
}

QUIC_STATUS QUIC_API TqWindowsRelayWorker::StreamCallback(
    MsQuicStream* stream,
    void* context,
    QUIC_STREAM_EVENT* event) noexcept {
    auto* binding = static_cast<TqWindowsRelayWorker::CallbackBinding*>(context);
    (void)stream;
    if (binding == nullptr || binding->Worker == nullptr || event == nullptr) {
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

    auto* worker = binding->Worker;
    if (event->Type == QUIC_STREAM_EVENT_SEND_COMPLETE) {
        std::unique_ptr<IoOperation> completed(
            static_cast<IoOperation*>(event->SEND_COMPLETE.ClientContext));
        if (completed && completed->Relay) {
            auto relay = completed->Relay;
            relay->InFlightQuicSends.fetch_sub(1);
            if ((completed->QuicSendFlags & QUIC_SEND_FLAG_FIN) != 0) {
                relay->QuicSendFinCompleted.store(true, std::memory_order_release);
                TqTraceRelayStopCondition(
                    "windows",
                    worker->WorkerIndex_,
                    "quic_send_fin_completed",
                    worker->BuildRelayTraceState(relay));
                if (!relay->Closing.load(std::memory_order_acquire) &&
                    relay->TcpRecvClosed.load(std::memory_order_acquire) &&
                    relay->QueuedQuicReceives.load(std::memory_order_acquire) == 0 &&
                    relay->InFlightTcpSends.load(std::memory_order_acquire) == 0 &&
                    relay->PendingQuicReceiveBytes.load(std::memory_order_acquire) == 0 &&
                    relay->TcpWriteBytes.load(std::memory_order_acquire) > 0) {
                    relay->CloseAfterDrained.store(true, std::memory_order_release);
                }
            }
            if (relay->Closing.load(std::memory_order_acquire)) {
                worker->TryRetireRelay(relay);
                return QUIC_STATUS_SUCCESS;
            }
            worker->RetryPendingQuicSends(relay);
            bool hasPendingQuicSends = false;
            {
                std::lock_guard<std::mutex> guard(relay->PendingQuicSendLock);
                hasPendingQuicSends = !relay->PendingQuicSends.empty();
            }
            const bool postNextRecv =
                !binding->Closing.load(std::memory_order_acquire) && !relay->Closing.load() &&
                !relay->TcpRecvClosed.load() && !hasPendingQuicSends;
            if (postNextRecv) {
                completed->BufferOwner.reset();
                completed->Buffer.clear();
                completed->QuicBuffer = QUIC_BUFFER{};
                completed->ReceiveView.reset();
                completed->Relay.reset();
                completed->Offset = 0;
                completed->PostedLength = 0;
                bool posted = false;
                {
                    std::lock_guard<std::mutex> guard(relay->TcpRecvOpsLock);
                    relay->TcpRecvOpsFree.push_back(std::move(completed));
                }
                posted = worker->PostTcpRecv(relay);
                if (!posted && !binding->Closing.load(std::memory_order_acquire) &&
                    !relay->Closing.load(std::memory_order_acquire) && !relay->TcpRecvClosed.load()) {
                    worker->CloseRelay(relay, TqRelayCloseMode::GracefulDrain, "post_tcp_recv_after_quic_send_complete_failed");
                }
            } else if (!hasPendingQuicSends) {
                (void)worker->CloseRelayIfDrained(relay);
            }
        }
        return QUIC_STATUS_SUCCESS;
    }

    if (binding->Closing.load(std::memory_order_acquire)) {
        return QUIC_STATUS_SUCCESS;
    }
    RelayContext* relayHint = binding->RelayHint.load(std::memory_order_acquire);

    std::shared_ptr<RelayContext> relay;
    {
        std::lock_guard<std::mutex> guard(worker->Lock_);
        const auto it = worker->Relays_.find(binding->RelayId);
        if (it != worker->Relays_.end() && it->second && it->second->Callback.get() == binding &&
            (relayHint == nullptr || it->second.get() == relayHint)) {
            relay = it->second;
        }
    }
    if (!relay || relay->Closing.load(std::memory_order_acquire)) {
        return QUIC_STATUS_SUCCESS;
    }

    if (event->Type == QUIC_STREAM_EVENT_RECEIVE) {
        if (binding->Closing.load(std::memory_order_acquire) ||
            relay->Closing.load(std::memory_order_acquire)) {
            return QUIC_STATUS_SUCCESS;
        }
        const bool fin = (event->RECEIVE.Flags & QUIC_RECEIVE_FLAG_FIN) != 0;
        if (!worker->QueueDeferredQuicReceive(
                relay,
                stream,
                event->RECEIVE.Buffers,
                event->RECEIVE.BufferCount,
                fin)) {
            worker->FailRelayFatal(relay, "quic_receive_queue_failed");
            return QUIC_STATUS_SUCCESS;
        }
        return QUIC_STATUS_PENDING;
    }
    if (event->Type == QUIC_STREAM_EVENT_PEER_SEND_SHUTDOWN) {
        return QUIC_STATUS_SUCCESS;
    }
    if (event->Type == QUIC_STREAM_EVENT_PEER_SEND_ABORTED ||
        event->Type == QUIC_STREAM_EVENT_PEER_RECEIVE_ABORTED) {
        worker->FailRelayFatal(relay, event->Type == QUIC_STREAM_EVENT_PEER_SEND_ABORTED
            ? "stream_peer_send_aborted"
            : "stream_peer_receive_aborted");
        return QUIC_STATUS_SUCCESS;
    }
    if (event->Type == QUIC_STREAM_EVENT_SHUTDOWN_COMPLETE) {
        relay->CloseAfterDrained.store(true);
        TqTraceRelayStreamShutdown("windows", worker->BuildRelayTraceState(relay));
        (void)worker->CloseRelayIfDrained(relay);
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
        auto worker = std::make_unique<TqWindowsRelayWorker>(
            i,
            tuning.WindowsRelayEventQueueCapacity,
            tuning.WindowsRelayWorkerEventBudget);
        if (!worker->Start()) {
            Workers_.clear();
            return false;
        }
        Workers_.push_back(std::move(worker));
    }
    return true;
}

void TqWindowsRelayRuntime::Stop() {
    std::lock_guard<std::mutex> guard(Lock_);
    Workers_.clear();
}

TqWindowsRelayWorkerSnapshot TqWindowsRelayRuntime::Snapshot() const {
    TqWindowsRelayWorkerSnapshot total{};
    std::lock_guard<std::mutex> guard(Lock_);
    for (const auto& worker : Workers_) {
        if (!worker) {
            continue;
        }
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
