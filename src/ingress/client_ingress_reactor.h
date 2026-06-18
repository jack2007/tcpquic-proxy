#pragma once

#include "client_ingress_state.h"
#include "config.h"
#include "socket_reactor.h"
#if defined(_WIN32)
#include "windows_reactor.h"
#else
#include "linux_reactor.h"
#endif
#include "client_tunnel_open.h"

#include <atomic>
#include <cstddef>
#include <condition_variable>
#include <deque>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>

#if defined(_WIN32)
using TqClientIngressPlatformReactor = TqWindowsReactor;
#else
using TqClientIngressPlatformReactor = TqLinuxReactor;
#endif

using TqClientIngressTunnelStartFn =
    std::function<TqClientTunnelOpenHandle*(
        const TunnelRequest&,
        TqSocketHandle,
        TqClientTunnelOpenComplete)>;
using TqClientIngressTunnelAcceptFn = std::function<bool(TqClientTunnelOpenHandle*)>;
using TqClientIngressTunnelCloseFn = std::function<void(TqClientTunnelOpenHandle*)>;

struct TqClientIngressPeer {
    std::string PeerId;
    std::string SocksListen;
    std::string HttpListen;
    TqConfig Config;
    TqClientIngressTunnelStartFn StartTunnel;
    TqClientIngressTunnelAcceptFn AcceptTunnel;
    TqClientIngressTunnelCloseFn RejectTunnel;
    TqClientIngressTunnelCloseFn CancelTunnel;
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
        TqSocketHandle SocksFd{TqInvalidSocket};
        TqSocketHandle HttpFd{TqInvalidSocket};
        std::string SocksAddress;
        std::string HttpAddress;
    };

    struct ListenEntry {
        std::string PeerId;
        ListenProto Proto{ListenProto::Socks5};
    };

    enum class ClientPhase {
        Handshake,
        Opening,
        WritingOpenResponse,
    };

    struct ClientEntry {
        std::string PeerId;
        ListenProto Proto{ListenProto::Socks5};
        TqClientIngressState State{TqClientIngressProto::Socks5};
        TqClientIngressTunnelStartFn StartTunnel;
        TqClientIngressTunnelAcceptFn AcceptTunnel;
        TqClientIngressTunnelCloseFn RejectTunnel;
        TqClientIngressTunnelCloseFn CancelTunnel;
        ClientPhase Phase{ClientPhase::Handshake};
        std::string PendingWrite;
        TqClientTunnelOpenHandle* OpenHandle{nullptr};
        bool OpenSucceeded{false};
    };

    void Run();
    bool EnqueueSync(std::function<bool()> task);
    bool EnqueueAsync(std::function<void()> task);
    void ProcessPendingTasks();
    void AcceptLoop(TqSocketHandle listenFd);
    void HandleClientEvents(TqSocketHandle clientFd, uint32_t events);
    void HandleClientRead(TqSocketHandle clientFd);
    void HandleClientWrite(TqSocketHandle clientFd);
    void HandleIngressResult(TqSocketHandle clientFd, TqClientIngressResult result);
    void StartClientOpen(TqSocketHandle clientFd);
    void CompleteClientOpen(
        TqSocketHandle clientFd,
        TqClientTunnelOpenHandle* handle,
        TqTunnelStartResult result);
    void CloseClientLocked(TqSocketHandle clientFd, bool closeFd);
    void CloseClientOwnedByTunnelLocked(TqSocketHandle clientFd);
    void RemovePeerLocked(const std::string& peerId);
    void CloseAllLocked();

    mutable std::mutex LifecycleMutex;
    std::condition_variable LifecycleCv;
    mutable std::mutex Mutex;
    LifecycleState State{LifecycleState::Stopped};
    std::atomic<bool> Running{false};
    TqSocketStartup SocketStartup;
    TqClientIngressPlatformReactor Reactor;
    std::thread Worker;
    std::deque<std::function<void()>> PendingTasks;
    std::unordered_map<std::string, PeerEntry> Peers;
    std::unordered_map<TqSocketHandle, ListenEntry> Listens;
    std::unordered_map<TqSocketHandle, ClientEntry> Clients;
};
