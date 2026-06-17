#include "compress.h"
#include "linux_relay_worker.h"

#include <algorithm>
#include <cassert>
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <fcntl.h>
#include <sys/socket.h>
#include <csignal>
#include <cerrno>
#include <chrono>
#include <mutex>
#include <thread>
#include <unistd.h>
#include <vector>

namespace {

std::mutex g_FakeSendMutex;
std::vector<void*> g_FakeSendContexts;

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

void InstallFakeMsQuicForSend(QUIC_API_TABLE& table) {
    std::memset(&table, 0, sizeof(table));
    table.StreamSend = FakeStreamSend;
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
        (void)worker.DispatchStreamEventForTest(stream, &complete);
    }
}

} // namespace

int main() {
    (void)::signal(SIGPIPE, SIG_IGN);
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
        if (snapshot.TcpReadClosedRelays != 1) {
            std::fprintf(stderr,
                "expected EPOLLRDHUP-only event to drain TCP EOF, read_closed=%llu\n",
                static_cast<unsigned long long>(snapshot.TcpReadClosedRelays));
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
        if (!worker.Start()) {
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
        if (!handle.Stop.load()) {
            std::fprintf(stderr, "expected TCP hard error to stop relay, stop=%d pending=%llu\n",
                handle.Stop.load() ? 1 : 0,
                static_cast<unsigned long long>(snapshot.PendingBytes));
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
        if (worker.Snapshot().PendingBytes == 0) {
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
        if (worker.DrainForTest(config.EventBudget) != 1) {
            worker.Stop();
            ::close(fds[1]);
            return 1;
        }
        if (worker.Snapshot().PendingBytes != 0) {
            std::fprintf(stderr, "expected shutdown complete to clear pending receive, got %llu\n",
                static_cast<unsigned long long>(worker.Snapshot().PendingBytes));
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
        worker.Stop();

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
        if (status != QUIC_STATUS_PENDING || !handle.Stop.load()) {
            std::fprintf(stderr, "expected queue-full receive to stop relay with PENDING, status=%d stop=%d\n",
                status,
                handle.Stop.load() ? 1 : 0);
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
        if (fakeStream->Callback != TqLinuxRelayWorker::StreamCallback ||
            fakeStream->Context == &worker ||
            fakeStream->Context == nullptr) {
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
        config.ReadChunkSize = 1024;
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
        config.EventBudget = 256;
        config.ReadChunkSize = 1024;
        config.ReadBatchBytes = 4096;
        config.MaxIov = 4;
        config.MaxPendingBufferBytes = 256 * 1024;

        TqLinuxRelayWorker worker(config);
        assert(worker.Start());

        constexpr int kRelayCount = 8;
        std::vector<std::thread> threads;
        threads.reserve(kRelayCount);
        for (int t = 0; t < kRelayCount; ++t) {
            threads.emplace_back([&worker]() {
                for (int i = 0; i < 4; ++i) {
                    int fds[2]{-1, -1};
                    if (::socketpair(AF_UNIX, SOCK_STREAM, 0, fds) != 0) {
                        continue;
                    }

                    TqLinuxRelayRegistration registration{};
                    registration.TcpFd = fds[0];
                    registration.Stream = nullptr;
                    registration.Handle = nullptr;
                    registration.EnableQuicSends = false;
                    const auto result = worker.RegisterRelayWithId(registration);
                    if (!result.Ok) {
                        ::close(fds[0]);
                        ::close(fds[1]);
                        continue;
                    }

                    const char payload[] = "stress";
                    ::write(fds[1], payload, sizeof(payload));
                    worker.UnregisterRelay(result.RelayId);
                    ::close(fds[1]);
                }
            });
        }
        for (auto& thread : threads) {
            thread.join();
        }

        worker.Stop();
    }

    return 0;
}
