#include "linux_relay_worker.h"

#include <msquic.hpp>

#include <algorithm>
#include <chrono>
#include <climits>
#include <cerrno>
#include <cstdlib>
#include <functional>
#include <cstring>
#include <new>
#include <thread>

#include <fcntl.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/epoll.h>
#include <sys/eventfd.h>
#include <sys/socket.h>
#include <sys/uio.h>
#include <unistd.h>

#if defined(__GNUC__)
__attribute__((weak)) const MsQuicApi* MsQuic = nullptr;
#endif

namespace {

ssize_t WritevNoSignal(int fd, const iovec* iov, int iovcnt) {
    msghdr message{};
    message.msg_iov = const_cast<iovec*>(iov);
    message.msg_iovlen = static_cast<size_t>(iovcnt);
    return ::sendmsg(fd, &message, MSG_NOSIGNAL);
}

void AbandonOrphanedEventBuffers(TqLinuxRelayEvent& event) {
    event.Buffer.abandon();
    for (auto& buffer : event.Buffers) {
        buffer.abandon();
    }
    event.Buffers.clear();
}

void UpdateAtomicMax(std::atomic<uint64_t>& target, uint64_t value) {
    uint64_t previous = target.load(std::memory_order_relaxed);
    while (previous < value &&
           !target.compare_exchange_weak(
               previous, value, std::memory_order_relaxed, std::memory_order_relaxed)) {
    }
}

std::string SocketAddressToString(const sockaddr_storage& storage, socklen_t length) {
    char host[INET6_ADDRSTRLEN]{};
    uint16_t port = 0;
    if (storage.ss_family == AF_INET && length >= sizeof(sockaddr_in)) {
        const auto* addr = reinterpret_cast<const sockaddr_in*>(&storage);
        if (::inet_ntop(AF_INET, &addr->sin_addr, host, sizeof(host)) == nullptr) {
            return "inet:unknown";
        }
        port = ntohs(addr->sin_port);
    } else if (storage.ss_family == AF_INET6 && length >= sizeof(sockaddr_in6)) {
        const auto* addr = reinterpret_cast<const sockaddr_in6*>(&storage);
        if (::inet_ntop(AF_INET6, &addr->sin6_addr, host, sizeof(host)) == nullptr) {
            return "inet6:unknown";
        }
        port = ntohs(addr->sin6_port);
    } else if (storage.ss_family == AF_UNIX) {
        return "unix";
    } else {
        return "family:" + std::to_string(storage.ss_family);
    }
    return std::string(host) + ":" + std::to_string(port);
}

std::string GetSocketNameString(int fd, bool peer) {
    sockaddr_storage storage{};
    socklen_t length = sizeof(storage);
    const int rc = peer
        ? ::getpeername(fd, reinterpret_cast<sockaddr*>(&storage), &length)
        : ::getsockname(fd, reinterpret_cast<sockaddr*>(&storage), &length);
    if (rc != 0) {
        return peer ? "peer:unknown" : "local:unknown";
    }
    return SocketAddressToString(storage, length);
}

}  // namespace

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
    bool SinkQuicReceives{false};
    std::atomic<uint64_t>* SinkQuicReceiveBytes{nullptr};
    bool Closing{false};
    bool TcpReadClosed{false};
    bool TcpWriteClosed{false};
    bool QuicSendFinSubmitted{false};
    bool QuicSendFinCompleted{false};
    uint64_t OutstandingQuicSends{0};
    std::deque<TqBufferView> PendingTcpWrites;
    std::deque<std::shared_ptr<TqPendingQuicReceive>> PendingQuicReceives;
    uint64_t PendingQuicReceiveBytes{0};
    bool QuicReceivePaused{false};
    bool TcpWriteShutdownQueued{false};
    bool TcpReadArmed{true};
    bool TcpWriteArmed{false};
    uint64_t TcpWriteBytes{0};
    uint64_t TcpWriteEagainCount{0};
    uint64_t EpollOutEvents{0};
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
          SinkQuicReceives(registration.SinkQuicReceives),
          SinkQuicReceiveBytes(registration.SinkQuicReceiveBytes),
          Pool(
              config.ReadChunkSize,
              config.WorkerSlots,
              config.MaxPendingBytes) {
        Pool.Reserve(config.WorkerSlots);
    }
};

TqLinuxRelayWorker::TqLinuxRelayWorker(const TqLinuxRelayWorkerConfig& config)
    : Config(config),
      EventQueue(config.EventQueueCapacity) {}

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
    EpollFd = ::epoll_create1(EPOLL_CLOEXEC);
    if (EpollFd < 0) {
        ::close(WakeFd);
        WakeFd = -1;
        Running.store(false);
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

bool TqLinuxRelayWorker::Enqueue(TqLinuxRelayEvent event) {
    RecordEventProducer();
    if (!EventQueue.TryPush(std::move(event))) {
        RecordError(RelayErrorKind::EventQueueFull);
        return false;
    }
    Wake();
    return true;
}

void TqLinuxRelayWorker::RecordError(RelayErrorKind kind) {
    Errors.fetch_add(1);
    switch (kind) {
    case RelayErrorKind::EventQueueFull:
        EventQueueFullErrors.fetch_add(1);
        break;
    case RelayErrorKind::TcpReadBufferAcquire:
        TcpReadBufferAcquireFailures.fetch_add(1);
        break;
    case RelayErrorKind::TcpToQuicCompress:
        TcpToQuicCompressFailures.fetch_add(1);
        break;
    case RelayErrorKind::TcpToQuicBufferAcquire:
        TcpToQuicBufferAcquireFailures.fetch_add(1);
        break;
    case RelayErrorKind::QuicSend:
        QuicSendFailures.fetch_add(1);
        break;
    case RelayErrorKind::QuicReceiveView:
        QuicReceiveViewFailures.fetch_add(1);
        break;
    case RelayErrorKind::QuicReceiveDecompress:
        QuicReceiveDecompressFailures.fetch_add(1);
        break;
    case RelayErrorKind::QuicReceiveTcpBufferAcquire:
        QuicReceiveTcpBufferAcquireFailures.fetch_add(1);
        break;
    case RelayErrorKind::TcpWriteHard:
        TcpWriteHardErrors.fetch_add(1);
        break;
    }
}

void TqLinuxRelayWorker::RecordBufferAcquireFailure(
    RelayErrorKind kind,
    TqBufferAcquireFailure failure) {
    RecordError(kind);

    auto recordByReason = [failure](
        std::atomic<uint64_t>& pendingBudget,
        std::atomic<uint64_t>& slotLimit,
        std::atomic<uint64_t>& alloc) {
        switch (failure) {
        case TqBufferAcquireFailure::PendingBytesLimit:
            pendingBudget.fetch_add(1);
            break;
        case TqBufferAcquireFailure::SlotLimit:
            slotLimit.fetch_add(1);
            break;
        case TqBufferAcquireFailure::AllocationFailure:
            alloc.fetch_add(1);
            break;
        case TqBufferAcquireFailure::None:
            break;
        }
    };

    switch (kind) {
    case RelayErrorKind::TcpReadBufferAcquire:
        recordByReason(
            TcpReadBufferAcquirePendingBudgetFailures,
            TcpReadBufferAcquireSlotLimitFailures,
            TcpReadBufferAcquireAllocFailures);
        break;
    case RelayErrorKind::TcpToQuicBufferAcquire:
        recordByReason(
            TcpToQuicBufferAcquirePendingBudgetFailures,
            TcpToQuicBufferAcquireSlotLimitFailures,
            TcpToQuicBufferAcquireAllocFailures);
        break;
    case RelayErrorKind::QuicReceiveTcpBufferAcquire:
        recordByReason(
            QuicReceiveTcpBufferAcquirePendingBudgetFailures,
            QuicReceiveTcpBufferAcquireSlotLimitFailures,
            QuicReceiveTcpBufferAcquireAllocFailures);
        break;
    default:
        break;
    }
}

void TqLinuxRelayWorker::RecordTcpWriteAttempt(uint64_t bytes) {
    if (bytes == 0) {
        return;
    }
    TcpWriteAttemptBytes.fetch_add(bytes);
    UpdateAtomicMax(MaxTcpWriteAttemptBytes, bytes);
    if (bytes <= 64ull * 1024) {
        TcpWriteAttemptBytesLe64K.fetch_add(1);
    } else if (bytes <= 256ull * 1024) {
        TcpWriteAttemptBytesLe256K.fetch_add(1);
    } else if (bytes <= 1024ull * 1024) {
        TcpWriteAttemptBytesLe1M.fetch_add(1);
    } else if (bytes <= 4ull * 1024 * 1024) {
        TcpWriteAttemptBytesLe4M.fetch_add(1);
    } else {
        TcpWriteAttemptBytesGt4M.fetch_add(1);
    }
}

void TqLinuxRelayWorker::RecordTcpWriteReturned(uint64_t bytes) {
    if (bytes == 0) {
        return;
    }
    if (bytes <= 64ull * 1024) {
        TcpWriteReturnedBytesLe64K.fetch_add(1);
    } else if (bytes <= 256ull * 1024) {
        TcpWriteReturnedBytesLe256K.fetch_add(1);
    } else if (bytes <= 1024ull * 1024) {
        TcpWriteReturnedBytesLe1M.fetch_add(1);
    } else if (bytes <= 4ull * 1024 * 1024) {
        TcpWriteReturnedBytesLe4M.fetch_add(1);
    } else {
        TcpWriteReturnedBytesGt4M.fetch_add(1);
    }
}

void TqLinuxRelayWorker::RecordQuicReceiveView(uint64_t bytes, uint64_t slices) {
    QuicReceiveViewCount.fetch_add(1);
    QuicReceiveViewBytes.fetch_add(bytes);
    UpdateAtomicMax(MaxQuicReceiveViewBytes, bytes);
    UpdateAtomicMax(MaxQuicReceiveViewSlices, slices);
    if (bytes <= 64ull * 1024) {
        QuicReceiveViewBytesLe64K.fetch_add(1);
    } else if (bytes <= 256ull * 1024) {
        QuicReceiveViewBytesLe256K.fetch_add(1);
    } else if (bytes <= 1024ull * 1024) {
        QuicReceiveViewBytesLe1M.fetch_add(1);
    } else if (bytes <= 4ull * 1024 * 1024) {
        QuicReceiveViewBytesLe4M.fetch_add(1);
    } else {
        QuicReceiveViewBytesGt4M.fetch_add(1);
    }

    if (slices <= 1) {
        QuicReceiveViewSlices1.fetch_add(1);
    } else if (slices <= 4) {
        QuicReceiveViewSlices2To4.fetch_add(1);
    } else if (slices <= 16) {
        QuicReceiveViewSlices5To16.fetch_add(1);
    } else {
        QuicReceiveViewSlicesGt16.fetch_add(1);
    }
}

bool TqLinuxRelayWorker::EnqueueForTest(TqLinuxRelayEvent event) {
    return Enqueue(std::move(event));
}

void TqLinuxRelayWorker::RecordEventProducer() {
    if (!Config.TrackEventProducers) {
        return;
    }
    size_t hash = std::hash<std::thread::id>{}(std::this_thread::get_id());
    if (hash == 0) {
        hash = 1;
    }
    size_t expected = 0;
    if (FirstEventProducerHash.compare_exchange_strong(
            expected, hash, std::memory_order_acq_rel, std::memory_order_acquire)) {
        uint64_t count = EventProducerThreadCount.load(std::memory_order_acquire);
        while (count < 1 && !EventProducerThreadCount.compare_exchange_weak(
                                count, 1, std::memory_order_acq_rel, std::memory_order_acquire)) {
        }
        return;
    }
    if (expected != hash && FirstEventProducerHash.load(std::memory_order_acquire) != hash) {
        MultipleEventProducerThreadsObserved.store(true, std::memory_order_release);
        uint64_t count = EventProducerThreadCount.load(std::memory_order_acquire);
        while (count < 2 && !EventProducerThreadCount.compare_exchange_weak(
                               count, 2, std::memory_order_acq_rel, std::memory_order_acquire)) {
        }
    }
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
        if (!EventQueue.TryPop(event)) {
            break;
        }
        switch (event.Type) {
        case TqLinuxRelayEventType::QuicReceive:
            ProcessQuicReceiveEvent(event);
            break;
        case TqLinuxRelayEventType::QuicReceiveView:
            ProcessQuicReceiveViewEvent(event);
            break;
        case TqLinuxRelayEventType::QuicSendComplete:
            CompleteQuicSend(reinterpret_cast<void*>(event.Value));
            break;
        case TqLinuxRelayEventType::TcpWritable: {
            auto relay = FindRelayById(event.RelayId);
            if (relay != nullptr) {
                FlushTcpWrites(relay.get());
                FlushDeferredQuicReceives(relay.get());
                FlushTcpWrites(relay.get());
            }
            break;
        }
        case TqLinuxRelayEventType::Shutdown:
            UnregisterRelay(event.RelayId);
            break;
        default:
            break;
        }
        ++processed;
    }
    EventsProcessed.fetch_add(processed);

    PurgeRetiredRelaysIfIdle();

    WakeArmed.store(false, std::memory_order_release);
    if (EventQueue.SizeApprox() != 0) {
        Wake();
    }
    return processed;
}

void TqLinuxRelayWorker::PurgeRetiredRelaysIfIdle() {
    if (EventQueue.SizeApprox() != 0) {
        return;
    }
    std::lock_guard<std::mutex> guard(RelayLock);
    RetiredRelays.clear();
}

TqLinuxRelayRegistrationResult TqLinuxRelayWorker::RegisterRelayWithId(
    const TqLinuxRelayRegistration& registration) {
    TqLinuxRelayRegistrationResult result{};
    if ((!registration.SinkQuicReceives && registration.TcpFd < 0) ||
        (registration.SinkQuicReceives && registration.Stream == nullptr) ||
        EpollFd < 0) {
        return result;
    }
    if (registration.TcpFd >= 0 && !TqSetNonBlocking(registration.TcpFd)) {
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

    if (registration.TcpFd >= 0 &&
        ::epoll_ctl(EpollFd, EPOLL_CTL_ADD, registration.TcpFd, &event) != 0) {
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
            if (registration.TcpFd >= 0) {
                ::epoll_ctl(EpollFd, EPOLL_CTL_DEL, registration.TcpFd, nullptr);
            }
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
    removed->Stream = nullptr;
    removed->StreamBinding = nullptr;
    if (binding != nullptr) {
        while (binding->CallbackRefs.load(std::memory_order_acquire) != 0) {
            std::this_thread::yield();
        }
        std::lock_guard<std::mutex> guard(RetiredBindingLock);
        RetiredStreamBindings.emplace_back(binding);
    }
    removed->PendingTcpWrites.clear();
    for (auto& pending : removed->PendingQuicReceives) {
        if (pending) {
            FlushDeferredReceiveCompletion(*pending, true);
        }
    }
    removed->PendingQuicReceives.clear();
    removed->PendingQuicReceiveBytes = 0;
    {
        std::lock_guard<std::mutex> guard(RelayLock);
        RetiredRelays.push_back(removed);
    }
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
            ArmTcpReadable(relay, false);
            break;
        }

        std::vector<TqBufferRef> refs;
        std::vector<iovec> iov;
        const size_t maxIov = std::min<size_t>(Config.MaxIov, 1024);
        refs.reserve(maxIov);
        iov.reserve(maxIov);

        for (size_t i = 0; i < maxIov && readBytes + Config.ReadChunkSize <= tickBudget; ++i) {
            TqBufferAcquireFailure acquireFailure = TqBufferAcquireFailure::None;
            auto buffer = relay->Pool.AcquireWorker(&acquireFailure);
            if (!buffer) {
                if (acquireFailure == TqBufferAcquireFailure::PendingBytesLimit ||
                    acquireFailure == TqBufferAcquireFailure::SlotLimit) {
                    ArmTcpReadable(relay, false);
                } else {
                    RecordBufferAcquireFailure(RelayErrorKind::TcpReadBufferAcquire, acquireFailure);
                }
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
                uint8_t* data = ref->Data();
                views.push_back(TqBufferView{data, len, std::move(ref)});
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
            if (!relay->TcpReadClosed) {
                relay->TcpReadClosed = true;
                ArmTcpReadable(relay, false);
                if (!FinishTcpToQuic(relay)) {
                    if (relay->Handle != nullptr) {
                        relay->Handle->Stop.store(true);
                    }
                }
            }
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
            RecordError(RelayErrorKind::TcpToQuicCompress);
            TcpToQuicCompressUpdateFailures.fetch_add(1);
            return false;
        }
    }
    if (relay->CompressionOutput.empty() &&
        !relay->Compressor->Flush(relay->CompressionOutput)) {
        RecordError(RelayErrorKind::TcpToQuicCompress);
        TcpToQuicCompressFlushFailures.fetch_add(1);
        return false;
    }
    if (relay->CompressionOutput.empty()) {
        input.clear();
        return true;
    }

    size_t offset = 0;
    while (offset < relay->CompressionOutput.size()) {
        TqBufferAcquireFailure acquireFailure = TqBufferAcquireFailure::None;
        auto buffer = relay->Pool.AcquireWorker(&acquireFailure);
        if (!buffer) {
            RecordBufferAcquireFailure(
                RelayErrorKind::TcpToQuicBufferAcquire, acquireFailure);
            return false;
        }
        const size_t chunk = std::min(buffer->Capacity(), relay->CompressionOutput.size() - offset);
        std::memcpy(buffer->Data(), relay->CompressionOutput.data() + offset, chunk);
        buffer->SetLength(chunk);
        uint8_t* data = buffer->Data();
        output.push_back(TqBufferView{data, chunk, std::move(buffer)});
        offset += chunk;
    }
    CompressedTcpBytes.fetch_add(relay->CompressionOutput.size());
    input.clear();
    return true;
}

bool TqLinuxRelayWorker::FinishTcpToQuic(RelayState* relay) {
    if (relay == nullptr || relay->Closing) {
        return false;
    }

    std::vector<TqBufferView> sendViews;
    if (relay->Compressor != nullptr && relay->CompressAlgo != TqCompressAlgo::None) {
        relay->CompressionOutput.clear();
        if (!relay->Compressor->Compress(nullptr, 0, relay->CompressionOutput, true)) {
            RecordError(RelayErrorKind::TcpToQuicCompress);
            TcpToQuicCompressFlushFailures.fetch_add(1);
            return false;
        }

        size_t offset = 0;
        while (offset < relay->CompressionOutput.size()) {
            TqBufferAcquireFailure acquireFailure = TqBufferAcquireFailure::None;
            auto buffer = relay->Pool.AcquireWorker(&acquireFailure);
            if (!buffer) {
                RecordBufferAcquireFailure(
                    RelayErrorKind::TcpToQuicBufferAcquire, acquireFailure);
                return false;
            }
            const size_t chunk = std::min(buffer->Capacity(), relay->CompressionOutput.size() - offset);
            std::memcpy(buffer->Data(), relay->CompressionOutput.data() + offset, chunk);
            buffer->SetLength(chunk);
            uint8_t* data = buffer->Data();
            sendViews.push_back(TqBufferView{data, chunk, std::move(buffer)});
            offset += chunk;
        }
        CompressedTcpBytes.fetch_add(relay->CompressionOutput.size());
    }

    return SubmitTcpBatchToQuic(relay, sendViews, QUIC_SEND_FLAG_FIN);
}

void TqLinuxRelayWorker::MaybeStopFullyClosedRelay(RelayState* relay) {
    if (relay == nullptr || relay->Closing || relay->Handle == nullptr) {
        return;
    }
    if (!relay->TcpReadClosed || !relay->TcpWriteClosed) {
        return;
    }
    if (!relay->QuicSendFinSubmitted || !relay->QuicSendFinCompleted) {
        return;
    }
    if (relay->OutstandingQuicSends != 0 ||
        !relay->PendingTcpWrites.empty() ||
        !relay->PendingQuicReceives.empty() ||
        relay->PendingQuicReceiveBytes != 0) {
        return;
    }
    relay->Handle->Stop.store(true, std::memory_order_release);
}

// In production, ClientContext owns TqLinuxRelaySendOperation until
// QUIC_STREAM_EVENT_SEND_COMPLETE is delivered back to the owner worker.
// Tests can disable sends to verify readv batching without a live MsQuic stream.
bool TqLinuxRelayWorker::SubmitTcpBatchToQuic(
    RelayState* relay,
    std::vector<TqBufferView>& views,
    QUIC_SEND_FLAGS flags) {
    if (relay == nullptr) {
        return false;
    }
    if (views.empty()) {
        if (flags == QUIC_SEND_FLAG_FIN) {
            relay->QuicSendFinSubmitted = true;
        }
        if (!relay->EnableQuicSends) {
            if (flags == QUIC_SEND_FLAG_FIN) {
                relay->QuicSendFinCompleted = true;
                MaybeStopFullyClosedRelay(relay);
            }
            return true;
        }
        if (flags == QUIC_SEND_FLAG_FIN) {
            if (relay->Closing || relay->Stream == nullptr || relay->Stream->Handle == nullptr) {
                RecordError(RelayErrorKind::QuicSend);
                return false;
            }
            const QUIC_STATUS status = relay->Stream->Send(nullptr, 0, QUIC_SEND_FLAG_FIN, nullptr);
            if (QUIC_FAILED(status)) {
                RecordError(RelayErrorKind::QuicSend);
                QuicSendApiFailures.fetch_add(1);
                LastQuicSendStatus.store(static_cast<int64_t>(status));
                return false;
            }
            relay->QuicSendFinCompleted = true;
            MaybeStopFullyClosedRelay(relay);
        }
        return true;
    }
    if (!relay->EnableQuicSends) {
        if (flags == QUIC_SEND_FLAG_FIN) {
            relay->QuicSendFinSubmitted = true;
            relay->QuicSendFinCompleted = true;
            MaybeStopFullyClosedRelay(relay);
        }
        for (const auto& view : views) {
            relay->CapturedQuicBytesForTest.insert(
                relay->CapturedQuicBytesForTest.end(),
                view.Data,
                view.Data + view.Len);
        }
        views.clear();
        return true;
    }
    if (relay->Closing || relay->Stream == nullptr || relay->Stream->Handle == nullptr) {
        views.clear();
        RecordError(RelayErrorKind::QuicSend);
        return false;
    }

    std::vector<QUIC_BUFFER> quicBuffers;
    quicBuffers.reserve(views.size());
    for (const auto& view : views) {
        if (view.Len > UINT32_MAX) {
            RecordError(RelayErrorKind::QuicSend);
            QuicSendBufferTooLargeFailures.fetch_add(1);
            return false;
        }
        QUIC_BUFFER buffer{};
        buffer.Buffer = view.Data;
        buffer.Length = static_cast<uint32_t>(view.Len);
        quicBuffers.push_back(buffer);
    }

    auto* operation = new (std::nothrow) TqLinuxRelaySendOperation{};
    if (operation == nullptr) {
        RecordError(RelayErrorKind::QuicSend);
        QuicSendOperationAllocFailures.fetch_add(1);
        return false;
    }
    operation->RelayId = relay->Id;
    operation->Fin = (flags == QUIC_SEND_FLAG_FIN);
    if (operation->Fin) {
        relay->QuicSendFinSubmitted = true;
    }
    operation->Views = std::move(views);
    operation->QuicBuffers = std::move(quicBuffers);
    MsQuicStream* stream = relay->Stream;
    if (stream == nullptr || stream->Handle == nullptr) {
        delete operation;
        RecordError(RelayErrorKind::QuicSend);
        return false;
    }

    const QUIC_STATUS status = stream->Send(
        operation->QuicBuffers.data(),
        static_cast<uint32_t>(operation->QuicBuffers.size()),
        flags,
        operation);
    if (QUIC_FAILED(status)) {
        delete operation;
        RecordError(RelayErrorKind::QuicSend);
        QuicSendApiFailures.fetch_add(1);
        LastQuicSendStatus.store(static_cast<int64_t>(status));
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
    const bool finCompleted = operation->Fin;
    delete operation;
    if (relay != nullptr && !relay->Closing) {
        if (finCompleted) {
            relay->QuicSendFinCompleted = true;
        }
        FlushDeferredQuicReceives(relay.get());
        ArmTcpReadable(relay.get(), true);
        MaybeStopFullyClosedRelay(relay.get());
    }
}

std::shared_ptr<TqLinuxRelayWorker::RelayState> TqLinuxRelayWorker::FindRelayById(uint64_t relayId) {
    std::lock_guard<std::mutex> guard(RelayLock);
    for (const auto& relay : Relays) {
        if (relay->Id == relayId) {
            return relay;
        }
    }
    for (const auto& relay : RetiredRelays) {
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

void TqLinuxRelayWorker::AbortRelayFromCallback(uint64_t relayId, MsQuicStream* stream) {
    auto relay = FindRelayById(relayId);
    if (relay != nullptr && relay->Handle != nullptr) {
        relay->Handle->Stop.store(true);
    }
    if (stream != nullptr && stream->Handle != nullptr) {
        (void)stream->Shutdown(0, QUIC_STREAM_SHUTDOWN_FLAG_ABORT);
    }
    TqLinuxRelayEvent shutdown{};
    shutdown.Type = TqLinuxRelayEventType::Shutdown;
    shutdown.RelayId = relayId;
    (void)Enqueue(std::move(shutdown));
}

bool TqLinuxRelayWorker::QueueDeferredQuicReceive(
    RelayState* relay,
    MsQuicStream* stream,
    const QUIC_BUFFER* buffers,
    uint32_t bufferCount,
    bool fin) {
    return QueueDeferredQuicReceiveFromOffset(relay, stream, buffers, bufferCount, 0, fin);
}

bool TqLinuxRelayWorker::QueueDeferredQuicReceiveFromOffset(
    RelayState* relay,
    MsQuicStream* stream,
    const QUIC_BUFFER* buffers,
    uint32_t bufferCount,
    uint64_t completedPrefix,
    bool fin) {
    if (relay == nullptr || relay->Closing ||
        (bufferCount != 0 && buffers == nullptr)) {
        return false;
    }

    std::shared_ptr<TqPendingQuicReceive> view(new (std::nothrow) TqPendingQuicReceive{});
    if (!view) {
        RecordError(RelayErrorKind::QuicReceiveView);
        QuicReceiveViewAllocFailures.fetch_add(1);
        return false;
    }
    view->Stream = stream;
    view->RelayId = relay->Id;
    view->Fin = fin;
    view->PendingCompleteBytes = completedPrefix;
    view->Slices.reserve(bufferCount);

    uint64_t skip = completedPrefix;
    for (uint32_t i = 0; i < bufferCount; ++i) {
        if (buffers[i].Length == 0) {
            continue;
        }
        if (buffers[i].Buffer == nullptr) {
            RecordError(RelayErrorKind::QuicReceiveView);
            QuicReceiveViewNullBufferFailures.fetch_add(1);
            return false;
        }
        const uint8_t* data = buffers[i].Buffer;
        uint32_t length = buffers[i].Length;
        if (skip >= length) {
            skip -= length;
            continue;
        }
        if (skip > 0) {
            data += skip;
            length -= static_cast<uint32_t>(skip);
            skip = 0;
        }
        view->Slices.push_back(TqQuicReceiveSlice{data, length});
        view->TotalLength += length;
    }
    if (view->TotalLength == 0 && !fin) {
        RecordError(RelayErrorKind::QuicReceiveView);
        QuicReceiveViewEmptyFailures.fetch_add(1);
        return false;
    }
    RecordQuicReceiveView(view->TotalLength, view->Slices.size());

    TqLinuxRelayEvent event{};
    event.Type = TqLinuxRelayEventType::QuicReceiveView;
    event.RelayId = relay->Id;
    event.TotalLength = view->TotalLength;
    event.Fin = fin;
    event.ReceiveView = std::move(view);
    if (!Enqueue(std::move(event))) {
        QuicReceiveViewFailures.fetch_add(1);
        QuicReceiveViewEnqueueFailures.fetch_add(1);
        return false;
    }
    return true;
}

void TqLinuxRelayWorker::CompleteDeferredQuicReceive(MsQuicStream* stream, uint64_t bytes) {
    if (bytes == 0) {
        return;
    }
    DeferredReceiveCompleteBytes.fetch_add(bytes);
    DeferredReceiveCompletes.fetch_add(1);
    if (stream != nullptr && stream->Handle != nullptr) {
        stream->ReceiveComplete(bytes);
    }
}

void TqLinuxRelayWorker::FlushDeferredReceiveCompletion(
    TqPendingQuicReceive& view,
    bool force) {
    if (view.PendingCompleteBytes == 0) {
        return;
    }
    const uint64_t threshold = Config.DeferredReceiveCompleteBatchBytes;
    if (!force && threshold != 0 && view.PendingCompleteBytes < threshold) {
        return;
    }
    CompleteDeferredQuicReceive(view.Stream, view.PendingCompleteBytes);
    view.PendingCompleteBytes = 0;
    DeferredReceiveCompletionFlushes.fetch_add(1);
}

uint64_t TqLinuxRelayWorker::MaxPendingQuicReceiveBytesPerRelay() const {
    if (Config.MaxPendingQuicReceiveBytesPerRelay != 0) {
        return Config.MaxPendingQuicReceiveBytesPerRelay;
    }
    return Config.MaxPendingBytes;
}

uint64_t TqLinuxRelayWorker::LowPendingQuicReceiveBytesPerRelay() const {
    return MaxPendingQuicReceiveBytesPerRelay() / 2;
}

void TqLinuxRelayWorker::SetQuicReceiveEnabled(RelayState* relay, bool enabled) {
    if (relay == nullptr || relay->Stream == nullptr) {
        return;
    }
    if (enabled) {
        QuicReceiveResumedCount.fetch_add(1);
    } else {
        QuicReceivePausedCount.fetch_add(1);
    }
    if (relay->Stream->Handle != nullptr) {
        (void)relay->Stream->ReceiveSetEnabled(enabled);
    }
}

void TqLinuxRelayWorker::MaybePauseQuicReceive(RelayState* relay) {
    if (relay == nullptr || relay->QuicReceivePaused) {
        return;
    }
    if (relay->PendingQuicReceiveBytes >= MaxPendingQuicReceiveBytesPerRelay()) {
        relay->QuicReceivePaused = true;
        SetQuicReceiveEnabled(relay, false);
    }
}

void TqLinuxRelayWorker::MaybeResumeQuicReceive(RelayState* relay) {
    if (relay == nullptr || !relay->QuicReceivePaused) {
        return;
    }
    if (relay->PendingQuicReceiveBytes <= LowPendingQuicReceiveBytesPerRelay()) {
        relay->QuicReceivePaused = false;
        SetQuicReceiveEnabled(relay, true);
    }
}

void TqLinuxRelayWorker::ProcessQuicReceiveViewEvent(TqLinuxRelayEvent& event) {
    auto relay = FindRelayById(event.RelayId);
    if (relay == nullptr || relay->Closing || !event.ReceiveView) {
        return;
    }
    relay->PendingQuicReceiveBytes += event.ReceiveView->TotalLength - event.ReceiveView->CompletedLength;
    relay->PendingQuicReceives.push_back(std::move(event.ReceiveView));
    UpdateAtomicMax(MaxPendingQuicReceiveBytesObserved, relay->PendingQuicReceiveBytes);
    UpdateAtomicMax(MaxPendingQuicReceiveQueueObserved, relay->PendingQuicReceives.size());
    MaybePauseQuicReceive(relay.get());
    FlushDeferredQuicReceives(relay.get());
}

void TqLinuxRelayWorker::FlushDeferredQuicReceives(RelayState* relay) {
    if (relay == nullptr || relay->Closing) {
        return;
    }

    uint64_t burstBytes = 0;
    while (!relay->PendingQuicReceives.empty()) {
        if (Config.TcpWriteBurstBytes != 0 && burstBytes >= Config.TcpWriteBurstBytes) {
            TcpWriteBurstStops.fetch_add(1);
            ArmTcpWritable(relay, true);
            break;
        }

        auto& view = relay->PendingQuicReceives.front();
        if (!view) {
            relay->PendingQuicReceives.pop_front();
            continue;
        }
        FlushDeferredReceiveCompletion(*view, false);

        if (relay->SinkQuicReceives) {
            const uint64_t remaining =
                view->TotalLength >= view->CompletedLength
                    ? view->TotalLength - view->CompletedLength
                    : 0;
            if (remaining > 0) {
                if (relay->SinkQuicReceiveBytes != nullptr) {
                    relay->SinkQuicReceiveBytes->fetch_add(remaining, std::memory_order_relaxed);
                }
                view->PendingCompleteBytes += remaining;
                view->CompletedLength += remaining;
                relay->PendingQuicReceiveBytes =
                    relay->PendingQuicReceiveBytes >= remaining
                        ? relay->PendingQuicReceiveBytes - remaining
                        : 0;
            }
            FlushDeferredReceiveCompletion(*view, true);
            if (view->Fin && relay->Handle != nullptr) {
                relay->Handle->Stop.store(true, std::memory_order_release);
            }
            relay->PendingQuicReceives.pop_front();
            MaybeResumeQuicReceive(relay);
            continue;
        }

        const bool needsDecompress =
            relay->Decompressor != nullptr && relay->CompressAlgo == TqCompressAlgo::Zstd;
        if (needsDecompress) {
            if (DrainCompressedQuicReceiveView(relay, *view)) {
                if (view->Fin) {
                    relay->TcpWriteShutdownQueued = true;
                }
                relay->PendingQuicReceives.pop_front();
                continue;
            }
            break;
        }

        std::vector<iovec> iov;
        iov.reserve(Config.MaxIov);
        size_t sliceIndex = view->SliceIndex;
        size_t sliceOffset = view->SliceOffset;
        uint64_t attemptedBytes = 0;
        uint64_t maxWriteBytes = Config.TcpWriteMaxBytes;
        if (Config.TcpWriteBurstBytes != 0) {
            const uint64_t remainingBurst = Config.TcpWriteBurstBytes - burstBytes;
            maxWriteBytes = maxWriteBytes == 0
                ? remainingBurst
                : std::min(maxWriteBytes, remainingBurst);
        }
        while (sliceIndex < view->Slices.size() &&
               iov.size() < Config.MaxIov &&
               (maxWriteBytes == 0 || attemptedBytes < maxWriteBytes)) {
            const auto& slice = view->Slices[sliceIndex];
            if (sliceOffset >= slice.Length) {
                ++sliceIndex;
                sliceOffset = 0;
                continue;
            }
            uint64_t length = slice.Length - sliceOffset;
            if (maxWriteBytes != 0 && attemptedBytes + length > maxWriteBytes) {
                length = maxWriteBytes - attemptedBytes;
            }
            if (length == 0) {
                break;
            }
            iovec item{};
            item.iov_base = const_cast<uint8_t*>(slice.Data + sliceOffset);
            item.iov_len = static_cast<size_t>(length);
            iov.push_back(item);
            attemptedBytes += length;
            ++sliceIndex;
            sliceOffset = 0;
        }

        if (iov.empty()) {
            FlushDeferredReceiveCompletion(*view, true);
            if (view->Fin) {
                relay->TcpWriteShutdownQueued = true;
            }
            relay->PendingQuicReceives.pop_front();
            continue;
        }

        RecordTcpWriteAttempt(attemptedBytes);
        const ssize_t sent = WritevNoSignal(relay->TcpFd, iov.data(), static_cast<int>(iov.size()));
        if (sent > 0) {
            size_t remaining = static_cast<size_t>(sent);
            burstBytes += static_cast<uint64_t>(sent);
            relay->TcpWriteBytes += static_cast<uint64_t>(sent);
            TcpWriteBytes.fetch_add(static_cast<uint64_t>(sent));
            TcpWriteBatches.fetch_add(1);
            TcpWriteSendmsgCalls.fetch_add(1);
            UpdateAtomicMax(MaxTcpWriteSendmsgBytes, static_cast<uint64_t>(sent));
            RecordTcpWriteReturned(static_cast<uint64_t>(sent));
            if (static_cast<uint64_t>(sent) < attemptedBytes) {
                TcpWritePartialCount.fetch_add(1);
            }
            uint64_t previous = MaxTcpWriteIovUsed.load();
            while (previous < iov.size() &&
                   !MaxTcpWriteIovUsed.compare_exchange_weak(previous, iov.size())) {
            }

            view->PendingCompleteBytes += static_cast<uint64_t>(sent);
            view->CompletedLength += static_cast<uint64_t>(sent);
            relay->PendingQuicReceiveBytes =
                relay->PendingQuicReceiveBytes >= static_cast<uint64_t>(sent)
                    ? relay->PendingQuicReceiveBytes - static_cast<uint64_t>(sent)
                    : 0;

            while (remaining > 0 && view->SliceIndex < view->Slices.size()) {
                const auto& front = view->Slices[view->SliceIndex];
                const size_t available = front.Length - view->SliceOffset;
                if (remaining >= available) {
                    remaining -= available;
                    ++view->SliceIndex;
                    view->SliceOffset = 0;
                } else {
                    view->SliceOffset += remaining;
                    remaining = 0;
                }
            }

            const bool viewComplete = view->CompletedLength >= view->TotalLength;
            FlushDeferredReceiveCompletion(*view, viewComplete);
            MaybeResumeQuicReceive(relay);

            if (viewComplete) {
                if (view->Fin) {
                    relay->TcpWriteShutdownQueued = true;
                }
                relay->PendingQuicReceives.pop_front();
            }
            if (Config.TcpWriteBurstBytes != 0 &&
                burstBytes >= Config.TcpWriteBurstBytes &&
                !relay->PendingQuicReceives.empty()) {
                TcpWriteBurstStops.fetch_add(1);
                ArmTcpWritable(relay, true);
                break;
            }
            continue;
        }
        if (sent < 0 && errno == EINTR) {
            continue;
        }
        if (sent < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            TcpWriteEagainCount.fetch_add(1);
            relay->TcpWriteEagainCount += 1;
            ArmTcpWritable(relay, true);
            break;
        }
        RecordError(RelayErrorKind::TcpWriteHard);
        LastTcpWriteErrno.store(static_cast<uint64_t>(errno));
        if (relay->Handle != nullptr) {
            relay->Handle->Stop.store(true);
        }
        for (auto& pending : relay->PendingQuicReceives) {
            if (pending) {
                FlushDeferredReceiveCompletion(*pending, true);
            }
        }
        relay->PendingQuicReceives.clear();
        relay->PendingQuicReceiveBytes = 0;
        ArmTcpWritable(relay, false);
        break;
    }

    if (relay->PendingQuicReceives.empty() && relay->TcpWriteShutdownQueued) {
        ::shutdown(relay->TcpFd, SHUT_WR);
        relay->TcpWriteShutdownQueued = false;
        relay->TcpWriteClosed = true;
        MaybeStopFullyClosedRelay(relay);
    }
    MaybeResumeQuicReceive(relay);
    if (relay->PendingQuicReceives.empty() && relay->PendingTcpWrites.empty()) {
        ArmTcpWritable(relay, false);
    }
}

bool TqLinuxRelayWorker::DrainCompressedQuicReceiveView(
    RelayState* relay,
    TqPendingQuicReceive& view) {
    if (relay == nullptr || relay->Decompressor == nullptr) {
        return false;
    }

    while (true) {
        const bool hasInput = view.SliceIndex < view.Slices.size();
        const uint8_t* input = nullptr;
        size_t inputLength = 0;
        if (hasInput) {
            const auto& slice = view.Slices[view.SliceIndex];
            if (view.SliceOffset >= slice.Length) {
                ++view.SliceIndex;
                view.SliceOffset = 0;
                continue;
            }
            input = slice.Data + view.SliceOffset;
            inputLength = slice.Length - view.SliceOffset;
        }

        TqBufferAcquireFailure acquireFailure = TqBufferAcquireFailure::None;
        auto output = relay->Pool.AcquireWorker(&acquireFailure);
        if (!output) {
            RecordBufferAcquireFailure(RelayErrorKind::QuicReceiveTcpBufferAcquire, acquireFailure);
            ArmTcpWritable(relay, true);
            return false;
        }

        TqDecompressResult result{};
        ZstdDecompressCalls.fetch_add(1);
        if (!relay->Decompressor->DecompressInto(
                input,
                inputLength,
                output->Data(),
                output->Capacity(),
                &result)) {
            ZstdDecompressFailures.fetch_add(1);
            RecordError(RelayErrorKind::QuicReceiveDecompress);
            if (relay->Handle != nullptr) {
                relay->Handle->Stop.store(true);
            }
            return false;
        }
        if (result.InputConsumed > inputLength || result.OutputProduced > output->Capacity()) {
            ZstdDecompressFailures.fetch_add(1);
            RecordError(RelayErrorKind::QuicReceiveDecompress);
            if (relay->Handle != nullptr) {
                relay->Handle->Stop.store(true);
            }
            return false;
        }
        if (result.NeedsMoreInput) {
            ZstdDecompressNeedInput.fetch_add(1);
        }
        if (result.NeedsMoreOutput) {
            ZstdDecompressNeedOutput.fetch_add(1);
        }

        if (result.InputConsumed > 0) {
            ZstdDecompressInputBytes.fetch_add(result.InputConsumed);
            view.PendingCompleteBytes += static_cast<uint64_t>(result.InputConsumed);
            view.CompletedLength += static_cast<uint64_t>(result.InputConsumed);
            relay->PendingQuicReceiveBytes =
                relay->PendingQuicReceiveBytes >= static_cast<uint64_t>(result.InputConsumed)
                    ? relay->PendingQuicReceiveBytes - static_cast<uint64_t>(result.InputConsumed)
                    : 0;
            view.SliceOffset += result.InputConsumed;
            while (view.SliceIndex < view.Slices.size()) {
                const auto& slice = view.Slices[view.SliceIndex];
                if (view.SliceOffset < slice.Length) {
                    break;
                }
                view.SliceOffset = 0;
                ++view.SliceIndex;
            }
            FlushDeferredReceiveCompletion(view, false);
            MaybeResumeQuicReceive(relay);
        }

        if (result.OutputProduced > 0) {
            output->SetLength(result.OutputProduced);
            uint8_t* data = output->Data();
            relay->PendingTcpWrites.push_back(
                TqBufferView{data, result.OutputProduced, std::move(output)});
            DecompressedTcpBytes.fetch_add(result.OutputProduced);
            ZstdDecompressOutputBytes.fetch_add(result.OutputProduced);
            FlushTcpWrites(relay);
            if (!relay->PendingTcpWrites.empty()) {
                return false;
            }
        }

        if (result.InputConsumed == 0 && result.OutputProduced == 0) {
            if (!hasInput && result.NeedsMoreInput) {
                break;
            }
            ZstdDecompressFailures.fetch_add(1);
            RecordError(RelayErrorKind::QuicReceiveDecompress);
            if (relay->Handle != nullptr) {
                relay->Handle->Stop.store(true);
            }
            return false;
        }

        if (view.SliceIndex >= view.Slices.size() &&
            result.OutputProduced == 0 &&
            result.NeedsMoreInput) {
            break;
        }
    }

    FlushDeferredReceiveCompletion(view, true);
    return true;
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

bool TqLinuxRelayWorker::FlushTcpWritableForTest(int tcpFd) {
    auto relay = FindRelayByFd(tcpFd);
    if (relay == nullptr) {
        return false;
    }
    FlushTcpWrites(relay.get());
    FlushDeferredQuicReceives(relay.get());
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
        ZstdDecompressCalls.fetch_add(1);
        if (!relay->Decompressor->Decompress(data, length, relay->DecompressionOutput)) {
            ZstdDecompressFailures.fetch_add(1);
            RecordError(RelayErrorKind::QuicReceiveDecompress);
            return false;
        }
        DecompressedTcpBytes.fetch_add(relay->DecompressionOutput.size());
        ZstdDecompressInputBytes.fetch_add(length);
        ZstdDecompressOutputBytes.fetch_add(relay->DecompressionOutput.size());
        writeData = relay->DecompressionOutput.data();
        writeLength = relay->DecompressionOutput.size();
    }

    size_t offset = 0;
    while (offset < writeLength) {
        TqBufferAcquireFailure acquireFailure = TqBufferAcquireFailure::None;
        auto buffer = relay->Pool.AcquireWorker(&acquireFailure);
        if (!buffer) {
            RecordBufferAcquireFailure(
                RelayErrorKind::QuicReceiveTcpBufferAcquire, acquireFailure);
            return false;
        }
        const size_t chunk = std::min(buffer->Capacity(), writeLength - offset);
        std::memcpy(buffer->Data(), writeData + offset, chunk);
        buffer->SetLength(chunk);
        uint8_t* data = buffer->Data();
        relay->PendingTcpWrites.push_back(TqBufferView{data, chunk, std::move(buffer)});
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

    uint64_t burstBytes = 0;
    while (!relay->PendingTcpWrites.empty()) {
        if (Config.TcpWriteBurstBytes != 0 && burstBytes >= Config.TcpWriteBurstBytes) {
            TcpWriteBurstStops.fetch_add(1);
            ArmTcpWritable(relay, true);
            break;
        }

        std::vector<iovec> iov;
        iov.reserve(Config.MaxIov);
        uint64_t attemptedBytes = 0;
        uint64_t maxWriteBytes = Config.TcpWriteMaxBytes;
        if (Config.TcpWriteBurstBytes != 0) {
            const uint64_t remainingBurst = Config.TcpWriteBurstBytes - burstBytes;
            maxWriteBytes = maxWriteBytes == 0
                ? remainingBurst
                : std::min(maxWriteBytes, remainingBurst);
        }
        for (const auto& view : relay->PendingTcpWrites) {
            if (iov.size() >= Config.MaxIov ||
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
            attemptedBytes += length;
        }

        if (iov.empty()) {
            break;
        }
        RecordTcpWriteAttempt(attemptedBytes);
        const ssize_t sent = WritevNoSignal(relay->TcpFd, iov.data(), static_cast<int>(iov.size()));
        if (sent > 0) {
            size_t remaining = static_cast<size_t>(sent);
            burstBytes += static_cast<uint64_t>(sent);
            relay->TcpWriteBytes += static_cast<uint64_t>(sent);
            TcpWriteBytes.fetch_add(static_cast<uint64_t>(sent));
            TcpWriteBatches.fetch_add(1);
            TcpWriteSendmsgCalls.fetch_add(1);
            UpdateAtomicMax(MaxTcpWriteSendmsgBytes, static_cast<uint64_t>(sent));
            RecordTcpWriteReturned(static_cast<uint64_t>(sent));
            if (static_cast<uint64_t>(sent) < attemptedBytes) {
                TcpWritePartialCount.fetch_add(1);
            }
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
            if (Config.TcpWriteBurstBytes != 0 &&
                burstBytes >= Config.TcpWriteBurstBytes &&
                !relay->PendingTcpWrites.empty()) {
                TcpWriteBurstStops.fetch_add(1);
                ArmTcpWritable(relay, true);
                break;
            }
            continue;
        }
        if (sent < 0 && errno == EINTR) {
            continue;
        }
        if (sent < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            TcpWriteEagainCount.fetch_add(1);
            relay->TcpWriteEagainCount += 1;
            ArmTcpWritable(relay, true);
            break;
        }
        RecordError(RelayErrorKind::TcpWriteHard);
        LastTcpWriteErrno.store(static_cast<uint64_t>(errno));
        break;
    }

    if (relay->PendingTcpWrites.empty() && relay->TcpWriteShutdownQueued) {
        ::shutdown(relay->TcpFd, SHUT_WR);
        relay->TcpWriteShutdownQueued = false;
        relay->TcpWriteClosed = true;
        MaybeStopFullyClosedRelay(relay);
    }
    if (relay->PendingTcpWrites.empty() && relay->PendingQuicReceives.empty()) {
        ArmTcpWritable(relay, false);
    }
}

void TqLinuxRelayWorker::ArmTcpReadable(RelayState* relay, bool enabled) {
    if (relay == nullptr || relay->TcpFd < 0 || EpollFd < 0 || relay->TcpReadArmed == enabled) {
        return;
    }
    relay->TcpReadArmed = enabled;
    if (!enabled) {
        ReadDisabledCount.fetch_add(1);
    }
    UpdateTcpInterest(relay);
}

void TqLinuxRelayWorker::ArmTcpWritable(RelayState* relay, bool enabled) {
    if (relay == nullptr || relay->TcpFd < 0 || EpollFd < 0 || relay->TcpWriteArmed == enabled) {
        return;
    }
    relay->TcpWriteArmed = enabled;
    UpdateTcpInterest(relay);
}

void TqLinuxRelayWorker::UpdateTcpInterest(RelayState* relay) {
    if (relay == nullptr || relay->TcpFd < 0 || EpollFd < 0) {
        return;
    }
    epoll_event event{};
    event.events = EPOLLRDHUP | EPOLLERR;
    if (relay->TcpReadArmed) {
        event.events |= EPOLLIN;
    }
    if (relay->TcpWriteArmed) {
        event.events |= EPOLLOUT;
    }
    event.data.u64 = relay->Id;
    (void)::epoll_ctl(EpollFd, EPOLL_CTL_MOD, relay->TcpFd, &event);
}

void TqLinuxRelayWorker::ProcessQuicReceiveEvent(TqLinuxRelayEvent& event) {
    auto relay = FindRelayById(event.RelayId);
    if (relay == nullptr) {
        AbandonOrphanedEventBuffers(event);
        return;
    }
    if (relay->Closing) {
        return;
    }
    const bool needsDecompress =
        relay->Decompressor != nullptr && relay->CompressAlgo != TqCompressAlgo::None;
    if (needsDecompress) {
        if (!event.Buffers.empty()) {
            for (size_t i = 0; i < event.Buffers.size(); ++i) {
                auto& buffer = event.Buffers[i];
                if (!buffer) {
                    continue;
                }
                const bool fin = event.Fin && i + 1 == event.Buffers.size();
                if (!EnqueueQuicReceive(relay.get(), buffer->Data(), buffer->Length(), fin)) {
                    if (relay->Handle != nullptr) {
                        relay->Handle->Stop.store(true);
                    }
                    return;
                }
            }
            if (event.Fin && !relay->TcpWriteShutdownQueued) {
                relay->TcpWriteShutdownQueued = true;
            }
        } else {
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
        }
    } else {
        if (!event.Buffers.empty()) {
            for (auto& buffer : event.Buffers) {
                if (buffer && buffer->Length() > 0) {
                    uint8_t* data = buffer->Data();
                    const size_t length = buffer->Length();
                    relay->PendingTcpWrites.push_back(
                        TqBufferView{data, length, std::move(buffer)});
                }
            }
        } else if (event.Buffer && event.Length > 0) {
            uint8_t* data = event.Buffer->Data();
            relay->PendingTcpWrites.push_back(
                TqBufferView{data, event.Length, std::move(event.Buffer)});
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
    TqLinuxRelayWorkerSnapshot snapshot{};
    snapshot.EventsProcessed = EventsProcessed.load();
    snapshot.WakeupWrites = WakeupWrites.load();
    snapshot.PendingEvents = EventQueue.SizeApprox();
    snapshot.TcpReadBatches = TcpReadBatches.load();
    snapshot.TcpReadBytes = TcpReadBytes.load();
    snapshot.QuicSendOperations = QuicSendOperations.load();
    snapshot.MaxTcpReadIovUsed = MaxTcpReadIovUsed.load();
    snapshot.TcpWriteBatches = TcpWriteBatches.load();
    snapshot.TcpWriteBytes = TcpWriteBytes.load();
    snapshot.MaxTcpWriteIovUsed = MaxTcpWriteIovUsed.load();
    snapshot.TcpWriteSendmsgCalls = TcpWriteSendmsgCalls.load();
    snapshot.TcpWriteAttemptBytes = TcpWriteAttemptBytes.load();
    snapshot.MaxTcpWriteAttemptBytes = MaxTcpWriteAttemptBytes.load();
    snapshot.MaxTcpWriteSendmsgBytes = MaxTcpWriteSendmsgBytes.load();
    snapshot.TcpWriteAttemptBytesLe64K = TcpWriteAttemptBytesLe64K.load();
    snapshot.TcpWriteAttemptBytesLe256K = TcpWriteAttemptBytesLe256K.load();
    snapshot.TcpWriteAttemptBytesLe1M = TcpWriteAttemptBytesLe1M.load();
    snapshot.TcpWriteAttemptBytesLe4M = TcpWriteAttemptBytesLe4M.load();
    snapshot.TcpWriteAttemptBytesGt4M = TcpWriteAttemptBytesGt4M.load();
    snapshot.TcpWriteReturnedBytesLe64K = TcpWriteReturnedBytesLe64K.load();
    snapshot.TcpWriteReturnedBytesLe256K = TcpWriteReturnedBytesLe256K.load();
    snapshot.TcpWriteReturnedBytesLe1M = TcpWriteReturnedBytesLe1M.load();
    snapshot.TcpWriteReturnedBytesLe4M = TcpWriteReturnedBytesLe4M.load();
    snapshot.TcpWriteReturnedBytesGt4M = TcpWriteReturnedBytesGt4M.load();
    snapshot.TcpWriteEagainCount = TcpWriteEagainCount.load();
    snapshot.TcpWritePartialCount = TcpWritePartialCount.load();
    snapshot.TcpWriteBurstStops = TcpWriteBurstStops.load();
    snapshot.ReadDisabledCount = ReadDisabledCount.load();
    snapshot.StreamLookupScanCount = StreamLookupScanCount.load();
    snapshot.CompressedTcpBytes = CompressedTcpBytes.load();
    snapshot.DecompressedTcpBytes = DecompressedTcpBytes.load();
    snapshot.ZstdDecompressInputBytes = ZstdDecompressInputBytes.load();
    snapshot.ZstdDecompressOutputBytes = ZstdDecompressOutputBytes.load();
    snapshot.ZstdDecompressCalls = ZstdDecompressCalls.load();
    snapshot.ZstdDecompressNeedInput = ZstdDecompressNeedInput.load();
    snapshot.ZstdDecompressNeedOutput = ZstdDecompressNeedOutput.load();
    snapshot.ZstdDecompressFailures = ZstdDecompressFailures.load();
    snapshot.DeferredReceiveCompleteBytes = DeferredReceiveCompleteBytes.load();
    snapshot.DeferredReceiveCompletes = DeferredReceiveCompletes.load();
    snapshot.DeferredReceiveCompletionFlushes = DeferredReceiveCompletionFlushes.load();
    snapshot.MaxPendingQuicReceiveBytes = MaxPendingQuicReceiveBytesObserved.load();
    snapshot.MaxPendingQuicReceiveQueue = MaxPendingQuicReceiveQueueObserved.load();
    snapshot.QuicReceiveViewCount = QuicReceiveViewCount.load();
    snapshot.QuicReceiveViewBytes = QuicReceiveViewBytes.load();
    snapshot.MaxQuicReceiveViewBytes = MaxQuicReceiveViewBytes.load();
    snapshot.MaxQuicReceiveViewSlices = MaxQuicReceiveViewSlices.load();
    snapshot.QuicReceiveViewBytesLe64K = QuicReceiveViewBytesLe64K.load();
    snapshot.QuicReceiveViewBytesLe256K = QuicReceiveViewBytesLe256K.load();
    snapshot.QuicReceiveViewBytesLe1M = QuicReceiveViewBytesLe1M.load();
    snapshot.QuicReceiveViewBytesLe4M = QuicReceiveViewBytesLe4M.load();
    snapshot.QuicReceiveViewBytesGt4M = QuicReceiveViewBytesGt4M.load();
    snapshot.QuicReceiveViewSlices1 = QuicReceiveViewSlices1.load();
    snapshot.QuicReceiveViewSlices2To4 = QuicReceiveViewSlices2To4.load();
    snapshot.QuicReceiveViewSlices5To16 = QuicReceiveViewSlices5To16.load();
    snapshot.QuicReceiveViewSlicesGt16 = QuicReceiveViewSlicesGt16.load();
    snapshot.QuicReceivePausedCount = QuicReceivePausedCount.load();
    snapshot.QuicReceiveResumedCount = QuicReceiveResumedCount.load();
    snapshot.Errors = Errors.load();
    snapshot.EventQueueFullErrors = EventQueueFullErrors.load();
    snapshot.TcpReadBufferAcquireFailures = TcpReadBufferAcquireFailures.load();
    snapshot.TcpReadBufferAcquirePendingBudgetFailures =
        TcpReadBufferAcquirePendingBudgetFailures.load();
    snapshot.TcpReadBufferAcquireSlotLimitFailures =
        TcpReadBufferAcquireSlotLimitFailures.load();
    snapshot.TcpReadBufferAcquireAllocFailures = TcpReadBufferAcquireAllocFailures.load();
    snapshot.TcpToQuicCompressFailures = TcpToQuicCompressFailures.load();
    snapshot.TcpToQuicCompressUpdateFailures = TcpToQuicCompressUpdateFailures.load();
    snapshot.TcpToQuicCompressFlushFailures = TcpToQuicCompressFlushFailures.load();
    snapshot.TcpToQuicBufferAcquireFailures = TcpToQuicBufferAcquireFailures.load();
    snapshot.TcpToQuicBufferAcquirePendingBudgetFailures =
        TcpToQuicBufferAcquirePendingBudgetFailures.load();
    snapshot.TcpToQuicBufferAcquireSlotLimitFailures =
        TcpToQuicBufferAcquireSlotLimitFailures.load();
    snapshot.TcpToQuicBufferAcquireAllocFailures =
        TcpToQuicBufferAcquireAllocFailures.load();
    snapshot.QuicSendFailures = QuicSendFailures.load();
    snapshot.QuicSendBufferTooLargeFailures = QuicSendBufferTooLargeFailures.load();
    snapshot.QuicSendOperationAllocFailures = QuicSendOperationAllocFailures.load();
    snapshot.QuicSendApiFailures = QuicSendApiFailures.load();
    snapshot.QuicReceiveViewFailures = QuicReceiveViewFailures.load();
    snapshot.QuicReceiveViewAllocFailures = QuicReceiveViewAllocFailures.load();
    snapshot.QuicReceiveViewNullBufferFailures = QuicReceiveViewNullBufferFailures.load();
    snapshot.QuicReceiveViewEmptyFailures = QuicReceiveViewEmptyFailures.load();
    snapshot.QuicReceiveViewEnqueueFailures = QuicReceiveViewEnqueueFailures.load();
    snapshot.QuicReceiveDecompressFailures = QuicReceiveDecompressFailures.load();
    snapshot.QuicReceiveTcpBufferAcquireFailures = QuicReceiveTcpBufferAcquireFailures.load();
    snapshot.QuicReceiveTcpBufferAcquirePendingBudgetFailures =
        QuicReceiveTcpBufferAcquirePendingBudgetFailures.load();
    snapshot.QuicReceiveTcpBufferAcquireSlotLimitFailures =
        QuicReceiveTcpBufferAcquireSlotLimitFailures.load();
    snapshot.QuicReceiveTcpBufferAcquireAllocFailures =
        QuicReceiveTcpBufferAcquireAllocFailures.load();
    snapshot.TcpWriteHardErrors = TcpWriteHardErrors.load();
    snapshot.LastTcpWriteErrno = LastTcpWriteErrno.load();
    snapshot.LastQuicSendStatus = LastQuicSendStatus.load();
    snapshot.EventProducerThreadsObserved = EventProducerThreadCount.load();
    snapshot.MultipleEventProducerThreadsObserved =
        MultipleEventProducerThreadsObserved.load(std::memory_order_acquire);

    {
        std::lock_guard<std::mutex> relayGuard(RelayLock);
        snapshot.ActiveRelays = Relays.size();
        snapshot.MaxWorkerActiveRelays = snapshot.ActiveRelays;
        bool hasHotRelay = false;
        for (const auto& relay : Relays) {
            uint64_t relayPendingBytes = 0;
            snapshot.BufferAcquireCount += relay->Pool.AcquireCount();
            snapshot.WorkerSlotsAllocated += relay->Pool.AllocatedCount();
            snapshot.WorkerSlotsFree += relay->Pool.FreeCount();
            if (relay->TcpFd >= 0) {
                ++snapshot.ActiveTcpRelays;
            }
            if (relay->SinkQuicReceives) {
                ++snapshot.ActiveSinkRelays;
            }
            if (relay->EnableQuicSends) {
                ++snapshot.ActiveQuicSendRelays;
            }
            if (relay->TcpReadArmed) {
                ++snapshot.TcpReadArmedRelays;
            } else if (relay->TcpFd >= 0) {
                ++snapshot.TcpReadDisabledRelays;
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
            if (relay->TcpWriteShutdownQueued) {
                ++snapshot.TcpWriteShutdownQueuedRelays;
            }
            snapshot.OutstandingQuicSends += relay->OutstandingQuicSends;
            snapshot.PendingTcpWriteQueue += relay->PendingTcpWrites.size();
            relayPendingBytes += relay->Pool.PendingBytes();
            for (const auto& view : relay->PendingTcpWrites) {
                relayPendingBytes += view.Len;
                snapshot.PendingTcpWriteBytes += view.Len;
            }
            relayPendingBytes += relay->PendingQuicReceiveBytes;
            snapshot.CurrentPendingQuicReceiveBytes += relay->PendingQuicReceiveBytes;
            snapshot.CurrentPendingQuicReceiveQueue += relay->PendingQuicReceives.size();
            snapshot.PendingBytes += relayPendingBytes;
            snapshot.MaxRelayPendingQuicReceiveBytes = std::max(
                snapshot.MaxRelayPendingQuicReceiveBytes,
                relay->PendingQuicReceiveBytes);
            snapshot.MaxRelayPendingQuicReceiveQueue = std::max<uint64_t>(
                snapshot.MaxRelayPendingQuicReceiveQueue,
                relay->PendingQuicReceives.size());
            snapshot.MaxRelayTcpWriteEagainCount = std::max(
                snapshot.MaxRelayTcpWriteEagainCount,
                relay->TcpWriteEagainCount);
            if (!hasHotRelay ||
                relay->PendingQuicReceiveBytes > snapshot.HotRelayPendingQuicReceiveBytes ||
                (relay->PendingQuicReceiveBytes == snapshot.HotRelayPendingQuicReceiveBytes &&
                 relay->TcpWriteEagainCount > snapshot.HotRelayTcpWriteEagainCount)) {
                hasHotRelay = true;
                snapshot.HotRelayId = relay->Id;
                snapshot.HotRelayWorkerIndex = Config.WorkerIndex;
                snapshot.HotRelayTcpFd = relay->TcpFd;
                snapshot.HotRelayPendingQuicReceiveBytes = relay->PendingQuicReceiveBytes;
                snapshot.HotRelayPendingQuicReceiveQueue = relay->PendingQuicReceives.size();
                snapshot.HotRelayTcpWriteBytes = relay->TcpWriteBytes;
                snapshot.HotRelayTcpWriteEagainCount = relay->TcpWriteEagainCount;
                snapshot.HotRelayEpollOutEvents = relay->EpollOutEvents;
                snapshot.HotRelayTcpReadArmed = relay->TcpReadArmed;
                snapshot.HotRelayTcpWriteArmed = relay->TcpWriteArmed;
                snapshot.HotRelayLocalAddress = GetSocketNameString(relay->TcpFd, false);
                snapshot.HotRelayPeerAddress = GetSocketNameString(relay->TcpFd, true);
            }
        }
        snapshot.MaxWorkerPendingBytes = snapshot.PendingBytes;
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
                    relay->EpollOutEvents += 1;
                    FlushTcpWrites(relay.get());
                    FlushDeferredQuicReceives(relay.get());
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
        if (!Enqueue(std::move(queued))) {
            return QUIC_STATUS_OUT_OF_MEMORY;
        }
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
        const bool fin = (event->RECEIVE.Flags & QUIC_RECEIVE_FLAG_FIN) != 0;
        if (!QueueDeferredQuicReceive(
                relay,
                stream,
                event->RECEIVE.Buffers,
                event->RECEIVE.BufferCount,
                fin)) {
            AbortRelayFromCallback(relayId, stream);
        }
        return QUIC_STATUS_PENDING;
    }
    if (event->Type == QUIC_STREAM_EVENT_PEER_SEND_ABORTED) {
        return QUIC_STATUS_SUCCESS;
    }
    if (event->Type == QUIC_STREAM_EVENT_SHUTDOWN_COMPLETE ||
        event->Type == QUIC_STREAM_EVENT_PEER_RECEIVE_ABORTED) {
        binding->Closing.store(true, std::memory_order_release);
        binding->Relay.store(nullptr, std::memory_order_release);
        relay->Stream = nullptr;
        relay->Closing = true;
        TqLinuxRelayEvent shutdown{};
        shutdown.Type = TqLinuxRelayEventType::Shutdown;
        shutdown.RelayId = relayId;
        if (!Enqueue(std::move(shutdown))) {
            return QUIC_STATUS_OUT_OF_MEMORY;
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
        config.WorkerIndex = i;
        config.ByteBudgetPerTick = tuning.LinuxRelayWorkerByteBudgetPerTick;
        config.ReadChunkSize = tuning.LinuxRelayReadChunkSize;
        config.ReadBatchBytes = tuning.LinuxRelayReadBatchBytes;
        config.MaxIov = tuning.LinuxRelayMaxIov;
        config.WorkerSlots = tuning.LinuxRelayWorkerSlots;
        config.TcpWriteMaxBytes = tuning.LinuxRelayTcpWriteMaxBytes;
        config.TcpWriteBurstBytes = tuning.LinuxRelayTcpWriteBurstBytes;
        config.MaxPendingBytes = tuning.LinuxRelayPerWorkerPendingBytes;
        config.MaxPendingQuicReceiveBytesPerRelay = tuning.LinuxRelayPerTunnelPendingBytes;
        config.DeferredReceiveCompleteBatchBytes = tuning.LinuxRelayQuicReceiveCompleteBatchBytes;
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
        total.ActiveRelays += snapshot.ActiveRelays;
        total.ActiveTcpRelays += snapshot.ActiveTcpRelays;
        total.ActiveSinkRelays += snapshot.ActiveSinkRelays;
        total.ActiveQuicSendRelays += snapshot.ActiveQuicSendRelays;
        total.CurrentPendingQuicReceiveBytes += snapshot.CurrentPendingQuicReceiveBytes;
        total.CurrentPendingQuicReceiveQueue += snapshot.CurrentPendingQuicReceiveQueue;
        total.WorkerSlotsAllocated += snapshot.WorkerSlotsAllocated;
        total.WorkerSlotsFree += snapshot.WorkerSlotsFree;
        total.TcpReadArmedRelays += snapshot.TcpReadArmedRelays;
        total.TcpReadDisabledRelays += snapshot.TcpReadDisabledRelays;
        total.TcpWriteArmedRelays += snapshot.TcpWriteArmedRelays;
        total.ClosingRelays += snapshot.ClosingRelays;
        total.TcpReadClosedRelays += snapshot.TcpReadClosedRelays;
        total.TcpWriteShutdownQueuedRelays += snapshot.TcpWriteShutdownQueuedRelays;
        total.OutstandingQuicSends += snapshot.OutstandingQuicSends;
        total.PendingTcpWriteQueue += snapshot.PendingTcpWriteQueue;
        total.PendingTcpWriteBytes += snapshot.PendingTcpWriteBytes;
        total.MaxWorkerPendingBytes = std::max(total.MaxWorkerPendingBytes, snapshot.MaxWorkerPendingBytes);
        total.MaxWorkerActiveRelays = std::max(total.MaxWorkerActiveRelays, snapshot.MaxWorkerActiveRelays);
        total.MaxRelayPendingQuicReceiveBytes = std::max(total.MaxRelayPendingQuicReceiveBytes, snapshot.MaxRelayPendingQuicReceiveBytes);
        total.MaxRelayPendingQuicReceiveQueue = std::max(total.MaxRelayPendingQuicReceiveQueue, snapshot.MaxRelayPendingQuicReceiveQueue);
        total.MaxRelayTcpWriteEagainCount = std::max(total.MaxRelayTcpWriteEagainCount, snapshot.MaxRelayTcpWriteEagainCount);
        if (snapshot.HotRelayId != 0 &&
            (total.HotRelayId == 0 ||
             snapshot.HotRelayPendingQuicReceiveBytes > total.HotRelayPendingQuicReceiveBytes ||
             (snapshot.HotRelayPendingQuicReceiveBytes == total.HotRelayPendingQuicReceiveBytes &&
              snapshot.HotRelayTcpWriteEagainCount > total.HotRelayTcpWriteEagainCount))) {
            total.HotRelayId = snapshot.HotRelayId;
            total.HotRelayWorkerIndex = snapshot.HotRelayWorkerIndex;
            total.HotRelayTcpFd = snapshot.HotRelayTcpFd;
            total.HotRelayPendingQuicReceiveBytes = snapshot.HotRelayPendingQuicReceiveBytes;
            total.HotRelayPendingQuicReceiveQueue = snapshot.HotRelayPendingQuicReceiveQueue;
            total.HotRelayTcpWriteBytes = snapshot.HotRelayTcpWriteBytes;
            total.HotRelayTcpWriteEagainCount = snapshot.HotRelayTcpWriteEagainCount;
            total.HotRelayEpollOutEvents = snapshot.HotRelayEpollOutEvents;
            total.HotRelayTcpReadArmed = snapshot.HotRelayTcpReadArmed;
            total.HotRelayTcpWriteArmed = snapshot.HotRelayTcpWriteArmed;
            total.HotRelayLocalAddress = snapshot.HotRelayLocalAddress;
            total.HotRelayPeerAddress = snapshot.HotRelayPeerAddress;
        }
        total.TcpReadBatches += snapshot.TcpReadBatches;
        total.TcpReadBytes += snapshot.TcpReadBytes;
        total.QuicSendOperations += snapshot.QuicSendOperations;
        total.MaxTcpReadIovUsed = std::max(total.MaxTcpReadIovUsed, snapshot.MaxTcpReadIovUsed);
        total.TcpWriteBatches += snapshot.TcpWriteBatches;
        total.TcpWriteBytes += snapshot.TcpWriteBytes;
        total.MaxTcpWriteIovUsed = std::max(total.MaxTcpWriteIovUsed, snapshot.MaxTcpWriteIovUsed);
        total.TcpWriteSendmsgCalls += snapshot.TcpWriteSendmsgCalls;
        total.TcpWriteAttemptBytes += snapshot.TcpWriteAttemptBytes;
        total.MaxTcpWriteAttemptBytes = std::max(total.MaxTcpWriteAttemptBytes, snapshot.MaxTcpWriteAttemptBytes);
        total.MaxTcpWriteSendmsgBytes = std::max(total.MaxTcpWriteSendmsgBytes, snapshot.MaxTcpWriteSendmsgBytes);
        total.TcpWriteAttemptBytesLe64K += snapshot.TcpWriteAttemptBytesLe64K;
        total.TcpWriteAttemptBytesLe256K += snapshot.TcpWriteAttemptBytesLe256K;
        total.TcpWriteAttemptBytesLe1M += snapshot.TcpWriteAttemptBytesLe1M;
        total.TcpWriteAttemptBytesLe4M += snapshot.TcpWriteAttemptBytesLe4M;
        total.TcpWriteAttemptBytesGt4M += snapshot.TcpWriteAttemptBytesGt4M;
        total.TcpWriteReturnedBytesLe64K += snapshot.TcpWriteReturnedBytesLe64K;
        total.TcpWriteReturnedBytesLe256K += snapshot.TcpWriteReturnedBytesLe256K;
        total.TcpWriteReturnedBytesLe1M += snapshot.TcpWriteReturnedBytesLe1M;
        total.TcpWriteReturnedBytesLe4M += snapshot.TcpWriteReturnedBytesLe4M;
        total.TcpWriteReturnedBytesGt4M += snapshot.TcpWriteReturnedBytesGt4M;
        total.TcpWriteEagainCount += snapshot.TcpWriteEagainCount;
        total.TcpWritePartialCount += snapshot.TcpWritePartialCount;
        total.TcpWriteBurstStops += snapshot.TcpWriteBurstStops;
        total.DeferredReceiveCompleteBytes += snapshot.DeferredReceiveCompleteBytes;
        total.DeferredReceiveCompletes += snapshot.DeferredReceiveCompletes;
        total.DeferredReceiveCompletionFlushes += snapshot.DeferredReceiveCompletionFlushes;
        total.MaxPendingQuicReceiveBytes = std::max(total.MaxPendingQuicReceiveBytes, snapshot.MaxPendingQuicReceiveBytes);
        total.MaxPendingQuicReceiveQueue = std::max(total.MaxPendingQuicReceiveQueue, snapshot.MaxPendingQuicReceiveQueue);
        total.QuicReceiveViewCount += snapshot.QuicReceiveViewCount;
        total.QuicReceiveViewBytes += snapshot.QuicReceiveViewBytes;
        total.MaxQuicReceiveViewBytes = std::max(total.MaxQuicReceiveViewBytes, snapshot.MaxQuicReceiveViewBytes);
        total.MaxQuicReceiveViewSlices = std::max(total.MaxQuicReceiveViewSlices, snapshot.MaxQuicReceiveViewSlices);
        total.QuicReceiveViewBytesLe64K += snapshot.QuicReceiveViewBytesLe64K;
        total.QuicReceiveViewBytesLe256K += snapshot.QuicReceiveViewBytesLe256K;
        total.QuicReceiveViewBytesLe1M += snapshot.QuicReceiveViewBytesLe1M;
        total.QuicReceiveViewBytesLe4M += snapshot.QuicReceiveViewBytesLe4M;
        total.QuicReceiveViewBytesGt4M += snapshot.QuicReceiveViewBytesGt4M;
        total.QuicReceiveViewSlices1 += snapshot.QuicReceiveViewSlices1;
        total.QuicReceiveViewSlices2To4 += snapshot.QuicReceiveViewSlices2To4;
        total.QuicReceiveViewSlices5To16 += snapshot.QuicReceiveViewSlices5To16;
        total.QuicReceiveViewSlicesGt16 += snapshot.QuicReceiveViewSlicesGt16;
        total.QuicReceivePausedCount += snapshot.QuicReceivePausedCount;
        total.QuicReceiveResumedCount += snapshot.QuicReceiveResumedCount;
        total.ReadDisabledCount += snapshot.ReadDisabledCount;
        total.CompressedTcpBytes += snapshot.CompressedTcpBytes;
        total.DecompressedTcpBytes += snapshot.DecompressedTcpBytes;
        total.ZstdDecompressInputBytes += snapshot.ZstdDecompressInputBytes;
        total.ZstdDecompressOutputBytes += snapshot.ZstdDecompressOutputBytes;
        total.ZstdDecompressCalls += snapshot.ZstdDecompressCalls;
        total.ZstdDecompressNeedInput += snapshot.ZstdDecompressNeedInput;
        total.ZstdDecompressNeedOutput += snapshot.ZstdDecompressNeedOutput;
        total.ZstdDecompressFailures += snapshot.ZstdDecompressFailures;
        total.Errors += snapshot.Errors;
        total.EventQueueFullErrors += snapshot.EventQueueFullErrors;
        total.TcpReadBufferAcquireFailures += snapshot.TcpReadBufferAcquireFailures;
        total.TcpReadBufferAcquirePendingBudgetFailures +=
            snapshot.TcpReadBufferAcquirePendingBudgetFailures;
        total.TcpReadBufferAcquireSlotLimitFailures +=
            snapshot.TcpReadBufferAcquireSlotLimitFailures;
        total.TcpReadBufferAcquireAllocFailures += snapshot.TcpReadBufferAcquireAllocFailures;
        total.TcpToQuicCompressFailures += snapshot.TcpToQuicCompressFailures;
        total.TcpToQuicCompressUpdateFailures += snapshot.TcpToQuicCompressUpdateFailures;
        total.TcpToQuicCompressFlushFailures += snapshot.TcpToQuicCompressFlushFailures;
        total.TcpToQuicBufferAcquireFailures += snapshot.TcpToQuicBufferAcquireFailures;
        total.TcpToQuicBufferAcquirePendingBudgetFailures +=
            snapshot.TcpToQuicBufferAcquirePendingBudgetFailures;
        total.TcpToQuicBufferAcquireSlotLimitFailures +=
            snapshot.TcpToQuicBufferAcquireSlotLimitFailures;
        total.TcpToQuicBufferAcquireAllocFailures +=
            snapshot.TcpToQuicBufferAcquireAllocFailures;
        total.QuicSendFailures += snapshot.QuicSendFailures;
        total.QuicSendBufferTooLargeFailures += snapshot.QuicSendBufferTooLargeFailures;
        total.QuicSendOperationAllocFailures += snapshot.QuicSendOperationAllocFailures;
        total.QuicSendApiFailures += snapshot.QuicSendApiFailures;
        total.QuicReceiveViewFailures += snapshot.QuicReceiveViewFailures;
        total.QuicReceiveViewAllocFailures += snapshot.QuicReceiveViewAllocFailures;
        total.QuicReceiveViewNullBufferFailures += snapshot.QuicReceiveViewNullBufferFailures;
        total.QuicReceiveViewEmptyFailures += snapshot.QuicReceiveViewEmptyFailures;
        total.QuicReceiveViewEnqueueFailures += snapshot.QuicReceiveViewEnqueueFailures;
        total.QuicReceiveDecompressFailures += snapshot.QuicReceiveDecompressFailures;
        total.QuicReceiveTcpBufferAcquireFailures += snapshot.QuicReceiveTcpBufferAcquireFailures;
        total.QuicReceiveTcpBufferAcquirePendingBudgetFailures +=
            snapshot.QuicReceiveTcpBufferAcquirePendingBudgetFailures;
        total.QuicReceiveTcpBufferAcquireSlotLimitFailures +=
            snapshot.QuicReceiveTcpBufferAcquireSlotLimitFailures;
        total.QuicReceiveTcpBufferAcquireAllocFailures +=
            snapshot.QuicReceiveTcpBufferAcquireAllocFailures;
        total.TcpWriteHardErrors += snapshot.TcpWriteHardErrors;
        total.LastTcpWriteErrno =
            snapshot.LastTcpWriteErrno != 0 ? snapshot.LastTcpWriteErrno : total.LastTcpWriteErrno;
        total.LastQuicSendStatus =
            snapshot.LastQuicSendStatus != 0 ? snapshot.LastQuicSendStatus : total.LastQuicSendStatus;
    }
    return total;
}
