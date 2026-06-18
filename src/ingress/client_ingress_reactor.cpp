#include "client_ingress_reactor.h"

#include "http_connect_server.h"
#include "socks5_server.h"

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
#include <vector>

namespace {

constexpr int TqMaxIngressAcceptsPerEvent = 64;
// Avoid reading past the SOCKS/HTTP handshake boundary; payload must remain queued for relay.
constexpr size_t TqIngressReadBufferSize = 1;

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

TqClientIngressProto TqToIngressProto(bool socks) {
    return socks
        ? TqClientIngressProto::Socks5
        : TqClientIngressProto::HttpConnect;
}

std::string TqBuildSocks5Response(TqOpenError error) {
    const uint8_t response[] = {
        0x05,
        TqSocks5RepForOpenError(error),
        0x00,
        0x01,
        0x00, 0x00, 0x00, 0x00,
        0x00, 0x00};
    return std::string(reinterpret_cast<const char*>(response), sizeof(response));
}

std::string TqBuildHttpConnectResponse(TqOpenError error) {
    const int status = TqHttpStatusForOpenError(error);
    const char* reason = "Internal Server Error";
    switch (status) {
    case 200:
        reason = "Connection Established";
        break;
    case 400:
        reason = "Bad Request";
        break;
    case 403:
        reason = "Forbidden";
        break;
    case 502:
        reason = "Bad Gateway";
        break;
    case 504:
        reason = "Gateway Timeout";
        break;
    default:
        break;
    }
    return "HTTP/1.1 " + std::to_string(status) + " " + reason + "\r\n\r\n";
}

std::string TqBuildOpenResponse(bool socks, TqOpenError error) {
    if (socks) {
        return TqBuildSocks5Response(error);
    }
    return TqBuildHttpConnectResponse(error);
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
        if (!Reactor.Add(socks->Fd, TqReactorEvents::Read,
                [this](int fd, uint32_t events) {
                    if ((events & (TqReactorEvents::Read | TqReactorEvents::Error)) != 0) {
                        AcceptLoop(fd);
                    }
                })) {
            cleanup();
            return false;
        }

        if (http->Fd >= 0) {
            Listens.emplace(http->Fd, ListenEntry{peer.PeerId, ListenProto::HttpConnect});
            if (!Reactor.Add(http->Fd, TqReactorEvents::Read,
                    [this](int fd, uint32_t events) {
                        if ((events & (TqReactorEvents::Read | TqReactorEvents::Error)) != 0) {
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

bool TqClientIngressReactor::EnqueueAsync(std::function<void()> task) {
    std::lock_guard<std::mutex> lifecycleLock(LifecycleMutex);
    if (State != LifecycleState::Running || !Running.load(std::memory_order_acquire)) {
        return false;
    }
    {
        std::lock_guard<std::mutex> lock(Mutex);
        PendingTasks.emplace_back(std::move(task));
    }
    (void)Reactor.Wake();
    return true;
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
            const auto peerIt = Peers.find(peerId);
            if (peerIt == Peers.end()) {
                TqCloseFd(clientFd);
                continue;
            }

            ClientEntry client{};
            client.PeerId = peerId;
            client.Proto = proto;
            client.State = TqClientIngressState(TqToIngressProto(proto == ListenProto::Socks5));
            client.StartTunnel = peerIt->second.Peer.StartTunnel;
            client.AcceptTunnel = peerIt->second.Peer.AcceptTunnel;
            client.RejectTunnel = peerIt->second.Peer.RejectTunnel;
            client.CancelTunnel = peerIt->second.Peer.CancelTunnel;
            if (!Reactor.Add(clientFd, TqReactorEvents::Read,
                    [this](int fd, uint32_t events) {
                        HandleClientEvents(fd, events);
                    })) {
                TqCloseFd(clientFd);
                continue;
            }
            Clients.emplace(clientFd, std::move(client));
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

void TqClientIngressReactor::HandleClientEvents(int clientFd, uint32_t events) {
    if ((events & TqReactorEvents::Read) != 0) {
        HandleClientRead(clientFd);
    }
    if ((events & TqReactorEvents::Write) != 0) {
        HandleClientWrite(clientFd);
    }
    if ((events & TqReactorEvents::Error) != 0) {
        std::lock_guard<std::mutex> lock(Mutex);
        if (Clients.find(clientFd) != Clients.end()) {
            CloseClientLocked(clientFd, true);
        }
    }
}

void TqClientIngressReactor::HandleClientRead(int clientFd) {
    for (;;) {
        uint8_t buffer[TqIngressReadBufferSize]{};
        const ssize_t received = ::recv(clientFd, buffer, sizeof(buffer), 0);
        if (received > 0) {
            TqClientIngressResult result = TqClientIngressResult::Close;
            {
                std::lock_guard<std::mutex> lock(Mutex);
                auto it = Clients.find(clientFd);
                if (it == Clients.end() || it->second.Phase != ClientPhase::Handshake) {
                    return;
                }
                result = it->second.State.Feed(buffer, static_cast<size_t>(received));
            }
            HandleIngressResult(clientFd, result);
            if (result != TqClientIngressResult::NeedRead) {
                return;
            }
            continue;
        }

        if (received == 0) {
            std::lock_guard<std::mutex> lock(Mutex);
            CloseClientLocked(clientFd, true);
            return;
        }

        if (errno == EINTR) {
            continue;
        }
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            return;
        }

        std::lock_guard<std::mutex> lock(Mutex);
        CloseClientLocked(clientFd, true);
        return;
    }
}

void TqClientIngressReactor::HandleClientWrite(int clientFd) {
    for (;;) {
        TqClientIngressTunnelAcceptFn acceptTunnel;
        TqClientIngressTunnelCloseFn rejectTunnel;
        TqClientTunnelOpenHandle* completedHandle = nullptr;
        bool completedOpenSucceeded = false;
        bool removeOwnedByTunnel = false;
        bool closeOwnedByReactor = false;
        bool feedAfterHandshakeWrite = false;

        {
            std::lock_guard<std::mutex> lock(Mutex);
            auto it = Clients.find(clientFd);
            if (it == Clients.end()) {
                return;
            }
            ClientEntry& client = it->second;
            const bool handshakeWrite = client.Phase == ClientPhase::Handshake;
            const std::string& pending = handshakeWrite ? client.State.PendingWrite() : client.PendingWrite;
            if (pending.empty()) {
                if (handshakeWrite) {
                    feedAfterHandshakeWrite = true;
                } else {
                    completedHandle = client.OpenHandle;
                    completedOpenSucceeded = client.OpenSucceeded;
                    acceptTunnel = client.AcceptTunnel;
                    rejectTunnel = client.RejectTunnel;
                    removeOwnedByTunnel = completedHandle != nullptr;
                    closeOwnedByReactor = completedHandle == nullptr;
                }
            } else {
                const ssize_t sent = ::send(clientFd, pending.data(), pending.size(), MSG_NOSIGNAL);
                if (sent > 0) {
                    if (handshakeWrite) {
                        client.State.MarkWriteComplete(static_cast<size_t>(sent));
                        feedAfterHandshakeWrite = client.State.PendingWrite().empty();
                    } else {
                        client.PendingWrite.erase(0, static_cast<size_t>(sent));
                        if (client.PendingWrite.empty()) {
                            completedHandle = client.OpenHandle;
                            completedOpenSucceeded = client.OpenSucceeded;
                            acceptTunnel = client.AcceptTunnel;
                            rejectTunnel = client.RejectTunnel;
                            removeOwnedByTunnel = completedHandle != nullptr;
                            closeOwnedByReactor = completedHandle == nullptr;
                        }
                    }
                } else if (sent < 0 && errno == EINTR) {
                    continue;
                } else if (sent < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
                    return;
                } else {
                    CloseClientLocked(clientFd, true);
                    return;
                }
            }
        }

        if (feedAfterHandshakeWrite) {
            TqClientIngressResult result = TqClientIngressResult::Close;
            {
                std::lock_guard<std::mutex> lock(Mutex);
                auto it = Clients.find(clientFd);
                if (it == Clients.end() || it->second.Phase != ClientPhase::Handshake) {
                    return;
                }
                result = it->second.State.Feed(nullptr, 0);
            }
            HandleIngressResult(clientFd, result);
            return;
        }

        if (completedHandle != nullptr) {
            if (completedOpenSucceeded) {
                bool accepted = false;
                if (acceptTunnel) {
                    accepted = acceptTunnel(completedHandle);
                }
                if (!accepted && rejectTunnel) {
                    rejectTunnel(completedHandle);
                }
            } else if (rejectTunnel) {
                rejectTunnel(completedHandle);
            }
        }

        if (removeOwnedByTunnel) {
            std::lock_guard<std::mutex> lock(Mutex);
            CloseClientOwnedByTunnelLocked(clientFd);
            return;
        }
        if (closeOwnedByReactor) {
            std::lock_guard<std::mutex> lock(Mutex);
            CloseClientLocked(clientFd, true);
            return;
        }
    }
}

void TqClientIngressReactor::HandleIngressResult(int clientFd, TqClientIngressResult result) {
    if (result == TqClientIngressResult::NeedRead) {
        (void)Reactor.Modify(clientFd, TqReactorEvents::Read);
        return;
    }
    if (result == TqClientIngressResult::NeedWrite) {
        (void)Reactor.Modify(clientFd, TqReactorEvents::Write | TqReactorEvents::Error);
        return;
    }
    if (result == TqClientIngressResult::ReadyToOpen) {
        StartClientOpen(clientFd);
        return;
    }

    std::lock_guard<std::mutex> lock(Mutex);
    CloseClientLocked(clientFd, true);
}

void TqClientIngressReactor::StartClientOpen(int clientFd) {
    TunnelRequest request{};
    TqClientIngressTunnelStartFn startTunnel;
    TqClientIngressTunnelCloseFn rejectTunnel;
    bool socks = true;
    {
        std::lock_guard<std::mutex> lock(Mutex);
        auto it = Clients.find(clientFd);
        if (it == Clients.end()) {
            return;
        }
        ClientEntry& client = it->second;
        if (client.Phase != ClientPhase::Handshake) {
            return;
        }
        request = client.State.Request();
        startTunnel = client.StartTunnel;
        rejectTunnel = client.RejectTunnel;
        socks = client.Proto == ListenProto::Socks5;
        client.Phase = ClientPhase::Opening;
        (void)Reactor.Modify(clientFd, TqReactorEvents::Error);
    }

    if (!startTunnel) {
        CompleteClientOpen(clientFd, nullptr, TqTunnelStartResult{false, TqOpenError::Internal, 0});
        return;
    }

    TqClientTunnelOpenHandle* handle = startTunnel(
        request,
        clientFd,
        [this, clientFd](TqClientTunnelOpenHandle* completedHandle, TqTunnelStartResult result) {
            (void)EnqueueAsync([this, clientFd, completedHandle, result]() {
                CompleteClientOpen(clientFd, completedHandle, result);
            });
        });

    if (handle == nullptr) {
        CompleteClientOpen(clientFd, nullptr, TqTunnelStartResult{false, TqOpenError::Internal, 0});
        return;
    }

    std::lock_guard<std::mutex> lock(Mutex);
    auto it = Clients.find(clientFd);
    if (it == Clients.end()) {
        if (rejectTunnel) {
            rejectTunnel(handle);
        }
        return;
    }
    it->second.OpenHandle = handle;
    it->second.PendingWrite = TqBuildOpenResponse(socks, TqOpenError::Internal);
}

void TqClientIngressReactor::CompleteClientOpen(
    int clientFd,
    TqClientTunnelOpenHandle* handle,
    TqTunnelStartResult result) {
    std::lock_guard<std::mutex> lock(Mutex);
    auto it = Clients.find(clientFd);
    if (it == Clients.end()) {
        return;
    }
    ClientEntry& client = it->second;
    if (client.Phase != ClientPhase::Opening) {
        return;
    }
    if (client.OpenHandle != nullptr && handle != nullptr && client.OpenHandle != handle) {
        return;
    }
    if (client.OpenHandle == nullptr) {
        client.OpenHandle = handle;
    }

    const TqOpenError error = result.Ok ? TqOpenError::Ok : result.Error;
    client.OpenSucceeded = result.Ok;
    client.PendingWrite = TqBuildOpenResponse(client.Proto == ListenProto::Socks5, error);
    client.Phase = ClientPhase::WritingOpenResponse;
    (void)Reactor.Modify(clientFd, TqReactorEvents::Write | TqReactorEvents::Error);
}

void TqClientIngressReactor::CloseClientLocked(int clientFd, bool closeFd) {
    auto it = Clients.find(clientFd);
    if (it == Clients.end()) {
        return;
    }
    ClientEntry client = std::move(it->second);
    Clients.erase(it);
    (void)Reactor.Remove(clientFd);
    if (client.OpenHandle != nullptr) {
        if (client.CancelTunnel) {
            client.CancelTunnel(client.OpenHandle);
        }
        return;
    }
    if (closeFd) {
        int fd = clientFd;
        TqCloseFd(fd);
    }
}

void TqClientIngressReactor::CloseClientOwnedByTunnelLocked(int clientFd) {
    auto it = Clients.find(clientFd);
    if (it == Clients.end()) {
        return;
    }
    Clients.erase(it);
    (void)Reactor.Remove(clientFd);
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

    std::vector<int> clientFds;
    for (auto clientIt = Clients.begin(); clientIt != Clients.end(); ++clientIt) {
        if (clientIt->second.PeerId == peerId) {
            clientFds.push_back(clientIt->first);
        }
    }
    for (int fd : clientFds) {
        CloseClientLocked(fd, true);
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

    std::vector<int> clientFds;
    clientFds.reserve(Clients.size());
    for (const auto& item : Clients) {
        clientFds.push_back(item.first);
    }
    for (int fd : clientFds) {
        CloseClientLocked(fd, true);
    }

    Clients.clear();
    Listens.clear();
    Peers.clear();
}
