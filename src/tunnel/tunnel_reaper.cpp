#include "tunnel_reaper.h"

#include <chrono>

struct TqTunnelContext;
bool TqTunnelTerminalReleaseReady(const TqTunnelContext* ctx);
void TqReapTunnelContext(TqTunnelContext* ctx);
#include <mutex>
#include <thread>
#include <vector>

namespace {

constexpr auto TqReaperPollInterval = std::chrono::milliseconds(100);

} // namespace

TqTunnelReaper& TqTunnelReaper::Instance() {
    static TqTunnelReaper instance;
    return instance;
}

void TqTunnelReaper::Start() {
    std::lock_guard<std::mutex> guard(Mutex);
    if (Running) {
        return;
    }

    StopRequested = false;
    Running = true;
    Thread = std::thread([this] { ReaperLoop(); });
}

void TqTunnelReaper::Stop() {
    std::thread worker;
    {
        std::lock_guard<std::mutex> guard(Mutex);
        if (!Running) {
            return;
        }

        StopRequested = true;
        Wakeup.notify_all();
        worker = std::move(Thread);
        Running = false;
    }

    if (worker.joinable()) {
        worker.join();
    }
}

void TqTunnelReaper::Register(TqTunnelContext* ctx) {
    if (ctx == nullptr) {
        return;
    }

    {
        std::lock_guard<std::mutex> guard(Mutex);
        Contexts.push_back(ctx);
    }
    Wakeup.notify_one();
}

void TqTunnelReaper::Unregister(TqTunnelContext* ctx) {
    if (ctx == nullptr) {
        return;
    }

    std::lock_guard<std::mutex> guard(Mutex);
    for (auto it = Contexts.begin(); it != Contexts.end(); ++it) {
        if (*it == ctx) {
            Contexts.erase(it);
            return;
        }
    }
}

void TqTunnelReaper::ReaperLoop() {
    for (;;) {
        std::vector<TqTunnelContext*> ready;
        {
            std::unique_lock<std::mutex> guard(Mutex);
            Wakeup.wait_for(guard, TqReaperPollInterval, [this] {
                return StopRequested;
            });

            if (StopRequested) {
                return;
            }

            for (auto it = Contexts.begin(); it != Contexts.end();) {
                TqTunnelContext* ctx = *it;
                if (TqTunnelTerminalReleaseReady(ctx)) {
                    ready.push_back(ctx);
                    it = Contexts.erase(it);
                } else {
                    ++it;
                }
            }
        }

        for (TqTunnelContext* ctx : ready) {
            TqReapTunnelContext(ctx);
        }
    }
}
