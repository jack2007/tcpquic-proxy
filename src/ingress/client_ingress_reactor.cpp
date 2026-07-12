#include "client_ingress_reactor.h"

#include "http_connect_server.h"
#include "listen_socket.h"
#include "scoped_socket.h"
#include "socks5_server.h"

#include <algorithm>
#include <cerrno>
#include <cstdarg>
#include <cstdio>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <future>
#include <memory>
#include <vector>

#if !defined(_WIN32)
#include <fcntl.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

namespace {

constexpr int TqMaxIngressAcceptsPerEvent = 64;
// Avoid reading past the SOCKS/HTTP handshake boundary; payload must remain queued for relay.
constexpr size_t TqIngressReadBufferSize = 1;

bool TqTunnelDebugEnabled() {
    static const bool enabled = []() {
#if defined(_WIN32)
        char* value = nullptr;
        size_t len = 0;
        if (_dupenv_s(&value, &len, "TQ_TUNNEL_DEBUG") != 0 || value == nullptr) {
            return false;
        }
        const bool on = len > 1 && std::strcmp(value, "0") != 0;
        std::free(value);
        return on;
#else
        const char* value = std::getenv("TQ_TUNNEL_DEBUG");
        return value != nullptr && value[0] != '\0' && std::strcmp(value, "0") != 0;
#endif
    }();
    return enabled;
}

void TqTunnelDebugLog(const char* fmt, ...) {
    if (!TqTunnelDebugEnabled()) {
        return;
    }
    char buffer[1024];
    va_list args;
    va_start(args, fmt);
#if defined(__clang__)
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wformat-nonliteral"
#endif
    std::vsnprintf(buffer, sizeof(buffer), fmt, args);
#if defined(__clang__)
#pragma clang diagnostic pop
#endif
    va_end(args);
    std::fprintf(stderr, "tcpquic-proxy tunnel-debug: %s\n", buffer);
}

const char* TqOpenErrorName(TqOpenError error) {
    switch (error) {
    case TqOpenError::Ok:
        return "ok";
    case TqOpenError::AclDenied:
        return "acl_denied";
    case TqOpenError::DnsFailed:
        return "dns_failed";
    case TqOpenError::TcpTimeout:
        return "tcp_timeout";
    case TqOpenError::TcpRefused:
        return "tcp_refused";
    case TqOpenError::Internal:
    default:
        return "internal";
    }
}

std::string TqRequestTarget(const TunnelRequest& request) {
    return std::string(request.Host) + ":" + std::to_string(request.Port);
}

void TqCloseFd(TqSocketHandle& fd) {
    if (TqSocketValid(fd)) {
        TqCloseSocket(fd);
        fd = TqInvalidSocket;
    }
}

bool TqIsSocketInterrupted() {
    return TqSocketInterrupted(TqLastSocketError());
}

bool TqIsSocketWouldBlock() {
    return TqSocketWouldBlock(TqLastSocketError());
}

TqSocketHandle TqAcceptClient(TqSocketHandle listenFd) {
#if defined(_WIN32)
    TqSocketHandle clientFd = ::accept(listenFd, nullptr, nullptr);
    if (TqSocketValid(clientFd) && !TqSetNonBlocking(clientFd)) {
        TqCloseFd(clientFd);
    }
    return clientFd;
#elif defined(__APPLE__)
    TqSocketHandle clientFd = ::accept(listenFd, nullptr, nullptr);
    if (TqSocketValid(clientFd)) {
        const int fdFlags = ::fcntl(clientFd, F_GETFD, 0);
        if (!TqSetNonBlocking(clientFd) || fdFlags < 0 ||
            ::fcntl(clientFd, F_SETFD, fdFlags | FD_CLOEXEC) != 0) {
            TqCloseFd(clientFd);
        }
    }
    return clientFd;
#else
    TqSocketHandle clientFd = ::accept4(listenFd, nullptr, nullptr, SOCK_NONBLOCK | SOCK_CLOEXEC);
    if (!TqSocketValid(clientFd) && errno == ENOSYS) {
        clientFd = ::accept(listenFd, nullptr, nullptr);
        if (TqSocketValid(clientFd)) {
            const int fdFlags = ::fcntl(clientFd, F_GETFD, 0);
            if (!TqSetNonBlocking(clientFd) || fdFlags < 0 ||
                ::fcntl(clientFd, F_SETFD, fdFlags | FD_CLOEXEC) != 0) {
                TqCloseFd(clientFd);
            }
        }
    }
    return clientFd;
#endif
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

uint8_t TqAddrTypeForForwardHost(const std::string& host) {
    in_addr ipv4{};
    if (TqInetPton(AF_INET, host.c_str(), &ipv4)) {
        return TQ_ADDR_IPV4;
    }

    in6_addr ipv6{};
    if (TqInetPton(AF_INET6, host.c_str(), &ipv6)) {
        return TQ_ADDR_IPV6;
    }

    return TQ_ADDR_DOMAIN;
}

TunnelRequest TqBuildPortForwardRequest(const TqPortForwardConfig& forward) {
    TunnelRequest request{};
    request.AddrType = TqAddrTypeForForwardHost(forward.TargetHost);
    std::snprintf(request.Host, sizeof(request.Host), "%s", forward.TargetHost.c_str());
    request.Port = forward.TargetPort;
    request.IngressTraceProto = 3;
    return request;
}

} // namespace

bool TqClientIngressReactor::MarkOpenCompletionTerminal(
    const std::shared_ptr<OpenCompletionState>& completionState) {
    if (!completionState) {
        return true;
    }
    std::lock_guard<std::mutex> completionLock(completionState->Mutex);
    if (completionState->TerminalCalled) {
        return false;
    }
    completionState->TerminalCalled = true;
    return true;
}

void TqClientIngressReactor::RejectOpenHandleOnce(
    TqClientTunnelOpenHandle* handle,
    const TqClientIngressTunnelCloseFn& rejectTunnel,
    const std::shared_ptr<OpenCompletionState>& completionState) {
    if (handle == nullptr || !rejectTunnel || !MarkOpenCompletionTerminal(completionState)) {
        return;
    }
    rejectTunnel(handle);
}

void TqClientIngressReactor::CancelOpenHandleOnce(
    TqClientTunnelOpenHandle* handle,
    const TqClientIngressTunnelCloseFn& cancelTunnel,
    const std::shared_ptr<OpenCompletionState>& completionState) {
    if (handle == nullptr || !cancelTunnel || !MarkOpenCompletionTerminal(completionState)) {
        return;
    }
    cancelTunnel(handle);
}

TqClientIngressReactor::TqClientIngressReactor() {
    CompletionTokenPtr->Reactor = this;
}

TqClientIngressReactor::~TqClientIngressReactor() {
    Stop();
}

const char* TqClientIngressReactor::ListenProtoName(ListenProto proto) {
    switch (proto) {
    case ListenProto::Socks5:
        return "socks5";
    case ListenProto::HttpConnect:
        return "http";
    case ListenProto::PortForward:
        return "port_forward";
    }
    return "unknown";
}

const char* TqClientIngressReactor::ClientPhaseName(ClientPhase phase) {
    switch (phase) {
    case ClientPhase::Handshake:
        return "handshake";
    case ClientPhase::Opening:
        return "opening";
    case ClientPhase::WritingOpenResponse:
        return "writing_open_response";
    }
    return "unknown";
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
    if (!SocketStartup.Ok()) {
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
    std::shared_ptr<CompletionToken> oldToken;
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
        oldToken = std::move(CompletionTokenPtr);
        {
            std::lock_guard<std::mutex> tokenLock(oldToken->Mutex);
            oldToken->Reactor = nullptr;
        }
        CompletionTokenPtr = std::make_shared<CompletionToken>();
        CompletionTokenPtr->Reactor = this;
        Running.store(false, std::memory_order_release);
        (void)Reactor.Wake();
    }

    if (Worker.joinable()) {
        Worker.join();
    }
    ProcessPendingTasks();
    {
        std::unique_lock<std::mutex> lifecycleLock(LifecycleMutex);
        LifecycleCv.wait(lifecycleLock, [this]() {
            return ActiveDelayedTasks == 0;
        });
    }

    {
        std::lock_guard<std::mutex> lifecycleLock(LifecycleMutex);
        {
            std::lock_guard<std::mutex> lock(Mutex);
            CloseAllLocked();
            PendingTasks.clear();
            DelayedTasks.clear();
        }
        Reactor.Stop();
        State = LifecycleState::Stopped;
    }
    if (oldToken) {
        std::unique_lock<std::mutex> tokenLock(oldToken->Mutex);
        oldToken->Cv.wait(tokenLock, [&]() {
            return oldToken->ActiveCallbacks == 0;
        });
    }
    LifecycleCv.notify_all();
}

bool TqClientIngressReactor::AddPeer(const TqClientIngressPeer& peer) {
    {
        std::lock_guard<std::mutex> lock(Mutex);
        if (!Running.load(std::memory_order_acquire) || peer.PeerId.empty() ||
            Peers.find(peer.PeerId) != Peers.end()) {
            return false;
        }
    }

    struct PendingListen {
        ListenProto Proto{ListenProto::Socks5};
        TqListenSocket Socket;
        TqPortForwardConfig Forward;
    };

    auto pending = std::make_shared<std::vector<PendingListen>>();
    if (!peer.SocksListen.empty()) {
        PendingListen listen;
        listen.Proto = ListenProto::Socks5;
        if (!TqCreateNonBlockingListenSocket(peer.SocksListen, listen.Socket)) {
            return false;
        }
        pending->push_back(std::move(listen));
    }
    if (!peer.HttpListen.empty()) {
        PendingListen listen;
        listen.Proto = ListenProto::HttpConnect;
        if (!TqCreateNonBlockingListenSocket(peer.HttpListen, listen.Socket)) {
            for (auto& pendingListen : *pending) {
                TqCloseFd(pendingListen.Socket.Fd);
            }
            return false;
        }
        pending->push_back(std::move(listen));
    }
    for (const auto& forward : peer.PortForwards) {
        PendingListen listen;
        listen.Proto = ListenProto::PortForward;
        listen.Forward = forward;
        if (!TqCreateNonBlockingListenSocket(forward.Listen, listen.Socket)) {
            for (auto& pendingListen : *pending) {
                TqCloseFd(pendingListen.Socket.Fd);
            }
            return false;
        }
        pending->push_back(std::move(listen));
    }
    if (pending->empty()) {
        return false;
    }

    const bool added = EnqueueSync([this, peer, pending]() {
        std::lock_guard<std::mutex> lock(Mutex);
        auto cleanup = [&]() {
            for (auto& pendingListen : *pending) {
                if (TqSocketValid(pendingListen.Socket.Fd)) {
                    (void)Reactor.Remove(pendingListen.Socket.Fd);
                    Listens.erase(pendingListen.Socket.Fd);
                    TqCloseFd(pendingListen.Socket.Fd);
                }
            }
        };

        if (!Running.load(std::memory_order_acquire) || Peers.find(peer.PeerId) != Peers.end()) {
            cleanup();
            return false;
        }

        for (const auto& pendingListen : *pending) {
            Listens.emplace(pendingListen.Socket.Fd,
                ListenEntry{peer.PeerId, pendingListen.Proto, pendingListen.Forward});
            if (!Reactor.Add(pendingListen.Socket.Fd, TqReactorEvents::Read,
                [this](TqSocketHandle fd, uint32_t events) {
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
        entry.Listeners.reserve(pending->size());
        for (auto& pendingListen : *pending) {
            PeerListen listen;
            listen.Proto = pendingListen.Proto;
            listen.Fd = pendingListen.Socket.Fd;
            listen.Address = pendingListen.Socket.Address;
            listen.Forward = std::move(pendingListen.Forward);
            pendingListen.Socket.Fd = TqInvalidSocket;
            entry.Listeners.push_back(std::move(listen));
        }
        Peers.emplace(peer.PeerId, std::move(entry));
        return true;
    });
    if (!added) {
        for (auto& pendingListen : *pending) {
            TqCloseFd(pendingListen.Socket.Fd);
        }
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
    for (const auto& listen : it->second.Listeners) {
        if (listen.Proto == ListenProto::Socks5) {
            return listen.Address;
        }
    }
    return {};
}

std::string TqClientIngressReactor::HttpListenAddressForTest(const std::string& peerId) const {
    std::lock_guard<std::mutex> lock(Mutex);
    const auto it = Peers.find(peerId);
    if (it == Peers.end()) {
        return {};
    }
    for (const auto& listen : it->second.Listeners) {
        if (listen.Proto == ListenProto::HttpConnect) {
            return listen.Address;
        }
    }
    return {};
}

std::string TqClientIngressReactor::PortForwardListenAddressForTest(
    const std::string& peerId,
    size_t index) const {
    std::lock_guard<std::mutex> lock(Mutex);
    const auto it = Peers.find(peerId);
    if (it == Peers.end()) {
        return {};
    }
    size_t forwardIndex = 0;
    for (const auto& listen : it->second.Listeners) {
        if (listen.Proto != ListenProto::PortForward) {
            continue;
        }
        if (forwardIndex == index) {
            return listen.Address;
        }
        ++forwardIndex;
    }
    return {};
}

void TqClientIngressReactor::SetOpenTimeoutForTest(std::chrono::milliseconds timeout) {
    if (timeout < std::chrono::milliseconds(1)) {
        timeout = std::chrono::milliseconds(1);
    }
    std::lock_guard<std::mutex> lock(Mutex);
    OpenTimeout = timeout;
}
#endif

void TqClientIngressReactor::Run() {
    {
        std::lock_guard<std::mutex> lifecycleLock(LifecycleMutex);
        ReactorThreadId = std::this_thread::get_id();
    }
    while (Running.load(std::memory_order_acquire)) {
        ProcessPendingTasks();
        if (!Running.load(std::memory_order_acquire)) {
            break;
        }
        ProcessDueDelayedTasks();
        int timeoutMs = 50;
        {
            std::lock_guard<std::mutex> lock(Mutex);
            timeoutMs = NextRunTimeoutMsLocked();
        }
        const auto runStarted = std::chrono::steady_clock::now();
        (void)Reactor.RunOnce(timeoutMs);
        const auto elapsedMicros = std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::steady_clock::now() - runStarted).count();
        const uint64_t timeoutMicros = static_cast<uint64_t>(timeoutMs) * 1000;
        const uint64_t overshootMicros = elapsedMicros > static_cast<int64_t>(timeoutMicros)
            ? static_cast<uint64_t>(elapsedMicros) - timeoutMicros
            : 0;
        constexpr std::array<uint64_t, 12> kOvershootBucketBounds{
            100, 250, 500, 1000, 2500, 5000,
            10000, 25000, 50000, 100000, 250000, UINT64_MAX};
        std::lock_guard<std::mutex> lock(Mutex);
        ++ReactorTimeoutOvershootSamples;
        ReactorTimeoutOvershootMaxMicros =
            std::max(ReactorTimeoutOvershootMaxMicros, overshootMicros);
        const auto bucket = std::lower_bound(
            kOvershootBucketBounds.begin(), kOvershootBucketBounds.end(), overshootMicros);
        ++ReactorTimeoutOvershootBuckets[static_cast<size_t>(
            bucket - kOvershootBucketBounds.begin())];
    }
    {
        std::lock_guard<std::mutex> lifecycleLock(LifecycleMutex);
        ReactorThreadId = std::thread::id{};
    }
}

bool TqClientIngressReactor::IsReactorThread() const {
    std::lock_guard<std::mutex> lifecycleLock(LifecycleMutex);
    return std::this_thread::get_id() == ReactorThreadId;
}

bool TqClientIngressReactor::EnqueueDelayed(
    std::chrono::milliseconds delay,
    std::function<void()> task) {
    if (!task) {
        return false;
    }
    if (delay < std::chrono::milliseconds(0)) {
        delay = std::chrono::milliseconds(0);
    }
    std::lock_guard<std::mutex> lifecycleLock(LifecycleMutex);
    if (State != LifecycleState::Running || !Running.load(std::memory_order_acquire)) {
        return false;
    }
    {
        std::lock_guard<std::mutex> lock(Mutex);
        DelayedTasks.push_back(DelayedTask{
            std::chrono::steady_clock::now() + delay,
            NextDelayedTaskOrder++,
            std::move(task)});
        MaxDelayedTaskQueueDepth = std::max<uint64_t>(
            MaxDelayedTaskQueueDepth, static_cast<uint64_t>(DelayedTasks.size()));
    }
    (void)Reactor.Wake();
    return true;
}

TqClientIngressDiagnostics TqClientIngressReactor::SnapshotDiagnostics() const {
    std::lock_guard<std::mutex> lifecycleLock(LifecycleMutex);
    std::lock_guard<std::mutex> lock(Mutex);
    TqClientIngressDiagnostics snapshot;
    snapshot.DelayedTaskQueueDepth = static_cast<uint64_t>(DelayedTasks.size());
    snapshot.MaxDelayedTaskQueueDepth = MaxDelayedTaskQueueDepth;
    snapshot.ReactorTimeoutOvershootSamples = ReactorTimeoutOvershootSamples;
    snapshot.ReactorTimeoutOvershootMaxMicros = ReactorTimeoutOvershootMaxMicros;
    if (ReactorTimeoutOvershootSamples == 0) {
        return snapshot;
    }
    constexpr std::array<uint64_t, 12> kOvershootBucketBounds{
        100, 250, 500, 1000, 2500, 5000,
        10000, 25000, 50000, 100000, 250000, UINT64_MAX};
    const auto percentile = [&](uint64_t numerator) {
        const uint64_t quotient = ReactorTimeoutOvershootSamples / 100;
        const uint64_t remainder = ReactorTimeoutOvershootSamples % 100;
        const uint64_t target = quotient * numerator +
            (remainder * numerator + 99) / 100;
        uint64_t cumulative = 0;
        for (size_t i = 0; i < ReactorTimeoutOvershootBuckets.size(); ++i) {
            cumulative += ReactorTimeoutOvershootBuckets[i];
            if (cumulative >= target) {
                return kOvershootBucketBounds[i];
            }
        }
        return kOvershootBucketBounds.back();
    };
    snapshot.ReactorTimeoutOvershootP95Micros = percentile(95);
    snapshot.ReactorTimeoutOvershootP99Micros = percentile(99);
    return snapshot;
}

bool TqClientIngressReactor::EnqueueSync(std::function<bool()> task) {
    if (IsReactorThread()) {
        return task ? task() : false;
    }

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

void TqClientIngressReactor::ProcessDueDelayedTasks() {
    std::vector<std::function<void()>> tasks;
    const auto now = std::chrono::steady_clock::now();
    {
        std::lock_guard<std::mutex> lock(Mutex);
        auto out = DelayedTasks.begin();
        for (auto it = DelayedTasks.begin(); it != DelayedTasks.end(); ++it) {
            if (it->Due <= now) {
                tasks.push_back(std::move(it->Task));
            } else {
                if (out != it) {
                    *out = std::move(*it);
                }
                ++out;
            }
        }
        DelayedTasks.erase(out, DelayedTasks.end());
    }

    for (auto& task : tasks) {
        if (task) {
            {
                std::lock_guard<std::mutex> lifecycleLock(LifecycleMutex);
                if (State != LifecycleState::Running ||
                    !Running.load(std::memory_order_acquire)) {
                    return;
                }
                ++ActiveDelayedTasks;
            }
            task();
            {
                std::lock_guard<std::mutex> lifecycleLock(LifecycleMutex);
                if (ActiveDelayedTasks > 0) {
                    --ActiveDelayedTasks;
                }
                if (State == LifecycleState::Stopping && ActiveDelayedTasks == 0) {
                    LifecycleCv.notify_all();
                }
            }
        }
    }
}

int TqClientIngressReactor::NextRunTimeoutMsLocked() const {
    constexpr int kMaxTimeoutMs = 50;
    if (DelayedTasks.empty()) {
        return kMaxTimeoutMs;
    }

    const auto now = std::chrono::steady_clock::now();
    auto earliest = DelayedTasks.front().Due;
    auto earliestOrder = DelayedTasks.front().Order;
    for (const auto& task : DelayedTasks) {
        if (task.Due < earliest ||
            (task.Due == earliest && task.Order < earliestOrder)) {
            earliest = task.Due;
            earliestOrder = task.Order;
        }
    }
    if (earliest <= now) {
        return 0;
    }

    const auto delay = std::chrono::duration_cast<std::chrono::milliseconds>(earliest - now);
    if (delay.count() <= 0) {
        return 0;
    }
    return static_cast<int>(std::min<int64_t>(kMaxTimeoutMs, delay.count()));
}

void TqClientIngressReactor::AcceptLoop(TqSocketHandle listenFd) {
    std::string peerId;
    ListenProto proto = ListenProto::Socks5;
    TqPortForwardConfig forward;
    {
        std::lock_guard<std::mutex> lock(Mutex);
        const auto listenIt = Listens.find(listenFd);
        if (listenIt == Listens.end()) {
            return;
        }
        peerId = listenIt->second.PeerId;
        proto = listenIt->second.Proto;
        forward = listenIt->second.Forward;
    }

    for (int accepted = 0; accepted < TqMaxIngressAcceptsPerEvent; ++accepted) {
        TqSocketHandle clientFd = TqAcceptClient(listenFd);

        if (TqSocketValid(clientFd)) {
            bool startDirectOpen = false;
            {
                std::lock_guard<std::mutex> lock(Mutex);
                const auto peerIt = Peers.find(peerId);
                if (peerIt == Peers.end()) {
                    TqCloseFd(clientFd);
                    continue;
                }

                ClientEntry client{};
                client.PeerId = peerId;
                client.Proto = proto;
                if (proto == ListenProto::PortForward) {
                    client.State = TqClientIngressState(TqClientIngressProto::Socks5);
                    client.FixedRequest = TqBuildPortForwardRequest(forward);
                    client.HasFixedRequest = true;
                    startDirectOpen = true;
                } else {
                    std::shared_ptr<const TqProxyAuthTable> auth =
                        std::make_shared<TqProxyAuthTable>(peerIt->second.Peer.Config.Router.ProxyAuth);
                    client.State = TqClientIngressState(
                        TqToIngressProto(proto == ListenProto::Socks5),
                        auth);
                }
                client.StartTunnel = peerIt->second.Peer.StartTunnel;
                client.AcceptTunnel = peerIt->second.Peer.AcceptTunnel;
                client.RejectTunnel = peerIt->second.Peer.RejectTunnel;
                client.CancelTunnel = peerIt->second.Peer.CancelTunnel;
                if (!Reactor.Add(clientFd, TqReactorEvents::Read,
                        [this](TqSocketHandle fd, uint32_t events) {
                            HandleClientEvents(fd, events);
                        })) {
                    TqCloseFd(clientFd);
                    continue;
                }
                Clients.emplace(clientFd, std::move(client));
                TqTunnelDebugLog(
                    "event=ingress_accept fd=%lld listen_fd=%lld peer=%s proto=%s",
                    static_cast<long long>(clientFd),
                    static_cast<long long>(listenFd),
                    peerId.c_str(),
                    ListenProtoName(proto));
            }
            if (startDirectOpen) {
                StartClientOpen(clientFd);
            }
            continue;
        }

        if (TqIsSocketInterrupted()) {
            continue;
        }
        if (TqIsSocketWouldBlock()) {
            return;
        }
        return;
    }
}

void TqClientIngressReactor::HandleClientEvents(TqSocketHandle clientFd, uint32_t events) {
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

void TqClientIngressReactor::HandleClientRead(TqSocketHandle clientFd) {
    for (;;) {
        uint8_t buffer[TqIngressReadBufferSize]{};
        const int received = TqRecv(clientFd, buffer, sizeof(buffer), TqRecvFlags::None);
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

        if (TqIsSocketInterrupted()) {
            continue;
        }
        if (TqIsSocketWouldBlock()) {
            return;
        }

        std::lock_guard<std::mutex> lock(Mutex);
        CloseClientLocked(clientFd, true);
        return;
    }
}

void TqClientIngressReactor::HandleClientWrite(TqSocketHandle clientFd) {
    for (;;) {
        TqClientIngressTunnelAcceptFn acceptTunnel;
        TqClientIngressTunnelCloseFn rejectTunnel;
        TqClientTunnelOpenHandle* completedHandle = nullptr;
        std::shared_ptr<OpenCompletionState> completedState;
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
                    completedState = client.OpenCompletion;
                    completedOpenSucceeded = client.OpenSucceeded;
                    acceptTunnel = client.AcceptTunnel;
                    rejectTunnel = client.RejectTunnel;
                    if (client.Proto == ListenProto::PortForward && !completedOpenSucceeded) {
                        client.OpenHandle = nullptr;
                        removeOwnedByTunnel = false;
                        closeOwnedByReactor = true;
                    } else {
                        removeOwnedByTunnel = completedHandle != nullptr;
                        closeOwnedByReactor = completedHandle == nullptr;
                    }
                }
            } else {
                const int sent = TqSend(clientFd, pending.data(), pending.size(), TqSendFlags::NoSignal);
                if (sent > 0) {
                    if (!handshakeWrite && sent > 0) {
                        TqTunnelDebugLog(
                            "event=ingress_open_response_write fd=%lld proto=%s phase=%s bytes=%d remaining_before=%zu open_ok=%d",
                            static_cast<long long>(clientFd),
                            ListenProtoName(client.Proto),
                            ClientPhaseName(client.Phase),
                            sent,
                            pending.size(),
                            client.OpenSucceeded ? 1 : 0);
                    }
                    if (handshakeWrite) {
                        client.State.MarkWriteComplete(static_cast<size_t>(sent));
                        feedAfterHandshakeWrite = client.State.PendingWrite().empty();
                    } else {
                        client.PendingWrite.erase(0, static_cast<size_t>(sent));
                        if (client.PendingWrite.empty()) {
                            completedHandle = client.OpenHandle;
                            completedState = client.OpenCompletion;
                            completedOpenSucceeded = client.OpenSucceeded;
                            acceptTunnel = client.AcceptTunnel;
                            rejectTunnel = client.RejectTunnel;
                            if (client.Proto == ListenProto::PortForward && !completedOpenSucceeded) {
                                client.OpenHandle = nullptr;
                                removeOwnedByTunnel = false;
                                closeOwnedByReactor = true;
                            } else {
                                removeOwnedByTunnel = completedHandle != nullptr;
                                closeOwnedByReactor = completedHandle == nullptr;
                            }
                        }
                    }
                } else if (sent < 0 && TqIsSocketInterrupted()) {
                    continue;
                } else if (sent < 0 && TqIsSocketWouldBlock()) {
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
                const bool terminalReserved = MarkOpenCompletionTerminal(completedState);
                if (terminalReserved && acceptTunnel) {
                    accepted = acceptTunnel(completedHandle);
                }
                TqTunnelDebugLog(
                    "event=ingress_tunnel_accept fd=%lld accepted=%d",
                    static_cast<long long>(clientFd),
                    accepted ? 1 : 0);
                if (!accepted) {
                    if (terminalReserved && completedState) {
                        std::lock_guard<std::mutex> completionLock(completedState->Mutex);
                        completedState->TerminalCalled = false;
                    }
                    RejectOpenHandleOnce(completedHandle, rejectTunnel, completedState);
                }
            } else {
                TqTunnelDebugLog(
                    "event=ingress_tunnel_reject fd=%lld reason=open_failed",
                    static_cast<long long>(clientFd));
                RejectOpenHandleOnce(completedHandle, rejectTunnel, completedState);
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

void TqClientIngressReactor::HandleIngressResult(TqSocketHandle clientFd, TqClientIngressResult result) {
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

void TqClientIngressReactor::StartClientOpen(TqSocketHandle clientFd) {
    TunnelRequest request{};
    TqClientIngressTunnelStartFn startTunnel;
    TqClientIngressTunnelCloseFn rejectTunnel;
    bool socks = true;
    bool portForward = false;
    std::weak_ptr<CompletionToken> completionToken;
    std::shared_ptr<OpenCompletionState> completionState;
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
        request = client.HasFixedRequest ? client.FixedRequest : client.State.Request();
        startTunnel = client.StartTunnel;
        rejectTunnel = client.RejectTunnel;
        socks = client.Proto == ListenProto::Socks5;
        portForward = client.Proto == ListenProto::PortForward;
        completionToken = CompletionTokenPtr;
        completionState = std::make_shared<OpenCompletionState>();
        client.OpenCompletion = completionState;
        client.Phase = ClientPhase::Opening;
        (void)Reactor.Modify(clientFd, TqReactorEvents::Error);
        TqTunnelDebugLog(
            "event=ingress_open_start fd=%lld peer=%s proto=%s target=%s",
            static_cast<long long>(clientFd),
            client.PeerId.c_str(),
            ListenProtoName(client.Proto),
            TqRequestTarget(request).c_str());
    }

    if (!startTunnel) {
        TqTunnelDebugLog(
            "event=ingress_open_start_failed fd=%lld target=%s reason=no_start_tunnel",
            static_cast<long long>(clientFd),
            TqRequestTarget(request).c_str());
        CompleteClientOpen(clientFd, nullptr, TqTunnelStartResult{false, TqOpenError::Internal, 0});
        return;
    }

    TqClientTunnelOpenHandle* handle = startTunnel(
        request,
        clientFd,
        [completionToken, clientFd, rejectTunnel, completionState](
            TqClientTunnelOpenHandle* completedHandle,
            TqTunnelStartResult result) mutable {
            auto token = completionToken.lock();
            if (!token) {
                RejectOpenHandleOnce(completedHandle, rejectTunnel, completionState);
                return;
            }
            TqClientIngressReactor* reactor = nullptr;
            {
                std::lock_guard<std::mutex> tokenLock(token->Mutex);
                reactor = token->Reactor;
                if (reactor != nullptr) {
                    ++token->ActiveCallbacks;
                }
            }
            if (reactor == nullptr) {
                RejectOpenHandleOnce(completedHandle, rejectTunnel, completionState);
                return;
            }
            const bool queued = reactor->EnqueueOpenCompletion(
                completionToken,
                clientFd,
                completedHandle,
                result,
                rejectTunnel,
                completionState);
            {
                std::lock_guard<std::mutex> tokenLock(token->Mutex);
                --token->ActiveCallbacks;
            }
            token->Cv.notify_all();
            if (!queued) {
                RejectOpenHandleOnce(completedHandle, rejectTunnel, completionState);
            }
        });

    if (handle == nullptr) {
        TqTunnelDebugLog(
            "event=ingress_open_start_failed fd=%lld target=%s reason=null_handle",
            static_cast<long long>(clientFd),
            TqRequestTarget(request).c_str());
        CompleteClientOpen(clientFd, nullptr, TqTunnelStartResult{false, TqOpenError::Internal, 0});
        return;
    }

    std::chrono::milliseconds timeout{};
    {
        std::lock_guard<std::mutex> lock(Mutex);
        auto it = Clients.find(clientFd);
        if (it == Clients.end()) {
            TqTunnelDebugLog(
                "event=ingress_open_orphaned fd=%lld target=%s",
                static_cast<long long>(clientFd),
                TqRequestTarget(request).c_str());
            RejectOpenHandleOnce(handle, rejectTunnel, completionState);
            return;
        }
        it->second.OpenHandle = handle;
        if (it->second.PendingWrite.empty() && !portForward) {
            it->second.PendingWrite = TqBuildOpenResponse(socks, TqOpenError::Internal);
        }
        timeout = OpenTimeout;
    }
    (void)EnqueueDelayed(timeout, [this, clientFd, completionState]() {
        TimeoutClientOpen(clientFd, completionState);
    });
}

void TqClientIngressReactor::TimeoutClientOpen(
    TqSocketHandle clientFd,
    std::shared_ptr<OpenCompletionState> completionState) {
    TqClientTunnelOpenHandle* handle = nullptr;
    TqClientIngressTunnelCloseFn cancelTunnel;
    {
        std::lock_guard<std::mutex> lock(Mutex);
        auto it = Clients.find(clientFd);
        if (it == Clients.end()) {
            return;
        }
        ClientEntry& client = it->second;
        if (client.Phase != ClientPhase::Opening || client.OpenCompletion != completionState) {
            return;
        }
        TqTunnelDebugLog(
            "event=ingress_open_timeout fd=%lld proto=%s",
            static_cast<long long>(clientFd),
            ListenProtoName(client.Proto));
        handle = client.OpenHandle;
        cancelTunnel = client.CancelTunnel;
        client.OpenHandle = nullptr;
        client.OpenSucceeded = false;
        if (client.Proto == ListenProto::PortForward) {
            client.PendingWrite.clear();
        } else {
            client.PendingWrite = TqBuildOpenResponse(client.Proto == ListenProto::Socks5, TqOpenError::TcpTimeout);
        }
        client.Phase = ClientPhase::WritingOpenResponse;
        (void)Reactor.Modify(clientFd, TqReactorEvents::Write | TqReactorEvents::Error);
    }
    CancelOpenHandleOnce(handle, cancelTunnel, completionState);
    HandleClientWrite(clientFd);
}

bool TqClientIngressReactor::EnqueueOpenCompletion(
    const std::weak_ptr<CompletionToken>& token,
    TqSocketHandle clientFd,
    TqClientTunnelOpenHandle* handle,
    TqTunnelStartResult result,
    TqClientIngressTunnelCloseFn rejectTunnel,
    std::shared_ptr<OpenCompletionState> completionState) {
    return EnqueueAsync([token,
                            clientFd,
                            handle,
                            result,
                            rejectTunnel = std::move(rejectTunnel),
                            completionState = std::move(completionState)]() mutable {
        auto lockedToken = token.lock();
        if (!lockedToken) {
            RejectOpenHandleOnce(handle, rejectTunnel, completionState);
            return;
        }
        TqClientIngressReactor* reactor = nullptr;
        {
            std::lock_guard<std::mutex> tokenLock(lockedToken->Mutex);
            reactor = lockedToken->Reactor;
        }
        if (reactor == nullptr) {
            RejectOpenHandleOnce(handle, rejectTunnel, completionState);
            return;
        }
        reactor->CompleteClientOpen(clientFd, handle, result, rejectTunnel, completionState);
    });
}

void TqClientIngressReactor::CompleteClientOpen(
    TqSocketHandle clientFd,
    TqClientTunnelOpenHandle* handle,
    TqTunnelStartResult result,
    TqClientIngressTunnelCloseFn rejectTunnel,
    std::shared_ptr<OpenCompletionState> completionState) {
    bool writeNow = false;
    {
        std::lock_guard<std::mutex> lock(Mutex);
        auto it = Clients.find(clientFd);
        if (it == Clients.end()) {
            TqTunnelDebugLog(
                "event=ingress_open_complete_orphaned fd=%lld tunnel_id=%llu ok=%d error=%s",
                static_cast<long long>(clientFd),
                static_cast<unsigned long long>(result.TraceTunnelId),
                result.Ok ? 1 : 0,
                TqOpenErrorName(result.Error));
            RejectOpenHandleOnce(handle, rejectTunnel, completionState);
            return;
        }
        ClientEntry& client = it->second;
        if (client.Phase != ClientPhase::Opening) {
            TqTunnelDebugLog(
                "event=ingress_open_complete_wrong_phase fd=%lld tunnel_id=%llu phase=%s ok=%d error=%s",
                static_cast<long long>(clientFd),
                static_cast<unsigned long long>(result.TraceTunnelId),
                ClientPhaseName(client.Phase),
                result.Ok ? 1 : 0,
                TqOpenErrorName(result.Error));
            RejectOpenHandleOnce(handle, rejectTunnel, completionState);
            return;
        }
        if (client.OpenHandle != nullptr && handle != nullptr && client.OpenHandle != handle) {
            TqTunnelDebugLog(
                "event=ingress_open_complete_handle_mismatch fd=%lld tunnel_id=%llu ok=%d error=%s",
                static_cast<long long>(clientFd),
                static_cast<unsigned long long>(result.TraceTunnelId),
                result.Ok ? 1 : 0,
                TqOpenErrorName(result.Error));
            RejectOpenHandleOnce(handle, rejectTunnel, completionState);
            return;
        }
        if (client.OpenHandle == nullptr) {
            client.OpenHandle = handle;
        }

        const TqOpenError error = result.Ok ? TqOpenError::Ok : result.Error;
        client.OpenCompletion = completionState;
        client.OpenSucceeded = result.Ok;
        TqTunnelDebugLog(
            "event=ingress_open_complete fd=%lld proto=%s tunnel_id=%llu ok=%d error=%s response_error=%s",
            static_cast<long long>(clientFd),
            ListenProtoName(client.Proto),
            static_cast<unsigned long long>(result.TraceTunnelId),
            result.Ok ? 1 : 0,
            TqOpenErrorName(result.Error),
            TqOpenErrorName(error));
        if (client.Proto == ListenProto::PortForward) {
            client.PendingWrite.clear();
        } else {
            client.PendingWrite = TqBuildOpenResponse(client.Proto == ListenProto::Socks5, error);
        }
        client.Phase = ClientPhase::WritingOpenResponse;
        (void)Reactor.Modify(clientFd, TqReactorEvents::Write | TqReactorEvents::Error);
        writeNow = true;
    }
    if (writeNow) {
        HandleClientWrite(clientFd);
    }
}

void TqClientIngressReactor::CloseClientLocked(TqSocketHandle clientFd, bool closeFd) {
    auto it = Clients.find(clientFd);
    if (it == Clients.end()) {
        return;
    }
    ClientEntry client = std::move(it->second);
    Clients.erase(it);
    (void)Reactor.Remove(clientFd);
    TqTunnelDebugLog(
        "event=ingress_close fd=%lld proto=%s phase=%s close_fd=%d open_handle=%d",
        static_cast<long long>(clientFd),
        ListenProtoName(client.Proto),
        ClientPhaseName(client.Phase),
        closeFd ? 1 : 0,
        client.OpenHandle != nullptr ? 1 : 0);
    if (client.OpenHandle != nullptr) {
        CancelOpenHandleOnce(client.OpenHandle, client.CancelTunnel, client.OpenCompletion);
        return;
    }
    if (closeFd) {
        TqSocketHandle fd = clientFd;
        TqCloseFd(fd);
    }
}

void TqClientIngressReactor::CloseClientOwnedByTunnelLocked(TqSocketHandle clientFd) {
    auto it = Clients.find(clientFd);
    if (it == Clients.end()) {
        return;
    }
    TqTunnelDebugLog(
        "event=ingress_owned_by_tunnel fd=%lld proto=%s",
        static_cast<long long>(clientFd),
        ListenProtoName(it->second.Proto));
    Clients.erase(it);
    (void)Reactor.Remove(clientFd);
}

void TqClientIngressReactor::RemovePeerLocked(const std::string& peerId) {
    auto it = Peers.find(peerId);
    if (it == Peers.end()) {
        return;
    }

    for (auto& listen : it->second.Listeners) {
        if (TqSocketValid(listen.Fd)) {
            (void)Reactor.Remove(listen.Fd);
            Listens.erase(listen.Fd);
            TqCloseFd(listen.Fd);
        }
    }
    it->second.Listeners.clear();

    std::vector<TqSocketHandle> clientFds;
    for (auto clientIt = Clients.begin(); clientIt != Clients.end(); ++clientIt) {
        if (clientIt->second.PeerId == peerId) {
            clientFds.push_back(clientIt->first);
        }
    }
    for (TqSocketHandle fd : clientFds) {
        CloseClientLocked(fd, true);
    }

    Peers.erase(it);
}

void TqClientIngressReactor::CloseAllLocked() {
    for (auto& item : Peers) {
        for (auto& listen : item.second.Listeners) {
            if (TqSocketValid(listen.Fd)) {
                (void)Reactor.Remove(listen.Fd);
                Listens.erase(listen.Fd);
                TqCloseFd(listen.Fd);
            }
        }
        item.second.Listeners.clear();
    }

    std::vector<TqSocketHandle> clientFds;
    clientFds.reserve(Clients.size());
    for (const auto& item : Clients) {
        clientFds.push_back(item.first);
    }
    for (TqSocketHandle fd : clientFds) {
        CloseClientLocked(fd, true);
    }

    Clients.clear();
    Listens.clear();
    Peers.clear();
}
