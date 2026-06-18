#include "windows_reactor.h"

#if defined(_WIN32)

#include <algorithm>
#include <vector>

namespace {

constexpr uint32_t ValidEvents = TqReactorEvents::Read |
    TqReactorEvents::Write | TqReactorEvents::Error;

bool HasRequestedEvents(uint32_t events) {
    return events != 0 && (events & ~ValidEvents) == 0;
}

long ToNetworkEvents(uint32_t events) {
    long networkEvents = FD_CLOSE;
    if ((events & TqReactorEvents::Read) != 0) {
        networkEvents |= FD_ACCEPT | FD_READ;
    }
    if ((events & TqReactorEvents::Write) != 0) {
        networkEvents |= FD_CONNECT | FD_WRITE;
    }
    if ((events & TqReactorEvents::Error) != 0) {
        networkEvents |= FD_CONNECT | FD_CLOSE;
    }
    return networkEvents;
}

uint32_t FromNetworkEvents(long networkEvents, const int (&errors)[FD_MAX_EVENTS]) {
    uint32_t reactorEvents = 0;
    if ((networkEvents & (FD_ACCEPT | FD_READ)) != 0) {
        reactorEvents |= TqReactorEvents::Read;
    }
    if ((networkEvents & (FD_CONNECT | FD_WRITE)) != 0) {
        reactorEvents |= TqReactorEvents::Write;
    }
    if ((networkEvents & FD_CLOSE) != 0) {
        reactorEvents |= TqReactorEvents::Error;
    }

    constexpr long ErrorCheckedEvents = FD_ACCEPT | FD_READ | FD_CONNECT | FD_WRITE | FD_CLOSE;
    for (int bit = 0; bit < FD_MAX_EVENTS; ++bit) {
        const long eventBit = 1l << bit;
        if ((ErrorCheckedEvents & eventBit) != 0 &&
            (networkEvents & eventBit) != 0 &&
            errors[bit] != 0) {
            reactorEvents |= TqReactorEvents::Error;
        }
    }
    return reactorEvents;
}

void CloseEvent(WSAEVENT& event) {
    if (event != WSA_INVALID_EVENT) {
        (void)::WSACloseEvent(event);
        event = WSA_INVALID_EVENT;
    }
}

} // namespace

TqWindowsReactor::~TqWindowsReactor() {
    Stop();
}

bool TqWindowsReactor::Start() {
    if (WakeEvent_ != WSA_INVALID_EVENT) {
        return true;
    }

    WakeEvent_ = ::WSACreateEvent();
    return WakeEvent_ != WSA_INVALID_EVENT;
}

void TqWindowsReactor::Stop() {
    for (auto& item : Entries_) {
        (void)::WSAEventSelect(item.first, nullptr, 0);
        CloseEvent(item.second.Event);
    }
    Entries_.clear();
    CloseEvent(WakeEvent_);
}

bool TqWindowsReactor::Add(TqSocketHandle fd, uint32_t events, Handler handler) {
    if (WakeEvent_ == WSA_INVALID_EVENT || !TqSocketValid(fd) ||
        !HasRequestedEvents(events) || !handler || Entries_.find(fd) != Entries_.end()) {
        return false;
    }

    WSAEVENT event = ::WSACreateEvent();
    if (event == WSA_INVALID_EVENT) {
        return false;
    }

    if (::WSAEventSelect(fd, event, ToNetworkEvents(events)) != 0) {
        CloseEvent(event);
        return false;
    }

    Entries_.emplace(fd, Entry{event, events, std::move(handler)});
    return true;
}

bool TqWindowsReactor::Modify(TqSocketHandle fd, uint32_t events) {
    if (WakeEvent_ == WSA_INVALID_EVENT || !TqSocketValid(fd) || !HasRequestedEvents(events)) {
        return false;
    }

    auto it = Entries_.find(fd);
    if (it == Entries_.end()) {
        return false;
    }

    if (::WSAEventSelect(fd, it->second.Event, ToNetworkEvents(events)) != 0) {
        return false;
    }
    it->second.Events = events;
    return true;
}

bool TqWindowsReactor::Remove(TqSocketHandle fd) {
    if (WakeEvent_ == WSA_INVALID_EVENT || !TqSocketValid(fd)) {
        return false;
    }

    auto it = Entries_.find(fd);
    if (it == Entries_.end()) {
        return false;
    }

    const bool deselected = ::WSAEventSelect(fd, nullptr, 0) == 0;
    CloseEvent(it->second.Event);
    Entries_.erase(it);
    return deselected;
}

bool TqWindowsReactor::Wake() {
    return WakeEvent_ != WSA_INVALID_EVENT && ::WSASetEvent(WakeEvent_) == TRUE;
}

bool TqWindowsReactor::RunOnce(int timeoutMs) {
    if (WakeEvent_ == WSA_INVALID_EVENT) {
        return false;
    }

    std::vector<TqSocketHandle> sockets;
    sockets.reserve(Entries_.size());
    for (const auto& item : Entries_) {
        sockets.push_back(item.first);
    }

    const DWORD waitTimeout = timeoutMs < 0 ? WSA_INFINITE : static_cast<DWORD>(timeoutMs);
    const size_t socketChunkSize = WSA_MAXIMUM_WAIT_EVENTS - 1;
    bool handledWork = false;

    for (size_t offset = 0;; offset += socketChunkSize) {
        std::vector<WSAEVENT> waitEvents;
        waitEvents.reserve(WSA_MAXIMUM_WAIT_EVENTS);
        waitEvents.push_back(WakeEvent_);

        const size_t remaining = offset < sockets.size() ? sockets.size() - offset : 0;
        const size_t chunkCount = std::min(socketChunkSize, remaining);
        for (size_t i = 0; i < chunkCount; ++i) {
            auto it = Entries_.find(sockets[offset + i]);
            if (it != Entries_.end()) {
                waitEvents.push_back(it->second.Event);
            }
        }

        if (waitEvents.size() == 1 && offset != 0) {
            break;
        }

        const DWORD result = ::WSAWaitForMultipleEvents(
            static_cast<DWORD>(waitEvents.size()),
            waitEvents.data(),
            FALSE,
            waitTimeout,
            FALSE);
        if (result == WSA_WAIT_FAILED) {
            return false;
        }
        if (result == WSA_WAIT_TIMEOUT) {
            if (chunkCount < socketChunkSize || sockets.empty()) {
                return handledWork;
            }
            continue;
        }

        const DWORD index = result - WSA_WAIT_EVENT_0;
        if (index >= waitEvents.size()) {
            return false;
        }
        if (index == 0) {
            (void)::WSAResetEvent(WakeEvent_);
            return handledWork;
        }

        const size_t socketIndex = offset + index - 1;
        if (socketIndex >= sockets.size()) {
            return false;
        }

        const TqSocketHandle fd = sockets[socketIndex];
        auto it = Entries_.find(fd);
        if (it == Entries_.end()) {
            return handledWork;
        }

        WSANETWORKEVENTS networkEvents{};
        if (::WSAEnumNetworkEvents(fd, it->second.Event, &networkEvents) != 0) {
            return false;
        }

        uint32_t reactorEvents = FromNetworkEvents(networkEvents.lNetworkEvents, networkEvents.iErrorCode);
        reactorEvents &= (it->second.Events | TqReactorEvents::Error);
        if (reactorEvents == 0) {
            return handledWork;
        }

        Handler handler = it->second.Callback;
        handler(fd, reactorEvents);
        handledWork = true;
        return true;
    }

    return handledWork;
}

#else

TqWindowsReactor::~TqWindowsReactor() {
    Stop();
}

bool TqWindowsReactor::Start() {
    Started_ = true;
    return true;
}

void TqWindowsReactor::Stop() {
    Started_ = false;
}

bool TqWindowsReactor::Add(TqSocketHandle, uint32_t, Handler) {
    return false;
}

bool TqWindowsReactor::Modify(TqSocketHandle, uint32_t) {
    return false;
}

bool TqWindowsReactor::Remove(TqSocketHandle) {
    return false;
}

bool TqWindowsReactor::Wake() {
    return false;
}

bool TqWindowsReactor::RunOnce(int) {
    return false;
}

#endif
