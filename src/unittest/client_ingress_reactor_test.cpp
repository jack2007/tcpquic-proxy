#include "client_ingress_reactor.h"

#include <cerrno>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <algorithm>
#include <functional>
#include <poll.h>
#include <netdb.h>
#include <string>
#include <sys/socket.h>
#include <thread>
#include <unistd.h>
#include <vector>

namespace {

void CloseFd(int& fd) {
    if (fd >= 0) {
        (void)::close(fd);
        fd = -1;
    }
}

bool SplitHostPort(const std::string& value, std::string& host, std::string& port) {
    const size_t colon = value.rfind(':');
    if (colon == std::string::npos || colon == 0 || colon + 1 >= value.size()) {
        return false;
    }
    host = value.substr(0, colon);
    port = value.substr(colon + 1);
    return true;
}

bool ConnectTo(const std::string& address, int& connectedFd) {
    connectedFd = -1;

    std::string host;
    std::string port;
    if (!SplitHostPort(address, host, port)) {
        return false;
    }

    addrinfo hints{};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;

    addrinfo* result = nullptr;
    if (::getaddrinfo(host.c_str(), port.c_str(), &hints, &result) != 0) {
        return false;
    }

    for (addrinfo* ai = result; ai != nullptr; ai = ai->ai_next) {
        const int fd = ::socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
        if (fd < 0) {
            continue;
        }
        if (::connect(fd, ai->ai_addr, ai->ai_addrlen) == 0) {
            connectedFd = fd;
            ::freeaddrinfo(result);
            return true;
        }
        int closeFd = fd;
        CloseFd(closeFd);
    }

    ::freeaddrinfo(result);
    return false;
}

bool SendAll(int fd, const std::vector<uint8_t>& data) {
    size_t sent = 0;
    while (sent < data.size()) {
        const ssize_t result = ::send(fd, data.data() + sent, data.size() - sent, 0);
        if (result < 0 && errno == EINTR) {
            continue;
        }
        if (result <= 0) {
            return false;
        }
        sent += static_cast<size_t>(result);
    }
    return true;
}

bool ReadExactWithTimeout(int fd, size_t size, std::vector<uint8_t>& out) {
    out.clear();
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
    while (out.size() < size) {
        const auto now = std::chrono::steady_clock::now();
        if (now >= deadline) {
            return false;
        }
        const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now);
        pollfd pfd{};
        pfd.fd = fd;
        pfd.events = POLLIN;
        const int timeoutMs = remaining.count() > 0 ? static_cast<int>(remaining.count()) : 1;
        const int pollResult = ::poll(&pfd, 1, timeoutMs);
        if (pollResult < 0 && errno == EINTR) {
            continue;
        }
        if (pollResult <= 0 || (pfd.revents & (POLLERR | POLLHUP | POLLNVAL)) != 0) {
            return false;
        }
        uint8_t buffer[64]{};
        const size_t want = std::min(sizeof(buffer), size - out.size());
        const ssize_t received = ::recv(fd, buffer, want, 0);
        if (received < 0 && errno == EINTR) {
            continue;
        }
        if (received <= 0) {
            return false;
        }
        out.insert(out.end(), buffer, buffer + received);
    }
    return true;
}

std::vector<uint8_t> ReadAvailable(int fd) {
    std::vector<uint8_t> out;
    uint8_t buffer[128]{};
    for (;;) {
        const ssize_t received = ::recv(fd, buffer, sizeof(buffer), MSG_DONTWAIT);
        if (received > 0) {
            out.insert(out.end(), buffer, buffer + received);
            continue;
        }
        if (received < 0 && errno == EINTR) {
            continue;
        }
        return out;
    }
}

bool WaitUntil(std::function<bool()> predicate) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
    while (std::chrono::steady_clock::now() < deadline) {
        if (predicate()) {
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    return predicate();
}

TqClientIngressPeer MakePeer(const std::string& id) {
    TqClientIngressPeer peer{};
    peer.PeerId = id;
    peer.SocksListen = "127.0.0.1:0";
    peer.HttpListen = "";
    return peer;
}

int TestStartAddPeerCountRemoveStop() {
    TqClientIngressReactor reactor;
    if (reactor.AddPeer(MakePeer("before-start"))) {
        return 10;
    }
    if (!reactor.Start()) {
        return 11;
    }

    TqClientIngressPeer peer = MakePeer("peer-a");
    peer.HttpListen = "127.0.0.1:0";
    if (!reactor.AddPeer(peer)) {
        reactor.Stop();
        return 12;
    }
    if (reactor.PeerCountForTest() != 1) {
        reactor.Stop();
        return 13;
    }

    const std::string socksAddress = reactor.SocksListenAddressForTest("peer-a");
    const std::string httpAddress = reactor.HttpListenAddressForTest("peer-a");
    if (socksAddress.empty() || httpAddress.empty() ||
        socksAddress == "127.0.0.1:0" || httpAddress == "127.0.0.1:0") {
        reactor.Stop();
        return 14;
    }

    int socksFd = -1;
    if (!ConnectTo(socksAddress, socksFd)) {
        reactor.Stop();
        return 15;
    }
    CloseFd(socksFd);

    int httpFd = -1;
    if (!ConnectTo(httpAddress, httpFd)) {
        reactor.Stop();
        return 16;
    }
    CloseFd(httpFd);

    if (!reactor.RemovePeer("peer-a")) {
        reactor.Stop();
        return 17;
    }
    if (reactor.PeerCountForTest() != 0) {
        reactor.Stop();
        return 18;
    }

    int removedFd = -1;
    if (ConnectTo(socksAddress, removedFd)) {
        CloseFd(removedFd);
        reactor.Stop();
        return 19;
    }

    reactor.Stop();
    if (reactor.PeerCountForTest() != 0) {
        return 20;
    }
    return 0;
}

int TestDuplicateAddPeerReturnsFalse() {
    TqClientIngressReactor reactor;
    if (!reactor.Start()) {
        return 30;
    }
    if (!reactor.AddPeer(MakePeer("dup"))) {
        reactor.Stop();
        return 31;
    }
    if (reactor.AddPeer(MakePeer("dup"))) {
        reactor.Stop();
        return 32;
    }
    if (reactor.PeerCountForTest() != 1) {
        reactor.Stop();
        return 33;
    }
    reactor.Stop();
    return 0;
}

int TestRemovePeerClosesListenPort() {
    TqClientIngressReactor reactor;
    if (!reactor.Start()) {
        return 40;
    }
    if (!reactor.AddPeer(MakePeer("remove"))) {
        reactor.Stop();
        return 41;
    }

    const std::string socksAddress = reactor.SocksListenAddressForTest("remove");
    if (socksAddress.empty()) {
        reactor.Stop();
        return 42;
    }

    if (!reactor.RemovePeer("remove")) {
        reactor.Stop();
        return 43;
    }
    if (reactor.RemovePeer("remove")) {
        reactor.Stop();
        return 44;
    }
    if (reactor.PeerCountForTest() != 0) {
        reactor.Stop();
        return 45;
    }

    int removedFd = -1;
    if (ConnectTo(socksAddress, removedFd)) {
        CloseFd(removedFd);
        reactor.Stop();
        return 46;
    }

    reactor.Stop();
    return 0;
}

int TestDestructorCleansUp() {
    std::string socksAddress;
    {
        TqClientIngressReactor reactor;
        if (!reactor.Start()) {
            return 50;
        }
        if (!reactor.AddPeer(MakePeer("dtor"))) {
            reactor.Stop();
            return 51;
        }
        socksAddress = reactor.SocksListenAddressForTest("dtor");
        if (socksAddress.empty()) {
            reactor.Stop();
            return 52;
        }
    }

    int fd = -1;
    if (ConnectTo(socksAddress, fd)) {
        CloseFd(fd);
        return 53;
    }
    return 0;
}

int TestSocksHandshakeStartsFakeOpenAndReturnsSuccess() {
    int startCalls = 0;
    int acceptCalls = 0;
    int rejectCalls = 0;
    int cancelCalls = 0;
    std::string host;
    uint16_t port = 0;
    auto* fakeHandle = reinterpret_cast<TqClientTunnelOpenHandle*>(static_cast<uintptr_t>(0x1));

    TqClientIngressPeer peer = MakePeer("socks-open");
    peer.StartTunnel = [&](const TunnelRequest& req, TqSocketHandle fd, TqClientTunnelOpenComplete onComplete) {
        if (fd < 0) {
            return static_cast<TqClientTunnelOpenHandle*>(nullptr);
        }
        ++startCalls;
        host = req.Host;
        port = req.Port;
        std::thread([onComplete, fakeHandle]() {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
            onComplete(fakeHandle, TqTunnelStartResult{true, TqOpenError::Ok, 99});
        }).detach();
        return fakeHandle;
    };
    peer.AcceptTunnel = [&](TqClientTunnelOpenHandle* handle) {
        if (handle != fakeHandle) {
            return false;
        }
        ++acceptCalls;
        return true;
    };
    peer.RejectTunnel = [&](TqClientTunnelOpenHandle* handle) {
        if (handle == fakeHandle) {
            ++rejectCalls;
        }
    };
    peer.CancelTunnel = [&](TqClientTunnelOpenHandle* handle) {
        if (handle == fakeHandle) {
            ++cancelCalls;
        }
    };

    TqClientIngressReactor reactor;
    if (!reactor.Start()) {
        return 70;
    }
    if (!reactor.AddPeer(peer)) {
        reactor.Stop();
        return 71;
    }

    int fd = -1;
    if (!ConnectTo(reactor.SocksListenAddressForTest("socks-open"), fd)) {
        reactor.Stop();
        return 72;
    }

    if (!SendAll(fd, std::vector<uint8_t>{0x05, 0x01, 0x00})) {
        CloseFd(fd);
        reactor.Stop();
        return 73;
    }
    std::vector<uint8_t> response;
    if (!ReadExactWithTimeout(fd, 2, response)) {
        CloseFd(fd);
        reactor.Stop();
        return 74;
    }
    if (response.size() != 2 || response[0] != 0x05 || response[1] != 0x00) {
        CloseFd(fd);
        reactor.Stop();
        return 75;
    }

    const std::vector<uint8_t> connect = {
        0x05, 0x01, 0x00, 0x03, 0x0b,
        'e', 'x', 'a', 'm', 'p', 'l', 'e', '.', 'c', 'o', 'm',
        0x00, 0x50};
    if (!SendAll(fd, connect)) {
        CloseFd(fd);
        reactor.Stop();
        return 76;
    }
    if (!ReadExactWithTimeout(fd, 10, response)) {
        CloseFd(fd);
        reactor.Stop();
        return 77;
    }
    if (response.size() != 10 || response[0] != 0x05 || response[1] != 0x00) {
        CloseFd(fd);
        reactor.Stop();
        return 78;
    }

    if (!WaitUntil([&]() { return acceptCalls == 1; })) {
        CloseFd(fd);
        reactor.Stop();
        return 79;
    }
    if (startCalls != 1 || rejectCalls != 0 || cancelCalls != 0) {
        CloseFd(fd);
        reactor.Stop();
        return 80;
    }
    if (host != "example.com" || port != 80) {
        CloseFd(fd);
        reactor.Stop();
        return 81;
    }

    CloseFd(fd);
    reactor.Stop();
    return 0;
}

int TestHttpConnectSamePacketPayloadRemainsForTunnel() {
    int startCalls = 0;
    int acceptCalls = 0;
    std::vector<uint8_t> tunneledBytes;
    auto* fakeHandle = reinterpret_cast<TqClientTunnelOpenHandle*>(static_cast<uintptr_t>(0x2));

    TqClientIngressPeer peer = MakePeer("http-early");
    peer.HttpListen = "127.0.0.1:0";
    peer.StartTunnel = [&](const TunnelRequest& req, TqSocketHandle fd, TqClientTunnelOpenComplete onComplete) {
        if (fd < 0) {
            return static_cast<TqClientTunnelOpenHandle*>(nullptr);
        }
        if (std::string(req.Host) != "example.com" || req.Port != 443) {
            return static_cast<TqClientTunnelOpenHandle*>(nullptr);
        }
        ++startCalls;
        tunneledBytes = ReadAvailable(fd);
        std::thread([onComplete, fakeHandle]() {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
            onComplete(fakeHandle, TqTunnelStartResult{true, TqOpenError::Ok, 100});
        }).detach();
        return fakeHandle;
    };
    peer.AcceptTunnel = [&](TqClientTunnelOpenHandle* handle) {
        if (handle != fakeHandle) {
            return false;
        }
        ++acceptCalls;
        return true;
    };
    peer.RejectTunnel = [](TqClientTunnelOpenHandle*) {};
    peer.CancelTunnel = [](TqClientTunnelOpenHandle*) {};

    TqClientIngressReactor reactor;
    if (!reactor.Start()) {
        return 90;
    }
    if (!reactor.AddPeer(peer)) {
        reactor.Stop();
        return 91;
    }

    int fd = -1;
    if (!ConnectTo(reactor.HttpListenAddressForTest("http-early"), fd)) {
        reactor.Stop();
        return 92;
    }

    const std::string request =
        "CONNECT example.com:443 HTTP/1.1\r\nHost: example.com:443\r\n\r\npayload";
    if (!SendAll(fd, std::vector<uint8_t>(request.begin(), request.end()))) {
        CloseFd(fd);
        reactor.Stop();
        return 93;
    }

    std::vector<uint8_t> response;
    const std::string expectedResponse = "HTTP/1.1 200 Connection Established\r\n\r\n";
    if (!ReadExactWithTimeout(fd, expectedResponse.size(), response)) {
        CloseFd(fd);
        reactor.Stop();
        return 94;
    }
    if (std::string(response.begin(), response.end()) != expectedResponse) {
        CloseFd(fd);
        reactor.Stop();
        return 95;
    }

    if (!WaitUntil([&]() { return acceptCalls == 1; })) {
        CloseFd(fd);
        reactor.Stop();
        return 96;
    }
    const std::vector<uint8_t> expectedPayload{'p', 'a', 'y', 'l', 'o', 'a', 'd'};
    if (startCalls != 1 || tunneledBytes != expectedPayload) {
        CloseFd(fd);
        reactor.Stop();
        return 97;
    }

    CloseFd(fd);
    reactor.Stop();
    return 0;
}

int TestSocksOpenFailureWritesReplyThenRejects() {
    int rejectCalls = 0;
    auto* fakeHandle = reinterpret_cast<TqClientTunnelOpenHandle*>(static_cast<uintptr_t>(0x3));

    TqClientIngressPeer peer = MakePeer("socks-fail");
    peer.StartTunnel = [&](const TunnelRequest&, TqSocketHandle fd, TqClientTunnelOpenComplete onComplete) {
        if (fd < 0) {
            return static_cast<TqClientTunnelOpenHandle*>(nullptr);
        }
        std::thread([onComplete, fakeHandle]() {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
            onComplete(fakeHandle, TqTunnelStartResult{false, TqOpenError::AclDenied, 0});
        }).detach();
        return fakeHandle;
    };
    peer.AcceptTunnel = [](TqClientTunnelOpenHandle*) {
        return false;
    };
    peer.RejectTunnel = [&](TqClientTunnelOpenHandle* handle) {
        if (handle == fakeHandle) {
            ++rejectCalls;
        }
    };
    peer.CancelTunnel = [](TqClientTunnelOpenHandle*) {};

    TqClientIngressReactor reactor;
    if (!reactor.Start()) {
        return 100;
    }
    if (!reactor.AddPeer(peer)) {
        reactor.Stop();
        return 101;
    }

    int fd = -1;
    if (!ConnectTo(reactor.SocksListenAddressForTest("socks-fail"), fd)) {
        reactor.Stop();
        return 102;
    }
    const std::vector<uint8_t> handshakeAndConnect = {
        0x05, 0x01, 0x00,
        0x05, 0x01, 0x00, 0x03, 0x0b,
        'e', 'x', 'a', 'm', 'p', 'l', 'e', '.', 'c', 'o', 'm',
        0x00, 0x50};
    if (!SendAll(fd, handshakeAndConnect)) {
        CloseFd(fd);
        reactor.Stop();
        return 103;
    }

    std::vector<uint8_t> response;
    if (!ReadExactWithTimeout(fd, 2, response)) {
        CloseFd(fd);
        reactor.Stop();
        return 104;
    }
    if (response.size() != 2 || response[0] != 0x05 || response[1] != 0x00) {
        CloseFd(fd);
        reactor.Stop();
        return 105;
    }
    if (!ReadExactWithTimeout(fd, 10, response)) {
        CloseFd(fd);
        reactor.Stop();
        return 106;
    }
    if (response.size() != 10 || response[0] != 0x05 || response[1] != 0x02) {
        CloseFd(fd);
        reactor.Stop();
        return 107;
    }
    if (!WaitUntil([&]() { return rejectCalls == 1; })) {
        CloseFd(fd);
        reactor.Stop();
        return 108;
    }

    CloseFd(fd);
    reactor.Stop();
    return 0;
}

int TestConcurrentStartStopDoesNotCrashOrDeadlock() {
    for (int i = 0; i < 200; ++i) {
        TqClientIngressReactor reactor;
        if (!reactor.Start()) {
            return 60;
        }

        bool stopOneDone = false;
        bool stopTwoDone = false;
        std::thread stopOne([&]() {
            reactor.Stop();
            stopOneDone = true;
        });
        std::thread stopTwo([&]() {
            reactor.Stop();
            stopTwoDone = true;
        });

        stopOne.join();
        stopTwo.join();
        if (!stopOneDone || !stopTwoDone) {
            return 61;
        }

        if (!reactor.Start()) {
            return 62;
        }
        reactor.Stop();
    }
    return 0;
}

} // namespace

int main() {
    int result = TestStartAddPeerCountRemoveStop();
    if (result != 0) return result;
    result = TestDuplicateAddPeerReturnsFalse();
    if (result != 0) return result;
    result = TestRemovePeerClosesListenPort();
    if (result != 0) return result;
    result = TestDestructorCleansUp();
    if (result != 0) return result;
    result = TestSocksHandshakeStartsFakeOpenAndReturnsSuccess();
    if (result != 0) return result;
    result = TestHttpConnectSamePacketPayloadRemainsForTunnel();
    if (result != 0) return result;
    result = TestSocksOpenFailureWritesReplyThenRejects();
    if (result != 0) return result;
    result = TestConcurrentStartStopDoesNotCrashOrDeadlock();
    if (result != 0) return result;
    return 0;
}
