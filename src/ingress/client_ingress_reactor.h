#pragma once

#include "client_ingress_state.h"
#include "config.h"
#include "socket_reactor.h"
#if defined(_WIN32)
#include "windows_reactor.h"
#elif defined(__APPLE__)
#include "darwin_reactor.h"
#else
#include "linux_reactor.h"
#endif
#include "client_tunnel_open.h"

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <condition_variable>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#if defined(_WIN32)
using TqClientIngressPlatformReactor = TqWindowsReactor;
#elif defined(__APPLE__)
using TqClientIngressPlatformReactor = TqDarwinReactor;
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
    std::vector<TqPortForwardConfig> PortForwards;
    TqConfig Config;
    TqClientIngressTunnelStartFn StartTunnel;
    TqClientIngressTunnelAcceptFn AcceptTunnel;
    TqClientIngressTunnelCloseFn RejectTunnel;
    TqClientIngressTunnelCloseFn CancelTunnel;
};

class TqClientIngressReactor {
public:
    TqClientIngressReactor();
    ~TqClientIngressReactor();

    TqClientIngressReactor(const TqClientIngressReactor&) = delete;
    TqClientIngressReactor& operator=(const TqClientIngressReactor&) = delete;

    bool Start();
    void Stop();
    bool AddPeer(const TqClientIngressPeer& peer);
    bool RemovePeer(const std::string& peerId);
    bool EnqueueDelayed(std::chrono::milliseconds delay, std::function<void()> task);
    size_t PeerCountForTest() const;

#if defined(TQ_UNIT_TESTING)
    std::string SocksListenAddressForTest(const std::string& peerId) const;
    std::string HttpListenAddressForTest(const std::string& peerId) const;
    std::string PortForwardListenAddressForTest(const std::string& peerId, size_t index) const;
    void SetOpenTimeoutForTest(std::chrono::milliseconds timeout);
#endif

private:
    enum class ListenProto {
        Socks5,
        HttpConnect,
        PortForward,
    };

    enum class LifecycleState {
        Stopped,
        Running,
        Stopping,
    };

    struct ListenEntry {
        std::string PeerId;
        ListenProto Proto{ListenProto::Socks5};
        TqPortForwardConfig Forward;
    };

    struct PeerListen {
        ListenProto Proto{ListenProto::Socks5};
        TqSocketHandle Fd{TqInvalidSocket};
        std::string Address;
        TqPortForwardConfig Forward;
    };

    struct PeerEntry {
        TqClientIngressPeer Peer;
        std::vector<PeerListen> Listeners;
    };

    struct DelayedTask {
        std::chrono::steady_clock::time_point Due;
        uint64_t Order{0};
        std::function<void()> Task;
    };

    enum class ClientPhase {
        Handshake,
        Opening,
        WritingOpenResponse,
    };

    struct OpenCompletionState {
        std::mutex Mutex;
        bool TerminalCalled{false};
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
        TunnelRequest FixedRequest{};
        bool HasFixedRequest{false};
        std::string PendingWrite;
        TqClientTunnelOpenHandle* OpenHandle{nullptr};
        std::shared_ptr<OpenCompletionState> OpenCompletion;
        bool OpenSucceeded{false};
    };

    struct CompletionToken {
        std::mutex Mutex;
        std::condition_variable Cv;
        TqClientIngressReactor* Reactor{nullptr};
        size_t ActiveCallbacks{0};
    };

    void Run();
    bool EnqueueSync(std::function<bool()> task);
    bool IsReactorThread() const;
    bool EnqueueAsync(std::function<void()> task);
    void ProcessPendingTasks();
    void ProcessDueDelayedTasks();
    int NextRunTimeoutMsLocked() const;
    void AcceptLoop(TqSocketHandle listenFd);
    void HandleClientEvents(TqSocketHandle clientFd, uint32_t events);
    void HandleClientRead(TqSocketHandle clientFd);
    void HandleClientWrite(TqSocketHandle clientFd);
    void HandleIngressResult(TqSocketHandle clientFd, TqClientIngressResult result);
    void StartClientOpen(TqSocketHandle clientFd);
    void TimeoutClientOpen(TqSocketHandle clientFd, std::shared_ptr<OpenCompletionState> completionState);
    void CompleteClientOpen(
        TqSocketHandle clientFd,
        TqClientTunnelOpenHandle* handle,
        TqTunnelStartResult result,
        TqClientIngressTunnelCloseFn rejectTunnel = TqClientIngressTunnelCloseFn{},
        std::shared_ptr<OpenCompletionState> completionState = nullptr);
    bool EnqueueOpenCompletion(
        const std::weak_ptr<CompletionToken>& token,
        TqSocketHandle clientFd,
        TqClientTunnelOpenHandle* handle,
        TqTunnelStartResult result,
        TqClientIngressTunnelCloseFn rejectTunnel,
        std::shared_ptr<OpenCompletionState> completionState);
    static bool MarkOpenCompletionTerminal(const std::shared_ptr<OpenCompletionState>& completionState);
    static void RejectOpenHandleOnce(
        TqClientTunnelOpenHandle* handle,
        const TqClientIngressTunnelCloseFn& rejectTunnel,
        const std::shared_ptr<OpenCompletionState>& completionState);
    static void CancelOpenHandleOnce(
        TqClientTunnelOpenHandle* handle,
        const TqClientIngressTunnelCloseFn& cancelTunnel,
        const std::shared_ptr<OpenCompletionState>& completionState);
    void CloseClientLocked(TqSocketHandle clientFd, bool closeFd);
    void CloseClientOwnedByTunnelLocked(TqSocketHandle clientFd);
    void RemovePeerLocked(const std::string& peerId);
    void CloseAllLocked();
    static const char* ListenProtoName(ListenProto proto);
    static const char* ClientPhaseName(ClientPhase phase);

    mutable std::mutex LifecycleMutex;
    std::condition_variable LifecycleCv;
    mutable std::mutex Mutex;
    LifecycleState State{LifecycleState::Stopped};
    std::atomic<bool> Running{false};
    TqSocketStartup SocketStartup;
    TqClientIngressPlatformReactor Reactor;
    std::shared_ptr<CompletionToken> CompletionTokenPtr{std::make_shared<CompletionToken>()};
    std::thread Worker;
    std::thread::id ReactorThreadId{};
    std::deque<std::function<void()>> PendingTasks;
    std::vector<DelayedTask> DelayedTasks;
    uint64_t NextDelayedTaskOrder{1};
    size_t ActiveDelayedTasks{0};
    std::unordered_map<std::string, PeerEntry> Peers;
    std::unordered_map<TqSocketHandle, ListenEntry> Listens;
    std::unordered_map<TqSocketHandle, ClientEntry> Clients;
    std::chrono::milliseconds OpenTimeout{std::chrono::seconds(10)};
};
