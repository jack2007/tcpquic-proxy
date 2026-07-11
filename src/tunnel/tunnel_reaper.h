#pragma once

#include <condition_variable>
#include <mutex>
#include <thread>
#include <vector>

struct TqTunnelContext;

class TqTunnelReaper {
public:
    static TqTunnelReaper& Instance();

    void Start();
    void Stop();
    void Register(TqTunnelContext* ctx);
    void Unregister(TqTunnelContext* ctx);
#if defined(TQ_UNIT_TESTING)
    size_t ReapReadyForTest();
#endif

private:
    void ReaperLoop();
    std::vector<TqTunnelContext*> TakeReady();

    std::mutex Mutex;
    std::condition_variable Wakeup;
    std::vector<TqTunnelContext*> Contexts;
    std::thread Thread;
    bool Running{false};
    bool StopRequested{false};
};
