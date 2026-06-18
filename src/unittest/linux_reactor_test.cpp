#include "linux_reactor.h"

#include <cstdint>
#include <sys/socket.h>
#include <unistd.h>

namespace {

constexpr uint32_t UnknownEventBit = 0x80000000u;

void ClosePair(int (&fds)[2]) {
    if (fds[0] >= 0) {
        ::close(fds[0]);
        fds[0] = -1;
    }
    if (fds[1] >= 0) {
        ::close(fds[1]);
        fds[1] = -1;
    }
}

bool MakeSocketPair(int (&fds)[2]) {
    fds[0] = -1;
    fds[1] = -1;
    return ::socketpair(AF_UNIX, SOCK_STREAM, 0, fds) == 0;
}

bool WriteByte(int fd, char value) {
    return ::write(fd, &value, 1) == 1;
}

bool ReadByte(int fd, char& value) {
    return ::read(fd, &value, 1) == 1;
}

int TestReadRemoveWake() {
    TqLinuxReactor reactor;
    if (!reactor.Start()) {
        return 1;
    }

    int fds[2]{-1, -1};
    if (::pipe(fds) != 0) {
        reactor.Stop();
        return 2;
    }

    bool readHandlerRan = false;
    int observedFd = -1;
    uint32_t observedEvents = 0;
    char observedByte = '\0';
    if (!reactor.Add(fds[0], TqReactorEvents::Read,
            [&](int fd, uint32_t events) {
                readHandlerRan = true;
                observedFd = fd;
                observedEvents = events;
                (void)ReadByte(fd, observedByte);
            })) {
        reactor.Stop();
        ClosePair(fds);
        return 3;
    }

    const char payload = 'x';
    if (!WriteByte(fds[1], payload)) {
        reactor.Stop();
        ClosePair(fds);
        return 4;
    }
    if (!reactor.RunOnce(100)) {
        reactor.Stop();
        ClosePair(fds);
        return 5;
    }
    if (!readHandlerRan || observedFd != fds[0] ||
        (observedEvents & TqReactorEvents::Read) == 0 || observedByte != payload) {
        reactor.Stop();
        ClosePair(fds);
        return 6;
    }

    if (!reactor.Remove(fds[0])) {
        reactor.Stop();
        ClosePair(fds);
        return 7;
    }
    readHandlerRan = false;
    if (!WriteByte(fds[1], payload)) {
        reactor.Stop();
        ClosePair(fds);
        return 8;
    }
    if (reactor.RunOnce(10) || readHandlerRan) {
        reactor.Stop();
        ClosePair(fds);
        return 9;
    }

    if (!reactor.Wake()) {
        reactor.Stop();
        ClosePair(fds);
        return 10;
    }
    if (reactor.RunOnce(100)) {
        reactor.Stop();
        ClosePair(fds);
        return 11;
    }

    reactor.Stop();
    ClosePair(fds);
    return 0;
}

int TestModifyReadWriteTransitions() {
    TqLinuxReactor reactor;
    if (!reactor.Start()) {
        return 20;
    }

    int fds[2]{-1, -1};
    if (!MakeSocketPair(fds)) {
        reactor.Stop();
        return 21;
    }

    int writeEvents = 0;
    int readEvents = 0;
    char observed = '\0';
    if (!reactor.Add(fds[0], TqReactorEvents::Write,
            [&](int fd, uint32_t events) {
                if ((events & TqReactorEvents::Write) != 0) {
                    ++writeEvents;
                }
                if ((events & TqReactorEvents::Read) != 0) {
                    ++readEvents;
                    (void)ReadByte(fd, observed);
                }
            })) {
        reactor.Stop();
        ClosePair(fds);
        return 22;
    }

    if (!reactor.RunOnce(100) || writeEvents != 1 || readEvents != 0) {
        reactor.Stop();
        ClosePair(fds);
        return 23;
    }
    if (!reactor.Modify(fds[0], TqReactorEvents::Read)) {
        reactor.Stop();
        ClosePair(fds);
        return 24;
    }
    if (reactor.RunOnce(10) || readEvents != 0) {
        reactor.Stop();
        ClosePair(fds);
        return 25;
    }
    if (!WriteByte(fds[1], 'r')) {
        reactor.Stop();
        ClosePair(fds);
        return 26;
    }
    if (!reactor.RunOnce(100) || readEvents != 1 || observed != 'r') {
        reactor.Stop();
        ClosePair(fds);
        return 27;
    }
    if (!reactor.Modify(fds[0], TqReactorEvents::Write)) {
        reactor.Stop();
        ClosePair(fds);
        return 28;
    }
    if (!reactor.RunOnce(100) || writeEvents < 2) {
        reactor.Stop();
        ClosePair(fds);
        return 29;
    }

    reactor.Stop();
    ClosePair(fds);
    return 0;
}

int TestInvalidOperationsAndErrorOnly() {
    TqLinuxReactor reactor;
    if (!reactor.Start()) {
        return 40;
    }

    int fds[2]{-1, -1};
    if (!MakeSocketPair(fds)) {
        reactor.Stop();
        return 41;
    }

    if (reactor.Add(fds[0], 0, [](int, uint32_t) {})) {
        reactor.Stop();
        ClosePair(fds);
        return 42;
    }
    if (reactor.Add(fds[0], TqReactorEvents::Read, TqLinuxReactor::Handler{})) {
        reactor.Stop();
        ClosePair(fds);
        return 43;
    }
    if (reactor.Add(fds[0], TqReactorEvents::Read | UnknownEventBit,
            [](int, uint32_t) {})) {
        reactor.Stop();
        ClosePair(fds);
        return 44;
    }
    if (reactor.Modify(fds[0], TqReactorEvents::Read)) {
        reactor.Stop();
        ClosePair(fds);
        return 45;
    }
    if (reactor.Remove(fds[0])) {
        reactor.Stop();
        ClosePair(fds);
        return 46;
    }

    bool errorOnlyRan = false;
    if (!reactor.Add(fds[0], TqReactorEvents::Error,
            [&](int, uint32_t) {
                errorOnlyRan = true;
            })) {
        reactor.Stop();
        ClosePair(fds);
        return 47;
    }
    if (!reactor.Modify(fds[0], TqReactorEvents::Error)) {
        reactor.Stop();
        ClosePair(fds);
        return 48;
    }
    if (reactor.Modify(fds[0], TqReactorEvents::Error | UnknownEventBit)) {
        reactor.Stop();
        ClosePair(fds);
        return 49;
    }
    if (!WriteByte(fds[1], 'e')) {
        reactor.Stop();
        ClosePair(fds);
        return 50;
    }
    if (reactor.RunOnce(10) || errorOnlyRan) {
        reactor.Stop();
        ClosePair(fds);
        return 51;
    }

    reactor.Stop();
    ClosePair(fds);
    return 0;
}

int TestCallbackSelfRemove() {
    TqLinuxReactor reactor;
    if (!reactor.Start()) {
        return 60;
    }

    int fds[2]{-1, -1};
    if (!MakeSocketPair(fds)) {
        reactor.Stop();
        return 61;
    }

    int calls = 0;
    if (!reactor.Add(fds[0], TqReactorEvents::Read,
            [&](int fd, uint32_t events) {
                if ((events & TqReactorEvents::Read) != 0) {
                    char value = '\0';
                    (void)ReadByte(fd, value);
                    ++calls;
                    (void)reactor.Remove(fd);
                }
            })) {
        reactor.Stop();
        ClosePair(fds);
        return 62;
    }
    if (!WriteByte(fds[1], 'a')) {
        reactor.Stop();
        ClosePair(fds);
        return 63;
    }
    if (!reactor.RunOnce(100) || calls != 1) {
        reactor.Stop();
        ClosePair(fds);
        return 64;
    }
    if (!WriteByte(fds[1], 'b')) {
        reactor.Stop();
        ClosePair(fds);
        return 65;
    }
    if (reactor.RunOnce(10) || calls != 1) {
        reactor.Stop();
        ClosePair(fds);
        return 66;
    }

    reactor.Stop();
    ClosePair(fds);
    return 0;
}

int TestCallbackSelfModify() {
    TqLinuxReactor reactor;
    if (!reactor.Start()) {
        return 80;
    }

    int fds[2]{-1, -1};
    if (!MakeSocketPair(fds)) {
        reactor.Stop();
        return 81;
    }

    int readCalls = 0;
    int writeCalls = 0;
    if (!reactor.Add(fds[0], TqReactorEvents::Read,
            [&](int fd, uint32_t events) {
                if ((events & TqReactorEvents::Read) != 0) {
                    char value = '\0';
                    (void)ReadByte(fd, value);
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
        return 82;
    }
    if (!WriteByte(fds[1], 'm')) {
        reactor.Stop();
        ClosePair(fds);
        return 83;
    }
    if (!reactor.RunOnce(100) || readCalls != 1 || writeCalls != 0) {
        reactor.Stop();
        ClosePair(fds);
        return 84;
    }
    if (!reactor.RunOnce(100) || readCalls != 1 || writeCalls != 1) {
        reactor.Stop();
        ClosePair(fds);
        return 85;
    }

    reactor.Stop();
    ClosePair(fds);
    return 0;
}

int TestCallbackSelfStopAndStoppedOperations() {
    TqLinuxReactor reactor;
    if (!reactor.Start()) {
        return 100;
    }

    int fds[2]{-1, -1};
    if (!MakeSocketPair(fds)) {
        reactor.Stop();
        return 101;
    }

    int calls = 0;
    if (!reactor.Add(fds[0], TqReactorEvents::Read,
            [&](int fd, uint32_t) {
                char value = '\0';
                (void)ReadByte(fd, value);
                ++calls;
                reactor.Stop();
            })) {
        reactor.Stop();
        ClosePair(fds);
        return 102;
    }
    if (!WriteByte(fds[1], 's')) {
        reactor.Stop();
        ClosePair(fds);
        return 103;
    }
    if (!reactor.RunOnce(100) || calls != 1) {
        reactor.Stop();
        ClosePair(fds);
        return 104;
    }
    if (reactor.Wake() || reactor.RunOnce(10) ||
        reactor.Add(fds[0], TqReactorEvents::Read, [](int, uint32_t) {}) ||
        reactor.Modify(fds[0], TqReactorEvents::Read) ||
        reactor.Remove(fds[0])) {
        ClosePair(fds);
        return 105;
    }

    ClosePair(fds);
    return 0;
}

} // namespace

int main() {
    int result = TestReadRemoveWake();
    if (result != 0) {
        return result;
    }
    result = TestModifyReadWriteTransitions();
    if (result != 0) {
        return result;
    }
    result = TestInvalidOperationsAndErrorOnly();
    if (result != 0) {
        return result;
    }
    result = TestCallbackSelfRemove();
    if (result != 0) {
        return result;
    }
    result = TestCallbackSelfModify();
    if (result != 0) {
        return result;
    }
    return TestCallbackSelfStopAndStoppedOperations();
}
