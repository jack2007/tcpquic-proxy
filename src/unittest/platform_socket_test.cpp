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
    assert(TqShutdownSend(pair[0]));

    TqCloseSocket(pair[0]);
    TqCloseSocket(pair[1]);
    pair[0] = TqInvalidSocket;
    pair[1] = TqInvalidSocket;
    assert(!TqSocketValid(pair[0]));
    return 0;
}
