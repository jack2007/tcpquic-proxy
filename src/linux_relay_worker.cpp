#include "linux_relay_worker.h"

#include <cerrno>
#include <cstring>

#include <sys/epoll.h>
#include <sys/eventfd.h>
#include <unistd.h>

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

TqLinuxRelayWorkerSnapshot TqLinuxRelayWorker::Snapshot() const {
    std::lock_guard<std::mutex> guard(QueueLock);
    TqLinuxRelayWorkerSnapshot snapshot{};
    snapshot.EventsProcessed = EventsProcessed.load();
    snapshot.WakeupWrites = WakeupWrites.load();
    snapshot.PendingEvents = Queue.size();
    snapshot.PendingBytes = 0;
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
        uint64_t value = 0;
        while (::read(WakeFd, &value, sizeof(value)) > 0) {
        }
        DrainEvents(Config.EventBudget);
    }
}
