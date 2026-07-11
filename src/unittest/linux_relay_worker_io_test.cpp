#include "compress.h"
#include "linux_relay_worker.h"
#include "relay.h"

#include <algorithm>
#include <array>
#include <cassert>
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <fcntl.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <csignal>
#include <cerrno>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <thread>
#include <unistd.h>
#include <vector>

namespace {

std::mutex g_FakeSendMutex;
std::vector<void*> g_FakeSendContexts;
std::shared_ptr<TqStreamLifetime> g_PrecommitOwner;
std::array<uint8_t, 32> g_PrecommitPayload{};
QUIC_STATUS g_PrecommitReceiveStatus = QUIC_STATUS_INTERNAL_ERROR;
bool g_PrecommitSnapshotHidden = false;
bool g_PrecommitPublishTerminal = false;

std::mutex g_RuntimeSnapshotHookMutex;
std::condition_variable g_RuntimeSnapshotHookCv;
bool g_RuntimeSnapshotHookEntered = false;
bool g_RuntimeSnapshotHookRelease = false;

void BlockSecondRuntimeWorkerSnapshot(TqLinuxRelayWorker* worker) {
    if (worker == nullptr || worker->WorkerIndexForTest() != 1) {
        return;
    }
    std::unique_lock<std::mutex> lock(g_RuntimeSnapshotHookMutex);
    g_RuntimeSnapshotHookEntered = true;
    g_RuntimeSnapshotHookCv.notify_all();
    g_RuntimeSnapshotHookCv.wait(lock, []() {
        return g_RuntimeSnapshotHookRelease;
    });
}

void AfterManagedPublishHook(TqLinuxRelayWorker* worker, uint64_t) {
    g_PrecommitSnapshotHidden =
        worker != nullptr && worker->CommittedRelayCountForTest() == 0;
    if (g_PrecommitPublishTerminal) {
        QUIC_STREAM_EVENT terminal{};
        terminal.Type = QUIC_STREAM_EVENT_SHUTDOWN_COMPLETE;
        g_PrecommitReceiveStatus = g_PrecommitOwner->DispatchForTest(&terminal);
        return;
    }
    QUIC_BUFFER buffer{};
    buffer.Buffer = g_PrecommitPayload.data();
    buffer.Length = static_cast<uint32_t>(g_PrecommitPayload.size());
    QUIC_STREAM_EVENT receive{};
    receive.Type = QUIC_STREAM_EVENT_RECEIVE;
    receive.RECEIVE.BufferCount = 1;
    receive.RECEIVE.Buffers = &buffer;
    g_PrecommitReceiveStatus = g_PrecommitOwner->DispatchForTest(&receive);
}

QUIC_STATUS QUIC_API FakeStreamSend(
    HQUIC,
    const QUIC_BUFFER* const,
    uint32_t,
    QUIC_SEND_FLAGS,
    void* clientSendContext) {
    std::lock_guard<std::mutex> guard(g_FakeSendMutex);
    g_FakeSendContexts.push_back(clientSendContext);
    return QUIC_STATUS_SUCCESS;
}

void QUIC_API FakeStreamReceiveComplete(HQUIC, uint64_t) {
}

std::mutex g_FakeStreamShutdownMutex;
uint64_t g_FakeStreamShutdownCalls = 0;
uint64_t g_FakeStreamCloseCalls = 0;

void QUIC_API FakeSetCallbackHandler(HQUIC, void*, void*) {
}

void QUIC_API FakeStreamClose(HQUIC) {
    ++g_FakeStreamCloseCalls;
}

QUIC_STATUS QUIC_API FakeStreamShutdown(HQUIC, QUIC_STREAM_SHUTDOWN_FLAGS, QUIC_UINT62) {
    std::lock_guard<std::mutex> guard(g_FakeStreamShutdownMutex);
    ++g_FakeStreamShutdownCalls;
    return QUIC_STATUS_SUCCESS;
}

void ResetFakeStreamShutdownCalls() {
    std::lock_guard<std::mutex> guard(g_FakeStreamShutdownMutex);
    g_FakeStreamShutdownCalls = 0;
}

uint64_t ReadFakeStreamShutdownCalls() {
    std::lock_guard<std::mutex> guard(g_FakeStreamShutdownMutex);
    return g_FakeStreamShutdownCalls;
}

QUIC_STATUS QUIC_API FakeStreamReceiveSetEnabled(HQUIC, BOOLEAN) {
    return QUIC_STATUS_SUCCESS;
}

void InstallFakeMsQuicForSend(QUIC_API_TABLE& table) {
    std::memset(&table, 0, sizeof(table));
    table.StreamSend = FakeStreamSend;
    table.SetCallbackHandler = FakeSetCallbackHandler;
    table.StreamClose = FakeStreamClose;
    table.StreamShutdown = FakeStreamShutdown;
    table.StreamReceiveComplete = FakeStreamReceiveComplete;
    table.StreamReceiveSetEnabled = FakeStreamReceiveSetEnabled;
    MsQuic = reinterpret_cast<const MsQuicApi*>(&table);
}

std::vector<void*> TakeFakeSendContexts() {
    std::lock_guard<std::mutex> guard(g_FakeSendMutex);
    std::vector<void*> contexts;
    contexts.swap(g_FakeSendContexts);
    return contexts;
}

void CompleteFakeSends(TqLinuxRelayWorker& worker, MsQuicStream* stream) {
    for (void* context : TakeFakeSendContexts()) {
        QUIC_STREAM_EVENT complete{};
        complete.Type = QUIC_STREAM_EVENT_SEND_COMPLETE;
        complete.SEND_COMPLETE.ClientContext = context;
        if (stream != nullptr && stream->Callback != nullptr) {
            (void)stream->Callback(stream, stream->Context, &complete);
        } else {
            (void)worker.DispatchStreamEventForTest(stream, &complete);
        }
    }
}

bool FindActiveRelayState(
    const TqLinuxRelayWorkerSnapshot& snapshot,
    uint64_t relayId,
    TqLinuxRelayActiveSnapshot* out) {
    for (const auto& relay : snapshot.ActiveRelayStates) {
        if (relay.RelayId == relayId) {
            if (out != nullptr) {
                *out = relay;
            }
            return true;
        }
    }
    return false;
}

bool MakeTcpLoopbackPair(int out[2]) {
    out[0] = -1;
    out[1] = -1;
    const int listener = ::socket(AF_INET, SOCK_STREAM, 0);
    if (listener < 0) {
        return false;
    }
    int reuse = 1;
    (void)::setsockopt(listener, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = 0;
    if (::bind(listener, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0 ||
        ::listen(listener, 1) != 0) {
        ::close(listener);
        return false;
    }

    socklen_t addrLen = sizeof(addr);
    if (::getsockname(listener, reinterpret_cast<sockaddr*>(&addr), &addrLen) != 0) {
        ::close(listener);
        return false;
    }

    const int client = ::socket(AF_INET, SOCK_STREAM, 0);
    if (client < 0) {
        ::close(listener);
        return false;
    }
    if (::connect(client, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
        ::close(client);
        ::close(listener);
        return false;
    }

    const int server = ::accept(listener, nullptr, nullptr);
    ::close(listener);
    if (server < 0) {
        ::close(client);
        return false;
    }
    out[0] = server;
    out[1] = client;
    return true;
}

} // namespace

int main() {
    (void)::signal(SIGPIPE, SIG_IGN);

    TqRelayResetQuicReadAheadForTest(1024 * 1024);
    if (TqRelayCurrentQuicReadAheadBytesForTest() != 1024ull * 1024) {
        return 1;
    }
    TqRelayUpdateQuicReadAheadFromNetworkStats(10ull * 1024 * 1024, 200000);
    if (TqRelayCurrentQuicReadAheadBytesForTest() != 4ull * 1024 * 1024) {
        std::fprintf(stderr, "expected 2*BDP read-ahead, got %llu\n",
            static_cast<unsigned long long>(TqRelayCurrentQuicReadAheadBytesForTest()));
        return 1;
    }
    TqRelayUpdateQuicReadAheadFromNetworkStats(1000, 1000);
    if (TqRelayCurrentQuicReadAheadBytesForTest() != 1024ull * 1024) {
        std::fprintf(stderr, "expected read-ahead to stay at initial minimum, got %llu\n",
            static_cast<unsigned long long>(TqRelayCurrentQuicReadAheadBytesForTest()));
        return 1;
    }

    TqLinuxRelayWorkerConfig config{};
    config.EventBudget = 128;
    config.ReadChunkSize = 4096;
    config.ReadBatchBytes = 16 * 1024;
    config.MaxIov = 4;
    config.MaxPendingBufferBytes = 64 * 1024;

    TqLinuxRelayWorker worker(config);
    assert(worker.Start());

    int fds[2]{-1, -1};
    assert(::socketpair(AF_UNIX, SOCK_STREAM, 0, fds) == 0);

    TqLinuxRelayRegistration registration{};
    registration.TcpFd = fds[0];
    registration.Stream = nullptr;
    registration.Handle = nullptr;
    registration.EnableQuicSends = false;
    assert(worker.RegisterRelayForTest(registration));

    const char payload[] = "linux-worker-epoll-read";
    assert(::write(fds[1], payload, sizeof(payload)) == static_cast<ssize_t>(sizeof(payload)));
    assert(worker.WaitForObservedTcpBytesForTest(sizeof(payload), 2000));

    TqLinuxRelayWorkerSnapshot snapshot = worker.Snapshot();
    assert(snapshot.TcpReadBatches >= 1);
    assert(snapshot.TcpReadBytes >= sizeof(payload));

    worker.Stop();
    ::close(fds[1]);

    {
        TqLinuxRelayWorkerConfig config{};
        TqLinuxRelayWorker worker(config);
        if (!worker.StartForTest()) {
            return 1;
        }
        int relayFds[2]{-1, -1};
        if (::socketpair(AF_UNIX, SOCK_STREAM, 0, relayFds) != 0) {
            worker.Stop();
            return 1;
        }

        TqLinuxRelayRegistration registration{};
        registration.TcpFd = relayFds[0];
        registration.Stream = nullptr;
        registration.Handle = nullptr;
        registration.EnableQuicSends = false;
        const TqLinuxRelayRegistrationResult registered = worker.RegisterRelayWithId(registration);
        if (!registered.Ok) {
            worker.Stop();
            ::close(relayFds[0]);
            ::close(relayFds[1]);
            return 1;
        }

        worker.UnregisterRelay(registered.RelayId);
        errno = 0;
        if (::fcntl(relayFds[0], F_GETFD) != -1 || errno != EBADF) {
            std::fprintf(stderr, "expected relay fd to be closed by unregister\n");
            worker.Stop();
            ::close(relayFds[0]);
            ::close(relayFds[1]);
            return 1;
        }
        worker.Stop();
        ::close(relayFds[1]);
    }

    {
        TqLinuxRelayWorkerConfig config{};
        config.EventBudget = 128;
        TqLinuxRelayWorker worker(config);
        if (!worker.StartForTest()) {
            return 3101;
        }

        int relayFds[2]{-1, -1};
        if (::socketpair(AF_UNIX, SOCK_STREAM, 0, relayFds) != 0) {
            worker.Stop();
            return 3102;
        }

        const int wakeFd = worker.WakeFdForTest();
        if (wakeFd <= 0) {
            worker.Stop();
            ::close(relayFds[0]);
            ::close(relayFds[1]);
            return 3103;
        }
        worker.SetNextRelayIdForTest(static_cast<uint64_t>(wakeFd));

        TqLinuxRelayRegistration registration{};
        registration.TcpFd = relayFds[0];
        registration.Stream = nullptr;
        registration.Handle = nullptr;
        registration.EnableQuicSends = false;
        const TqLinuxRelayRegistrationResult registered = worker.RegisterRelayWithId(registration);
        if (!registered.Ok) {
            worker.Stop();
            ::close(relayFds[0]);
            ::close(relayFds[1]);
            return 3104;
        }
        if (registered.RelayId != static_cast<uint64_t>(wakeFd)) {
            worker.Stop();
            ::close(relayFds[1]);
            return 3105;
        }

        const uint64_t encodedRelay = worker.EncodeEpollRelayForTest(registered.RelayId);
        if (worker.IsEpollWakeForTest(encodedRelay)) {
            worker.Stop();
            ::close(relayFds[1]);
            return 3106;
        }

        ::shutdown(relayFds[1], SHUT_RDWR);
        ::close(relayFds[1]);
        relayFds[1] = -1;

        if (!worker.DispatchEncodedEpollEventForTest(
                encodedRelay,
                EPOLLIN | EPOLLRDHUP | EPOLLHUP)) {
            worker.Stop();
            return 3107;
        }

        TqLinuxRelayWorkerSnapshot snapshot = worker.Snapshot();
        bool sawTcpReadClosed = false;
        bool sawTcpReadDisarmed = false;
        for (const auto& relay : snapshot.ActiveRelayStates) {
            if (relay.RelayId == registered.RelayId) {
                sawTcpReadClosed = relay.TcpReadClosed;
                sawTcpReadDisarmed = !relay.TcpReadArmed;
            }
        }
        if (!sawTcpReadClosed || !sawTcpReadDisarmed) {
            std::fprintf(stderr, "expected encoded relay event to close TCP read, relay=%llu\n",
                static_cast<unsigned long long>(registered.RelayId));
            worker.Stop();
            return 3108;
        }

        worker.Stop();
    }

    {
        TqLinuxRelayWorkerConfig config{};
        config.EventQueueCapacity = 1024;
        TqLinuxRelayWorker worker(config);
        if (!worker.Start()) {
            return 3401;
        }

        std::atomic<bool> snapshotStarted{false};
        std::atomic<bool> registerReturned{false};
        std::thread snapshotThread([&]() {
            snapshotStarted.store(true, std::memory_order_release);
            (void)worker.Snapshot();
        });

        while (!snapshotStarted.load(std::memory_order_acquire)) {
            std::this_thread::yield();
        }

        int fds[2]{-1, -1};
        if (::socketpair(AF_UNIX, SOCK_STREAM, 0, fds) != 0) {
            snapshotThread.join();
            worker.Stop();
            return 3403;
        }

        TqLinuxRelayRegistration registration{};
        registration.TcpFd = fds[0];
        registration.SinkQuicReceives = false;
        registration.EnableQuicSends = false;
        registration.Stream = nullptr;
        registration.Handle = nullptr;
        TqLinuxRelayRegistrationResult result = worker.RegisterRelayWithId(registration);
        registerReturned.store(true, std::memory_order_release);

        snapshotThread.join();
        if (!registerReturned.load(std::memory_order_acquire)) {
            worker.Stop();
            ::close(fds[0]);
            ::close(fds[1]);
            return 3402;
        }
        if (result.Ok) {
            worker.UnregisterRelay(result.RelayId);
        } else {
            ::close(fds[0]);
        }
        const TqLinuxRelayWorkerSnapshot snapshot = worker.Snapshot();
        if (snapshot.ControlLockAcquireCount == 0) {
            std::fprintf(stderr, "expected ControlLock acquisitions to be recorded\n");
            worker.Stop();
            ::close(fds[1]);
            return 3404;
        }
        worker.Stop();
        ::close(fds[1]);
    }

    {
        TqLinuxRelayWorkerConfig config{};
        config.EventBudget = 128;
        config.ReadChunkSize = 1024;
        config.ReadBatchBytes = 4096;
        config.MaxIov = 4;
        config.MaxPendingBufferBytes = 64 * 1024;

        TqLinuxRelayWorker worker(config);
        if (!worker.StartForTest()) {
            return 1;
        }

        int fds[2]{-1, -1};
        if (::socketpair(AF_UNIX, SOCK_STREAM, 0, fds) != 0) {
            worker.Stop();
            return 1;
        }

        TqLinuxRelayRegistration registration{};
        registration.TcpFd = fds[0];
        registration.Stream = nullptr;
        registration.Handle = nullptr;
        registration.EnableQuicSends = false;
        assert(worker.RegisterRelayForTest(registration));

        std::vector<uint8_t> payload(3000, 0x5A);
        assert(::write(fds[1], payload.data(), payload.size()) == static_cast<ssize_t>(payload.size()));
        assert(worker.WaitForObservedTcpBytesForTest(payload.size(), 2000));

        TqLinuxRelayWorkerSnapshot snapshot = worker.Snapshot();
        assert(snapshot.TcpReadBytes >= payload.size());
        assert(snapshot.QuicSendOperations == 0);
        assert(snapshot.MaxTcpReadIovUsed >= 2);
        assert(snapshot.PendingBytes == 0);
        assert(snapshot.BufferAcquireCount >= 1);
        assert(snapshot.RelayBufferBytesInUse == 0);

        worker.Stop();
        ::close(fds[1]);
    }

    {
        TqLinuxRelayWorkerConfig config{};
        config.EventBudget = 128;
        config.ReadChunkSize = 1024;
        config.ReadBatchBytes = 4096;
        config.MaxIov = 4;

        TqLinuxRelayWorker worker(config);
        if (!worker.StartForTest()) {
            return 1;
        }
        int fds[2]{-1, -1};
        if (!MakeTcpLoopbackPair(fds)) {
            worker.Stop();
            return 1;
        }

        TqLinuxRelayRegistration registration{};
        registration.TcpFd = fds[0];
        registration.Stream = nullptr;
        registration.Handle = nullptr;
        registration.EnableQuicSends = false;
        const auto registered = worker.RegisterRelayWithId(registration);
        if (!registered.Ok) {
            worker.Stop();
            ::close(fds[1]);
            return 1;
        }

        linger resetLinger{};
        resetLinger.l_onoff = 1;
        resetLinger.l_linger = 0;
        (void)::setsockopt(fds[1], SOL_SOCKET, SO_LINGER, &resetLinger, sizeof(resetLinger));
        ::close(fds[1]);

        if (!worker.DispatchTcpEventsForTest(registered.RelayId, EPOLLIN | EPOLLRDHUP)) {
            worker.Stop();
            return 1;
        }
        const TqLinuxRelayWorkerSnapshot snapshot = worker.Snapshot();
        if (snapshot.TcpReadHardErrors == 0 ||
            snapshot.FatalRelayResets == 0 ||
            snapshot.TcpReadClosedRelays != 0) {
            std::fprintf(stderr,
                "expected tcp reset during read to fatal-reset relay, read_errors=%llu "
                "resets=%llu read_closed=%llu errno=%llu\n",
                static_cast<unsigned long long>(snapshot.TcpReadHardErrors),
                static_cast<unsigned long long>(snapshot.FatalRelayResets),
                static_cast<unsigned long long>(snapshot.TcpReadClosedRelays),
                static_cast<unsigned long long>(snapshot.LastTcpReadErrno));
            worker.Stop();
            return 1;
        }

        worker.Stop();
    }

    {
        TqLinuxRelayWorkerConfig config{};
        config.EventBudget = 128;
        config.ReadChunkSize = 1024;
        config.ReadBatchBytes = 4096;
        config.MaxIov = 4;
        config.MaxPendingBufferBytes = 64 * 1024;

        TqLinuxRelayWorker worker(config);
        if (!worker.StartForTest()) {
            return 1;
        }
        int fds[2]{-1, -1};
        if (!MakeTcpLoopbackPair(fds)) {
            worker.Stop();
            return 1;
        }

        TqRelayHandle handle{};
        TqLinuxRelayRegistration registration{};
        registration.TcpFd = fds[0];
        registration.Stream = nullptr;
        registration.Handle = &handle;
        registration.EnableQuicSends = false;
        const auto registered = worker.RegisterRelayWithId(registration);
        if (!registered.Ok) {
            worker.Stop();
            ::close(fds[1]);
            return 1;
        }

        linger resetLinger{};
        resetLinger.l_onoff = 1;
        resetLinger.l_linger = 0;
        (void)::setsockopt(fds[1], SOL_SOCKET, SO_LINGER, &resetLinger, sizeof(resetLinger));
        ::close(fds[1]);
        std::this_thread::sleep_for(std::chrono::milliseconds(10));

        if (!worker.DispatchTcpEventsForTest(registered.RelayId, EPOLLERR)) {
            worker.Stop();
            return 1;
        }
        const TqLinuxRelayWorkerSnapshot snapshot = worker.Snapshot();
        if (!handle.Stop.load(std::memory_order_acquire) ||
            snapshot.FatalRelayResets == 0 ||
            (snapshot.LastTcpWriteErrno == 0 && snapshot.LastTcpReadErrno == 0)) {
            std::fprintf(stderr,
                "expected EPOLLERR/SO_ERROR to fatal-reset relay, stop=%d resets=%llu write_errno=%llu read_errno=%llu\n",
                handle.Stop.load(std::memory_order_acquire) ? 1 : 0,
                static_cast<unsigned long long>(snapshot.FatalRelayResets),
                static_cast<unsigned long long>(snapshot.LastTcpWriteErrno),
                static_cast<unsigned long long>(snapshot.LastTcpReadErrno));
            worker.Stop();
            return 1;
        }

        worker.Stop();
    }

    {
        TqLinuxRelayWorkerConfig config{};
        config.EventBudget = 128;
        config.ReadChunkSize = 1024;
        config.ReadBatchBytes = 4096;
        config.MaxIov = 4;
        config.MaxPendingBufferBytes = 64 * 1024;

        TqLinuxRelayWorker worker(config);
        if (!worker.StartForTest()) {
            return 1;
        }

        int fds[2]{-1, -1};
        if (::socketpair(AF_UNIX, SOCK_STREAM, 0, fds) != 0) {
            worker.Stop();
            return 1;
        }

        TqLinuxRelayRegistration registration{};
        registration.TcpFd = fds[0];
        registration.Stream = nullptr;
        registration.EnableQuicSends = false;
        const auto registered = worker.RegisterRelayWithId(registration);
        if (!registered.Ok) {
            worker.Stop();
            ::close(fds[1]);
            return 1;
        }

        if (::shutdown(fds[1], SHUT_WR) != 0) {
            worker.Stop();
            ::close(fds[1]);
            return 1;
        }
        worker.DispatchTcpEventsForTest(registered.RelayId, EPOLLRDHUP);

        const TqLinuxRelayWorkerSnapshot snapshot = worker.Snapshot();
        if (snapshot.TcpReadClosedRelays != 1 || snapshot.TcpReadArmedRelays != 0) {
            std::fprintf(stderr,
                "expected EPOLLRDHUP-only event to drain TCP EOF and disarm read, "
                "read_closed=%llu read_armed=%llu\n",
                static_cast<unsigned long long>(snapshot.TcpReadClosedRelays),
                static_cast<unsigned long long>(snapshot.TcpReadArmedRelays));
            worker.Stop();
            ::close(fds[1]);
            return 1;
        }

        if (!worker.ArmTcpReadableForTest(registered.RelayId, true)) {
            worker.Stop();
            ::close(fds[1]);
            return 1;
        }
        const TqLinuxRelayWorkerSnapshot rearmed = worker.Snapshot();
        if (rearmed.TcpReadClosedRelays != 1 || rearmed.TcpReadArmedRelays != 0) {
            std::fprintf(stderr,
                "expected closed TCP read to reject rearm, read_closed=%llu read_armed=%llu\n",
                static_cast<unsigned long long>(rearmed.TcpReadClosedRelays),
                static_cast<unsigned long long>(rearmed.TcpReadArmedRelays));
            worker.Stop();
            ::close(fds[1]);
            return 1;
        }

        worker.Stop();
        ::close(fds[1]);
    }

    {
        TqLinuxRelayWorkerConfig config{};
        config.EventBudget = 128;
        config.ReadChunkSize = 4096;
        config.ReadBatchBytes = 64 * 1024;
        config.MaxIov = 8;
        config.MaxPendingBufferBytes = 256 * 1024;

        TqLinuxRelayWorker worker(config);
        if (!worker.Start()) {
            return 372;
        }

        int fds[2]{-1, -1};
        if (::socketpair(AF_UNIX, SOCK_STREAM, 0, fds) != 0) {
            worker.Stop();
            return 373;
        }

        QUIC_API_TABLE fakeApi{};
        InstallFakeMsQuicForSend(fakeApi);

        alignas(MsQuicStream) uint8_t fakeStreamStorage[sizeof(MsQuicStream)]{};
        std::memset(fakeStreamStorage, 0, sizeof(fakeStreamStorage));
        MsQuicStream* fakeStream = reinterpret_cast<MsQuicStream*>(fakeStreamStorage);
        fakeStream->Handle = reinterpret_cast<HQUIC>(static_cast<uintptr_t>(1));

        TqLinuxRelayRegistration registration{};
        registration.TcpFd = fds[0];
        registration.Stream = fakeStream;
        registration.EnableQuicSends = false;
        if (!worker.RegisterRelayForTest(registration)) {
            worker.Stop();
            ::close(fds[1]);
            return 374;
        }

        QUIC_STREAM_EVENT fakeFin{};
        fakeFin.Type = QUIC_STREAM_EVENT_RECEIVE;
        fakeFin.RECEIVE.AbsoluteOffset = UINT64_MAX;
        fakeFin.RECEIVE.TotalBufferLength = 0;
        fakeFin.RECEIVE.BufferCount = 0;
        fakeFin.RECEIVE.Buffers = nullptr;
        fakeFin.RECEIVE.Flags = QUIC_RECEIVE_FLAG_FIN;

        const QUIC_STATUS status = worker.DispatchStreamEventForTest(fakeStream, &fakeFin);
        if (status != QUIC_STATUS_SUCCESS) {
            std::fprintf(stderr, "expected fake FIN to return SUCCESS, got %d\n", status);
            worker.Stop();
            ::close(fds[1]);
            return 375;
        }

        const TqLinuxRelayWorkerSnapshot snapshot = worker.Snapshot();
        if (snapshot.FakeFinReceiveCount != 1) {
            std::fprintf(stderr, "expected fake FIN count 1, got %llu\n",
                static_cast<unsigned long long>(snapshot.FakeFinReceiveCount));
            worker.Stop();
            ::close(fds[1]);
            return 376;
        }
        if (snapshot.FatalRelayResets != 1) {
            std::fprintf(stderr, "expected fatal relay resets 1, got %llu\n",
                static_cast<unsigned long long>(snapshot.FatalRelayResets));
            worker.Stop();
            ::close(fds[1]);
            return 377;
        }
        if (snapshot.DeferredReceiveCompletes != 0) {
            std::fprintf(stderr, "fake FIN must not complete deferred receive buffers\n");
            worker.Stop();
            ::close(fds[1]);
            return 378;
        }
        if (snapshot.ClosingRelays == 0) {
            std::fprintf(stderr, "expected fake FIN relay to be closing\n");
            worker.Stop();
            ::close(fds[1]);
            return 379;
        }

        worker.Stop();
        ::close(fds[1]);
    }

    {
        QUIC_API_TABLE fakeApi{};
        InstallFakeMsQuicForSend(fakeApi);

        TqLinuxRelayWorkerConfig config{};
        config.EventBudget = 128;
        config.ReadChunkSize = 1024;
        config.ReadBatchBytes = 8192;
        config.MaxIov = 4;
        config.MaxPendingBufferBytes = 64 * 1024;
        config.MaxBufferedQuicSendBytes = 8192;
        config.MaxInFlightQuicSends = 1;

        TqLinuxRelayWorker worker(config);
        if (!worker.Start()) {
            MsQuic = nullptr;
            return 1;
        }

        int fds[2]{-1, -1};
        if (::socketpair(AF_UNIX, SOCK_STREAM, 0, fds) != 0) {
            worker.Stop();
            MsQuic = nullptr;
            return 1;
        }

        alignas(MsQuicStream) uint8_t fakeStreamStorage[sizeof(MsQuicStream)]{};
        std::memset(fakeStreamStorage, 0, sizeof(fakeStreamStorage));
        MsQuicStream* fakeStream = reinterpret_cast<MsQuicStream*>(fakeStreamStorage);
        fakeStream->Handle = reinterpret_cast<HQUIC>(static_cast<uintptr_t>(1));

        TqLinuxRelayRegistration registration{};
        registration.TcpFd = fds[0];
        registration.Stream = fakeStream;
        registration.EnableQuicSends = true;
        if (!worker.RegisterRelayForTest(registration)) {
            worker.Stop();
            ::close(fds[1]);
            MsQuic = nullptr;
            return 1;
        }

        QUIC_STREAM_EVENT idealEvent{};
        idealEvent.Type = QUIC_STREAM_EVENT_IDEAL_SEND_BUFFER_SIZE;
        idealEvent.IDEAL_SEND_BUFFER_SIZE.ByteCount = 8192;
        if (worker.DispatchStreamEventForTest(fakeStream, &idealEvent) != QUIC_STATUS_SUCCESS ||
            worker.DrainForTest(config.EventBudget) != 1) {
            std::fprintf(stderr, "expected ideal-send event before max-inflight bypass test\n");
            worker.Stop();
            ::close(fds[1]);
            MsQuic = nullptr;
            return 1;
        }

        std::vector<uint8_t> payload(8192, 0x5a);
        if (::write(fds[1], payload.data(), payload.size()) !=
            static_cast<ssize_t>(payload.size())) {
            worker.Stop();
            ::close(fds[1]);
            MsQuic = nullptr;
            return 1;
        }
        if (!worker.WaitForObservedTcpBytesForTest(payload.size(), 2000)) {
            const auto snapshot = worker.Snapshot();
            std::fprintf(stderr,
                "expected TCP read to ignore MaxInFlightQuicSends and fill ideal bytes, read=%llu sends=%llu outstanding_bytes=%llu threshold=%llu disabled=%llu\n",
                static_cast<unsigned long long>(snapshot.TcpReadBytes),
                static_cast<unsigned long long>(snapshot.OutstandingQuicSends),
                static_cast<unsigned long long>(snapshot.OutstandingQuicSendBytes),
                static_cast<unsigned long long>(snapshot.MaxBufferedQuicSendBytes),
                static_cast<unsigned long long>(snapshot.TcpReadDisabledRelays));
            worker.Stop();
            ::close(fds[1]);
            MsQuic = nullptr;
            return 1;
        }

        TqLinuxRelayWorkerSnapshot paused = worker.Snapshot();
        if (paused.OutstandingQuicSends < 2 ||
            paused.OutstandingQuicSendBytes != payload.size() ||
            paused.MaxBufferedQuicSendBytes != payload.size()) {
            std::fprintf(stderr,
                "expected ideal-send bytes to be the only TCP read cap, sends=%llu outstanding_bytes=%llu threshold=%llu\n",
                static_cast<unsigned long long>(paused.OutstandingQuicSends),
                static_cast<unsigned long long>(paused.OutstandingQuicSendBytes),
                static_cast<unsigned long long>(paused.MaxBufferedQuicSendBytes));
            worker.Stop();
            ::close(fds[1]);
            MsQuic = nullptr;
            return 1;
        }

        CompleteFakeSends(worker, fakeStream);
        worker.Stop();
        ::close(fds[1]);
        MsQuic = nullptr;
    }

    {
        QUIC_API_TABLE fakeApi{};
        InstallFakeMsQuicForSend(fakeApi);

        TqLinuxRelayWorkerConfig config{};
        config.EventBudget = 128;
        config.ReadChunkSize = 1024;
        config.ReadBatchBytes = 4096;
        config.MaxIov = 4;
        config.MaxPendingBufferBytes = 64 * 1024;
        config.MaxBufferedQuicSendBytes = 2048;

        TqLinuxRelayWorker worker(config);
        if (!worker.Start()) {
            MsQuic = nullptr;
            return 1;
        }

        int fds[2]{-1, -1};
        if (::socketpair(AF_UNIX, SOCK_STREAM, 0, fds) != 0) {
            worker.Stop();
            MsQuic = nullptr;
            return 1;
        }

        alignas(MsQuicStream) uint8_t fakeStreamStorage[sizeof(MsQuicStream)]{};
        std::memset(fakeStreamStorage, 0, sizeof(fakeStreamStorage));
        MsQuicStream* fakeStream = reinterpret_cast<MsQuicStream*>(fakeStreamStorage);
        fakeStream->Handle = reinterpret_cast<HQUIC>(static_cast<uintptr_t>(1));

        TqLinuxRelayRegistration registration{};
        registration.TcpFd = fds[0];
        registration.Stream = fakeStream;
        registration.EnableQuicSends = true;
        if (!worker.RegisterRelayForTest(registration)) {
            worker.Stop();
            ::close(fds[1]);
            MsQuic = nullptr;
            return 1;
        }

        QUIC_STREAM_EVENT idealEvent{};
        idealEvent.Type = QUIC_STREAM_EVENT_IDEAL_SEND_BUFFER_SIZE;
        idealEvent.IDEAL_SEND_BUFFER_SIZE.ByteCount = 4096;
        if (worker.DispatchStreamEventForTest(fakeStream, &idealEvent) != QUIC_STATUS_SUCCESS ||
            worker.DrainForTest(config.EventBudget) != 1) {
            std::fprintf(stderr, "expected ideal-send event to enqueue and drain\n");
            worker.Stop();
            ::close(fds[1]);
            MsQuic = nullptr;
            return 1;
        }

        std::vector<uint8_t> payload(8192, 0x49);
        if (::write(fds[1], payload.data(), payload.size()) !=
            static_cast<ssize_t>(payload.size())) {
            worker.Stop();
            ::close(fds[1]);
            MsQuic = nullptr;
            return 1;
        }
        if (!worker.WaitForObservedTcpBytesForTest(4096, 2000)) {
            std::fprintf(stderr, "expected ideal-send threshold to allow 4096 bytes before pause\n");
            worker.Stop();
            ::close(fds[1]);
            MsQuic = nullptr;
            return 1;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        TqLinuxRelayWorkerSnapshot paused = worker.Snapshot();
        if (paused.TcpReadBytes != 4096 ||
            paused.TcpReadDisabledRelays != 1 ||
            paused.OutstandingQuicSendBytes != 4096 ||
            paused.MaxBufferedQuicSendBytes != 4096) {
            std::fprintf(stderr,
                "expected ideal-send ByteCount to drive pause threshold, read=%llu disabled=%llu outstanding=%llu threshold=%llu\n",
                static_cast<unsigned long long>(paused.TcpReadBytes),
                static_cast<unsigned long long>(paused.TcpReadDisabledRelays),
                static_cast<unsigned long long>(paused.OutstandingQuicSendBytes),
                static_cast<unsigned long long>(paused.MaxBufferedQuicSendBytes));
            worker.Stop();
            ::close(fds[1]);
            MsQuic = nullptr;
            return 1;
        }

        CompleteFakeSends(worker, fakeStream);
        if (!worker.WaitForObservedTcpBytesForTest(payload.size(), 2000)) {
            std::fprintf(stderr, "expected TCP read to resume when outstanding falls below ideal-send threshold\n");
            worker.Stop();
            ::close(fds[1]);
            MsQuic = nullptr;
            return 1;
        }
        CompleteFakeSends(worker, fakeStream);

        worker.Stop();
        ::close(fds[1]);
        MsQuic = nullptr;
    }

    {
        QUIC_API_TABLE fakeApi{};
        InstallFakeMsQuicForSend(fakeApi);

        TqLinuxRelayWorkerConfig config{};
        config.EventBudget = 128;
        config.ReadChunkSize = 1024;
        config.ReadBatchBytes = 4096;
        config.MaxIov = 3;
        config.MaxPendingBufferBytes = 2048;

        TqLinuxRelayWorker worker(config);
        if (!worker.Start()) {
            MsQuic = nullptr;
            return 1;
        }

        int fds[2]{-1, -1};
        if (::socketpair(AF_UNIX, SOCK_STREAM, 0, fds) != 0) {
            worker.Stop();
            MsQuic = nullptr;
            return 1;
        }

        alignas(MsQuicStream) uint8_t fakeStreamStorage[sizeof(MsQuicStream)]{};
        std::memset(fakeStreamStorage, 0, sizeof(fakeStreamStorage));
        MsQuicStream* fakeStream = reinterpret_cast<MsQuicStream*>(fakeStreamStorage);
        fakeStream->Handle = reinterpret_cast<HQUIC>(static_cast<uintptr_t>(1));

        TqLinuxRelayRegistration registration{};
        registration.TcpFd = fds[0];
        registration.Stream = fakeStream;
        registration.EnableQuicSends = true;
        errno = 0;
        if (!worker.RegisterRelayForTest(registration)) {
            std::fprintf(stderr, "slot-pause register failed errno=%d fd=%d\n", errno, fds[0]);
            worker.Stop();
            ::close(fds[1]);
            MsQuic = nullptr;
            return 1;
        }

        std::vector<uint8_t> payload(4096, 0x71);
        if (::write(fds[1], payload.data(), payload.size()) !=
            static_cast<ssize_t>(payload.size())) {
            std::fprintf(stderr, "slot-pause write failed errno=%d\n", errno);
            worker.Stop();
            ::close(fds[1]);
            MsQuic = nullptr;
            return 1;
        }
        if (!worker.WaitForObservedTcpBytesForTest(2048, 2000)) {
            std::fprintf(stderr, "expected first TCP read before slot pause\n");
            worker.Stop();
            ::close(fds[1]);
            MsQuic = nullptr;
            return 1;
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        TqLinuxRelayWorkerSnapshot snapshot = worker.Snapshot();
        if (snapshot.ReadDisabledCount == 0) {
            std::fprintf(stderr, "pending budget exhausted should pause TCP read, disabled=%llu read=%llu iov=%llu pending=%llu sends=%llu\n",
                static_cast<unsigned long long>(snapshot.ReadDisabledCount),
                static_cast<unsigned long long>(snapshot.TcpReadBytes),
                static_cast<unsigned long long>(snapshot.MaxTcpReadIovUsed),
                static_cast<unsigned long long>(snapshot.PendingBytes),
                static_cast<unsigned long long>(snapshot.QuicSendOperations));
            worker.Stop();
            ::close(fds[1]);
            MsQuic = nullptr;
            return 1;
        }

        CompleteFakeSends(worker, fakeStream);
        if (!worker.WaitForObservedTcpBytesForTest(payload.size(), 2000)) {
            std::fprintf(stderr, "expected TCP read to resume after send complete\n");
            worker.Stop();
            ::close(fds[1]);
            MsQuic = nullptr;
            return 1;
        }
        CompleteFakeSends(worker, fakeStream);

        worker.Stop();
        ::close(fds[1]);
        MsQuic = nullptr;
    }

    {
        QUIC_API_TABLE fakeApi{};
        InstallFakeMsQuicForSend(fakeApi);

        TqLinuxRelayWorkerConfig config{};
        config.EventBudget = 128;
        config.ReadChunkSize = 1024;
        config.ReadBatchBytes = 2048;
        config.MaxIov = 2;
        config.MaxPendingBufferBytes = 64 * 1024;
        config.MaxBufferedQuicSendBytes = 2048;

        TqLinuxRelayWorker worker(config);
        if (!worker.Start()) {
            MsQuic = nullptr;
            return 1;
        }

        int fds[2]{-1, -1};
        if (::socketpair(AF_UNIX, SOCK_STREAM, 0, fds) != 0) {
            worker.Stop();
            MsQuic = nullptr;
            return 1;
        }

        alignas(MsQuicStream) uint8_t fakeStreamStorage[sizeof(MsQuicStream)]{};
        std::memset(fakeStreamStorage, 0, sizeof(fakeStreamStorage));
        MsQuicStream* fakeStream = reinterpret_cast<MsQuicStream*>(fakeStreamStorage);
        fakeStream->Handle = reinterpret_cast<HQUIC>(static_cast<uintptr_t>(1));

        TqLinuxRelayRegistration registration{};
        registration.TcpFd = fds[0];
        registration.Stream = fakeStream;
        registration.EnableQuicSends = true;
        if (!worker.RegisterRelayForTest(registration)) {
            worker.Stop();
            ::close(fds[1]);
            MsQuic = nullptr;
            return 1;
        }

        QUIC_STREAM_EVENT idealEvent{};
        idealEvent.Type = QUIC_STREAM_EVENT_IDEAL_SEND_BUFFER_SIZE;
        idealEvent.IDEAL_SEND_BUFFER_SIZE.ByteCount = 2048;
        if (worker.DispatchStreamEventForTest(fakeStream, &idealEvent) != QUIC_STATUS_SUCCESS ||
            worker.DrainForTest(config.EventBudget) != 1) {
            std::fprintf(stderr, "expected ideal-send event to set buffered byte cap\n");
            worker.Stop();
            ::close(fds[1]);
            MsQuic = nullptr;
            return 1;
        }

        std::vector<uint8_t> payload(4096, 0x31);
        if (::write(fds[1], payload.data(), payload.size()) !=
            static_cast<ssize_t>(payload.size())) {
            worker.Stop();
            ::close(fds[1]);
            MsQuic = nullptr;
            return 1;
        }
        if (!worker.WaitForObservedTcpBytesForTest(2048, 2000)) {
            std::fprintf(stderr, "expected first read before in-flight pause\n");
            worker.Stop();
            ::close(fds[1]);
            MsQuic = nullptr;
            return 1;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        TqLinuxRelayWorkerSnapshot paused = worker.Snapshot();
        if (paused.TcpReadBytes != 2048 ||
            paused.TcpReadDisabledRelays != 1 ||
            paused.OutstandingQuicSendBytes != 2048) {
            std::fprintf(stderr,
                "expected buffered byte cap to pause TCP read, read=%llu disabled=%llu outstanding_bytes=%llu\n",
                static_cast<unsigned long long>(paused.TcpReadBytes),
                static_cast<unsigned long long>(paused.TcpReadDisabledRelays),
                static_cast<unsigned long long>(paused.OutstandingQuicSendBytes));
            worker.Stop();
            ::close(fds[1]);
            MsQuic = nullptr;
            return 1;
        }

        CompleteFakeSends(worker, fakeStream);
        if (!worker.WaitForObservedTcpBytesForTest(payload.size(), 2000)) {
            std::fprintf(stderr, "expected TCP read to resume after in-flight send complete\n");
            worker.Stop();
            ::close(fds[1]);
            MsQuic = nullptr;
            return 1;
        }
        CompleteFakeSends(worker, fakeStream);

        worker.Stop();
        ::close(fds[1]);
        MsQuic = nullptr;
    }

    {
        QUIC_API_TABLE fakeApi{};
        InstallFakeMsQuicForSend(fakeApi);

        TqLinuxRelayWorkerConfig config{};
        config.EventBudget = 128;
        config.ReadChunkSize = 4096;
        config.ReadBatchBytes = 64 * 1024;
        config.MaxIov = 8;
        config.MaxPendingBufferBytes = 256 * 1024;

        TqLinuxRelayWorker worker(config);
        if (!worker.StartForTest()) {
            MsQuic = nullptr;
            return 1;
        }

        int fds[2]{-1, -1};
        if (::socketpair(AF_UNIX, SOCK_STREAM, 0, fds) != 0) {
            worker.Stop();
            MsQuic = nullptr;
            return 1;
        }
        int sendBuffer = 4096;
        (void)::setsockopt(fds[0], SOL_SOCKET, SO_SNDBUF, &sendBuffer, sizeof(sendBuffer));
        const int peerFlags = ::fcntl(fds[1], F_GETFL, 0);
        (void)::fcntl(fds[1], F_SETFL, peerFlags | O_NONBLOCK);

        alignas(MsQuicStream) uint8_t fakeStreamStorage[sizeof(MsQuicStream)]{};
        std::memset(fakeStreamStorage, 0, sizeof(fakeStreamStorage));
        MsQuicStream* fakeStream = reinterpret_cast<MsQuicStream*>(fakeStreamStorage);
        fakeStream->Handle = reinterpret_cast<HQUIC>(static_cast<uintptr_t>(1));
        TqRelayHandle handle{};

        TqLinuxRelayRegistration registration{};
        registration.TcpFd = fds[0];
        registration.Stream = fakeStream;
        registration.Handle = &handle;
        registration.EnableQuicSends = true;
        const auto registered = worker.RegisterRelayWithId(registration);
        if (!registered.Ok) {
            worker.Stop();
            ::close(fds[1]);
            MsQuic = nullptr;
            return 1;
        }

        if (::shutdown(fds[1], SHUT_WR) != 0 ||
            !worker.DispatchTcpEventsForTest(registered.RelayId, EPOLLRDHUP)) {
            worker.Stop();
            ::close(fds[1]);
            MsQuic = nullptr;
            return 1;
        }
        TqLinuxRelayWorkerSnapshot readClosed = worker.Snapshot();
        if (readClosed.TcpReadClosedRelays != 1 || readClosed.OutstandingQuicSends != 1) {
            std::fprintf(stderr,
                "expected TCP read FIN to submit one QUIC FIN send, read_closed=%llu sends=%llu\n",
                static_cast<unsigned long long>(readClosed.TcpReadClosedRelays),
                static_cast<unsigned long long>(readClosed.OutstandingQuicSends));
            worker.Stop();
            ::close(fds[1]);
            MsQuic = nullptr;
            return 1;
        }
        CompleteFakeSends(worker, fakeStream);
        (void)worker.DrainForTest(config.EventBudget);

        const std::vector<uint8_t> plain(2 * 1024 * 1024, 0x72);
        QUIC_BUFFER quicBuffer{};
        quicBuffer.Buffer = const_cast<uint8_t*>(plain.data());
        quicBuffer.Length = static_cast<uint32_t>(plain.size());

        QUIC_STREAM_EVENT receiveEvent{};
        receiveEvent.Type = QUIC_STREAM_EVENT_RECEIVE;
        receiveEvent.RECEIVE.BufferCount = 1;
        receiveEvent.RECEIVE.Buffers = &quicBuffer;
        receiveEvent.RECEIVE.Flags = QUIC_RECEIVE_FLAG_FIN;

        if (worker.DispatchStreamEventForTest(fakeStream, &receiveEvent) != QUIC_STATUS_PENDING ||
            worker.DrainForTest(config.EventBudget) != 1) {
            worker.Stop();
            ::close(fds[1]);
            MsQuic = nullptr;
            return 1;
        }

        const TqLinuxRelayWorkerSnapshot beforeShutdown = worker.Snapshot();
        if (beforeShutdown.PendingBytes == 0 || handle.Stop.load(std::memory_order_acquire)) {
            std::fprintf(stderr,
                "expected pending TCP writes before shutdown, pending=%llu stop=%d\n",
                static_cast<unsigned long long>(beforeShutdown.PendingBytes),
                handle.Stop.load(std::memory_order_acquire) ? 1 : 0);
            worker.Stop();
            ::close(fds[1]);
            MsQuic = nullptr;
            return 1;
        }

        QUIC_STREAM_EVENT shutdownEvent{};
        shutdownEvent.Type = QUIC_STREAM_EVENT_SHUTDOWN_COMPLETE;
        if (QUIC_FAILED(worker.DispatchStreamEventForTest(fakeStream, &shutdownEvent))) {
            worker.Stop();
            ::close(fds[1]);
            MsQuic = nullptr;
            return 1;
        }
        (void)worker.DrainForTest(config.EventBudget);

        const TqLinuxRelayWorkerSnapshot completed = worker.Snapshot();
        if (!handle.Stop.load(std::memory_order_acquire) ||
            completed.PendingBytes != 0 ||
            completed.FatalRelayResets == 0) {
            std::fprintf(stderr,
                "expected deferred stream shutdown with pending data to reset relay, pending=%llu stop=%d active=%llu resets=%llu\n",
                static_cast<unsigned long long>(completed.PendingBytes),
                handle.Stop.load(std::memory_order_acquire) ? 1 : 0,
                static_cast<unsigned long long>(completed.ActiveRelays),
                static_cast<unsigned long long>(completed.FatalRelayResets));
            worker.Stop();
            ::close(fds[1]);
            MsQuic = nullptr;
            return 1;
        }

        worker.Stop();
        ::close(fds[1]);
        MsQuic = nullptr;
    }

    {
        QUIC_API_TABLE fakeApi{};
        InstallFakeMsQuicForSend(fakeApi);

        TqLinuxRelayWorkerConfig config{};
        config.EventBudget = 128;
        config.ReadChunkSize = 4096;
        config.ReadBatchBytes = 64 * 1024;
        config.MaxIov = 8;
        config.MaxPendingBufferBytes = 256 * 1024;
        config.MaxPendingQuicReceiveBytesPerRelay = 32 * 1024;
        config.TcpWriteMaxBytes = 20 * 1024;
        config.TcpWriteBurstBytes = 20 * 1024;

        TqLinuxRelayWorker worker(config);
        if (!worker.StartForTest()) {
            MsQuic = nullptr;
            return 1;
        }

        int fds[2]{-1, -1};
        if (::socketpair(AF_UNIX, SOCK_STREAM, 0, fds) != 0) {
            worker.Stop();
            MsQuic = nullptr;
            return 1;
        }

        alignas(MsQuicStream) uint8_t fakeStreamStorage[sizeof(MsQuicStream)]{};
        MsQuicStream* fakeStream = reinterpret_cast<MsQuicStream*>(fakeStreamStorage);

        TqLinuxRelayRegistration registration{};
        registration.TcpFd = fds[0];
        registration.Stream = fakeStream;
        registration.EnableQuicSends = false;
        const auto registered = worker.RegisterRelayWithId(registration);
        if (!registered.Ok) {
            worker.Stop();
            ::close(fds[1]);
            MsQuic = nullptr;
            return 1;
        }

        const std::vector<uint8_t> plain(48 * 1024, 0x52);
        QUIC_BUFFER quicBuffer{};
        quicBuffer.Buffer = const_cast<uint8_t*>(plain.data());
        quicBuffer.Length = static_cast<uint32_t>(plain.size());

        QUIC_STREAM_EVENT receiveEvent{};
        receiveEvent.Type = QUIC_STREAM_EVENT_RECEIVE;
        receiveEvent.RECEIVE.BufferCount = 1;
        receiveEvent.RECEIVE.Buffers = &quicBuffer;

        if (worker.DispatchStreamEventForTest(fakeStream, &receiveEvent) != QUIC_STATUS_PENDING ||
            worker.DrainForTest(config.EventBudget) != 1) {
            worker.Stop();
            ::close(fds[1]);
            MsQuic = nullptr;
            return 1;
        }

        const TqLinuxRelayWorkerSnapshot partial = worker.Snapshot();
        if (partial.CurrentPendingQuicReceiveBytes >= config.MaxPendingQuicReceiveBytesPerRelay ||
            partial.CurrentPendingQuicReceiveBytes <= config.MaxPendingQuicReceiveBytesPerRelay / 2 ||
            partial.QuicReceivePausedCount != 1 ||
            partial.QuicReceiveResumedCount != 1) {
            std::fprintf(stderr,
                "expected receive resume below relay_pending, pending=%llu max=%llu pauses=%llu resumes=%llu\n",
                static_cast<unsigned long long>(partial.CurrentPendingQuicReceiveBytes),
                static_cast<unsigned long long>(config.MaxPendingQuicReceiveBytesPerRelay),
                static_cast<unsigned long long>(partial.QuicReceivePausedCount),
                static_cast<unsigned long long>(partial.QuicReceiveResumedCount));
            worker.Stop();
            ::close(fds[1]);
            MsQuic = nullptr;
            return 1;
        }

        worker.Stop();
        ::close(fds[1]);
        MsQuic = nullptr;
    }

    {
        TqLinuxRelayWorkerConfig config{};
        config.EventBudget = 128;
        config.ReadChunkSize = 4096;
        config.ReadBatchBytes = 64 * 1024;
        config.MaxIov = 8;
        config.MaxPendingBufferBytes = 256 * 1024;
        config.MaxPendingQuicReceiveBytesPerRelay = 32 * 1024;

        TqLinuxRelayWorker worker(config);
        if (!worker.StartForTest()) {
            return 1;
        }

        int fds[2]{-1, -1};
        if (::socketpair(AF_UNIX, SOCK_STREAM, 0, fds) != 0) {
            worker.Stop();
            return 1;
        }
        int sendBuffer = 4096;
        (void)::setsockopt(fds[0], SOL_SOCKET, SO_SNDBUF, &sendBuffer, sizeof(sendBuffer));

        alignas(MsQuicStream) uint8_t fakeStreamStorage[sizeof(MsQuicStream)]{};
        MsQuicStream* fakeStream = reinterpret_cast<MsQuicStream*>(fakeStreamStorage);

        TqLinuxRelayRegistration registration{};
        registration.TcpFd = fds[0];
        registration.Stream = fakeStream;
        registration.EnableQuicSends = false;
        const auto registered = worker.RegisterRelayWithId(registration);
        if (!registered.Ok) {
            worker.Stop();
            ::close(fds[1]);
            return 1;
        }

        const std::vector<uint8_t> plain(2 * 1024 * 1024, 0x4D);
        QUIC_BUFFER quicBuffer{};
        quicBuffer.Buffer = const_cast<uint8_t*>(plain.data());
        quicBuffer.Length = static_cast<uint32_t>(plain.size());

        QUIC_STREAM_EVENT receiveEvent{};
        receiveEvent.Type = QUIC_STREAM_EVENT_RECEIVE;
        receiveEvent.RECEIVE.BufferCount = 1;
        receiveEvent.RECEIVE.Buffers = &quicBuffer;

        const QUIC_STATUS budgetStatus = worker.DispatchStreamEventForTest(fakeStream, &receiveEvent);
        if (budgetStatus != QUIC_STATUS_PENDING) {
            std::fprintf(stderr, "expected budget fallback PENDING, got %d\n", budgetStatus);
            worker.Stop();
            ::close(fds[1]);
            return 1;
        }
        if (worker.DrainForTest(config.EventBudget) != 1) {
            worker.Stop();
            ::close(fds[1]);
            return 1;
        }

        TqLinuxRelayWorkerSnapshot partial = worker.Snapshot();
        if (partial.DeferredReceiveCompleteBytes == 0 ||
            partial.DeferredReceiveCompleteBytes >= plain.size() ||
            partial.PendingBytes == 0 ||
            partial.QuicReceivePausedCount != 1) {
            std::fprintf(stderr, "expected partial deferred receive with pause, complete=%llu pending=%llu pauses=%llu\n",
                static_cast<unsigned long long>(partial.DeferredReceiveCompleteBytes),
                static_cast<unsigned long long>(partial.PendingBytes),
                static_cast<unsigned long long>(partial.QuicReceivePausedCount));
            worker.Stop();
            ::close(fds[1]);
            return 1;
        }

        std::vector<uint8_t> output;
        output.reserve(plain.size());
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(3000);
        uint8_t buffer[8192];
        while (std::chrono::steady_clock::now() < deadline && output.size() < plain.size()) {
            const ssize_t received = ::recv(fds[1], buffer, sizeof(buffer), MSG_DONTWAIT);
            if (received > 0) {
                output.insert(output.end(), buffer, buffer + received);
                continue;
            }
            if (received < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
                TqLinuxRelayEvent writable{};
                writable.Type = TqLinuxRelayEventType::TcpWritable;
                writable.RelayId = registered.RelayId;
                (void)worker.EnqueueForTest(std::move(writable));
                (void)worker.DrainForTest(config.EventBudget);
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
                continue;
            }
            break;
        }

        const TqLinuxRelayWorkerSnapshot completed = worker.Snapshot();
        if (output != plain ||
            completed.DeferredReceiveCompleteBytes != plain.size() ||
            completed.PendingBytes != 0 ||
            completed.QuicReceiveResumedCount != 1) {
            std::fprintf(stderr, "expected deferred receive to drain and resume, output=%zu complete=%llu pending=%llu resumes=%llu\n",
                output.size(),
                static_cast<unsigned long long>(completed.DeferredReceiveCompleteBytes),
                static_cast<unsigned long long>(completed.PendingBytes),
                static_cast<unsigned long long>(completed.QuicReceiveResumedCount));
            worker.Stop();
            ::close(fds[1]);
            return 1;
        }

        worker.Stop();
        ::close(fds[1]);
    }

    {
        QUIC_API_TABLE fakeApi{};
        InstallFakeMsQuicForSend(fakeApi);

        TqLinuxRelayWorkerConfig config{};
        config.EventBudget = 128;
        config.ReadChunkSize = 4096;
        config.ReadBatchBytes = 64 * 1024;
        config.MaxIov = 8;
        config.MaxPendingBufferBytes = 256 * 1024;

        TqLinuxRelayWorker worker(config);
        if (!worker.Start()) {
            MsQuic = nullptr;
            return 1;
        }

        int fds[2]{-1, -1};
        if (::socketpair(AF_UNIX, SOCK_STREAM, 0, fds) != 0) {
            worker.Stop();
            MsQuic = nullptr;
            return 1;
        }

        alignas(MsQuicStream) uint8_t fakeStreamStorage[sizeof(MsQuicStream)]{};
        std::memset(fakeStreamStorage, 0, sizeof(fakeStreamStorage));
        MsQuicStream* fakeStream = reinterpret_cast<MsQuicStream*>(fakeStreamStorage);
        fakeStream->Handle = reinterpret_cast<HQUIC>(static_cast<uintptr_t>(1));

        TqRelayHandle handle{};
        TqLinuxRelayRegistration registration{};
        registration.TcpFd = fds[0];
        registration.Stream = fakeStream;
        registration.Handle = &handle;
        registration.EnableQuicSends = true;
        if (!worker.RegisterRelayForTest(registration)) {
            worker.Stop();
            ::close(fds[1]);
            MsQuic = nullptr;
            return 1;
        }

        if (::shutdown(fds[1], SHUT_WR) != 0) {
            worker.Stop();
            ::close(fds[1]);
            MsQuic = nullptr;
            return 1;
        }

        const auto readClosedDeadline =
            std::chrono::steady_clock::now() + std::chrono::milliseconds(2000);
        while (std::chrono::steady_clock::now() < readClosedDeadline) {
            const auto snapshot = worker.Snapshot();
            if (snapshot.TcpReadClosedRelays == 1) {
                break;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }

        QUIC_STREAM_EVENT finEvent{};
        finEvent.Type = QUIC_STREAM_EVENT_RECEIVE;
        finEvent.RECEIVE.BufferCount = 0;
        finEvent.RECEIVE.Buffers = nullptr;
        finEvent.RECEIVE.Flags = QUIC_RECEIVE_FLAG_FIN;
        if (worker.DispatchStreamEventForTest(fakeStream, &finEvent) != QUIC_STATUS_PENDING) {
            worker.Stop();
            ::close(fds[1]);
            MsQuic = nullptr;
            return 1;
        }
        (void)worker.DrainForTest(config.EventBudget);
        CompleteFakeSends(worker, fakeStream);
        (void)worker.DrainForTest(config.EventBudget);

        const auto stopDeadline =
            std::chrono::steady_clock::now() + std::chrono::milliseconds(2000);
        while (std::chrono::steady_clock::now() < stopDeadline &&
               !handle.Stop.load(std::memory_order_acquire)) {
            (void)worker.DrainForTest(config.EventBudget);
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }

        const TqLinuxRelayWorkerSnapshot snapshot = worker.Snapshot();
        if (!handle.Stop.load(std::memory_order_acquire)) {
            std::fprintf(stderr,
                "expected relay stop after both TCP read and QUIC receive FIN, active=%llu read_closed=%llu write_shutdown=%llu closing=%llu sends=%llu pending=%llu\n",
                static_cast<unsigned long long>(snapshot.ActiveRelays),
                static_cast<unsigned long long>(snapshot.TcpReadClosedRelays),
                static_cast<unsigned long long>(snapshot.TcpWriteShutdownQueuedRelays),
                static_cast<unsigned long long>(snapshot.ClosingRelays),
                static_cast<unsigned long long>(snapshot.OutstandingQuicSends),
                static_cast<unsigned long long>(snapshot.PendingBytes));
            worker.Stop();
            ::close(fds[1]);
            MsQuic = nullptr;
            return 1;
        }

        worker.Stop();
        ::close(fds[1]);
        MsQuic = nullptr;
    }

    {
        TqLinuxRelayWorkerConfig config{};
        config.EventBudget = 128;
        config.ReadChunkSize = 4096;
        config.ReadBatchBytes = 64 * 1024;
        config.MaxIov = 8;
        config.MaxPendingBufferBytes = 256 * 1024;

        TqLinuxRelayWorker worker(config);
        if (!worker.StartForTest()) {
            return 1;
        }

        alignas(MsQuicStream) uint8_t fakeStreamStorage[sizeof(MsQuicStream)]{};
        std::memset(fakeStreamStorage, 0, sizeof(fakeStreamStorage));
        MsQuicStream* fakeStream = reinterpret_cast<MsQuicStream*>(fakeStreamStorage);
        TqRelayHandle handle{};
        std::atomic<uint64_t> sinkBytes{0};

        TqLinuxRelayRegistration registration{};
        registration.TcpFd = -1;
        registration.Stream = fakeStream;
        registration.Handle = &handle;
        registration.EnableQuicSends = false;
        registration.SinkQuicReceives = true;
        registration.SinkQuicReceiveBytes = &sinkBytes;
        if (!worker.RegisterRelayForTest(registration)) {
            std::fprintf(stderr, "expected sink relay registration without TCP fd\n");
            worker.Stop();
            return 1;
        }

        const std::vector<uint8_t> first(64 * 1024, 0x21);
        const std::vector<uint8_t> second(32 * 1024, 0x22);
        QUIC_BUFFER quicBuffers[2]{};
        quicBuffers[0].Buffer = const_cast<uint8_t*>(first.data());
        quicBuffers[0].Length = static_cast<uint32_t>(first.size());
        quicBuffers[1].Buffer = const_cast<uint8_t*>(second.data());
        quicBuffers[1].Length = static_cast<uint32_t>(second.size());

        QUIC_STREAM_EVENT receiveEvent{};
        receiveEvent.Type = QUIC_STREAM_EVENT_RECEIVE;
        receiveEvent.RECEIVE.BufferCount = 2;
        receiveEvent.RECEIVE.Buffers = quicBuffers;

        if (worker.DispatchStreamEventForTest(fakeStream, &receiveEvent) != QUIC_STATUS_PENDING) {
            worker.Stop();
            return 1;
        }
        if (worker.DrainForTest(config.EventBudget) != 1) {
            worker.Stop();
            return 1;
        }

        const TqLinuxRelayWorkerSnapshot snapshot = worker.Snapshot();
        const uint64_t expectedBytes = first.size() + second.size();
        if (sinkBytes.load(std::memory_order_relaxed) != expectedBytes ||
            snapshot.DeferredReceiveCompleteBytes != expectedBytes ||
            snapshot.QuicReceiveViewCount != 1 ||
            snapshot.MaxQuicReceiveViewSlices != 2 ||
            snapshot.TcpWriteBytes != 0 ||
            snapshot.TcpWriteSendmsgCalls != 0 ||
            snapshot.PendingBytes != 0 ||
            handle.Stop.load(std::memory_order_acquire)) {
            std::fprintf(stderr,
                "expected sink relay to discard and complete receive, sink=%llu complete=%llu views=%llu slices=%llu tcp_write=%llu calls=%llu pending=%llu stop=%d\n",
                static_cast<unsigned long long>(sinkBytes.load(std::memory_order_relaxed)),
                static_cast<unsigned long long>(snapshot.DeferredReceiveCompleteBytes),
                static_cast<unsigned long long>(snapshot.QuicReceiveViewCount),
                static_cast<unsigned long long>(snapshot.MaxQuicReceiveViewSlices),
                static_cast<unsigned long long>(snapshot.TcpWriteBytes),
                static_cast<unsigned long long>(snapshot.TcpWriteSendmsgCalls),
                static_cast<unsigned long long>(snapshot.PendingBytes),
                handle.Stop.load(std::memory_order_acquire) ? 1 : 0);
            worker.Stop();
            return 1;
        }

        worker.Stop();
    }

    {
        TqLinuxRelayWorkerConfig config{};
        config.EventBudget = 128;
        config.ReadChunkSize = 1024;
        config.ReadBatchBytes = 4096;
        config.MaxIov = 16;
        config.MaxPendingBufferBytes = 64ull * 1024 * 1024;
        config.TcpWriteMaxBytes = 4ull * 1024 * 1024;

        TqLinuxRelayWorker worker(config);
        if (!worker.StartForTest()) {
            return 1;
        }

        int fds[2]{-1, -1};
        if (::socketpair(AF_UNIX, SOCK_STREAM, 0, fds) != 0) {
            worker.Stop();
            return 1;
        }

        int sendBuffer = 64 * 1024;
        (void)::setsockopt(fds[0], SOL_SOCKET, SO_SNDBUF, &sendBuffer, sizeof(sendBuffer));
        const int peerFlags = ::fcntl(fds[1], F_GETFL, 0);
        (void)::fcntl(fds[1], F_SETFL, peerFlags | O_NONBLOCK);

        alignas(MsQuicStream) uint8_t fakeStreamStorage[sizeof(MsQuicStream)]{};
        MsQuicStream* fakeStream = reinterpret_cast<MsQuicStream*>(fakeStreamStorage);

        TqLinuxRelayRegistration registration{};
        registration.TcpFd = fds[0];
        registration.Stream = fakeStream;
        registration.EnableQuicSends = false;
        if (!worker.RegisterRelayForTest(registration)) {
            worker.Stop();
            ::close(fds[1]);
            return 1;
        }

        std::vector<uint8_t> payload(32ull * 1024 * 1024, 0x6D);
        QUIC_BUFFER quicBuffer{};
        quicBuffer.Buffer = payload.data();
        quicBuffer.Length = static_cast<uint32_t>(payload.size());

        QUIC_STREAM_EVENT receiveEvent{};
        receiveEvent.Type = QUIC_STREAM_EVENT_RECEIVE;
        receiveEvent.RECEIVE.BufferCount = 1;
        receiveEvent.RECEIVE.Buffers = &quicBuffer;

        if (worker.DispatchStreamEventForTest(fakeStream, &receiveEvent) != QUIC_STATUS_PENDING) {
            worker.Stop();
            ::close(fds[1]);
            return 1;
        }
        if (worker.DrainForTest(config.EventBudget) != 1) {
            worker.Stop();
            ::close(fds[1]);
            return 1;
        }

        TqLinuxRelayWorkerSnapshot blocked = worker.Snapshot();
        if (blocked.PendingBytes == 0 ||
            blocked.TcpWriteEagainCount == 0 ||
            !blocked.HotRelayTcpWriteArmed) {
            std::fprintf(stderr,
                "expected blocked QUIC receive to arm TCP writable, pending=%llu eagain=%llu armed=%d\n",
                static_cast<unsigned long long>(blocked.PendingBytes),
                static_cast<unsigned long long>(blocked.TcpWriteEagainCount),
                blocked.HotRelayTcpWriteArmed ? 1 : 0);
            worker.Stop();
            ::close(fds[1]);
            return 1;
        }

        std::vector<uint8_t> drained(512 * 1024);
        size_t drainedTotal = 0;
        while (drainedTotal < drained.size()) {
            const ssize_t received = ::recv(
                fds[1],
                drained.data() + drainedTotal,
                drained.size() - drainedTotal,
                0);
            if (received > 0) {
                drainedTotal += static_cast<size_t>(received);
                continue;
            }
            if (received < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
                break;
            }
            std::fprintf(stderr, "failed to drain peer before writable flush errno=%d\n", errno);
            worker.Stop();
            ::close(fds[1]);
            return 1;
        }
        if (drainedTotal == 0) {
            std::fprintf(stderr, "expected bytes available to drain before writable flush\n");
                worker.Stop();
                ::close(fds[1]);
                return 1;
        }

        if (!worker.FlushTcpWritableForTest(fds[0])) {
            worker.Stop();
            ::close(fds[1]);
            return 1;
        }

        TqLinuxRelayWorkerSnapshot afterWritable = worker.Snapshot();
        if (afterWritable.PendingBytes == 0 ||
            !afterWritable.HotRelayTcpWriteArmed) {
            std::fprintf(stderr,
                "expected writable flush to keep TCP write armed while QUIC pending remains, pending=%llu armed=%d\n",
                static_cast<unsigned long long>(afterWritable.PendingBytes),
                afterWritable.HotRelayTcpWriteArmed ? 1 : 0);
            worker.Stop();
            ::close(fds[1]);
            return 1;
        }

        worker.Stop();
        ::close(fds[1]);
    }

    {
        TqLinuxRelayWorkerConfig config{};
        config.EventBudget = 128;
        config.ReadChunkSize = 1024;
        config.ReadBatchBytes = 4096;
        config.MaxIov = 4;
        config.MaxPendingBufferBytes = 256 * 1024;

        TqLinuxRelayWorker worker(config);
        if (!worker.StartForTest()) {
            return 1;
        }

        int fds1[2]{-1, -1};
        int fds2[2]{-1, -1};
        if (::socketpair(AF_UNIX, SOCK_STREAM, 0, fds1) != 0 ||
            ::socketpair(AF_UNIX, SOCK_STREAM, 0, fds2) != 0) {
            worker.Stop();
            if (fds1[0] >= 0) ::close(fds1[0]);
            if (fds1[1] >= 0) ::close(fds1[1]);
            if (fds2[0] >= 0) ::close(fds2[0]);
            if (fds2[1] >= 0) ::close(fds2[1]);
            return 1;
        }

        alignas(MsQuicStream) uint8_t fakeStreamStorage1[sizeof(MsQuicStream)]{};
        alignas(MsQuicStream) uint8_t fakeStreamStorage2[sizeof(MsQuicStream)]{};
        MsQuicStream* fakeStream1 = reinterpret_cast<MsQuicStream*>(fakeStreamStorage1);
        MsQuicStream* fakeStream2 = reinterpret_cast<MsQuicStream*>(fakeStreamStorage2);

        TqLinuxRelayRegistration registration1{};
        registration1.TcpFd = fds1[0];
        registration1.Stream = fakeStream1;
        registration1.EnableQuicSends = false;
        TqLinuxRelayRegistration registration2{};
        registration2.TcpFd = fds2[0];
        registration2.Stream = fakeStream2;
        registration2.EnableQuicSends = false;
        if (!worker.RegisterRelayForTest(registration1) ||
            !worker.RegisterRelayForTest(registration2)) {
            worker.Stop();
            ::close(fds1[1]);
            ::close(fds2[1]);
            return 1;
        }

        const TqLinuxRelayWorkerSnapshot snapshot = worker.Snapshot();
        if (snapshot.ActiveRelayStates.size() != 2) {
            std::fprintf(
                stderr,
                "expected 2 active relay states, got %zu\n",
                snapshot.ActiveRelayStates.size());
            worker.Stop();
            ::close(fds1[1]);
            ::close(fds2[1]);
            return 3202;
        }
        if (snapshot.ActiveRelays != 2 ||
            snapshot.MaxWorkerActiveRelays != 2 ||
            snapshot.MaxWorkerPendingBytes != snapshot.PendingBytes ||
            snapshot.MaxRelayPendingQuicReceiveBytes != 0 ||
            snapshot.MaxRelayPendingQuicReceiveQueue != 0 ||
            snapshot.MaxRelayTcpWriteEagainCount != 0 ||
            snapshot.HotRelayId == 0 ||
            snapshot.HotRelayWorkerIndex != config.WorkerIndex ||
            snapshot.HotRelayTcpFd < 0 ||
            snapshot.HotRelayTcpWriteBytes != 0 ||
            snapshot.HotRelayTcpWriteEagainCount != 0 ||
            snapshot.HotRelayEpollOutEvents != 0 ||
            !snapshot.HotRelayTcpReadArmed ||
            snapshot.HotRelayTcpWriteArmed ||
            snapshot.HotRelayLocalAddress.empty() ||
            snapshot.HotRelayPeerAddress.empty()) {
            std::fprintf(stderr,
                "expected relay distribution metrics, relays=%llu max_worker_relays=%llu max_worker_pending=%llu pending=%llu max_relay_pending=%llu max_relay_queue=%llu max_relay_eagain=%llu hot_id=%llu hot_worker=%llu hot_fd=%d local=%s peer=%s\n",
                static_cast<unsigned long long>(snapshot.ActiveRelays),
                static_cast<unsigned long long>(snapshot.MaxWorkerActiveRelays),
                static_cast<unsigned long long>(snapshot.MaxWorkerPendingBytes),
                static_cast<unsigned long long>(snapshot.PendingBytes),
                static_cast<unsigned long long>(snapshot.MaxRelayPendingQuicReceiveBytes),
                static_cast<unsigned long long>(snapshot.MaxRelayPendingQuicReceiveQueue),
                static_cast<unsigned long long>(snapshot.MaxRelayTcpWriteEagainCount),
                static_cast<unsigned long long>(snapshot.HotRelayId),
                static_cast<unsigned long long>(snapshot.HotRelayWorkerIndex),
                snapshot.HotRelayTcpFd,
                snapshot.HotRelayLocalAddress.c_str(),
                snapshot.HotRelayPeerAddress.c_str());
            worker.Stop();
            ::close(fds1[1]);
            ::close(fds2[1]);
            return 1;
        }

        worker.Stop();
        ::close(fds1[1]);
        ::close(fds2[1]);
    }

    {
        TqLinuxRelayWorkerConfig config{};
        config.EventBudget = 128;
        config.ReadChunkSize = 1024;
        config.ReadBatchBytes = 4096;
        config.MaxIov = 4;
        config.MaxPendingBufferBytes = 256 * 1024;
        config.TcpWriteMaxBytes = 4096;
        config.TcpWriteBurstBytes = 8192;

        TqLinuxRelayWorker worker(config);
        if (!worker.StartForTest()) {
            return 1;
        }

        int fds[2]{-1, -1};
        if (::socketpair(AF_UNIX, SOCK_STREAM, 0, fds) != 0) {
            worker.Stop();
            return 1;
        }

        alignas(MsQuicStream) uint8_t fakeStreamStorage[sizeof(MsQuicStream)]{};
        MsQuicStream* fakeStream = reinterpret_cast<MsQuicStream*>(fakeStreamStorage);

        TqLinuxRelayRegistration registration{};
        registration.TcpFd = fds[0];
        registration.Stream = fakeStream;
        registration.EnableQuicSends = false;
        if (!worker.RegisterRelayForTest(registration)) {
            worker.Stop();
            ::close(fds[1]);
            return 1;
        }

        std::vector<uint8_t> payload(64 * 1024, 0x4A);
        QUIC_BUFFER quicBuffer{};
        quicBuffer.Buffer = payload.data();
        quicBuffer.Length = static_cast<uint32_t>(payload.size());

        QUIC_STREAM_EVENT receiveEvent{};
        receiveEvent.Type = QUIC_STREAM_EVENT_RECEIVE;
        receiveEvent.RECEIVE.BufferCount = 1;
        receiveEvent.RECEIVE.Buffers = &quicBuffer;

        if (worker.DispatchStreamEventForTest(fakeStream, &receiveEvent) != QUIC_STATUS_PENDING) {
            worker.Stop();
            ::close(fds[1]);
            return 1;
        }
        if (worker.DrainForTest(config.EventBudget) != 1) {
            worker.Stop();
            ::close(fds[1]);
            return 1;
        }

        TqLinuxRelayWorkerSnapshot partial = worker.Snapshot();
        if (partial.TcpWriteBytes != config.TcpWriteBurstBytes ||
            partial.TcpWriteSendmsgCalls != 2 ||
            partial.MaxTcpWriteSendmsgBytes > config.TcpWriteMaxBytes ||
            partial.MaxPendingQuicReceiveBytes != payload.size() ||
            partial.PendingBytes != payload.size() - config.TcpWriteBurstBytes ||
            partial.TcpWriteBurstStops != 1) {
            std::fprintf(stderr,
                "expected burst-limited flush, bytes=%llu calls=%llu pending=%llu stops=%llu\n",
                static_cast<unsigned long long>(partial.TcpWriteBytes),
                static_cast<unsigned long long>(partial.TcpWriteSendmsgCalls),
                static_cast<unsigned long long>(partial.PendingBytes),
                static_cast<unsigned long long>(partial.TcpWriteBurstStops));
            worker.Stop();
            ::close(fds[1]);
            return 1;
        }

        std::vector<uint8_t> output(config.TcpWriteBurstBytes);
        size_t receivedTotal = 0;
        while (receivedTotal < output.size()) {
            const ssize_t received = ::recv(
                fds[1],
                output.data() + receivedTotal,
                output.size() - receivedTotal,
                0);
            if (received <= 0) {
                worker.Stop();
                ::close(fds[1]);
                return 1;
            }
            receivedTotal += static_cast<size_t>(received);
        }
        if (!std::equal(output.begin(), output.end(), payload.begin())) {
            worker.Stop();
            ::close(fds[1]);
            return 1;
        }

        worker.Stop();
        ::close(fds[1]);
    }

    {
        TqLinuxRelayWorkerConfig config{};
        config.EventBudget = 128;
        config.ReadChunkSize = 4096;
        config.ReadBatchBytes = 64 * 1024;
        config.MaxIov = 8;
        config.MaxPendingBufferBytes = 512 * 1024;

        TqLinuxRelayWorker worker(config);
        if (!worker.StartForTest()) {
            return 1;
        }

        int fds[2]{-1, -1};
        if (::socketpair(AF_UNIX, SOCK_STREAM, 0, fds) != 0) {
            worker.Stop();
            return 1;
        }

        auto compressor = TqCreateCompressor(TqCompressAlgo::Zstd, 1);
        auto decompressor = TqCreateDecompressor(TqCompressAlgo::Zstd);
        if (!compressor || !decompressor) {
            worker.Stop();
            ::close(fds[1]);
            return 1;
        }

        alignas(MsQuicStream) uint8_t fakeStreamStorage[sizeof(MsQuicStream)]{};
        MsQuicStream* fakeStream = reinterpret_cast<MsQuicStream*>(fakeStreamStorage);

        TqLinuxRelayRegistration registration{};
        registration.TcpFd = fds[0];
        registration.Stream = fakeStream;
        registration.Decompressor = decompressor.get();
        registration.CompressAlgo = TqCompressAlgo::Zstd;
        registration.EnableQuicSends = false;
        const auto registered = worker.RegisterRelayWithId(registration);
        if (!registered.Ok) {
            worker.Stop();
            ::close(fds[1]);
            return 1;
        }

        std::vector<uint8_t> plain(1024 * 1024, 0);
        std::vector<uint8_t> compressed;
        if (!compressor->Compress(plain.data(), plain.size(), compressed, true) ||
            compressed.empty()) {
            worker.Stop();
            ::close(fds[1]);
            return 1;
        }

        QUIC_BUFFER quicBuffer{};
        quicBuffer.Buffer = compressed.data();
        quicBuffer.Length = static_cast<uint32_t>(compressed.size());

        QUIC_STREAM_EVENT receiveEvent{};
        receiveEvent.Type = QUIC_STREAM_EVENT_RECEIVE;
        receiveEvent.RECEIVE.BufferCount = 1;
        receiveEvent.RECEIVE.Buffers = &quicBuffer;
        receiveEvent.RECEIVE.Flags = QUIC_RECEIVE_FLAG_FIN;

        const QUIC_STATUS receiveStatus = worker.DispatchStreamEventForTest(fakeStream, &receiveEvent);
        if (receiveStatus != QUIC_STATUS_PENDING) {
            std::fprintf(stderr, "expected compressed pending receive, got %d\n", receiveStatus);
            worker.Stop();
            ::close(fds[1]);
            return 1;
        }
        (void)worker.DrainForTest(config.EventBudget);

        std::vector<uint8_t> output;
        output.reserve(plain.size());
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(3000);
        uint8_t readBuffer[8192];
        while (std::chrono::steady_clock::now() < deadline && output.size() < plain.size()) {
            const ssize_t received = ::recv(fds[1], readBuffer, sizeof(readBuffer), MSG_DONTWAIT);
            if (received > 0) {
                output.insert(output.end(), readBuffer, readBuffer + received);
                continue;
            }
            if (received < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
                TqLinuxRelayEvent writable{};
                writable.Type = TqLinuxRelayEventType::TcpWritable;
                writable.RelayId = registered.RelayId;
                (void)worker.EnqueueForTest(std::move(writable));
                (void)worker.DrainForTest(config.EventBudget);
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
                continue;
            }
            break;
        }

        const TqLinuxRelayWorkerSnapshot snapshot = worker.Snapshot();
        if (output != plain ||
            snapshot.DecompressedTcpBytes < plain.size() ||
            snapshot.ZstdDecompressInputBytes != compressed.size() ||
            snapshot.ZstdDecompressOutputBytes != plain.size() ||
            snapshot.ZstdDecompressCalls == 0 ||
            snapshot.ZstdDecompressFailures != 0 ||
            snapshot.DeferredReceiveCompleteBytes != compressed.size() ||
            snapshot.PendingBytes != 0) {
            std::fprintf(stderr, "expected large compressed receive to drain, output=%zu decompressed=%llu zstd=%llu/%llu calls=%llu failures=%llu complete=%llu/%zu pending=%llu\n",
                output.size(),
                static_cast<unsigned long long>(snapshot.DecompressedTcpBytes),
                static_cast<unsigned long long>(snapshot.ZstdDecompressInputBytes),
                static_cast<unsigned long long>(snapshot.ZstdDecompressOutputBytes),
                static_cast<unsigned long long>(snapshot.ZstdDecompressCalls),
                static_cast<unsigned long long>(snapshot.ZstdDecompressFailures),
                static_cast<unsigned long long>(snapshot.DeferredReceiveCompleteBytes),
                compressed.size(),
                static_cast<unsigned long long>(snapshot.PendingBytes));
            worker.Stop();
            ::close(fds[1]);
            return 1;
        }

        worker.Stop();
        ::close(fds[1]);
    }

    {
        TqLinuxRelayWorkerConfig config{};
        config.EventBudget = 128;
        config.ReadChunkSize = 4096;
        config.ReadBatchBytes = 64 * 1024;
        config.MaxIov = 8;
        config.MaxPendingBufferBytes = 256 * 1024;

        TqLinuxRelayWorker worker(config);
        if (!worker.StartForTest()) {
            return 1;
        }

        int fds[2]{-1, -1};
        if (::socketpair(AF_UNIX, SOCK_STREAM, 0, fds) != 0) {
            worker.Stop();
            return 1;
        }

        alignas(MsQuicStream) uint8_t fakeStreamStorage[sizeof(MsQuicStream)]{};
        MsQuicStream* fakeStream = reinterpret_cast<MsQuicStream*>(fakeStreamStorage);
        TqRelayHandle handle{};

        TqLinuxRelayRegistration registration{};
        registration.TcpFd = fds[0];
        registration.Stream = fakeStream;
        registration.Handle = &handle;
        registration.EnableQuicSends = false;
        if (!worker.RegisterRelayForTest(registration)) {
            worker.Stop();
            ::close(fds[1]);
            return 1;
        }

        ::close(fds[1]);
        fds[1] = -1;

        const std::vector<uint8_t> plain(64 * 1024, 0x6E);
        QUIC_BUFFER quicBuffer{};
        quicBuffer.Buffer = const_cast<uint8_t*>(plain.data());
        quicBuffer.Length = static_cast<uint32_t>(plain.size());

        QUIC_STREAM_EVENT receiveEvent{};
        receiveEvent.Type = QUIC_STREAM_EVENT_RECEIVE;
        receiveEvent.RECEIVE.BufferCount = 1;
        receiveEvent.RECEIVE.Buffers = &quicBuffer;

        if (worker.DispatchStreamEventForTest(fakeStream, &receiveEvent) != QUIC_STATUS_PENDING) {
            worker.Stop();
            return 1;
        }
        if (worker.DrainForTest(config.EventBudget) != 1) {
            worker.Stop();
            return 1;
        }

        const TqLinuxRelayWorkerSnapshot snapshot = worker.Snapshot();
        if (!handle.Stop.load(std::memory_order_acquire) ||
            snapshot.FatalRelayResets == 0 ||
            snapshot.PendingBytes != 0 ||
            snapshot.DeferredReceiveCompleteBytes != plain.size() ||
            snapshot.LastTcpWriteErrno == 0) {
            std::fprintf(stderr,
                "expected TCP hard write error to fatal-reset and complete pending receive, stop=%d pending=%llu resets=%llu complete=%llu errno=%llu\n",
                handle.Stop.load() ? 1 : 0,
                static_cast<unsigned long long>(snapshot.PendingBytes),
                static_cast<unsigned long long>(snapshot.FatalRelayResets),
                static_cast<unsigned long long>(snapshot.DeferredReceiveCompleteBytes),
                static_cast<unsigned long long>(snapshot.LastTcpWriteErrno));
            worker.Stop();
            return 1;
        }

        worker.Stop();
    }

    {
        TqLinuxRelayWorkerConfig config{};
        config.EventBudget = 128;
        config.ReadChunkSize = 4096;
        config.ReadBatchBytes = 64 * 1024;
        config.MaxIov = 8;
        config.MaxPendingBufferBytes = 256 * 1024;

        TqLinuxRelayWorker worker(config);
        if (!worker.Start()) {
            return 1;
        }

        int fds[2]{-1, -1};
        if (::socketpair(AF_UNIX, SOCK_STREAM, 0, fds) != 0) {
            worker.Stop();
            return 1;
        }

        alignas(MsQuicStream) uint8_t fakeStreamStorage[sizeof(MsQuicStream)]{};
        MsQuicStream* fakeStream = reinterpret_cast<MsQuicStream*>(fakeStreamStorage);

        TqLinuxRelayRegistration registration{};
        registration.TcpFd = fds[0];
        registration.Stream = fakeStream;
        TqRelayHandle handle{};
        registration.Handle = &handle;
        registration.EnableQuicSends = false;
        if (!worker.RegisterRelayForTest(registration)) {
            worker.Stop();
            ::close(fds[1]);
            return 1;
        }

        QUIC_STREAM_EVENT finEvent{};
        finEvent.Type = QUIC_STREAM_EVENT_RECEIVE;
        finEvent.RECEIVE.BufferCount = 0;
        finEvent.RECEIVE.Buffers = nullptr;
        finEvent.RECEIVE.Flags = QUIC_RECEIVE_FLAG_FIN;

        if (worker.DispatchStreamEventForTest(fakeStream, &finEvent) != QUIC_STATUS_PENDING) {
            worker.Stop();
            ::close(fds[1]);
            return 1;
        }
        if (worker.DrainForTest(config.EventBudget) != 1) {
            worker.Stop();
            ::close(fds[1]);
            return 1;
        }

        uint8_t one = 0;
        const ssize_t received = ::read(fds[1], &one, sizeof(one));
        if (received != 0) {
            std::fprintf(stderr, "expected FIN-only receive to half-close TCP, got %zd\n", received);
            worker.Stop();
            ::close(fds[1]);
            return 1;
        }
        const TqLinuxRelayWorkerSnapshot snapshot = worker.Snapshot();
        if (handle.Stop.load(std::memory_order_acquire) ||
            snapshot.Errors != 0 ||
            snapshot.QuicReceiveViewEmptyFailures != 0) {
            std::fprintf(stderr,
                "FIN-only receive should be graceful, stop=%d errors=%llu empty=%llu\n",
                handle.Stop.load(std::memory_order_acquire) ? 1 : 0,
                static_cast<unsigned long long>(snapshot.Errors),
                static_cast<unsigned long long>(snapshot.QuicReceiveViewEmptyFailures));
            worker.Stop();
            ::close(fds[1]);
            return 1;
        }

        worker.Stop();
        ::close(fds[1]);
    }

    {
        TqLinuxRelayWorkerConfig config{};
        config.EventBudget = 128;
        config.ReadChunkSize = 4096;
        config.ReadBatchBytes = 64 * 1024;
        config.MaxIov = 8;
        config.MaxPendingBufferBytes = 256 * 1024;

        TqLinuxRelayWorker worker(config);
        if (!worker.Start()) {
            return 1;
        }

        int fds[2]{-1, -1};
        if (::socketpair(AF_UNIX, SOCK_STREAM, 0, fds) != 0) {
            worker.Stop();
            return 1;
        }
        int sendBuffer = 4096;
        (void)::setsockopt(fds[0], SOL_SOCKET, SO_SNDBUF, &sendBuffer, sizeof(sendBuffer));

        alignas(MsQuicStream) uint8_t fakeStreamStorage[sizeof(MsQuicStream)]{};
        MsQuicStream* fakeStream = reinterpret_cast<MsQuicStream*>(fakeStreamStorage);

        TqLinuxRelayRegistration registration{};
        registration.TcpFd = fds[0];
        registration.Stream = fakeStream;
        registration.EnableQuicSends = false;
        if (!worker.RegisterRelayForTest(registration)) {
            worker.Stop();
            ::close(fds[1]);
            return 1;
        }

        const std::vector<uint8_t> plain(2 * 1024 * 1024, 0x71);
        QUIC_BUFFER quicBuffer{};
        quicBuffer.Buffer = const_cast<uint8_t*>(plain.data());
        quicBuffer.Length = static_cast<uint32_t>(plain.size());

        QUIC_STREAM_EVENT receiveEvent{};
        receiveEvent.Type = QUIC_STREAM_EVENT_RECEIVE;
        receiveEvent.RECEIVE.BufferCount = 1;
        receiveEvent.RECEIVE.Buffers = &quicBuffer;

        if (worker.DispatchStreamEventForTest(fakeStream, &receiveEvent) != QUIC_STATUS_PENDING) {
            worker.Stop();
            ::close(fds[1]);
            return 1;
        }
        if (worker.DrainForTest(config.EventBudget) != 1) {
            worker.Stop();
            ::close(fds[1]);
            return 1;
        }
        const TqLinuxRelayWorkerSnapshot beforeShutdown = worker.Snapshot();
        if (beforeShutdown.PendingBytes == 0 || beforeShutdown.ActiveRelays != 1) {
            worker.Stop();
            ::close(fds[1]);
            return 1;
        }

        QUIC_STREAM_EVENT shutdownEvent{};
        shutdownEvent.Type = QUIC_STREAM_EVENT_SHUTDOWN_COMPLETE;
        if (QUIC_FAILED(worker.DispatchStreamEventForTest(fakeStream, &shutdownEvent))) {
            worker.Stop();
            ::close(fds[1]);
            return 1;
        }
        (void)worker.DrainForTest(config.EventBudget);

        const TqLinuxRelayWorkerSnapshot afterShutdown = worker.Snapshot();
        if (afterShutdown.PendingBytes != 0 ||
            afterShutdown.FatalRelayResets == 0) {
            std::fprintf(stderr,
                "expected shutdown_complete with pending data to reset relay, before_pending=%llu after_pending=%llu active=%llu closing=%llu resets=%llu\n",
                static_cast<unsigned long long>(beforeShutdown.PendingBytes),
                static_cast<unsigned long long>(afterShutdown.PendingBytes),
                static_cast<unsigned long long>(afterShutdown.ActiveRelays),
                static_cast<unsigned long long>(afterShutdown.ClosingRelays),
                static_cast<unsigned long long>(afterShutdown.FatalRelayResets));
            worker.Stop();
            ::close(fds[1]);
            return 1;
        }

        worker.Stop();
        ::close(fds[1]);
    }

    {
        TqLinuxRelayWorkerConfig config{};
        config.EventBudget = 128;
        config.ReadChunkSize = 4096;
        config.ReadBatchBytes = 64 * 1024;
        config.MaxIov = 8;
        config.MaxPendingBufferBytes = 256 * 1024;
        config.EventQueueCapacity = 2;

        TqLinuxRelayWorker worker(config);
        if (!worker.StartForTest()) {
            return 1;
        }

        int fds[2]{-1, -1};
        if (::socketpair(AF_UNIX, SOCK_STREAM, 0, fds) != 0) {
            worker.Stop();
            return 1;
        }

        alignas(MsQuicStream) uint8_t fakeStreamStorage[sizeof(MsQuicStream)]{};
        MsQuicStream* fakeStream = reinterpret_cast<MsQuicStream*>(fakeStreamStorage);
        TqRelayHandle handle{};

        TqLinuxRelayRegistration registration{};
        registration.TcpFd = fds[0];
        registration.Stream = fakeStream;
        registration.Handle = &handle;
        registration.EnableQuicSends = false;
        if (!worker.RegisterRelayForTest(registration)) {
            worker.Stop();
            ::close(fds[1]);
            return 1;
        }
        TqLinuxRelayEvent markerA{};
        markerA.Type = TqLinuxRelayEventType::TestMarker;
        if (!worker.EnqueueForTest(std::move(markerA))) {
            ::close(fds[1]);
            return 1;
        }
        TqLinuxRelayEvent markerB{};
        markerB.Type = TqLinuxRelayEventType::TestMarker;
        if (!worker.EnqueueForTest(std::move(markerB))) {
            ::close(fds[1]);
            return 1;
        }

        const std::vector<uint8_t> plain(1024, 0x51);
        QUIC_BUFFER quicBuffer{};
        quicBuffer.Buffer = const_cast<uint8_t*>(plain.data());
        quicBuffer.Length = static_cast<uint32_t>(plain.size());

        QUIC_STREAM_EVENT receiveEvent{};
        receiveEvent.Type = QUIC_STREAM_EVENT_RECEIVE;
        receiveEvent.RECEIVE.BufferCount = 1;
        receiveEvent.RECEIVE.Buffers = &quicBuffer;

        const QUIC_STATUS status = worker.DispatchStreamEventForTest(fakeStream, &receiveEvent);
        const TqLinuxRelayWorkerSnapshot snapshot = worker.Snapshot();
        if (status != QUIC_STATUS_PENDING ||
            handle.Stop.load() ||
            snapshot.QuicReceiveViewBackpressureQueued == 0 ||
            snapshot.QuicReceiveViewEnqueueFailures != 0) {
            std::fprintf(stderr,
                "expected queue-full receive backpressure with PENDING, status=%d stop=%d backpressure=%llu enqueue_fail=%llu\n",
                status,
                handle.Stop.load() ? 1 : 0,
                static_cast<unsigned long long>(snapshot.QuicReceiveViewBackpressureQueued),
                static_cast<unsigned long long>(snapshot.QuicReceiveViewEnqueueFailures));
            worker.Stop();
            ::close(fds[1]);
            return 1;
        }

        worker.Stop();
        ::close(fds[1]);
    }

    {
        TqLinuxRelayWorkerConfig config{};
        config.EventBudget = 128;
        config.ReadChunkSize = 1024;
        config.ReadBatchBytes = 4096;
        config.MaxIov = 4;
        config.MaxPendingBufferBytes = 256 * 1024;
        config.TcpWriteMaxBytes = 4096;

        TqLinuxRelayWorker worker(config);
        if (!worker.Start()) {
            return 1;
        }

        int fds[2]{-1, -1};
        if (::socketpair(AF_UNIX, SOCK_STREAM, 0, fds) != 0) {
            worker.Stop();
            return 1;
        }

        alignas(MsQuicStream) uint8_t fakeStreamStorage[sizeof(MsQuicStream)]{};
        MsQuicStream* fakeStream = reinterpret_cast<MsQuicStream*>(fakeStreamStorage);

        TqLinuxRelayRegistration registration{};
        registration.TcpFd = fds[0];
        registration.Stream = fakeStream;
        registration.EnableQuicSends = false;
        if (!worker.RegisterRelayForTest(registration)) {
            worker.Stop();
            ::close(fds[1]);
            return 1;
        }

        std::vector<uint8_t> payload(64 * 1024, 0x7C);
        QUIC_BUFFER quicBuffer{};
        quicBuffer.Buffer = payload.data();
        quicBuffer.Length = static_cast<uint32_t>(payload.size());

        QUIC_STREAM_EVENT receiveEvent{};
        receiveEvent.Type = QUIC_STREAM_EVENT_RECEIVE;
        receiveEvent.RECEIVE.BufferCount = 1;
        receiveEvent.RECEIVE.Buffers = &quicBuffer;

        if (worker.DispatchStreamEventForTest(fakeStream, &receiveEvent) != QUIC_STATUS_PENDING) {
            worker.Stop();
            ::close(fds[1]);
            return 1;
        }
        if (worker.DrainForTest(config.EventBudget) != 1) {
            worker.Stop();
            ::close(fds[1]);
            return 1;
        }

        std::vector<uint8_t> output(payload.size());
        size_t receivedTotal = 0;
        while (receivedTotal < output.size()) {
            const ssize_t received = ::recv(
                fds[1],
                output.data() + receivedTotal,
                output.size() - receivedTotal,
                0);
            if (received <= 0) {
                worker.Stop();
                ::close(fds[1]);
                return 1;
            }
            receivedTotal += static_cast<size_t>(received);
        }

        const TqLinuxRelayWorkerSnapshot snapshot = worker.Snapshot();
        if (output != payload ||
            snapshot.MaxTcpWriteSendmsgBytes > config.TcpWriteMaxBytes ||
            snapshot.TcpWriteSendmsgCalls < 2 ||
            snapshot.QuicReceiveViewCount != 1 ||
            snapshot.MaxQuicReceiveViewBytes != payload.size() ||
            snapshot.MaxQuicReceiveViewSlices != 1 ||
            snapshot.TcpWriteReturnedBytesLe64K == 0 ||
            snapshot.TcpWriteAttemptBytesLe64K == 0 ||
            snapshot.QuicReceiveViewBytesLe64K == 0) {
            std::fprintf(stderr,
                "expected capped tcp writes and receive metrics, output=%zu max_send=%llu calls=%llu views=%llu max_view=%llu max_slices=%llu\n",
                output.size(),
                static_cast<unsigned long long>(snapshot.MaxTcpWriteSendmsgBytes),
                static_cast<unsigned long long>(snapshot.TcpWriteSendmsgCalls),
                static_cast<unsigned long long>(snapshot.QuicReceiveViewCount),
                static_cast<unsigned long long>(snapshot.MaxQuicReceiveViewBytes),
                static_cast<unsigned long long>(snapshot.MaxQuicReceiveViewSlices));
            worker.Stop();
            ::close(fds[1]);
            return 1;
        }

        worker.Stop();
        ::close(fds[1]);
    }

    {
        TqLinuxRelayWorkerConfig config{};
        config.EventBudget = 128;
        config.ReadChunkSize = 1024;
        config.ReadBatchBytes = 4096;
        config.MaxIov = 4;
        config.MaxPendingBufferBytes = 64 * 1024;

        TqLinuxRelayWorker worker(config);
        if (!worker.Start()) {
            std::fprintf(stderr, "tcp write metric worker start failed\n"); return 1;
        }

        int fds[2]{-1, -1};
        if (::socketpair(AF_UNIX, SOCK_STREAM, 0, fds) != 0) {
            worker.Stop();
            std::fprintf(stderr, "tcp write metric socketpair failed\n"); return 1;
        }

        TqLinuxRelayRegistration registration{};
        registration.TcpFd = fds[0];
        registration.Stream = nullptr;
        registration.Handle = nullptr;
        registration.EnableQuicSends = false;
        if (!worker.RegisterRelayForTest(registration)) {
            worker.Stop();
            ::close(fds[1]);
            std::fprintf(stderr, "tcp write metric register failed\n"); return 1;
        }

        const uint8_t first[] = {1, 2, 3, 4};
        const uint8_t second[] = {5, 6, 7, 8, 9};
        if (!worker.EnqueueQuicReceiveForTest(fds[0], first, sizeof(first), false) ||
            !worker.EnqueueQuicReceiveForTest(fds[0], second, sizeof(second), true)) {
            return 1;
        }

        uint8_t output[16]{};
        const ssize_t received = ::read(fds[1], output, sizeof(output));
        assert(received == 9);
        for (int i = 0; i < 9; ++i) {
            assert(output[i] == static_cast<uint8_t>(i + 1));
        }

        TqLinuxRelayWorkerSnapshot snapshot = worker.Snapshot();
        if (snapshot.TcpWriteBatches < 1 ||
            snapshot.TcpWriteBytes != 9 ||
            snapshot.MaxTcpWriteIovUsed < 1) {
            std::fprintf(stderr, "expected tcp write metrics, batches=%llu bytes=%llu iov=%llu\n",
                static_cast<unsigned long long>(snapshot.TcpWriteBatches),
                static_cast<unsigned long long>(snapshot.TcpWriteBytes),
                static_cast<unsigned long long>(snapshot.MaxTcpWriteIovUsed));
            return 1;
        }
        if (snapshot.TcpWriteSendmsgCalls < 1 || snapshot.MaxTcpWriteSendmsgBytes == 0) {
            std::fprintf(stderr, "expected sendmsg metrics, calls=%llu max_bytes=%llu\n",
                static_cast<unsigned long long>(snapshot.TcpWriteSendmsgCalls),
                static_cast<unsigned long long>(snapshot.MaxTcpWriteSendmsgBytes));
            return 1;
        }

        worker.Stop();
        ::close(fds[1]);
    }

    {
        TqLinuxRelayWorkerConfig config{};
        config.EventBudget = 128;
        config.ReadChunkSize = 1024;
        config.ReadBatchBytes = 4096;
        config.MaxIov = 4;
        config.MaxPendingBufferBytes = 2048;

        TqLinuxRelayWorker worker(config);
        assert(worker.Start());

        int fds[2]{-1, -1};
        assert(::socketpair(AF_UNIX, SOCK_STREAM, 0, fds) == 0);

        TqLinuxRelayRegistration registration{};
        registration.TcpFd = fds[0];
        registration.Stream = nullptr;
        registration.Handle = nullptr;
        registration.EnableQuicSends = false;
        assert(worker.RegisterRelayForTest(registration));

        std::vector<uint8_t> payload(8192, 0x11);
        assert(::write(fds[1], payload.data(), payload.size()) == static_cast<ssize_t>(payload.size()));
        assert(worker.WaitForObservedTcpBytesForTest(2048, 2000));

        TqLinuxRelayWorkerSnapshot snapshot = worker.Snapshot();
        assert(snapshot.TcpReadBytes <= 4096);
        assert(snapshot.ReadDisabledCount >= 1);

        worker.Stop();
        ::close(fds[1]);
    }

    {
        TqLinuxRelayWorkerConfig config{};
        config.EventBudget = 128;
        config.ReadChunkSize = 1024;
        config.ReadBatchBytes = 4096;
        config.MaxIov = 4;
        config.MaxPendingBufferBytes = 64 * 1024;

        TqLinuxRelayWorker worker(config);
        assert(worker.Start());

        int fds[2]{-1, -1};
        assert(::socketpair(AF_UNIX, SOCK_STREAM, 0, fds) == 0);

        auto compressor = TqCreateCompressor(TqCompressAlgo::Zstd, 1);
        auto decompressor = TqCreateDecompressor(TqCompressAlgo::Zstd);
        assert(compressor);
        assert(decompressor);

        TqLinuxRelayRegistration registration{};
        registration.TcpFd = fds[0];
        registration.Stream = nullptr;
        registration.Handle = nullptr;
        registration.Compressor = compressor.get();
        registration.CompressAlgo = TqCompressAlgo::Zstd;
        registration.EnableQuicSends = false;
        assert(worker.RegisterRelayForTest(registration));

        std::vector<uint8_t> payload(4096, 0x42);
        assert(::write(fds[1], payload.data(), payload.size()) == static_cast<ssize_t>(payload.size()));
        assert(worker.WaitForObservedTcpBytesForTest(payload.size(), 2000));

        const std::vector<uint8_t> compressed = worker.TakeCapturedQuicBytesForTest(fds[0]);
        assert(!compressed.empty());

        std::vector<uint8_t> restored;
        assert(decompressor->Decompress(compressed.data(), compressed.size(), restored));
        assert(restored == payload);

        worker.Stop();
        ::close(fds[1]);
    }

    {
        TqLinuxRelayWorkerConfig config{};
        config.EventBudget = 128;
        config.ReadChunkSize = 512;
        config.ReadBatchBytes = 4096;
        config.MaxIov = 4;
        config.MaxPendingBufferBytes = 64 * 1024;

        TqLinuxRelayWorker worker(config);
        assert(worker.Start());

        int fds[2]{-1, -1};
        assert(::socketpair(AF_UNIX, SOCK_STREAM, 0, fds) == 0);

        auto compressor = TqCreateCompressor(TqCompressAlgo::Zstd, 1);
        auto decompressor = TqCreateDecompressor(TqCompressAlgo::Zstd);
        assert(compressor);
        assert(decompressor);

        alignas(MsQuicStream) uint8_t fakeStreamStorage[sizeof(MsQuicStream)]{};
        MsQuicStream* fakeStream = reinterpret_cast<MsQuicStream*>(fakeStreamStorage);

        TqLinuxRelayRegistration registration{};
        registration.TcpFd = fds[0];
        registration.Stream = fakeStream;
        registration.Handle = nullptr;
        registration.Decompressor = decompressor.get();
        registration.CompressAlgo = TqCompressAlgo::Zstd;
        registration.EnableQuicSends = false;
        assert(worker.RegisterRelayForTest(registration));

        const std::vector<uint8_t> plain(2048, 0x7C);
        std::vector<uint8_t> compressed;
        assert(compressor->Compress(plain.data(), plain.size(), compressed, true));
        assert(!compressed.empty());

        QUIC_BUFFER quicBuffer{};
        quicBuffer.Buffer = compressed.data();
        quicBuffer.Length = static_cast<uint32_t>(compressed.size());

        QUIC_STREAM_EVENT receiveEvent{};
        receiveEvent.Type = QUIC_STREAM_EVENT_RECEIVE;
        receiveEvent.RECEIVE.BufferCount = 1;
        receiveEvent.RECEIVE.Buffers = &quicBuffer;
        receiveEvent.RECEIVE.Flags = QUIC_RECEIVE_FLAG_FIN;

        assert(worker.DispatchStreamEventForTest(fakeStream, &receiveEvent) == QUIC_STATUS_PENDING);
        assert(worker.DrainForTest(config.EventBudget) >= 1);

        std::vector<uint8_t> output(plain.size());
        size_t offset = 0;
        while (offset < output.size()) {
            const ssize_t received = ::read(fds[1], output.data() + offset, output.size() - offset);
            assert(received > 0);
            offset += static_cast<size_t>(received);
        }
        assert(output == plain);

        worker.Stop();
        ::close(fds[1]);
    }

    {
        TqLinuxRelayWorkerConfig config{};
        config.EventBudget = 128;
        config.ReadChunkSize = 4096;
        config.ReadBatchBytes = 64 * 1024;
        config.MaxIov = 8;
        config.MaxPendingBufferBytes = 256 * 1024;

        TqLinuxRelayWorker worker(config);
        if (!worker.Start()) {
            return 1;
        }

        int fds[2]{-1, -1};
        if (::socketpair(AF_UNIX, SOCK_STREAM, 0, fds) != 0) {
            worker.Stop();
            return 1;
        }

        alignas(MsQuicStream) uint8_t fakeStreamStorage[sizeof(MsQuicStream)]{};
        MsQuicStream* fakeStream = reinterpret_cast<MsQuicStream*>(fakeStreamStorage);

        TqLinuxRelayRegistration registration{};
        registration.TcpFd = fds[0];
        registration.Stream = fakeStream;
        registration.EnableQuicSends = false;
        if (!worker.RegisterRelayForTest(registration)) {
            worker.Stop();
            ::close(fds[1]);
            return 1;
        }
        // The inert test registration must not install a production handler;
        // tests drive the binding explicitly through DispatchStreamEventForTest.
        if (fakeStream->Callback != nullptr || fakeStream->Context != nullptr) {
            worker.Stop();
            ::close(fds[1]);
            return 1;
        }

        const std::vector<uint8_t> plain(8192, 0x5A);
        QUIC_BUFFER quicBuffer{};
        quicBuffer.Buffer = const_cast<uint8_t*>(plain.data());
        quicBuffer.Length = static_cast<uint32_t>(plain.size());

        QUIC_STREAM_EVENT receiveEvent{};
        receiveEvent.Type = QUIC_STREAM_EVENT_RECEIVE;
        receiveEvent.RECEIVE.BufferCount = 1;
        receiveEvent.RECEIVE.Buffers = &quicBuffer;

        const QUIC_STATUS receiveStatus = worker.DispatchStreamEventForTest(fakeStream, &receiveEvent);
        if (receiveStatus != QUIC_STATUS_PENDING) {
            std::fprintf(stderr, "expected deferred receive status PENDING, got %d\n", receiveStatus);
            worker.Stop();
            ::close(fds[1]);
            return 1;
        }
        if (worker.DrainForTest(config.EventBudget) < 1) {
            worker.Stop();
            ::close(fds[1]);
            return 1;
        }

        std::vector<uint8_t> output(plain.size());
        size_t offset = 0;
        while (offset < output.size()) {
            const ssize_t received = ::read(fds[1], output.data() + offset, output.size() - offset);
            if (received <= 0) {
                std::fprintf(stderr, "deferred receive read failed at offset=%zu received=%zd errno=%d\n", offset, received, errno);
                worker.Stop();
                ::close(fds[1]);
                return 1;
            }
            offset += static_cast<size_t>(received);
        }
        if (output != plain) {
            std::fprintf(stderr, "deferred receive output mismatch\n");
            worker.Stop();
            ::close(fds[1]);
            return 1;
        }

        const TqLinuxRelayWorkerSnapshot snapshot = worker.Snapshot();
        if (snapshot.BufferAcquireCount != 0) {
            std::fprintf(stderr, "expected zero buffer acquires for deferred receive, got %llu\n",
                static_cast<unsigned long long>(snapshot.BufferAcquireCount));
            worker.Stop();
            ::close(fds[1]);
            return 1;
        }
        if (snapshot.DeferredReceiveCompleteBytes != plain.size() ||
            snapshot.DeferredReceiveCompletes != 1) {
            std::fprintf(stderr, "expected one deferred complete for %zu bytes, got %llu/%llu\n",
                plain.size(),
                static_cast<unsigned long long>(snapshot.DeferredReceiveCompleteBytes),
                static_cast<unsigned long long>(snapshot.DeferredReceiveCompletes));
            worker.Stop();
            ::close(fds[1]);
            return 1;
        }
        if (snapshot.StreamLookupScanCount != 0) {
            std::fprintf(stderr, "expected 0 stream lookup scans, got %llu\n",
                static_cast<unsigned long long>(snapshot.StreamLookupScanCount));
            worker.Stop();
            ::close(fds[1]);
            return 1;
        }

        worker.Stop();
        ::close(fds[1]);
    }

    {
        TqLinuxRelayWorkerConfig config{};
        config.EventBudget = 128;
        config.ReadChunkSize = 4096;
        config.ReadBatchBytes = 64 * 1024;
        config.MaxIov = 8;
        config.MaxPendingBufferBytes = 256 * 1024;

        TqLinuxRelayWorker worker(config);
        if (!worker.Start()) {
            return 372;
        }

        int fds[2]{-1, -1};
        if (::socketpair(AF_UNIX, SOCK_STREAM, 0, fds) != 0) {
            worker.Stop();
            return 373;
        }

        QUIC_API_TABLE fakeApi{};
        InstallFakeMsQuicForSend(fakeApi);

        alignas(MsQuicStream) uint8_t fakeStreamStorage[sizeof(MsQuicStream)]{};
        std::memset(fakeStreamStorage, 0, sizeof(fakeStreamStorage));
        MsQuicStream* fakeStream = reinterpret_cast<MsQuicStream*>(fakeStreamStorage);
        fakeStream->Handle = reinterpret_cast<HQUIC>(static_cast<uintptr_t>(1));

        TqLinuxRelayRegistration registration{};
        registration.TcpFd = fds[0];
        registration.Stream = fakeStream;
        registration.EnableQuicSends = false;
        if (!worker.RegisterRelayForTest(registration)) {
            worker.Stop();
            ::close(fds[0]);
            ::close(fds[1]);
            MsQuic = nullptr;
            return 374;
        }

        QUIC_STREAM_EVENT fakeFin{};
        fakeFin.Type = QUIC_STREAM_EVENT_RECEIVE;
        fakeFin.RECEIVE.AbsoluteOffset = UINT64_MAX;
        fakeFin.RECEIVE.TotalBufferLength = 0;
        fakeFin.RECEIVE.BufferCount = 0;
        fakeFin.RECEIVE.Buffers = nullptr;
        fakeFin.RECEIVE.Flags = QUIC_RECEIVE_FLAG_FIN;

        const QUIC_STATUS status = worker.DispatchStreamEventForTest(fakeStream, &fakeFin);
        if (status != QUIC_STATUS_SUCCESS) {
            std::fprintf(stderr, "expected fake FIN to return SUCCESS, got %d\n", status);
            worker.Stop();
            ::close(fds[1]);
            MsQuic = nullptr;
            return 375;
        }

        const TqLinuxRelayWorkerSnapshot snapshot = worker.Snapshot();
        if (snapshot.FakeFinReceiveCount != 1) {
            std::fprintf(stderr, "expected fake FIN count 1, got %llu\n",
                static_cast<unsigned long long>(snapshot.FakeFinReceiveCount));
            worker.Stop();
            ::close(fds[1]);
            MsQuic = nullptr;
            return 376;
        }
        if (snapshot.FatalRelayResets != 1) {
            std::fprintf(stderr, "expected fatal relay resets 1, got %llu\n",
                static_cast<unsigned long long>(snapshot.FatalRelayResets));
            worker.Stop();
            ::close(fds[1]);
            MsQuic = nullptr;
            return 377;
        }
        if (snapshot.DeferredReceiveCompletes != 0) {
            std::fprintf(stderr, "fake FIN must not complete deferred receive buffers\n");
            worker.Stop();
            ::close(fds[1]);
            MsQuic = nullptr;
            return 378;
        }
        if (snapshot.ClosingRelays == 0) {
            std::fprintf(stderr, "expected fake FIN relay to be closing\n");
            worker.Stop();
            ::close(fds[1]);
            MsQuic = nullptr;
            return 379;
        }

        worker.Stop();
        ::close(fds[1]);
        MsQuic = nullptr;
    }

    {
        TqLinuxRelayWorkerConfig config{};
        config.EventBudget = 128;
        config.ReadChunkSize = 1024;
        config.ReadBatchBytes = 64 * 1024;
        config.MaxIov = 8;
        config.MaxPendingBufferBytes = 256 * 1024;

        TqLinuxRelayWorker worker(config);
        if (!worker.StartForTest()) {
            return 1;
        }

        int fds[2]{-1, -1};
        if (::socketpair(AF_UNIX, SOCK_STREAM, 0, fds) != 0) {
            worker.Stop();
            return 1;
        }

        alignas(MsQuicStream) uint8_t fakeStreamStorage[sizeof(MsQuicStream)]{};
        MsQuicStream* fakeStream = reinterpret_cast<MsQuicStream*>(fakeStreamStorage);

        TqLinuxRelayRegistration registration{};
        registration.TcpFd = fds[0];
        registration.Stream = fakeStream;
        registration.EnableQuicSends = false;
        if (!worker.RegisterRelayForTest(registration)) {
            worker.Stop();
            ::close(fds[1]);
            return 1;
        }

        std::vector<uint8_t> first(600, 0x11);
        std::vector<uint8_t> second(700, 0x22);
        std::vector<uint8_t> third(800, 0x33);
        QUIC_BUFFER quicBuffers[3]{};
        quicBuffers[0].Buffer = first.data();
        quicBuffers[0].Length = static_cast<uint32_t>(first.size());
        quicBuffers[1].Buffer = second.data();
        quicBuffers[1].Length = static_cast<uint32_t>(second.size());
        quicBuffers[2].Buffer = third.data();
        quicBuffers[2].Length = static_cast<uint32_t>(third.size());

        QUIC_STREAM_EVENT receiveEvent{};
        receiveEvent.Type = QUIC_STREAM_EVENT_RECEIVE;
        receiveEvent.RECEIVE.BufferCount = 3;
        receiveEvent.RECEIVE.Buffers = quicBuffers;

        if (QUIC_FAILED(worker.DispatchStreamEventForTest(fakeStream, &receiveEvent))) {
            worker.Stop();
            ::close(fds[1]);
            return 1;
        }

        const TqLinuxRelayWorkerSnapshot queued = worker.Snapshot();
        if (queued.PendingEvents != 1) {
            std::fprintf(stderr, "expected 1 batched receive event, got %llu\n",
                static_cast<unsigned long long>(queued.PendingEvents));
            worker.Stop();
            ::close(fds[1]);
            return 1;
        }

        if (worker.DrainForTest(config.EventBudget) != 1) {
            worker.Stop();
            ::close(fds[1]);
            return 1;
        }

        std::vector<uint8_t> expected;
        expected.insert(expected.end(), first.begin(), first.end());
        expected.insert(expected.end(), second.begin(), second.end());
        expected.insert(expected.end(), third.begin(), third.end());

        std::vector<uint8_t> output(expected.size());
        size_t offset = 0;
        while (offset < output.size()) {
            const ssize_t received = ::read(fds[1], output.data() + offset, output.size() - offset);
            if (received <= 0) {
                worker.Stop();
                ::close(fds[1]);
                return 1;
            }
            offset += static_cast<size_t>(received);
        }
        if (output != expected) {
            worker.Stop();
            ::close(fds[1]);
            return 1;
        }

        worker.Stop();
        ::close(fds[1]);
    }

    {
        TqLinuxRelayWorkerConfig config{};
        config.EventBudget = 128;
        config.ReadChunkSize = 512;
        config.ReadBatchBytes = 4096;
        config.MaxIov = 4;
        config.MaxPendingBufferBytes = 128 * 1024;

        TqLinuxRelayWorker worker(config);
        assert(worker.Start());

        int fds[2]{-1, -1};
        assert(::socketpair(AF_UNIX, SOCK_STREAM, 0, fds) == 0);

        auto compressor = TqCreateCompressor(TqCompressAlgo::Zstd, 1);
        auto decompressor = TqCreateDecompressor(TqCompressAlgo::Zstd);
        assert(compressor);
        assert(decompressor);

        TqLinuxRelayRegistration registration{};
        registration.TcpFd = fds[0];
        registration.Stream = nullptr;
        registration.Handle = nullptr;
        registration.Compressor = compressor.get();
        registration.CompressAlgo = TqCompressAlgo::Zstd;
        registration.EnableQuicSends = false;
        assert(worker.RegisterRelayForTest(registration));

        std::vector<uint8_t> payload(6000);
        for (size_t i = 0; i < payload.size(); ++i) {
            payload[i] = static_cast<uint8_t>((i * 7919U + 104729U) & 0xFFU);
        }
        assert(::write(fds[1], payload.data(), payload.size()) == static_cast<ssize_t>(payload.size()));
        assert(worker.WaitForObservedTcpBytesForTest(payload.size(), 2000));

        const std::vector<uint8_t> compressed = worker.TakeCapturedQuicBytesForTest(fds[0]);
        assert(!compressed.empty());

        std::vector<uint8_t> restored;
        assert(decompressor->Decompress(compressed.data(), compressed.size(), restored));
        assert(restored == payload);

        worker.Stop();
        ::close(fds[1]);
    }

    {
        TqLinuxRelayWorkerConfig config{};
        config.EventBudget = 128;
        config.ReadChunkSize = 1024;
        config.ReadBatchBytes = 4096;
        config.MaxIov = 4;
        config.MaxPendingBufferBytes = 128 * 1024;

        TqLinuxRelayWorker worker(config);
        assert(worker.Start());

        int fds[2]{-1, -1};
        assert(::socketpair(AF_UNIX, SOCK_STREAM, 0, fds) == 0);

        auto compressor = TqCreateCompressor(TqCompressAlgo::Zstd, 1);
        auto decompressor = TqCreateDecompressor(TqCompressAlgo::Zstd);
        assert(compressor);
        assert(decompressor);

        TqLinuxRelayRegistration registration{};
        registration.TcpFd = fds[0];
        registration.Stream = nullptr;
        registration.Handle = nullptr;
        registration.Compressor = compressor.get();
        registration.CompressAlgo = TqCompressAlgo::Zstd;
        registration.EnableQuicSends = false;
        assert(worker.RegisterRelayForTest(registration));

        std::vector<uint8_t> payload(3500, 0x5A);
        assert(::write(fds[1], payload.data(), payload.size()) == static_cast<ssize_t>(payload.size()));
        assert(worker.WaitForObservedTcpBytesForTest(payload.size(), 2000));

        const std::vector<uint8_t> compressed = worker.TakeCapturedQuicBytesForTest(fds[0]);
        assert(!compressed.empty());

        std::vector<uint8_t> restored;
        assert(decompressor->Decompress(compressed.data(), compressed.size(), restored));
        assert(restored == payload);

        TqLinuxRelayWorkerSnapshot snapshot = worker.Snapshot();
        assert(snapshot.MaxTcpReadIovUsed >= 2);

        worker.Stop();
        ::close(fds[1]);
    }

    {
        TqLinuxRelayWorkerConfig config{};
        config.EventBudget = 128;
        config.ReadChunkSize = 4096;
        config.ReadBatchBytes = 64 * 1024;
        config.MaxIov = 8;
        config.MaxPendingBufferBytes = 256 * 1024;

        TqLinuxRelayWorker worker(config);
        if (!worker.Start()) {
            return 1;
        }

        int fds[2]{-1, -1};
        if (::socketpair(AF_UNIX, SOCK_STREAM, 0, fds) != 0) {
            worker.Stop();
            return 1;
        }

        alignas(MsQuicStream) uint8_t fakeStreamStorage[sizeof(MsQuicStream)]{};
        MsQuicStream* fakeStream = reinterpret_cast<MsQuicStream*>(fakeStreamStorage);

        TqLinuxRelayRegistration registration{};
        registration.TcpFd = fds[0];
        registration.Stream = fakeStream;
        registration.EnableQuicSends = false;
        if (!worker.RegisterRelayForTest(registration)) {
            worker.Stop();
            ::close(fds[1]);
            return 1;
        }

        const std::vector<uint8_t> plain(8192, 0x5A);
        QUIC_BUFFER quicBuffer{};
        quicBuffer.Buffer = const_cast<uint8_t*>(plain.data());
        quicBuffer.Length = static_cast<uint32_t>(plain.size());

        QUIC_STREAM_EVENT receiveEvent{};
        receiveEvent.Type = QUIC_STREAM_EVENT_RECEIVE;
        receiveEvent.RECEIVE.BufferCount = 1;
        receiveEvent.RECEIVE.Buffers = &quicBuffer;

        const QUIC_STATUS receiveStatus = worker.DispatchStreamEventForTest(fakeStream, &receiveEvent);
        if (receiveStatus != QUIC_STATUS_PENDING) {
            std::fprintf(stderr, "expected queued receive status PENDING, got %d\n", receiveStatus);
            worker.Stop();
            ::close(fds[1]);
            return 1;
        }
        if (worker.DrainForTest(config.EventBudget) != 1) {
            worker.Stop();
            ::close(fds[1]);
            return 1;
        }

        std::vector<uint8_t> output(plain.size());
        size_t offset = 0;
        while (offset < output.size()) {
            const ssize_t received = ::read(fds[1], output.data() + offset, output.size() - offset);
            if (received <= 0) {
                worker.Stop();
                ::close(fds[1]);
                return 1;
            }
            offset += static_cast<size_t>(received);
        }
        if (output != plain) {
            worker.Stop();
            ::close(fds[1]);
            return 1;
        }

        const TqLinuxRelayWorkerSnapshot snapshot = worker.Snapshot();
        if (snapshot.PendingEvents != 0 ||
            snapshot.DeferredReceiveCompleteBytes != plain.size() ||
            snapshot.DeferredReceiveCompletes != 1) {
            std::fprintf(stderr, "expected queued receive write, pending=%llu complete=%llu/%llu\n",
                static_cast<unsigned long long>(snapshot.PendingEvents),
                static_cast<unsigned long long>(snapshot.DeferredReceiveCompleteBytes),
                static_cast<unsigned long long>(snapshot.DeferredReceiveCompletes));
            worker.Stop();
            ::close(fds[1]);
            return 1;
        }

        worker.Stop();
        ::close(fds[1]);
    }

    {
        TqLinuxRelayWorkerConfig config{};
        config.EventBudget = 128;
        config.ReadChunkSize = 4096;
        config.ReadBatchBytes = 64 * 1024;
        config.MaxIov = 8;
        config.MaxPendingBufferBytes = 256 * 1024;
        config.DeferredReceiveCompleteBatchBytes = 16 * 1024;

        TqLinuxRelayWorker worker(config);
        if (!worker.Start()) {
            return 1;
        }

        int fds[2]{-1, -1};
        if (::socketpair(AF_UNIX, SOCK_STREAM, 0, fds) != 0) {
            worker.Stop();
            return 1;
        }

        alignas(MsQuicStream) uint8_t fakeStreamStorage[sizeof(MsQuicStream)]{};
        MsQuicStream* fakeStream = reinterpret_cast<MsQuicStream*>(fakeStreamStorage);

        TqLinuxRelayRegistration registration{};
        registration.TcpFd = fds[0];
        registration.Stream = fakeStream;
        registration.EnableQuicSends = false;
        if (!worker.RegisterRelayForTest(registration)) {
            worker.Stop();
            ::close(fds[1]);
            return 1;
        }

        std::vector<uint8_t> first(4096, 0x11);
        std::vector<uint8_t> second(4096, 0x22);
        QUIC_BUFFER quicBuffers[2]{};
        quicBuffers[0].Buffer = first.data();
        quicBuffers[0].Length = static_cast<uint32_t>(first.size());
        quicBuffers[1].Buffer = second.data();
        quicBuffers[1].Length = static_cast<uint32_t>(second.size());

        QUIC_STREAM_EVENT receiveEvent{};
        receiveEvent.Type = QUIC_STREAM_EVENT_RECEIVE;
        receiveEvent.RECEIVE.BufferCount = 2;
        receiveEvent.RECEIVE.Buffers = quicBuffers;

        if (worker.DispatchStreamEventForTest(fakeStream, &receiveEvent) != QUIC_STATUS_PENDING) {
            worker.Stop();
            ::close(fds[1]);
            return 1;
        }
        if (worker.DrainForTest(config.EventBudget) != 1) {
            worker.Stop();
            ::close(fds[1]);
            return 1;
        }

        const TqLinuxRelayWorkerSnapshot snapshot = worker.Snapshot();
        if (snapshot.DeferredReceiveCompleteBytes != first.size() + second.size() ||
            snapshot.DeferredReceiveCompletes != 1 ||
            snapshot.DeferredReceiveCompletionFlushes != 1) {
            std::fprintf(stderr, "expected final batched complete, bytes=%llu completes=%llu flushes=%llu\n",
                static_cast<unsigned long long>(snapshot.DeferredReceiveCompleteBytes),
                static_cast<unsigned long long>(snapshot.DeferredReceiveCompletes),
                static_cast<unsigned long long>(snapshot.DeferredReceiveCompletionFlushes));
            worker.Stop();
            ::close(fds[1]);
            return 1;
        }

        worker.Stop();
        ::close(fds[1]);
    }

    {
        TqLinuxRelayWorkerConfig config{};
        config.EventBudget = 128;
        config.ReadChunkSize = 1024;
        config.ReadBatchBytes = 4096;
        config.MaxIov = 4;

        TqLinuxRelayWorker worker(config);
        if (!worker.StartForTest()) {
            return 1;
        }
        int fds[2]{-1, -1};
        if (::socketpair(AF_UNIX, SOCK_STREAM, 0, fds) != 0) {
            worker.Stop();
            return 1;
        }
        alignas(MsQuicStream) uint8_t fakeStreamStorage[sizeof(MsQuicStream)]{};
        MsQuicStream* fakeStream = reinterpret_cast<MsQuicStream*>(fakeStreamStorage);
        TqRelayHandle handle{};
        TqLinuxRelayRegistration registration{};
        registration.TcpFd = fds[0];
        registration.Stream = fakeStream;
        registration.Handle = &handle;
        registration.EnableQuicSends = false;
        const auto registered = worker.RegisterRelayWithId(registration);
        if (!registered.Ok) {
            worker.Stop();
            ::close(fds[1]);
            return 1;
        }

        ::close(fds[0]);
        if (!worker.DispatchTcpEventsForTest(registered.RelayId, EPOLLIN)) {
            worker.Stop();
            ::close(fds[1]);
            return 1;
        }
        const TqLinuxRelayWorkerSnapshot snapshot = worker.Snapshot();
        if (!handle.Stop.load(std::memory_order_acquire) ||
            snapshot.TcpReadHardErrors == 0 ||
            snapshot.LastTcpReadErrno == 0 ||
            snapshot.FatalRelayResets == 0) {
            std::fprintf(stderr,
                "expected tcp read hard error fatal reset, stop=%d read_errors=%llu errno=%llu resets=%llu\n",
                handle.Stop.load(std::memory_order_acquire) ? 1 : 0,
                static_cast<unsigned long long>(snapshot.TcpReadHardErrors),
                static_cast<unsigned long long>(snapshot.LastTcpReadErrno),
                static_cast<unsigned long long>(snapshot.FatalRelayResets));
            worker.Stop();
            ::close(fds[1]);
            return 1;
        }
        worker.Stop();
        ::close(fds[1]);
    }

    {
        QUIC_API_TABLE fakeApi{};
        InstallFakeMsQuicForSend(fakeApi);

        TqLinuxRelayWorkerConfig config{};
        config.EventBudget = 128;
        config.ReadChunkSize = 1024;
        config.ReadBatchBytes = 4096;
        config.MaxIov = 4;
        config.MaxPendingBufferBytes = 256 * 1024;

        TqLinuxRelayWorker worker(config);
        if (!worker.StartForTest()) {
            MsQuic = nullptr;
            return 1;
        }
        int fds[2]{-1, -1};
        if (::socketpair(AF_UNIX, SOCK_STREAM, 0, fds) != 0) {
            worker.Stop();
            MsQuic = nullptr;
            return 1;
        }
        alignas(MsQuicStream) uint8_t fakeStreamStorage[sizeof(MsQuicStream)]{};
        std::memset(fakeStreamStorage, 0, sizeof(fakeStreamStorage));
        MsQuicStream* fakeStream = reinterpret_cast<MsQuicStream*>(fakeStreamStorage);
        fakeStream->Handle = reinterpret_cast<HQUIC>(static_cast<uintptr_t>(1));
        TqRelayHandle handle{};
        TqLinuxRelayRegistration registration{};
        registration.TcpFd = fds[0];
        registration.Stream = fakeStream;
        registration.Handle = &handle;
        registration.EnableQuicSends = true;
        const auto registered = worker.RegisterRelayWithId(registration);
        if (!registered.Ok) {
            worker.Stop();
            ::close(fds[1]);
            MsQuic = nullptr;
            return 1;
        }

        const std::vector<uint8_t> payload(4096, 0x4B);
        if (::write(fds[1], payload.data(), payload.size()) !=
            static_cast<ssize_t>(payload.size()) ||
            !worker.DispatchTcpEventsForTest(registered.RelayId, EPOLLIN)) {
            worker.Stop();
            ::close(fds[1]);
            MsQuic = nullptr;
            return 1;
        }

        const TqLinuxRelayWorkerSnapshot beforeAbort = worker.Snapshot();
        if (beforeAbort.OutstandingQuicSends == 0 || beforeAbort.PendingBytes == 0) {
            std::fprintf(stderr,
                "expected outstanding QUIC send before abort, sends=%llu pending=%llu\n",
                static_cast<unsigned long long>(beforeAbort.OutstandingQuicSends),
                static_cast<unsigned long long>(beforeAbort.PendingBytes));
            worker.Stop();
            ::close(fds[1]);
            MsQuic = nullptr;
            return 1;
        }

        QUIC_STREAM_EVENT aborted{};
        aborted.Type = QUIC_STREAM_EVENT_PEER_RECEIVE_ABORTED;
        if (worker.DispatchStreamEventForTest(fakeStream, &aborted) != QUIC_STATUS_SUCCESS) {
            worker.Stop();
            ::close(fds[1]);
            MsQuic = nullptr;
            return 1;
        }
        (void)worker.DrainForTest(config.EventBudget);

        worker.UnregisterRelay(registered.RelayId);
        CompleteFakeSends(worker, fakeStream);
        (void)worker.DrainForTest(config.EventBudget);
        const TqLinuxRelayWorkerSnapshot afterComplete = worker.Snapshot();
        if (!handle.Stop.load(std::memory_order_acquire) ||
            afterComplete.FatalRelayResets == 0 ||
            afterComplete.OutstandingQuicSends != 0 ||
            afterComplete.PendingBytes != 0 ||
            afterComplete.ActiveRelays != 0) {
            std::fprintf(stderr,
                "expected unregistered aborted relay to release outstanding send on SEND_COMPLETE, stop=%d resets=%llu sends=%llu pending=%llu active=%llu\n",
                handle.Stop.load(std::memory_order_acquire) ? 1 : 0,
                static_cast<unsigned long long>(afterComplete.FatalRelayResets),
                static_cast<unsigned long long>(afterComplete.OutstandingQuicSends),
                static_cast<unsigned long long>(afterComplete.PendingBytes),
                static_cast<unsigned long long>(afterComplete.ActiveRelays));
            worker.Stop();
            ::close(fds[1]);
            MsQuic = nullptr;
            return 1;
        }

        worker.Stop();
        ::close(fds[1]);
        MsQuic = nullptr;
    }

    {
        TqLinuxRelayWorkerConfig config{};
        TqLinuxRelayWorker worker(config);
        if (!worker.StartForTest()) {
            return 1;
        }
        int fds[2]{-1, -1};
        if (::socketpair(AF_UNIX, SOCK_STREAM, 0, fds) != 0) {
            worker.Stop();
            return 1;
        }
        alignas(MsQuicStream) uint8_t fakeStreamStorage[sizeof(MsQuicStream)]{};
        MsQuicStream* fakeStream = reinterpret_cast<MsQuicStream*>(fakeStreamStorage);
        TqRelayHandle handle{};
        TqLinuxRelayRegistration registration{};
        registration.TcpFd = fds[0];
        registration.Stream = fakeStream;
        registration.Handle = &handle;
        registration.EnableQuicSends = false;
        if (!worker.RegisterRelayForTest(registration)) {
            worker.Stop();
            ::close(fds[1]);
            return 1;
        }

        QUIC_STREAM_EVENT aborted{};
        aborted.Type = QUIC_STREAM_EVENT_PEER_SEND_ABORTED;
        if (worker.DispatchStreamEventForTest(fakeStream, &aborted) != QUIC_STATUS_SUCCESS) {
            worker.Stop();
            ::close(fds[1]);
            return 1;
        }
        (void)worker.DrainForTest(config.EventBudget);
        const TqLinuxRelayWorkerSnapshot snapshot = worker.Snapshot();
        if (!handle.Stop.load(std::memory_order_acquire) || snapshot.FatalRelayResets == 0) {
            std::fprintf(stderr,
                "expected peer send abort fatal reset, stop=%d resets=%llu\n",
                handle.Stop.load(std::memory_order_acquire) ? 1 : 0,
                static_cast<unsigned long long>(snapshot.FatalRelayResets));
            worker.Stop();
            ::close(fds[1]);
            return 1;
        }
        worker.Stop();
        ::close(fds[1]);
    }

    {
        TqLinuxRelayWorkerConfig config{};
        config.TcpWriteMaxBytes = 1;
        // Force one byte to remain pending after the first drain.
        config.TcpWriteBurstBytes = 1;
        TqLinuxRelayWorker worker(config);
        if (!worker.StartForTest()) {
            return 1;
        }
        int fds[2]{-1, -1};
        if (::socketpair(AF_UNIX, SOCK_STREAM, 0, fds) != 0) {
            worker.Stop();
            return 1;
        }
        alignas(MsQuicStream) uint8_t fakeStreamStorage[sizeof(MsQuicStream)]{};
        MsQuicStream* fakeStream = reinterpret_cast<MsQuicStream*>(fakeStreamStorage);
        TqRelayHandle handle{};
        TqLinuxRelayRegistration registration{};
        registration.TcpFd = fds[0];
        registration.Stream = fakeStream;
        registration.Handle = &handle;
        registration.EnableQuicSends = false;
        if (!worker.RegisterRelayForTest(registration)) {
            worker.Stop();
            ::close(fds[1]);
            return 1;
        }

        const std::vector<uint8_t> plain(4096, 0x5a);
        QUIC_BUFFER quicBuffer{};
        quicBuffer.Buffer = const_cast<uint8_t*>(plain.data());
        quicBuffer.Length = static_cast<uint32_t>(plain.size());

        QUIC_STREAM_EVENT receiveEvent{};
        receiveEvent.Type = QUIC_STREAM_EVENT_RECEIVE;
        receiveEvent.RECEIVE.BufferCount = 1;
        receiveEvent.RECEIVE.Buffers = &quicBuffer;

        if (worker.DispatchStreamEventForTest(fakeStream, &receiveEvent) != QUIC_STATUS_PENDING ||
            worker.DrainForTest(config.EventBudget) != 1) {
            worker.Stop();
            ::close(fds[1]);
            return 1;
        }
        const TqLinuxRelayWorkerSnapshot beforeShutdown = worker.Snapshot();
        if (beforeShutdown.PendingBytes == 0 ||
            beforeShutdown.CurrentPendingQuicReceiveQueue == 0) {
            std::fprintf(stderr,
                "expected pending receive before shutdown callback, pending=%llu queue=%llu\n",
                static_cast<unsigned long long>(beforeShutdown.PendingBytes),
                static_cast<unsigned long long>(beforeShutdown.CurrentPendingQuicReceiveQueue));
            worker.Stop();
            ::close(fds[1]);
            return 1;
        }

        QUIC_STREAM_EVENT shutdownEvent{};
        shutdownEvent.Type = QUIC_STREAM_EVENT_SHUTDOWN_COMPLETE;
        if (QUIC_FAILED(worker.DispatchStreamEventForTest(fakeStream, &shutdownEvent))) {
            worker.Stop();
            ::close(fds[1]);
            return 1;
        }

        const TqLinuxRelayWorkerSnapshot queuedShutdown = worker.Snapshot();
        if (queuedShutdown.PendingBytes == 0 ||
            queuedShutdown.CurrentPendingQuicReceiveQueue == 0 ||
            queuedShutdown.FatalRelayResets != beforeShutdown.FatalRelayResets ||
            handle.Stop.load(std::memory_order_acquire)) {
            std::fprintf(stderr,
                "shutdown callback must not mutate pending relay state before worker drain, pending=%llu queue=%llu resets=%llu stop=%d\n",
                static_cast<unsigned long long>(queuedShutdown.PendingBytes),
                static_cast<unsigned long long>(queuedShutdown.CurrentPendingQuicReceiveQueue),
                static_cast<unsigned long long>(queuedShutdown.FatalRelayResets),
                handle.Stop.load(std::memory_order_acquire) ? 1 : 0);
            worker.Stop();
            ::close(fds[1]);
            return 1;
        }

        (void)worker.DrainForTest(config.EventBudget);
        const TqLinuxRelayWorkerSnapshot afterDrain = worker.Snapshot();
        if (!handle.Stop.load(std::memory_order_acquire) ||
            afterDrain.PendingBytes != 0 ||
            afterDrain.FatalRelayResets == beforeShutdown.FatalRelayResets) {
            std::fprintf(stderr,
                "expected queued shutdown to reset relay on worker drain, pending=%llu resets=%llu stop=%d\n",
                static_cast<unsigned long long>(afterDrain.PendingBytes),
                static_cast<unsigned long long>(afterDrain.FatalRelayResets),
                handle.Stop.load(std::memory_order_acquire) ? 1 : 0);
            worker.Stop();
            ::close(fds[1]);
            return 1;
        }
        worker.Stop();
        ::close(fds[1]);
    }

    {
        TqLinuxRelayWorkerConfig config{};
        TqLinuxRelayWorker worker(config);
        if (!worker.StartForTest()) {
            return 1;
        }
        int fds[2]{-1, -1};
        if (::socketpair(AF_UNIX, SOCK_STREAM, 0, fds) != 0) {
            worker.Stop();
            return 1;
        }
        alignas(MsQuicStream) uint8_t fakeStreamStorage[sizeof(MsQuicStream)]{};
        MsQuicStream* fakeStream = reinterpret_cast<MsQuicStream*>(fakeStreamStorage);
        TqRelayHandle handle{};
        TqLinuxRelayRegistration registration{};
        registration.TcpFd = fds[0];
        registration.Stream = fakeStream;
        registration.Handle = &handle;
        registration.EnableQuicSends = false;
        if (!worker.RegisterRelayForTest(registration)) {
            worker.Stop();
            ::close(fds[1]);
            return 1;
        }

        QUIC_STREAM_EVENT aborted{};
        aborted.Type = QUIC_STREAM_EVENT_PEER_RECEIVE_ABORTED;
        if (worker.DispatchStreamEventForTest(fakeStream, &aborted) != QUIC_STATUS_SUCCESS) {
            worker.Stop();
            ::close(fds[1]);
            return 1;
        }
        (void)worker.DrainForTest(config.EventBudget);
        const TqLinuxRelayWorkerSnapshot snapshot = worker.Snapshot();
        if (!handle.Stop.load(std::memory_order_acquire) || snapshot.FatalRelayResets == 0) {
            std::fprintf(stderr,
                "expected peer receive abort fatal reset, stop=%d resets=%llu\n",
                handle.Stop.load(std::memory_order_acquire) ? 1 : 0,
                static_cast<unsigned long long>(snapshot.FatalRelayResets));
            worker.Stop();
            ::close(fds[1]);
            return 1;
        }
        worker.Stop();
        ::close(fds[1]);
    }

    {
        QUIC_API_TABLE fakeApi{};
        InstallFakeMsQuicForSend(fakeApi);
        ResetFakeStreamShutdownCalls();

        TqLinuxRelayWorkerConfig config{};
        config.EventBudget = 128;
        config.ReadChunkSize = 1024;
        config.ReadBatchBytes = 4096;
        config.MaxIov = 4;
        config.MaxPendingBufferBytes = 64 * 1024;

        TqLinuxRelayWorker worker(config);
        if (!worker.StartForTest()) {
            MsQuic = nullptr;
            return 4121;
        }

        int fds[2]{-1, -1};
        if (::socketpair(AF_UNIX, SOCK_STREAM, 0, fds) != 0) {
            worker.Stop();
            MsQuic = nullptr;
            return 4122;
        }

        alignas(MsQuicStream) uint8_t fakeStreamStorage[sizeof(MsQuicStream)]{};
        std::memset(fakeStreamStorage, 0, sizeof(fakeStreamStorage));
        MsQuicStream* fakeStream = reinterpret_cast<MsQuicStream*>(fakeStreamStorage);
        fakeStream->Handle = reinterpret_cast<HQUIC>(static_cast<uintptr_t>(0x3456));

        TqRelayHandle handle{};
        TqLinuxRelayRegistration registration{};
        registration.TcpFd = fds[0];
        registration.Stream = fakeStream;
        registration.Handle = &handle;
        registration.EnableQuicSends = true;
        const TqLinuxRelayRegistrationResult registered = worker.RegisterRelayWithId(registration);
        if (!registered.Ok) {
            worker.Stop();
            ::close(fds[1]);
            MsQuic = nullptr;
            return 4123;
        }

        const char payload[] = "pending-send-before-shutdown-complete";
        if (::write(fds[1], payload, sizeof(payload)) != static_cast<ssize_t>(sizeof(payload))) {
            worker.Stop();
            ::close(fds[1]);
            MsQuic = nullptr;
            return 4124;
        }
        if (!worker.DispatchEncodedEpollEventForTest(
                worker.EncodeEpollRelayForTest(registered.RelayId),
                EPOLLIN)) {
            worker.Stop();
            ::close(fds[1]);
            MsQuic = nullptr;
            return 4125;
        }

        TqLinuxRelayWorkerSnapshot beforeShutdown = worker.Snapshot();
        if (beforeShutdown.OutstandingQuicSends == 0) {
            std::fprintf(stderr, "expected outstanding QUIC send before shutdown complete\n");
            worker.Stop();
            ::close(fds[1]);
            MsQuic = nullptr;
            return 4126;
        }

        QUIC_STREAM_EVENT shutdownEvent{};
        shutdownEvent.Type = QUIC_STREAM_EVENT_SHUTDOWN_COMPLETE;
        if (QUIC_FAILED(worker.DispatchStreamEventForTest(fakeStream, &shutdownEvent))) {
            worker.Stop();
            ::close(fds[1]);
            MsQuic = nullptr;
            return 4127;
        }
        (void)worker.DrainForTest(config.EventBudget);

        TqLinuxRelayActiveSnapshot relayState{};
        const TqLinuxRelayWorkerSnapshot afterShutdown = worker.Snapshot();
        if (!FindActiveRelayState(afterShutdown, registered.RelayId, &relayState) ||
            !relayState.StreamDetached ||
            afterShutdown.OutstandingQuicSends == 0) {
            std::fprintf(stderr,
                "expected stream detached with outstanding send preserved after shutdown complete\n");
            worker.Stop();
            ::close(fds[1]);
            MsQuic = nullptr;
            return 4128;
        }

        CompleteFakeSends(worker, fakeStream);
        (void)worker.DrainForTest(config.EventBudget);

        const TqLinuxRelayWorkerSnapshot afterSendComplete = worker.Snapshot();
        if (afterSendComplete.OutstandingQuicSends != 0) {
            std::fprintf(stderr,
                "shutdown-complete detach must preserve send complete cleanup, outstanding=%llu\n",
                static_cast<unsigned long long>(afterSendComplete.OutstandingQuicSends));
            worker.Stop();
            ::close(fds[1]);
            MsQuic = nullptr;
            return 4129;
        }

        worker.Stop();
        ::close(fds[1]);
        MsQuic = nullptr;
    }

    {
        QUIC_API_TABLE fakeApi{};
        InstallFakeMsQuicForSend(fakeApi);
        ResetFakeStreamShutdownCalls();

        TqLinuxRelayWorkerConfig config{};
        config.EventBudget = 128;
        config.ReadChunkSize = 4096;
        config.ReadBatchBytes = 64 * 1024;
        config.MaxIov = 8;
        config.MaxPendingBufferBytes = 256 * 1024;

        TqLinuxRelayWorker worker(config);
        if (!worker.StartForTest()) {
            MsQuic = nullptr;
            return 4101;
        }

        int fds[2]{-1, -1};
        if (!MakeTcpLoopbackPair(fds)) {
            worker.Stop();
            MsQuic = nullptr;
            return 4102;
        }

        alignas(MsQuicStream) uint8_t fakeStreamStorage[sizeof(MsQuicStream)]{};
        std::memset(fakeStreamStorage, 0, sizeof(fakeStreamStorage));
        MsQuicStream* fakeStream = reinterpret_cast<MsQuicStream*>(fakeStreamStorage);
        fakeStream->Handle = reinterpret_cast<HQUIC>(static_cast<uintptr_t>(0x1234));

        TqRelayHandle handle{};
        TqLinuxRelayRegistration registration{};
        registration.TcpFd = fds[0];
        registration.Stream = fakeStream;
        registration.Handle = &handle;
        registration.EnableQuicSends = false;
        const TqLinuxRelayRegistrationResult registered = worker.RegisterRelayWithId(registration);
        if (!registered.Ok) {
            worker.Stop();
            ::close(fds[1]);
            MsQuic = nullptr;
            return 4103;
        }

        QUIC_STREAM_EVENT shutdownEvent{};
        shutdownEvent.Type = QUIC_STREAM_EVENT_SHUTDOWN_COMPLETE;
        if (QUIC_FAILED(worker.DispatchStreamEventForTest(fakeStream, &shutdownEvent))) {
            worker.Stop();
            ::close(fds[1]);
            MsQuic = nullptr;
            return 4104;
        }
        (void)worker.DrainForTest(config.EventBudget);

        TqLinuxRelayActiveSnapshot relayState{};
        const TqLinuxRelayWorkerSnapshot afterShutdown = worker.Snapshot();
        if (FindActiveRelayState(afterShutdown, registered.RelayId, &relayState) ||
            !handle.Stop.load(std::memory_order_acquire)) {
            std::fprintf(stderr, "expected terminal relay to be logically removed\n");
            worker.Stop();
            ::close(fds[1]);
            MsQuic = nullptr;
            return 4105;
        }

        linger rst{};
        rst.l_onoff = 1;
        rst.l_linger = 0;
        (void)::setsockopt(fds[1], SOL_SOCKET, SO_LINGER, &rst, sizeof(rst));
        ::close(fds[1]);
        fds[1] = -1;

        const uint64_t beforeShutdownCalls = ReadFakeStreamShutdownCalls();
        if (!worker.DispatchEncodedEpollEventForTest(
                worker.EncodeEpollRelayForTest(registered.RelayId),
                EPOLLERR | EPOLLHUP | EPOLLRDHUP)) {
            worker.Stop();
            if (fds[1] >= 0) {
                ::close(fds[1]);
            }
            MsQuic = nullptr;
            return 4106;
        }

        const TqLinuxRelayWorkerSnapshot afterLateError = worker.Snapshot();
        if (ReadFakeStreamShutdownCalls() != beforeShutdownCalls) {
            std::fprintf(stderr, "late TCP error after shutdown complete must not abort stream\n");
            worker.Stop();
            if (fds[1] >= 0) {
                ::close(fds[1]);
            }
            MsQuic = nullptr;
            return 4107;
        }
        if (afterLateError.LateTcpErrorAfterStreamShutdown == 0) {
            std::fprintf(stderr, "expected late TCP error after stream shutdown metric\n");
            worker.Stop();
            if (fds[1] >= 0) {
                ::close(fds[1]);
            }
            MsQuic = nullptr;
            return 4108;
        }

        worker.Stop();
        if (fds[1] >= 0) {
            ::close(fds[1]);
        }
        MsQuic = nullptr;
    }

    {
        QUIC_API_TABLE fakeApi{};
        InstallFakeMsQuicForSend(fakeApi);
        ResetFakeStreamShutdownCalls();

        TqLinuxRelayWorkerConfig config{};
        config.EventBudget = 128;
        config.ReadChunkSize = 4096;
        config.ReadBatchBytes = 64 * 1024;
        config.MaxIov = 8;
        config.MaxPendingBufferBytes = 256 * 1024;

        TqLinuxRelayWorker worker(config);
        if (!worker.StartForTest()) {
            MsQuic = nullptr;
            return 4109;
        }

        int fds[2]{-1, -1};
        if (!MakeTcpLoopbackPair(fds)) {
            worker.Stop();
            MsQuic = nullptr;
            return 4110;
        }

        alignas(MsQuicStream) uint8_t fakeStreamStorage[sizeof(MsQuicStream)]{};
        std::memset(fakeStreamStorage, 0, sizeof(fakeStreamStorage));
        MsQuicStream* fakeStream = reinterpret_cast<MsQuicStream*>(fakeStreamStorage);
        fakeStream->Handle = reinterpret_cast<HQUIC>(static_cast<uintptr_t>(0x2345));

        TqRelayHandle handle{};
        TqLinuxRelayRegistration registration{};
        registration.TcpFd = fds[0];
        registration.Stream = fakeStream;
        registration.Handle = &handle;
        registration.EnableQuicSends = false;
        const TqLinuxRelayRegistrationResult registered = worker.RegisterRelayWithId(registration);
        if (!registered.Ok) {
            worker.Stop();
            ::close(fds[1]);
            MsQuic = nullptr;
            return 4111;
        }

        QUIC_STREAM_EVENT finEvent{};
        finEvent.Type = QUIC_STREAM_EVENT_RECEIVE;
        finEvent.RECEIVE.BufferCount = 0;
        finEvent.RECEIVE.Buffers = nullptr;
        finEvent.RECEIVE.Flags = QUIC_RECEIVE_FLAG_FIN;
        if (worker.DispatchStreamEventForTest(fakeStream, &finEvent) != QUIC_STATUS_PENDING) {
            worker.Stop();
            ::close(fds[1]);
            MsQuic = nullptr;
            return 4112;
        }
        (void)worker.DrainForTest(config.EventBudget);

        TqLinuxRelayActiveSnapshot relayState{};
        const TqLinuxRelayWorkerSnapshot afterFin = worker.Snapshot();
        if (!FindActiveRelayState(afterFin, registered.RelayId, &relayState) ||
            !relayState.TcpWriteClosed ||
            relayState.Closing ||
            relayState.StreamDetached) {
            std::fprintf(stderr,
                "expected active relay with tcp write closed before late error, found=%d tcp_write_closed=%d closing=%d detached=%d\n",
                FindActiveRelayState(afterFin, registered.RelayId, nullptr) ? 1 : 0,
                relayState.TcpWriteClosed ? 1 : 0,
                relayState.Closing ? 1 : 0,
                relayState.StreamDetached ? 1 : 0);
            worker.Stop();
            ::close(fds[1]);
            MsQuic = nullptr;
            return 4113;
        }

        linger rst{};
        rst.l_onoff = 1;
        rst.l_linger = 0;
        (void)::setsockopt(fds[1], SOL_SOCKET, SO_LINGER, &rst, sizeof(rst));
        ::close(fds[1]);
        fds[1] = -1;

        const uint64_t beforeShutdownCalls = ReadFakeStreamShutdownCalls();
        if (!worker.DispatchEncodedEpollEventForTest(
                worker.EncodeEpollRelayForTest(registered.RelayId),
                EPOLLERR | EPOLLHUP | EPOLLRDHUP)) {
            worker.Stop();
            if (fds[1] >= 0) {
                ::close(fds[1]);
            }
            MsQuic = nullptr;
            return 4114;
        }

        if (ReadFakeStreamShutdownCalls() != beforeShutdownCalls) {
            std::fprintf(stderr, "late TCP error after tcp_write_shutdown_complete must not abort stream\n");
            worker.Stop();
            if (fds[1] >= 0) {
                ::close(fds[1]);
            }
            MsQuic = nullptr;
            return 4115;
        }

        worker.Stop();
        if (fds[1] >= 0) {
            ::close(fds[1]);
        }
        MsQuic = nullptr;
    }

    {
        QUIC_API_TABLE fakeApi{};
        InstallFakeMsQuicForSend(fakeApi);
        ResetFakeStreamShutdownCalls();

        TqLinuxRelayWorkerConfig config{};
        config.EventBudget = 128;
        TqLinuxRelayWorker worker(config);
        if (!worker.StartForTest()) {
            MsQuic = nullptr;
            return 4116;
        }

        int fds[2]{-1, -1};
        if (!MakeTcpLoopbackPair(fds)) {
            worker.Stop();
            MsQuic = nullptr;
            return 4117;
        }

        alignas(MsQuicStream) uint8_t fakeStreamStorage[sizeof(MsQuicStream)]{};
        std::memset(fakeStreamStorage, 0, sizeof(fakeStreamStorage));
        MsQuicStream* fakeStream = reinterpret_cast<MsQuicStream*>(fakeStreamStorage);
        fakeStream->Handle = reinterpret_cast<HQUIC>(static_cast<uintptr_t>(0x5678));

        TqRelayHandle handle{};
        TqLinuxRelayRegistration registration{};
        registration.TcpFd = fds[0];
        registration.Stream = fakeStream;
        registration.Handle = &handle;
        registration.EnableQuicSends = false;
        const TqLinuxRelayRegistrationResult registered = worker.RegisterRelayWithId(registration);
        if (!registered.Ok) {
            worker.Stop();
            ::close(fds[1]);
            MsQuic = nullptr;
            return 4118;
        }

        linger rst{};
        rst.l_onoff = 1;
        rst.l_linger = 0;
        (void)::setsockopt(fds[1], SOL_SOCKET, SO_LINGER, &rst, sizeof(rst));
        ::close(fds[1]);
        fds[1] = -1;

        if (!worker.DispatchEncodedEpollEventForTest(
                worker.EncodeEpollRelayForTest(registered.RelayId),
                EPOLLERR | EPOLLHUP | EPOLLRDHUP)) {
            worker.Stop();
            if (fds[1] >= 0) {
                ::close(fds[1]);
            }
            MsQuic = nullptr;
            return 4119;
        }

        if (ReadFakeStreamShutdownCalls() != 1) {
            std::fprintf(stderr, "active stream hard TCP error must abort stream exactly once\n");
            worker.Stop();
            if (fds[1] >= 0) {
                ::close(fds[1]);
            }
            MsQuic = nullptr;
            return 4120;
        }

        worker.Stop();
        if (fds[1] >= 0) {
            ::close(fds[1]);
        }
        MsQuic = nullptr;
    }

    {
        TqLinuxRelayWorkerConfig config{};
        config.EventBudget = 256;
        config.ReadChunkSize = 1024;
        config.ReadBatchBytes = 4096;
        config.MaxIov = 4;
        config.MaxPendingBufferBytes = 256 * 1024;

        TqLinuxRelayWorker worker(config);
        if (!worker.StartForTest()) {
            return 1;
        }

        constexpr int kRelayCount = 64;
        std::vector<std::array<int, 2>> fds(
            kRelayCount,
            std::array<int, 2>{-1, -1});
        std::vector<TqLinuxRelayRegistrationResult> registrations;
        registrations.reserve(kRelayCount);

        auto cleanupRelays = [&]() {
            for (const auto& registration : registrations) {
                worker.UnregisterRelay(registration.RelayId);
            }
            for (int i = 0; i < kRelayCount; ++i) {
                if (fds[i][1] >= 0) {
                    ::close(fds[i][1]);
                    fds[i][1] = -1;
                }
            }
            worker.Stop();
        };

        for (int i = 0; i < kRelayCount; ++i) {
            fds[i] = std::array<int, 2>{-1, -1};
            if (::socketpair(AF_UNIX, SOCK_STREAM, 0, fds[i].data()) != 0) {
                cleanupRelays();
                return 1;
            }

            TqLinuxRelayRegistration registration{};
            registration.TcpFd = fds[i][0];
            registration.Stream = nullptr;
            registration.Handle = nullptr;
            registration.EnableQuicSends = false;

            const auto result = worker.RegisterRelayWithId(registration);
            if (!result.Ok) {
                ::close(fds[i][0]);
                fds[i][0] = -1;
                ::close(fds[i][1]);
                fds[i][1] = -1;
                cleanupRelays();
                return 1;
            }
            registrations.push_back(result);
        }
        if (!worker.RelayIndexesConsistentForTest()) {
            cleanupRelays();
            return 1;
        }

        const char payload[] = "relay-index-hit";
        if (::write(fds[kRelayCount - 1][1], payload, sizeof(payload)) !=
            static_cast<ssize_t>(sizeof(payload))) {
            cleanupRelays();
            return 1;
        }
        if (!worker.DispatchTcpEventsForTest(
                registrations[kRelayCount - 1].RelayId,
                EPOLLIN)) {
            cleanupRelays();
            return 1;
        }

        const TqLinuxRelayWorkerSnapshot snapshot = worker.Snapshot();
        if (snapshot.ActiveRelays != kRelayCount ||
            snapshot.TcpReadBytes < sizeof(payload)) {
            std::fprintf(stderr,
                "expected relay-index snapshot, relays=%llu tcp_read=%llu\n",
                static_cast<unsigned long long>(snapshot.ActiveRelays),
                static_cast<unsigned long long>(snapshot.TcpReadBytes));
            cleanupRelays();
            return 3201;
        }

        bool sawFirst = false;
        bool sawLast = false;
        for (const auto& active : snapshot.ActiveRelayStates) {
            if (active.WorkerIndex != config.WorkerIndex) {
                std::fprintf(stderr, "unexpected worker index %u\n", active.WorkerIndex);
                cleanupRelays();
                return 3203;
            }
            if (active.RelayId == registrations.front().RelayId) {
                sawFirst = true;
            }
            if (active.RelayId == registrations.back().RelayId) {
                sawLast = true;
            }
        }
        if (!sawFirst || !sawLast) {
            std::fprintf(stderr, "active relay states missing registered relay ids\n");
            cleanupRelays();
            return 3204;
        }

        for (const auto& registration : registrations) {
            worker.UnregisterRelay(registration.RelayId);
        }
        for (int i = 0; i < kRelayCount; ++i) {
            if (fds[i][1] >= 0) {
                ::close(fds[i][1]);
                fds[i][1] = -1;
            }
        }
        if (!worker.RelayIndexesConsistentForTest()) {
            worker.Stop();
            return 1;
        }
        worker.Stop();
    }

    {
        TqLinuxRelayWorkerConfig config{};
        config.EventBudget = 256;
        config.ReadChunkSize = 1024;
        config.ReadBatchBytes = 4096;
        config.MaxIov = 4;
        config.MaxPendingBufferBytes = 256 * 1024;

        TqLinuxRelayWorker worker(config);
        assert(worker.Start());

        constexpr int kThreadCount = 8;
        constexpr int kIterationsPerThread = 8;
        std::vector<std::thread> threads;
        threads.reserve(kThreadCount);
        for (int t = 0; t < kThreadCount; ++t) {
            threads.emplace_back([&worker]() {
                for (int i = 0; i < kIterationsPerThread; ++i) {
                    int fds[2]{-1, -1};
                    assert(::socketpair(AF_UNIX, SOCK_STREAM, 0, fds) == 0);

                    TqLinuxRelayRegistration registration{};
                    registration.TcpFd = fds[0];
                    registration.Stream = nullptr;
                    registration.Handle = nullptr;
                    registration.EnableQuicSends = false;
                    const auto result = worker.RegisterRelayWithId(registration);
                    if (!result.Ok) {
                        ::close(fds[0]);
                        ::close(fds[1]);
                        assert(result.Ok);
                        continue;
                    }

                    const TqLinuxRelayWorkerSnapshot snapshot = worker.Snapshot();
                    if (snapshot.ActiveRelays == 0) {
                        worker.UnregisterRelay(result.RelayId);
                        ::close(fds[1]);
                        assert(snapshot.ActiveRelays > 0);
                        continue;
                    }
                    assert(snapshot.ActiveRelays > 0);
                    worker.UnregisterRelay(result.RelayId);
                    ::close(fds[1]);
                }
            });
        }
        for (auto& thread : threads) {
            thread.join();
        }

        const TqLinuxRelayWorkerSnapshot snapshot = worker.Snapshot();
        assert(snapshot.ActiveRelays == 0);
        assert(worker.RelayIndexesConsistentForTest());
        worker.Stop();
    }

    {
        TqTuningConfig tuning{};
        tuning.RelayWorkerCount = 2;
        tuning.RelayWorkerEventBudget = 128;
        tuning.RelayReadChunkSize = 4096;
        tuning.RelayReadBatchBytes = 16 * 1024;
        tuning.RelayMaxIov = 4;
        tuning.MaxPendingBufferBytesPerRelay = 64 * 1024;
        if (!TqLinuxRelayRuntime::Instance().Start(tuning)) {
            return 1;
        }
        int runtimeFds[2]{-1, -1};
        if (::socketpair(AF_UNIX, SOCK_STREAM, 0, runtimeFds) != 0) {
            TqLinuxRelayRuntime::Instance().Stop();
            return 1;
        }
        TqLinuxRelayWorker* runtimeWorker = TqLinuxRelayRuntime::Instance().PickWorker();
        if (runtimeWorker == nullptr) {
            TqLinuxRelayRuntime::Instance().Stop();
            ::close(runtimeFds[0]);
            ::close(runtimeFds[1]);
            return 1;
        }
        TqLinuxRelayRegistration runtimeRegistration{};
        runtimeRegistration.TcpFd = runtimeFds[0];
        runtimeRegistration.Stream = nullptr;
        runtimeRegistration.Handle = nullptr;
        runtimeRegistration.EnableQuicSends = false;
        const auto runtimeRelay = runtimeWorker->RegisterRelayWithId(runtimeRegistration);
        if (!runtimeRelay.Ok) {
            TqLinuxRelayRuntime::Instance().Stop();
            ::close(runtimeFds[0]);
            ::close(runtimeFds[1]);
            return 1;
        }
        const auto snapshots = TqLinuxRelayRuntime::Instance().SnapshotWorkers();
        if (!snapshots.SnapshotComplete || !snapshots.IdentitiesComplete ||
            snapshots.Workers.size() != 2) {
            runtimeWorker->UnregisterRelay(runtimeRelay.RelayId);
            ::close(runtimeFds[1]);
            TqLinuxRelayRuntime::Instance().Stop();
            return 1;
        }
        if (snapshots.Workers[0].WorkerIndex != 0 ||
            snapshots.Workers[1].WorkerIndex != 1) {
            runtimeWorker->UnregisterRelay(runtimeRelay.RelayId);
            ::close(runtimeFds[1]);
            TqLinuxRelayRuntime::Instance().Stop();
            return 1;
        }
        const auto aggregate = TqLinuxRelayRuntime::Instance().Snapshot();
        if (aggregate.ActiveRelays != 1 ||
            aggregate.SnapshotActiveRelaysScanned != 1 ||
            aggregate.ActiveRelayStates.size() != 1 ||
            aggregate.ActiveRelayStates.front().RelayId != runtimeRelay.RelayId ||
            aggregate.ActiveRelayStates.front().WorkerIndex != 0) {
            std::fprintf(
                stderr,
                "aggregate active states mismatch active=%llu scanned=%llu states=%zu relay=%llu worker=%u\n",
                static_cast<unsigned long long>(aggregate.ActiveRelays),
                static_cast<unsigned long long>(aggregate.SnapshotActiveRelaysScanned),
                aggregate.ActiveRelayStates.size(),
                aggregate.ActiveRelayStates.empty()
                    ? 0ull
                    : static_cast<unsigned long long>(aggregate.ActiveRelayStates.front().RelayId),
                aggregate.ActiveRelayStates.empty()
                    ? 0u
                    : aggregate.ActiveRelayStates.front().WorkerIndex);
            runtimeWorker->UnregisterRelay(runtimeRelay.RelayId);
            ::close(runtimeFds[1]);
            TqLinuxRelayRuntime::Instance().Stop();
            return 3298;
        }

        bool producerAFailed = false;
        bool producerBFailed = false;
        std::thread producerA([runtimeWorker, &producerAFailed]() {
            for (uint64_t i = 0; i < 16; ++i) {
                TqLinuxRelayEvent event{};
                event.Type = TqLinuxRelayEventType::TestMarker;
                event.Value = i;
                if (!runtimeWorker->EnqueueForTest(std::move(event))) {
                    producerAFailed = true;
                    return;
                }
            }
        });
        std::thread producerB([runtimeWorker, &producerBFailed]() {
            for (uint64_t i = 0; i < 16; ++i) {
                TqLinuxRelayEvent event{};
                event.Type = TqLinuxRelayEventType::TestMarker;
                event.Value = 1000 + i;
                if (!runtimeWorker->EnqueueForTest(std::move(event))) {
                    producerBFailed = true;
                    return;
                }
            }
        });
        producerA.join();
        producerB.join();
        if (producerAFailed || producerBFailed) {
            runtimeWorker->UnregisterRelay(runtimeRelay.RelayId);
            ::close(runtimeFds[1]);
            TqLinuxRelayRuntime::Instance().Stop();
            return 3299;
        }
        const auto producerAggregate = TqLinuxRelayRuntime::Instance().Snapshot();
        if (producerAggregate.EventProducerThreadsObserved < 2 ||
            !producerAggregate.MultipleEventProducerThreadsObserved) {
            std::fprintf(
                stderr,
                "runtime producer aggregate mismatch observed=%llu multiple=%u\n",
                static_cast<unsigned long long>(
                    producerAggregate.EventProducerThreadsObserved),
                producerAggregate.MultipleEventProducerThreadsObserved ? 1u : 0u);
            runtimeWorker->UnregisterRelay(runtimeRelay.RelayId);
            ::close(runtimeFds[1]);
            TqLinuxRelayRuntime::Instance().Stop();
            return 3300;
        }
        runtimeWorker->UnregisterRelay(runtimeRelay.RelayId);
        ::close(runtimeFds[1]);
        TqLinuxRelayRuntime::Instance().Stop();
    }

    {
        TqLinuxRelayRuntime::Instance().Stop();
        TqTuningConfig tuning{};
        tuning.RelayWorkerCount = 1;
        tuning.RelayEventQueueCapacity = 1024;
        if (!TqLinuxRelayRuntime::Instance().Start(tuning)) return 3410;
        std::atomic<bool> stop{false};
        std::thread snapshots([&]() {
            for (uint32_t i = 0; i < 100 && !stop.load(std::memory_order_acquire); ++i) {
                (void)TqLinuxRelayRuntime::Instance().Snapshot();
            }
        });
        bool pickOk = true;
        for (uint32_t i = 0; i < 100; ++i) {
            if (TqLinuxRelayRuntime::Instance().PickWorker() == nullptr) {
                pickOk = false;
                break;
            }
        }
        stop.store(true, std::memory_order_release);
        snapshots.join();
        const auto snapshot = TqLinuxRelayRuntime::Instance().Snapshot();
        TqLinuxRelayRuntime::Instance().Stop();
        if (!pickOk) return 3411;
        if (snapshot.RuntimeLockAcquireCount == 0) return 3412;
    }

    // Start publishes a complete worker generation or nothing at all.  A
    // failed worker must not leave a partially started runtime behind.
    {
        auto& runtime = TqLinuxRelayRuntime::Instance();
        runtime.Stop();
        runtime.SetFailStartWorkerIndexForTest(1);
        TqTuningConfig tuning{};
        tuning.RelayWorkerCount = 2;
        if (runtime.Start(tuning)) return 3413;
        const auto stopped = runtime.SnapshotWorkers();
        if (!stopped.SnapshotComplete || !stopped.IdentitiesComplete ||
            !stopped.Workers.empty()) return 3414;

        runtime.SetFailStartWorkerIndexForTest(-1);
        if (!runtime.Start(tuning)) return 3415;
        const auto restarted = runtime.SnapshotWorkers();
        runtime.Stop();
        if (!restarted.SnapshotComplete || !restarted.IdentitiesComplete ||
            restarted.Workers.size() != 2 ||
            restarted.Workers[0].WorkerIndex != 0 ||
            restarted.Workers[1].WorkerIndex != 1) return 3416;
    }

    // All workers consume one runtime deadline.  The blocked second worker
    // returns an incomplete, but still slot-identifiable, row rather than
    // extending the request by another per-worker timeout.
    {
        auto& runtime = TqLinuxRelayRuntime::Instance();
        runtime.Stop();
        {
            std::lock_guard<std::mutex> lock(g_RuntimeSnapshotHookMutex);
            g_RuntimeSnapshotHookEntered = false;
            g_RuntimeSnapshotHookRelease = false;
        }
        runtime.SetBeforeWorkerSnapshotHookForTest(BlockSecondRuntimeWorkerSnapshot);
        TqTuningConfig tuning{};
        tuning.RelayWorkerCount = 2;
        if (!runtime.Start(tuning)) return 3417;

        std::mutex resultMutex;
        std::condition_variable resultCv;
        bool resultReady = false;
        TqRelayRuntimeSnapshotResult<TqLinuxRelayWorkerSnapshot> result{};
        const auto started = std::chrono::steady_clock::now();
        std::thread snapshotThread([&]() {
            const auto sampled = runtime.SnapshotWorkers(
                std::chrono::steady_clock::now() + std::chrono::milliseconds(100));
            {
                std::lock_guard<std::mutex> lock(resultMutex);
                result = sampled;
                resultReady = true;
            }
            resultCv.notify_all();
        });

        bool hookEntered = false;
        {
            std::unique_lock<std::mutex> lock(g_RuntimeSnapshotHookMutex);
            hookEntered = g_RuntimeSnapshotHookCv.wait_for(lock, std::chrono::seconds(2), []() {
                    return g_RuntimeSnapshotHookEntered;
                });
        }
        bool completed = false;
        {
            std::unique_lock<std::mutex> lock(resultMutex);
            completed = resultCv.wait_for(lock, std::chrono::seconds(2), [&]() {
                    return resultReady;
                });
        }
        const auto elapsed = std::chrono::steady_clock::now() - started;
        {
            std::lock_guard<std::mutex> lock(g_RuntimeSnapshotHookMutex);
            g_RuntimeSnapshotHookRelease = true;
        }
        g_RuntimeSnapshotHookCv.notify_all();
        snapshotThread.join();
        runtime.Stop();
        runtime.SetBeforeWorkerSnapshotHookForTest(nullptr);

        if (!hookEntered) return 3418;
        if (!completed) return 3419;
        if (elapsed >= std::chrono::seconds(1) || result.SnapshotComplete ||
            !result.IdentitiesComplete || result.Workers.size() != 2 ||
            result.Workers[0].WorkerIndex != 0 || result.Workers[1].WorkerIndex != 1 ||
            result.Workers[1].SnapshotComplete) return 3420;
    }

    // prepare failure leaves fd ownership with the caller and publishes no relay.
    {
        TqLinuxRelayWorkerConfig config{};
        config.FailPrepareForTest = true;
        TqLinuxRelayWorker worker(config);
        if (!worker.StartForTest()) return 5001;
        int fds[2]{-1, -1};
        if (::socketpair(AF_UNIX, SOCK_STREAM, 0, fds) != 0) return 5002;
        auto owner = TqStreamLifetime::CreateForTest(TqStreamLifetime::Phase::Started);
        alignas(MsQuicStream) uint8_t storage[sizeof(MsQuicStream)]{};
        TqLinuxRelayRegistration registration{};
        registration.TcpFd = fds[0];
        registration.Stream = reinterpret_cast<MsQuicStream*>(storage);
        registration.StreamOwner = owner;
        const auto result = worker.RegisterRelayWithId(registration);
        if (result.Ok || result.TcpFdConsumed || ::fcntl(fds[0], F_GETFD) < 0 ||
            worker.Snapshot().ActiveRelays != 0) return 5003;
        QUIC_STREAM_EVENT terminal{};
        terminal.Type = QUIC_STREAM_EVENT_SHUTDOWN_COMPLETE;
        (void)owner->DispatchForTest(&terminal);
        worker.Stop();
        ::close(fds[0]);
        ::close(fds[1]);
    }

    // receive between publish/commit is bounded and invisible; commit failure
    // consumes/closes the old fd exactly once and cannot later close its reused number.
    {
        TqLinuxRelayWorkerConfig config{};
        config.MaxPendingQuicReceiveBytesPerRelay = g_PrecommitPayload.size();
        config.FailCommitForTest = true;
        config.AfterPublishHookForTest = AfterManagedPublishHook;
        TqLinuxRelayWorker worker(config);
        if (!worker.StartForTest()) return 5004;
        int fds[2]{-1, -1};
        if (::socketpair(AF_UNIX, SOCK_STREAM, 0, fds) != 0) return 5005;
        const int consumedFd = fds[0];
        g_PrecommitOwner = TqStreamLifetime::CreateForTest(
            TqStreamLifetime::Phase::Started);
        g_PrecommitReceiveStatus = QUIC_STATUS_INTERNAL_ERROR;
        g_PrecommitSnapshotHidden = false;
        g_PrecommitPublishTerminal = false;
        alignas(MsQuicStream) uint8_t storage[sizeof(MsQuicStream)]{};
        TqLinuxRelayRegistration registration{};
        registration.TcpFd = consumedFd;
        registration.Stream = reinterpret_cast<MsQuicStream*>(storage);
        registration.StreamOwner = g_PrecommitOwner;
        const auto result = worker.RegisterRelayWithId(registration);
        if (result.Ok || !result.TcpFdConsumed || !g_PrecommitSnapshotHidden ||
            g_PrecommitReceiveStatus != QUIC_STATUS_PENDING ||
            ::fcntl(consumedFd, F_GETFD) != -1 || errno != EBADF) return 5006;

        int reused[2]{-1, -1};
        if (::socketpair(AF_UNIX, SOCK_STREAM, 0, reused) != 0) return 5007;
        QUIC_STREAM_EVENT terminal{};
        terminal.Type = QUIC_STREAM_EVENT_SHUTDOWN_COMPLETE;
        (void)g_PrecommitOwner->DispatchForTest(&terminal);
        g_PrecommitOwner.reset();
        (void)worker.DrainForTest(128);
        worker.Stop();
        if (::fcntl(reused[0], F_GETFD) < 0 || ::fcntl(reused[1], F_GETFD) < 0) return 5008;
        ::close(fds[1]);
        ::close(reused[0]);
        ::close(reused[1]);
    }

    // terminal at the publish boundary wins before epoll commit and still consumes fd.
    {
        TqLinuxRelayWorkerConfig config{};
        config.AfterPublishHookForTest = AfterManagedPublishHook;
        TqLinuxRelayWorker worker(config);
        if (!worker.StartForTest()) return 5009;
        int fds[2]{-1, -1};
        if (::socketpair(AF_UNIX, SOCK_STREAM, 0, fds) != 0) return 5010;
        g_PrecommitOwner = TqStreamLifetime::CreateForTest(
            TqStreamLifetime::Phase::Started);
        g_PrecommitPublishTerminal = true;
        g_PrecommitSnapshotHidden = false;
        alignas(MsQuicStream) uint8_t storage[sizeof(MsQuicStream)]{};
        TqLinuxRelayRegistration registration{};
        registration.TcpFd = fds[0];
        registration.Stream = reinterpret_cast<MsQuicStream*>(storage);
        registration.StreamOwner = g_PrecommitOwner;
        const auto result = worker.RegisterRelayWithId(registration);
        if (result.Ok || !result.TcpFdConsumed || !g_PrecommitSnapshotHidden ||
            g_PrecommitOwner->GetPhase() != TqStreamLifetime::Phase::TerminalPublished) return 5011;
        g_PrecommitOwner.reset();
        (void)worker.DrainForTest(128);
        worker.Stop();
        ::close(fds[1]);
        g_PrecommitPublishTerminal = false;
    }

    // A full normal event queue uses the intrusive terminal fallback lane.
    // This case uses a normally constructed CleanUpManual wrapper and verifies
    // that callback/context remain the stable router until wrapper close.
    {
        QUIC_API_TABLE fakeApi{};
        InstallFakeMsQuicForSend(fakeApi);
        g_FakeStreamCloseCalls = 0;
        TqLinuxRelayWorkerConfig config{};
        config.EventQueueCapacity = 2;
        TqLinuxRelayWorker worker(config);
        if (!worker.StartForTest()) return 5020;
        int fds[2]{-1, -1};
        if (::socketpair(AF_UNIX, SOCK_STREAM, 0, fds) != 0) return 5021;

        const HQUIC raw = reinterpret_cast<HQUIC>(static_cast<uintptr_t>(0x9876));
        auto owner = TqStreamLifetime::AdoptAccepted(raw, nullptr);
        if (owner == nullptr) return 5022;
        MsQuicStream* stream = owner->StreamForInitialization();
        const auto originalCallback = stream->Callback;
        void* const originalContext = stream->Context;
        auto control = std::make_shared<TqRelayStopControl>();
        TqLinuxRelayRegistration registration{};
        registration.TcpFd = fds[0];
        registration.Stream = stream;
        registration.StreamOwner = owner;
        registration.StopControl = control;
        registration.ControlGeneration = control->Generation;
        const auto registered = worker.RegisterRelayWithId(registration);
        if (!registered.Ok || !registered.TcpFdConsumed) return 5023;

        const uint64_t mismatchesBefore =
            TqRelayControlGenerationMismatchCount().load(std::memory_order_relaxed);
        TqLinuxRelayEvent staleTerminal{};
        staleTerminal.Type = TqLinuxRelayEventType::QuicShutdownComplete;
        staleTerminal.RelayId = registered.RelayId;
        staleTerminal.Generation = control->Generation + 1;
        if (!worker.EnqueueForTest(std::move(staleTerminal))) return 5029;
        (void)worker.DrainForTest(4);
        if (TqRelayControlGenerationMismatchCount().load(std::memory_order_relaxed) !=
                mismatchesBefore + 1 ||
            owner->GetPhase() != TqStreamLifetime::Phase::Started) return 5030;

        for (int i = 0; i < 2; ++i) {
            TqLinuxRelayEvent marker{};
            marker.Type = TqLinuxRelayEventType::TestMarker;
            if (!worker.EnqueueForTest(std::move(marker))) return 5024;
        }
        QUIC_STREAM_EVENT terminal{};
        terminal.Type = QUIC_STREAM_EVENT_SHUTDOWN_COMPLETE;
        if (QUIC_FAILED(stream->Callback(stream, stream->Context, &terminal))) return 5025;
        if (stream->Callback != originalCallback || stream->Context != originalContext ||
            owner->GetPhase() != TqStreamLifetime::Phase::TerminalPublished) return 5026;

        (void)worker.DrainForTest(16);
        (void)worker.DrainForTest(16);
        if (!control->Stop.load(std::memory_order_acquire) ||
            worker.RetiredBindingCountForTest() != 0 ||
            ::fcntl(fds[0], F_GETFD) != -1 || errno != EBADF) return 5027;
        owner.reset();
        if (g_FakeStreamCloseCalls != 1) return 5028;
        worker.Stop();
        ::close(fds[1]);
        MsQuic = nullptr;
    }

    return 0;
}
