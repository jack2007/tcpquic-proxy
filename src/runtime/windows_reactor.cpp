#include "windows_reactor.h"

#if defined(_WIN32)

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <memory>
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
    NextWaitOffset_ = 0;
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

    const size_t socketChunkSize = WSA_MAXIMUM_WAIT_EVENTS - 1;

    auto dispatchReady = [&](size_t socketIndex) -> int {
        if (socketIndex >= sockets.size()) {
            return -1;
        }

        const TqSocketHandle fd = sockets[socketIndex];
        auto it = Entries_.find(fd);
        if (it == Entries_.end()) {
            return 0;
        }

        WSANETWORKEVENTS networkEvents{};
        if (::WSAEnumNetworkEvents(fd, it->second.Event, &networkEvents) != 0) {
            return -1;
        }

        uint32_t reactorEvents = FromNetworkEvents(networkEvents.lNetworkEvents, networkEvents.iErrorCode);
        reactorEvents &= (it->second.Events | TqReactorEvents::Error);
        if (reactorEvents == 0) {
            return 0;
        }

        auto entry = std::make_shared<Entry>(it->second);
        Handler handler = entry->Callback;
        handler(fd, reactorEvents);
        return 1;
    };

    const size_t chunkCount = sockets.empty() ? 0 :
        (sockets.size() + socketChunkSize - 1) / socketChunkSize;
    const size_t startChunk = chunkCount == 0 ? 0 :
        (NextWaitOffset_ / socketChunkSize) % chunkCount;

    auto waitChunk = [&](size_t chunk, DWORD timeout) -> int {
        std::vector<WSAEVENT> waitEvents;
        waitEvents.reserve(WSA_MAXIMUM_WAIT_EVENTS);
        waitEvents.push_back(WakeEvent_);

        const size_t offset = chunk * socketChunkSize;
        const size_t remaining = offset < sockets.size() ? sockets.size() - offset : 0;
        const size_t eventCount = std::min(socketChunkSize, remaining);
        for (size_t i = 0; i < eventCount; ++i) {
            auto it = Entries_.find(sockets[offset + i]);
            if (it != Entries_.end()) {
                waitEvents.push_back(it->second.Event);
            }
        }

        const DWORD result = ::WSAWaitForMultipleEvents(
            static_cast<DWORD>(waitEvents.size()),
            waitEvents.data(),
            FALSE,
            timeout,
            FALSE);
        if (result == WSA_WAIT_FAILED) {
            return -1;
        }
        if (result == WSA_WAIT_TIMEOUT) {
            return 0;
        }

        const DWORD index = result - WSA_WAIT_EVENT_0;
        if (index >= waitEvents.size()) {
            return -1;
        }
        if (index == 0) {
            (void)::WSAResetEvent(WakeEvent_);
            return 2;
        }

        const int dispatchResult = dispatchReady(offset + index - 1);
        if (dispatchResult > 0) {
            const size_t nextSocket = offset + index;
            NextWaitOffset_ = sockets.empty() ? 0 : nextSocket % sockets.size();
        }
        return dispatchResult;
    };

    if (chunkCount == 0) {
        const DWORD result = ::WSAWaitForMultipleEvents(1, &WakeEvent_, FALSE,
            timeoutMs < 0 ? WSA_INFINITE : static_cast<DWORD>(timeoutMs), FALSE);
        if (result == WSA_WAIT_FAILED) {
            return false;
        }
        if (result == WSA_WAIT_EVENT_0) {
            (void)::WSAResetEvent(WakeEvent_);
        }
        return false;
    }

    for (size_t step = 0; step < chunkCount; ++step) {
        const size_t chunk = (startChunk + step) % chunkCount;
        const int result = waitChunk(chunk, 0);
        if (result < 0) {
            return false;
        }
        if (result > 0) {
            return result == 1;
        }
    }

    if (chunkCount == 1) {
        const DWORD waitTimeout = timeoutMs < 0 ? WSA_INFINITE : static_cast<DWORD>(timeoutMs);
        const int result = waitChunk(startChunk, waitTimeout);
        if (result < 0) {
            return false;
        }
        return result == 1;
    }

    constexpr DWORD WaitChunkMs = 10;
    const auto deadline = timeoutMs >= 0 ?
        std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs) :
        std::chrono::steady_clock::time_point::max();
    size_t chunk = startChunk;
    while (true) {
        DWORD waitTimeout = WaitChunkMs;
        if (timeoutMs >= 0) {
            const auto now = std::chrono::steady_clock::now();
            if (now >= deadline) {
                return false;
            }

            const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now);
            waitTimeout = static_cast<DWORD>(std::min<int64_t>(WaitChunkMs, remaining.count()));
        }

        const int result = waitChunk(chunk, waitTimeout);
        if (result < 0) {
            return false;
        }
        if (result > 0) {
            return result == 1;
        }

        chunk = (chunk + 1) % chunkCount;
    }
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
