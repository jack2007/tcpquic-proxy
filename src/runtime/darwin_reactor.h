#pragma once

#include "socket_reactor.h"

#if defined(__APPLE__)
#include <unordered_map>
#endif

class TqDarwinReactor final : public ITqSocketReactor {
public:
    TqDarwinReactor() = default;
    ~TqDarwinReactor() override;

    TqDarwinReactor(const TqDarwinReactor&) = delete;
    TqDarwinReactor& operator=(const TqDarwinReactor&) = delete;

    bool Start() override;
    void Stop() override;
    bool Add(TqSocketHandle fd, uint32_t events, Handler handler) override;
    bool Modify(TqSocketHandle fd, uint32_t events) override;
    bool Remove(TqSocketHandle fd) override;
    bool Wake() override;
    bool RunOnce(int timeoutMs) override;

private:
#if defined(__APPLE__)
    bool ApplyFilters(TqSocketHandle fd, uint32_t events, bool addMissing);
    bool DeleteFilter(TqSocketHandle fd, int16_t filter);

    int KqueueFd{-1};
    struct Entry {
        uint32_t Events{0};
        Handler Callback;
    };

    std::unordered_map<TqSocketHandle, Entry> Handlers;
#else
    bool Started{false};
#endif
};
