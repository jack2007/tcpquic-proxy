#include "server_dial_reactor.h"

#include <chrono>
#include <cstdint>
#include <future>
#include <memory>
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
        if (!TqSocketWouldBlock(error) && !TqSocketInterrupted(error)) {
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

int PlatformAccessError() {
#if defined(_WIN32)
    return WSAEACCES;
#else
    return EACCES;
#endif
}

int TestLiteralConnectSucceeds() {
    TqSocketStartup startup;
    if (!startup.Ok()) {
        return 1;
    }

    ScopedSocket listener;
    uint16_t port = 0;
    if (!MakeLoopbackListener(listener, port)) {
        return 2;
    }

    TqServerDialReactor reactor(AllowAllAcl());
    if (!reactor.Start()) {
        return 3;
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
        return 4;
    }
    if (!RunUntil(completed, reactor, 3000)) {
        reactor.Stop();
        return 5;
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

int TestAclDenyReturnsAclDenied() {
    TqSocketStartup startup;
    if (!startup.Ok()) {
        return 20;
    }

    TqAcl acl;
    acl.AllowCidrs = {"10.0.0.0/8"};
    TqServerDialReactor reactor(acl);
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

    if (!observed.Done || observed.Error != TqOpenError::AclDenied) {
        return 24;
    }
    if (TqSocketValid(observed.Fd)) {
        TqCloseSocket(observed.Fd);
        return 25;
    }
    return 0;
}

int TestDnsFailureReturnsDnsFailed() {
    TqSocketStartup startup;
    if (!startup.Ok()) {
        return 30;
    }

    TqServerDialReactor::TestHooks hooks;
    hooks.Resolve = [](const std::string&, uint16_t, TqDnsResolveCallback callback) {
        TqDnsResolveResult result;
        result.Completed = true;
        result.Success = false;
        result.Status = -1;
        callback(result);
        return 1;
    };
    hooks.RunDnsOnce = [](int) {
        return false;
    };

    TqServerDialReactor reactor(AllowAllAcl(), hooks);
    if (!reactor.Start()) {
        return 31;
    }

    bool completed = false;
    TqServerDialResult observed{};
    TqServerDialRequest request;
    request.Host = "dns-failure.test";
    request.Port = 443;
    request.Complete = [&](const TqServerDialResult& result) {
        completed = true;
        observed = result;
    };

    const uint64_t token = reactor.Submit(std::move(request));
    if (token == 0) {
        reactor.Stop();
        return 32;
    }
    if (!RunUntil(completed, reactor, 1000)) {
        reactor.Stop();
        return 33;
    }
    reactor.Stop();

    if (!observed.Done || observed.Error != TqOpenError::DnsFailed) {
        return 34;
    }
    if (TqSocketValid(observed.Fd)) {
        TqCloseSocket(observed.Fd);
        return 35;
    }
    return 0;
}

int TestConnectRefusedMapsError() {
    TqSocketStartup startup;
    if (!startup.Ok()) {
        return 40;
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

    if (!observed.Done || observed.Error != TqOpenError::TcpRefused) {
        return 44;
    }
    if (TqSocketValid(observed.Fd)) {
        TqCloseSocket(observed.Fd);
        return 45;
    }
    return 0;
}

int TestUnknownConnectFailureReturnsInternal() {
    TqSocketStartup startup;
    if (!startup.Ok()) {
        return 50;
    }

    bool connectCalled = false;
    TqServerDialReactor::TestHooks hooks;
    hooks.Connect = [&](TqSocketHandle, const sockaddr*, socklen_t) {
        connectCalled = true;
        return -1;
    };
    hooks.GetLastSocketError = [](TqSocketHandle) {
        return PlatformAccessError();
    };

    TqServerDialReactor reactor(AllowAllAcl(), hooks);
    if (!reactor.Start()) {
        return 51;
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
        return 52;
    }
    if (!RunUntil(completed, reactor, 1000)) {
        reactor.Stop();
        return 53;
    }
    reactor.Stop();

    if (!connectCalled) {
        return 54;
    }
    if (!observed.Done || observed.Error != TqOpenError::Internal) {
        return 55;
    }
    if (TqSocketValid(observed.Fd)) {
        TqCloseSocket(observed.Fd);
        return 56;
    }
    return 0;
}

int TestTimeoutRunOnceCompletes() {
    TqSocketStartup startup;
    if (!startup.Ok()) {
        return 60;
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
        return 61;
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
        return 62;
    }
    if (!RunUntil(completed, reactor, 1000)) {
        reactor.Stop();
        return 63;
    }
    reactor.Stop();

    if (!observed.Done || observed.Error != TqOpenError::TcpTimeout) {
        return 64;
    }
    if (TqSocketValid(observed.Fd)) {
        TqCloseSocket(observed.Fd);
        return 65;
    }
    return 0;
}

int TestSocketErrorMappingFromReactorReady() {
    TqSocketStartup startup;
    if (!startup.Ok()) {
        return 70;
    }

    ScopedSocket listener;
    uint16_t port = 0;
    if (!MakeLoopbackListener(listener, port)) {
        return 71;
    }

    int getSocketErrorCalls = 0;
    TqServerDialReactor::TestHooks hooks;
    hooks.GetSocketError = [&](TqSocketHandle, int* error) {
        ++getSocketErrorCalls;
        *error = PlatformRefusedError();
        return 0;
    };

    TqServerDialReactor reactor(AllowAllAcl(), hooks);
    if (!reactor.Start()) {
        return 72;
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
        return 73;
    }
    if (!RunUntil(completed, reactor, 3000)) {
        reactor.Stop();
        return 74;
    }
    reactor.Stop();

    if (getSocketErrorCalls == 0) {
        return 75;
    }
    if (!observed.Done || observed.Error != TqOpenError::TcpRefused) {
        return 76;
    }
    if (TqSocketValid(observed.Fd)) {
        TqCloseSocket(observed.Fd);
        return 77;
    }
    return 0;
}

int TestCancelSuppressesPendingDnsCompletion() {
    TqSocketStartup startup;
    if (!startup.Ok()) {
        return 80;
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
        return 81;
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
        return 82;
    }
    reactor.Cancel(token);
    (void)fakeDns.RunOnce(0);
    reactor.Stop();

    if (completed || fakeDns.PendingId != 0) {
        return 83;
    }
    return 0;
}

int TestStopSuppressesPendingCompletion() {
    TqSocketStartup startup;
    if (!startup.Ok()) {
        return 90;
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
        return 91;
    }

    bool completed = false;
    TqServerDialRequest request;
    request.Host = "pending-stop.test";
    request.Port = 443;
    request.Complete = [&](const TqServerDialResult&) {
        completed = true;
    };

    const uint64_t token = reactor.Submit(std::move(request));
    if (token == 0) {
        reactor.Stop();
        return 92;
    }
    reactor.Stop();
    (void)fakeDns.RunOnce(0);

    if (completed || fakeDns.PendingId != 0) {
        return 93;
    }
    return 0;
}

int TestCancelClosesPendingConnectFdAndSuppressesCompletion() {
    TqSocketStartup startup;
    if (!startup.Ok()) {
        return 100;
    }

    int closeCount = 0;
    bool completed = false;
    TqServerDialReactor::TestHooks hooks;
    hooks.Connect = [](TqSocketHandle, const sockaddr*, socklen_t) {
        return -1;
    };
    hooks.GetLastSocketError = [](TqSocketHandle) {
        return PlatformInProgressError();
    };
    hooks.CloseSocket = [&](TqSocketHandle fd) {
        ++closeCount;
        TqCloseSocket(fd);
    };

    TqServerDialReactor reactor(AllowAllAcl(), hooks);
    if (!reactor.Start()) {
        return 101;
    }

    TqServerDialRequest request;
    request.Host = "127.0.0.1";
    request.Port = 443;
    request.Complete = [&](const TqServerDialResult&) {
        completed = true;
    };

    const uint64_t token = reactor.Submit(std::move(request));
    if (token == 0) {
        reactor.Stop();
        return 102;
    }
    reactor.Cancel(token);
    for (int i = 0; i < 5; ++i) {
        (void)reactor.RunOnce(0);
    }
    reactor.Stop();

    if (completed) {
        return 103;
    }
    if (closeCount != 1) {
        return 104;
    }
    return 0;
}

int TestStopClosesPendingConnectFdAndSuppressesCompletion() {
    TqSocketStartup startup;
    if (!startup.Ok()) {
        return 110;
    }

    int closeCount = 0;
    bool completed = false;
    TqServerDialReactor::TestHooks hooks;
    hooks.Connect = [](TqSocketHandle, const sockaddr*, socklen_t) {
        return -1;
    };
    hooks.GetLastSocketError = [](TqSocketHandle) {
        return PlatformInProgressError();
    };
    hooks.CloseSocket = [&](TqSocketHandle fd) {
        ++closeCount;
        TqCloseSocket(fd);
    };

    TqServerDialReactor reactor(AllowAllAcl(), hooks);
    if (!reactor.Start()) {
        return 111;
    }

    TqServerDialRequest request;
    request.Host = "127.0.0.1";
    request.Port = 443;
    request.Complete = [&](const TqServerDialResult&) {
        completed = true;
    };

    const uint64_t token = reactor.Submit(std::move(request));
    if (token == 0) {
        reactor.Stop();
        return 112;
    }
    reactor.Stop();

    if (completed) {
        return 113;
    }
    if (closeCount != 1) {
        return 114;
    }
    return 0;
}

int TestDestructorClosesPendingConnectFd() {
    TqSocketStartup startup;
    if (!startup.Ok()) {
        return 120;
    }

    int closeCount = 0;
    bool completed = false;
    {
        TqServerDialReactor::TestHooks hooks;
        hooks.Connect = [](TqSocketHandle, const sockaddr*, socklen_t) {
            return -1;
        };
        hooks.GetLastSocketError = [](TqSocketHandle) {
            return PlatformInProgressError();
        };
        hooks.CloseSocket = [&](TqSocketHandle fd) {
            ++closeCount;
            TqCloseSocket(fd);
        };

        TqServerDialReactor reactor(AllowAllAcl(), hooks);
        if (!reactor.Start()) {
            return 121;
        }

        TqServerDialRequest request;
        request.Host = "127.0.0.1";
        request.Port = 443;
        request.Complete = [&](const TqServerDialResult&) {
            completed = true;
        };

        const uint64_t token = reactor.Submit(std::move(request));
        if (token == 0) {
            return 122;
        }
    }

    if (completed) {
        return 123;
    }
    if (closeCount != 1) {
        return 124;
    }
    return 0;
}

int TestSynchronousDnsSuccessDoesNotCancelCompletedQuery() {
    TqSocketStartup startup;
    if (!startup.Ok()) {
        return 130;
    }

    sockaddr_storage address{};
    if (!MakeLoopbackAddress(address, 443)) {
        return 131;
    }

    int cancelCount = 0;
    TqServerDialReactor::TestHooks hooks;
    hooks.Resolve = [&](const std::string&, uint16_t, TqDnsResolveCallback callback) {
        TqDnsResolveResult result;
        result.Completed = true;
        result.Success = true;
        result.Addresses.push_back(address);
        callback(result);
        return 77;
    };
    hooks.CancelResolve = [&](uint64_t) {
        ++cancelCount;
    };
    hooks.RunDnsOnce = [](int) {
        return false;
    };
    hooks.Connect = [](TqSocketHandle, const sockaddr*, socklen_t) {
        return -1;
    };
    hooks.GetLastSocketError = [](TqSocketHandle) {
        return PlatformInProgressError();
    };

    TqServerDialReactor reactor(AllowAllAcl(), hooks);
    if (!reactor.Start()) {
        return 132;
    }

    bool completed = false;
    TqServerDialRequest request;
    request.Host = "sync-success.test";
    request.Port = 443;
    request.Complete = [&](const TqServerDialResult&) {
        completed = true;
    };

    const uint64_t token = reactor.Submit(std::move(request));
    if (token == 0) {
        reactor.Stop();
        return 133;
    }

    reactor.Stop();
    if (completed) {
        return 134;
    }
    if (cancelCount != 0) {
        return 135;
    }
    return 0;
}

int TestCompletionCanReenterCancel() {
    TqSocketStartup startup;
    if (!startup.Ok()) {
        return 140;
    }

    TqServerDialReactor::TestHooks hooks;
    hooks.Connect = [](TqSocketHandle, const sockaddr*, socklen_t) {
        return 0;
    };
    hooks.SetBlocking = [](TqSocketHandle) {
        return true;
    };
    auto reactor = std::make_unique<TqServerDialReactor>(AllowAllAcl(), hooks);
    if (!reactor->Start()) {
        return 141;
    }

    bool completed = false;
    TqServerDialReactor* reactorPtr = reactor.get();
    uint64_t token = 0;
    TqServerDialRequest request;
    request.Host = "127.0.0.1";
    request.Port = 443;
    request.Complete = [&](const TqServerDialResult& result) {
        completed = true;
        reactorPtr->Cancel(token);
        if (TqSocketValid(result.Fd)) {
            TqCloseSocket(result.Fd);
        }
    };

    std::future<uint64_t> submit = std::async(std::launch::async, [&]() {
        return reactorPtr->Submit(std::move(request));
    });
    if (submit.wait_for(std::chrono::milliseconds(1000)) != std::future_status::ready) {
        return 142;
    }
    token = submit.get();
    if (token == 0) {
        reactor->Stop();
        return 143;
    }
    if (!completed) {
        reactor->Stop();
        return 144;
    }
    reactor->Stop();
    return 0;
}

int TestCompletionCanReenterCancelFromAsyncPath() {
    TqSocketStartup startup;
    if (!startup.Ok()) {
        return 150;
    }

    sockaddr_storage address{};
    if (!MakeLoopbackAddress(address, 443)) {
        return 151;
    }

    FakeDns fakeDns;
    fakeDns.Result.Completed = true;
    fakeDns.Result.Success = true;
    fakeDns.Result.Addresses.push_back(address);

    TqServerDialReactor::TestHooks hooks;
    hooks.Resolve = [&](const std::string&, uint16_t, TqDnsResolveCallback callback) {
        return fakeDns.Resolve("", 0, std::move(callback));
    };
    hooks.CancelResolve = [&](uint64_t id) {
        fakeDns.Cancel(id);
    };
    hooks.RunDnsOnce = [&](int timeoutMs) {
        return fakeDns.RunOnce(timeoutMs);
    };
    hooks.Connect = [](TqSocketHandle, const sockaddr*, socklen_t) {
        return 0;
    };
    hooks.SetBlocking = [](TqSocketHandle) {
        return true;
    };

    TqServerDialReactor reactor(AllowAllAcl(), hooks);
    if (!reactor.Start()) {
        return 152;
    }

    bool completed = false;
    uint64_t token = 0;
    TqServerDialRequest request;
    request.Host = "async-reenter.test";
    request.Port = 443;
    request.Complete = [&](const TqServerDialResult& result) {
        completed = true;
        reactor.Cancel(token);
        if (TqSocketValid(result.Fd)) {
            TqCloseSocket(result.Fd);
        }
    };

    token = reactor.Submit(std::move(request));
    if (token == 0) {
        reactor.Stop();
        return 153;
    }
    if (!RunUntil(completed, reactor, 1000)) {
        reactor.Stop();
        return 154;
    }
    reactor.Stop();
    return 0;
}

int TestCompletionCallbackDoesNotHoldReactorLock() {
    TqSocketStartup startup;
    if (!startup.Ok()) {
        return 160;
    }

    TqServerDialReactor::TestHooks hooks;
    hooks.Connect = [](TqSocketHandle, const sockaddr*, socklen_t) {
        return 0;
    };
    hooks.SetBlocking = [](TqSocketHandle) {
        return true;
    };
    auto reactor = std::make_unique<TqServerDialReactor>(AllowAllAcl(), hooks);
    if (!reactor->Start()) {
        return 161;
    }

    std::promise<bool> entered;
    std::future<bool> enteredFuture = entered.get_future();
    bool signaled = false;
    uint64_t token = 0;
    TqServerDialRequest request;
    request.Host = "127.0.0.1";
    request.Port = 443;
    request.Complete = [&](const TqServerDialResult& result) {
        signaled = true;
        entered.set_value(true);
        if (TqSocketValid(result.Fd)) {
            TqCloseSocket(result.Fd);
        }
    };

    token = reactor->Submit(std::move(request));
    if (token == 0) {
        reactor->Stop();
        return 162;
    }
    reactor->Cancel(token);
    if (enteredFuture.wait_for(std::chrono::milliseconds(1000)) != std::future_status::ready) {
        reactor->Stop();
        return 163;
    }
    if (!enteredFuture.get() || !signaled) {
        reactor->Stop();
        return 164;
    }
    reactor->Stop();
    return 0;
}

} // namespace

int main() {
    int result = TestLiteralConnectSucceeds();
    if (result != 0) {
        return result;
    }
    result = TestDnsCallbackConnectSuccess();
    if (result != 0) {
        return result;
    }
    result = TestAclDenyReturnsAclDenied();
    if (result != 0) {
        return result;
    }
    result = TestDnsFailureReturnsDnsFailed();
    if (result != 0) {
        return result;
    }
    result = TestConnectRefusedMapsError();
    if (result != 0) {
        return result;
    }
    result = TestUnknownConnectFailureReturnsInternal();
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
    result = TestCancelSuppressesPendingDnsCompletion();
    if (result != 0) {
        return result;
    }
    result = TestStopSuppressesPendingCompletion();
    if (result != 0) {
        return result;
    }
    result = TestCancelClosesPendingConnectFdAndSuppressesCompletion();
    if (result != 0) {
        return result;
    }
    result = TestStopClosesPendingConnectFdAndSuppressesCompletion();
    if (result != 0) {
        return result;
    }
    result = TestDestructorClosesPendingConnectFd();
    if (result != 0) {
        return result;
    }
    result = TestSynchronousDnsSuccessDoesNotCancelCompletedQuery();
    if (result != 0) {
        return result;
    }
    result = TestCompletionCanReenterCancel();
    if (result != 0) {
        return result;
    }
    result = TestCompletionCanReenterCancelFromAsyncPath();
    if (result != 0) {
        return result;
    }
    result = TestCompletionCallbackDoesNotHoldReactorLock();
    if (result != 0) {
        return result;
    }
    return 0;
}
