#include "client_ingress_reactor.h"

#include "http_connect_server.h"
#include "listen_socket.h"
#include "scoped_socket.h"
#include "socks5_server.h"

#include <cerrno>
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
        Running.store(false, std::memory_order_release);
        oldToken = std::move(CompletionTokenPtr);
        {
            std::lock_guard<std::mutex> tokenLock(oldToken->Mutex);
            oldToken->Reactor = nullptr;
        }
        CompletionTokenPtr = std::make_shared<CompletionToken>();
        CompletionTokenPtr->Reactor = this;
        (void)Reactor.Wake();
    }

    if (Worker.joinable()) {
        Worker.join();
    }
    ProcessPendingTasks();

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
            if (TqSocketValid(socks->Fd)) {
                (void)Reactor.Remove(socks->Fd);
                Listens.erase(socks->Fd);
                TqCloseFd(socks->Fd);
            }
            if (TqSocketValid(http->Fd)) {
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
                [this](TqSocketHandle fd, uint32_t events) {
                    if ((events & (TqReactorEvents::Read | TqReactorEvents::Error)) != 0) {
                        AcceptLoop(fd);
                    }
                })) {
            cleanup();
            return false;
        }

        if (TqSocketValid(http->Fd)) {
            Listens.emplace(http->Fd, ListenEntry{peer.PeerId, ListenProto::HttpConnect});
            if (!Reactor.Add(http->Fd, TqReactorEvents::Read,
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
        entry.SocksFd = socks->Fd;
        entry.HttpFd = http->Fd;
        entry.SocksAddress = socks->Address;
        entry.HttpAddress = http->Address;
        socks->Fd = TqInvalidSocket;
        http->Fd = TqInvalidSocket;
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

void TqClientIngressReactor::AcceptLoop(TqSocketHandle listenFd) {
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
        TqSocketHandle clientFd = TqAcceptClient(listenFd);

        if (TqSocketValid(clientFd)) {
            std::lock_guard<std::mutex> lock(Mutex);
            const auto peerIt = Peers.find(peerId);
            if (peerIt == Peers.end()) {
                TqCloseFd(clientFd);
                continue;
            }

            ClientEntry client{};
            client.PeerId = peerId;
            client.Proto = proto;
            std::shared_ptr<const TqProxyAuthTable> auth =
                std::make_shared<TqProxyAuthTable>(peerIt->second.Peer.Config.Router.ProxyAuth);
            client.State = TqClientIngressState(
                TqToIngressProto(proto == ListenProto::Socks5),
                auth);
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
                    removeOwnedByTunnel = completedHandle != nullptr;
                    closeOwnedByReactor = completedHandle == nullptr;
                }
            } else {
                const int sent = TqSend(clientFd, pending.data(), pending.size(), TqSendFlags::NoSignal);
                if (sent > 0) {
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
                            removeOwnedByTunnel = completedHandle != nullptr;
                            closeOwnedByReactor = completedHandle == nullptr;
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
                if (!accepted) {
                    if (terminalReserved && completedState) {
                        std::lock_guard<std::mutex> completionLock(completedState->Mutex);
                        completedState->TerminalCalled = false;
                    }
                    RejectOpenHandleOnce(completedHandle, rejectTunnel, completedState);
                }
            } else {
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
        request = client.State.Request();
        startTunnel = client.StartTunnel;
        rejectTunnel = client.RejectTunnel;
        socks = client.Proto == ListenProto::Socks5;
        completionToken = CompletionTokenPtr;
        completionState = std::make_shared<OpenCompletionState>();
        client.OpenCompletion = completionState;
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
        CompleteClientOpen(clientFd, nullptr, TqTunnelStartResult{false, TqOpenError::Internal, 0});
        return;
    }

    std::lock_guard<std::mutex> lock(Mutex);
    auto it = Clients.find(clientFd);
    if (it == Clients.end()) {
        RejectOpenHandleOnce(handle, rejectTunnel, completionState);
        return;
    }
    it->second.OpenHandle = handle;
    if (it->second.PendingWrite.empty()) {
        it->second.PendingWrite = TqBuildOpenResponse(socks, TqOpenError::Internal);
    }
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
            RejectOpenHandleOnce(handle, rejectTunnel, completionState);
            return;
        }
        ClientEntry& client = it->second;
        if (client.Phase != ClientPhase::Opening) {
            RejectOpenHandleOnce(handle, rejectTunnel, completionState);
            return;
        }
        if (client.OpenHandle != nullptr && handle != nullptr && client.OpenHandle != handle) {
            RejectOpenHandleOnce(handle, rejectTunnel, completionState);
            return;
        }
        if (client.OpenHandle == nullptr) {
            client.OpenHandle = handle;
        }

        const TqOpenError error = result.Ok ? TqOpenError::Ok : result.Error;
        client.OpenCompletion = completionState;
        client.OpenSucceeded = result.Ok;
        client.PendingWrite = TqBuildOpenResponse(client.Proto == ListenProto::Socks5, error);
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
    Clients.erase(it);
    (void)Reactor.Remove(clientFd);
}

void TqClientIngressReactor::RemovePeerLocked(const std::string& peerId) {
    auto it = Peers.find(peerId);
    if (it == Peers.end()) {
        return;
    }

    if (TqSocketValid(it->second.SocksFd)) {
        (void)Reactor.Remove(it->second.SocksFd);
        Listens.erase(it->second.SocksFd);
        TqCloseFd(it->second.SocksFd);
    }
    if (TqSocketValid(it->second.HttpFd)) {
        (void)Reactor.Remove(it->second.HttpFd);
        Listens.erase(it->second.HttpFd);
        TqCloseFd(it->second.HttpFd);
    }

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
        if (TqSocketValid(item.second.SocksFd)) {
            (void)Reactor.Remove(item.second.SocksFd);
            TqCloseFd(item.second.SocksFd);
        }
        if (TqSocketValid(item.second.HttpFd)) {
            (void)Reactor.Remove(item.second.HttpFd);
            TqCloseFd(item.second.HttpFd);
        }
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
