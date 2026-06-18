#pragma once

#include "config.h"
#include "linux_reactor.h"

#include <atomic>
#include <cstddef>
#include <condition_variable>
#include <deque>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>

struct TqClientIngressPeer {
    std::string PeerId;
    std::string SocksListen;
    std::string HttpListen;
    TqConfig Config;
};

class TqClientIngressReactor {
public:
    TqClientIngressReactor() = default;
    ~TqClientIngressReactor();

    TqClientIngressReactor(const TqClientIngressReactor&) = delete;
    TqClientIngressReactor& operator=(const TqClientIngressReactor&) = delete;

    bool Start();
    void Stop();
    bool AddPeer(const TqClientIngressPeer& peer);
    bool RemovePeer(const std::string& peerId);
    size_t PeerCountForTest() const;

#if defined(TQ_UNIT_TESTING)
    std::string SocksListenAddressForTest(const std::string& peerId) const;
    std::string HttpListenAddressForTest(const std::string& peerId) const;
#endif

private:
    enum class ListenProto {
        Socks5,
        HttpConnect,
    };

    enum class LifecycleState {
        Stopped,
        Running,
        Stopping,
    };

    struct PeerEntry {
        TqClientIngressPeer Peer;
        int SocksFd{-1};
        int HttpFd{-1};
        std::string SocksAddress;
        std::string HttpAddress;
    };

    struct ListenEntry {
        std::string PeerId;
        ListenProto Proto{ListenProto::Socks5};
    };

    struct ClientEntry {
        std::string PeerId;
        ListenProto Proto{ListenProto::Socks5};
    };

    void Run();
    bool EnqueueSync(std::function<bool()> task);
    void ProcessPendingTasks();
    void AcceptLoop(int listenFd);
    void RemovePeerLocked(const std::string& peerId);
    void CloseAllLocked();

    mutable std::mutex LifecycleMutex;
    std::condition_variable LifecycleCv;
    mutable std::mutex Mutex;
    LifecycleState State{LifecycleState::Stopped};
    std::atomic<bool> Running{false};
    TqLinuxReactor Reactor;
    std::thread Worker;
    std::deque<std::function<void()>> PendingTasks;
    std::unordered_map<std::string, PeerEntry> Peers;
    std::unordered_map<int, ListenEntry> Listens;
    std::unordered_map<int, ClientEntry> Clients;
};
