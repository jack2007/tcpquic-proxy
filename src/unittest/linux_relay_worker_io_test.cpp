#include "compress.h"
#include "linux_relay_worker.h"

#include <cassert>
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <sys/socket.h>
#include <csignal>
#include <cerrno>
#include <thread>
#include <unistd.h>
#include <vector>

int main() {
    (void)::signal(SIGPIPE, SIG_IGN);
    TqLinuxRelayWorkerConfig config{};
    config.EventBudget = 128;
    config.ReadChunkSize = 4096;
    config.ReadBatchBytes = 16 * 1024;
    config.MaxIov = 4;
    config.MaxPendingBytes = 64 * 1024;

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
        config.ReadChunkSize = 4096;
        config.ReadBatchBytes = 64 * 1024;
        config.MaxIov = 8;
        config.MaxPendingBytes = 256 * 1024;
        config.InlineSendmsgMaxCalls = 1;
        config.InlineWriteByteBudget = 4096;

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
        const auto registered = worker.RegisterRelayWithId(registration);
        if (!registered.Ok) {
            worker.Stop();
            ::close(fds[1]);
            return 1;
        }
        const std::vector<uint8_t> plain(64 * 1024, 0x4E);
        QUIC_BUFFER quicBuffer{};
        quicBuffer.Buffer = const_cast<uint8_t*>(plain.data());
        quicBuffer.Length = static_cast<uint32_t>(plain.size());

        QUIC_STREAM_EVENT receiveEvent{};
        receiveEvent.Type = QUIC_STREAM_EVENT_RECEIVE;
        receiveEvent.RECEIVE.BufferCount = 1;
        receiveEvent.RECEIVE.Buffers = &quicBuffer;

        const QUIC_STATUS status = worker.DispatchStreamEventForTest(fakeStream, &receiveEvent);
        if (status != QUIC_STATUS_PENDING) {
            std::fprintf(stderr, "expected budget-limited receive PENDING, got %d\n", status);
            worker.Stop();
            ::close(fds[1]);
            return 1;
        }

        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
        TqLinuxRelayWorkerSnapshot snapshot{};
        do {
            snapshot = worker.Snapshot();
            if (snapshot.TcpWriteBytes >= plain.size() && snapshot.PendingEvents == 0) {
                break;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        } while (std::chrono::steady_clock::now() < deadline);

        if (snapshot.InlineBudgetExceededCount != 1 ||
            snapshot.InlineSendmsgCalls != 0 ||
            snapshot.InlineWriteBytes != 0 ||
            snapshot.DeferredReceiveCompleteBytes != plain.size() ||
            snapshot.TcpWriteBytes != plain.size() ||
            snapshot.QuicReceiveBytesBuckets[1] != 1 ||
            snapshot.PendingEvents != 0) {
            std::fprintf(stderr,
                "expected whole receive deferred on budget overflow, budget=%llu calls=%llu inline=%llu complete=%llu tcp=%llu recv_bucket=%llu events=%llu\n",
                static_cast<unsigned long long>(snapshot.InlineBudgetExceededCount),
                static_cast<unsigned long long>(snapshot.InlineSendmsgCalls),
                static_cast<unsigned long long>(snapshot.InlineWriteBytes),
                static_cast<unsigned long long>(snapshot.DeferredReceiveCompleteBytes),
                static_cast<unsigned long long>(snapshot.TcpWriteBytes),
                static_cast<unsigned long long>(snapshot.QuicReceiveBytesBuckets[1]),
                static_cast<unsigned long long>(snapshot.PendingEvents));
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
        config.MaxPendingBytes = 64 * 1024;

        TqLinuxRelayWorker worker(config);
        if (!worker.Start()) return 83;

        int fds[2]{-1, -1};
        if (::socketpair(AF_UNIX, SOCK_STREAM, 0, fds) != 0) return 84;

        TqLinuxRelayRegistration registration{};
        registration.TcpFd = fds[0];
        registration.Stream = nullptr;
        registration.Handle = nullptr;
        registration.EnableQuicSends = false;
        if (!worker.RegisterRelayForTest(registration)) return 85;

        std::vector<uint8_t> payload(3000, 0x5A);
        if (::write(fds[1], payload.data(), payload.size()) != static_cast<ssize_t>(payload.size())) return 86;
        if (!worker.WaitForObservedTcpBytesForTest(payload.size(), 2000)) return 87;

        TqLinuxRelayWorkerSnapshot snapshot{};
        const auto sendBucketDeadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
        do {
            snapshot = worker.Snapshot();
            if (snapshot.QuicSendBytesBuckets[0] != 0) {
                break;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        } while (std::chrono::steady_clock::now() < sendBucketDeadline);
        assert(snapshot.TcpReadBytes >= payload.size());
        assert(snapshot.QuicSendOperations == 0);
        if (snapshot.QuicSendBytesBuckets[0] == 0) {
            std::fprintf(stderr,
                "expected send bucket <=16k, buckets=%llu,%llu,%llu,%llu,%llu,%llu tcp=%llu\n",
                static_cast<unsigned long long>(snapshot.QuicSendBytesBuckets[0]),
                static_cast<unsigned long long>(snapshot.QuicSendBytesBuckets[1]),
                static_cast<unsigned long long>(snapshot.QuicSendBytesBuckets[2]),
                static_cast<unsigned long long>(snapshot.QuicSendBytesBuckets[3]),
                static_cast<unsigned long long>(snapshot.QuicSendBytesBuckets[4]),
                static_cast<unsigned long long>(snapshot.QuicSendBytesBuckets[5]),
                static_cast<unsigned long long>(snapshot.TcpReadBytes));
            return 82;
        }
        assert(snapshot.MaxTcpReadIovUsed >= 2);
        assert(snapshot.PendingBytes == 0);

        worker.Stop();
        ::close(fds[1]);
    }

    {
        TqLinuxRelayWorkerConfig config{};
        config.EventBudget = 128;
        config.ReadChunkSize = 4096;
        config.ReadBatchBytes = 64 * 1024;
        config.MaxIov = 8;
        config.MaxPendingBytes = 256 * 1024;
        config.MaxPendingQuicReceiveBytesPerRelay = 32 * 1024;

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

        const QUIC_STATUS partialStatus = worker.DispatchStreamEventForTest(fakeStream, &receiveEvent);
        if (partialStatus != QUIC_STATUS_PENDING) {
            std::fprintf(stderr, "expected partial receive PENDING, got %d\n", partialStatus);
            worker.Stop();
            ::close(fds[1]);
            return 1;
        }
        (void)worker.DrainForTest(config.EventBudget);

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
        TqLinuxRelayWorkerConfig config{};
        config.EventBudget = 128;
        config.ReadChunkSize = 4096;
        config.ReadBatchBytes = 64 * 1024;
        config.MaxIov = 8;
        config.MaxPendingBytes = 256 * 1024;
        config.MaxPendingQuicReceiveBytesPerRelay = 32 * 1024;

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
        worker.Stop();

        const std::vector<uint8_t> first(2 * 1024 * 1024, 0x71);
        QUIC_BUFFER firstBuffer{};
        firstBuffer.Buffer = const_cast<uint8_t*>(first.data());
        firstBuffer.Length = static_cast<uint32_t>(first.size());

        QUIC_STREAM_EVENT firstEvent{};
        firstEvent.Type = QUIC_STREAM_EVENT_RECEIVE;
        firstEvent.RECEIVE.BufferCount = 1;
        firstEvent.RECEIVE.Buffers = &firstBuffer;

        if (worker.DispatchStreamEventForTest(fakeStream, &firstEvent) != QUIC_STATUS_PENDING) {
            ::close(fds[1]);
            return 1;
        }
        const TqLinuxRelayWorkerSnapshot paused = worker.Snapshot();
        if (paused.DeferredReceiveCompleteBytes != 0 ||
            paused.PendingEvents != 1 ||
            paused.QuicReceivePausedCount != 1) {
            std::fprintf(stderr, "expected first over-budget receive to queue and pause, complete=%llu events=%llu pauses=%llu\n",
                static_cast<unsigned long long>(paused.DeferredReceiveCompleteBytes),
                static_cast<unsigned long long>(paused.PendingEvents),
                static_cast<unsigned long long>(paused.QuicReceivePausedCount));
            ::close(fds[1]);
            return 1;
        }

        uint8_t drainBuffer[8192];
        while (::recv(fds[1], drainBuffer, sizeof(drainBuffer), MSG_DONTWAIT) > 0) {
        }

        const std::vector<uint8_t> second(1024, 0x72);
        QUIC_BUFFER secondBuffer{};
        secondBuffer.Buffer = const_cast<uint8_t*>(second.data());
        secondBuffer.Length = static_cast<uint32_t>(second.size());

        QUIC_STREAM_EVENT secondEvent{};
        secondEvent.Type = QUIC_STREAM_EVENT_RECEIVE;
        secondEvent.RECEIVE.BufferCount = 1;
        secondEvent.RECEIVE.Buffers = &secondBuffer;

        if (worker.DispatchStreamEventForTest(fakeStream, &secondEvent) != QUIC_STATUS_PENDING) {
            ::close(fds[1]);
            return 1;
        }
        const TqLinuxRelayWorkerSnapshot queued = worker.Snapshot();
        if (queued.DeferredReceiveCompleteBytes != 0 ||
            queued.PendingEvents != 2 ||
            queued.QuicReceivePausedCount != 1) {
            std::fprintf(stderr, "expected paused in-flight receive to queue only, complete=%llu:%llu events=%llu pauses=%llu\n",
                static_cast<unsigned long long>(queued.DeferredReceiveCompleteBytes),
                static_cast<unsigned long long>(paused.DeferredReceiveCompleteBytes),
                static_cast<unsigned long long>(queued.PendingEvents),
                static_cast<unsigned long long>(queued.QuicReceivePausedCount));
            ::close(fds[1]);
            return 1;
        }

        ::close(fds[1]);
    }

    {
        TqLinuxRelayWorkerConfig config{};
        config.EventBudget = 128;
        config.ReadChunkSize = 4096;
        config.ReadBatchBytes = 64 * 1024;
        config.MaxIov = 8;
        config.MaxPendingBytes = 256 * 1024;

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

        const QUIC_STATUS hardErrorStatus = worker.DispatchStreamEventForTest(fakeStream, &receiveEvent);
        if (hardErrorStatus != QUIC_STATUS_PENDING) {
            std::fprintf(stderr, "expected hard-error receive PENDING, got %d stop=%d\n",
                hardErrorStatus,
                handle.Stop.load() ? 1 : 0);
            worker.Stop();
            return 1;
        }
        if (worker.DrainForTest(config.EventBudget) != 1) {
            worker.Stop();
            return 1;
        }

        const TqLinuxRelayWorkerSnapshot snapshot = worker.Snapshot();
        if (!handle.Stop.load() || snapshot.PendingBytes != 0) {
            std::fprintf(stderr, "expected TCP hard error to stop relay and clear pending, stop=%d pending=%llu\n",
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
        config.MaxPendingBytes = 256 * 1024;

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

        QUIC_STREAM_EVENT finEvent{};
        finEvent.Type = QUIC_STREAM_EVENT_RECEIVE;
        finEvent.RECEIVE.BufferCount = 0;
        finEvent.RECEIVE.Buffers = nullptr;
        finEvent.RECEIVE.Flags = QUIC_RECEIVE_FLAG_FIN;

        if (QUIC_FAILED(worker.DispatchStreamEventForTest(fakeStream, &finEvent))) {
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

        worker.Stop();
        ::close(fds[1]);
    }

    {
        TqLinuxRelayWorkerConfig config{};
        config.EventBudget = 128;
        config.ReadChunkSize = 4096;
        config.ReadBatchBytes = 64 * 1024;
        config.MaxIov = 8;
        config.MaxPendingBytes = 256 * 1024;

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
        config.MaxPendingBytes = 256 * 1024;
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
        auto decompressor = TqCreateDecompressor(TqCompressAlgo::Zstd);
        if (!decompressor) {
            worker.Stop();
            ::close(fds[1]);
            return 1;
        }

        registration.TcpFd = fds[0];
        registration.Stream = fakeStream;
        registration.Handle = &handle;
        registration.Decompressor = decompressor.get();
        registration.CompressAlgo = TqCompressAlgo::Zstd;
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
            std::fprintf(stderr, "expected queue-full receive to abort inline with PENDING, status=%d stop=%d\n",
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
        config.MaxPendingBytes = 64 * 1024;

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

        const uint8_t first[] = {1, 2, 3, 4};
        const uint8_t second[] = {5, 6, 7, 8, 9};
        assert(worker.EnqueueQuicReceiveForTest(fds[0], first, sizeof(first), false));
        assert(worker.EnqueueQuicReceiveForTest(fds[0], second, sizeof(second), true));

        uint8_t output[16]{};
        const ssize_t received = ::read(fds[1], output, sizeof(output));
        assert(received == 9);
        for (int i = 0; i < 9; ++i) {
            assert(output[i] == static_cast<uint8_t>(i + 1));
        }

        TqLinuxRelayWorkerSnapshot snapshot = worker.Snapshot();
        assert(snapshot.TcpWriteBatches >= 1);
        assert(snapshot.TcpWriteBytes == 9);
        assert(snapshot.MaxTcpWriteIovUsed >= 2);

        worker.Stop();
        ::close(fds[1]);
    }

    {
        TqLinuxRelayWorkerConfig config{};
        config.EventBudget = 128;
        config.ReadChunkSize = 1024;
        config.ReadBatchBytes = 4096;
        config.MaxIov = 4;
        config.MaxPendingBytes = 2048;

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
        config.MaxPendingBytes = 64 * 1024;

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
        config.ReadChunkSize = 1024;
        config.ReadBatchBytes = 4096;
        config.MaxIov = 4;
        config.MaxPendingBytes = 64 * 1024;

        TqLinuxRelayWorker worker(config);
        assert(worker.Start());

        int fds[2]{-1, -1};
        assert(::socketpair(AF_UNIX, SOCK_STREAM, 0, fds) == 0);

        auto compressor = TqCreateCompressor(TqCompressAlgo::Lz4, 1);
        auto decompressor = TqCreateDecompressor(TqCompressAlgo::Lz4);
        assert(compressor);
        assert(decompressor);

        TqLinuxRelayRegistration registration{};
        registration.TcpFd = fds[0];
        registration.Stream = nullptr;
        registration.Handle = nullptr;
        registration.Decompressor = decompressor.get();
        registration.CompressAlgo = TqCompressAlgo::Lz4;
        registration.EnableQuicSends = false;
        assert(worker.RegisterRelayForTest(registration));

        const std::vector<uint8_t> plain(2048, 0x7C);
        std::vector<uint8_t> compressed;
        assert(compressor->Compress(plain.data(), plain.size(), compressed, false));
        assert(compressor->Flush(compressed));
        assert(!compressed.empty());

        assert(worker.EnqueueQuicReceiveForTest(fds[0], compressed.data(), compressed.size(), true));

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
        config.ReadChunkSize = 512;
        config.ReadBatchBytes = 4096;
        config.MaxIov = 4;
        config.MaxPendingBytes = 64 * 1024;

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
        assert(compressor->Compress(plain.data(), plain.size(), compressed, false));
        assert(compressor->Flush(compressed));
        assert(!compressed.empty());

        QUIC_BUFFER quicBuffer{};
        quicBuffer.Buffer = compressed.data();
        quicBuffer.Length = static_cast<uint32_t>(compressed.size());

        QUIC_STREAM_EVENT receiveEvent{};
        receiveEvent.Type = QUIC_STREAM_EVENT_RECEIVE;
        receiveEvent.RECEIVE.BufferCount = 1;
        receiveEvent.RECEIVE.Buffers = &quicBuffer;
        receiveEvent.RECEIVE.Flags = QUIC_RECEIVE_FLAG_FIN;

        assert(QUIC_SUCCEEDED(worker.DispatchStreamEventForTest(fakeStream, &receiveEvent)));
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
        config.MaxPendingBytes = 256 * 1024;

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
        if (receiveStatus != QUIC_STATUS_SUCCESS) {
            std::fprintf(stderr, "expected synchronous receive status SUCCESS, got %d\n", receiveStatus);
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
        if (snapshot.BufferAcquireCount != 0) {
            std::fprintf(stderr, "expected zero buffer acquires for deferred receive, got %llu\n",
                static_cast<unsigned long long>(snapshot.BufferAcquireCount));
            worker.Stop();
            ::close(fds[1]);
            return 1;
        }
        if (snapshot.DeferredReceiveCompleteBytes != 0 ||
            snapshot.DeferredReceiveCompletes != 0) {
            std::fprintf(stderr, "expected no deferred complete on synchronous receive, got %llu/%llu\n",
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
        config.MaxPendingBytes = 256 * 1024;

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

        const QUIC_STATUS receiveStatus = worker.DispatchStreamEventForTest(fakeStream, &receiveEvent);
        if (receiveStatus != QUIC_STATUS_SUCCESS) {
            std::fprintf(stderr, "expected synchronous multi-buffer receive SUCCESS, got %d\n", receiveStatus);
            worker.Stop();
            ::close(fds[1]);
            return 1;
        }

        const TqLinuxRelayWorkerSnapshot queued = worker.Snapshot();
        if (queued.PendingEvents != 0) {
            std::fprintf(stderr, "expected synchronous multi-buffer receive to skip event queue, got %llu\n",
                static_cast<unsigned long long>(queued.PendingEvents));
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
        config.MaxPendingBytes = 128 * 1024;

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
        config.MaxPendingBytes = 128 * 1024;

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
        config.EventBudget = 256;
        config.ReadChunkSize = 1024;
        config.ReadBatchBytes = 4096;
        config.MaxIov = 4;
        config.MaxPendingBytes = 256 * 1024;

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
