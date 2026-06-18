#include "client_ingress_reactor.h"

#include <arpa/inet.h>
#include <cerrno>
#include <fcntl.h>
#include <future>
#include <limits>
#include <memory>
#include <netdb.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

namespace {

constexpr int TqMaxIngressAcceptsPerEvent = 64;

struct TqParsedHostPort {
    std::string Host;
    uint16_t Port{};
};

struct TqListenSocket {
    int Fd{-1};
    std::string Address;
};

class TqScopedFd {
public:
    explicit TqScopedFd(int fd = -1) : Fd(fd) {}
    ~TqScopedFd() { Reset(); }

    TqScopedFd(const TqScopedFd&) = delete;
    TqScopedFd& operator=(const TqScopedFd&) = delete;

    int Get() const { return Fd; }

    int Release() {
        const int fd = Fd;
        Fd = -1;
        return fd;
    }

    void Reset(int fd = -1) {
        if (Fd >= 0) {
            (void)::close(Fd);
        }
        Fd = fd;
    }

private:
    int Fd{-1};
};

void TqCloseFd(int& fd) {
    if (fd >= 0) {
        (void)::close(fd);
        fd = -1;
    }
}

bool TqParsePortAllowZero(const std::string& text, uint16_t& port) {
    if (text.empty()) {
        return false;
    }

    unsigned long value = 0;
    for (char ch : text) {
        if (ch < '0' || ch > '9') {
            return false;
        }
        value = (value * 10) + static_cast<unsigned long>(ch - '0');
        if (value > std::numeric_limits<uint16_t>::max()) {
            return false;
        }
    }

    port = static_cast<uint16_t>(value);
    return true;
}

bool TqParseHostPortAllowZero(const std::string& target, TqParsedHostPort& out) {
    if (target.empty() || target.find("://") != std::string::npos) {
        return false;
    }

    std::string host;
    std::string portText;
    if (target.front() == '[') {
        const size_t close = target.find(']');
        if (close == std::string::npos || close + 2 > target.size() || target[close + 1] != ':') {
            return false;
        }
        host = target.substr(1, close - 1);
        portText = target.substr(close + 2);
    } else {
        const size_t colon = target.rfind(':');
        if (colon == std::string::npos || colon + 1 >= target.size()) {
            return false;
        }
        host = target.substr(0, colon);
        portText = target.substr(colon + 1);
        if (host.find(':') != std::string::npos) {
            return false;
        }
    }

    uint16_t port = 0;
    if (!TqParsePortAllowZero(portText, port)) {
        return false;
    }

    out.Host = std::move(host);
    out.Port = port;
    return true;
}

bool TqSetNonBlockingCloseOnExec(int fd) {
    const int flags = ::fcntl(fd, F_GETFL, 0);
    if (flags < 0 || ::fcntl(fd, F_SETFL, flags | O_NONBLOCK) != 0) {
        return false;
    }

    const int fdFlags = ::fcntl(fd, F_GETFD, 0);
    if (fdFlags < 0 || ::fcntl(fd, F_SETFD, fdFlags | FD_CLOEXEC) != 0) {
        return false;
    }
    return true;
}

bool TqSetReuseAddress(int fd) {
    int value = 1;
    return ::setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &value, sizeof(value)) == 0;
}

std::string TqBoundAddressString(int fd, const std::string& requestedHost) {
    sockaddr_storage storage{};
    socklen_t length = sizeof(storage);
    if (::getsockname(fd, reinterpret_cast<sockaddr*>(&storage), &length) != 0) {
        return {};
    }

    char host[INET6_ADDRSTRLEN]{};
    uint16_t port = 0;
    if (storage.ss_family == AF_INET) {
        const auto* addr = reinterpret_cast<const sockaddr_in*>(&storage);
        if (::inet_ntop(AF_INET, &addr->sin_addr, host, sizeof(host)) == nullptr) {
            return {};
        }
        port = ntohs(addr->sin_port);
        return std::string(host) + ":" + std::to_string(port);
    }

    if (storage.ss_family == AF_INET6) {
        const auto* addr = reinterpret_cast<const sockaddr_in6*>(&storage);
        if (::inet_ntop(AF_INET6, &addr->sin6_addr, host, sizeof(host)) == nullptr) {
            return {};
        }
        port = ntohs(addr->sin6_port);
        return "[" + std::string(host) + "]:" + std::to_string(port);
    }

    if (!requestedHost.empty()) {
        return requestedHost + ":0";
    }
    return {};
}

bool TqCreateNonBlockingListenSocket(const std::string& listen, TqListenSocket& out) {
    TqParsedHostPort hostPort{};
    if (!TqParseHostPortAllowZero(listen, hostPort)) {
        return false;
    }

    addrinfo hints{};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_flags = AI_PASSIVE;

    addrinfo* result = nullptr;
    const std::string port = std::to_string(hostPort.Port);
    const int status = ::getaddrinfo(
        hostPort.Host.empty() ? nullptr : hostPort.Host.c_str(),
        port.c_str(),
        &hints,
        &result);
    if (status != 0) {
        return false;
    }

    for (addrinfo* ai = result; ai != nullptr; ai = ai->ai_next) {
        int rawFd = ::socket(ai->ai_family, ai->ai_socktype | SOCK_NONBLOCK | SOCK_CLOEXEC, ai->ai_protocol);
        TqScopedFd fd(rawFd);
        if (fd.Get() < 0 && (errno == EINVAL || errno == EPROTONOSUPPORT)) {
            fd.Reset(::socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol));
            if (fd.Get() >= 0 && !TqSetNonBlockingCloseOnExec(fd.Get())) {
                fd.Reset();
                continue;
            }
        }
        if (fd.Get() < 0) {
            continue;
        }

        (void)TqSetReuseAddress(fd.Get());
        if (::bind(fd.Get(), ai->ai_addr, ai->ai_addrlen) == 0 && ::listen(fd.Get(), SOMAXCONN) == 0) {
            std::string address = TqBoundAddressString(fd.Get(), hostPort.Host);
            if (address.empty()) {
                continue;
            }
            out.Fd = fd.Release();
            out.Address = std::move(address);
            ::freeaddrinfo(result);
            return true;
        }
    }

    ::freeaddrinfo(result);
    return false;
}

} // namespace

TqClientIngressReactor::~TqClientIngressReactor() {
    Stop();
}

bool TqClientIngressReactor::Start() {
    std::lock_guard<std::mutex> lifecycleLock(LifecycleMutex);
    if (State == LifecycleState::Running) {
        return true;
    }
    if (State == LifecycleState::Stopping) {
        return false;
    }
    if (Worker.joinable()) {
        return false;
    }
    if (!Reactor.Start()) {
        return false;
    }

    State = LifecycleState::Running;
    Running.store(true, std::memory_order_release);
    Worker = std::thread(&TqClientIngressReactor::Run, this);
    return true;
}

void TqClientIngressReactor::Stop() {
    {
        std::unique_lock<std::mutex> lifecycleLock(LifecycleMutex);
        if (State == LifecycleState::Stopped) {
            return;
        }
        if (State == LifecycleState::Stopping) {
            LifecycleCv.wait(lifecycleLock, [this]() {
                return State == LifecycleState::Stopped;
            });
            return;
        }
        State = LifecycleState::Stopping;
        Running.store(false, std::memory_order_release);
        (void)Reactor.Wake();
    }

    if (Worker.joinable()) {
        Worker.join();
    }

    {
        std::lock_guard<std::mutex> lifecycleLock(LifecycleMutex);
        {
            std::lock_guard<std::mutex> lock(Mutex);
            CloseAllLocked();
            PendingTasks.clear();
        }
        Reactor.Stop();
        State = LifecycleState::Stopped;
    }
    LifecycleCv.notify_all();
}

bool TqClientIngressReactor::AddPeer(const TqClientIngressPeer& peer) {
    {
        std::lock_guard<std::mutex> lock(Mutex);
        if (!Running.load(std::memory_order_acquire) || peer.PeerId.empty() || peer.SocksListen.empty() ||
            Peers.find(peer.PeerId) != Peers.end()) {
            return false;
        }
    }

    auto socks = std::make_shared<TqListenSocket>();
    if (!TqCreateNonBlockingListenSocket(peer.SocksListen, *socks)) {
        return false;
    }

    auto http = std::make_shared<TqListenSocket>();
    if (!peer.HttpListen.empty() && !TqCreateNonBlockingListenSocket(peer.HttpListen, *http)) {
        TqCloseFd(socks->Fd);
        return false;
    }

    const bool added = EnqueueSync([this, peer, socks, http]() {
        std::lock_guard<std::mutex> lock(Mutex);
        auto cleanup = [&]() {
            if (socks->Fd >= 0) {
                (void)Reactor.Remove(socks->Fd);
                Listens.erase(socks->Fd);
                TqCloseFd(socks->Fd);
            }
            if (http->Fd >= 0) {
                (void)Reactor.Remove(http->Fd);
                Listens.erase(http->Fd);
                TqCloseFd(http->Fd);
            }
        };

        if (!Running.load(std::memory_order_acquire) || Peers.find(peer.PeerId) != Peers.end()) {
            cleanup();
            return false;
        }

        Listens.emplace(socks->Fd, ListenEntry{peer.PeerId, ListenProto::Socks5});
        if (!Reactor.Add(socks->Fd, TqLinuxReactorEvents::Read,
                [this](int fd, uint32_t events) {
                    if ((events & (TqLinuxReactorEvents::Read | TqLinuxReactorEvents::Error)) != 0) {
                        AcceptLoop(fd);
                    }
                })) {
            cleanup();
            return false;
        }

        if (http->Fd >= 0) {
            Listens.emplace(http->Fd, ListenEntry{peer.PeerId, ListenProto::HttpConnect});
            if (!Reactor.Add(http->Fd, TqLinuxReactorEvents::Read,
                    [this](int fd, uint32_t events) {
                        if ((events & (TqLinuxReactorEvents::Read | TqLinuxReactorEvents::Error)) != 0) {
                            AcceptLoop(fd);
                        }
                    })) {
                cleanup();
                return false;
            }
        }

        PeerEntry entry{};
        entry.Peer = peer;
        entry.SocksFd = socks->Fd;
        entry.HttpFd = http->Fd;
        entry.SocksAddress = socks->Address;
        entry.HttpAddress = http->Address;
        socks->Fd = -1;
        http->Fd = -1;
        Peers.emplace(peer.PeerId, std::move(entry));
        return true;
    });
    if (!added) {
        TqCloseFd(socks->Fd);
        TqCloseFd(http->Fd);
    }
    return added;
}

bool TqClientIngressReactor::RemovePeer(const std::string& peerId) {
    return EnqueueSync([this, peerId]() {
        std::lock_guard<std::mutex> lock(Mutex);
        if (Peers.find(peerId) == Peers.end()) {
            return false;
        }
        RemovePeerLocked(peerId);
        return true;
    });
}

size_t TqClientIngressReactor::PeerCountForTest() const {
    std::lock_guard<std::mutex> lock(Mutex);
    return Peers.size();
}

#if defined(TQ_UNIT_TESTING)
std::string TqClientIngressReactor::SocksListenAddressForTest(const std::string& peerId) const {
    std::lock_guard<std::mutex> lock(Mutex);
    const auto it = Peers.find(peerId);
    if (it == Peers.end()) {
        return {};
    }
    return it->second.SocksAddress;
}

std::string TqClientIngressReactor::HttpListenAddressForTest(const std::string& peerId) const {
    std::lock_guard<std::mutex> lock(Mutex);
    const auto it = Peers.find(peerId);
    if (it == Peers.end()) {
        return {};
    }
    return it->second.HttpAddress;
}
#endif

void TqClientIngressReactor::Run() {
    for (;;) {
        ProcessPendingTasks();
        if (!Running.load(std::memory_order_acquire)) {
            break;
        }
        (void)Reactor.RunOnce(50);
    }
}

bool TqClientIngressReactor::EnqueueSync(std::function<bool()> task) {
    auto promise = std::make_shared<std::promise<bool>>();
    std::future<bool> future = promise->get_future();
    {
        std::lock_guard<std::mutex> lifecycleLock(LifecycleMutex);
        if (State != LifecycleState::Running || !Running.load(std::memory_order_acquire)) {
            return false;
        }
        {
            std::lock_guard<std::mutex> lock(Mutex);
            PendingTasks.emplace_back([task = std::move(task), promise]() mutable {
                promise->set_value(task());
            });
        }
        (void)Reactor.Wake();
    }
    return future.get();
}

void TqClientIngressReactor::ProcessPendingTasks() {
    std::deque<std::function<void()>> tasks;
    {
        std::lock_guard<std::mutex> lock(Mutex);
        tasks.swap(PendingTasks);
    }

    for (auto& task : tasks) {
        task();
    }
}

void TqClientIngressReactor::AcceptLoop(int listenFd) {
    std::string peerId;
    ListenProto proto = ListenProto::Socks5;
    {
        std::lock_guard<std::mutex> lock(Mutex);
        const auto listenIt = Listens.find(listenFd);
        if (listenIt == Listens.end()) {
            return;
        }
        peerId = listenIt->second.PeerId;
        proto = listenIt->second.Proto;
    }

    for (int accepted = 0; accepted < TqMaxIngressAcceptsPerEvent; ++accepted) {
        int clientFd = ::accept4(listenFd, nullptr, nullptr, SOCK_NONBLOCK | SOCK_CLOEXEC);
        if (clientFd < 0 && errno == ENOSYS) {
            clientFd = ::accept(listenFd, nullptr, nullptr);
            if (clientFd >= 0 && !TqSetNonBlockingCloseOnExec(clientFd)) {
                TqCloseFd(clientFd);
                continue;
            }
        }

        if (clientFd >= 0) {
            std::lock_guard<std::mutex> lock(Mutex);
            Clients.emplace(clientFd, ClientEntry{peerId, proto});
            continue;
        }

        if (errno == EINTR) {
            continue;
        }
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            return;
        }
        return;
    }
}

void TqClientIngressReactor::RemovePeerLocked(const std::string& peerId) {
    auto it = Peers.find(peerId);
    if (it == Peers.end()) {
        return;
    }

    if (it->second.SocksFd >= 0) {
        (void)Reactor.Remove(it->second.SocksFd);
        Listens.erase(it->second.SocksFd);
        TqCloseFd(it->second.SocksFd);
    }
    if (it->second.HttpFd >= 0) {
        (void)Reactor.Remove(it->second.HttpFd);
        Listens.erase(it->second.HttpFd);
        TqCloseFd(it->second.HttpFd);
    }

    for (auto clientIt = Clients.begin(); clientIt != Clients.end();) {
        if (clientIt->second.PeerId == peerId) {
            int fd = clientIt->first;
            TqCloseFd(fd);
            clientIt = Clients.erase(clientIt);
        } else {
            ++clientIt;
        }
    }

    Peers.erase(it);
}

void TqClientIngressReactor::CloseAllLocked() {
    for (auto& item : Peers) {
        if (item.second.SocksFd >= 0) {
            (void)Reactor.Remove(item.second.SocksFd);
            TqCloseFd(item.second.SocksFd);
        }
        if (item.second.HttpFd >= 0) {
            (void)Reactor.Remove(item.second.HttpFd);
            TqCloseFd(item.second.HttpFd);
        }
    }

    for (auto& item : Clients) {
        int fd = item.first;
        TqCloseFd(fd);
    }

    Clients.clear();
    Listens.clear();
    Peers.clear();
}
