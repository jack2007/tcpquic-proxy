#include "client_ingress_reactor.h"

#include <chrono>
#include <cstdint>
#include <cstring>
#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#if !defined(_WIN32)
#include <netdb.h>
#endif

namespace {

void CloseFd(TqSocketHandle& fd) {
    if (TqSocketValid(fd)) {
        TqCloseSocket(fd);
        fd = TqInvalidSocket;
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

bool ConnectTo(const std::string& address, TqSocketHandle& connectedFd) {
    connectedFd = TqInvalidSocket;

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
        TqSocketHandle fd = ::socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
        if (!TqSocketValid(fd)) {
            continue;
        }
        if (::connect(fd, ai->ai_addr, static_cast<int>(ai->ai_addrlen)) == 0 && TqSetNonBlocking(fd)) {
            connectedFd = fd;
            ::freeaddrinfo(result);
            return true;
        }
        CloseFd(fd);
    }

    ::freeaddrinfo(result);
    return false;
}

bool SendAll(TqSocketHandle fd, const std::vector<uint8_t>& data) {
    size_t sent = 0;
    while (sent < data.size()) {
        const int result = TqSend(fd, data.data() + sent, data.size() - sent, TqSendFlags::None);
        if (result < 0 && TqSocketInterrupted(TqLastSocketError())) {
            continue;
        }
        if (result <= 0) {
            return false;
        }
        sent += static_cast<size_t>(result);
    }
    return true;
}

bool ReadExactWithTimeout(TqSocketHandle fd, size_t size, std::vector<uint8_t>& out) {
    out.clear();
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
    while (out.size() < size) {
        if (std::chrono::steady_clock::now() >= deadline) {
            return false;
        }
        uint8_t buffer[64]{};
        const size_t want = std::min(sizeof(buffer), size - out.size());
        const int received = TqRecv(fd, buffer, want, TqRecvFlags::None);
        if (received < 0 && TqSocketInterrupted(TqLastSocketError())) {
            continue;
        }
        if (received < 0 && TqSocketWouldBlock(TqLastSocketError())) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
            continue;
        }
        if (received <= 0) {
            return false;
        }
        out.insert(out.end(), buffer, buffer + received);
    }
    return true;
}

std::vector<uint8_t> ReadAvailable(TqSocketHandle fd) {
    std::vector<uint8_t> out;
    uint8_t buffer[128]{};
    for (;;) {
        const int received = TqRecv(fd, buffer, sizeof(buffer), TqRecvFlags::DontWait);
        if (received > 0) {
            out.insert(out.end(), buffer, buffer + received);
            continue;
        }
        if (received < 0 && TqSocketInterrupted(TqLastSocketError())) {
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

bool WaitBrieflyFor(std::function<bool()> predicate) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(200);
    while (std::chrono::steady_clock::now() < deadline) {
        if (predicate()) {
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    return predicate();
}

struct DeferredOpen {
    std::mutex Mutex;
    std::condition_variable Cv;
    TqClientTunnelOpenComplete Complete;
    bool Started{false};
};

bool StartSocksOpen(
    TqClientIngressReactor& reactor,
    const std::string& peerId,
    TqSocketHandle& fd,
    std::vector<uint8_t>& response) {
    fd = TqInvalidSocket;
    if (!ConnectTo(reactor.SocksListenAddressForTest(peerId), fd)) {
        return false;
    }
    const std::vector<uint8_t> handshakeAndConnect = {
        0x05, 0x01, 0x00,
        0x05, 0x01, 0x00, 0x03, 0x0b,
        'e', 'x', 'a', 'm', 'p', 'l', 'e', '.', 'c', 'o', 'm',
        0x00, 0x50};
    if (!SendAll(fd, handshakeAndConnect)) {
        return false;
    }
    return ReadExactWithTimeout(fd, 2, response) &&
        response.size() == 2 && response[0] == 0x05 && response[1] == 0x00;
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

    TqSocketHandle socksFd = TqInvalidSocket;
    if (!ConnectTo(socksAddress, socksFd)) {
        reactor.Stop();
        return 15;
    }
    CloseFd(socksFd);

    TqSocketHandle httpFd = TqInvalidSocket;
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

    TqSocketHandle removedFd = TqInvalidSocket;
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

    TqSocketHandle removedFd = TqInvalidSocket;
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

    TqSocketHandle fd = TqInvalidSocket;
    if (ConnectTo(socksAddress, fd)) {
        CloseFd(fd);
        return 53;
    }
    return 0;
}

int TestSocksHandshakeStartsFakeOpenAndReturnsSuccess() {
    int startCalls = 0;
    std::atomic<int> acceptCalls{0};
    std::atomic<int> rejectCalls{0};
    std::atomic<int> cancelCalls{0};
    std::string host;
    uint16_t port = 0;
    auto* fakeHandle = reinterpret_cast<TqClientTunnelOpenHandle*>(static_cast<uintptr_t>(0x1));

    TqClientIngressPeer peer = MakePeer("socks-open");
    peer.StartTunnel = [&](const TunnelRequest& req, TqSocketHandle fd, TqClientTunnelOpenComplete onComplete) {
        if (!TqSocketValid(fd)) {
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

    TqSocketHandle fd = TqInvalidSocket;
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
        if (!TqSocketValid(fd)) {
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

    TqSocketHandle fd = TqInvalidSocket;
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
        if (!TqSocketValid(fd)) {
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

    TqSocketHandle fd = TqInvalidSocket;
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

int TestRemovePeerBeforeLateCompletionCancelsOnce() {
    std::atomic<int> acceptCalls{0};
    std::atomic<int> rejectCalls{0};
    std::atomic<int> cancelCalls{0};
    auto* fakeHandle = reinterpret_cast<TqClientTunnelOpenHandle*>(static_cast<uintptr_t>(0x4));
    auto deferred = std::make_shared<DeferredOpen>();

    TqClientIngressPeer peer = MakePeer("late-remove");
    peer.StartTunnel = [deferred, fakeHandle](const TunnelRequest&, TqSocketHandle fd, TqClientTunnelOpenComplete onComplete) {
        if (!TqSocketValid(fd)) {
            return static_cast<TqClientTunnelOpenHandle*>(nullptr);
        }
        {
            std::lock_guard<std::mutex> lock(deferred->Mutex);
            deferred->Complete = std::move(onComplete);
            deferred->Started = true;
        }
        deferred->Cv.notify_all();
        return fakeHandle;
    };
    peer.AcceptTunnel = [&](TqClientTunnelOpenHandle*) {
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
        return 110;
    }
    if (!reactor.AddPeer(peer)) {
        reactor.Stop();
        return 111;
    }

    TqSocketHandle fd = TqInvalidSocket;
    std::vector<uint8_t> response;
    if (!StartSocksOpen(reactor, "late-remove", fd, response)) {
        CloseFd(fd);
        reactor.Stop();
        return 112;
    }
    if (!WaitUntil([&]() {
            std::lock_guard<std::mutex> lock(deferred->Mutex);
            return deferred->Started;
        })) {
        CloseFd(fd);
        reactor.Stop();
        return 113;
    }
    if (!reactor.RemovePeer("late-remove")) {
        CloseFd(fd);
        reactor.Stop();
        return 114;
    }
    if (!WaitUntil([&]() { return cancelCalls == 1; })) {
        CloseFd(fd);
        reactor.Stop();
        return 115;
    }

    TqClientTunnelOpenComplete complete;
    {
        std::lock_guard<std::mutex> lock(deferred->Mutex);
        complete = deferred->Complete;
    }
    complete(fakeHandle, TqTunnelStartResult{true, TqOpenError::Ok, 101});
    if (WaitBrieflyFor([&]() { return acceptCalls != 0 || rejectCalls != 0 || cancelCalls != 1; })) {
        CloseFd(fd);
        reactor.Stop();
        return 116;
    }

    CloseFd(fd);
    reactor.Stop();
    return 0;
}

int TestStopBeforeQueuedCompletionRejectsOnce() {
    std::atomic<int> acceptCalls{0};
    std::atomic<int> rejectCalls{0};
    std::atomic<int> cancelCalls{0};
    auto* fakeHandle = reinterpret_cast<TqClientTunnelOpenHandle*>(static_cast<uintptr_t>(0x5));
    auto deferred = std::make_shared<DeferredOpen>();

    TqClientIngressPeer peer = MakePeer("late-stop");
    peer.StartTunnel = [deferred, fakeHandle](const TunnelRequest&, TqSocketHandle fd, TqClientTunnelOpenComplete onComplete) {
        if (!TqSocketValid(fd)) {
            return static_cast<TqClientTunnelOpenHandle*>(nullptr);
        }
        {
            std::lock_guard<std::mutex> lock(deferred->Mutex);
            deferred->Complete = std::move(onComplete);
            deferred->Started = true;
        }
        deferred->Cv.notify_all();
        return fakeHandle;
    };
    peer.AcceptTunnel = [&](TqClientTunnelOpenHandle*) {
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
        return 120;
    }
    if (!reactor.AddPeer(peer)) {
        reactor.Stop();
        return 121;
    }
    TqSocketHandle fd = TqInvalidSocket;
    std::vector<uint8_t> response;
    if (!StartSocksOpen(reactor, "late-stop", fd, response)) {
        CloseFd(fd);
        reactor.Stop();
        return 122;
    }
    if (!WaitUntil([&]() {
            std::lock_guard<std::mutex> lock(deferred->Mutex);
            return deferred->Started;
        })) {
        CloseFd(fd);
        reactor.Stop();
        return 123;
    }

    TqClientTunnelOpenComplete complete;
    {
        std::lock_guard<std::mutex> lock(deferred->Mutex);
        complete = deferred->Complete;
    }
    std::thread completeThread([complete, fakeHandle]() mutable {
        complete(fakeHandle, TqTunnelStartResult{true, TqOpenError::Ok, 102});
    });
    reactor.Stop();
    completeThread.join();

    if (acceptCalls != 0 || rejectCalls + cancelCalls != 1) {
        CloseFd(fd);
        return 124;
    }
    CloseFd(fd);
    return 0;
}

int TestCompletionAfterStopRejectsOnce() {
    std::atomic<int> acceptCalls{0};
    std::atomic<int> rejectCalls{0};
    std::atomic<int> cancelCalls{0};
    auto* fakeHandle = reinterpret_cast<TqClientTunnelOpenHandle*>(static_cast<uintptr_t>(0x6));
    auto deferred = std::make_shared<DeferredOpen>();

    TqClientIngressPeer peer = MakePeer("post-stop");
    peer.StartTunnel = [deferred, fakeHandle](const TunnelRequest&, TqSocketHandle fd, TqClientTunnelOpenComplete onComplete) {
        if (!TqSocketValid(fd)) {
            return static_cast<TqClientTunnelOpenHandle*>(nullptr);
        }
        {
            std::lock_guard<std::mutex> lock(deferred->Mutex);
            deferred->Complete = std::move(onComplete);
            deferred->Started = true;
        }
        deferred->Cv.notify_all();
        return fakeHandle;
    };
    peer.AcceptTunnel = [&](TqClientTunnelOpenHandle*) {
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
        return 130;
    }
    if (!reactor.AddPeer(peer)) {
        reactor.Stop();
        return 131;
    }
    TqSocketHandle fd = TqInvalidSocket;
    std::vector<uint8_t> response;
    if (!StartSocksOpen(reactor, "post-stop", fd, response)) {
        CloseFd(fd);
        reactor.Stop();
        return 132;
    }
    if (!WaitUntil([&]() {
            std::lock_guard<std::mutex> lock(deferred->Mutex);
            return deferred->Started;
        })) {
        CloseFd(fd);
        reactor.Stop();
        return 133;
    }

    reactor.Stop();
    if (cancelCalls != 1) {
        CloseFd(fd);
        return 134;
    }
    TqClientTunnelOpenComplete complete;
    {
        std::lock_guard<std::mutex> lock(deferred->Mutex);
        complete = deferred->Complete;
    }
    complete(fakeHandle, TqTunnelStartResult{true, TqOpenError::Ok, 103});
    if (WaitBrieflyFor([&]() { return acceptCalls != 0 || rejectCalls != 0 || cancelCalls != 1; })) {
        CloseFd(fd);
        return 135;
    }
    CloseFd(fd);
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
    TqSocketStartup socketStartup;
    if (!socketStartup.Ok()) {
        return 1;
    }

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
    result = TestRemovePeerBeforeLateCompletionCancelsOnce();
    if (result != 0) return result;
    result = TestStopBeforeQueuedCompletionRejectsOnce();
    if (result != 0) return result;
    result = TestCompletionAfterStopRejectsOnce();
    if (result != 0) return result;
    result = TestConcurrentStartStopDoesNotCrashOrDeadlock();
    if (result != 0) return result;
    return 0;
}
