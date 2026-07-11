#include "linux_relay_worker.h"
#include "tcp_tunnel.h"
#include "tunnel_reaper.h"

#include <arpa/inet.h>
#include <atomic>
#include <cstdlib>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <functional>
#include <mutex>
#include <netinet/in.h>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <thread>
#include <unistd.h>
#include <vector>

namespace {

#define CHECK(x) do { if (!(x)) { std::fprintf(stderr, "CHECK %s:%d: %s\n", \
    __FILE__, __LINE__, #x); return false; } } while (false)

std::atomic<uint64_t> g_shutdowns{0}, g_closes{0};
std::atomic<uint32_t> g_lastFlags{0};
std::mutex g_sendLock;
std::vector<void*> g_sendContexts;
std::vector<QUIC_SEND_FLAGS> g_sendFlags;
struct FakeTransportHarness {
    virtual ~FakeTransportHarness() = default;
    virtual void OnSend(MsQuicStream*, QUIC_SEND_FLAGS, void*) = 0;
    virtual void OnShutdown(HQUIC, QUIC_STREAM_SHUTDOWN_FLAGS) = 0;
};
std::atomic<FakeTransportHarness*> g_transport{nullptr};

void QUIC_API FakeSetCallbackHandler(HQUIC, void*, void*) {}
void QUIC_API FakeStreamClose(HQUIC) { ++g_closes; }
QUIC_STATUS QUIC_API FakeStreamShutdown(HQUIC handle, QUIC_STREAM_SHUTDOWN_FLAGS flags, QUIC_UINT62) {
    ++g_shutdowns;
    g_lastFlags.store(static_cast<uint32_t>(flags));
    if (auto* transport = g_transport.load(std::memory_order_acquire)) {
        transport->OnShutdown(handle, flags);
    }
    return QUIC_STATUS_SUCCESS;
}
void QUIC_API FakeStreamReceiveComplete(HQUIC, uint64_t) {}
QUIC_STATUS QUIC_API FakeStreamReceiveSetEnabled(HQUIC, BOOLEAN) { return QUIC_STATUS_SUCCESS; }
QUIC_STATUS CaptureSend(MsQuicStream* stream, const QUIC_BUFFER*, uint32_t,
                        QUIC_SEND_FLAGS flags, void* context) {
    std::lock_guard<std::mutex> guard(g_sendLock);
    g_sendContexts.push_back(context);
    g_sendFlags.push_back(flags);
    if (auto* transport = g_transport.load(std::memory_order_acquire)) {
        transport->OnSend(stream, flags, context);
    }
    return QUIC_STATUS_SUCCESS;
}

struct GlobalGuard {
    QUIC_API_TABLE Api{};
    GlobalGuard() {
        const auto beforeMetrics = TqTerminalMetricsSnapshot();
        const auto beforeRegistries = TqStreamLifetime::SnapshotRegistries();
        if (TqGetActiveRelayCount() != 0 || beforeMetrics.TerminalSinkPending != 0 ||
            beforeMetrics.TerminalTimeoutPending != 0 ||
            TqStreamLifetime::SnapshotSendCompletions().ActiveCount != 0 ||
            TqStreamLifetime::SnapshotTerminalRetentions().OwnerCount != 0 ||
            beforeRegistries.SendCompletionCount != 0) {
            std::fprintf(stderr, "dirty terminal baseline before case\n");
            std::abort();
        }
        TqTerminalScheduler::ResetForTest();
        TqResetTerminalMetricsForTest();
        TqStreamLifetime::ResetLifecycleRegistriesForTest();
        g_shutdowns = 0; g_closes = 0; g_lastFlags = 0;
        { std::lock_guard<std::mutex> guard(g_sendLock); g_sendContexts.clear(); g_sendFlags.clear(); }
        Api.SetCallbackHandler = FakeSetCallbackHandler;
        Api.StreamClose = FakeStreamClose;
        Api.StreamShutdown = FakeStreamShutdown;
        Api.StreamReceiveComplete = FakeStreamReceiveComplete;
        Api.StreamReceiveSetEnabled = FakeStreamReceiveSetEnabled;
        MsQuic = reinterpret_cast<const MsQuicApi*>(&Api);
        TqLinuxRelaySetStreamSendForTest(CaptureSend);
    }
    ~GlobalGuard() {
        TqLinuxRelaySetStreamSendForTest(nullptr);
        g_transport.store(nullptr, std::memory_order_release);
        TqTerminalScheduler::SetBeforeExecuteForTest({});
        TqTerminalScheduler::SetAfterEnqueueForTest({});
        TqTerminalScheduler::ResetForTest();
        MsQuic = nullptr;
    }
};

struct TcpPair {
    int Relay{-1};
    int Peer{-1};
    TcpPair() = default;
    TcpPair(const TcpPair&) = delete;
    TcpPair& operator=(const TcpPair&) = delete;
    TcpPair(TcpPair&& other) noexcept
        : Relay(std::exchange(other.Relay, -1)), Peer(std::exchange(other.Peer, -1)) {}
    TcpPair& operator=(TcpPair&& other) noexcept {
        if (this != &other) {
            if (Relay >= 0) ::close(Relay);
            if (Peer >= 0) ::close(Peer);
            Relay = std::exchange(other.Relay, -1);
            Peer = std::exchange(other.Peer, -1);
        }
        return *this;
    }
    ~TcpPair() {
        if (Relay >= 0) ::close(Relay);
        if (Peer >= 0) ::close(Peer);
    }
};

struct ScopeGuard {
    std::function<void()> Cleanup;
    explicit ScopeGuard(std::function<void()> cleanup) : Cleanup(std::move(cleanup)) {}
    ~ScopeGuard() { if (Cleanup) Cleanup(); }
    void Dismiss() { Cleanup = {}; }
};
TcpPair LoopbackPair() {
    TcpPair out;
    int listener = ::socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (listener < 0) return out;
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    address.sin_port = 0;
    socklen_t length = sizeof(address);
    if (::bind(listener, reinterpret_cast<sockaddr*>(&address), length) != 0 ||
        ::listen(listener, 1) != 0 ||
        ::getsockname(listener, reinterpret_cast<sockaddr*>(&address), &length) != 0) {
        ::close(listener); return out;
    }
    out.Peer = ::socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (out.Peer < 0 || ::connect(out.Peer, reinterpret_cast<sockaddr*>(&address), length) != 0) {
        if (out.Peer >= 0) ::close(out.Peer);
        ::close(listener);
        return {};
    }
    out.Relay = ::accept4(listener, nullptr, nullptr, SOCK_CLOEXEC);
    ::close(listener);
    return out;
}

struct Escalation final : TqTerminalEscalation {
    std::atomic<uint64_t> Calls{0};
    void RequestConnectionShutdown(uint64_t, uint64_t, QUIC_STATUS, uint64_t) noexcept override {
        ++Calls;
    }
};

struct RelayFixture {
    GlobalGuard Globals;
    TqLinuxRelayWorker Worker;
    TcpPair Tcp;
    alignas(MsQuicStream) unsigned char Storage[sizeof(MsQuicStream)]{};
    MsQuicStream* Stream{reinterpret_cast<MsQuicStream*>(Storage)};
    std::shared_ptr<TqStreamLifetime> Owner;
    std::shared_ptr<TqRelayStopControl> Control{std::make_shared<TqRelayStopControl>()};
    uint64_t RelayId{0};
    bool Consumed{false};
    bool RealThread{false};

    explicit RelayFixture(
        TqLinuxRelayWorkerConfig config = {}, bool realThread = false,
        std::shared_ptr<TqTerminalEscalation> escalation = nullptr)
        : Worker(config), RealThread(realThread) {
        Control->TerminalEscalation = std::move(escalation);
    }
    bool Start() {
        if (!(RealThread ? Worker.Start() : Worker.StartForTest())) return false;
        Tcp = LoopbackPair();
        if (Tcp.Relay < 0) return false;
        Stream->Handle = reinterpret_cast<HQUIC>(static_cast<uintptr_t>(1));
        Owner = TqStreamLifetime::CreateForTest(TqStreamLifetime::Phase::Started);
        if (!Owner->InstallDetachedStreamForTest(Stream)) return false;
        TqLinuxRelayRegistration registration{};
        registration.TcpFd = Tcp.Relay;
        registration.Stream = Stream;
        registration.StreamOwner = Owner;
        registration.StopControl = Control;
        registration.ControlGeneration = Control->Generation;
        registration.EnableQuicSends = true;
        const auto result = Worker.RegisterRelayWithId(registration);
        Consumed = result.TcpFdConsumed;
        RelayId = result.RelayId;
        if (Consumed) Tcp.Relay = -1;
        return result.Ok && result.TcpFdConsumed;
    }
    ~RelayFixture() {
        Worker.Stop();
        if (Tcp.Relay >= 0) ::close(Tcp.Relay);
        if (Tcp.Peer >= 0) ::close(Tcp.Peer);
        if (Owner != nullptr && Owner->GetTerminalPhase() != TerminalPhase::TerminalObserved) {
            QUIC_STREAM_EVENT terminal{};
            terminal.Type = QUIC_STREAM_EVENT_SHUTDOWN_COMPLETE;
            (void)Owner->DispatchForTest(&terminal);
        }
        Owner.reset();
    }
};

void PublishTerminal(const std::shared_ptr<TqStreamLifetime>& owner) {
    QUIC_STREAM_EVENT terminal{};
    terminal.Type = QUIC_STREAM_EVENT_SHUTDOWN_COMPLETE;
    (void)owner->DispatchForTest(&terminal);
}

bool ResetPeer(TcpPair& pair) {
    linger reset{1, 0};
    if (pair.Peer < 0 ||
        ::setsockopt(pair.Peer, SOL_SOCKET, SO_LINGER, &reset, sizeof(reset)) != 0 ||
        ::close(pair.Peer) != 0) return false;
    pair.Peer = -1;
    return true;
}

bool TestReaperAtAllHandoffBoundaries() {
    GlobalGuard globals;
    TqTunnelReaper::Instance().Stop();
    std::atomic<unsigned> destroys{0};
    TcpPair tcp = LoopbackPair();
    CHECK(tcp.Relay >= 0);
    alignas(MsQuicStream) unsigned char storage[sizeof(MsQuicStream)]{};
    auto* stream = reinterpret_cast<MsQuicStream*>(storage);
    stream->Handle = reinterpret_cast<HQUIC>(static_cast<uintptr_t>(1));
    auto serverOwner = TqStreamLifetime::CreateForTest(TqStreamLifetime::Phase::Started);
    CHECK(serverOwner->InstallDetachedStreamForTest(stream));
    std::shared_ptr<TqRelayStopControl> control;
    auto* context = TqCreateTestLinuxRelayTunnel(
        &destroys, TqTunnelRole::ServerOpen, tcp.Relay, serverOwner, &control,
        nullptr);
    CHECK(context != nullptr && control != nullptr);
    tcp.Relay = -1;
    auto* handle = TqTestLinuxTunnelRelayHandle(context);
    auto committed = TqRelayLinuxCommittedSnapshot(handle);
    CHECK(committed && committed->Control.get() == control.get());
    std::vector<TqStreamLifetime::TerminalBoundaryForTest> seen;
    bool hookOk = true;
    std::mutex boundaryLock;
    std::condition_variable boundaryCv;
    size_t reached = 0;
    size_t released = 0;
    serverOwner->SetTerminalBoundaryHookForTest([&](auto boundary) {
        std::unique_lock<std::mutex> lock(boundaryLock);
        seen.push_back(boundary);
        auto handoff = std::atomic_load(&control->TerminalHandoff);
        if (handoff == nullptr) { hookOk = false; }
        if (handoff != nullptr && boundary != TqStreamLifetime::TerminalBoundaryForTest::AfterSubmit) {
            hookOk = hookOk && !TqTerminalReleaseReady(handoff->Snapshot());
        }
        reached = seen.size();
        boundaryCv.notify_all();
        boundaryCv.wait(lock, [&] { return released >= reached; });
    });
    CHECK(ResetPeer(tcp));
    CHECK(committed->Control->WorkerEndpointAlive.load());
    auto* worker = reinterpret_cast<TqLinuxRelayWorker*>(committed->WorkerIdentity);
    bool dispatched = false;
    std::thread fatal([&] {
        dispatched = worker->DispatchTcpEventsForTest(
            committed->RelayId, EPOLLERR | EPOLLHUP);
    });
    ScopeGuard fatalJoin([&] {
        {
            std::lock_guard<std::mutex> lock(boundaryLock);
            released = 3;
        }
        boundaryCv.notify_all();
        if (fatal.joinable()) fatal.join();
    });
    for (size_t boundary = 1; boundary <= 3; ++boundary) {
        {
            std::unique_lock<std::mutex> lock(boundaryLock);
            boundaryCv.wait(lock, [&] { return reached >= boundary; });
        }
        CHECK(TqTunnelReaper::Instance().ReapReadyForTest() == 0);
        CHECK(destroys == 0);
        {
            std::lock_guard<std::mutex> lock(boundaryLock);
            released = boundary;
        }
        boundaryCv.notify_all();
    }
    fatal.join();
    fatalJoin.Dismiss();
    CHECK(dispatched);
    CHECK(hookOk && destroys == 0);
    CHECK(TqTunnelReaper::Instance().ReapReadyForTest() == 1);
    CHECK(destroys == 1);
    CHECK(seen == (std::vector<TqStreamLifetime::TerminalBoundaryForTest>{
        TqStreamLifetime::TerminalBoundaryForTest::AfterReserve,
        TqStreamLifetime::TerminalBoundaryForTest::InsideShutdownDowncall,
        TqStreamLifetime::TerminalBoundaryForTest::AfterSubmit}));
    PublishTerminal(serverOwner);
    auto clientOwner = TqStreamLifetime::CreateForTest(TqStreamLifetime::Phase::Started);
    PublishTerminal(clientOwner);
    CHECK(serverOwner->GetTerminalPhase() == TerminalPhase::TerminalObserved);
    CHECK(clientOwner->GetTerminalPhase() == TerminalPhase::TerminalObserved);

    // Exact production chain: two real workers, owners and tunnel contexts.
    TqLinuxRelayWorker clientWorker({}), serverWorker({});
    CHECK(clientWorker.Start() && serverWorker.Start());
    TcpPair clientTcp = LoopbackPair(), targetTcp = LoopbackPair();
    CHECK(clientTcp.Relay >= 0 && targetTcp.Relay >= 0);
    auto exactClientOwner = TqStreamLifetime::CreateForTest(TqStreamLifetime::Phase::Started);
    auto exactServerOwner = TqStreamLifetime::CreateForTest(TqStreamLifetime::Phase::Started);
    std::unique_ptr<MsQuicStream> clientStreamHolder(new (std::nothrow) MsQuicStream(
        reinterpret_cast<HQUIC>(static_cast<uintptr_t>(11)), CleanUpManual,
        TqStreamLifetime::Callback, exactClientOwner.get()));
    std::unique_ptr<MsQuicStream> serverStreamHolder(new (std::nothrow) MsQuicStream(
        reinterpret_cast<HQUIC>(static_cast<uintptr_t>(12)), CleanUpManual,
        TqStreamLifetime::Callback, exactServerOwner.get()));
    auto* clientStream = clientStreamHolder.get();
    auto* serverStream = serverStreamHolder.get();
    CHECK(clientStream != nullptr && serverStream != nullptr);
    CHECK(exactClientOwner->InstallStreamForTest(clientStream));
    (void)clientStreamHolder.release();
    CHECK(exactServerOwner->InstallStreamForTest(serverStream));
    (void)serverStreamHolder.release();
    std::atomic<unsigned> clientDestroys{0}, serverDestroys{0};
    std::shared_ptr<TqRelayStopControl> clientControl, serverControl;
    TqTunnelContext* clientContext = nullptr;
    TqTunnelContext* serverContext = nullptr;
    std::shared_ptr<const TqRelayLinuxCommittedState> clientCommitted, serverCommitted;
    std::shared_ptr<FakeTransportHarness> transportLifetime;
    ScopeGuard exactCleanup([&] {
        g_transport.store(nullptr, std::memory_order_release);
        if (exactClientOwner != nullptr &&
            exactClientOwner->GetTerminalPhase() != TerminalPhase::TerminalObserved) {
            PublishTerminal(exactClientOwner);
        }
        if (exactServerOwner != nullptr &&
            exactServerOwner->GetTerminalPhase() != TerminalPhase::TerminalObserved) {
            PublishTerminal(exactServerOwner);
        }
        if (clientCommitted != nullptr) clientWorker.UnregisterRelay(clientCommitted->RelayId);
        if (serverCommitted != nullptr) serverWorker.UnregisterRelay(serverCommitted->RelayId);
        clientWorker.Stop();
        serverWorker.Stop();
        (void)TqTunnelReaper::Instance().ReapReadyForTest();
        if (clientContext != nullptr && clientDestroys == 0) {
            TqTunnelReaper::Instance().Unregister(clientContext);
            TqReapTunnelContext(clientContext);
        }
        if (serverContext != nullptr && serverDestroys == 0) {
            TqTunnelReaper::Instance().Unregister(serverContext);
            TqReapTunnelContext(serverContext);
        }
        exactClientOwner.reset();
        exactServerOwner.reset();
        transportLifetime.reset();
    });
    clientContext = TqCreateTestLinuxRelayTunnel(
        &clientDestroys, TqTunnelRole::ClientOpen, clientTcp.Relay,
        exactClientOwner, &clientControl, nullptr, &clientWorker);
    CHECK(clientContext != nullptr);
    clientTcp.Relay = -1;
    serverContext = TqCreateTestLinuxRelayTunnel(
        &serverDestroys, TqTunnelRole::ServerOpen, targetTcp.Relay,
        exactServerOwner, &serverControl, nullptr, &serverWorker);
    CHECK(serverContext != nullptr);
    targetTcp.Relay = -1;
    clientCommitted = TqRelayLinuxCommittedSnapshot(
        TqTestLinuxTunnelRelayHandle(clientContext));
    serverCommitted = TqRelayLinuxCommittedSnapshot(
        TqTestLinuxTunnelRelayHandle(serverContext));
    CHECK(clientCommitted != nullptr && serverCommitted != nullptr);

    struct ExactTransport final : FakeTransportHarness {
        MsQuicStream* ClientStream;
        HQUIC ServerHandle;
        std::shared_ptr<TqStreamLifetime> ClientOwner;
        TqLinuxRelayWorker* ClientWorker;
        TqLinuxRelayWorker* ServerWorker;
        int ServerTcpFd;
        std::mutex Lock;
        std::condition_variable Cv;
        void* FinKey{nullptr};
        bool ServerAbortPending{false};
        std::vector<std::string> Sequence;
        ExactTransport(
            MsQuicStream* clientStream,
            HQUIC serverHandle,
            std::shared_ptr<TqStreamLifetime> clientOwner,
            TqLinuxRelayWorker* clientWorker,
            TqLinuxRelayWorker* serverWorker,
            int serverTcpFd)
            : ClientStream(clientStream), ServerHandle(serverHandle),
              ClientOwner(std::move(clientOwner)), ClientWorker(clientWorker),
              ServerWorker(serverWorker), ServerTcpFd(serverTcpFd) {}
        void OnSend(MsQuicStream* stream, QUIC_SEND_FLAGS flags, void* key) override {
            if (stream != ClientStream || (flags & QUIC_SEND_FLAG_FIN) == 0) return;
            std::lock_guard<std::mutex> lock(Lock);
            FinKey = key;
            Sequence.push_back("client_fin");
            Cv.notify_all();
        }
        void OnShutdown(HQUIC handle, QUIC_STREAM_SHUTDOWN_FLAGS flags) override {
            if (handle != ServerHandle || (flags & QUIC_STREAM_SHUTDOWN_FLAG_ABORT) == 0) return;
            std::lock_guard<std::mutex> lock(Lock);
            ServerAbortPending = true;
            Sequence.push_back("server_abort");
            Cv.notify_all();
        }
        bool PumpClientFin(std::chrono::steady_clock::time_point deadline) {
            void* key = nullptr;
            {
                std::unique_lock<std::mutex> lock(Lock);
                if (!Cv.wait_until(lock, deadline, [&] { return FinKey != nullptr; })) return false;
                key = FinKey;
                Sequence.push_back("client_fin_complete");
            }
            QUIC_STREAM_EVENT complete{};
            complete.Type = QUIC_STREAM_EVENT_SEND_COMPLETE;
            complete.SEND_COMPLETE.ClientContext = key;
            if (QUIC_FAILED(ClientOwner->DispatchForTest(&complete))) return false;
            const uint8_t empty = 0;
            if (!ServerWorker->EnqueueQuicReceiveForTest(ServerTcpFd, &empty, 0, true)) return false;
            std::lock_guard<std::mutex> lock(Lock);
            Sequence.push_back("server_peer_fin");
            return true;
        }
        bool PumpServerAbort(std::chrono::steady_clock::time_point deadline) {
            {
                std::unique_lock<std::mutex> lock(Lock);
                if (!Cv.wait_until(lock, deadline, [&] { return ServerAbortPending; })) return false;
            }
            QUIC_STREAM_EVENT abort{};
            abort.Type = QUIC_STREAM_EVENT_PEER_RECEIVE_ABORTED;
            abort.PEER_RECEIVE_ABORTED.ErrorCode = TqRelayStreamErrorCancelOnLoss;
            if (QUIC_FAILED(ClientWorker->DispatchStreamEventForTest(ClientStream, &abort))) return false;
            std::lock_guard<std::mutex> lock(Lock);
            Sequence.push_back("client_peer_abort");
            return true;
        }
    };
    auto transport = std::make_shared<ExactTransport>(
        clientStream,
        serverStream->Handle,
        exactClientOwner,
        &clientWorker,
        &serverWorker,
        serverWorker.Snapshot().HotRelayTcpFd);
    transportLifetime = transport;
    CHECK(transport->ServerTcpFd >= 0);
    g_transport.store(transport.get(), std::memory_order_release);

    CHECK(::shutdown(clientTcp.Peer, SHUT_WR) == 0);
    auto clientFinEvent = clientWorker.PostTcpEventsForTestAsync(
        clientCommitted->RelayId, EPOLLRDHUP | EPOLLIN);
    CHECK(clientFinEvent != nullptr);
    const auto clientAdmission = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (clientFinEvent->CurrentPhase.load(std::memory_order_acquire) <
               TqLinuxRelayAsyncTestCompletion::Phase::EnteredProcess &&
           std::chrono::steady_clock::now() < clientAdmission) std::this_thread::yield();
    CHECK(clientFinEvent->CurrentPhase.load(std::memory_order_acquire) >=
          TqLinuxRelayAsyncTestCompletion::Phase::EnteredProcess);
    clientFinEvent->ReleaseProcess();
    CHECK(clientFinEvent->Wait());
    CHECK(transport->PumpClientFin(std::chrono::steady_clock::now() + std::chrono::seconds(2)));
    CHECK(TqSetNonBlocking(targetTcp.Peer));
    const auto eofDeadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    char eofByte{};
    ssize_t eofResult = -1;
    while (std::chrono::steady_clock::now() < eofDeadline) {
        eofResult = ::read(targetTcp.Peer, &eofByte, 1);
        if (eofResult == 0) break;
        if (eofResult < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            std::this_thread::yield();
            continue;
        }
        break;
    }
    CHECK(eofResult == 0);

    auto serverFatal = serverWorker.PostTcpEventsForTestAsync(
        serverCommitted->RelayId, EPOLLERR | EPOLLHUP);
    CHECK(serverFatal != nullptr);
    const auto fatalAdmission = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (serverFatal->CurrentPhase.load(std::memory_order_acquire) <
               TqLinuxRelayAsyncTestCompletion::Phase::EnteredProcess &&
           std::chrono::steady_clock::now() < fatalAdmission) std::this_thread::yield();
    CHECK(serverFatal->CurrentPhase.load(std::memory_order_acquire) >=
          TqLinuxRelayAsyncTestCompletion::Phase::EnteredProcess);
    CHECK(ResetPeer(targetTcp));
    serverFatal->ReleaseProcess();
    CHECK(serverFatal->Wait());
    CHECK(serverWorker.Snapshot().LastTcpWriteErrno == ECONNRESET);
    CHECK((g_lastFlags.load() & QUIC_STREAM_SHUTDOWN_FLAG_ABORT) != 0);
    CHECK((g_lastFlags.load() & QUIC_STREAM_SHUTDOWN_FLAG_IMMEDIATE) != 0);
    CHECK(TqTunnelReaper::Instance().ReapReadyForTest() == 1);
    CHECK(serverDestroys == 1);
    PublishTerminal(exactServerOwner);

    CHECK(transport->PumpServerAbort(std::chrono::steady_clock::now() + std::chrono::seconds(2)));
    CHECK(transport->Sequence == (std::vector<std::string>{
        "client_fin", "client_fin_complete", "server_peer_fin",
        "server_abort", "client_peer_abort"}));
    g_transport.store(nullptr, std::memory_order_release);
    CHECK(TqTestDispatchLinuxOwnerEvent(clientContext, QUIC_STREAM_EVENT_SHUTDOWN_COMPLETE));
    clientWorker.UnregisterRelay(clientCommitted->RelayId);
    CHECK(TqTunnelReaper::Instance().ReapReadyForTest() == 1);
    CHECK(clientDestroys == 1);
    exactClientOwner.reset(); exactServerOwner.reset();
    clientWorker.Stop(); serverWorker.Stop();
    transportLifetime.reset();
    transport.reset();
    if (clientTcp.Peer >= 0) { ::close(clientTcp.Peer); clientTcp.Peer = -1; }
    exactCleanup.Dismiss();
    return true;
}

bool TestTerminalReentersShutdown() {
    RelayFixture f;
    CHECK(f.Start());
    std::atomic<uint64_t> calls{0};
    std::atomic<bool> nestedTerminal{false};
    f.Owner->SetShutdownHookForTest([&](uint64_t, QUIC_STREAM_SHUTDOWN_FLAGS) {
        ++calls;
        PublishTerminal(f.Owner);
        const auto nested = f.Owner->BeginTerminalShutdown(
            112, TqTerminalSink::Create(f.Owner, f.Owner->TerminalLedger()), nullptr);
        nestedTerminal.store(nested.AlreadyTerminal);
        return QUIC_STATUS_SUCCESS;
    });
    CHECK(ResetPeer(f.Tcp));
    CHECK(f.Worker.DispatchTcpEventsForTest(f.RelayId, EPOLLERR | EPOLLHUP));
    CHECK(calls == 1);
    CHECK(nestedTerminal);
    CHECK(f.Owner->GetTerminalPhase() == TerminalPhase::TerminalObserved);
    return true;
}

bool TestTcpAdminWorkerConnectionRace() {
    GlobalGuard globals;
    TqTunnelReaper::Instance().Stop();
    TqLinuxRelayWorker worker({});
    CHECK(worker.Start());
    TcpPair tcp = LoopbackPair();
    CHECK(tcp.Relay >= 0);
    alignas(MsQuicStream) unsigned char storage[sizeof(MsQuicStream)]{};
    auto* stream = reinterpret_cast<MsQuicStream*>(storage);
    stream->Handle = reinterpret_cast<HQUIC>(static_cast<uintptr_t>(21));
    auto owner = TqStreamLifetime::CreateForTest(TqStreamLifetime::Phase::Started);
    CHECK(owner->InstallDetachedStreamForTest(stream));
    std::atomic<unsigned> destroys{0};
    std::shared_ptr<TqRelayStopControl> control;
    TqTunnelContext* context = TqCreateTestLinuxRelayTunnel(
        &destroys, TqTunnelRole::ServerOpen, tcp.Relay, owner, &control, nullptr, &worker);
    std::shared_ptr<const TqRelayLinuxCommittedState> committed;
    ScopeGuard raceCleanup([&] {
        if (owner != nullptr && owner->GetTerminalPhase() != TerminalPhase::TerminalObserved) {
            PublishTerminal(owner);
        }
        if (committed != nullptr) worker.UnregisterRelay(committed->RelayId);
        worker.Stop();
        (void)TqTunnelReaper::Instance().ReapReadyForTest();
        if (context != nullptr && destroys == 0) {
            TqTunnelReaper::Instance().Unregister(context);
            TqReapTunnelContext(context);
        }
        owner.reset();
    });
    CHECK(context != nullptr && control != nullptr);
    tcp.Relay = -1;
    committed = TqRelayLinuxCommittedSnapshot(TqTestLinuxTunnelRelayHandle(context));
    CHECK(committed != nullptr);
    auto fatal = worker.PostTcpEventsForTestAsync(committed->RelayId, EPOLLERR | EPOLLHUP);
    CHECK(fatal != nullptr);
    const auto admissionDeadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (fatal->CurrentPhase.load(std::memory_order_acquire) <
               TqLinuxRelayAsyncTestCompletion::Phase::EnteredProcess &&
           std::chrono::steady_clock::now() < admissionDeadline) std::this_thread::yield();
    if (fatal->CurrentPhase.load(std::memory_order_acquire) <
        TqLinuxRelayAsyncTestCompletion::Phase::EnteredProcess) {
        fatal->ReleaseProcess();
        std::fprintf(stderr, "async fatal admission timeout phase=%u\n",
            static_cast<unsigned>(fatal->CurrentPhase.load(std::memory_order_acquire)));
        return false;
    }
    auto workerStop = worker.PostUnregisterRelayForTestAsync(committed->RelayId);
    CHECK(workerStop != nullptr);
    CHECK(ResetPeer(tcp));
    std::atomic<bool> go{false};
    std::atomic<bool> adminOk{false};
    auto wait = [&] { while (!go.load(std::memory_order_acquire)) std::this_thread::yield(); };
    std::thread tcpRoute([&] { wait(); fatal->ReleaseProcess(); });
    std::thread admin([&] { wait(); adminOk = TqTestLinuxTunnelDrainFromAdmin(context); });
    std::thread workerStopThread([&] { wait(); (void)workerStop->Wait(); });
    std::thread connection([&] { wait(); PublishTerminal(owner); });
    go.store(true, std::memory_order_release);
    tcpRoute.join(); admin.join(); workerStopThread.join(); connection.join();
    CHECK(adminOk);
    CHECK(fatal->Wait());
    CHECK(workerStop->Wait());
    CHECK(TqTunnelReaper::Instance().ReapReadyForTest() == 1);
    CHECK(destroys == 1);
    owner.reset();
    worker.Stop();
    CHECK(g_shutdowns.load() <= 1);
    CHECK(TqTerminalMetricsSnapshot().ExactlyOnceViolation == 0);
    raceCleanup.Dismiss();
    return true;
}

bool TestQueueFullSinkAfterTunnelRelease() {
    TqLinuxRelayWorkerConfig config{};
    config.ForceTerminalQueueFullForTest = true;
    GlobalGuard globals;
    TqTunnelReaper::Instance().Stop();
    TqLinuxRelayWorker worker(config);
    CHECK(worker.Start());
    TcpPair tcp = LoopbackPair();
    CHECK(tcp.Relay >= 0);
    alignas(MsQuicStream) unsigned char storage[sizeof(MsQuicStream)]{};
    auto* stream = reinterpret_cast<MsQuicStream*>(storage);
    stream->Handle = reinterpret_cast<HQUIC>(static_cast<uintptr_t>(3));
    auto owner = TqStreamLifetime::CreateForTest(TqStreamLifetime::Phase::Started);
    CHECK(owner->InstallDetachedStreamForTest(stream));
    std::atomic<unsigned> destroys{0};
    std::shared_ptr<TqRelayStopControl> control;
    TqTunnelContext* context = TqCreateTestLinuxRelayTunnel(
        &destroys, TqTunnelRole::ServerOpen, tcp.Relay, owner, &control, nullptr, &worker);
    std::shared_ptr<const TqRelayLinuxCommittedState> queueCommitted;
    ScopeGuard queueCleanup([&] {
        if (owner != nullptr && owner->GetTerminalPhase() != TerminalPhase::TerminalObserved) {
            PublishTerminal(owner);
        }
        if (queueCommitted != nullptr) worker.UnregisterRelay(queueCommitted->RelayId);
        worker.Stop();
        (void)TqTunnelReaper::Instance().ReapReadyForTest();
        if (context != nullptr && destroys == 0) {
            TqTunnelReaper::Instance().Unregister(context);
            TqReapTunnelContext(context);
        }
        owner.reset();
    });
    CHECK(context != nullptr && control != nullptr);
    tcp.Relay = -1;
    queueCommitted = TqRelayLinuxCommittedSnapshot(TqTestLinuxTunnelRelayHandle(context));
    CHECK(queueCommitted != nullptr);
    CHECK(TqTestDispatchLinuxOwnerEvent(context, QUIC_STREAM_EVENT_SHUTDOWN_COMPLETE));
    for (int i = 0; i != 111; ++i) {
        QUIC_STREAM_EVENT late{};
        late.Type = QUIC_STREAM_EVENT_SHUTDOWN_COMPLETE;
        (void)worker.DispatchStreamEventForTest(stream, &late);
    }
    (void)worker.DrainForTest(256);
    const auto queueFullDeadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (!control->Stop.load(std::memory_order_acquire) &&
           std::chrono::steady_clock::now() < queueFullDeadline) std::this_thread::yield();
    CHECK(control->Stop.load(std::memory_order_acquire));
    size_t reaped = 0;
    while (reaped == 0 && std::chrono::steady_clock::now() < queueFullDeadline) {
        reaped = TqTunnelReaper::Instance().ReapReadyForTest();
        if (reaped == 0) std::this_thread::yield();
    }
    CHECK(reaped == 1);
    CHECK(destroys == 1);
    owner.reset();
    worker.Stop();
    ::close(tcp.Peer); tcp.Peer = -1;
    queueCleanup.Dismiss();
    return true;
}

bool TestLateTerminalFdAndGenerationReuse() {
    RelayFixture old;
    CHECK(old.Start());
    const int oldFd = old.Worker.Snapshot().HotRelayTcpFd;
    const uint64_t oldToken = old.Worker.EncodeEpollRelayForTest(old.RelayId);
    old.Worker.UnregisterRelay(old.RelayId);
    TcpPair replacement = LoopbackPair();
    CHECK(replacement.Relay >= 0);
    CHECK(::dup2(replacement.Relay, oldFd) == oldFd);
    if (replacement.Relay != oldFd) ::close(replacement.Relay);
    replacement.Relay = oldFd;
    TqLinuxRelayRegistration reg{};
    reg.TcpFd = replacement.Relay;
    alignas(MsQuicStream) unsigned char freshStorage[sizeof(MsQuicStream)]{};
    auto* freshStream = reinterpret_cast<MsQuicStream*>(freshStorage);
    freshStream->Handle = reinterpret_cast<HQUIC>(static_cast<uintptr_t>(2));
    auto freshOwner = TqStreamLifetime::CreateForTest(TqStreamLifetime::Phase::Started);
    CHECK(freshOwner->InstallDetachedStreamForTest(freshStream));
    auto freshControl = std::make_shared<TqRelayStopControl>();
    reg.Stream = freshStream;
    reg.StreamOwner = freshOwner;
    reg.StopControl = freshControl;
    reg.ControlGeneration = freshControl->Generation;
    reg.EnableQuicSends = true;
    auto fresh = old.Worker.RegisterRelayWithId(reg);
    CHECK(fresh.Ok && fresh.TcpFdConsumed && fresh.RelayId != old.RelayId);
    replacement.Relay = -1;
    CHECK(old.Worker.DispatchEncodedEpollEventForTest(oldToken, EPOLLERR));
    PublishTerminal(old.Owner);
    CHECK(old.Worker.Snapshot().ActiveRelays == 1);
    CHECK(!freshControl->Stop.load(std::memory_order_acquire));
    CHECK(freshOwner->GetTerminalPhase() != TerminalPhase::TerminalObserved);
    old.Worker.UnregisterRelay(fresh.RelayId);
    PublishTerminal(freshOwner);
    ::close(replacement.Peer);
    return true;
}

bool TestSuppressedTerminalEscalatesOnlyConnection() {
    TqLinuxRelayWorkerConfig config{};
    config.SuppressTerminalCallbackForTest = true;
    auto targetEscalation = std::make_shared<Escalation>();
    auto unrelatedEscalation = std::make_shared<Escalation>();
    RelayFixture f(config, false, targetEscalation);
    CHECK(f.Start());
    f.Owner->SetShutdownHookForTest([&](uint64_t, QUIC_STREAM_SHUTDOWN_FLAGS) {
        QUIC_STREAM_EVENT suppressed{};
        suppressed.Type = QUIC_STREAM_EVENT_SHUTDOWN_COMPLETE;
        (void)f.Worker.DispatchStreamEventForTest(f.Stream, &suppressed);
        return QUIC_STATUS_PENDING;
    });
    CHECK(ResetPeer(f.Tcp));
    CHECK(f.Worker.DispatchTcpEventsForTest(f.RelayId, EPOLLERR | EPOLLHUP));
    CHECK(f.Worker.Snapshot().SuppressedTerminalCallbacksForTest == 1);
    CHECK(f.Owner->GetTerminalPhase() != TerminalPhase::TerminalObserved);
    TqTerminalScheduler::AdvanceForTest(std::chrono::seconds(5));
    CHECK(targetEscalation->Calls == 1 && unrelatedEscalation->Calls == 0 && g_closes == 0);
    CHECK(TqTerminalMetricsSnapshot().TerminalTimeoutPending == 1);
    TqTerminalScheduler::AdvanceForTest(std::chrono::seconds(30));
    CHECK(TqTerminalMetricsSnapshot().TerminalTimeoutPending == 0 &&
          targetEscalation->Calls == 1 && unrelatedEscalation->Calls == 0 && g_closes == 0);
    PublishTerminal(f.Owner);
    CHECK(TqStreamLifetime::SnapshotTerminalRetentions().OwnerCount == 0);
    return true;
}

bool TestDuplicateAbortStopTerminalReaper() {
    GlobalGuard globals;
    TqTunnelReaper::Instance().Stop();
    TqLinuxRelayWorker worker({});
    CHECK(worker.Start());
    TcpPair tcp = LoopbackPair();
    CHECK(tcp.Relay >= 0);
    alignas(MsQuicStream) unsigned char storage[sizeof(MsQuicStream)]{};
    auto* stream = reinterpret_cast<MsQuicStream*>(storage);
    stream->Handle = reinterpret_cast<HQUIC>(static_cast<uintptr_t>(4));
    auto owner = TqStreamLifetime::CreateForTest(TqStreamLifetime::Phase::Started);
    CHECK(owner->InstallDetachedStreamForTest(stream));
    std::atomic<unsigned> destroys{0};
    std::shared_ptr<TqRelayStopControl> control;
    TqTunnelContext* context = TqCreateTestLinuxRelayTunnel(
        &destroys, TqTunnelRole::ServerOpen, tcp.Relay, owner, &control, nullptr, &worker);
    std::shared_ptr<const TqRelayLinuxCommittedState> committed;
    ScopeGuard duplicateCleanup([&] {
        if (owner != nullptr && owner->GetTerminalPhase() != TerminalPhase::TerminalObserved) {
            PublishTerminal(owner);
        }
        if (committed != nullptr) worker.UnregisterRelay(committed->RelayId);
        worker.Stop();
        (void)TqTunnelReaper::Instance().ReapReadyForTest();
        if (context != nullptr && destroys == 0) {
            TqTunnelReaper::Instance().Unregister(context);
            TqReapTunnelContext(context);
        }
        owner.reset();
    });
    CHECK(context != nullptr && control != nullptr);
    tcp.Relay = -1;
    committed = TqRelayLinuxCommittedSnapshot(TqTestLinuxTunnelRelayHandle(context));
    CHECK(committed != nullptr);
    CHECK(ResetPeer(tcp));
    for (int i = 0; i != 4; ++i) {
        (void)owner->RequestShutdown(TqStreamLifetime::ShutdownIntent::AbortBothImmediate, 115);
        (void)control->SignalStop(control->Generation);
        (void)worker.DispatchTcpEventsForTest(committed->RelayId, EPOLLERR);
    }
    CHECK(g_shutdowns == 1);
    (void)TqTestDispatchLinuxOwnerEvent(context, QUIC_STREAM_EVENT_SHUTDOWN_COMPLETE);
    worker.UnregisterRelay(committed->RelayId);
    CHECK(TqTunnelReaper::Instance().ReapReadyForTest() == 1);
    CHECK(TqTunnelReaper::Instance().ReapReadyForTest() == 0);
    CHECK(destroys == 1);
    owner.reset();
    worker.Stop();
    CHECK(TqTerminalMetricsSnapshot().ExactlyOnceViolation == 0);
    duplicateCleanup.Dismiss();
    return true;
}

bool TestGracefulHalfCloseReverseFlow() {
    GlobalGuard globals;
    TqLinuxRelayWorker worker({});
    CHECK(worker.StartForTest());
    TcpPair tcp = LoopbackPair();
    CHECK(tcp.Relay >= 0);
    alignas(MsQuicStream) unsigned char storage[sizeof(MsQuicStream)]{};
    auto* stream = reinterpret_cast<MsQuicStream*>(storage);
    stream->Handle = reinterpret_cast<HQUIC>(static_cast<uintptr_t>(1));
    TqLinuxRelayRegistration registration{};
    registration.TcpFd = tcp.Relay;
    registration.Stream = stream;
    registration.EnableQuicSends = true;
    auto registered = worker.RegisterRelayWithId(registration);
    CHECK(registered.Ok && registered.TcpFdConsumed);
    tcp.Relay = -1;
    const char request[] = "request";
    CHECK(::write(tcp.Peer, request, sizeof(request)) == static_cast<ssize_t>(sizeof(request)));
    CHECK(worker.DispatchTcpEventsForTest(registered.RelayId, EPOLLIN));
    CHECK(::shutdown(tcp.Peer, SHUT_WR) == 0);
    CHECK(worker.DispatchTcpEventsForTest(registered.RelayId, EPOLLRDHUP));
    void* finContext = nullptr;
    { std::lock_guard<std::mutex> guard(g_sendLock);
      CHECK(!g_sendFlags.empty());
      CHECK((g_sendFlags.back() & QUIC_SEND_FLAG_FIN) != 0);
      finContext = g_sendContexts.back(); }
    QUIC_STREAM_EVENT sent{};
    sent.Type = QUIC_STREAM_EVENT_SEND_COMPLETE;
    sent.SEND_COMPLETE.ClientContext = finContext;
    CHECK(QUIC_SUCCEEDED(worker.DispatchStreamEventForTest(stream, &sent)));

    const char response[] = "reverse-payload";
    QUIC_BUFFER buffer{static_cast<uint32_t>(sizeof(response)),
                       reinterpret_cast<uint8_t*>(const_cast<char*>(response))};
    QUIC_STREAM_EVENT receive{};
    receive.Type = QUIC_STREAM_EVENT_RECEIVE;
    receive.RECEIVE.BufferCount = 1;
    receive.RECEIVE.Buffers = &buffer;
    CHECK(worker.DispatchStreamEventForTest(stream, &receive) == QUIC_STATUS_PENDING);
    (void)worker.DrainForTest(128);
    char observed[sizeof(response)]{};
    CHECK(::read(tcp.Peer, observed, sizeof(observed)) == static_cast<ssize_t>(sizeof(observed)));
    CHECK(std::memcmp(observed, response, sizeof(response)) == 0);
    receive.RECEIVE.BufferCount = 0;
    receive.RECEIVE.Buffers = nullptr;
    receive.RECEIVE.Flags = QUIC_RECEIVE_FLAG_FIN;
    CHECK(worker.DispatchStreamEventForTest(stream, &receive) == QUIC_STATUS_PENDING);
    (void)worker.DrainForTest(128);
    char byte{};
    CHECK(::read(tcp.Peer, &byte, 1) == 0);
    CHECK(g_shutdowns == 0);
    CHECK(TqTerminalMetricsSnapshot().WatchdogArmed == 0);
    worker.UnregisterRelay(registered.RelayId);
    worker.Stop();
    ::close(tcp.Peer); tcp.Peer = -1;
    return true;
}

struct TestCase { const char* Name; bool (*Run)(); };
const TestCase cases[] = {
    {"reaper_before_during_after_downcall", TestReaperAtAllHandoffBoundaries},
    {"terminal_reenters_shutdown", TestTerminalReentersShutdown},
    {"four_way_fatal_race", TestTcpAdminWorkerConnectionRace},
    {"queue_full_after_tunnel_release", TestQueueFullSinkAfterTunnelRelease},
    {"late_terminal_fd_and_slot_reuse", TestLateTerminalFdAndGenerationReuse},
    {"watchdog_escalates_without_close", TestSuppressedTerminalEscalatesOnlyConnection},
    {"duplicates_are_exactly_once", TestDuplicateAbortStopTerminalReaper},
    {"half_close_keeps_reverse_flow", TestGracefulHalfCloseReverseFlow},
};

} // namespace

int main() {
    for (const auto& test : cases) {
        if (!test.Run()) return 1;
        const auto metrics = TqTerminalMetricsSnapshot();
        if (metrics.TerminalSinkPending != 0 || metrics.TerminalTimeoutPending != 0 ||
            metrics.ExactlyOnceViolation != 0 ||
            TqGetActiveRelayCount() != 0 ||
            TqStreamLifetime::SnapshotSendCompletions().ActiveCount != 0 ||
            TqStreamLifetime::SnapshotTerminalRetentions().OwnerCount != 0) {
            std::fprintf(stderr, "baseline leak after %s sink=%llu timeout=%llu once=%llu active_relays=%u send=%llu retain=%llu\n",
                test.Name,
                static_cast<unsigned long long>(metrics.TerminalSinkPending),
                static_cast<unsigned long long>(metrics.TerminalTimeoutPending),
                static_cast<unsigned long long>(metrics.ExactlyOnceViolation),
                TqGetActiveRelayCount(),
                static_cast<unsigned long long>(TqStreamLifetime::SnapshotSendCompletions().ActiveCount),
                static_cast<unsigned long long>(TqStreamLifetime::SnapshotTerminalRetentions().OwnerCount));
            return 1;
        }
        std::printf("PASS %s\n", test.Name);
    }
    return 0;
}
