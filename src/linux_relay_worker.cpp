#include "linux_relay_worker.h"

#include <algorithm>
#include <chrono>
#include <cerrno>
#include <cstring>
#include <thread>

#include <fcntl.h>
#include <sys/epoll.h>
#include <sys/eventfd.h>
#include <sys/uio.h>
#include <unistd.h>

struct TqLinuxRelayWorker::RelayState {
    uint64_t Id{0};
    int TcpFd{-1};
    MsQuicStream* Stream{nullptr};
    TqRelayHandle* Handle{nullptr};
    bool EnableQuicSends{true};
    TqLinuxRelayBufferPool Pool;

    RelayState(const TqLinuxRelayRegistration& registration, const TqLinuxRelayWorkerConfig& config)
        : TcpFd(registration.TcpFd),
          Stream(registration.Stream),
          Handle(registration.Handle),
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

bool TqLinuxRelayWorker::RegisterRelay(const TqLinuxRelayRegistration& registration) {
    if (registration.TcpFd < 0 || EpollFd < 0) {
        return false;
    }
    if (!TqSetNonBlocking(registration.TcpFd)) {
        return false;
    }

    auto relay = std::make_unique<RelayState>(registration, Config);
    relay->Id = NextRelayId++;
    RelayState* raw = relay.get();

    epoll_event event{};
    event.events = EPOLLIN | EPOLLRDHUP | EPOLLERR;
    event.data.ptr = raw;
    if (::epoll_ctl(EpollFd, EPOLL_CTL_ADD, registration.TcpFd, &event) != 0) {
        return false;
    }

    std::lock_guard<std::mutex> guard(RelayLock);
    Relays.push_back(std::move(relay));
    return true;
}

bool TqLinuxRelayWorker::RegisterRelayForTest(const TqLinuxRelayRegistration& registration) {
    return RegisterRelay(registration);
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
    if (relay == nullptr) {
        return;
    }

    uint64_t readBytes = 0;
    while (readBytes < Config.ReadBatchBytes) {
        std::vector<TqBufferRef> refs;
        std::vector<iovec> iov;
        const size_t maxIov = std::min<size_t>(Config.MaxIov, 1024);
        refs.reserve(maxIov);
        iov.reserve(maxIov);

        for (size_t i = 0; i < maxIov && readBytes + Config.ReadChunkSize <= Config.ReadBatchBytes; ++i) {
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
            readBytes += static_cast<uint64_t>(received);
            TcpReadBytes.fetch_add(static_cast<uint64_t>(received));
            TcpReadBatches.fetch_add(1);
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

TqLinuxRelayWorkerSnapshot TqLinuxRelayWorker::Snapshot() const {
    std::lock_guard<std::mutex> guard(QueueLock);
    TqLinuxRelayWorkerSnapshot snapshot{};
    snapshot.EventsProcessed = EventsProcessed.load();
    snapshot.WakeupWrites = WakeupWrites.load();
    snapshot.PendingEvents = Queue.size();
    snapshot.PendingBytes = 0;
    snapshot.TcpReadBatches = TcpReadBatches.load();
    snapshot.TcpReadBytes = TcpReadBytes.load();
    return snapshot;
}

// PendingBytes is zero in this queue-only task because relay pools do not exist yet.
// Task 7 replaces this with real pool and pending-write accounting before metrics use it.

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
                DrainTcpReadable(relay);
            }
        }
    }
}
