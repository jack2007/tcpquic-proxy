// Copyright (c) Microsoft Corporation.
// Licensed under the MIT License.

#include "tcp_write_queue.h"

#include <cassert>
#include <vector>

int main() {
    TqSocketStartup startup;
    assert(startup.Ok());

    {
        TqSocketHandle fds[2]{TqInvalidSocket, TqInvalidSocket};
        assert(TqSocketPair(fds));
        TqCloseSocket(fds[1]); // peer closed → write fails

        std::atomic<bool> stopped{false};
        TqTcpWriteQueue q(fds[0], &stopped, 16, 1 << 20);
        assert(q.Start());

        std::vector<uint8_t> payload(128, 0xAB);
        assert(q.Enqueue(payload.data(), payload.size(), false));
        assert(q.WaitUntilDrainedOrStopped(2000));

        assert(stopped.load());
        q.Stop();
        TqCloseSocket(fds[0]);
    }

    {
        TqSocketHandle fds[2]{TqInvalidSocket, TqInvalidSocket};
        assert(TqSocketPair(fds));

        std::atomic<bool> stopped{false};
        TqTcpWriteQueue q(fds[0], &stopped, 16, 1 << 20);
        assert(q.Start());
        q.Stop();

        std::vector<uint8_t> payload(16, 0xCD);
        assert(!q.Enqueue(payload.data(), payload.size(), false));

        TqCloseSocket(fds[0]);
        TqCloseSocket(fds[1]);
    }

    return 0;
}
