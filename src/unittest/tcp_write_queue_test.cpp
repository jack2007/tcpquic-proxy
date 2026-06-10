// Copyright (c) Microsoft Corporation.
// Licensed under the MIT License.

#include "tcp_write_queue.h"

#include <cassert>
#include <sys/socket.h>
#include <unistd.h>
#include <vector>

int main() {
    {
        int fds[2]{-1, -1};
        assert(socketpair(AF_UNIX, SOCK_STREAM, 0, fds) == 0);
        ::close(fds[1]); // peer closed → write fails

        std::atomic<bool> stopped{false};
        TqTcpWriteQueue q(fds[0], &stopped, 16, 1 << 20);
        assert(q.Start());

        std::vector<uint8_t> payload(128, 0xAB);
        assert(q.Enqueue(payload.data(), payload.size(), false));
        assert(q.WaitUntilDrainedOrStopped(2000));

        assert(stopped.load());
        q.Stop();
        ::close(fds[0]);
    }

    {
        int fds[2]{-1, -1};
        assert(socketpair(AF_UNIX, SOCK_STREAM, 0, fds) == 0);

        std::atomic<bool> stopped{false};
        TqTcpWriteQueue q(fds[0], &stopped, 16, 1 << 20);
        assert(q.Start());
        q.Stop();

        std::vector<uint8_t> payload(16, 0xCD);
        assert(!q.Enqueue(payload.data(), payload.size(), false));

        ::close(fds[0]);
        ::close(fds[1]);
    }

    return 0;
}
