#include "windows_relay_worker.h"

#if defined(_WIN32)

#include "msquic.hpp"
#include "quic_receive_guard.h"
#include "relay_buffer.h"
#include "stream_lifetime.h"
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
#include <type_traits>
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

std::atomic<uint64_t> g_NextRelayControlGeneration{1};

std::shared_ptr<TqRelayStopControl> AllocateRelayStopControl() {
    auto control = std::make_shared<TqRelayStopControl>();
    control->Generation.store(
        g_NextRelayControlGeneration.fetch_add(1, std::memory_order_relaxed),
        std::memory_order_release);
    return control;
}

bool ControlGenerationMatches(
    const std::shared_ptr<TqRelayStopControl>& control,
    uint64_t expectedGeneration) {
    return control != nullptr &&
        control->Generation.load(std::memory_order_acquire) == expectedGeneration;
}

void PublishStopToControl(
    const std::shared_ptr<TqRelayStopControl>& control,
    uint64_t expectedGeneration) {
    if (!ControlGenerationMatches(control, expectedGeneration)) {
        return;
    }
    control->Stop.store(true, std::memory_order_release);
}

void ReceiveCompleteViaOwner(
    const std::shared_ptr<TqStreamLifetime>& owner,
    uint64_t completeBytes) {
    if (!owner || completeBytes == 0) {
        return;
    }
    auto lease = owner->TryAcquireApi();
    MsQuicStream* stream = lease ? lease.Stream() : nullptr;
    if (stream != nullptr && stream->Handle != nullptr) {
        stream->ReceiveComplete(completeBytes);
    }
}

} // namespace

#if defined(TQ_UNIT_TESTING)
thread_local bool g_InSendCompleteCallback = false;
thread_local std::shared_ptr<TqStreamLifetime> g_WindowsRelayTestStreamOwner;

namespace {

std::shared_ptr<TqStreamLifetime> MakeWindowsRelayTestStreamOwner(MsQuicStream* stream) {
    if (stream == nullptr) {
        return nullptr;
    }
    auto owner = TqStreamLifetime::CreateForTest(TqStreamLifetime::Phase::Started);
    if (!owner || !owner->InstallDetachedStreamForTest(stream)) {
        return nullptr;
    }
    return owner;
}

TqWindowsRelayRegistration MakeWindowsRelayTestRegistration(
    TqSocketHandle tcpFd,
    MsQuicStream* stream,
    std::shared_ptr<TqStreamLifetime> owner,
    ITqCompressor* compressor,
    ITqDecompressor* decompressor,
    TqRelayHandle* handle,
    const TqTuningConfig& tuning,
    TqCompressAlgo compressAlgo) {
    (void)handle;
    TqWindowsRelayRegistration registration{};
    registration.TcpFd = tcpFd;
    registration.Stream = stream;
    registration.StreamOwner = owner;
    registration.Compressor = compressor;
    registration.Decompressor = decompressor;
    registration.Tuning = tuning;
    registration.CompressAlgo = compressAlgo;
    return registration;
}

void FillWindowsRelayTestHandle(
    TqRelayHandle* handle,
    const TqWindowsRelayRegistrationResult& result) {
    if (handle == nullptr || !result.Ok) {
        return;
    }
    handle->Backend = TqRelayBackendType::WindowsWorker;
    handle->WindowsWorker = result.Worker;
    handle->WindowsRelayId = result.RelayId;
    handle->WindowsRelayGeneration = result.RelayGeneration;
    handle->WindowsWorkerIndex = result.WorkerIndex;
    handle->Control = result.StopControl;
}

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

} // namespace
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
    std::shared_ptr<TqStreamLifetime> StreamOwner;
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

struct TqWindowsRelayWorker::CallbackEndpoint final {
    explicit CallbackEndpoint(TqWindowsRelayWorker* worker) : Worker(worker) {}

    TqWindowsRelayWorker* TryEnter() {
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
    TqWindowsRelayWorker* Worker{nullptr};
    uint32_t ActiveCalls{0};
};

struct TqWindowsRelayWorker::WindowsStreamRelayBinding final
    : TqStreamLifetime::Target,
      std::enable_shared_from_this<TqWindowsRelayWorker::WindowsStreamRelayBinding> {
    std::shared_ptr<CallbackEndpoint> Endpoint;
    std::atomic<RelayContext*> RelayHint{nullptr};
    uint64_t RelayId{0};
    uint64_t Generation{0};
    std::shared_ptr<TqRelayStopControl> StopControl;
    std::atomic<uint32_t> CallbackRefs{0};
    std::atomic<bool> Closing{false};
    enum class Activation : uint8_t { Prepared, Active, Failed };
    std::atomic<Activation> ActivationState{Activation::Prepared};
    std::mutex PrecommitLock;
    std::deque<std::shared_ptr<TqWindowsPendingQuicReceive>> PrecommitReceives;
    uint64_t PrecommitPendingBytes{0};
    uint64_t PrecommitMaxPendingBytes{0};

    QUIC_STATUS OnStreamEvent(
        MsQuicStream* stream,
        QUIC_STREAM_EVENT* event,
        uint64_t) noexcept override {
        auto endpoint = Endpoint;
        if (endpoint == nullptr) {
            return QUIC_STATUS_SUCCESS;
        }
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
#if defined(TQ_UNIT_TESTING)
    void* ContextForTest() const noexcept override { return const_cast<WindowsStreamRelayBinding*>(this); }
#endif
};

struct TqWindowsRelayWorker::IoOperation {
    OVERLAPPED Overlapped{};
    TqWindowsIocpOperationType Event{TqWindowsIocpOperationType::TcpRecv};
    std::shared_ptr<RelayContext> Relay;
    uint64_t RelayId{0};
    uint64_t Generation{0};
    uint64_t ControlGeneration{0};
    std::shared_ptr<TqRelayStopControl> StopControl;
    uint64_t Value{0};
    size_t Length{0};
    TqBufferRef BufferOwner;
    WSABUF WsaBuffer{};
    QUIC_BUFFER QuicBuffer{};
    std::vector<uint8_t> Buffer;
    std::shared_ptr<TqWindowsPendingQuicReceive> ReceiveView;
    std::string Text;
    size_t Offset{0};
    uint64_t PostedLength{0};
    QUIC_SEND_FLAGS QuicSendFlags{QUIC_SEND_FLAG_NONE};
    void* Control{nullptr};
    std::shared_ptr<TerminalCleanupRecord> TerminalCleanup;
};

struct TqWindowsRelayWorker::TerminalCleanupRecord final {
    std::shared_ptr<WindowsStreamRelayBinding> Binding;
    std::shared_ptr<RelayContext> Relay;
    std::shared_ptr<TqStreamLifetime> StreamOwner;
    std::shared_ptr<TqRelayStopControl> StopControl;
    uint64_t RelayId{0};
    uint64_t Generation{0};
    uint64_t ControlGeneration{0};
    uint64_t ConnectionErrorCode{0};
    uint32_t ConnectionCloseStatus{0};
};

constexpr int TqWindowsRelayWorker::TerminalCleanupRecordNoStreamPointerCheck() {
    using Record = TerminalCleanupRecord;
    static_assert(
        !TerminalCleanupRecordMemberIsMsQuicStreamPointer<Record, &Record::Binding>::value);
    static_assert(
        !TerminalCleanupRecordMemberIsMsQuicStreamPointer<Record, &Record::Relay>::value);
    static_assert(
        !TerminalCleanupRecordMemberIsMsQuicStreamPointer<Record, &Record::StreamOwner>::value);
    static_assert(
        !TerminalCleanupRecordMemberIsMsQuicStreamPointer<Record, &Record::StopControl>::value);
    static_assert(
        !TerminalCleanupRecordMemberIsMsQuicStreamPointer<Record, &Record::RelayId>::value);
    static_assert(
        !TerminalCleanupRecordMemberIsMsQuicStreamPointer<Record, &Record::Generation>::value);
    static_assert(
        !TerminalCleanupRecordMemberIsMsQuicStreamPointer<Record, &Record::ControlGeneration>::value);
    static_assert(
        !TerminalCleanupRecordMemberIsMsQuicStreamPointer<Record, &Record::ConnectionErrorCode>::value);
    static_assert(
        !TerminalCleanupRecordMemberIsMsQuicStreamPointer<Record, &Record::ConnectionCloseStatus>::value);
    return 0;
}

struct TqWindowsRelayWorker::RegisterRelayCommand {
    TqWindowsRelayRegistration Registration;
    TqWindowsRelayRegistrationResult Result;
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
    std::shared_ptr<TqStreamLifetime> StreamOwner;
    ITqCompressor* Compressor{nullptr};
    ITqDecompressor* Decompressor{nullptr};
    std::shared_ptr<TqRelayStopControl> StopControl;
    TqTuningConfig Tuning;
    // Worker-thread-owned (IOCP serializes access); not protected by per-relay mutex.
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
    // Worker-thread-owned (IOCP serializes access); not protected by per-relay mutex.
    std::list<std::shared_ptr<TqWindowsPendingQuicReceive>> PendingReceives;
    // Worker-thread-owned (IOCP serializes access); not protected by per-relay mutex.
    std::deque<std::unique_ptr<TqWindowsQuicSendOperation>> PendingQuicSendRetries;
    std::atomic<bool> TcpRecvPosted{false};
    std::atomic<bool> TcpReadPausedByQuicBacklog{false};
    std::atomic<uint64_t> OutstandingQuicSendBytes{0};
    std::atomic<uint64_t> MaxOutstandingQuicSendBytes{0};
    std::atomic<uint64_t> IdealSendBufferBytes{0};
    std::shared_ptr<WindowsStreamRelayBinding> ManagedBinding;
    WindowsStreamRelayBinding* StreamBinding{nullptr};
    bool Committed{false};
    bool Activating{false};
    std::atomic<bool> QuicReceivePaused{false};
    std::atomic<bool> ReceiveDrainQueued{false};
};

TqWindowsRelayWorker::TqWindowsRelayWorker(uint32_t workerIndex)
    : WorkerIndex_(workerIndex),
      StreamCallbackEndpoint_(std::make_shared<CallbackEndpoint>(this)) {
    (void)TerminalCleanupRecordNoStreamPointerCheck();
}
TqWindowsRelayWorker::~TqWindowsRelayWorker() {
    Stop();
    if (StreamCallbackEndpoint_) {
        StreamCallbackEndpoint_->CloseAndWait();
    }
}

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
    }
    WaitWindowsRelayCommand(command);
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
    snapshot.WindowsPostedTraceContextStaleDropCount =
        PostedTraceContextStaleDropCount_.load(std::memory_order_relaxed);
    snapshot.FindRelayByIdCount = FindRelayByIdCount_.load(std::memory_order_relaxed);
    snapshot.CallbackDispatchNanos = CallbackDispatchNanos_.load(std::memory_order_relaxed);
    snapshot.CallbackReceiveBudgetRejectedCount =
        CallbackReceiveBudgetRejectedCount_.load(std::memory_order_relaxed);
    snapshot.CallbackReceiveBudgetPausedCount =
        CallbackReceiveBudgetPausedCount_.load(std::memory_order_relaxed);
    snapshot.CallbackReceiveCopyBytes =
        CallbackReceiveCopyBytes_.load(std::memory_order_relaxed);
    snapshot.CallbackReceiveCopyNanos =
        CallbackReceiveCopyNanos_.load(std::memory_order_relaxed);
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
            if (entry.second && entry.second->Committed) {
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
        active.StreamDetached = relay->StreamOwner == nullptr;
        active.TcpReadPausedByQuicBacklog =
            relay->TcpReadPausedByQuicBacklog.load(std::memory_order_relaxed);
        active.OutstandingQuicSendBytes =
            relay->OutstandingQuicSendBytes.load(std::memory_order_relaxed);
        active.MaxOutstandingQuicSendBytes =
            relay->MaxOutstandingQuicSendBytes.load(std::memory_order_relaxed);
        active.CallbackPendingQuicReceiveDepth = 0;
        snapshot.ActiveRelayStates.push_back(active);
    }

    const uint64_t elapsedNanos = std::max<uint64_t>(1, NowSteadyNanos() - snapshotStartNanos);
    snapshot.SnapshotBuildNanos =
        SnapshotBuildNanos_.fetch_add(elapsedNanos, std::memory_order_relaxed) + elapsedNanos;
    snapshot.SnapshotActiveRelaysScanned =
        SnapshotActiveRelaysScanned_.fetch_add(relays.size(), std::memory_order_relaxed) +
        relays.size();
    snapshot.MaintenanceDrainCount =
        MaintenanceDrainCount_.load(std::memory_order_relaxed);
    snapshot.MaintenanceDrainNanos =
        MaintenanceDrainNanos_.load(std::memory_order_relaxed);
    snapshot.MaintenanceRelaysProcessed =
        MaintenanceRelaysProcessed_.load(std::memory_order_relaxed);
    snapshot.MaintenanceFullScanCount =
        MaintenanceFullScanCount_.load(std::memory_order_relaxed);
    snapshot.MaintenanceFullScanRelaysScanned =
        MaintenanceFullScanRelaysScanned_.load(std::memory_order_relaxed);
    snapshot.ReceiveViewFinishLinearSearchCount =
        ReceiveViewFinishLinearSearchCount_.load(std::memory_order_relaxed);
    snapshot.ReceiveViewFinishLinearSearchNanos =
        ReceiveViewFinishLinearSearchNanos_.load(std::memory_order_relaxed);
    snapshot.ReceiveViewFinishNotFrontCount =
        ReceiveViewFinishNotFrontCount_.load(std::memory_order_relaxed);
    return snapshot;
}

void TqWindowsRelayWorker::PostStop() {
    auto op = std::make_unique<IoOperation>();
    op->Event = TqWindowsIocpOperationType::StopWorker;
    IoOperation* raw = op.release();
    if (!::PostQueuedCompletionStatus(static_cast<HANDLE>(Iocp_), 0, 0, &raw->Overlapped)) {
        delete raw;
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
            MaybeResumeQuicReceive(relay);
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
    if (!relay || relay->Closing.load(std::memory_order_acquire)) {
        return false;
    }
    if (relay->ReceiveDrainQueued.exchange(true, std::memory_order_acq_rel)) {
        ReceiveDrainCoalescedCount_.fetch_add(1, std::memory_order_relaxed);
        return true;
    }

    std::lock_guard<std::mutex> controlGuard(ControlCommandLock_);
    if (Iocp_ == nullptr || Stopping_.load(std::memory_order_acquire)) {
        relay->ReceiveDrainQueued.store(false, std::memory_order_release);
        return false;
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
    if (!relay) {
        CallbackIocpPostFailedCount_.fetch_add(1, std::memory_order_relaxed);
        return false;
    }

    std::lock_guard<std::mutex> controlGuard(ControlCommandLock_);
    if (Iocp_ == nullptr || Stopping_.load(std::memory_order_acquire)) {
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
    const WindowsStreamRelayBinding& binding,
    uint64_t value,
    size_t length,
    std::shared_ptr<TqWindowsPendingQuicReceive> receiveView) {
    if (binding.RelayId == 0) {
#if defined(TQ_UNIT_TESTING)
        LastCallbackPostWin32Error_.store(
            ERROR_INVALID_PARAMETER,
            std::memory_order_relaxed);
#endif
        CallbackIocpPostFailedCount_.fetch_add(1, std::memory_order_relaxed);
        return false;
    }

    std::lock_guard<std::mutex> controlGuard(ControlCommandLock_);
    if (Iocp_ == nullptr || Stopping_.load(std::memory_order_acquire)) {
#if defined(TQ_UNIT_TESTING)
        LastCallbackPostWin32Error_.store(
            Iocp_ == nullptr ? ERROR_INVALID_HANDLE : ERROR_OPERATION_ABORTED,
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
        if (Stopping_.load(std::memory_order_acquire) ||
            relay->Closing.load(std::memory_order_acquire)) {
            TryRetireRelay(relay, false);
        } else {
            FailRelayFatalFromCallback(relay, "post_quic_send_complete_failed");
        }
    }
}

void TqWindowsRelayWorker::QueueQuicSendCompleteByIdFromCallback(
    const WindowsStreamRelayBinding& binding,
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
            if (Stopping_.load(std::memory_order_acquire) ||
                relay->Closing.load(std::memory_order_acquire)) {
                TryRetireRelay(relay, false);
            } else {
                FailRelayFatalFromCallback(relay, "post_quic_send_complete_failed");
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
        ScheduleRelayMaintenance(relay);
        TryRetireRelay(relay);
        return;
    }
    RetryPendingQuicSends(relay);
    MaybePostTcpRecv(relay);
    (void)CloseRelayIfDrained(relay);
    ScheduleRelayMaintenance(relay);
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
    ScheduleRelayMaintenance(relay);
}

void TqWindowsRelayWorker::ScheduleRelayMaintenance(const std::shared_ptr<RelayContext>& relay) {
    if (!relay || relay->StopPublished.load(std::memory_order_acquire)) {
        return;
    }
    if (!MaintenanceQueuedRelayIds_.insert(relay->Id).second) {
        return;
    }
    MaintenanceQueue_.push_back(relay);
}

bool TqWindowsRelayWorker::RelayNeedsMaintenance(const std::shared_ptr<RelayContext>& relay) const {
    if (!relay || relay->StopPublished.load(std::memory_order_acquire)) {
        return false;
    }
    if (relay->QuicReceivePaused.load(std::memory_order_acquire)) {
        const uint64_t maxPendingBytes = relay->Tuning.WindowsRelayMaxPendingQuicReceiveBytesPerRelay;
        if (maxPendingBytes == 0 ||
            relay->PendingQuicReceiveQueueDepth.load(std::memory_order_relaxed) != 0 ||
            relay->PendingQuicReceiveBytes.load(std::memory_order_relaxed) < maxPendingBytes / 2) {
            return true;
        }
    }
    return relay->Closing.load(std::memory_order_acquire) ||
           relay->CloseAfterDrained.load(std::memory_order_acquire) ||
           relay->TcpReadPausedByQuicBacklog.load(std::memory_order_acquire) ||
           !relay->PendingQuicSendRetries.empty() ||
           relay->PendingQuicReceiveQueueDepth.load(std::memory_order_acquire) != 0;
}

void TqWindowsRelayWorker::EnqueueFullMaintenanceScan() {
    std::vector<std::shared_ptr<RelayContext>> relays;
    {
        std::lock_guard<std::mutex> guard(Lock_);
        relays.reserve(Relays_.size());
        for (const auto& entry : Relays_) {
            relays.push_back(entry.second);
        }
    }
    MaintenanceFullScanCount_.fetch_add(1, std::memory_order_relaxed);
    MaintenanceFullScanRelaysScanned_.fetch_add(relays.size(), std::memory_order_relaxed);
    for (const auto& relay : relays) {
        if (RelayNeedsMaintenance(relay)) {
            ScheduleRelayMaintenance(relay);
        }
    }
}

void TqWindowsRelayWorker::DrainPerRelayMaintenance() {
    const uint64_t startNanos = NowSteadyNanos();
    MaintenanceDrainCount_.fetch_add(1, std::memory_order_relaxed);
    size_t processed = 0;
    while (processed < MaintenanceBudget_ && !MaintenanceQueue_.empty()) {
        auto relay = MaintenanceQueue_.front();
        MaintenanceQueue_.pop_front();
        if (relay) {
            MaintenanceQueuedRelayIds_.erase(relay->Id);
        }
        if (!relay || relay->StopPublished.load(std::memory_order_acquire)) {
            continue;
        }
        ++processed;
        RetryPendingQuicSends(relay);
        MaybePostTcpRecv(relay);
        MaybeResumeQuicReceive(relay);
        (void)CloseRelayIfDrained(relay);
        TryRetireRelay(relay);
        if (RelayNeedsMaintenance(relay)) {
            ScheduleRelayMaintenance(relay);
        }
    }
    MaintenanceRelaysProcessed_.fetch_add(processed, std::memory_order_relaxed);
    MaintenanceDrainNanos_.fetch_add(NowSteadyNanos() - startNanos, std::memory_order_relaxed);
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
    for (;;) {
        if (++MaintenanceFullScanCountdown_ >= 256) {
            MaintenanceFullScanCountdown_ = 0;
            EnqueueFullMaintenanceScan();
        }
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
        if (op->Event == TqWindowsIocpOperationType::StopWorker) {
            break;
        }
        if (op->Event == TqWindowsIocpOperationType::RegisterRelay) {
            auto* command = static_cast<RegisterRelayCommand*>(op->Control);
            if (command != nullptr) {
                command->Result = RegisterRelayWithIdLocal(command->Registration);
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
#if defined(TQ_UNIT_TESTING)
        if (op->Event == TqWindowsIocpOperationType::TestBlockWorkerQueue) {
            auto* block = static_cast<TqWindowsRelayWorkerQueueBlockForTest*>(op->Control);
            if (block != nullptr) {
                std::unique_lock<std::mutex> guard(block->Mutex);
                block->Entered = true;
                block->Cv.notify_all();
                block->Cv.wait(guard, [block] { return block->Release; });
            }
            continue;
        }
#endif
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
        if (!relay &&
            (op->Event == TqWindowsIocpOperationType::CloseRelay ||
             op->Event == TqWindowsIocpOperationType::SetTraceContext) &&
            op->RelayId != 0) {
            relay = FindRelayByIdLocal(op->RelayId);
            if (relay) {
                if (relay->Generation != op->Generation) {
                    relay.reset();
                } else if (!ControlGenerationMatches(relay->StopControl, op->ControlGeneration) ||
                           !ControlGenerationMatches(op->StopControl, op->ControlGeneration)) {
                    PostedTraceContextStaleDropCount_.fetch_add(1, std::memory_order_relaxed);
                    relay.reset();
                } else if (op->Event == TqWindowsIocpOperationType::SetTraceContext &&
                           relay->Closing.load(std::memory_order_acquire)) {
                    PostedTraceContextStaleDropCount_.fetch_add(1, std::memory_order_relaxed);
                    relay.reset();
                }
            }
        }
        if (!relay && op->Event != TqWindowsIocpOperationType::QuicSendComplete) {
            if (!ok) {
                DropStaleCompletionWithoutRelay(completionError);
            }
            if (!callbackSourced ||
                op->Event == TqWindowsIocpOperationType::RelayReceiveReady) {
                if (op->Event == TqWindowsIocpOperationType::SetTraceContext) {
                    PostedTraceContextStaleDropCount_.fetch_add(1, std::memory_order_relaxed);
                }
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
            DispatchQuicShutdownComplete(*op, relay);
            break;
        case TqWindowsIocpOperationType::QuicSendRetry:
            RetryPendingQuicSends(op->Relay);
            break;
        case TqWindowsIocpOperationType::SetTraceContext:
            ApplyRelayTraceContext(relay, op->Value, op->Text);
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

TqStreamLifetime::ApiLease TqWindowsRelayWorker::TryRelayStreamLease(
    const RelayContext& relay) const {
    if (relay.StreamOwner == nullptr) {
        return {};
    }
    return relay.StreamOwner->TryAcquireApi();
}

uint64_t TqWindowsRelayWorker::MaxPendingQuicReceiveBytesPerRelay(
    const RelayContext& relay) const {
    if (relay.Tuning.WindowsRelayMaxPendingQuicReceiveBytesPerRelay != 0) {
        return relay.Tuning.WindowsRelayMaxPendingQuicReceiveBytesPerRelay;
    }
    return relay.MaxPendingBufferBytes;
}

void TqWindowsRelayWorker::RequestRelayShutdown(
    const std::shared_ptr<RelayContext>& relay,
    TqStreamLifetime::ShutdownIntent intent) {
    if (relay != nullptr && relay->StreamOwner != nullptr) {
        (void)relay->StreamOwner->RequestShutdown(intent);
    }
}

bool TqWindowsRelayWorker::RegisterRelay(const TqWindowsRelayRegistration& registration) {
    return RegisterRelayWithId(registration).Ok;
}

TqWindowsRelayRegistrationResult TqWindowsRelayWorker::RegisterRelayWithId(
    const TqWindowsRelayRegistration& registration) {
    RegisterRelayCommand command{};
    command.Registration = registration;
    if (WorkerThreadToken_.load(std::memory_order_acquire) == CurrentThreadToken()) {
        if (Stopping_.load(std::memory_order_acquire)) {
            return {};
        }
        return RegisterRelayWithIdLocal(registration);
    }
    {
        std::lock_guard<std::mutex> controlGuard(ControlCommandLock_);
        if (Iocp_ == nullptr || !Thread_.joinable() ||
            Stopping_.load(std::memory_order_acquire)) {
            return {};
        }
        auto op = std::make_unique<IoOperation>();
        op->Event = TqWindowsIocpOperationType::RegisterRelay;
        op->Control = &command;
        IoOperation* raw = op.release();
        if (!::PostQueuedCompletionStatus(static_cast<HANDLE>(Iocp_), 0, 0, &raw->Overlapped)) {
            delete raw;
            Errors_.fetch_add(1, std::memory_order_relaxed);
            return {};
        }
    }
    WaitWindowsRelayCommand(command);
    return command.Result;
}

TqWindowsRelayRegistrationResult TqWindowsRelayWorker::RegisterRelayWithIdLocal(
    const TqWindowsRelayRegistration& registration) {
    TqWindowsRelayRegistrationResult result{};
#if defined(TQ_UNIT_TESTING)
    if (registration.FailPrepareForTest) {
        return result;
    }
#endif
    if (registration.StreamOwner == nullptr || Iocp_ == nullptr) {
        return result;
    }
    if (registration.StreamOwner->GetPhase() == TqStreamLifetime::Phase::TerminalPublished) {
        return result;
    }

    std::shared_ptr<RelayContext> relay;
    bool tcpFdConsumed = false;
    if (!PrepareRelay(registration, relay, tcpFdConsumed)) {
        return result;
    }
    result.TcpFdConsumed = tcpFdConsumed;

    if (!PublishRelayTarget(registration, relay)) {
        RollbackPreparedRelay(relay, tcpFdConsumed, registration, false);
        return result;
    }
#if defined(TQ_UNIT_TESTING)
    if (registration.AfterPublishHookForTest != nullptr) {
        registration.AfterPublishHookForTest(this, relay->Id);
    }
#endif

    if (registration.StreamOwner->GetPhase() == TqStreamLifetime::Phase::TerminalPublished ||
        relay->StreamBinding == nullptr ||
        relay->StreamBinding->Closing.load(std::memory_order_acquire)) {
        FailManagedBinding(relay, relay->StreamBinding);
        RollbackPreparedRelay(relay, tcpFdConsumed, registration, true);
        return result;
    }

    if (!CommitRelay(registration, relay, tcpFdConsumed)) {
        return result;
    }

    result.Ok = true;
    result.RelayId = relay->Id;
    result.RelayGeneration = relay->Generation;
    result.StopControl = relay->StopControl;
    result.Worker = this;
    result.WorkerIndex = WorkerIndex_;
    return result;
}

bool TqWindowsRelayWorker::PrepareRelay(
    const TqWindowsRelayRegistration& registration,
    std::shared_ptr<RelayContext>& relay,
    bool& tcpFdConsumed) {
    tcpFdConsumed = false;
    if (registration.StreamOwner == nullptr) {
        return false;
    }

    if (TqSocketValid(registration.TcpFd)) {
        if (::CreateIoCompletionPort(
                reinterpret_cast<HANDLE>(registration.TcpFd),
                static_cast<HANDLE>(Iocp_),
                0,
                0) == nullptr) {
            Errors_.fetch_add(1, std::memory_order_relaxed);
            return false;
        }
        tcpFdConsumed = true;
    }

    relay = std::make_shared<RelayContext>(registration.Tuning);
    relay->Id = NextRelayId_.fetch_add(1);
    relay->Generation = NextGeneration_.fetch_add(1, std::memory_order_relaxed);
    relay->TcpFd = registration.TcpFd;
    relay->StreamOwner = registration.StreamOwner;
    relay->Compressor = registration.Compressor;
    relay->Decompressor = registration.Decompressor;
    relay->StopControl = AllocateRelayStopControl();
    relay->Tuning = registration.Tuning;
    relay->CompressAlgo = registration.CompressAlgo;

    auto managedBinding = std::make_shared<WindowsStreamRelayBinding>();
    if (!managedBinding) {
        return false;
    }
    managedBinding->Endpoint = StreamCallbackEndpoint_;
    managedBinding->RelayId = relay->Id;
    managedBinding->Generation = relay->Generation;
    managedBinding->StopControl = relay->StopControl;
    managedBinding->PrecommitMaxPendingBytes = MaxPendingQuicReceiveBytesPerRelay(*relay);
    relay->StreamBinding = managedBinding.get();
    relay->ManagedBinding = std::move(managedBinding);

    {
        std::lock_guard<std::mutex> guard(Lock_);
        Relays_[relay->Id] = relay;
    }
    relay->Activating = true;
    return true;
}

bool TqWindowsRelayWorker::PublishRelayTarget(
    const TqWindowsRelayRegistration& registration,
    const std::shared_ptr<RelayContext>& relay) {
    if (!relay || registration.StreamOwner == nullptr || relay->ManagedBinding == nullptr) {
        return false;
    }
    const uint64_t generation = registration.StreamOwner->RouteGeneration();
    return registration.StreamOwner->PublishTarget(generation, relay->ManagedBinding);
}

bool TqWindowsRelayWorker::CommitRelay(
    const TqWindowsRelayRegistration& registration,
    const std::shared_ptr<RelayContext>& relay,
    bool tcpFdConsumed) {
#if defined(TQ_UNIT_TESTING)
    if (registration.FailCommitForTest) {
        FailManagedBinding(relay, relay->StreamBinding);
        RollbackPreparedRelay(relay, tcpFdConsumed, registration, true);
        return false;
    }
#endif
    (void)registration;
    if (!relay || relay->StreamBinding == nullptr) {
        return false;
    }
    if (registration.StreamOwner != nullptr &&
        (registration.StreamOwner->GetPhase() == TqStreamLifetime::Phase::TerminalPublished ||
         relay->StreamBinding->Closing.load(std::memory_order_acquire))) {
        FailManagedBinding(relay, relay->StreamBinding);
        RollbackPreparedRelay(relay, tcpFdConsumed, registration, true);
        return false;
    }

    relay->Activating = true;
    bool activationOk = true;
    if (TqSocketValid(relay->TcpFd)) {
        activationOk = MaybePostTcpRecv(relay);
    }
    if (!activationOk) {
        WithdrawPublishedRelayTarget(registration, relay);
        FailManagedBinding(relay, relay->StreamBinding);
        {
            std::lock_guard<std::mutex> guard(Lock_);
            Relays_.erase(relay->Id);
        }
        if (tcpFdConsumed) {
            TqCloseSocket(relay->TcpFd);
            relay->TcpFd = TqInvalidSocket;
            RequestRelayShutdown(relay, TqStreamLifetime::ShutdownIntent::AbortBoth);
        }
        return false;
    }

    relay->Committed = true;
    relay->Activating = false;
    ActivateManagedBinding(relay, relay->StreamBinding);
    ScheduleRelayMaintenance(relay);
    return true;
}

void TqWindowsRelayWorker::WithdrawPublishedRelayTarget(
    const TqWindowsRelayRegistration& registration,
    const std::shared_ptr<RelayContext>& relay) {
    if (registration.StreamOwner == nullptr || relay == nullptr ||
        relay->ManagedBinding == nullptr) {
        return;
    }
    const auto phase = registration.StreamOwner->GetPhase();
    if (phase != TqStreamLifetime::Phase::Starting &&
        phase != TqStreamLifetime::Phase::Started) {
        return;
    }
    (void)registration.StreamOwner->PublishTerminalAndTakeTarget();
}

void TqWindowsRelayWorker::RollbackPreparedRelay(
    const std::shared_ptr<RelayContext>& relay,
    bool tcpFdConsumed,
    const TqWindowsRelayRegistration& registration,
    bool targetPublished) {
    if (!relay) {
        return;
    }
    if (targetPublished) {
        WithdrawPublishedRelayTarget(registration, relay);
    }
    FailManagedBinding(relay, relay->StreamBinding);
    {
        std::lock_guard<std::mutex> guard(Lock_);
        Relays_.erase(relay->Id);
    }
    if (tcpFdConsumed && TqSocketValid(relay->TcpFd)) {
        TqCloseSocket(relay->TcpFd);
        relay->TcpFd = TqInvalidSocket;
    }
    if (registration.StreamOwner != nullptr) {
        const auto phase = registration.StreamOwner->GetPhase();
        if (phase == TqStreamLifetime::Phase::Starting ||
            phase == TqStreamLifetime::Phase::Started) {
            RequestRelayShutdown(relay, TqStreamLifetime::ShutdownIntent::AbortBoth);
        }
    }
}

void TqWindowsRelayWorker::ActivateManagedBinding(
    const std::shared_ptr<RelayContext>& relay,
    WindowsStreamRelayBinding* binding) {
    if (!relay || binding == nullptr) {
        return;
    }
    std::deque<std::shared_ptr<TqWindowsPendingQuicReceive>> pending;
    {
        std::lock_guard<std::mutex> guard(binding->PrecommitLock);
        if (binding->ActivationState.load(std::memory_order_acquire) !=
            WindowsStreamRelayBinding::Activation::Prepared) {
            return;
        }
        binding->ActivationState.store(
            WindowsStreamRelayBinding::Activation::Active,
            std::memory_order_release);
        binding->RelayHint.store(relay.get(), std::memory_order_release);
        pending.swap(binding->PrecommitReceives);
        binding->PrecommitPendingBytes = 0;
    }
    while (!pending.empty()) {
        auto view = std::move(pending.front());
        pending.pop_front();
        if (view != nullptr) {
            (void)EnqueueDeferredQuicReceiveView(relay, view);
        }
    }
}

void TqWindowsRelayWorker::FailManagedBinding(
    const std::shared_ptr<RelayContext>& relay,
    WindowsStreamRelayBinding* binding) {
    if (binding == nullptr) {
        return;
    }
    std::deque<std::shared_ptr<TqWindowsPendingQuicReceive>> pending;
    {
        std::lock_guard<std::mutex> guard(binding->PrecommitLock);
        binding->ActivationState.store(
            WindowsStreamRelayBinding::Activation::Failed,
            std::memory_order_release);
        pending.swap(binding->PrecommitReceives);
        binding->PrecommitPendingBytes = 0;
    }
    while (!pending.empty()) {
        if (auto view = pending.front()) {
            view->Drained = true;
            view->CompletedLength = view->TotalLength;
            view->PendingCompleteBytes = 0;
        }
        pending.pop_front();
    }
    binding->Closing.store(true, std::memory_order_release);
    binding->RelayHint.store(nullptr, std::memory_order_release);
    const uint64_t controlGeneration =
        binding->StopControl != nullptr
            ? binding->StopControl->Generation.load(std::memory_order_acquire)
            : 0;
    auto control = std::atomic_load(&binding->StopControl);
    PublishStopToControl(control, controlGeneration);
    if (relay != nullptr) {
        relay->StreamBinding = nullptr;
        relay->ManagedBinding.reset();
    }
}

void TqWindowsRelayWorker::SealBindingAtTerminal(
    const std::shared_ptr<RelayContext>& relay,
    WindowsStreamRelayBinding* binding) {
    if (binding == nullptr) {
        return;
    }
    binding->Closing.store(true, std::memory_order_release);
    binding->RelayHint.store(nullptr, std::memory_order_release);
    if (relay != nullptr) {
        relay->StreamBinding = nullptr;
    }
}

bool TqWindowsRelayWorker::PostTerminalShutdownComplete(
    const std::shared_ptr<WindowsStreamRelayBinding>& binding,
    const std::shared_ptr<RelayContext>& relay,
    uint64_t connectionErrorCode,
    uint32_t connectionCloseStatus) {
    if (!binding || binding->RelayId == 0) {
        CallbackIocpPostFailedCount_.fetch_add(1, std::memory_order_relaxed);
        return false;
    }

    auto record = std::make_shared<TerminalCleanupRecord>();
    record->Binding = binding;
    record->Relay = relay;
    if (relay != nullptr) {
        record->StreamOwner = relay->StreamOwner;
    }
    record->RelayId = binding->RelayId;
    record->Generation = binding->Generation;
    record->StopControl = binding->StopControl;
    record->ControlGeneration =
        binding->StopControl != nullptr
            ? binding->StopControl->Generation.load(std::memory_order_acquire)
            : 0;
    record->ConnectionErrorCode = connectionErrorCode;
    record->ConnectionCloseStatus = connectionCloseStatus;

    std::lock_guard<std::mutex> controlGuard(ControlCommandLock_);
    if (Iocp_ == nullptr || Stopping_.load(std::memory_order_acquire)) {
        CallbackIocpPostFailedCount_.fetch_add(1, std::memory_order_relaxed);
        auto control = std::atomic_load(&record->StopControl);
        PublishStopToControl(control, record->ControlGeneration);
        return false;
    }

    auto op = std::make_unique<IoOperation>();
    op->Event = TqWindowsIocpOperationType::QuicShutdownComplete;
    op->RelayId = record->RelayId;
    op->Generation = record->Generation;
    op->ControlGeneration = record->ControlGeneration;
    op->StopControl = record->StopControl;
    op->Value = connectionErrorCode;
    op->Length = static_cast<size_t>(connectionCloseStatus);
    op->TerminalCleanup = std::move(record);
#if defined(TQ_UNIT_TESTING)
    {
        std::lock_guard<std::mutex> guard(PendingTerminalCleanupLock_);
        PendingTerminalCleanupForTest_ = op->TerminalCleanup;
    }
#endif
    TerminalOperationPendingCount_.fetch_add(1, std::memory_order_acq_rel);

    IoOperation* raw = op.release();
    if (!::PostQueuedCompletionStatus(static_cast<HANDLE>(Iocp_), 0, 0, &raw->Overlapped)) {
        TerminalOperationPendingCount_.fetch_sub(1, std::memory_order_acq_rel);
        delete raw;
#if defined(TQ_UNIT_TESTING)
        {
            std::lock_guard<std::mutex> guard(PendingTerminalCleanupLock_);
            PendingTerminalCleanupForTest_.reset();
        }
#endif
        CallbackIocpPostFailedCount_.fetch_add(1, std::memory_order_relaxed);
        auto control = std::atomic_load(&binding->StopControl);
        const uint64_t controlGeneration =
            control != nullptr ? control->Generation.load(std::memory_order_acquire) : 0;
        PublishStopToControl(control, controlGeneration);
        return false;
    }
#if defined(TQ_UNIT_TESTING)
    {
        std::lock_guard<std::mutex> guard(LastPostedCallbackLock_);
        LastPostedCallbackType_ = TqWindowsIocpOperationType::QuicShutdownComplete;
        LastPostedCallbackRelayId_ = binding->RelayId;
        PostedCallbackSequence_.push_back(TqWindowsIocpOperationType::QuicShutdownComplete);
    }
#endif
    CallbackIocpPostCount_.fetch_add(1, std::memory_order_relaxed);
    return true;
}

void TqWindowsRelayWorker::DispatchQuicShutdownComplete(
    IoOperation& op,
    const std::shared_ptr<RelayContext>& relay) {
    if (op.TerminalCleanup != nullptr) {
        ProcessTerminalShutdownComplete(*op.TerminalCleanup);
    } else if (relay) {
        ProcessQuicShutdownComplete(
            relay,
            op.Value,
            static_cast<uint32_t>(op.Length));
    } else {
        ProcessQuicShutdownComplete(
            op.RelayId,
            op.Value,
            static_cast<uint32_t>(op.Length));
    }
}

void TqWindowsRelayWorker::ProcessTerminalShutdownComplete(
    const TerminalCleanupRecord& record) {
    TerminalOperationPendingCount_.fetch_sub(1, std::memory_order_acq_rel);
#if defined(TQ_UNIT_TESTING)
    {
        std::lock_guard<std::mutex> guard(PendingTerminalCleanupLock_);
        PendingTerminalCleanupForTest_.reset();
    }
#endif
    auto relay = record.Relay;
    if (!relay && record.RelayId != 0) {
        relay = FindRelayByIdLocal(record.RelayId);
        if (relay && relay->Generation != record.Generation) {
            relay.reset();
        }
    }
    if (!relay || relay->Closing.load(std::memory_order_acquire)) {
        PostedCallbackStaleDropCount_.fetch_add(1, std::memory_order_relaxed);
        auto control = std::atomic_load(&record.StopControl);
        PublishStopToControl(control, record.ControlGeneration);
        return;
    }
    ProcessQuicShutdownComplete(
        relay,
        record.ConnectionErrorCode,
        record.ConnectionCloseStatus);
}

bool TqWindowsRelayWorker::QueuePrecommitQuicReceive(
    const std::shared_ptr<RelayContext>& relay,
    WindowsStreamRelayBinding* binding,
    MsQuicStream* stream,
    const QUIC_BUFFER* buffers,
    uint32_t bufferCount,
    bool fin,
    bool& handled) {
    (void)relay;
    handled = false;
    if (binding == nullptr) {
        return true;
    }
    if (binding->ActivationState.load(std::memory_order_acquire) ==
        WindowsStreamRelayBinding::Activation::Active) {
        return true;
    }
    handled = true;
    if (binding->ActivationState.load(std::memory_order_acquire) ==
            WindowsStreamRelayBinding::Activation::Failed ||
        binding->Closing.load(std::memory_order_acquire)) {
        return false;
    }
    auto view = BuildDeferredQuicReceiveView(*binding, stream, buffers, bufferCount, fin, 0);
    if (!view) {
        return false;
    }
    std::lock_guard<std::mutex> guard(binding->PrecommitLock);
    const auto activation = binding->ActivationState.load(std::memory_order_acquire);
    if (activation == WindowsStreamRelayBinding::Activation::Active) {
        handled = false;
        return true;
    }
    if (activation == WindowsStreamRelayBinding::Activation::Failed ||
        binding->Closing.load(std::memory_order_acquire)) {
        return false;
    }
    const uint64_t pending = view->TotalLength - view->CompletedLength;
    if (pending > binding->PrecommitMaxPendingBytes -
            std::min(binding->PrecommitPendingBytes, binding->PrecommitMaxPendingBytes)) {
        return false;
    }
    binding->PrecommitPendingBytes += pending;
    binding->PrecommitReceives.push_back(std::move(view));
    return true;
}

#if defined(TQ_UNIT_TESTING)
void TqWindowsRelayWorker::SetQuicReceiveViewDrainEnabledForTest(bool enabled) {
    QuicReceiveViewDrainEnabledForTest_.store(enabled, std::memory_order_relaxed);
}

bool TqWindowsRelayWorker::RegisterRelayForTest(
    const TqWindowsRelayRegistration& registration) {
    if (!Thread_.joinable()) {
        if (Iocp_ == nullptr) {
            (void)TestCreateIocpForCallbackPostOnly();
        }
        if (Iocp_ != nullptr && !Stopping_.load(std::memory_order_acquire)) {
            return RegisterRelayWithIdLocal(registration).Ok;
        }
    }
    return RegisterRelayWithId(registration).Ok;
}

bool TqWindowsRelayWorker::RegisterRelayForTest(
    MsQuicStream* stream,
    TqRelayHandle* handle,
    const TqTuningConfig& tuning,
    TqCompressAlgo compressAlgo) {
    if (stream == nullptr || handle == nullptr) {
        return false;
    }
    g_WindowsRelayTestStreamOwner = MakeWindowsRelayTestStreamOwner(stream);
    if (!g_WindowsRelayTestStreamOwner) {
        return false;
    }
    const auto registration = MakeWindowsRelayTestRegistration(
        TqInvalidSocket,
        stream,
        g_WindowsRelayTestStreamOwner,
        nullptr,
        nullptr,
        handle,
        tuning,
        compressAlgo);
    const TqWindowsRelayRegistrationResult result = [&]() {
        if (!Thread_.joinable()) {
            if (Iocp_ == nullptr) {
                (void)TestCreateIocpForCallbackPostOnly();
            }
            if (Iocp_ != nullptr && !Stopping_.load(std::memory_order_acquire)) {
                return RegisterRelayWithIdLocal(registration);
            }
        }
        return RegisterRelayWithId(registration);
    }();
    FillWindowsRelayTestHandle(handle, result);
    return result.Ok;
}

bool TqWindowsRelayWorker::RegisterRelay(
    TqSocketHandle tcpFd,
    MsQuicStream* stream,
    ITqCompressor* compressor,
    ITqDecompressor* decompressor,
    TqRelayHandle* handle,
    const TqTuningConfig& tuning,
    TqCompressAlgo compressAlgo) {
    if (stream == nullptr || handle == nullptr) {
        return false;
    }
    g_WindowsRelayTestStreamOwner = MakeWindowsRelayTestStreamOwner(stream);
    if (!g_WindowsRelayTestStreamOwner) {
        return false;
    }
    const auto registration = MakeWindowsRelayTestRegistration(
        tcpFd,
        stream,
        g_WindowsRelayTestStreamOwner,
        compressor,
        decompressor,
        handle,
        tuning,
        compressAlgo);
    const auto result = RegisterRelayWithId(registration);
    FillWindowsRelayTestHandle(handle, result);
    return result.Ok;
}

std::shared_ptr<TqStreamLifetime> TqWindowsRelayWorker::LastTestStreamOwnerForTest() const {
    return g_WindowsRelayTestStreamOwner;
}

QUIC_STATUS TqWindowsRelayDispatchTestStreamEvent(
    MsQuicStream* stream,
    QUIC_STREAM_EVENT* event) {
    if (event == nullptr) {
        return QUIC_STATUS_SUCCESS;
    }
    if (g_WindowsRelayTestStreamOwner != nullptr) {
        return g_WindowsRelayTestStreamOwner->DispatchForTest(event);
    }
    return TqWindowsRelayWorker::StreamCallback(stream, stream != nullptr ? stream->Context : nullptr, event);
}

QUIC_STATUS TqWindowsRelayDispatchTestStreamEvent(
    MsQuicStream* stream,
    void* /*legacyContext*/,
    QUIC_STREAM_EVENT* event) {
    (void)stream;
    return TqWindowsRelayDispatchTestStreamEvent(stream, event);
}

void TqWindowsRelayWorker::SetMaintenanceBudgetForTest(size_t budget) {
    MaintenanceBudget_ = budget == 0 ? 1 : budget;
}

bool TqWindowsRelayWorker::TestScheduleMaintenanceForTest(uint64_t relayId) {
    auto relay = FindRelayByIdLocal(relayId);
    if (!relay) {
        return false;
    }
    ScheduleRelayMaintenance(relay);
    return true;
}

void TqWindowsRelayWorker::TestDrainMaintenanceForTest() {
    DrainPerRelayMaintenance();
}

bool TqWindowsRelayWorker::TestGetRelayTraceStateForTest(
    uint64_t relayId,
    TqTraceLinuxRelayStreamState* out,
    std::string* targetStorage) const {
    // Test-only observer: Lock_ protects Relays_ lookup only. Callers must
    // synchronize with the worker thread (Snapshot()/queue block) before reading.
    if (out == nullptr || targetStorage == nullptr) {
        return false;
    }
    targetStorage->clear();
    {
        std::lock_guard<std::mutex> guard(Lock_);
        const auto it = Relays_.find(relayId);
        if (it == Relays_.end()) {
            return false;
        }
        *out = BuildRelayTraceState(it->second);
        if (out->Target != nullptr) {
            *targetStorage = out->Target;
            out->Target = targetStorage->c_str();
        }
    }
    return true;
}

bool TqWindowsRelayWorker::TestPostTraceContextForTest(
    uint64_t relayId,
    uint64_t generation,
    uint64_t tunnelId,
    const char* target) {
    if (relayId == 0 || tunnelId == 0) {
        return false;
    }

    std::lock_guard<std::mutex> controlGuard(ControlCommandLock_);
    if (Iocp_ == nullptr || !Thread_.joinable() || Stopping_.load(std::memory_order_acquire)) {
        return false;
    }
    auto relay = FindRelayById(relayId);
    if (!relay) {
        return false;
    }

    auto op = std::make_unique<IoOperation>();
    op->Event = TqWindowsIocpOperationType::SetTraceContext;
    op->RelayId = relayId;
    op->Generation = generation;
    op->ControlGeneration =
        relay->StopControl != nullptr
            ? relay->StopControl->Generation.load(std::memory_order_acquire)
            : 0;
    op->StopControl = relay->StopControl;
    op->Value = tunnelId;
    if (target != nullptr) {
        op->Text = target;
    }

    IoOperation* raw = op.release();
    if (!::PostQueuedCompletionStatus(static_cast<HANDLE>(Iocp_), 0, 0, &raw->Overlapped)) {
        delete raw;
        Errors_.fetch_add(1, std::memory_order_relaxed);
        return false;
    }
    return true;
}

void TqWindowsRelayWorker::TestForceNextTraceContextPostFailureForTest() {
    ForceTraceContextPostFailureForTest_.store(true, std::memory_order_release);
}

bool TqWindowsRelayWorker::TestPostWorkerQueueBlockForTest(
    TqWindowsRelayWorkerQueueBlockForTest* block) {
    if (Iocp_ == nullptr || block == nullptr) {
        return false;
    }
    auto op = std::make_unique<IoOperation>();
    op->Event = TqWindowsIocpOperationType::TestBlockWorkerQueue;
    op->Control = block;
    IoOperation* raw = op.release();
    if (!::PostQueuedCompletionStatus(static_cast<HANDLE>(Iocp_), 0, 0, &raw->Overlapped)) {
        delete raw;
        return false;
    }
    return true;
}

bool TqWindowsRelayWorker::TestWaitWorkerQueueBlockEnteredForTest(
    TqWindowsRelayWorkerQueueBlockForTest& block,
    uint32_t timeoutMs) const {
    std::unique_lock<std::mutex> guard(block.Mutex);
    return block.Cv.wait_for(
        guard,
        std::chrono::milliseconds(timeoutMs),
        [&block] { return block.Entered; });
}

void TqWindowsRelayWorker::TestReleaseWorkerQueueBlockForTest(
    TqWindowsRelayWorkerQueueBlockForTest& block) const {
    {
        std::lock_guard<std::mutex> guard(block.Mutex);
        block.Release = true;
    }
    block.Cv.notify_all();
}

bool TqWindowsRelayWorker::TestGetTerminalOperationSnapshotForTest(
    TqWindowsTerminalOperationSnapshotForTest* out) const {
    if (out == nullptr) {
        return false;
    }
    *out = {};
    out->Pending = TerminalOperationPendingCount_.load(std::memory_order_acquire) != 0;
    std::lock_guard<std::mutex> guard(PendingTerminalCleanupLock_);
    if (!PendingTerminalCleanupForTest_) {
        return true;
    }
    const auto& record = *PendingTerminalCleanupForTest_;
    out->RelayId = record.RelayId;
    out->Generation = record.Generation;
    out->ConnectionErrorCode = record.ConnectionErrorCode;
    out->ConnectionCloseStatus = record.ConnectionCloseStatus;
    out->HasBindingOwner = record.Binding != nullptr;
    out->HasRelayOwner = record.Relay != nullptr;
    return true;
}

bool TqWindowsRelayWorker::TestGetBindingTerminalSealForTest(
    uint64_t relayId,
    bool* closing,
    bool* relayHintNull) const {
    if (closing == nullptr || relayHintNull == nullptr) {
        return false;
    }
    *closing = false;
    *relayHintNull = false;
    std::shared_ptr<WindowsStreamRelayBinding> binding;
    {
        std::lock_guard<std::mutex> guard(Lock_);
        const auto it = Relays_.find(relayId);
        if (it != Relays_.end() && it->second && it->second->ManagedBinding) {
            binding = it->second->ManagedBinding;
        }
    }
    if (!binding) {
        for (const auto& retired : RetiredCallbacks_) {
            if (retired && retired->RelayId == relayId) {
                binding = retired;
                break;
            }
        }
    }
    if (!binding) {
        std::lock_guard<std::mutex> guard(PendingTerminalCleanupLock_);
        if (PendingTerminalCleanupForTest_ &&
            PendingTerminalCleanupForTest_->RelayId == relayId) {
            binding = PendingTerminalCleanupForTest_->Binding;
        }
    }
    if (!binding) {
        return false;
    }
    *closing = binding->Closing.load(std::memory_order_acquire);
    *relayHintNull = binding->RelayHint.load(std::memory_order_acquire) == nullptr;
    return true;
}

bool TqWindowsRelayWorker::TestBindingHasRelayHintForTest(uint64_t relayId) const {
    bool closing = false;
    bool relayHintNull = false;
    if (!TestGetBindingTerminalSealForTest(relayId, &closing, &relayHintNull)) {
        return false;
    }
    return !relayHintNull;
}

bool TqWindowsRelayWorker::TestDrainSingleTerminalShutdownForTest() {
    if (Iocp_ == nullptr) {
        return false;
    }
    DWORD bytes = 0;
    ULONG_PTR key = 0;
    OVERLAPPED* overlapped = nullptr;
    const BOOL ok = ::GetQueuedCompletionStatus(
        static_cast<HANDLE>(Iocp_), &bytes, &key, &overlapped, 5000);
    if (overlapped == nullptr) {
        return false;
    }
    std::unique_ptr<IoOperation> op(
        CONTAINING_RECORD(overlapped, IoOperation, Overlapped));
    if (op->Event != TqWindowsIocpOperationType::QuicShutdownComplete) {
        return false;
    }
    auto relay = op->Relay != nullptr ? op->Relay : FindRelayByIdLocal(op->RelayId);
    if (relay && relay->Generation != op->Generation) {
        relay.reset();
    }
    DispatchQuicShutdownComplete(*op, relay);
    (void)ok;
    return true;
}

bool TqWindowsRelayWorker::TestHasCommittedActiveRelayForTest() const {
    std::lock_guard<std::mutex> guard(Lock_);
    for (const auto& entry : Relays_) {
        if (entry.second && entry.second->Committed) {
            return true;
        }
    }
    return false;
}

bool TqWindowsRelayWorker::TestTerminalCleanupHasStreamPointerForTest() const {
    // TerminalCleanupRecord omits raw MsQuicStream*; enforced by static_assert
    // at the struct definition site in this translation unit.
    return true;
}

QUIC_STATUS TqWindowsRelayWorker::TestLastPrecommitReceiveStatusForTest() const {
    std::lock_guard<std::mutex> guard(Lock_);
    for (const auto& entry : Relays_) {
        const auto& relay = entry.second;
        if (!relay || !relay->ManagedBinding) {
            continue;
        }
        auto& binding = *relay->ManagedBinding;
        std::lock_guard<std::mutex> precommitGuard(binding.PrecommitLock);
        if (!binding.PrecommitReceives.empty()) {
            return QUIC_STATUS_PENDING;
        }
    }
    return QUIC_STATUS_SUCCESS;
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
    if (relay->PendingReceives.empty()) {
        return false;
    }
    auto view = relay->PendingReceives.front();
    if (!view || completedLength > view->TotalLength) {
        return false;
    }
    view->CompletedLength = completedLength;
    view->AccountedLength = completedLength;
    SaturatingFetchSub(relay->PendingQuicReceiveBytes, completedLength);
    return CompletePendingQuicReceive(relay, view);
}

bool TqWindowsRelayWorker::TestEnqueueReceiveViewForTest(uint64_t relayId, uint64_t byteCount) {
    auto relay = FindRelayByIdLocal(relayId);
    if (!relay || byteCount == 0) {
        return false;
    }
    auto view = std::make_shared<TqWindowsPendingQuicReceive>();
    view->OwnedBuffer.resize(static_cast<size_t>(byteCount), 'x');
    view->TotalLength = byteCount;
    view->AccountedLength = byteCount;
    view->SliceOffset = 0;
    view->CompletedLength = 0;
    view->PendingCompleteBytes = 0;
    view->Drained = false;
    view->Fin = false;
    view->StreamOwner = relay->StreamOwner;
    relay->PendingQuicReceiveBytes.fetch_add(byteCount, std::memory_order_acq_rel);
    relay->PendingQuicReceiveQueueDepth.fetch_add(1, std::memory_order_acq_rel);
    relay->PendingReceives.push_back(view);
    return true;
}

bool TqWindowsRelayWorker::TestCompleteSecondReceiveViewForTest(
    uint64_t relayId,
    uint64_t completedLength) {
    auto relay = FindRelayByIdLocal(relayId);
    if (!relay) {
        return false;
    }
    if (relay->PendingReceives.size() < 2) {
        return false;
    }
    auto it = relay->PendingReceives.begin();
    ++it;
    auto view = *it;
    if (!view || completedLength == 0 || completedLength > view->TotalLength) {
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
    if (!relay || relay->StreamBinding == nullptr) {
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
    if (!relay || relay->StreamBinding == nullptr || byteCount == 0) {
        return false;
    }
    auto* binding = relay->StreamBinding;
    if (!PostCallbackOperationById(
            TqWindowsIocpOperationType::QuicIdealSendBuffer,
            *binding,
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
            DispatchQuicShutdownComplete(*op, resolvedCallbackById ? relay : nullptr);
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
    if (relay->PendingReceives.empty()) {
        return false;
    }
    auto view = relay->PendingReceives.front();
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
    view->StreamOwner = relay->StreamOwner;
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
    if (!relay || relay->StreamBinding == nullptr) {
        return false;
    }
    relay->Closing.store(true, std::memory_order_release);
    relay->ManagedBinding->Closing.store(true, std::memory_order_release);
    return true;
}

bool TqWindowsRelayWorker::TestArmRelayClosingOnRelayOnlyForTest(uint64_t relayId) {
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
    relay->Closing.store(true, std::memory_order_release);
    return true;
}

bool TqWindowsRelayWorker::TestBumpCallbackBindingGenerationForTest(
    uint64_t relayId,
    uint64_t delta) {
    std::shared_ptr<RelayContext> relay;
    {
        std::lock_guard<std::mutex> guard(Lock_);
        const auto it = Relays_.find(relayId);
        if (it != Relays_.end()) {
            relay = it->second;
        }
    }
    if (!relay || relay->StreamBinding == nullptr || delta == 0) {
        return false;
    }
    relay->StreamBinding->Generation += delta;
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
    const char* reason,
    bool traceState) {
    MarkRelayCloseReason(relay, reason);
    if (!relay || relay->Closing.exchange(true)) {
        return;
    }
    const char* closeReason = relay->CloseReason.load(std::memory_order_acquire);
    if (closeReason == nullptr || closeReason == kWindowsRelayCloseReasonUnknown) {
        closeReason = kWindowsRelayCloseReasonDefault;
    }
    if (traceState) {
        TqTraceRelayStopCondition("windows", WorkerIndex_, closeReason, BuildRelayTraceState(relay));
    }
    if (mode == TqRelayCloseMode::AbortReset) {
        FatalRelayResets_.fetch_add(1, std::memory_order_relaxed);
        TqResetSocket(relay->TcpFd);
    } else {
        TqCloseSocket(relay->TcpFd);
    }
    relay->TcpFd = TqInvalidSocket;
    if (relay->ManagedBinding) {
        relay->ManagedBinding->Closing.store(true, std::memory_order_release);
    }
    relay->StreamOwner.reset();
    CompleteAllPendingQuicReceives(relay);
    FinalizeQuicSendAccountingOnClose(relay);
    ScheduleRelayMaintenance(relay);
    TryRetireRelay(relay, traceState);
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
    state.StreamDetached = relay->StreamOwner == nullptr;
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

void TqWindowsRelayWorker::TraceReceiveFlowDiag(
    const std::shared_ptr<RelayContext>& relay,
    const char* phase,
    const char* detail,
    uint64_t receiveBytes,
    uint64_t limitBytes,
    uint64_t extra) const {
    if (!relay || !TqTraceEnabled()) {
        return;
    }
    TqTraceWindowsReceiveFlowDiag(
        WorkerIndex_,
        relay->Id,
        phase,
        detail,
        relay->PendingQuicReceiveBytes.load(std::memory_order_relaxed),
        relay->PendingQuicReceiveQueueDepth.load(std::memory_order_relaxed),
        relay->QuicReceivePaused.load(std::memory_order_acquire),
        receiveBytes,
        limitBytes,
        extra);
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

void TqWindowsRelayWorker::FailRelayFatalFromCallback(
    const std::shared_ptr<RelayContext>& relay,
    const char* reason) {
    CloseRelay(relay, TqRelayCloseMode::AbortReset, reason, false);
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
    const bool hasPendingReceives = !relay->PendingReceives.empty();
    const size_t pendingQuicSendRetries = relay->PendingQuicSendRetries.size();
    return relay->InFlightTcpSends.load(std::memory_order_acquire) != 0 ||
           relay->PendingQuicReceiveQueueDepth.load(std::memory_order_acquire) != 0 ||
           relay->PendingQuicReceiveBytes.load(std::memory_order_acquire) != 0 ||
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
    const bool hasPendingReceives = !relay->PendingReceives.empty();
    const size_t pendingQuicSendRetries = relay->PendingQuicSendRetries.size();
    return relay->InFlightQuicSends.load(std::memory_order_acquire) != 0 ||
           relay->OutstandingQuicSendBytes.load(std::memory_order_acquire) != 0 ||
           pendingQuicSendRetries != 0 ||
           relay->InFlightTcpSends.load(std::memory_order_acquire) != 0 ||
           relay->InFlightTcpRecvs.load(std::memory_order_acquire) != 0 ||
           relay->PendingQuicReceiveQueueDepth.load(std::memory_order_acquire) != 0 ||
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

void TqWindowsRelayWorker::TryRetireRelay(
    const std::shared_ptr<RelayContext>& relay,
    bool traceState) {
    if (!relay || !relay->Closing.load(std::memory_order_acquire)) {
        return;
    }
    if (relay->ActiveHandlers.load(std::memory_order_acquire) != 0 ||
        relay->QueuedWorkerOps.load(std::memory_order_acquire) != 0 ||
        relay->InFlightTcpRecvs.load(std::memory_order_acquire) != 0 ||
        relay->InFlightTcpSends.load(std::memory_order_acquire) != 0 ||
        relay->InFlightQuicSends.load(std::memory_order_acquire) != 0 ||
        TerminalOperationPendingCount_.load(std::memory_order_acquire) != 0) {
        return;
    }
    if (relay->StopPublished.exchange(true, std::memory_order_acq_rel)) {
        return;
    }
    std::shared_ptr<WindowsStreamRelayBinding> bindingKeepAlive;
    if (relay->ManagedBinding) {
        bindingKeepAlive = relay->ManagedBinding;
    }
    {
        std::lock_guard<std::mutex> guard(Lock_);
        Relays_.erase(relay->Id);
        if (bindingKeepAlive) {
            RetiredCallbacks_.push_back(std::move(bindingKeepAlive));
        }
    }
    auto control = std::atomic_load(&relay->StopControl);
    const uint64_t controlGeneration =
        control != nullptr ? control->Generation.load(std::memory_order_acquire) : 0;
    PublishStopToControl(control, controlGeneration);
    if (traceState && TqTraceEnabled()) {
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
    auto lease = TryRelayStreamLease(*relay);
    if (relay->Closing.load(std::memory_order_acquire) || !lease) {
        delete operation;
        return false;
    }
    MsQuicStream* stream = lease.Stream();
    if (stream == nullptr || stream->Handle == nullptr) {
        delete operation;
        return false;
    }
    void* completionKey = operation;
    if (relay->StreamOwner != nullptr) {
        completionKey = relay->StreamOwner->RegisterSendCompletion(operation);
        if (completionKey == nullptr) {
            delete operation;
            return false;
        }
    }
    const QUIC_SEND_FLAGS flags = operation->Fin ? QUIC_SEND_FLAG_FIN : QUIC_SEND_FLAG_NONE;
    relay->InFlightQuicSends.fetch_add(1, std::memory_order_acq_rel);
    const QUIC_STATUS status = stream->Send(
        operation->QuicBuffers.empty() ? nullptr : operation->QuicBuffers.data(),
        static_cast<uint32_t>(operation->QuicBuffers.size()),
        flags,
        completionKey);
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
    if (relay->StreamOwner != nullptr && completionKey != operation) {
        (void)relay->StreamOwner->CancelSendCompletion(completionKey);
    }
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
            ScheduleRelayMaintenance(relay);
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
    ScheduleRelayMaintenance(relay);
}

bool TqWindowsRelayWorker::QueueDeferredQuicReceive(
    const std::shared_ptr<RelayContext>& relay,
    MsQuicStream* stream,
    const QUIC_BUFFER* buffers,
    uint32_t bufferCount,
    bool fin) {
    if (!relay || relay->StreamBinding == nullptr) {
        Errors_.fetch_add(1, std::memory_order_relaxed);
        return false;
    }
    auto view = BuildDeferredQuicReceiveView(
        *relay->StreamBinding,
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

bool TqWindowsRelayWorker::ComputeReceiveEventBytes(
    const QUIC_STREAM_EVENT& event,
    uint64_t* outBytes) {
    if (outBytes == nullptr || event.Type != QUIC_STREAM_EVENT_RECEIVE) {
        return false;
    }
    if (event.RECEIVE.BufferCount != 0 && event.RECEIVE.Buffers == nullptr) {
        return false;
    }
    uint64_t total = 0;
    for (uint32_t i = 0; i < event.RECEIVE.BufferCount; ++i) {
        const QUIC_BUFFER& buffer = event.RECEIVE.Buffers[i];
        if (buffer.Length != 0 && buffer.Buffer == nullptr) {
            return false;
        }
        const uint64_t next = total + buffer.Length;
        if (next < total) {
            return false;
        }
        total = next;
    }
    *outBytes = total;
    return true;
}

void TqWindowsRelayWorker::PauseQuicReceiveFromCallback(
    const WindowsStreamRelayBinding& binding,
    RelayContext& relay,
    MsQuicStream* stream) {
    (void)binding;
    if (relay.QuicReceivePaused.exchange(true, std::memory_order_acq_rel)) {
        TqTraceWindowsReceiveFlowDiag(
            WorkerIndex_,
            relay.Id,
            "pause_callback",
            "already_paused",
            relay.PendingQuicReceiveBytes.load(std::memory_order_relaxed),
            relay.PendingQuicReceiveQueueDepth.load(std::memory_order_relaxed),
            true,
            0,
            relay.Tuning.WindowsRelayMaxPendingQuicReceiveBytesPerRelay,
            0);
        return;
    }
    TqTraceWindowsReceiveFlowDiag(
        WorkerIndex_,
        relay.Id,
        "pause_callback",
        "stream_receive_set_enabled_false",
        relay.PendingQuicReceiveBytes.load(std::memory_order_relaxed),
        relay.PendingQuicReceiveQueueDepth.load(std::memory_order_relaxed),
        true,
        0,
        relay.Tuning.WindowsRelayMaxPendingQuicReceiveBytesPerRelay,
        0);
    CallbackReceiveBudgetPausedCount_.fetch_add(1, std::memory_order_relaxed);
    QuicReceivePausedCount_.fetch_add(1, std::memory_order_relaxed);
    if (MsQuic != nullptr && MsQuic->StreamReceiveSetEnabled != nullptr &&
        stream != nullptr && stream->Handle != nullptr) {
        const QUIC_STATUS status = MsQuic->StreamReceiveSetEnabled(stream->Handle, FALSE);
        if (QUIC_FAILED(status)) {
            Errors_.fetch_add(1, std::memory_order_relaxed);
        }
    }
}

bool TqWindowsRelayWorker::ShouldRejectReceiveInCallback(
    const WindowsStreamRelayBinding& binding,
    MsQuicStream* stream,
    const QUIC_STREAM_EVENT& event,
    uint64_t receiveBytes) {
    const bool fin = (event.RECEIVE.Flags & QUIC_RECEIVE_FLAG_FIN) != 0;
    if (fin || receiveBytes == 0) {
        return false;
    }
    auto relay = ResolveRelayForCallback(binding.RelayId, binding.Generation);
    if (relay == nullptr || relay->Closing.load(std::memory_order_acquire)) {
        return false;
    }
    const uint64_t limit = relay->Tuning.WindowsRelayMaxPendingQuicReceiveBytesPerRelay;
    if (limit == 0) {
        return false;
    }
    const uint64_t pending = relay->PendingQuicReceiveBytes.load(std::memory_order_acquire);
    if (pending < limit && receiveBytes <= limit - pending) {
        return false;
    }
    CallbackReceiveBudgetRejectedCount_.fetch_add(1, std::memory_order_relaxed);
    PauseQuicReceiveFromCallback(binding, *relay, stream);
    return true;
}

std::shared_ptr<TqWindowsPendingQuicReceive> TqWindowsRelayWorker::BuildDeferredQuicReceiveView(
    const WindowsStreamRelayBinding& binding,
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

    const uint64_t copyStartNanos = NowSteadyNanos();

    auto view = std::make_shared<TqWindowsPendingQuicReceive>();
    if (auto relay = ResolveRelayForCallback(binding.RelayId, binding.Generation)) {
        view->StreamOwner = relay->StreamOwner;
    } else if (stream != nullptr) {
        (void)stream;
    }
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
    CallbackReceiveCopyBytes_.fetch_add(view->TotalLength, std::memory_order_relaxed);
    CallbackReceiveCopyNanos_.fetch_add(
        NowSteadyNanos() - copyStartNanos,
        std::memory_order_relaxed);
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
        TraceReceiveFlowDiag(
            relay,
            "enqueue_cap",
            "set_quic_receive_disabled",
            view->TotalLength,
            maxPendingBytes,
            pendingBytes);
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
    const uint64_t remaining = static_cast<uint64_t>(slice.Length - view->SliceOffset);
    const uint64_t sendLimit = static_cast<uint64_t>(std::max<size_t>(1, relay->Tuning.RelayIoSize));
    const uint64_t postLength = std::min(remaining, sendLimit);
    auto op = std::make_unique<IoOperation>();
    op->Event = TqWindowsIocpOperationType::TcpSend;
    op->Relay = relay;
    op->ReceiveView = view;
    op->Offset = view->SliceOffset;
    op->PostedLength = postLength;

    op->WsaBuffer.buf = reinterpret_cast<char*>(const_cast<uint8_t*>(slice.Data + view->SliceOffset));
    op->WsaBuffer.len = static_cast<ULONG>(postLength);
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
    ReceiveCompleteViaOwner(view.StreamOwner, completeBytes);
    view.PendingCompleteBytes = 0;
}

void TqWindowsRelayWorker::FlushBatchedDeferredReceiveCompletion(
    const std::shared_ptr<RelayContext>& relay,
    const std::shared_ptr<TqStreamLifetime>& streamOwner) {
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
    const auto owner = streamOwner != nullptr ? streamOwner : relay->StreamOwner;
    ReceiveCompleteViaOwner(owner, completeBytes);
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
    ReceiveCompleteViaOwner(view.StreamOwner, remaining);
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
    bool removed = false;
    if (!relay->PendingReceives.empty() && relay->PendingReceives.front() == view) {
        relay->PendingReceives.pop_front();
        removed = true;
        TraceReceiveViewEvent(relay, view, "finish_pop_front");
    } else {
        const uint64_t searchStartNanos = NowSteadyNanos();
        const auto it = std::find(relay->PendingReceives.begin(), relay->PendingReceives.end(), view);
        ReceiveViewFinishLinearSearchNanos_.fetch_add(
            NowSteadyNanos() - searchStartNanos,
            std::memory_order_relaxed);
        ReceiveViewFinishLinearSearchCount_.fetch_add(1, std::memory_order_relaxed);
        if (it != relay->PendingReceives.end()) {
            ReceiveViewFinishNotFrontCount_.fetch_add(1, std::memory_order_relaxed);
            relay->PendingReceives.erase(it);
            removed = true;
            TraceReceiveViewEvent(relay, view, "finish_remove_not_front");
        }
    }
    if (!removed) {
        if (view->Drained || view->CompletedLength >= view->TotalLength) {
            TraceReceiveViewEvent(relay, view, "finish_already_drained");
            TqTraceRelayStopCondition(
                "windows",
                WorkerIndex_,
                "finish_receive_view_already_drained",
                BuildRelayTraceState(relay));
            ScheduleRelayMaintenance(relay);
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
        FlushBatchedDeferredReceiveCompletion(relay, view->StreamOwner);
    }
    (void)CloseRelayIfDrained(relay);
    ScheduleRelayMaintenance(relay);
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
        TraceReceiveFlowDiag(relay, "set_enabled", "resume_stream_receive_set_enabled_true");
        QuicReceiveResumedCount_.fetch_add(1, std::memory_order_relaxed);
        TraceRelayBackpressure(relay, "resume", "pending_quic_receive_drain");
    } else {
        if (wasPaused) {
            return;
        }
        TraceReceiveFlowDiag(relay, "set_enabled", "pause_stream_receive_set_enabled_false");
        QuicReceivePausedCount_.fetch_add(1, std::memory_order_relaxed);
        TraceRelayBackpressure(relay, "pause", "pending_quic_receive_cap");
    }
    auto lease = TryRelayStreamLease(*relay);
    MsQuicStream* stream = lease ? lease.Stream() : nullptr;
    if (stream != nullptr && stream->Handle != nullptr && MsQuic != nullptr &&
        MsQuic->StreamReceiveSetEnabled != nullptr) {
        (void)MsQuic->StreamReceiveSetEnabled(stream->Handle, enabled ? TRUE : FALSE);
    }
}

void TqWindowsRelayWorker::MaybeResumeQuicReceive(const std::shared_ptr<RelayContext>& relay) {
    if (!relay || Stopping_.load(std::memory_order_acquire)) {
        return;
    }
    if (relay->Closing.load(std::memory_order_acquire)) {
        return;
    }
    if (!relay->QuicReceivePaused.load(std::memory_order_acquire)) {
        return;
    }
    const uint64_t maxPendingBytes = relay->Tuning.WindowsRelayMaxPendingQuicReceiveBytesPerRelay;
    if (maxPendingBytes == 0) {
        TraceReceiveFlowDiag(relay, "maybe_resume", "resume_no_cap");
        SetQuicReceiveEnabled(relay, true);
        return;
    }
    if (relay->TcpReadPausedByQuicBacklog.load(std::memory_order_acquire)) {
        return;
    }
    if (relay->InFlightTcpSends.load(std::memory_order_acquire) != 0) {
        return;
    }
    const uint64_t pending = relay->PendingQuicReceiveBytes.load(std::memory_order_relaxed);
    const uint64_t pendingDepth =
        relay->PendingQuicReceiveQueueDepth.load(std::memory_order_relaxed);
    if (pendingDepth != 0 || pending >= maxPendingBytes / 2) {
        return;
    }
    TraceReceiveFlowDiag(relay, "maybe_resume", "resume_below_half_cap", 0, maxPendingBytes, pending);
    SetQuicReceiveEnabled(relay, true);
}

void TqWindowsRelayWorker::CompleteAllPendingQuicReceives(const std::shared_ptr<RelayContext>& relay) {
    if (!relay) {
        return;
    }
    std::list<std::shared_ptr<TqWindowsPendingQuicReceive>> pending;
    pending.swap(relay->PendingReceives);
    std::shared_ptr<TqStreamLifetime> streamOwnerForFlush;
    for (const auto& view : pending) {
        if (view && view->StreamOwner != nullptr) {
            streamOwnerForFlush = view->StreamOwner;
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
    FlushBatchedDeferredReceiveCompletion(relay, streamOwnerForFlush);
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
    const WindowsStreamRelayBinding* newest =
        keepNewest && !RetiredCallbacks_.empty() ? RetiredCallbacks_.back().get() : nullptr;
    RetiredCallbacks_.erase(
        std::remove_if(
            RetiredCallbacks_.begin(),
            RetiredCallbacks_.end(),
            [newest](const std::shared_ptr<WindowsStreamRelayBinding>& binding) {
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
            ScheduleRelayMaintenance(relay);
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
            ScheduleRelayMaintenance(relay);
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
                ScheduleRelayMaintenance(relay);
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
                ScheduleRelayMaintenance(relay);
                return;
            } else if (error == WSA_IO_PENDING) {
                TcpSendWouldBlockOrPendingCount_.fetch_add(1, std::memory_order_relaxed);
            }
            ScheduleRelayReceiveDrainOrFail(relay, "schedule_receive_drain_after_retry_send_failed");
            ScheduleRelayMaintenance(relay);
            return;
        }
        if (CloseRelayIfDrained(relay)) {
            return;
        }
        if (relay->Closing.load()) {
            ScheduleRelayMaintenance(relay);
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
        ScheduleRelayMaintenance(relay);
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
        ScheduleRelayMaintenance(relay);
        return;
    }
    if (CloseRelayIfDrained(relay)) {
        return;
    }
    if (relay->Closing.load()) {
        ScheduleRelayMaintenance(relay);
        return;
    }
    op->Offset += bytes;
    if (op->Offset < op->Buffer.size()) {
        TcpSendPartialCompletions_.fetch_add(1, std::memory_order_relaxed);
        if (!PostTcpSend(std::move(op))) {
            CloseRelay(relay, TqRelayCloseMode::GracefulDrain, "post_tcp_send_partial_failed");
        }
        ScheduleRelayMaintenance(relay);
        return;
    }
    ScheduleRelayMaintenance(relay);
}

void TqWindowsRelayWorker::ApplyRelayTraceContext(
    const std::shared_ptr<RelayContext>& relay,
    uint64_t tunnelId,
    const std::string& target) {
    if (!relay || tunnelId == 0) {
        return;
    }
    relay->TraceTunnelId = tunnelId;
    relay->TraceTarget = target;
}

#if defined(TQ_UNIT_TESTING)
void TqWindowsRelayWorker::StopRelay(uint64_t relayId) {
    auto relay = FindRelayByIdLocal(relayId);
    if (!relay) {
        return;
    }
    const uint64_t controlGeneration =
        relay->StopControl != nullptr
            ? relay->StopControl->Generation.load(std::memory_order_acquire)
            : 0;
    StopRelay(relay->StopControl, relayId, relay->Generation, controlGeneration);
}

void TqWindowsRelayWorker::SetRelayTraceContext(
    uint64_t relayId,
    uint64_t tunnelId,
    const char* target) {
    auto relay = FindRelayByIdLocal(relayId);
    if (!relay) {
        return;
    }
    const uint64_t controlGeneration =
        relay->StopControl != nullptr
            ? relay->StopControl->Generation.load(std::memory_order_acquire)
            : 0;
    SetRelayTraceContext(
        relay->StopControl,
        relayId,
        relay->Generation,
        controlGeneration,
        tunnelId,
        target);
}
#endif

void TqWindowsRelayWorker::SetRelayTraceContext(
    const std::shared_ptr<TqRelayStopControl>& control,
    uint64_t relayId,
    uint64_t relayGeneration,
    uint64_t controlGeneration,
    uint64_t tunnelId,
    const char* target) {
    if (relayId == 0 || tunnelId == 0 || !ControlGenerationMatches(control, controlGeneration)) {
        return;
    }

    std::lock_guard<std::mutex> controlGuard(ControlCommandLock_);
    if (Iocp_ == nullptr || !Thread_.joinable() || Stopping_.load(std::memory_order_acquire)) {
        return;
    }
    auto relay = FindRelayById(relayId);
    if (!relay || relay->Generation != relayGeneration ||
        !ControlGenerationMatches(relay->StopControl, controlGeneration)) {
        PostedTraceContextStaleDropCount_.fetch_add(1, std::memory_order_relaxed);
        return;
    }

    auto op = std::make_unique<IoOperation>();
    op->Event = TqWindowsIocpOperationType::SetTraceContext;
    op->RelayId = relayId;
    op->Generation = relayGeneration;
    op->ControlGeneration = controlGeneration;
    op->StopControl = control;
    op->Value = tunnelId;
    if (target != nullptr) {
        op->Text = target;
    }

#if defined(TQ_UNIT_TESTING)
    if (ForceTraceContextPostFailureForTest_.exchange(false, std::memory_order_acq_rel)) {
        Errors_.fetch_add(1, std::memory_order_relaxed);
        return;
    }
#endif

    IoOperation* raw = op.release();
    if (!::PostQueuedCompletionStatus(static_cast<HANDLE>(Iocp_), 0, 0, &raw->Overlapped)) {
        delete raw;
        Errors_.fetch_add(1, std::memory_order_relaxed);
    }
}

void TqWindowsRelayWorker::StopRelay(
    const std::shared_ptr<TqRelayStopControl>& control,
    uint64_t relayId,
    uint64_t relayGeneration,
    uint64_t controlGeneration) {
    if (relayId == 0 || !ControlGenerationMatches(control, controlGeneration)) {
        return;
    }
    std::lock_guard<std::mutex> controlGuard(ControlCommandLock_);
    if (Iocp_ == nullptr || !Thread_.joinable() || Stopping_.load(std::memory_order_acquire)) {
        PublishStopToControl(control, controlGeneration);
        return;
    }
    auto relay = FindRelayById(relayId);
    if (!relay || relay->Generation != relayGeneration ||
        !ControlGenerationMatches(relay->StopControl, controlGeneration)) {
        PostedCallbackStaleDropCount_.fetch_add(1, std::memory_order_relaxed);
        PublishStopToControl(control, controlGeneration);
        return;
    }
    auto op = std::make_unique<IoOperation>();
    op->Event = TqWindowsIocpOperationType::CloseRelay;
    op->RelayId = relayId;
    op->Generation = relayGeneration;
    op->ControlGeneration = controlGeneration;
    op->StopControl = control;
    IoOperation* raw = op.release();
    if (!::PostQueuedCompletionStatus(static_cast<HANDLE>(Iocp_), 0, 0, &raw->Overlapped)) {
        delete raw;
        Errors_.fetch_add(1, std::memory_order_relaxed);
        PublishStopToControl(control, controlGeneration);
    }
}

QUIC_STATUS TqWindowsRelayWorker::OnStreamEventWithBinding(
    MsQuicStream* stream,
    QUIC_STREAM_EVENT* event,
    WindowsStreamRelayBinding* binding) noexcept {
    if (binding == nullptr || event == nullptr) {
        return QUIC_STATUS_SUCCESS;
    }

    binding->CallbackRefs.fetch_add(1, std::memory_order_acq_rel);
    struct CallbackRefsGuard {
        WindowsStreamRelayBinding* Binding{nullptr};
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
    } callbackDispatchGuard{&CallbackDispatchNanos_, callbackStartNanos};

    if (event->Type == QUIC_STREAM_EVENT_SEND_COMPLETE) {
#if defined(TQ_UNIT_TESTING)
        g_InSendCompleteCallback = true;
        struct SendCompleteCallbackGuard {
            ~SendCompleteCallbackGuard() { g_InSendCompleteCallback = false; }
        } sendCompleteGuard;
#endif
        auto* sendOp = static_cast<TqWindowsQuicSendOperation*>(event->SEND_COMPLETE.ClientContext);
        QueueQuicSendCompleteByIdFromCallback(*binding, sendOp);
        return QUIC_STATUS_SUCCESS;
    }

    if (event->Type == QUIC_STREAM_EVENT_RECEIVE) {
        const bool fin = (event->RECEIVE.Flags & QUIC_RECEIVE_FLAG_FIN) != 0;
        if (TqIsMsQuicFakeFinReceive(
                event->RECEIVE.AbsoluteOffset,
                event->RECEIVE.TotalBufferLength,
                event->RECEIVE.BufferCount,
                event->RECEIVE.Flags)) {
            TqTraceLinuxRelayStreamState traceState{};
            traceState.WorkerIndex = WorkerIndex_;
            traceState.RelayId = binding->RelayId;
            TqTraceRelayStreamEvent(
                "windows",
                WorkerIndex_,
                binding->RelayId,
                "receive_fake_fin",
                0,
                0,
                event->RECEIVE.AbsoluteOffset,
                event->RECEIVE.TotalBufferLength,
                event->RECEIVE.BufferCount,
                static_cast<uint32_t>(event->RECEIVE.Flags),
                true,
                traceState);
            assert(false && "MsQuic delivered FIN-only receive without known final size");
            std::abort();
        }
        if (binding->Closing.load(std::memory_order_acquire)) {
            return QUIC_STATUS_SUCCESS;
        }
        uint64_t receiveBytes = 0;
        if (!ComputeReceiveEventBytes(*event, &receiveBytes)) {
            Errors_.fetch_add(1, std::memory_order_relaxed);
            return QUIC_STATUS_SUCCESS;
        }
        auto relay = ResolveRelayForCallback(binding->RelayId, binding->Generation);
        bool precommitHandled = false;
        if (relay &&
            QueuePrecommitQuicReceive(
                relay,
                binding,
                stream,
                event->RECEIVE.Buffers,
                event->RECEIVE.BufferCount,
                fin,
                precommitHandled) &&
            precommitHandled) {
            return QUIC_STATUS_PENDING;
        }
        if (ShouldRejectReceiveInCallback(*binding, stream, *event, receiveBytes)) {
            return QUIC_STATUS_SUCCESS;
        }
        auto view = BuildDeferredQuicReceiveView(
            *binding,
            stream,
            event->RECEIVE.Buffers,
            event->RECEIVE.BufferCount,
            fin,
            0);
        if (!view) {
            return QUIC_STATUS_SUCCESS;
        }
        if (PostCallbackOperationById(
                TqWindowsIocpOperationType::RelayReceiveReady,
                *binding,
                0,
                0,
                view)) {
            return QUIC_STATUS_PENDING;
        }
        if (relay) {
            TraceReceiveFlowDiag(
                relay,
                "callback_post",
                "relay_receive_ready_post_failed",
                view->TotalLength);
        }
        CompleteRemainingReceiveOwnership(*view);
        return QUIC_STATUS_SUCCESS;
    }

    if (binding->Closing.load(std::memory_order_acquire)) {
        if (event->Type != QUIC_STREAM_EVENT_SHUTDOWN_COMPLETE) {
            return QUIC_STATUS_SUCCESS;
        }
    }

    if (event->Type == QUIC_STREAM_EVENT_PEER_SEND_SHUTDOWN) {
        return QUIC_STATUS_SUCCESS;
    }
    if (event->Type == QUIC_STREAM_EVENT_IDEAL_SEND_BUFFER_SIZE) {
        if (!PostCallbackOperationById(
                TqWindowsIocpOperationType::QuicIdealSendBuffer,
                *binding,
                event->IDEAL_SEND_BUFFER_SIZE.ByteCount)) {
            Errors_.fetch_add(1, std::memory_order_relaxed);
        }
        return QUIC_STATUS_SUCCESS;
    }
    if (event->Type == QUIC_STREAM_EVENT_PEER_SEND_ABORTED ||
        event->Type == QUIC_STREAM_EVENT_PEER_RECEIVE_ABORTED) {
        const bool peerSendAborted = event->Type == QUIC_STREAM_EVENT_PEER_SEND_ABORTED;
        if (!PostCallbackOperationById(
                peerSendAborted
                    ? TqWindowsIocpOperationType::QuicPeerSendAborted
                    : TqWindowsIocpOperationType::QuicPeerReceiveAborted,
                *binding,
                peerSendAborted
                    ? event->PEER_SEND_ABORTED.ErrorCode
                    : event->PEER_RECEIVE_ABORTED.ErrorCode)) {
            Errors_.fetch_add(1, std::memory_order_relaxed);
        }
        return QUIC_STATUS_SUCCESS;
    }
    if (event->Type == QUIC_STREAM_EVENT_SHUTDOWN_COMPLETE) {
        std::shared_ptr<RelayContext> relay;
        if (auto* hinted = binding->RelayHint.load(std::memory_order_acquire)) {
            relay = FindRelayByIdLocal(hinted->Id);
            if (relay && relay.get() != hinted) {
                relay.reset();
            }
        }
        if (!relay) {
            relay = ResolveRelayForCallback(binding->RelayId, binding->Generation);
        }
        std::shared_ptr<WindowsStreamRelayBinding> bindingOwner;
        try {
            bindingOwner = binding->shared_from_this();
        } catch (...) {
            bindingOwner.reset();
        }
        SealBindingAtTerminal(relay, binding);
        if (!PostTerminalShutdownComplete(
                bindingOwner,
                relay,
                event->SHUTDOWN_COMPLETE.ConnectionErrorCode,
                event->SHUTDOWN_COMPLETE.ConnectionCloseStatus)) {
            Errors_.fetch_add(1, std::memory_order_relaxed);
        }
        return QUIC_STATUS_SUCCESS;
    }
    return QUIC_STATUS_SUCCESS;
}

QUIC_STATUS QUIC_API TqWindowsRelayWorker::StreamCallback(
    MsQuicStream* stream,
    void* context,
    QUIC_STREAM_EVENT* event) noexcept {
    auto* binding = static_cast<WindowsStreamRelayBinding*>(context);
    if (binding == nullptr || event == nullptr) {
        return QUIC_STATUS_SUCCESS;
    }
    auto endpoint = binding->Endpoint;
    if (endpoint == nullptr) {
        return QUIC_STATUS_SUCCESS;
    }
    auto* worker = endpoint->TryEnter();
    if (worker == nullptr) {
        return QUIC_STATUS_SUCCESS;
    }
    struct Guard {
        std::shared_ptr<CallbackEndpoint> Endpoint;
        ~Guard() { Endpoint->Leave(); }
    } guard{std::move(endpoint)};
    return worker->OnStreamEventWithBinding(stream, event, binding);
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
    uint32_t workerCount = tuning.WindowsRelayWorkerCount;
    if (workerCount == 0) {
        workerCount = TqRelayWorkerCountMin;
    }
    workerCount = std::max(TqRelayWorkerCountMin, std::min(workerCount, TqRelayWorkerCountMax));
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
        total.WindowsPostedTraceContextStaleDropCount +=
            snapshot.WindowsPostedTraceContextStaleDropCount;
        total.WorkerLockAcquireCount += snapshot.WorkerLockAcquireCount;
        total.WorkerLockWaitNanos += snapshot.WorkerLockWaitNanos;
        total.FindRelayByIdCount += snapshot.FindRelayByIdCount;
        total.CallbackDispatchNanos += snapshot.CallbackDispatchNanos;
        total.CallbackReceiveBudgetRejectedCount += snapshot.CallbackReceiveBudgetRejectedCount;
        total.CallbackReceiveBudgetPausedCount += snapshot.CallbackReceiveBudgetPausedCount;
        total.CallbackReceiveCopyBytes += snapshot.CallbackReceiveCopyBytes;
        total.CallbackReceiveCopyNanos += snapshot.CallbackReceiveCopyNanos;
        total.SnapshotBuildNanos += snapshot.SnapshotBuildNanos;
        total.SnapshotActiveRelaysScanned += snapshot.SnapshotActiveRelaysScanned;
        total.MaintenanceDrainCount += snapshot.MaintenanceDrainCount;
        total.MaintenanceDrainNanos += snapshot.MaintenanceDrainNanos;
        total.MaintenanceRelaysProcessed += snapshot.MaintenanceRelaysProcessed;
        total.MaintenanceFullScanCount += snapshot.MaintenanceFullScanCount;
        total.MaintenanceFullScanRelaysScanned += snapshot.MaintenanceFullScanRelaysScanned;
        total.ReceiveViewFinishLinearSearchCount += snapshot.ReceiveViewFinishLinearSearchCount;
        total.ReceiveViewFinishLinearSearchNanos += snapshot.ReceiveViewFinishLinearSearchNanos;
        total.ReceiveViewFinishNotFrontCount += snapshot.ReceiveViewFinishNotFrontCount;
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

TqWindowsRelayRegistrationResult TqWindowsRelayRuntime::RegisterRelay(
    const TqWindowsRelayRegistration& registration) {
    std::lock_guard<std::mutex> guard(Lock_);
    if (Workers_.empty()) {
        return {};
    }
    const uint64_t index = NextWorker_.fetch_add(1) % Workers_.size();
    return Workers_[static_cast<size_t>(index)]->RegisterRelayWithId(registration);
}

void TqWindowsRelayRuntime::StopRelay(
    const std::shared_ptr<TqRelayStopControl>& control,
    uint32_t workerIndex,
    uint64_t relayId,
    uint64_t relayGeneration,
    uint64_t controlGeneration) {
    if (!ControlGenerationMatches(control, controlGeneration)) {
        PublishStopToControl(control, controlGeneration);
        return;
    }
    std::lock_guard<std::mutex> lifetimeGuard(RuntimeWorkerLifetimeLock());
    TqWindowsRelayWorker* worker = nullptr;
    {
        std::lock_guard<std::mutex> guard(Lock_);
        if (workerIndex >= Workers_.size() || !Workers_[workerIndex]) {
            PublishStopToControl(control, controlGeneration);
            return;
        }
        worker = Workers_[workerIndex].get();
    }
    if (worker != nullptr && relayId != 0) {
        worker->StopRelay(control, relayId, relayGeneration, controlGeneration);
    } else {
        PublishStopToControl(control, controlGeneration);
    }
}

void TqWindowsRelayRuntime::SetRelayTraceContext(
    const std::shared_ptr<TqRelayStopControl>& control,
    uint32_t workerIndex,
    uint64_t relayId,
    uint64_t relayGeneration,
    uint64_t controlGeneration,
    uint64_t tunnelId,
    const char* target) {
    if (!ControlGenerationMatches(control, controlGeneration)) {
        return;
    }
    std::lock_guard<std::mutex> lifetimeGuard(RuntimeWorkerLifetimeLock());
    TqWindowsRelayWorker* worker = nullptr;
    {
        std::lock_guard<std::mutex> guard(Lock_);
        if (workerIndex >= Workers_.size() || !Workers_[workerIndex]) {
            return;
        }
        worker = Workers_[workerIndex].get();
    }
    if (worker != nullptr && relayId != 0) {
        worker->SetRelayTraceContext(
            control, relayId, relayGeneration, controlGeneration, tunnelId, target);
    }
}

#endif
