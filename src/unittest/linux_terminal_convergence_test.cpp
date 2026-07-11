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

void QUIC_API FakeSetCallbackHandler(HQUIC, void*, void*) {}
void QUIC_API FakeStreamClose(HQUIC) { ++g_closes; }
QUIC_STATUS QUIC_API FakeStreamShutdown(HQUIC, QUIC_STREAM_SHUTDOWN_FLAGS flags, QUIC_UINT62) {
    ++g_shutdowns;
    g_lastFlags.store(static_cast<uint32_t>(flags));
    return QUIC_STATUS_SUCCESS;
}
void QUIC_API FakeStreamReceiveComplete(HQUIC, uint64_t) {}
QUIC_STATUS QUIC_API FakeStreamReceiveSetEnabled(HQUIC, BOOLEAN) { return QUIC_STATUS_SUCCESS; }
QUIC_STATUS CaptureSend(MsQuicStream*, const QUIC_BUFFER*, uint32_t,
                        QUIC_SEND_FLAGS flags, void* context) {
    std::lock_guard<std::mutex> guard(g_sendLock);
    g_sendContexts.push_back(context);
    g_sendFlags.push_back(flags);
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
        TqTerminalScheduler::SetBeforeExecuteForTest({});
        TqTerminalScheduler::SetAfterEnqueueForTest({});
        TqTerminalScheduler::ResetForTest();
        MsQuic = nullptr;
    }
};

struct TcpPair { int Relay{-1}; int Peer{-1}; };
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

    explicit RelayFixture(TqLinuxRelayWorkerConfig config = {}, bool realThread = false)
        : Worker(config), RealThread(realThread) {}
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
    auto* clientStream = new (std::nothrow) MsQuicStream(
        reinterpret_cast<HQUIC>(static_cast<uintptr_t>(11)), CleanUpManual,
        TqStreamLifetime::Callback, exactClientOwner.get());
    auto* serverStream = new (std::nothrow) MsQuicStream(
        reinterpret_cast<HQUIC>(static_cast<uintptr_t>(12)), CleanUpManual,
        TqStreamLifetime::Callback, exactServerOwner.get());
    CHECK(clientStream != nullptr && serverStream != nullptr);
    CHECK(exactClientOwner->InstallStreamForTest(clientStream));
    CHECK(exactServerOwner->InstallStreamForTest(serverStream));
    std::atomic<unsigned> clientDestroys{0}, serverDestroys{0};
    std::shared_ptr<TqRelayStopControl> clientControl, serverControl;
    auto* clientContext = TqCreateTestLinuxRelayTunnel(
        &clientDestroys, TqTunnelRole::ClientOpen, clientTcp.Relay,
        exactClientOwner, &clientControl, nullptr, &clientWorker);
    auto* serverContext = TqCreateTestLinuxRelayTunnel(
        &serverDestroys, TqTunnelRole::ServerOpen, targetTcp.Relay,
        exactServerOwner, &serverControl, nullptr, &serverWorker);
    CHECK(clientContext != nullptr && serverContext != nullptr);
    clientTcp.Relay = targetTcp.Relay = -1;
    auto clientCommitted = TqRelayLinuxCommittedSnapshot(
        TqTestLinuxTunnelRelayHandle(clientContext));
    auto serverCommitted = TqRelayLinuxCommittedSnapshot(
        TqTestLinuxTunnelRelayHandle(serverContext));
    CHECK(clientCommitted != nullptr && serverCommitted != nullptr);

    size_t sendsBefore = 0;
    { std::lock_guard<std::mutex> lock(g_sendLock); sendsBefore = g_sendFlags.size(); }
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
    const auto finDeadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    void* finContext = nullptr;
    while (std::chrono::steady_clock::now() < finDeadline) {
        std::lock_guard<std::mutex> lock(g_sendLock);
        if (g_sendFlags.size() > sendsBefore &&
            (g_sendFlags.back() & QUIC_SEND_FLAG_FIN) != 0) {
            finContext = g_sendContexts.back();
            break;
        }
        std::this_thread::yield();
    }
    if (finContext == nullptr) {
        const auto clientSnapshot = clientWorker.Snapshot();
        std::lock_guard<std::mutex> lock(g_sendLock);
        std::fprintf(stderr, "FIN missing before=%zu after=%zu active=%llu tcp_read_closed=%d fin_submitted=%d closing=%d\n",
            sendsBefore, g_sendFlags.size(),
            static_cast<unsigned long long>(clientSnapshot.ActiveRelays),
            clientSnapshot.ActiveRelayStates.empty() ? -1 : clientSnapshot.ActiveRelayStates.front().TcpReadClosed,
            clientSnapshot.ActiveRelayStates.empty() ? -1 : clientSnapshot.ActiveRelayStates.front().QuicSendFinSubmitted,
            clientSnapshot.ActiveRelayStates.empty() ? -1 : clientSnapshot.ActiveRelayStates.front().Closing);
        return false;
    }
    QUIC_STREAM_EVENT finComplete{};
    finComplete.Type = QUIC_STREAM_EVENT_SEND_COMPLETE;
    finComplete.SEND_COMPLETE.ClientContext = finContext;
    CHECK(QUIC_SUCCEEDED(exactClientOwner->DispatchForTest(&finComplete)));

    const int serverTcpFd = serverWorker.Snapshot().HotRelayTcpFd;
    CHECK(serverTcpFd >= 0);
    const uint8_t emptyReceive = 0;
    CHECK(serverWorker.EnqueueQuicReceiveForTest(serverTcpFd, &emptyReceive, 0, true));
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

    QUIC_STREAM_EVENT peerAbort{};
    peerAbort.Type = QUIC_STREAM_EVENT_PEER_RECEIVE_ABORTED;
    peerAbort.PEER_RECEIVE_ABORTED.ErrorCode = TqRelayStreamErrorCancelOnLoss;
    (void)clientWorker.DispatchStreamEventForTest(clientStream, &peerAbort);
    CHECK(TqTestDispatchLinuxOwnerEvent(clientContext, QUIC_STREAM_EVENT_SHUTDOWN_COMPLETE));
    clientWorker.UnregisterRelay(clientCommitted->RelayId);
    CHECK(TqTunnelReaper::Instance().ReapReadyForTest() == 1);
    CHECK(clientDestroys == 1);
    exactClientOwner.reset(); exactServerOwner.reset();
    clientWorker.Stop(); serverWorker.Stop();
    if (clientTcp.Peer >= 0) { ::close(clientTcp.Peer); clientTcp.Peer = -1; }
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
    RelayFixture f({}, true);
    CHECK(f.Start());
    auto fatal = f.Worker.PostTcpEventsForTestAsync(f.RelayId, EPOLLERR | EPOLLHUP);
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
    CHECK(ResetPeer(f.Tcp));
    std::atomic<bool> go{false};
    auto wait = [&] { while (!go.load(std::memory_order_acquire)) std::this_thread::yield(); };
    std::thread tcp([&] { wait(); fatal->ReleaseProcess(); });
    std::thread admin([&] { wait(); (void)f.Control->SignalStop(f.Control->Generation); });
    std::thread worker([&] { wait(); (void)f.Control->SignalStop(f.Control->Generation); });
    std::thread connection([&] { wait(); (void)fatal->Wait(); PublishTerminal(f.Owner); });
    go.store(true, std::memory_order_release);
    tcp.join(); admin.join(); worker.join(); connection.join();
    CHECK(fatal->Wait());
    PublishTerminal(f.Owner);
    f.Worker.UnregisterRelay(f.RelayId);
    (void)f.Worker.DrainForTest(128);
    CHECK(f.Worker.Snapshot().ActiveRelays == 0);
    CHECK(g_shutdowns.load() <= 1);
    CHECK(TqTerminalMetricsSnapshot().ExactlyOnceViolation == 0);
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
    auto* context = TqCreateTestLinuxRelayTunnel(
        &destroys, TqTunnelRole::ServerOpen, tcp.Relay, owner, &control, nullptr, &worker);
    CHECK(context != nullptr && control != nullptr);
    tcp.Relay = -1;
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
    RelayFixture f(config);
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
    CHECK(g_closes == 0);
    CHECK(TqTerminalMetricsSnapshot().TerminalTimeoutPending == 1);
    TqTerminalScheduler::AdvanceForTest(std::chrono::seconds(30));
    CHECK(TqTerminalMetricsSnapshot().TerminalTimeoutPending == 0 && g_closes == 0);
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
    auto* context = TqCreateTestLinuxRelayTunnel(
        &destroys, TqTunnelRole::ServerOpen, tcp.Relay, owner, &control, nullptr, &worker);
    CHECK(context != nullptr && control != nullptr);
    tcp.Relay = -1;
    auto committed = TqRelayLinuxCommittedSnapshot(TqTestLinuxTunnelRelayHandle(context));
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
            TqStreamLifetime::SnapshotSendCompletions().ActiveCount != 0 ||
            TqStreamLifetime::SnapshotTerminalRetentions().OwnerCount != 0) {
            std::fprintf(stderr, "baseline leak after %s sink=%llu timeout=%llu once=%llu send=%llu retain=%llu\n",
                test.Name,
                static_cast<unsigned long long>(metrics.TerminalSinkPending),
                static_cast<unsigned long long>(metrics.TerminalTimeoutPending),
                static_cast<unsigned long long>(metrics.ExactlyOnceViolation),
                static_cast<unsigned long long>(TqStreamLifetime::SnapshotSendCompletions().ActiveCount),
                static_cast<unsigned long long>(TqStreamLifetime::SnapshotTerminalRetentions().OwnerCount));
            return 1;
        }
        std::printf("PASS %s\n", test.Name);
    }
    return 0;
}
