#include "windows_relay_worker.h"

#if defined(_WIN32)

#include "msquic.hpp"
#include "relay_buffer.h"

#include <algorithm>
#include <deque>
#include <spdlog/spdlog.h>

#include <windows.h>

const MsQuicApi* MsQuic = nullptr;

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

enum class TqWindowsRelayEvent : uint32_t {
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
    TqWindowsRelayEvent Event{TqWindowsRelayEvent::TcpRecv};
    std::shared_ptr<RelayContext> Relay;
    TqBufferRef BufferOwner;
    WSABUF WsaBuffer{};
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
    std::atomic<bool> CloseAfterDrained{false};
    std::atomic<uint32_t> InFlightQuicSends{0};
    std::atomic<uint32_t> QueuedQuicReceives{0};
    std::atomic<uint32_t> InFlightTcpSends{0};
    std::atomic<uint64_t> PendingQuicReceiveBytes{0};
    std::atomic<uint64_t> PendingQuicReceiveQueueDepth{0};
    std::mutex PendingReceiveLock;
    std::list<std::shared_ptr<TqWindowsPendingQuicReceive>> PendingReceives;
    std::mutex PendingQuicSendLock;
    std::deque<std::unique_ptr<IoOperation>> PendingQuicSends;
    std::shared_ptr<CallbackBinding> Callback;
    std::atomic<bool> QuicReceivePaused{false};
};

TqWindowsRelayWorker::TqWindowsRelayWorker() = default;
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
        CloseRelay(relay, TqRelayCloseMode::GracefulDrain);
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
    {
        std::lock_guard<std::mutex> guard(Lock_);
        for (const auto& entry : Relays_) {
            if (entry.second) {
                snapshot.PendingQuicReceiveBytes +=
                    entry.second->PendingQuicReceiveBytes.load(std::memory_order_relaxed);
                snapshot.PendingQuicReceiveQueueDepth +=
                    entry.second->PendingQuicReceiveQueueDepth.load(std::memory_order_relaxed);
                const uint64_t pendingBufferBytes =
                    entry.second->PendingBufferBytes.load(std::memory_order_relaxed);
                const uint64_t allocateCount =
                    entry.second->AllocateCount.load(std::memory_order_relaxed);
                snapshot.RelayBufferBytesInUse += pendingBufferBytes;
                snapshot.RelayBufferAllocateCount += allocateCount;
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

void TqWindowsRelayWorker::Run() {
    while (!Stopping_.load()) {
        DWORD bytes = 0;
        ULONG_PTR key = 0;
        OVERLAPPED* overlapped = nullptr;
        const BOOL ok = ::GetQueuedCompletionStatus(
            static_cast<HANDLE>(Iocp_), &bytes, &key, &overlapped, INFINITE);
        if (overlapped == nullptr && Stopping_.load()) {
            break;
        }
        if (overlapped == nullptr) {
            continue;
        }
        std::unique_ptr<IoOperation> op(reinterpret_cast<IoOperation*>(overlapped));
        if (!ok) {
            RecordTcpHardErrorAndFail(op->Relay, "iocp_completion_error");
            continue;
        }
        switch (op->Event) {
        case TqWindowsRelayEvent::TcpRecv:
            HandleTcpRecv(std::move(op), bytes);
            break;
        case TqWindowsRelayEvent::TcpSend:
            HandleTcpSend(std::move(op), bytes);
            break;
        case TqWindowsRelayEvent::QuicReceiveQueued:
            HandleQuicReceiveQueued(std::move(op));
            break;
        case TqWindowsRelayEvent::QuicReceiveViewQueued:
            HandleQuicReceiveViewQueued(std::move(op));
            break;
        case TqWindowsRelayEvent::QuicSendRetry:
            RetryPendingQuicSends(op->Relay);
            break;
        case TqWindowsRelayEvent::CloseRelay:
            CloseRelay(op->Relay, TqRelayCloseMode::GracefulDrain);
            break;
        default:
            break;
        }
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
    op->Event = TqWindowsRelayEvent::TcpSend;
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
    op->Event = TqWindowsRelayEvent::TcpSend;
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
    op->Event = TqWindowsRelayEvent::TcpRecv;
    op->Relay = relay;
    op->BufferOwner.reset();
    op->Buffer.clear();
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
    IoOperation* raw = op.release();
    const int rc = ::WSARecv(relay->TcpFd, &raw->WsaBuffer, 1, &received, &flags, &raw->Overlapped, nullptr);
    if (rc == 0 || ::WSAGetLastError() == WSA_IO_PENDING) {
        return true;
    }
    delete raw;
    RecordTcpHardErrorAndFail(relay, "wsa_recv_post_failed");
    return false;
}

void TqWindowsRelayWorker::CloseRelay(
    const std::shared_ptr<RelayContext>& relay,
    TqRelayCloseMode mode) {
    if (!relay || relay->Closing.exchange(true)) {
        return;
    }
    std::shared_ptr<CallbackBinding> bindingKeepAlive;
    if (mode == TqRelayCloseMode::AbortReset) {
        FatalRelayResets_.fetch_add(1, std::memory_order_relaxed);
        TqResetSocket(relay->TcpFd);
    } else {
        TqCloseSocket(relay->TcpFd);
    }
    relay->TcpFd = TqInvalidSocket;
    if (relay->Callback) {
        bindingKeepAlive = relay->Callback;
        relay->Callback->Closing.store(true, std::memory_order_release);
        relay->Callback->RelayHint.store(nullptr, std::memory_order_release);
    }
    CompleteAllPendingQuicReceives(relay);
    if (relay->Stream != nullptr && relay->Stream->Context == relay->Callback.get()) {
        relay->Stream->Callback = MsQuicStream::NoOpCallback;
        relay->Stream->Context = nullptr;
    }
    {
        std::lock_guard<std::mutex> guard(Lock_);
        Relays_.erase(relay->Id);
        if (bindingKeepAlive) {
            RetiredCallbacks_.push_back(std::move(bindingKeepAlive));
        }
    }
    PruneRetiredCallbacks(true);
    if (relay->PublicHandle != nullptr) {
        relay->PublicHandle->Stop.store(true);
    }
}

void TqWindowsRelayWorker::FailRelayFatal(const std::shared_ptr<RelayContext>& relay, const char* reason) {
    if (relay) {
        size_t pendingQuicSends = 0;
        {
            std::lock_guard<std::mutex> guard(relay->PendingQuicSendLock);
            pendingQuicSends = relay->PendingQuicSends.size();
        }
        spdlog::error(
            "windows relay unrecoverable error reason={} relay_id={} socket={} pending_quic_receive_bytes={} pending_quic_receive_queue={} pending_quic_sends={} inflight_quic_sends={} inflight_tcp_sends={}",
            reason != nullptr ? reason : "unknown",
            relay->Id,
            static_cast<uint64_t>(relay->TcpFd),
            relay->PendingQuicReceiveBytes.load(std::memory_order_relaxed),
            relay->PendingQuicReceiveQueueDepth.load(std::memory_order_relaxed),
            static_cast<uint64_t>(pendingQuicSends),
            relay->InFlightQuicSends.load(std::memory_order_relaxed),
            relay->InFlightTcpSends.load(std::memory_order_relaxed));
    }
    CloseRelay(relay, TqRelayCloseMode::AbortReset);
}

void TqWindowsRelayWorker::RecordTcpHardErrorAndFail(
    const std::shared_ptr<RelayContext>& relay,
    const char* reason) {
    Errors_.fetch_add(1, std::memory_order_relaxed);
    TcpHardErrors_.fetch_add(1, std::memory_order_relaxed);
    FailRelayFatal(relay, reason);
}

bool TqWindowsRelayWorker::IsQuicSendBackpressureStatus(QUIC_STATUS status) const {
    return status == QUIC_STATUS_OUT_OF_MEMORY || status == QUIC_STATUS_BUFFER_TOO_SMALL;
}

bool TqWindowsRelayWorker::PostQuicSend(
    std::unique_ptr<IoOperation> op,
    QUIC_SEND_FLAGS flags,
    bool repostOnBackpressure) {
    auto relay = op ? op->Relay : nullptr;
    if (!relay || relay->Closing.load() || relay->Stream == nullptr || relay->Stream->Handle == nullptr) {
        return false;
    }
    QUIC_BUFFER buffer{};
    if (!op->Buffer.empty()) {
        buffer.Buffer = op->Buffer.data();
        buffer.Length = static_cast<uint32_t>(op->Buffer.size());
    } else if (op->BufferOwner) {
        buffer.Buffer = op->BufferOwner->Data();
        buffer.Length = static_cast<uint32_t>(op->BufferOwner->Length());
    }
    op->QuicSendFlags = flags;
    relay->InFlightQuicSends.fetch_add(1);
    IoOperation* raw = op.release();
    const QUIC_STATUS status = relay->Stream->Send(
        buffer.Length == 0 ? nullptr : &buffer,
        buffer.Length == 0 ? 0 : 1,
        flags,
        raw);
    if (!QUIC_FAILED(status)) {
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
            retry->Event = TqWindowsRelayEvent::QuicSendRetry;
            retry->Relay = relay;
            IoOperation* retryRaw = retry.release();
            if (!::PostQueuedCompletionStatus(static_cast<HANDLE>(Iocp_), 0, 0, &retryRaw->Overlapped)) {
                delete retryRaw;
                Errors_.fetch_add(1, std::memory_order_relaxed);
            }
        }
        return true;
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
    relay->CloseAfterDrained.store(false);
    if (TqSocketValid(relay->TcpFd)) {
        (void)TqShutdownSend(relay->TcpFd);
        GracefulRelayDrains_.fetch_add(1, std::memory_order_relaxed);
    }
    if (relay->PublicHandle != nullptr) {
        relay->PublicHandle->Stop.store(true);
    }
    return true;
}

void TqWindowsRelayWorker::HandleTcpRecv(std::unique_ptr<IoOperation> op, DWORD bytes) {
    auto relay = op->Relay;
    if (!relay || relay->Closing.load()) {
        return;
    }
    if (bytes == 0) {
        op->BufferOwner.reset();
        if (!relay->TcpRecvClosed.exchange(true)) {
            std::vector<uint8_t> finalOutput;
            if (relay->Compressor != nullptr) {
                if (!relay->Compressor->Compress(nullptr, 0, finalOutput, true)) {
                    CloseRelay(relay, TqRelayCloseMode::GracefulDrain);
                    return;
                }
            }
            if (!finalOutput.empty()) {
                op->Buffer.swap(finalOutput);
                if (!PostQuicSend(std::move(op), QUIC_SEND_FLAG_FIN, true)) {
                    return;
                }
            } else {
                op->BufferOwner.reset();
                op->Buffer.clear();
                if (!PostQuicSend(std::move(op), QUIC_SEND_FLAG_FIN, true)) {
                    return;
                }
            }
            TqCloseSocket(relay->TcpFd);
            relay->TcpFd = TqInvalidSocket;
        }
        return;
    }
    if (!op->BufferOwner) {
        Errors_.fetch_add(1, std::memory_order_relaxed);
        CloseRelay(relay, TqRelayCloseMode::GracefulDrain);
        return;
    }
    op->BufferOwner->SetLength(bytes);
    uint8_t* sendData = op->BufferOwner->Data();
    uint32_t sendLength = static_cast<uint32_t>(op->BufferOwner->Length());
    if (relay->Compressor != nullptr) {
        std::vector<uint8_t> compressed;
        if (!relay->Compressor->Compress(sendData, sendLength, compressed, false)) {
            CloseRelay(relay, TqRelayCloseMode::GracefulDrain);
            return;
        }
        if (compressed.empty() && !relay->Compressor->Flush(compressed)) {
            CloseRelay(relay, TqRelayCloseMode::GracefulDrain);
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
    if (relay->Decompressor != nullptr && relay->CompressAlgo == TqCompressAlgo::Zstd) {
        if (!PostTcpSendFromCompressedReceiveView(relay, view)) {
            (void)CompletePendingQuicReceive(relay, view);
            CloseRelay(relay, TqRelayCloseMode::GracefulDrain);
        }
        return;
    }
    if (!PostTcpSendFromReceiveView(relay, view)) {
        (void)CompletePendingQuicReceive(relay, view);
        CloseRelay(relay, TqRelayCloseMode::GracefulDrain);
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
            CloseRelay(relay, TqRelayCloseMode::GracefulDrain);
            return;
        }
        op->Buffer.swap(output);
    }
    if (op->Buffer.empty()) {
        (void)CloseRelayIfDrained(relay);
        return;
    }
    op->Event = TqWindowsRelayEvent::TcpSend;
    op->Offset = 0;
    if (!PostTcpSend(std::move(op))) {
        CloseRelay(relay, TqRelayCloseMode::GracefulDrain);
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

    auto view = std::make_shared<TqWindowsPendingQuicReceive>();
    view->Stream = stream;
    view->RelayId = relay->Id;
    view->Fin = fin;
    view->CompleteBatchBytes = relay->Tuning.WindowsRelayQuicReceiveCompleteBatchBytes;
    view->Slices.reserve(bufferCount);
    for (uint32_t i = 0; i < bufferCount; ++i) {
        const QUIC_BUFFER& buffer = buffers[i];
        if (buffer.Length != 0 && buffer.Buffer == nullptr) {
            Errors_.fetch_add(1, std::memory_order_relaxed);
            return false;
        }
        if (buffer.Length == 0) {
            continue;
        }
        view->Slices.push_back(TqWindowsQuicReceiveSlice{buffer.Buffer, buffer.Length});
        view->TotalLength += buffer.Length;
    }
    if (view->TotalLength == 0 && !fin) {
        return true;
    }
    if (view->TotalLength == 0 && fin) {
        if (TqSocketValid(relay->TcpFd)) {
            (void)TqShutdownSend(relay->TcpFd);
            GracefulRelayDrains_.fetch_add(1, std::memory_order_relaxed);
        }
        return true;
    }

    auto op = std::make_unique<IoOperation>();
    op->Event = TqWindowsRelayEvent::QuicReceiveViewQueued;
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
            !relay->QuicReceivePaused.exchange(true, std::memory_order_acq_rel)) {
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
    if (!::PostQueuedCompletionStatus(static_cast<HANDLE>(Iocp_), 0, 0, &raw->Overlapped)) {
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
    op->Event = TqWindowsRelayEvent::TcpSend;
    op->Relay = relay;
    op->ReceiveView = view;
    op->Offset = view->SliceOffset;
    op->PostedLength = static_cast<uint64_t>(slice.Length - view->SliceOffset);

    WSABUF buf{};
    buf.buf = reinterpret_cast<char*>(const_cast<uint8_t*>(slice.Data + view->SliceOffset));
    buf.len = static_cast<ULONG>(op->PostedLength);
    DWORD sent = 0;
    relay->InFlightTcpSends.fetch_add(1);
    IoOperation* raw = op.release();
    const int rc = ::WSASend(relay->TcpFd, &buf, 1, &sent, 0, &raw->Overlapped, nullptr);
    const int error = rc == 0 ? 0 : ::WSAGetLastError();
    if (rc != 0 && error != WSA_IO_PENDING) {
        relay->InFlightTcpSends.fetch_sub(1);
        delete raw;
        RecordTcpHardErrorAndFail(relay, "wsa_send_receive_view_failed");
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
            op->Event = TqWindowsRelayEvent::TcpSend;
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
                RecordTcpHardErrorAndFail(relay, "wsa_send_compressed_receive_view_failed");
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
    if (view->Fin && TqSocketValid(relay->TcpFd)) {
        (void)TqShutdownSend(relay->TcpFd);
        GracefulRelayDrains_.fetch_add(1, std::memory_order_relaxed);
    }
    MaybeResumeQuicReceive(relay);
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
    } else {
        if (wasPaused) {
            return;
        }
        QuicReceivePausedCount_.fetch_add(1, std::memory_order_relaxed);
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
    WSABUF buf{};
    buf.buf = reinterpret_cast<char*>(op->Buffer.data() + op->Offset);
    buf.len = static_cast<ULONG>(op->Buffer.size() - op->Offset);
    DWORD sent = 0;
    relay->InFlightTcpSends.fetch_add(1);
    IoOperation* raw = op.release();
    const int rc = ::WSASend(relay->TcpFd, &buf, 1, &sent, 0, &raw->Overlapped, nullptr);
    const int error = rc == 0 ? 0 : ::WSAGetLastError();
    if (rc != 0 && error != WSA_IO_PENDING) {
        relay->InFlightTcpSends.fetch_sub(1);
        delete raw;
        RecordTcpHardErrorAndFail(relay, "wsa_send_buffer_failed");
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
            RecordTcpHardErrorAndFail(relay, "tcp_send_receive_view_completion_error");
            return;
        }
        TcpSendBytes_.fetch_add(bytes, std::memory_order_relaxed);
        if (static_cast<uint64_t>(bytes) < op->PostedLength) {
            TcpSendPartialCompletions_.fetch_add(1, std::memory_order_relaxed);
            if (!(relay->Decompressor != nullptr && relay->CompressAlgo == TqCompressAlgo::Zstd)) {
                AdvanceReceiveView(relay, *view, bytes);
                FlushDeferredReceiveCompletion(*view, false);
                if (!PostTcpSendFromReceiveView(relay, view)) {
                    CloseRelay(relay, TqRelayCloseMode::GracefulDrain);
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
                RecordTcpHardErrorAndFail(relay, "wsa_send_receive_view_retry_failed");
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
                CloseRelay(relay, TqRelayCloseMode::GracefulDrain);
            }
        } else {
            AdvanceReceiveView(relay, *view, bytes);
            const bool viewComplete = view->CompletedLength >= view->TotalLength;
            FlushDeferredReceiveCompletion(*view, viewComplete);
            if (!PostTcpSendFromReceiveView(relay, view)) {
                CloseRelay(relay, TqRelayCloseMode::GracefulDrain);
            }
        }
        return;
    }
    TcpSendBytes_.fetch_add(bytes, std::memory_order_relaxed);
    if (bytes == 0) {
        RecordTcpHardErrorAndFail(relay, "tcp_send_completion_zero");
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
            CloseRelay(relay, TqRelayCloseMode::GracefulDrain);
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
    auto op = std::make_unique<IoOperation>();
    op->Event = TqWindowsRelayEvent::CloseRelay;
    op->Relay = relay;
    IoOperation* raw = op.release();
    if (!::PostQueuedCompletionStatus(static_cast<HANDLE>(Iocp_), 0, 0, &raw->Overlapped)) {
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
                    worker->CloseRelay(relay, TqRelayCloseMode::GracefulDrain);
                }
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
        if (event->SHUTDOWN_COMPLETE.ConnectionShutdown) {
            worker->FailRelayFatal(relay, "stream_connection_shutdown");
            return QUIC_STATUS_SUCCESS;
        }
        relay->CloseAfterDrained.store(true);
        (void)worker->CloseRelayIfDrained(relay);
    }
    return QUIC_STATUS_SUCCESS;
}

TqWindowsRelayRuntime& TqWindowsRelayRuntime::Instance() {
    static TqWindowsRelayRuntime runtime;
    return runtime;
}

bool TqWindowsRelayRuntime::Start(uint32_t workerCount) {
    std::lock_guard<std::mutex> guard(Lock_);
    if (!Workers_.empty()) {
        return true;
    }
    if (workerCount == 0) {
        workerCount = 1;
    }
    for (uint32_t i = 0; i < workerCount; ++i) {
        auto worker = std::make_unique<TqWindowsRelayWorker>();
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
        total.Errors += snapshot.Errors;
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
