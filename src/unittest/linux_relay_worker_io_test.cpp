#include "../linux_relay_worker.h"

#include <cassert>
#include <cstring>
#include <sys/socket.h>
#include <unistd.h>

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
    return 0;
}
