#pragma once

#include <condition_variable>
#include <cstdint>
#include <functional>
#include <mutex>
#include <queue>
#include <thread>
#include <vector>

class TqThreadPool {
public:
    explicit TqThreadPool(uint32_t workers);
    ~TqThreadPool();

    TqThreadPool(const TqThreadPool&) = delete;
    TqThreadPool& operator=(const TqThreadPool&) = delete;

    void Start();
    void Stop();

    // Returns false when the pool is not running or the task is empty (caller must handle resources).
    bool Submit(std::function<void()> fn);

private:
    void WorkerLoop();

    uint32_t WorkerCount;
    std::mutex Mutex;
    std::condition_variable Wake;
    std::queue<std::function<void()>> Queue;
    std::vector<std::thread> Threads;
    bool Running{false};
    bool StopRequested{false};
};
