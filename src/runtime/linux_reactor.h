#pragma once

#include <cstdint>
#include <functional>
#include <unordered_map>

namespace TqLinuxReactorEvents {
constexpr uint32_t Read = 0x1;
constexpr uint32_t Write = 0x2;
constexpr uint32_t Error = 0x4;
} // namespace TqLinuxReactorEvents

class TqLinuxReactor {
public:
    using Handler = std::function<void(int fd, uint32_t events)>;

    TqLinuxReactor() = default;
    ~TqLinuxReactor();

    TqLinuxReactor(const TqLinuxReactor&) = delete;
    TqLinuxReactor& operator=(const TqLinuxReactor&) = delete;

    bool Start();
    void Stop();
    bool Add(int fd, uint32_t events, Handler handler);
    bool Modify(int fd, uint32_t events);
    bool Remove(int fd);
    bool Wake();
    bool RunOnce(int timeoutMs);

private:
    int EpollFd{-1};
    int WakeFd{-1};
    std::unordered_map<int, Handler> Handlers;
};
