#include "server_dial_reactor.h"

#include "acl_filter.h"
#include "linux_reactor.h"

#include <cerrno>
#include <chrono>
#include <utility>
#include <unordered_map>
#include <vector>

#include <sys/socket.h>

namespace {

constexpr int kConnectAttemptTimeoutMs = 10000;

TqAcl AllowAllAcl() {
    TqAcl acl;
    acl.AllowCidrs = {"0.0.0.0/0", "::/0"};
    return acl;
}

socklen_t SockaddrLength(const sockaddr_storage& addr) {
    switch (addr.ss_family) {
    case AF_INET:
        return sizeof(sockaddr_in);
    case AF_INET6:
        return sizeof(sockaddr_in6);
    default:
        return 0;
    }
}

bool ParseIpLiteral(const std::string& host, std::vector<sockaddr_storage>& out) {
    out.clear();

    sockaddr_storage addr4Storage{};
    auto* addr4 = reinterpret_cast<sockaddr_in*>(&addr4Storage);
    addr4Storage.ss_family = AF_INET;
    if (TqInetPton(AF_INET, host.c_str(), &addr4->sin_addr)) {
        out.push_back(addr4Storage);
        return true;
    }

    sockaddr_storage addr6Storage{};
    auto* addr6 = reinterpret_cast<sockaddr_in6*>(&addr6Storage);
    addr6Storage.ss_family = AF_INET6;
    if (TqInetPton(AF_INET6, host.c_str(), &addr6->sin6_addr)) {
        out.push_back(addr6Storage);
        return true;
    }

    return false;
}

bool IsRefusedError(int error) {
    return error == ECONNREFUSED;
}

bool IsTimeoutLikeError(int error) {
    return error == ETIMEDOUT || error == EHOSTUNREACH || error == ENETUNREACH ||
        error == EAGAIN || error == EWOULDBLOCK;
}

void CloseOwnedSocket(TqSocketHandle& fd) {
    if (TqSocketValid(fd)) {
        TqCloseSocket(fd);
        fd = TqInvalidSocket;
    }
}

} // namespace

struct TqServerDialReactor::Impl {
    struct DialState {
        uint64_t Token{0};
        TqServerDialRequest Request;
        uint64_t DnsId{0};
        std::vector<sockaddr_storage> Candidates;
        size_t NextCandidate{0};
        TqSocketHandle Fd{TqInvalidSocket};
        bool Registered{false};
        bool DnsCallbackRan{false};
        bool SawRefused{false};
        bool SawTimeout{false};
        bool SawInternal{false};
        std::chrono::steady_clock::time_point Deadline;
    };

    TqAcl Acl;
    TqLinuxReactor Reactor;
    TqAresDnsResolver DnsResolver;
#ifdef TQ_UNIT_TESTING
    TestHooks Hooks;
#endif
    bool Started{false};
    uint64_t NextToken{1};
    std::unordered_map<uint64_t, std::unique_ptr<DialState>> Pending;

    explicit Impl(TqAcl acl)
        : Acl(std::move(acl)) {
    }

#ifdef TQ_UNIT_TESTING
    Impl(TqAcl acl, TestHooks hooks)
        : Acl(std::move(acl))
        , Hooks(std::move(hooks)) {
    }
#endif

    bool Start() {
        if (Started) {
            return true;
        }
        if (!Reactor.Start()) {
            return false;
        }
#ifdef TQ_UNIT_TESTING
        if (!Hooks.Resolve && !DnsResolver.Start()) {
            Reactor.Stop();
            return false;
        }
#else
        if (!DnsResolver.Start()) {
            Reactor.Stop();
            return false;
        }
#endif
        Started = true;
        return true;
    }

    void Stop() {
        for (auto& entry : Pending) {
            CleanupState(*entry.second, true);
        }
        Pending.clear();
#ifdef TQ_UNIT_TESTING
        if (!Hooks.Resolve) {
            DnsResolver.Stop();
        }
#else
        DnsResolver.Stop();
#endif
        Reactor.Stop();
        Started = false;
    }

    uint64_t Submit(TqServerDialRequest request) {
        if (!Started || request.Host.empty() || request.Port == 0 || !request.Complete) {
            return 0;
        }

        auto state = std::make_unique<DialState>();
        state->Token = NextAvailableToken();
        state->Request = std::move(request);
        const uint64_t token = state->Token;
        Pending.emplace(token, std::move(state));

        std::vector<sockaddr_storage> literalAddrs;
        if (ParseIpLiteral(Pending[token]->Request.Host, literalAddrs)) {
            OnResolved(token, literalAddrs, true);
            return token;
        }

        const uint64_t dnsId = Resolve(Pending[token]->Request.Host, Pending[token]->Request.Port,
            [this, token](const TqDnsResolveResult& result) {
                OnDnsResult(token, result);
            });
        if (dnsId == 0) {
            auto it = Pending.find(token);
            if (it != Pending.end()) {
                CleanupState(*it->second, true);
                Pending.erase(it);
            }
            return 0;
        }
        auto it = Pending.find(token);
        if (it != Pending.end() && !it->second->DnsCallbackRan) {
            it->second->DnsId = dnsId;
        }
        return token;
    }

    void Cancel(uint64_t token) {
        auto it = Pending.find(token);
        if (it == Pending.end()) {
            return;
        }
        CleanupState(*it->second, true);
        Pending.erase(it);
    }

    bool RunOnce(int timeoutMs) {
        if (!Started) {
            return false;
        }

        const int waitMs = NextWaitMs(timeoutMs);
        bool didWork = RunDnsOnce(0);
        didWork = Reactor.RunOnce(waitMs) || didWork;
        didWork = RunDnsOnce(0) || didWork;
        didWork = ExpireAttempts() || didWork;
        return didWork;
    }

    uint64_t NextAvailableToken() {
        while (NextToken == 0 || Pending.find(NextToken) != Pending.end()) {
            ++NextToken;
        }
        return NextToken++;
    }

    uint64_t Resolve(
        const std::string& host,
        uint16_t port,
        TqDnsResolveCallback callback) {
#ifdef TQ_UNIT_TESTING
        if (Hooks.Resolve) {
            return Hooks.Resolve(host, port, std::move(callback));
        }
#endif
        return DnsResolver.Resolve(host, port, std::move(callback));
    }

    void CancelResolve(uint64_t id) {
        if (id == 0) {
            return;
        }
#ifdef TQ_UNIT_TESTING
        if (Hooks.CancelResolve) {
            Hooks.CancelResolve(id);
            return;
        }
#endif
        DnsResolver.Cancel(id);
    }

    bool RunDnsOnce(int timeoutMs) {
#ifdef TQ_UNIT_TESTING
        if (Hooks.RunDnsOnce) {
            return Hooks.RunDnsOnce(timeoutMs);
        }
#endif
        return DnsResolver.RunOnce(timeoutMs);
    }

    void OnDnsResult(uint64_t token, const TqDnsResolveResult& result) {
        auto it = Pending.find(token);
        if (it == Pending.end()) {
            return;
        }
        it->second->DnsCallbackRan = true;
        it->second->DnsId = 0;
        if (!result.Completed || !result.Success || result.Addresses.empty()) {
            Complete(token, TqOpenError::DnsFailed);
            return;
        }
        OnResolved(token, result.Addresses, false);
    }

    void OnResolved(
        uint64_t token,
        const std::vector<sockaddr_storage>& addresses,
        bool literal) {
        auto it = Pending.find(token);
        if (it == Pending.end()) {
            return;
        }

        std::vector<sockaddr_storage> filtered;
        if (!TqAclFilterResolvedAddresses(Acl, addresses, it->second->Request.Port, filtered)) {
            Complete(token, TqOpenError::AclDenied);
            return;
        }
        if (filtered.empty()) {
            Complete(token, literal ? TqOpenError::AclDenied : TqOpenError::TcpTimeout);
            return;
        }

        it->second->Candidates = std::move(filtered);
        it->second->NextCandidate = 0;
        StartNextAttempt(token);
    }

    void StartNextAttempt(uint64_t token) {
        auto it = Pending.find(token);
        if (it == Pending.end()) {
            return;
        }
        DialState& state = *it->second;
        CleanupConnect(state);

        while (state.NextCandidate < state.Candidates.size()) {
            const sockaddr_storage& addr = state.Candidates[state.NextCandidate++];
            const socklen_t addrLen = SockaddrLength(addr);
            if (addrLen == 0) {
                continue;
            }

            TqSocketHandle fd = CreateSocket(addr.ss_family, SOCK_STREAM, IPPROTO_TCP);
            if (!TqSocketValid(fd)) {
                RecordInternalFailure(state);
                continue;
            }
            if (!SetNonBlocking(fd)) {
                RecordInternalFailure(state);
                TqCloseSocket(fd);
                continue;
            }

            const int connectResult = Connect(fd, reinterpret_cast<const sockaddr*>(&addr), addrLen);
            if (connectResult == 0) {
                state.Fd = fd;
                CompleteConnected(token);
                return;
            }

            const int connectError = LastSocketError(fd);
            if (!TqSocketInProgress(connectError)) {
                RecordConnectFailure(state, connectError);
                TqCloseSocket(fd);
                continue;
            }

            state.Fd = fd;
            state.Deadline =
                std::chrono::steady_clock::now() + std::chrono::milliseconds(kConnectAttemptTimeoutMs);
            if (!Reactor.Add(fd, TqLinuxReactorEvents::Write | TqLinuxReactorEvents::Error,
                    [this, token](int readyFd, uint32_t events) {
                        OnConnectReady(token, readyFd, events);
                    })) {
                state.Fd = TqInvalidSocket;
                TqCloseSocket(fd);
                Complete(token, TqOpenError::Internal);
                return;
            }
            state.Registered = true;
            return;
        }

        Complete(token, FinalConnectError(state));
    }

    void OnConnectReady(uint64_t token, int readyFd, uint32_t) {
        auto it = Pending.find(token);
        if (it == Pending.end()) {
            return;
        }
        DialState& state = *it->second;
        if (state.Fd != readyFd) {
            return;
        }

        int error = 0;
        const bool gotSocketError = GetSocketError(state.Fd, error);

        if (state.Registered) {
            (void)Reactor.Remove(state.Fd);
            state.Registered = false;
        }

        if (!gotSocketError) {
            RecordInternalFailure(state);
            CloseOwnedSocket(state.Fd);
            StartNextAttempt(token);
            return;
        }

        if (error == 0) {
            CompleteConnected(token);
            return;
        }

        RecordConnectFailure(state, error);
        CloseOwnedSocket(state.Fd);
        StartNextAttempt(token);
    }

    bool ExpireAttempts() {
        const auto now = std::chrono::steady_clock::now();
        std::vector<uint64_t> expired;
        for (const auto& entry : Pending) {
            const DialState& state = *entry.second;
            if (TqSocketValid(state.Fd) && state.Deadline <= now) {
                expired.push_back(entry.first);
            }
        }

        for (uint64_t token : expired) {
            auto it = Pending.find(token);
            if (it == Pending.end()) {
                continue;
            }
            DialState& state = *it->second;
            state.SawTimeout = true;
            CleanupConnect(state);
            StartNextAttempt(token);
        }
        return !expired.empty();
    }

    int NextWaitMs(int requestedTimeoutMs) const {
        int selected = requestedTimeoutMs;
        const auto now = std::chrono::steady_clock::now();
        for (const auto& entry : Pending) {
            const DialState& state = *entry.second;
            if (!TqSocketValid(state.Fd)) {
                continue;
            }
            const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(
                state.Deadline - now);
            int remainingMs = remaining.count() <= 0 ? 0 : static_cast<int>(remaining.count());
            if (selected < 0 || remainingMs < selected) {
                selected = remainingMs;
            }
        }
        if (HasPendingDns()) {
            constexpr int kDnsPollMs = 50;
            if (selected < 0 || selected > kDnsPollMs) {
                selected = kDnsPollMs;
            }
        }
        return selected;
    }

    bool HasPendingDns() const {
        for (const auto& entry : Pending) {
            if (entry.second->DnsId != 0) {
                return true;
            }
        }
        return false;
    }

    void RecordConnectFailure(DialState& state, int error) {
        if (IsRefusedError(error)) {
            state.SawRefused = true;
            return;
        }
        if (IsTimeoutLikeError(error)) {
            state.SawTimeout = true;
            return;
        }
        RecordInternalFailure(state);
    }

    void RecordInternalFailure(DialState& state) {
        state.SawInternal = true;
    }

    TqOpenError FinalConnectError(const DialState& state) const {
        if (state.SawInternal) {
            return TqOpenError::Internal;
        }
        if (state.SawRefused) {
            return TqOpenError::TcpRefused;
        }
        if (state.SawTimeout) {
            return TqOpenError::TcpTimeout;
        }
        return TqOpenError::TcpTimeout;
    }

    TqSocketHandle CreateSocket(int family, int type, int protocol) {
#ifdef TQ_UNIT_TESTING
        if (Hooks.CreateSocket) {
            return Hooks.CreateSocket(family, type, protocol);
        }
#endif
        return ::socket(family, type, protocol);
    }

    bool SetNonBlocking(TqSocketHandle fd) {
#ifdef TQ_UNIT_TESTING
        if (Hooks.SetNonBlocking) {
            return Hooks.SetNonBlocking(fd);
        }
#endif
        return TqSetNonBlocking(fd);
    }

    int Connect(TqSocketHandle fd, const sockaddr* addr, socklen_t addrLen) {
#ifdef TQ_UNIT_TESTING
        if (Hooks.Connect) {
            return Hooks.Connect(fd, addr, addrLen);
        }
#endif
        return ::connect(fd, addr, addrLen);
    }

    int LastSocketError(TqSocketHandle fd) {
#ifdef TQ_UNIT_TESTING
        if (Hooks.GetLastSocketError) {
            return Hooks.GetLastSocketError(fd);
        }
#else
        (void)fd;
#endif
        return TqLastSocketError();
    }

    bool GetSocketError(TqSocketHandle fd, int& error) {
#ifdef TQ_UNIT_TESTING
        if (Hooks.GetSocketError) {
            return Hooks.GetSocketError(fd, &error) == 0;
        }
#endif
        socklen_t errorLen = sizeof(error);
        return ::getsockopt(fd, SOL_SOCKET, SO_ERROR, &error, &errorLen) == 0;
    }

    void CompleteConnected(uint64_t token) {
        auto it = Pending.find(token);
        if (it == Pending.end()) {
            return;
        }
        DialState& state = *it->second;
        if (state.Registered) {
            (void)Reactor.Remove(state.Fd);
            state.Registered = false;
        }

        TqServerDialResult result;
        result.Done = true;
        result.Error = TqOpenError::Ok;
        result.Fd = state.Fd;
        state.Fd = TqInvalidSocket;

        TqServerDialComplete complete = std::move(state.Request.Complete);
        Pending.erase(it);
        if (complete) {
            complete(result);
        } else {
            CloseOwnedSocket(result.Fd);
        }
    }

    void Complete(uint64_t token, TqOpenError error) {
        auto it = Pending.find(token);
        if (it == Pending.end()) {
            return;
        }
        DialState& state = *it->second;
        CleanupState(state, false);

        TqServerDialResult result;
        result.Done = true;
        result.Error = error;
        result.Fd = TqInvalidSocket;

        TqServerDialComplete complete = std::move(state.Request.Complete);
        Pending.erase(it);
        if (complete) {
            complete(result);
        }
    }

    void CleanupState(DialState& state, bool suppressCompletion) {
        if (suppressCompletion) {
            state.Request.Complete = {};
        }
        if (state.DnsId != 0) {
            CancelResolve(state.DnsId);
            state.DnsId = 0;
        }
        CleanupConnect(state);
    }

    void CleanupConnect(DialState& state) {
        if (state.Registered && TqSocketValid(state.Fd)) {
            (void)Reactor.Remove(state.Fd);
            state.Registered = false;
        }
        CloseOwnedSocket(state.Fd);
    }
};

TqServerDialReactor::TqServerDialReactor()
    : State(std::make_unique<Impl>(AllowAllAcl())) {
}

TqServerDialReactor::TqServerDialReactor(TqAcl acl)
    : State(std::make_unique<Impl>(std::move(acl))) {
}

#ifdef TQ_UNIT_TESTING
TqServerDialReactor::TqServerDialReactor(TqAcl acl, TestHooks hooks)
    : State(std::make_unique<Impl>(std::move(acl), std::move(hooks))) {
}
#endif

TqServerDialReactor::~TqServerDialReactor() {
    Stop();
}

bool TqServerDialReactor::Start() {
    return State != nullptr && State->Start();
}

void TqServerDialReactor::Stop() {
    if (State != nullptr) {
        State->Stop();
    }
}

uint64_t TqServerDialReactor::Submit(TqServerDialRequest request) {
    if (State == nullptr) {
        return 0;
    }
    return State->Submit(std::move(request));
}

void TqServerDialReactor::Cancel(uint64_t token) {
    if (State != nullptr) {
        State->Cancel(token);
    }
}

bool TqServerDialReactor::RunOnce(int timeoutMs) {
    return State != nullptr && State->RunOnce(timeoutMs);
}
