#include "ares_dns_resolver.h"

#include "linux_reactor.h"

#include <ares.h>

#include <algorithm>
#include <cstring>
#include <sys/time.h>
#include <unordered_map>
#include <unordered_set>
#include <vector>

struct TqAresDnsResolver::Impl {
    struct PendingQuery {
        Impl* Owner{nullptr};
        uint64_t Id{0};
        uint16_t Port{0};
        bool InternalCompleted{false};
        TqDnsResolveResult Result;
        TqDnsResolveCallback Callback;
    };

    TqLinuxReactor Reactor;
    ares_channel_t* Channel{nullptr};
    uint64_t NextId{1};
    bool CallbackCompleted{false};
    std::unordered_map<uint64_t, PendingQuery*> Pending;
    std::vector<uint64_t> CompletedIds;
    std::unordered_set<int> RegisteredSockets;

    static void SockStateCallback(void* data, ares_socket_t socketFd, int readable, int writable) {
        auto* self = static_cast<Impl*>(data);
        if (self == nullptr) {
            return;
        }
        self->UpdateSocket(socketFd, readable, writable);
    }

    static void ResolveCallback(void* arg, int status, int, ares_addrinfo* res) {
        auto* query = static_cast<PendingQuery*>(arg);
        if (query == nullptr) {
            if (res != nullptr) {
                ares_freeaddrinfo(res);
            }
            return;
        }

        query->Result.Completed = true;
        query->Result.Success = status == ARES_SUCCESS;
        query->Result.Status = status;

        if (res != nullptr) {
            for (ares_addrinfo_node* node = res->nodes; node != nullptr; node = node->ai_next) {
                if (node->ai_addr == nullptr ||
                    node->ai_addrlen > static_cast<ares_socklen_t>(sizeof(sockaddr_storage))) {
                    continue;
                }

                sockaddr_storage address{};
                std::memcpy(&address, node->ai_addr, node->ai_addrlen);
                SetPort(address, query->Port);
                query->Result.Addresses.push_back(address);
            }
            ares_freeaddrinfo(res);
        }

        query->InternalCompleted = true;
        if (query->Owner == nullptr || query->Owner->Pending.find(query->Id) == query->Owner->Pending.end()) {
            delete query;
            return;
        }
        query->Owner->CompletedIds.push_back(query->Id);
    }

    static void SetPort(sockaddr_storage& address, uint16_t port) {
        if (address.ss_family == AF_INET) {
            auto* addr4 = reinterpret_cast<sockaddr_in*>(&address);
            addr4->sin_port = htons(port);
        } else if (address.ss_family == AF_INET6) {
            auto* addr6 = reinterpret_cast<sockaddr_in6*>(&address);
            addr6->sin6_port = htons(port);
        }
    }

    bool Start() {
        if (Channel != nullptr) {
            return true;
        }
        if (!Reactor.Start()) {
            return false;
        }

        ares_options options{};
        options.sock_state_cb = &Impl::SockStateCallback;
        options.sock_state_cb_data = this;
        if (ares_init_options(&Channel, &options, ARES_OPT_SOCK_STATE_CB) != ARES_SUCCESS) {
            Reactor.Stop();
            Channel = nullptr;
            return false;
        }
        return true;
    }

    void Stop() {
        for (auto& entry : Pending) {
            PendingQuery* query = entry.second;
            if (query == nullptr) {
                continue;
            }
            query->Callback = {};
            if (query->InternalCompleted) {
                delete query;
            }
        }
        Pending.clear();
        CompletedIds.clear();
        RegisteredSockets.clear();
        if (Channel != nullptr) {
            ares_cancel(Channel);
            ares_destroy(Channel);
            Channel = nullptr;
        }
        Reactor.Stop();
        CallbackCompleted = false;
    }

    uint64_t Resolve(const std::string& host, uint16_t port, TqDnsResolveCallback callback) {
        if (Channel == nullptr || host.empty() || !callback) {
            return 0;
        }

        auto* query = new PendingQuery();
        query->Owner = this;
        query->Id = NextToken();
        query->Port = port;
        query->Callback = std::move(callback);

        const uint64_t id = query->Id;
        Pending.emplace(id, query);

        ares_addrinfo_hints hints{};
        hints.ai_family = AF_UNSPEC;
        hints.ai_socktype = SOCK_STREAM;
        ares_getaddrinfo(Channel, host.c_str(), nullptr, &hints, &Impl::ResolveCallback, query);
        return id;
    }

    void Cancel(uint64_t id) {
        auto it = Pending.find(id);
        if (it != Pending.end()) {
            PendingQuery* query = it->second;
            Pending.erase(it);
            CompletedIds.erase(std::remove(CompletedIds.begin(), CompletedIds.end(), id), CompletedIds.end());
            if (query != nullptr) {
                query->Callback = {};
                if (query->InternalCompleted) {
                    delete query;
                }
            }
        }
    }

    bool RunOnce(int timeoutMs) {
        if (Channel == nullptr) {
            return false;
        }

        CallbackCompleted = false;
        DeliverCompleted();
        if (CallbackCompleted) {
            return true;
        }
        const bool reactorWork = Reactor.RunOnce(NextWaitMs(timeoutMs));
        const bool timeoutProgress = ProcessTimeouts();
        DeliverCompleted();
        return CallbackCompleted || reactorWork || timeoutProgress;
    }

    void UpdateSocket(ares_socket_t socketFd, int readable, int writable) {
        if (Channel == nullptr || socketFd == ARES_SOCKET_BAD) {
            return;
        }

        const int fd = static_cast<int>(socketFd);
        const uint32_t events = ToReactorEvents(readable, writable);
        auto it = RegisteredSockets.find(fd);
        if (events == 0) {
            if (it != RegisteredSockets.end()) {
                (void)Reactor.Remove(fd);
                RegisteredSockets.erase(it);
            }
            return;
        }

        if (it == RegisteredSockets.end()) {
            if (Reactor.Add(fd, events, [this](int readyFd, uint32_t readyEvents) {
                    ProcessFd(readyFd, readyEvents);
                })) {
                RegisteredSockets.insert(fd);
            }
            return;
        }

        if (!Reactor.Modify(fd, events)) {
            (void)Reactor.Remove(fd);
            RegisteredSockets.erase(it);
            if (Reactor.Add(fd, events, [this](int readyFd, uint32_t readyEvents) {
                    ProcessFd(readyFd, readyEvents);
                })) {
                RegisteredSockets.insert(fd);
            }
        }
    }

    void ProcessFd(int fd, uint32_t events) {
        if (Channel == nullptr) {
            return;
        }

        ares_fd_events_t fdEvent{};
        fdEvent.fd = static_cast<ares_socket_t>(fd);
        if ((events & (TqLinuxReactorEvents::Read | TqLinuxReactorEvents::Error)) != 0) {
            fdEvent.events |= ARES_FD_EVENT_READ;
        }
        if ((events & TqLinuxReactorEvents::Write) != 0) {
            fdEvent.events |= ARES_FD_EVENT_WRITE;
        }

        if (fdEvent.events != ARES_FD_EVENT_NONE) {
            (void)ares_process_fds(Channel, &fdEvent, 1, ARES_PROCESS_FLAG_NONE);
        }
    }

    bool ProcessTimeouts() {
        if (Channel == nullptr) {
            return false;
        }
        const size_t completedBefore = CompletedIds.size();
        (void)ares_process_fds(Channel, nullptr, 0, ARES_PROCESS_FLAG_NONE);
        return CompletedIds.size() != completedBefore;
    }

    void DeliverCompleted() {
        std::vector<uint64_t> completed;
        completed.swap(CompletedIds);
        for (uint64_t id : completed) {
            auto it = Pending.find(id);
            if (it == Pending.end()) {
                continue;
            }
            PendingQuery* query = it->second;
            Pending.erase(it);
            if (query == nullptr) {
                continue;
            }

            TqDnsResolveCallback callback = std::move(query->Callback);
            TqDnsResolveResult result = std::move(query->Result);
            delete query;
            if (callback) {
                CallbackCompleted = true;
                callback(result);
            }
        }
    }

    int NextWaitMs(int requestedTimeoutMs) {
        if (requestedTimeoutMs < 0) {
            timeval timeout{};
            timeval* selected = ares_timeout(Channel, nullptr, &timeout);
            if (selected == nullptr) {
                return requestedTimeoutMs;
            }
            return TimevalToMs(*selected);
        }

        timeval maxTimeout{};
        maxTimeout.tv_sec = requestedTimeoutMs / 1000;
        maxTimeout.tv_usec = (requestedTimeoutMs % 1000) * 1000;

        timeval timeout{};
        timeval* selected = ares_timeout(Channel, &maxTimeout, &timeout);
        if (selected == nullptr) {
            return requestedTimeoutMs;
        }

        const int milliseconds = TimevalToMs(*selected);
        if (milliseconds <= 0) {
            return 0;
        }
        if (milliseconds > requestedTimeoutMs) {
            return requestedTimeoutMs;
        }
        return milliseconds;
    }

    static int TimevalToMs(const timeval& value) {
        const long long milliseconds =
            static_cast<long long>(value.tv_sec) * 1000 +
            (static_cast<long long>(value.tv_usec) + 999) / 1000;
        if (milliseconds <= 0) {
            return 0;
        }
        if (milliseconds > INT32_MAX) {
            return INT32_MAX;
        }
        return static_cast<int>(milliseconds);
    }

    static uint32_t ToReactorEvents(int readable, int writable) {
        uint32_t events = 0;
        if (readable != 0) {
            events |= TqLinuxReactorEvents::Read;
        }
        if (writable != 0) {
            events |= TqLinuxReactorEvents::Write;
        }
        return events;
    }

    uint64_t NextToken() {
        while (NextId == 0 || Pending.find(NextId) != Pending.end()) {
            ++NextId;
        }
        return NextId++;
    }
};

TqAresDnsResolver::TqAresDnsResolver()
    : State(new Impl()) {
}

TqAresDnsResolver::~TqAresDnsResolver() {
    Stop();
    delete State;
    State = nullptr;
}

bool TqAresDnsResolver::Start() {
    return State != nullptr && State->Start();
}

void TqAresDnsResolver::Stop() {
    if (State != nullptr) {
        State->Stop();
    }
}

uint64_t TqAresDnsResolver::Resolve(
    const std::string& host,
    uint16_t port,
    TqDnsResolveCallback callback) {
    if (State == nullptr) {
        return 0;
    }
    return State->Resolve(host, port, std::move(callback));
}

void TqAresDnsResolver::Cancel(uint64_t id) {
    if (State != nullptr) {
        State->Cancel(id);
    }
}

bool TqAresDnsResolver::RunOnce(int timeoutMs) {
    return State != nullptr && State->RunOnce(timeoutMs);
}

#ifdef TQ_UNIT_TESTING
int TqAresDnsResolver::TestOnlyNextWaitMs(int requestedTimeoutMs) {
    if (State == nullptr) {
        return requestedTimeoutMs;
    }
    return State->NextWaitMs(requestedTimeoutMs);
}
#endif
