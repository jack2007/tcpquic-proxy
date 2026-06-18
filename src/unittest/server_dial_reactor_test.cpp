#include "server_dial_reactor.h"

#include <chrono>
#include <cstdint>
#include <string>
#include <thread>
#include <utility>

#include <cerrno>

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

    TqSocketHandle Release() {
        const TqSocketHandle fd = Fd;
        Fd = TqInvalidSocket;
        return fd;
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

struct FakeDns {
    uint64_t NextId{1};
    uint64_t PendingId{0};
    TqDnsResolveCallback Callback;
    TqDnsResolveResult Result;

    uint64_t Resolve(const std::string&, uint16_t, TqDnsResolveCallback callback) {
        PendingId = NextId++;
        Callback = std::move(callback);
        return PendingId;
    }

    void Cancel(uint64_t id) {
        if (id == PendingId) {
            PendingId = 0;
            Callback = {};
        }
    }

    bool RunOnce(int) {
        if (PendingId == 0 || !Callback) {
            return false;
        }
        TqDnsResolveCallback callback = std::move(Callback);
        PendingId = 0;
        callback(Result);
        return true;
    }
};

TqAcl AllowAllAcl() {
    TqAcl acl;
    acl.AllowCidrs = {"0.0.0.0/0", "::/0"};
    return acl;
}

bool MakeLoopbackAddress(sockaddr_storage& storage, uint16_t port) {
    storage = {};
    auto* addr = reinterpret_cast<sockaddr_in*>(&storage);
    storage.ss_family = AF_INET;
    addr->sin_port = htons(port);
    return TqInetPton(AF_INET, "127.0.0.1", &addr->sin_addr);
}

bool RunUntil(bool& done, TqServerDialReactor& reactor, int timeoutMs) {
    const auto start = std::chrono::steady_clock::now();
    while (!done) {
        (void)reactor.RunOnce(25);
        const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - start);
        if (elapsed.count() >= timeoutMs) {
            return false;
        }
    }
    return true;
}

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
    if (::bind(listener.Get(), reinterpret_cast<const sockaddr*>(&addr), static_cast<socklen_t>(sizeof(addr))) != 0) {
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

uint16_t ReserveThenCloseLoopbackPort() {
    ScopedSocket listener;
    uint16_t port = 0;
    if (!MakeLoopbackListener(listener, port)) {
        return 0;
    }
    listener.Reset();
    return port;
}

int PlatformRefusedError() {
#if defined(_WIN32)
    return WSAECONNREFUSED;
#else
    return ECONNREFUSED;
#endif
}

int PlatformInProgressError() {
#if defined(_WIN32)
    return WSAEWOULDBLOCK;
#else
    return EINPROGRESS;
#endif
}

int TestDnsCallbackConnectSuccess() {
    TqSocketStartup startup;
    if (!startup.Ok()) {
        return 10;
    }

    sockaddr_storage address{};
    if (!MakeLoopbackAddress(address, 443)) {
        return 11;
    }

    FakeDns fakeDns;
    fakeDns.Result.Completed = true;
    fakeDns.Result.Success = true;
    fakeDns.Result.Addresses.push_back(address);

    int connectCalls = 0;
    int setBlockingCalls = 0;
    TqServerDialReactor::TestHooks hooks;
    hooks.Resolve = [&](const std::string& host, uint16_t port, TqDnsResolveCallback callback) {
        return fakeDns.Resolve(host, port, std::move(callback));
    };
    hooks.CancelResolve = [&](uint64_t id) {
        fakeDns.Cancel(id);
    };
    hooks.RunDnsOnce = [&](int timeoutMs) {
        return fakeDns.RunOnce(timeoutMs);
    };
    hooks.Connect = [&](TqSocketHandle, const sockaddr*, socklen_t) {
        ++connectCalls;
        return 0;
    };
    hooks.SetBlocking = [&](TqSocketHandle) {
        ++setBlockingCalls;
        return true;
    };

    TqServerDialReactor reactor(AllowAllAcl(), hooks);
    if (!reactor.Start()) {
        return 12;
    }

    bool completed = false;
    TqServerDialResult observed{};
    TqServerDialRequest request;
    request.Host = "dns-success.test";
    request.Port = 443;
    request.Complete = [&](const TqServerDialResult& result) {
        completed = true;
        observed = result;
    };

    const uint64_t token = reactor.Submit(std::move(request));
    if (token == 0) {
        reactor.Stop();
        return 13;
    }
    if (!RunUntil(completed, reactor, 1000)) {
        reactor.Stop();
        return 14;
    }
    reactor.Stop();

    ScopedSocket connected(observed.Fd);
    if (!observed.Done || observed.Error != TqOpenError::Ok) {
        return 15;
    }
    if (!TqSocketValid(connected.Get())) {
        return 16;
    }
    if (connectCalls != 1 || setBlockingCalls != 1) {
        return 17;
    }
    return 0;
}

int TestConnectRefusedMapsError() {
    TqSocketStartup startup;
    if (!startup.Ok()) {
        return 20;
    }

    TqServerDialReactor::TestHooks hooks;
    hooks.Connect = [](TqSocketHandle, const sockaddr*, socklen_t) {
        return -1;
    };
    hooks.GetLastSocketError = [](TqSocketHandle) {
        return PlatformRefusedError();
    };

    TqServerDialReactor reactor(AllowAllAcl(), hooks);
    if (!reactor.Start()) {
        return 21;
    }

    bool completed = false;
    TqServerDialResult observed{};
    TqServerDialRequest request;
    request.Host = "127.0.0.1";
    request.Port = 443;
    request.Complete = [&](const TqServerDialResult& result) {
        completed = true;
        observed = result;
    };

    const uint64_t token = reactor.Submit(std::move(request));
    if (token == 0) {
        reactor.Stop();
        return 22;
    }
    if (!RunUntil(completed, reactor, 1000)) {
        reactor.Stop();
        return 23;
    }
    reactor.Stop();

    if (!observed.Done || observed.Error != TqOpenError::TcpRefused) {
        return 24;
    }
    if (TqSocketValid(observed.Fd)) {
        TqCloseSocket(observed.Fd);
        return 25;
    }
    return 0;
}

int TestCancelSuppressesPendingDnsCompletion() {
    TqSocketStartup startup;
    if (!startup.Ok()) {
        return 30;
    }

    FakeDns fakeDns;
    fakeDns.Result.Completed = true;
    fakeDns.Result.Success = true;

    TqServerDialReactor::TestHooks hooks;
    hooks.Resolve = [&](const std::string& host, uint16_t port, TqDnsResolveCallback callback) {
        return fakeDns.Resolve(host, port, std::move(callback));
    };
    hooks.CancelResolve = [&](uint64_t id) {
        fakeDns.Cancel(id);
    };
    hooks.RunDnsOnce = [&](int timeoutMs) {
        return fakeDns.RunOnce(timeoutMs);
    };

    TqServerDialReactor reactor(AllowAllAcl(), hooks);
    if (!reactor.Start()) {
        return 31;
    }

    bool completed = false;
    TqServerDialRequest request;
    request.Host = "pending-cancel.test";
    request.Port = 443;
    request.Complete = [&](const TqServerDialResult&) {
        completed = true;
    };

    const uint64_t token = reactor.Submit(std::move(request));
    if (token == 0) {
        reactor.Stop();
        return 32;
    }
    reactor.Cancel(token);
    (void)fakeDns.RunOnce(0);
    reactor.Stop();

    if (completed || fakeDns.PendingId != 0) {
        return 33;
    }
    return 0;
}

int TestTimeoutRunOnceCompletes() {
    TqSocketStartup startup;
    if (!startup.Ok()) {
        return 40;
    }

    TqServerDialReactor::TestHooks hooks;
    hooks.Connect = [](TqSocketHandle, const sockaddr*, socklen_t) {
        return -1;
    };
    hooks.GetLastSocketError = [](TqSocketHandle) {
        return PlatformInProgressError();
    };
    hooks.ConnectTimeoutMs = 0;

    TqServerDialReactor reactor(AllowAllAcl(), hooks);
    if (!reactor.Start()) {
        return 41;
    }

    bool completed = false;
    TqServerDialResult observed{};
    TqServerDialRequest request;
    request.Host = "127.0.0.1";
    request.Port = 443;
    request.Complete = [&](const TqServerDialResult& result) {
        completed = true;
        observed = result;
    };

    const uint64_t token = reactor.Submit(std::move(request));
    if (token == 0) {
        reactor.Stop();
        return 42;
    }
    if (!RunUntil(completed, reactor, 1000)) {
        reactor.Stop();
        return 43;
    }
    reactor.Stop();

    if (!observed.Done || observed.Error != TqOpenError::TcpTimeout) {
        return 44;
    }
    if (TqSocketValid(observed.Fd)) {
        TqCloseSocket(observed.Fd);
        return 45;
    }
    return 0;
}

int TestSocketErrorMappingFromReactorReady() {
    TqSocketStartup startup;
    if (!startup.Ok()) {
        return 50;
    }

    const uint16_t port = ReserveThenCloseLoopbackPort();
    if (port == 0) {
        return 51;
    }

    int socketError = 0;
    int getSocketErrorCalls = 0;
    TqServerDialReactor::TestHooks hooks;
    hooks.GetSocketError = [&](TqSocketHandle fd, int* error) {
        ++getSocketErrorCalls;
        const bool ok = TqGetSocketError(fd, *error);
        socketError = *error;
        return ok ? 0 : -1;
    };

    TqServerDialReactor reactor(AllowAllAcl(), hooks);
    if (!reactor.Start()) {
        return 52;
    }

    bool completed = false;
    TqServerDialResult observed{};
    TqServerDialRequest request;
    request.Host = "127.0.0.1";
    request.Port = port;
    request.Complete = [&](const TqServerDialResult& result) {
        completed = true;
        observed = result;
    };

    const uint64_t token = reactor.Submit(std::move(request));
    if (token == 0) {
        reactor.Stop();
        return 53;
    }
    if (!RunUntil(completed, reactor, 3000)) {
        reactor.Stop();
        return 54;
    }
    reactor.Stop();

    if (getSocketErrorCalls == 0 || socketError == 0) {
        return 55;
    }
    if (!observed.Done || observed.Error != TqOpenError::TcpRefused) {
        return 56;
    }
    if (TqSocketValid(observed.Fd)) {
        TqCloseSocket(observed.Fd);
        return 57;
    }
    return 0;
}

} // namespace

int main() {
    int result = TestDnsCallbackConnectSuccess();
    if (result != 0) {
        return result;
    }
    result = TestConnectRefusedMapsError();
    if (result != 0) {
        return result;
    }
    result = TestCancelSuppressesPendingDnsCompletion();
    if (result != 0) {
        return result;
    }
    result = TestTimeoutRunOnceCompletes();
    if (result != 0) {
        return result;
    }
    result = TestSocketErrorMappingFromReactorReady();
    if (result != 0) {
        return result;
    }
    return 0;
}
