#include "speed_test.h"

#include "platform_socket.h"

#include <array>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <future>
#include <thread>
#include <vector>

namespace {

bool ConnectLoopback(uint16_t port, TqSocketHandle& outSocket) {
    outSocket = TqInvalidSocket;
    TqSocketHandle sock = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (!TqSocketValid(sock)) {
        return false;
    }

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    if (!TqInetPton(AF_INET, "127.0.0.1", &addr.sin_addr)) {
        TqCloseSocket(sock);
        return false;
    }

    if (::connect(sock, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
        TqCloseSocket(sock);
        return false;
    }

    outSocket = sock;
    return true;
}

int TestStartAndAuthorization() {
    TqServerSpeedTestController controller;
    TqSpeedStart start{};
    start.SessionId = 1;
    start.Direction = TqSpeedDirection::Upload;
    start.DurationSec = 2;
    start.Parallel = 2;

    TqSpeedReady ready{};
    if (!controller.StartSession(start, ready)) {
        return 1;
    }
    if (ready.SessionId != 1) {
        return 2;
    }
    if (ready.AddrType != TQ_ADDR_IPV4) {
        return 3;
    }
    if (ready.Addr.size() != 4) {
        return 4;
    }
    if (ready.Addr[0] != 127 || ready.Addr[1] != 0 || ready.Addr[2] != 0 || ready.Addr[3] != 1) {
        return 5;
    }
    if (ready.Port == 0) {
        return 6;
    }
    if (!controller.IsAllowedEphemeralTarget("127.0.0.1", ready.Port)) {
        return 7;
    }

    TqSpeedResult result{};
    if (!controller.FinishSession(1, 11, 22, result)) {
        return 8;
    }
    if (controller.IsAllowedEphemeralTarget("127.0.0.1", ready.Port)) {
        return 9;
    }
    return 0;
}

int TestUploadPath() {
    TqServerSpeedTestController controller;
    TqSpeedStart start{};
    start.SessionId = 2;
    start.Direction = TqSpeedDirection::Upload;
    start.DurationSec = 2;
    start.Parallel = 1;

    TqSpeedReady ready{};
    if (!controller.StartSession(start, ready)) {
        return 10;
    }

    TqSocketHandle client = TqInvalidSocket;
    if (!ConnectLoopback(ready.Port, client)) {
        controller.StopAll();
        return 11;
    }

    std::vector<uint8_t> payload(1024 * 1024, 0x5a);
    size_t sent = 0;
    while (sent < payload.size()) {
        const int rc = TqSend(client, payload.data() + sent, payload.size() - sent, TqSendFlags::NoSignal);
        if (rc <= 0) {
            TqCloseSocket(client);
            controller.StopAll();
            return 12;
        }
        sent += static_cast<size_t>(rc);
    }
    (void)TqShutdownSend(client);
    TqCloseSocket(client);

    TqSpeedResult result{};
    if (!controller.FinishSession(2, payload.size(), 12345, result)) {
        return 13;
    }
    if (result.ServerBytes != payload.size()) {
        return 14;
    }
    if (result.AcceptedConnections != 1) {
        return 15;
    }
    if (result.ClosedConnections != 1) {
        return 16;
    }
    return 0;
}

int TestDownloadPath() {
    TqServerSpeedTestController controller;
    TqSpeedStart start{};
    start.SessionId = 3;
    start.Direction = TqSpeedDirection::Download;
    start.DurationSec = 2;
    start.Parallel = 1;

    TqSpeedReady ready{};
    if (!controller.StartSession(start, ready)) {
        return 20;
    }

    TqSocketHandle client = TqInvalidSocket;
    if (!ConnectLoopback(ready.Port, client)) {
        controller.StopAll();
        return 21;
    }

    std::array<uint8_t, 4096> buffer{};
    const int received = TqRecv(client, buffer.data(), buffer.size(), TqRecvFlags::None);
    if (received <= 0) {
        TqCloseSocket(client);
        controller.StopAll();
        return 22;
    }
    TqCloseSocket(client);

    TqSpeedResult result{};
    if (!controller.FinishSession(3, static_cast<uint64_t>(received), 6789, result)) {
        return 23;
    }
    if (result.ServerBytes == 0) {
        return 24;
    }
    if (result.AcceptedConnections != 1) {
        return 25;
    }
    if (result.ClosedConnections != 1) {
        return 26;
    }
    return 0;
}

int TestUploadFinishWithOpenClient() {
    TqServerSpeedTestController controller;
    TqSpeedStart start{};
    start.SessionId = 4;
    start.Direction = TqSpeedDirection::Upload;
    start.DurationSec = 2;
    start.Parallel = 1;

    TqSpeedReady ready{};
    if (!controller.StartSession(start, ready)) {
        return 30;
    }

    TqSocketHandle client = TqInvalidSocket;
    if (!ConnectLoopback(ready.Port, client)) {
        controller.StopAll();
        return 31;
    }

    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    auto finishFuture = std::async(std::launch::async, [&controller]() {
        TqSpeedResult result{};
        if (!controller.FinishSession(4, 0, 0, result)) {
            return 34;
        }
        if (result.AcceptedConnections != 1) {
            return 35;
        }
        if (result.ClosedConnections != 1) {
            return 36;
        }
        return 0;
    });

    if (finishFuture.wait_for(std::chrono::seconds(2)) != std::future_status::ready) {
        TqCloseSocket(client);
        return 32;
    }

    TqCloseSocket(client);
    const int finishRc = finishFuture.get();
    if (finishRc != 0) {
        return finishRc;
    }
    return 0;
}

} // namespace

int main() {
    TqSocketStartup startup;
    if (!startup.Ok()) {
        return 100;
    }
    if (const int rc = TestStartAndAuthorization(); rc != 0) {
        return rc;
    }
    if (const int rc = TestUploadPath(); rc != 0) {
        return rc;
    }
    if (const int rc = TestDownloadPath(); rc != 0) {
        return rc;
    }
    if (const int rc = TestUploadFinishWithOpenClient(); rc != 0) {
        return rc;
    }
    return 0;
}
