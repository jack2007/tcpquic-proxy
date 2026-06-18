#include "server_dial_reactor.h"

#include <chrono>
#include <cstdint>
#include <string>
#include <thread>

#include <cerrno>
#include <dirent.h>
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

int CountOpenFds() {
    DIR* dir = ::opendir("/proc/self/fd");
    if (dir == nullptr) {
        return -1;
    }

    int count = 0;
    for (;;) {
        errno = 0;
        dirent* entry = ::readdir(dir);
        if (entry == nullptr) {
            break;
        }
        if (std::string(entry->d_name) == "." || std::string(entry->d_name) == "..") {
            continue;
        }
        ++count;
    }
    const int readErrno = errno;
    (void)::closedir(dir);
    if (readErrno != 0) {
        return -1;
    }
    return count;
}

int TestCancelLiteralSuppressesCompletion() {
    TqSocketStartup startup;
    if (!startup.Ok()) {
        return 1;
    }

    TqServerDialReactor reactor;
    if (!reactor.Start()) {
        return 2;
    }

    bool completed = false;
    TqServerDialRequest request;
    request.Host = "127.0.0.1";
    request.Port = 9;
    request.Complete = [&](const TqServerDialResult&) {
        completed = true;
    };

    const uint64_t token = reactor.Submit(std::move(request));
    if (token == 0) {
        reactor.Stop();
        return 3;
    }
    reactor.Cancel(token);

    for (int i = 0; i < 10; ++i) {
        (void)reactor.RunOnce(10);
        if (completed) {
            reactor.Stop();
            return 4;
        }
    }

    reactor.Stop();
    return 0;
}

int TestLiteralConnectSucceeds() {
    TqSocketStartup startup;
    if (!startup.Ok()) {
        return 20;
    }

    ScopedSocket listener;
    uint16_t port = 0;
    if (!MakeLoopbackListener(listener, port)) {
        return 21;
    }

    TqServerDialReactor reactor;
    if (!reactor.Start()) {
        return 22;
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
        return 23;
    }
    if (!RunUntil(completed, reactor, 3000)) {
        reactor.Stop();
        return 24;
    }

    reactor.Stop();

    ScopedSocket connected(observed.Fd);
    if (!observed.Done || observed.Error != TqOpenError::Ok) {
        return 25;
    }
    if (!TqSocketValid(connected.Get())) {
        return 26;
    }
    if (!AcceptOne(listener.Get(), 1000)) {
        return 27;
    }
    return 0;
}

int TestAclDenyReturnsAclDenied() {
    TqSocketStartup startup;
    if (!startup.Ok()) {
        return 40;
    }

    TqAcl acl;
    acl.AllowCidrs = {"10.0.0.0/8"};
    TqServerDialReactor reactor(acl);
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

    if (!observed.Done || observed.Error != TqOpenError::AclDenied) {
        return 44;
    }
    if (TqSocketValid(observed.Fd)) {
        TqCloseSocket(observed.Fd);
        return 45;
    }
    return 0;
}

int TestDnsFailureReturnsDnsFailed() {
    TqSocketStartup startup;
    if (!startup.Ok()) {
        return 60;
    }

    FakeDns fakeDns;
    fakeDns.Result.Completed = true;
    fakeDns.Result.Success = false;
    fakeDns.Result.Status = -1;

    TqServerDialReactor::TestHooks hooks;
    hooks.Resolve = [&](const std::string&, uint16_t, TqDnsResolveCallback callback) {
        callback(fakeDns.Result);
        return fakeDns.NextId++;
    };
    hooks.CancelResolve = [&](uint64_t id) {
        fakeDns.Cancel(id);
    };
    hooks.RunDnsOnce = [&](int timeoutMs) {
        return fakeDns.RunOnce(timeoutMs);
    };

    TqServerDialReactor reactor(TqAcl{}, hooks);
    if (!reactor.Start()) {
        return 61;
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
        return 62;
    }
    if (!RunUntil(completed, reactor, 1000)) {
        reactor.Stop();
        return 63;
    }
    reactor.Stop();

    if (!observed.Done || observed.Error != TqOpenError::DnsFailed) {
        return 64;
    }
    if (TqSocketValid(observed.Fd)) {
        TqCloseSocket(observed.Fd);
        return 65;
    }
    return 0;
}

int TestUnknownConnectFailureReturnsInternal() {
    TqSocketStartup startup;
    if (!startup.Ok()) {
        return 70;
    }

    TqServerDialReactor::TestHooks hooks;
    bool connectCalled = false;
    hooks.CreateSocket = [](int family, int type, int protocol) {
        return ::socket(family, type, protocol);
    };
    hooks.SetNonBlocking = [](TqSocketHandle) {
        return true;
    };
    hooks.Connect = [&](TqSocketHandle, const sockaddr*, socklen_t) {
        connectCalled = true;
        return -1;
    };
    hooks.GetLastSocketError = [](TqSocketHandle) {
        return EACCES;
    };

    TqAcl acl;
    acl.AllowCidrs = {"0.0.0.0/0", "::/0"};
    TqServerDialReactor reactor(acl, hooks);
    if (!reactor.Start()) {
        return 71;
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
        return 72;
    }
    if (!RunUntil(completed, reactor, 1000)) {
        reactor.Stop();
        return 73;
    }
    reactor.Stop();

    if (!connectCalled) {
        return 74;
    }
    if (!observed.Done || observed.Error != TqOpenError::Internal) {
        return 75;
    }
    if (TqSocketValid(observed.Fd)) {
        TqCloseSocket(observed.Fd);
        return 76;
    }
    return 0;
}

int TestStopSuppressesPendingCompletion() {
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

    TqServerDialReactor reactor(TqAcl{}, hooks);
    if (!reactor.Start()) {
        return 81;
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
        return 82;
    }

    reactor.Stop();
    (void)fakeDns.RunOnce(0);
    if (completed) {
        return 83;
    }
    return 0;
}

int TestDestructorClosesPendingConnectFd() {
    TqSocketStartup startup;
    if (!startup.Ok()) {
        return 90;
    }

    const int before = CountOpenFds();
    if (before < 0) {
        return 91;
    }

    bool completed = false;
    {
        TqServerDialReactor::TestHooks hooks;
        hooks.Resolve = [](const std::string&, uint16_t, TqDnsResolveCallback) {
            return 1;
        };
        hooks.Connect = [](TqSocketHandle, const sockaddr*, socklen_t) {
            return -1;
        };
        hooks.GetLastSocketError = [](TqSocketHandle) {
            return EINPROGRESS;
        };

        TqAcl acl;
        acl.AllowCidrs = {"0.0.0.0/0", "::/0"};
        TqServerDialReactor reactor(acl, hooks);
        if (!reactor.Start()) {
            return 92;
        }

        TqServerDialRequest request;
        request.Host = "127.0.0.1";
        request.Port = 443;
        request.Complete = [&](const TqServerDialResult&) {
            completed = true;
        };

        const uint64_t token = reactor.Submit(std::move(request));
        if (token == 0) {
            return 93;
        }
    }

    const int after = CountOpenFds();
    if (after < 0) {
        return 94;
    }
    if (completed) {
        return 95;
    }
    if (after != before) {
        return 96;
    }
    return 0;
}

int TestSynchronousDnsSuccessDoesNotCancelCompletedQuery() {
    TqSocketStartup startup;
    if (!startup.Ok()) {
        return 100;
    }

    sockaddr_storage address{};
    auto* addr4 = reinterpret_cast<sockaddr_in*>(&address);
    address.ss_family = AF_INET;
    if (!TqInetPton(AF_INET, "127.0.0.1", &addr4->sin_addr)) {
        return 101;
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
        return EINPROGRESS;
    };

    TqAcl acl;
    acl.AllowCidrs = {"0.0.0.0/0", "::/0"};
    TqServerDialReactor reactor(acl, hooks);
    if (!reactor.Start()) {
        return 102;
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
        return 103;
    }

    reactor.Stop();
    if (completed) {
        return 104;
    }
    if (cancelCount != 0) {
        return 105;
    }
    return 0;
}

} // namespace

int main() {
    int result = TestCancelLiteralSuppressesCompletion();
    if (result != 0) {
        return result;
    }
    result = TestLiteralConnectSucceeds();
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
    result = TestUnknownConnectFailureReturnsInternal();
    if (result != 0) {
        return result;
    }
    result = TestStopSuppressesPendingCompletion();
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
    return 0;
}
