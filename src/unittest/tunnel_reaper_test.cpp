// Copyright (c) Microsoft Corporation.
// Licensed under the MIT License.

#include "../tunnel_reaper.h"

#include <atomic>
#include <cassert>
#include <chrono>
#include <thread>

struct TestTunnelContext {
    std::atomic<bool> stopped{false};
};

static std::atomic<int> g_reapedCount{0};
static TestTunnelContext* g_lastReaped = nullptr;

bool TqTunnelRelayStopped(const TqTunnelContext* ctx) {
    const auto* tunnel = reinterpret_cast<const TestTunnelContext*>(ctx);
    return tunnel != nullptr && tunnel->stopped.load();
}

void TqReapTunnelContext(TqTunnelContext* ctx) {
    g_lastReaped = reinterpret_cast<TestTunnelContext*>(ctx);
    g_reapedCount.fetch_add(1, std::memory_order_relaxed);
}

static bool WaitForReap(int expected, int timeoutMs) {
    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
    while (g_reapedCount.load(std::memory_order_relaxed) < expected &&
           std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    return g_reapedCount.load(std::memory_order_relaxed) >= expected;
}

int main() {
    TqTunnelReaper& reaper = TqTunnelReaper::Instance();
    reaper.Start();
    reaper.Stop();
    reaper.Start();

    TestTunnelContext tunnel;
    g_reapedCount.store(0, std::memory_order_relaxed);
    g_lastReaped = nullptr;

    reaper.Register(reinterpret_cast<TqTunnelContext*>(&tunnel));
    tunnel.stopped.store(true, std::memory_order_relaxed);
    if (!WaitForReap(1, 2000)) {
        reaper.Stop();
        return 100;
    }
    if (g_lastReaped != &tunnel) {
        reaper.Stop();
        return 101;
    }

    reaper.Stop();
    return 0;
}
