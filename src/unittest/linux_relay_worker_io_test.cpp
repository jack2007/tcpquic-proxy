#include "../compress.h"
#include "../linux_relay_worker.h"

#include <cassert>
#include <cstdint>
#include <cstring>
#include <sys/socket.h>
#include <unistd.h>
#include <vector>

int main() {
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

        std::vector<uint8_t> payload(3000, 0x5A);
        assert(::write(fds[1], payload.data(), payload.size()) == static_cast<ssize_t>(payload.size()));
        assert(worker.WaitForObservedTcpBytesForTest(payload.size(), 2000));

        TqLinuxRelayWorkerSnapshot snapshot = worker.Snapshot();
        assert(snapshot.TcpReadBytes >= payload.size());
        assert(snapshot.QuicSendOperations == 0);
        assert(snapshot.MaxTcpReadIovUsed >= 2);
        assert(snapshot.PendingBytes == 0);

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

    return 0;
}
