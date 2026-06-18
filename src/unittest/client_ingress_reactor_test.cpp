#include "client_ingress_reactor.h"

#include <cerrno>
#include <cstdint>
#include <cstring>
#include <netdb.h>
#include <string>
#include <sys/socket.h>
#include <thread>
#include <unistd.h>

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
    result = TestConcurrentStartStopDoesNotCrashOrDeadlock();
    if (result != 0) return result;
    return 0;
}
