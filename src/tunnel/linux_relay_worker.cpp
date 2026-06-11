#include "linux_relay_worker.h"

#include <msquic.hpp>

#include <algorithm>
#include <chrono>
#include <climits>
#include <cerrno>
#include <cstdlib>
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

struct TqLinuxRelayWorker::StreamRelayBinding {
    TqLinuxRelayWorker* Worker{nullptr};
    std::atomic<RelayState*> Relay{nullptr};
    std::atomic<uint32_t> CallbackRefs{0};
    std::atomic<bool> Closing{false};
};

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
    std::vector<uint8_t> CapturedQuicBytesForTest;
    bool EnableQuicSends{true};
    bool Closing{false};
    uint64_t OutstandingQuicSends{0};
    std::deque<TqBufferView> PendingTcpWrites;
    bool TcpWriteShutdownQueued{false};
    bool TcpWriteArmed{false};
    StreamRelayBinding* StreamBinding{nullptr};
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

    auto relay = std::make_shared<RelayState>(registration, Config);

    epoll_event event{};
    event.events = EPOLLIN | EPOLLRDHUP | EPOLLERR;

    {
        std::lock_guard<std::mutex> guard(RelayLock);
        relay->Id = NextRelayId++;
        event.data.u64 = relay->Id;
        Relays.push_back(relay);
        result.Ok = true;
        result.RelayId = relay->Id;
    }

    if (::epoll_ctl(EpollFd, EPOLL_CTL_ADD, registration.TcpFd, &event) != 0) {
        std::lock_guard<std::mutex> guard(RelayLock);
        for (auto it = Relays.begin(); it != Relays.end(); ++it) {
            if ((*it)->Id == relay->Id) {
                Relays.erase(it);
                break;
            }
        }
        result.Ok = false;
        result.RelayId = 0;
        return result;
    }

    if (registration.Stream != nullptr) {
        std::unique_ptr<StreamRelayBinding> binding(new (std::nothrow) StreamRelayBinding{});
        if (!binding) {
            ::epoll_ctl(EpollFd, EPOLL_CTL_DEL, registration.TcpFd, nullptr);
            std::lock_guard<std::mutex> guard(RelayLock);
            for (auto it = Relays.begin(); it != Relays.end(); ++it) {
                if ((*it)->Id == relay->Id) {
                    Relays.erase(it);
                    break;
                }
            }
            result.Ok = false;
            result.RelayId = 0;
            return result;
        }
        binding->Worker = this;
        binding->Relay.store(relay.get(), std::memory_order_release);
        relay->StreamBinding = binding.get();
        registration.Stream->Callback = TqLinuxRelayWorker::StreamCallback;
        registration.Stream->Context = binding.release();
    }

    return result;
}

bool TqLinuxRelayWorker::RegisterRelay(const TqLinuxRelayRegistration& registration) {
    return RegisterRelayWithId(registration).Ok;
}

bool TqLinuxRelayWorker::RegisterRelayForTest(const TqLinuxRelayRegistration& registration) {
    return RegisterRelay(registration);
}

void TqLinuxRelayWorker::UnregisterRelay(uint64_t relayId) {
    std::shared_ptr<RelayState> removed;
    {
        std::lock_guard<std::mutex> guard(RelayLock);
        for (auto it = Relays.begin(); it != Relays.end(); ++it) {
            if ((*it)->Id == relayId) {
                removed = *it;
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
    auto* binding = removed->StreamBinding;
    if (binding != nullptr) {
        binding->Closing.store(true, std::memory_order_release);
        binding->Relay.store(nullptr, std::memory_order_release);
    }
    if (removed->Stream != nullptr && removed->Stream->Context == binding) {
        removed->Stream->Callback = MsQuicStream::NoOpCallback;
        removed->Stream->Context = nullptr;
    }
    removed->StreamBinding = nullptr;
    if (binding != nullptr) {
        while (binding->CallbackRefs.load(std::memory_order_acquire) != 0) {
            std::this_thread::yield();
        }
        std::lock_guard<std::mutex> guard(RetiredBindingLock);
        RetiredStreamBindings.emplace_back(binding);
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

std::vector<uint8_t> TqLinuxRelayWorker::TakeCapturedQuicBytesForTest(int tcpFd) {
    auto relay = FindRelayByFd(tcpFd);
    if (relay == nullptr) {
        return {};
    }
    std::vector<uint8_t> out;
    out.swap(relay->CapturedQuicBytesForTest);
    return out;
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
                Errors.fetch_add(1);
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
            std::vector<TqBufferView> sendViews;
            if (!BuildTcpToQuicViews(relay, views, sendViews) ||
                !SubmitTcpBatchToQuic(relay, sendViews)) {
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

bool TqLinuxRelayWorker::BuildTcpToQuicViews(
    RelayState* relay,
    std::vector<TqBufferView>& input,
    std::vector<TqBufferView>& output) {
    output.clear();
    if (relay == nullptr) {
        return false;
    }
    if (relay->Compressor == nullptr || relay->CompressAlgo == TqCompressAlgo::None) {
        output = std::move(input);
        return true;
    }

    relay->CompressionOutput.clear();
    for (const auto& view : input) {
        if (!relay->Compressor->Compress(view.Data, view.Len, relay->CompressionOutput, false)) {
            Errors.fetch_add(1);
            return false;
        }
    }
    if (relay->CompressionOutput.empty() &&
        !relay->Compressor->Flush(relay->CompressionOutput)) {
        Errors.fetch_add(1);
        return false;
    }
    if (relay->CompressionOutput.empty()) {
        input.clear();
        return true;
    }

    size_t offset = 0;
    while (offset < relay->CompressionOutput.size()) {
        auto buffer = relay->Pool.Acquire();
        if (!buffer) {
            Errors.fetch_add(1);
            return false;
        }
        const size_t chunk = std::min(buffer->Capacity(), relay->CompressionOutput.size() - offset);
        std::memcpy(buffer->Data(), relay->CompressionOutput.data() + offset, chunk);
        buffer->SetLength(chunk);
        output.push_back(TqBufferView{buffer->Data(), chunk, buffer});
        offset += chunk;
    }
    CompressedTcpBytes.fetch_add(relay->CompressionOutput.size());
    input.clear();
    return true;
}

// In production, ClientContext owns TqLinuxRelaySendOperation until
// QUIC_STREAM_EVENT_SEND_COMPLETE is delivered back to the owner worker.
// Tests can disable sends to verify readv batching without a live MsQuic stream.
bool TqLinuxRelayWorker::SubmitTcpBatchToQuic(RelayState* relay, std::vector<TqBufferView>& views) {
    if (relay == nullptr || views.empty()) {
        return true;
    }
    if (!relay->EnableQuicSends || relay->Stream == nullptr) {
        for (const auto& view : views) {
            relay->CapturedQuicBytesForTest.insert(
                relay->CapturedQuicBytesForTest.end(),
                view.Data,
                view.Data + view.Len);
        }
        views.clear();
        return true;
    }

    std::vector<QUIC_BUFFER> quicBuffers;
    quicBuffers.reserve(views.size());
    for (const auto& view : views) {
        if (view.Len > UINT32_MAX) {
            Errors.fetch_add(1);
            return false;
        }
        QUIC_BUFFER buffer{};
        buffer.Buffer = view.Data;
        buffer.Length = static_cast<uint32_t>(view.Len);
        quicBuffers.push_back(buffer);
    }

    auto* operation = new (std::nothrow) TqLinuxRelaySendOperation{};
    if (operation == nullptr) {
        Errors.fetch_add(1);
        return false;
    }
    operation->RelayId = relay->Id;
    operation->Views = std::move(views);
    operation->QuicBuffers = std::move(quicBuffers);

    const QUIC_STATUS status = relay->Stream->Send(
        operation->QuicBuffers.data(),
        static_cast<uint32_t>(operation->QuicBuffers.size()),
        QUIC_SEND_FLAG_NONE,
        operation);
    if (QUIC_FAILED(status)) {
        delete operation;
        Errors.fetch_add(1);
        return false;
    }
    ++relay->OutstandingQuicSends;
    QuicSendOperations.fetch_add(1);
    return true;
}

void TqLinuxRelayWorker::CompleteQuicSend(void* context) {
    if (context == nullptr) {
        return;
    }
    auto* operation = static_cast<TqLinuxRelaySendOperation*>(context);
    if (operation->Magic != TqLinuxRelaySendOperation::MagicValue) {
        // Late tunnel-phase send completion after relay replaced the stream callback.
        std::free(context);
        return;
    }
    auto relay = FindRelayById(operation->RelayId);
    if (relay != nullptr && relay->OutstandingQuicSends > 0) {
        --relay->OutstandingQuicSends;
    }
    delete operation;
}

std::shared_ptr<TqLinuxRelayWorker::RelayState> TqLinuxRelayWorker::FindRelayById(uint64_t relayId) {
    std::lock_guard<std::mutex> guard(RelayLock);
    for (const auto& relay : Relays) {
        if (relay->Id == relayId) {
            return relay;
        }
    }
    return nullptr;
}

std::shared_ptr<TqLinuxRelayWorker::RelayState> TqLinuxRelayWorker::FindRelayByFd(int tcpFd) {
    std::lock_guard<std::mutex> guard(RelayLock);
    for (const auto& relay : Relays) {
        if (relay->TcpFd == tcpFd) {
            return relay;
        }
    }
    return nullptr;
}

uint64_t TqLinuxRelayWorker::FindRelayIdByStream(MsQuicStream* stream) {
    if (stream == nullptr) {
        return 0;
    }
    StreamLookupScanCount.fetch_add(1);
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
    auto relay = FindRelayById(relayId);
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
            Errors.fetch_add(1);
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
    auto relay = FindRelayByFd(tcpFd);
    if (relay == nullptr) {
        return false;
    }
    if (!EnqueueQuicReceive(relay.get(), data, length, fin)) {
        return false;
    }
    FlushTcpWrites(relay.get());
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

    const uint8_t* writeData = data;
    size_t writeLength = length;
    if (relay->Decompressor != nullptr && relay->CompressAlgo != TqCompressAlgo::None) {
        relay->DecompressionOutput.clear();
        if (!relay->Decompressor->Decompress(data, length, relay->DecompressionOutput)) {
            Errors.fetch_add(1);
            return false;
        }
        DecompressedTcpBytes.fetch_add(relay->DecompressionOutput.size());
        writeData = relay->DecompressionOutput.data();
        writeLength = relay->DecompressionOutput.size();
    }

    size_t offset = 0;
    while (offset < writeLength) {
        auto buffer = relay->Pool.Acquire();
        if (!buffer) {
            Errors.fetch_add(1);
            return false;
        }
        const size_t chunk = std::min(buffer->Capacity(), writeLength - offset);
        std::memcpy(buffer->Data(), writeData + offset, chunk);
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
    event.data.u64 = relay->Id;
    if (::epoll_ctl(EpollFd, EPOLL_CTL_MOD, relay->TcpFd, &event) == 0) {
        relay->TcpWriteArmed = enabled;
    }
}

void TqLinuxRelayWorker::ProcessQuicReceiveEvent(const TqLinuxRelayEvent& event) {
    auto relay = FindRelayById(event.RelayId);
    if (relay == nullptr || relay->Closing) {
        return;
    }
    const bool needsDecompress =
        relay->Decompressor != nullptr && relay->CompressAlgo != TqCompressAlgo::None;
    if (needsDecompress) {
        const uint8_t* data = nullptr;
        size_t length = 0;
        if (event.Buffer) {
            data = event.Buffer->Data();
            length = event.Length;
        }
        if (!EnqueueQuicReceive(relay.get(), data, length, event.Fin)) {
            if (relay->Handle != nullptr) {
                relay->Handle->Stop.store(true);
            }
            return;
        }
    } else {
        if (event.Buffer && event.Length > 0) {
            relay->PendingTcpWrites.push_back(
                TqBufferView{event.Buffer->Data(), event.Length, event.Buffer});
        }
        if (event.Fin) {
            relay->TcpWriteShutdownQueued = true;
        }
    }
    FlushTcpWrites(relay.get());
}

QUIC_STATUS TqLinuxRelayWorker::DispatchStreamEventForTest(
    MsQuicStream* stream,
    QUIC_STREAM_EVENT* event) {
    return OnStreamEvent(stream, event);
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
    snapshot.StreamLookupScanCount = StreamLookupScanCount.load();
    snapshot.CompressedTcpBytes = CompressedTcpBytes.load();
    snapshot.DecompressedTcpBytes = DecompressedTcpBytes.load();
    snapshot.Errors = Errors.load();

    {
        std::lock_guard<std::mutex> relayGuard(RelayLock);
        for (const auto& relay : Relays) {
            snapshot.BufferAcquireCount += relay->Pool.AcquireCount();
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
                auto relay = FindRelayById(events[i].data.u64);
                if (relay == nullptr || relay->Closing) {
                    continue;
                }
                if ((events[i].events & EPOLLOUT) != 0) {
                    FlushTcpWrites(relay.get());
                }
                if ((events[i].events & EPOLLIN) != 0) {
                    DrainTcpReadable(relay.get());
                }
            }
        }
    }
}

QUIC_STATUS QUIC_API TqLinuxRelayWorker::StreamCallback(
    _In_ MsQuicStream* stream,
    _In_opt_ void* context,
    _Inout_ QUIC_STREAM_EVENT* event) noexcept {
    auto* binding = static_cast<StreamRelayBinding*>(context);
    if (binding == nullptr || binding->Worker == nullptr) {
        return QUIC_STATUS_SUCCESS;
    }
    return binding->Worker->OnStreamEventWithBinding(stream, event, binding);
}

QUIC_STATUS TqLinuxRelayWorker::OnStreamEvent(
    MsQuicStream* stream,
    QUIC_STREAM_EVENT* event) noexcept {
    auto* binding = static_cast<StreamRelayBinding*>(stream != nullptr ? stream->Context : nullptr);
    return OnStreamEventWithBinding(stream, event, binding);
}

QUIC_STATUS TqLinuxRelayWorker::OnStreamEventWithBinding(
    MsQuicStream* stream,
    QUIC_STREAM_EVENT* event,
    StreamRelayBinding* binding) noexcept {
    if (event == nullptr) {
        return QUIC_STATUS_SUCCESS;
    }
    if (event->Type == QUIC_STREAM_EVENT_SEND_COMPLETE) {
        TqLinuxRelayEvent queued{};
        queued.Type = TqLinuxRelayEventType::QuicSendComplete;
        queued.Value = reinterpret_cast<uintptr_t>(event->SEND_COMPLETE.ClientContext);
        Enqueue(queued);
        return QUIC_STATUS_SUCCESS;
    }
    if (binding == nullptr || binding->Worker != this) {
        return QUIC_STATUS_SUCCESS;
    }
    binding->CallbackRefs.fetch_add(1, std::memory_order_acq_rel);
    struct CallbackRefGuard {
        StreamRelayBinding* Binding{nullptr};
        ~CallbackRefGuard() {
            Binding->CallbackRefs.fetch_sub(1, std::memory_order_acq_rel);
        }
    } guard{binding};
    RelayState* relay = binding->Relay.load(std::memory_order_acquire);
    if (binding->Closing.load(std::memory_order_acquire) || relay == nullptr) {
        return QUIC_STATUS_SUCCESS;
    }
    const uint64_t relayId = relay->Id;
    if (event->Type == QUIC_STREAM_EVENT_RECEIVE) {
        if (relay->Closing) {
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
        TqLinuxRelayEvent shutdown{};
        shutdown.Type = TqLinuxRelayEventType::Shutdown;
        shutdown.RelayId = relayId;
        Enqueue(shutdown);
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
        total.CompressedTcpBytes += snapshot.CompressedTcpBytes;
        total.DecompressedTcpBytes += snapshot.DecompressedTcpBytes;
        total.Errors += snapshot.Errors;
    }
    return total;
}
