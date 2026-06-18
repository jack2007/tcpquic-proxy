#include "server_dial_reactor.h"

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <thread>

#include <cerrno>
#include <netinet/in.h>
#include <sys/socket.h>

namespace {

class ScopedSocket {
public:
    explicit ScopedSocket(TqSocketHandle fd = TqInvalidSocket)
        : Fd(fd) {
    }

    ~ScopedSocket() {
        Reset();
    }

    ScopedSocket(const ScopedSocket&) = delete;
    ScopedSocket& operator=(const ScopedSocket&) = delete;

    TqSocketHandle Get() const {
        return Fd;
    }

    void Reset(TqSocketHandle fd = TqInvalidSocket) {
        if (TqSocketValid(Fd)) {
            TqCloseSocket(Fd);
        }
        Fd = fd;
    }

private:
    TqSocketHandle Fd{TqInvalidSocket};
};

bool MakeLoopbackListener(ScopedSocket& listener, uint16_t& port) {
    listener.Reset(::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP));
    if (!TqSocketValid(listener.Get())) {
        return false;
    }
    if (!TqSetReuseAddr(listener.Get())) {
        return false;
    }

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = 0;
    if (::bind(listener.Get(), reinterpret_cast<const sockaddr*>(&addr), sizeof(addr)) != 0) {
        return false;
    }
    if (::listen(listener.Get(), 8) != 0) {
        return false;
    }
    if (!TqSetNonBlocking(listener.Get())) {
        return false;
    }

    socklen_t len = sizeof(addr);
    if (::getsockname(listener.Get(), reinterpret_cast<sockaddr*>(&addr), &len) != 0) {
        return false;
    }
    port = ntohs(addr.sin_port);
    return port != 0;
}

bool AcceptOne(TqSocketHandle listener, int timeoutMs) {
    const auto start = std::chrono::steady_clock::now();
    for (;;) {
        sockaddr_storage peer{};
        socklen_t peerLen = sizeof(peer);
        ScopedSocket accepted(::accept(listener, reinterpret_cast<sockaddr*>(&peer), &peerLen));
        if (TqSocketValid(accepted.Get())) {
            return true;
        }

        const int error = TqLastSocketError();
        if (error != EAGAIN && error != EWOULDBLOCK && error != EINTR) {
            return false;
        }

        const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - start);
        if (elapsed.count() >= timeoutMs) {
            return false;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
}

int TestWorkerCompletesLoopbackConnectWithoutManualRunOnce() {
    TqSocketStartup startup;
    if (!startup.Ok()) {
        return 1;
    }

    ScopedSocket listener;
    uint16_t port = 0;
    if (!MakeLoopbackListener(listener, port)) {
        return 2;
    }

    TqServerDialReactor reactor;
    if (!reactor.Start()) {
        return 3;
    }

    std::mutex mutex;
    std::condition_variable cv;
    bool completed = false;
    TqServerDialResult observed{};

    TqServerDialRequest request;
    request.Host = "127.0.0.1";
    request.Port = port;
    request.Complete = [&](const TqServerDialResult& result) {
        {
            std::lock_guard<std::mutex> lock(mutex);
            completed = true;
            observed = result;
        }
        cv.notify_one();
    };

    const uint64_t token = reactor.Submit(std::move(request));
    if (token == 0) {
        reactor.Stop();
        return 4;
    }

    {
        std::unique_lock<std::mutex> lock(mutex);
        if (!cv.wait_for(lock, std::chrono::seconds(3), [&] { return completed; })) {
            reactor.Stop();
            return 5;
        }
    }

    reactor.Stop();

    ScopedSocket connected(observed.Fd);
    if (!observed.Done || observed.Error != TqOpenError::Ok) {
        return 6;
    }
    if (!TqSocketValid(connected.Get())) {
        return 7;
    }
    if (!AcceptOne(listener.Get(), 1000)) {
        return 8;
    }
    return 0;
}

} // namespace

int main() {
    if (int rc = TestWorkerCompletesLoopbackConnectWithoutManualRunOnce()) return rc;
    return 0;
}
