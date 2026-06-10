// Copyright (c) Microsoft Corporation.
// Licensed under the MIT License.

#include "../thread_pool.h"

#include <atomic>
#include <chrono>
#include <thread>

int main() {
    TqThreadPool pool(2);
    pool.Start();

    std::atomic<int> counter{0};
    for (int i = 0; i < 4; ++i) {
        if (!pool.Submit([&counter]() {
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
                counter.fetch_add(1, std::memory_order_relaxed);
            })) {
            pool.Stop();
            return 10 + i;
        }
    }

    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (counter.load(std::memory_order_relaxed) < 4 &&
           std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }

    if (counter.load(std::memory_order_relaxed) != 4) {
        pool.Stop();
        return 100 + counter.load(std::memory_order_relaxed);
    }

    pool.Stop();
    if (pool.Submit([]() {})) {
        return 201;
    }
    return 0;
}
