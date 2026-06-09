// Copyright (c) Microsoft Corporation.
// Licensed under the MIT License.

#include "../thread_pool.h"

#include <atomic>
#include <cassert>
#include <chrono>
#include <sys/socket.h>
#include <thread>
#include <unistd.h>

int main() {
    TqThreadPool pool(2);
    pool.Start();

    std::atomic<int> counter{0};
    for (int i = 0; i < 4; ++i) {
        pool.Submit([&counter]() {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
            counter.fetch_add(1, std::memory_order_relaxed);
        });
    }

    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (counter.load(std::memory_order_relaxed) < 4 &&
           std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }

    assert(counter.load(std::memory_order_relaxed) == 4);

    int fds[2]{-1, -1};
    assert(socketpair(AF_UNIX, SOCK_STREAM, 0, fds) == 0);
    pool.Stop();
    assert(!pool.Submit([fd = fds[0]]() { (void)fd; }));
    assert(::close(fds[0]) == 0);
    assert(::close(fds[1]) == 0);
    return 0;
}
