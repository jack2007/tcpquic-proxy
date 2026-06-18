#include "linux_reactor.h"

#include <cerrno>
#include <sys/epoll.h>
#include <sys/eventfd.h>
#include <unistd.h>

namespace {

uint32_t ToEpollEvents(uint32_t events) {
    uint32_t epollEvents = EPOLLRDHUP;
    if ((events & TqReactorEvents::Read) != 0) {
        epollEvents |= EPOLLIN;
    }
    if ((events & TqReactorEvents::Write) != 0) {
        epollEvents |= EPOLLOUT;
    }
    return epollEvents;
}

uint32_t FromEpollEvents(uint32_t events) {
    uint32_t reactorEvents = 0;
    if ((events & EPOLLIN) != 0) {
        reactorEvents |= TqReactorEvents::Read;
    }
    if ((events & EPOLLOUT) != 0) {
        reactorEvents |= TqReactorEvents::Write;
    }
    if ((events & (EPOLLERR | EPOLLHUP | EPOLLRDHUP)) != 0) {
        reactorEvents |= TqReactorEvents::Error;
    }
    return reactorEvents;
}

bool HasRequestedEvents(uint32_t events) {
    constexpr uint32_t ValidEvents = TqReactorEvents::Read |
        TqReactorEvents::Write | TqReactorEvents::Error;
    return events != 0 && (events & ~ValidEvents) == 0;
}

void CloseFd(int& fd) {
    if (fd >= 0) {
        (void)::close(fd);
        fd = -1;
    }
}

void DrainEventFd(int fd) {
    uint64_t value = 0;
    for (;;) {
        const ssize_t result = ::read(fd, &value, sizeof(value));
        if (result == static_cast<ssize_t>(sizeof(value))) {
            continue;
        }
        if (result < 0 && errno == EINTR) {
            continue;
        }
        if (result < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            return;
        }
        return;
    }
}

} // namespace

TqLinuxReactor::~TqLinuxReactor() {
    Stop();
}

bool TqLinuxReactor::Start() {
    if (EpollFd >= 0) {
        return true;
    }

    const int epollFd = ::epoll_create1(EPOLL_CLOEXEC);
    if (epollFd < 0) {
        return false;
    }

    const int wakeFd = ::eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
    if (wakeFd < 0) {
        int closeErrno = errno;
        int mutableEpollFd = epollFd;
        CloseFd(mutableEpollFd);
        errno = closeErrno;
        return false;
    }

    epoll_event event{};
    event.events = EPOLLIN;
    event.data.fd = wakeFd;
    if (::epoll_ctl(epollFd, EPOLL_CTL_ADD, wakeFd, &event) != 0) {
        int closeErrno = errno;
        int mutableWakeFd = wakeFd;
        int mutableEpollFd = epollFd;
        CloseFd(mutableWakeFd);
        CloseFd(mutableEpollFd);
        errno = closeErrno;
        return false;
    }

    EpollFd = epollFd;
    WakeFd = wakeFd;
    return true;
}

void TqLinuxReactor::Stop() {
    Handlers.clear();
    CloseFd(WakeFd);
    CloseFd(EpollFd);
}

bool TqLinuxReactor::Add(int fd, uint32_t events, Handler handler) {
    if (EpollFd < 0 || fd < 0 || !HasRequestedEvents(events) || !handler) {
        return false;
    }

    epoll_event event{};
    event.events = ToEpollEvents(events);
    event.data.fd = fd;
    if (::epoll_ctl(EpollFd, EPOLL_CTL_ADD, fd, &event) != 0) {
        return false;
    }

    Handlers[fd] = std::move(handler);
    return true;
}

bool TqLinuxReactor::Modify(int fd, uint32_t events) {
    if (EpollFd < 0 || fd < 0 || !HasRequestedEvents(events) ||
        Handlers.find(fd) == Handlers.end()) {
        return false;
    }

    epoll_event event{};
    event.events = ToEpollEvents(events);
    event.data.fd = fd;
    return ::epoll_ctl(EpollFd, EPOLL_CTL_MOD, fd, &event) == 0;
}

bool TqLinuxReactor::Remove(int fd) {
    if (EpollFd < 0 || fd < 0) {
        return false;
    }
    auto it = Handlers.find(fd);
    if (it == Handlers.end()) {
        return false;
    }

    if (::epoll_ctl(EpollFd, EPOLL_CTL_DEL, fd, nullptr) != 0) {
        return false;
    }
    Handlers.erase(it);
    return true;
}

bool TqLinuxReactor::Wake() {
    if (WakeFd < 0) {
        return false;
    }

    const uint64_t value = 1;
    for (;;) {
        const ssize_t result = ::write(WakeFd, &value, sizeof(value));
        if (result == static_cast<ssize_t>(sizeof(value))) {
            return true;
        }
        if (result < 0 && errno == EINTR) {
            continue;
        }
        if (result < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            return true;
        }
        return false;
    }
}

bool TqLinuxReactor::RunOnce(int timeoutMs) {
    if (EpollFd < 0) {
        return false;
    }

    epoll_event events[32]{};
    int count = 0;
    for (;;) {
        count = ::epoll_wait(EpollFd, events, 32, timeoutMs);
        if (count >= 0) {
            break;
        }
        if (errno == EINTR) {
            continue;
        }
        return false;
    }

    bool handledWork = false;
    for (int i = 0; i < count; ++i) {
        const int fd = events[i].data.fd;
        if (fd == WakeFd) {
            DrainEventFd(WakeFd);
            continue;
        }

        auto it = Handlers.find(fd);
        if (it == Handlers.end()) {
            continue;
        }

        const uint32_t reactorEvents = FromEpollEvents(events[i].events);
        if (reactorEvents == 0) {
            continue;
        }
        Handler handler = it->second;
        handler(fd, reactorEvents);
        handledWork = true;
    }

    return handledWork;
}
