#include "linux_relay_worker.h"

#include <msquic.hpp>

#include <algorithm>
#include <chrono>
#include <climits>
#include <cerrno>
#include <cstring>
#include <new>
#include <thread>

#include <fcntl.h>
#include <sys/epoll.h>
#include <sys/eventfd.h>
#include <sys/socket.h>
#include <sys/uio.h>
#include <unistd.h>

#if defined(__GNUC__)
__attribute__((weak)) const MsQuicApi* MsQuic = nullptr;
#endif

struct TqLinuxRelayWorker::RelayState {
    uint64_t Id{0};
    int TcpFd{-1};
    MsQuicStream* Stream{nullptr};
    TqRelayHandle* Handle{nullptr};
    ITqCompressor* Compressor{nullptr};
    ITqDecompressor* Decompressor{nullptr};
    TqCompressAlgo CompressAlgo{TqCompressAlgo::None};
    std::vector<uint8_t> CompressionOutput;
    std::vector<uint8_t> DecompressionOutput;
    bool EnableQuicSends{true};
    bool Closing{false};
    uint64_t OutstandingQuicSends{0};
    std::deque<TqBufferView> PendingTcpWrites;
    bool TcpWriteShutdownQueued{false};
    bool TcpWriteArmed{false};
    TqLinuxRelayBufferPool Pool;

    RelayState(const TqLinuxRelayRegistration& registration, const TqLinuxRelayWorkerConfig& config)
        : TcpFd(registration.TcpFd),
          Stream(registration.Stream),
          Handle(registration.Handle),
          Compressor(registration.Compressor),
          Decompressor(registration.Decompressor),
          CompressAlgo(registration.CompressAlgo),
          EnableQuicSends(registration.EnableQuicSends),
          Pool(config.ReadChunkSize, config.MaxIov * 4, config.MaxPendingBytes) {}
};

namespace {

bool TqSetNonBlocking(int fd) {
    const int flags = ::fcntl(fd, F_GETFL, 0);
    if (flags < 0) {
        return false;
    }
    return ::fcntl(fd, F_SETFL, flags | O_NONBLOCK) == 0;
}

} // namespace

TqLinuxRelayWorker::TqLinuxRelayWorker(const TqLinuxRelayWorkerConfig& config)
    : Config(config) {}

TqLinuxRelayWorker::~TqLinuxRelayWorker() {
    Stop();
}

bool TqLinuxRelayWorker::Start() {
    if (Running.load()) {
        return false;
    }
    WakeFd = ::eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
    if (WakeFd < 0) {
        return false;
    }
    EpollFd = ::epoll_create1(EPOLL_CLOEXEC);
    if (EpollFd < 0) {
        ::close(WakeFd);
        WakeFd = -1;
        return false;
    }
    epoll_event event{};
    event.events = EPOLLIN;
    event.data.fd = WakeFd;
    if (::epoll_ctl(EpollFd, EPOLL_CTL_ADD, WakeFd, &event) != 0) {
        ::close(EpollFd);
        ::close(WakeFd);
        EpollFd = -1;
        WakeFd = -1;
        return false;
    }
    Running.store(true);
    Thread = std::thread(&TqLinuxRelayWorker::Run, this);
    return true;
}

bool TqLinuxRelayWorker::StartForTest() {
    if (Running.exchange(true)) {
        return false;
    }
    WakeFd = ::eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
    if (WakeFd < 0) {
        Running.store(false);
        return false;
    }
    return true;
}

void TqLinuxRelayWorker::Stop() {
    if (!Running.exchange(false)) {
        return;
    }
    Wake();
    if (Thread.joinable()) {
        Thread.join();
    }
    if (EpollFd >= 0) {
        ::close(EpollFd);
        EpollFd = -1;
    }
    if (WakeFd >= 0) {
        ::close(WakeFd);
        WakeFd = -1;
    }
}

void TqLinuxRelayWorker::Enqueue(const TqLinuxRelayEvent& event) {
    {
        std::lock_guard<std::mutex> guard(QueueLock);
        Queue.push_back(event);
    }
    Wake();
}

void TqLinuxRelayWorker::EnqueueForTest(const TqLinuxRelayEvent& event) {
    Enqueue(event);
}

void TqLinuxRelayWorker::Wake() {
    if (WakeFd < 0) {
        return;
    }
    if (WakeArmed.exchange(true, std::memory_order_acq_rel)) {
        return;
    }
    const uint64_t one = 1;
    const ssize_t written = ::write(WakeFd, &one, sizeof(one));
    if (written == static_cast<ssize_t>(sizeof(one))) {
        WakeupWrites.fetch_add(1);
        return;
    }
    if (errno != EAGAIN && errno != EINTR) {
        WakeArmed.store(false, std::memory_order_release);
    }
}

size_t TqLinuxRelayWorker::DrainForTest(size_t budget) {
    return DrainEvents(budget);
}

size_t TqLinuxRelayWorker::DrainEvents(size_t budget) {
    size_t processed = 0;
    while (processed < budget) {
        TqLinuxRelayEvent event{};
        {
            std::lock_guard<std::mutex> guard(QueueLock);
            if (Queue.empty()) {
                break;
            }
            event = std::move(Queue.front());
            Queue.pop_front();
        }
        switch (event.Type) {
        case TqLinuxRelayEventType::QuicReceive:
            ProcessQuicReceiveEvent(event);
            break;
        case TqLinuxRelayEventType::QuicSendComplete:
            CompleteQuicSend(reinterpret_cast<void*>(event.Value));
            break;
        case TqLinuxRelayEventType::Shutdown:
            UnregisterRelay(event.RelayId);
            break;
        default:
            break;
        }
        ++processed;
    }
    EventsProcessed.fetch_add(processed);

    WakeArmed.store(false, std::memory_order_release);
    {
        std::lock_guard<std::mutex> guard(QueueLock);
        if (!Queue.empty()) {
            Wake();
        }
    }
    return processed;
}

TqLinuxRelayRegistrationResult TqLinuxRelayWorker::RegisterRelayWithId(
    const TqLinuxRelayRegistration& registration) {
    TqLinuxRelayRegistrationResult result{};
    if (registration.TcpFd < 0 || EpollFd < 0) {
        return result;
    }
    if (!TqSetNonBlocking(registration.TcpFd)) {
        return result;
    }

    auto relay = std::make_unique<RelayState>(registration, Config);
    relay->Id = NextRelayId++;
    RelayState* raw = relay.get();

    epoll_event event{};
    event.events = EPOLLIN | EPOLLRDHUP | EPOLLERR;
    event.data.ptr = raw;
    if (::epoll_ctl(EpollFd, EPOLL_CTL_ADD, registration.TcpFd, &event) != 0) {
        return result;
    }

    if (registration.Stream != nullptr) {
        registration.Stream->Callback = TqLinuxRelayWorker::StreamCallback;
        registration.Stream->Context = this;
    }

    std::lock_guard<std::mutex> guard(RelayLock);
    Relays.push_back(std::move(relay));
    result.Ok = true;
    result.RelayId = raw->Id;
    return result;
}

bool TqLinuxRelayWorker::RegisterRelay(const TqLinuxRelayRegistration& registration) {
    return RegisterRelayWithId(registration).Ok;
}

bool TqLinuxRelayWorker::RegisterRelayForTest(const TqLinuxRelayRegistration& registration) {
    return RegisterRelay(registration);
}

void TqLinuxRelayWorker::UnregisterRelay(uint64_t relayId) {
    std::unique_ptr<RelayState> removed;
    {
        std::lock_guard<std::mutex> guard(RelayLock);
        for (auto it = Relays.begin(); it != Relays.end(); ++it) {
            if ((*it)->Id == relayId) {
                removed = std::move(*it);
                Relays.erase(it);
                break;
            }
        }
    }
    if (!removed) {
        return;
    }
    removed->Closing = true;
    if (EpollFd >= 0 && removed->TcpFd >= 0) {
        ::epoll_ctl(EpollFd, EPOLL_CTL_DEL, removed->TcpFd, nullptr);
        ::shutdown(removed->TcpFd, SHUT_RDWR);
    }
    if (removed->Stream != nullptr && removed->Stream->Context == this) {
        removed->Stream->Callback = MsQuicStream::NoOpCallback;
        removed->Stream->Context = nullptr;
    }
    removed->PendingTcpWrites.clear();
}

bool TqLinuxRelayWorker::WaitForObservedTcpBytesForTest(uint64_t bytes, int timeoutMs) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
    while (std::chrono::steady_clock::now() < deadline) {
        if (TcpReadBytes.load() >= bytes) {
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    return TcpReadBytes.load() >= bytes;
}

void TqLinuxRelayWorker::DrainTcpReadable(RelayState* relay) {
    if (relay == nullptr || relay->Closing) {
        return;
    }

    uint64_t readBytes = 0;
    const uint64_t tickBudget = std::min<uint64_t>(Config.ReadBatchBytes, Config.ByteBudgetPerTick);
    while (readBytes < tickBudget) {
        if (relay->Pool.PendingBytes() + Config.ReadChunkSize > relay->Pool.MaxPendingBytes()) {
            ReadDisabledCount.fetch_add(1);
            break;
        }

        std::vector<TqBufferRef> refs;
        std::vector<iovec> iov;
        const size_t maxIov = std::min<size_t>(Config.MaxIov, 1024);
        refs.reserve(maxIov);
        iov.reserve(maxIov);

        for (size_t i = 0; i < maxIov && readBytes + Config.ReadChunkSize <= tickBudget; ++i) {
            auto buffer = relay->Pool.Acquire();
            if (!buffer) {
                break;
            }
            iovec item{};
            item.iov_base = buffer->Data();
            item.iov_len = buffer->Capacity();
            iov.push_back(item);
            refs.push_back(std::move(buffer));
        }
        if (iov.empty()) {
            break;
        }

        const ssize_t received = ::readv(relay->TcpFd, iov.data(), static_cast<int>(iov.size()));
        if (received > 0) {
            size_t remaining = static_cast<size_t>(received);
            std::vector<TqBufferView> views;
            views.reserve(refs.size());
            for (auto& ref : refs) {
                if (remaining == 0) {
                    break;
                }
                const size_t len = std::min(ref->Capacity(), remaining);
                ref->SetLength(len);
                views.push_back(TqBufferView{ref->Data(), len, ref});
                remaining -= len;
            }

            readBytes += static_cast<uint64_t>(received);
            TcpReadBytes.fetch_add(static_cast<uint64_t>(received));
            TcpReadBatches.fetch_add(1);
            uint64_t previous = MaxTcpReadIovUsed.load();
            while (previous < views.size() &&
                   !MaxTcpReadIovUsed.compare_exchange_weak(previous, views.size())) {
            }
            if (!SubmitTcpBatchToQuic(relay, views)) {
                if (relay->Handle != nullptr) {
                    relay->Handle->Stop.store(true);
                }
                break;
            }
            continue;
        }
        if (received == 0) {
            break;
        }
        if (errno == EINTR) {
            continue;
        }
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            break;
        }
        break;
    }
}

// In production, ClientContext owns TqLinuxRelaySendOperation until
// QUIC_STREAM_EVENT_SEND_COMPLETE is delivered back to the owner worker.
// Tests can disable sends to verify readv batching without a live MsQuic stream.
bool TqLinuxRelayWorker::SubmitTcpBatchToQuic(RelayState* relay, std::vector<TqBufferView>& views) {
    if (relay == nullptr || views.empty()) {
        return true;
    }
    if (!relay->EnableQuicSends || relay->Stream == nullptr) {
        views.clear();
        return true;
    }

    std::vector<QUIC_BUFFER> quicBuffers;
    quicBuffers.reserve(views.size());
    for (const auto& view : views) {
        if (view.Len > UINT32_MAX) {
            return false;
        }
        QUIC_BUFFER buffer{};
        buffer.Buffer = view.Data;
        buffer.Length = static_cast<uint32_t>(view.Len);
        quicBuffers.push_back(buffer);
    }

    auto* operation = new (std::nothrow) TqLinuxRelaySendOperation{};
    if (operation == nullptr) {
        return false;
    }
    operation->RelayId = relay->Id;
    operation->Views = std::move(views);

    const QUIC_STATUS status = relay->Stream->Send(
        quicBuffers.data(),
        static_cast<uint32_t>(quicBuffers.size()),
        QUIC_SEND_FLAG_NONE,
        operation);
    if (QUIC_FAILED(status)) {
        delete operation;
        return false;
    }
    ++relay->OutstandingQuicSends;
    QuicSendOperations.fetch_add(1);
    return true;
}

void TqLinuxRelayWorker::CompleteQuicSend(void* context) {
    auto* operation = static_cast<TqLinuxRelaySendOperation*>(context);
    if (operation == nullptr) {
        return;
    }
    RelayState* relay = FindRelayById(operation->RelayId);
    if (relay != nullptr && relay->OutstandingQuicSends > 0) {
        --relay->OutstandingQuicSends;
    }
    delete operation;
}

TqLinuxRelayWorker::RelayState* TqLinuxRelayWorker::FindRelayById(uint64_t relayId) {
    std::lock_guard<std::mutex> guard(RelayLock);
    for (const auto& relay : Relays) {
        if (relay->Id == relayId) {
            return relay.get();
        }
    }
    return nullptr;
}

TqLinuxRelayWorker::RelayState* TqLinuxRelayWorker::FindRelayByFd(int tcpFd) {
    std::lock_guard<std::mutex> guard(RelayLock);
    for (const auto& relay : Relays) {
        if (relay->TcpFd == tcpFd) {
            return relay.get();
        }
    }
    return nullptr;
}

uint64_t TqLinuxRelayWorker::FindRelayIdByStream(MsQuicStream* stream) {
    if (stream == nullptr) {
        return 0;
    }
    std::lock_guard<std::mutex> guard(RelayLock);
    for (const auto& relay : Relays) {
        if (relay->Stream == stream) {
            return relay->Id;
        }
    }
    return 0;
}

bool TqLinuxRelayWorker::CopyQuicReceiveToEvent(
    uint64_t relayId,
    const uint8_t* data,
    uint32_t length) {
    RelayState* relay = FindRelayById(relayId);
    if (relay == nullptr || relay->Closing) {
        return false;
    }
    if (length == 0) {
        return true;
    }
    if (data == nullptr) {
        return false;
    }

    size_t offset = 0;
    while (offset < length) {
        auto buffer = relay->Pool.Acquire();
        if (!buffer) {
            return false;
        }
        const size_t chunk = std::min(buffer->Capacity(), static_cast<size_t>(length - offset));
        std::memcpy(buffer->Data(), data + offset, chunk);
        buffer->SetLength(chunk);
        TqLinuxRelayEvent event{};
        event.Type = TqLinuxRelayEventType::QuicReceive;
        event.RelayId = relayId;
        event.Buffer = buffer;
        event.Length = chunk;
        Enqueue(event);
        offset += chunk;
    }
    return true;
}

void TqLinuxRelayWorker::AbortRelayFromCallback(uint64_t relayId, MsQuicStream* stream) {
    (void)stream;
    TqLinuxRelayEvent shutdown{};
    shutdown.Type = TqLinuxRelayEventType::Shutdown;
    shutdown.RelayId = relayId;
    Enqueue(shutdown);
}

bool TqLinuxRelayWorker::EnqueueQuicReceiveForTest(
    int tcpFd,
    const uint8_t* data,
    size_t length,
    bool fin) {
    RelayState* relay = FindRelayByFd(tcpFd);
    if (relay == nullptr) {
        return false;
    }
    if (!EnqueueQuicReceive(relay, data, length, fin)) {
        return false;
    }
    FlushTcpWrites(relay);
    return true;
}

bool TqLinuxRelayWorker::EnqueueQuicReceive(
    RelayState* relay,
    const uint8_t* data,
    size_t length,
    bool fin) {
    if (relay == nullptr || (length > 0 && data == nullptr)) {
        return false;
    }

    size_t offset = 0;
    while (offset < length) {
        auto buffer = relay->Pool.Acquire();
        if (!buffer) {
            return false;
        }
        const size_t chunk = std::min(buffer->Capacity(), length - offset);
        std::memcpy(buffer->Data(), data + offset, chunk);
        buffer->SetLength(chunk);
        relay->PendingTcpWrites.push_back(TqBufferView{buffer->Data(), chunk, buffer});
        offset += chunk;
    }
    if (fin) {
        relay->TcpWriteShutdownQueued = true;
    }
    return true;
}

void TqLinuxRelayWorker::FlushTcpWrites(RelayState* relay) {
    if (relay == nullptr || relay->Closing) {
        return;
    }

    while (!relay->PendingTcpWrites.empty()) {
        std::vector<iovec> iov;
        iov.reserve(Config.MaxIov);
        for (const auto& view : relay->PendingTcpWrites) {
            if (iov.size() >= Config.MaxIov) {
                break;
            }
            iovec item{};
            item.iov_base = view.Data;
            item.iov_len = view.Len;
            iov.push_back(item);
        }

        const ssize_t sent = ::writev(relay->TcpFd, iov.data(), static_cast<int>(iov.size()));
        if (sent > 0) {
            size_t remaining = static_cast<size_t>(sent);
            TcpWriteBytes.fetch_add(static_cast<uint64_t>(sent));
            TcpWriteBatches.fetch_add(1);
            uint64_t previous = MaxTcpWriteIovUsed.load();
            while (previous < iov.size() &&
                   !MaxTcpWriteIovUsed.compare_exchange_weak(previous, iov.size())) {
            }
            while (remaining > 0 && !relay->PendingTcpWrites.empty()) {
                auto& front = relay->PendingTcpWrites.front();
                if (remaining >= front.Len) {
                    remaining -= front.Len;
                    relay->PendingTcpWrites.pop_front();
                } else {
                    front.Data += remaining;
                    front.Len -= remaining;
                    remaining = 0;
                }
            }
            continue;
        }
        if (sent < 0 && errno == EINTR) {
            continue;
        }
        if (sent < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            ArmTcpWritable(relay, true);
            break;
        }
        break;
    }

    if (relay->PendingTcpWrites.empty() && relay->TcpWriteShutdownQueued) {
        ::shutdown(relay->TcpFd, SHUT_WR);
        relay->TcpWriteShutdownQueued = false;
    }
    if (relay->PendingTcpWrites.empty()) {
        ArmTcpWritable(relay, false);
    }
}

void TqLinuxRelayWorker::ArmTcpWritable(RelayState* relay, bool enabled) {
    if (relay == nullptr || relay->TcpFd < 0 || EpollFd < 0 || relay->TcpWriteArmed == enabled) {
        return;
    }
    epoll_event event{};
    event.events = EPOLLIN | EPOLLRDHUP | EPOLLERR;
    if (enabled) {
        event.events |= EPOLLOUT;
    }
    event.data.ptr = relay;
    if (::epoll_ctl(EpollFd, EPOLL_CTL_MOD, relay->TcpFd, &event) == 0) {
        relay->TcpWriteArmed = enabled;
    }
}

void TqLinuxRelayWorker::ProcessQuicReceiveEvent(const TqLinuxRelayEvent& event) {
    RelayState* relay = FindRelayById(event.RelayId);
    if (relay == nullptr || relay->Closing) {
        return;
    }
    if (event.Buffer) {
        relay->PendingTcpWrites.push_back(TqBufferView{
            event.Buffer->Data(),
            event.Length,
            event.Buffer});
    }
    if (event.Fin) {
        relay->TcpWriteShutdownQueued = true;
    }
    FlushTcpWrites(relay);
}

TqLinuxRelayWorkerSnapshot TqLinuxRelayWorker::Snapshot() const {
    std::lock_guard<std::mutex> queueGuard(QueueLock);
    TqLinuxRelayWorkerSnapshot snapshot{};
    snapshot.EventsProcessed = EventsProcessed.load();
    snapshot.WakeupWrites = WakeupWrites.load();
    snapshot.PendingEvents = Queue.size();
    snapshot.TcpReadBatches = TcpReadBatches.load();
    snapshot.TcpReadBytes = TcpReadBytes.load();
    snapshot.QuicSendOperations = QuicSendOperations.load();
    snapshot.MaxTcpReadIovUsed = MaxTcpReadIovUsed.load();
    snapshot.TcpWriteBatches = TcpWriteBatches.load();
    snapshot.TcpWriteBytes = TcpWriteBytes.load();
    snapshot.MaxTcpWriteIovUsed = MaxTcpWriteIovUsed.load();
    snapshot.ReadDisabledCount = ReadDisabledCount.load();

    {
        std::lock_guard<std::mutex> relayGuard(RelayLock);
        for (const auto& relay : Relays) {
            snapshot.PendingBytes += relay->Pool.PendingBytes();
            for (const auto& view : relay->PendingTcpWrites) {
                snapshot.PendingBytes += view.Len;
            }
        }
    }
    return snapshot;
}

void TqLinuxRelayWorker::Run() {
    epoll_event events[16]{};
    while (Running.load()) {
        const int count = ::epoll_wait(EpollFd, events, 16, 100);
        if (count < 0 && errno == EINTR) {
            continue;
        }
        if (count <= 0) {
            continue;
        }
        for (int i = 0; i < count; ++i) {
            if (events[i].data.fd == WakeFd) {
                uint64_t value = 0;
                while (::read(WakeFd, &value, sizeof(value)) > 0) {
                }
                DrainEvents(Config.EventBudget);
            } else {
                auto* relay = static_cast<RelayState*>(events[i].data.ptr);
                if ((events[i].events & EPOLLOUT) != 0) {
                    FlushTcpWrites(relay);
                }
                if ((events[i].events & EPOLLIN) != 0) {
                    DrainTcpReadable(relay);
                }
            }
        }
    }
}

QUIC_STATUS QUIC_API TqLinuxRelayWorker::StreamCallback(
    _In_ MsQuicStream* stream,
    _In_opt_ void* context,
    _Inout_ QUIC_STREAM_EVENT* event) noexcept {
    auto* worker = static_cast<TqLinuxRelayWorker*>(context);
    if (worker == nullptr) {
        return QUIC_STATUS_SUCCESS;
    }
    return worker->OnStreamEvent(stream, event);
}

QUIC_STATUS TqLinuxRelayWorker::OnStreamEvent(
    MsQuicStream* stream,
    QUIC_STREAM_EVENT* event) noexcept {
    if (event->Type == QUIC_STREAM_EVENT_SEND_COMPLETE) {
        TqLinuxRelayEvent queued{};
        queued.Type = TqLinuxRelayEventType::QuicSendComplete;
        queued.Value = reinterpret_cast<uintptr_t>(event->SEND_COMPLETE.ClientContext);
        Enqueue(queued);
        return QUIC_STATUS_SUCCESS;
    }
    if (event->Type == QUIC_STREAM_EVENT_RECEIVE) {
        const uint64_t relayId = FindRelayIdByStream(stream);
        if (relayId == 0) {
            return QUIC_STATUS_SUCCESS;
        }
        for (uint32_t i = 0; i < event->RECEIVE.BufferCount; ++i) {
            const auto& buffer = event->RECEIVE.Buffers[i];
            if (!CopyQuicReceiveToEvent(relayId, buffer.Buffer, buffer.Length)) {
                AbortRelayFromCallback(relayId, stream);
                return QUIC_STATUS_SUCCESS;
            }
        }
        if ((event->RECEIVE.Flags & QUIC_RECEIVE_FLAG_FIN) != 0) {
            TqLinuxRelayEvent fin{};
            fin.Type = TqLinuxRelayEventType::QuicReceive;
            fin.RelayId = relayId;
            fin.Fin = true;
            Enqueue(fin);
        }
        return QUIC_STATUS_SUCCESS;
    }
    if (event->Type == QUIC_STREAM_EVENT_SHUTDOWN_COMPLETE ||
        event->Type == QUIC_STREAM_EVENT_PEER_SEND_ABORTED ||
        event->Type == QUIC_STREAM_EVENT_PEER_RECEIVE_ABORTED) {
        const uint64_t relayId = FindRelayIdByStream(stream);
        if (relayId != 0) {
            TqLinuxRelayEvent shutdown{};
            shutdown.Type = TqLinuxRelayEventType::Shutdown;
            shutdown.RelayId = relayId;
            Enqueue(shutdown);
        }
        return QUIC_STATUS_SUCCESS;
    }
    return QUIC_STATUS_SUCCESS;
}

TqLinuxRelayRuntime& TqLinuxRelayRuntime::Instance() {
    static TqLinuxRelayRuntime runtime;
    return runtime;
}

bool TqLinuxRelayRuntime::Start(const TqTuningConfig& tuning) {
    std::lock_guard<std::mutex> guard(Lock);
    if (!Workers.empty()) {
        return true;
    }

    const uint32_t workerCount = std::max<uint32_t>(1, tuning.LinuxRelayWorkerCount);
    for (uint32_t i = 0; i < workerCount; ++i) {
        TqLinuxRelayWorkerConfig config{};
        config.EventBudget = tuning.LinuxRelayWorkerEventBudget;
        config.ByteBudgetPerTick = tuning.LinuxRelayWorkerByteBudgetPerTick;
        config.ReadChunkSize = tuning.LinuxRelayReadChunkSize;
        config.ReadBatchBytes = tuning.LinuxRelayReadBatchBytes;
        config.MaxIov = tuning.LinuxRelayMaxIov;
        config.MaxPendingBytes = tuning.LinuxRelayPerWorkerPendingBytes;
        auto worker = std::make_unique<TqLinuxRelayWorker>(config);
        if (!worker->Start()) {
            Workers.clear();
            return false;
        }
        Workers.push_back(std::move(worker));
    }
    return true;
}

void TqLinuxRelayRuntime::Stop() {
    std::lock_guard<std::mutex> guard(Lock);
    Workers.clear();
    NextWorker = 0;
}

TqLinuxRelayWorker* TqLinuxRelayRuntime::PickWorker() {
    std::lock_guard<std::mutex> guard(Lock);
    if (Workers.empty()) {
        return nullptr;
    }
    TqLinuxRelayWorker* worker = Workers[NextWorker % Workers.size()].get();
    ++NextWorker;
    return worker;
}

TqLinuxRelayWorkerSnapshot TqLinuxRelayRuntime::Snapshot() const {
    std::lock_guard<std::mutex> guard(Lock);
    TqLinuxRelayWorkerSnapshot total{};
    for (const auto& worker : Workers) {
        const auto snapshot = worker->Snapshot();
        total.EventsProcessed += snapshot.EventsProcessed;
        total.WakeupWrites += snapshot.WakeupWrites;
        total.PendingEvents += snapshot.PendingEvents;
        total.PendingBytes += snapshot.PendingBytes;
        total.TcpReadBatches += snapshot.TcpReadBatches;
        total.TcpReadBytes += snapshot.TcpReadBytes;
        total.QuicSendOperations += snapshot.QuicSendOperations;
        total.MaxTcpReadIovUsed = std::max(total.MaxTcpReadIovUsed, snapshot.MaxTcpReadIovUsed);
        total.TcpWriteBatches += snapshot.TcpWriteBatches;
        total.TcpWriteBytes += snapshot.TcpWriteBytes;
        total.MaxTcpWriteIovUsed = std::max(total.MaxTcpWriteIovUsed, snapshot.MaxTcpWriteIovUsed);
        total.ReadDisabledCount += snapshot.ReadDisabledCount;
    }
    return total;
}
