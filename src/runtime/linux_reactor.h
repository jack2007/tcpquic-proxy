#pragma once

#include "socket_reactor.h"

#include <unordered_map>

class TqLinuxReactor final : public ITqSocketReactor {
public:
    TqLinuxReactor() = default;
    ~TqLinuxReactor() override;

    TqLinuxReactor(const TqLinuxReactor&) = delete;
    TqLinuxReactor& operator=(const TqLinuxReactor&) = delete;

    bool Start() override;
    void Stop() override;
    bool Add(TqSocketHandle fd, uint32_t events, Handler handler) override;
    bool Modify(TqSocketHandle fd, uint32_t events) override;
    bool Remove(TqSocketHandle fd) override;
    bool Wake() override;
    bool RunOnce(int timeoutMs) override;

private:
    int EpollFd{-1};
    int WakeFd{-1};
    std::unordered_map<TqSocketHandle, Handler> Handlers;
};
