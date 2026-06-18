#include "windows_reactor.h"

#include "platform_socket.h"

#include <chrono>
#include <cstdint>
#include <vector>

namespace {

constexpr uint32_t UnknownEventBit = 0x80000000u;

void ClosePair(TqSocketHandle (&fds)[2]) {
    TqCloseSocket(fds[0]);
    TqCloseSocket(fds[1]);
    fds[0] = TqInvalidSocket;
    fds[1] = TqInvalidSocket;
}

bool MakeSocketPair(TqSocketHandle (&fds)[2]) {
    fds[0] = TqInvalidSocket;
    fds[1] = TqInvalidSocket;
    return TqSocketPair(fds);
}

bool SendByte(TqSocketHandle fd, char value) {
    return TqSend(fd, &value, 1, TqSendFlags::None) == 1;
}

bool RecvByte(TqSocketHandle fd, char& value) {
    return TqRecv(fd, &value, 1, TqRecvFlags::None) == 1;
}

int TestWake() {
    TqWindowsReactor reactor;
    if (!reactor.Start()) {
        return 1;
    }
    if (!reactor.Wake()) {
        reactor.Stop();
        return 2;
    }
    if (reactor.RunOnce(100)) {
        reactor.Stop();
        return 3;
    }
    reactor.Stop();
    return 0;
}

int TestReadReadiness() {
    TqWindowsReactor reactor;
    if (!reactor.Start()) {
        return 10;
    }

    TqSocketHandle fds[2]{TqInvalidSocket, TqInvalidSocket};
    if (!MakeSocketPair(fds)) {
        reactor.Stop();
        return 11;
    }

    bool readHandlerRan = false;
    TqSocketHandle observedFd = TqInvalidSocket;
    uint32_t observedEvents = 0;
    char observedByte = '\0';
    if (!reactor.Add(fds[0], TqReactorEvents::Read,
            [&](TqSocketHandle fd, uint32_t events) {
                readHandlerRan = true;
                observedFd = fd;
                observedEvents = events;
                (void)RecvByte(fd, observedByte);
            })) {
        reactor.Stop();
        ClosePair(fds);
        return 12;
    }

    const char payload = 'r';
    if (!SendByte(fds[1], payload)) {
        reactor.Stop();
        ClosePair(fds);
        return 13;
    }
    if (!reactor.RunOnce(100)) {
        reactor.Stop();
        ClosePair(fds);
        return 14;
    }
    if (!readHandlerRan || observedFd != fds[0] ||
        (observedEvents & TqReactorEvents::Read) == 0 || observedByte != payload) {
        reactor.Stop();
        ClosePair(fds);
        return 15;
    }

    reactor.Stop();
    ClosePair(fds);
    return 0;
}

int TestRemoveSuppressesDispatch() {
    TqWindowsReactor reactor;
    if (!reactor.Start()) {
        return 20;
    }

    TqSocketHandle fds[2]{TqInvalidSocket, TqInvalidSocket};
    if (!MakeSocketPair(fds)) {
        reactor.Stop();
        return 21;
    }

    int calls = 0;
    if (!reactor.Add(fds[0], TqReactorEvents::Read,
            [&](TqSocketHandle, uint32_t) {
                ++calls;
            })) {
        reactor.Stop();
        ClosePair(fds);
        return 22;
    }
    if (!reactor.Remove(fds[0])) {
        reactor.Stop();
        ClosePair(fds);
        return 23;
    }
    if (!SendByte(fds[1], 'x')) {
        reactor.Stop();
        ClosePair(fds);
        return 24;
    }
    if (reactor.RunOnce(10) || calls != 0) {
        reactor.Stop();
        ClosePair(fds);
        return 25;
    }

    reactor.Stop();
    ClosePair(fds);
    return 0;
}

int TestChunkedWaitBeyondMaximumEvents() {
    TqWindowsReactor reactor;
    if (!reactor.Start()) {
        return 30;
    }

    constexpr int PairCount = WSA_MAXIMUM_WAIT_EVENTS + 3;
    std::vector<TqSocketHandle> sockets;
    sockets.reserve(PairCount * 2);

    int calls = 0;
    char observedByte = '\0';
    for (int i = 0; i < PairCount; ++i) {
        TqSocketHandle fds[2]{TqInvalidSocket, TqInvalidSocket};
        if (!MakeSocketPair(fds)) {
            for (TqSocketHandle fd : sockets) {
                TqCloseSocket(fd);
            }
            reactor.Stop();
            return 31;
        }
        sockets.push_back(fds[0]);
        sockets.push_back(fds[1]);
        if (!reactor.Add(fds[0], TqReactorEvents::Read,
                [&](TqSocketHandle fd, uint32_t events) {
                    if ((events & TqReactorEvents::Read) != 0) {
                        (void)RecvByte(fd, observedByte);
                        ++calls;
                    }
                })) {
            for (TqSocketHandle fd : sockets) {
                TqCloseSocket(fd);
            }
            reactor.Stop();
            return 32;
        }
    }

    for (int i = 0; i < PairCount; ++i) {
        const char payload = static_cast<char>('a' + (i % 26));
        if (!SendByte(sockets[(i * 2) + 1], payload)) {
            for (TqSocketHandle fd : sockets) {
                TqCloseSocket(fd);
            }
            reactor.Stop();
            return 33;
        }
    }

    for (int i = 0; i < PairCount; ++i) {
        if (!reactor.RunOnce(100)) {
            for (TqSocketHandle fd : sockets) {
                TqCloseSocket(fd);
            }
            reactor.Stop();
            return 34;
        }
    }
    if (calls != PairCount || observedByte == '\0') {
        for (TqSocketHandle fd : sockets) {
            TqCloseSocket(fd);
        }
        reactor.Stop();
        return 35;
    }

    reactor.Stop();
    for (TqSocketHandle fd : sockets) {
        TqCloseSocket(fd);
    }
    return 0;
}

int TestChunkedWaitTimeoutIsBounded() {
    TqWindowsReactor reactor;
    if (!reactor.Start()) {
        return 50;
    }

    constexpr int PairCount = (WSA_MAXIMUM_WAIT_EVENTS * 2) + 3;
    std::vector<TqSocketHandle> sockets;
    sockets.reserve(PairCount * 2);

    for (int i = 0; i < PairCount; ++i) {
        TqSocketHandle fds[2]{TqInvalidSocket, TqInvalidSocket};
        if (!MakeSocketPair(fds)) {
            for (TqSocketHandle fd : sockets) {
                TqCloseSocket(fd);
            }
            reactor.Stop();
            return 51;
        }
        sockets.push_back(fds[0]);
        sockets.push_back(fds[1]);
        if (!reactor.Add(fds[0], TqReactorEvents::Read, [](TqSocketHandle, uint32_t) {})) {
            for (TqSocketHandle fd : sockets) {
                TqCloseSocket(fd);
            }
            reactor.Stop();
            return 52;
        }
    }

    const auto start = std::chrono::steady_clock::now();
    const bool result = reactor.RunOnce(100);
    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - start);

    reactor.Stop();
    for (TqSocketHandle fd : sockets) {
        TqCloseSocket(fd);
    }

    if (result) {
        return 53;
    }
    if (elapsed > std::chrono::milliseconds(250)) {
        return 54;
    }
    return 0;
}

int TestInvalidOperations() {
    TqWindowsReactor reactor;
    if (!reactor.Start()) {
        return 40;
    }

    TqSocketHandle fds[2]{TqInvalidSocket, TqInvalidSocket};
    if (!MakeSocketPair(fds)) {
        reactor.Stop();
        return 41;
    }

    if (reactor.Add(fds[0], 0, [](TqSocketHandle, uint32_t) {}) ||
        reactor.Add(fds[0], TqReactorEvents::Read, TqWindowsReactor::Handler{}) ||
        reactor.Add(fds[0], TqReactorEvents::Read | UnknownEventBit, [](TqSocketHandle, uint32_t) {}) ||
        reactor.Modify(fds[0], TqReactorEvents::Read) ||
        reactor.Remove(fds[0])) {
        reactor.Stop();
        ClosePair(fds);
        return 42;
    }

    if (!reactor.Add(fds[0], TqReactorEvents::Read, [](TqSocketHandle, uint32_t) {})) {
        reactor.Stop();
        ClosePair(fds);
        return 43;
    }
    if (!reactor.Modify(fds[0], TqReactorEvents::Write) ||
        reactor.Modify(fds[0], TqReactorEvents::Write | UnknownEventBit)) {
        reactor.Stop();
        ClosePair(fds);
        return 44;
    }

    reactor.Stop();
    if (reactor.Wake() || reactor.RunOnce(10) ||
        reactor.Add(fds[0], TqReactorEvents::Read, [](TqSocketHandle, uint32_t) {}) ||
        reactor.Modify(fds[0], TqReactorEvents::Read) || reactor.Remove(fds[0])) {
        ClosePair(fds);
        return 45;
    }

    ClosePair(fds);
    return 0;
}

} // namespace

int main() {
    TqSocketStartup sockets;
    if (!sockets.Ok()) {
        return 1000;
    }

    int result = TestWake();
    if (result != 0) {
        return result;
    }
    result = TestReadReadiness();
    if (result != 0) {
        return result;
    }
    result = TestRemoveSuppressesDispatch();
    if (result != 0) {
        return result;
    }
    result = TestChunkedWaitBeyondMaximumEvents();
    if (result != 0) {
        return result;
    }
    result = TestChunkedWaitTimeoutIsBounded();
    if (result != 0) {
        return result;
    }
    return TestInvalidOperations();
}
