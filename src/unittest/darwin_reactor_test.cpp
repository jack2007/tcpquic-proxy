#include "darwin_reactor.h"

#include "platform_socket.h"

#if defined(__APPLE__)

#include <cstdint>

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
    if (!TqSocketPair(fds)) {
        return false;
    }
    return TqSetNonBlocking(fds[0]) && TqSetNonBlocking(fds[1]);
}

bool SendByte(TqSocketHandle fd, char value) {
    return TqSend(fd, &value, 1, TqSendFlags::None) == 1;
}

bool RecvByte(TqSocketHandle fd, char& value) {
    return TqRecv(fd, &value, 1, TqRecvFlags::None) == 1;
}

int TestWake() {
    TqDarwinReactor reactor;
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
    TqDarwinReactor reactor;
    if (!reactor.Start()) {
        return 10;
    }

    TqSocketHandle fds[2]{TqInvalidSocket, TqInvalidSocket};
    if (!MakeSocketPair(fds)) {
        reactor.Stop();
        ClosePair(fds);
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

int TestWriteReadiness() {
    TqDarwinReactor reactor;
    if (!reactor.Start()) {
        return 20;
    }

    TqSocketHandle fds[2]{TqInvalidSocket, TqInvalidSocket};
    if (!MakeSocketPair(fds)) {
        reactor.Stop();
        ClosePair(fds);
        return 21;
    }

    int writeCalls = 0;
    uint32_t observedEvents = 0;
    if (!reactor.Add(fds[0], TqReactorEvents::Write,
            [&](TqSocketHandle, uint32_t events) {
                ++writeCalls;
                observedEvents = events;
            })) {
        reactor.Stop();
        ClosePair(fds);
        return 22;
    }

    if (!reactor.RunOnce(100) || writeCalls != 1 ||
        (observedEvents & TqReactorEvents::Write) == 0) {
        reactor.Stop();
        ClosePair(fds);
        return 23;
    }

    reactor.Stop();
    ClosePair(fds);
    return 0;
}

int TestModifyReadToWrite() {
    TqDarwinReactor reactor;
    if (!reactor.Start()) {
        return 30;
    }

    TqSocketHandle fds[2]{TqInvalidSocket, TqInvalidSocket};
    if (!MakeSocketPair(fds)) {
        reactor.Stop();
        ClosePair(fds);
        return 31;
    }

    int readCalls = 0;
    int writeCalls = 0;
    char observed = '\0';
    if (!reactor.Add(fds[0], TqReactorEvents::Read,
            [&](TqSocketHandle fd, uint32_t events) {
                if ((events & TqReactorEvents::Read) != 0) {
                    ++readCalls;
                    (void)RecvByte(fd, observed);
                }
                if ((events & TqReactorEvents::Write) != 0) {
                    ++writeCalls;
                }
            })) {
        reactor.Stop();
        ClosePair(fds);
        return 32;
    }

    if (reactor.RunOnce(10) || readCalls != 0 || writeCalls != 0) {
        reactor.Stop();
        ClosePair(fds);
        return 33;
    }
    if (!SendByte(fds[1], 'm')) {
        reactor.Stop();
        ClosePair(fds);
        return 34;
    }
    if (!reactor.RunOnce(100) || readCalls != 1 || observed != 'm') {
        reactor.Stop();
        ClosePair(fds);
        return 35;
    }
    if (!reactor.Modify(fds[0], TqReactorEvents::Write)) {
        reactor.Stop();
        ClosePair(fds);
        return 36;
    }
    if (!reactor.RunOnce(100) || writeCalls != 1) {
        reactor.Stop();
        ClosePair(fds);
        return 37;
    }

    reactor.Stop();
    ClosePair(fds);
    return 0;
}

int TestRemoveSuppressesDispatch() {
    TqDarwinReactor reactor;
    if (!reactor.Start()) {
        return 40;
    }

    TqSocketHandle fds[2]{TqInvalidSocket, TqInvalidSocket};
    if (!MakeSocketPair(fds)) {
        reactor.Stop();
        ClosePair(fds);
        return 41;
    }

    int calls = 0;
    if (!reactor.Add(fds[0], TqReactorEvents::Read,
            [&](TqSocketHandle, uint32_t) {
                ++calls;
            })) {
        reactor.Stop();
        ClosePair(fds);
        return 42;
    }
    if (!reactor.Remove(fds[0])) {
        reactor.Stop();
        ClosePair(fds);
        return 43;
    }
    if (!SendByte(fds[1], 'x')) {
        reactor.Stop();
        ClosePair(fds);
        return 44;
    }
    if (reactor.RunOnce(10) || calls != 0) {
        reactor.Stop();
        ClosePair(fds);
        return 45;
    }

    reactor.Stop();
    ClosePair(fds);
    return 0;
}

int TestInvalidOperationsAndErrorOnly() {
    TqDarwinReactor reactor;
    if (!reactor.Start()) {
        return 50;
    }

    TqSocketHandle fds[2]{TqInvalidSocket, TqInvalidSocket};
    if (!MakeSocketPair(fds)) {
        reactor.Stop();
        ClosePair(fds);
        return 51;
    }

    if (reactor.Add(fds[0], 0, [](TqSocketHandle, uint32_t) {}) ||
        reactor.Add(fds[0], TqReactorEvents::Read, TqDarwinReactor::Handler{}) ||
        reactor.Add(fds[0], TqReactorEvents::Read | UnknownEventBit,
            [](TqSocketHandle, uint32_t) {}) ||
        reactor.Modify(fds[0], TqReactorEvents::Read) || reactor.Remove(fds[0])) {
        reactor.Stop();
        ClosePair(fds);
        return 52;
    }

    bool errorOnlyRan = false;
    uint32_t observedEvents = 0;
    if (!reactor.Add(fds[0], TqReactorEvents::Error,
            [&](TqSocketHandle, uint32_t events) {
                errorOnlyRan = true;
                observedEvents = events;
            })) {
        reactor.Stop();
        ClosePair(fds);
        return 53;
    }
    if (!reactor.Modify(fds[0], TqReactorEvents::Error)) {
        reactor.Stop();
        ClosePair(fds);
        return 54;
    }
    if (reactor.Modify(fds[0], TqReactorEvents::Error | UnknownEventBit)) {
        reactor.Stop();
        ClosePair(fds);
        return 55;
    }
    if (!SendByte(fds[1], 'e')) {
        reactor.Stop();
        ClosePair(fds);
        return 56;
    }
    if (reactor.RunOnce(10) || errorOnlyRan) {
        reactor.Stop();
        ClosePair(fds);
        return 57;
    }

    TqCloseSocket(fds[1]);
    fds[1] = TqInvalidSocket;
    if (!reactor.RunOnce(100) || !errorOnlyRan ||
        (observedEvents & TqReactorEvents::Error) == 0) {
        reactor.Stop();
        ClosePair(fds);
        return 58;
    }

    reactor.Stop();
    ClosePair(fds);
    return 0;
}

int TestCallbackSelfRemoveModifyStop() {
    TqDarwinReactor reactor;
    if (!reactor.Start()) {
        return 60;
    }

    TqSocketHandle fds[2]{TqInvalidSocket, TqInvalidSocket};
    if (!MakeSocketPair(fds)) {
        reactor.Stop();
        ClosePair(fds);
        return 61;
    }

    int removeCalls = 0;
    if (!reactor.Add(fds[0], TqReactorEvents::Read,
            [&](TqSocketHandle fd, uint32_t events) {
                if ((events & TqReactorEvents::Read) != 0) {
                    char value = '\0';
                    (void)RecvByte(fd, value);
                    ++removeCalls;
                    (void)reactor.Remove(fd);
                }
            })) {
        reactor.Stop();
        ClosePair(fds);
        return 62;
    }
    if (!SendByte(fds[1], 'a')) {
        reactor.Stop();
        ClosePair(fds);
        return 63;
    }
    if (!reactor.RunOnce(100) || removeCalls != 1) {
        reactor.Stop();
        ClosePair(fds);
        return 64;
    }
    if (!SendByte(fds[1], 'b')) {
        reactor.Stop();
        ClosePair(fds);
        return 65;
    }
    if (reactor.RunOnce(10) || removeCalls != 1) {
        reactor.Stop();
        ClosePair(fds);
        return 66;
    }

    ClosePair(fds);
    if (!MakeSocketPair(fds)) {
        reactor.Stop();
        return 67;
    }

    int readCalls = 0;
    int writeCalls = 0;
    if (!reactor.Add(fds[0], TqReactorEvents::Read,
            [&](TqSocketHandle fd, uint32_t events) {
                if ((events & TqReactorEvents::Read) != 0) {
                    char value = '\0';
                    (void)RecvByte(fd, value);
                    ++readCalls;
                    (void)reactor.Modify(fd, TqReactorEvents::Write);
                }
                if ((events & TqReactorEvents::Write) != 0) {
                    ++writeCalls;
                    (void)reactor.Modify(fd, TqReactorEvents::Read);
                }
            })) {
        reactor.Stop();
        ClosePair(fds);
        return 68;
    }
    if (!SendByte(fds[1], 'm')) {
        reactor.Stop();
        ClosePair(fds);
        return 69;
    }
    if (!reactor.RunOnce(100) || readCalls != 1 || writeCalls != 0) {
        reactor.Stop();
        ClosePair(fds);
        return 70;
    }
    if (!reactor.RunOnce(100) || readCalls != 1 || writeCalls != 1) {
        reactor.Stop();
        ClosePair(fds);
        return 71;
    }

    if (!reactor.Remove(fds[0])) {
        reactor.Stop();
        ClosePair(fds);
        return 72;
    }

    ClosePair(fds);
    if (!MakeSocketPair(fds)) {
        reactor.Stop();
        return 73;
    }

    int stopCalls = 0;
    if (!reactor.Add(fds[0], TqReactorEvents::Read,
            [&](TqSocketHandle fd, uint32_t) {
                char value = '\0';
                (void)RecvByte(fd, value);
                ++stopCalls;
                reactor.Stop();
            })) {
        reactor.Stop();
        ClosePair(fds);
        return 74;
    }
    if (!SendByte(fds[1], 's')) {
        reactor.Stop();
        ClosePair(fds);
        return 75;
    }
    if (!reactor.RunOnce(100) || stopCalls != 1) {
        reactor.Stop();
        ClosePair(fds);
        return 76;
    }
    if (reactor.Wake() || reactor.RunOnce(10) ||
        reactor.Add(fds[0], TqReactorEvents::Read, [](TqSocketHandle, uint32_t) {}) ||
        reactor.Modify(fds[0], TqReactorEvents::Read) || reactor.Remove(fds[0])) {
        ClosePair(fds);
        return 77;
    }

    ClosePair(fds);
    return 0;
}

} // namespace

#endif

int main() {
#if defined(__APPLE__)
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
    result = TestWriteReadiness();
    if (result != 0) {
        return result;
    }
    result = TestModifyReadToWrite();
    if (result != 0) {
        return result;
    }
    result = TestRemoveSuppressesDispatch();
    if (result != 0) {
        return result;
    }
    result = TestInvalidOperationsAndErrorOnly();
    if (result != 0) {
        return result;
    }
    return TestCallbackSelfRemoveModifyStop();
#else
    return 0;
#endif
}
