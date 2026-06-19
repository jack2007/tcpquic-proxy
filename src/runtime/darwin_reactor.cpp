#include "darwin_reactor.h"

#if defined(__APPLE__)

#include <cerrno>
#include <fcntl.h>
#include <sys/event.h>
#include <sys/time.h>
#include <unistd.h>
#include <vector>

namespace {

constexpr uintptr_t WakeIdent = 1;
constexpr uint32_t ValidEvents = TqReactorEvents::Read |
    TqReactorEvents::Write | TqReactorEvents::Error;

bool HasRequestedEvents(uint32_t events) {
    return events != 0 && (events & ~ValidEvents) == 0;
}

bool WantsReadFilter(uint32_t events) {
    return (events & (TqReactorEvents::Read | TqReactorEvents::Error)) != 0;
}

bool WantsWriteFilter(uint32_t events) {
    return (events & TqReactorEvents::Write) != 0;
}

uint32_t FromKqueueEvent(const struct kevent& event) {
    uint32_t reactorEvents = 0;
    if (event.filter == EVFILT_READ) {
        reactorEvents |= TqReactorEvents::Read;
    }
    if (event.filter == EVFILT_WRITE) {
        reactorEvents |= TqReactorEvents::Write;
    }
    if ((event.flags & (EV_EOF | EV_ERROR)) != 0) {
        reactorEvents |= TqReactorEvents::Error;
    }
    return reactorEvents;
}

void CloseFd(int& fd) {
    if (fd >= 0) {
        (void)::close(fd);
        fd = -1;
    }
}

bool ApplySingleChange(int kqueueFd, uintptr_t ident, int16_t filter, uint16_t flags,
    uint32_t fflags = 0) {
    struct kevent change{};
    EV_SET(&change, ident, filter, flags, fflags, 0, nullptr);
    for (;;) {
        if (::kevent(kqueueFd, &change, 1, nullptr, 0, nullptr) == 0) {
            return true;
        }
        if (errno == EINTR) {
            continue;
        }
        return false;
    }
}

} // namespace

TqDarwinReactor::~TqDarwinReactor() {
    Stop();
}

bool TqDarwinReactor::Start() {
    if (KqueueFd >= 0) {
        return true;
    }

    const int kqueueFd = ::kqueue();
    if (kqueueFd < 0) {
        return false;
    }

    const int flags = ::fcntl(kqueueFd, F_GETFD, 0);
    if (flags < 0 || ::fcntl(kqueueFd, F_SETFD, flags | FD_CLOEXEC) != 0) {
        int savedErrno = errno;
        int mutableKqueueFd = kqueueFd;
        CloseFd(mutableKqueueFd);
        errno = savedErrno;
        return false;
    }

    if (!ApplySingleChange(kqueueFd, WakeIdent, EVFILT_USER, EV_ADD | EV_CLEAR)) {
        int savedErrno = errno;
        int mutableKqueueFd = kqueueFd;
        CloseFd(mutableKqueueFd);
        errno = savedErrno;
        return false;
    }

    KqueueFd = kqueueFd;
    return true;
}

void TqDarwinReactor::Stop() {
    Handlers.clear();
    CloseFd(KqueueFd);
}

bool TqDarwinReactor::ApplyFilters(TqSocketHandle fd, uint32_t events, bool addMissing) {
    const uint16_t upsertFlags = static_cast<uint16_t>(EV_ADD | EV_ENABLE | EV_CLEAR);
    if (WantsReadFilter(events)) {
        if (!ApplySingleChange(KqueueFd, static_cast<uintptr_t>(fd), EVFILT_READ, upsertFlags)) {
            return false;
        }
    } else if (!addMissing && !DeleteFilter(fd, EVFILT_READ)) {
        return false;
    }

    if (WantsWriteFilter(events)) {
        if (!ApplySingleChange(KqueueFd, static_cast<uintptr_t>(fd), EVFILT_WRITE, upsertFlags)) {
            return false;
        }
    } else if (!addMissing && !DeleteFilter(fd, EVFILT_WRITE)) {
        return false;
    }

    return true;
}

bool TqDarwinReactor::DeleteFilter(TqSocketHandle fd, int16_t filter) {
    if (ApplySingleChange(KqueueFd, static_cast<uintptr_t>(fd), filter, EV_DELETE)) {
        return true;
    }
    return errno == ENOENT;
}

bool TqDarwinReactor::Add(TqSocketHandle fd, uint32_t events, Handler handler) {
    if (KqueueFd < 0 || !TqSocketValid(fd) || !HasRequestedEvents(events) || !handler ||
        Handlers.find(fd) != Handlers.end()) {
        return false;
    }

    if (!ApplyFilters(fd, events, true)) {
        const int savedErrno = errno;
        (void)DeleteFilter(fd, EVFILT_READ);
        (void)DeleteFilter(fd, EVFILT_WRITE);
        errno = savedErrno;
        return false;
    }

    Handlers.emplace(fd, Entry{events, std::move(handler)});
    return true;
}

bool TqDarwinReactor::Modify(TqSocketHandle fd, uint32_t events) {
    if (KqueueFd < 0 || !TqSocketValid(fd) || !HasRequestedEvents(events)) {
        return false;
    }

    auto it = Handlers.find(fd);
    if (it == Handlers.end()) {
        return false;
    }

    const uint32_t previousEvents = it->second.Events;
    if (!ApplyFilters(fd, events, false)) {
        const int savedErrno = errno;
        (void)ApplyFilters(fd, previousEvents, true);
        errno = savedErrno;
        return false;
    }

    it->second.Events = events;
    return true;
}

bool TqDarwinReactor::Remove(TqSocketHandle fd) {
    if (KqueueFd < 0 || !TqSocketValid(fd)) {
        return false;
    }

    auto it = Handlers.find(fd);
    if (it == Handlers.end()) {
        return false;
    }

    const bool readDeleted = DeleteFilter(fd, EVFILT_READ);
    const bool writeDeleted = DeleteFilter(fd, EVFILT_WRITE);
    if (!readDeleted || !writeDeleted) {
        return false;
    }

    Handlers.erase(it);
    return true;
}

bool TqDarwinReactor::Wake() {
    return KqueueFd >= 0 && ApplySingleChange(KqueueFd, WakeIdent, EVFILT_USER, 0, NOTE_TRIGGER);
}

bool TqDarwinReactor::RunOnce(int timeoutMs) {
    if (KqueueFd < 0) {
        return false;
    }

    timespec timeout{};
    timespec* timeoutPtr = nullptr;
    if (timeoutMs >= 0) {
        timeout.tv_sec = timeoutMs / 1000;
        timeout.tv_nsec = static_cast<long>(timeoutMs % 1000) * 1000L * 1000L;
        timeoutPtr = &timeout;
    }

    constexpr int MaxEvents = 32;
    std::vector<struct kevent> events(MaxEvents);
    int count = 0;
    for (;;) {
        count = ::kevent(KqueueFd, nullptr, 0, events.data(), MaxEvents, timeoutPtr);
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
        const struct kevent& event = events[static_cast<size_t>(i)];
        if (event.filter == EVFILT_USER && event.ident == WakeIdent) {
            continue;
        }

        const TqSocketHandle fd = static_cast<TqSocketHandle>(event.ident);
        auto it = Handlers.find(fd);
        if (it == Handlers.end()) {
            continue;
        }

        uint32_t reactorEvents = FromKqueueEvent(event);
        reactorEvents &= (it->second.Events | TqReactorEvents::Error);
        if (reactorEvents == 0) {
            continue;
        }

        Handler handler = it->second.Callback;
        handler(fd, reactorEvents);
        handledWork = true;
    }

    return handledWork;
}

#else

TqDarwinReactor::~TqDarwinReactor() {
    Stop();
}

bool TqDarwinReactor::Start() {
    Started = true;
    return true;
}

void TqDarwinReactor::Stop() {
    Started = false;
}

bool TqDarwinReactor::Add(TqSocketHandle, uint32_t, Handler) {
    return false;
}

bool TqDarwinReactor::Modify(TqSocketHandle, uint32_t) {
    return false;
}

bool TqDarwinReactor::Remove(TqSocketHandle) {
    return false;
}

bool TqDarwinReactor::Wake() {
    return false;
}

bool TqDarwinReactor::RunOnce(int) {
    return false;
}

#endif
