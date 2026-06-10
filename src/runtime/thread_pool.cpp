#include "thread_pool.h"

#include <algorithm>

namespace {

uint32_t ResolveWorkerCount(uint32_t workers) {
    if (workers != 0) {
        return workers;
    }

    unsigned int hw = std::thread::hardware_concurrency();
    if (hw == 0) {
        hw = 1;
    }
    return static_cast<uint32_t>(std::min<unsigned int>(hw, 64));
}

} // namespace

TqThreadPool::TqThreadPool(uint32_t workers)
    : WorkerCount(ResolveWorkerCount(workers)) {}

TqThreadPool::~TqThreadPool() {
    Stop();
}

void TqThreadPool::Start() {
    std::lock_guard<std::mutex> guard(Mutex);
    if (Running) {
        return;
    }

    StopRequested = false;
    Running = true;
    Threads.reserve(WorkerCount);
    for (uint32_t i = 0; i < WorkerCount; ++i) {
        Threads.emplace_back([this] { WorkerLoop(); });
    }
}

void TqThreadPool::Stop() {
    std::vector<std::thread> workers;
    {
        std::lock_guard<std::mutex> guard(Mutex);
        if (!Running) {
            return;
        }

        StopRequested = true;
        Wake.notify_all();
        workers = std::move(Threads);
        Running = false;
    }

    for (auto& worker : workers) {
        if (worker.joinable()) {
            worker.join();
        }
    }
}

bool TqThreadPool::Submit(std::function<void()> fn) {
    if (!fn) {
        return false;
    }

    {
        std::lock_guard<std::mutex> guard(Mutex);
        if (!Running || StopRequested) {
            return false;
        }
        Queue.push(std::move(fn));
    }
    Wake.notify_one();
    return true;
}

void TqThreadPool::WorkerLoop() {
    for (;;) {
        std::function<void()> task;
        {
            std::unique_lock<std::mutex> lock(Mutex);
            Wake.wait(lock, [this] {
                return StopRequested || !Queue.empty();
            });

            if (StopRequested && Queue.empty()) {
                return;
            }
            if (Queue.empty()) {
                continue;
            }

            task = std::move(Queue.front());
            Queue.pop();
        }

        task();
    }
}
