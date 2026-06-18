#include "platform_socket.h"

#include <array>
#include <cassert>
#include <cstring>

int main() {
    TqSocketStartup startup;
    assert(startup.Ok());

    TqSocketHandle pair[2]{TqInvalidSocket, TqInvalidSocket};
    assert(TqSocketPair(pair));
    assert(TqSocketValid(pair[0]));
    assert(TqSocketValid(pair[1]));

    const char payload[] = "tcpquic-platform-socket";
    assert(TqSend(pair[0], payload, std::strlen(payload), TqSendFlags::None) ==
        static_cast<int>(std::strlen(payload)));

    std::array<char, 64> buffer{};
    const int received = TqRecv(pair[1], buffer.data(), buffer.size(), TqRecvFlags::None);
    assert(received == static_cast<int>(std::strlen(payload)));
    assert(std::memcmp(buffer.data(), payload, std::strlen(payload)) == 0);

    assert(TqSetNonBlocking(pair[0]));
    assert(TqSetSocketBuffer(pair[0], SO_RCVBUF, 256 * 1024));
    assert(TqSetSocketBuffer(pair[0], SO_SNDBUF, 256 * 1024));
    const int receiveBuffer = TqGetSocketBuffer(pair[0], SO_RCVBUF);
    const int sendBuffer = TqGetSocketBuffer(pair[0], SO_SNDBUF);
    assert(receiveBuffer > 0);
    assert(sendBuffer > 0);
    assert(TqShutdownSend(pair[0]));

    TqCloseSocket(pair[0]);
    TqCloseSocket(pair[1]);
    pair[0] = TqInvalidSocket;
    pair[1] = TqInvalidSocket;
    assert(!TqSocketValid(pair[0]));

    TqSocketHandle resetPair[2]{TqInvalidSocket, TqInvalidSocket};
    assert(TqSocketPair(resetPair));
    assert(TqSocketValid(resetPair[0]));
    assert(TqSocketValid(resetPair[1]));
    TqResetSocket(resetPair[0]);
    resetPair[0] = TqInvalidSocket;
    TqCloseSocket(resetPair[1]);
    resetPair[1] = TqInvalidSocket;
    return 0;
}
